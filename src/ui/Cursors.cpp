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

void Cursors::pushAvg(AvgState& st, float dB, int bin) const {
    // Reset if cursor moved to a different bin or averaging was reduced.
    if (bin != st.lastBin) {
        st.samples.clear();
        st.sum = 0.0;
        st.lastBin = bin;
    }
    st.samples.push_back(dB);
    st.sum += dB;
    int maxN = std::max(1, avgCount);
    while (static_cast<int>(st.samples.size()) > maxN) {
        st.sum -= st.samples.front();
        st.samples.pop_front();
    }
}

float Cursors::avgDBA() const {
    return avgA_.samples.empty() ? cursorA.dB
         : static_cast<float>(avgA_.sum / avgA_.samples.size());
}

float Cursors::avgDBB() const {
    return avgB_.samples.empty() ? cursorB.dB
         : static_cast<float>(avgB_.sum / avgB_.samples.size());
}

void Cursors::update(const std::vector<float>& spectrumDB,
                     double sampleRate, bool isIQ, int fftSize) {
    // Update dB values at cursor bin positions
    if (cursorA.active && cursorA.bin >= 0 &&
        cursorA.bin < static_cast<int>(spectrumDB.size())) {
        cursorA.dB = spectrumDB[cursorA.bin];
        pushAvg(avgA_, cursorA.dB, cursorA.bin);
    }
    if (cursorB.active && cursorB.bin >= 0 &&
        cursorB.bin < static_cast<int>(spectrumDB.size())) {
        cursorB.dB = spectrumDB[cursorB.bin];
        pushAvg(avgB_, cursorB.dB, cursorB.bin);
    }
}

void Cursors::draw(const SpectrumDisplay& specDisplay,
                   float posX, float posY, float sizeX, float sizeY,
                   double sampleRate, bool isIQ, FreqScale freqScale,
                   float minDB, float maxDB,
                   float viewLo, float viewHi) const {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Draw cursor lines and crosshairs (no labels here).
    auto drawCursorMarker = [&](const CursorInfo& c, float dispDB, ImU32 color) {
        if (!c.active) return;
        float x = specDisplay.freqToScreenX(c.freq, posX, sizeX,
                                             sampleRate, isIQ, freqScale,
                                             viewLo, viewHi);
        float dbNorm = (dispDB - minDB) / (maxDB - minDB);
        dbNorm = std::clamp(dbNorm, 0.0f, 1.0f);
        float y = posY + sizeY * (1.0f - dbNorm);

        dl->AddLine({x, posY}, {x, posY + sizeY}, color, 1.0f);
        dl->AddLine({posX, y}, {posX + sizeX, y}, color & 0x80FFFFFF, 1.0f);
        dl->AddCircle({x, y}, 5.0f, color, 12, 2.0f);
    };

    // Format a cursor label string.
    auto formatLabel = [](char* buf, size_t sz, const char* label, double freq, float dB) {
        fmtFreqDB(buf, sz, label, freq, dB);
    };

    float aDB = avgDBA(), bDB = avgDBB();
    drawCursorMarker(cursorA, aDB, IM_COL32(255, 255, 0, 220));
    drawCursorMarker(cursorB, bDB, IM_COL32(0, 200, 255, 220));

    // Draw labels at the top, touching the cursor's vertical line.
    // If the label would overflow the right edge, flip it to the left side.
    auto drawCursorLabel = [&](const CursorInfo& c, float dispDB, ImU32 color,
                               const char* label, int row) {
        if (!c.active) return;
        float x = specDisplay.freqToScreenX(c.freq, posX, sizeX,
                                             sampleRate, isIQ, freqScale,
                                             viewLo, viewHi);
        char buf[128];
        formatLabel(buf, sizeof(buf), label, c.freq, dispDB);
        ImVec2 sz = ImGui::CalcTextSize(buf);
        float lineH = ImGui::GetTextLineHeight();
        float ty = posY + 4 + row * (lineH + 4);

        // Place right of cursor line; flip left if it would overflow.
        float tx;
        if (x + 6 + sz.x + 2 <= posX + sizeX)
            tx = x + 6;
        else
            tx = x - 6 - sz.x;

        dl->AddRectFilled({tx - 2, ty - 1}, {tx + sz.x + 2, ty + sz.y + 1},
                          IM_COL32(0, 0, 0, 180));
        dl->AddText({tx, ty}, color, buf);
    };

    drawCursorLabel(cursorA, aDB, IM_COL32(255, 255, 0, 220), "A", 0);
    drawCursorLabel(cursorB, bDB, IM_COL32(0, 200, 255, 220), "B", cursorA.active ? 1 : 0);

    // Delta display (two lines, column-aligned on '=')
    if (showDelta && cursorA.active && cursorB.active) {
        double dFreq = cursorB.freq - cursorA.freq;
        float dDB = bDB - aDB;
        char val1[48], val2[48];
        fmtFreq(val1, sizeof(val1), dFreq);
        std::snprintf(val2, sizeof(val2), "%.1f dB", dDB);

        ImVec2 labelSz = ImGui::CalcTextSize("dF = ");
        ImVec2 v1Sz = ImGui::CalcTextSize(val1);
        ImVec2 v2Sz = ImGui::CalcTextSize(val2);
        float valW = std::max(v1Sz.x, v2Sz.x);
        float lineH = labelSz.y;
        float totalW = labelSz.x + valW;
        float tx = posX + sizeX - totalW - 158;
        float ty = posY + 4;
        ImU32 col = IM_COL32(255, 200, 100, 255);
        float eqX = tx + labelSz.x;  // values start here (right of '= ')

        dl->AddText({tx, ty}, col, "dF =");
        dl->AddText({eqX + valW - v1Sz.x, ty}, col, val1);
        dl->AddText({tx, ty + lineH + 2}, col, "dA =");
        dl->AddText({eqX + valW - v2Sz.x, ty + lineH + 2}, col, val2);
    }

    // (Hover cursor line is drawn cross-panel by Application.)
}

void Cursors::drawPanel() {
    auto showCursor = [](const char* label, const CursorInfo& c, float dispDB) {
        if (!c.active) {
            ImGui::TextDisabled("%s: --", label);
            return;
        }
        char buf[128];
        fmtFreqDB(buf, sizeof(buf), label, c.freq, dispDB);
        ImGui::Text("%s", buf);
    };

    float aDB = avgDBA(), bDB = avgDBB();
    showCursor("A", cursorA, aDB);
    showCursor("B", cursorB, bDB);

    if (cursorA.active && cursorB.active) {
        double dF = cursorB.freq - cursorA.freq;
        float dA = bDB - aDB;
        char dbuf[128];
        fmtFreqDB(dbuf, sizeof(dbuf), "D", dF, dA);
        ImGui::Text("%s", dbuf);
    }

    // Averaging slider (logarithmic scale)
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderInt("##avgcount", &avgCount, 1, 20000, avgCount == 1 ? "No avg" : "Avg: %d",
                     ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cursor averaging (samples)");
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
