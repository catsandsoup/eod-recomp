// Direct session start. See session_start.h and docs RACE_SETUP_MAP.md.
//
// Guest facts used (SLUS_209.10):
//   0x2BEC08  title screen (start.lyt); returns 1 when passed
//   0x2A1298  main menu (main.lyt: Single Race/Career/Multiplayer/...), called by the main loop 0x2A2720
//   0x2A2C98  GameDoSingle: Action single player session driver
//   0x2A75E0  GameDoSingle car-select screen: (u32 *car_number, 0) -> nonzero = proceed
//   0x2C7340  GameDoSingle track-select screen: (i32 *eve, i32 *aux, Options *) -> nonzero = proceed
//   0x2A3220  GameDoHeadToHead: multiplayer session driver (MenuDoRace 'multi', 0x2C0E10)
//   0x2B7710  GameDoHeadToHead join/car screen: (i32 *players, i32 cars[]) -> 1 = back, else play;
//             then 0x2A30B0 runs the same track screen (0x2C7340) and event setup (0x120650)
//   0x120650  per-event setup (EveTracker at 0x3C4BF8 -> contest -> roster -> session description)
//   0x120BE8  SelectEvent(pos): tracker vtable +0x14 (0x121850: pending = pos) then 0x3C4BE8 = 1, the flag the in-world event list sets
#include "vfs/custom_map.h"
#include "session_start.h"
#include "battle_layout.h"
#include "battle_events.h"
#include "netplay_hooks.h"
#include "netplay_impact_fixture.h"

#include "core/config.h"
#include "core/cpu.h"
#include "core/eod_tracy.h"
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
void arcade_mod_badges_run_menu(uint32_t caller, void (*run_menu)());
extern "C" {
void f_00120650_orig(void);
void f_00120FB8_orig(void);
void f_002A2C98(void);
void f_002A3220(void);
void f_002B9858(void);
void f_002E66B8(void);
void f_002E66D8(void);
void f_002E66F8(void);
void f_00335110(void);
void f_003356D0(void);
void f_00119428(void);
void f_00128CB0(void);
void f_002C1338(void);
void f_00120BE8(void);
void f_001252C0(void);
void f_00123958(void);
void f_0012CC40(void);
void f_002E06A0(void);
void f_002E0AF8(void);
void f_002E0098(void);
}

namespace {
constexpr uint32_t kMode = 0x003C4C04, kEveCount = 0x003C4CF4, kContestCount = 0x003C4CF8;
constexpr uint32_t kEves = 0x003C4D08, kEveStride = 0x84;
constexpr uint32_t kVehicleTable = 0x003CBEE4, kVehicleCount = 0x00439704, kVehicleStride = 0x54;
constexpr uint32_t kTrackerPtr = 0x003C4BF8, kTrackerCount = 0xC, kTrackerList = 0x1798;
constexpr uint32_t kFlags = 0x003C4190;  // bit 0 is held set for the duration of a single player session
constexpr uint32_t kActionMode = 0x12F;
constexpr uint32_t kHeadToHeadMode = 0x131;
constexpr uint32_t kSessionEventType = 0x9A4, kSessionLaps = 0x9B0, kSessionContest = 0x9D0;
constexpr uint32_t kLapRaceFlag = 0x100;
// World initialization copies session+0x990 to 0x495BE0. The retail AI
// strength lookup at 0x15F4D8 reads offset 0x28 and accepts levels 0..4.
constexpr uint32_t kSessionAIStrength = 0x990 + 0x28;
constexpr uint32_t kHardAIStrength = 4;

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
    uint32_t eve = 0, contest = 0xFFFFFFFFu, car = 0;
    bool car_done = false, track_done = false, event_done = false;
    bool players_done = false;  // humans > 1: the head-to-head join/car screen was answered
    bool mode_answered = false;   // online: the Arcade single/multi prompt was answered once
    bool net = false;   // online event (lobby START)
    bool direct = false; // online event with track, event and car chosen in the lobby: every native screen is auto-answered
    // mode=career: the main menu answers Career once; the Career's opening screens are answered until the hub
    // has said Drive Around (then src/career_warp.cpp takes over in free roam).
    bool career = false, career_entered = false, career_driving = false;
    bool career_classic_hub = false; // true = stop at native Career Hub instead of auto Drive Around
} P;

PendingOfflineMatch g_pending_offline{};
PendingCareerWarp g_career_warp{};
SessionTransition g_pending_transition{};
bool g_unwind_active = false;
bool g_unwind_career = false;
EEStateKind g_ee_state = EEStateKind::Boot;
void (*g_online_transition_handler)(const SessionTransition &) = nullptr;
void (*g_hub_arrival_handler)() = nullptr;
void (*g_career_save_handler)() = nullptr;
void arm_offline_match_plan(const PendingOfflineMatch &m);

void log(const char *fmt, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0) {
    std::fprintf(stderr, "[SESSION] ");
    std::fprintf(stderr, fmt, a, b, c);
    std::fprintf(stderr, "\n");
}

// Eve (track meet) by track: display name first (it is what the player sees), then the
// internal id; this keeps "Welch" on the real Welch Eve rather than "test: Welch Dirt".
int find_eve(const std::string &track) {
    std::string t = trim(track);
    const size_t slash = t.find_last_of("/\\");
    if (slash != std::string::npos) t = t.substr(slash + 1);
    if (t.size() > 4 && ieq(t.substr(t.size() - 4), ".trk")) t = t.substr(0, t.size() - 4);
    static const struct { const char *alias; const char *canonical; } kTrackAliases[] = {
        {"afton", "Woodly"}, {"woodly", "Woodly"},
        {"emptyt_1", "EmptyTrack"}, {"emptytrack", "EmptyTrack"},
        {"homeba_1", "HomeBase"}, {"homebase", "HomeBase"},
        {"spring_1", "Springfield"}, {"springfield", "Springfield"},
        {"dennis_1", "Dennison"}, {"dennison", "Dennison"},
        {"hastin_1", "Hastings"}, {"hastings", "Hastings"},
        {"mankat_1", "Mankato"}, {"mankato", "Mankato"},
        {"bemidj_1", "Bemidji"}, {"bemidji", "Bemidji"},
    };
    std::string canonical = t;
    for (const auto &a : kTrackAliases) {
        if (ieq(t, a.alias)) { canonical = a.canonical; break; }
    }
    const uint32_t n = ee_r32(kEveCount);
    for (const std::string & query : {canonical, t}) {
        for (int pass = 0; pass < 2; ++pass)
            for (uint32_t i = 0; i < n && i < 64; ++i) {
                const uint32_t e = kEves + i * kEveStride;
                if (ieq(guest_text(e + (pass ? 8 : 0x28), 63), query)) return (int)i;
            }
    }
    return -1;
}

// EmptyTrack is a hidden developer Eve. Its sole retail contest is a 100-lap
// normal race. Only a creator map mounted on this physical arena may offer it.
bool blank_creator_eve(int eve) {
    if (eve < 0 || !ieq(guest_text(kEves + eve*kEveStride + 8, 32), "EmptyTrack")) return false;
    const auto map = eod::vfs::active_map();
    return map && map->replace_retail_spawns && find_eve(map->base_arena) == eve;
}

bool has_active_contest(uint32_t eve) {
    // The first contest lookup after the picker (0x1252F0) requires an
    // enabled record for this Eve, even when the event is chosen manually.
    const uint32_t n = std::min(ee_r32(kContestCount), battle_layout::kContestCapacity);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t record = battle_layout::kContestTable + i * battle_layout::kContestStride;
        if (ee_r32(record + 4) == eve && ee_r32(record + 36)) return true;
    }
    return false;
}

bool head_to_head_track_visible(uint32_t eve_record) {
    // The retail list builder (0x2C71B0) reads the Eve's "hidden" attribute
    // through the section vtable before the picker tests the profile lock.
    constexpr uint32_t kHiddenKey = 0x0041A660;
    const uint32_t section = ee_r32(eve_record + 0x80);
    if (!section) return false;
    const uint32_t vtable = ee_r32(section);
    const EEFunc get_string = ee_lookup_func(ee_r32(vtable + 0x18));
    if (!get_string) return false;
    const std::string hidden = guest_text(native(get_string, {section, kHiddenKey, 0}));
    if (ieq(hidden, "yes")) return false;
    if (!ieq(hidden, "until_career_done")) return true;
    native(f_002E06A0);
    const bool career_done = native(f_002E0AF8) != 0;
    native(f_002E0098);
    return career_done;
}

const char *head_to_head_track_unavailable(int eve) {
    if (ee_r32(kMode) != kHeadToHeadMode) return "head-to-head catalog is inactive";
    if (eve < 0) return "absent from head-to-head catalog";
    if (!has_active_contest((uint32_t)eve)) return "no active contest";
    const uint32_t record = kEves + (uint32_t)eve * kEveStride;
    if (!head_to_head_track_visible(record)) return "hidden in native picker";
    if (!native(f_0012CC40, {record + 0x28})) return "locked in native picker";
    return nullptr;
}

std::string canonical_contest_alias(const std::string &name) {
    const std::string n = lower(trim(name));
    static const struct { const char *alias; const char *canonical; } kAliases[] = {
        {"normal", "No-Rules Race"},
        {"normal_race", "No-Rules Race"},
        {"c_normal_race", "No-Rules Race"},
        {"no rules race", "No-Rules Race"},
        {"no-rules race", "No-Rules Race"},
        {"fig8jump", "Figure-8 Jump Race"},
        {"figure8_jump_race", "Figure-8 Jump Race"},
        {"c_figure8_jump_race", "Figure-8 Jump Race"},
        {"figure 8 jump race", "Figure-8 Jump Race"},
        {"figure-8 jump race", "Figure-8 Jump Race"},
        {"fig8", "Figure-8"},
        {"figure8_race", "Figure-8"},
        {"c_figure8_race", "Figure-8"},
        {"figure 8", "Figure-8"},
        {"figure-8", "Figure-8"},
        {"figure 8 race", "Figure-8"},
        {"figure-8 race", "Figure-8"},
        {"road_race", "Jump Race"},
        {"jump_race", "Jump Race"},
        {"c_road_race", "Jump Race"},
        {"c_jump_race", "Jump Race"},
        {"jump race", "Jump Race"},
        {"demo_derby_contest", "Demolition Derby"},
        {"demo_derby", "Demolition Derby"},
        {"kill_derby", "Demolition Derby"},
        {"c_kill_derby", "Demolition Derby"},
        {"demolition derby", "Demolition Derby"},
        {"knockout_contest", "Knockout"},
        {"kill_knock", "Knockout"},
        {"c_kill_knock", "Knockout"},
        {"knockout", "Knockout"},
        {"pushoff_contest", "Push-Off"},
        {"kill_push", "Push-Off"},
        {"pushoff", "Push-Off"},
        {"c_pushoff", "Push-Off"},
        {"push-off", "Push-Off"},
        {"save_ring", "Suicide Race"},
        {"c_save_ring", "Suicide Race"},
        {"suicide", "Suicide Race"},
        {"suicide_race", "Suicide Race"},
        {"c_suicide_race", "Suicide Race"},
        {"suicide race", "Suicide Race"},
        {"stop_and_go", "Stop-and-Go"},
        {"stop_and_go_race", "Stop-and-Go"},
        {"c_stop_and_go_race", "Stop-and-Go"},
        {"stop and go", "Stop-and-Go"},
        {"stop-and-go", "Stop-and-Go"},
        {"stop & go", "Stop-and-Go"},
        {"shortcut", "Shortcut Race"},
        {"shortcut_race", "Shortcut Race"},
        {"c_shortcut_race", "Shortcut Race"},
        {"shortcut race", "Shortcut Race"},
        {"flag_pole", "Flagpole Race"},
        {"flagpole_race", "Flagpole Race"},
        {"c_flagpole_race", "Flagpole Race"},
        {"flagpole race", "Flagpole Race"},
        {"flag pole", "Flagpole Race"},
        {"whip", "Whip-Around"},
        {"whip_around_race", "Whip-Around"},
        {"c_whip_around_race", "Whip-Around"},
        {"whip-around", "Whip-Around"},
        {"whip around", "Whip-Around"},
        {"chain_race", "Chain Race"},
        {"chain race", "Chain Race"},
        {"fwd_bwd_race", "Forward-Backward"},
        {"c_fwd_bwd_race", "Forward-Backward"},
        {"forward-backward", "Forward-Backward"},
        {"forward backward", "Forward-Backward"},
        {"last_man_race", "Last Man"},
        {"c_last_man_race", "Last Man"},
        {"last man", "Last Man"},
        {"last man race", "Last Man"},
        {"school_bus", "School Bus"},
        {"c_school_bus", "School Bus"},
        {"school bus", "School Bus"},
        {"school bus race", "School Bus"},
        {"trailer_race", "Trailer Race"},
        {"c_trailer_race", "Trailer Race"},
        {"trailer race", "Trailer Race"},
        {"pointtopoint", "Point-to-Point"},
        {"c_pointtopoint", "Point-to-Point"},
        {"point-to-point", "Point-to-Point"},
        {"point to point", "Point-to-Point"},
        {"detention", "Detention"},
        {"c_detention", "Detention"},
        {"gauntlet", "Gauntlet"},
        {"c_gauntlet", "Gauntlet"},
        {"red_rover", "Red Rover"},
        {"c_red_rover", "Red Rover"},
        {"red rover", "Red Rover"},
        {"wrangle", "Wrangler"},
        {"c_wrangle", "Wrangler"},
        {"wrangler", "Wrangler"},
        {"capture", "Downtown"},
        {"c_capture", "Downtown"},
        {"downtown", "Downtown"},
        {"stuntman", "Daredevil"},
        {"c_stuntman", "Daredevil"},
        {"daredevil", "Daredevil"},
        {"battle", "Battle"},
        {"c_battle", "Battle"},
        {"battle (free roam)", "Battle"},
        {"free roam (battle solo)", "Battle"},
        {"free roam", "Battle"},
    };
    for (const auto &a : kAliases) {
        if (n == a.alias) return a.canonical;
    }
    return name;
}

bool is_battle_request_name(const std::string &name) {
    const std::string n = lower(trim(name));
    return n == "battle" || n == "c_battle" || n == "free roam" ||
           n.rfind("battle (", 0) == 0 || n.rfind("free roam", 0) == 0;
}

// Contest (event) of an Eve by name or "#<position>"; position is the order of the Eve's
// contests in the contest table, which is the EveTracker list order.
int find_contest(uint32_t eve, const std::string &name) {
    const uint32_t n = ee_r32(kContestCount);
    long want = -1;
    if (!name.empty() && name[0] == '#') number(name.substr(1), want);
    const std::string canonical = canonical_contest_alias(name);
    long pos = 0;
    int first_battle = -1;
    int battle_8 = -1;
    int battle_4 = -1;
    int first_contest = -1;
    for (uint32_t c = 0; c < n && c < battle_layout::kContestCapacity; ++c) {
        const uint32_t cp = battle_layout::kContestTable + c * battle_layout::kContestStride;
        if (ee_r32(cp + 4) != eve) continue;
        if (first_contest < 0) first_contest = (int)c;
        if (want >= 0) { if (pos++ == want) return (int)c; continue; }
        const std::string have = guest_text(native(f_001252C0, {c}), 40);
        if (ieq(have, name) || ieq(have, canonical)) return (int)c;
        if (ieq(have, "Battle (8 AI)")) battle_8 = (int)c;
        else if (ieq(have, "Battle (4 AI)")) battle_4 = (int)c;
        else if (ieq(have, "Battle") && first_battle < 0) first_battle = (int)c;
        ++pos;
    }
    if (is_battle_request_name(name) || is_battle_request_name(canonical)) {
        if (battle_8 >= 0) return battle_8;
        if (battle_4 >= 0) return battle_4;
        if (first_battle >= 0) return first_battle;
    }
    return -1;
}

// Vehicle number for "car" (model name or number) and 0-based paint among that model's records.
long find_car(const std::string &car, int paint) {
    const uint32_t table = ee_r32(kVehicleTable), n = ee_r32(kVehicleCount);
    if (!table || n == 0 || n > 4096) return -1;
    std::string c = trim(car);
    if (c.size() > 4 && ieq(c.substr(c.size() - 4), ".car")) c = c.substr(0, c.size() - 4);
    static const struct { const char *alias; const char *canonical; } kCarAliases[] = {
        {"ambula", "Ambulance"}, {"ambula_1", "Ambulance"},
        {"bulldo", "Bulldog"},   {"bulldo_1", "Bulldog"},
        {"carrer", "Carrere"},   {"carrer_1", "Carrere"},
        {"fillmo", "Fillmore"},  {"fillmo_1", "Fillmore"},
        {"gladia", "Gladiator"}, {"gladia_1", "Gladiator"},
        {"gueril", "Guerilla"},  {"gueril_1", "Guerilla"},
        {"hurric", "Hurricane"}, {"hurric_1", "Hurricane"},
        {"mongoo", "Mongoose"},  {"mongoo_1", "Mongoose"},
        {"orbita", "Orbital"},   {"orbita_1", "Orbital"},
        {"school bus", "Bus"},   {"police", "Copcar"},
    };
    for (const auto &a : kCarAliases) {
        if (ieq(c, a.alias)) { c = a.canonical; break; }
    }
    long want = -1;
    if (number(c, want)) {
        for (uint32_t i = 0; i < n; ++i)
            if ((long)ee_r32(table + i * kVehicleStride + 4) == want) return want;
        return -1;
    }
    int seen = 0;
    long first_match = -1;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t v = table + i * kVehicleStride;
        const std::string vname = guest_text(v + 8, 32);
        if (ieq(vname, c) || ieq(vname, car)) {
            if (first_match < 0) first_match = (long)ee_r32(v + 4);
            if (seen++ == paint) return (long)ee_r32(v + 4);
        }
    }
    return first_match;
}

std::vector<uint32_t> split_vehicle_numbers(uint32_t selected, size_t seats) {
    const uint32_t table = ee_r32(kVehicleTable), count = ee_r32(kVehicleCount);
    if (!table || !count || count > 4096) return {};

    std::string model;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t record = table + i * kVehicleStride;
        if (ee_r32(record + 4) != selected) continue;
        model = guest_text(record + 8, 32);
        break;
    }
    if (model.empty()) return {};

    std::vector<uint32_t> cars = {selected};
    for (uint32_t i = 0; i < count && cars.size() < seats; ++i) {
        const uint32_t record = table + i * kVehicleStride;
        const uint32_t number = ee_r32(record + 4);
        if (guest_text(record + 8, 32) == model &&
            std::find(cars.begin(), cars.end(), number) == cars.end())
            cars.push_back(number);
    }
    return cars;
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
const Track *track_for_arena(const std::string &arena) {
    const int eve = find_eve(arena);
    for (const auto &track : g_tracks) if (track.eve == eve) return &track;
    if (blank_creator_eve(eve)) {
        static thread_local Track blank;
        blank = {eve, guest_text(kEves + eve*kEveStride + 0x28, 40), "Battle",
                 {kHumanBattle, "Battle"}, {0x18000224, 0x18000224}};
        return &blank;
    }
    return nullptr;
}
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
            if (!name.empty()) {
                constexpr uint32_t kBattle = 0x18000224;
                if (ee_r32(cp + 8) == kBattle) {
                    // Offer the human-only roster on any map with a Battle template.
                    // Prefer the stock contest and present it under its online name.
                    if (t.human_battle_contest.empty()) {
                        t.human_battle_contest = name;
                        t.events.push_back(kHumanBattle);
                        t.event_types.push_back(kBattle);
                    }
                    if (ieq(name, "Battle")) {
                        t.human_battle_contest = name;
                        continue;
                    }
                }
                t.events.push_back(name);
                t.event_types.push_back(ee_r32(cp + 8));
            }
        }
        if (!t.events.empty()) g_tracks.push_back(t);
    }
    for (uint32_t i = 0; i < nv; ++i) {
        const uint32_t v = table + i * kVehicleStride;
        Vehicle e; e.number = ee_r32(v + 4); e.name = guest_text(v + 8, 32); e.record = v;
        // 111C78 asks 12A2F0 for vehicles with bit 0 set when appending trailers.
        e.is_trailer = (ee_r32(v + 0x44) & 1u) != 0 || (e.number >= 400 && e.number <= 413) ||
                       ieq(e.name.substr(0, std::min<size_t>(7, e.name.size())), "trailer") ||
                       ieq(e.name.substr(0, std::min<size_t>(6, e.name.size())), "camper") ||
                       ieq(e.name.substr(0, std::min<size_t>(7, e.name.size())), "caravan");
        e.car_class = ee_r32(v + 0x40);
        e.is_modded = (e.car_class >= 4) || (e.number >= 775 && e.number <= 789) || (e.number >= 800);
        if (!e.name.empty()) g_vehicles.push_back(e);
    }
    g_ready = !g_tracks.empty() && !g_vehicles.empty();
    if (g_ready) std::fprintf(stderr, "[SESSION] catalog: %zu tracks, %zu vehicles\n", g_tracks.size(), g_vehicles.size());
    return g_ready;
}
bool is_online_driver_choice(const Vehicle &vehicle) {
    if (!vehicle.number || vehicle.name.empty() || vehicle.is_trailer ||
        (vehicle.number >= 400 && vehicle.number <= 413)) return false;
    constexpr size_t prefix_length = 7;
    const auto &name = vehicle.name;
    return !ieq(name.substr(0, prefix_length), "trailer") ||
           !std::all_of(name.begin() + prefix_length, name.end(),
                        [](char c) { return c >= '0' && c <= '9'; });
}

bool online_vehicle_is_drivable(uint32_t number) {
    const auto it = std::find_if(g_vehicles.begin(), g_vehicles.end(),
                                 [number](const Vehicle &vehicle) { return vehicle.number == number; });
    return it != g_vehicles.end() && is_online_driver_choice(*it);
}

std::vector<std::string> models() {
    std::vector<std::string> m;
    for (const auto &v : g_vehicles)
        if (is_online_driver_choice(v) && std::find(m.begin(), m.end(), v.name) == m.end())
            m.push_back(v.name);
    return m;
}
std::vector<uint32_t> paints(const std::string &model) {
    std::vector<uint32_t> p;
    for (const auto &v : g_vehicles)
        if (v.name == model && is_online_driver_choice(v)) p.push_back(v.number);
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
    std::fprintf(stderr, "[CATALOG] vehicles:");
    for (auto &v : g_vehicles) std::fprintf(stderr, " #%u '%s'", v.number, v.name.c_str());
    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "[CATALOG] %zu models\n", models().size());
}
}  // namespace session_catalog
namespace {

void start() {
    const char *text = std::getenv("EOD_DIRECT_START");
    auto map = eod::vfs::active_map();
    if (!map && (!text || !*text)) return;
    std::string err;
    if (text && *text && !session_parse(text, P.req, err)) {
        std::fprintf(stderr, "[SESSION] %s\n", err.c_str());
        return;
    }
    if (map) {
        P.req.mode = "action";
        P.req.track = map->base_arena;
        P.req.event = "Battle";
        P.req.ai = 0;
        if (P.req.car.empty()) P.req.car = "Algar";
    }
    P.requested = true;
    if (P.req.mode == "career") {
        P.career = true;
        std::fprintf(stderr, "[SESSION] career start (the Career on the memory card, or a new one), then %s%s\n",
                     lower(P.req.place) == "here" ? "where it was saved" : career_place_name(career_place_index(P.req.place)),
                     P.req.eve >= 0 ? (", then Eve " + std::to_string(P.req.eve)).c_str() : "");
        return;
    }
    if (ee_r32(kEveCount) == 0 || ee_r32(kMode) != kActionMode) {
        log("mode is %x with %u Eves: selecting Action (0x123958)", ee_r32(kMode), ee_r32(kEveCount));
        native(f_00123958);
    }
    // GameDoHeadToHead loads its own catalog (mode 0x131), which is only available
    // after the join screen has registered the players.
    const bool split = P.req.humans > 1;
    const int eve = split ? -1 : find_eve(P.req.track);
    if (!split && eve < 0) { std::fprintf(stderr, "[SESSION] unknown track '%s'\n", P.req.track.c_str()); return; }
    const std::string event = session_event_name(P.req);
    if (blank_creator_eve(eve)) battle_events_begin_blank_map(static_cast<uint32_t>(eve));
    const int contest = split ? -1 : find_contest((uint32_t)eve, event);
    if (!split && contest < 0) { std::fprintf(stderr, "[SESSION] Eve %d has no event '%s'\n", eve, event.c_str()); return; }
    const long car = find_car(P.req.car, P.req.paint);
    if (car < 0) { std::fprintf(stderr, "[SESSION] no vehicle '%s' paint %d\n", P.req.car.c_str(), P.req.paint); return; }
    P.car = (uint32_t)car;
    if (split)
        std::fprintf(stderr, "[SESSION] head-to-head request: track '%s' event '%s' vehicle %ld humans %d\n",
                     P.req.track.c_str(), event.c_str(), car, P.req.humans);
    else {
        P.eve = (uint32_t)eve;
        P.contest = (uint32_t)contest;
        const std::string track = guest_text(kEves + P.eve * kEveStride + 0x28, 40);
        std::fprintf(stderr, "[SESSION] start: Eve %d '%s' contest %d '%s' vehicle %ld humans %d\n",
                     eve, track.c_str(), contest, event.c_str(), car, P.req.humans);
    }
    P.armed = true;
    // MenuDoRace's "single" branch (0x2C0E4C-0x2C0E74), minus the menu: bit 0 of the global
    // flags is held while the session runs, the profile name goes to human 0, then the
    // single player driver (which sets the human count and pad map itself).
    const uint32_t old = ee_r32(kFlags) & 1;
    ee_w32(kFlags, ee_r32(kFlags) | 1);
    if (split) {
        // MenuDoRace's "multi" branch (0x2C0DF4-0x2C0E18) minus the menu and its pad-count prompt: the
        // head-to-head driver, whose join/car screen (0x2B7710 below) sets the human count and cars.
        native(f_002A3220);
    } else {
        const uint32_t profile = native(f_00119428, {0});
        native(f_00128CB0, {profile, 0});
        native(f_002C1338);
        native(f_002A2C98);
    }
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
    if (info.event == session_catalog::kHumanBattle) {
        for (const auto &track : session_catalog::tracks())
            if (track.name == info.track) P.req.event = track.human_battle_contest;
        P.req.ai = 0;
    } else if (netplay_impact_fixture::enabled()) {
        P.req.ai = 0;
    }
    if (session_test_native_lap()) { P.req.laps = 1; P.req.ai = 0; }
    P.requested = true;
    P.armed = true;
    if (!info.track.empty()) {
        const int eve = find_eve(info.track);
        if (blank_creator_eve(eve)) battle_events_begin_blank_map(static_cast<uint32_t>(eve));
        const int contest = eve < 0 ? -1 : find_contest((uint32_t)eve, P.req.event);
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

void arm_offline_match_plan(const PendingOfflineMatch &m) {
    P = Plan{};
    P.requested = true;
    P.req.mode = "action";
    P.req.track = m.track;
    P.req.event = m.event;
    P.req.humans = std::clamp(m.humans, 1, 4);
    P.req.ai = m.ai_bots;
    P.req.laps = m.laps;
    P.req.seat_vehicles = m.seat_vehicles;
    P.req.seat_paints = m.seat_paints;
    P.req.seat_cars = m.seat_cars;
    P.req.local_human_mask = m.local_human_mask ? m.local_human_mask : static_cast<uint16_t>((1u << P.req.humans) - 1u);
    if (!m.seat_cars[0].empty()) P.req.car = m.seat_cars[0];
    else if (m.seat_vehicles[0] != 0) P.req.car = std::to_string(m.seat_vehicles[0]);
    P.req.paint = m.seat_paints[0];

    if (ee_r32(kEveCount) == 0 || ee_r32(kMode) != kActionMode) native(f_00123958);
    const bool split = P.req.humans > 1;
    const int eve = split ? -1 : find_eve(P.req.track);
    if (!split && eve < 0) {
        std::fprintf(stderr, "[SESSION] offline launch: unknown track '%s'\n", P.req.track.c_str());
        P = Plan{};
        return;
    }
    const std::string event = session_event_name(P.req);
    if (!split && blank_creator_eve(eve)) battle_events_begin_blank_map(static_cast<uint32_t>(eve));
    const int contest = split ? -1 : find_contest(static_cast<uint32_t>(eve), event);
    if (!split && contest < 0) {
        std::fprintf(stderr, "[SESSION] offline launch: track '%s' has no event '%s'\n", P.req.track.c_str(), event.c_str());
        P = Plan{};
        return;
    }
    long car = m.seat_vehicles[0] ? static_cast<long>(m.seat_vehicles[0]) : find_car(P.req.car, P.req.paint);
    if (car < 0) {
        const auto &vehs = session_catalog::vehicles();
        for (const auto &v : vehs) {
            if (session_catalog::is_online_driver_choice(v)) { car = static_cast<long>(v.number); break; }
        }
    }
    if (car < 0) {
        std::fprintf(stderr, "[SESSION] offline launch: no valid vehicle found\n");
        P = Plan{};
        return;
    }
    P.car = static_cast<uint32_t>(car);
    if (!split) {
        P.eve = static_cast<uint32_t>(eve);
        P.contest = static_cast<uint32_t>(contest);
    }
    P.armed = true;
}
int consume_main_menu_transition() {
    if (g_pending_transition.target == TransitionTarget::None) return 0;
    const SessionTransition tr = g_pending_transition;
    g_pending_transition = SessionTransition{};
    battle_events_end_blank_map();
    eod::vfs::global_vfs().unmount(eod::vfs::Layer::EphemeralPie);
    if (ee_r32(kMode) != kActionMode) native(f_00123958);
    switch (tr.target) {
    case TransitionTarget::Career:
        P = Plan{};
        P.career = true;
        P.career_entered = true;
        P.career_classic_hub = tr.classic_career_hub;
        if (tr.career_warp.active || tr.career_warp.target_eve >= 0) {
            career_warp_arm(tr.career_warp.target_place, tr.career_warp.target_eve, tr.career_warp.delay_frames);
        } else {
            career_warp_clear();
        }
        log("transition -> CAREER (15)");
        return 15;
    case TransitionTarget::Solo:
    case TransitionTarget::SplitScreen:
        session_set_pending_offline(tr.offline);
        arm_offline_match_plan(tr.offline);
        if (P.armed) {
            log("transition -> ACTION (16), humans=%u", (uint32_t)P.req.humans);
            return 16;
        }
        session_clear_pending_offline();
        return 0;
    case TransitionTarget::OnlineHost:
    case TransitionTarget::OnlineJoinCode:
    case TransitionTarget::OnlineJoinAddress:
    case TransitionTarget::OnlineJoinCrew:
        if (netplay_hooks::menu_enter) netplay_hooks::menu_enter();
        if (g_online_transition_handler) g_online_transition_handler(tr);
        return 0;
    case TransitionTarget::Hub:
        if (g_hub_arrival_handler) g_hub_arrival_handler();
        return 0;
    case TransitionTarget::None:
    default:
        return 0;
    }
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
        else if (key == "ai") { if (!number(val, v) || v < 0 || v > 15) { err = "bad ai '" + val + "'"; return false; } r.ai = (int)v; }
        else if (key == "laps") { if (!number(val, v) || v < 1 || v > 50) { err = "bad laps '" + val + "'"; return false; } r.laps = (int)v; }
        else if (key == "place") r.place = val;
        else if (key == "eve") { if (!number(val, v) || v < 0 || v > 63) { err = "bad eve '" + val + "'"; return false; } r.eve = (int)v; }
        else { err = "unknown key '" + key + "'"; return false; }
    }
    if (r.laps >= 0 && r.mode != "action") { err = "laps= requires Action mode"; return false; }
    if (r.mode == "career") {
        if (!r.track.empty() || !r.event.empty() || r.ai >= 0)
            { err = "career takes place= and eve= (and optional car=), not track/event/ai"; return false; }
        if (r.place.empty()) r.place = "home";
        if (lower(r.place) != "here" && career_place_index(r.place) < 0) {
            err = "unknown place '" + r.place + "' (here, home, malt, next_eve, dealer, paintshop, gate1, gate2, petes)";
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
    {"home", {"hub", "trailer", "your home"}, "home (Career hub)"},
    {"malt", {"maltshop", "tj's", "tjs"}, "TJ's Diner (Malt Shop)"},
    {"next_eve", {"eve", "next eve", "choose eve"}, "Choose Eve"},
    {"junkyard", {"dealer", "scrapyard", "sweeny's"}, "dealer (Sweeny's Scrapyard)"},
    {"paintshop", {"garage", "upgrades", "paint"}, "Garage (Upgrades and Paint)"},
    {"gate1", {"gate 1", "time challenge 1", "time1"}, "Time Challenge (Gate 1)"},
    {"gate2", {"gate 2", "time challenge 2", "time2"}, "Time Challenge (Gate 2)"},
    {"petes", {"troy", "troy's", "troy's place"}, "Troy's Place (challengers)"},
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

void career_warp_arm(int place, int eve, int delay_frames) {
    g_career_warp.active = (place >= 0 && place < kCareerPlaces);
    g_career_warp.target_place = g_career_warp.active ? place : -1;
    g_career_warp.target_eve = eve;
    g_career_warp.delay_frames = std::max(0, delay_frames);
    g_career_warp.frames_elapsed = 0;
}

void career_warp_clear() {
    g_career_warp = PendingCareerWarp{};
}

PendingCareerWarp &career_warp_state() { return g_career_warp; }

void session_set_pending_offline(const PendingOfflineMatch &m) {
    g_pending_offline = m;
    g_pending_offline.active = true;
}

const PendingOfflineMatch &session_pending_offline() { return g_pending_offline; }

void session_clear_pending_offline() { g_pending_offline = PendingOfflineMatch{}; }

void session_set_online_transition_handler(void (*fn)(const SessionTransition &)) {
    g_online_transition_handler = fn;
}

void session_set_hub_arrival_handler(void (*fn)()) {
    g_hub_arrival_handler = fn;
}

bool session_transition_pending() { return g_pending_transition.target != TransitionTarget::None; }

bool session_unwind_active() { return g_unwind_active || g_unwind_career; }

EEStateKind session_ee_state() {
    if (!g_ram) return g_ee_state;
    if (ee_r32(0x003CDA04) != 0) return EEStateKind::CareerRoam;
    if (ee_r32(0x003C4C04) == 0x130) {
        return ee_r32(0x00437BB8) != 0 ? EEStateKind::CareerEve : EEStateKind::CareerHub;
    }
    if (P.net && P.armed) return EEStateKind::OnlineMatch;
    if (ee_r32(0x003C4C04) == kHeadToHeadMode) return EEStateKind::SplitScreen;
    if (ee_r32(0x00437BB8) != 0) return EEStateKind::SoloAction;
    return g_ee_state;
}

void session_cancel_transition() {
    g_pending_transition = SessionTransition{};
    g_unwind_active = false;
    g_unwind_career = false;
}

void session_request_transition(const SessionTransition &t) {
    g_pending_transition = t;
    const EEStateKind st = session_ee_state();
    if (st != EEStateKind::MainMenu && st != EEStateKind::Boot) {
        g_unwind_active = true;
        if (st == EEStateKind::CareerHub || st == EEStateKind::CareerRoam || st == EEStateKind::CareerEve) {
            if (t.save_career_first && g_career_save_handler) {
                g_career_save_handler();
            }
            g_unwind_career = true;
            // Clear Career Free Roam HomeDriver driving flag / New Career loop flag (0x003D937C)
            if (g_ram) ee_w32(0x003D937C, 0);
        }
        session_abort_event();
    }
}

int session_ai_override() {
    if (netplay_hooks::online_ai_override) {
        const int ov = netplay_hooks::online_ai_override();
        if (ov >= 0) return ov;
    }
    if (g_pending_offline.active && g_pending_offline.ai_bots >= 0) {
        return g_pending_offline.ai_bots;
    }
    return (P.requested && P.req.mode == "action") ? P.req.ai : -1;
}

uint32_t session_single_event_contest() {
    if (P.armed && P.contest != 0xFFFFFFFFu &&
        (P.net || g_pending_offline.active || P.req.mode == "action")) {
        return P.contest;
    }
    return 0xFFFFFFFFu;
}

bool session_test_native_lap() {
    static const bool enabled = [] {
        const char *fixture = std::getenv("EOD_NETPLAY_TEST_NATIVE_LAP");
        return netplay_impact_fixture::test_session() && fixture && !std::strcmp(fixture, "1");
    }();
    return enabled;
}

std::string session_event_name(const SessionRequest &r) {
    if (r.ai >= 0 && ieq(r.event, "battle")) return r.ai == 1 ? "Battle (1v1)" : "Battle (" + std::to_string(r.ai) + " AI)";
    return r.event;
}

// Title screen ("press the START button", start.lyt): returns 1 once the player passed it.
// A direct start or FastBoot (default ON in eod.ini) jumps straight to the Main Menu.
extern "C" void f_002BEC08(void) {
    // Developer: EOD_FATAL_TEST=1 raises a game panic at the title screen, to check the in-game
    // error screen and crash.log (a windowed run; headless runs just log and abort).
    if (std::getenv("EOD_FATAL_TEST")) fatal("developer panic test (EOD_FATAL_TEST)");
    const char *text = std::getenv("EOD_DIRECT_START");
    if ((text && *text) || eod::vfs::active_map()) {
        std::fprintf(stderr, "[SESSION] title screen skipped (EOD_DIRECT_START)\n");
        ret_s32(1);
        return;
    }
    static bool fast_boot_skipped_title = false;
    const char *pad_script = std::getenv("EOD_PAD_SCRIPT");
    if (!fast_boot_skipped_title && g_config.fast_boot && !(pad_script && pad_script[0])) {
        fast_boot_skipped_title = true;
        std::fprintf(stderr, "[BOOT] title screen skipped (FastBoot=1)\n");
        ret_s32(1);
        return;
    }
    netplay_hooks::title_screen = true;
    f_002BEC08_orig();
    netplay_hooks::title_screen = false;
}

extern "C" void f_002A1298(void) {
    eod_profile::mark_main_menu();
    g_ee_state = EEStateKind::MainMenu;
    // Unconditional first-entry warmup while kMode == 0x12F (Action), BEFORE any
    // EOD_DIRECT_START="mode=career" or Career transition switches kMode to 0x130.
    if (!session_catalog::ready()) {
        session_catalog::refresh();
    }
    static bool tried = false;
    if (!tried) {
        tried = true;
        start();
        battle_events_end_blank_map();
        eod::vfs::global_vfs().unmount(eod::vfs::Layer::EphemeralPie);
        if (std::getenv("EOD_CATALOG_DUMP") && session_catalog::refresh()) session_catalog::dump();
    }
    // Clear unwind flags and previous launch-scoped offline match on arrival at MainMenu.
    if (g_unwind_active || g_unwind_career) {
        g_unwind_active = false;
        g_unwind_career = false;
        battle_events_end_blank_map();
        eod::vfs::global_vfs().unmount(eod::vfs::Layer::EphemeralPie);
        if (ee_r32(kMode) != kActionMode) native(f_00123958);
        P = Plan{};
    }
    if (P.armed && !P.net && (P.car_done || P.players_done || P.event_done)) {
        battle_events_end_blank_map();
        session_clear_pending_offline();
        P = Plan{};
    }
    if (P.career && P.career_entered) {
        P.career = false;
        P.career_entered = false;
        P.career_driving = false;
        P.career_classic_hub = false;
    }
    // Consume any queued SessionTransition cleanly from MainMenu (kMode == 0x12F).
    if (const int code = consume_main_menu_transition()) {
        ret_s32(code);
        return;
    }
    if (P.career && !P.career_entered) {
        // MainMenuUpdate's result 15 is CAREER: the dispatcher (0x2A2720, table 0x414460) runs the Career driver
        // 0x112298 exactly as when the player picks it. Nothing of the main menu has been built yet.
        P.career_entered = true;
        log("main menu answered CAREER (15)");
        ret_s32(15);
        return;
    }
    if (P.net) {   // the online event that was running has returned to the main menu
        battle_events_end_blank_map();
        P = Plan{};
        if (netplay_hooks::event_ended) netplay_hooks::event_ended();
    }
    if (netplay_hooks::menu_enter) netplay_hooks::menu_enter();
    if (g_hub_arrival_handler) g_hub_arrival_handler();
    f_002A1298_orig();
    if (netplay_hooks::menu_leave) netplay_hooks::menu_leave();
}

// MenuRun runs every native menu screen. The only one answered here is the Arcade "single player or
// multiplayer?" prompt (s_and_m.lyt, the call at 0x2C0DC0 in ArcadeSingleOrMultiPromptScreen 0x2C0BB8):
// online, every game has exactly one local human and the others are puppets, so the answer is always
// "single" (1). When that single-player session returns the prompt loops; answering "back" (-1) leaves
// the Arcade menu for the main menu, where the lobby reopens.
bool head_to_head_menu();  // below: answers the head-to-head join screen

namespace {
uint32_t find_vehicle_record(uint32_t veh_number, uint32_t *out_table_idx = nullptr) {
    const uint32_t table = ee_r32(kVehicleTable), count = ee_r32(kVehicleCount);
    if (!table || !count || count > 4096) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t v = table + i * kVehicleStride;
        if (ee_r32(v + 4) == veh_number) {
            if (out_table_idx) *out_table_idx = ee_r32(v);
            return v;
        }
    }
    return 0;
}

// 0x0012A0B8 searches kDriverTable (0x003CBEE0) to set human_slot[0].vehicle_index (0x00439708+0x4C).
// Vehicles without a kDriverTable entry (Trailer..Trailer5, BuyCar*, custom cars) miss 0x0012A0B8 and
// fall back to index 0. Explicitly bind human_slot[0].vehicle_index from kVehicleTable.
uint32_t apply_human_vehicle_override(uint32_t veh_number) {
    uint32_t table_idx = 0;
    const uint32_t v = find_vehicle_record(veh_number, &table_idx);
    if (v) {
        ee_w32(0x00439708 + 0x4C, table_idx);
    }
    return v;
}

// Exiting Event 1 in a multi-event Eve leaves the global fader (0x003CDAA8) in state 3 (HELD BLACK).
// Entering RaceDayDo (0x0011F640) in Action mode calls RaceModeSwitch(0x1200, 0) (0x00175648), where
// bit 0x1000 skips f_00176630 fade-in, causing 0x0036E568 to draw a solid black quad over the 3D
// track behind Event 2 of 5 (tuevent_mp.lyt). Clear the held black fader on inter-event menus.
void clear_held_black_fader_for_menu() {
    if (ee_r32(0x003CDAB4) >= 2) {
        ee_w32(0x003CDAB4, 0);
        ee_w8(0x003CDABC, 0xFF);
        const uint32_t world = ee_r32(0x00495C48);
        if (world && !(world & 3) && world <= EE_RAM_SIZE - 0x1968) {
            ee_w32(world + 0x1964, 0);
        }
    }
}
}  // namespace

extern "C" void f_00270C10(void) {
    const uint32_t caller = g_ee.r[31].uw[0];
    if (ee_r32(0x004A1AD8) == 0x0036E568 || (caller >= 0x00376000 && caller < 0x0037B000)) {
        clear_held_black_fader_for_menu();
    }
    // FastBoot (default ON): bypass artificial 1-second pre-load transition cards
    // (f_002C18B8 / 0x2C1A0C, 0x2C2018, f_002C2058 / 0x2C2178, f_002C2420 / 0x2C26F4)
    // and the 4-second boot memory card notice (f_002C6068 / 0x2C6118).
    {
        const char *slow = std::getenv("EOD_SLOW_LOAD");
        const char *pad_script = std::getenv("EOD_PAD_SCRIPT");
        const bool fast = g_config.fast_boot && !(slow && slow[0] == '1') && !(pad_script && pad_script[0]);
        if (fast && (caller == 0x002C1A0C || caller == 0x002C2018 || caller == 0x002C2178 || caller == 0x002C26F4)) {
            ret_s32(-2);
            return;
        }
        if (fast && g_config.skip_memcard && caller == 0x002C6118) {
            ret_s32(-2);
            return;
        }
    }
    if (caller == 0x002A16D8) {
        // Synchronous transition dispatch from MainMenu without waiting on native input.
        if (const int code = consume_main_menu_transition()) {
            ret_s32(code);
            return;
        }
        netplay_hooks::main_selection_active = true;
        f_00270C10_orig();
        netplay_hooks::main_selection_active = false;
        // The main dispatcher maps result 16 to Arcade, 15 to Career.
        if (P.armed && P.net) ret_s32(16);
        else if (const int code = consume_main_menu_transition()) ret_s32(code);
        return;
    }
    if (caller == 0x002C1108) {
        if (g_unwind_active) { ret_s32(-1); return; }
        if (P.armed && (P.net || g_pending_offline.active)) {
            ret_s32(P.mode_answered ? -1 : 0); // Arcade: Race on entry, Back on return.
            return;
        }
    }
    if (caller == 0x002C0DC8) {
        if (g_unwind_active) { ret_s32(-1); return; }
        if (P.armed && P.net) {
            if (!P.mode_answered) { P.mode_answered = true; ret_s32(1); log("online: single/multi prompt answered 'single'"); }
            else ret_s32(-1);
            return;
        }
        if (P.armed && g_pending_offline.active) {
            if (!P.mode_answered) {
                P.mode_answered = true;
                ret_s32(P.req.humans > 1 ? 0 : 1); // 1 = Single, 0 = Multi (Head-to-Head)
                log("offline: single/multi prompt answered %u", P.req.humans > 1 ? 0u : 1u);
            } else {
                ret_s32(-1);
            }
            return;
        }
    }
    if (caller == 0x002B8120) {
        if (g_unwind_active) { ret_s32(1); return; }
        if (P.armed && !P.net && P.req.humans > 1 && head_to_head_menu()) return;
    }
    if (g_unwind_career) {
        if (caller == 0x0036AB8C) {
            ee_w32(0x003D937C, 0); // Clear New Career flag read by f_002DE1A0
            log("career unwind: hub answered Exit Career (7)");
            ret_s32(7);
            return;
        }
        if (caller == 0x00272030) {
            log("career unwind: auto-confirmed Exit dialog (-2)");
            ret_s32(-2);
            return;
        }
    }
    if (P.career && P.career_entered && !P.career_driving && !P.career_classic_hub) {
        // A fresh Career's opening screens (callers from EOD_MENU_RUN_TRACE): the tutorial note (0x3861B4) and the
        // "select Drive Around" note (0x383C94) close with -2; the Career hub (0x36AB8C) answers Drive Around (4).
        // The loading screens (0x2C1A0C) run natively.
        if (caller == 0x003861B4 || caller == 0x00383C94) { log("career: note %08x closed", caller); ret_s32(-2); return; }
        if (caller == 0x0036AB8C) { P.career_driving = true; log("career: hub answered Drive Around (4)"); ret_s32(4); return; }
    }
    arcade_mod_badges_run_menu(caller, f_00270C10_orig);
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
    if (g_unwind_active || (P.armed && (P.net || g_pending_offline.active) && P.car_done)) {
        log("car screen unwind -> leave GameDoSingle");
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
        apply_human_vehicle_override(P.car);
        log("car select answered: vehicle %u", P.car);
        ret_s32(1);
        return;
    }
    f_002A75E0_orig();
}

// Head-to-head join/car screen: (i32 *players, i32 cars[]). Answered once with the requested human count.
// The screen's play branch sorts active join records by pad and registers the pad,
// vehicle, name and profile through the same path as the retail menu.
bool head_to_head_menu() {
    if (P.players_done) { log("head-to-head: back to the front end"); ret_s32(1); return true; }
    const bool multitap = native(f_00335110) != 0;
    const int capacity = multitap ? 4 : 2;
    uint32_t pads[4];
    int connected = 0;
    for (int seat = 0; seat < capacity; ++seat) {
        // Lobby conversion 0x2BAA78 maps panels to game pad IDs 0,4 or 0,1,2,3.
        const uint32_t pad = multitap ? (uint32_t)seat : (uint32_t)seat * 4;
        if (native(f_003356D0, {pad})) pads[connected++] = pad;
    }
    if (connected < P.req.humans || native(f_002E66F8) < (uint32_t)P.req.humans) {
        log("head-to-head: %u seats available for %u requested, using native join lobby",
            (uint32_t)connected, (uint32_t)P.req.humans);
        P.armed = false;
        return false;
    }
    // Resolve per-seat vehicles (seat_vehicles[i] / seat_cars[i] + seat_paints[i]),
    // keeping vehicle numbers unique across seats (native roster keys humans by vehicle number).
    const auto fallback_cars = split_vehicle_numbers(P.car, (size_t)P.req.humans);
    std::vector<uint32_t> cars;
    cars.reserve((size_t)P.req.humans);
    for (int i = 0; i < P.req.humans; ++i) {
        long chosen = -1;
        if (P.req.seat_vehicles[i] != 0) {
            chosen = static_cast<long>(P.req.seat_vehicles[i]);
        } else if (!P.req.seat_cars[i].empty()) {
            chosen = find_car(P.req.seat_cars[i], P.req.seat_paints[i]);
        }
        if (chosen > 0 && std::find(cars.begin(), cars.end(), static_cast<uint32_t>(chosen)) == cars.end()) {
            cars.push_back(static_cast<uint32_t>(chosen));
            continue;
        }
        bool added = false;
        for (uint32_t fb : fallback_cars) {
            if (std::find(cars.begin(), cars.end(), fb) == cars.end()) {
                cars.push_back(fb);
                added = true;
                break;
            }
        }
        if (!added) {
            // Fall back to any remaining drivable vehicle from kVehicleTable
            const uint32_t table = ee_r32(kVehicleTable), count = ee_r32(kVehicleCount);
            for (uint32_t k = 0; k < count && k < 4096; ++k) {
                const uint32_t num = ee_r32(table + k * kVehicleStride + 4);
                if (num && std::find(cars.begin(), cars.end(), num) == cars.end()) {
                    cars.push_back(num);
                    added = true;
                    break;
                }
            }
        }
    }
    if (cars.size() < (size_t)P.req.humans) {
        log("head-to-head: could not resolve %u distinct vehicles, using native join lobby",
            (uint32_t)P.req.humans);
        P.armed = false;
        return false;
    }
    P.req.local_human_mask = static_cast<uint16_t>((1u << P.req.humans) - 1u);
    P.players_done = true;
    for (uint32_t i = 0; i < (uint32_t)P.req.humans; ++i) {
        const uint32_t join = native(f_002B9858); // Sets active and increments the join count.
        ee_w32(join, pads[i]);
        ee_w32(join + 0x04, cars[i]);
        const uint32_t name = native(f_002E66D8, {i});
        char display_name[18]{};
        ee_read_string(name, display_name, sizeof(display_name));
        for (uint32_t k = 0; k < sizeof(display_name); ++k)
            ee_w8(join + 0x08 + k, (uint8_t)display_name[k]);
        ee_w32(join + 0x1C, native(f_002E66B8, {i}));
        log("head-to-head: seat %u joined on pad %u, vehicle %u", i + 1, pads[i], cars[i]);
    }
    ret_s32(0);
    return true;
}

// Track screen: *eve = chosen Eve, *aux = 5 (its initial value), options+0x28 = 1 (both
// written by the native screen before it opens); nonzero return = play.
// When the online event finishes and unwinds back to the middle loop, P.track_done is already
// true, so returning 0 exits the middle loop back to the outer loop (f_002A75E0).
extern "C" void f_002C7340(void) {
    if (g_unwind_active || (P.armed && (P.net || g_pending_offline.active) && P.track_done)) {
        log("track screen unwind -> outer loop");
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
        if (P.players_done) {
            const int eve = find_eve(P.req.track);
            if (const char *reason = head_to_head_track_unavailable(eve)) {
                std::fprintf(stderr, "[SESSION] head-to-head: track '%s' %s, using native track picker\n",
                             P.req.track.c_str(), reason);
                P.armed = false;
                f_002C7340_orig();
                return;
            }
            P.eve = (uint32_t)eve;
            const std::string track = guest_text(kEves + P.eve * kEveStride + 0x28, 40);
            std::fprintf(stderr, "[SESSION] head-to-head: live track Eve %u '%s'\n", P.eve, track.c_str());
        }
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
    const uint32_t session = g_ee.r[4].uw[0];
    int laps = -1;
    uint32_t selected_contest = 0;
    if (P.event_done) {
        clear_held_black_fader_for_menu();
    }
    long override_car = -1;
    if (P.armed && P.req.humans <= 1) {
        override_car = (long)P.car;
    } else if (P.career && !P.req.car.empty()) {
        override_car = find_car(P.req.car, P.req.paint);
    }
    if (override_car >= 0) {
        apply_human_vehicle_override((uint32_t)override_car);
    }
    if (P.armed && !P.event_done && P.players_done) {
        P.event_done = true;
        const uint32_t tracker = ee_r32(kTrackerPtr);
        const uint32_t count = tracker ? ee_r32(tracker + kTrackerCount) : 0;
        const std::string wanted = session_event_name(P.req);
        int pos = -1;
        long requested_pos = -1;
        if (!wanted.empty() && wanted[0] == '#') {
            if (number(wanted.substr(1), requested_pos) && requested_pos >= 0 &&
                static_cast<unsigned long>(requested_pos) < count)
                pos = (int)requested_pos;
        } else {
            const std::string canonical = canonical_contest_alias(wanted);
            for (uint32_t i = 0; i < count && i < 64; ++i) {
                const uint32_t contest = ee_r32(tracker + kTrackerList + 4 * i);
                const std::string name = guest_text(native(f_001252C0, {contest}), 40);
                if (ieq(name, wanted)) { pos = (int)i; break; }
                if (pos < 0 && ieq(canonical_contest_alias(name), canonical)) pos = (int)i;
            }
        }
        if (pos >= 0) {
            // Native EventSetupSelectInitial below builds the selected event's roster
            // before GameDoHeadToHead can enter the race.
            const uint32_t contest = ee_r32(tracker + kTrackerList + 4 * (uint32_t)pos);
            selected_contest = contest;
            laps = P.req.laps;
            native(f_00120BE8, {(uint32_t)pos});
            log("head-to-head: selected tracker position %u (contest %u)", (uint32_t)pos, contest);
        } else {
            std::fprintf(stderr, "[SESSION] head-to-head: event '%s' unavailable, using native event lobby\n",
                         wanted.c_str());
            P.armed = false;
        }
    }
    if (P.armed && !P.event_done) {
        P.event_done = true;
        const uint32_t tracker = ee_r32(kTrackerPtr);
        const uint32_t count = tracker ? ee_r32(tracker + kTrackerCount) : 0;
        int pos = -1;
        for (uint32_t i = 0; i < count && i < 64; ++i)
            if (ee_r32(tracker + kTrackerList + 4 * i) == P.contest) pos = (int)i;
        // Match by name if the catalog ID does not appear in the active tracker.
        const std::string wanted = P.net ? P.req.event : session_event_name(P.req);
        for (uint32_t i = 0; pos < 0 && i < count && i < 64; ++i)
            if (ieq(guest_text(native(f_001252C0, {ee_r32(tracker + kTrackerList + 4 * i)}), 40), wanted)) pos = (int)i;
        if (pos < 0) {
            std::fprintf(stderr, "[SESSION] events in this list:");
            for (uint32_t i = 0; i < count && i < 64; ++i)
                std::fprintf(stderr, " [%s]", guest_text(native(f_001252C0, {ee_r32(tracker + kTrackerList + 4 * i)}), 40).c_str());
            std::fprintf(stderr, "\n");
        }
        if (pos >= 0) {
            selected_contest = ee_r32(tracker + kTrackerList + 4 * (uint32_t)pos);
            native(f_00120BE8, {(uint32_t)pos});
            laps = P.req.laps;
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
    if (override_car >= 0) {
        const uint32_t v = apply_human_vehicle_override((uint32_t)override_car);
        if (v && session && !(session & 3) && session <= EE_RAM_SIZE - 0x9A8) {
            const uint32_t pcount = ee_r32(session + 0x98C);
            for (uint32_t i = 0; i < pcount && i < battle_layout::kCapacity; ++i) {
                const uint32_t rec = session + 0x2C + i * 0x78;
                if (ee_r32(rec) == 0x50 && (ee_r8(rec + 0x70) & 3) == 0) {
                    if (ee_r32(rec + 0x6C) != (uint32_t)override_car) {
                        ee_w32(rec + 0x6C, (uint32_t)override_car);
                        ee_w32(rec + 0x74, ee_r32(v + 0x48));
                        for (uint32_t k = 0; k < 32; ++k) ee_w8(rec + 0x44 + k, ee_r8(v + 8 + k));
                        if (P.career) ee_w8(rec + 0x70, ee_r8(rec + 0x70) & ~4u);
                        log("human slot 0 vehicle record bound to %u", (uint32_t)override_car);
                    }
                    break;
                }
            }
        }
    }
    if (laps < 0 || ee_r32(session + kSessionContest) != selected_contest) return;
    if (!(ee_r32(session + kSessionEventType) & kLapRaceFlag)) {
        log("laps= ignored for contest %u (no lap race)", selected_contest);
        return;
    }
    // Native setup scales the catalog lap count by the profile race length, then copies it here.
    // 0x114860 uses this session descriptor to initialize the race. Override the final count
    // for this race while keeping the catalog and profile available for later native events.
    if (session_test_native_lap() && P.net)
        ee_w32(session + kSessionAIStrength, kHardAIStrength);
    const uint32_t previous = ee_r32(session + kSessionLaps);
    ee_w32(session + kSessionLaps, (uint32_t)laps);
    log("race lap count set to %u (was %u, contest %u)", (uint32_t)laps, previous, selected_contest);
}

// EveHasMoreEvents (0x120FB8) is checked before each contest in GameDoSingle's inner loop
// (0x2A2FC4). Once the single online event has run (P.event_done is true), returning 0 exits
// the inner Eve loop instead of opening the native in-world Eve event selection screen (0x11F640).
#include "decomp_race_lifecycle.h"

extern "C" void f_00120FB8(void) {
    if (g_unwind_active || (P.armed && (P.net || g_pending_offline.active) && P.event_done)) {
        log("event finished / unwind -> leave Eve inner loop");
        ret_s32(0);
        return;
    }
    eod_race_00120FB8();
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
extern "C" void f_00114358() {
    const bool online = P.net && P.armed && g_ee.r[31].uw[0] == 0x00114BA0;
    eod_race_00114358();
    if (online) ee_w32(0x00437BA8, 1);
}
