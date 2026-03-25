#pragma once

#include "core/Types.h"
#include <imgui.h>
#include <vector>

namespace baudline {

struct ChannelStyle {
    ImU32 lineColor;
    ImU32 fillColor;
};

class SpectrumDisplay {
public:
    // Draw multiple channel spectra overlaid.
    // `spectra` has one entry per channel; `styles` has matching colors.
    void draw(const std::vector<std::vector<float>>& spectra,
              const std::vector<ChannelStyle>& styles,
              float minDB, float maxDB,
              double sampleRate, bool isIQ,
              FreqScale freqScale,
              float posX, float posY, float sizeX, float sizeY) const;

    // Convenience: single-channel draw (backward compat).
    void draw(const std::vector<float>& spectrumDB,
              float minDB, float maxDB,
              double sampleRate, bool isIQ,
              FreqScale freqScale,
              float posX, float posY, float sizeX, float sizeY) const;

    double screenXToFreq(float screenX, float posX, float sizeX,
                         double sampleRate, bool isIQ, FreqScale freqScale) const;
    float freqToScreenX(double freq, float posX, float sizeX,
                        double sampleRate, bool isIQ, FreqScale freqScale) const;
    float screenYToDB(float screenY, float posY, float sizeY,
                      float minDB, float maxDB) const;

    bool showGrid = true;
    bool fillSpectrum = false;
};

} // namespace baudline
