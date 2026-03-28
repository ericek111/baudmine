#pragma once

#include "core/Types.h"
#include "core/Config.h"
#include "audio/AudioEngine.h"
#include "ui/ColorMap.h"
#include "ui/WaterfallDisplay.h"
#include "ui/SpectrumDisplay.h"
#include "ui/Cursors.h"
#include "ui/Measurements.h"

#include <SDL.h>
#include <string>
#include <vector>

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
    void renderControlPanel();
    void renderSpectrumPanel();
    void renderWaterfallPanel();
    void handleSpectrumInput(float posX, float posY, float sizeX, float sizeY);

    void openDevice();
    void openMultiDevice();
    void openFile(const std::string& path, InputFormat format, double sampleRate);
    void updateAnalyzerSettings();
    void renderMathPanel();

    void loadConfig();
    void saveConfig() const;

    // SDL / GL / ImGui
    SDL_Window*   window_    = nullptr;
    SDL_GLContext  glContext_ = nullptr;
    bool           running_  = false;

    // Audio engine (owns sources, analyzers, math channels)
    AudioEngine audio_;

    // UI state
    ColorMap          colorMap_;
    WaterfallDisplay  waterfall_;
    SpectrumDisplay   specDisplay_;
    Cursors           cursors_;
    Measurements      measurements_;

    // Display settings
    float     minDB_       = -120.0f;
    float     maxDB_       = 0.0f;
    FreqScale freqScale_   = FreqScale::Linear;
    bool      paused_      = false;
    bool      vsync_       = true;
    float     uiScale_       = 0.0f;  // 0 = auto (use DPI), >0 = manual override
    float     appliedScale_  = 0.0f; // currently applied user-facing scale
    float     pendingScale_  = 0.0f; // deferred scale (applied before next frame)
    float     logicalScale_  = 1.0f; // scale after compensating for framebuffer DPI
    float     lastDpr_       = 0.0f; // last devicePixelRatio (to detect changes)
    void      applyUIScale(float scale);
    void      requestUIScale(float scale); // safe to call mid-frame
    void      syncCanvasSize();

    // FFT size options
    static constexpr int kFFTSizes[] = {256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
    static constexpr int kNumFFTSizes = 9;
    int fftSizeIdx_ = 4; // default 4096

    // Overlap (continuous 0–95%)
    float overlapPct_ = 50.0f;

    // Window
    int windowIdx_ = static_cast<int>(WindowType::BlackmanHarris);

    // Color map
    int colorMapIdx_ = static_cast<int>(ColorMapType::Magma);

    // File playback
    std::string filePath_;
    int         fileFormatIdx_ = 0;
    float       fileSampleRate_ = 48000.0f;
    bool        fileLoop_ = true;

    // Channel colors (up to kMaxChannels).  Defaults: L=green, R=purple.
    ImVec4 channelColors_[kMaxChannels] = {
        {0.20f, 0.90f, 0.30f, 1.0f},  // green
        {0.70f, 0.30f, 1.00f, 1.0f},  // purple
        {1.00f, 0.55f, 0.00f, 1.0f},  // orange
        {0.00f, 0.75f, 1.00f, 1.0f},  // cyan
        {1.00f, 0.25f, 0.25f, 1.0f},  // red
        {1.00f, 1.00f, 0.30f, 1.0f},  // yellow
        {0.50f, 0.80f, 0.50f, 1.0f},  // light green
        {0.80f, 0.50f, 0.80f, 1.0f},  // pink
    };
    int  waterfallChannel_ = 0;    // which channel drives the waterfall (single mode)
    bool waterfallMultiCh_ = true; // true = multi-channel overlay mode
    bool channelEnabled_[kMaxChannels] = {true,true,true,true,true,true,true,true};

    // Frequency zoom/pan (normalized 0–1 over full bandwidth)
    float viewLo_ = 0.0f;   // left edge
    float viewHi_ = 0.5f;   // right edge (default 2x zoom from left)

    // Spectrum/waterfall split ratio (fraction of content height for spectrum)
    float spectrumFrac_ = 0.35f;
    bool  draggingSplit_ = false;

    // Panel geometry (stored for cursor interaction)
    float specPosX_ = 0, specPosY_ = 0, specSizeX_ = 0, specSizeY_ = 0;
    float wfPosX_ = 0, wfPosY_ = 0, wfSizeX_ = 0, wfSizeY_ = 0;

    // Hover state: which panel is being hovered
    enum class HoverPanel { None, Spectrum, Waterfall };
    HoverPanel hoverPanel_ = HoverPanel::None;
    float      hoverWfTimeOffset_ = 0.0f;  // seconds from newest line

    // Touch gesture state (pinch-zoom / two-finger pan)
    struct TouchState {
        int    count = 0;         // active finger count
        float  startDist = 0.0f;  // initial distance between two fingers
        float  startLo = 0.0f;    // viewLo_ at gesture start
        float  startHi = 0.0f;    // viewHi_ at gesture start
        float  startCenterX = 0.0f; // midpoint screen-X at gesture start
        float  lastCenterX = 0.0f;  // last midpoint screen-X
        float  lastDist = 0.0f;   // last distance between fingers
    } touch_;
    void handleTouchEvent(const SDL_Event& event);

    // Config persistence
    Config config_;

    // UI visibility
    bool showSidebar_       = true;

#ifndef IMGUI_DISABLE_DEBUG_TOOLS
    // ImGui debug windows
    bool showDemoWindow_    = false;
    bool showMetricsWindow_ = false;
    bool showDebugLog_      = false;
    bool showStackTool_     = false;
#endif

    // Pre-allocated scratch buffers (avoid per-frame heap allocations)
    std::vector<std::vector<float>>  wfSpectraScratch_;
    std::vector<WaterfallChannelInfo> wfChInfoScratch_;
    std::vector<std::vector<float>>  allSpectraScratch_;
    std::vector<ChannelStyle>        stylesScratch_;
};

} // namespace baudmine
