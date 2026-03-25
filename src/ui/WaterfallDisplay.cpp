#include "ui/WaterfallDisplay.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace baudline {

WaterfallDisplay::WaterfallDisplay() = default;

WaterfallDisplay::~WaterfallDisplay() {
    if (texture_) glDeleteTextures(1, &texture_);
}

void WaterfallDisplay::init(int width, int height) {
    width_  = width;
    height_ = height;
    currentRow_ = height_ - 1;

    pixelBuf_.resize(width_ * height_ * 3, 0);

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

void WaterfallDisplay::resize(int width, int height) {
    if (width == width_ && height == height_) return;
    init(width, height);
}

float WaterfallDisplay::sampleBin(const std::vector<float>& spec, float binF) {
    int bins = static_cast<int>(spec.size());
    int b0 = static_cast<int>(binF);
    int b1 = std::min(b0 + 1, bins - 1);
    float t = binF - b0;
    return spec[b0] * (1.0f - t) + spec[b1] * t;
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

    for (int x = 0; x < width_; ++x) {
        float frac = static_cast<float>(x) / (width_ - 1);
        float dB = sampleBin(spectrumDB, frac * (bins - 1));
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

    for (int x = 0; x < width_; ++x) {
        float frac = static_cast<float>(x) / (width_ - 1);

        // Accumulate color contributions from each enabled channel.
        float accR = 0.0f, accG = 0.0f, accB = 0.0f;

        for (int ch = 0; ch < nCh; ++ch) {
            if (ch >= static_cast<int>(channels.size()) || !channels[ch].enabled)
                continue;
            if (channelSpectra[ch].empty()) continue;

            int bins = static_cast<int>(channelSpectra[ch].size());
            float dB = sampleBin(channelSpectra[ch], frac * (bins - 1));
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
