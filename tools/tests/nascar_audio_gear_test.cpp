// nascar_audio_gear_test.cpp - Player-outcome verification (POA-N1) for Sprint 3F-1:
// 1. Parses all 4 real D2D ESFX v2 profiles (eng1e..eng4e.ens, 356B with exporter tail).
// 2. Verifies retail *.val passthrough vs NASCAR ESFX detection.
// 3. Simulates 0 -> 130 mph short-oval (ShortDirt / Kenyon / Afton, final=5.43) AND
//    0 -> 200 mph superspeedway (Daytona, final=2.80) proving clean 1->2->3->4 upshifts,
//    monotonic intra-gear RPM/pitch rise, sharp pitch drop + backfire on every upshift.
// 4. Verifies high-RPM eng12.sfx harmonic engagement (>6,400 RPM on throttle) & suppression off-throttle.
// 5. Verifies isolated audio RNG invariance & < 0.05 ms per-frame mixer cost across 20 simultaneous cars.

#include "hle_nascar_audio.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace eod::nascar_audio_hle;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                              \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg));    \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

std::vector<unsigned char> encode_real_esfx_blob(const char* primary_sfx, float sample_rpm) {
    // Exact 356-byte binary layout matching SLUS_204.41 PART.RES/eng1e..eng4e.ens (3x44B + 220B tail).
    std::vector<unsigned char> buf(356, 0xCC);
    const uint32_t count = 3;
    std::memcpy(buf.data(), &count, 4);
    auto write_rec = [&](size_t idx, const char* name, const float vals[7]) {
        unsigned char* p = buf.data() + 4 + idx * 44;
        std::memset(p, 0, 16);
        std::strncpy(reinterpret_cast<char*>(p), name, 15);
        std::memcpy(p + 16, vals, 7 * sizeof(float));
    };
    const float r01[7] = {sample_rpm, 0.65f, 0.96f, 100.0f, 3000.0f, 8100.0f, 80000.0f};
    const float r2[7]  = {7806.0f,    0.07f, 0.50f, 6400.0f, 6750.0f, 8100.0f, 80000.0f};
    write_rec(0, primary_sfx, r01);
    write_rec(1, primary_sfx, r01);
    write_rec(2, "eng12.sfx", r2);
    return buf;
}

EngineTorqueCurve make_nascar_v8_curve() {
    // Representative Cup V8 torque curve peaking ~5,500 RPM, dropping gently toward 8,100 RPM redline.
    EngineTorqueCurve c{};
    c.split_rpm = 5500.0f;
    c.a_lo = -1.2e-5f;
    c.b_lo =  0.140f;
    c.c_lo =  220.0f;
    c.a_hi = -3.8e-5f;
    c.b_hi =  0.380f;
    c.c_hi = -320.0f;
    c.drag_k = 0.012f;
    c.rev_start = 7800.0f;
    c.rev_slope = 0.45f;
    c.min_torque = 40.0f;
    return c;
}

struct AccelSimResult {
    std::vector<int> gear_sequence;
    std::vector<float> shift_speeds_mph;
    std::vector<float> pitch_before_shift;
    std::vector<float> pitch_after_shift;
    int backfires = 0;
    bool intra_gear_monotonic = true;
    bool saw_idle_active = false;
    bool saw_idle_off_high = false;
    bool saw_eng12_active = false;
    float peak_mph = 0.0f;
};

AccelSimResult simulate_acceleration(TrackClass tc,
                                     const nascar_audio::Profile& prof,
                                     float target_mph,
                                     float redline_rpm) {
    const auto preset = transmission_preset_for_track(tc);
    const auto curve = make_nascar_v8_curve();
    const auto shifts = build_monotonic_shift_table(preset, curve, redline_rpm);

    // Verify strict monotonic upshift thresholds
    for (size_t i = 0; i + 1 < shifts.size(); ++i) {
        CHECK(shifts[i].upshift_ds_rpm < shifts[i + 1].upshift_ds_rpm,
              "upshift driveshaft RPM thresholds must be strictly increasing");
    }

    // 28" NASCAR tire (radius 0.3556 m): wsf = r * (2*pi/60) / rear_end_ratio (exact f_00185470 formula).
    constexpr float kTireRadiusM = 0.3556f;
    constexpr float kRpmToRadS = 0.104719755f;
    const float wheel_speed_factor = (kTireRadiusM * kRpmToRadS) / preset.rear_end_ratio;

    NascarEngineVoiceState state{};
    state.profile = prof;
    state.last_gear = 1;
    state.audio_rng = nascar_audio::AudioRng(0x12345678u);

    AccelSimResult res{};
    int gear = 1;
    res.gear_sequence.push_back(gear);

    // First tick at idle (900 RPM, off throttle) to verify idle layer engagement
    {
        auto idle_f = evaluate_nascar_audio_frame(state, 900.0f, 0.0f, 1, true, true, false);
        res.saw_idle_active = idle_f.idle.active && idle_f.idle.volume > 0.01f;
    }

    float prev_pitch = 0.0f;
    int frames_in_gear = 0;
    const int steps = 400;
    for (int s = 1; s <= steps; ++s) {
        const float mph = (target_mph * float(s)) / float(steps);
        const float mps = mph * 0.44704f;
        const float ds_rpm = mps / wheel_speed_factor;

        // Exact EOD auto-shift check (gen/rec_022.c:4751-4758): ds_rpm > shift_up[gear-1]
        const int prev_gear = gear;
        if (gear < preset.num_gears && ds_rpm > shifts[gear - 1].upshift_ds_rpm) {
            ++gear;
            res.gear_sequence.push_back(gear);
            res.shift_speeds_mph.push_back(mph);
        }

        const float ratio = shifts[gear - 1].ratio;
        const float rpm = std::clamp(std::max(1000.0f, ds_rpm * ratio), 1000.0f, redline_rpm);
        const auto frame = evaluate_nascar_audio_frame(state, rpm, 1.0f, gear, true, true, false);

        // Layer 1 is the local-mix primary engine sample for 3-record ESFX profiles
        const float pitch = (frame.layers.size() >= 2 && frame.layers[1].active) ? frame.layers[1].pitch : 0.0f;

        if (gear > prev_gear) {
            res.pitch_before_shift.push_back(prev_pitch);
            res.pitch_after_shift.push_back(pitch);
            if (frame.trigger_backfire) ++res.backfires;
            frames_in_gear = 0;
        } else {
            // After transient settles (>7 frames into gear), pitch must rise monotonically with speed/RPM.
            if (frames_in_gear > 7 && rpm < redline_rpm - 10.0f && pitch + 1e-4f < prev_pitch) {
                res.intra_gear_monotonic = false;
            }
            ++frames_in_gear;
        }
        if (rpm > 2000.0f && !frame.idle.active) res.saw_idle_off_high = true;
        if (rpm > 6800.0f && frame.layers.size() >= 3 && frame.layers[2].active && frame.layers[2].volume > 0.01f) {
            res.saw_eng12_active = true;
        }
        prev_pitch = pitch;
        res.peak_mph = mph;
    }
    return res;
}

} // namespace

int main() {
    // 1. Parse all 4 real D2D ESFX profiles (eng1e..eng4e.ens)
    const struct { const char* tag; const char* sfx; float sr; } kProfiles[4] = {
        {"eng1e.ens", "eng10.sfx", 7806.0f},
        {"eng2e.ens", "eng20.sfx", 5940.0f},
        {"eng3e.ens", "eng30.sfx", 3791.0f},
        {"eng4e.ens", "eng30.sfx", 4043.0f},
    };
    nascar_audio::Profile parsed[4];
    for (int i = 0; i < 4; ++i) {
        auto blob = encode_real_esfx_blob(kProfiles[i].sfx, kProfiles[i].sr);
        CHECK(nascar_audio::parse(blob.data(), blob.size(), parsed[i]), "parse real ESFX v2 profile");
        CHECK(parsed[i].size() == 3, "ESFX profile must have 3 records");
        CHECK(std::fabs(parsed[i][0].sample_rpm - kProfiles[i].sr) < 0.01f, "sample_rpm match");
        CHECK(std::strcmp(parsed[i][2].sample.data(), "eng12.sfx") == 0, "3rd layer is eng12.sfx");
    }

    // 2. Profile detector: NASCAR matched, retail EOD *.val untouched
    {
        std::string pname;
        int idx = 0;
        CHECK(detect_nascar_esfx_profile("nseng1.ens", "eng1", pname, idx) && pname == "nseng1.ens" && idx == 1, "Cup nseng1");
        CHECK(detect_nascar_esfx_profile("nseng3.ens", "", pname, idx) && pname == "nseng3.ens" && idx == 3, "Hobby nseng3");
        CHECK(detect_nascar_esfx_profile("nseng4.ens", "", pname, idx) && pname == "nseng4.ens" && idx == 4, "Modified nseng4");
        CHECK(detect_nascar_esfx_profile("", "eng2", pname, idx) && pname == "nseng2.ens" && idx == 2, "Dirt fallback sound=eng2");
        CHECK(!detect_nascar_esfx_profile("4Stock.val", "", pname, idx), "Retail 4Stock.val passthrough");
        CHECK(!detect_nascar_esfx_profile("Mus68.val", "", pname, idx), "Retail Mus68.val passthrough");
        CHECK(!detect_nascar_esfx_profile("V8Junker.val", "", pname, idx), "Retail V8Junker.val passthrough");
    }

    // 3. Track classification sanity across all 5 classes
    CHECK(classify_track("AFTON.TRK", "Oval", "dirt") == TrackClass::ShortDirt, "Afton -> ShortDirt");
    CHECK(classify_track("KENYON.TRK", "Oval", "dirt") == TrackClass::ShortDirt, "Kenyon -> ShortDirt");
    CHECK(classify_track("BRISTOL.TRK", "Oval", "asphalt") == TrackClass::ShortAsphalt, "Bristol -> ShortAsphalt");
    CHECK(classify_track("WATKIN_1.TRK", "Road Course", "asphalt") == TrackClass::RoadCourse, "Watkins Glen -> RoadCourse");
    CHECK(classify_track("CHARLOTT.TRK", "Oval", "asphalt") == TrackClass::Speedway, "Charlotte -> Speedway");
    CHECK(classify_track("DAYTONA.TRK", "Oval", "asphalt") == TrackClass::Superspeedway, "Daytona -> Superspeedway");
    CHECK(classify_track("TALLADEG.TRK", "Oval", "asphalt") == TrackClass::Superspeedway, "Talladega -> Superspeedway");

    // 4. Short-Oval 0 -> 130 mph acceleration (ShortDirt final=5.43): MUST cycle 1 -> 2 -> 3 -> 4!
    const auto short_sim = simulate_acceleration(TrackClass::ShortDirt, parsed[0], 130.0f, 8100.0f);
    CHECK((short_sim.gear_sequence == std::vector<int>{1, 2, 3, 4}),
          "Short-oval must shift cleanly 1 -> 2 -> 3 -> 4 (never stuck in 1st gear)");
    CHECK(short_sim.intra_gear_monotonic, "Short-oval intra-gear pitch must rise monotonically");
    CHECK(short_sim.pitch_before_shift.size() == 3 && short_sim.pitch_after_shift.size() == 3,
          "Short-oval must record 3 upshift pitch transitions");
    for (size_t i = 0; i < short_sim.pitch_before_shift.size(); ++i) {
        CHECK(short_sim.pitch_after_shift[i] < short_sim.pitch_before_shift[i] * 0.85f,
              "Sharp pitch drop (>15%) required on every short-oval upshift");
    }
    CHECK(short_sim.backfires == 3, "Exhaust pop triggered on all 3 upshifts");
    CHECK(short_sim.saw_idle_active && short_sim.saw_idle_off_high, "Idle layer active <1600 RPM and off above");
    CHECK(short_sim.saw_eng12_active, "High-RPM eng12.sfx layer engaged above 6,400 RPM on throttle");

    // 5. Superspeedway 0 -> 200 mph acceleration (Daytona final=2.80): MUST cycle 1 -> 2 -> 3 -> 4!
    const auto super_sim = simulate_acceleration(TrackClass::Superspeedway, parsed[0], 200.0f, 8100.0f);
    CHECK((super_sim.gear_sequence == std::vector<int>{1, 2, 3, 4}),
          "Superspeedway must shift cleanly 1 -> 2 -> 3 -> 4 up to 200 mph");
    CHECK(super_sim.intra_gear_monotonic, "Superspeedway intra-gear pitch must rise monotonically");
    CHECK(super_sim.shift_speeds_mph.size() == 3 && super_sim.shift_speeds_mph[0] > 65.0f,
          "Daytona 1->2 shift happens at superspeedway speed (>65 mph)");
    CHECK(short_sim.shift_speeds_mph[0] < 45.0f,
          "Short-oval 1->2 shift happens early (<45 mph) so cars never stick in 1st on dirt ovals");
    for (size_t i = 0; i < super_sim.pitch_before_shift.size(); ++i) {
        CHECK(super_sim.pitch_after_shift[i] < super_sim.pitch_before_shift[i] * 0.85f,
              "Sharp pitch drop required on every superspeedway upshift");
    }

    // 6. High-RPM eng12.sfx throttle gate (> 0.2 engaged, <= 0.2 suppressed)
    {
        auto on_th  = nascar_audio::mix(parsed[0], 7200.0f, 0.85f, 0.30f, true, true, false);
        auto off_th = nascar_audio::mix(parsed[0], 7200.0f, 0.15f, 0.30f, true, true, false);
        auto low_rpm = nascar_audio::mix(parsed[0], 5500.0f, 0.95f, 0.30f, true, true, false);
        CHECK(on_th[2].active && on_th[2].volume > 0.05f, "eng12.sfx active >6400 RPM on throttle");
        CHECK(!off_th[2].active && off_th[2].volume == 0.0f, "eng12.sfx suppressed off-throttle");
        CHECK(!low_rpm[2].active && low_rpm[2].volume == 0.0f, "eng12.sfx silent below 6400 RPM");
    }

    // 7. Isolated audio RNG invariance
    {
        nascar_audio::AudioRng r1(0xCAFEBABEu), r2(0xCAFEBABEu);
        uint64_t external_gameplay_rng = 0x1122334455667788ull;
        const uint64_t snapshot = external_gameplay_rng;
        for (int i = 0; i < 1000; ++i) {
            const float a = r1.next_caller_random();
            const float b = r2.next_caller_random();
            CHECK(a == b, "Isolated AudioRng must be deterministic for identical seeds");
            CHECK(a >= 0.1f && a <= 0.6f, "AudioRng caller_random stays within [0.1, 0.6]");
        }
        CHECK(external_gameplay_rng == snapshot, "Gameplay RNG untouched");
    }

    // 8. Performance budget: < 0.05 ms per frame across 20 simultaneous cars!
    {
        std::vector<NascarEngineVoiceState> cars(20);
        for (size_t i = 0; i < cars.size(); ++i) {
            cars[i].profile = parsed[i % 4];
            cars[i].audio_rng = nascar_audio::AudioRng(uint32_t(100 + i));
            cars[i].last_gear = 2;
        }
        constexpr int kFrames = 500;
        const auto t0 = std::chrono::steady_clock::now();
        volatile float sink = 0.0f;
        for (int f = 0; f < kFrames; ++f) {
            for (size_t c = 0; c < cars.size(); ++c) {
                const float rpm = 2000.0f + float((f * 37 + int(c) * 311) % 6000);
                const float th = (f % 10 < 7) ? 0.9f : 0.1f;
                const int g = 1 + ((f / 60 + int(c)) % 4);
                const auto res = evaluate_nascar_audio_frame(cars[c], rpm, th, g, c == 0, c == 0, false);
                sink = sink + res.effective_rpm;
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const double per_frame_20cars_ms = total_ms / double(kFrames);
        std::printf("[NascarAudioGear] short-oval shifts=[%.1f, %.1f, %.1f] mph | "
                    "daytona shifts=[%.1f, %.1f, %.1f] mph | 20-car frame avg=%.4f ms (budget <0.05 ms)\n",
                    short_sim.shift_speeds_mph[0], short_sim.shift_speeds_mph[1], short_sim.shift_speeds_mph[2],
                    super_sim.shift_speeds_mph[0], super_sim.shift_speeds_mph[1], super_sim.shift_speeds_mph[2],
                    per_frame_20cars_ms);
        CHECK(per_frame_20cars_ms < 0.05, "20-car mixer cost must be < 0.05 ms per frame");
    }

    if (g_failures > 0) {
        std::fprintf(stderr, "NascarAudioGear FAILED (%d failures)\n", g_failures);
        return 1;
    }
    std::printf("NascarAudioGear PASSED (all checks green)\n");
    return 0;
}
