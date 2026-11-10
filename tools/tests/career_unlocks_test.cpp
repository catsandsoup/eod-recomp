// tools/tests/career_unlocks_test.cpp - the Career unlocks (src/hle_career.cpp): gate table, money and
// rank floors, Eve window, Troy's rep_required, rank clamp and display, free swap, the paged dealer.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/cpu.h"
#include "core/log.h"
#include "hle_career.h"
#include "career_upgrades.h"
#include "platform/platform.h"

EEState g_ee{};
uint8_t *g_ram = nullptr;

extern "C" uint32_t ee_read32_slow(uint32_t) { std::abort(); }
extern "C" void ee_write32_slow(uint32_t, uint32_t) { std::abort(); }
extern "C" uint8_t ee_read8_slow(uint32_t a) { return a < EE_RAM_SIZE ? g_ram[a] : 0; }
extern "C" void ee_write8_slow(uint32_t a, uint8_t v) { if (a < EE_RAM_SIZE) g_ram[a] = v; }
extern "C" void *ee_ptr(uint32_t a) { return a < EE_RAM_SIZE ? (g_ram + a) : nullptr; }
extern "C" void ee_call(uint32_t) {}
bool g_log_enabled[LOG_CAT_COUNT] = {};
void log_write(LogCat, const char *, ...) {}
std::string g_game_root;
static std::string s_banner;
void platform_show_hud_banner(const char *text, int) { s_banner = text; }
bool platform_find_file(const char *, const char *, char *, size_t) { return false; }
void platform_get_pad(int, HostPadState *) {}

// the native functions the wrappers sit on
static int s_orig_calls[8];
static int32_t s_orig_result;
extern "C" void f_001C31A0_orig() {}
extern "C" void f_001D2DC8_orig() {}
extern "C" void f_002E5200_orig() { s_orig_calls[0]++; g_ee.r[2].sd[0] = 0; }
extern "C" void f_002E6C78_orig() { s_orig_calls[1]++; g_ee.r[2].sd[0] = s_orig_result; }
extern "C" void f_002E0F10_orig() { s_orig_calls[2]++; g_ee.r[2].sd[0] = MEM_LW(MEM_LW(0x3D9394)); }
extern "C" void f_002E5758_orig() { s_orig_calls[3]++; }
extern "C" void f_002E3BC8_orig() { s_orig_calls[4]++; g_ee.r[2].sd[0] = 123; }
extern "C" void f_002E3E70_orig() { s_orig_calls[4]++; g_ee.r[2].sd[0] = 54; }
extern "C" void f_002E35F0_orig() {
    s_orig_calls[5]++;
    const uint32_t out = g_ee.r[4].uw[0];
    for (int i = 0; i < 5; i++) { MEM_SW(0x4AB130 + 4 * i, 0); MEM_SW(out + 4 * i, 800); }  // the native pick
    MEM_SW(0x3D93D0, 5);
}
extern "C" void f_0018AAD0_orig() { s_orig_calls[6]++; g_ee.r[2].sd[0] = 1; }
extern "C" void f_001741C0_orig() { g_ee.r[2].ud[0] = MEM_LW(0x3CDA24) != 6 ? 1 : 0; }
static int s_info_calls;
extern "C" void f_003702F0_orig() { s_info_calls++; }
#include <functional>
static int s_buyscreen_orig_calls;
static std::function<void()> s_buyscreen_hook;
extern "C" void f_0036E9C0_orig() {
    s_buyscreen_orig_calls++;
    if (s_buyscreen_hook) s_buyscreen_hook();
}
static int s_get_slot_orig_calls;
extern "C" void f_00370DD8_orig() { s_get_slot_orig_calls++; g_ee.r[2].sd[0] = 0; }
extern "C" void f_002DFDF8_orig() { g_ee.r[2].sd[0] = 0x01; }
extern "C" void f_002DED18_orig() { g_ee.r[2].sd[0] = 4; }
extern "C" void f_002E1BB0_orig() { g_ee.r[2].sd[0] = 4; }
extern "C" void f_002E1BC0_orig() { g_ee.r[2].sd[0] = 1; }
extern "C" void f_002E1380_orig() { g_ee.r[2].sd[0] = 1; }
extern "C" void f_0016F4E8_orig() {}
extern "C" void f_0016F748_orig() {}
extern "C" void f_0016F4E8();
extern "C" void f_0016F748();
extern "C" void f_002DFDF8();
extern "C" void f_002DED18();
extern "C" void f_002E1BB0();
extern "C" void f_002E1BC0();
extern "C" void f_002E1380();
extern "C" void f_002E5200();
extern "C" void f_002E6C78();
extern "C" void f_002E0F10();
extern "C" void f_002E5758();
extern "C" void f_002E3BC8();
extern "C" void f_002E3E70();
extern "C" void f_002E35F0();
extern "C" void f_001741C0();
extern "C" void f_003702F0();
extern "C" void f_0036E9C0();
extern "C" void f_00370DD8();

static int failures = 0;
static void check(bool ok, const char *what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); failures++; }
}

static void career_state(uint32_t rank, uint32_t money) {
    MEM_SW(0x3D9384, 0x10000);  // CareerSaveInfo: +0 active
    MEM_SW(0x10000, 1);
    MEM_SW(0x3D9394, 0x10100);  // CDBResSaveInfo
    MEM_SW(0x10100, rank);
    MEM_SW(0x3D9398, 0x10200);  // CDBupgr
    MEM_SW(0x10204, money);
}

int main() {
    std::vector<uint8_t> ram(EE_RAM_SIZE, 0);
    g_ram = ram.data();

    // ---- gate table
    g_config = Config{};
    check(g_config.career_money && g_config.career_rank && g_config.career_open && g_config.career_all_cars &&
          g_config.career_free_swap && g_config.career_killable, "every Career unlock is on by default");
    MEM_SW(0x41EFF8, 79); MEM_SW(0x41EFFC, 49); MEM_SW(0x41F000, 0);
    career_vblank(0);
    check(MEM_LW(0x41EFF8) == 99 && MEM_LW(0x41EFFC) == 99 && MEM_LW(0x41F000) == 0, "gates open, champion lock-out kept");
    g_config.career_open = false;
    career_vblank(0);
    check(MEM_LW(0x41EFF8) == 79 && MEM_LW(0x41EFFC) == 49 && MEM_LW(0x41F000) == 0, "OpenCareer off restores the native gates");
    g_config.career_open = true;

    // ---- money and rank floors only while the free-roam drive runs
    career_state(99, 75);
    career_vblank(0);
    check(MEM_LW(0x10204) == 75 && MEM_LW(0x10100) == 99, "no floors outside the free-roam world");
    MEM_SW(0x3CF19C, 0x8000226);  // the free-roam world is running
    career_vblank(0);
    check(MEM_LW(0x10204) == 10000, "money is raised to the floor");
    check(MEM_LW(0x10100) == 1, "rank goes to the top of the list (stored 1)");
    MEM_SW(0x10204, 25000);
    career_vblank(0);
    check(MEM_LW(0x10204) == 25000, "money above the floor is never lowered");
    MEM_SW(0x10204, 9990);  // a purchase
    career_vblank(0);
    check(MEM_LW(0x10204) == 10000, "after a purchase the floor is back");
    career_state(0, 5);  // a Career already won: left alone
    career_vblank(0);
    check(MEM_LW(0x10100) == 0, "a champion is not demoted");
    MEM_SW(0x10000, 0);  // no Career active
    MEM_SW(0x10204, 5);
    career_vblank(0);
    check(MEM_LW(0x10204) == 5, "no money without an active Career");
    career_state(50, 5);
    g_config.career_money = false; g_config.career_rank = false;
    career_vblank(0);
    check(MEM_LW(0x10204) == 5 && MEM_LW(0x10100) == 50, "floors off: nothing changes");
    g_config.career_money = true; g_config.career_rank = true;
    MEM_SW(0x3CF19C, 0x18000224);  // a Battle

    // ---- Eve windows: open when the native window says so or the Eve is not played yet
    std::memset(s_orig_calls, 0, sizeof s_orig_calls);
    career_state(1, 0);
    MEM_SW(0x4AB150, 3);
    for (int i = 0; i < 3; i++) { MEM_SW(0x4AB158 + 12 * i, 5 + i); g_ram[0x10100 + 0x118 + 5 + i] = 0xFF; }
    g_ee.r[4].uw[0] = 1; g_ee.r[5].uw[0] = 0;
    f_002E5200();
    check(g_ee.r[2].sd[0] == 1 && s_orig_calls[0] == 1, "an Eve not played yet is open at any rank");
    g_ram[0x10100 + 0x118 + 5] = 0x03;  // Eve 5 played
    g_ee.r[5].uw[0] = 0; f_002E5200();
    check(g_ee.r[2].sd[0] == 0, "a played Eve is closed again outside its native window (others are still to play)");
    g_ee.r[5].uw[0] = 1; f_002E5200();
    check(g_ee.r[2].sd[0] == 1, "the next Eve comes up");
    g_ram[0x10100 + 0x118 + 6] = 0x01; g_ram[0x10100 + 0x118 + 7] = 0x02;
    g_ee.r[5].uw[0] = 0; f_002E5200();
    check(g_ee.r[2].sd[0] == 1, "every Eve played: all open again (replay)");
    g_config.career_open = false;
    g_ee.r[5].uw[0] = 0; f_002E5200();
    check(g_ee.r[2].sd[0] == 0, "OpenCareer off: the native window test only");
    g_config.career_open = true;

    // ---- Troy's Place: rep_required (record + 0x2C) cleared, other fields kept
    MEM_SW(0x50000, 3);  // count
    for (int i = 0; i < 3; i++) { MEM_SW(0x51000 + 0x44 * i + 0x2C, 100 + i); MEM_SW(0x51000 + 0x44 * i + 0x30, 77); }
    g_ee.r[4].uw[0] = 0x50000; g_ee.r[6].uw[0] = 0x51000; g_ee.r[7].uw[0] = 9;
    f_002E5758();
    check(MEM_LW(0x5102C) == 0 && MEM_LW(0x5102C + 0x44) == 0 && MEM_LW(0x5102C + 0x88) == 0, "rep_required cleared");
    check(MEM_LW(0x51030) == 77, "other challenge fields untouched");
    check(MEM_LW(0x5102C + 0xCC) == 0, "records past the count untouched");

    // ---- rank after an Eve: never the champion
    career_state(1, 0);
    s_orig_result = 0;
    f_002E6C78();
    check(g_ee.r[2].sd[0] == 1, "winning at rank 1 keeps the player on the list");
    s_orig_result = 37;
    f_002E6C78();
    check(g_ee.r[2].sd[0] == 37, "other ranks pass through");
    g_config.career_rank = false; s_orig_result = 0;
    f_002E6C78();
    check(g_ee.r[2].sd[0] == 0, "CareerRank off: native champion");
    g_config.career_rank = true;
    career_state(0, 0);
    f_002E6C78();
    check(g_ee.r[2].sd[0] == 0, "an existing champion stays one");

    // ---- rank shown: only display callers see 0 (1st)
    career_state(1, 0);
    g_ee.r[31].uw[0] = 0x170DDC;
    f_002E0F10();
    check(g_ee.r[2].sd[0] == 0, "the HUD shows 1st");
    g_ee.r[31].uw[0] = 0x2E6C90;  // the rank update
    f_002E0F10();
    check(g_ee.r[2].sd[0] == 1, "game logic sees the real rank");
    career_state(5, 0);
    g_ee.r[31].uw[0] = 0x170DDC;
    f_002E0F10();
    check(g_ee.r[2].sd[0] == 5, "only stored rank 1 is shown as 1st");

    // ---- free swap
    f_002E3BC8();
    check(g_ee.r[2].sd[0] == 0, "no depreciation");
    MEM_SW(0x4AB0E0 + 0x14 * 1 + 8, 60);   // the second owned car cost $60
    g_ee.r[4].uw[0] = 1; f_002E3E70();
    check(g_ee.r[2].sd[0] == 60, "used value is the full price");
    g_config.career_free_swap = false;
    f_002E3BC8();
    check(g_ee.r[2].sd[0] == 123, "FreeSwap off: native depreciation");
    g_ee.r[4].uw[0] = 1; f_002E3E70();
    check(g_ee.r[2].sd[0] == 54, "FreeSwap off: native 90 % used value");
    g_config.career_free_swap = true;

    // ---- dealer: 6 chassis (Phantom and the Cup car among them), 3 paint jobs each
    const uint32_t chassis = 0x60000, cars = 0x61000, names = 0x62000;
    const char *nm[6] = {"Top", "Moth", "Phantom", "Rocket", "Cupcar", "Algar"};
    const uint32_t price[6] = {60, 85, 1200, 10000, 1200, 120};
    for (int c = 0; c < 6; c++) {
        MEM_SW(chassis + 8 * c, names + 16 * c);
        MEM_SW(chassis + 8 * c + 4, price[c]);
        std::memcpy(g_ram + names + 16 * c, nm[c], std::strlen(nm[c]) + 1);
        for (int k = 0; k < 3; k++) {
            const uint32_t i = c * 3 + k;
            MEM_SW(cars + 0x14 * i, i);
            MEM_SW(cars + 0x14 * i + 4, 800 + i);
            MEM_SW(cars + 0x14 * i + 8, price[c]);
            MEM_SW(cars + 0x14 * i + 0xC, names + 16 * c);
        }
    }
    MEM_SW(0x3D93B8, chassis); MEM_SW(0x3D93BC, 6);
    MEM_SW(0x3D93C0, cars); MEM_SW(0x3D93C4, 18);
    const uint32_t out = 0x63000;
    auto visit = [&](std::vector<uint32_t> &veh) {
        g_ee.r[4].uw[0] = out;
        f_002E35F0();
        veh.clear();
        for (int i = 0; i < 5; i++) {
            veh.push_back(MEM_LW(out + 4 * i));
            check(MEM_LW(cars + 0x14 * MEM_LW(0x4AB130 + 4 * i) + 4) == MEM_LW(out + 4 * i), "slot index and vehicle agree");
        }
    };
    std::vector<uint32_t> v;
    visit(v);
    check(MEM_LW(0x3D93D0) == 5, "five dealer slots");
    check(v[0] == 800 + 0 * 3 && v[1] == 800 + 1 * 3 && v[2] == 800 + 5 * 3 && v[3] == 800 + 2 * 3 && v[4] == 800 + 4 * 3,
          "page 1: cheapest first (Top, Moth, Algar, then the two 1200 cars in list order)");
    visit(v);                                       // the next visit: the next page
    bool phantom = false, cup = false, rocket = false;
    for (int round = 0; round < 3; round++) {
        for (uint32_t x : v) {
            const uint32_t chassis_idx = (x - 800) / 3;
            phantom |= chassis_idx == 2; cup |= chassis_idx == 4; rocket |= chassis_idx == 3;
        }
        visit(v);
    }
    check(phantom && cup && rocket, "Phantom, the Cup car and the dearest car turn up while paging");

    // the dealer screen says which page it is, once per visit
    f_003702F0();                                   // the dealer's car list is on screen
    career_vblank(0);
    check(s_banner.find("PAGE") != std::string::npos && s_banner.find("OF 2") != std::string::npos, "the dealer says which page it is");
    s_banner.clear();
    f_003702F0(); career_vblank(0);
    check(s_banner.empty(), "...once");
    s_info_calls = 0;
    f_003702F0();
    check(s_info_calls == 1, "the selection callback still runs the native code");

    // ---- live dealer Buy screen paging (L2 / R2, L1 / R1, Q / E, 1 / 3, Left / Right)
    const uint32_t yard_table = 0x64000;
    MEM_SW(0x495D80, yard_table);
    MEM_SW(yard_table + 0x1774, 5);
    for (int i = 0; i < 5; i++)
        MEM_SW(yard_table + i * 0x12C + 0x120, 800 + i);

    const uint32_t simulated_sp = 0x50000;
    const uint32_t simulated_widgets[5] = {0x52000, 0x52020, 0x52040, 0x52060, 0x52080};
    MEM_SW(simulated_sp + 0x81C, simulated_widgets[0]);
    MEM_SW(simulated_sp + 0x884, simulated_widgets[1]);
    MEM_SW(simulated_sp + 0x8C4, simulated_widgets[2]);
    MEM_SW(simulated_sp + 0x7E4, simulated_widgets[3]);
    MEM_SW(simulated_sp + 0x824, simulated_widgets[4]);

    s_buyscreen_hook = [&]() {
        // While inside f_0036E9C0 MenuRun loop, live page cycling updates catalog without stack corruption:
        career_vblank(PAD_R2);
        f_003702F0();
        check(s_banner.find("PAGE 2 OF 2") != std::string::npos, "live paging: L2/R2 advanced to page 2");
        check(MEM_LW(0x4AB130) != 0, "dealer slots updated with new car index");

        // Test get_slot_for_car with vehicle in yard vs vehicle off-page
        g_ee.r[4].uw[0] = 800; // in yard
        s_get_slot_orig_calls = 0;
        f_00370DD8();
        check(s_get_slot_orig_calls == 1, "in-yard vehicle delegates to orig");

        g_ee.r[4].uw[0] = 999; // off-page car
        MEM_SW(0x004DD388 + 0x280, 3); // UI row 3 selected
        s_get_slot_orig_calls = 0;
        f_00370DD8();
        check(s_get_slot_orig_calls == 0 && g_ee.r[2].sd[0] == 3, "off-page vehicle returns UI slot 3 without panic");

        // Verify f_0016F748 and f_0016F4E8 prevent 'car 10 not found' panic:
        const uint32_t home_obj = 0x65000;
        MEM_SW(0x3CDA04, home_obj);
        for (int s = 0; s < 5; s++) MEM_SW(home_obj + 0x4BC + 4 * s, 5 + s); // Page 0 had cars 5..9
        g_ee.r[4].uw[0] = home_obj;
        g_ee.r[5].uw[0] = 10; // Page 1 car 10 queried
        f_0016F748();
        check(g_ee.r[2].sd[0] == 0, "f_0016F748 maps car 10 to slot 0 without panic");

        g_ee.r[4].uw[0] = 10;
        f_0016F4E8();
        check(g_ee.r[2].sd[0] == 0, "f_0016F4E8 maps car 10 to slot 0 without panic");

        // Live page cycle backward via PAD_L1 (gamepad L1 / keyboard Q)
        career_vblank(PAD_L1);
        f_003702F0();
        check(s_banner.find("PAGE 1 OF 2") != std::string::npos, "live paging: L1/Q cycled back to page 1");

        // Live page cycle forward via PAD_RIGHT (gamepad Right / keyboard Right arrow)
        career_vblank(PAD_RIGHT);
        f_003702F0();
        check(s_banner.find("PAGE 2 OF 2") != std::string::npos, "live paging: Right arrow cycled to page 2");

        // Live page cycle backward via PAD_LEFT (gamepad Left / keyboard Left arrow)
        career_vblank(PAD_LEFT);
        f_003702F0();
        check(s_banner.find("PAGE 1 OF 2") != std::string::npos, "live paging: Left arrow cycled to page 1");
    };

    g_ee.r[29].uw[0] = simulated_sp + 0x980;
    f_0036E9C0();
    check(s_buyscreen_orig_calls == 1, "Buy screen init invoked and SP tracked");
    s_buyscreen_hook = nullptr;

    g_config.career_all_cars = false;
    std::memset(s_orig_calls, 0, sizeof s_orig_calls);
    visit(v);
    check(s_orig_calls[5] == 1 && v[0] == 800 && MEM_LW(0x4AB130 + 16) == 0, "AllCars off: the native five");

    // ---- killable challengers: AI cars bypass the 0.9f free-roam damage clamp in f_0018CA98
    const uint32_t ai_car = 0x70000, player_car = 0x72000;
    MEM_SW(ai_car + 0x1E0, 0x51);
    MEM_SW(player_car + 0x1E0, 0x50);
    MEM_SW(0x3CDA24, 1);  // Career free-roam / challenge active
    g_ee.r[18].uw[0] = ai_car;
    g_ee.r[31].uw[0] = 0x0018D3F8;
    f_001741C0();
    check(g_ee.r[2].ud[0] == 0, "AI challenger bypasses the 0.9f damage clamp in f_0018CA98");
    g_ee.r[31].uw[0] = 0x0018D7EC;
    f_001741C0();
    check(g_ee.r[2].ud[0] == 0, "AI challenger bypasses the instant-wreck gate in f_0018CA98");
    g_ee.r[31].uw[0] = 0x0018AB2C;  // f_0018AAD0 (can_damage)
    f_001741C0();
    check(g_ee.r[2].ud[0] == 1, "can_damage still sees Career free roam active");
    g_ee.r[18].uw[0] = player_car;
    g_ee.r[31].uw[0] = 0x0018D3F8;
    f_001741C0();
    check(g_ee.r[2].ud[0] == 1, "player car keeps free-roam wreck protection");
    g_config.career_killable = false;
    g_ee.r[18].uw[0] = ai_car;
    g_ee.r[31].uw[0] = 0x0018D3F8;
    f_001741C0();
    check(g_ee.r[2].ud[0] == 1, "KillableChallengers off: native 0.9f clamp restored");
    g_config.career_killable = true;
    // Wrecked Highway challenger retires the challenge prompt (state 1 -> 6)
    MEM_SW(0x3CF19C, 0x8000226);
    MEM_SW(0x3CDA78, 0x3CDA48);
    MEM_SW(0x3CDA48 + 8, 0);
    MEM_SW(0x447458, ai_car);
    MEM_SW(ai_car + 0x1BEC, 0x3F800000);  // 1.0f = wrecked
    career_vblank(0);
    check(MEM_LW(0x3CDA24) == 6, "wrecked Highway challenger retires the challenge prompt");

    // ---- Vehicle Upgrades & Performance Decompilation verification
    // 1. Bit-exact data structure layouts
    check(sizeof(eod::CDBPartEntry) == 4, "CDBPartEntry size == 4");
    check(sizeof(eod::CarTuningBlock) == 0x4C, "CarTuningBlock size == 0x4C (76 B)");
    check(sizeof(eod::CarDamagePanelsState) == 0x2AC, "CarDamagePanelsState size == 0x2AC (684 B)");
    check(sizeof(eod::CDBGarageSlot) == 0x300, "CDBGarageSlot size == 0x300 (768 B)");
    check(sizeof(eod::CDBupgr) == 0xC68, "CDBupgr size == 0xC68 (3176 B)");
    check(sizeof(eod::CareerSaveInfo) == 0xE7C, "CareerSaveInfo size == 0xE7C (3708 B)");

    // 2. Universal Parts: class unlock
    g_config.career_universal_parts = true;
    f_002DFDF8();
    check(g_ee.r[2].sd[0] == (int64_t)(int32_t)0xFFFFFFFF, "UniversalParts returns 0xFFFFFFFF universal mask");
    check(eod::AutoShopIsPartCompatible(0, 15) && eod::AutoShopIsPartCompatible(2, 5), "any part compatible with any class");

    // 3. Valuation & depreciation
    g_config.career_free_swap = false;
    check(eod::CareerGetOwnedCarResaleValue(1000) == 900, "chassis resale is 90%");
    check(eod::CareerCalcTradeInDepreciation(1000, 200, 0.8f) == 240, "depreciation at 80% health is 20% of value");
    g_config.career_free_swap = true;
    check(eod::CareerGetOwnedCarResaleValue(1000) == 1000, "FreeSwap pays 100% chassis value");
    check(eod::CareerCalcTradeInDepreciation(1000, 200, 0.5f) == 0, "FreeSwap eliminates depreciation");

    // 4. Expanded Garage (16 cars)
    eod::CareerUpgrades_Init();
    g_config.career_garage_expansion = true;
    check(eod::Garage_GetCapacity() == 16, "expanded garage capacity is 16");
    career_state(50, 5000);
    // Add 2 native cars (slots 0, 1) and 3 extended cars (slots 4, 5, 6)
    MEM_SW(0x1020C, 2); // 2 native cars
    check(eod::Garage_AddCar(780), "adds car into extended slot 4");
    check(eod::Garage_AddCar(781), "adds car into extended slot 5");
    check(eod::Garage_AddCar(782), "adds car into extended slot 6");
    check(eod::Garage_GetCarCount() == 5, "total garage count is 5 cars (2 native + 3 ext)");
    check(eod::Garage_GetTotalPages() == 2, "5 cars occupy 2 garage pages");
    eod::Garage_CyclePage(1);
    check(eod::Garage_GetActivePage() == 1, "garage cycles to page 1");
    check(eod::Garage_GetSlotOnCurrentPage(0) == 4, "button 0 on page 1 maps to slot 4");

    // Tuning block retrieval for extended slot (f_002E1380)
    g_ee.r[4].uw[0] = 4; // Slot 4
    g_ee.r[5].uw[0] = 0x20000; // Destination buffer
    f_002E1380();
    check(g_ee.r[2].sd[0] == 1, "f_002E1380 returns tuning block for extended slot 4 without panic");

    // 5. Multi-Stage Upgrades & Category Part Swapping
    g_config.career_multistage_upgrades = true;
    const uint32_t cdb = 0x10200;
    MEM_SW(cdb + 4, 2000); // $2000 cash
    MEM_SW(cdb + 0x10, 0); // 0 installed parts
    // Install initial Stage 1 Engine part (key 1, category 0)
    check(eod::CDBupgr_AddPart(0, 0, 1, 100), "CDBupgr_AddPart installs initial category 0 part");
    uint32_t installed_key = 0;
    check(eod::CDBupgr_GetCarPart(0, 0, &installed_key) && installed_key == 1, "installed part key is 1");
    // Swap to Stage 3 Engine part (key 2, category 0) with multi-stage
    int32_t res = eod::AutoShopPurchasePartWithMultiStage(0, 2);
    check(res == 4, "AutoShopPurchasePartWithMultiStage successfully swaps category 0 part");
    check(eod::CDBupgr_GetCarPart(0, 0, &installed_key) && installed_key == 2, "category 0 part updated to key 2");

    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("career unlocks: ok");
    return 0;
}
