#include "ui/SpectrumDisplay.h"
#include <cmath>
#include <algorithm>

namespace baudmine {

static float freqToLogFrac(double freq, double minFreq, double maxFreq) {
    if (freq <= 0 || minFreq <= 0) return 0.0f;
    double logMin = std::log10(minFreq);
    double logMax = std::log10(maxFreq);
    double logF   = std::log10(freq);
    return static_cast<float>((logF - logMin) / (logMax - logMin));
}

// Map a "full-range screen fraction" (0–1) to a bin fraction, applying log if needed.
static float screenFracToBinFrac(float frac, FreqScale freqScale, bool isIQ) {
    if (freqScale == FreqScale::Logarithmic && !isIQ) {
        constexpr float kMinBinFrac = 0.001f;
        float logMin = std::log10(kMinBinFrac);
        float logMax = 0.0f;
        return std::pow(10.0f, logMin + frac * (logMax - logMin));
    }
    return frac;
}

// Inverse: bin fraction → screen fraction.
static float binFracToScreenFrac(float binFrac, FreqScale freqScale, bool isIQ) {
    if (freqScale == FreqScale::Logarithmic && !isIQ) {
        constexpr float kMinBinFrac = 0.001f;
        float logMin = std::log10(kMinBinFrac);
        float logMax = 0.0f;
        if (binFrac < kMinBinFrac) binFrac = kMinBinFrac;
        return (std::log10(binFrac) - logMin) / (logMax - logMin);
    }
    return binFrac;
}

// Build a decimated polyline for one spectrum, considering view range.
static void buildPolyline(const std::vector<float>& spectrumDB,
                           float minDB, float maxDB,
                           bool isIQ, FreqScale freqScale,
                           float posX, float posY, float sizeX, float sizeY,
                           float viewLo, float viewHi,
                           std::vector<ImVec2>& outPoints) {
    int bins = static_cast<int>(spectrumDB.size());
    int displayPts = std::min(bins, static_cast<int>(sizeX));
    if (displayPts < 2) displayPts = 2;

    outPoints.resize(displayPts);
    for (int idx = 0; idx < displayPts; ++idx) {
        float screenFrac = static_cast<float>(idx) / (displayPts - 1);
        // Map screen pixel → full-range fraction via viewLo/viewHi
        float viewFrac = viewLo + screenFrac * (viewHi - viewLo);
        // Map to bin fraction (apply log scale if needed)
        float binFrac = screenFracToBinFrac(viewFrac, freqScale, isIQ);

        float binF = binFrac * (bins - 1);

        // Bucket range for peak-hold decimation.
        float prevViewFrac = (idx > 0)
            ? viewLo + static_cast<float>(idx - 1) / (displayPts - 1) * (viewHi - viewLo)
            : viewFrac;
        float nextViewFrac = (idx < displayPts - 1)
            ? viewLo + static_cast<float>(idx + 1) / (displayPts - 1) * (viewHi - viewLo)
            : viewFrac;
        float prevBinF = screenFracToBinFrac(prevViewFrac, freqScale, isIQ) * (bins - 1);
        float nextBinF = screenFracToBinFrac(nextViewFrac, freqScale, isIQ) * (bins - 1);

        int b0 = static_cast<int>((prevBinF + binF) * 0.5f);
        int b1 = static_cast<int>((binF + nextBinF) * 0.5f);
        b0 = std::clamp(b0, 0, bins - 1);
        b1 = std::clamp(b1, b0, bins - 1);

        float peakDB = spectrumDB[b0];
        for (int b = b0 + 1; b <= b1; ++b)
            peakDB = std::max(peakDB, spectrumDB[b]);

        float x = posX + screenFrac * sizeX;
        float dbNorm = std::clamp((peakDB - minDB) / (maxDB - minDB), 0.0f, 1.0f);
        float y = posY + sizeY * (1.0f - dbNorm);
        outPoints[idx] = {x, y};
    }
}

void SpectrumDisplay::draw(const std::vector<std::vector<float>>& spectra,
                           const std::vector<ChannelStyle>& styles,
                           float minDB, float maxDB,
                           double sampleRate, bool isIQ,
                           FreqScale freqScale,
                           float posX, float posY,
                           float sizeX, float sizeY,
                           float viewLo, float viewHi) const {
    if (spectra.empty() || spectra[0].empty() || sizeX <= 0 || sizeY <= 0) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    double freqFullMin = isIQ ? -sampleRate / 2.0 : 0.0;
    double freqFullMax = isIQ ?  sampleRate / 2.0 : sampleRate / 2.0;

    // Helper to convert a view fraction to frequency.
    auto viewFracToFreq = [&](float vf) -> double {
        float binFrac = screenFracToBinFrac(vf, freqScale, isIQ);
        return freqFullMin + binFrac * (freqFullMax - freqFullMin);
    };

    // Background
    dl->AddRectFilled({posX, posY}, {posX + sizeX, posY + sizeY},
                      IM_COL32(20, 20, 30, 255));

    // Grid lines
    if (showGrid) {
        ImU32 gridCol = IM_COL32(60, 60, 80, 128);
        ImU32 textCol = IM_COL32(180, 180, 200, 200);

        // ── Horizontal (dB) grid — adapt step to available height ──
        constexpr float kMinPixPerHLine = 40.0f;  // minimum pixels between labels
        float dbRange = maxDB - minDB;
        // Pick a nice step: 5, 10, 20, 50, ...
        float dbStep = 10.0f;
        static const float niceSteps[] = {5.0f, 10.0f, 20.0f, 50.0f, 100.0f};
        for (float s : niceSteps) {
            float pixPerStep = sizeY * s / dbRange;
            if (pixPerStep >= kMinPixPerHLine) { dbStep = s; break; }
        }

        for (float db = std::ceil(minDB / dbStep) * dbStep; db <= maxDB; db += dbStep) {
            float y = posY + sizeY * (1.0f - (db - minDB) / (maxDB - minDB));
            dl->AddLine({posX, y}, {posX + sizeX, y}, gridCol);
            char label[16];
            std::snprintf(label, sizeof(label), "%.0f", db);
            dl->AddText({posX + 2, y - 12}, textCol, label);
        }

        // ── Vertical (frequency) grid — adapt count to available width ──
        constexpr float kMinPixPerVLine = 80.0f;
        int numVLines = std::max(2, static_cast<int>(sizeX / kMinPixPerVLine));

        for (int i = 0; i <= numVLines; ++i) {
            float frac = static_cast<float>(i) / numVLines;
            float vf = viewLo + frac * (viewHi - viewLo);
            double freq = viewFracToFreq(vf);
            float x = posX + frac * sizeX;
            dl->AddLine({x, posY}, {x, posY + sizeY}, gridCol);

            char label[32];
            if (std::abs(freq) >= 1e6)
                std::snprintf(label, sizeof(label), "%.2fM", freq / 1e6);
            else if (std::abs(freq) >= 1e3)
                std::snprintf(label, sizeof(label), "%.1fk", freq / 1e3);
            else
                std::snprintf(label, sizeof(label), "%.0f", freq);
            dl->AddText({x + 2, posY + sizeY - 14}, textCol, label);
        }
    }

    // Draw each channel's spectrum.
    std::vector<ImVec2> points;
    int nCh = static_cast<int>(spectra.size());
    for (int ch = 0; ch < nCh; ++ch) {
        if (spectra[ch].empty()) continue;
        const ChannelStyle& st = (ch < static_cast<int>(styles.size()))
            ? styles[ch]
            : styles.back();

        buildPolyline(spectra[ch], minDB, maxDB,
                      isIQ, freqScale, posX, posY, sizeX, sizeY,
                      viewLo, viewHi, points);

        // Fill
        if (fillSpectrum && points.size() >= 2) {
            for (size_t i = 0; i + 1 < points.size(); ++i) {
                ImVec2 tl = points[i];
                ImVec2 tr = points[i + 1];
                ImVec2 bl = {tl.x, posY + sizeY};
                ImVec2 br = {tr.x, posY + sizeY};
                dl->AddQuadFilled(tl, tr, br, bl, st.fillColor);
            }
        }

        // Line
        if (points.size() >= 2)
            dl->AddPolyline(points.data(), static_cast<int>(points.size()),
                            st.lineColor, ImDrawFlags_None, 1.5f);
    }

    // Peak hold traces (drawn as dashed-style thin lines above the live spectrum).
    if (peakHoldEnable && !peakHold_.empty()) {
        for (int ch = 0; ch < nCh && ch < static_cast<int>(peakHold_.size()); ++ch) {
            if (peakHold_[ch].empty()) continue;
            const ChannelStyle& st = (ch < static_cast<int>(styles.size()))
                ? styles[ch] : styles.back();

            // Use the same line color but dimmer.
            ImU32 col = (st.lineColor & 0x00FFFFFF) | 0x90000000;

            buildPolyline(peakHold_[ch], minDB, maxDB,
                          isIQ, freqScale, posX, posY, sizeX, sizeY,
                          viewLo, viewHi, points);

            if (points.size() >= 2)
                dl->AddPolyline(points.data(), static_cast<int>(points.size()),
                                col, ImDrawFlags_None, 1.0f);
        }
    }

    // Border
    dl->AddRect({posX, posY}, {posX + sizeX, posY + sizeY},
                IM_COL32(100, 100, 120, 200));
}

// Single-channel convenience wrapper.
void SpectrumDisplay::draw(const std::vector<float>& spectrumDB,
                           float minDB, float maxDB,
                           double sampleRate, bool isIQ,
                           FreqScale freqScale,
                           float posX, float posY,
                           float sizeX, float sizeY) const {
    std::vector<std::vector<float>> spectra = {spectrumDB};
    std::vector<ChannelStyle> styles = {{IM_COL32(0, 255, 128, 255),
                                         IM_COL32(0, 255, 128, 40)}};
    draw(spectra, styles, minDB, maxDB, sampleRate, isIQ, freqScale,
         posX, posY, sizeX, sizeY);
}

double SpectrumDisplay::screenXToFreq(float screenX, float posX, float sizeX,
                                       double sampleRate, bool isIQ,
                                       FreqScale freqScale,
                                       float viewLo, float viewHi) const {
    float screenFrac = std::clamp((screenX - posX) / sizeX, 0.0f, 1.0f);
    // Map screen fraction to view fraction
    float viewFrac = viewLo + screenFrac * (viewHi - viewLo);
    // Map view fraction to bin fraction (undo log if needed)
    float binFrac = screenFracToBinFrac(viewFrac, freqScale, isIQ);

    double freqMin = isIQ ? -sampleRate / 2.0 : 0.0;
    double freqMax = isIQ ?  sampleRate / 2.0 : sampleRate / 2.0;
    return freqMin + binFrac * (freqMax - freqMin);
}

float SpectrumDisplay::freqToScreenX(double freq, float posX, float sizeX,
                                      double sampleRate, bool isIQ,
                                      FreqScale freqScale,
                                      float viewLo, float viewHi) const {
    double freqMin = isIQ ? -sampleRate / 2.0 : 0.0;
    double freqMax = isIQ ?  sampleRate / 2.0 : sampleRate / 2.0;

    // Freq → bin fraction
    float binFrac = static_cast<float>((freq - freqMin) / (freqMax - freqMin));
    // Bin fraction → full-range screen fraction (apply log inverse)
    float viewFrac = binFracToScreenFrac(binFrac, freqScale, isIQ);
    // View fraction → screen fraction
    float screenFrac = (viewFrac - viewLo) / (viewHi - viewLo);
    return posX + screenFrac * sizeX;
}

float SpectrumDisplay::screenYToDB(float screenY, float posY, float sizeY,
                                    float minDB, float maxDB) const {
    float frac = 1.0f - (screenY - posY) / sizeY;
    frac = std::clamp(frac, 0.0f, 1.0f);
    return minDB + frac * (maxDB - minDB);
}

void SpectrumDisplay::updatePeakHold(const std::vector<std::vector<float>>& spectra) {
    if (!peakHoldEnable) return;

    int nCh = static_cast<int>(spectra.size());

    // Grow/shrink channel count.
    if (static_cast<int>(peakHold_.size()) != nCh) {
        peakHold_.resize(nCh);
    }

    for (int ch = 0; ch < nCh; ++ch) {
        int bins = static_cast<int>(spectra[ch].size());
        if (bins == 0) continue;

        // Reset if bin count changed.
        if (static_cast<int>(peakHold_[ch].size()) != bins)
            peakHold_[ch].assign(bins, -200.0f);

        float dt = ImGui::GetIO().DeltaTime;  // seconds since last frame
        float decayThisFrame = peakHoldDecay * dt;

        for (int i = 0; i < bins; ++i) {
            if (spectra[ch][i] >= peakHold_[ch][i]) {
                peakHold_[ch][i] = spectra[ch][i];
            } else {
                peakHold_[ch][i] -= decayThisFrame;
                if (peakHold_[ch][i] < spectra[ch][i])
                    peakHold_[ch][i] = spectra[ch][i];
            }
        }
    }
}

void SpectrumDisplay::clearPeakHold() {
    peakHold_.clear();
}

} // namespace baudmine
