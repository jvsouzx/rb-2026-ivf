#ifndef QUANTIZE_HPP
#define QUANTIZE_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

inline constexpr int kQuantizeLevels = 10000;
inline constexpr float kQuantizeScale = 1.0f / kQuantizeLevels;

inline std::int16_t quantize(float value) {
    if (value < 0.0f) {
        return static_cast<std::int16_t>(-kQuantizeLevels); // dado faltante (-1)
    }
    int q = static_cast<int>(std::lround(value * kQuantizeLevels));
    return static_cast<std::int16_t>(std::clamp(q, 0, kQuantizeLevels));
}

inline std::array<std::int16_t, 14> quantizeVector(const std::array<float, 14>& v) {
    std::array<std::int16_t, 14> out{};
    for (int i = 0; i < 14; ++i) out[i] = quantize(v[i]);
    return out;
}

#endif
