#pragma once

#include "core/Types.h"
#include "core/Config.h"
#include "dsp/SpectrumAnalyzer.h"
#include "audio/AudioSource.h"
#include "audio/MiniAudioSource.h"
#include "ui/ColorMap.h"
#include "ui/WaterfallDisplay.h"
#include "ui/SpectrumDisplay.h"
#include "ui/Cursors.h"
#include "ui/Measurements.h"

#include <SDL.h>
#include <complex>
#include <memory>
#include <string>
#include <vector>

namespace baudline {

// ── Channel math operations ──────────────────────────────────────────────────

enum class MathOp {
    // Unary (on channel X)
    Negate,       // -x  (negate dB)
    Absolute,     // |x| (absolute value of dB)
    Square,       // x^2 in linear  → 2*x_dB
    Cube,         // x^3 in linear  → 3*x_dB
    Sqrt,         // sqrt in linear → 0.5*x_dB
    Log,          // 10*log10(10^(x_dB/10) + 1)  — compressed scale
    // Binary (on channels X and Y)
    Add,          // linear(x) + linear(y)  → dB
    Subtract,     // |linear(x) - linear(y)| → dB
    Multiply,     // x_dB + y_dB  (multiply in linear = add in dB)
    Phase,        // angle(X_cplx * conj(Y_cplx)) in degrees
    CrossCorr,    // |X_cplx * conj(Y_cplx)| → dB
    Count
};

inline bool mathOpIsBinary(MathOp op) {
    return op >= MathOp::Add;
}

inline const char* mathOpName(MathOp op) {
    switch (op) {
        case MathOp::Negate:    return "-x";
        case MathOp::Absolute:  return "|x|";
        case MathOp::Square:    return "x^2";
        case MathOp::Cube:      return "x^3";
        case MathOp::Sqrt:      return "sqrt(x)";
        case MathOp::Log:       return "log(x)";
        case MathOp::Add:       return "x + y";
        case MathOp::Subtract:  return "x - y";
        case MathOp::Multiply:  return "x * y";
        case MathOp::Phase:     return "phase(x,y)";
        case MathOp::CrossCorr: return "xcorr(x,y)";
        default:                return "?";
    }
}

struct MathChannel {
    MathOp op        = MathOp::Subtract;
    int    sourceX   = 0;
    int    sourceY   = 1;
    ImVec4 color     = {1.0f, 1.0f, 1.0f, 1.0f};
    bool   enabled   = true;
    bool   waterfall = false;  // include on waterfall overlay
};

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

    void openPortAudio();
    void openFile(const std::string& path, InputFormat format, double sampleRate);
    void updateAnalyzerSettings();
    void computeMathChannels();
    void renderMathPanel();

    void loadConfig();
    void saveConfig() const;

    // SDL / GL / ImGui
    SDL_Window*   window_    = nullptr;
    SDL_GLContext  glContext_ = nullptr;
    bool           running_  = false;

    // Audio
    std::unique_ptr<AudioSource> audioSource_;
    std::vector<float>           audioBuf_;     // temp read buffer

    // DSP
    SpectrumAnalyzer analyzer_;
    AnalyzerSettings settings_;

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
    // (waterfallW_ removed — texture width tracks bin count automatically)
    // (waterfallH_ removed — fixed history depth of 1024 rows)

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

    // Device selection
    std::vector<MiniAudioSource::DeviceInfo> paDevices_;
    int paDeviceIdx_ = 0;

    // Channel colors (up to kMaxChannels).  Defaults: L=purple, R=green.
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

    // Math channels
    std::vector<MathChannel>          mathChannels_;
    std::vector<std::vector<float>>   mathSpectra_;  // computed each frame

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

    // Config persistence
    Config config_;

    // UI visibility
    bool showSidebar_       = true;

    // ImGui debug windows
    bool showDemoWindow_    = false;
    bool showMetricsWindow_ = false;
    bool showDebugLog_      = false;
    bool showStackTool_     = false;

    // Pre-allocated scratch buffers (avoid per-frame heap allocations)
    std::vector<std::vector<float>>  wfSpectraScratch_;
    std::vector<WaterfallChannelInfo> wfChInfoScratch_;
    std::vector<std::vector<float>>  allSpectraScratch_;
    std::vector<ChannelStyle>        stylesScratch_;
};

} // namespace baudline
