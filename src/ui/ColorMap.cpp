#include "ui/ColorMap.h"
#include <algorithm>
#include <cmath>

namespace baudmine {

// Interpolation helper for colormaps defined as control points.
struct ColorStop {
    float pos;
    uint8_t r, g, b;
};

static Color3 interpolate(const ColorStop* stops, int count, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    // Find surrounding stops
    int i = 0;
    while (i < count - 1 && stops[i + 1].pos < t) ++i;
    if (i >= count - 1) return {stops[count - 1].r, stops[count - 1].g, stops[count - 1].b};

    float range = stops[i + 1].pos - stops[i].pos;
    float frac = (range > 0.0f) ? (t - stops[i].pos) / range : 0.0f;

    auto lerp = [](uint8_t a, uint8_t b, float f) -> uint8_t {
        return static_cast<uint8_t>(a + (b - a) * f);
    };
    return {lerp(stops[i].r, stops[i + 1].r, frac),
            lerp(stops[i].g, stops[i + 1].g, frac),
            lerp(stops[i].b, stops[i + 1].b, frac)};
}

// ── Colormap definitions (simplified control points) ─────────────────────────

static const ColorStop kMagma[] = {
    {0.00f,   0,   0,   4}, {0.13f,  27,  12,  65}, {0.25f,  72,  12, 107},
    {0.38f, 117,  15, 110}, {0.50f, 159,  42,  99}, {0.63f, 200,  72,  65},
    {0.75f, 231, 117,  36}, {0.88f, 251, 178,  55}, {1.00f, 252, 253, 191}
};

static const ColorStop kViridis[] = {
    {0.00f,  68,   1,  84}, {0.13f,  72,  36, 117}, {0.25f,  56,  88, 140},
    {0.38f,  39, 126, 142}, {0.50f,  31, 161, 135}, {0.63f,  53, 194, 114},
    {0.75f, 122, 209,  81}, {0.88f, 189, 222,  38}, {1.00f, 253, 231,  37}
};

static const ColorStop kInferno[] = {
    {0.00f,   0,   0,   4}, {0.13f,  31,  12,  72}, {0.25f,  85,  15, 109},
    {0.38f, 136,  34,  85}, {0.50f, 186,  54,  55}, {0.63f, 227,  89,  22},
    {0.75f, 249, 140,  10}, {0.88f, 249, 200,  50}, {1.00f, 252, 255, 164}
};

static const ColorStop kPlasma[] = {
    {0.00f,  13,   8, 135}, {0.13f,  75,   3, 161}, {0.25f, 125,   3, 168},
    {0.38f, 168,  34, 150}, {0.50f, 203,  70, 121}, {0.63f, 229, 107,  93},
    {0.75f, 248, 148,  65}, {0.88f, 253, 195,  40}, {1.00f, 240, 249,  33}
};

ColorMap::ColorMap(ColorMapType type) : type_(type), lut_(256) {
    buildLUT();
}

void ColorMap::setType(ColorMapType type) {
    if (type == type_) return;
    type_ = type;
    buildLUT();
}

Color3 ColorMap::map(float value) const {
    int idx = static_cast<int>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
    return lut_[idx];
}

Color3 ColorMap::mapDB(float dB, float minDB, float maxDB) const {
    float norm = (dB - minDB) / (maxDB - minDB);
    return map(std::clamp(norm, 0.0f, 1.0f));
}

void ColorMap::buildLUT() {
    lut_.resize(256);
    for (int i = 0; i < 256; ++i) {
        float t = i / 255.0f;
        switch (type_) {
            case ColorMapType::Magma:
                lut_[i] = interpolate(kMagma, 9, t);
                break;
            case ColorMapType::Viridis:
                lut_[i] = interpolate(kViridis, 9, t);
                break;
            case ColorMapType::Inferno:
                lut_[i] = interpolate(kInferno, 9, t);
                break;
            case ColorMapType::Plasma:
                lut_[i] = interpolate(kPlasma, 9, t);
                break;
            case ColorMapType::Grayscale:
                lut_[i] = {static_cast<uint8_t>(i),
                           static_cast<uint8_t>(i),
                           static_cast<uint8_t>(i)};
                break;
            default:
                lut_[i] = {static_cast<uint8_t>(i),
                           static_cast<uint8_t>(i),
                           static_cast<uint8_t>(i)};
                break;
        }
    }
}

} // namespace baudmine
