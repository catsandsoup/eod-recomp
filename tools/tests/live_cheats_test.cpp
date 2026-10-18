// tools/tests/live_cheats_test.cpp - Verification for live controller cheat combos,
// feats unlocking out of the box, and Battle Mode Eve contest injection.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/config.h"
#include "core/cpu.h"
#include "core/log.h"
#include "platform/platform.h"

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

// Mock native functions
extern "C" void f_001244A8();
extern "C" void f_001244A8_orig() {}
extern "C" void f_0018AFE0_orig() {}
extern "C" void f_0017DA58_orig() {}
extern "C" void f_0018F2A8_orig() {}
extern "C" void f_0034D328_orig() {}
extern "C" void f_00179A08() {}
extern "C" void f_0018AA50() {}
extern "C" void f_0018AA80() {}
extern "C" void f_0018AA70() {}
extern "C" void f_0018AA68() {}
extern "C" void f_0017DE78() {}
extern "C" void f_0017E5C8() {}

void game_patches_vblank();

// Mock platform hooks
static HostPadState s_mock_pad{};
void platform_get_pad(int, HostPadState *out) { *out = s_mock_pad; }
void platform_get_controller(int, HostPadState *out) { *out = s_mock_pad; }
static int s_vibration_calls = 0;
void platform_set_vibration(int, uint8_t, uint8_t) { s_vibration_calls++; }
static int s_haptic_pulse_calls = 0;
void platform_haptic_pulse(int, int, uint8_t, uint8_t) { s_haptic_pulse_calls++; }
static int s_audio_feedback_calls = 0;
void platform_audio_feedback(bool) { s_audio_feedback_calls++; }
static std::string s_last_status;
void platform_set_status(const char *text) { s_last_status = text ? text : ""; }
static std::string s_last_hud_banner;
void platform_show_hud_banner(const char *text, int) { s_last_hud_banner = text ? text : ""; }
static bool s_mod_overlay_visible = false;
void platform_set_mod_overlay(bool visible) { s_mod_overlay_visible = visible; }
void platform_set_aspect(int) {}

static void check(bool cond, const char *msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        std::exit(1);
    }
}

int main() {
    std::vector<uint8_t> ram(EE_RAM_SIZE, 0);
    g_ram = ram.data();

    // ------------------------------------------------------------ 1. Out of the box config
    Config cfg_default;
    check(cfg_default.unlock_all == true, "unlock_all is true out of the box");
    check(cfg_default.career_cheats == true, "career_cheats is true out of the box");
    check(cfg_default.sp_battle_mode == true, "sp_battle_mode is true out of the box");
    check(cfg_default.turbo_scale > 1.0f, "turbo_scale is enhanced out of the box");
    check(cfg_default.chicken_force_scale > 1.0f, "chicken_force_scale is enhanced out of the box");
    check(cfg_default.chicken_radius_scale > 1.0f, "chicken_radius_scale is enhanced out of the box");

    // ------------------------------------------------------------ 2. Feats out of the box
    g_config = cfg_default;
    game_patches_vblank();
    check(MEM_LW(0x003D18E0) == 0xFFFFFFFF, "0x003D18E0 feats bitmask unlocked out of the box");
    check(MEM_LW(0x003D18E4) == 0xFFFFFFFF, "0x003D18E4 feat cheats bitmask unlocked out of the box");

    // ------------------------------------------------------------ 3. Battle Mode Eve Injection
    // Set active mode: 0x12F = Action mode
    MEM_SW(0x003C4C04, 0x12F);
    // Number of eves = 2
    MEM_SW(0x003C4CF4, 2);
    // Eve 0 at 0x003C4D08, cur_contests at +0x70 = 4 (e.g. 4 events)
    MEM_SW(0x003C4D08 + 0x70, 4);
    // Eve 1 at 0x003C4D08 + 0x84, cur_contests at +0x70 = 3
    MEM_SW(0x003C4D08 + 0x84 + 0x70, 3);
    // Total contests initially = 7
    MEM_SW(0x003C4CF8, 7);

    // Call Eve loader hook
    f_001244A8();

    // Contest count should be 7 + 2 = 9
    check(MEM_LW(0x003C4CF8) == 9, "two Battle contests appended across the 2 eves");
    // Eve 0 should now have 5 contests
    check(MEM_LW(0x003C4D08 + 0x70) == 5, "Eve 0 now has 5 contests (Battle mode visible in Event 1 of 5 screen)");
    // Eve 1 should now have 4 contests
    check(MEM_LW(0x003C4D08 + 0x84 + 0x70) == 4, "Eve 1 now has 4 contests");

    // Inspect injected contest 7 (in Eve 0) at 0x003C61A8 + 7 * 0x28
    uint32_t c7 = 0x003C61A8 + 7 * 0x28;
    check(MEM_LW(c7 + 0x00) == 7, "contest 7: global index is 7");
    check(MEM_LW(c7 + 0x04) == 0, "contest 7: eve index is 0 (belongs to Eve 0)");
    check(MEM_LW(c7 + 0x08) == 0x18000224, "contest 7: event_type is 0x18000224 (BATTLE)");
    check(MEM_LW(c7 + 0x0C) == 1, "contest 7: laps is 1");
    check(MEM_LW(c7 + 0x10) == 4, "contest 7: max_cars is 4 (BattleMaster struct bound)");
    check(MEM_LW(c7 + 0x14) == 0xFFFFFFFF, "contest 7: terrain_mask is 0xFFFFFFFF");
    check(MEM_LW(c7 + 0x18) == 1, "contest 7: rounds is 1");
    check(MEM_LW(c7 + 0x1C) == 500, "contest 7: purse points is 500");
    check(MEM_LW(c7 + 0x24) == 1, "contest 7: enabled flag is 1 (selectable in event menu)");

    // Inspect injected contest 8 (in Eve 1)
    uint32_t c8 = 0x003C61A8 + 8 * 0x28;
    check(MEM_LW(c8 + 0x00) == 8, "contest 8: global index is 8");
    check(MEM_LW(c8 + 0x04) == 1, "contest 8: eve index is 1 (belongs to Eve 1)");
    check(MEM_LW(c8 + 0x08) == 0x18000224, "contest 8: event_type is 0x18000224 (BATTLE)");
    check(MEM_LW(c8 + 0x24) == 1, "contest 8: enabled flag is 1");

    // Re-calling f_001244A8 must be idempotent (no duplicate insertions)
    f_001244A8();
    check(MEM_LW(0x003C4CF8) == 9, "idempotent: does not duplicate Battle contests if already present");

    // ------------------------------------------------------------ 4. Live Controller Combos
    s_mock_pad.connected = true;

    // Combo 1: L1 + R1 + D-Pad UP -> Toggle Chicken Fight
    g_config.cheat_chickens = false;
    s_mock_pad.buttons = PAD_L1 | PAD_R1 | PAD_UP;
    game_patches_vblank();
    check(g_config.cheat_chickens == true, "L1 + R1 + D-Pad Up toggles Chicken Fight ON");
    check((MEM_LW(0x003C4190) & 0x19) == 0x19, "0x003C4190 has bits 0, 3, 4 set (Master + Weapon + Chicken)");
    check(!s_last_hud_banner.empty(), "HUD banner displayed on chicken toggle");
    check(s_mod_overlay_visible == true, "Mod overlay visible while L1+R1 held");
    check(s_haptic_pulse_calls > 0, "Haptic pulse triggered on toggle");
    check(s_audio_feedback_calls > 0, "Audio feedback triggered on toggle");
    check((game_patches_suppressed_buttons() & (PAD_L1 | PAD_R1 | PAD_UP)) == (PAD_L1 | PAD_R1 | PAD_UP),
          "Chord buttons suppressed during L1+R1 hold to prevent car steering / pause menu jump");

    // Release buttons then press L1 + R1 + D-Pad UP again -> Toggle Chicken Fight OFF
    s_mock_pad.buttons = 0;
    game_patches_vblank();
    check(game_patches_suppressed_buttons() == 0, "Suppressed buttons cleared on combo release");
    s_mock_pad.buttons = PAD_L1 | PAD_R1 | PAD_UP;
    game_patches_vblank();
    check(g_config.cheat_chickens == false, "L1 + R1 + D-Pad Up toggles Chicken Fight OFF");
    check((MEM_LW(0x003C4190) & 0x10) == 0, "0x003C4190 bit 4 cleared");

    // Combo 2: L1 + R1 + D-Pad RIGHT -> Toggle Turbo Boost
    s_mock_pad.buttons = 0;
    game_patches_vblank();
    s_mock_pad.buttons = PAD_L1 | PAD_R1 | PAD_RIGHT;
    game_patches_vblank();
    check(g_config.cheat_turbo == true, "L1 + R1 + D-Pad Right toggles Turbo Boost ON");
    check((MEM_LW(0x003C4190) & 0x03) == 0x03, "0x003C4190 has bits 0 and 1 set (Master + Turbo Boost)");

    // Combo 3: L1 + R1 + D-Pad DOWN -> Toggle Raining Chickens
    s_mock_pad.buttons = 0;
    game_patches_vblank();
    s_mock_pad.buttons = PAD_L1 | PAD_R1 | PAD_DOWN;
    game_patches_vblank();
    check((MEM_LW(0x003C4190) & 0x89) == 0x89, "0x003C4190 has bits 0, 3, 7 set (Raining Chickens)");

    // Combo 4: L1 + R1 + D-Pad LEFT -> Toggle AI Vengeance & Hardcore
    s_mock_pad.buttons = 0;
    game_patches_vblank();
    s_mock_pad.buttons = PAD_L1 | PAD_R1 | PAD_LEFT;
    game_patches_vblank();
    check((MEM_LW(0x003C4190) & 0x25) == 0x25, "0x003C4190 has bits 0, 2, 5 set (AI Vengeance & Hardcore)");

    // Combo 5: L1 + R1 + Triangle -> Chaos Mode (Both Chickens + Turbo simultaneously!)
    s_mock_pad.buttons = 0;
    game_patches_vblank();
    s_mock_pad.buttons = PAD_L1 | PAD_R1 | PAD_TRIANGLE;
    game_patches_vblank();
    check(g_config.cheat_chickens == true, "Chaos Mode sets cheat_chickens ON");
    check(g_config.cheat_turbo == true, "Chaos Mode sets cheat_turbo ON");
    check((MEM_LW(0x003C4190) & 0x1B) == 0x1B, "0x003C4190 has Master(1) + Turbo(2) + Weapon(8) + Chicken(16)");

    // Combo 6: L1 + R1 + Select -> Quick toggle Chicken Fight
    s_mock_pad.buttons = 0;
    game_patches_vblank();
    s_mock_pad.buttons = PAD_L1 | PAD_R1 | PAD_SELECT;
    game_patches_vblank();
    check(g_config.cheat_chickens == false, "L1 + R1 + Select toggles Chicken Fight OFF");

    // Shortcut: L3 + R3 (both sticks click) -> Instant toggle Chicken Fight
    s_mock_pad.buttons = 0;
    game_patches_vblank();
    s_mock_pad.buttons = PAD_L3 | PAD_R3;
    game_patches_vblank();
    check(g_config.cheat_chickens == true, "L3 + R3 toggles Chicken Fight ON");
    check((game_patches_suppressed_buttons() & (PAD_L3 | PAD_R3)) == (PAD_L3 | PAD_R3),
          "Stick clicks suppressed from game on chord to avoid unwanted horn honk");

    // Shortcut: Holding Select alone for 30 frames (500ms)
    s_mock_pad.buttons = 0;
    game_patches_vblank();
    s_mock_pad.buttons = PAD_SELECT;
    for (int frame = 0; frame < 29; frame++) {
        game_patches_vblank();
        check(g_config.cheat_chickens == true, "Chickens unchanged before 30 frames of Select hold");
        check((game_patches_suppressed_buttons() & PAD_SELECT) == 0,
              "Select not suppressed before 30 frames so tap recovery works");
    }
    // On frame 30: toggle triggers!
    game_patches_vblank();
    check(g_config.cheat_chickens == false, "Holding Select for 30 frames toggles Chicken Fight OFF");
    check((game_patches_suppressed_buttons() & PAD_SELECT) == PAD_SELECT,
          "Select suppressed once hold threshold reached to prevent accidental car reset on release");

    // Release Select
    s_mock_pad.buttons = 0;
    game_patches_vblank();
    check((game_patches_suppressed_buttons() & PAD_SELECT) == 0, "Select unsuppressed after release");

    // ------------------------------------------------------------ 5. Config Persistence Roundtrip
    Config test_cfg;
    test_cfg.unlock_all = true;
    test_cfg.sp_battle_mode = true;
    test_cfg.cheat_chickens = true;
    test_cfg.cheat_turbo = true;
    test_cfg.turbo_scale = 2.25f;
    test_cfg.chicken_force_scale = 3.0f;
    test_cfg.chicken_radius_scale = 2.5f;

    const char *test_ini = "test_live_cheats.ini";
    check(config_save(test_ini, test_cfg), "config_save writes ini");
    Config loaded_cfg;
    check(config_load(test_ini, loaded_cfg), "config_load reads ini");
    check(loaded_cfg.unlock_all == true, "persisted unlock_all");
    check(loaded_cfg.sp_battle_mode == true, "persisted sp_battle_mode");
    check(loaded_cfg.cheat_chickens == true, "persisted cheat_chickens");
    check(loaded_cfg.cheat_turbo == true, "persisted cheat_turbo");
    check(loaded_cfg.turbo_scale == 2.25f, "persisted turbo_scale");
    check(loaded_cfg.chicken_force_scale == 3.0f, "persisted chicken_force_scale");
    check(loaded_cfg.chicken_radius_scale == 2.5f, "persisted chicken_radius_scale");
    std::remove(test_ini);

    std::puts("PASS: Live cheats, controller combo triggers, and Battle mode verification succeeded.");
    return 0;
}
