#pragma once

#include "core/Types.h"
#include <imgui.h>

namespace baudmine {

// Shared display state accessed by both ControlPanel and DisplayPanel.
struct UIState {
    float     minDB       = -120.0f;
    float     maxDB       = 0.0f;
    FreqScale freqScale   = FreqScale::Linear;
    float     viewLo      = 0.0f;
    float     viewHi      = 0.5f;
    bool      paused      = false;

    int  waterfallChannel = 0;
    bool waterfallMultiCh = true;
    bool channelEnabled[kMaxChannels] = {true,true,true,true,true,true,true,true};
    ImVec4 channelColors[kMaxChannels] = {
        {0.20f, 0.90f, 0.30f, 1.0f},  // green
        {0.70f, 0.30f, 1.00f, 1.0f},  // purple
        {1.00f, 0.55f, 0.00f, 1.0f},  // orange
        {0.00f, 0.75f, 1.00f, 1.0f},  // cyan
        {1.00f, 0.25f, 0.25f, 1.0f},  // red
        {1.00f, 1.00f, 0.30f, 1.0f},  // yellow
        {0.50f, 0.80f, 0.50f, 1.0f},  // light green
        {0.80f, 0.50f, 0.80f, 1.0f},  // pink
    };
};

} // namespace baudmine
