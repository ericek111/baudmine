#include "ui/WaterfallDisplay.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace baudmine {

WaterfallDisplay::WaterfallDisplay() = default;

WaterfallDisplay::~WaterfallDisplay() {
    if (texture_) glDeleteTextures(1, &texture_);
}

void WaterfallDisplay::init(int binCount, int height) {
    width_  = binCount;
    height_ = height;
    currentRow_ = height_ - 1;

    pixelBuf_.assign(width_ * height_ * 3, 0);

    if (texture_) glDeleteTextures(1, &texture_);
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);  // RGB rows may not be 4-byte aligned
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width_, height_, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, pixelBuf_.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

void WaterfallDisplay::resize(int binCount, int height) {
    if (binCount == width_ && height == height_) return;

    // If width unchanged and height is growing, preserve existing data.
    if (binCount == width_ && height > height_ && height_ > 0 && texture_) {
        int oldH = height_;
        int oldRow = currentRow_;
        std::vector<uint8_t> oldBuf = std::move(pixelBuf_);

        width_  = binCount;
        height_ = height;
        pixelBuf_.assign(width_ * height_ * 3, 0);

        // Copy old rows into the new buffer, preserving their circular order.
        // Old rows occupy indices 0..oldH-1; new rows oldH..height-1 are black.
        // The circular position stays the same since old indices are valid in
        // the larger buffer.
        int rowBytes = width_ * 3;
        for (int r = 0; r < oldH; ++r)
            std::memcpy(pixelBuf_.data() + r * rowBytes,
                        oldBuf.data() + r * rowBytes, rowBytes);

        currentRow_ = oldRow;

        // Recreate texture at new size and upload all data.
        if (texture_) glDeleteTextures(1, &texture_);
        glGenTextures(1, &texture_);
        glBindTexture(GL_TEXTURE_2D, texture_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width_, height_, 0,
                     GL_RGB, GL_UNSIGNED_BYTE, pixelBuf_.data());
        return;
    }

    init(binCount, height);
}

void WaterfallDisplay::advanceRow() {
    currentRow_ = (currentRow_ - 1 + height_) % height_;
}

// ── Single-channel (colormap) mode ───────────────────────────────────────────

void WaterfallDisplay::pushLine(const std::vector<float>& spectrumDB,
                                float minDB, float maxDB) {
    if (width_ == 0 || height_ == 0) return;

    int bins = static_cast<int>(spectrumDB.size());
    int row = currentRow_;
    int rowOffset = row * width_ * 3;

    // One texel per bin — direct 1:1 mapping.
    for (int x = 0; x < width_; ++x) {
        float dB = (x < bins) ? spectrumDB[x] : -200.0f;
        Color3 c = colorMap_.mapDB(dB, minDB, maxDB);

        pixelBuf_[rowOffset + x * 3 + 0] = c.r;
        pixelBuf_[rowOffset + x * 3 + 1] = c.g;
        pixelBuf_[rowOffset + x * 3 + 2] = c.b;
    }

    uploadRow(row);
    advanceRow();
}

// ── Multi-channel overlay mode ───────────────────────────────────────────────

void WaterfallDisplay::pushLineMulti(
        const std::vector<std::vector<float>>& channelSpectra,
        const std::vector<WaterfallChannelInfo>& channels,
        float minDB, float maxDB) {
    if (width_ == 0 || height_ == 0) return;

    int row = currentRow_;
    int rowOffset = row * width_ * 3;
    float range = maxDB - minDB;
    if (range < 1.0f) range = 1.0f;
    float invRange = 1.0f / range;

    // Pre-filter enabled channels to avoid per-texel branching.
    struct ActiveCh { const float* data; int bins; float r, g, b; };
    activeChBuf_.clear();
    int nCh = std::min(static_cast<int>(channelSpectra.size()),
                       static_cast<int>(channels.size()));
    for (int ch = 0; ch < nCh; ++ch) {
        if (!channels[ch].enabled || channelSpectra[ch].empty()) continue;
        activeChBuf_.push_back({channelSpectra[ch].data(),
                                static_cast<int>(channelSpectra[ch].size()),
                                channels[ch].r, channels[ch].g, channels[ch].b});
    }

    // One texel per bin — direct 1:1 mapping.
    for (int x = 0; x < width_; ++x) {
        float accR = 0.0f, accG = 0.0f, accB = 0.0f;

        for (const auto& ac : activeChBuf_) {
            float dB = (x < ac.bins) ? ac.data[x] : -200.0f;
            float intensity = std::clamp((dB - minDB) * invRange, 0.0f, 1.0f);

            accR += ac.r * intensity;
            accG += ac.g * intensity;
            accB += ac.b * intensity;
        }

        pixelBuf_[rowOffset + x * 3 + 0] =
            static_cast<uint8_t>(std::min(accR, 1.0f) * 255.0f);
        pixelBuf_[rowOffset + x * 3 + 1] =
            static_cast<uint8_t>(std::min(accG, 1.0f) * 255.0f);
        pixelBuf_[rowOffset + x * 3 + 2] =
            static_cast<uint8_t>(std::min(accB, 1.0f) * 255.0f);
    }

    uploadRow(row);
    advanceRow();
}

void WaterfallDisplay::uploadRow(int row) {
    glBindTexture(GL_TEXTURE_2D, texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, width_, 1,
                    GL_RGB, GL_UNSIGNED_BYTE,
                    pixelBuf_.data() + row * width_ * 3);
    // Note: no unbind — ImGui will bind its own textures before drawing.
}

} // namespace baudmine
