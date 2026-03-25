#include "ui/Measurements.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace baudline {

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
                                  float viewLo, float viewHi) const {
    if (!enabled || !showOnWaterfall || peaks_.empty()) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();

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

void Measurements::drawPanel() {
    if (!enabled) {
        ImGui::TextDisabled("Disabled");
        return;
    }

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
            if (std::abs(p.freq) >= 1e6)
                ImGui::Text("P%d: %.6f MHz, %.1f dB", i + 1, p.freq / 1e6, p.dB);
            else if (std::abs(p.freq) >= 1e3)
                ImGui::Text("P%d: %.3f kHz, %.1f dB", i + 1, p.freq / 1e3, p.dB);
            else
                ImGui::Text("P%d: %.1f Hz, %.1f dB", i + 1, p.freq, p.dB);
            ImGui::PopStyleColor();
        }
    }
}

} // namespace baudline
