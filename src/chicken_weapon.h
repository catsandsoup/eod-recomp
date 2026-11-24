#pragma once

#include "core/memory.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace chicken_weapon {
constexpr uint32_t kInterface = 0x1DC;
inline bool valid(uint32_t a, uint32_t bytes) {
    return a && !(a & 3) && a <= EE_RAM_SIZE - bytes;
}
inline uint32_t interface_for(uint32_t owner) {
    const uint32_t count = ee_r32(0x3CE4B8);
    if (count > 16) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t row = 0x3CE3F8 + i * 12, iface = ee_r32(row + 8);
        if (ee_r32(row) == owner && ee_r32(row + 4) == 0 && iface > kInterface &&
            valid(iface - kInterface, 0x228) && ee_r32(iface) == 0x3F9528 &&
            ee_r32(iface + 12) == owner) return iface;
    }
    return 0;
}
inline float read_float(uint32_t a) {
    const uint32_t word = ee_r32(a);
    float value;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}
inline void read3(uint32_t a, float out[3]) {
    for (unsigned i = 0; i < 3; ++i) out[i] = read_float(a + i * 4);
}
inline bool finite3(const float p[3]) {
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
}
inline bool tracing() {
    static const bool enabled = std::getenv("EOD_BATTLE_TRACE") != nullptr;
    return enabled;
}
inline void trace(const char *phase, uint32_t owner, const float p[3]) {
    if (tracing())
        std::fprintf(stderr, "[chicken] phase=%s slot=%u pos=%.3f,%.3f,%.3f\n",
                     phase, owner, p[0], p[1], p[2]);
}
} // namespace chicken_weapon
