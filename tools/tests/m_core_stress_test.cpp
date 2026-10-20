// tools/tests/m_core_stress_test.cpp
// Empirical stress-test harness for Milestone M_CORE in eod-recomp.
// Validates:
// 1. Lead-Pursuit Interception Math (extreme closing velocities, zero distance, destroyed target filtering).
// 2. Power-Slide Ramming & Handbrake Activation (face_dot and aspeed boundary thresholds, turn-exit reset).
// 3. Barrier Unstick Recovery State Machine (pin detection frame timing, reverse gear duration, steer reversal).
// 4. Ballistic Chicken Weapon Firing (distance boundaries <8m, 8m, 45m, >45m, facing dot <=0.70 vs >0.70, cooldown 65 frames).
// 5. Demolition Derby Infield Respawn Relocation & Arena Boundary Clamping/Reflection.
// 6. Dynamic Bot Participant Scaling (1P+3AI, 2P+2AI, 3P+1AI, 4P) & Event State Arbitration Guard.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

#include "core/config.h"
#include "core/cpu.h"
#include "core/log.h"
#include "platform/platform.h"
#include "game_patches.h"

// EE State & Memory mock
EEState g_ee{};
uint8_t *g_ram = nullptr;

extern "C" uint32_t ee_read32_slow(uint32_t) { std::abort(); }
extern "C" void ee_write32_slow(uint32_t, uint32_t) { std::abort(); }
extern "C" uint8_t ee_read8_slow(uint32_t a) { return a < EE_RAM_SIZE ? g_ram[a] : 0; }
extern "C" void ee_write8_slow(uint32_t a, uint8_t v) { if (a < EE_RAM_SIZE) g_ram[a] = v; }
extern "C" void *ee_ptr(uint32_t a) { return a < EE_RAM_SIZE ? (g_ram + a) : nullptr; }
extern "C" void ee_call(uint32_t) {}

std::atomic<bool> g_movie_playing{false};
std::string g_game_root;
bool g_log_enabled[LOG_CAT_COUNT] = {};
void log_write(LogCat, const char *, ...) {}
bool platform_find_file(const char *, const char *, char *, size_t) { return false; }
void platform_get_pad(int, HostPadState *) {}
void platform_get_controller(int, HostPadState *) {}
void platform_set_vibration(int, uint8_t, uint8_t) {}
void platform_haptic_pulse(int, int, uint8_t, uint8_t) {}
void platform_audio_feedback(bool) {}
void platform_set_status(const char *) {}
void platform_show_hud_banner(const char *, int) {}
void platform_set_mod_overlay(bool) {}
void platform_set_aspect(int) {}

// Instrumentation structs for recording engine calls
struct CarCallLog {
    int gear_calls = 0;
    int last_gear = 0;

    int steer_calls = 0;
    float last_steer = 0.0f;

    int gas_calls = 0;
    float last_gas = 0.0f;

    int footbrake_calls = 0;
    float last_footbrake = 0.0f;

    int handbrake_calls = 0;
    float last_handbrake = 0.0f;
};

static CarCallLog s_car_logs[4];
static int s_weapon_calls = 0;
static int s_weapon_fire_calls = 0;
static uint32_t s_mock_weapon_ptr = 0x00550000;

extern "C" void f_001244A8();
extern "C" void f_001244A8_orig() {}
extern "C" void f_0018AFE0_orig() {}
extern "C" void f_0018AD20_orig() {}
extern "C" void f_0018CA98_orig() {}
extern "C" void f_001B5840_orig() {}
extern "C" void f_0017DA58_orig() {}
extern "C" void f_0018F2A8_orig() {}
extern "C" void f_0034D328_orig() {}

extern "C" void f_00179A08() {
    uint32_t car_idx = (uint32_t)g_ee.r[4].sd[0];
    if (car_idx < 4) {
        g_ee.r[2].sd[0] = (int64_t)(int32_t)(0x00450000 + car_idx * 0x2000);
    } else {
        g_ee.r[2].sd[0] = 0;
    }
}

extern "C" void f_0018AA50() {
    uint32_t car = (uint32_t)g_ee.r[4].sd[0];
    int gear = (int)g_ee.r[5].sd[0];
    for (int i = 0; i < 4; i++) {
        if (car == 0x00450000 + i * 0x2000) {
            s_car_logs[i].gear_calls++;
            s_car_logs[i].last_gear = gear;
            break;
        }
    }
}

extern "C" void f_0018AA80() {
    uint32_t car = (uint32_t)g_ee.r[4].sd[0];
    float steer = g_ee.fpr[12].f;
    for (int i = 0; i < 4; i++) {
        if (car == 0x00450000 + i * 0x2000) {
            s_car_logs[i].steer_calls++;
            s_car_logs[i].last_steer = steer;
            break;
        }
    }
}

extern "C" void f_0018AA70() {
    uint32_t car = (uint32_t)g_ee.r[4].sd[0];
    float gas = g_ee.fpr[12].f;
    for (int i = 0; i < 4; i++) {
        if (car == 0x00450000 + i * 0x2000) {
            s_car_logs[i].gas_calls++;
            s_car_logs[i].last_gas = gas;
            break;
        }
    }
}

extern "C" void f_0018AA68() {
    uint32_t car = (uint32_t)g_ee.r[4].sd[0];
    float footbrake = g_ee.fpr[12].f;
    for (int i = 0; i < 4; i++) {
        if (car == 0x00450000 + i * 0x2000) {
            s_car_logs[i].footbrake_calls++;
            s_car_logs[i].last_footbrake = footbrake;
            break;
        }
    }
}

// Override the weak f_0018AA60 to record handbrake calls and write car + 0x1B78
extern "C" void f_0018AA60(void) {
    uint32_t car = (uint32_t)g_ee.r[4].sd[0];
    float handbrake = g_ee.fpr[12].f;
    for (int i = 0; i < 4; i++) {
        if (car == 0x00450000 + i * 0x2000) {
            s_car_logs[i].handbrake_calls++;
            s_car_logs[i].last_handbrake = handbrake;
            break;
        }
    }
    if (car && !(car & 3) && car < EE_RAM_SIZE - 0x2000) {
        uint32_t v;
        memcpy(&v, &handbrake, 4);
        MEM_SW(car + 0x1B78, v);
    }
}

static int s_last_firing_ai_car = -1;
static int s_ai_weapon_fire_calls[4] = {0};

extern "C" void f_0017DE78() {
    s_weapon_calls++;
    s_last_firing_ai_car = (int)g_ee.r[4].sd[0];
    g_ee.r[2].sd[0] = (int64_t)(int32_t)s_mock_weapon_ptr;
}

extern "C" void f_0017E5C8() {
    s_weapon_fire_calls++;
    if (s_last_firing_ai_car >= 0 && s_last_firing_ai_car < 4) {
        s_ai_weapon_fire_calls[s_last_firing_ai_car]++;
    }
}

static void clear_logs() {
    for (int i = 0; i < 4; i++) {
        s_car_logs[i] = CarCallLog{};
        s_ai_weapon_fire_calls[i] = 0;
    }
    s_weapon_calls = 0;
    s_weapon_fire_calls = 0;
    s_last_firing_ai_car = -1;
}

static void write_f(uint32_t addr, float val) {
    uint32_t v;
    memcpy(&v, &val, 4);
    MEM_SW(addr, v);
}

static float read_f(uint32_t addr) {
    uint32_t v = MEM_LW(addr);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static void init_car(int idx, float px, float pz, float vx, float vz, float heading_x, float heading_z, float damage) {
    uint32_t c = 0x00450000 + idx * 0x2000;
    // Right vector (perpendicular to forward: right = (heading_z, -heading_x))
    write_f(c + 0x64, heading_z);  // rx
    write_f(c + 0x68, 0.0f);       // ry
    write_f(c + 0x6C, -heading_x); // rz

    // Forward vector
    write_f(c + 0x7C, heading_x);  // fx
    write_f(c + 0x80, 0.0f);        // fy
    write_f(c + 0x84, heading_z);  // fz

    // Position
    write_f(c + 0x88, px);
    write_f(c + 0x8C, 0.0f);
    write_f(c + 0x90, pz);

    // Velocity
    write_f(c + 0x104, vx);
    write_f(c + 0x108, 0.0f);
    write_f(c + 0x10C, vz);

    // Damage
    write_f(c + 0x1BEC, damage);
    // Handbrake
    write_f(c + 0x1B78, 0.0f);
}

static void assert_true(bool cond, const char *msg, int line) {
    if (!cond) {
        std::fprintf(stderr, "ASSERTION FAILED at line %d: %s\n", line, msg);
        std::exit(1);
    }
}
#define CHECK(cond, msg) assert_true((cond), (msg), __LINE__)

int main() {
    std::puts("================================================================================");
    std::puts("  CHALLENGER 1: Milestone M_CORE Empirical Stress Test & Boundary Verification");
    std::puts("================================================================================");

    std::vector<uint8_t> ram(EE_RAM_SIZE, 0);
    g_ram = ram.data();

    // Enable Battle Mode in config
    g_config.sp_battle_mode = true;

    const uint32_t kBattleMaster = 0x003D0000;
    MEM_SW(0x003CF194, kBattleMaster);      // battle_master pointer
    MEM_SW(0x003CF19C, 0x18000224);         // cur_event_type = BATTLE
    MEM_SW(kBattleMaster + 0x04, 1);        // event_state = 1 (active combat)
    MEM_SW(kBattleMaster + 0x450, 16);      // num_respawns = 16
    MEM_SW(0x0048522C, 4);                  // num_cars = 4
    MEM_SW(0x003C41AC, 1);                  // num_players = 1 (1P + 3 AI)

    for (int i = 0; i < 4; i++) {
        MEM_SW(0x00447458 + i * 4, 0x00450000 + i * 0x2000);
    }

    // ==========================================================================================
    // TEST SUITE 1: Lead-Pursuit Interception Math & Extreme Velocities
    // ==========================================================================================
    std::puts("\n[1] Stress-Testing Lead-Pursuit Interception Math...");

    // Test 1.1: Extreme target closing velocity (tvx = 500 m/s)
    // AI at (0, 0) facing +Z (fx=0, fz=1, rx=1, rz=0). Speed = 10 m/s (+Z).
    // Target at (50, 100) moving with tvx = 500.0f, tvz = 0.0f.
    init_car(0, 50.0f, 100.0f, 500.0f, 0.0f, 1.0f, 0.0f, 0.0f); // Target (car 0)
    init_car(1, 0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 1.0f, 0.0f);     // AI (car 1)
    init_car(2, 200.0f, 200.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f); // Far car 2
    init_car(3, 300.0f, 300.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f); // Far car 3

    clear_logs();
    game_patches_vblank();

    CHECK(!std::isnan(s_car_logs[1].last_steer), "AI steer must not be NaN under extreme target velocity");
    CHECK(!std::isinf(s_car_logs[1].last_steer), "AI steer must not be Inf under extreme target velocity");
    CHECK(s_car_logs[1].last_steer >= -1.0f && s_car_logs[1].last_steer <= 1.0f,
          "AI steer must remain clamped to [-1.0, 1.0]");
    CHECK(s_car_logs[1].last_steer > 0.0f, "AI steer should aim towards positive X (lead interception)");
    std::printf("  1.1 Extreme target velocity (500 m/s): steer = %.4f (Clamped & Finite) - PASS\n", s_car_logs[1].last_steer);

    // Test 1.2: Extreme AI speed (avz = 50,000 m/s)
    init_car(1, 0.0f, 0.0f, 0.0f, 50000.0f, 0.0f, 1.0f, 0.0f);
    clear_logs();
    game_patches_vblank();

    CHECK(!std::isnan(s_car_logs[1].last_steer), "AI steer must not be NaN under 50,000 m/s AI speed");
    CHECK(!std::isinf(s_car_logs[1].last_steer), "AI steer must not be Inf under 50,000 m/s AI speed");
    CHECK(s_car_logs[1].last_steer >= -1.0f && s_car_logs[1].last_steer <= 1.0f,
          "AI steer must remain clamped under extreme AI speed");
    std::printf("  1.2 Extreme AI speed (50,000 m/s): steer = %.4f - PASS\n", s_car_logs[1].last_steer);

    // Test 1.3: Target co-located with AI (dist = 0.0m)
    init_car(0, 5.0f, 5.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    init_car(1, 5.0f, 5.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    clear_logs();
    game_patches_vblank();

    CHECK(!std::isnan(s_car_logs[1].last_steer), "Zero-distance must not produce NaN division");
    CHECK(!std::isinf(s_car_logs[1].last_steer), "Zero-distance must not produce Inf division");
    std::printf("  1.3 Co-located vehicles (d = 0.0m): steer = %.4f (Zero-div protected) - PASS\n", s_car_logs[1].last_steer);

    // Test 1.4: Destroyed target filtering (damage >= 1.0f)
    // Car 0: 5m away, damage = 1.0f (DESTROYED)
    // Car 2: 15m away, damage = 0.2f (ACTIVE)
    // Car 1 (AI) should ignore Car 0 and target Car 2!
    init_car(1, 0.0f, 0.0f, 0.0f, 5.0f, 0.0f, 1.0f, 0.0f);      // AI at origin facing +Z
    init_car(0, -5.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);     // Destroyed car at (-5, 0) [Left]
    init_car(2, 15.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.2f);     // Active car at (+15, 0) [Right]
    init_car(3, 50.0f, 50.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);

    clear_logs();
    game_patches_vblank();

    // Since active car is to the right (+X) and destroyed car is to the left (-X),
    // steer should be positive (> 0) pointing towards Car 2!
    CHECK(s_car_logs[1].last_steer > 0.0f, "AI must skip destroyed car (damage=1.0) and steer right towards active car");
    std::printf("  1.4 Destroyed target filtering (Car 0 damage=1.0 skipped for Car 2): steer = %.4f - PASS\n", s_car_logs[1].last_steer);

    // Test 1.5: Damage boundary condition (0.999f vs 1.000f)
    // If Car 0 has damage = 0.999f (still alive), AI should target Car 0 (steer < 0, to the left)
    init_car(0, -5.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.999f);
    clear_logs();
    game_patches_vblank();
    CHECK(s_car_logs[1].last_steer < 0.0f, "AI must engage near car when damage is 0.999f (< 1.0f)");
    std::printf("  1.5 Damage boundary 0.999f (Active target chosen): steer = %.4f - PASS\n", s_car_logs[1].last_steer);

    // Test 1.6: Fallback when ALL opponents are wrecked (all damage >= 1.0f)
    init_car(0, -5.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    init_car(2, 15.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    init_car(3, 50.0f, 50.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    clear_logs();
    game_patches_vblank();
    // Fallback selects closest wrecked car (Car 0 at -5m, left), so steer < 0
    CHECK(s_car_logs[1].last_steer < 0.0f, "AI fallback selects closest wrecked car when all opponents wrecked");
    std::printf("  1.6 Fallback when all opponents wrecked: steer = %.4f - PASS\n", s_car_logs[1].last_steer);

    // ==========================================================================================
    // TEST SUITE 2: Handbrake Power-Slide Ramming Activation & Exit Reset
    // ==========================================================================================
    std::puts("\n[2] Stress-Testing Handbrake Power-Slide Activation Conditions...");

    // Handbrake triggers when: face_dot < 0.15f && aspeed > 9.0f
    // AI at (0,0) facing +Z. Right is +X.
    // Target placed at (20, 0) [90 degree right turn -> face_dot ~ 0.0 < 0.15f].

    // Test 2.1: face_dot < 0.15f and aspeed = 10.0f (> 9.0f) -> ACTIVATED
    init_car(1, 0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 1.0f, 0.0f); // aspeed = 10.0f
    init_car(0, 20.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f); // Target 90 deg right
    clear_logs();
    game_patches_vblank();

    CHECK(s_car_logs[1].last_handbrake == 0.90f, "Handbrake must activate to 0.90f on sharp turn at speed");
    CHECK(s_car_logs[1].last_footbrake == 0.0f, "Footbrake must be 0.0f during power-slide");
    CHECK(read_f(0x00450000 + 1 * 0x2000 + 0x1B78) == 0.90f, "car + 0x1B78 must receive 0.90f");
    std::printf("  2.1 Sharp turn at speed (aspeed=10.0, 90 deg): handbrake = %.2f, footbrake = %.2f - PASS\n",
                s_car_logs[1].last_handbrake, s_car_logs[1].last_footbrake);

    // Test 2.2: Low speed boundary (aspeed = 9.0f) -> NOT ACTIVATED
    init_car(1, 0.0f, 0.0f, 0.0f, 9.0f, 0.0f, 1.0f, 0.0f); // aspeed = 9.0f (not > 9.0f)
    clear_logs();
    game_patches_vblank();
    CHECK(s_car_logs[1].last_handbrake == 0.0f, "Handbrake must be 0.0f at aspeed <= 9.0f");
    std::printf("  2.2 Speed boundary aspeed=9.0f: handbrake = %.2f - PASS\n", s_car_logs[1].last_handbrake);

    // Test 2.3: Slightly above speed boundary (aspeed = 9.05f) -> ACTIVATED
    init_car(1, 0.0f, 0.0f, 0.0f, 9.05f, 0.0f, 1.0f, 0.0f); // aspeed = 9.05f (> 9.0f)
    clear_logs();
    game_patches_vblank();
    CHECK(s_car_logs[1].last_handbrake == 0.90f, "Handbrake must activate at aspeed=9.05f");
    std::printf("  2.3 Speed boundary aspeed=9.05f: handbrake = %.2f - PASS\n", s_car_logs[1].last_handbrake);

    // Test 2.4: Shallow angle (target directly ahead, face_dot ~ 1.0 >= 0.15f, aspeed = 15.0f) -> NOT ACTIVATED
    init_car(0, 0.0f, 30.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f); // Target directly ahead
    init_car(1, 0.0f, 0.0f, 0.0f, 15.0f, 0.0f, 1.0f, 0.0f);
    clear_logs();
    game_patches_vblank();
    CHECK(s_car_logs[1].last_handbrake == 0.0f, "Handbrake must be 0.0f when facing target (face_dot >= 0.15)");
    std::printf("  2.4 Shallow angle (face_dot ~ 1.0): handbrake = %.2f - PASS\n", s_car_logs[1].last_handbrake);

    // Test 2.5: Turn exit reset: after power-slide, straight path immediately resets handbrake to 0.0f
    CHECK(read_f(0x00450000 + 1 * 0x2000 + 0x1B78) == 0.0f, "car + 0x1B78 reset to 0.0f on turn exit");
    std::printf("  2.5 Handbrake reset on exit verified on memory offset +0x1B78 - PASS\n");

    // ==========================================================================================
    // TEST SUITE 3: Barrier Unstick Recovery State Machine Timing
    // ==========================================================================================
    std::puts("\n[3] Stress-Testing Barrier Unstick Recovery State Machine...");

    // Unstick condition: aspeed < 1.4f && moved_d < 0.35f
    // Target stuck: place AI at (10.0, 10.0) with zero velocity.
    // Target opponent at (10.0, 15.0).
    init_car(0, 10.0f, 15.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    init_car(1, 10.0f, 10.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);

    // Frame 0 seed (seeds prev_x, prev_z)
    clear_logs();
    game_patches_vblank();

    int frames_forward = 0;
    int first_reverse_frame = -1;

    // Simulate 120 frames of being stuck against a wall
    std::vector<int> gears;
    for (int frame = 1; frame <= 120; frame++) {
        clear_logs();
        // Keep car pinned at (10.0, 10.0)
        init_car(1, 10.0f, 10.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
        game_patches_vblank();
        gears.push_back(s_car_logs[1].last_gear);

        if (s_car_logs[1].last_gear == -1 && first_reverse_frame == -1) {
            first_reverse_frame = frame;
        }
        if (s_car_logs[1].last_gear == 1 && first_reverse_frame == -1) {
            frames_forward++;
        }
    }

    // Count reverse frames
    int reverse_count = 0;
    for (int g : gears) {
        if (g == -1) reverse_count++;
    }

    std::printf("  3.1 Pin onset to reverse gear transition: frame %d (Forward frames: %d)\n",
                first_reverse_frame, frames_forward);
    std::printf("  3.2 Total consecutive reverse frames: %d (Target: 55 frames = 0.916s)\n", reverse_count);

    // Verify reverse gear duration is exactly 55 frames
    CHECK(reverse_count == 55, "Unstick recovery must execute exactly 55 frames of reverse gear");
    // Verify reverse was triggered within 40-42 frames
    CHECK(first_reverse_frame <= 42, "Unstick recovery must trigger reverse within ~40 frames");
    // Verify total time from pin to recovery is < 2.0s (120 frames)
    CHECK(first_reverse_frame + reverse_count < 120, "Total unstick duration must be strictly < 2.0s (120 frames)");
    std::printf("  3.3 Total recovery duration: %d frames (%.3f s < 2.0s) - PASS\n",
                first_reverse_frame + reverse_count, (first_reverse_frame + reverse_count) / 60.0f);

    // Test 3.4: Movement clears stuck frames before threshold is reached
    // First, drive normally for 60 frames to clear any residual reverse state
    for (int frame = 0; frame < 60; frame++) {
        init_car(1, 10.0f + frame * 0.5f, 10.0f, 5.0f, 0.0f, 1.0f, 0.0f, 0.0f);
        game_patches_vblank();
    }
    // Now simulate 30 frames stuck (< 40 frames threshold)
    for (int frame = 0; frame < 30; frame++) {
        init_car(1, 40.0f, 10.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
        game_patches_vblank();
    }
    // Now move 1.0m at speed 2.0m/s
    init_car(1, 11.0f, 10.0f, 2.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    clear_logs();
    game_patches_vblank();
    CHECK(s_car_logs[1].last_gear == 1, "Movement breaks stuck accumulation; gear remains 1");
    std::printf("  3.4 Movement reset (car escapes before 40 frames): gear = %d - PASS\n", s_car_logs[1].last_gear);

    // ==========================================================================================
    // TEST SUITE 4: Ballistic Chicken Weapon Firing Boundary Conditions
    // ==========================================================================================
    std::puts("\n[4] Stress-Testing Ballistic Chicken Weapon Firing Boundaries...");

    // Chicken trigger condition:
    // fire_cooldown >= 65 && direct_face_dot > 0.70f && direct_dist >= 8.0f && direct_dist <= 45.0f

    auto reset_cooldown = [&]() {
        // Place all opponents behind Car 1 so AI does not fire during cooldown recharge
        // Also face Car 2 and Car 3 in -Z (heading_z = -1.0f) so they face away from all targets
        init_car(1, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
        init_car(0, 0.0f, -10.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f);
        init_car(2, 0.0f, -15.0f, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f);
        init_car(3, 0.0f, -20.0f, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f);
        for (int i = 0; i < 70; i++) {
            game_patches_vblank();
        }
    };

    reset_cooldown();

    // Test 4.1: Range boundary d = 7.9m (< 8m minimum range) -> NO FIRE
    init_car(1, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    init_car(0, 0.0f, 7.9f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f); // d = 7.9m, face_dot = 1.0
    clear_logs();
    game_patches_vblank();
    CHECK(s_ai_weapon_fire_calls[1] == 0, "Chicken must NOT fire at d = 7.9m (< 8.0m min range)");
    std::printf("  4.1 Min range boundary d = 7.9m: Car 1 weapon_fire_calls = %d - PASS\n", s_ai_weapon_fire_calls[1]);

    // Test 4.2: Range boundary d = 8.0m (lower boundary) -> FIRES!
    init_car(1, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    init_car(0, 0.0f, 8.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f); // d = 8.0m, face_dot = 1.0
    clear_logs();
    game_patches_vblank();
    CHECK(s_ai_weapon_fire_calls[1] == 1, "Chicken MUST fire at d = 8.0m (exact boundary)");
    std::printf("  4.2 Min range boundary d = 8.0m: Car 1 weapon_fire_calls = %d - PASS\n", s_ai_weapon_fire_calls[1]);

    reset_cooldown();

    // Test 4.3: Range boundary d = 45.0m (upper boundary) -> FIRES!
    // Inside arena: Car 1 at (0, -22.5), Car 0 at (0, +22.5). Distance = 45.0m, both within radius 22.5m < 24.5m
    init_car(1, 0.0f, -22.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    init_car(0, 0.0f, 22.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    clear_logs();
    game_patches_vblank();
    CHECK(s_ai_weapon_fire_calls[1] == 1, "Chicken MUST fire at d = 45.0m (exact boundary)");
    std::printf("  4.3 Max range boundary d = 45.0m: Car 1 weapon_fire_calls = %d - PASS\n", s_ai_weapon_fire_calls[1]);

    reset_cooldown();

    // Test 4.4: Range boundary d = 45.1m (> 45m maximum range) -> NO FIRE
    // Inside arena: Car 1 at (0, -22.55), Car 0 at (0, +22.55). Distance = 45.1m, both within radius 22.55m < 24.5m
    init_car(1, 0.0f, -22.55f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    init_car(0, 0.0f, 22.55f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    clear_logs();
    game_patches_vblank();
    CHECK(s_ai_weapon_fire_calls[1] == 0, "Chicken must NOT fire at d = 45.1m (> 45.0m max range)");
    std::printf("  4.4 Max range boundary d = 45.1m: Car 1 weapon_fire_calls = %d - PASS\n", s_ai_weapon_fire_calls[1]);

    reset_cooldown();

    // Test 4.5: Facing angle boundary face_dot <= 0.70f -> NO FIRE
    // Let target be at distance 20m.
    // If target is at an angle such that face_dot = 0.70f:
    // fx = 0, fz = 1. target = (dx, dz). d = 20m. dz / 20 = 0.70 -> dz = 14.0m, dx = sqrt(400 - 196) = 14.2828m.
    init_car(1, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    init_car(0, 14.2828568f, 14.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    clear_logs();
    game_patches_vblank();
    CHECK(s_ai_weapon_fire_calls[1] == 0, "Chicken must NOT fire when face_dot <= 0.70f");
    std::printf("  4.5 Facing angle boundary face_dot = 0.70f: Car 1 weapon_fire_calls = %d - PASS\n", s_ai_weapon_fire_calls[1]);

    // Test 4.6: Facing angle boundary face_dot = 0.72f (> 0.70f) -> FIRES!
    // dz / 20 = 0.72 -> dz = 14.4m, dx = sqrt(400 - 207.36) = 13.87948m.
    init_car(1, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    init_car(0, 13.87948f, 14.4f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    clear_logs();
    game_patches_vblank();
    CHECK(s_ai_weapon_fire_calls[1] == 1, "Chicken MUST fire when face_dot > 0.70f (0.72f)");
    std::printf("  4.6 Facing angle boundary face_dot = 0.72f: Car 1 weapon_fire_calls = %d - PASS\n", s_ai_weapon_fire_calls[1]);

    // Test 4.7: Cooldown enforcement (65 frames)
    // Immediately after firing, weapon must not fire for 64 consecutive frames, and fire on frame 65
    int fire_on_cooldown = 0;
    for (int frame = 1; frame < 65; frame++) {
        clear_logs();
        init_car(1, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
        init_car(0, 0.0f, 20.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f); // Perfect range & angle
        game_patches_vblank();
        if (s_ai_weapon_fire_calls[1] > 0) fire_on_cooldown++;
    }
    CHECK(fire_on_cooldown == 0, "Chicken weapon must enforce strict 65-frame cooldown");

    // Frame 65: triggers again!
    clear_logs();
    init_car(1, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    init_car(0, 0.0f, 20.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    game_patches_vblank();
    CHECK(s_ai_weapon_fire_calls[1] == 1, "Chicken weapon triggers again on frame 65");


    // ==========================================================================================
    // TEST SUITE 5: Arena Infield Respawns & Kinetic Boundary Reflection
    // ==========================================================================================
    std::puts("\n[5] Stress-Testing Arena Respawns & Boundary Clamping...");

    // Test 5.1: Respawn points relocated to radius 10-18m facing center
    for (int i = 0; i < 16; i++) {
        uint32_t pt = kBattleMaster + 0x10 + i * 0x44;
        float rx = read_f(pt + 0x24);
        float rz = read_f(pt + 0x2C);
        float r = sqrtf(rx * rx + rz * rz);
        CHECK(r >= 10.0f && r <= 18.0f, "Respawn points must lie strictly within Demolition Derby infield [10m, 18m]");

        // Facing vectors
        float fx = read_f(pt + 0x18);
        float fz = read_f(pt + 0x20);
        // Facing dot position should be negative (facing inward towards center)
        float dot_inward = (rx * fx + rz * fz) / r;
        CHECK(dot_inward < -0.99f, "Respawn facing vector must point directly towards arena center");
    }
    std::printf("  5.1 Infield respawns (16 points in 10-18m radius facing center) - PASS\n");

    // Test 5.2: Kinetic boundary reflection (car escaping at radius 26.0m heading outward)
    // Car placed at (0, 26.0) heading outward (+Z, vz = 15.0m/s)
    init_car(1, 0.0f, 26.0f, 0.0f, 15.0f, 0.0f, 1.0f, 0.0f);
    game_patches_vblank();

    float clamped_z = read_f(0x00450000 + 1 * 0x2000 + 0x90);
    float reflected_vz = read_f(0x00450000 + 1 * 0x2000 + 0x10C);
    CHECK(clamped_z <= 24.1f && clamped_z >= 23.9f, "Position clamped to ~24.0m boundary circle");
    CHECK(reflected_vz < 0.0f, "Velocity reflected inward (negative vz)");
    // Normal is (0, -1). Outward velocity is (0, 15). dot = -15.
    // vz - 1.8 * dot * nz = 15 - 1.8 * (-15) * (-1) = 15 - 27 = -12.0m/s
    CHECK(std::abs(reflected_vz - (-12.0f)) < 0.1f, "Velocity reflected with restitution 0.8 (vz = -12.0 m/s)");
    std::printf("  5.2 Kinetic boundary reflection: clamped pos = %.2f, reflected vz = %.2f - PASS\n", clamped_z, reflected_vz);

    // ==========================================================================================
    // TEST SUITE 6: Bot Participant Scaling (1P..4P) & Finish Guard
    // ==========================================================================================
    std::puts("\n[6] Stress-Testing Bot Scaling (1P..4P) & Finish Arbitration Guard...");

    // Test 6.1: Event state guard (event_state = 0 countdown -> no AI updates)
    MEM_SW(kBattleMaster + 0x04, 0); // event_state = 0
    clear_logs();
    init_car(1, 0.0f, 0.0f, 0.0f, 5.0f, 0.0f, 1.0f, 0.0f);
    game_patches_vblank();
    CHECK(s_car_logs[1].steer_calls == 0, "No AI updates when event_state == 0 (countdown)");

    // Test 6.2: Event state guard (event_state = 2 match finished / 5 kills -> no AI updates)
    MEM_SW(kBattleMaster + 0x04, 2); // event_state = 2
    clear_logs();
    game_patches_vblank();
    CHECK(s_car_logs[1].steer_calls == 0, "No AI updates when event_state == 2 (match finished / scoreboard)");

    // Test 6.3: Bot scaling 1P + 3 AI (num_players = 1) -> AI drives 1, 2, 3. Car 0 untouched.
    MEM_SW(kBattleMaster + 0x04, 1); // event_state = 1
    MEM_SW(0x003C41AC, 1);          // 1 player
    clear_logs();
    game_patches_vblank();
    CHECK(s_car_logs[0].steer_calls == 0, "1P mode: Car 0 is human, untouched by AI");
    CHECK(s_car_logs[1].steer_calls == 1, "1P mode: Car 1 driven by AI");
    CHECK(s_car_logs[2].steer_calls == 1, "1P mode: Car 2 driven by AI");
    CHECK(s_car_logs[3].steer_calls == 1, "1P mode: Car 3 driven by AI");
    std::printf("  6.1 1P + 3 AI: Car 0 untouched, Cars 1..3 controlled - PASS\n");

    // Test 6.4: Bot scaling 2P + 2 AI (num_players = 2) -> AI drives 2, 3. Cars 0, 1 untouched.
    MEM_SW(0x003C41AC, 2); // 2 players
    clear_logs();
    game_patches_vblank();
    CHECK(s_car_logs[0].steer_calls == 0, "2P mode: Car 0 is human, untouched by AI");
    CHECK(s_car_logs[1].steer_calls == 0, "2P mode: Car 1 is human, untouched by AI");
    CHECK(s_car_logs[2].steer_calls == 1, "2P mode: Car 2 driven by AI");
    CHECK(s_car_logs[3].steer_calls == 1, "2P mode: Car 3 driven by AI");
    std::printf("  6.2 2P + 2 AI: Cars 0..1 untouched, Cars 2..3 controlled - PASS\n");

    // Test 6.5: Bot scaling 3P + 1 AI (num_players = 3) -> AI drives 3. Cars 0, 1, 2 untouched.
    MEM_SW(0x003C41AC, 3); // 3 players
    clear_logs();
    game_patches_vblank();
    CHECK(s_car_logs[0].steer_calls == 0, "3P mode: Car 0 is human, untouched");
    CHECK(s_car_logs[1].steer_calls == 0, "3P mode: Car 1 is human, untouched");
    CHECK(s_car_logs[2].steer_calls == 0, "3P mode: Car 2 is human, untouched");
    CHECK(s_car_logs[3].steer_calls == 1, "3P mode: Car 3 driven by AI");
    std::printf("  6.3 3P + 1 AI: Cars 0..2 untouched, Car 3 controlled - PASS\n");

    // Test 6.6: Bot scaling 4P (num_players = 4) -> 0 AI bots, all cars untouched.
    MEM_SW(0x003C41AC, 4); // 4 players
    clear_logs();
    game_patches_vblank();
    CHECK(s_car_logs[0].steer_calls == 0, "4P mode: Car 0 untouched");
    CHECK(s_car_logs[1].steer_calls == 0, "4P mode: Car 1 untouched");
    CHECK(s_car_logs[2].steer_calls == 0, "4P mode: Car 2 untouched");
    CHECK(s_car_logs[3].steer_calls == 0, "4P mode: Car 3 untouched");
    std::printf("  6.4 4P all human: Cars 0..3 untouched by AI - PASS\n");

    std::puts("\n================================================================================");
    std::puts("  ALL EMPIRICAL STRESS TESTS & BOUNDARY CONDITIONS PASSED (100% SUCCESS)!");
    std::puts("================================================================================");
    return 0;
}
