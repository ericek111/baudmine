#pragma once

#include "core/Types.h"
#include <vector>

namespace baudmine {

class ColorMap {
public:
    explicit ColorMap(ColorMapType type = ColorMapType::Magma);

    void setType(ColorMapType type);
    ColorMapType type() const { return type_; }

    // Map a normalized value [0,1] to RGB.
    Color3 map(float value) const;

    // Map dB value to RGB given current range.
    Color3 mapDB(float dB, float minDB, float maxDB) const;

    // Get the full 256-entry LUT.
    const std::vector<Color3>& lut() const { return lut_; }

private:
    void buildLUT();

    ColorMapType        type_;
    std::vector<Color3> lut_;  // 256 entries
};

} // namespace baudmine
