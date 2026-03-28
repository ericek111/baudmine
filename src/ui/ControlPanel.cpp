#include "ui/ControlPanel.h"
#include "audio/AudioEngine.h"
#include "ui/SpectrumDisplay.h"
#include "ui/Cursors.h"
#include "ui/Measurements.h"
#include "ui/ColorMap.h"
#include "ui/WaterfallDisplay.h"

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace baudmine {

void ControlPanel::render(AudioEngine& audio, UIState& ui,
                           SpectrumDisplay& specDisplay, Cursors& cursors,
                           Measurements& measurements, ColorMap& colorMap,
                           WaterfallDisplay& waterfall) {
    const auto& settings = audio.settings();

    // ── Playback ──
    float btnW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;
    if (ImGui::Button(ui.paused ? "Resume" : "Pause", {btnW, 0}))
        ui.paused = !ui.paused;
    ImGui::SameLine();
    if (ImGui::Button("Clear", {btnW, 0})) {
        audio.clearHistory();
    }
    ImGui::SameLine();
    if (ImGui::Button("Peak", {btnW, 0})) {
        int pkCh = std::clamp(ui.waterfallChannel, 0, audio.totalNumSpectra() - 1);
        cursors.snapToPeak(audio.getSpectrum(pkCh),
                           settings.sampleRate, settings.isIQ,
                           settings.fftSize);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Snap cursor A to peak");

    // ── FFT ──
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("FFT", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* sizeNames[] = {"256", "512", "1024", "2048", "4096",
                                   "8192", "16384", "32768", "65536"};
        float availSpace = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x);
        ImGui::SetNextItemWidth(availSpace * 0.35f);
        if (ImGui::Combo("##fftsize", &fftSizeIdx, sizeNames, kNumFFTSizes)) {
            audio.settings().fftSize = kFFTSizes[fftSizeIdx];
            flagUpdate();
            flagSave();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("FFT Size");

        ImGui::SameLine();
        const char* winNames[] = {"Rectangular", "Hann", "Hamming", "Blackman",
                                  "Blackman-Harris", "Kaiser", "Flat Top"};
        ImGui::SetNextItemWidth(availSpace * 0.65f);
        if (ImGui::Combo("##window", &windowIdx, winNames,
                         static_cast<int>(WindowType::Count))) {
            audio.settings().window = static_cast<WindowType>(windowIdx);
            flagUpdate();
            flagSave();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Window Function");

        if (settings.window == WindowType::Kaiser) {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##kaiser", &audio.settings().kaiserBeta, 0.0f, 20.0f, "Kaiser: %.1f"))
                flagUpdate();
        }

        // Overlap
        {
            int hopSamples = static_cast<int>(settings.fftSize * (1.0f - settings.overlap));
            if (hopSamples < 1) hopSamples = 1;
            int overlapSamples = settings.fftSize - hopSamples;

            ImGui::SetNextItemWidth(-1);
            float sliderVal = 1.0f - std::pow(1.0f - overlapPct / 99.0f, 0.25f);
            if (ImGui::SliderFloat("##overlap", &sliderVal, 0.0f, 1.0f, "")) {
                float inv = 1.0f - sliderVal;
                float inv2 = inv * inv;
                overlapPct = 99.0f * (1.0f - inv2 * inv2);
                audio.settings().overlap = overlapPct / 100.0f;
                flagUpdate();
                flagSave();
            }

            char overlayText[64];
            std::snprintf(overlayText, sizeof(overlayText), "%.1f%% (%d samples)", overlapPct, overlapSamples);
            ImVec2 textSize = ImGui::CalcTextSize(overlayText);
            ImVec2 rMin = ImGui::GetItemRectMin();
            ImVec2 rMax = ImGui::GetItemRectMax();
            float tx = rMin.x + ((rMax.x - rMin.x) - textSize.x) * 0.5f;
            float ty = rMin.y + ((rMax.y - rMin.y) - textSize.y) * 0.5f;
            ImGui::GetWindowDrawList()->AddText({tx, ty}, IM_COL32(255, 255, 255, 220), overlayText);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Overlap");
        }
    }

    // ── Display ──
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloatRange2("##dbrange", &ui.minDB, &ui.maxDB, 1.0f, -200.0f, 20.0f,
                               "Min: %.0f dB", "Max: %.0f dB");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("dB Range (min / max)");

        ImGui::Checkbox("Peak Hold", &specDisplay.peakHoldEnable);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Draws a \"maximum\" line in the spectrogram");
        if (specDisplay.peakHoldEnable) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x
                                    - ImGui::CalcTextSize("Clear").x
                                    - ImGui::GetStyle().ItemSpacing.x
                                    - ImGui::GetStyle().FramePadding.x * 2);
            ImGui::SliderFloat("##decay", &specDisplay.peakHoldDecay, 0.0f, 120.0f, "%.0f dB/s");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decay rate");
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear##peakhold"))
                specDisplay.clearPeakHold();
        }

        {
            bool isLog = (ui.freqScale == FreqScale::Logarithmic);
            bool canLog = !settings.isIQ;
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Freq. scale:");
            ImGui::SameLine();
            if (ImGui::Button(isLog ? "Logarithmic" : "Linear", {ImGui::GetContentRegionAvail().x, 0})) {
                if (canLog) {
                    constexpr float kMinBF = 0.001f;
                    float logMin = std::log10(kMinBF);
                    auto screenToBin = [&](float sf) -> float {
                        if (isLog) return std::pow(10.0f, logMin + sf * (0.0f - logMin));
                        return sf;
                    };
                    auto binToScreen = [&](float bf, bool toLog) -> float {
                        if (toLog) {
                            if (bf < kMinBF) bf = kMinBF;
                            return (std::log10(bf) - logMin) / (0.0f - logMin);
                        }
                        return bf;
                    };
                    float bfLo = screenToBin(ui.viewLo);
                    float bfHi = screenToBin(ui.viewHi);
                    bool newLog = !isLog;
                    ui.freqScale = newLog ? FreqScale::Logarithmic : FreqScale::Linear;
                    ui.viewLo = std::clamp(binToScreen(bfLo, newLog), 0.0f, 1.0f);
                    ui.viewHi = std::clamp(binToScreen(bfHi, newLog), 0.0f, 1.0f);
                    if (ui.viewHi <= ui.viewLo) { ui.viewLo = 0.0f; ui.viewHi = 1.0f; }
                    flagSave();
                }
            }
            if (!canLog && ImGui::IsItemHovered())
                ImGui::SetTooltip("Log scale not available in I/Q mode");
        }

        {
            float span = ui.viewHi - ui.viewLo;
            float zoomX = 1.0f / span;
            float resetBtnW = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2;
            float zoomLabelW = ImGui::CalcTextSize("Zoom:").x + ImGui::GetStyle().ItemSpacing.x;
            float sliderW = ImGui::GetContentRegionAvail().x - zoomLabelW - resetBtnW - ImGui::GetStyle().ItemSpacing.x;
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Zoom:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(sliderW);
            if (ImGui::SliderFloat("##zoom", &zoomX, 1.0f, 200.0f, "%.1fx", ImGuiSliderFlags_Logarithmic)) {
                zoomX = std::clamp(zoomX, 1.0f, 1000.0f);
                float newSpan = 1.0f / zoomX;
                ui.viewLo = 0.0f;
                ui.viewHi = std::clamp(newSpan, 0.0f, 1.0f);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##zoom")) {
                ui.viewLo = 0.0f;
                ui.viewHi = 0.5f;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset to 2x zoom");
        }
    }

    // ── Channels ──
    ImGui::Spacing();
    {
        int nCh = audio.totalNumSpectra();
        bool isMulti = ui.waterfallMultiCh && nCh > 1;

        float widgetW = (nCh > 1) ? ImGui::CalcTextSize(" Multi ").x + ImGui::GetStyle().FramePadding.x * 2 : 0.0f;
        float gap = ImGui::GetStyle().ItemSpacing.x * 0.25f;
        ImVec2 hdrMin = ImGui::GetCursorScreenPos();
        float winLeft = ImGui::GetWindowPos().x;
        float hdrRight = hdrMin.x + ImGui::GetContentRegionAvail().x;
        ImGui::PushClipRect({winLeft, hdrMin.y}, {hdrRight - widgetW - gap, hdrMin.y + 200}, true);
        bool headerOpen = ImGui::CollapsingHeader("##channels_hdr",
                                                   ImGuiTreeNodeFlags_DefaultOpen |
                                                   ImGuiTreeNodeFlags_AllowOverlap);
        ImGui::PopClipRect();
        ImGui::SameLine();
        ImGui::Text("Channels");
        if (nCh > 1) {
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - widgetW + ImGui::GetStyle().FramePadding.x);
            if (ImGui::Button(isMulti ? " Multi " : "Single ", {widgetW, 0})) {
                ui.waterfallMultiCh = !ui.waterfallMultiCh;
            }
        }

        if (headerOpen) {
            if (isMulti) {
                static const char* defaultNames[] = {
                    "Left", "Right", "Ch 3", "Ch 4", "Ch 5", "Ch 6", "Ch 7", "Ch 8"
                };
                for (int ch = 0; ch < nCh && ch < kMaxChannels; ++ch) {
                    ImGui::PushID(ch);
                    ImGui::Checkbox("##en", &ui.channelEnabled[ch]);
                    ImGui::SameLine();
                    ImGui::ColorEdit3(defaultNames[ch], &ui.channelColors[ch].x,
                                      ImGuiColorEditFlags_NoInputs);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", audio.getDeviceName(ch));
                    ImGui::PopID();
                }
            } else {
                const char* cmNames[] = {"Magma", "Viridis", "Inferno", "Plasma", "Grayscale"};
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##colormap", &colorMapIdx, cmNames,
                                 static_cast<int>(ColorMapType::Count))) {
                    colorMap.setType(static_cast<ColorMapType>(colorMapIdx));
                    waterfall.setColorMap(colorMap);
                    flagSave();
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Color Map");

                if (nCh > 1) {
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::SliderInt("##wfch", &ui.waterfallChannel, 0, nCh - 1))
                        ui.waterfallChannel = std::clamp(ui.waterfallChannel, 0, nCh - 1);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Waterfall Channel");
                }
            }
        }
    }

    // ── Math ──
    ImGui::Spacing();
    {
        float btnW2 = ImGui::GetFrameHeight();
        float gap = ImGui::GetStyle().ItemSpacing.x * 0.25f;
        ImVec2 hdrMin = ImGui::GetCursorScreenPos();
        float winLeft = ImGui::GetWindowPos().x;
        float hdrRight = hdrMin.x + ImGui::GetContentRegionAvail().x;
        ImGui::PushClipRect({winLeft, hdrMin.y}, {hdrRight - btnW2 - gap, hdrMin.y + 200}, true);
        bool mathOpen = ImGui::CollapsingHeader("##math_hdr",
                                                 ImGuiTreeNodeFlags_DefaultOpen |
                                                 ImGuiTreeNodeFlags_AllowOverlap);
        ImGui::PopClipRect();
        ImGui::SameLine();
        ImGui::Text("Math");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - btnW2 + ImGui::GetStyle().FramePadding.x);
        if (ImGui::Button("+##addmath", {btnW2, 0})) {
            int nPhys = audio.totalNumSpectra();
            MathChannel mc;
            mc.op = MathOp::Subtract;
            mc.sourceX = 0;
            mc.sourceY = std::min(1, nPhys - 1);
            mc.color[0] = 1.0f; mc.color[1] = 1.0f; mc.color[2] = 0.5f; mc.color[3] = 1.0f;
            audio.mathChannels().push_back(mc);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add math channel");

        if (mathOpen) {
            renderMathPanel(audio);
        }
    }

    // ── Cursors ──
    ImGui::Spacing();
    {
        float btnW2 = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2;
        float gap = ImGui::GetStyle().ItemSpacing.x * 0.25f;
        ImVec2 hdrMin = ImGui::GetCursorScreenPos();
        float winLeft = ImGui::GetWindowPos().x;
        float hdrRight = hdrMin.x + ImGui::GetContentRegionAvail().x;
        ImGui::PushClipRect({winLeft, hdrMin.y}, {hdrRight - btnW2 - gap, hdrMin.y + 200}, true);
        bool cursorsOpen = ImGui::CollapsingHeader("##cursors_hdr",
                                                    ImGuiTreeNodeFlags_DefaultOpen |
                                                    ImGuiTreeNodeFlags_AllowOverlap);
        ImGui::PopClipRect();
        ImGui::SameLine();
        ImGui::Text("Cursors");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - btnW2 + ImGui::GetStyle().FramePadding.x);
        if (ImGui::SmallButton("Reset##cursors")) {
            cursors.cursorA.active = false;
            cursors.cursorB.active = false;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear cursors A and B");

        if (cursorsOpen) {
            bool prevSnap = cursors.snapToPeaks;
            cursors.drawPanel();
            if (cursors.snapToPeaks != prevSnap) flagSave();
        }
    }

    // ── Measurements ──
    ImGui::Spacing();
    {
        float cbW = ImGui::GetFrameHeight();
        float gap = ImGui::GetStyle().ItemSpacing.x * 0.25f;
        ImVec2 hdrMin = ImGui::GetCursorScreenPos();
        float winLeft = ImGui::GetWindowPos().x;
        float hdrRight = hdrMin.x + ImGui::GetContentRegionAvail().x;
        ImGui::PushClipRect({winLeft, hdrMin.y}, {hdrRight - cbW - gap, hdrMin.y + 200}, true);
        bool headerOpen = ImGui::CollapsingHeader("##meas_hdr",
                                                   ImGuiTreeNodeFlags_DefaultOpen |
                                                   ImGuiTreeNodeFlags_AllowOverlap);
        ImGui::PopClipRect();
        ImGui::SameLine();
        ImGui::Text("Measurements");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - cbW + ImGui::GetStyle().FramePadding.x);
        ImGui::Checkbox("##meas_en", &measurements.enabled);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable measurements");

        if (headerOpen) {
            float prevMin = measurements.traceMinFreq;
            float prevMax = measurements.traceMaxFreq;
            measurements.drawPanel();
            if (measurements.traceMinFreq != prevMin || measurements.traceMaxFreq != prevMax)
                flagSave();
        }
    }

    // ── Status (bottom) ──
    ImGui::Separator();
    ImGui::TextDisabled("Mode: %s", settings.isIQ ? "I/Q"
                        : (settings.numChannels > 1 ? "Multi-ch" : "Real"));
}

void ControlPanel::renderMathPanel(AudioEngine& audio) {
    int nPhys = audio.totalNumSpectra();
    auto& mathChannels = audio.mathChannels();

    static const char* chNames[] = {
        "Ch 0 (L)", "Ch 1 (R)", "Ch 2", "Ch 3", "Ch 4", "Ch 5", "Ch 6", "Ch 7"
    };

    int toRemove = -1;
    for (int mi = 0; mi < static_cast<int>(mathChannels.size()); ++mi) {
        auto& mc = mathChannels[mi];
        ImGui::PushID(1000 + mi);

        ImGui::Checkbox("##en", &mc.enabled);
        ImGui::SameLine();
        ImGui::ColorEdit3("##col", mc.color, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();

        if (ImGui::BeginCombo("##op", mathOpName(mc.op), ImGuiComboFlags_NoPreview)) {
            for (int o = 0; o < static_cast<int>(MathOp::Count); ++o) {
                auto op = static_cast<MathOp>(o);
                if (ImGui::Selectable(mathOpName(op), mc.op == op))
                    mc.op = op;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::Text("%s", mathOpName(mc.op));

        ImGui::SetNextItemWidth(80);
        ImGui::Combo("X", &mc.sourceX, chNames, std::min(nPhys, kMaxChannels));

        if (mathOpIsBinary(mc.op)) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::Combo("Y", &mc.sourceY, chNames, std::min(nPhys, kMaxChannels));
        }

        ImGui::SameLine();
        ImGui::Checkbox("WF", &mc.waterfall);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Show on waterfall");
        ImGui::SameLine();
        if (ImGui::SmallButton("X##del"))
            toRemove = mi;

        ImGui::PopID();
    }

    if (toRemove >= 0)
        mathChannels.erase(mathChannels.begin() + toRemove);
}

} // namespace baudmine
