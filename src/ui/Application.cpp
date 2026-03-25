#include "ui/Application.h"
#include "audio/FileSource.h"

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <GLES2/gl2.h>
#else
#include <GL/gl.h>
#endif
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

#ifdef __EMSCRIPTEN__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#endif
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
#ifdef __EMSCRIPTEN__
    ImGui_ImplOpenGL3_Init("#version 100");
#else
    ImGui_ImplOpenGL3_Init("#version 120");
#endif

    // Enumerate audio devices
    paDevices_ = MiniAudioSource::listInputDevices();

    // Load saved config (overwrites defaults for FFT size, overlap, window, etc.)
    loadConfig();

    // Apply loaded settings
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

void Application::mainLoopStep() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);
        if (event.type == SDL_QUIT)
            running_ = false;
        if (event.type == SDL_KEYDOWN) {
            auto key = event.key.keysym.sym;
#ifndef __EMSCRIPTEN__
            if (key == SDLK_ESCAPE) running_ = false;
#endif
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

#ifdef __EMSCRIPTEN__
static void emMainLoop(void* arg) {
    static_cast<Application*>(arg)->mainLoopStep();
}
#endif

void Application::run() {
#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop_arg(emMainLoop, this, 0, true);
#else
    while (running_) {
        mainLoopStep();
    }
#endif
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
            measurements_.update(analyzer_.channelSpectrum(curCh),
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

    hoverPanel_ = HoverPanel::None;

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
        // Sidebar toggle (leftmost)
        if (ImGui::Button(showSidebar_ ? "  <<  " : "  >>  ")) {
            showSidebar_ = !showSidebar_;
            saveConfig();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(showSidebar_ ? "Hide sidebar" : "Show sidebar");

        ImGui::Separator();

        if (ImGui::BeginMenu("File")) {
            // ── File input ──
            static char filePathBuf[512] = "";
            if (filePath_.size() < sizeof(filePathBuf))
                std::strncpy(filePathBuf, filePath_.c_str(), sizeof(filePathBuf) - 1);
            ImGui::SetNextItemWidth(200);
            if (ImGui::InputText("Path", filePathBuf, sizeof(filePathBuf)))
                filePath_ = filePathBuf;

            const char* formatNames[] = {"Float32 I/Q", "Int16 I/Q", "Uint8 I/Q", "WAV"};
            ImGui::SetNextItemWidth(140);
            ImGui::Combo("Format", &fileFormatIdx_, formatNames, 4);

            ImGui::SetNextItemWidth(140);
            ImGui::DragFloat("Sample Rate", &fileSampleRate_, 1000.0f, 1000.0f, 100e6f, "%.0f Hz");
            ImGui::Checkbox("Loop", &fileLoop_);

            if (ImGui::MenuItem("Open File")) {
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

            ImGui::Separator();

            // ── Audio device ──
            if (!paDevices_.empty()) {
                ImGui::Text("Audio Device");
                std::vector<const char*> devNames;
                for (auto& d : paDevices_) devNames.push_back(d.name.c_str());
                ImGui::SetNextItemWidth(250);
                if (ImGui::Combo("##device", &paDeviceIdx_, devNames.data(),
                                 static_cast<int>(devNames.size()))) {
                    openPortAudio();
                    updateAnalyzerSettings();
                    saveConfig();
                }
            }
            if (ImGui::MenuItem("Open Audio Device")) {
                openPortAudio();
                updateAnalyzerSettings();
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Esc")) running_ = false;
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Grid", nullptr, &specDisplay_.showGrid);
            ImGui::MenuItem("Fill Spectrum", nullptr, &specDisplay_.fillSpectrum);

            ImGui::Separator();

            // Frequency scale
            int fs = static_cast<int>(freqScale_);
            const char* fsNames[] = {"Linear", "Logarithmic"};
            ImGui::SetNextItemWidth(120);
            if (ImGui::Combo("Freq Scale", &fs, fsNames, 2)) {
                freqScale_ = static_cast<FreqScale>(fs);
                saveConfig();
            }

            ImGui::Separator();
            if (ImGui::MenuItem("VSync", nullptr, &vsync_)) {
                SDL_GL_SetSwapInterval(vsync_ ? 1 : 0);
                saveConfig();
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

        // Right-aligned status in menu bar
        {
            float barW = ImGui::GetWindowWidth();
            char statusBuf[128];
            std::snprintf(statusBuf, sizeof(statusBuf), "%.0f Hz | %d pt | %.1f Hz/bin | %.0f FPS",
                          settings_.sampleRate, settings_.fftSize,
                          settings_.sampleRate / settings_.fftSize,
                          ImGui::GetIO().Framerate);
            ImVec2 textSz = ImGui::CalcTextSize(statusBuf);
            ImGui::SameLine(barW - textSz.x - 16);
            ImGui::TextDisabled("%s", statusBuf);
        }

        ImGui::EndMenuBar();
    }

    // Layout
    float totalW = ImGui::GetContentRegionAvail().x;
    float contentH = ImGui::GetContentRegionAvail().y;
    float controlW = showSidebar_ ? 270.0f : 0.0f;
    float contentW = totalW - (showSidebar_ ? controlW + 8 : 0);

    // Control panel (sidebar)
    if (showSidebar_) {
        ImGui::BeginChild("Controls", {controlW, contentH}, true);
        renderControlPanel();
        ImGui::EndChild();
        ImGui::SameLine();
    }

    // Waterfall (top) + Spectrum (bottom) with draggable splitter
    ImGui::BeginChild("Display", {contentW, contentH}, false);
    {
        constexpr float kSplitterH = 6.0f;

        renderWaterfallPanel();

        // ── Draggable splitter bar ──
        ImVec2 splPos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##splitter", {contentW, kSplitterH});
        bool hovered = ImGui::IsItemHovered();
        bool active  = ImGui::IsItemActive();

        if (hovered || active)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

        if (active) {
            float dy = ImGui::GetIO().MouseDelta.y;
            // Dragging down = more waterfall = less spectrum
            spectrumFrac_ -= dy / contentH;
            spectrumFrac_ = std::clamp(spectrumFrac_, 0.1f, 0.9f);
            draggingSplit_ = true;
        } else if (draggingSplit_) {
            draggingSplit_ = false;
            saveConfig();
        }

        // Draw splitter line
        ImU32 splCol = (hovered || active)
            ? IM_COL32(100, 150, 255, 220)
            : IM_COL32(80, 80, 100, 150);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float cy = splPos.y + kSplitterH * 0.5f;
        dl->AddLine({splPos.x, cy}, {splPos.x + contentW, cy}, splCol, 2.0f);

        renderSpectrumPanel();

        // ── Cross-panel hover line & frequency label ──
        if (cursors_.hover.active && specSizeX_ > 0 && wfSizeX_ > 0) {
            ImDrawList* dlp = ImGui::GetWindowDrawList();
            float hx = specDisplay_.freqToScreenX(cursors_.hover.freq,
                           specPosX_, specSizeX_, settings_.sampleRate,
                           settings_.isIQ, freqScale_, viewLo_, viewHi_);
            ImU32 hoverCol = IM_COL32(200, 200, 200, 80);

            // Line in spectrum area only
            dlp->AddLine({hx, specPosY_}, {hx, specPosY_ + specSizeY_}, hoverCol, 1.0f);

            // Frequency label at top of waterfall
            char freqLabel[48];
            fmtFreq(freqLabel, sizeof(freqLabel), cursors_.hover.freq);

            ImVec2 tSz = ImGui::CalcTextSize(freqLabel);
            float lx = std::min(hx + 4, wfPosX_ + wfSizeX_ - tSz.x - 4);
            float ly = wfPosY_ + 2;
            dlp->AddRectFilled({lx - 2, ly - 1}, {lx + tSz.x + 2, ly + tSz.y + 1},
                               IM_COL32(0, 0, 0, 180));
            dlp->AddText({lx, ly}, IM_COL32(220, 220, 240, 240), freqLabel);

            // ── Hover info (right side of spectrum/waterfall) ──
            {
                // Use bin center for frequency
                int bins = analyzer_.spectrumSize();
                double fMin = settings_.isIQ ? -settings_.sampleRate / 2.0 : 0.0;
                double fMax = settings_.isIQ ?  settings_.sampleRate / 2.0 : settings_.sampleRate / 2.0;
                double binCenterFreq = fMin + (static_cast<double>(cursors_.hover.bin) + 0.5)
                                       / bins * (fMax - fMin);

                char hoverBuf[128];
                if (hoverPanel_ == HoverPanel::Spectrum) {
                    fmtFreqDB(hoverBuf, sizeof(hoverBuf), "", binCenterFreq, cursors_.hover.dB);
                } else if (hoverPanel_ == HoverPanel::Waterfall) {
                    fmtFreqTime(hoverBuf, sizeof(hoverBuf), "", binCenterFreq, -hoverWfTimeOffset_);
                } else {
                    fmtFreq(hoverBuf, sizeof(hoverBuf), binCenterFreq);
                }

                // Right-align the text
                ImU32 hoverTextCol = IM_COL32(100, 230, 130, 240);
                float rightEdge = specPosX_ + specSizeX_ - 8;
                float hy2 = specPosY_ + 4;
                ImVec2 hSz = ImGui::CalcTextSize(hoverBuf);
                dlp->AddText({rightEdge - hSz.x, hy2}, hoverTextCol, hoverBuf);
            }
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
    // ── Playback ──
    float btnW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;
    if (ImGui::Button(paused_ ? "Resume" : "Pause", {btnW, 0}))
        paused_ = !paused_;
    ImGui::SameLine();
    if (ImGui::Button("Clear", {btnW, 0}))
        analyzer_.clearHistory();
    ImGui::SameLine();
    if (ImGui::Button("Peak", {btnW, 0})) {
        int pkCh = std::clamp(waterfallChannel_, 0, analyzer_.numSpectra() - 1);
        cursors_.snapToPeak(analyzer_.channelSpectrum(pkCh),
                            settings_.sampleRate, settings_.isIQ,
                            settings_.fftSize);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Snap cursor A to peak");

    // ── FFT ──
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("FFT", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* sizeNames[] = {"256", "512", "1024", "2048", "4096",
                                   "8192", "16384", "32768", "65536"};
        float availSpace = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x);
        ImGui::SetNextItemWidth(availSpace * 0.35f);
        if (ImGui::Combo("##fftsize", &fftSizeIdx_, sizeNames, kNumFFTSizes)) {
            settings_.fftSize = kFFTSizes[fftSizeIdx_];
            updateAnalyzerSettings();
            saveConfig();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("FFT Size");

        ImGui::SameLine();
        const char* winNames[] = {"Rectangular", "Hann", "Hamming", "Blackman",
                                  "Blackman-Harris", "Kaiser", "Flat Top"};
        ImGui::SetNextItemWidth(availSpace * 0.65f);
        if (ImGui::Combo("##window", &windowIdx_, winNames,
                         static_cast<int>(WindowType::Count))) {
            settings_.window = static_cast<WindowType>(windowIdx_);
            updateAnalyzerSettings();
            saveConfig();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Window Function");

        if (settings_.window == WindowType::Kaiser) {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##kaiser", &settings_.kaiserBeta, 0.0f, 20.0f, "Kaiser: %.1f"))
                updateAnalyzerSettings();
        }

        // Overlap
        {
            int hopSamples = static_cast<int>(settings_.fftSize * (1.0f - settings_.overlap));
            if (hopSamples < 1) hopSamples = 1;
            int overlapSamples = settings_.fftSize - hopSamples;

            ImGui::SetNextItemWidth(-1);
            float sliderVal = 1.0f - std::pow(1.0f - overlapPct_ / 99.0f, 0.25f);
            if (ImGui::SliderFloat("##overlap", &sliderVal, 0.0f, 1.0f, "")) {
                float inv = 1.0f - sliderVal;
                float inv2 = inv * inv;
                overlapPct_ = 99.0f * (1.0f - inv2 * inv2);
                settings_.overlap = overlapPct_ / 100.0f;
                updateAnalyzerSettings();
                saveConfig();
            }

            char overlayText[64];
            std::snprintf(overlayText, sizeof(overlayText), "%.1f%% (%d samples)", overlapPct_, overlapSamples);
            ImVec2 textSize = ImGui::CalcTextSize(overlayText);
            ImVec2 rMin = ImGui::GetItemRectMin();
            ImVec2 rMax = ImGui::GetItemRectMax();
            float tx = rMin.x + ((rMax.x - rMin.x) - textSize.x) * 0.5f;
            float ty = rMin.y + ((rMax.y - rMin.y) - textSize.y) * 0.5f;
            ImGui::GetWindowDrawList()->AddText({tx, ty}, IM_COL32(255, 255, 255, 220), overlayText);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Overlap");
        }
    }

    // ── Display ──
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloatRange2("##dbrange", &minDB_, &maxDB_, 1.0f, -200.0f, 20.0f,
                               "Min: %.0f dB", "Max: %.0f dB");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("dB Range (min / max)");

        ImGui::Checkbox("Peak Hold", &specDisplay_.peakHoldEnable);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Draws a \"maximum\" line in the spectrogram");
        if (specDisplay_.peakHoldEnable) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x
                                    - ImGui::CalcTextSize("Clear").x
                                    - ImGui::GetStyle().ItemSpacing.x
                                    - ImGui::GetStyle().FramePadding.x * 2);
            ImGui::SliderFloat("##decay", &specDisplay_.peakHoldDecay, 0.0f, 120.0f, "%.0f dB/s");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decay rate");
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear##peakhold"))
                specDisplay_.clearPeakHold();
        }

        {
            float span = viewHi_ - viewLo_;
            float zoomX = 1.0f / span;
            float resetBtnW = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2;
            float zoomLabelW = ImGui::CalcTextSize("Zoom:").x + ImGui::GetStyle().ItemSpacing.x;
            float sliderW = ImGui::GetContentRegionAvail().x - zoomLabelW - resetBtnW - ImGui::GetStyle().ItemSpacing.x;
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Zoom:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(sliderW);
            if (ImGui::SliderFloat("##zoom", &zoomX, 1.0f, 200.0f, "%.1fx", ImGuiSliderFlags_Logarithmic)) {
                zoomX = std::clamp(zoomX, 1.0f, 1000.0f);
                float newSpan = 1.0f / zoomX;
                viewLo_ = 0.0f;
                viewHi_ = std::clamp(newSpan, 0.0f, 1.0f);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##zoom")) {
                viewLo_ = 0.0f;
                viewHi_ = 0.5f;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset to 2x zoom");
        }
    }

    // ── Channels ──
    ImGui::Spacing();
    {
        int nCh = analyzer_.numSpectra();
        bool isMulti = waterfallMultiCh_ && nCh > 1;

        // Header with inline Single/Multi toggle
        float widgetW = (nCh > 1) ? 60.0f : 0.0f;
        float gap = ImGui::GetStyle().ItemSpacing.x * 0.25f;
        ImVec2 hdrMin = ImGui::GetCursorScreenPos();
        float winLeft = ImGui::GetWindowPos().x;
        float hdrRight = hdrMin.x + ImGui::GetContentRegionAvail().x;
        ImGui::PushClipRect({winLeft, hdrMin.y}, {hdrRight - widgetW - gap, hdrMin.y + 200}, true);
        bool headerOpen = ImGui::CollapsingHeader("##channels_hdr",
                                                   ImGuiTreeNodeFlags_DefaultOpen |
                                                   ImGuiTreeNodeFlags_AllowOverlap);
        ImGui::PopClipRect();
        ImGui::SameLine();
        ImGui::Text("Channels");
        if (nCh > 1) {
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - widgetW + ImGui::GetStyle().FramePadding.x);
            if (ImGui::Button(isMulti ? " Multi " : "Single ", {widgetW, 0})) {
                waterfallMultiCh_ = !waterfallMultiCh_;
            }
        }

        if (headerOpen) {
            if (isMulti) {
                // Multi-channel: per-channel colors and enable
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
            } else {
                // Single-channel: color map + channel selector
                const char* cmNames[] = {"Magma", "Viridis", "Inferno", "Plasma", "Grayscale"};
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##colormap", &colorMapIdx_, cmNames,
                                 static_cast<int>(ColorMapType::Count))) {
                    colorMap_.setType(static_cast<ColorMapType>(colorMapIdx_));
                    waterfall_.setColorMap(colorMap_);
                    saveConfig();
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Color Map");

                if (nCh > 1) {
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::SliderInt("##wfch", &waterfallChannel_, 0, nCh - 1))
                        waterfallChannel_ = std::clamp(waterfallChannel_, 0, nCh - 1);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Waterfall Channel");
                }
            }
        }
    }

    // ── Math ──
    ImGui::Spacing();
    {
        float btnW = ImGui::GetFrameHeight();
        float gap = ImGui::GetStyle().ItemSpacing.x * 0.25f;
        ImVec2 hdrMin = ImGui::GetCursorScreenPos();
        float winLeft = ImGui::GetWindowPos().x;
        float hdrRight = hdrMin.x + ImGui::GetContentRegionAvail().x;
        ImGui::PushClipRect({winLeft, hdrMin.y}, {hdrRight - btnW - gap, hdrMin.y + 200}, true);
        bool mathOpen = ImGui::CollapsingHeader("##math_hdr",
                                                 ImGuiTreeNodeFlags_DefaultOpen |
                                                 ImGuiTreeNodeFlags_AllowOverlap);
        ImGui::PopClipRect();
        ImGui::SameLine();
        ImGui::Text("Math");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - btnW + ImGui::GetStyle().FramePadding.x);
        if (ImGui::Button("+##addmath", {btnW, 0})) {
            int nPhys = analyzer_.numSpectra();
            MathChannel mc;
            mc.op = MathOp::Subtract;
            mc.sourceX = 0;
            mc.sourceY = std::min(1, nPhys - 1);
            mc.color = {1.0f, 1.0f, 0.5f, 1.0f};
            mathChannels_.push_back(mc);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add math channel");

        if (mathOpen) {
            renderMathPanel();
        }
    }

    // ── Cursors ──
    ImGui::Spacing();
    {
        float btnW = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2;
        float gap = ImGui::GetStyle().ItemSpacing.x * 0.25f;
        ImVec2 hdrMin = ImGui::GetCursorScreenPos();
        float winLeft = ImGui::GetWindowPos().x;
        float hdrRight = hdrMin.x + ImGui::GetContentRegionAvail().x;
        ImGui::PushClipRect({winLeft, hdrMin.y}, {hdrRight - btnW - gap, hdrMin.y + 200}, true);
        bool cursorsOpen = ImGui::CollapsingHeader("##cursors_hdr",
                                                    ImGuiTreeNodeFlags_DefaultOpen |
                                                    ImGuiTreeNodeFlags_AllowOverlap);
        ImGui::PopClipRect();
        ImGui::SameLine();
        ImGui::Text("Cursors");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - btnW + ImGui::GetStyle().FramePadding.x);
        if (ImGui::SmallButton("Reset##cursors")) {
            cursors_.cursorA.active = false;
            cursors_.cursorB.active = false;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear cursors A and B");

        if (cursorsOpen) {
            bool prevSnap = cursors_.snapToPeaks;
            cursors_.drawPanel();
            if (cursors_.snapToPeaks != prevSnap) saveConfig();
        }
    }

    // ── Measurements ──
    ImGui::Spacing();
    {
        float cbW = ImGui::GetFrameHeight();
        float gap = ImGui::GetStyle().ItemSpacing.x * 0.25f;
        ImVec2 hdrMin = ImGui::GetCursorScreenPos();
        float winLeft = ImGui::GetWindowPos().x;
        float hdrRight = hdrMin.x + ImGui::GetContentRegionAvail().x;
        ImGui::PushClipRect({winLeft, hdrMin.y}, {hdrRight - cbW - gap, hdrMin.y + 200}, true);
        bool headerOpen = ImGui::CollapsingHeader("##meas_hdr",
                                                   ImGuiTreeNodeFlags_DefaultOpen |
                                                   ImGuiTreeNodeFlags_AllowOverlap);
        ImGui::PopClipRect();
        ImGui::SameLine();
        ImGui::Text("Measurements");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - cbW + ImGui::GetStyle().FramePadding.x);
        ImGui::Checkbox("##meas_en", &measurements_.enabled);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable measurements");

        if (headerOpen) {
            measurements_.drawPanel();
        }
    }

    // ── Status (bottom) ──
    ImGui::Separator();
    ImGui::TextDisabled("Mode: %s", settings_.isIQ ? "I/Q"
                        : (settings_.numChannels > 1 ? "Multi-ch" : "Real"));

}

void Application::renderSpectrumPanel() {
    float availW = ImGui::GetContentRegionAvail().x;
    // Spectrum is at the bottom — use all remaining height after waterfall + splitter.
    float specH = ImGui::GetContentRegionAvail().y;

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

    measurements_.draw(specDisplay_, specPosX_, specPosY_, specSizeX_, specSizeY_,
                       settings_.sampleRate, settings_.isIQ, freqScale_, minDB_, maxDB_,
                       viewLo_, viewHi_);

    handleSpectrumInput(specPosX_, specPosY_, specSizeX_, specSizeY_);

    ImGui::Dummy({availW, specH});
}

void Application::renderWaterfallPanel() {
    float availW = ImGui::GetContentRegionAvail().x;
    // Waterfall is at the top — compute height from the split fraction.
    constexpr float kSplitterH = 6.0f;
    float parentH = ImGui::GetContentRegionAvail().y;
    float availH = (parentH - kSplitterH) * (1.0f - spectrumFrac_);

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
        int screenRows = std::min(static_cast<int>(availH), h);

        // Newest row index in the circular buffer.
        int newestRow = (waterfall_.currentRow() + 1) % h;

        // Render 1:1 (one texture row = one screen pixel), bottom-aligned,
        // newest line at bottom, scrolling upward.
        //
        // We flip the V coordinates (v1 before v0) so that the vertical
        // direction is reversed: newest at the bottom of the draw region.
        float rowToV = 1.0f / h;

        bool logMode = (freqScale_ == FreqScale::Logarithmic && !settings_.isIQ);

        // drawSpan renders rows [rowStart..rowStart+rowCount) but with
        // flipped V so oldest is at top and newest at bottom.
        auto drawSpan = [&](int rowStart, int rowCount, float yStart, float spanH) {
            float v0 = rowStart * rowToV;
            float v1 = (rowStart + rowCount) * rowToV;

            // Flip: swap v0 and v1 so texture is vertically inverted
            if (!logMode) {
                dl->AddImage(texID,
                             {pos.x, yStart},
                             {pos.x + availW, yStart + spanH},
                             {viewLo_, v1}, {viewHi_, v0});
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
                                 {uL, v1}, {uR, v0});
                }
            }
        };

        // From newestRow, walk forward (increasing index mod h) for
        // screenRows steps to cover newest→oldest.
        // With V-flip, oldest rows render at the top, newest at the bottom.
        float pxPerRow = availH / static_cast<float>(screenRows);

        if (newestRow + screenRows <= h) {
            drawSpan(newestRow, screenRows, pos.y, availH);
        } else {
            // Wrap-around: two spans.  Because we flip V, the second span
            // (wrap-around, containing older rows) goes at the TOP.
            int firstCount = h - newestRow;  // rows newestRow..h-1
            int secondCount = screenRows - firstCount;  // rows 0..secondCount-1

            // Second span (older, wraps to index 0) at top
            float secondH = secondCount * pxPerRow;
            if (secondCount > 0)
                drawSpan(0, secondCount, pos.y, secondH);

            // First span (newer, includes newestRow) at bottom
            float firstH = availH - secondH;
            drawSpan(newestRow, firstCount, pos.y + secondH, firstH);
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

            dl->AddText({x + 2, pos.y + 2}, textCol, label);
        }

        // Store waterfall geometry for cross-panel cursor drawing.
        wfPosX_ = pos.x; wfPosY_ = pos.y; wfSizeX_ = availW; wfSizeY_ = availH;

        measurements_.drawWaterfall(specDisplay_, wfPosX_, wfPosY_, wfSizeX_, wfSizeY_,
                                     settings_.sampleRate, settings_.isIQ, freqScale_,
                                     viewLo_, viewHi_);

        // ── Mouse interaction: zoom, pan & hover on waterfall ──
        ImGuiIO& io = ImGui::GetIO();
        float mx = io.MousePos.x;
        float my = io.MousePos.y;
        bool inWaterfall = mx >= pos.x && mx <= pos.x + availW &&
                           my >= pos.y && my <= pos.y + availH;

        // Hover cursor from waterfall
        if (inWaterfall) {
            hoverPanel_ = HoverPanel::Waterfall;
            double freq = specDisplay_.screenXToFreq(mx, pos.x, availW,
                                                      settings_.sampleRate,
                                                      settings_.isIQ, freqScale_,
                                                      viewLo_, viewHi_);
            int bins = analyzer_.spectrumSize();
            double fMin = settings_.isIQ ? -settings_.sampleRate / 2.0 : 0.0;
            double fMax = settings_.isIQ ?  settings_.sampleRate / 2.0 : settings_.sampleRate / 2.0;
            int bin = static_cast<int>((freq - fMin) / (fMax - fMin) * (bins - 1));
            bin = std::clamp(bin, 0, bins - 1);

            // Time offset: bottom = newest (0s), top = oldest
            float yFrac = 1.0f - (my - pos.y) / availH;  // 0 at bottom, 1 at top
            int hopSamples = static_cast<int>(settings_.fftSize * (1.0f - settings_.overlap));
            if (hopSamples < 1) hopSamples = 1;
            double secondsPerLine = static_cast<double>(hopSamples) / settings_.sampleRate;
            hoverWfTimeOffset_ = static_cast<float>(yFrac * screenRows * secondsPerLine);

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
        hoverPanel_ = HoverPanel::Spectrum;
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

        // Left drag: cursor A
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            int cBin = cursors_.snapToPeaks ? cursors_.findLocalPeak(spec, bin, 10) : bin;
            double cFreq = analyzer_.binToFreq(cBin);
            cursors_.setCursorA(cFreq, spec[cBin], cBin);
        }
        // Right drag: cursor B
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            int cBin = cursors_.snapToPeaks ? cursors_.findLocalPeak(spec, bin, 10) : bin;
            double cFreq = analyzer_.binToFreq(cBin);
            cursors_.setCursorB(cFreq, spec[cBin], cBin);
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
        // Only clear hover if waterfall didn't already set it this frame
        if (hoverPanel_ != HoverPanel::Waterfall) {
            hoverPanel_ = HoverPanel::None;
            cursors_.hover.active = false;
        }
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
    auto src = std::make_unique<MiniAudioSource>(sr, reqCh, deviceIdx);
    if (src->open()) {
        audioSource_ = std::move(src);
        settings_.sampleRate = audioSource_->sampleRate();
        settings_.isIQ = false;
        settings_.numChannels = audioSource_->channels();
    } else {
        std::fprintf(stderr, "Failed to open audio device\n");
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
}

void Application::loadConfig() {
    config_.load();
    fftSizeIdx_   = config_.getInt("fft_size_idx", fftSizeIdx_);
    overlapPct_   = config_.getFloat("overlap_pct", overlapPct_);
    windowIdx_    = config_.getInt("window_idx", windowIdx_);
    colorMapIdx_  = config_.getInt("colormap_idx", colorMapIdx_);
    minDB_        = config_.getFloat("min_db", minDB_);
    maxDB_        = config_.getFloat("max_db", maxDB_);
    int fs        = config_.getInt("freq_scale", static_cast<int>(freqScale_));
    freqScale_    = static_cast<FreqScale>(fs);
    vsync_        = config_.getBool("vsync", vsync_);
    spectrumFrac_ = config_.getFloat("spectrum_frac", spectrumFrac_);
    showSidebar_  = config_.getBool("show_sidebar", showSidebar_);
    specDisplay_.peakHoldEnable = config_.getBool("peak_hold", specDisplay_.peakHoldEnable);
    specDisplay_.peakHoldDecay  = config_.getFloat("peak_hold_decay", specDisplay_.peakHoldDecay);
    cursors_.snapToPeaks        = config_.getBool("snap_to_peaks", cursors_.snapToPeaks);

    // Clamp
    fftSizeIdx_   = std::clamp(fftSizeIdx_, 0, kNumFFTSizes - 1);
    windowIdx_    = std::clamp(windowIdx_, 0, static_cast<int>(WindowType::Count) - 1);
    colorMapIdx_  = std::clamp(colorMapIdx_, 0, static_cast<int>(ColorMapType::Count) - 1);
    spectrumFrac_ = std::clamp(spectrumFrac_, 0.1f, 0.9f);

    // Find device by saved name.
    std::string devName = config_.getString("device_name", "");
    if (!devName.empty()) {
        for (int i = 0; i < static_cast<int>(paDevices_.size()); ++i) {
            if (paDevices_[i].name == devName) {
                paDeviceIdx_ = i;
                break;
            }
        }
    }

    // Apply
    settings_.fftSize = kFFTSizes[fftSizeIdx_];
    settings_.overlap = overlapPct_ / 100.0f;
    settings_.window  = static_cast<WindowType>(windowIdx_);
    colorMap_.setType(static_cast<ColorMapType>(colorMapIdx_));
    SDL_GL_SetSwapInterval(vsync_ ? 1 : 0);
}

void Application::saveConfig() const {
    Config cfg;
    cfg.setInt("fft_size_idx", fftSizeIdx_);
    cfg.setFloat("overlap_pct", overlapPct_);
    cfg.setInt("window_idx", windowIdx_);
    cfg.setInt("colormap_idx", colorMapIdx_);
    cfg.setFloat("min_db", minDB_);
    cfg.setFloat("max_db", maxDB_);
    cfg.setInt("freq_scale", static_cast<int>(freqScale_));
    cfg.setBool("vsync", vsync_);
    cfg.setFloat("spectrum_frac", spectrumFrac_);
    cfg.setBool("show_sidebar", showSidebar_);
    cfg.setBool("peak_hold", specDisplay_.peakHoldEnable);
    cfg.setFloat("peak_hold_decay", specDisplay_.peakHoldDecay);
    cfg.setBool("snap_to_peaks", cursors_.snapToPeaks);

    if (paDeviceIdx_ >= 0 && paDeviceIdx_ < static_cast<int>(paDevices_.size()))
        cfg.setString("device_name", paDevices_[paDeviceIdx_].name);

    cfg.save();
}

} // namespace baudline
