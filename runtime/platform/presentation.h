// Per-composed-frame aspect. Only the EE chooses game context; the GS worker
// receives its snapshot in command order, without reading guest RAM.
#pragma once
#include "display_rect.h"
#include <atomic>
#include <cstdint>
namespace presentation {
inline std::atomic<uint64_t> fresh_game_presents{0};
// The displayed aspect the game wants now: set by platform_set_aspect (from
// src/game_patches.cpp, EE thread) on all platforms; -1 = the
// launch setting.
inline std::atomic<int> requested{-1};
inline std::atomic<int> drawable{0};
inline std::atomic<uint64_t> refresh_period_ns{0};
inline std::atomic<int> swapchain_aspect{-1};
inline int active_drawable() {
    const int swapchain = swapchain_aspect.load(std::memory_order_relaxed);
    return swapchain >= 0 ? swapchain : drawable.load(std::memory_order_relaxed);
}
inline void set_drawable_size(int width, int height) {
    if (width > 0 && height > 0)
        drawable.store(drawable_aspect(width, height), std::memory_order_relaxed);
}
inline thread_local int frame_aspect = -1;
inline int current(int startup_aspect) { return frame_aspect < 0 ? startup_aspect : frame_aspect; }
inline void set(int aspect) { frame_aspect = (aspect >= 0 && aspect <= 3) || custom_aspect(aspect) ? aspect : -1; }
// Zero is reserved for old GS captures, which retain their launch aspect.
inline uint32_t tag(int aspect) { return custom_aspect(aspect) ? uint32_t(aspect) : 0x41535000u | uint32_t(aspect + 1); }
inline int from_tag(uint32_t value) {
    if (custom_aspect(int(value))) return int(value);
    return (value & ~7u) == 0x41535000u && (value & 7u) >= 1 && (value & 7u) <= 4 ? int(value & 7u) - 1 : -1;
}
inline int for_context(int launch_aspect, bool racing) {
    return (launch_aspect == 1 || launch_aspect == 3) && !racing ? 0 : launch_aspect;
}
} // namespace presentation
// Target-specific producer, called only on the EE thread.
int eod_presentation_aspect();
