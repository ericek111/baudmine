#pragma once

#include "core/Types.h"
#include "ui/SpectrumDisplay.h"
#include <vector>

namespace baudline {

struct PeakInfo {
    int    bin  = 0;
    double freq = 0.0;     // Hz
    float  dB   = -200.0f;
};

class Measurements {
public:
    // Detect peaks from the spectrum.  Call once per frame with fresh data.
    void update(const std::vector<float>& spectrumDB,
                double sampleRate, bool isIQ, int fftSize);

    // Draw markers on the spectrum display area.
    void draw(const SpectrumDisplay& specDisplay,
              float posX, float posY, float sizeX, float sizeY,
              double sampleRate, bool isIQ, FreqScale freqScale,
              float minDB, float maxDB,
              float viewLo, float viewHi) const;

    // Draw vertical markers on the waterfall panel.
    void drawWaterfall(const SpectrumDisplay& specDisplay,
                       float posX, float posY, float sizeX, float sizeY,
                       double sampleRate, bool isIQ, FreqScale freqScale,
                       float viewLo, float viewHi) const;

    // Draw sidebar panel (ImGui widgets).
    void drawPanel();

    // Current detected peaks (sorted by amplitude, highest first).
    const std::vector<PeakInfo>& peaks() const { return peaks_; }

    // Settings
    bool enabled       = false;  // master enable
    int  maxPeaks      = 5;      // how many peaks to detect (1–20)
    int  minPeakDist   = 10;     // minimum distance between peaks in bins
    float peakThreshold = -120.0f; // ignore peaks below this dB
    bool showOnSpectrum = true;   // draw markers on spectrum
    bool showOnWaterfall = false; // draw vertical lines on waterfall

private:
    std::vector<PeakInfo> peaks_;

    // Find top-N peaks with minimum bin separation.
    void findPeaks(const std::vector<float>& spectrumDB, int maxN,
                   int minDist, float threshold);

    static double binToFreq(int bin, double sampleRate, bool isIQ, int fftSize);
};

} // namespace baudline
