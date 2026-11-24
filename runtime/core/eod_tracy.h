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
#include <mutex>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach_init.h>
#include <mach/task.h>
#include <mach/task_info.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// The Win64 unwind metadata names a bitfield R. Some EE translation units
// have already defined R as the emulated register bank.
#pragma push_macro("R")
#undef R
#include <windows.h>
#include <psapi.h>
#pragma pop_macro("R")
#elif defined(__linux__)
#include <unistd.h>
#endif

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

inline const char *trace_out_path() {
    static const char *path = []() -> const char * {
        const char *v = std::getenv("EOD_TRACE_OUT");
        return (v && v[0] != '\0') ? v : nullptr;
    }();
    return path;
}

inline bool trace_enabled() {
    return trace_out_path() != nullptr;
}

inline bool metrics_enabled() {
    return enabled() || trace_enabled();
}

inline double current_rss_mb() {
#if defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return double(info.resident_size) / (1024.0 * 1024.0);
    }
    return 0.0;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return double(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    }
    return 0.0;
#elif defined(__linux__)
    FILE *f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0.0;
    long size_pages = 0;
    long rss_pages = 0;
    const int n = std::fscanf(f, "%ld %ld", &size_pages, &rss_pages);
    std::fclose(f);
    if (n == 2 && rss_pages > 0) {
        const long page_sz = sysconf(_SC_PAGESIZE);
        if (page_sz > 0) {
            return (double(rss_pages) * double(page_sz)) / (1024.0 * 1024.0);
        }
    }
    return 0.0;
#else
    return 0.0;
#endif
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
    if (!metrics_enabled() || p < 0 || p >= PHASE_COUNT) return;
    state().phase_ns[p].fetch_add(delta_ns, std::memory_order_relaxed);
    state().phase_calls[p].fetch_add(1, std::memory_order_relaxed);
}

inline void add_io_bytes(Phase p, uint64_t bytes) {
    if (!metrics_enabled()) return;
    if (p == PHASE_TRK_LOAD) state().trk_bytes.fetch_add(bytes, std::memory_order_relaxed);
    else if (p == PHASE_CAR_LOAD) state().car_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

inline void record_movie(bool skipped, uint64_t delta_ns) {
    if (!metrics_enabled()) return;
    State &s = state();
    if (skipped) s.movies_skipped.fetch_add(1, std::memory_order_relaxed);
    else s.movies_played.fetch_add(1, std::memory_order_relaxed);
    s.phase_ns[PHASE_INTRO_MOVIES].fetch_add(delta_ns, std::memory_order_relaxed);
    s.phase_calls[PHASE_INTRO_MOVIES].fetch_add(1, std::memory_order_relaxed);
}

inline void mark_boot_init_done() {
    if (!metrics_enabled()) return;
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
    if (!metrics_enabled()) return;
    init();
    uint64_t expected = 0;
    const uint64_t t = now_ns();
    if (state().main_menu_ns.compare_exchange_strong(expected, t, std::memory_order_relaxed)) {
        report("main_menu", true);
    }
}

inline void mark_first_gameplay_frame(bool unfaded) {
    if (!metrics_enabled()) return;
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
    explicit PhaseScope(Phase p) : phase_(p), active_(metrics_enabled()), start_ns_(active_ ? now_ns() : 0) {}
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

struct TraceEvent {
    const char *name;
    double ts_us;
    double dur_us;
    double rss_mb;
    uint32_t tid;
};

struct TraceBuffer {
    static constexpr size_t kMaxEvents = 65536;
    std::mutex mu;
    std::vector<TraceEvent> ring;
    size_t write_idx = 0;
    uint64_t total_recorded = 0;
};

inline TraceBuffer &trace_buffer() {
    static TraceBuffer buf;
    return buf;
}

inline uint32_t current_tid() {
    static std::atomic<uint32_t> next_id{1};
    thread_local uint32_t tid = next_id.fetch_add(1, std::memory_order_relaxed);
    return tid;
}

inline void record_trace_zone(const char *name, double ts_us, double dur_us, double rss_mb) {
    if (!trace_enabled()) return;
    TraceBuffer &tb = trace_buffer();
    std::lock_guard<std::mutex> lock(tb.mu);
    if (tb.ring.size() < TraceBuffer::kMaxEvents) {
        tb.ring.push_back(TraceEvent{name ? name : "zone", ts_us, dur_us, rss_mb, current_tid()});
    } else {
        tb.ring[tb.write_idx % TraceBuffer::kMaxEvents] =
            TraceEvent{name ? name : "zone", ts_us, dur_us, rss_mb, current_tid()};
    }
    tb.write_idx++;
    tb.total_recorded++;
}

class ScopedTraceZone {
public:
    explicit ScopedTraceZone(const char *name)
        : name_(name), active_(trace_enabled()), start_ns_(0) {
        if (active_) {
            init();
            start_ns_ = now_ns();
        }
    }
    ~ScopedTraceZone() {
        if (active_) {
            const uint64_t end_ns = now_ns();
            const uint64_t base = state().process_start_ns;
            const double ts_us = (start_ns_ >= base) ? double(start_ns_ - base) / 1000.0 : 0.0;
            const double dur_us = (end_ns >= start_ns_) ? double(end_ns - start_ns_) / 1000.0 : 0.0;
            record_trace_zone(name_, ts_us, dur_us, current_rss_mb());
        }
    }
    ScopedTraceZone(const ScopedTraceZone &) = delete;
    ScopedTraceZone &operator=(const ScopedTraceZone &) = delete;

private:
    const char *name_;
    bool active_;
    uint64_t start_ns_;
};

inline void write_json_escaped(FILE *f, const char *s) {
    std::fputc('"', f);
    if (s) {
        for (const unsigned char *p = reinterpret_cast<const unsigned char *>(s); *p; ++p) {
            const unsigned char c = *p;
            if (c == '"' || c == '\\') {
                std::fputc('\\', f);
                std::fputc(char(c), f);
            } else if (c == '\n') {
                std::fputs("\\n", f);
            } else if (c == '\r') {
                std::fputs("\\r", f);
            } else if (c == '\t') {
                std::fputs("\\t", f);
            } else if (c < 0x20) {
                std::fprintf(f, "\\u%04x", unsigned(c));
            } else {
                std::fputc(char(c), f);
            }
        }
    }
    std::fputc('"', f);
}

inline void flush_chrome_trace() {
    const char *out_path = trace_out_path();
    if (!out_path) return;
    init();

    std::vector<TraceEvent> ordered;
    uint64_t total_recorded = 0;
    {
        TraceBuffer &tb = trace_buffer();
        std::lock_guard<std::mutex> lock(tb.mu);
        total_recorded = tb.total_recorded;
        if (!tb.ring.empty()) {
            ordered.reserve(tb.ring.size());
            if (tb.total_recorded <= TraceBuffer::kMaxEvents) {
                ordered.assign(tb.ring.begin(), tb.ring.end());
            } else {
                const size_t start = tb.write_idx % TraceBuffer::kMaxEvents;
                for (size_t i = 0; i < tb.ring.size(); ++i) {
                    ordered.push_back(tb.ring[(start + i) % TraceBuffer::kMaxEvents]);
                }
            }
        }
    }

    FILE *f = std::fopen(out_path, "wb");
    if (!f) {
        std::fprintf(stderr, "[EOD_TRACE] failed to open trace output: %s\n", out_path);
        return;
    }

    const State &s = state();
    const uint64_t now = now_ns();
    const double now_us = (now >= s.process_start_ns) ? double(now - s.process_start_ns) / 1000.0 : 0.0;
    const double rss_end = current_rss_mb();

    std::fputs("{\"displayTimeUnit\":\"ms\",\"traceEvents\":[\n", f);
    std::fputs("  {\"name\":\"process_name\",\"ph\":\"M\",\"pid\":1,\"tid\":0,\"args\":{\"name\":\"EOD Game Runtime\"}}", f);
    for (const auto &ev : ordered) {
        std::fputs(",\n  {\"name\":", f);
        write_json_escaped(f, ev.name);
        std::fprintf(f,
            ",\"cat\":\"runtime\",\"ph\":\"X\",\"ts\":%.3f,\"dur\":%.3f,\"pid\":1,\"tid\":%u,\"args\":{\"rss_mb\":%.3f}}",
            ev.ts_us, ev.dur_us, ev.tid, ev.rss_mb);
        std::fprintf(f,
            ",\n  {\"name\":\"RSS_MB\",\"cat\":\"memory\",\"ph\":\"C\",\"ts\":%.3f,\"pid\":1,\"tid\":0,\"args\":{\"rss_mb\":%.3f}}",
            ev.ts_us + ev.dur_us, ev.rss_mb);
    }
    std::fprintf(f,
        ",\n  {\"name\":\"RSS_MB\",\"cat\":\"memory\",\"ph\":\"C\",\"ts\":%.3f,\"pid\":1,\"tid\":0,\"args\":{\"rss_mb\":%.3f}}\n",
        now_us, rss_end);

    const double boot_ms = double(s.phase_ns[PHASE_BOOT_INIT].load(std::memory_order_relaxed)) / 1e6;
    const double movies_ms = double(s.phase_ns[PHASE_INTRO_MOVIES].load(std::memory_order_relaxed)) / 1e6;
    const double mc_ms = double(s.phase_ns[PHASE_MEMCARD_CHECK].load(std::memory_order_relaxed)) / 1e6;
    const double trk_ms = double(s.phase_ns[PHASE_TRK_LOAD].load(std::memory_order_relaxed)) / 1e6;
    const double car_ms = double(s.phase_ns[PHASE_CAR_LOAD].load(std::memory_order_relaxed)) / 1e6;
    const uint64_t mm_ns = s.main_menu_ns.load(std::memory_order_relaxed);
    const uint64_t fg_ns = s.first_gameplay_frame_ns.load(std::memory_order_relaxed);
    const uint64_t fg_uf_ns = s.first_gameplay_unfade_ns.load(std::memory_order_relaxed);

    std::fprintf(f,
        "],\"otherData\":{\"app\":\"EOD Game Runtime\",\"wall_ms\":%.3f,\"rss_mb_end\":%.3f,"
        "\"zone_events_recorded\":%llu,\"phaseSummary\":{"
        "\"boot_init_ms\":%.3f,\"intro_movies_ms\":%.3f,\"memcard_check_ms\":%.3f,"
        "\"trk_load_ms\":%.3f,\"trk_kb\":%llu,\"car_load_ms\":%.3f,\"car_kb\":%llu,"
        "\"main_menu_ms\":%.3f,\"first_gameplay_frame_ms\":%.3f,\"first_gameplay_unfade_ms\":%.3f}}}\n",
        elapsed_ms_since_start(now),
        rss_end,
        (unsigned long long)total_recorded,
        boot_ms,
        movies_ms,
        mc_ms,
        trk_ms,
        (unsigned long long)(s.trk_bytes.load(std::memory_order_relaxed) / 1024),
        car_ms,
        (unsigned long long)(s.car_bytes.load(std::memory_order_relaxed) / 1024),
        mm_ns ? elapsed_ms_since_start(mm_ns) : 0.0,
        fg_ns ? elapsed_ms_since_start(fg_ns) : 0.0,
        fg_uf_ns ? elapsed_ms_since_start(fg_uf_ns) : 0.0);
    std::fclose(f);
}

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

#define EOD_TRACE_CAT2(a, b) a##b
#define EOD_TRACE_CAT(a, b) EOD_TRACE_CAT2(a, b)
#ifdef EOD_TRACY
#define EOD_TRACE_ZONE(name)                                                         \
    ZoneNamedN(EOD_TRACE_CAT(eod_tracy_zone_, __LINE__), name, true);                \
    ::eod_profile::ScopedTraceZone EOD_TRACE_CAT(eod_trace_zone_, __LINE__)(name)
#else
#define EOD_TRACE_ZONE(name) \
    ::eod_profile::ScopedTraceZone EOD_TRACE_CAT(eod_trace_zone_, __LINE__)(name)
#endif
