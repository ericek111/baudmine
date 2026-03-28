#include "ui/Application.h"

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES2/gl2.h>

EM_JS(void, js_toggleFullscreen, (), {
    if (document.fullscreenElement) {
        document.exitFullscreen();
    } else {
        document.documentElement.requestFullscreen();
    }
});

EM_JS(int, js_isFullscreen, (), {
    return document.fullscreenElement ? 1 : 0;
});

EM_JS(float, js_devicePixelRatio, (), {
    return window.devicePixelRatio || 1.0;
});

// SDL_CreateWindow sets inline width/height on the canvas which overrides
// the stylesheet's 100vw/100vh.  Clear them once so CSS stays in control.
EM_JS(void, js_clearCanvasInlineSize, (), {
    var c = document.getElementById('canvas');
    if (c) { c.style.width = ''; c.style.height = ''; }
});

#else
#include <GL/gl.h>
#endif
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace baudmine {

Application::Application() = default;

void Application::syncCanvasSize() {
#ifdef __EMSCRIPTEN__
    double cssW, cssH;
    emscripten_get_element_css_size("#canvas", &cssW, &cssH);
    float dpr = js_devicePixelRatio();
    int targetW = static_cast<int>(cssW * dpr + 0.5);
    int targetH = static_cast<int>(cssH * dpr + 0.5);
    int curW, curH;
    emscripten_get_canvas_element_size("#canvas", &curW, &curH);
    if (curW != targetW || curH != targetH) {
        emscripten_set_canvas_element_size("#canvas", targetW, targetH);
        glViewport(0, 0, targetW, targetH);
    }
    if (std::abs(dpr - lastDpr_) > 0.01f) {
        lastDpr_ = dpr;
        float scale = (uiScale_ > 0.0f) ? uiScale_ : dpr;
        applyUIScale(scale);
    }
#endif
}

void Application::applyUIScale(float scale) {
    scale = std::clamp(scale, 0.5f, 4.0f);
    if (std::abs(scale - appliedScale_) < 0.01f) return;
    appliedScale_ = scale;

    static ImGuiStyle baseStyle = [] {
        ImGuiStyle s;
        ImGui::StyleColorsDark(&s);
        s.WindowRounding = 4.0f;
        s.FrameRounding = 2.0f;
        s.GrabRounding = 2.0f;
        return s;
    }();

    float fbScale = 1.0f;
    int winW, winH, drawW, drawH;
    SDL_GetWindowSize(window_, &winW, &winH);
    SDL_GL_GetDrawableSize(window_, &drawW, &drawH);
    if (winW > 0) fbScale = static_cast<float>(drawW) / winW;

    logicalScale_ = scale / fbScale;

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig fc;
    fc.SizePixels = std::max(8.0f, 13.0f * scale);
    io.Fonts->AddFontDefault(&fc);
    io.Fonts->Build();
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    io.FontGlobalScale = 1.0f / fbScale;

    ImGui::GetStyle() = baseStyle;
    ImGui::GetStyle().ScaleAllSizes(logicalScale_);
}

void Application::requestUIScale(float scale) {
    pendingScale_ = scale;
}

Application::~Application() {
    shutdown();
}

bool Application::init(int argc, char** argv) {
    // Parse command line
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
            audio_.settings().isIQ = true;
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

    window_ = SDL_CreateWindow("Baudmine Spectrum Analyzer",
                               SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               1400, 900,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                               SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window_) {
        std::fprintf(stderr, "SDL_CreateWindow error: %s\n", SDL_GetError());
        return false;
    }

#ifdef __EMSCRIPTEN__
    js_clearCanvasInlineSize();
#endif

    glContext_ = SDL_GL_CreateContext(window_);
    SDL_GL_MakeCurrent(window_, glContext_);
    SDL_GL_SetSwapInterval(1);

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
    audio_.enumerateDevices();

    // Load saved config
    loadConfig();

    // Sync canvas to physical pixels before first frame (WASM)
    syncCanvasSize();

    // Apply DPI-aware UI scaling
    {
        float dpiScale = 1.0f;
#ifdef __EMSCRIPTEN__
        dpiScale = js_devicePixelRatio();
        lastDpr_ = dpiScale;
#else
        float ddpi = 0;
        if (SDL_GetDisplayDPI(0, &ddpi, nullptr, nullptr) == 0 && ddpi > 0)
            dpiScale = ddpi / 96.0f;
#endif
        float scale = (uiScale_ > 0.0f) ? uiScale_ : dpiScale;
        applyUIScale(scale);
    }

    // Apply loaded settings
    auto& settings = audio_.settings();
    settings.fftSize    = kFFTSizes[fftSizeIdx_];
    settings.overlap    = overlapPct_ / 100.0f;
    settings.window     = static_cast<WindowType>(windowIdx_);
    settings.sampleRate = fileSampleRate_;
    settings.isIQ       = false;

    // Open source
    if (!filePath_.empty()) {
        InputFormat fmt;
        switch (fileFormatIdx_) {
            case 0: fmt = InputFormat::Float32IQ; settings.isIQ = true; break;
            case 1: fmt = InputFormat::Int16IQ;   settings.isIQ = true; break;
            case 2: fmt = InputFormat::Uint8IQ;   settings.isIQ = true; break;
            default: fmt = InputFormat::WAV;      break;
        }
        openFile(filePath_, fmt, fileSampleRate_);
    } else {
        openDevice();
    }

    updateAnalyzerSettings();

    running_ = true;
    return true;
}

void Application::mainLoopStep() {
    syncCanvasSize();

    if (pendingScale_ > 0.0f) {
        applyUIScale(pendingScale_);
        pendingScale_ = 0.0f;
    }

    const auto& settings = audio_.settings();

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);
        handleTouchEvent(event);
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
                                      audio_.totalNumSpectra() - 1);
                cursors_.snapToPeak(audio_.getSpectrum(pkCh),
                                    settings.sampleRate, settings.isIQ,
                                    settings.fftSize);
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
    audio_.closeAll();

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
    if (!audio_.hasSource()) return;

    const auto& settings = audio_.settings();
    int spectraThisFrame = audio_.processAudio();

    if (spectraThisFrame > 0) {
        audio_.computeMathChannels();

        int nSpec = audio_.totalNumSpectra();
        const auto& mathChannels = audio_.mathChannels();
        const auto& mathSpectra  = audio_.mathSpectra();

        if (waterfallMultiCh_ && nSpec > 1) {
            wfSpectraScratch_.clear();
            wfChInfoScratch_.clear();

            for (int ch = 0; ch < nSpec; ++ch) {
                const auto& c = channelColors_[ch % kMaxChannels];
                wfSpectraScratch_.push_back(audio_.getSpectrum(ch));
                wfChInfoScratch_.push_back({c.x, c.y, c.z,
                                    channelEnabled_[ch % kMaxChannels]});
            }
            for (size_t mi = 0; mi < mathChannels.size(); ++mi) {
                if (mathChannels[mi].enabled && mathChannels[mi].waterfall &&
                    mi < mathSpectra.size()) {
                    const auto& c = mathChannels[mi].color;
                    wfSpectraScratch_.push_back(mathSpectra[mi]);
                    wfChInfoScratch_.push_back({c[0], c[1], c[2], true});
                }
            }
            waterfall_.pushLineMulti(wfSpectraScratch_, wfChInfoScratch_, minDB_, maxDB_);
        } else {
            int wfCh = std::clamp(waterfallChannel_, 0, nSpec - 1);
            waterfall_.pushLine(audio_.getSpectrum(wfCh), minDB_, maxDB_);
        }
        int curCh = std::clamp(waterfallChannel_, 0, nSpec - 1);
        cursors_.update(audio_.getSpectrum(curCh),
                       settings.sampleRate, settings.isIQ, settings.fftSize);
        measurements_.update(audio_.getSpectrum(curCh),
                             settings.sampleRate, settings.isIQ, settings.fftSize);
    }

    if (audio_.source()->isEOF() && !audio_.source()->isRealTime()) {
        paused_ = true;
    }
}

void Application::render() {
    if (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) {
        SDL_Delay(16);
        return;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    hoverPanel_ = HoverPanel::None;

    const auto& settings = audio_.settings();

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
            const auto& devices = audio_.devices();
            if (!devices.empty()) {
                bool multiMode = audio_.multiDeviceMode();
                if (ImGui::Checkbox("Multi-Device", &multiMode)) {
                    audio_.setMultiDeviceMode(multiMode);
                    audio_.clearDeviceSelections();
                    if (!multiMode) {
                        openDevice();
                        updateAnalyzerSettings();
                        saveConfig();
                    }
                }

                if (audio_.multiDeviceMode()) {
                    ImGui::Text("Select devices (each = 1 channel):");
                    int maxDevs = std::min(static_cast<int>(devices.size()), kMaxChannels);
                    bool changed = false;
                    for (int i = 0; i < maxDevs; ++i) {
                        bool sel = audio_.deviceSelected(i);
                        if (ImGui::Checkbox(
                                (devices[i].name + "##mdev" + std::to_string(i)).c_str(),
                                &sel)) {
                            audio_.setDeviceSelected(i, sel);
                            changed = true;
                        }
                    }
                    if (changed) {
                        openMultiDevice();
                        updateAnalyzerSettings();
                        saveConfig();
                    }
                } else {
                    ImGui::Text("Audio Device");
                    std::vector<const char*> devNames;
                    for (auto& d : devices) devNames.push_back(d.name.c_str());
                    int devIdx = audio_.deviceIdx();
                    ImGui::SetNextItemWidth(250);
                    if (ImGui::Combo("##device", &devIdx, devNames.data(),
                                     static_cast<int>(devNames.size()))) {
                        audio_.setDeviceIdx(devIdx);
                        openDevice();
                        updateAnalyzerSettings();
                        saveConfig();
                    }
                }
            }
            if (ImGui::MenuItem("Open Audio Device")) {
                if (audio_.multiDeviceMode())
                    openMultiDevice();
                else
                    openDevice();
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
            if (ImGui::MenuItem("VSync", nullptr, &vsync_)) {
                SDL_GL_SetSwapInterval(vsync_ ? 1 : 0);
                saveConfig();
            }

            if (ImGui::BeginMenu("UI scale")) {
                static constexpr int kScales[] = {100, 150, 175, 200, 225, 250, 300};
                int curPct = static_cast<int>(appliedScale_ * 100.0f + 0.5f);
                if (ImGui::MenuItem("Auto", nullptr, uiScale_ == 0.0f)) {
                    uiScale_ = 0.0f;
                    float dpiScale = 1.0f;
#ifdef __EMSCRIPTEN__
                    dpiScale = js_devicePixelRatio();
#else
                    float ddpi = 0;
                    if (SDL_GetDisplayDPI(0, &ddpi, nullptr, nullptr) == 0 && ddpi > 0)
                        dpiScale = ddpi / 96.0f;
#endif
                    requestUIScale(dpiScale);
                    saveConfig();
                }
                for (int s : kScales) {
                    char label[16];
                    std::snprintf(label, sizeof(label), "%d%%", s);
                    if (ImGui::MenuItem(label, nullptr, uiScale_ > 0.0f && std::abs(curPct - s) <= 2)) {
                        uiScale_ = s / 100.0f;
                        requestUIScale(uiScale_);
                        saveConfig();
                    }
                }
                ImGui::EndMenu();
            }

            ImGui::EndMenu();
        }

#ifndef IMGUI_DISABLE_DEBUG_TOOLS
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
#endif

#ifdef __EMSCRIPTEN__
        if (ImGui::SmallButton(js_isFullscreen() ? "Exit Fullscreen" : "Fullscreen")) {
            js_toggleFullscreen();
        }
#endif

        // Right-aligned status in menu bar
        {
            float barW = ImGui::GetWindowWidth();
            char statusBuf[128];
            std::snprintf(statusBuf, sizeof(statusBuf), "%.0f Hz | %d pt | %.1f Hz/bin | %.0f FPS",
                          settings.sampleRate, settings.fftSize,
                          settings.sampleRate / settings.fftSize,
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
    float controlW = showSidebar_ ? 270.0f * logicalScale_ : 0.0f;
    float contentW = totalW - (showSidebar_ ? controlW + 8 : 0);

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
            spectrumFrac_ -= dy / contentH;
            spectrumFrac_ = std::clamp(spectrumFrac_, 0.1f, 0.9f);
            draggingSplit_ = true;
        } else if (draggingSplit_) {
            draggingSplit_ = false;
            saveConfig();
        }

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
                           specPosX_, specSizeX_, settings.sampleRate,
                           settings.isIQ, freqScale_, viewLo_, viewHi_);
            ImU32 hoverCol = IM_COL32(200, 200, 200, 80);

            dlp->AddLine({hx, specPosY_}, {hx, specPosY_ + specSizeY_}, hoverCol, 1.0f);

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
                int bins = audio_.spectrumSize();
                double fMin = settings.isIQ ? -settings.sampleRate / 2.0 : 0.0;
                double fMax = settings.isIQ ?  settings.sampleRate / 2.0 : settings.sampleRate / 2.0;
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

#ifndef IMGUI_DISABLE_DEBUG_TOOLS
    if (showDemoWindow_)    ImGui::ShowDemoWindow(&showDemoWindow_);
    if (showMetricsWindow_) ImGui::ShowMetricsWindow(&showMetricsWindow_);
    if (showDebugLog_)      ImGui::ShowDebugLogWindow(&showDebugLog_);
    if (showStackTool_)     ImGui::ShowIDStackToolWindow(&showStackTool_);
#endif

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
    const auto& settings = audio_.settings();

    // ── Playback ──
    float btnW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;
    if (ImGui::Button(paused_ ? "Resume" : "Pause", {btnW, 0}))
        paused_ = !paused_;
    ImGui::SameLine();
    if (ImGui::Button("Clear", {btnW, 0})) {
        audio_.clearHistory();
    }
    ImGui::SameLine();
    if (ImGui::Button("Peak", {btnW, 0})) {
        int pkCh = std::clamp(waterfallChannel_, 0, audio_.totalNumSpectra() - 1);
        cursors_.snapToPeak(audio_.getSpectrum(pkCh),
                            settings.sampleRate, settings.isIQ,
                            settings.fftSize);
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
            audio_.settings().fftSize = kFFTSizes[fftSizeIdx_];
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
            audio_.settings().window = static_cast<WindowType>(windowIdx_);
            updateAnalyzerSettings();
            saveConfig();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Window Function");

        if (settings.window == WindowType::Kaiser) {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##kaiser", &audio_.settings().kaiserBeta, 0.0f, 20.0f, "Kaiser: %.1f"))
                updateAnalyzerSettings();
        }

        // Overlap
        {
            int hopSamples = static_cast<int>(settings.fftSize * (1.0f - settings.overlap));
            if (hopSamples < 1) hopSamples = 1;
            int overlapSamples = settings.fftSize - hopSamples;

            ImGui::SetNextItemWidth(-1);
            float sliderVal = 1.0f - std::pow(1.0f - overlapPct_ / 99.0f, 0.25f);
            if (ImGui::SliderFloat("##overlap", &sliderVal, 0.0f, 1.0f, "")) {
                float inv = 1.0f - sliderVal;
                float inv2 = inv * inv;
                overlapPct_ = 99.0f * (1.0f - inv2 * inv2);
                audio_.settings().overlap = overlapPct_ / 100.0f;
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
            bool isLog = (freqScale_ == FreqScale::Logarithmic);
            bool canLog = !settings.isIQ;
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Freq. scale:");
            ImGui::SameLine();
            if (ImGui::Button(isLog ? "Logarithmic" : "Linear", {ImGui::GetContentRegionAvail().x, 0})) {
                if (canLog) {
                    constexpr float kMinBF = 0.001f;
                    float logMin = std::log10(kMinBF);
                    auto screenToBin = [&](float sf) -> float {
                        if (isLog) return std::pow(10.0f, logMin + sf * (0.0f - logMin));
                        return sf;
                    };
                    auto binToScreen = [&](float bf, bool toLog) -> float {
                        if (toLog) {
                            if (bf < kMinBF) bf = kMinBF;
                            return (std::log10(bf) - logMin) / (0.0f - logMin);
                        }
                        return bf;
                    };
                    float bfLo = screenToBin(viewLo_);
                    float bfHi = screenToBin(viewHi_);
                    bool newLog = !isLog;
                    freqScale_ = newLog ? FreqScale::Logarithmic : FreqScale::Linear;
                    viewLo_ = std::clamp(binToScreen(bfLo, newLog), 0.0f, 1.0f);
                    viewHi_ = std::clamp(binToScreen(bfHi, newLog), 0.0f, 1.0f);
                    if (viewHi_ <= viewLo_) { viewLo_ = 0.0f; viewHi_ = 1.0f; }
                    saveConfig();
                }
            }
            if (!canLog && ImGui::IsItemHovered())
                ImGui::SetTooltip("Log scale not available in I/Q mode");
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
        int nCh = audio_.totalNumSpectra();
        bool isMulti = waterfallMultiCh_ && nCh > 1;

        float widgetW = (nCh > 1) ? ImGui::CalcTextSize(" Multi ").x + ImGui::GetStyle().FramePadding.x * 2 : 0.0f;
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
                static const char* defaultNames[] = {
                    "Left", "Right", "Ch 3", "Ch 4", "Ch 5", "Ch 6", "Ch 7", "Ch 8"
                };
                for (int ch = 0; ch < nCh && ch < kMaxChannels; ++ch) {
                    ImGui::PushID(ch);
                    ImGui::Checkbox("##en", &channelEnabled_[ch]);
                    ImGui::SameLine();
                    ImGui::ColorEdit3(defaultNames[ch], &channelColors_[ch].x,
                                      ImGuiColorEditFlags_NoInputs);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", audio_.getDeviceName(ch));
                    ImGui::PopID();
                }
            } else {
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
        float btnW2 = ImGui::GetFrameHeight();
        float gap = ImGui::GetStyle().ItemSpacing.x * 0.25f;
        ImVec2 hdrMin = ImGui::GetCursorScreenPos();
        float winLeft = ImGui::GetWindowPos().x;
        float hdrRight = hdrMin.x + ImGui::GetContentRegionAvail().x;
        ImGui::PushClipRect({winLeft, hdrMin.y}, {hdrRight - btnW2 - gap, hdrMin.y + 200}, true);
        bool mathOpen = ImGui::CollapsingHeader("##math_hdr",
                                                 ImGuiTreeNodeFlags_DefaultOpen |
                                                 ImGuiTreeNodeFlags_AllowOverlap);
        ImGui::PopClipRect();
        ImGui::SameLine();
        ImGui::Text("Math");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - btnW2 + ImGui::GetStyle().FramePadding.x);
        if (ImGui::Button("+##addmath", {btnW2, 0})) {
            int nPhys = audio_.totalNumSpectra();
            MathChannel mc;
            mc.op = MathOp::Subtract;
            mc.sourceX = 0;
            mc.sourceY = std::min(1, nPhys - 1);
            mc.color[0] = 1.0f; mc.color[1] = 1.0f; mc.color[2] = 0.5f; mc.color[3] = 1.0f;
            audio_.mathChannels().push_back(mc);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add math channel");

        if (mathOpen) {
            renderMathPanel();
        }
    }

    // ── Cursors ──
    ImGui::Spacing();
    {
        float btnW2 = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2;
        float gap = ImGui::GetStyle().ItemSpacing.x * 0.25f;
        ImVec2 hdrMin = ImGui::GetCursorScreenPos();
        float winLeft = ImGui::GetWindowPos().x;
        float hdrRight = hdrMin.x + ImGui::GetContentRegionAvail().x;
        ImGui::PushClipRect({winLeft, hdrMin.y}, {hdrRight - btnW2 - gap, hdrMin.y + 200}, true);
        bool cursorsOpen = ImGui::CollapsingHeader("##cursors_hdr",
                                                    ImGuiTreeNodeFlags_DefaultOpen |
                                                    ImGuiTreeNodeFlags_AllowOverlap);
        ImGui::PopClipRect();
        ImGui::SameLine();
        ImGui::Text("Cursors");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - btnW2 + ImGui::GetStyle().FramePadding.x);
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
            float prevMin = measurements_.traceMinFreq;
            float prevMax = measurements_.traceMaxFreq;
            measurements_.drawPanel();
            if (measurements_.traceMinFreq != prevMin || measurements_.traceMaxFreq != prevMax)
                saveConfig();
        }
    }

    // ── Status (bottom) ──
    ImGui::Separator();
    ImGui::TextDisabled("Mode: %s", settings.isIQ ? "I/Q"
                        : (settings.numChannels > 1 ? "Multi-ch" : "Real"));
}

void Application::renderSpectrumPanel() {
    const auto& settings = audio_.settings();

    float availW = ImGui::GetContentRegionAvail().x;
    float specH = ImGui::GetContentRegionAvail().y;

    ImVec2 pos = ImGui::GetCursorScreenPos();
    specPosX_ = pos.x;
    specPosY_ = pos.y;
    specSizeX_ = availW;
    specSizeY_ = specH;

    int nPhys = audio_.totalNumSpectra();
    const auto& mathChannels = audio_.mathChannels();
    const auto& mathSpectra  = audio_.mathSpectra();
    int nMath = static_cast<int>(mathSpectra.size());

    allSpectraScratch_.clear();
    stylesScratch_.clear();

    // Physical channels (skip disabled ones).
    for (int ch = 0; ch < nPhys; ++ch) {
        if (!channelEnabled_[ch % kMaxChannels]) continue;
        allSpectraScratch_.push_back(audio_.getSpectrum(ch));
        const auto& c = channelColors_[ch % kMaxChannels];
        uint8_t r = static_cast<uint8_t>(c.x * 255);
        uint8_t g = static_cast<uint8_t>(c.y * 255);
        uint8_t b = static_cast<uint8_t>(c.z * 255);
        stylesScratch_.push_back({IM_COL32(r, g, b, 220), IM_COL32(r, g, b, 35)});
    }

    // Math channels.
    for (int mi = 0; mi < nMath; ++mi) {
        if (mi < static_cast<int>(mathChannels.size()) && mathChannels[mi].enabled) {
            allSpectraScratch_.push_back(mathSpectra[mi]);
            const auto& c = mathChannels[mi].color;
            uint8_t r = static_cast<uint8_t>(c[0] * 255);
            uint8_t g = static_cast<uint8_t>(c[1] * 255);
            uint8_t b = static_cast<uint8_t>(c[2] * 255);
            stylesScratch_.push_back({IM_COL32(r, g, b, 220), IM_COL32(r, g, b, 35)});
        }
    }

    specDisplay_.updatePeakHold(allSpectraScratch_);
    specDisplay_.draw(allSpectraScratch_, stylesScratch_, minDB_, maxDB_,
                      settings.sampleRate, settings.isIQ, freqScale_,
                      specPosX_, specPosY_, specSizeX_, specSizeY_,
                      viewLo_, viewHi_);

    cursors_.draw(specDisplay_, specPosX_, specPosY_, specSizeX_, specSizeY_,
                  settings.sampleRate, settings.isIQ, freqScale_, minDB_, maxDB_,
                  viewLo_, viewHi_);

    measurements_.draw(specDisplay_, specPosX_, specPosY_, specSizeX_, specSizeY_,
                       settings.sampleRate, settings.isIQ, freqScale_, minDB_, maxDB_,
                       viewLo_, viewHi_);

    handleSpectrumInput(specPosX_, specPosY_, specSizeX_, specSizeY_);

    ImGui::Dummy({availW, specH});
}

void Application::renderWaterfallPanel() {
    const auto& settings = audio_.settings();

    float availW = ImGui::GetContentRegionAvail().x;
    constexpr float kSplitterH = 6.0f;
    float parentH = ImGui::GetContentRegionAvail().y;
    float availH = (parentH - kSplitterH) * (1.0f - spectrumFrac_);

    int neededH = std::max(1024, static_cast<int>(availH) + 1);
    int binCount = std::max(1, audio_.spectrumSize());
    if (binCount != waterfall_.width() || waterfall_.height() < neededH) {
        waterfall_.resize(binCount, neededH);
        waterfall_.setColorMap(colorMap_);
    }

    if (waterfall_.textureID()) {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto texID = static_cast<ImTextureID>(waterfall_.textureID());

        int h = waterfall_.height();
        int screenRows = std::min(static_cast<int>(availH), h);
        int newestRow = (waterfall_.currentRow() + 1) % h;

        float rowToV = 1.0f / h;

        bool logMode = (freqScale_ == FreqScale::Logarithmic && !settings.isIQ);

        auto drawSpan = [&](int rowStart, int rowCount, float yStart, float spanH) {
            float v0 = rowStart * rowToV;
            float v1 = (rowStart + rowCount) * rowToV;

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

        float pxPerRow = availH / static_cast<float>(screenRows);

        if (newestRow + screenRows <= h) {
            drawSpan(newestRow, screenRows, pos.y, availH);
        } else {
            int firstCount = h - newestRow;
            int secondCount = screenRows - firstCount;

            float secondH = secondCount * pxPerRow;
            if (secondCount > 0)
                drawSpan(0, secondCount, pos.y, secondH);

            float firstH = availH - secondH;
            drawSpan(newestRow, firstCount, pos.y + secondH, firstH);
        }

        // ── Frequency axis labels ──
        ImU32 textCol = IM_COL32(180, 180, 200, 200);
        double freqFullMin = settings.isIQ ? -settings.sampleRate / 2.0 : 0.0;
        double freqFullMax = settings.isIQ ?  settings.sampleRate / 2.0 : settings.sampleRate / 2.0;

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

        wfPosX_ = pos.x; wfPosY_ = pos.y; wfSizeX_ = availW; wfSizeY_ = availH;

        measurements_.drawWaterfall(specDisplay_, wfPosX_, wfPosY_, wfSizeX_, wfSizeY_,
                                     settings.sampleRate, settings.isIQ, freqScale_,
                                     viewLo_, viewHi_, screenRows, audio_.spectrumSize());

        // ── Mouse interaction: zoom, pan & hover on waterfall ──
        ImGuiIO& io = ImGui::GetIO();
        float mx = io.MousePos.x;
        float my = io.MousePos.y;
        bool inWaterfall = mx >= pos.x && mx <= pos.x + availW &&
                           my >= pos.y && my <= pos.y + availH;

        if (inWaterfall) {
            hoverPanel_ = HoverPanel::Waterfall;
            double freq = specDisplay_.screenXToFreq(mx, pos.x, availW,
                                                      settings.sampleRate,
                                                      settings.isIQ, freqScale_,
                                                      viewLo_, viewHi_);
            int bins = audio_.spectrumSize();
            double fMin = settings.isIQ ? -settings.sampleRate / 2.0 : 0.0;
            double fMax = settings.isIQ ?  settings.sampleRate / 2.0 : settings.sampleRate / 2.0;
            int bin = static_cast<int>((freq - fMin) / (fMax - fMin) * (bins - 1));
            bin = std::clamp(bin, 0, bins - 1);

            float yFrac = 1.0f - (my - pos.y) / availH;
            int hopSamples = static_cast<int>(settings.fftSize * (1.0f - settings.overlap));
            if (hopSamples < 1) hopSamples = 1;
            double secondsPerLine = static_cast<double>(hopSamples) / settings.sampleRate;
            hoverWfTimeOffset_ = static_cast<float>(yFrac * screenRows * secondsPerLine);

            int curCh = std::clamp(waterfallChannel_, 0, audio_.totalNumSpectra() - 1);
            const auto& spec = audio_.getSpectrum(curCh);
            if (!spec.empty()) {
                cursors_.hover = {true, freq, spec[bin], bin};
            }
        }

        if (inWaterfall) {
            if (io.MouseWheel != 0) {
                float cursorFrac = (mx - pos.x) / availW;
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

            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle)) {
                viewLo_ = 0.0f;
                viewHi_ = 1.0f;
            }
        }
    }

    ImGui::Dummy({availW, availH});
}

void Application::handleTouchEvent(const SDL_Event& event) {
    if (event.type == SDL_FINGERDOWN) {
        ++touch_.count;
    } else if (event.type == SDL_FINGERUP) {
        touch_.count = std::max(0, touch_.count - 1);
    }

    if (touch_.count == 2 && event.type == SDL_FINGERDOWN) {
        int w, h;
        SDL_GetWindowSize(window_, &w, &h);
        SDL_TouchID tid = event.tfinger.touchId;
        int nf = SDL_GetNumTouchFingers(tid);
        if (nf >= 2) {
            SDL_Finger* f0 = SDL_GetTouchFinger(tid, 0);
            SDL_Finger* f1 = SDL_GetTouchFinger(tid, 1);
            float x0 = f0->x * w, x1 = f1->x * w;
            float dx = x1 - x0, dy = (f1->y - f0->y) * h;
            touch_.startDist = std::sqrt(dx * dx + dy * dy);
            touch_.lastDist = touch_.startDist;
            touch_.startCenterX = (x0 + x1) * 0.5f;
            touch_.lastCenterX = touch_.startCenterX;
            touch_.startLo = viewLo_;
            touch_.startHi = viewHi_;
        }
    }

    if (touch_.count == 2 && event.type == SDL_FINGERMOTION) {
        int w, h;
        SDL_GetWindowSize(window_, &w, &h);
        SDL_TouchID tid = event.tfinger.touchId;
        int nf = SDL_GetNumTouchFingers(tid);
        if (nf >= 2) {
            SDL_Finger* f0 = SDL_GetTouchFinger(tid, 0);
            SDL_Finger* f1 = SDL_GetTouchFinger(tid, 1);
            float x0 = f0->x * w, x1 = f1->x * w;
            float dx = x1 - x0, dy = (f1->y - f0->y) * h;
            float dist = std::sqrt(dx * dx + dy * dy);
            float centerX = (x0 + x1) * 0.5f;

            if (touch_.startDist > 1.0f) {
                float span0 = touch_.startHi - touch_.startLo;
                float ratio = touch_.startDist / std::max(dist, 1.0f);
                float newSpan = std::clamp(span0 * ratio, 0.001f, 1.0f);

                float panelW = wfSizeX_ > 0 ? wfSizeX_ : static_cast<float>(w);
                float panelX = wfPosX_;
                float midFrac = (touch_.startCenterX - panelX) / panelW;
                float midView = touch_.startLo + midFrac * span0;

                float panDelta = -(centerX - touch_.startCenterX) / panelW * newSpan;

                float newLo = midView - midFrac * newSpan + panDelta;
                float newHi = newLo + newSpan;

                if (newLo < 0.0f) { newHi -= newLo; newLo = 0.0f; }
                if (newHi > 1.0f) { newLo -= (newHi - 1.0f); newHi = 1.0f; }
                viewLo_ = std::clamp(newLo, 0.0f, 1.0f);
                viewHi_ = std::clamp(newHi, 0.0f, 1.0f);
            }
        }
    }
}

void Application::handleSpectrumInput(float posX, float posY,
                                       float sizeX, float sizeY) {
    const auto& settings = audio_.settings();

    ImGuiIO& io = ImGui::GetIO();
    float mx = io.MousePos.x;
    float my = io.MousePos.y;

    bool inRegion = mx >= posX && mx <= posX + sizeX &&
                    my >= posY && my <= posY + sizeY;

    if (inRegion) {
        hoverPanel_ = HoverPanel::Spectrum;
        double freq = specDisplay_.screenXToFreq(mx, posX, sizeX,
                                                  settings.sampleRate,
                                                  settings.isIQ, freqScale_,
                                                  viewLo_, viewHi_);
        float dB = specDisplay_.screenYToDB(my, posY, sizeY, minDB_, maxDB_);

        int bins = audio_.spectrumSize();
        double freqMin = settings.isIQ ? -settings.sampleRate / 2.0 : 0.0;
        double freqMax = settings.isIQ ?  settings.sampleRate / 2.0 : settings.sampleRate / 2.0;
        int bin = static_cast<int>((freq - freqMin) / (freqMax - freqMin) * (bins - 1));
        bin = std::clamp(bin, 0, bins - 1);

        int curCh = std::clamp(waterfallChannel_, 0, audio_.totalNumSpectra() - 1);
        const auto& spec = audio_.getSpectrum(curCh);
        if (!spec.empty()) {
            dB = spec[bin];
            cursors_.hover = {true, freq, dB, bin};
        }

        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            int cBin = cursors_.snapToPeaks ? cursors_.findLocalPeak(spec, bin, 10) : bin;
            double cFreq = audio_.binToFreq(cBin);
            cursors_.setCursorA(cFreq, spec[cBin], cBin);
        }
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            int cBin = cursors_.snapToPeaks ? cursors_.findLocalPeak(spec, bin, 10) : bin;
            double cFreq = audio_.binToFreq(cBin);
            cursors_.setCursorB(cFreq, spec[cBin], cBin);
        }

        {
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

            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle)) {
                viewLo_ = 0.0f;
                viewHi_ = 1.0f;
            }
        }
    } else {
        if (hoverPanel_ != HoverPanel::Waterfall) {
            hoverPanel_ = HoverPanel::None;
            cursors_.hover.active = false;
        }
    }
}

// ── Source management (delegates to AudioEngine) ─────────────────────────────

void Application::openDevice() {
    audio_.openDevice(audio_.deviceIdx());
    fileSampleRate_ = static_cast<float>(audio_.settings().sampleRate);
}

void Application::openMultiDevice() {
    bool selected[kMaxChannels] = {};
    const auto& devices = audio_.devices();
    int maxDevs = std::min(static_cast<int>(devices.size()), kMaxChannels);
    for (int i = 0; i < maxDevs; ++i)
        selected[i] = audio_.deviceSelected(i);
    audio_.openMultiDevice(selected, maxDevs);
}

void Application::openFile(const std::string& path, InputFormat format, double sampleRate) {
    audio_.openFile(path, format, sampleRate, fileLoop_);
    fileSampleRate_ = static_cast<float>(audio_.settings().sampleRate);
}

void Application::updateAnalyzerSettings() {
    auto& settings = audio_.settings();
    int  oldFFTSize = settings.fftSize;
    bool oldIQ      = settings.isIQ;
    int  oldNCh     = settings.numChannels;

    settings.fftSize = kFFTSizes[fftSizeIdx_];
    settings.overlap = overlapPct_ / 100.0f;
    settings.window  = static_cast<WindowType>(windowIdx_);
    audio_.configure(settings);

    bool sizeChanged = settings.fftSize     != oldFFTSize ||
                       settings.isIQ        != oldIQ      ||
                       settings.numChannels != oldNCh;

    if (sizeChanged) {
        audio_.drainSources();

        cursors_.cursorA.active = false;
        cursors_.cursorB.active = false;

        int reinitH = std::max(1024, waterfall_.height());
        int binCount2 = std::max(1, audio_.spectrumSize());
        waterfall_.init(binCount2, reinitH);
    }
}

// ── Math panel ───────────────────────────────────────────────────────────────

void Application::renderMathPanel() {
    int nPhys = audio_.totalNumSpectra();
    auto& mathChannels = audio_.mathChannels();

    static const char* chNames[] = {
        "Ch 0 (L)", "Ch 1 (R)", "Ch 2", "Ch 3", "Ch 4", "Ch 5", "Ch 6", "Ch 7"
    };

    int toRemove = -1;
    for (int mi = 0; mi < static_cast<int>(mathChannels.size()); ++mi) {
        auto& mc = mathChannels[mi];
        ImGui::PushID(1000 + mi);

        ImGui::Checkbox("##en", &mc.enabled);
        ImGui::SameLine();
        ImGui::ColorEdit3("##col", mc.color, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();

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

        ImGui::SetNextItemWidth(80);
        ImGui::Combo("X", &mc.sourceX, chNames, std::min(nPhys, kMaxChannels));

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
        mathChannels.erase(mathChannels.begin() + toRemove);
}

// ── Config persistence ──────────────────────────────────────────────────────

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
    uiScale_      = config_.getFloat("ui_scale", uiScale_);
    spectrumFrac_ = config_.getFloat("spectrum_frac", spectrumFrac_);
    showSidebar_  = config_.getBool("show_sidebar", showSidebar_);
    specDisplay_.peakHoldEnable = config_.getBool("peak_hold", specDisplay_.peakHoldEnable);
    specDisplay_.peakHoldDecay  = config_.getFloat("peak_hold_decay", specDisplay_.peakHoldDecay);
    cursors_.snapToPeaks        = config_.getBool("snap_to_peaks", cursors_.snapToPeaks);
    measurements_.traceMinFreq  = config_.getFloat("trace_min_freq", measurements_.traceMinFreq);
    measurements_.traceMaxFreq  = config_.getFloat("trace_max_freq", measurements_.traceMaxFreq);

    // Clamp
    fftSizeIdx_   = std::clamp(fftSizeIdx_, 0, kNumFFTSizes - 1);
    windowIdx_    = std::clamp(windowIdx_, 0, static_cast<int>(WindowType::Count) - 1);
    colorMapIdx_  = std::clamp(colorMapIdx_, 0, static_cast<int>(ColorMapType::Count) - 1);
    spectrumFrac_ = std::clamp(spectrumFrac_, 0.1f, 0.9f);

    // Restore device selection.
    const auto& devices = audio_.devices();
    audio_.setMultiDeviceMode(config_.getBool("multi_device", false));
    std::string devName = config_.getString("device_name", "");
    if (!devName.empty()) {
        for (int i = 0; i < static_cast<int>(devices.size()); ++i) {
            if (devices[i].name == devName) {
                audio_.setDeviceIdx(i);
                break;
            }
        }
    }
    // Restore multi-device selections from comma-separated device names.
    audio_.clearDeviceSelections();
    std::string multiNames = config_.getString("multi_device_names", "");
    if (!multiNames.empty()) {
        size_t pos = 0;
        while (pos < multiNames.size()) {
            size_t comma = multiNames.find(',', pos);
            if (comma == std::string::npos) comma = multiNames.size();
            std::string name = multiNames.substr(pos, comma - pos);
            for (int i = 0; i < std::min(static_cast<int>(devices.size()), kMaxChannels); ++i) {
                if (devices[i].name == name)
                    audio_.setDeviceSelected(i, true);
            }
            pos = comma + 1;
        }
    }

    // Apply
    auto& settings = audio_.settings();
    settings.fftSize = kFFTSizes[fftSizeIdx_];
    settings.overlap = overlapPct_ / 100.0f;
    settings.window  = static_cast<WindowType>(windowIdx_);
    colorMap_.setType(static_cast<ColorMapType>(colorMapIdx_));
    SDL_GL_SetSwapInterval(vsync_ ? 1 : 0);
}

void Application::saveConfig() const {
    const auto& settings = audio_.settings();
    const auto& devices  = audio_.devices();

    Config cfg;
    cfg.setInt("fft_size_idx", fftSizeIdx_);
    cfg.setFloat("overlap_pct", overlapPct_);
    cfg.setInt("window_idx", windowIdx_);
    cfg.setInt("colormap_idx", colorMapIdx_);
    cfg.setFloat("min_db", minDB_);
    cfg.setFloat("max_db", maxDB_);
    cfg.setInt("freq_scale", static_cast<int>(freqScale_));
    cfg.setBool("vsync", vsync_);
    cfg.setFloat("ui_scale", uiScale_);
    cfg.setFloat("spectrum_frac", spectrumFrac_);
    cfg.setBool("show_sidebar", showSidebar_);
    cfg.setBool("peak_hold", specDisplay_.peakHoldEnable);
    cfg.setFloat("peak_hold_decay", specDisplay_.peakHoldDecay);
    cfg.setBool("snap_to_peaks", cursors_.snapToPeaks);
    cfg.setFloat("trace_min_freq", measurements_.traceMinFreq);
    cfg.setFloat("trace_max_freq", measurements_.traceMaxFreq);

    int devIdx = audio_.deviceIdx();
    if (devIdx >= 0 && devIdx < static_cast<int>(devices.size()))
        cfg.setString("device_name", devices[devIdx].name);

    cfg.setBool("multi_device", audio_.multiDeviceMode());
    std::string multiNames;
    for (int i = 0; i < std::min(static_cast<int>(devices.size()), kMaxChannels); ++i) {
        if (audio_.deviceSelected(i)) {
            if (!multiNames.empty()) multiNames += ',';
            multiNames += devices[i].name;
        }
    }
    cfg.setString("multi_device_names", multiNames);

    cfg.save();
}

} // namespace baudmine
