#include "ui/SpectrumDisplay.h"
#include <cmath>
#include <algorithm>

namespace baudline {

static float freqToLogFrac(double freq, double minFreq, double maxFreq) {
    if (freq <= 0 || minFreq <= 0) return 0.0f;
    double logMin = std::log10(minFreq);
    double logMax = std::log10(maxFreq);
    double logF   = std::log10(freq);
    return static_cast<float>((logF - logMin) / (logMax - logMin));
}

// Build a decimated polyline for one spectrum.
static void buildPolyline(const std::vector<float>& spectrumDB,
                           float minDB, float maxDB,
                           double freqMin, double freqMax,
                           bool isIQ, FreqScale freqScale,
                           float posX, float posY, float sizeX, float sizeY,
                           std::vector<ImVec2>& outPoints) {
    int bins = static_cast<int>(spectrumDB.size());
    int displayPts = std::min(bins, static_cast<int>(sizeX));
    if (displayPts < 2) displayPts = 2;

    outPoints.resize(displayPts);
    for (int idx = 0; idx < displayPts; ++idx) {
        float frac = static_cast<float>(idx) / (displayPts - 1);
        float xFrac;

        if (freqScale == FreqScale::Logarithmic && !isIQ) {
            double freq = frac * (freqMax - freqMin) + freqMin;
            double logMin = std::max(freqMin, 1.0);
            xFrac = freqToLogFrac(freq, logMin, freqMax);
        } else {
            xFrac = frac;
        }

        // Bucket range for peak-hold decimation.
        float binF    = frac * (bins - 1);
        float binPrev = (idx > 0)
            ? static_cast<float>(idx - 1) / (displayPts - 1) * (bins - 1)
            : binF;
        float binNext = (idx < displayPts - 1)
            ? static_cast<float>(idx + 1) / (displayPts - 1) * (bins - 1)
            : binF;
        int b0 = static_cast<int>((binPrev + binF) * 0.5f);
        int b1 = static_cast<int>((binF + binNext) * 0.5f);
        b0 = std::clamp(b0, 0, bins - 1);
        b1 = std::clamp(b1, b0, bins - 1);

        float peakDB = spectrumDB[b0];
        for (int b = b0 + 1; b <= b1; ++b)
            peakDB = std::max(peakDB, spectrumDB[b]);

        float x = posX + xFrac * sizeX;
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
                           float sizeX, float sizeY) const {
    if (spectra.empty() || spectra[0].empty() || sizeX <= 0 || sizeY <= 0) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    double freqMin = isIQ ? -sampleRate / 2.0 : 0.0;
    double freqMax = isIQ ?  sampleRate / 2.0 : sampleRate / 2.0;

    // Background
    dl->AddRectFilled({posX, posY}, {posX + sizeX, posY + sizeY},
                      IM_COL32(20, 20, 30, 255));

    // Grid lines
    if (showGrid) {
        ImU32 gridCol = IM_COL32(60, 60, 80, 128);
        ImU32 textCol = IM_COL32(180, 180, 200, 200);

        float dbStep = 10.0f;
        for (float db = std::ceil(minDB / dbStep) * dbStep; db <= maxDB; db += dbStep) {
            float y = posY + sizeY * (1.0f - (db - minDB) / (maxDB - minDB));
            dl->AddLine({posX, y}, {posX + sizeX, y}, gridCol);
            char label[32];
            std::snprintf(label, sizeof(label), "%.0f dB", db);
            dl->AddText({posX + 2, y - 12}, textCol, label);
        }

        int numVLines = 8;
        for (int i = 0; i <= numVLines; ++i) {
            float frac = static_cast<float>(i) / numVLines;
            double freq;
            float screenFrac;

            if (freqScale == FreqScale::Linear) {
                freq = freqMin + frac * (freqMax - freqMin);
                screenFrac = frac;
            } else {
                double logMinF = std::max(freqMin, 1.0);
                double logMaxF = freqMax;
                freq = std::pow(10.0, std::log10(logMinF) +
                       frac * (std::log10(logMaxF) - std::log10(logMinF)));
                screenFrac = frac;
            }

            float x = posX + screenFrac * sizeX;
            dl->AddLine({x, posY}, {x, posY + sizeY}, gridCol);

            char label[32];
            if (std::abs(freq) >= 1e6)
                std::snprintf(label, sizeof(label), "%.2f MHz", freq / 1e6);
            else if (std::abs(freq) >= 1e3)
                std::snprintf(label, sizeof(label), "%.1f kHz", freq / 1e3);
            else
                std::snprintf(label, sizeof(label), "%.0f Hz", freq);
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

        buildPolyline(spectra[ch], minDB, maxDB, freqMin, freqMax,
                      isIQ, freqScale, posX, posY, sizeX, sizeY, points);

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
                                       FreqScale freqScale) const {
    float frac = (screenX - posX) / sizeX;
    frac = std::clamp(frac, 0.0f, 1.0f);

    double freqMin = isIQ ? -sampleRate / 2.0 : 0.0;
    double freqMax = isIQ ?  sampleRate / 2.0 : sampleRate / 2.0;

    if (freqScale == FreqScale::Logarithmic && !isIQ) {
        double logMin = std::log10(std::max(freqMin, 1.0));
        double logMax = std::log10(freqMax);
        return std::pow(10.0, logMin + frac * (logMax - logMin));
    }
    return freqMin + frac * (freqMax - freqMin);
}

float SpectrumDisplay::freqToScreenX(double freq, float posX, float sizeX,
                                      double sampleRate, bool isIQ,
                                      FreqScale freqScale) const {
    double freqMin = isIQ ? -sampleRate / 2.0 : 0.0;
    double freqMax = isIQ ?  sampleRate / 2.0 : sampleRate / 2.0;

    float frac;
    if (freqScale == FreqScale::Logarithmic && !isIQ) {
        frac = freqToLogFrac(freq, std::max(freqMin, 1.0), freqMax);
    } else {
        frac = static_cast<float>((freq - freqMin) / (freqMax - freqMin));
    }
    return posX + frac * sizeX;
}

float SpectrumDisplay::screenYToDB(float screenY, float posY, float sizeY,
                                    float minDB, float maxDB) const {
    float frac = 1.0f - (screenY - posY) / sizeY;
    frac = std::clamp(frac, 0.0f, 1.0f);
    return minDB + frac * (maxDB - minDB);
}

} // namespace baudline
