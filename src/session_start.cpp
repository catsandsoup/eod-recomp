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
#include "netplay_hooks.h"

#include "core/cpu.h"
#include "core/log.h"
#include "core/memory.h"

#include <cctype>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

extern "C" {
void f_002A1298_orig(void);
void f_002BEC08_orig(void);
void f_002A75E0_orig(void);
void f_002C7340_orig(void);
void f_00270C10_orig(void);
}
extern uint64_t kernel_vblank_count();
extern "C" {
void f_00120650_orig(void);
void f_00120FB8_orig(void);
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
    bool mode_answered = false;   // online: the Arcade single/multi prompt was answered once
    bool net = false;   // online event (lobby START)
    bool direct = false; // online event with track, event and car chosen in the lobby: every native screen is auto-answered
    // mode=career: the main menu answers Career once; the Career's opening screens are answered until the hub
    // has said Drive Around (then src/career_warp.cpp takes over in free roam).
    bool career = false, career_entered = false, career_driving = false;
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


}  // namespace
namespace session_catalog {
namespace {
std::vector<Track> g_tracks;
std::vector<Vehicle> g_vehicles;
bool g_ready = false;
}
bool ready() { return g_ready; }
const std::vector<Track> &tracks() { return g_tracks; }
const std::vector<Vehicle> &vehicles() { return g_vehicles; }
bool refresh() {
    if (g_ready) return true;
    if (ee_r32(kEveCount) == 0 || ee_r32(kMode) != kActionMode) native(f_00123958);
    const uint32_t n = ee_r32(kEveCount), contests = ee_r32(kContestCount);
    const uint32_t table = ee_r32(kVehicleTable), nv = ee_r32(kVehicleCount);
    if (n == 0 || n > 64 || !table || nv == 0 || nv > 4096) return false;
    g_tracks.clear(); g_vehicles.clear();
    for (uint32_t i = 0; i < n; ++i) {
        Track t; t.eve = (int)i; t.name = guest_text(kEves + i * kEveStride + 0x28, 40);
        if (t.name.empty() || ieq(t.name.substr(0, 5), "test:") || ieq(t.name, "test")) continue;   // developer test tracks
        for (uint32_t c = 0; c < contests && c < battle_layout::kContestCapacity; ++c) {
            const uint32_t cp = battle_layout::kContestTable + c * battle_layout::kContestStride;
            if (ee_r32(cp + 4) != i) continue;
            const std::string name = guest_text(native(f_001252C0, {c}), 40);
            if (!name.empty()) t.events.push_back(name);
        }
        if (!t.events.empty()) g_tracks.push_back(t);
    }
    for (uint32_t i = 0; i < nv; ++i) {
        const uint32_t v = table + i * kVehicleStride;
        Vehicle e; e.number = ee_r32(v + 4); e.name = guest_text(v + 8, 32); e.record = v;
        if (!e.name.empty()) g_vehicles.push_back(e);
    }
    g_ready = !g_tracks.empty() && !g_vehicles.empty();
    if (g_ready) std::fprintf(stderr, "[SESSION] catalog: %zu tracks, %zu vehicles\n", g_tracks.size(), g_vehicles.size());
    return g_ready;
}
std::vector<std::string> models() {
    std::vector<std::string> m;
    for (auto &v : g_vehicles) if (std::find(m.begin(), m.end(), v.name) == m.end()) m.push_back(v.name);
    return m;
}
std::vector<uint32_t> paints(const std::string &model) {
    std::vector<uint32_t> p;
    for (auto &v : g_vehicles) if (v.name == model) p.push_back(v.number);
    return p;
}
void dump() {
    for (auto &t : g_tracks) { std::fprintf(stderr, "[CATALOG] track %d '%s':", t.eve, t.name.c_str()); for (auto &e : t.events) std::fprintf(stderr, " [%s]", e.c_str()); std::fprintf(stderr, "\n"); }
    int k = 0;
    for (auto &v : g_vehicles) {
        if (k++ > 40) break;
        std::fprintf(stderr, "[CATALOG] veh #%u '%s' raw:", v.number, v.name.c_str());
        for (uint32_t o = 0; o < 0x54; o += 4) std::fprintf(stderr, " %08x", ee_r32(v.record + o));
        std::fprintf(stderr, "\n");
    }
    std::fprintf(stderr, "[CATALOG] %zu models\n", models().size());
}
}  // namespace session_catalog
namespace {

void start() {
    const char *text = std::getenv("EOD_DIRECT_START");
    if (!text || !*text) return;
    std::string err;
    if (!session_parse(text, P.req, err)) { log("EOD_DIRECT_START rejected: "); std::fprintf(stderr, "[SESSION]   %s\n", err.c_str()); return; }
    P.requested = true;
    if (P.req.mode == "career") {
        P.career = true;
        std::fprintf(stderr, "[SESSION] career start (the Career on the memory card, or a new one), then %s%s\n", career_place_name(career_place_index(P.req.place)),
                     P.req.eve >= 0 ? (", then Eve " + std::to_string(P.req.eve)).c_str() : "");
        return;
    }
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
// Online event (lobby START): the next GameDoSingle (reached through the native main menu's
// ACTION item) runs with the native car screen shown for real on every machine and the track
// screen real on the host / answered with the host's pick on guests. The remote players are
// puppet cars (src/netplay_session.cpp).
void arm_net(const netplay_hooks::StartInfo &info) {
    P = Plan{};
    P.net = true;
    P.req.event = info.event;
    P.requested = true;
    P.armed = true;
    if (!info.track.empty()) {
        const int eve = find_eve(info.track);
        const int contest = eve < 0 ? -1 : find_contest((uint32_t)eve, info.event);
        if (eve >= 0 && contest >= 0) {
            P.direct = true; P.eve = (uint32_t)eve; P.contest = (uint32_t)contest; P.car = info.car;
        } else {
            std::fprintf(stderr, "[SESSION] online direct start impossible (track '%s' event '%s'): native screens\n", info.track.c_str(), info.event.c_str());
        }
    }
    std::fprintf(stderr, "[SESSION] online event %u: %s on %s, vehicle %u (%s, %s)\n", info.event_id, info.event.c_str(), info.track.c_str(), info.car,
                 info.host ? "host" : "guest", P.direct ? "direct" : "native screens");
}
const bool g_arm_registered = [] { netplay_hooks::arm = &arm_net; return true; }();
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
        else if (key == "place") r.place = val;
        else if (key == "eve") { if (!number(val, v) || v < 0 || v > 63) { err = "bad eve '" + val + "'"; return false; } r.eve = (int)v; }
        else { err = "unknown key '" + key + "'"; return false; }
    }
    if (r.humans != 1) { err = "humans > 1 is not implemented (GameDoHeadToHead path is mapped, not wired)"; return false; }
    if (r.mode == "career") {
        if (!r.track.empty() || !r.event.empty() || !r.car.empty() || r.ai >= 0)
            { err = "career takes place= and eve=, not track/event/car/ai"; return false; }
        if (r.place.empty()) r.place = "home";
        if (career_place_index(r.place) < 0) {
            err = "unknown place '" + r.place + "' (home, malt, next_eve, dealer, paintshop, gate1, gate2, petes)";
            return false;
        }
        out = r;
        return true;
    }
    if (r.mode != "action") { err = "mode '" + r.mode + "' is not implemented (action or career)"; return false; }
    if (!r.place.empty() || r.eve >= 0) { err = "place= and eve= are career keys (mode=career)"; return false; }
    if (r.track.empty() || r.event.empty() || r.car.empty()) { err = "track, event and car are required"; return false; }
    out = r;
    return true;
}

namespace {
// teleport.lyt items in order (0x386DE8 builds them with codes 0..7; the code is the Home area index).
struct PlaceName { const char *id; const char *alias[3]; const char *plain; };
const PlaceName kPlaces[kCareerPlaces] = {
    {"home", {"your home", "garage", nullptr}, "home"},
    {"malt", {"maltshop", "malt shop", "the malt shop"}, "Malt Shop"},
    {"next_eve", {"eve", "next eve", nullptr}, "next Eve"},
    {"junkyard", {"dealer", "scrapyard", "sweeny's scrapyard"}, "dealer (Sweeny's Scrapyard)"},
    {"paintshop", {"paint shop", "the paintshop", "paint"}, "Paintshop"},
    {"gate1", {"gate 1", nullptr, nullptr}, "Gate 1"},
    {"gate2", {"gate 2", nullptr, nullptr}, "Gate 2"},
    {"petes", {"pete's", "pete", nullptr}, "Pete's"},
};
}  // namespace

int career_place_index(const std::string &name) {
    const std::string n = lower(trim(name));
    for (int i = 0; i < kCareerPlaces; ++i) {
        if (n == kPlaces[i].id) return i;
        for (const char *a : kPlaces[i].alias) if (a && n == a) return i;
    }
    long v = 0;
    if (number(n, v) && v >= 0 && v < kCareerPlaces) return (int)v;
    return -1;
}
const char *career_place_name(int index) { return index >= 0 && index < kCareerPlaces ? kPlaces[index].plain : "?"; }

const SessionRequest *career_warp_request() { return P.career ? &P.req : nullptr; }

std::string session_event_name(const SessionRequest &r) {
    if (r.ai >= 0 && ieq(r.event, "battle")) return r.ai == 1 ? "Battle (1v1)" : "Battle (" + std::to_string(r.ai) + " AI)";
    return r.event;
}

// Title screen ("press the START button", start.lyt): returns 1 once the player passed it.
// A direct start is a request already made, so it is treated as passed.
extern "C" void f_002BEC08(void) {
    // Developer: EOD_FATAL_TEST=1 raises a game panic at the title screen, to check the in-game
    // error screen and crash.log (a windowed run; headless runs just log and abort).
    if (std::getenv("EOD_FATAL_TEST")) fatal("developer panic test (EOD_FATAL_TEST)");
    const char *text = std::getenv("EOD_DIRECT_START");
    if (text && *text) {
        std::fprintf(stderr, "[SESSION] title screen skipped (EOD_DIRECT_START)\n");
        ret_s32(1);
        return;
    }
    netplay_hooks::title_screen = true;
    f_002BEC08_orig();
    netplay_hooks::title_screen = false;
}

extern "C" void f_002A1298(void) {
    static bool tried = false;
    if (!tried) { tried = true; start(); if (std::getenv("EOD_CATALOG_DUMP") && session_catalog::refresh()) session_catalog::dump(); }
    if (P.career && !P.career_entered) {
        // MainMenuUpdate's result 15 is CAREER: the dispatcher (0x2A2720, table 0x414460) runs the Career driver
        // 0x112298 exactly as when the player picks it. Nothing of the main menu has been built yet.
        P.career_entered = true;
        log("main menu answered CAREER (15)");
        ret_s32(15);
        return;
    }
    if (P.net) {   // the online event that was running has returned to the main menu
        P = Plan{};
        if (netplay_hooks::event_ended) netplay_hooks::event_ended();
    }
    if (netplay_hooks::menu_enter) netplay_hooks::menu_enter();
    f_002A1298_orig();
    if (netplay_hooks::menu_leave) netplay_hooks::menu_leave();
}

// MenuRun runs every native menu screen. The only one answered here is the Arcade "single player or
// multiplayer?" prompt (s_and_m.lyt, the call at 0x2C0DC0 in ArcadeSingleOrMultiPromptScreen 0x2C0BB8):
// online, every game has exactly one local human and the others are puppets, so the answer is always
// "single" (1). When that single-player session returns the prompt loops; answering "back" (-1) leaves
// the Arcade menu for the main menu, where the lobby reopens.
extern "C" void f_00270C10(void) {
    const uint32_t caller = g_ee.r[31].uw[0];
    if (caller == 0x002A16D8) {
        f_00270C10_orig();
        // The main dispatcher maps result 16 to Arcade. Preserve MenuRun cleanup.
        if (P.armed && P.net) ret_s32(16);
        return;
    }
    if (caller == 0x002C1108 && P.armed && P.net) {
        ret_s32(P.mode_answered ? -1 : 0); // Arcade: Race on entry, Back on return.
        return;
    }
    if (P.armed && P.net && caller == 0x002C0DC8) {
        if (!P.mode_answered) { P.mode_answered = true; ret_s32(1); log("online: single/multi prompt answered 'single'"); }
        else ret_s32(-1);
        return;
    }
    if (P.career && P.career_entered && !P.career_driving) {
        // A fresh Career's opening screens (callers from EOD_MENU_RUN_TRACE): the tutorial note (0x3861B4) and the
        // "select Drive Around" note (0x383C94) close with -2; the Career hub (0x36AB8C) answers Drive Around (4).
        // The loading screens (0x2C1A0C) run natively.
        if (caller == 0x003861B4 || caller == 0x00383C94) { log("career: note %08x closed", caller); ret_s32(-2); return; }
        if (caller == 0x0036AB8C) { P.career_driving = true; log("career: hub answered Drive Around (4)"); ret_s32(4); return; }
    }
    f_00270C10_orig();
    // Developer (EOD_MENU_RUN_TRACE=1): every native menu screen's caller and the answer it returned, the map
    // a direct start needs to answer a screen without showing it.
    static const bool trace = std::getenv("EOD_MENU_RUN_TRACE") != nullptr;
    if (trace) std::fprintf(stderr, "[MENU_RUN] caller=%08x result=%d vblank=%llu\n", caller, g_ee.r[2].sw[0],
                            (unsigned long long)kernel_vblank_count());
}

// GameDoSingle opens the car screen first. The answer is the human's vehicle number, which
// the loop body hands to 0x12A0B8 (Garage: set human driver); nonzero return = proceed.
// When the online event finishes and unwinds back to the outer loop, P.car_done is already
// true, so returning 0 exits GameDoSingle back to ArcadeSingleOrMultiPromptScreen (0x2C0BB8).
extern "C" void f_002A75E0(void) {
    if (P.armed && P.net && P.car_done) {
        log("online: car screen unwind -> leave GameDoSingle");
        ret_s32(0);
        return;
    }
    if (P.armed && P.net && P.direct) {
        P.car_done = true;
        ee_w32(arg_u32(0), P.car);
        if (netplay_hooks::car_chosen) netplay_hooks::car_chosen(P.car);
        log("online: car screen answered, vehicle %u", P.car);
        ret_s32(1);
        return;
    }
    if (P.armed && P.net) {
        const uint32_t number_ptr = arg_u32(0);
        f_002A75E0_orig();
        if (g_ee.r[2].uw[0] && !P.car_done) {
            P.car_done = true;
            if (netplay_hooks::car_chosen) netplay_hooks::car_chosen(ee_r32(number_ptr));
        }
        return;
    }
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
// When the online event finishes and unwinds back to the middle loop, P.track_done is already
// true, so returning 0 exits the middle loop back to the outer loop (f_002A75E0).
extern "C" void f_002C7340(void) {
    if (P.armed && P.net && P.track_done) {
        log("online: track screen unwind -> outer loop");
        ret_s32(0);
        return;
    }
    if (P.armed && P.net && P.direct) {
        P.track_done = true;
        ee_w32(arg_u32(0), P.eve); ee_w32(arg_u32(1), 5); ee_w32(arg_u32(2) + 0x28, 1);
        if (netplay_hooks::track_chosen) netplay_hooks::track_chosen(guest_text(kEves + P.eve * kEveStride + 0x28, 40));
        log("online: track screen answered, Eve %u", P.eve);
        ret_s32(1);
        return;
    }
    if (P.armed && P.net && !P.track_done) {
        const uint32_t eve_ptr = arg_u32(0), aux_ptr = arg_u32(1), options = arg_u32(2);
        std::string track;
        if (netplay_hooks::is_host && netplay_hooks::is_host()) {
            f_002C7340_orig();
            if (g_ee.r[2].uw[0]) {
                P.track_done = true;
                P.eve = ee_r32(eve_ptr);
                track = guest_text(kEves + P.eve * kEveStride + 0x28, 40);
                if (netplay_hooks::track_chosen) netplay_hooks::track_chosen(track);
                const int c = find_contest(P.eve, P.req.event);
                P.contest = c < 0 ? 0xFFFFFFFFu : (uint32_t)c;
                if (c < 0) std::fprintf(stderr, "[SESSION] '%s' has no event '%s': native lobby event\n", track.c_str(), P.req.event.c_str());
            }
            return;
        }
        if (!netplay_hooks::wait_track(track)) { ret_s32(0); return; }
        const int eve = find_eve(track);
        if (eve < 0) { std::fprintf(stderr, "[SESSION] unknown track '%s'\n", track.c_str()); ret_s32(0); return; }
        P.track_done = true; P.eve = (uint32_t)eve;
        const int c = find_contest(P.eve, P.req.event);
        P.contest = c < 0 ? 0xFFFFFFFFu : (uint32_t)c;
        ee_w32(eve_ptr, P.eve); ee_w32(aux_ptr, 5); ee_w32(options + 0x28, 1);
        log("online track answered: Eve %u", P.eve);
        ret_s32(1);
        return;
    }
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
    // Developer (EOD_CAREER_EVE + EOD_CAREER_LAST=1, src/hle_dev_career.cpp): a Career Eve starts
    // its LAST listed event (an added Career Battle) instead of the opening lobby event.
    static bool career_last_done = false;
    if (!P.armed && !career_last_done && std::getenv("EOD_CAREER_EVE") && std::getenv("EOD_CAREER_LAST")) {
        career_last_done = true;
        const uint32_t tracker = ee_r32(kTrackerPtr);
        const uint32_t count = tracker ? ee_r32(tracker + kTrackerCount) : 0;
        if (count) {
            std::fprintf(stderr, "[CAREER_EVE] event list (%u):", count);
            for (uint32_t i = 0; i < count && i < 64; ++i) std::fprintf(stderr, " %u", ee_r32(tracker + kTrackerList + 4 * i));
            std::fprintf(stderr, "; starting position %u\n", count - 1);
            native(f_00120BE8, {count - 1});
        }
    }
    f_00120650_orig();
}

// EveHasMoreEvents (0x120FB8) is checked before each contest in GameDoSingle's inner loop
// (0x2A2FC4). Once the single online event has run (P.event_done is true), returning 0 exits
// the inner Eve loop instead of opening the native in-world Eve event selection screen (0x11F640).
extern "C" void f_00120FB8(void) {
    if (P.armed && P.net && P.event_done) {
        log("online: event finished -> leave Eve inner loop");
        ret_s32(0);
        return;
    }
    f_00120FB8_orig();
}

void session_abort_event() {
    if (!g_ram) return;
    // 0x00437BB8: race frame loop active flag (f_00114020)
    // 0x00437BAC: race quit flag (prevents retry and skips post-race scoring in f_00114358)
    // 0x00437BA8: contest loop exit flag (exits f_00114860 back to GameDoSingle)
    if (ee_r32(0x00437BB8) != 0) {
        ee_w32(0x00437BAC, 1);
        ee_w32(0x00437BA8, 1);
        ee_w32(0x00437BB8, 0);
        log("online: aborted active race loop -> unwinding to main menu");
    }
}

// The native contest clears the exit flag during teardown. Its caller checks it
// immediately after return to decide whether to open the Eve event list again.
extern "C" void f_00114358_orig();
extern "C" void f_00114358() {
    const bool online = P.net && P.armed && g_ee.r[31].uw[0] == 0x00114BA0;
    f_00114358_orig();
    if (online) ee_w32(0x00437BA8, 1);
}
