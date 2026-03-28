#pragma once

#include "core/Types.h"
#include "core/Config.h"
#include "audio/AudioEngine.h"
#include "ui/UIState.h"
#include "ui/ControlPanel.h"
#include "ui/DisplayPanel.h"
#include "ui/ColorMap.h"
#include "ui/WaterfallDisplay.h"
#include "ui/SpectrumDisplay.h"
#include "ui/Cursors.h"
#include "ui/Measurements.h"

#include <SDL.h>
#include <string>

namespace baudmine {

class Application {
public:
    Application();
    ~Application();

    bool init(int argc, char** argv);
    void run();
    void mainLoopStep();  // single iteration (public for Emscripten callback)
    void shutdown();

private:
    void processAudio();
    void render();

    void openDevice();
    void openMultiDevice();
    void openFile(const std::string& path, InputFormat format, double sampleRate);
    void updateAnalyzerSettings();

    void loadConfig();
    void saveConfig() const;

    // SDL / GL / ImGui
    SDL_Window*   window_    = nullptr;
    SDL_GLContext  glContext_ = nullptr;
    bool           running_  = false;

    // Core subsystems
    AudioEngine   audio_;
    UIState       ui_;
    ControlPanel  controlPanel_;
    DisplayPanel  displayPanel_;

    // Shared UI components
    ColorMap          colorMap_;
    WaterfallDisplay  waterfall_;
    SpectrumDisplay   specDisplay_;
    Cursors           cursors_;
    Measurements      measurements_;

    // UI scaling
    bool      vsync_         = true;
    float     uiScale_       = 0.0f;
    float     appliedScale_  = 0.0f;
    float     pendingScale_  = 0.0f;
    float     logicalScale_  = 1.0f;
    float     lastDpr_       = 0.0f;
    void      applyUIScale(float scale);
    void      requestUIScale(float scale);
    void      syncCanvasSize();

    // UI visibility
    bool showSidebar_ = true;

#ifndef IMGUI_DISABLE_DEBUG_TOOLS
    bool showDemoWindow_    = false;
    bool showMetricsWindow_ = false;
    bool showDebugLog_      = false;
    bool showStackTool_     = false;
#endif

    // Config persistence
    Config config_;
};

} // namespace baudmine
