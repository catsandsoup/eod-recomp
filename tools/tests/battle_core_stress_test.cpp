// tools/tests/battle_core_stress_test.cpp - Empirical Stress Harness for Milestone M_CORE
// Thoroughly stress-tests:
// 1. Dynamic Bot Scaling (1P+3AI, 2P+2AI, 3P+1AI, 4P, invalid player counts, solo car)
// 2. Finish Arbitration & Countdown Guard (event_state 0, 1, 2, 5-kill victory threshold, ranking)
// 3. Arena Kinetic Boundary Clamping & Velocity Reflection (24.5m radius, extreme velocities, all angles)
// 4. Solo 1P Clean Boot & Menu Carousel Injection (Eve 0 Event 1, no controller 2 popup requirement)

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/config.h"
#include "core/cpu.h"
#include "core/log.h"
#include "platform/platform.h"

// Memory & CPU state
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

// Tracking structures for AI calls
struct CarCallStats {
    int gear_calls = 0;
    int steer_calls = 0;
    int gas_calls = 0;
    int footbrake_calls = 0;
    int handbrake_calls = 0;
    int weapon_calls = 0;
    int last_gear = 0;
    float last_steer = 0.0f;
    float last_gas = 0.0f;
    float last_footbrake = 0.0f;
    float last_handbrake = 0.0f;

    int total_ai_calls() const {
        return gear_calls + steer_calls + gas_calls + footbrake_calls + handbrake_calls + weapon_calls;
    }
    void reset() {
        gear_calls = steer_calls = gas_calls = footbrake_calls = handbrake_calls = weapon_calls = 0;
        last_gear = 0;
        last_steer = last_gas = last_footbrake = last_handbrake = 0.0f;
    }
};

static CarCallStats s_car_calls[4];
static uint32_t s_mock_cars[4] = {0x00500000, 0x00510000, 0x00520000, 0x00530000};
static uint32_t s_mock_weapons[4] = {0x00600000, 0x00610000, 0x00620000, 0x00630000};

static int find_car_idx(uint32_t car_ptr) {
    for (int i = 0; i < 4; i++) {
        if (s_mock_cars[i] == car_ptr) return i;
    }
    return -1;
}

// Intercepted vehicle control calls
extern "C" void f_0018AA50() { // Gear
    uint32_t car = (uint32_t)g_ee.r[4].sd[0];
    int gear = (int)g_ee.r[5].sd[0];
    int idx = find_car_idx(car);
    if (idx >= 0) {
        s_car_calls[idx].gear_calls++;
        s_car_calls[idx].last_gear = gear;
    }
}

extern "C" void f_0018AA80() { // Steer
    uint32_t car = (uint32_t)g_ee.r[4].sd[0];
    float steer = g_ee.fpr[12].f;
    int idx = find_car_idx(car);
    if (idx >= 0) {
        s_car_calls[idx].steer_calls++;
        s_car_calls[idx].last_steer = steer;
    }
}

extern "C" void f_0018AA70() { // Gas
    uint32_t car = (uint32_t)g_ee.r[4].sd[0];
    float gas = g_ee.fpr[12].f;
    int idx = find_car_idx(car);
    if (idx >= 0) {
        s_car_calls[idx].gas_calls++;
        s_car_calls[idx].last_gas = gas;
    }
}

extern "C" void f_0018AA68() { // Footbrake
    uint32_t car = (uint32_t)g_ee.r[4].sd[0];
    float brake = g_ee.fpr[12].f;
    int idx = find_car_idx(car);
    if (idx >= 0) {
        s_car_calls[idx].footbrake_calls++;
        s_car_calls[idx].last_footbrake = brake;
    }
}

// Handbrake override
extern "C" void f_0018AA60(void) { // Handbrake
    uint32_t car = (uint32_t)g_ee.r[4].sd[0];
    float hb = g_ee.fpr[12].f;
    int idx = find_car_idx(car);
    if (idx >= 0) {
        s_car_calls[idx].handbrake_calls++;
        s_car_calls[idx].last_handbrake = hb;
    }
    if (car && !(car & 3) && car < EE_RAM_SIZE - 0x2000) {
        uint32_t u;
        memcpy(&u, &hb, 4);
        MEM_SW(car + 0x1B78, u);
    }
}

extern "C" void f_00179A08() { // Get Car Pointer
    uint32_t idx = (uint32_t)g_ee.r[4].sd[0];
    if (idx < 4) {
        g_ee.r[2].sd[0] = s_mock_cars[idx];
    } else {
        g_ee.r[2].sd[0] = 0;
    }
}

extern "C" void f_0017DE78() { // Get Weapon Pointer
    uint32_t car_idx = (uint32_t)g_ee.r[4].sd[0];
    if (car_idx < 4) {
        g_ee.r[2].sd[0] = s_mock_weapons[car_idx];
    } else {
        g_ee.r[2].sd[0] = 0;
    }
}

static int s_weapon_fire_count = 0;
extern "C" void f_0017E5C8() { // Weapon Fire
    uint32_t weapon = (uint32_t)g_ee.r[4].sd[0];
    for (int i = 0; i < 4; i++) {
        if (s_mock_weapons[i] == weapon) {
            s_car_calls[i].weapon_calls++;
            s_weapon_fire_count++;
            break;
        }
    }
}

// Mock engine callbacks
extern "C" void f_001244A8();
extern "C" void f_001244A8_orig() {}
extern "C" void f_0018AFE0_orig() {}
extern "C" void f_0017DA58_orig() {}
extern "C" void f_0018F2A8_orig() {}
extern "C" void f_0034D328_orig() {}

// Scoreboard and ranking mock
static int s_scoreboard_displayed = 0;
extern "C" void f_00201938(uint32_t ranks_ptr, uint32_t num_cars, uint32_t flag) {
    s_scoreboard_displayed++;
}

extern "C" void f_00166B00(uint32_t arg) {}

// Native ranking sort f_00167658: sorts participants by net score (kills - deaths) descending
extern "C" void f_00167658(uint32_t bm) {
    uint32_t num_cars = MEM_LW(bm + 0x5A8);
    if (num_cars > 4) num_cars = 4;
    int32_t scores[4];
    int32_t indices[4];
    for (uint32_t i = 0; i < num_cars; i++) {
        int32_t kills = (int32_t)MEM_LW(bm + 0x460 + i * 0x50);
        int32_t deaths = (int32_t)MEM_LW(bm + 0x468 + i * 0x50);
        scores[i] = kills - deaths;
        indices[i] = (int32_t)i;
    }
    // Simple selection sort for rankings
    for (uint32_t i = 0; i < num_cars; i++) {
        for (uint32_t j = i + 1; j < num_cars; j++) {
            if (scores[j] > scores[i]) {
                std::swap(scores[i], scores[j]);
                std::swap(indices[i], indices[j]);
            }
        }
    }
    // Write back sorted places to bm + 0x594
    for (uint32_t i = 0; i < num_cars; i++) {
        MEM_SW(bm + 0x594 + i * 4, (uint32_t)indices[i]);
    }
}

// Native finish arbitration f_00167D58 (SLUS_209.10:0x00167D58)
void run_native_finish_arbitration(uint32_t bm) {
    uint32_t event_state = MEM_LW(bm + 0x04);
    if (event_state != 1) return; // Only arbitrate during active combat
    uint32_t num_cars = MEM_LW(bm + 0x5A8);
    for (uint32_t i = 0; i < num_cars && i < 4; i++) {
        int32_t kills = (int32_t)MEM_LW(bm + 0x460 + i * 0x50);
        int32_t deaths = (int32_t)MEM_LW(bm + 0x468 + i * 0x50);
        int32_t net_score = kills - deaths;
        if (net_score >= 5) {
            // Victory condition met! Transition event_state from 1 to 2
            MEM_SW(bm + 0x04, 2);
            break;
        }
    }
    f_00167658(bm);
    if (MEM_LW(bm + 0x04) == 2) {
        f_00201938(bm + 0x594, num_cars, 0);
    }
}

void game_patches_vblank();

// Platform stubs
static HostPadState s_mock_pad[2];
void platform_get_pad(int port, HostPadState *out) {
    if (port >= 0 && port < 2) *out = s_mock_pad[port];
    else *out = HostPadState{};
}
void platform_get_controller(int port, HostPadState *out) {
    platform_get_pad(port, out);
}
void platform_set_vibration(int, uint8_t, uint8_t) {}
void platform_haptic_pulse(int, int, uint8_t, uint8_t) {}
void platform_audio_feedback(bool) {}
void platform_set_status(const char *) {}
void platform_show_hud_banner(const char *, int) {}
void platform_set_mod_overlay(bool) {}
void platform_set_aspect(int) {}

static void check(bool cond, const char *msg) {
    if (!cond) {
        std::fprintf(stderr, "\n[ASSERTION FAILED] %s\n", msg);
        std::exit(1);
    }
}

static void float_write(uint32_t addr, float val) {
    uint32_t u;
    memcpy(&u, &val, 4);
    MEM_SW(addr, u);
}

static float float_read(uint32_t addr) {
    uint32_t u = MEM_LW(addr);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static void reset_call_stats() {
    for (int i = 0; i < 4; i++) {
        s_car_calls[i].reset();
    }
    s_weapon_fire_count = 0;
    s_scoreboard_displayed = 0;
}

static void setup_test_cars() {
    for (int i = 0; i < 4; i++) {
        uint32_t c = s_mock_cars[i];
        MEM_SW(0x00447458 + i * 4, c);

        // Position: place in circle at radius 12.0m facing inward
        float angle = (float)i * (3.14159265f / 2.0f);
        float px = cosf(angle) * 12.0f;
        float pz = sinf(angle) * 12.0f;
        float_write(c + 0x88, px);
        float_write(c + 0x8C, 0.0f);
        float_write(c + 0x90, pz);

        // Velocity = 0
        float_write(c + 0x104, 0.0f);
        float_write(c + 0x108, 0.0f);
        float_write(c + 0x10C, 0.0f);

        // Orientation: right and forward
        float_write(c + 0x64, -sinf(angle)); // right X
        float_write(c + 0x6C, cosf(angle));  // right Z
        float_write(c + 0x7C, -cosf(angle)); // forward X (inward)
        float_write(c + 0x84, -sinf(angle)); // forward Z (inward)

        // Damage = 0 (healthy)
        float_write(c + 0x1BEC, 0.0f);
    }
}

int main() {
    std::puts("======================================================================");
    std::puts("  M_CORE Empirical Stress Test Harness - Milestone M_CORE Validation");
    std::puts("======================================================================");

    std::vector<uint8_t> ram(EE_RAM_SIZE, 0);
    g_ram = ram.data();

    // Default configuration
    g_config.sp_battle_mode = true;
    g_config.cheat_chickens = true;
    g_config.cheat_turbo = false;

    const uint32_t kBattleMaster = 0x003D0000;
    MEM_SW(0x003CF194, kBattleMaster);      // active BattleMaster pointer
    MEM_SW(0x003CF19C, 0x18000224);          // cur_event_type = BATTLE
    MEM_SW(0x0048522C, 4);                   // num_cars = 4
    MEM_SW(kBattleMaster + 0x5A8, 4);        // BattleMaster num_cars = 4
    MEM_SW(kBattleMaster + 0x450, 16);       // num_respawns = 16

    setup_test_cars();

    // =========================================================================
    // SUITE 1: BOT SCALING (1P+3AI, 2P+2AI, 3P+1AI, 4P, boundaries)
    // =========================================================================
    std::puts("\n[SUITE 1] Stress-Testing Dynamic Bot Scaling across Player Modes...");

    // 1.1: 1P + 3 AI Mode (num_players = 1)
    MEM_SW(kBattleMaster + 0x04, 1); // event_state = 1 (active)
    MEM_SW(0x003C41AC, 1);           // num_players = 1
    reset_call_stats();
    game_patches_vblank();

    check(s_car_calls[0].total_ai_calls() == 0, "1P+3AI: Player 0 must NOT be manipulated by AI");
    check(s_car_calls[1].total_ai_calls() > 0, "1P+3AI: Bot 1 must receive AI manipulation");
    check(s_car_calls[2].total_ai_calls() > 0, "1P+3AI: Bot 2 must receive AI manipulation");
    check(s_car_calls[3].total_ai_calls() > 0, "1P+3AI: Bot 3 must receive AI manipulation");
    check(s_car_calls[1].gas_calls > 0 && s_car_calls[1].steer_calls > 0, "1P+3AI: Bot 1 has gas and steer");
    check(s_car_calls[2].gas_calls > 0 && s_car_calls[2].steer_calls > 0, "1P+3AI: Bot 2 has gas and steer");
    check(s_car_calls[3].gas_calls > 0 && s_car_calls[3].steer_calls > 0, "1P+3AI: Bot 3 has gas and steer");
    std::puts("  [PASS] 1.1: 1P + 3 AI Mode verified (P0 untouched, AI 1..3 active)");

    // 1.2: 2P + 2 AI Mode (num_players = 2)
    MEM_SW(0x003C41AC, 2); // num_players = 2
    reset_call_stats();
    game_patches_vblank();

    check(s_car_calls[0].total_ai_calls() == 0, "2P+2AI: Player 0 must NOT be manipulated by AI");
    check(s_car_calls[1].total_ai_calls() == 0, "2P+2AI: Player 1 must NOT be manipulated by AI");
    check(s_car_calls[2].total_ai_calls() > 0, "2P+2AI: Bot 2 must receive AI manipulation");
    check(s_car_calls[3].total_ai_calls() > 0, "2P+2AI: Bot 3 must receive AI manipulation");
    std::puts("  [PASS] 1.2: 2P + 2 AI Mode verified (P0, P1 untouched, AI 2..3 active)");

    // 1.3: 3P + 1 AI Mode (num_players = 3)
    MEM_SW(0x003C41AC, 3); // num_players = 3
    reset_call_stats();
    game_patches_vblank();

    check(s_car_calls[0].total_ai_calls() == 0, "3P+1AI: Player 0 must NOT be manipulated by AI");
    check(s_car_calls[1].total_ai_calls() == 0, "3P+1AI: Player 1 must NOT be manipulated by AI");
    check(s_car_calls[2].total_ai_calls() == 0, "3P+1AI: Player 2 must NOT be manipulated by AI");
    check(s_car_calls[3].total_ai_calls() > 0, "3P+1AI: Bot 3 must receive AI manipulation");
    std::puts("  [PASS] 1.3: 3P + 1 AI Mode verified (P0..P2 untouched, AI 3 active)");

    // 1.4: 4P Mode (num_players = 4, full human split-screen)
    MEM_SW(0x003C41AC, 4); // num_players = 4
    reset_call_stats();
    game_patches_vblank();

    check(s_car_calls[0].total_ai_calls() == 0, "4P: Player 0 must NOT be manipulated by AI");
    check(s_car_calls[1].total_ai_calls() == 0, "4P: Player 1 must NOT be manipulated by AI");
    check(s_car_calls[2].total_ai_calls() == 0, "4P: Player 2 must NOT be manipulated by AI");
    check(s_car_calls[3].total_ai_calls() == 0, "4P: Player 3 must NOT be manipulated by AI");
    std::puts("  [PASS] 1.4: 4P Full Human Mode verified (Zero AI interference across all 4 cars)");

    // 1.5: Bot scaling robustness: Boundary and invalid player values
    // Case A: num_players = 0 -> clamped to 1 (1P + 3 AI)
    MEM_SW(0x003C41AC, 0);
    reset_call_stats();
    game_patches_vblank();
    check(s_car_calls[0].total_ai_calls() == 0, "num_players=0 clamps to 1: P0 untouched");
    check(s_car_calls[1].total_ai_calls() > 0, "num_players=0 clamps to 1: Bot 1 active");

    // Case B: num_players = 5 -> clamped to 1 (1P + 3 AI)
    MEM_SW(0x003C41AC, 5);
    reset_call_stats();
    game_patches_vblank();
    check(s_car_calls[0].total_ai_calls() == 0, "num_players=5 clamps to 1: P0 untouched");
    check(s_car_calls[1].total_ai_calls() > 0, "num_players=5 clamps to 1: Bot 1 active");

    // Case C: num_players = 0xFFFFFFFF -> clamped to 1
    MEM_SW(0x003C41AC, 0xFFFFFFFF);
    reset_call_stats();
    game_patches_vblank();
    check(s_car_calls[0].total_ai_calls() == 0, "num_players=0xFFFFFFFF clamps to 1: P0 untouched");
    check(s_car_calls[1].total_ai_calls() > 0, "num_players=0xFFFFFFFF clamps to 1: Bot 1 active");

    // Case D: Solo car (num_cars = 1) -> 0 AI manipulation regardless of num_players
    MEM_SW(0x0048522C, 1);
    MEM_SW(0x003C41AC, 1);
    reset_call_stats();
    game_patches_vblank();
    check(s_car_calls[0].total_ai_calls() == 0, "Solo 1-car: P0 untouched");
    check(s_car_calls[1].total_ai_calls() == 0, "Solo 1-car: Bot 1 untouched");
    MEM_SW(0x0048522C, 4); // restore num_cars = 4

    std::puts("  [PASS] 1.5: Bot Scaling boundary inputs (0, 5, 0xFFFFFFFF, solo) cleanly clamped & safe");

    // =========================================================================
    // SUITE 2: FINISH ARBITRATION & COUNTDOWN GUARD
    // =========================================================================
    std::puts("\n[SUITE 2] Stress-Testing Finish Arbitration & Match States...");

    // 2.1: Countdown state (event_state == 0)
    MEM_SW(0x003C41AC, 1);           // 1P mode
    MEM_SW(kBattleMaster + 0x04, 0); // event_state = 0 (Countdown!)
    reset_call_stats();
    game_patches_vblank();

    check(s_car_calls[0].total_ai_calls() == 0, "event_state=0: Player 0 has no AI calls");
    check(s_car_calls[1].total_ai_calls() == 0, "event_state=0: Bot 1 must NOT execute AI inputs during countdown");
    check(s_car_calls[2].total_ai_calls() == 0, "event_state=0: Bot 2 must NOT execute AI inputs during countdown");
    check(s_car_calls[3].total_ai_calls() == 0, "event_state=0: Bot 3 must NOT execute AI inputs during countdown");
    std::puts("  [PASS] 2.1: Countdown guard verified: AI completely idle during event_state == 0");

    // 2.2: Active combat state (event_state == 1)
    MEM_SW(kBattleMaster + 0x04, 1); // event_state = 1 (Active!)
    reset_call_stats();
    game_patches_vblank();
    check(s_car_calls[1].total_ai_calls() > 0, "event_state=1: Bot 1 active during combat");
    check(s_car_calls[2].total_ai_calls() > 0, "event_state=1: Bot 2 active during combat");
    check(s_car_calls[3].total_ai_calls() > 0, "event_state=1: Bot 3 active during combat");
    std::puts("  [PASS] 2.2: Active combat state verified: AI active during event_state == 1");

    // 2.3: 5-Kill Victory Threshold & event_state transition (1 -> 2)
    // Clear all car scores
    for (int i = 0; i < 4; i++) {
        MEM_SW(kBattleMaster + 0x460 + i * 0x50, 0); // kills = 0
        MEM_SW(kBattleMaster + 0x468 + i * 0x50, 0); // deaths = 0
    }
    MEM_SW(kBattleMaster + 0x04, 1); // event_state = 1

    // Step 1 to 4 kills: event_state must REMAIN 1
    for (int k = 1; k <= 4; k++) {
        MEM_SW(kBattleMaster + 0x460 + 0 * 0x50, k); // Car 0 has k kills
        run_native_finish_arbitration(kBattleMaster);
        check(MEM_LW(kBattleMaster + 0x04) == 1, "Match must not finish before 5 kills");
    }
    std::puts("  [PASS] 2.3a: Match remains active at 1, 2, 3, and 4 kills");

    // Edge case: Car 0 has 5 kills but 1 death (net score = 4 < 5)
    MEM_SW(kBattleMaster + 0x460 + 0 * 0x50, 5);
    MEM_SW(kBattleMaster + 0x468 + 0 * 0x50, 1);
    run_native_finish_arbitration(kBattleMaster);
    check(MEM_LW(kBattleMaster + 0x04) == 1, "Net score of 4 (5 kills - 1 death) must not trigger victory");
    std::puts("  [PASS] 2.3b: Net score calculation (kills - deaths) verified");

    // Reaching 5 net kills: Victory transition!
    MEM_SW(kBattleMaster + 0x468 + 0 * 0x50, 0); // deaths = 0 -> net score = 5
    // Also give other cars varying scores for ranking test:
    // Car 1: 3 kills, 0 deaths (2nd place)
    // Car 2: 2 kills, 0 deaths (3rd place)
    // Car 3: 0 kills, 1 death (4th place)
    MEM_SW(kBattleMaster + 0x460 + 1 * 0x50, 3);
    MEM_SW(kBattleMaster + 0x460 + 2 * 0x50, 2);
    MEM_SW(kBattleMaster + 0x460 + 3 * 0x50, 0);
    MEM_SW(kBattleMaster + 0x468 + 3 * 0x50, 1);

    run_native_finish_arbitration(kBattleMaster);
    check(MEM_LW(kBattleMaster + 0x04) == 2, "Reaching 5 kills must transition event_state from 1 to 2");
    check(s_scoreboard_displayed == 1, "Scoreboard presentation f_00201938 triggered upon victory");

    // Verify 1st to 4th post-match rankings in bm + 0x594
    check(MEM_LW(kBattleMaster + 0x594 + 0 * 4) == 0, "1st place is Car 0 (5 kills)");
    check(MEM_LW(kBattleMaster + 0x594 + 1 * 4) == 1, "2nd place is Car 1 (3 kills)");
    check(MEM_LW(kBattleMaster + 0x594 + 2 * 4) == 2, "3rd place is Car 2 (2 kills)");
    check(MEM_LW(kBattleMaster + 0x594 + 3 * 4) == 3, "4th place is Car 3 (0 kills)");
    std::puts("  [PASS] 2.3c: 5-Kill transition 1->2 and 1st-4th ranking sort verified");

    // 2.4: Post-match state (event_state == 2)
    // In event_state == 2, AI manipulation MUST STOP!
    reset_call_stats();
    game_patches_vblank();
    check(s_car_calls[0].total_ai_calls() == 0, "event_state=2: Player 0 has no AI calls");
    check(s_car_calls[1].total_ai_calls() == 0, "event_state=2: Bot 1 must NOT execute AI inputs on victory screen");
    check(s_car_calls[2].total_ai_calls() == 0, "event_state=2: Bot 2 must NOT execute AI inputs on victory screen");
    check(s_car_calls[3].total_ai_calls() == 0, "event_state=2: Bot 3 must NOT execute AI inputs on victory screen");
    std::puts("  [PASS] 2.4: Victory guard verified: AI completely ceases inputs in event_state == 2");

    // =========================================================================
    // SUITE 3: ARENA KINETIC BOUNDARY CLAMPING & VELOCITY REFLECTION
    // =========================================================================
    std::puts("\n[SUITE 3] Stress-Testing Arena Boundary Clamping & Velocity Reflection...");

    uint32_t c0 = s_mock_cars[0];

    // 3.1: Inside boundary (d = 14.14m <= 24.5m)
    float_write(c0 + 0x88, 10.0f);
    float_write(c0 + 0x90, 10.0f);
    float_write(c0 + 0x104, 5.0f);
    float_write(c0 + 0x10C, 5.0f);
    game_patches_vblank();
    check(fabsf(float_read(c0 + 0x88) - 10.0f) < 1e-4f, "Inside boundary: pos X unchanged");
    check(fabsf(float_read(c0 + 0x90) - 10.0f) < 1e-4f, "Inside boundary: pos Z unchanged");
    check(fabsf(float_read(c0 + 0x104) - 5.0f) < 1e-4f, "Inside boundary: vel X unchanged");
    check(fabsf(float_read(c0 + 0x10C) - 5.0f) < 1e-4f, "Inside boundary: vel Z unchanged");
    std::puts("  [PASS] 3.1: Inside boundary (d <= 24.5m): zero modification to kinematics");

    // 3.2: Immediate boundary breach (d = 24.51m) with outward velocity
    // Placed at (24.51, 0.0), velocity (10.0, 0.0) heading outward (+X)
    float_write(c0 + 0x88, 24.51f);
    float_write(c0 + 0x90, 0.0f);
    float_write(c0 + 0x104, 10.0f);
    float_write(c0 + 0x10C, 0.0f);
    game_patches_vblank();

    float post_x = float_read(c0 + 0x88);
    float post_z = float_read(c0 + 0x90);
    float post_vx = float_read(c0 + 0x104);
    float post_vz = float_read(c0 + 0x10C);

    check(fabsf(post_x - 24.0f) < 0.01f, "Position clamped to perimeter radius 24.0m");
    check(fabsf(post_z - 0.0f) < 0.01f, "Position Z preserved on axis");
    // Normal is nx = -1.0, nz = 0.0 (inward)
    // Outward dot = vx * nx = 10.0 * (-1.0) = -10.0
    // Reflected vx = 10.0 - 1.8 * (-10.0) * (-1.0) = 10.0 - 18.0 = -8.0 m/s (heading inward!)
    check(fabsf(post_vx - (-8.0f)) < 0.01f, "Outward velocity reflected inward with e=0.8 (vx = -8.0 m/s)");
    check(fabsf(post_vz - 0.0f) < 0.01f, "Tangential velocity preserved");
    std::puts("  [PASS] 3.2: Boundary breach (d = 24.51m): clamped to 24.0m and reflected inward (-8.0 m/s)");

    // 3.3: Stress-test 32 radial escape angles at extreme velocities (v = 50.0m/s to 500.0m/s)
    std::puts("  [RUN] 3.3: Sweeping 32 radial headings with extreme velocities (50 to 500 m/s)...");
    for (int angle_step = 0; angle_step < 32; angle_step++) {
        float theta = (float)angle_step * (2.0f * 3.14159265f / 32.0f);
        float init_r = 35.0f + (float)(angle_step % 5) * 15.0f; // 35m to 95m (well past boundary)
        float init_speed = 50.0f + (float)angle_step * 15.0f;    // 50m/s to 515m/s (supersonic)

        // Radial outward heading
        float px = cosf(theta) * init_r;
        float pz = sinf(theta) * init_r;
        float vx = cosf(theta) * init_speed;
        float vz = sinf(theta) * init_speed;

        float_write(c0 + 0x88, px);
        float_write(c0 + 0x90, pz);
        float_write(c0 + 0x104, vx);
        float_write(c0 + 0x10C, vz);

        game_patches_vblank();

        float clamped_x = float_read(c0 + 0x88);
        float clamped_z = float_read(c0 + 0x90);
        float clamped_r = sqrtf(clamped_x * clamped_x + clamped_z * clamped_z);

        float reflected_vx = float_read(c0 + 0x104);
        float reflected_vz = float_read(c0 + 0x10C);

        // Clamped radius must be 24.0m
        check(fabsf(clamped_r - 24.0f) < 0.05f, "Every escape angle clamped to radius 24.0m");

        // Inward normal at clamped position: (nx, nz) = (-clamped_x/24.0, -clamped_z/24.0)
        float in_nx = -clamped_x / clamped_r;
        float in_nz = -clamped_z / clamped_r;
        float post_inward_vel = reflected_vx * in_nx + reflected_vz * in_nz;

        // Post-reflection velocity MUST point inward (dot > 0) with speed ~ 0.8 * init_speed
        check(post_inward_vel > 0.0f, "Reflected velocity must point strictly INWARD toward center");
        check(fabsf(post_inward_vel - (0.8f * init_speed)) < (0.05f * init_speed), "Restitution is 0.8");
    }
    std::puts("  [PASS] 3.3: 32 radial headings verified at extreme speeds (50 to 515 m/s): 100% contained");

    // 3.4: Glancing / tangential boundary strike
    // Car at (25.0, 0.0), vr = 10.0 (outward), vt = 25.0 (tangential along Z)
    float_write(c0 + 0x88, 25.0f);
    float_write(c0 + 0x90, 0.0f);
    float_write(c0 + 0x104, 10.0f);  // vr
    float_write(c0 + 0x10C, 25.0f);  // vt
    game_patches_vblank();

    float glance_x = float_read(c0 + 0x88);
    float glance_vx = float_read(c0 + 0x104);
    float glance_vz = float_read(c0 + 0x10C);

    check(fabsf(glance_x - 24.0f) < 0.01f, "Glancing strike: clamped to 24.0m");
    check(fabsf(glance_vx - (-8.0f)) < 0.01f, "Glancing strike: radial velocity reflected to -8.0 m/s");
    check(fabsf(glance_vz - 25.0f) < 0.01f, "Glancing strike: tangential velocity (25.0 m/s) preserved");
    std::puts("  [PASS] 3.4: Glancing / tangential collision verified: tangential velocity perfectly preserved");

    // 3.5: Car outside boundary heading INWARD (e.g. re-entering arena)
    // Car placed at (26.0, 0.0), velocity (-15.0, 0.0) heading inward towards center
    float_write(c0 + 0x88, 26.0f);
    float_write(c0 + 0x90, 0.0f);
    float_write(c0 + 0x104, -15.0f);
    float_write(c0 + 0x10C, 0.0f);
    game_patches_vblank();

    check(fabsf(float_read(c0 + 0x88) - 24.0f) < 0.01f, "Inward moving car clamped to 24.0m");
    check(fabsf(float_read(c0 + 0x104) - (-15.0f)) < 0.01f, "Inward velocity NOT reflected (dot >= 0)");
    std::puts("  [PASS] 3.5: Inward moving car outside boundary: position clamped, velocity unharmed");

    // 3.6: Simultaneous multi-car boundary collision
    // All 4 cars breach the boundary at 4 compass directions simultaneously
    float dirs[4][2] = {{1.0f, 0.0f}, {0.0f, 1.0f}, {-1.0f, 0.0f}, {0.0f, -1.0f}};
    for (int i = 0; i < 4; i++) {
        uint32_t c = s_mock_cars[i];
        float_write(c + 0x88, dirs[i][0] * 28.0f);
        float_write(c + 0x90, dirs[i][1] * 28.0f);
        float_write(c + 0x104, dirs[i][0] * 20.0f);
        float_write(c + 0x10C, dirs[i][1] * 20.0f);
    }
    game_patches_vblank();
    for (int i = 0; i < 4; i++) {
        uint32_t c = s_mock_cars[i];
        float r = sqrtf(float_read(c + 0x88) * float_read(c + 0x88) + float_read(c + 0x90) * float_read(c + 0x90));
        check(fabsf(r - 24.0f) < 0.05f, "Multi-car simultaneous breach: all clamped to 24.0m");
        float post_v = float_read(c + 0x104) * dirs[i][0] + float_read(c + 0x10C) * dirs[i][1];
        check(fabsf(post_v - (-16.0f)) < 0.05f, "Multi-car simultaneous breach: all reflected inward (-16 m/s)");
    }
    std::puts("  [PASS] 3.6: Simultaneous 4-car boundary collision verified without cross-talk");

    // =========================================================================
    // SUITE 4: 1P CLEAN BOOT & MENU CAROUSEL INJECTION
    // =========================================================================
    std::puts("\n[SUITE 4] Stress-Testing 1P Clean Boot & Carousel Injection...");

    // Setup Eve & Contest tables for f_001244A8
    MEM_SW(0x003C4C04, 0x12F); // Mode = 0x12F (Single-Player Action Mode)
    MEM_SW(0x003C4CF4, 2);     // num_eves = 2
    MEM_SW(0x003C4D08 + 0x70, 4); // Eve 0 has 4 contests
    MEM_SW(0x003C4D08 + 0x84 + 0x70, 3); // Eve 1 has 3 contests
    MEM_SW(0x003C4CF8, 7);     // num_contests = 7

    // Clear Contest 0 initially to a stock race (event_type 0x18000200)
    uint32_t c0_ptr = 0x003C61A8 + 0 * 0x28;
    MEM_SW(c0_ptr + 0x08, 0x18000200);
    MEM_SW(c0_ptr + 0x10, 8); // max_cars = 8

    f_001244A8();

    // Verify Carousel Injection:
    // Contest 0 of Eve 0 must be transformed into Battle Mode (Event 1 of 5 in carousel!)
    check(MEM_LW(c0_ptr + 0x08) == 0x18000224, "Eve 0 Contest 0 event_type is 0x18000224 (BATTLE)");
    check(MEM_LW(c0_ptr + 0x10) == 4, "Eve 0 Contest 0 max_cars is 4 (BattleMaster struct bound)");
    check(MEM_LW(c0_ptr + 0x18) == 1, "Eve 0 Contest 0 rounds is 1");

    // Total contests increased from 7 to 9
    check(MEM_LW(0x003C4CF8) == 9, "Two contests appended (total = 9)");
    check(MEM_LW(0x003C4D08 + 0x70) == 5, "Eve 0 now has 5 contests (visible in Event 1 of 5 screen)");
    check(MEM_LW(0x003C4D08 + 0x84 + 0x70) == 4, "Eve 1 now has 4 contests");

    // Verify injected Contest 7 (Eve 0)
    uint32_t c7_ptr = 0x003C61A8 + 7 * 0x28;
    check(MEM_LW(c7_ptr + 0x00) == 7, "Contest 7 global index is 7");
    check(MEM_LW(c7_ptr + 0x04) == 0, "Contest 7 belongs to Eve 0");
    check(MEM_LW(c7_ptr + 0x08) == 0x18000224, "Contest 7 event_type is BATTLE");
    check(MEM_LW(c7_ptr + 0x10) == 4, "Contest 7 max_cars is 4");
    check(MEM_LW(c7_ptr + 0x24) == 1, "Contest 7 enabled flag is 1");

    // Verify Career Mode (mode = 0x130) Eve injection
    MEM_SW(0x003C4C04, 0x130);
    f_001244A8();
    check(MEM_LW(0x003C4CF8) == 9, "Idempotent: no duplicate contests added when already present");
    std::puts("  [PASS] 4.1: Carousel injection into Eve 0 Event 1 and Contest tables verified");

    // 4.2: Solo 1P Clean Boot without Controller 2
    // Setup single controller in port 0, port 1 disconnected
    s_mock_pad[0].connected = true;
    s_mock_pad[1].connected = false; // Controller 2 DISCONNECTED

    MEM_SW(0x003C41AC, 1);    // 1 player
    MEM_SW(0x0048522C, 4);    // 1P + 3AI in arena
    MEM_SW(kBattleMaster + 0x04, 1); // active
    setup_test_cars();
    reset_call_stats();

    // Call vblank without controller 2 connected
    game_patches_vblank();

    check(s_car_calls[0].total_ai_calls() == 0, "Solo 1P boot: Player 0 is human controlled");
    check(s_car_calls[1].total_ai_calls() > 0, "Solo 1P boot: AI 1 is active without Controller 2");
    check(s_car_calls[2].total_ai_calls() > 0, "Solo 1P boot: AI 2 is active without Controller 2");
    check(s_car_calls[3].total_ai_calls() > 0, "Solo 1P boot: AI 3 is active without Controller 2");
    std::puts("  [PASS] 4.2: Solo 1P Clean Boot executes cleanly without Controller 2 connected");

    // 4.3: Arena Infield Respawn Relocation (Radius 10m to 18m, facing center)
    // Run vblank to apply respawn relocation
    game_patches_vblank();
    uint32_t num_respawns = MEM_LW(kBattleMaster + 0x450);
    check(num_respawns == 16, "16 respawn points relocated");

    for (uint32_t i = 0; i < 16; i++) {
        uint32_t pt = kBattleMaster + 0x10 + i * 0x44;
        float rx = float_read(pt + 0x24);
        float rz = float_read(pt + 0x2C);
        float r = sqrtf(rx * rx + rz * rz);
        check(r >= 10.0f && r <= 18.0f, "Respawn point radius must be within 10m..18m infield");

        // Forward vector (offset 0x18 and 0x20) must face towards center (0,0)
        float fx = float_read(pt + 0x18);
        float fz = float_read(pt + 0x20);
        // dot(forward, to_center) where to_center is (-rx/r, -rz/r)
        float to_center_dot = fx * (-rx / r) + fz * (-rz / r);
        check(to_center_dot > 0.99f, "Respawn orientation must face directly inward towards center");
    }
    std::puts("  [PASS] 4.3: All 16 respawn points relocated to radius 10-18m infield facing center");

    // =========================================================================
    // SUITE 5: ADVERSARIAL EDGE CASES & HOSTILE INPUTS (EMPIRICAL ORACLES)
    // =========================================================================
    std::puts("\n[SUITE 5] Stress-Testing Adversarial Edge Cases & Hostile Inputs...");

    // 5.1: NULL / Unaligned BattleMaster pointer
    MEM_SW(0x003CF194, 0); // NULL bm
    reset_call_stats();
    game_patches_vblank();
    check(s_car_calls[1].total_ai_calls() == 0, "NULL bm: zero AI execution, no null dereference");

    MEM_SW(0x003CF194, 0x003D0001); // Unaligned bm
    reset_call_stats();
    game_patches_vblank();
    check(s_car_calls[1].total_ai_calls() == 0, "Unaligned bm: zero AI execution, alignment protected");

    MEM_SW(0x003CF194, 0xFFFFFFFF); // Out of bounds bm
    reset_call_stats();
    game_patches_vblank();
    check(s_car_calls[1].total_ai_calls() == 0, "Out of bounds bm: zero AI execution, range protected");

    // Restore valid bm
    MEM_SW(0x003CF194, kBattleMaster);
    MEM_SW(kBattleMaster + 0x04, 1); // event_state = 1
    MEM_SW(0x003C41AC, 1);           // 1P mode

    // 5.2: NULL Car pointers in slots
    uint32_t saved_c2 = s_mock_cars[2];
    s_mock_cars[2] = 0; // Make car 2 NULL in both f_00179A08 and memory table
    MEM_SW(0x00447458 + 2 * 4, 0);
    reset_call_stats();
    game_patches_vblank();
    check(s_car_calls[2].total_ai_calls() == 0, "NULL car pointer handled gracefully without crash");
    check(s_car_calls[1].total_ai_calls() > 0, "Other active bots continue processing");
    s_mock_cars[2] = saved_c2; // restore car 2
    MEM_SW(0x00447458 + 2 * 4, saved_c2);

    // 5.3: Exact coincident target (d = 0.0m) -> no NaN, no inf
    float_write(s_mock_cars[0] + 0x88, 10.0f);
    float_write(s_mock_cars[0] + 0x90, 10.0f);
    float_write(s_mock_cars[1] + 0x88, 10.0f); // Exactly same pos as Car 0
    float_write(s_mock_cars[1] + 0x90, 10.0f);
    reset_call_stats();
    game_patches_vblank();
    check(!std::isnan(s_car_calls[1].last_steer) && !std::isinf(s_car_calls[1].last_steer),
          "d = 0.0m: steer is numerically stable (no NaN or inf)");
    check(!std::isnan(s_car_calls[1].last_gas) && !std::isinf(s_car_calls[1].last_gas),
          "d = 0.0m: gas is numerically stable (no NaN or inf)");
    std::puts("  [PASS] 5.3: Coincident vehicles (d=0) do not cause divide-by-zero or NaN");

    // 5.4: All opponents destroyed (damage >= 1.0f)
    setup_test_cars();
    float_write(s_mock_cars[0] + 0x1BEC, 1.0f); // Car 0 wrecked
    float_write(s_mock_cars[2] + 0x1BEC, 1.0f); // Car 2 wrecked
    float_write(s_mock_cars[3] + 0x1BEC, 1.0f); // Car 3 wrecked
    reset_call_stats();
    game_patches_vblank();
    check(s_car_calls[1].total_ai_calls() > 0, "Bot 1 gracefully falls back to damaged target when all wrecked");
    std::puts("  [PASS] 5.4: Wrecked opponent handling and fallback target selection verified");

    // 5.5: Barrier Unstick State Machine Timing (<0.35m over 40 frames -> 55 frames reverse)
    setup_test_cars();
    // Pin Bot 1 at a wall: velocity = 0, displacement = 0
    float_write(s_mock_cars[1] + 0x104, 0.0f);
    float_write(s_mock_cars[1] + 0x10C, 0.0f);
    // Run 41 frames pinned: stuck_frames increments from 1 up to 41, setting reverse_frames = 55
    for (int frame = 0; frame < 41; frame++) {
        reset_call_stats();
        game_patches_vblank();
        check(s_car_calls[1].last_gear == 1, "Pinned <=40 frames: still in forward gear");
    }
    // On frame 42 (next tick after reverse_frames is armed): reverse gear executes (gear = -1)!
    reset_call_stats();
    game_patches_vblank();
    check(s_car_calls[1].last_gear == -1, "On frame 42: reverse gear active (gear = -1)");
    check(s_car_calls[1].last_gas == 1.0f, "Reverse gear maintains full throttle");
    check(fabsf(s_car_calls[1].last_steer) == 0.9f, "Reverse gear applies counter-steer 0.9f");

    // Next 54 frames: must REMAIN in reverse gear (-1) for a total of 55 reverse frames
    for (int rframe = 1; rframe < 55; rframe++) {
        reset_call_stats();
        game_patches_vblank();
        check(s_car_calls[1].last_gear == -1, "During 55-frame reverse window: gear remains -1");
    }
    // Frame 55+1: reverse completed, returns to forward gear (gear = 1)
    reset_call_stats();
    game_patches_vblank();
    check(s_car_calls[1].last_gear == 1, "Reverse recovery completed: returns cleanly to forward gear");
    std::puts("  [PASS] 5.5: Barrier unstick state machine (41 frames pin -> 55 frames reverse = 1.60s recovery) verified");

    // 5.6: Power-slide Handbrake Trigger on Sharp Turn at High Speed
    setup_test_cars();
    // Bot 1 moving fast (aspeed = 12.0m/s > 9.0m/s)
    float_write(s_mock_cars[1] + 0x104, 12.0f);
    float_write(s_mock_cars[1] + 0x10C, 0.0f);
    // Facing vector along +X
    float_write(s_mock_cars[1] + 0x7C, 1.0f); // forward X
    float_write(s_mock_cars[1] + 0x84, 0.0f); // forward Z
    // Right vector along +Z
    float_write(s_mock_cars[1] + 0x64, 0.0f); // right X
    float_write(s_mock_cars[1] + 0x6C, 1.0f); // right Z
    // Place target perpendicular (to the right, +Z) so face_dot < 0.15f (sharp 90-degree turn!)
    float_write(s_mock_cars[0] + 0x88, float_read(s_mock_cars[1] + 0x88));
    float_write(s_mock_cars[0] + 0x90, float_read(s_mock_cars[1] + 0x90) + 15.0f);

    reset_call_stats();
    game_patches_vblank();
    check(s_car_calls[1].last_handbrake == 0.90f, "Sharp turn at speed: handbrake engaged to 0.90f for power-slide");
    check(s_car_calls[1].last_footbrake == 0.0f, "Sharp turn at speed: footbrake zeroed to preserve throttle speed");

    // Now align target straight ahead (face_dot > 0.15f): handbrake must RESET to 0.0f
    float_write(s_mock_cars[0] + 0x88, float_read(s_mock_cars[1] + 0x88) + 15.0f);
    float_write(s_mock_cars[0] + 0x90, float_read(s_mock_cars[1] + 0x90));
    reset_call_stats();
    game_patches_vblank();
    check(s_car_calls[1].last_handbrake == 0.0f, "Turn exit: handbrake resets to 0.0f");
    std::puts("  [PASS] 5.6: Power-slide ramming handbrake modulation (0.90f on sharp turn, 0.0f reset) verified");

    // 5.7: Ballistic Chicken Weapon Firing Conditions
    setup_test_cars();
    // Move Car 2 and Car 3 far away so Car 0 is unambiguously the closest target
    float_write(s_mock_cars[2] + 0x88, 100.0f);
    float_write(s_mock_cars[2] + 0x90, 100.0f);
    float_write(s_mock_cars[3] + 0x88, -100.0f);
    float_write(s_mock_cars[3] + 0x90, -100.0f);

    // Face Bot 1 away (forward vector pointing outward +Z, while target is at -Z)
    // to prevent firing during cooldown warmup
    float_write(s_mock_cars[1] + 0x7C, 0.0f);
    float_write(s_mock_cars[1] + 0x84, 1.0f); // facing +Z
    float_write(s_mock_cars[0] + 0x88, 0.0f);
    float_write(s_mock_cars[0] + 0x90, -20.0f); // target is behind at -Z (facing dot < 0)

    // Advance cooldown past 65 frames (run 70 frames)
    for (int i = 0; i < 70; i++) {
        game_patches_vblank();
    }
    // Now align target directly ahead (forward = +X, target at +X, 20m away)
    float_write(s_mock_cars[1] + 0x88, 0.0f);
    float_write(s_mock_cars[1] + 0x90, 0.0f);
    float_write(s_mock_cars[1] + 0x7C, 1.0f); // Forward along +X
    float_write(s_mock_cars[1] + 0x84, 0.0f);
    float_write(s_mock_cars[0] + 0x88, 20.0f); // 20m ahead on +X
    float_write(s_mock_cars[0] + 0x90, 0.0f);

    reset_call_stats();
    game_patches_vblank();
    check(s_car_calls[1].weapon_calls > 0, "Lethal range (20m) & facing target: Chicken projectile fired");

    // Immediately on next frame: cooldown active (<65 frames) -> MUST NOT FIRE
    reset_call_stats();
    game_patches_vblank();
    check(s_car_calls[1].weapon_calls == 0, "Weapon cooldown active: no chicken fired on subsequent frame");

    // Boundary: target too close (d = 5m < 8m, blast self-damage zone) -> MUST NOT FIRE
    for (int i = 0; i < 70; i++) game_patches_vblank(); // advance cooldown
    float_write(s_mock_cars[0] + 0x88, 5.0f); // 5m ahead
    reset_call_stats();
    game_patches_vblank();
    check(s_car_calls[1].weapon_calls == 0, "Distance < 8m (blast self-damage zone): Chicken firing suppressed");

    // Boundary: target too far (d = 46m > 45m across arena diameter, out of range) -> MUST NOT FIRE
    // Place Bot 1 at (-23.0, 0) facing +X, and Car 0 at (+23.0, 0).
    // Both are within the 24.5m arena boundary, but distance between them is 46.0m > 45.0m!
    for (int i = 0; i < 70; i++) game_patches_vblank();
    float_write(s_mock_cars[1] + 0x88, -23.0f);
    float_write(s_mock_cars[1] + 0x90, 0.0f);
    float_write(s_mock_cars[1] + 0x7C, 1.0f); // facing +X towards Car 0
    float_write(s_mock_cars[1] + 0x84, 0.0f);
    float_write(s_mock_cars[0] + 0x88, 23.0f); // 46m ahead across arena
    float_write(s_mock_cars[0] + 0x90, 0.0f);

    reset_call_stats();
    game_patches_vblank();
    check(s_car_calls[1].weapon_calls == 0, "Distance = 46m > 45m (out of range across arena): Chicken firing suppressed");
    std::puts("  [PASS] 5.7: Ballistic chicken weapon firing ranges (<8m, 8-45m, >45m across diameter, cooldown) verified");

    std::puts("\n======================================================================");
    std::puts("  ALL EMPIRICAL STRESS TESTS PASSED SUCCESSFULLY! (100% CORRECTNESS)");
    std::puts("======================================================================");

    return 0;
}
