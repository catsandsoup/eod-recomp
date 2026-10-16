// CPU-only scheduling policy for VK_GOOGLE_display_timing.
// Inspired by the refresh-grid approach identified in RoadTripAdventure-recomp;
// this implementation is original and uses the Vulkan extension's public API.
// https://docs.vulkan.org/refpages/latest/refpages/source/VkPresentTimeGOOGLE.html
#pragma once
#include <algorithm>
#include <cstdint>

namespace vkgs {
class PresentCadence {
    uint64_t refresh_ = 0, phase_ = 0, target_ = 0;

  public:
    void refresh(uint64_t ns) {
        // Reject missing or implausible feedback, including arithmetic hazards.
        if (ns < 1000000 || ns > 100000000)
            ns = 0;
        if (ns != refresh_) {
            refresh_ = ns;
            phase_ = target_ = 0;
        }
    }
    void observe(uint64_t actual_ns) {
        if (actual_ns > phase_)
            phase_ = actual_ns;
    }
    uint64_t schedule(uint64_t now_ns, bool vsync) {
        if (!vsync || !refresh_ || !phase_ || phase_ > now_ns) {
            target_ = 0;
            return 0;
        }
        // Choose the closest integral refresh count to an NTSC guest frame.
        // This only schedules finished pictures; it never advances guest time.
        constexpr uint64_t guest_ns = 1001000000000ull / 60000;
        const uint64_t count = std::max<uint64_t>(1, (guest_ns + refresh_ / 2) / refresh_);
        const uint64_t step = count * refresh_;
        const uint64_t earliest = now_ns + refresh_ / 2;
        uint64_t next = target_ ? target_ + step : 0;
        // Re-anchor after stalls, bursty catch-up, or eventual 59.94/120 drift.
        // The deadline stays close to the producer instead of building latency.
        if (next < earliest || next > earliest + step) {
            const uint64_t elapsed = earliest - phase_;
            next = phase_ + ((elapsed + refresh_ - 1) / refresh_) * refresh_;
        }
        target_ = next;
        // Permit the image just before the intended refresh, avoiding an exact
        // boundary whose scheduling jitter could defer it by another refresh.
        return next - refresh_ / 2;
    }
};
} // namespace vkgs
