#include <cstdlib>
// Parser tests for the EOD_DIRECT_START developer hook (no guest memory needed).
#include "session_start.h"
#include "core/cpu.h"
#include "core/memory.h"
#include <cstdio>
EEState g_ee{};
uint8_t *g_ram = nullptr;
[[noreturn]] void fatal(const char *, ...) { std::abort(); }  // developer EOD_FATAL_TEST path; never reached here
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
extern "C" {
uint32_t ee_read32_slow(uint32_t) { return 0; }
void ee_write32_slow(uint32_t, uint32_t) {}
// Hook entry points are not exercised here; satisfy the linker.
void f_00114358_orig() {}
void f_002A1298_orig(void) {} void f_002BEC08_orig(void) {} void f_002A75E0_orig(void) {}
void f_002C7340_orig(void) {} void f_00270C10_orig(void) {} void f_00120650_orig(void) {} void f_00120FB8_orig(void) {} void f_002A2C98(void) {}
void f_00119428(void) {} void f_00128CB0(void) {} void f_002C1338(void) {} void f_00120BE8(void) {}
void f_001252C0(void) {} void f_00123958(void) {}
}
uint64_t kernel_vblank_count() { return 0; }
extern "C" {
}
int main() {
    SessionRequest r; std::string e;
    CHECK(session_parse("mode=action;track=Anoka;event=Battle (8 AI);car=Phantom;paint=0;humans=1", r, e));
    CHECK(r.track == "Anoka" && r.event == "Battle (8 AI)" && r.car == "Phantom" && r.paint == 0);
    CHECK(session_event_name(r) == "Battle (8 AI)");
    CHECK(session_parse("track=Anoka;event=battle;ai=4;car=12", r, e) && session_event_name(r) == "Battle (4 AI)");
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
    CHECK(!session_parse("track=a;event=b;car=c;humans=2", r, e));
    CHECK(!session_parse("track=a;event=b", r, e));
    return failures ? 1 : 0;
}
