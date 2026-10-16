// Default-off observation of the existing EE/GS/audio timing policies.
// All callers run on the EE thread. Counts are neither simulation updates nor
// device underruns; headless/muted runs have no physical audio queue.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace eod_timing {
inline bool enabled() {
    static const bool value = [] { const char *v = std::getenv("EOD_TIMING_TRACE"); return v && v[0] == '1'; }();
    return value;
}
struct Trace {
    uint64_t previous_service = 0, service_gap_max = 0, vblank_debt_max = 0;
    uint64_t catchups = 0, resyncs = 0, gs_block_max = 0;
    uint64_t previous_audio = 0, audio_gap_max = 0, audio_debt_max = 0;
    uint64_t audio_mixed = 0, audio_debt_dropped = 0, audio_queue_suppressed = 0, audio_no_device = 0;
    uint64_t last_report = 0;
    unsigned lines = 0;
    bool report_due(uint64_t now) {
        if (!last_report) { last_report = now; return false; }
        if (lines >= 120 || now - last_report < 1000000) return false;
        last_report = now;
        ++lines;
        return true;
    }
    void clear_interval() {
        service_gap_max = vblank_debt_max = catchups = resyncs = gs_block_max = 0;
        audio_gap_max = audio_debt_max = audio_mixed = audio_debt_dropped = audio_queue_suppressed = audio_no_device = 0;
    }
};
inline Trace &state() { static Trace value; return value; }
inline void service(uint64_t now) {
    auto &s = state();
    if (s.previous_service) s.service_gap_max = std::max(s.service_gap_max, now - s.previous_service);
    s.previous_service = now;
}
inline void audio_call(uint64_t now) {
    auto &s = state();
    if (s.previous_audio) s.audio_gap_max = std::max(s.audio_gap_max, now - s.previous_audio);
    s.previous_audio = now;
}
inline void report(uint64_t now, uint64_t vblank, bool configured_skip) {
    auto &s = state();
    if (!s.report_due(now)) return;
    const char *override_skip = std::getenv("EOD_FRAMESKIP");
    const bool skip = override_skip ? std::atoi(override_skip) != 0 : configured_skip;
    std::fprintf(stderr,
        "[timing] wall_us=%llu vblank=%llu service_gap_max_us=%llu vblank_debt_max_us=%llu "
        "catchups=%llu resyncs=%llu gs_block_max_us=%llu audio_gap_max_us=%llu "
        "audio_debt_max_samples=%llu audio_mixed_samples=%llu audio_debt_dropped_samples=%llu "
        "audio_queue_suppressed_samples=%llu audio_no_device_samples=%llu frameskip=%d skipall=%d\n",
        (unsigned long long)now, (unsigned long long)vblank,
        (unsigned long long)s.service_gap_max, (unsigned long long)s.vblank_debt_max,
        (unsigned long long)s.catchups, (unsigned long long)s.resyncs, (unsigned long long)s.gs_block_max,
        (unsigned long long)s.audio_gap_max, (unsigned long long)s.audio_debt_max,
        (unsigned long long)s.audio_mixed, (unsigned long long)s.audio_debt_dropped,
        (unsigned long long)s.audio_queue_suppressed, (unsigned long long)s.audio_no_device,
        skip, std::getenv("EOD_GS_SKIPALL") != nullptr);
    s.clear_interval();
}
} // namespace eod_timing
