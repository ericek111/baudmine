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
            computeMathChannels();

            int nSpec = analyzer_.numSpectra();
            if (waterfallMultiCh_ && nSpec > 1) {
                // Multi-channel overlay waterfall: physical + math channels.
                wfSpectraScratch_.clear();
                wfChInfoScratch_.clear();

                for (int ch = 0; ch < nSpec; ++ch) {
                    const auto& c = channelColors_[ch % kMaxChannels];
                    wfSpectraScratch_.push_back(analyzer_.channelSpectrum(ch));
                    wfChInfoScratch_.push_back({c.x, c.y, c.z,
                                        channelEnabled_[ch % kMaxChannels]});
                }
                for (size_t mi = 0; mi < mathChannels_.size(); ++mi) {
                    if (mathChannels_[mi].enabled && mathChannels_[mi].waterfall &&
                        mi < mathSpectra_.size()) {
                        const auto& c = mathChannels_[mi].color;
                        wfSpectraScratch_.push_back(mathSpectra_[mi]);
                        wfChInfoScratch_.push_back({c.x, c.y, c.z, true});
                    }
                }
                waterfall_.pushLineMulti(wfSpectraScratch_, wfChInfoScratch_, minDB_, maxDB_);
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
            ImGui::Separator();
            if (ImGui::MenuItem("VSync", nullptr, &vsync_)) {
                SDL_GL_SetSwapInterval(vsync_ ? 1 : 0);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Debug")) {
            ImGui::MenuItem("Metrics/Debugger", nullptr, &showMetricsWindow_);
            ImGui::MenuItem("Debug Log", nullptr, &showDebugLog_);
            ImGui::MenuItem("Stack Tool", nullptr, &showStackTool_);
            ImGui::MenuItem("Demo Window", nullptr, &showDemoWindow_);
            ImGui::Separator();
            ImGui::Text("%.1f FPS (%.3f ms)", ImGui::GetIO().Framerate,
                        1000.0f / ImGui::GetIO().Framerate);
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

    // Spectrum + Waterfall with draggable splitter
    ImGui::BeginChild("Display", {contentW, contentH}, false);
    {
        constexpr float kSplitterH = 6.0f;
        float specH  = contentH * spectrumFrac_;
        float waterfH = contentH - specH - kSplitterH;

        renderSpectrumPanel();

        // ── Draggable splitter bar ──
        ImVec2 splPos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##splitter", {contentW, kSplitterH});
        bool hovered = ImGui::IsItemHovered();
        bool active  = ImGui::IsItemActive();

        if (hovered || active)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

        if (active) {
            float dy = ImGui::GetIO().MouseDelta.y;
            spectrumFrac_ += dy / contentH;
            spectrumFrac_ = std::clamp(spectrumFrac_, 0.1f, 0.9f);
        }

        // Draw splitter line
        ImU32 splCol = (hovered || active)
            ? IM_COL32(100, 150, 255, 220)
            : IM_COL32(80, 80, 100, 150);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float cy = splPos.y + kSplitterH * 0.5f;
        dl->AddLine({splPos.x, cy}, {splPos.x + contentW, cy}, splCol, 2.0f);

        renderWaterfallPanel();

        // ── Cross-panel hover line & frequency label ──
        if (cursors_.hover.active && specSizeX_ > 0 && wfSizeX_ > 0) {
            ImDrawList* dlp = ImGui::GetWindowDrawList();
            float hx = specDisplay_.freqToScreenX(cursors_.hover.freq,
                           specPosX_, specSizeX_, settings_.sampleRate,
                           settings_.isIQ, freqScale_, viewLo_, viewHi_);
            ImU32 hoverCol = IM_COL32(200, 200, 200, 80);

            // Line spanning spectrum + splitter + waterfall
            dlp->AddLine({hx, specPosY_}, {hx, wfPosY_ + wfSizeY_}, hoverCol, 1.0f);

            // Frequency label at top of the line
            char freqLabel[48];
            double hf = cursors_.hover.freq;
            if (std::abs(hf) >= 1e6)
                std::snprintf(freqLabel, sizeof(freqLabel), "%.6f MHz", hf / 1e6);
            else if (std::abs(hf) >= 1e3)
                std::snprintf(freqLabel, sizeof(freqLabel), "%.3f kHz", hf / 1e3);
            else
                std::snprintf(freqLabel, sizeof(freqLabel), "%.1f Hz", hf);

            ImVec2 tSz = ImGui::CalcTextSize(freqLabel);
            float lx = std::min(hx + 4, specPosX_ + specSizeX_ - tSz.x - 4);
            float ly = specPosY_ + 2;
            dlp->AddRectFilled({lx - 2, ly - 1}, {lx + tSz.x + 2, ly + tSz.y + 1},
                               IM_COL32(0, 0, 0, 180));
            dlp->AddText({lx, ly}, IM_COL32(220, 220, 240, 240), freqLabel);
        }
    }
    ImGui::EndChild();

    ImGui::End();

    // ImGui debug windows
    if (showDemoWindow_)    ImGui::ShowDemoWindow(&showDemoWindow_);
    if (showMetricsWindow_) ImGui::ShowMetricsWindow(&showMetricsWindow_);
    if (showDebugLog_)      ImGui::ShowDebugLogWindow(&showDebugLog_);
    if (showStackTool_)     ImGui::ShowIDStackToolWindow(&showStackTool_);

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

    // Overlap — inverted x⁴ curve: sensitive at the high end (90%+).
    {
        int hopSamples = static_cast<int>(settings_.fftSize * (1.0f - settings_.overlap));
        if (hopSamples < 1) hopSamples = 1;
        int overlapSamples = settings_.fftSize - hopSamples;

        float sliderVal = 1.0f - std::pow(1.0f - overlapPct_ / 99.0f, 0.25f);
        if (ImGui::SliderFloat("Overlap", &sliderVal, 0.0f, 1.0f, "")) {
            float inv = 1.0f - sliderVal;
            float inv2 = inv * inv;
            overlapPct_ = 99.0f * (1.0f - inv2 * inv2);
            settings_.overlap = overlapPct_ / 100.0f;
            updateAnalyzerSettings();
        }

        // Draw overlay text centered on the slider frame (not the label).
        char overlayText[64];
        std::snprintf(overlayText, sizeof(overlayText), "%.1f%% (%d samples)", overlapPct_, overlapSamples);
        ImVec2 textSize = ImGui::CalcTextSize(overlayText);
        // The slider frame width = total widget width minus label.
        // ImGui::CalcItemWidth() gives the frame width.
        ImVec2 sliderMin = ImGui::GetItemRectMin();
        float frameW = ImGui::CalcItemWidth();
        float frameH = ImGui::GetItemRectMax().y - sliderMin.y;
        float tx = sliderMin.x + (frameW - textSize.x) * 0.5f;
        float ty = sliderMin.y + (frameH - textSize.y) * 0.5f;
        ImGui::GetForegroundDrawList()->AddText({tx, ty}, IM_COL32(255, 255, 255, 220), overlayText);
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

    // Zoom info & reset
    if (viewLo_ > 0.0f || viewHi_ < 1.0f) {
        float zoomPct = 1.0f / (viewHi_ - viewLo_);
        ImGui::Text("Zoom: %.1fx", zoomPct);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset")) {
            viewLo_ = 0.0f;
            viewHi_ = 1.0f;
        }
    }
    ImGui::TextDisabled("Scroll: freq zoom | MMB drag: pan");
    ImGui::TextDisabled("Ctrl+Scroll: dB zoom | MMB dbl: reset");

    // dB range
    ImGui::DragFloatRange2("dB Range", &minDB_, &maxDB_, 1.0f, -200.0f, 20.0f,
                           "Min: %.0f", "Max: %.0f");

    // Peak hold
    ImGui::Checkbox("Peak Hold", &specDisplay_.peakHoldEnable);
    if (specDisplay_.peakHoldEnable) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::SliderFloat("Decay", &specDisplay_.peakHoldDecay, 0.0f, 120.0f, "%.0f dB/s");
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##peakhold"))
            specDisplay_.clearPeakHold();
    }

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

    // Math channels section (always shown).
    ImGui::Separator();
    renderMathPanel();

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
    // Use the parent's full content height (availY includes spectrum + splitter + waterfall)
    // to compute the spectrum height from the split fraction.
    constexpr float kSplitterH = 6.0f;
    float parentH = ImGui::GetContentRegionAvail().y;
    float specH = (parentH - kSplitterH) * spectrumFrac_;

    ImVec2 pos = ImGui::GetCursorScreenPos();
    specPosX_ = pos.x;
    specPosY_ = pos.y;
    specSizeX_ = availW;
    specSizeY_ = specH;

    // Build per-channel styles and combine physical + math spectra.
    int nPhys = analyzer_.numSpectra();
    int nMath = static_cast<int>(mathSpectra_.size());

    allSpectraScratch_.clear();
    stylesScratch_.clear();

    // Physical channels.
    for (int ch = 0; ch < nPhys; ++ch) {
        allSpectraScratch_.push_back(analyzer_.channelSpectrum(ch));
        const auto& c = channelColors_[ch % kMaxChannels];
        uint8_t r = static_cast<uint8_t>(c.x * 255);
        uint8_t g = static_cast<uint8_t>(c.y * 255);
        uint8_t b = static_cast<uint8_t>(c.z * 255);
        stylesScratch_.push_back({IM_COL32(r, g, b, 220), IM_COL32(r, g, b, 35)});
    }

    // Math channels.
    for (int mi = 0; mi < nMath; ++mi) {
        if (mi < static_cast<int>(mathChannels_.size()) && mathChannels_[mi].enabled) {
            allSpectraScratch_.push_back(mathSpectra_[mi]);
            const auto& c = mathChannels_[mi].color;
            uint8_t r = static_cast<uint8_t>(c.x * 255);
            uint8_t g = static_cast<uint8_t>(c.y * 255);
            uint8_t b = static_cast<uint8_t>(c.z * 255);
            stylesScratch_.push_back({IM_COL32(r, g, b, 220), IM_COL32(r, g, b, 35)});
        }
    }

    specDisplay_.updatePeakHold(allSpectraScratch_);
    specDisplay_.draw(allSpectraScratch_, stylesScratch_, minDB_, maxDB_,
                      settings_.sampleRate, settings_.isIQ, freqScale_,
                      specPosX_, specPosY_, specSizeX_, specSizeY_,
                      viewLo_, viewHi_);

    cursors_.draw(specDisplay_, specPosX_, specPosY_, specSizeX_, specSizeY_,
                  settings_.sampleRate, settings_.isIQ, freqScale_, minDB_, maxDB_,
                  viewLo_, viewHi_);

    handleSpectrumInput(specPosX_, specPosY_, specSizeX_, specSizeY_);

    ImGui::Dummy({availW, specH});
}

void Application::renderWaterfallPanel() {
    float availW = ImGui::GetContentRegionAvail().x;
    float availH = ImGui::GetContentRegionAvail().y;

    // History depth must be >= panel height for 1:1 pixel mapping.
    // Only recreate when bin count or needed height actually changes.
    int neededH = std::max(1024, static_cast<int>(availH) + 1);
    int binCount = std::max(1, analyzer_.spectrumSize());
    if (binCount != waterfall_.width() || waterfall_.height() < neededH) {
        waterfall_.resize(binCount, neededH);
        waterfall_.setColorMap(colorMap_);
    }

    if (waterfall_.textureID()) {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto texID = static_cast<ImTextureID>(waterfall_.textureID());

        int h = waterfall_.height();
        // The newest row was just written at currentRow()+1 (mod h) — but
        // advanceRow already decremented, so currentRow() IS the newest.
        // The row *after* currentRow() (i.e. currentRow()+1) is the oldest
        // visible row.  We only want the most recent screenRows rows so
        // that every texture row maps to exactly one screen pixel.
        int screenRows = std::min(static_cast<int>(availH), h);

        // Newest row index in the circular buffer.
        int newestRow = (waterfall_.currentRow() + 1) % h;

        // Render 1:1 (one texture row = one screen pixel), top-aligned,
        // newest line at top (right below the spectrogram), scrolling down.
        //
        // advanceRow() decrements currentRow_, so rows are written at
        // decreasing indices.  Going from newest to oldest = increasing
        // index (mod h).  Normal V order (no flip needed).
        float rowToV = 1.0f / h;
        float screenY = pos.y;

        bool logMode = (freqScale_ == FreqScale::Logarithmic && !settings_.isIQ);

        auto drawSpan = [&](int rowStart, int rowCount, float yStart, float spanH) {
            float v0 = rowStart * rowToV;
            float v1 = (rowStart + rowCount) * rowToV;

            if (!logMode) {
                dl->AddImage(texID,
                             {pos.x, yStart},
                             {pos.x + availW, yStart + spanH},
                             {viewLo_, v0}, {viewHi_, v1});
            } else {
                constexpr float kMinBinFrac = 0.001f;
                float logMin2 = std::log10(kMinBinFrac);
                float logMax2 = 0.0f;
                int numStrips = std::min(512, static_cast<int>(availW));
                for (int s = 0; s < numStrips; ++s) {
                    float sL = static_cast<float>(s) / numStrips;
                    float sR = static_cast<float>(s + 1) / numStrips;
                    float vfL = viewLo_ + sL * (viewHi_ - viewLo_);
                    float vfR = viewLo_ + sR * (viewHi_ - viewLo_);
                    float uL = std::pow(10.0f, logMin2 + vfL * (logMax2 - logMin2));
                    float uR = std::pow(10.0f, logMin2 + vfR * (logMax2 - logMin2));
                    dl->AddImage(texID,
                                 {pos.x + sL * availW, yStart},
                                 {pos.x + sR * availW, yStart + spanH},
                                 {uL, v0}, {uR, v1});
                }
            }
        };

        // From newestRow, walk forward (increasing index mod h) for
        // screenRows steps to cover newest→oldest.
        // Use availH for the screen extent so there's no fractional pixel gap.
        float pxPerRow = availH / static_cast<float>(screenRows);

        if (newestRow + screenRows <= h) {
            drawSpan(newestRow, screenRows, screenY, availH);
        } else {
            int firstCount = h - newestRow;
            float firstH = firstCount * pxPerRow;
            drawSpan(newestRow, firstCount, screenY, firstH);

            int secondCount = screenRows - firstCount;
            if (secondCount > 0)
                drawSpan(0, secondCount, screenY + firstH, availH - firstH);
        }

        // ── Frequency axis labels ──
        ImU32 textCol = IM_COL32(180, 180, 200, 200);
        double freqFullMin = settings_.isIQ ? -settings_.sampleRate / 2.0 : 0.0;
        double freqFullMax = settings_.isIQ ?  settings_.sampleRate / 2.0 : settings_.sampleRate / 2.0;

        // Map a view fraction to frequency.  In log mode, viewLo_/viewHi_
        // are in screen-fraction space; convert via the log mapping.
        auto viewFracToFreq = [&](float vf) -> double {
            if (logMode) {
                constexpr float kMinBinFrac = 0.001f;
                float logMin2 = std::log10(kMinBinFrac);
                float logMax2 = 0.0f;
                float binFrac = std::pow(10.0f, logMin2 + vf * (logMax2 - logMin2));
                return freqFullMin + binFrac * (freqFullMax - freqFullMin);
            }
            return freqFullMin + vf * (freqFullMax - freqFullMin);
        };

        int numLabels = 8;
        for (int i = 0; i <= numLabels; ++i) {
            float frac = static_cast<float>(i) / numLabels;
            float vf = viewLo_ + frac * (viewHi_ - viewLo_);
            double freq = viewFracToFreq(vf);
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

        // Store waterfall geometry for cross-panel cursor drawing.
        wfPosX_ = pos.x; wfPosY_ = pos.y; wfSizeX_ = availW; wfSizeY_ = availH;

        // ── Mouse interaction: zoom, pan & hover on waterfall ──
        ImGuiIO& io = ImGui::GetIO();
        float mx = io.MousePos.x;
        float my = io.MousePos.y;
        bool inWaterfall = mx >= pos.x && mx <= pos.x + availW &&
                           my >= pos.y && my <= pos.y + availH;

        // Hover cursor from waterfall
        if (inWaterfall) {
            double freq = specDisplay_.screenXToFreq(mx, pos.x, availW,
                                                      settings_.sampleRate,
                                                      settings_.isIQ, freqScale_,
                                                      viewLo_, viewHi_);
            int bins = analyzer_.spectrumSize();
            double fMin = settings_.isIQ ? -settings_.sampleRate / 2.0 : 0.0;
            double fMax = settings_.isIQ ?  settings_.sampleRate / 2.0 : settings_.sampleRate / 2.0;
            int bin = static_cast<int>((freq - fMin) / (fMax - fMin) * (bins - 1));
            bin = std::clamp(bin, 0, bins - 1);

            int curCh = std::clamp(waterfallChannel_, 0, analyzer_.numSpectra() - 1);
            const auto& spec = analyzer_.channelSpectrum(curCh);
            if (!spec.empty()) {
                cursors_.hover = {true, freq, spec[bin], bin};
            }
        }

        if (inWaterfall) {
            // Scroll wheel: zoom centered on cursor
            if (io.MouseWheel != 0) {
                float cursorFrac = (mx - pos.x) / availW;  // 0..1 on screen
                float viewFrac = viewLo_ + cursorFrac * (viewHi_ - viewLo_);

                float zoomFactor = (io.MouseWheel > 0) ? 0.85f : 1.0f / 0.85f;
                float newSpan = (viewHi_ - viewLo_) * zoomFactor;
                newSpan = std::clamp(newSpan, 0.001f, 1.0f);

                float newLo = viewFrac - cursorFrac * newSpan;
                float newHi = newLo + newSpan;

                // Clamp to [0, 1]
                if (newLo < 0.0f) { newHi -= newLo; newLo = 0.0f; }
                if (newHi > 1.0f) { newLo -= (newHi - 1.0f); newHi = 1.0f; }
                viewLo_ = std::clamp(newLo, 0.0f, 1.0f);
                viewHi_ = std::clamp(newHi, 0.0f, 1.0f);
            }

            // Middle-click + drag: pan
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.0f)) {
                float dx = io.MouseDelta.x;
                float panFrac = -dx / availW * (viewHi_ - viewLo_);
                float newLo = viewLo_ + panFrac;
                float newHi = viewHi_ + panFrac;
                float span = viewHi_ - viewLo_;
                if (newLo < 0.0f) { newLo = 0.0f; newHi = span; }
                if (newHi > 1.0f) { newHi = 1.0f; newLo = 1.0f - span; }
                viewLo_ = newLo;
                viewHi_ = newHi;
            }

            // Double-click: reset zoom
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle)) {
                viewLo_ = 0.0f;
                viewHi_ = 1.0f;
            }
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
                                                  settings_.isIQ, freqScale_,
                                                  viewLo_, viewHi_);
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
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            int peakBin = cursors_.findLocalPeak(spec, bin, 10);
            double peakFreq = analyzer_.binToFreq(peakBin);
            cursors_.setCursorA(peakFreq, spec[peakBin], peakBin);
        }
        // Right click: cursor B
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            int peakBin = cursors_.findLocalPeak(spec, bin, 10);
            double peakFreq = analyzer_.binToFreq(peakBin);
            cursors_.setCursorB(peakFreq, spec[peakBin], peakBin);
        }

        {
            // Ctrl+Scroll or Shift+Scroll: zoom dB range
            if (io.MouseWheel != 0 && (io.KeyCtrl || io.KeyShift)) {
                float zoom = io.MouseWheel * 5.0f;
                minDB_ += zoom;
                maxDB_ -= zoom;
                if (maxDB_ - minDB_ < 10.0f) {
                    float mid = (minDB_ + maxDB_) / 2.0f;
                    minDB_ = mid - 5.0f;
                    maxDB_ = mid + 5.0f;
                }
            }
            // Scroll (no modifier): zoom frequency axis centered on cursor
            else if (io.MouseWheel != 0) {
                float cursorFrac = (mx - posX) / sizeX;
                float viewFrac = viewLo_ + cursorFrac * (viewHi_ - viewLo_);

                float zoomFactor = (io.MouseWheel > 0) ? 0.85f : 1.0f / 0.85f;
                float newSpan = (viewHi_ - viewLo_) * zoomFactor;
                newSpan = std::clamp(newSpan, 0.001f, 1.0f);

                float newLo = viewFrac - cursorFrac * newSpan;
                float newHi = newLo + newSpan;

                if (newLo < 0.0f) { newHi -= newLo; newLo = 0.0f; }
                if (newHi > 1.0f) { newLo -= (newHi - 1.0f); newHi = 1.0f; }
                viewLo_ = std::clamp(newLo, 0.0f, 1.0f);
                viewHi_ = std::clamp(newHi, 0.0f, 1.0f);
            }

            // Middle-click + drag: pan
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.0f)) {
                float dx = io.MouseDelta.x;
                float panFrac = -dx / sizeX * (viewHi_ - viewLo_);
                float newLo = viewLo_ + panFrac;
                float newHi = viewHi_ + panFrac;
                float span = viewHi_ - viewLo_;
                if (newLo < 0.0f) { newLo = 0.0f; newHi = span; }
                if (newHi > 1.0f) { newHi = 1.0f; newLo = 1.0f - span; }
                viewLo_ = newLo;
                viewHi_ = newHi;
            }

            // Double middle-click: reset zoom
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle)) {
                viewLo_ = 0.0f;
                viewHi_ = 1.0f;
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
        int reinitH = std::max(1024, waterfall_.height());
        int binCount2 = std::max(1, analyzer_.spectrumSize());
        waterfall_.init(binCount2, reinitH);
    }
}

// ── Math channels ────────────────────────────────────────────────────────────

void Application::computeMathChannels() {
    int nPhys = analyzer_.numSpectra();
    int specSz = analyzer_.spectrumSize();
    mathSpectra_.resize(mathChannels_.size());

    for (size_t mi = 0; mi < mathChannels_.size(); ++mi) {
        const auto& mc = mathChannels_[mi];
        auto& out = mathSpectra_[mi];
        out.resize(specSz);

        if (!mc.enabled) {
            std::fill(out.begin(), out.end(), -200.0f);
            continue;
        }

        int sx = std::clamp(mc.sourceX, 0, nPhys - 1);
        int sy = std::clamp(mc.sourceY, 0, nPhys - 1);
        const auto& xDB = analyzer_.channelSpectrum(sx);
        const auto& yDB = analyzer_.channelSpectrum(sy);
        const auto& xC  = analyzer_.channelComplex(sx);
        const auto& yC  = analyzer_.channelComplex(sy);

        for (int i = 0; i < specSz; ++i) {
            float val = -200.0f;
            switch (mc.op) {
                // ── Unary ──
                case MathOp::Negate:
                    val = -xDB[i];
                    break;
                case MathOp::Absolute:
                    val = std::abs(xDB[i]);
                    break;
                case MathOp::Square:
                    val = 2.0f * xDB[i];
                    break;
                case MathOp::Cube:
                    val = 3.0f * xDB[i];
                    break;
                case MathOp::Sqrt:
                    val = 0.5f * xDB[i];
                    break;
                case MathOp::Log: {
                    // log10 of linear magnitude, back to dB-like scale.
                    float lin = std::pow(10.0f, xDB[i] / 10.0f);
                    float l = std::log10(lin + 1e-30f);
                    val = 10.0f * l;  // keep in dB-like range
                    break;
                }
                // ── Binary ──
                case MathOp::Add: {
                    float lx = std::pow(10.0f, xDB[i] / 10.0f);
                    float ly = std::pow(10.0f, yDB[i] / 10.0f);
                    float s = lx + ly;
                    val = (s > 1e-20f) ? 10.0f * std::log10(s) : -200.0f;
                    break;
                }
                case MathOp::Subtract: {
                    float lx = std::pow(10.0f, xDB[i] / 10.0f);
                    float ly = std::pow(10.0f, yDB[i] / 10.0f);
                    float d = std::abs(lx - ly);
                    val = (d > 1e-20f) ? 10.0f * std::log10(d) : -200.0f;
                    break;
                }
                case MathOp::Multiply:
                    val = xDB[i] + yDB[i];
                    break;
                case MathOp::Phase: {
                    if (i < static_cast<int>(xC.size()) &&
                        i < static_cast<int>(yC.size())) {
                        auto cross = xC[i] * std::conj(yC[i]);
                        float deg = std::atan2(cross.imag(), cross.real())
                                    * (180.0f / 3.14159265f);
                        // Map [-180, 180] degrees into the dB display range
                        // so it's visible on the plot.
                        val = deg;
                    }
                    break;
                }
                case MathOp::CrossCorr: {
                    if (i < static_cast<int>(xC.size()) &&
                        i < static_cast<int>(yC.size())) {
                        auto cross = xC[i] * std::conj(yC[i]);
                        float mag2 = std::norm(cross);
                        val = (mag2 > 1e-20f) ? 10.0f * std::log10(mag2) : -200.0f;
                    }
                    break;
                }
                default: break;
            }
            out[i] = val;
        }
    }
}

void Application::renderMathPanel() {
    ImGui::Text("Channel Math");
    ImGui::Separator();

    int nPhys = analyzer_.numSpectra();

    // Build source channel name list.
    static const char* chNames[] = {
        "Ch 0 (L)", "Ch 1 (R)", "Ch 2", "Ch 3", "Ch 4", "Ch 5", "Ch 6", "Ch 7"
    };

    // List existing math channels.
    int toRemove = -1;
    for (int mi = 0; mi < static_cast<int>(mathChannels_.size()); ++mi) {
        auto& mc = mathChannels_[mi];
        ImGui::PushID(1000 + mi);

        ImGui::Checkbox("##en", &mc.enabled);
        ImGui::SameLine();
        ImGui::ColorEdit3("##col", &mc.color.x, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();

        // Operation combo.
        if (ImGui::BeginCombo("##op", mathOpName(mc.op), ImGuiComboFlags_NoPreview)) {
            for (int o = 0; o < static_cast<int>(MathOp::Count); ++o) {
                auto op = static_cast<MathOp>(o);
                if (ImGui::Selectable(mathOpName(op), mc.op == op))
                    mc.op = op;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::Text("%s", mathOpName(mc.op));

        // Source X.
        ImGui::SetNextItemWidth(80);
        ImGui::Combo("X", &mc.sourceX, chNames, std::min(nPhys, kMaxChannels));

        // Source Y (only for binary ops).
        if (mathOpIsBinary(mc.op)) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::Combo("Y", &mc.sourceY, chNames, std::min(nPhys, kMaxChannels));
        }

        ImGui::SameLine();
        ImGui::Checkbox("WF", &mc.waterfall);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Show on waterfall");
        ImGui::SameLine();
        if (ImGui::SmallButton("X##del"))
            toRemove = mi;

        ImGui::PopID();
    }

    if (toRemove >= 0)
        mathChannels_.erase(mathChannels_.begin() + toRemove);

    if (ImGui::Button("+ Add Math Channel")) {
        MathChannel mc;
        mc.op = MathOp::Subtract;
        mc.sourceX = 0;
        mc.sourceY = std::min(1, nPhys - 1);
        mc.color = {1.0f, 1.0f, 0.5f, 1.0f};
        mathChannels_.push_back(mc);
    }
}

} // namespace baudline
