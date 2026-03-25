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

    // Draw vertical markers and peak trace on the waterfall panel.
    void drawWaterfall(const SpectrumDisplay& specDisplay,
                       float posX, float posY, float sizeX, float sizeY,
                       double sampleRate, bool isIQ, FreqScale freqScale,
                       float viewLo, float viewHi,
                       int screenRows, int spectrumSize) const;

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
    bool showPeakTrace  = false;  // draw peak history curve on waterfall
    float traceMinFreq  = 0.0f;   // min frequency for peak trace (Hz), 0 = no limit
    float traceMaxFreq  = 0.0f;   // max frequency for peak trace (Hz), 0 = no limit

private:
    PeakInfo globalPeak_;             // always-tracked highest peak
    std::vector<PeakInfo> peaks_;

    double           lastSampleRate_ = 48000.0;
    // Peak history for waterfall trace (circular buffer, newest at peakHistIdx_)
    std::vector<int> peakHistBins_;   // bin index per waterfall line
    int              peakHistIdx_ = 0;
    int              peakHistLen_ = 0; // how many entries are valid

    // Find top-N peaks with minimum bin separation.
    void findPeaks(const std::vector<float>& spectrumDB, int maxN,
                   int minDist, float threshold);

    static double binToFreq(int bin, double sampleRate, bool isIQ, int fftSize);
};

} // namespace baudline
