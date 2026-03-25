#include "ui/Application.h"
#include "audio/FileSource.h"

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>

#include <GL/gl.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace baudline {

Application::Application() = default;

Application::~Application() {
    shutdown();
}

bool Application::init(int argc, char** argv) {
    // Parse command line: baudline [file] [--format fmt] [--rate sr]
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--format" && i + 1 < argc) {
            std::string fmt = argv[++i];
            if (fmt == "f32")  fileFormatIdx_ = 0;
            if (fmt == "i16")  fileFormatIdx_ = 1;
            if (fmt == "u8")   fileFormatIdx_ = 2;
            if (fmt == "wav")  fileFormatIdx_ = 3;
        } else if (arg == "--rate" && i + 1 < argc) {
            fileSampleRate_ = std::stof(argv[++i]);
        } else if (arg == "--iq") {
            settings_.isIQ = true;
        } else if (arg[0] != '-') {
            filePath_ = arg;
        }
    }

    // SDL init
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    window_ = SDL_CreateWindow("Baudline Spectrum Analyzer",
                               SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               1400, 900,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                               SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window_) {
        std::fprintf(stderr, "SDL_CreateWindow error: %s\n", SDL_GetError());
        return false;
    }

    glContext_ = SDL_GL_CreateContext(window_);
    SDL_GL_MakeCurrent(window_, glContext_);
    SDL_GL_SetSwapInterval(1); // vsync

    // ImGui init
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.GrabRounding = 2.0f;

    ImGui_ImplSDL2_InitForOpenGL(window_, glContext_);
    ImGui_ImplOpenGL3_Init("#version 120");

    // Enumerate audio devices
    paDevices_ = PortAudioSource::listInputDevices();

    // Default settings
    settings_.fftSize    = kFFTSizes[fftSizeIdx_];
    settings_.overlap    = overlapPct_ / 100.0f;
    settings_.window     = static_cast<WindowType>(windowIdx_);
    settings_.sampleRate = fileSampleRate_;
    settings_.isIQ       = false;

    // Open source
    if (!filePath_.empty()) {
        InputFormat fmt;
        switch (fileFormatIdx_) {
            case 0: fmt = InputFormat::Float32IQ; settings_.isIQ = true; break;
            case 1: fmt = InputFormat::Int16IQ;   settings_.isIQ = true; break;
            case 2: fmt = InputFormat::Uint8IQ;   settings_.isIQ = true; break;
            default: fmt = InputFormat::WAV;      break;
        }
        openFile(filePath_, fmt, fileSampleRate_);
    } else {
        openPortAudio();
    }

    updateAnalyzerSettings();

    running_ = true;
    return true;
}

void Application::run() {
    while (running_) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                running_ = false;
            if (event.type == SDL_KEYDOWN) {
                auto key = event.key.keysym.sym;
                if (key == SDLK_ESCAPE) running_ = false;
                if (key == SDLK_SPACE)  paused_ = !paused_;
                if (key == SDLK_p) {
                    int pkCh = std::clamp(waterfallChannel_, 0,
                                          analyzer_.numSpectra() - 1);
                    cursors_.snapToPeak(analyzer_.channelSpectrum(pkCh),
                                        settings_.sampleRate, settings_.isIQ,
                                        settings_.fftSize);
                }
            }
        }

        if (!paused_)
            processAudio();

        render();
    }
}

void Application::shutdown() {
    if (audioSource_) {
        audioSource_->close();
        audioSource_.reset();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    if (glContext_) {
        SDL_GL_DeleteContext(glContext_);
        glContext_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    SDL_Quit();
}

void Application::processAudio() {
    if (!audioSource_) return;

    int channels = audioSource_->channels();
    // Read in hop-sized chunks, process up to a limited number of spectra per
    // frame to avoid freezing the UI when a large backlog has accumulated.
    size_t hopFrames = static_cast<size_t>(
        settings_.fftSize * (1.0f - settings_.overlap));
    if (hopFrames < 1) hopFrames = 1;
    size_t framesToRead = hopFrames;
    audioBuf_.resize(framesToRead * channels);

    constexpr int kMaxSpectraPerFrame = 8;
    int spectraThisFrame = 0;

    while (spectraThisFrame < kMaxSpectraPerFrame) {
        size_t framesRead = audioSource_->read(audioBuf_.data(), framesToRead);
        if (framesRead == 0) break;

        analyzer_.pushSamples(audioBuf_.data(), framesRead);

        if (analyzer_.hasNewSpectrum()) {
            int nSpec = analyzer_.numSpectra();
            if (waterfallMultiCh_ && nSpec > 1) {
                // Multi-channel overlay waterfall.
                std::vector<WaterfallChannelInfo> wfChInfo(nSpec);
                for (int ch = 0; ch < nSpec; ++ch) {
                    const auto& c = channelColors_[ch % kMaxChannels];
                    wfChInfo[ch] = {c.x, c.y, c.z,
                                    channelEnabled_[ch % kMaxChannels]};
                }
                waterfall_.pushLineMulti(analyzer_.allSpectra(),
                                         wfChInfo, minDB_, maxDB_);
            } else {
                int wfCh = std::clamp(waterfallChannel_, 0, nSpec - 1);
                waterfall_.pushLine(analyzer_.channelSpectrum(wfCh),
                                    minDB_, maxDB_);
            }
            int curCh = std::clamp(waterfallChannel_, 0, nSpec - 1);
            cursors_.update(analyzer_.channelSpectrum(curCh),
                           settings_.sampleRate, settings_.isIQ, settings_.fftSize);
            ++spectraThisFrame;
        }
    }

    if (audioSource_->isEOF() && !audioSource_->isRealTime()) {
        paused_ = true;
    }
}

void Application::render() {
    // Skip rendering entirely when the window is minimized — the drawable
    // size is 0, which would create zero-sized GL textures and divide-by-zero
    // in layout calculations.
    if (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) {
        SDL_Delay(16);
        return;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // Full-screen layout
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("##Main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_MenuBar);

    // Menu bar
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open WAV...")) {
                // TODO: file dialog integration
            }
            if (ImGui::MenuItem("Quit", "Esc")) running_ = false;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Grid", nullptr, &specDisplay_.showGrid);
            ImGui::MenuItem("Fill Spectrum", nullptr, &specDisplay_.fillSpectrum);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    // Layout: controls on left (250px), spectrum+waterfall on right
    float controlW = 260.0f;
    float contentW = ImGui::GetContentRegionAvail().x - controlW - 8;
    float contentH = ImGui::GetContentRegionAvail().y;

    // Control panel
    ImGui::BeginChild("Controls", {controlW, contentH}, true);
    renderControlPanel();
    ImGui::EndChild();

    ImGui::SameLine();

    // Spectrum + Waterfall
    ImGui::BeginChild("Display", {contentW, contentH}, false);
    float specH = contentH * 0.35f;
    float waterfH = contentH * 0.65f - 4;

    renderSpectrumPanel();
    renderWaterfallPanel();
    ImGui::EndChild();

    ImGui::End();

    // Render
    ImGui::Render();
    int displayW, displayH;
    SDL_GL_GetDrawableSize(window_, &displayW, &displayH);
    glViewport(0, 0, displayW, displayH);
    glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window_);
}

void Application::renderControlPanel() {
    ImGui::TextColored({0.4f, 0.8f, 1.0f, 1.0f}, "BAUDLINE");
    ImGui::Separator();

    // Input source
    ImGui::Text("Input Source");
    if (ImGui::Button("PortAudio (Mic)")) {
        openPortAudio();
        updateAnalyzerSettings();
    }

    ImGui::Separator();
    ImGui::Text("File Input");

    // Show file path input
    static char filePathBuf[512] = "";
    if (filePath_.size() < sizeof(filePathBuf))
        std::strncpy(filePathBuf, filePath_.c_str(), sizeof(filePathBuf) - 1);
    if (ImGui::InputText("Path", filePathBuf, sizeof(filePathBuf)))
        filePath_ = filePathBuf;

    const char* formatNames[] = {"Float32 I/Q", "Int16 I/Q", "Uint8 I/Q", "WAV"};
    ImGui::Combo("Format", &fileFormatIdx_, formatNames, 4);
    ImGui::DragFloat("Sample Rate", &fileSampleRate_, 1000.0f, 1000.0f, 100e6f, "%.0f Hz");
    ImGui::Checkbox("Loop", &fileLoop_);

    if (ImGui::Button("Open File")) {
        InputFormat fmt;
        switch (fileFormatIdx_) {
            case 0: fmt = InputFormat::Float32IQ; break;
            case 1: fmt = InputFormat::Int16IQ;   break;
            case 2: fmt = InputFormat::Uint8IQ;   break;
            default: fmt = InputFormat::WAV;       break;
        }
        openFile(filePath_, fmt, fileSampleRate_);
        updateAnalyzerSettings();
    }

    // PortAudio device list
    if (!paDevices_.empty()) {
        ImGui::Separator();
        ImGui::Text("Audio Device");
        std::vector<const char*> devNames;
        for (auto& d : paDevices_) devNames.push_back(d.name.c_str());
        if (ImGui::Combo("Device", &paDeviceIdx_, devNames.data(),
                         static_cast<int>(devNames.size()))) {
            openPortAudio();
            updateAnalyzerSettings();
        }
    }

    ImGui::Separator();
    ImGui::Text("FFT Settings");

    // FFT size
    {
        const char* sizeNames[] = {"256", "512", "1024", "2048", "4096",
                                   "8192", "16384", "32768", "65536"};
        if (ImGui::Combo("FFT Size", &fftSizeIdx_, sizeNames, kNumFFTSizes)) {
            settings_.fftSize = kFFTSizes[fftSizeIdx_];
            updateAnalyzerSettings();
        }
    }

    // Overlap
    if (ImGui::SliderFloat("Overlap", &overlapPct_, 0.0f, 95.0f, "%.1f%%")) {
        settings_.overlap = overlapPct_ / 100.0f;
        updateAnalyzerSettings();
    }

    // Window function
    {
        const char* winNames[] = {"Rectangular", "Hann", "Hamming", "Blackman",
                                  "Blackman-Harris", "Kaiser", "Flat Top"};
        if (ImGui::Combo("Window", &windowIdx_, winNames,
                         static_cast<int>(WindowType::Count))) {
            settings_.window = static_cast<WindowType>(windowIdx_);
            if (settings_.window == WindowType::Kaiser) {
                // Show Kaiser beta slider
            }
            updateAnalyzerSettings();
        }
    }

    if (settings_.window == WindowType::Kaiser) {
        if (ImGui::SliderFloat("Kaiser Beta", &settings_.kaiserBeta, 0.0f, 20.0f)) {
            updateAnalyzerSettings();
        }
    }

    // Averaging
    ImGui::SliderInt("Averaging", &settings_.averaging, 1, 32);

    ImGui::Separator();
    ImGui::Text("Display");

    // Color map
    {
        const char* cmNames[] = {"Magma", "Viridis", "Inferno", "Plasma", "Grayscale"};
        if (ImGui::Combo("Color Map", &colorMapIdx_, cmNames,
                         static_cast<int>(ColorMapType::Count))) {
            colorMap_.setType(static_cast<ColorMapType>(colorMapIdx_));
            waterfall_.setColorMap(colorMap_);
        }
    }

    // Frequency scale
    {
        int fs = static_cast<int>(freqScale_);
        const char* fsNames[] = {"Linear", "Logarithmic"};
        if (ImGui::Combo("Freq Scale", &fs, fsNames, 2))
            freqScale_ = static_cast<FreqScale>(fs);
    }

    // dB range
    ImGui::DragFloatRange2("dB Range", &minDB_, &maxDB_, 1.0f, -200.0f, 20.0f,
                           "Min: %.0f", "Max: %.0f");

    // Channel colors (only shown for multi-channel)
    int nCh = analyzer_.numSpectra();
    if (nCh > 1) {
        ImGui::Separator();
        ImGui::Text("Channels (%d)", nCh);

        static const char* defaultNames[] = {
            "Left", "Right", "Ch 3", "Ch 4", "Ch 5", "Ch 6", "Ch 7", "Ch 8"
        };
        for (int ch = 0; ch < nCh && ch < kMaxChannels; ++ch) {
            ImGui::PushID(ch);
            ImGui::Checkbox("##en", &channelEnabled_[ch]);
            ImGui::SameLine();
            ImGui::ColorEdit3(defaultNames[ch], &channelColors_[ch].x,
                              ImGuiColorEditFlags_NoInputs);
            ImGui::PopID();
        }

        // Waterfall mode
        ImGui::Checkbox("Multi-Ch Waterfall", &waterfallMultiCh_);
        if (!waterfallMultiCh_) {
            if (ImGui::SliderInt("Waterfall Ch", &waterfallChannel_, 0, nCh - 1))
                waterfallChannel_ = std::clamp(waterfallChannel_, 0, nCh - 1);
        }
    }

    ImGui::Separator();

    // Playback controls
    if (ImGui::Button(paused_ ? "Resume [Space]" : "Pause [Space]"))
        paused_ = !paused_;

    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        analyzer_.clearHistory();
    }

    ImGui::Separator();

    // Cursors
    cursors_.drawPanel();

    ImGui::Separator();
    if (ImGui::Button("Snap to Peak [P]")) {
        int pkCh = std::clamp(waterfallChannel_, 0, analyzer_.numSpectra() - 1);
        cursors_.snapToPeak(analyzer_.channelSpectrum(pkCh),
                            settings_.sampleRate, settings_.isIQ,
                            settings_.fftSize);
    }

    // Status
    ImGui::Separator();
    ImGui::Text("FFT: %d pt, %.1f Hz/bin",
                settings_.fftSize,
                settings_.sampleRate / settings_.fftSize);
    ImGui::Text("Sample Rate: %.0f Hz", settings_.sampleRate);
    ImGui::Text("Mode: %s", settings_.isIQ ? "I/Q (Complex)"
                : (settings_.numChannels > 1 ? "Multi-channel Real" : "Real"));

    int pkCh2 = std::clamp(waterfallChannel_, 0, analyzer_.numSpectra() - 1);
    auto [peakBin, peakDB] = analyzer_.findPeak(pkCh2);
    double peakFreq = analyzer_.binToFreq(peakBin);
    if (std::abs(peakFreq) >= 1e6)
        ImGui::Text("Peak: %.6f MHz, %.1f dB", peakFreq / 1e6, peakDB);
    else if (std::abs(peakFreq) >= 1e3)
        ImGui::Text("Peak: %.3f kHz, %.1f dB", peakFreq / 1e3, peakDB);
    else
        ImGui::Text("Peak: %.1f Hz, %.1f dB", peakFreq, peakDB);
}

void Application::renderSpectrumPanel() {
    float availW = ImGui::GetContentRegionAvail().x;
    float specH = ImGui::GetContentRegionAvail().y * 0.35f;

    ImVec2 pos = ImGui::GetCursorScreenPos();
    specPosX_ = pos.x;
    specPosY_ = pos.y;
    specSizeX_ = availW;
    specSizeY_ = specH;

    // Build per-channel styles and pass all spectra.
    int nCh = analyzer_.numSpectra();
    std::vector<ChannelStyle> styles(nCh);
    for (int ch = 0; ch < nCh; ++ch) {
        const auto& c = channelColors_[ch % kMaxChannels];
        uint8_t r = static_cast<uint8_t>(c.x * 255);
        uint8_t g = static_cast<uint8_t>(c.y * 255);
        uint8_t b = static_cast<uint8_t>(c.z * 255);
        styles[ch].lineColor = IM_COL32(r, g, b, 220);
        styles[ch].fillColor = IM_COL32(r, g, b, 35);
    }
    specDisplay_.draw(analyzer_.allSpectra(), styles, minDB_, maxDB_,
                      settings_.sampleRate, settings_.isIQ, freqScale_,
                      specPosX_, specPosY_, specSizeX_, specSizeY_);

    cursors_.draw(specDisplay_, specPosX_, specPosY_, specSizeX_, specSizeY_,
                  settings_.sampleRate, settings_.isIQ, freqScale_, minDB_, maxDB_);

    handleSpectrumInput(specPosX_, specPosY_, specSizeX_, specSizeY_);

    ImGui::Dummy({availW, specH});
}

void Application::renderWaterfallPanel() {
    float availW = ImGui::GetContentRegionAvail().x;
    float availH = ImGui::GetContentRegionAvail().y;

    int newW = static_cast<int>(availW);
    int newH = static_cast<int>(availH);
    if (newW < 1) newW = 1;
    if (newH < 1) newH = 1;

    if (newW != waterfallW_ || newH != waterfallH_) {
        waterfallW_ = newW;
        waterfallH_ = newH;
        waterfall_.resize(waterfallW_, waterfallH_);
        waterfall_.setColorMap(colorMap_);
    }

    if (waterfall_.textureID()) {
        // Render waterfall texture with circular buffer offset.
        // The texture rows wrap: currentRow_ is where the *next* line will go,
        // so the *newest* line is at currentRow_+1.
        float rowFrac = static_cast<float>(waterfall_.currentRow() + 1) /
                        waterfall_.height();

        // UV coordinates: bottom of display = newest = rowFrac
        // top of display = oldest = rowFrac + 1.0 (wraps)
        // We'll use two draw calls to handle the wrap, or use GL_REPEAT.
        // Simplest: just render with ImGui::Image and accept minor visual glitch,
        // or split into two parts.

        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto texID = static_cast<ImTextureID>(waterfall_.textureID());

        int h = waterfall_.height();
        int cur = (waterfall_.currentRow() + 1) % h;
        float splitFrac = static_cast<float>(h - cur) / h;

        // Top part: rows from cur to h-1 (oldest)
        float topH = availH * splitFrac;
        dl->AddImage(texID,
                     {pos.x, pos.y},
                     {pos.x + availW, pos.y + topH},
                     {0.0f, static_cast<float>(cur) / h},
                     {1.0f, 1.0f});

        // Bottom part: rows from 0 to cur-1 (newest)
        if (cur > 0) {
            dl->AddImage(texID,
                         {pos.x, pos.y + topH},
                         {pos.x + availW, pos.y + availH},
                         {0.0f, 0.0f},
                         {1.0f, static_cast<float>(cur) / h});
        }

        // Frequency axis labels at bottom
        ImU32 textCol = IM_COL32(180, 180, 200, 200);
        double freqMin = settings_.isIQ ? -settings_.sampleRate / 2.0 : 0.0;
        double freqMax = settings_.isIQ ?  settings_.sampleRate / 2.0 : settings_.sampleRate / 2.0;
        int numLabels = 8;
        for (int i = 0; i <= numLabels; ++i) {
            float frac = static_cast<float>(i) / numLabels;
            double freq = freqMin + frac * (freqMax - freqMin);
            float x = pos.x + frac * availW;

            char label[32];
            if (std::abs(freq) >= 1e6)
                std::snprintf(label, sizeof(label), "%.2fM", freq / 1e6);
            else if (std::abs(freq) >= 1e3)
                std::snprintf(label, sizeof(label), "%.1fk", freq / 1e3);
            else
                std::snprintf(label, sizeof(label), "%.0f", freq);

            dl->AddText({x + 2, pos.y + availH - 14}, textCol, label);
        }
    }

    ImGui::Dummy({availW, availH});
}

void Application::handleSpectrumInput(float posX, float posY,
                                       float sizeX, float sizeY) {
    ImGuiIO& io = ImGui::GetIO();
    float mx = io.MousePos.x;
    float my = io.MousePos.y;

    bool inRegion = mx >= posX && mx <= posX + sizeX &&
                    my >= posY && my <= posY + sizeY;

    if (inRegion) {
        // Update hover cursor
        double freq = specDisplay_.screenXToFreq(mx, posX, sizeX,
                                                  settings_.sampleRate,
                                                  settings_.isIQ, freqScale_);
        float dB = specDisplay_.screenYToDB(my, posY, sizeY, minDB_, maxDB_);

        // Find closest bin
        int bins = analyzer_.spectrumSize();
        double freqMin = settings_.isIQ ? -settings_.sampleRate / 2.0 : 0.0;
        double freqMax = settings_.isIQ ?  settings_.sampleRate / 2.0 : settings_.sampleRate / 2.0;
        int bin = static_cast<int>((freq - freqMin) / (freqMax - freqMin) * (bins - 1));
        bin = std::clamp(bin, 0, bins - 1);

        int curCh = std::clamp(waterfallChannel_, 0, analyzer_.numSpectra() - 1);
        const auto& spec = analyzer_.channelSpectrum(curCh);
        if (!spec.empty()) {
            dB = spec[bin];
            cursors_.hover = {true, freq, dB, bin};
        }

        // Left click: cursor A
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.WantCaptureMouse) {
            int peakBin = cursors_.findLocalPeak(spec, bin, 10);
            double peakFreq = analyzer_.binToFreq(peakBin);
            cursors_.setCursorA(peakFreq, spec[peakBin], peakBin);
        }
        // Right click: cursor B
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !io.WantCaptureMouse) {
            int peakBin = cursors_.findLocalPeak(spec, bin, 10);
            double peakFreq = analyzer_.binToFreq(peakBin);
            cursors_.setCursorB(peakFreq, spec[peakBin], peakBin);
        }

        // Scroll: zoom dB range
        if (io.MouseWheel != 0 && !io.WantCaptureMouse) {
            float zoom = io.MouseWheel * 5.0f;
            minDB_ += zoom;
            maxDB_ -= zoom;
            if (maxDB_ - minDB_ < 10.0f) {
                float mid = (minDB_ + maxDB_) / 2.0f;
                minDB_ = mid - 5.0f;
                maxDB_ = mid + 5.0f;
            }
        }
    } else {
        cursors_.hover.active = false;
    }
}

void Application::openPortAudio() {
    if (audioSource_) audioSource_->close();

    int deviceIdx = -1;
    double sr = 48000.0;
    if (paDeviceIdx_ >= 0 && paDeviceIdx_ < static_cast<int>(paDevices_.size())) {
        deviceIdx = paDevices_[paDeviceIdx_].index;
        sr = paDevices_[paDeviceIdx_].defaultSampleRate;
    }

    // Request stereo (or max available) so we can show per-channel spectra.
    int reqCh = 2;
    if (paDeviceIdx_ >= 0 && paDeviceIdx_ < static_cast<int>(paDevices_.size()))
        reqCh = std::min(paDevices_[paDeviceIdx_].maxInputChannels, kMaxChannels);
    if (reqCh < 1) reqCh = 1;
    auto src = std::make_unique<PortAudioSource>(sr, reqCh, deviceIdx);
    if (src->open()) {
        audioSource_ = std::move(src);
        settings_.sampleRate = sr;
        settings_.isIQ = false;
        settings_.numChannels = audioSource_->channels();
    } else {
        std::fprintf(stderr, "Failed to open PortAudio device\n");
    }
}

void Application::openFile(const std::string& path, InputFormat format, double sampleRate) {
    if (audioSource_) audioSource_->close();

    bool isIQ = (format != InputFormat::WAV);
    auto src = std::make_unique<FileSource>(path, format, sampleRate, fileLoop_);
    if (src->open()) {
        settings_.sampleRate = src->sampleRate();
        settings_.isIQ = isIQ;
        settings_.numChannels = isIQ ? 1 : src->channels();
        audioSource_ = std::move(src);
        fileSampleRate_ = static_cast<float>(settings_.sampleRate);
    } else {
        std::fprintf(stderr, "Failed to open file: %s\n", path.c_str());
    }
}

void Application::updateAnalyzerSettings() {
    int  oldFFTSize = settings_.fftSize;
    bool oldIQ      = settings_.isIQ;
    int  oldNCh     = settings_.numChannels;

    settings_.fftSize = kFFTSizes[fftSizeIdx_];
    settings_.overlap = overlapPct_ / 100.0f;
    settings_.window  = static_cast<WindowType>(windowIdx_);
    analyzer_.configure(settings_);

    bool sizeChanged = settings_.fftSize     != oldFFTSize ||
                       settings_.isIQ        != oldIQ      ||
                       settings_.numChannels != oldNCh;

    if (sizeChanged) {
        // Drain any stale audio data from the ring buffer so a backlog from
        // the reconfigure doesn't flood the new analyzer.
        if (audioSource_ && audioSource_->isRealTime()) {
            int channels = audioSource_->channels();
            std::vector<float> drain(4096 * channels);
            while (audioSource_->read(drain.data(), 4096) > 0) {}
        }

        // Invalidate cursor bin indices — they refer to the old FFT size.
        cursors_.cursorA.active = false;
        cursors_.cursorB.active = false;

        // Re-init waterfall texture so the old image from a different FFT
        // size doesn't persist.
        if (waterfallW_ > 0 && waterfallH_ > 0)
            waterfall_.init(waterfallW_, waterfallH_);
    }
}

} // namespace baudline
