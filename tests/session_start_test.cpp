#include <cstdlib>
// Parser tests for the EOD_DIRECT_START developer hook (no guest memory needed).
#include "session_start.h"
#include "core/cpu.h"
#include "core/memory.h"
#include <cstdio>
int battle_events_begin_blank_map(uint32_t) { return -1; }
void battle_events_end_blank_map() {}
EEState g_ee{};
uint8_t *g_ram = nullptr;
[[noreturn]] void fatal(const char *, ...) { std::abort(); }  // developer EOD_FATAL_TEST path; never reached here
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
extern "C" {
uint32_t ee_read32_slow(uint32_t) { return 0; }
uint8_t ee_read8_slow(uint32_t) { std::abort(); }
void ee_write32_slow(uint32_t, uint32_t) {}
void ee_write8_slow(uint32_t, uint8_t) { std::abort(); }
// Hook entry points are not exercised here; satisfy the linker.
void f_002A3220() { std::abort(); }
void f_002B9858() { std::abort(); }
void f_002E66B8() { std::abort(); }
void f_002E66D8() { std::abort(); }
void f_002E66F8() { std::abort(); }
void f_00335110() { std::abort(); }
void f_003356D0() { std::abort(); }
void f_00114358_orig() {}
void eod_race_00114358(void) { f_00114358_orig(); }
void f_002A1298_orig(void) {} void f_002BEC08_orig(void) {} void f_002A75E0_orig(void) {}
void f_002C7340_orig(void) {} void f_00270C10_orig(void) {} void f_00120650_orig(void) {} void f_00120FB8_orig(void) {} void eod_race_00120FB8(void) { f_00120FB8_orig(); } void f_002A2C98(void) {}
void f_00119428(void) {} void f_00128CB0(void) {} void f_002C1338(void) {} void f_00120BE8(void) {}
void f_001252C0(void) {} void f_00123958(void) {}
void f_0012CC40() { std::abort(); }
void f_002E06A0() { std::abort(); }
void f_002E0AF8() { std::abort(); }
void f_002E0098() { std::abort(); }
EEFunc ee_lookup_func(uint32_t) { std::abort(); }
}
uint64_t kernel_vblank_count() { return 0; }
void arcade_mod_badges_run_menu(uint32_t, void (*run_menu)()) { run_menu(); }
extern "C" {
}
extern "C" {
void f_00270C10(void);
}
int main() {
    using session_catalog::is_online_driver_choice;
    CHECK(is_online_driver_choice({14, "Moth", 0}));
    CHECK(is_online_driver_choice({505, "Bus", 0}));
    CHECK(is_online_driver_choice({600, "Custom Hatchback", 0}));
    CHECK(!is_online_driver_choice({0, "", 0}));
    for (uint32_t id = 400; id <= 413; ++id)
        CHECK(!is_online_driver_choice({id, "Renamed attachment", 0}));
    CHECK(!is_online_driver_choice({701, "Trailer", 0}));
    CHECK(!is_online_driver_choice({701, "trailer6", 0}));
    CHECK(is_online_driver_choice({702, "Trailer Rally", 0}));
    CHECK(!is_online_driver_choice({702, "Towable Camper", 0, true}));
    SessionRequest r; std::string e;
    CHECK(session_parse("mode=action;track=Anoka;event=Battle (8 AI);car=Phantom;paint=0;humans=1", r, e));
    CHECK(r.track == "Anoka" && r.event == "Battle (8 AI)" && r.car == "Phantom" && r.paint == 0 && r.laps == -1);
    CHECK(session_event_name(r) == "Battle (8 AI)");
    CHECK(session_parse("track=Anoka;event=battle;ai=4;car=12", r, e) && session_event_name(r) == "Battle (4 AI)");
    CHECK(session_parse("track=Anoka;event=No-Rules Race;car=Phantom;laps=1", r, e) && r.laps == 1);
    CHECK(session_parse("track=Anoka;event=No-Rules Race;car=Phantom;laps=50", r, e) && r.laps == 50);
    CHECK(!session_parse("track=Anoka;event=No-Rules Race;car=Phantom;laps=0", r, e));
    CHECK(!session_parse("track=Anoka;event=No-Rules Race;car=Phantom;laps=51", r, e));
    CHECK(!session_parse("track=Anoka;event=No-Rules Race;car=Phantom;laps=1.5", r, e));
    CHECK(!session_parse("mode=career;laps=5", r, e));
    CHECK(session_parse("track=Anoka;event=No-Rules Race;car=Phantom;humans=2;laps=5", r, e) && r.humans == 2 && r.laps == 5);
    CHECK(!session_parse("track=Anoka;event=x;car=y;bogus=1", r, e));
    CHECK(!session_parse("mode=career;track=a;event=b;car=c", r, e));
    CHECK(session_parse("mode=career", r, e) && r.mode == "career" && r.place == "home" && r.eve == -1);
    CHECK(session_parse("mode=career;place=dealer", r, e) && career_place_index(r.place) == 3);
    CHECK(session_parse("mode=career;place=TJ's;eve=2", r, e) && career_place_index(r.place) == 1 && r.eve == 2);
    CHECK(session_parse("mode=career;place=here", r, e));
    CHECK(career_place_index("troy's place") == 7 && career_place_index("garage") == 4 && career_place_index("gate 2") == 6 && career_place_index("5") == 5);
    CHECK(!session_parse("mode=career;place=moon", r, e));
    CHECK(!session_parse("mode=career;eve=99", r, e));
    CHECK(!session_parse("track=a;event=b;car=c;place=home", r, e));
    CHECK(session_parse("track=a;event=b;car=c;humans=2", r, e) && r.humans == 2);
    CHECK(session_parse("track=a;event=b;car=c;humans=2;ai=4", r, e) && r.humans == 2 && r.ai == 4);
    CHECK(!session_parse("track=a;event=b;car=c;humans=5", r, e));
    CHECK(!session_parse("track=a;event=b", r, e));

    // PendingOfflineMatch lifecycle & session_ai_override
    PendingOfflineMatch off;
    off.track = "Anoka";
    off.event = "Battle";
    off.humans = 4;
    off.ai_bots = 8;
    off.laps = 5;
    off.gametype_preset = 2;
    session_set_pending_offline(off);
    CHECK(session_pending_offline().active);
    CHECK(session_ai_override() == 8);
    session_clear_pending_offline();
    CHECK(!session_pending_offline().active);
    CHECK(session_ai_override() == -1);

    // Resettable PendingCareerWarp lifecycle
    career_warp_arm(3, 5, 10);
    CHECK(career_warp_state().active && career_warp_state().target_place == 3 && career_warp_state().target_eve == 5);
    career_warp_clear();
    CHECK(!career_warp_state().active && career_warp_state().target_place == -1);

    // Mock Career & Action state unwind -> MainMenu (0x002A1298)
    std::vector<uint8_t> ram(EE_RAM_SIZE, 0);
    g_ram = ram.data();
    ee_w32(0x003C4C04, 0x130); // Career mode
    ee_w32(0x003CDA04, 0x10000); // HomeDriver active (CareerRoam)
    ee_w32(0x003D937C, 1); // New Career flag
    CHECK(session_ee_state() == EEStateKind::CareerRoam);

    SessionTransition tr;
    tr.target = TransitionTarget::Hub;
    session_request_transition(tr);
    CHECK(session_unwind_active());
    CHECK(ee_r32(0x003D937C) == 0); // Cleared for clean exit

    // Career Hub MenuRun (0x0036AB8C) must return 7 (Exit Career)
    g_ee.r[31].ud[0] = 0x0036AB8C;
    f_00270C10();
    CHECK(g_ee.r[2].sw[0] == 7);

    // Career Exit confirm dialog (0x00272030) must return -2 (Yes)
    g_ee.r[31].ud[0] = 0x00272030;
    f_00270C10();
    CHECK(g_ee.r[2].sw[0] == -2);

    // EveHasMoreEvents (0x00120FB8) must return 0 during unwind
    f_00120FB8();
    CHECK(g_ee.r[2].sw[0] == 0);

    // Simulate CareerRun teardown clearing HomeDriver (0x003CDA04) and restoring kMode=0x12F
    ee_w32(0x003CDA04, 0);
    ee_w32(0x003C4C04, 0x12F);
    // Reaching f_002A1298 clears unwind flags
    f_002A1298();
    CHECK(!session_unwind_active());
    CHECK(session_ee_state() == EEStateKind::MainMenu);

    // Synchronous Main Menu Career transition returns 15 without pulsing CROSS
    SessionTransition go_career;
    go_career.target = TransitionTarget::Career;
    session_request_transition(go_career);
    g_ee.r[31].ud[0] = 0x002A16D8;
    f_00270C10();
    CHECK(g_ee.r[2].sw[0] == 15);
    session_cancel_transition();
    g_ram = nullptr;
    return failures ? 1 : 0;
}
