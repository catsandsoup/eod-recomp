#pragma once
// Track draw distance (docs/LOD_CULLING_INVESTIGATION.md, "Draw distance").
// Pure helpers shared by the runtime hooks and the CPU contract test.
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace eod_draw {
constexpr float kMin = 1.0f, kMax = 8.0f;
// mlist group records hold squared distances; 4e16 (2e8 world units) marks an
// open-ended level. Anything at or above this is never scaled.
constexpr float kOpenEnded = 1.0e15f;

// "1".."8" (decimals allowed). Rejects malformed/out-of-range text.
inline bool parse(const char *text, float &out) {
    out = 1;
    if (!text || !*text) return false;
    char *end = nullptr;
    errno = 0;
    const float v = std::strtof(text, &end);
    if (errno || end == text || *end || !std::isfinite(v) || v < kMin || v > kMax) return false;
    out = v;
    return true;
}

// One 32-byte mlist group record: {t0, t1, t2} squared level distances.
// Native f_00357E60 returns a level mask; past the last finite level no model
// matches it and the whole group is skipped. Only that final cutoff is scaled
// (by factor^2), so every native LOD transition distance is unchanged.
// Returns the index scaled, or -1 when the record is left alone.
inline int scale_cutoff(float t[3], float factor) {
    if (!(factor > 1) || !std::isfinite(factor)) return -1;
    int last = -1;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(t[i]) || t[i] < 0) return -1;
        if (i && t[i] < t[i - 1]) return -1;        // native order: nearest level first
        if (t[i] < kOpenEnded) last = i;
    }
    if (last < 0 || t[last] <= 0) return -1;
    const double scaled = double(t[last]) * double(factor) * double(factor);
    if (scaled >= double(kOpenEnded)) return -1;
    t[last] = float(scaled);
    return last;
}
}  // namespace eod_draw
