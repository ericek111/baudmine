#include "ui/WaterfallDisplay.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace baudline {

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

    int nCh = static_cast<int>(channelSpectra.size());
    int row = currentRow_;
    int rowOffset = row * width_ * 3;
    float range = maxDB - minDB;
    if (range < 1.0f) range = 1.0f;

    // One texel per bin — direct 1:1 mapping.
    for (int x = 0; x < width_; ++x) {
        float accR = 0.0f, accG = 0.0f, accB = 0.0f;

        for (int ch = 0; ch < nCh; ++ch) {
            if (ch >= static_cast<int>(channels.size()) || !channels[ch].enabled)
                continue;
            if (channelSpectra[ch].empty()) continue;

            int bins = static_cast<int>(channelSpectra[ch].size());
            float dB = (x < bins) ? channelSpectra[ch][x] : -200.0f;
            float intensity = std::clamp((dB - minDB) / range, 0.0f, 1.0f);

            accR += channels[ch].r * intensity;
            accG += channels[ch].g * intensity;
            accB += channels[ch].b * intensity;
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
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, width_, 1,
                    GL_RGB, GL_UNSIGNED_BYTE,
                    pixelBuf_.data() + row * width_ * 3);
    glBindTexture(GL_TEXTURE_2D, 0);
}

} // namespace baudline
