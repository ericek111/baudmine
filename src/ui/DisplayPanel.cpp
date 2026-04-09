#include "ui/DisplayPanel.h"
#include "audio/AudioEngine.h"

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace baudmine {

namespace {

// Zoom the view range centered on a screen-fraction cursor position.
void zoomView(float& viewLo, float& viewHi, float cursorScreenFrac, float wheelDir) {
    float viewFrac = viewLo + cursorScreenFrac * (viewHi - viewLo);
    float factor = (wheelDir > 0) ? kZoomFactor : 1.0f / kZoomFactor;
    float newSpan = std::clamp((viewHi - viewLo) * factor, 0.001f, 1.0f);

    float newLo = viewFrac - cursorScreenFrac * newSpan;
    float newHi = newLo + newSpan;

    if (newLo < 0.0f) { newHi -= newLo; newLo = 0.0f; }
    if (newHi > 1.0f) { newLo -= (newHi - 1.0f); newHi = 1.0f; }
    viewLo = std::clamp(newLo, 0.0f, 1.0f);
    viewHi = std::clamp(newHi, 0.0f, 1.0f);
}

// Pan the view range by a screen-pixel delta.
void panView(float& viewLo, float& viewHi, float dxPixels, float panelWidth) {
    float panFrac = -dxPixels / panelWidth * (viewHi - viewLo);
    float span = viewHi - viewLo;
    float newLo = viewLo + panFrac;
    float newHi = viewHi + panFrac;
    if (newLo < 0.0f) { newLo = 0.0f; newHi = span; }
    if (newHi > 1.0f) { newHi = 1.0f; newLo = 1.0f - span; }
    viewLo = newLo;
    viewHi = newHi;
}

} // anonymous namespace

void DisplayPanel::renderSpectrum(AudioEngine& audio, UIState& ui,
                                   SpectrumDisplay& specDisplay, Cursors& cursors,
                                   Measurements& measurements) {
    const auto& settings = audio.settings();

    float availW = ImGui::GetContentRegionAvail().x;
    float specH = ImGui::GetContentRegionAvail().y;

    ImVec2 pos = ImGui::GetCursorScreenPos();
    specPosX = pos.x;
    specPosY = pos.y;
    specSizeX = availW;
    specSizeY = specH;

    int nPhys = audio.totalNumSpectra();
    const auto& mathChannels = audio.mathChannels();
    const auto& mathSpectra  = audio.mathSpectra();
    int nMath = static_cast<int>(mathSpectra.size());

    allSpectraScratch_.clear();
    stylesScratch_.clear();

    for (int ch = 0; ch < nPhys; ++ch) {
        if (!ui.channelEnabled[ch % kMaxChannels]) continue;
        allSpectraScratch_.push_back(audio.getSpectrum(ch));
        const auto& c = ui.channelColors[ch % kMaxChannels];
        uint8_t r = static_cast<uint8_t>(c.x * 255);
        uint8_t g = static_cast<uint8_t>(c.y * 255);
        uint8_t b = static_cast<uint8_t>(c.z * 255);
        stylesScratch_.push_back({IM_COL32(r, g, b, 220), IM_COL32(r, g, b, 35)});
    }

    for (int mi = 0; mi < nMath; ++mi) {
        if (mi < static_cast<int>(mathChannels.size()) && mathChannels[mi].enabled) {
            allSpectraScratch_.push_back(mathSpectra[mi]);
            const auto& c = mathChannels[mi].color;
            uint8_t r = static_cast<uint8_t>(c[0] * 255);
            uint8_t g = static_cast<uint8_t>(c[1] * 255);
            uint8_t b = static_cast<uint8_t>(c[2] * 255);
            stylesScratch_.push_back({IM_COL32(r, g, b, 220), IM_COL32(r, g, b, 35)});
        }
    }

    specDisplay.updatePeakHold(allSpectraScratch_);
    specDisplay.draw(allSpectraScratch_, stylesScratch_, ui.minDB, ui.maxDB,
                     settings.sampleRate, settings.isIQ, ui.freqScale,
                     specPosX, specPosY, specSizeX, specSizeY,
                     ui.viewLo, ui.viewHi, ui.specMinPixPerBin);

    cursors.draw(specDisplay, specPosX, specPosY, specSizeX, specSizeY,
                 settings.sampleRate, settings.isIQ, ui.freqScale, ui.minDB, ui.maxDB,
                 ui.viewLo, ui.viewHi);

    measurements.draw(specDisplay, specPosX, specPosY, specSizeX, specSizeY,
                      settings.sampleRate, settings.isIQ, ui.freqScale, ui.minDB, ui.maxDB,
                      ui.viewLo, ui.viewHi);

    handleSpectrumInput(audio, ui, specDisplay, cursors,
                        specPosX, specPosY, specSizeX, specSizeY);

    ImGui::Dummy({availW, specH});
}

void DisplayPanel::renderWaterfall(AudioEngine& audio, UIState& ui,
                                    WaterfallDisplay& waterfall, SpectrumDisplay& specDisplay,
                                    Cursors& cursors, Measurements& measurements,
                                    ColorMap& colorMap) {
    const auto& settings = audio.settings();

    float availW = ImGui::GetContentRegionAvail().x;
    constexpr float kSplitterH = 6.0f;
    float parentH = ImGui::GetContentRegionAvail().y;
    float availH = (parentH - kSplitterH) * (1.0f - spectrumFrac);

    int neededH = std::max(1024, static_cast<int>(availH) + 1);
    int binCount = std::max(1, audio.spectrumSize());
    if (binCount != waterfall.width() || waterfall.height() < neededH) {
        waterfall.resize(binCount, neededH);
        waterfall.setColorMap(colorMap);
    }

    if (waterfall.textureID()) {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto texID = static_cast<ImTextureID>(waterfall.textureID());

        int h = waterfall.height();
        int screenRows = std::min(static_cast<int>(availH), h);
        int newestRow = (waterfall.currentRow() + 1) % h;

        float rowToV = 1.0f / h;
        bool logMode = (ui.freqScale == FreqScale::Logarithmic && !settings.isIQ);

        auto drawSpan = [&](int rowStart, int rowCount, float yStart, float spanH) {
            float v0 = rowStart * rowToV;
            float v1 = (rowStart + rowCount) * rowToV;

            if (!logMode) {
                dl->AddImage(texID,
                             {pos.x, yStart},
                             {pos.x + availW, yStart + spanH},
                             {ui.viewLo, v1}, {ui.viewHi, v0});
            } else {
                constexpr float kMinBinFrac = kMinLogBinFrac;
                float logMin2 = std::log10(kMinBinFrac);
                float logMax2 = 0.0f;
                int numStrips = std::min(512, static_cast<int>(availW));
                for (int s = 0; s < numStrips; ++s) {
                    float sL = static_cast<float>(s) / numStrips;
                    float sR = static_cast<float>(s + 1) / numStrips;
                    float vfL = ui.viewLo + sL * (ui.viewHi - ui.viewLo);
                    float vfR = ui.viewLo + sR * (ui.viewHi - ui.viewLo);
                    float uL = std::pow(10.0f, logMin2 + vfL * (logMax2 - logMin2));
                    float uR = std::pow(10.0f, logMin2 + vfR * (logMax2 - logMin2));
                    dl->AddImage(texID,
                                 {pos.x + sL * availW, yStart},
                                 {pos.x + sR * availW, yStart + spanH},
                                 {uL, v1}, {uR, v0});
                }
            }
        };

        float pxPerRow = availH / static_cast<float>(screenRows);

        if (newestRow + screenRows <= h) {
            drawSpan(newestRow, screenRows, pos.y, availH);
        } else {
            int firstCount = h - newestRow;
            int secondCount = screenRows - firstCount;

            float secondH = secondCount * pxPerRow;
            if (secondCount > 0)
                drawSpan(0, secondCount, pos.y, secondH);

            float firstH = availH - secondH;
            drawSpan(newestRow, firstCount, pos.y + secondH, firstH);
        }

        // ── Frequency axis labels ──
        ImU32 textCol = IM_COL32(180, 180, 200, 200);
        double freqFullMin = freqMin(settings.sampleRate, settings.isIQ);
        double freqFullMax = freqMax(settings.sampleRate, settings.isIQ);

        auto viewFracToFreq = [&](float vf) -> double {
            if (logMode) {
                constexpr float kMinBinFrac = kMinLogBinFrac;
                float logMin2 = std::log10(kMinBinFrac);
                float logMax2 = 0.0f;
                float binFrac = std::pow(10.0f, logMin2 + vf * (logMax2 - logMin2));
                return freqFullMin + binFrac * (freqFullMax - freqFullMin);
            }
            return freqFullMin + vf * (freqFullMax - freqFullMin);
        };

        int numLabels = 8;
        for (int i = 0; i <= numLabels; ++i) {
            float frac = static_cast<float>(i) / numLabels;
            float vf = ui.viewLo + frac * (ui.viewHi - ui.viewLo);
            double freq = viewFracToFreq(vf);
            float x = pos.x + frac * availW;

            char label[32];
            if (std::abs(freq) >= 1e6)
                std::snprintf(label, sizeof(label), "%.2fM", freq / 1e6);
            else if (std::abs(freq) >= 1e3)
                std::snprintf(label, sizeof(label), "%.1fk", freq / 1e3);
            else
                std::snprintf(label, sizeof(label), "%.0f", freq);

            dl->AddText({x + 2, pos.y + 2}, textCol, label);
        }

        wfPosX = pos.x; wfPosY = pos.y; wfSizeX = availW; wfSizeY = availH;

        measurements.drawWaterfall(specDisplay, wfPosX, wfPosY, wfSizeX, wfSizeY,
                                   settings.sampleRate, settings.isIQ, ui.freqScale,
                                   ui.viewLo, ui.viewHi, screenRows, audio.spectrumSize());

        // ── Mouse interaction: zoom, pan & hover on waterfall ──
        ImGuiIO& io = ImGui::GetIO();
        float mx = io.MousePos.x;
        float my = io.MousePos.y;
        bool inWaterfall = mx >= pos.x && mx <= pos.x + availW &&
                           my >= pos.y && my <= pos.y + availH;

        if (inWaterfall) {
            hoverPanel = HoverPanel::Waterfall;
            double freq = specDisplay.screenXToFreq(mx, pos.x, availW,
                                                     settings.sampleRate,
                                                     settings.isIQ, ui.freqScale,
                                                     ui.viewLo, ui.viewHi);
            int bins = audio.spectrumSize();
            double fMin = freqMin(settings.sampleRate, settings.isIQ);
            double fMax = freqMax(settings.sampleRate, settings.isIQ);
            int bin = static_cast<int>((freq - fMin) / (fMax - fMin) * (bins - 1));
            bin = std::clamp(bin, 0, bins - 1);

            float yFrac = 1.0f - (my - pos.y) / availH;
            int hopSamples = static_cast<int>(settings.fftSize * (1.0f - settings.overlap));
            if (hopSamples < 1) hopSamples = 1;
            double secondsPerLine = static_cast<double>(hopSamples) / settings.sampleRate;
            hoverWfTimeOff = static_cast<float>(yFrac * screenRows * secondsPerLine);

            int curCh = std::clamp(ui.waterfallChannel, 0, audio.totalNumSpectra() - 1);
            const auto& spec = audio.getSpectrum(curCh);
            if (!spec.empty()) {
                cursors.hover = {true, freq, spec[bin], bin};
            }
        }

        if (inWaterfall) {
            if (io.MouseWheel != 0)
                zoomView(ui.viewLo, ui.viewHi, (mx - pos.x) / availW, io.MouseWheel);

            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.0f))
                panView(ui.viewLo, ui.viewHi, io.MouseDelta.x, availW);

            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle)) {
                ui.viewLo = 0.0f;
                ui.viewHi = 1.0f;
            }
        }
    }

    ImGui::Dummy({availW, availH});
}

void DisplayPanel::renderHoverOverlay(const AudioEngine& audio, const UIState& ui,
                                       const Cursors& cursors, const SpectrumDisplay& specDisplay) {
    if (!cursors.hover.active || specSizeX <= 0 || wfSizeX <= 0)
        return;

    const auto& settings = audio.settings();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float hx = specDisplay.freqToScreenX(cursors.hover.freq,
                   specPosX, specSizeX, settings.sampleRate,
                   settings.isIQ, ui.freqScale, ui.viewLo, ui.viewHi);
    ImU32 hoverCol = IM_COL32(200, 200, 200, 80);

    dl->AddLine({hx, specPosY}, {hx, specPosY + specSizeY}, hoverCol, 1.0f);

    char freqLabel[48];
    fmtFreq(freqLabel, sizeof(freqLabel), cursors.hover.freq);

    ImVec2 tSz = ImGui::CalcTextSize(freqLabel);
    float lx = std::min(hx + 4, wfPosX + wfSizeX - tSz.x - 4);
    float ly = wfPosY + 2;
    dl->AddRectFilled({lx - 2, ly - 1}, {lx + tSz.x + 2, ly + tSz.y + 1},
                       IM_COL32(0, 0, 0, 180));
    dl->AddText({lx, ly}, IM_COL32(220, 220, 240, 240), freqLabel);

    // Hover info (right side)
    {
        int bins = audio.spectrumSize();
        double fMin = freqMin(settings.sampleRate, settings.isIQ);
        double fMax = freqMax(settings.sampleRate, settings.isIQ);
        double binCenterFreq = fMin + (static_cast<double>(cursors.hover.bin) + 0.5)
                               / bins * (fMax - fMin);

        char hoverBuf[128];
        if (hoverPanel == HoverPanel::Spectrum) {
            fmtFreqDB(hoverBuf, sizeof(hoverBuf), "", binCenterFreq, cursors.hover.dB);
        } else if (hoverPanel == HoverPanel::Waterfall) {
            fmtFreqTime(hoverBuf, sizeof(hoverBuf), "", binCenterFreq, -hoverWfTimeOff);
        } else {
            fmtFreq(hoverBuf, sizeof(hoverBuf), binCenterFreq);
        }

        ImU32 hoverTextCol = IM_COL32(100, 230, 130, 240);
        float rightEdge = specPosX + specSizeX - 8;
        float hy2 = specPosY + 4;
        ImVec2 hSz = ImGui::CalcTextSize(hoverBuf);
        dl->AddText({rightEdge - hSz.x, hy2}, hoverTextCol, hoverBuf);
    }
}

void DisplayPanel::handleSpectrumInput(AudioEngine& audio, UIState& ui,
                                        SpectrumDisplay& specDisplay, Cursors& cursors,
                                        float posX, float posY, float sizeX, float sizeY) {
    const auto& settings = audio.settings();

    ImGuiIO& io = ImGui::GetIO();
    float mx = io.MousePos.x;
    float my = io.MousePos.y;

    bool inRegion = mx >= posX && mx <= posX + sizeX &&
                    my >= posY && my <= posY + sizeY;

    if (inRegion) {
        hoverPanel = HoverPanel::Spectrum;
        double freq = specDisplay.screenXToFreq(mx, posX, sizeX,
                                                 settings.sampleRate,
                                                 settings.isIQ, ui.freqScale,
                                                 ui.viewLo, ui.viewHi);
        float dB = specDisplay.screenYToDB(my, posY, sizeY, ui.minDB, ui.maxDB);

        int bins = audio.spectrumSize();
        double fMin = freqMin(settings.sampleRate, settings.isIQ);
        double fMax = freqMax(settings.sampleRate, settings.isIQ);
        int bin = static_cast<int>((freq - fMin) / (fMax - fMin) * (bins - 1));
        bin = std::clamp(bin, 0, bins - 1);

        int curCh = std::clamp(ui.waterfallChannel, 0, audio.totalNumSpectra() - 1);
        const auto& spec = audio.getSpectrum(curCh);
        if (!spec.empty()) {
            dB = spec[bin];
            cursors.hover = {true, freq, dB, bin};
        }

        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            int cBin = cursors.snapToPeaks ? cursors.findLocalPeak(spec, bin, 10) : bin;
            double cFreq = audio.binToFreq(cBin);
            cursors.setCursorA(cFreq, spec[cBin], cBin);
        }
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            int cBin = cursors.snapToPeaks ? cursors.findLocalPeak(spec, bin, 10) : bin;
            double cFreq = audio.binToFreq(cBin);
            cursors.setCursorB(cFreq, spec[cBin], cBin);
        }

        {
            if (io.MouseWheel != 0 && (io.KeyCtrl || io.KeyShift)) {
                float zoom = io.MouseWheel * 5.0f;
                ui.minDB += zoom;
                ui.maxDB -= zoom;
                if (ui.maxDB - ui.minDB < 10.0f) {
                    float mid = (ui.minDB + ui.maxDB) / 2.0f;
                    ui.minDB = mid - 5.0f;
                    ui.maxDB = mid + 5.0f;
                }
            }
            else if (io.MouseWheel != 0) {
                zoomView(ui.viewLo, ui.viewHi, (mx - posX) / sizeX, io.MouseWheel);
            }

            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.0f))
                panView(ui.viewLo, ui.viewHi, io.MouseDelta.x, sizeX);

            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle)) {
                ui.viewLo = 0.0f;
                ui.viewHi = 1.0f;
            }
        }
    } else {
        if (hoverPanel != HoverPanel::Waterfall) {
            hoverPanel = HoverPanel::None;
            cursors.hover.active = false;
        }
    }
}

void DisplayPanel::handleTouch(const SDL_Event& event, UIState& ui, SDL_Window* window) {
    if (event.type == SDL_FINGERDOWN) {
        ++touch_.count;
    } else if (event.type == SDL_FINGERUP) {
        touch_.count = std::max(0, touch_.count - 1);
    }

    if (touch_.count == 2 && event.type == SDL_FINGERDOWN) {
        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        SDL_TouchID tid = event.tfinger.touchId;
        int nf = SDL_GetNumTouchFingers(tid);
        if (nf >= 2) {
            SDL_Finger* f0 = SDL_GetTouchFinger(tid, 0);
            SDL_Finger* f1 = SDL_GetTouchFinger(tid, 1);
            if (!f0 || !f1) return;
            float x0 = f0->x * w, x1 = f1->x * w;
            float dx = x1 - x0, dy = (f1->y - f0->y) * h;
            touch_.startDist = std::sqrt(dx * dx + dy * dy);
            touch_.lastDist = touch_.startDist;
            touch_.startCenterX = (x0 + x1) * 0.5f;
            touch_.lastCenterX = touch_.startCenterX;
            touch_.startLo = ui.viewLo;
            touch_.startHi = ui.viewHi;
        }
    }

    if (touch_.count == 2 && event.type == SDL_FINGERMOTION) {
        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        SDL_TouchID tid = event.tfinger.touchId;
        int nf = SDL_GetNumTouchFingers(tid);
        if (nf >= 2) {
            SDL_Finger* f0 = SDL_GetTouchFinger(tid, 0);
            SDL_Finger* f1 = SDL_GetTouchFinger(tid, 1);
            if (!f0 || !f1) return;
            float x0 = f0->x * w, x1 = f1->x * w;
            float dx = x1 - x0, dy = (f1->y - f0->y) * h;
            float dist = std::sqrt(dx * dx + dy * dy);
            float centerX = (x0 + x1) * 0.5f;

            if (touch_.startDist > 1.0f) {
                float span0 = touch_.startHi - touch_.startLo;
                float ratio = touch_.startDist / std::max(dist, 1.0f);
                float newSpan = std::clamp(span0 * ratio, 0.001f, 1.0f);

                float panelW = wfSizeX > 0 ? wfSizeX : static_cast<float>(w);
                float panelX = wfPosX;
                float midFrac = (touch_.startCenterX - panelX) / panelW;
                float midView = touch_.startLo + midFrac * span0;

                float panDelta = -(centerX - touch_.startCenterX) / panelW * newSpan;

                float newLo = midView - midFrac * newSpan + panDelta;
                float newHi = newLo + newSpan;

                if (newLo < 0.0f) { newHi -= newLo; newLo = 0.0f; }
                if (newHi > 1.0f) { newLo -= (newHi - 1.0f); newHi = 1.0f; }
                ui.viewLo = std::clamp(newLo, 0.0f, 1.0f);
                ui.viewHi = std::clamp(newHi, 0.0f, 1.0f);
            }
        }
    }
}

} // namespace baudmine
