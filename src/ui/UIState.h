#pragma once

#include "core/Types.h"
#include <imgui.h>
#include <array>

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
    std::array<bool, kMaxChannels> channelEnabled = {true,true,true,true,true,true,true,true};
    // Complementary pairs: colors at indices 0+1, 2+3, 4+5, 6+7 sum to white.
    std::array<ImVec4, kMaxChannels> channelColors = {{
        {0.20f, 0.90f, 0.20f, 1.0f},  // green
        {0.80f, 0.10f, 0.80f, 1.0f},  // purple
        {1.00f, 0.55f, 0.00f, 1.0f},  // orange
        {0.00f, 0.45f, 1.00f, 1.0f},  // cyan
        {1.00f, 0.25f, 0.25f, 1.0f},  // red
        {0.00f, 0.75f, 0.75f, 1.0f},  // teal
        {1.00f, 1.00f, 0.20f, 1.0f},  // yellow
        {0.00f, 0.00f, 0.80f, 1.0f},  // blue
    }};
};

} // namespace baudmine
