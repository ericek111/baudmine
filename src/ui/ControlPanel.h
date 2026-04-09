#pragma once

#include "core/Types.h"
#include "ui/UIState.h"

#include <string>

namespace baudmine {

class AudioEngine;
class SpectrumDisplay;
class Cursors;
class Measurements;
class ColorMap;
class WaterfallDisplay;

class ControlPanel {
public:
    void render(AudioEngine& audio, UIState& ui,
                SpectrumDisplay& specDisplay, Cursors& cursors,
                Measurements& measurements, ColorMap& colorMap,
                WaterfallDisplay& waterfall);

    // Consume action flags set during render(). Returns true once, then resets.
    bool consumeSaveRequest()   { bool v = needsSave_; needsSave_ = false; return v; }
    bool consumeUpdateRequest() { bool v = needsUpdate_; needsUpdate_ = false; return v; }

    // FFT / analysis controls
    static constexpr int kFFTSizes[] = {256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
    static constexpr int kNumFFTSizes = 9;
    int   fftSizeIdx  = 4;
    float overlapPct  = 50.0f;
    int   windowIdx   = static_cast<int>(WindowType::BlackmanHarris);
    int   colorMapIdx = static_cast<int>(ColorMapType::Magma);

    // File playback
    std::string filePath;
    int         fileFormatIdx  = 0;
    float       fileSampleRate = 48000.0f;
    bool        fileLoop       = true;

private:
    void renderMathPanel(AudioEngine& audio);

    bool needsSave_   = false;
    bool needsUpdate_ = false;

    // Overrun display
    int   lastOverrunCount_ = 0;
    float lastOverrunTime_  = 0.0f;  // ImGui time of last overrun

    void flagSave()   { needsSave_ = true; }
    void flagUpdate() { needsUpdate_ = true; }
};

} // namespace baudmine
