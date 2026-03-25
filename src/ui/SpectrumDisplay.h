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
    // viewLo/viewHi (0–1) control the visible frequency range (zoom/pan).
    void draw(const std::vector<std::vector<float>>& spectra,
              const std::vector<ChannelStyle>& styles,
              float minDB, float maxDB,
              double sampleRate, bool isIQ,
              FreqScale freqScale,
              float posX, float posY, float sizeX, float sizeY,
              float viewLo = 0.0f, float viewHi = 1.0f) const;

    // Convenience: single-channel draw (backward compat).
    void draw(const std::vector<float>& spectrumDB,
              float minDB, float maxDB,
              double sampleRate, bool isIQ,
              FreqScale freqScale,
              float posX, float posY, float sizeX, float sizeY) const;

    double screenXToFreq(float screenX, float posX, float sizeX,
                         double sampleRate, bool isIQ, FreqScale freqScale,
                         float viewLo = 0.0f, float viewHi = 1.0f) const;
    float freqToScreenX(double freq, float posX, float sizeX,
                        double sampleRate, bool isIQ, FreqScale freqScale,
                        float viewLo = 0.0f, float viewHi = 1.0f) const;
    float screenYToDB(float screenY, float posY, float sizeY,
                      float minDB, float maxDB) const;

    // Peak hold: update with current spectra, then draw the held peaks.
    void updatePeakHold(const std::vector<std::vector<float>>& spectra);
    void clearPeakHold();

    bool  showGrid       = true;
    bool  fillSpectrum   = false;
    bool  peakHoldEnable = false;
    float peakHoldDecay  = 20.0f;  // dB/second decay rate

private:
    // One peak-hold trace per channel.
    mutable std::vector<std::vector<float>> peakHold_;
};

} // namespace baudline
