// Lifecycle counters for the native graphics release gate.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace r7_audit {

enum class Counter {
    Vu1, GifQwords, GsRaster, GsSuppressed, SyncVram, DeferredWait, ClutWait,
    Vu0Mlist, Vu0Micro, Vu0Macro, Vu0Control,
    FrustumTested, FrustumCulled, ChunkVboHits, ChunkVboMisses, InterpPresents, Count
};

inline std::atomic<uint64_t> counts[static_cast<unsigned>(Counter::Count)]{};

inline bool enabled() {
    static const bool on = [] {
        const char *value = std::getenv("EOD_R7_AUDIT");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return on;
}

inline void add(Counter counter, uint64_t amount = 1) {
    if (enabled())
        counts[static_cast<unsigned>(counter)].fetch_add(amount, std::memory_order_relaxed);
}

inline void report() {
    if (!enabled()) return;
    auto get = [](Counter counter) {
        return static_cast<unsigned long long>(
            counts[static_cast<unsigned>(counter)].load(std::memory_order_relaxed));
    };
    std::fprintf(stderr,
                 "[r7-audit] v=2 vu1=%llu gif_qwords=%llu gs_raster=%llu gs_suppressed=%llu "
                 "sync_vram=%llu deferred_wait=%llu clut_wait=%llu vu0_mlist=%llu "
                 "vu0_micro=%llu vu0_macro=%llu vu0_control=%llu\n",
                 get(Counter::Vu1), get(Counter::GifQwords), get(Counter::GsRaster),
                 get(Counter::GsSuppressed), get(Counter::SyncVram), get(Counter::DeferredWait),
                 get(Counter::ClutWait), get(Counter::Vu0Mlist), get(Counter::Vu0Micro),
                 get(Counter::Vu0Macro), get(Counter::Vu0Control));
    std::fprintf(stderr,
                 "[r7_audit] vu1_exec=%llu gs_rast=%llu frustum_culled=%llu/%llu chunk_vbo_hits=%llu\n",
                 get(Counter::Vu1), get(Counter::GsRaster),
                 get(Counter::FrustumCulled), get(Counter::FrustumTested),
                 get(Counter::ChunkVboHits));
}

} // namespace r7_audit
