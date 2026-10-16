// lod_thresholds.h - EOD_CAR_LOD_MULTIPLIER parsing and the threshold scaling
// for src/hle_lod.cpp (separate so the test can check them alone).
#pragma once
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace eod_lod {
constexpr unsigned max_levels = 16;  // the loader's embedded table: 16 records of 16 bytes
// Missing: 1. Anything but a finite number >= 1: false (and 1).
inline bool parse_multiplier(const char *text, float &out) {
    out = 1;
    if (!text) return true;
    char *end = nullptr;
    errno = 0;
    const float value = std::strtof(text, &end);
    if (errno || end == text || *end || !std::isfinite(value) || value < 1) return false;
    out = value;
    return true;
}
// Scales the positive thresholds of an ordered table into out; false (out
// untouched) for an unordered table, a bad count or an overflow.
inline bool scale(const float *in, float *out, unsigned count, float multiplier) {
    if (!count || count > max_levels || !std::isfinite(multiplier) || multiplier < 1) return false;
    float prepared[max_levels];
    for (unsigned i = 0; i < count; ++i) {
        const float value = in[i];
        if (!std::isfinite(value) || (i && value < in[i-1])) return false;
        const double product = value > 0 ? double(value) * multiplier : double(value);
        if (product > std::numeric_limits<float>::max()) return false;
        prepared[i] = value > 0 && multiplier != 1 ? float(product) : value;
    }
    for (unsigned i = 0; i < count; ++i) out[i] = prepared[i];
    return true;
}
}
