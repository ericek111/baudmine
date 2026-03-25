#include "ui/Cursors.h"
#include <imgui.h>
#include <cmath>
#include <algorithm>

namespace baudline {

static double binToFreqHelper(int bin, double sampleRate, bool isIQ, int fftSize) {
    if (isIQ) {
        return -sampleRate / 2.0 + (static_cast<double>(bin) / fftSize) * sampleRate;
    } else {
        return (static_cast<double>(bin) / fftSize) * sampleRate;
    }
}

void Cursors::update(const std::vector<float>& spectrumDB,
                     double sampleRate, bool isIQ, int fftSize) {
    // Update dB values at cursor bin positions
    if (cursorA.active && cursorA.bin >= 0 &&
        cursorA.bin < static_cast<int>(spectrumDB.size())) {
        cursorA.dB = spectrumDB[cursorA.bin];
    }
    if (cursorB.active && cursorB.bin >= 0 &&
        cursorB.bin < static_cast<int>(spectrumDB.size())) {
        cursorB.dB = spectrumDB[cursorB.bin];
    }
}

void Cursors::draw(const SpectrumDisplay& specDisplay,
                   float posX, float posY, float sizeX, float sizeY,
                   double sampleRate, bool isIQ, FreqScale freqScale,
                   float minDB, float maxDB) const {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    auto drawCursor = [&](const CursorInfo& c, ImU32 color, const char* label) {
        if (!c.active) return;
        float x = specDisplay.freqToScreenX(c.freq, posX, sizeX,
                                             sampleRate, isIQ, freqScale);
        float dbNorm = (c.dB - minDB) / (maxDB - minDB);
        dbNorm = std::clamp(dbNorm, 0.0f, 1.0f);
        float y = posY + sizeY * (1.0f - dbNorm);

        // Vertical line
        dl->AddLine({x, posY}, {x, posY + sizeY}, color, 1.0f);
        // Horizontal line
        dl->AddLine({posX, y}, {posX + sizeX, y}, color & 0x80FFFFFF, 1.0f);
        // Crosshair
        dl->AddCircle({x, y}, 5.0f, color, 12, 2.0f);

        // Label
        char buf[128];
        if (std::abs(c.freq) >= 1e6)
            std::snprintf(buf, sizeof(buf), "%s: %.6f MHz  %.1f dB",
                          label, c.freq / 1e6, c.dB);
        else if (std::abs(c.freq) >= 1e3)
            std::snprintf(buf, sizeof(buf), "%s: %.3f kHz  %.1f dB",
                          label, c.freq / 1e3, c.dB);
        else
            std::snprintf(buf, sizeof(buf), "%s: %.1f Hz  %.1f dB",
                          label, c.freq, c.dB);

        ImVec2 textSize = ImGui::CalcTextSize(buf);
        float tx = std::min(x + 8, posX + sizeX - textSize.x - 4);
        float ty = std::max(y - 18, posY + 2);
        dl->AddRectFilled({tx - 2, ty - 1}, {tx + textSize.x + 2, ty + textSize.y + 1},
                          IM_COL32(0, 0, 0, 180));
        dl->AddText({tx, ty}, color, buf);
    };

    drawCursor(cursorA, IM_COL32(255, 255, 0, 220), "A");
    drawCursor(cursorB, IM_COL32(0, 200, 255, 220), "B");

    // Delta display
    if (showDelta && cursorA.active && cursorB.active) {
        double dFreq = cursorB.freq - cursorA.freq;
        float dDB = cursorB.dB - cursorA.dB;
        char buf[128];
        if (std::abs(dFreq) >= 1e6)
            std::snprintf(buf, sizeof(buf), "dF=%.6f MHz  dA=%.1f dB",
                          dFreq / 1e6, dDB);
        else if (std::abs(dFreq) >= 1e3)
            std::snprintf(buf, sizeof(buf), "dF=%.3f kHz  dA=%.1f dB",
                          dFreq / 1e3, dDB);
        else
            std::snprintf(buf, sizeof(buf), "dF=%.1f Hz  dA=%.1f dB",
                          dFreq, dDB);

        ImVec2 textSize = ImGui::CalcTextSize(buf);
        float tx = posX + sizeX - textSize.x - 8;
        float ty = posY + 4;
        dl->AddRectFilled({tx - 4, ty - 2}, {tx + textSize.x + 4, ty + textSize.y + 2},
                          IM_COL32(0, 0, 0, 200));
        dl->AddText({tx, ty}, IM_COL32(255, 200, 100, 255), buf);
    }

    // Hover cursor
    if (hover.active) {
        float x = specDisplay.freqToScreenX(hover.freq, posX, sizeX,
                                             sampleRate, isIQ, freqScale);
        dl->AddLine({x, posY}, {x, posY + sizeY}, IM_COL32(200, 200, 200, 80), 1.0f);
    }
}

void Cursors::drawPanel() const {
    ImGui::Text("Cursors:");
    ImGui::Separator();

    auto showCursor = [](const char* label, const CursorInfo& c) {
        if (!c.active) {
            ImGui::Text("%s: (inactive)", label);
            return;
        }
        if (std::abs(c.freq) >= 1e6)
            ImGui::Text("%s: %.6f MHz, %.1f dB", label, c.freq / 1e6, c.dB);
        else if (std::abs(c.freq) >= 1e3)
            ImGui::Text("%s: %.3f kHz, %.1f dB", label, c.freq / 1e3, c.dB);
        else
            ImGui::Text("%s: %.1f Hz, %.1f dB", label, c.freq, c.dB);
    };

    showCursor("A", cursorA);
    showCursor("B", cursorB);

    if (cursorA.active && cursorB.active) {
        double dF = cursorB.freq - cursorA.freq;
        float dA = cursorB.dB - cursorA.dB;
        ImGui::Separator();
        if (std::abs(dF) >= 1e6)
            ImGui::Text("Delta: %.6f MHz, %.1f dB", dF / 1e6, dA);
        else if (std::abs(dF) >= 1e3)
            ImGui::Text("Delta: %.3f kHz, %.1f dB", dF / 1e3, dA);
        else
            ImGui::Text("Delta: %.1f Hz, %.1f dB", dF, dA);
    }

    if (hover.active) {
        ImGui::Separator();
        if (std::abs(hover.freq) >= 1e6)
            ImGui::Text("Hover: %.6f MHz, %.1f dB", hover.freq / 1e6, hover.dB);
        else if (std::abs(hover.freq) >= 1e3)
            ImGui::Text("Hover: %.3f kHz, %.1f dB", hover.freq / 1e3, hover.dB);
        else
            ImGui::Text("Hover: %.1f Hz, %.1f dB", hover.freq, hover.dB);
    }
}

void Cursors::setCursorA(double freq, float dB, int bin) {
    cursorA = {true, freq, dB, bin};
}

void Cursors::setCursorB(double freq, float dB, int bin) {
    cursorB = {true, freq, dB, bin};
}

void Cursors::snapToPeak(const std::vector<float>& spectrumDB,
                          double sampleRate, bool isIQ, int fftSize) {
    if (spectrumDB.empty()) return;
    auto it = std::max_element(spectrumDB.begin(), spectrumDB.end());
    int bin = static_cast<int>(std::distance(spectrumDB.begin(), it));
    double freq = binToFreqHelper(bin, sampleRate, isIQ, fftSize);
    setCursorA(freq, *it, bin);
}

int Cursors::findLocalPeak(const std::vector<float>& spectrumDB,
                            int centerBin, int window) const {
    int bins = static_cast<int>(spectrumDB.size());
    int lo = std::max(0, centerBin - window);
    int hi = std::min(bins - 1, centerBin + window);
    int best = lo;
    for (int i = lo + 1; i <= hi; ++i) {
        if (spectrumDB[i] > spectrumDB[best]) best = i;
    }
    return best;
}

} // namespace baudline
