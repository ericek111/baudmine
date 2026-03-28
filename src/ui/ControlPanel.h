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
    // Render the sidebar.  Returns true if config should be saved.
    void render(AudioEngine& audio, UIState& ui,
                SpectrumDisplay& specDisplay, Cursors& cursors,
                Measurements& measurements, ColorMap& colorMap,
                WaterfallDisplay& waterfall);

    // Action flags — checked and cleared by Application after render().
    bool needsSave()           { bool v = needsSave_; needsSave_ = false; return v; }
    bool needsAnalyzerUpdate() { bool v = needsUpdate_; needsUpdate_ = false; return v; }

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

    void flagSave()   { needsSave_ = true; }
    void flagUpdate() { needsUpdate_ = true; }
};

} // namespace baudmine
