#pragma once

#include "core/Types.h"
#include "ui/ColorMap.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <GL/gl.h>
#ifndef GL_CLAMP_TO_EDGE // for Windows
#define GL_CLAMP_TO_EDGE 0x812F
#endif
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

    // Initialize with bin count and history height.
    // Texture width is capped to GPU max texture size; bins are resampled if needed.
    void init(int binCount, int height);

    // Single-channel colormap mode.
    void pushLine(const std::vector<float>& spectrumDB, float minDB, float maxDB);

    // Multi-channel overlay mode.
    void pushLineMulti(const std::vector<std::vector<float>>& channelSpectra,
                       const std::vector<WaterfallChannelInfo>& channels,
                       float minDB, float maxDB);

    GLuint textureID()  const { return texture_; }
    int    texWidth()   const { return texW_; }
    int    width()      const { return width_; }
    int    height()     const { return height_; }
    int    currentRow() const { return currentRow_; }

    void resize(int binCount, int height);
    void setColorMap(const ColorMap& cm) { colorMap_ = cm; }

private:
    void uploadRow(int row);
    void advanceRow();

    GLuint               texture_ = 0;
    int                  width_   = 0;   // logical width (bin count)
    int                  texW_    = 0;   // actual texture width (<= GPU max)
    int                  height_  = 0;
    int                  currentRow_ = 0;
    static int           maxTexSize_;    // cached GL_MAX_TEXTURE_SIZE

    ColorMap             colorMap_;
    std::vector<uint8_t> pixelBuf_;

    // Scratch buffer for pushLineMulti (pre-filtered enabled channels).
    struct ActiveCh { const float* data; int bins; float r, g, b; };
    std::vector<ActiveCh> activeChBuf_;
};

} // namespace baudmine
