// Pure-C++ Plume S5 memory, shadow-target, scene-capture and VRAM hash-memoization budgets.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace native {

// ---- Adaptive LRU Texture Cache Budget (replaces legacy 256 MB cap) -------------------------
inline constexpr uint64_t kDefaultTexBudgetMb = 1024ull;
inline constexpr uint64_t kMinTexBudgetMb = 256ull;
inline constexpr uint64_t kMaxTexBudgetMb = 16384ull;
inline constexpr uint64_t kTexEvictStaleVblanks = 120ull;
inline constexpr uint32_t kMaxTexEvictionsPerFrame = 16u;

inline uint64_t compute_adaptive_tex_budget_bytes(const char *env_override = nullptr) {
    uint64_t mb = kDefaultTexBudgetMb;
    if (env_override && env_override[0] != '\0') {
        char *end = nullptr;
        const unsigned long long parsed = std::strtoull(env_override, &end, 10);
        if (end != env_override && parsed > 0) {
            mb = std::clamp<uint64_t>(static_cast<uint64_t>(parsed), kMinTexBudgetMb, kMaxTexBudgetMb);
        }
    }
    return mb << 20;
}

// ---- Decoupled Shadow Accumulation Target Budget (4K / 6x / 8x) -----------------------------
inline constexpr uint32_t kDefaultMaxShadowScale = 3u;
inline constexpr uint32_t kMinShadowScale = 1u;
inline constexpr uint32_t kMaxShadowScale = 8u;

struct ShadowTargetExtent {
    uint32_t eff_rt_w = 0;      // logical shadow RT width encoded into shader geo (<= rt_w)
    uint32_t eff_rt_h = 0;      // logical shadow RT height encoded into shader geo (<= rt_h)
    uint32_t tex_w = 0;         // GPU target width = eff_rt_w * render_scale
    uint32_t tex_h = 0;         // GPU target height = eff_rt_h * render_scale
    uint32_t shadow_scale = 1;  // effective scale = min(render_scale, max_shadow_scale)
};

inline uint32_t resolve_max_shadow_scale(const char *env_override = nullptr) {
    uint32_t cap = kDefaultMaxShadowScale;
    if (env_override && env_override[0] != '\0') {
        char *end = nullptr;
        const unsigned long parsed = std::strtoul(env_override, &end, 10);
        if (end != env_override && parsed > 0) {
            cap = std::clamp<uint32_t>(static_cast<uint32_t>(parsed), kMinShadowScale, kMaxShadowScale);
        }
    }
    return cap;
}

// Preserves 100% pixel-identical extents for render_scale 1..max_shadow_scale (P6/Classic parity),
// and decouples high-scale (4x/6x/8x) shadow accumulation targets so shadow_ps maps main depth/color
// at render_scale cleanly onto a bounded shadow mask without subpixel drift or 11.8M-px blur waste.
inline ShadowTargetExtent compute_shadow_target_extent(
    uint32_t rt_w, uint32_t rt_h, uint32_t render_scale,
    uint32_t max_shadow_scale = kDefaultMaxShadowScale) {
    const uint32_t sc = std::max(1u, render_scale);
    const uint32_t cap = std::clamp(max_shadow_scale, kMinShadowScale, kMaxShadowScale);
    const uint32_t eff_scale = std::min(sc, cap);
    ShadowTargetExtent out{};
    out.shadow_scale = eff_scale;
    if (rt_w == 0 || rt_h == 0) return out;
    if (sc <= cap) {
        out.eff_rt_w = rt_w;
        out.eff_rt_h = rt_h;
        out.tex_w = rt_w * sc;
        out.tex_h = rt_h * sc;
        return out;
    }
    out.eff_rt_w = std::max(1u, (rt_w * eff_scale + sc / 2u) / sc);
    out.eff_rt_h = std::max(1u, (rt_h * eff_scale + sc / 2u) / sc);
    out.tex_w = out.eff_rt_w * sc;
    out.tex_h = out.eff_rt_h * sc;
    return out;
}

// ---- Bounded Sun Cascade & Scene-Capture Budgets --------------------------------------------
inline constexpr uint32_t kDefaultSunCascadeRes = 2048u;
inline constexpr uint32_t kMinSunCascadeRes = 512u;
inline constexpr uint32_t kMaxSunCascadeRes = 2048u;

inline uint32_t compute_sun_cascade_resolution(const char *env_override = nullptr,
                                               uint32_t device_max_dim = kMaxSunCascadeRes) {
    uint32_t res = kDefaultSunCascadeRes;
    if (env_override && env_override[0] != '\0') {
        char *end = nullptr;
        const unsigned long parsed = std::strtoul(env_override, &end, 10);
        if (end != env_override && parsed > 0) {
            res = std::clamp<uint32_t>(static_cast<uint32_t>(parsed), kMinSunCascadeRes, kMaxSunCascadeRes);
        }
    }
    if (device_max_dim >= kMinSunCascadeRes) {
        res = std::min(res, device_max_dim);
    }
    // Round down to nearest power of two for stable cascade texel snapping.
    uint32_t pow2 = kMinSunCascadeRes;
    while ((pow2 << 1) <= res) pow2 <<= 1;
    return pow2;
}

struct SceneCaptureExtent {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t divisor = 1;
};

inline constexpr uint32_t kDefaultMaxSceneCaptureDim = 1024u;

inline SceneCaptureExtent compute_scene_capture_extent(
    uint32_t view_w, uint32_t view_h, uint32_t render_scale, bool can_blit,
    uint32_t max_capture_dim = kDefaultMaxSceneCaptureDim) {
    SceneCaptureExtent out{};
    if (view_w == 0 || view_h == 0) return out;
    const uint32_t sc = std::max(1u, render_scale);
    uint32_t divisor = can_blit ? std::max(1u, (sc + 1u) / 2u) : 1u;
    if (can_blit && sc > kDefaultMaxShadowScale && max_capture_dim > 0) {
        while ((view_w + divisor - 1u) / divisor > max_capture_dim ||
               (view_h + divisor - 1u) / divisor > max_capture_dim) {
            ++divisor;
        }
    }
    out.divisor = divisor;
    out.width = std::max(1u, (view_w + divisor - 1u) / divisor);
    out.height = std::max(1u, (view_h + divisor - 1u) / divisor);
    return out;
}

// ---- O(1) Frame/VRAM-Generation Texture-Hash Memoization Table ------------------------------
struct VramTexMemoKey {
    uint32_t tbp = 0;
    uint32_t tbw = 0;
    uint32_t psm = 0;
    uint32_t tw = 0;
    uint32_t th = 0;
    uint64_t cbp = 0;             // CLUT base or palette content hash
    uint32_t cpsm = 0;            // CLUT format + TEXA/levels flags
    uint32_t vram_write_gen = 0;  // GS VRAM write clock (gs::g_vram_clock)
    uint64_t mip_hash = 0;        // 0 when levels == 0; hash of mip bases/pitches otherwise

    bool operator==(const VramTexMemoKey &o) const {
        return tbp == o.tbp && tbw == o.tbw && psm == o.psm && tw == o.tw && th == o.th &&
               cbp == o.cbp && cpsm == o.cpsm && vram_write_gen == o.vram_write_gen &&
               mip_hash == o.mip_hash;
    }
};

class VramTexMemoCache {
public:
    static constexpr size_t kBuckets = 2048;
    static constexpr size_t kWays = 2;

    struct Entry {
        VramTexMemoKey key{};
        uint64_t texture_key = 0;
        void *user_ptr = nullptr;
        uint32_t epoch = 0;
    };

    const Entry *find(const VramTexMemoKey &key) {
        if (key.vram_write_gen < last_write_gen_) {
            clear();
        }
        return find_direct(key);
    }

    // Lookup without monotonic-clock clearing (for per-texture max page_gen keys).
    const Entry *find_direct(const VramTexMemoKey &key) {
        const size_t bucket = hash_key(key) & (kBuckets - 1);
        const auto &ways = buckets_[bucket];
        for (size_t w = 0; w < kWays; ++w) {
            if (ways[w].epoch == epoch_ && ways[w].key == key) {
                ++hits_;
                return &ways[w];
            }
        }
        ++misses_;
        return nullptr;
    }

    void insert(const VramTexMemoKey &key, uint64_t texture_key, void *user_ptr = nullptr) {
        if (key.vram_write_gen < last_write_gen_) {
            clear();
        }
        last_write_gen_ = key.vram_write_gen;
        insert_direct(key, texture_key, user_ptr);
    }

    void insert_direct(const VramTexMemoKey &key, uint64_t texture_key, void *user_ptr = nullptr) {
        const size_t bucket = hash_key(key) & (kBuckets - 1);
        auto &ways = buckets_[bucket];

        // 1. Update matching key in place.
        for (size_t w = 0; w < kWays; ++w) {
            if (ways[w].epoch == epoch_ && ways[w].key == key) {
                ways[w].texture_key = texture_key;
                ways[w].user_ptr = user_ptr;
                return;
            }
        }
        // 2. Claim an empty/expired way or the oldest VRAM-generation way.
        size_t victim = 0;
        bool picked = false;
        for (size_t w = 0; w < kWays; ++w) {
            if (ways[w].epoch != epoch_) {
                victim = w;
                picked = true;
                break;
            }
        }
        if (!picked) {
            if (ways[0].key.vram_write_gen != ways[1].key.vram_write_gen) {
                victim = ways[0].key.vram_write_gen < ways[1].key.vram_write_gen ? 0 : 1;
            } else {
                victim = rr_[bucket] & 1u;
                rr_[bucket] ^= 1u;
            }
        }
        ways[victim] = Entry{key, texture_key, user_ptr, epoch_};
    }

    // O(1) epoch invalidation (zero memset unless epoch wraps).
    void clear() {
        ++epoch_;
        last_write_gen_ = 0;
        if (epoch_ == 0) {
            epoch_ = 1;
            for (auto &b : buckets_) {
                for (auto &e : b) e.epoch = 0;
            }
        }
    }

    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }
    void reset_stats() { hits_ = misses_ = 0; }

private:
    static inline uint64_t mix64(uint64_t x) {
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ull;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBull;
        x ^= x >> 31;
        return x;
    }

    static inline uint64_t hash_key(const VramTexMemoKey &k) {
        uint64_t h = (uint64_t(k.tbp) << 32) | k.tbw;
        h ^= mix64((uint64_t(k.psm) << 32) | (uint64_t(k.tw) << 16) | (k.th & 0xFFFFu));
        h ^= mix64(k.cbp ^ ((uint64_t(k.cpsm) << 32) | k.vram_write_gen));
        if (k.mip_hash) h ^= mix64(k.mip_hash);
        return mix64(h);
    }

    std::array<std::array<Entry, kWays>, kBuckets> buckets_{};
    std::array<uint8_t, kBuckets> rr_{};
    uint32_t epoch_ = 1;
    uint32_t last_write_gen_ = 0;
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;
};

}  // namespace native
