// eod_tracy.h - Optional Tracy C++ profiler macros (-DEOD_TRACY=ON, default OFF)
// and lightweight EOD_PROFILE=1 boot / asset-load phase timers.
//
// When EOD_TRACY is not defined, all Tracy macros compile to zero-overhead no-ops.
// When EOD_PROFILE=1 is set in the environment, high-resolution steady_clock
// (QueryPerformanceCounter on Windows) phase timings are recorded and printed:
//   boot_init, intro_movies, memcard_check, trk_load, car_load,
//   main_menu, first_gameplay_frame.
#pragma once

#include "trace.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef EOD_TRACY
#ifndef EOD_ZONE_SCOPED
#define EOD_ZONE_SCOPED ZoneScoped
#endif
#ifndef EOD_ZONE_SCOPED_N
#define EOD_ZONE_SCOPED_N(name) ZoneScopedN(name)
#endif
#else
#ifndef EOD_ZONE_SCOPED
#define EOD_ZONE_SCOPED do {} while (0)
#endif
#ifndef EOD_ZONE_SCOPED_N
#define EOD_ZONE_SCOPED_N(name) do {} while (0)
#endif
#endif

namespace eod_profile {

enum Phase : int {
    PHASE_BOOT_INIT = 0,
    PHASE_INTRO_MOVIES,
    PHASE_MEMCARD_CHECK,
    PHASE_TRK_LOAD,
    PHASE_CAR_LOAD,
    PHASE_COUNT
};

inline uint64_t now_ns() {
    using Clock = std::chrono::steady_clock;
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
}

inline bool enabled() {
    static const bool on = [] {
        const char *v = std::getenv("EOD_PROFILE");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return on;
}

struct State {
    uint64_t process_start_ns = 0;
    std::atomic<uint64_t> phase_ns[PHASE_COUNT]{};
    std::atomic<uint64_t> phase_calls[PHASE_COUNT]{};
    std::atomic<uint64_t> trk_bytes{0};
    std::atomic<uint64_t> car_bytes{0};
    std::atomic<uint32_t> movies_skipped{0};
    std::atomic<uint32_t> movies_played{0};
    std::atomic<uint64_t> boot_init_done_ns{0};
    std::atomic<uint64_t> main_menu_ns{0};
    std::atomic<uint64_t> first_gameplay_frame_ns{0};
    std::atomic<uint64_t> first_gameplay_unfade_ns{0};
    std::atomic<uint64_t> last_reported_tag{0};
};

inline State &state() {
    static State s;
    return s;
}

inline void init() {
    State &s = state();
    if (!s.process_start_ns) s.process_start_ns = now_ns();
}

inline double elapsed_ms_since_start(uint64_t t_ns) {
    const uint64_t start = state().process_start_ns;
    if (!start || t_ns < start) return 0.0;
    return double(t_ns - start) / 1e6;
}

inline void add_phase_ns(Phase p, uint64_t delta_ns) {
    if (!enabled() || p < 0 || p >= PHASE_COUNT) return;
    state().phase_ns[p].fetch_add(delta_ns, std::memory_order_relaxed);
    state().phase_calls[p].fetch_add(1, std::memory_order_relaxed);
}

inline void add_io_bytes(Phase p, uint64_t bytes) {
    if (!enabled()) return;
    if (p == PHASE_TRK_LOAD) state().trk_bytes.fetch_add(bytes, std::memory_order_relaxed);
    else if (p == PHASE_CAR_LOAD) state().car_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

inline void record_movie(bool skipped, uint64_t delta_ns) {
    if (!enabled()) return;
    State &s = state();
    if (skipped) s.movies_skipped.fetch_add(1, std::memory_order_relaxed);
    else s.movies_played.fetch_add(1, std::memory_order_relaxed);
    s.phase_ns[PHASE_INTRO_MOVIES].fetch_add(delta_ns, std::memory_order_relaxed);
    s.phase_calls[PHASE_INTRO_MOVIES].fetch_add(1, std::memory_order_relaxed);
}

inline void mark_boot_init_done() {
    if (!enabled()) return;
    init();
    uint64_t expected = 0;
    const uint64_t t = now_ns();
    if (state().boot_init_done_ns.compare_exchange_strong(expected, t, std::memory_order_relaxed)) {
        state().phase_ns[PHASE_BOOT_INIT].store(t - state().process_start_ns, std::memory_order_relaxed);
        state().phase_calls[PHASE_BOOT_INIT].store(1, std::memory_order_relaxed);
    }
}

inline void report(const char *reason, bool force = false) {
    if (!enabled()) return;
    init();
    State &s = state();
    const uint64_t now = now_ns();
    const uint64_t tag = s.main_menu_ns.load(std::memory_order_relaxed) ^
                         (s.first_gameplay_frame_ns.load(std::memory_order_relaxed) << 1) ^
                         (s.first_gameplay_unfade_ns.load(std::memory_order_relaxed) << 2) ^
                         s.trk_bytes.load(std::memory_order_relaxed) ^
                         s.car_bytes.load(std::memory_order_relaxed);
    if (!force && tag == s.last_reported_tag.load(std::memory_order_relaxed)) return;
    s.last_reported_tag.store(tag, std::memory_order_relaxed);

    const double boot_ms = double(s.phase_ns[PHASE_BOOT_INIT].load(std::memory_order_relaxed)) / 1e6;
    const double movies_ms = double(s.phase_ns[PHASE_INTRO_MOVIES].load(std::memory_order_relaxed)) / 1e6;
    const double mc_ms = double(s.phase_ns[PHASE_MEMCARD_CHECK].load(std::memory_order_relaxed)) / 1e6;
    const double trk_ms = double(s.phase_ns[PHASE_TRK_LOAD].load(std::memory_order_relaxed)) / 1e6;
    const double car_ms = double(s.phase_ns[PHASE_CAR_LOAD].load(std::memory_order_relaxed)) / 1e6;
    const uint64_t mm_ns = s.main_menu_ns.load(std::memory_order_relaxed);
    const uint64_t fg_ns = s.first_gameplay_frame_ns.load(std::memory_order_relaxed);
    const uint64_t fg_uf_ns = s.first_gameplay_unfade_ns.load(std::memory_order_relaxed);

    std::fprintf(stderr,
        "[EOD_PROFILE] (%s) wall=%.1f ms | boot_init=%.1f ms | intro_movies=%.1f ms (played=%u skipped=%u) | "
        "memcard_check=%.1f ms (%llu calls) | trk_load=%.1f ms (%llu KB, %llu reads) | "
        "car_load=%.1f ms (%llu KB, %llu reads)",
        reason ? reason : "summary",
        elapsed_ms_since_start(now),
        boot_ms,
        movies_ms,
        s.movies_played.load(std::memory_order_relaxed),
        s.movies_skipped.load(std::memory_order_relaxed),
        mc_ms,
        (unsigned long long)s.phase_calls[PHASE_MEMCARD_CHECK].load(std::memory_order_relaxed),
        trk_ms,
        (unsigned long long)(s.trk_bytes.load(std::memory_order_relaxed) / 1024),
        (unsigned long long)s.phase_calls[PHASE_TRK_LOAD].load(std::memory_order_relaxed),
        car_ms,
        (unsigned long long)(s.car_bytes.load(std::memory_order_relaxed) / 1024),
        (unsigned long long)s.phase_calls[PHASE_CAR_LOAD].load(std::memory_order_relaxed));
    if (mm_ns) std::fprintf(stderr, " | main_menu=%.1f ms", elapsed_ms_since_start(mm_ns));
    if (fg_ns) std::fprintf(stderr, " | first_gameplay_frame=%.1f ms", elapsed_ms_since_start(fg_ns));
    if (fg_uf_ns) std::fprintf(stderr, " | first_gameplay_unfade=%.1f ms", elapsed_ms_since_start(fg_uf_ns));
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

inline void mark_main_menu() {
    if (!enabled()) return;
    init();
    uint64_t expected = 0;
    const uint64_t t = now_ns();
    if (state().main_menu_ns.compare_exchange_strong(expected, t, std::memory_order_relaxed)) {
        report("main_menu", true);
    }
}

inline void mark_first_gameplay_frame(bool unfaded) {
    if (!enabled()) return;
    init();
    const uint64_t t = now_ns();
    uint64_t expected = 0;
    if (state().first_gameplay_frame_ns.compare_exchange_strong(expected, t, std::memory_order_relaxed)) {
        report("first_gameplay_frame", true);
    }
    if (unfaded) {
        uint64_t exp_uf = 0;
        if (state().first_gameplay_unfade_ns.compare_exchange_strong(exp_uf, t, std::memory_order_relaxed)) {
            report("first_gameplay_unfade", true);
        }
    }
}

class PhaseScope {
public:
    explicit PhaseScope(Phase p) : phase_(p), active_(enabled()), start_ns_(active_ ? now_ns() : 0) {}
    ~PhaseScope() {
        if (active_) add_phase_ns(phase_, now_ns() - start_ns_);
    }
    PhaseScope(const PhaseScope &) = delete;
    PhaseScope &operator=(const PhaseScope &) = delete;

private:
    Phase phase_;
    bool active_;
    uint64_t start_ns_;
};

inline bool has_suffix_ci(const char *path, const char *ext) {
    if (!path || !ext) return false;
    const size_t plen = std::strlen(path);
    const size_t elen = std::strlen(ext);
    if (plen < elen) return false;
    const char *p = path + (plen - elen);
    for (size_t i = 0; i < elen; ++i) {
        char a = p[i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

}  // namespace eod_profile
