// Direct session start. See session_start.h and docs RACE_SETUP_MAP.md.
//
// Guest facts used (SLUS_209.10):
//   0x2BEC08  title screen (start.lyt); returns 1 when passed
//   0x2A1298  main menu (main.lyt: Single Race/Career/Multiplayer/...), called by the main loop 0x2A2720
//   0x2A2C98  GameDoSingle: Action single player session driver
//   0x2A75E0  GameDoSingle car-select screen: (u32 *car_number, 0) -> nonzero = proceed
//   0x2C7340  GameDoSingle track-select screen: (i32 *eve, i32 *aux, Options *) -> nonzero = proceed
//   0x120650  per-event setup (EveTracker at 0x3C4BF8 -> contest -> roster -> session description)
//   0x120BE8  SelectEvent(pos): tracker vtable +0x14 (0x121850: pending = pos) then 0x3C4BE8 = 1, the flag the in-world event list sets
#include "session_start.h"
#include "battle_layout.h"

#include "core/cpu.h"
#include "core/memory.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

extern "C" {
void f_002A1298_orig(void);
void f_002BEC08_orig(void);
void f_002A75E0_orig(void);
void f_002C7340_orig(void);
void f_00120650_orig(void);
void f_002A2C98(void);
void f_00119428(void);
void f_00128CB0(void);
void f_002C1338(void);
void f_00120BE8(void);
void f_001252C0(void);
void f_00123958(void);
}

namespace {
constexpr uint32_t kMode = 0x003C4C04, kEveCount = 0x003C4CF4, kContestCount = 0x003C4CF8;
constexpr uint32_t kEves = 0x003C4D08, kEveStride = 0x84;
constexpr uint32_t kVehicleTable = 0x003CBEE4, kVehicleCount = 0x00439704, kVehicleStride = 0x54;
constexpr uint32_t kTrackerPtr = 0x003C4BF8, kTrackerCount = 0xC, kTrackerList = 0x1798;
constexpr uint32_t kFlags = 0x003C4190;  // bit 0 is held set for the duration of a single player session
constexpr uint32_t kActionMode = 0x12F;

bool ieq(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}
std::string trim(std::string s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}
std::string lower(std::string s) { for (char &c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string guest_text(uint32_t a, size_t limit = 40) {
    std::string out;
    while (a && a < EE_RAM_SIZE && out.size() < limit) { char c = (char)ee_r8(a++); if (!c) break; out.push_back(c); }
    return out;
}
bool number(const std::string &s, long &v) {
    if (s.empty()) return false;
    char *end; v = std::strtol(s.c_str(), &end, 0);
    return *end == 0;
}

// Calls a generated guest function with up to four integer arguments and returns $v0,
// leaving the caller's register file as it was (the same pattern battle_bots.cpp uses).
uint32_t native(void (*fn)(void), std::initializer_list<uint32_t> args = {}) {
    const EEState saved = g_ee;
    int r = 4;
    for (uint32_t a : args) g_ee.r[r++].ud[0] = a;
    fn();
    const uint32_t v0 = g_ee.r[2].uw[0];
    g_ee = saved;
    return v0;
}

struct Plan {
    bool requested = false, armed = false;
    SessionRequest req;
    uint32_t eve = 0, contest = 0, car = 0;
    bool car_done = false, track_done = false, event_done = false;
} P;

void log(const char *fmt, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0) {
    std::fprintf(stderr, "[SESSION] ");
    std::fprintf(stderr, fmt, a, b, c);
    std::fprintf(stderr, "\n");
}

// Eve (track meet) by track: display name first (it is what the player sees), then the
// internal id; this keeps "Welch" on the real Welch Eve rather than "test: Welch Dirt".
int find_eve(const std::string &track) {
    const uint32_t n = ee_r32(kEveCount);
    for (int pass = 0; pass < 2; ++pass)
        for (uint32_t i = 0; i < n && i < 64; ++i) {
            const uint32_t e = kEves + i * kEveStride;
            if (ieq(guest_text(e + (pass ? 8 : 0x28), 63), track)) return (int)i;
        }
    return -1;
}

// Contest (event) of an Eve by name or "#<position>"; position is the order of the Eve's
// contests in the contest table, which is the EveTracker list order.
int find_contest(uint32_t eve, const std::string &name) {
    const uint32_t n = ee_r32(kContestCount);
    long want = -1;
    if (!name.empty() && name[0] == '#') number(name.substr(1), want);
    long pos = 0;
    for (uint32_t c = 0; c < n && c < battle_layout::kContestCapacity; ++c) {
        const uint32_t cp = battle_layout::kContestTable + c * battle_layout::kContestStride;
        if (ee_r32(cp + 4) != eve) continue;
        if (want >= 0) { if (pos++ == want) return (int)c; continue; }
        const std::string have = guest_text(native(f_001252C0, {c}), 40);
        if (ieq(have, name)) return (int)c;
        ++pos;
    }
    return -1;
}

// Vehicle number for "car" (model name or number) and 0-based paint among that model's records.
long find_car(const std::string &car, int paint) {
    const uint32_t table = ee_r32(kVehicleTable), n = ee_r32(kVehicleCount);
    if (!table || n == 0 || n > 4096) return -1;
    long want = -1;
    if (number(car, want)) {
        for (uint32_t i = 0; i < n; ++i)
            if ((long)ee_r32(table + i * kVehicleStride + 4) == want) return want;
        return -1;
    }
    int seen = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t v = table + i * kVehicleStride;
        if (ieq(guest_text(v + 8, 32), car) && seen++ == paint) return (long)ee_r32(v + 4);
    }
    return -1;
}

void start() {
    const char *text = std::getenv("EOD_DIRECT_START");
    if (!text || !*text) return;
    std::string err;
    if (!session_parse(text, P.req, err)) { log("EOD_DIRECT_START rejected: "); std::fprintf(stderr, "[SESSION]   %s\n", err.c_str()); return; }
    P.requested = true;
    if (ee_r32(kEveCount) == 0 || ee_r32(kMode) != kActionMode) {
        log("mode is %x with %u Eves: selecting Action (0x123958)", ee_r32(kMode), ee_r32(kEveCount));
        native(f_00123958);
    }
    const int eve = find_eve(P.req.track);
    if (eve < 0) { std::fprintf(stderr, "[SESSION] unknown track '%s'\n", P.req.track.c_str()); return; }
    const std::string event = session_event_name(P.req);
    const int contest = find_contest((uint32_t)eve, event);
    if (contest < 0) { std::fprintf(stderr, "[SESSION] Eve %d has no event '%s'\n", eve, event.c_str()); return; }
    const long car = find_car(P.req.car, P.req.paint);
    if (car < 0) { std::fprintf(stderr, "[SESSION] no vehicle '%s' paint %d\n", P.req.car.c_str(), P.req.paint); return; }
    P.eve = (uint32_t)eve; P.contest = (uint32_t)contest; P.car = (uint32_t)car;
    std::fprintf(stderr, "[SESSION] start: Eve %d '%s' contest %d '%s' vehicle %ld humans %d\n", eve,
                 guest_text(kEves + P.eve * kEveStride + 0x28, 40).c_str(), contest, event.c_str(), car, P.req.humans);
    P.armed = true;
    // MenuDoRace's "single" branch (0x2C0E4C-0x2C0E74), minus the menu: bit 0 of the global
    // flags is held while the session runs, the profile name goes to human 0, then the
    // single player driver (which sets the human count and pad map itself).
    const uint32_t old = ee_r32(kFlags) & 1;
    ee_w32(kFlags, ee_r32(kFlags) | 1);
    const uint32_t profile = native(f_00119428, {0});
    native(f_00128CB0, {profile, 0});
    native(f_002C1338);
    native(f_002A2C98);
    ee_w32(kFlags, (ee_r32(kFlags) & ~1u) | old);
    P.armed = false;
    std::fprintf(stderr, "[SESSION] session returned to the front end\n");
}
} // namespace

bool session_parse(const char *text, SessionRequest &out, std::string &err) {
    SessionRequest r;
    std::string s = text ? text : "";
    size_t at = 0;
    while (at <= s.size()) {
        size_t end = s.find(';', at);
        if (end == std::string::npos) end = s.size();
        const std::string item = trim(s.substr(at, end - at));
        at = end + 1;
        if (item.empty()) continue;
        const size_t eq = item.find('=');
        if (eq == std::string::npos) { err = "missing '=' in '" + item + "'"; return false; }
        const std::string key = lower(trim(item.substr(0, eq))), val = trim(item.substr(eq + 1));
        long v = 0;
        if (key == "mode") r.mode = lower(val);
        else if (key == "track") r.track = val;
        else if (key == "event") r.event = val;
        else if (key == "car") r.car = val;
        else if (key == "paint") { if (!number(val, v) || v < 0 || v > 63) { err = "bad paint '" + val + "'"; return false; } r.paint = (int)v; }
        else if (key == "humans") { if (!number(val, v) || v < 1 || v > 4) { err = "bad humans '" + val + "'"; return false; } r.humans = (int)v; }
        else if (key == "ai") { if (!number(val, v) || v < 0 || v > 12) { err = "bad ai '" + val + "'"; return false; } r.ai = (int)v; }
        else { err = "unknown key '" + key + "'"; return false; }
    }
    if (r.mode != "action") { err = "mode '" + r.mode + "' is not implemented (only action)"; return false; }
    if (r.humans != 1) { err = "humans > 1 is not implemented (GameDoHeadToHead path is mapped, not wired)"; return false; }
    if (r.track.empty() || r.event.empty() || r.car.empty()) { err = "track, event and car are required"; return false; }
    out = r;
    return true;
}

std::string session_event_name(const SessionRequest &r) {
    if (r.ai >= 0 && ieq(r.event, "battle")) return r.ai == 1 ? "Battle (1v1)" : "Battle (" + std::to_string(r.ai) + " AI)";
    return r.event;
}

// Title screen ("press the START button", start.lyt): returns 1 once the player passed it.
// A direct start is a request already made, so it is treated as passed.
extern "C" void f_002BEC08(void) {
    const char *text = std::getenv("EOD_DIRECT_START");
    if (text && *text) {
        std::fprintf(stderr, "[SESSION] title screen skipped (EOD_DIRECT_START)\n");
        ret_s32(1);
        return;
    }
    f_002BEC08_orig();
}

extern "C" void f_002A1298(void) {
    static bool tried = false;
    if (!tried) { tried = true; start(); }
    f_002A1298_orig();
}

// GameDoSingle opens the car screen first. The answer is the human's vehicle number, which
// the loop body hands to 0x12A0B8 (Garage: set human driver); nonzero return = proceed.
extern "C" void f_002A75E0(void) {
    if (P.armed && !P.car_done) {
        P.car_done = true;
        ee_w32(arg_u32(0), P.car);
        log("car select answered: vehicle %u", P.car);
        ret_s32(1);
        return;
    }
    f_002A75E0_orig();
}

// Track screen: *eve = chosen Eve, *aux = 5 (its initial value), options+0x28 = 1 (both
// written by the native screen before it opens); nonzero return = play.
extern "C" void f_002C7340(void) {
    if (P.armed && !P.track_done) {
        P.track_done = true;
        ee_w32(arg_u32(0), P.eve);
        ee_w32(arg_u32(1), 5);
        ee_w32(arg_u32(2) + 0x28, 1);
        log("track select answered: Eve %u", P.eve);
        ret_s32(1);
        return;
    }
    f_002C7340_orig();
}

// First event setup of the session: put the chosen event in the native tracker's pending slot
// so 0x120650 starts it instead of the Eve's opening lobby event.
extern "C" void f_00120650(void) {
    if (P.armed && !P.event_done) {
        P.event_done = true;
        const uint32_t tracker = ee_r32(kTrackerPtr);
        const uint32_t count = tracker ? ee_r32(tracker + kTrackerCount) : 0;
        int pos = -1;
        for (uint32_t i = 0; i < count && i < 64; ++i)
            if (ee_r32(tracker + kTrackerList + 4 * i) == P.contest) pos = (int)i;
        if (pos >= 0) {
            native(f_00120BE8, {(uint32_t)pos});
            log("event pre-selected: tracker position %u (contest %u)", (uint32_t)pos, P.contest);
        } else {
            log("contest %u is not in the tracker list (%u entries): native lobby", P.contest, count);
        }
    }
    f_00120650_orig();
}
