#include "ui/Measurements.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace baudmine {

double Measurements::binToFreq(int bin, double sampleRate, bool isIQ, int fftSize) {
    // Use bin center (+0.5) for more accurate frequency estimation.
    double b = static_cast<double>(bin) + 0.5;
    if (isIQ) {
        return -sampleRate / 2.0 + (b / fftSize) * sampleRate;
    } else {
        return (b / fftSize) * sampleRate;
    }
}

void Measurements::findPeaks(const std::vector<float>& spectrumDB, int maxN,
                              int minDist, float threshold) {
    peaks_.clear();
    int bins = static_cast<int>(spectrumDB.size());
    if (bins < 3) return;

    // Mark bins that are local maxima (higher than both neighbors).
    // Collect all candidates, then pick top-N with minimum separation.
    struct Candidate { int bin; float dB; };
    std::vector<Candidate> candidates;
    candidates.reserve(bins / 2);

    for (int i = 1; i < bins - 1; ++i) {
        if (spectrumDB[i] >= spectrumDB[i - 1] &&
            spectrumDB[i] >= spectrumDB[i + 1] &&
            spectrumDB[i] >= threshold) {
            candidates.push_back({i, spectrumDB[i]});
        }
    }
    // Also check endpoints.
    if (bins > 0 && spectrumDB[0] >= spectrumDB[1] && spectrumDB[0] >= threshold)
        candidates.push_back({0, spectrumDB[0]});
    if (bins > 1 && spectrumDB[bins - 1] >= spectrumDB[bins - 2] && spectrumDB[bins - 1] >= threshold)
        candidates.push_back({bins - 1, spectrumDB[bins - 1]});

    // Sort by amplitude descending.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.dB > b.dB; });

    // Greedy selection with minimum distance constraint.
    for (const auto& c : candidates) {
        if (static_cast<int>(peaks_.size()) >= maxN) break;
        bool tooClose = false;
        for (const auto& p : peaks_) {
            if (std::abs(c.bin - p.bin) < minDist) {
                tooClose = true;
                break;
            }
        }
        if (!tooClose) {
            peaks_.push_back({c.bin, 0.0, c.dB});  // freq filled below
        }
    }
}

void Measurements::update(const std::vector<float>& spectrumDB,
                           double sampleRate, bool isIQ, int fftSize) {
    lastSampleRate_ = sampleRate;
    // Always track global peak (for the readout label).
    if (!spectrumDB.empty()) {
        auto it = std::max_element(spectrumDB.begin(), spectrumDB.end());
        int bin = static_cast<int>(std::distance(spectrumDB.begin(), it));
        globalPeak_.bin = bin;
        globalPeak_.dB = *it;
        globalPeak_.freq = binToFreq(bin, sampleRate, isIQ, fftSize);

        // Push into peak history circular buffer (with optional freq range filter)
        constexpr int kMaxHistory = 4096;
        if (static_cast<int>(peakHistBins_.size()) < kMaxHistory)
            peakHistBins_.resize(kMaxHistory, -1);

        // Find peak within the trace frequency range
        int traceBin = bin;
        int bins = static_cast<int>(spectrumDB.size());
        if (traceMinFreq > 0.0f || traceMaxFreq > 0.0f) {
            double fMin = isIQ ? -sampleRate / 2.0 : 0.0;
            double fMax = isIQ ?  sampleRate / 2.0 : sampleRate / 2.0;
            int loB = 0, hiB = bins - 1;
            if (traceMinFreq > 0.0f)
                loB = std::max(0, static_cast<int>((traceMinFreq - fMin) / (fMax - fMin) * bins));
            if (traceMaxFreq > 0.0f)
                hiB = std::min(bins - 1, static_cast<int>((traceMaxFreq - fMin) / (fMax - fMin) * bins));
            if (loB <= hiB) {
                auto rangeIt = std::max_element(spectrumDB.begin() + loB, spectrumDB.begin() + hiB + 1);
                traceBin = static_cast<int>(std::distance(spectrumDB.begin(), rangeIt));
            }
        }

        peakHistIdx_ = (peakHistIdx_ + 1) % kMaxHistory;
        peakHistBins_[peakHistIdx_] = traceBin;
        if (peakHistLen_ < kMaxHistory) ++peakHistLen_;
    }

    if (!enabled) { peaks_.clear(); return; }

    findPeaks(spectrumDB, maxPeaks, minPeakDist, peakThreshold);

    // Fill in frequencies.
    for (auto& p : peaks_) {
        p.freq = binToFreq(p.bin, sampleRate, isIQ, fftSize);
    }
}

void Measurements::draw(const SpectrumDisplay& specDisplay,
                         float posX, float posY, float sizeX, float sizeY,
                         double sampleRate, bool isIQ, FreqScale freqScale,
                         float minDB, float maxDB,
                         float viewLo, float viewHi) const {
    // Global peak readout (always shown, left of cursor delta)
    {
        char pkBuf[128];
        fmtFreqDB(pkBuf, sizeof(pkBuf), "Peak", globalPeak_.freq, globalPeak_.dB);
        ImVec2 pkSz = ImGui::CalcTextSize(pkBuf);
        // Reserve space for delta + hover labels to the right.
        float reserveW = ImGui::CalcTextSize("D:  00.000 kHz  000.0 dB   00.000 kHz  000.0 dB").x;
        float pkX = posX + sizeX - pkSz.x - 8 - reserveW;
        float pkY = posY + 4;
        ImGui::GetWindowDrawList()->AddText({pkX, pkY}, IM_COL32(180, 180, 200, 200), pkBuf);
    }

    if (!enabled || !showOnSpectrum || peaks_.empty()) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Colors: primary peak is bright, subsequent peaks fade.
    auto peakColor = [](int idx) -> ImU32 {
        if (idx == 0) return IM_COL32(255, 80, 80, 220);   // red — primary
        return IM_COL32(255, 140, 60, 180);                  // orange — secondary
    };

    for (int i = 0; i < static_cast<int>(peaks_.size()); ++i) {
        const auto& p = peaks_[i];
        ImU32 col = peakColor(i);

        float x = specDisplay.freqToScreenX(p.freq, posX, sizeX,
                                             sampleRate, isIQ, freqScale,
                                             viewLo, viewHi);
        float dbNorm = (p.dB - minDB) / (maxDB - minDB);
        dbNorm = std::clamp(dbNorm, 0.0f, 1.0f);
        float y = posY + sizeY * (1.0f - dbNorm);

        // Small downward triangle above the peak.
        float triSize = (i == 0) ? 6.0f : 4.0f;
        dl->AddTriangleFilled(
            {x - triSize, y - triSize * 2},
            {x + triSize, y - triSize * 2},
            {x, y},
            col);

        // Label: "P1: freq  dB" — only for the first few to avoid clutter.
        if (i < 3) {
            char buf[80];
            if (std::abs(p.freq) >= 1e6)
                std::snprintf(buf, sizeof(buf), "P%d: %.6f MHz  %.1f dB", i + 1, p.freq / 1e6, p.dB);
            else if (std::abs(p.freq) >= 1e3)
                std::snprintf(buf, sizeof(buf), "P%d: %.3f kHz  %.1f dB", i + 1, p.freq / 1e3, p.dB);
            else
                std::snprintf(buf, sizeof(buf), "P%d: %.1f Hz  %.1f dB", i + 1, p.freq, p.dB);

            ImVec2 sz = ImGui::CalcTextSize(buf);
            float tx = x - sz.x * 0.5f;
            float ty = y - triSize * 2 - sz.y - 2;

            // Clamp to display bounds.
            tx = std::clamp(tx, posX + 2, posX + sizeX - sz.x - 2);
            ty = std::max(ty, posY + 2);

            dl->AddRectFilled({tx - 2, ty - 1}, {tx + sz.x + 2, ty + sz.y + 1},
                              IM_COL32(0, 0, 0, 160));
            dl->AddText({tx, ty}, col, buf);
        }
    }
}

void Measurements::drawWaterfall(const SpectrumDisplay& specDisplay,
                                  float posX, float posY, float sizeX, float sizeY,
                                  double sampleRate, bool isIQ, FreqScale freqScale,
                                  float viewLo, float viewHi,
                                  int screenRows, int spectrumSize) const {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Peak trace: red squiggly line showing peak frequency history
    if (showPeakTrace && peakHistLen_ > 1 && screenRows > 1 && spectrumSize > 0) {
        int histSize = static_cast<int>(peakHistBins_.size());
        int count = std::min(screenRows, peakHistLen_);
        ImU32 traceCol = IM_COL32(255, 30, 30, 200);

        // Convert bin index to screen X (respects log scale)
        double fMin = isIQ ? -sampleRate / 2.0 : 0.0;
        double fMax = isIQ ?  sampleRate / 2.0 : sampleRate / 2.0;
        auto binToX = [&](int bin) -> float {
            double freq = fMin + (static_cast<double>(bin) + 0.5) / spectrumSize * (fMax - fMin);
            return specDisplay.freqToScreenX(freq, posX, sizeX,
                                              sampleRate, isIQ, freqScale,
                                              viewLo, viewHi);
        };

        // Walk from newest (bottom) to oldest (top)
        float prevX = 0, prevY = 0;
        bool havePrev = false;
        for (int i = 0; i < count; ++i) {
            int idx = (peakHistIdx_ - i + histSize) % histSize;
            int bin = peakHistBins_[idx];
            if (bin < 0) break;

            // Y: i=0 is newest (bottom), i=count-1 is oldest (top)
            float y = posY + sizeY - (static_cast<float>(i) + 0.5f) / screenRows * sizeY;
            float x = binToX(bin);

            if (havePrev) {
                dl->AddLine({prevX, prevY}, {x, y}, traceCol, 3.0f);
            }
            prevX = x;
            prevY = y;
            havePrev = true;
        }
    }

    // Vertical markers at current peak positions
    if (enabled && showOnWaterfall && !peaks_.empty()) {
        auto peakColor = [](int idx) -> ImU32 {
            if (idx == 0) return IM_COL32(255, 80, 80, 120);
            return IM_COL32(255, 140, 60, 80);
        };

        for (int i = 0; i < static_cast<int>(peaks_.size()); ++i) {
            const auto& p = peaks_[i];
            float x = specDisplay.freqToScreenX(p.freq, posX, sizeX,
                                                 sampleRate, isIQ, freqScale,
                                                 viewLo, viewHi);
            ImU32 col = peakColor(i);
            float thickness = (i == 0) ? 1.5f : 1.0f;
            dl->AddLine({x, posY}, {x, posY + sizeY}, col, thickness);
        }
    }
}

void Measurements::drawPanel() {
    ImGui::Checkbox("Peak trace", &showPeakTrace);
    if (showPeakTrace) {
        ImGui::SameLine();
        float nyquistKHz = static_cast<float>(lastSampleRate_ / 2000.0);
        float minKHz = traceMinFreq / 1000.0f;
        float maxKHz = traceMaxFreq / 1000.0f;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::DragFloatRange2("##tracerange", &minKHz, &maxKHz, 0.01f,
                                    0.0f, nyquistKHz,
                                    minKHz > 0.0f ? "%.2f kHz" : "Min",
                                    maxKHz > 0.0f ? "%.2f kHz" : "Max")) {
            traceMinFreq = std::max(0.0f, minKHz * 1000.0f);
            traceMaxFreq = std::max(0.0f, maxKHz * 1000.0f);
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            traceMinFreq = 0.0f;
            traceMaxFreq = 0.0f;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Peak trace frequency range (right-click to reset)");
    }
    if (!enabled) return;

    ImGui::SetNextItemWidth(-1);
    ImGui::SliderInt("##maxpeaks", &maxPeaks, 1, 20, "Peaks: %d");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Number of peaks to detect");

    ImGui::SetNextItemWidth(-1);
    ImGui::SliderInt("##mindist", &minPeakDist, 1, 200, "Min dist: %d bins",
                     ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Minimum distance between peaks (bins)");

    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##threshold", &peakThreshold, -200.0f, 0.0f, "Thresh: %.0f dB");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ignore peaks below this level");

    ImGui::Text("Markers:");
    ImGui::SameLine();
    ImGui::Checkbox("Spectrum##mkr", &showOnSpectrum);
    ImGui::SameLine();
    ImGui::Checkbox("Waterfall##mkr", &showOnWaterfall);

    // Peak readout table.
    if (!peaks_.empty()) {
        ImGui::Separator();
        for (int i = 0; i < static_cast<int>(peaks_.size()); ++i) {
            const auto& p = peaks_[i];
            ImU32 col = (i == 0) ? IM_COL32(255, 80, 80, 255)
                                 : IM_COL32(255, 140, 60, 255);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            char plabel[4];
            std::snprintf(plabel, sizeof(plabel), "P%d", i + 1);
            char pbuf[128];
            fmtFreqDB(pbuf, sizeof(pbuf), plabel, p.freq, p.dB);
            ImGui::Text("%s", pbuf);
            ImGui::PopStyleColor();
        }
    }
}

} // namespace baudmine
