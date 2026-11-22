// nascar_class_test.cpp - Unit tests for standalone NASCAR Class 4 registry,
// unbounded livery/wheel caps, Dirt Tyre Kit grip multiplier, and nascar_engine_audio.h.
#include "hle_nascar_class.h"
#include "nascar_engine_audio.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        std::exit(1);
    }
}

void write_u32(std::vector<unsigned char>& buf, uint32_t v) {
    buf.push_back(static_cast<unsigned char>(v & 0xFFu));
    buf.push_back(static_cast<unsigned char>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<unsigned char>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<unsigned char>((v >> 24) & 0xFFu));
}

void write_f32(std::vector<unsigned char>& buf, float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    write_u32(buf, bits);
}

void test_class_defaults_and_cycling() {
    using namespace eod::nascar;
    auto& reg = VehicleClassRegistry::instance();
    reg.reset_defaults();

    check(reg.class_count() == 5, "default registry must contain 5 classes (0..4)");
    const auto* nascar = reg.find_class(kClassNascar);
    check(nascar != nullptr, "Class 4 (NASCAR) must be registered by default");
    check(nascar->id == 4, "Class 4 id == 4");
    check(nascar->name == "Stock Car (NASCAR)", "Class 4 display name matches specification");
    check(nascar->isolated_ai_pool, "Class 4 must isolate AI opponent pool");
    check((nascar->upgrade_mask & kDirtTyreKitBit) != 0, "Class 4 upgrade mask includes kDirtTyreKitBit");

    // Forward 5-class cycle: 0 -> 1 -> 2 -> 3 -> 4 -> 0
    uint32_t cur = kClassCompact;
    cur = reg.cycle_class(cur, +1); check(cur == kClassMidsize, "cycle 0 -> 1");
    cur = reg.cycle_class(cur, +1); check(cur == kClassMuscle,  "cycle 1 -> 2");
    cur = reg.cycle_class(cur, +1); check(cur == kClassSpecial, "cycle 2 -> 3");
    cur = reg.cycle_class(cur, +1); check(cur == kClassNascar,  "cycle 3 -> 4");
    cur = reg.cycle_class(cur, +1); check(cur == kClassCompact, "cycle 4 -> 0");

    // Reverse 5-class cycle: 0 -> 4 -> 3 -> 2 -> 1 -> 0
    cur = reg.cycle_class(cur, -1); check(cur == kClassNascar,  "reverse cycle 0 -> 4");
    cur = reg.cycle_class(cur, -1); check(cur == kClassSpecial, "reverse cycle 4 -> 3");
    cur = reg.cycle_class(cur, -1); check(cur == kClassMuscle,  "reverse cycle 3 -> 2");
    cur = reg.cycle_class(cur, -1); check(cur == kClassMidsize, "reverse cycle 2 -> 1");
    cur = reg.cycle_class(cur, -1); check(cur == kClassCompact, "reverse cycle 1 -> 0");

    // Arbitrary custom mod class registration + cycling
    check(reg.register_class(VehicleClassDef{5, "Late Model Dirt", (1ull << 40) | kDirtTyreKitBit, true}),
          "custom class 5 registration succeeds");
    check(reg.class_count() == 6, "class count grows to 6");
    check(reg.cycle_class(kClassNascar, +1) == 5, "cycle reaches custom class 5");
    check(reg.cycle_class(5, +1) == kClassCompact, "cycle wraps from custom class 5 back to 0");
}

void test_compatibility_mask_no_mod4_wrap() {
    using namespace eod::nascar;
    auto& reg = VehicleClassRegistry::instance();
    reg.reset_defaults();

    const uint64_t compact_mask = reg.resolve_part_compatibility_mask(kClassCompact);
    const uint64_t nascar_mask  = reg.resolve_part_compatibility_mask(kClassNascar);
    check(nascar_mask != compact_mask, "Class 4 compatibility mask must NOT wrap % 4 to Compact (0)");
    check((nascar_mask & kDirtTyreKitBit) != 0, "Class 4 supports Dirt Tyre Kit bit (1ull << 24)");
    check((compact_mask & kDirtTyreKitBit) == 0, "Compact does not inherit Dirt Tyre Kit bit");

    // Custom 64-bit class with high bit > 31
    constexpr uint64_t kCustomHighBit = (1ull << 48);
    check(reg.register_class(VehicleClassDef{8, "Mod Class 8", kCustomHighBit | kDirtTyreKitBit, true}),
          "register custom class 8 (multiple of 4)");
    const uint64_t class8_mask = reg.resolve_part_compatibility_mask(8);
    check(class8_mask == (kCustomHighBit | kDirtTyreKitBit),
          "Class 8 (8 % 4 == 0) resolves full 64-bit mask without wrapping to Compact");
    check(reg.resolve_part_compatibility_mask(999) == 0ull,
          "unregistered class >= 4 returns 0 mask instead of wrapping % 4");
}

void test_unbounded_livery_pagination() {
    using namespace eod::nascar;
    auto& reg = VehicleClassRegistry::instance();
    reg.reset_defaults();

    constexpr uint32_t kCupChassisId = 104;
    reg.register_chassis_class(kCupChassisId, kClassNascar);
    check(reg.lookup_chassis_class(kCupChassisId, kClassCompact) == kClassNascar,
          "chassis class lookup returns Class 4");
    check(reg.lookup_chassis_class(9999, kClassMuscle) == kClassMuscle,
          "unknown chassis falls back cleanly");

    // Register 45 Cup liveries (breaking the legacy 10-paint PS2 cap)
    for (uint32_t i = 0; i < 45; ++i) {
        LiveryPageEntry e;
        e.vehicle_id = 1000u + i;
        e.driver     = "Cup Driver " + std::to_string(i + 1);
        e.car_number = std::to_string(i + 1);
        e.tex_name   = "cup_car_" + std::to_string(i + 1) + ".tex";
        reg.register_livery(kCupChassisId, std::move(e));
    }

    check(reg.liveries_for_chassis(kCupChassisId).size() == 45,
          "all 45 Cup liveries stored without 10-paint cap");
    check(reg.livery_page_count(kCupChassisId, 10) == 5,
          "45 liveries paginate into 5 pages of 10");

    for (size_t page = 0; page < 4; ++page) {
        const auto slice = reg.livery_page(kCupChassisId, page, 10);
        check(slice.size() == 10, "pages 0..3 contain 10 liveries each");
        check(slice.front().vehicle_id == 1000u + static_cast<uint32_t>(page * 10),
              "page first entry matches expected offset");
        check(slice.back().vehicle_id == 1000u + static_cast<uint32_t>(page * 10 + 9),
              "page last entry matches expected offset");
    }
    const auto last_page = reg.livery_page(kCupChassisId, 4, 10);
    check(last_page.size() == 5, "page 4 contains the remaining 5 liveries");
    check(last_page.front().vehicle_id == 1040u, "page 4 starts at entry 40");
    check(last_page.back().vehicle_id == 1044u, "page 4 ends at entry 44");
    check(reg.livery_page(kCupChassisId, 5, 10).empty(), "out-of-range page returns empty");
}

void test_unbounded_wheel_classes() {
    using namespace eod::nascar;
    auto& reg = VehicleClassRegistry::instance();
    reg.reset_defaults();

    // Verify all 5 pre-registered NASCAR wheel families and fast-mesh resolution
    const char* families[5][3] = {
        {"cup",      "ncu2.mod", "ncu2f.mod"},
        {"truck",    "ntr1.mod", "ntr1f.mod"},
        {"modified", "nmo3.mod", "nmo3f.mod"},
        {"dirt",     "ndt4.mod", "ndt4f.mod"},
        {"pace",     "npc0.mod", "npc0f.mod"},
    };
    for (const auto& f : families) {
        const auto* asset = reg.find_wheel_class(f[0]);
        check(asset != nullptr, "default NASCAR wheel family is pre-registered");
        check(asset->family == f[0], "wheel family name matches");
        check(asset->diameter_m > 0.6f && asset->diameter_m < 0.85f, "wheel diameter is realistic");
        std::string fast_out;
        check(reg.resolve_fast_wheel_mesh(f[1], fast_out), "resolve_fast_wheel_mesh finds default family");
        check(fast_out == f[2], "resolved fast wheel mesh matches expected LOD filename");
    }

    // Register 20 distinct wheel classes (breaking the legacy 8-slot EE cap at 0x21F988)
    for (int i = 0; i < 20; ++i) {
        WheelClassAsset w;
        w.key        = "custom_wheel_" + std::to_string(i);
        w.family     = (i % 2 == 0) ? "dirt" : "cup";
        w.slow_mesh  = "cw" + std::string(1, static_cast<char>('a' + i)) + "0.mod";
        w.fast_mesh  = "cw" + std::string(1, static_cast<char>('a' + i)) + "0f.mod";
        w.diameter_m = 0.70f + 0.005f * static_cast<float>(i);
        reg.register_wheel_class(std::move(w));
    }
    check(reg.wheel_class_count() >= 25, "registry holds 5 defaults + 20 custom wheel classes (>8 cap broken)");

    for (int i = 0; i < 20; ++i) {
        const std::string key = "custom_wheel_" + std::to_string(i);
        const auto* found = reg.find_wheel_class(key);
        check(found != nullptr, "all 20 custom wheel classes remain retrievable");
        const std::string probe = "cw" + std::string(1, static_cast<char>('a' + i)) + "3.mod";
        const std::string expect = "cw" + std::string(1, static_cast<char>('a' + i)) + "3f.mod";
        std::string fast_out;
        check(reg.resolve_fast_wheel_mesh(probe.c_str(), fast_out) && fast_out == expect,
              "custom registered wheel class resolves fast LOD mesh");
    }
}

void test_dirt_tyre_grip_boost() {
    using namespace eod::nascar;
    auto& reg = VehicleClassRegistry::instance();
    reg.reset_defaults();

    constexpr uint32_t kNascarChassis = 200;
    constexpr uint32_t kMuscleChassis = 201;
    reg.register_chassis_class(kNascarChassis, kClassNascar);
    reg.register_chassis_class(kMuscleChassis, kClassMuscle);

    const float boosted = reg.dirt_tyre_grip_multiplier(kNascarChassis, true, true);
    check(std::fabs(boosted - 1.28f) < 1e-5f, "Class 4 NASCAR + Dirt Tyre Kit on dirt gets 1.28x grip");
    check(std::fabs(reg.dirt_tyre_grip_multiplier(kNascarChassis, false, true) - 1.0f) < 1e-5f,
          "Class 4 without Dirt Tyre Kit gets 1.0x grip");
    check(std::fabs(reg.dirt_tyre_grip_multiplier(kNascarChassis, true, false) - 1.0f) < 1e-5f,
          "Class 4 with Dirt Tyre Kit on asphalt gets 1.0x grip");
    check(std::fabs(reg.dirt_tyre_grip_multiplier(kMuscleChassis, true, true) - 1.0f) < 1e-5f,
          "Non-NASCAR class on dirt stays at 1.0x grip");
}

void test_nascar_engine_audio_envelope() {
    // Build a valid 2-record ESFX binary profile and verify parse() + sample() + mix() RPM envelopes.
    std::vector<unsigned char> blob;
    write_u32(blob, 2); // 2 records

    auto append_record = [&](const char* name, float sample_rpm, float g_lo, float g_hi,
                             float o_lo, float i_lo, float i_hi, float o_hi) {
        char slot[16]{};
        std::strncpy(slot, name, sizeof(slot) - 1);
        blob.insert(blob.end(), slot, slot + 16);
        write_f32(blob, sample_rpm);
        write_f32(blob, g_lo);
        write_f32(blob, g_hi);
        write_f32(blob, o_lo);
        write_f32(blob, i_lo);
        write_f32(blob, i_hi);
        write_f32(blob, o_hi);
    };

    append_record("v8_idle.sfx", 1800.f, 0.8f, 1.0f, 500.f, 1000.f, 3000.f, 4000.f);
    append_record("v8_high.sfx", 6000.f, 0.7f, 1.2f, 2500.f, 3500.f, 7500.f, 9000.f);

    nascar_audio::Profile profile;
    check(nascar_audio::parse(blob.data(), blob.size(), profile), "nascar_audio::parse succeeds on valid ESFX profile");
    check(profile.size() == 2, "parsed ESFX profile contains 2 records");

    // RPM envelope sanity on record 1 (high band)
    const auto below = nascar_audio::sample(profile[1], 2000.f, 1.0f);
    check(!below.active && below.volume == 0.f, "below outer_low is inactive");
    const auto ramp = nascar_audio::sample(profile[1], 3000.f, 1.0f);
    check(ramp.active && ramp.volume > 0.f && std::fabs(ramp.pitch - 0.5f) < 1e-4f,
          "mid-ramp voice active with expected pitch (3000/6000)");
    const auto peak = nascar_audio::sample(profile[1], 6000.f, 1.0f);
    check(peak.active && peak.volume > ramp.volume && std::fabs(peak.pitch - 1.0f) < 1e-4f,
          "inner plateau voice louder than ramp with unity pitch at sample_rpm");
    const auto above = nascar_audio::sample(profile[1], 9500.f, 1.0f);
    check(!above.active && above.volume == 0.f, "above outer_high is inactive");

    // Full mix check: local_mix suppresses record 0, keeps record 1 active
    const auto mixed = nascar_audio::mix(profile, 6000.f, 0.9f, 0.5f, true, true, false);
    check(mixed.size() == 2, "mix returns 2 voices");
    check(!mixed[0].active, "local_mix suppresses external idle layer (index 0)");
    check(mixed[1].active && mixed[1].volume > 0.f, "local_mix activates high V8 layer at 6000 RPM");

    const auto muted = nascar_audio::mix(profile, 6000.f, 0.9f, 0.5f, true, true, true);
    check(!muted[0].active && !muted[1].active, "silenced flag mutes all voices");
}

} // namespace

int main() {
    test_class_defaults_and_cycling();
    test_compatibility_mask_no_mod4_wrap();
    test_unbounded_livery_pagination();
    test_unbounded_wheel_classes();
    test_dirt_tyre_grip_boost();
    test_nascar_engine_audio_envelope();
    std::puts("NASCAR Class 4 registry, unbounded caps, Dirt Tyre Kit & ESFX audio tests PASSED");
    return 0;
}
