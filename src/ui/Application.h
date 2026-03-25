#pragma once

#include "core/Types.h"
#include "dsp/SpectrumAnalyzer.h"
#include "audio/AudioSource.h"
#include "audio/PortAudioSource.h"
#include "ui/ColorMap.h"
#include "ui/WaterfallDisplay.h"
#include "ui/SpectrumDisplay.h"
#include "ui/Cursors.h"

#include <SDL.h>
#include <memory>
#include <string>
#include <vector>

namespace baudline {

class Application {
public:
    Application();
    ~Application();

    bool init(int argc, char** argv);
    void run();
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

    // Display settings
    float     minDB_       = -120.0f;
    float     maxDB_       = 0.0f;
    FreqScale freqScale_   = FreqScale::Linear;
    bool      paused_      = false;
    int       waterfallW_  = 0;
    int       waterfallH_  = 0;

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
    std::vector<PortAudioSource::DeviceInfo> paDevices_;
    int paDeviceIdx_ = 0;

    // Channel colors (up to kMaxChannels).  Defaults: L=purple, R=green.
    ImVec4 channelColors_[kMaxChannels] = {
        {0.70f, 0.30f, 1.00f, 1.0f},  // purple
        {0.20f, 0.90f, 0.30f, 1.0f},  // green
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

    // Spectrum panel geometry (stored for cursor interaction)
    float specPosX_ = 0, specPosY_ = 0, specSizeX_ = 0, specSizeY_ = 0;
};

} // namespace baudline
