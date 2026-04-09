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

EM_JS(void, js_clearCanvasInlineSize, (), {
    var c = document.getElementById('canvas');
    if (c) { c.style.width = ''; c.style.height = ''; }
});

#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>
#else
#include <GL/gl.h>
#endif
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace baudmine {

Application::Application() = default;

// ── UI scaling ──────────────────────────────────────────────────────────────

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

float Application::systemDpiScale() const {
#ifdef __EMSCRIPTEN__
    return js_devicePixelRatio();
#else
    float ddpi = 0;
    if (SDL_GetDisplayDPI(0, &ddpi, nullptr, nullptr) == 0 && ddpi > 0)
        return ddpi / 96.0f;
    return 1.0f;
#endif
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

Application::~Application() {
    shutdown();
}

bool Application::init(int argc, char** argv) {
    // Parse command line
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--format" && i + 1 < argc) {
            std::string fmt = argv[++i];
            if (fmt == "f32")  controlPanel_.fileFormatIdx = 0;
            if (fmt == "i16")  controlPanel_.fileFormatIdx = 1;
            if (fmt == "u8")   controlPanel_.fileFormatIdx = 2;
            if (fmt == "wav")  controlPanel_.fileFormatIdx = 3;
        } else if (arg == "--rate" && i + 1 < argc) {
            controlPanel_.fileSampleRate = std::stof(argv[++i]);
        } else if (arg == "--iq") {
            audio_.settings().isIQ = true;
        } else if (arg[0] != '-') {
            controlPanel_.filePath = arg;
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
                               kDefaultWindowWidth, kDefaultWindowHeight,
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

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

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

    audio_.enumerateDevices();
    loadConfig();
    syncCanvasSize();

    // DPI-aware UI scaling
    {
        float dpiScale = systemDpiScale();
#ifdef __EMSCRIPTEN__
        lastDpr_ = dpiScale;
#endif
        applyUIScale((uiScale_ > 0.0f) ? uiScale_ : dpiScale);
    }

    // Apply loaded settings
    auto& settings = audio_.settings();
    settings.fftSize    = ControlPanel::kFFTSizes[controlPanel_.fftSizeIdx];
    settings.overlap    = controlPanel_.overlapPct / 100.0f;
    settings.window     = static_cast<WindowType>(controlPanel_.windowIdx);
    settings.sampleRate = controlPanel_.fileSampleRate;
    settings.isIQ       = false;

    if (!controlPanel_.filePath.empty()) {
        InputFormat fmt;
        switch (controlPanel_.fileFormatIdx) {
            case 0: fmt = InputFormat::Float32IQ; settings.isIQ = true; break;
            case 1: fmt = InputFormat::Int16IQ;   settings.isIQ = true; break;
            case 2: fmt = InputFormat::Uint8IQ;   settings.isIQ = true; break;
            default: fmt = InputFormat::WAV;      break;
        }
        openFile(controlPanel_.filePath, fmt, controlPanel_.fileSampleRate);
    } else {
        openDevice();
    }

    updateAnalyzerSettings();
    running_ = true;
    return true;
}

void Application::shutdown() {
    if (!window_) return;  // already shut down
    audio_.closeAll();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    if (glContext_) { SDL_GL_DeleteContext(glContext_); glContext_ = nullptr; }
    SDL_DestroyWindow(window_);
    window_ = nullptr;
    SDL_Quit();
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
    while (running_) mainLoopStep();
#endif
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
        displayPanel_.handleTouch(event, ui_, window_);
        if (event.type == SDL_QUIT)
            running_ = false;
        if (event.type == SDL_KEYDOWN) {
            auto key = event.key.keysym.sym;
#ifndef __EMSCRIPTEN__
            if (key == SDLK_ESCAPE) running_ = false;
#endif
            if (key == SDLK_SPACE)  ui_.paused = !ui_.paused;
            if (key == SDLK_p) {
                int pkCh = std::clamp(ui_.waterfallChannel, 0,
                                      audio_.totalNumSpectra() - 1);
                cursors_.snapToPeak(audio_.getSpectrum(pkCh),
                                    settings.sampleRate, settings.isIQ,
                                    settings.fftSize);
            }
        }
    }

    if (!ui_.paused)
        processAudio();

    render();
}

// ── Audio processing ────────────────────────────────────────────────────────

void Application::processAudio() {
    if (!audio_.hasSource()) return;

    const auto& settings = audio_.settings();
    int spectraThisFrame = audio_.processAudio();

    if (spectraThisFrame > 0) {
        audio_.computeMathChannels();

        int nSpec = audio_.totalNumSpectra();
        const auto& mathChannels = audio_.mathChannels();
        const auto& mathSpectra  = audio_.mathSpectra();

        // Push ALL new spectra to the waterfall so that the scroll rate
        // is determined by the audio sample rate, not the display refresh.
        int curCh = std::clamp(ui_.waterfallChannel, 0, nSpec - 1);
        const auto& traceHist = audio_.getWaterfallHistory(curCh);
        int traceHistSz = static_cast<int>(traceHist.size());

        if (ui_.waterfallMultiCh && nSpec > 1) {
            // For multi-channel: replay the last spectraThisFrame entries
            // from channel 0's history to get per-step data.  Other
            // channels have the same count of new entries.
            const auto& hist0 = audio_.getWaterfallHistory(0);
            int histSz = static_cast<int>(hist0.size());
            int start = std::max(0, histSz - spectraThisFrame);

            for (int si = start; si < histSz; ++si) {
                std::vector<std::vector<float>> wfSpectra;
                std::vector<WaterfallChannelInfo> wfInfo;

                for (int ch = 0; ch < nSpec; ++ch) {
                    const auto& c = ui_.channelColors[ch % kMaxChannels];
                    const auto& hist = audio_.getWaterfallHistory(ch);
                    int idx = std::max(0, static_cast<int>(hist.size()) - (histSz - si));
                    wfSpectra.push_back(hist[idx]);
                    wfInfo.push_back({c.x, c.y, c.z,
                                      ui_.channelEnabled[ch % kMaxChannels]});
                }
                // Math channels: use their own waterfall history.
                for (size_t mi = 0; mi < mathChannels.size(); ++mi) {
                    if (mathChannels[mi].enabled && mathChannels[mi].waterfall &&
                        mi < mathSpectra.size()) {
                        const auto& c = mathChannels[mi].color;
                        const auto& mHist = audio_.mathWaterfallHistory(static_cast<int>(mi));
                        int mHistSz = static_cast<int>(mHist.size());
                        int mIdx = std::max(0, mHistSz - (histSz - si));
                        if (mIdx < mHistSz) {
                            wfSpectra.push_back(mHist[mIdx]);
                        } else {
                            wfSpectra.push_back(mathSpectra[mi]);
                        }
                        wfInfo.push_back({c[0], c[1], c[2], true});
                    }
                }
                waterfall_.pushLineMulti(wfSpectra, wfInfo, ui_.minDB, ui_.maxDB);

                // Push peak trace entry synchronized with each waterfall line.
                int tIdx = std::max(0, traceHistSz - (histSz - si));
                measurements_.pushPeakTrace(traceHist[tIdx],
                                            settings.sampleRate, settings.isIQ, settings.fftSize);
            }
        } else {
            const auto& hist = audio_.getWaterfallHistory(curCh);
            int histSz = static_cast<int>(hist.size());
            int start = std::max(0, histSz - spectraThisFrame);
            for (int si = start; si < histSz; ++si) {
                waterfall_.pushLine(hist[si], ui_.minDB, ui_.maxDB);
                measurements_.pushPeakTrace(hist[si],
                                            settings.sampleRate, settings.isIQ, settings.fftSize);
            }
        }
        cursors_.update(audio_.getSpectrum(curCh),
                       settings.sampleRate, settings.isIQ, settings.fftSize);
        measurements_.update(audio_.getSpectrum(curCh),
                             settings.sampleRate, settings.isIQ, settings.fftSize);
    }

    if (audio_.source()->isEOF() && !audio_.source()->isRealTime())
        ui_.paused = true;
}

// ── Rendering ───────────────────────────────────────────────────────────────

void Application::render() {
    if (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) {
        SDL_Delay(16);
        return;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    displayPanel_.hoverPanel = DisplayPanel::HoverPanel::None;

    const auto& settings = audio_.settings();

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("##Main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_MenuBar);

    // ── Menu bar ──
    if (ImGui::BeginMenuBar()) {
        if (ImGui::Button(showSidebar_ ? "  <<  " : "  >>  ")) {
            showSidebar_ = !showSidebar_;
            saveConfig();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(showSidebar_ ? "Hide sidebar" : "Show sidebar");

        ImGui::Separator();

        if (ImGui::BeginMenu("File")) {
            static char filePathBuf[512] = "";
            if (controlPanel_.filePath.size() < sizeof(filePathBuf))
                std::strncpy(filePathBuf, controlPanel_.filePath.c_str(), sizeof(filePathBuf) - 1);
            ImGui::SetNextItemWidth(200);
            if (ImGui::InputText("Path", filePathBuf, sizeof(filePathBuf)))
                controlPanel_.filePath = filePathBuf;

            const char* formatNames[] = {"Float32 I/Q", "Int16 I/Q", "Uint8 I/Q", "WAV"};
            ImGui::SetNextItemWidth(140);
            ImGui::Combo("Format", &controlPanel_.fileFormatIdx, formatNames, 4);

            ImGui::SetNextItemWidth(140);
            ImGui::DragFloat("Sample Rate", &controlPanel_.fileSampleRate, 1000.0f, 1000.0f, 100e6f, "%.0f Hz");
            ImGui::Checkbox("Loop", &controlPanel_.fileLoop);

            if (ImGui::MenuItem("Open File")) {
                InputFormat fmt;
                switch (controlPanel_.fileFormatIdx) {
                    case 0: fmt = InputFormat::Float32IQ; break;
                    case 1: fmt = InputFormat::Int16IQ;   break;
                    case 2: fmt = InputFormat::Uint8IQ;   break;
                    default: fmt = InputFormat::WAV;       break;
                }
                openFile(controlPanel_.filePath, fmt, controlPanel_.fileSampleRate);
                updateAnalyzerSettings();
            }

            ImGui::Separator();

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
                if (audio_.multiDeviceMode()) openMultiDevice();
                else openDevice();
                updateAnalyzerSettings();
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Esc")) running_ = false;
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Grid", nullptr, &specDisplay_.showGrid);
            ImGui::MenuItem("Fill Spectrum", nullptr, &specDisplay_.fillSpectrum);
            ImGui::MenuItem("Additive Blend", nullptr, &specDisplay_.additiveBlend);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Mix multi-channel spectrum colors additively");
            ImGui::MenuItem("Rulers", nullptr, &displayPanel_.showRuler);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Show timescale ruler on waterfall");
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
                    requestUIScale(systemDpiScale());
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
        if (ImGui::SmallButton(js_isFullscreen() ? "Exit Fullscreen" : "Fullscreen"))
            js_toggleFullscreen();
#endif

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

    // ── Layout ──
    float totalW = ImGui::GetContentRegionAvail().x;
    float contentH = ImGui::GetContentRegionAvail().y;
    float controlW = showSidebar_ ? 270.0f * logicalScale_ : 0.0f;
    float contentW = totalW - (showSidebar_ ? controlW + 8 : 0);

    if (showSidebar_) {
        ImGui::BeginChild("Controls", {controlW, contentH}, true);
        controlPanel_.render(audio_, ui_, specDisplay_, cursors_,
                             measurements_, colorMap_, waterfall_);
        ImGui::EndChild();

        if (controlPanel_.consumeUpdateRequest())
            updateAnalyzerSettings();
        if (controlPanel_.consumeSaveRequest())
            saveConfig();

        ImGui::SameLine();
    }

    // ── Display area ──
    ImGui::BeginChild("Display", {contentW, contentH}, false);
    {
        constexpr float kSplitterH = 6.0f;

        displayPanel_.renderWaterfall(audio_, ui_, waterfall_, specDisplay_,
                                      cursors_, measurements_, colorMap_);

        // Draggable splitter
        ImVec2 splPos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##splitter", {contentW, kSplitterH});
        bool hovered = ImGui::IsItemHovered();
        bool active  = ImGui::IsItemActive();

        if (hovered || active)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

        if (active) {
            displayPanel_.spectrumFrac -= ImGui::GetIO().MouseDelta.y / contentH;
            displayPanel_.spectrumFrac = std::clamp(displayPanel_.spectrumFrac, 0.1f, 0.9f);
            displayPanel_.draggingSplit = true;
        } else if (displayPanel_.draggingSplit) {
            displayPanel_.draggingSplit = false;
            saveConfig();
        }

        ImU32 splCol = (hovered || active)
            ? IM_COL32(100, 150, 255, 220)
            : IM_COL32(80, 80, 100, 150);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float cy = splPos.y + kSplitterH * 0.5f;
        dl->AddLine({splPos.x, cy}, {splPos.x + contentW, cy}, splCol, 2.0f);

        displayPanel_.renderSpectrum(audio_, ui_, specDisplay_, cursors_, measurements_);
        displayPanel_.renderHoverOverlay(audio_, ui_, cursors_, specDisplay_);
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

// ── Source management ───────────────────────────────────────────────────────

void Application::openDevice() {
    audio_.openDevice(audio_.deviceIdx());
    controlPanel_.fileSampleRate = static_cast<float>(audio_.settings().sampleRate);
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
    audio_.openFile(path, format, sampleRate, controlPanel_.fileLoop);
    controlPanel_.fileSampleRate = static_cast<float>(audio_.settings().sampleRate);
}

void Application::updateAnalyzerSettings() {
    auto& settings = audio_.settings();
    int  oldFFTSize = settings.fftSize;
    bool oldIQ      = settings.isIQ;
    int  oldNCh     = settings.numChannels;

    settings.fftSize = ControlPanel::kFFTSizes[controlPanel_.fftSizeIdx];
    settings.overlap = controlPanel_.overlapPct / 100.0f;
    settings.window  = static_cast<WindowType>(controlPanel_.windowIdx);
    audio_.configure(settings);

    bool sizeChanged = settings.fftSize     != oldFFTSize ||
                       settings.isIQ        != oldIQ      ||
                       settings.numChannels != oldNCh;

    if (sizeChanged) {
        audio_.drainSources();
        cursors_.cursorA.active = false;
        cursors_.cursorB.active = false;
        int reinitH = std::max(1024, waterfall_.height());
        int binCount = std::max(1, audio_.spectrumSize());
        waterfall_.init(binCount, reinitH);
    }
}

// ── Config persistence ──────────────────────────────────────────────────────

void Application::loadConfig() {
    config_.load();
    controlPanel_.fftSizeIdx      = config_.getInt("fft_size_idx", controlPanel_.fftSizeIdx);
    controlPanel_.overlapPct      = config_.getFloat("overlap_pct", controlPanel_.overlapPct);
    controlPanel_.windowIdx       = config_.getInt("window_idx", controlPanel_.windowIdx);
    controlPanel_.colorMapIdx     = config_.getInt("colormap_idx", controlPanel_.colorMapIdx);
    ui_.minDB                     = config_.getFloat("min_db", ui_.minDB);
    ui_.maxDB                     = config_.getFloat("max_db", ui_.maxDB);
    int fs                        = config_.getInt("freq_scale", static_cast<int>(ui_.freqScale));
    ui_.freqScale                 = static_cast<FreqScale>(fs);
    vsync_                        = config_.getBool("vsync", vsync_);
    uiScale_                      = config_.getFloat("ui_scale", uiScale_);
    displayPanel_.spectrumFrac    = config_.getFloat("spectrum_frac", displayPanel_.spectrumFrac);
    showSidebar_                  = config_.getBool("show_sidebar", showSidebar_);
    specDisplay_.peakHoldEnable   = config_.getBool("peak_hold", specDisplay_.peakHoldEnable);
    specDisplay_.peakHoldDecay    = config_.getFloat("peak_hold_decay", specDisplay_.peakHoldDecay);
    specDisplay_.additiveBlend    = config_.getBool("additive_blend", specDisplay_.additiveBlend);
    cursors_.snapToPeaks          = config_.getBool("snap_to_peaks", cursors_.snapToPeaks);
    displayPanel_.showRuler       = config_.getBool("show_ruler", displayPanel_.showRuler);
    measurements_.traceMinFreq    = config_.getFloat("trace_min_freq", measurements_.traceMinFreq);
    measurements_.traceMaxFreq    = config_.getFloat("trace_max_freq", measurements_.traceMaxFreq);
    ui_.specMinPixPerBin          = config_.getInt("spec_min_pix_per_bin", ui_.specMinPixPerBin);

    // Clamp
    controlPanel_.fftSizeIdx   = std::clamp(controlPanel_.fftSizeIdx, 0, ControlPanel::kNumFFTSizes - 1);
    controlPanel_.windowIdx    = std::clamp(controlPanel_.windowIdx, 0, static_cast<int>(WindowType::Count) - 1);
    controlPanel_.colorMapIdx  = std::clamp(controlPanel_.colorMapIdx, 0, static_cast<int>(ColorMapType::Count) - 1);
    displayPanel_.spectrumFrac = std::clamp(displayPanel_.spectrumFrac, 0.1f, 0.9f);

    // Restore device selection
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

    auto& settings = audio_.settings();
    settings.fftSize = ControlPanel::kFFTSizes[controlPanel_.fftSizeIdx];
    settings.overlap = controlPanel_.overlapPct / 100.0f;
    settings.window  = static_cast<WindowType>(controlPanel_.windowIdx);
    colorMap_.setType(static_cast<ColorMapType>(controlPanel_.colorMapIdx));
    SDL_GL_SetSwapInterval(vsync_ ? 1 : 0);
}

void Application::saveConfig() const {
    const auto& devices = audio_.devices();

    Config cfg;
    cfg.setInt("fft_size_idx", controlPanel_.fftSizeIdx);
    cfg.setFloat("overlap_pct", controlPanel_.overlapPct);
    cfg.setInt("window_idx", controlPanel_.windowIdx);
    cfg.setInt("colormap_idx", controlPanel_.colorMapIdx);
    cfg.setFloat("min_db", ui_.minDB);
    cfg.setFloat("max_db", ui_.maxDB);
    cfg.setInt("freq_scale", static_cast<int>(ui_.freqScale));
    cfg.setBool("vsync", vsync_);
    cfg.setFloat("ui_scale", uiScale_);
    cfg.setFloat("spectrum_frac", displayPanel_.spectrumFrac);
    cfg.setBool("show_sidebar", showSidebar_);
    cfg.setBool("peak_hold", specDisplay_.peakHoldEnable);
    cfg.setFloat("peak_hold_decay", specDisplay_.peakHoldDecay);
    cfg.setBool("additive_blend", specDisplay_.additiveBlend);
    cfg.setBool("snap_to_peaks", cursors_.snapToPeaks);
    cfg.setBool("show_ruler", displayPanel_.showRuler);
    cfg.setFloat("trace_min_freq", measurements_.traceMinFreq);
    cfg.setFloat("trace_max_freq", measurements_.traceMaxFreq);
    cfg.setInt("spec_min_pix_per_bin", ui_.specMinPixPerBin);

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
