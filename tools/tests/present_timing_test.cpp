// CPU-only regression tests for refresh-grid scheduling. No SDL/Vulkan device.
#include "../../runtime/gs/vulkan/present_timing.h"
#include <cstdio>
#include <cstdlib>

static void expect(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}
int main() {
    using vkgs::PresentCadence;
    constexpr uint64_t refresh = 8333333, phase = 1000000000;
    PresentCadence cadence;
    cadence.refresh(refresh);
    expect(cadence.schedule(phase, true) == 0, "wait for real display feedback instead of inventing a clock phase");
    cadence.observe(phase);
    const auto first = cadence.schedule(phase, true);
    expect(first == phase + refresh / 2 + refresh % 2, "request first image before next measured refresh");
    auto previous = first;
    for (int i = 1; i < 200; ++i) {
        // Drivers report completion a few images later; refresh phase advances
        // with real observations, and older/out-of-order feedback cannot rewind it.
        if (i >= 3) {
            cadence.observe(phase + 2 * refresh * (i - 2));
            cadence.observe(phase + 2 * refresh * (i - 3));
        }
        const int64_t jitter = (i % 3 - 1) * 200000;
        const auto next = cadence.schedule(phase + 2 * refresh * i + jitter, true);
        expect(next - previous == 2 * refresh, "120 Hz: small producer jitter keeps every image on two refreshes");
        previous = next;
    }
    cadence = {};
    cadence.refresh(16683333);
    cadence.observe(phase);
    previous = cadence.schedule(phase, true);
    for (int i = 1; i < 50; ++i) {
        auto next = cadence.schedule(phase + 16683333ull * i, true);
        expect(next - previous == 16683333, "59.94 Hz display: one refresh per guest frame");
        previous = next;
    }
    cadence = {};
    cadence.refresh(refresh);
    cadence.observe(phase);
    previous = cadence.schedule(phase, true);
    int longIntervals = 0;
    for (int i = 1; i < 1200; ++i) {
        auto now = phase + 1001000000000ull * i / 60000;
        auto next = cadence.schedule(now, true);
        const auto interval = next - previous;
        expect(interval == 2 * refresh || interval == 3 * refresh,
               "59.94 guest / exact120 display: bounded cadence correction, no speed change");
        longIntervals += interval == 3 * refresh;
        expect(next <= now + 3 * refresh, "rate mismatch must not accumulate latency");
        previous = next;
    }
    expect(longIntervals >= 1 && longIntervals <= 4, "120 / 59.94 mismatch occasionally needs an extra refresh");
    const auto lateNow = phase + 200000000000ull;
    auto recovered = cadence.schedule(lateNow, true);
    expect(recovered > lateNow && recovered <= lateNow + 2 * refresh,
           "late frame restarts near current refresh, no old deadline");
    expect(cadence.schedule(lateNow, false) == 0, "no-vsync / catch-up sends no delayed presentation request");
    expect(cadence.schedule(lateNow, true) > lateNow, "vsync resumes with bounded lookahead");
    cadence.refresh(16666667);
    expect(cadence.schedule(lateNow, true) == 0, "refresh-rate change needs fresh phase feedback");
    cadence.observe(lateNow);
    auto sixty = cadence.schedule(lateNow, true);
    expect(sixty > lateNow && sixty <= lateNow + 16666667, "60 Hz monitor resumes on one refresh");
    cadence.refresh(0);
    expect(cadence.schedule(lateNow, true) == 0, "invalid refresh duration falls back to FIFO");
    std::puts("presentation cadence tests passed");
}
