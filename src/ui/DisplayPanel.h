#pragma once

#include "core/Types.h"
#include "ui/UIState.h"
#include "ui/WaterfallDisplay.h"
#include "ui/SpectrumDisplay.h"
#include "ui/Cursors.h"
#include "ui/Measurements.h"
#include "ui/ColorMap.h"

#include <SDL.h>
#include <vector>

namespace baudmine {

class AudioEngine;

class DisplayPanel {
public:
    void renderSpectrum(AudioEngine& audio, UIState& ui,
                        SpectrumDisplay& specDisplay, Cursors& cursors,
                        Measurements& measurements);

    void renderWaterfall(AudioEngine& audio, UIState& ui,
                         WaterfallDisplay& waterfall, SpectrumDisplay& specDisplay,
                         Cursors& cursors, Measurements& measurements,
                         ColorMap& colorMap);

    void renderHoverOverlay(const AudioEngine& audio, const UIState& ui,
                            const Cursors& cursors, const SpectrumDisplay& specDisplay);

    void handleTouch(const SDL_Event& event, UIState& ui, SDL_Window* window);

    // Panel geometry (read by Application for layout)
    float specPosX = 0, specPosY = 0, specSizeX = 0, specSizeY = 0;
    float wfPosX   = 0, wfPosY   = 0, wfSizeX   = 0, wfSizeY   = 0;

    enum class HoverPanel { None, Spectrum, Waterfall };
    HoverPanel hoverPanel      = HoverPanel::None;
    float      hoverWfTimeOff  = 0.0f;

    float spectrumFrac  = 0.35f;
    bool  draggingSplit  = false;

    // Returns true if split was just released (caller should save config).
    bool splitReleased();

private:
    void handleSpectrumInput(AudioEngine& audio, UIState& ui,
                             SpectrumDisplay& specDisplay, Cursors& cursors,
                             float posX, float posY, float sizeX, float sizeY);

    struct TouchState {
        int   count = 0;
        float startDist = 0.0f;
        float startLo = 0.0f;
        float startHi = 0.0f;
        float startCenterX = 0.0f;
        float lastCenterX = 0.0f;
        float lastDist = 0.0f;
    } touch_;

    bool splitWasReleased_ = false;

    // Scratch buffers
    std::vector<std::vector<float>>   wfSpectraScratch_;
    std::vector<WaterfallChannelInfo> wfChInfoScratch_;
    std::vector<std::vector<float>>   allSpectraScratch_;
    std::vector<ChannelStyle>         stylesScratch_;
};

} // namespace baudmine
