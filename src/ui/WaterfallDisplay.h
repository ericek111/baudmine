#pragma once

#include "core/Types.h"
#include "ui/ColorMap.h"
#include <GL/gl.h>
#include <vector>
#include <deque>

namespace baudline {

// Per-channel color + enable flag for multi-channel waterfall mode.
struct WaterfallChannelInfo {
    float r, g, b;    // channel color [0,1]
    bool  enabled;
};

class WaterfallDisplay {
public:
    WaterfallDisplay();
    ~WaterfallDisplay();

    // Initialize OpenGL texture.  Call after GL context is ready.
    void init(int width, int height);

    // Single-channel mode: colormap-based.
    void pushLine(const std::vector<float>& spectrumDB, float minDB, float maxDB);

    // Multi-channel overlay mode: each channel is rendered in its own color,
    // intensity proportional to signal level.  Colors are additively blended.
    void pushLineMulti(const std::vector<std::vector<float>>& channelSpectra,
                       const std::vector<WaterfallChannelInfo>& channels,
                       float minDB, float maxDB);

    GLuint textureID()  const { return texture_; }
    int    width()      const { return width_; }
    int    height()     const { return height_; }
    int    currentRow() const { return currentRow_; }

    void resize(int width, int height);

    void setColorMap(const ColorMap& cm) { colorMap_ = cm; }

    float zoomX   = 1.0f;
    float zoomY   = 1.0f;
    float scrollX = 0.0f;
    float scrollY = 0.0f;

private:
    void uploadRow(int row);
    void advanceRow();

    // Interpolate a dB value at a fractional bin position.
    static float sampleBin(const std::vector<float>& spec, float binF);

    GLuint               texture_ = 0;
    int                  width_   = 0;
    int                  height_  = 0;
    int                  currentRow_ = 0;

    ColorMap             colorMap_;
    std::vector<uint8_t> pixelBuf_;
};

} // namespace baudline
