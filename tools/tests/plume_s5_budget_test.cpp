#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "render/plume_budget.h"
#include "render/sun_cascades.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool cond, const char *expr, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expr);
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

void set_env_var(const char *name, const char *val) {
#ifdef _WIN32
    _putenv_s(name, val ? val : "");
#else
    if (val) setenv(name, val, 1);
    else unsetenv(name);
#endif
}

void test_adaptive_tex_budget() {
    // Default must lift legacy 256 MB cap to >= 1,024 MB.
    const uint64_t def_bytes = native::compute_adaptive_tex_budget_bytes(nullptr);
    CHECK(def_bytes >= (1024ull << 20));
    CHECK(def_bytes == (native::kDefaultTexBudgetMb << 20));
    CHECK(native::compute_adaptive_tex_budget_bytes("") == def_bytes);
    CHECK(native::compute_adaptive_tex_budget_bytes("not-a-number") == def_bytes);

    // Direct overrides & clamping.
    CHECK(native::compute_adaptive_tex_budget_bytes("2048") == (2048ull << 20));
    CHECK(native::compute_adaptive_tex_budget_bytes("512") == (512ull << 20));
    CHECK(native::compute_adaptive_tex_budget_bytes("64") == (256ull << 20));
    CHECK(native::compute_adaptive_tex_budget_bytes("99999") == (native::kMaxTexBudgetMb << 20));

    // Environment variable integration.
    set_env_var("EOD_TEX_CACHE_MB", "1536");
    CHECK(native::compute_adaptive_tex_budget_bytes(std::getenv("EOD_TEX_CACHE_MB")) == (1536ull << 20));
    set_env_var("EOD_TEX_CACHE_MB", nullptr);
    CHECK(native::compute_adaptive_tex_budget_bytes(std::getenv("EOD_TEX_CACHE_MB")) == (1024ull << 20));

    // Bounded incremental eviction invariants.
    CHECK(native::kTexEvictStaleVblanks == 120ull);
    CHECK(native::kMaxTexEvictionsPerFrame == 16u);
}

void test_shadow_target_decoupling() {
    // 1x..3x must remain 100% pixel-identical (Classic / P6 parity).
    for (uint32_t sc = 1; sc <= 3; ++sc) {
        const auto ext = native::compute_shadow_target_extent(320, 512, sc);
        CHECK(ext.shadow_scale == sc);
        CHECK(ext.eff_rt_w == 320u);
        CHECK(ext.eff_rt_h == 512u);
        CHECK(ext.tex_w == 320u * sc);
        CHECK(ext.tex_h == 512u * sc);
    }

    // 6x standard 320x512 shadow RT: uncapped 1920x3072 -> capped 960x1536 (exact 4.0x reduction).
    const auto ext6_std = native::compute_shadow_target_extent(320, 512, 6);
    CHECK(ext6_std.shadow_scale == 3u);
    CHECK(ext6_std.eff_rt_w == 160u);
    CHECK(ext6_std.eff_rt_h == 256u);
    CHECK(ext6_std.tex_w == ext6_std.eff_rt_w * 6u);
    CHECK(ext6_std.tex_h == ext6_std.eff_rt_h * 6u);
    CHECK(ext6_std.tex_w == 960u && ext6_std.tex_h == 1536u);
    const uint64_t uncapped6_std = (320ull * 6ull) * (512ull * 6ull);
    const uint64_t capped6_std = uint64_t(ext6_std.tex_w) * ext6_std.tex_h;
    CHECK(uncapped6_std >= capped6_std * 4ull);

    // 6x full 640x512 target: uncapped 3840x3072 (11.8M px) -> capped 1920x1536 (2.95M px, 4.0x cut).
    const auto ext6_full = native::compute_shadow_target_extent(640, 512, 6);
    CHECK(ext6_full.tex_w == 1920u && ext6_full.tex_h == 1536u);
    CHECK(ext6_full.tex_w == ext6_full.eff_rt_w * 6u);
    CHECK(ext6_full.tex_h == ext6_full.eff_rt_h * 6u);
    const uint64_t uncapped6_full = 3840ull * 3072ull;
    const uint64_t capped6_full = uint64_t(ext6_full.tex_w) * ext6_full.tex_h;
    CHECK(uncapped6_full >= capped6_full * 4ull);

    // 8x: uncapped 2560x4096 (10.5M px) -> capped 960x1536 (1.47M px, > 7.1x cut).
    const auto ext8 = native::compute_shadow_target_extent(320, 512, 8);
    CHECK(ext8.shadow_scale == 3u);
    CHECK(ext8.tex_w == ext8.eff_rt_w * 8u);
    CHECK(ext8.tex_h == ext8.eff_rt_h * 8u);
    CHECK(ext8.tex_w == 960u && ext8.tex_h == 1536u);
    const uint64_t uncapped8 = (320ull * 8ull) * (512ull * 8ull);
    const uint64_t capped8 = uint64_t(ext8.tex_w) * ext8.tex_h;
    CHECK(uncapped8 >= capped8 * 7ull);

    // EOD_SHADOW_SCALE override.
    set_env_var("EOD_SHADOW_SCALE", "2");
    const uint32_t cap2 = native::resolve_max_shadow_scale(std::getenv("EOD_SHADOW_SCALE"));
    CHECK(cap2 == 2u);
    const auto ext6_cap2 = native::compute_shadow_target_extent(320, 512, 6, cap2);
    CHECK(ext6_cap2.shadow_scale == 2u);
    CHECK(uint64_t(ext6_cap2.tex_w) * ext6_cap2.tex_h * 8ull <= uncapped6_std);
    set_env_var("EOD_SHADOW_SCALE", nullptr);
    CHECK(native::resolve_max_shadow_scale(std::getenv("EOD_SHADOW_SCALE")) == native::kDefaultMaxShadowScale);
}

void test_sun_and_scene_capture_budgets() {
    CHECK(native::compute_sun_cascade_resolution(nullptr) == 2048u);
    CHECK(native::compute_sun_cascade_resolution("1024") == 1024u);
    CHECK(native::compute_sun_cascade_resolution("4096") == 2048u);
    CHECK(native::compute_sun_cascade_resolution("768") == 512u);
    CHECK(native::compute_sun_cascade_resolution(nullptr, 1024u) == 1024u);

    // Ensure sun_cascades::make_view accepts custom bounded resolution cleanly.
    const auto view = native::sun_cascades::make_view({0.0, 2.0, 0.0}, {0.0, 0.0, 1.0}, {0.3, -0.9, 0.2}, 1024u);
    CHECK(view.box[0].radius == 24.0 && view.box[1].radius == 120.0);

    // Scene capture parity for 1x..3x and bounding for 6x/8x + ultrawide.
    const auto c1 = native::compute_scene_capture_extent(640, 448, 1, true);
    CHECK(c1.width == 640u && c1.height == 448u && c1.divisor == 1u);
    const auto c2 = native::compute_scene_capture_extent(1280, 896, 2, true);
    CHECK(c2.width == 1280u && c2.height == 896u && c2.divisor == 1u);
    const auto c3 = native::compute_scene_capture_extent(1920, 1344, 3, true);
    CHECK(c3.width == 960u && c3.height == 672u && c3.divisor == 2u);
    const auto c6 = native::compute_scene_capture_extent(3840, 2688, 6, true);
    CHECK(c6.width <= 1920u && c6.height <= 1920u);
    const auto c8_wide = native::compute_scene_capture_extent(7680, 4320, 8, true);
    CHECK(c8_wide.width <= 1920u && c8_wide.height <= 1920u);
}

void test_vram_tex_memo_cache() {
    native::VramTexMemoCache cache;
    int sentinel_a = 1;
    int sentinel_b = 2;

    const native::VramTexMemoKey k1{0x2BC4u, 2u, 0x13u, 128u, 128u, 0xABCD1234567890EFULL, 0x00120001u, 42u, 0ull};
    CHECK(cache.find(k1) == nullptr);
    CHECK(cache.misses() == 1u && cache.hits() == 0u);

    cache.insert(k1, 0xDEADBEEFCAFEBABEULL, &sentinel_a);
    const auto *hit1 = cache.find(k1);
    CHECK(hit1 != nullptr);
    CHECK(hit1->texture_key == 0xDEADBEEFCAFEBABEULL);
    CHECK(hit1->user_ptr == &sentinel_a);
    CHECK(cache.hits() == 1u && cache.misses() == 1u);

    // Repeated lookups in same generation keep hitting O(1).
    for (int i = 0; i < 100; ++i) {
        const auto *h = cache.find(k1);
        CHECK(h != nullptr && h->texture_key == 0xDEADBEEFCAFEBABEULL && h->user_ptr == &sentinel_a);
    }
    CHECK(cache.hits() == 101u);

    // VRAM write generation bump MUST immediately invalidate!
    native::VramTexMemoKey k1_gen43 = k1;
    k1_gen43.vram_write_gen = 43u;
    CHECK(cache.find(k1_gen43) == nullptr);

    // Re-inserting under gen 43 replaces older gen smoothly.
    cache.insert(k1_gen43, 0x1122334455667788ULL, &sentinel_b);
    const auto *hit2 = cache.find(k1_gen43);
    CHECK(hit2 != nullptr && hit2->texture_key == 0x1122334455667788ULL && hit2->user_ptr == &sentinel_b);

    // Different CLUT/palette or mip hash must never collide.
    native::VramTexMemoKey k1_diff_clut = k1_gen43;
    k1_diff_clut.cbp ^= 1ull;
    CHECK(cache.find(k1_diff_clut) == nullptr);

    native::VramTexMemoKey k1_diff_mip = k1_gen43;
    k1_diff_mip.mip_hash = 0x9999ull;
    CHECK(cache.find(k1_diff_mip) == nullptr);

    // O(1) epoch clear invalidates immediately.
    cache.clear();
    CHECK(cache.find(k1_gen43) == nullptr);

    // Throughput microbenchmark: 100k lookups in same generation.
    cache.reset_stats();
    cache.insert(k1_gen43, 0x1122334455667788ULL, &sentinel_b);
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t accum = 0;
    for (int i = 0; i < 100000; ++i) {
        const auto *e = cache.find(k1_gen43);
        accum += e ? e->texture_key : 0ull;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns_per_lookup =
        std::chrono::duration<double, std::nano>(t1 - t0).count() / 100000.0;
    CHECK(accum == 100000ull * 0x1122334455667788ULL);
    CHECK(cache.hits() == 100000u && cache.misses() == 0u);
    std::printf("[plume_s5_budget_test] VramTexMemoCache O(1) lookup: %.2f ns/op (100k hits)\n", ns_per_lookup);
}

}  // namespace

int main() {
    test_adaptive_tex_budget();
    test_shadow_target_decoupling();
    test_sun_and_scene_capture_budgets();
    test_vram_tex_memo_cache();
    std::printf("[plume_s5_budget_test] %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
