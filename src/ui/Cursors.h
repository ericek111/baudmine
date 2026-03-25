#pragma once

#include "core/Types.h"
#include "ui/SpectrumDisplay.h"
#include <deque>
#include <vector>

namespace baudline {

struct CursorInfo {
    bool   active = false;
    double freq   = 0.0;     // Hz
    float  dB     = -200.0f;
    int    bin    = 0;
};

class Cursors {
public:
    // Update cursor positions from mouse input and spectrum data.
    void update(const std::vector<float>& spectrumDB,
                double sampleRate, bool isIQ, int fftSize);

    // Draw cursor overlays on the spectrum display area.
    void draw(const SpectrumDisplay& specDisplay,
              float posX, float posY, float sizeX, float sizeY,
              double sampleRate, bool isIQ, FreqScale freqScale,
              float minDB, float maxDB,
              float viewLo = 0.0f, float viewHi = 1.0f) const;

    // Draw cursor readout panel (ImGui widgets).
    void drawPanel();

    // Set cursor A/B positions from mouse click.
    void setCursorA(double freq, float dB, int bin);
    void setCursorB(double freq, float dB, int bin);

    // Auto-find peak and set cursor to it.
    void snapToPeak(const std::vector<float>& spectrumDB,
                    double sampleRate, bool isIQ, int fftSize);

    // Find peak near a given bin (within a window).
    int findLocalPeak(const std::vector<float>& spectrumDB,
                      int centerBin, int window = 20) const;

    CursorInfo cursorA;
    CursorInfo cursorB;
    bool       showDelta = true;
    bool       snapToPeaks = true;

    // Hover cursor (follows mouse, always active)
    CursorInfo hover;

    // Averaging: displayed dB is the mean of the last N samples.
    int avgCount = 1;  // 1 = no averaging

    // Averaged dB values (used for display and delta).
    float avgDBA() const;
    float avgDBB() const;

private:
    // Averaging state per cursor.
    struct AvgState {
        std::deque<float> samples;
        double sum = 0.0;
        int    lastBin = -1;  // reset when cursor moves
    };
    mutable AvgState avgA_, avgB_;
    void pushAvg(AvgState& st, float dB, int bin) const;
};

} // namespace baudline
