#pragma once

#include "core/Types.h"
#include "ui/ColorMap.h"
#include <GL/gl.h>
#include <vector>
#include <deque>

namespace baudmine {

struct WaterfallChannelInfo {
    float r, g, b;
    bool  enabled;
};

class WaterfallDisplay {
public:
    WaterfallDisplay();
    ~WaterfallDisplay();

    // Initialize with bin-resolution width and history height.
    // width = number of FFT bins (spectrum size), height = history rows.
    void init(int binCount, int height);

    // Single-channel colormap mode.  One texel per bin — no frequency remapping.
    void pushLine(const std::vector<float>& spectrumDB, float minDB, float maxDB);

    // Multi-channel overlay mode.  One texel per bin.
    void pushLineMulti(const std::vector<std::vector<float>>& channelSpectra,
                       const std::vector<WaterfallChannelInfo>& channels,
                       float minDB, float maxDB);

    GLuint textureID()  const { return texture_; }
    int    width()      const { return width_; }
    int    height()     const { return height_; }
    int    currentRow() const { return currentRow_; }

    void resize(int binCount, int height);
    void setColorMap(const ColorMap& cm) { colorMap_ = cm; }

private:
    void uploadRow(int row);
    void advanceRow();

    GLuint               texture_ = 0;
    int                  width_   = 0;
    int                  height_  = 0;
    int                  currentRow_ = 0;

    ColorMap             colorMap_;
    std::vector<uint8_t> pixelBuf_;

    // Scratch buffer for pushLineMulti (pre-filtered enabled channels).
    struct ActiveCh { const float* data; int bins; float r, g, b; };
    std::vector<ActiveCh> activeChBuf_;
};

} // namespace baudmine
