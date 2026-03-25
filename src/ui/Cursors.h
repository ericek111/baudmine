#pragma once

#include "core/Types.h"
#include "ui/SpectrumDisplay.h"
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
    void drawPanel() const;

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

    // Hover cursor (follows mouse, always active)
    CursorInfo hover;
};

} // namespace baudline
