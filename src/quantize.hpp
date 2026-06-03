#ifndef QUANTIZE_HPP
#define QUANTIZE_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
// scale = 1/254, zero_point=0, 255 valor sentinela (para os casos de -1)

inline constexpr float kQuantizeScale = 1.0f/254.0f;

inline uint8_t quantize(float value){
    if (value < 0) return 255;
    int q = static_cast<int>(std::round(value/kQuantizeScale));
    return static_cast<uint8_t>(std::clamp(q, 0, 254));
}

inline int16_t quantizedDistanceValue(uint8_t value) {
    return value == 255 ? static_cast<int16_t>(-254) : static_cast<int16_t>(value);
}

inline std::array<std::uint8_t, 14> quantizeVector(const std::array<float, 14>& v) {
    std::array<std::uint8_t, 14> out{};
    for (int i = 0; i < 14; ++i) out[i] = quantize(v[i]);
    return out;
}

#endif
