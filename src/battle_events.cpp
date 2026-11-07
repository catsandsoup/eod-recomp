// Battle on every map, strictly additive.
//
// Native Eve catalogs load per game mode (0x124E08 -> 0x124870 -> 0x1244A8, then
// the 0x1248E0 sort/remap). After the outermost load of the Action (0x12F) and
// multiplayer (0x131) catalogs, each visible track Eve gains two extra contests,
// "Battle (4 AI)" and "Battle (8 AI)", appended after every native contest.
// Native contests keep their index, values and order; Career, challenge and demo
// catalogs are never touched. The contest table lives in runtime-owned memory
// with room for 512 contests (tools/recomp.py relocates it from 0x003C61A8,
// where only 320 fit and the Action catalog already uses 312).
//
// Each added contest reuses the track's own native Battle descriptor (every
// retail track archive except Welch's ships c_battle.va0 and a multiplayer
// Battle section). Welch has neither; its Battle descriptor is a small VAL
// section in runtime memory, and its respawn table comes from the composed
// Battle alternate (battle-alternates/TRACKS/WELCH/c_battle.va0), supplied only
// when a Battle contest loads its external table. The retail WELCH.TRK is
// served unchanged for every event. Multiplayer has no native Welch Eve; one is
// added from the Welch Action Eve section, holding only the two Battle contests.
#include "battle_events.h"
#include "battle_layout.h"

#include "core/config.h"
#include "core/cpu.h"
#include "core/memory.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" void f_001244A8_orig(void);
extern "C" void f_001252C0_orig(void);
extern "C" void f_0010AFA0_orig(void);
extern "C" void f_001515B8_orig(void);
extern "C" void f_00135BB0(void);

namespace {
using namespace battle_layout;
constexpr uint32_t kEves = 0x003C4D08, kEveStride = 0x84, kEveCapacity = 40;
constexpr uint32_t kEveCount = 0x003C4CF4, kContestCount = 0x003C4CF8, kMode = 0x003C4C04;
constexpr uint32_t kRoots = 0x003C4C70, kRootCount = 0x003C4CF0; // 0x124870 records every VAL root
constexpr uint32_t kBattle = 0x18000224;
constexpr uint32_t kArcade = 0x12F, kMulti = 0x131, kCareerMode = 0x130;
constexpr uint32_t kSectionVtable = 0x003F21B0;
constexpr uint32_t kRootVtable = 0x003F2190;
constexpr uint32_t kPerEve = 32;
// 0x1515B8 external-table globals: VAL root, contest section, path text.
constexpr uint32_t kExternalRoot = 0x003CD5C8, kExternalSection = 0x003CD5CC, kExternalPath = 0x003CD5D0;
// Variant codes: kHeadToHead is "Battle (1v1)" (humans only, multiplayer);
// otherwise the code is the AI count added to the humans.
constexpr uint32_t kHeadToHead = 1;

unsigned load_depth = 0;
std::array<uint8_t, kContestCapacity> variant_ai{};     // 0: native contest
std::array<uint32_t, kContestCapacity> variant_section{};
std::string alternate_root_override;
uint32_t produced_mode = 0, produced_total = 0; // last table this module completed

bool object(uint32_t address, uint32_t size) {
    return address && !(address & 3) && size <= EE_RAM_SIZE && address <= EE_RAM_SIZE - size;
}

bool string_equals(uint32_t address, const char *expected, bool fold = false) {
    const size_t length = std::strlen(expected);
    if (!address || address >= EE_RAM_SIZE || length >= EE_RAM_SIZE - address) return false;
    for (size_t i = 0; i <= length; ++i) {
        uint8_t a = ee_r8(address + static_cast<uint32_t>(i)), b = static_cast<uint8_t>(expected[i]);
        if (fold) { a = static_cast<uint8_t>(std::tolower(a)); b = static_cast<uint8_t>(std::tolower(b)); }
        if (a != b) return false;
    }
    return true;
}

std::string text(uint32_t address, size_t limit = 32) {
    std::string out;
    while (address && address < EE_RAM_SIZE && out.size() < limit) {
        const char c = static_cast<char>(ee_r8(address++));
        if (!c) break;
        out.push_back(c);
    }
    return out;
}

uint32_t put_text(uint32_t &cursor, const char *s) {
    const uint32_t at = cursor;
    const size_t n = std::strlen(s) + 1;
    for (size_t i = 0; i < n; ++i) ee_w8(at + static_cast<uint32_t>(i), static_cast<uint8_t>(s[i]));
    cursor += static_cast<uint32_t>((n + 3) & ~size_t{3});
    return at;
}

bool valid_section_chain(uint32_t section) {
    for (unsigned depth = 0; depth < 32; ++depth) {
        if (!section) return true;
        if (!object(section, 0x1C) || ee_r32(section) != kSectionVtable) return false;
        const uint32_t count = ee_r32(section + 0xC);
        if (count > 1024 || (count && !object(ee_r32(section + 0x10), count * 8))) return false;
        section = ee_r32(section + 0x14);
    }
    return false;
}

// Mirrors the native VAL lookup's parent traversal (0x135C80) without calling
// any virtual method while discovering candidate resource sections.
uint32_t value(uint32_t section, const char *key) {
    for (unsigned depth = 0; section && depth < 32; ++depth) {
        if (!object(section, 0x1C) || ee_r32(section) != kSectionVtable) return 0;
        const uint32_t count = ee_r32(section + 0xC), pairs = ee_r32(section + 0x10);
        if (count > 1024 || (count && !object(pairs, count * 8))) return 0;
        for (uint32_t i = 0; i < count; ++i)
            if (string_equals(ee_r32(pairs + i * 8), key)) return ee_r32(pairs + i * 8 + 4);
        section = ee_r32(section + 0x14);
    }
    return 0;
}

bool valid_root(uint32_t root, uint32_t &count, uint32_t &sections) {
    if (!object(root, 0x10) || ee_r32(root) != kRootVtable) return false;
    // 0x134A70 relocates every section, including those excluded by mode
    // filters. +4 is ALL sections; +8 is only the top-level section count.
    count = ee_r32(root + 4); sections = ee_r32(root + 0xC);
    return count && count <= 4096 && object(sections, count * 0x1C);
}

uint32_t native_battle_section(uint32_t eve_section) {
    if (!object(eve_section, 0x1C) || !valid_section_chain(eve_section)) return 0;
    uint32_t count = 0, sections = 0;
    const uint32_t root = ee_r32(eve_section + 0x18);
    if (!valid_root(root, count, sections)) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t candidate = sections + i * 0x1C;
        if (ee_r32(candidate + 0x18) != root || !valid_section_chain(candidate) ||
            !string_equals(value(candidate, "event_type"), "battle")) continue;
        const uint32_t layout = value(candidate, "external_table");
        if (!layout || string_equals(layout, "c_battle")) return candidate;
    }
    return 0;
}

uint32_t native_integer(uint32_t section, uint32_t key, int32_t fallback) {
    const EEState saved = g_ee;
    g_ee.r[4].ud[0] = section;
    g_ee.r[5].ud[0] = key;
    g_ee.r[6].sd[0] = fallback;
    f_00135BB0(); // ValueTable virtual +0x20, including native inheritance.
    const uint32_t result = g_ee.r[2].uw[0];
    g_ee = saved;
    return result;
}

std::string upper(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string alternate_path(const std::string &track) {
    std::string root = alternate_root_override;
    if (root.empty()) {
        const char *mod = std::getenv("EOD_MOD_DIR");
        if (!mod || !*mod) return {};
        root = std::string(mod) + "/battle-alternates";
    }
    for (char c : track) if (!std::isalnum(static_cast<unsigned char>(c))) return {};
    return root + "/TRACKS/" + upper(track) + "/c_battle.va0";
}

std::vector<uint8_t> read_alternate(const std::string &track) {
    std::vector<uint8_t> data;
    const std::string path = alternate_path(track);
    if (path.empty()) return data;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return data;
    uint8_t buffer[4096];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0 && data.size() <= kExternalTableSize)
        data.insert(data.end(), buffer, buffer + n);
    std::fclose(f);
    if (data.size() > kExternalTableSize) data.clear();
    return data;
}

bool alternate_available(const std::string &track) {
    const std::string path = alternate_path(track);
    if (path.empty()) return false;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// Relocates a VALT image in guest memory exactly as native 0x134A70 does when
// a resource is first fetched: absolute section/pair/name pointers, native
// vtables, root back-pointers, then parent names resolved to sections.
uint32_t relocate_valt(uint32_t base, uint32_t size) {
    if (size < 0x10) return 0;
    const uint32_t count = ee_r32(base + 4);
    if (!count || count > 256 || 0x10 + count * 0x1C > size) return 0;
    auto in_image = [&](uint32_t offset, uint32_t bytes) { return offset <= size && bytes <= size - offset; };
    ee_w32(base, kRootVtable);
    ee_w32(base + 0xC, base + 0x10);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t s = base + 0x10 + i * 0x1C;
        const uint32_t name = ee_r32(s + 4), parent = ee_r32(s + 8), pairs = ee_r32(s + 0xC), table = ee_r32(s + 0x10);
        if ((name && !in_image(name, 1)) || (parent && !in_image(parent, 1)) || pairs > 1024 ||
            !in_image(table, pairs * 8)) return 0;
        for (uint32_t p = 0; p < pairs; ++p) {
            const uint32_t k = ee_r32(base + table + p * 8), v = ee_r32(base + table + p * 8 + 4);
            if (!in_image(k, 1) || !in_image(v, 1)) return 0;
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t s = base + 0x10 + i * 0x1C;
        ee_w32(s, kSectionVtable);
        const uint32_t table = base + ee_r32(s + 0x10);
        ee_w32(s + 0x10, table);
        ee_w32(s + 0x14, 0);
        if (ee_r32(s + 4)) ee_w32(s + 4, base + ee_r32(s + 4));
        if (ee_r32(s + 8)) ee_w32(s + 8, base + ee_r32(s + 8));
        for (uint32_t p = 0; p < ee_r32(s + 0xC); ++p) {
            ee_w32(table + p * 8, base + ee_r32(table + p * 8));
            ee_w32(table + p * 8 + 4, base + ee_r32(table + p * 8 + 4));
        }
        ee_w32(s + 0x18, base);
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t s = base + 0x10 + i * 0x1C, parent = ee_r32(s + 8);
        if (!parent || !ee_r8(parent)) continue;
        bool found = false;
        for (uint32_t j = 0; j < count && !found; ++j) {
            const uint32_t t = base + 0x10 + j * 0x1C;
            if (i != j && string_equals(ee_r32(t + 4), text(parent, 64).c_str())) { ee_w32(s + 0x14, t); found = true; }
        }
        if (!found) return 0;
    }
    return base;
}

struct GuestText { uint32_t name1, name4, name6, name8, event_type, battle, max_cars, two; };
GuestText texts{};

GuestText write_texts() {
    uint32_t cursor = kStrings;
    GuestText t{};
    t.name1 = put_text(cursor, "Battle (1v1)");
    t.name4 = put_text(cursor, "Battle (4 AI)");
    t.name6 = put_text(cursor, "Battle (6 AI)");
    t.name8 = put_text(cursor, "Battle (8 AI)");
    t.event_type = put_text(cursor, "event_type");
    t.battle = put_text(cursor, "battle");
    t.max_cars = put_text(cursor, "max_cars");
    t.two = put_text(cursor, "2");
    texts = t;
    return t;
}

// A generic native-shaped Battle descriptor (retail ones are exactly
// event_type=battle, max_cars=2) owned by the runtime, for a track whose
// catalog has none. Its root is the Eve's own VAL root.
uint32_t synthesized_battle_section(const GuestText &t, uint32_t root) {
    const uint32_t section = kSections, pairs = kSections + 0x20;
    ee_w32(section, kSectionVtable);
    ee_w32(section + 4, t.battle);
    ee_w32(section + 8, 0);
    ee_w32(section + 0xC, 2);
    ee_w32(section + 0x10, pairs);
    ee_w32(section + 0x14, 0);
    ee_w32(section + 0x18, root);
    ee_w32(pairs, t.event_type); ee_w32(pairs + 4, t.battle);
    ee_w32(pairs + 8, t.max_cars); ee_w32(pairs + 12, t.two);
    return section;
}

uint32_t eve_for_track(uint32_t eves, const char *track) {
    for (uint32_t i = 0; i < eves; ++i)
        if (string_equals(kEves + i * kEveStride + 8, track, true)) return kEves + i * kEveStride;
    return 0;
}

// The visible Action Eve section of a track (untagged mode, not hidden), found
// among the VAL roots the native include loader recorded.
uint32_t action_eve_section(const char *track) {
    const uint32_t roots = ee_r32(kRootCount);
    if (roots > 32) return 0;
    for (uint32_t r = 0; r < roots; ++r) {
        uint32_t count = 0, sections = 0;
        const uint32_t root = ee_r32(kRoots + r * 4);
        if (!valid_root(root, count, sections)) continue;
        const uint32_t top = std::min(count, ee_r32(root + 8));
        for (uint32_t i = 0; i < top; ++i) {
            const uint32_t s = sections + i * 0x1C;
            if (!valid_section_chain(s) || !string_equals(ee_r32(s + 4), "eve")) continue;
            const uint32_t mode = value(s, "mode"), hidden = value(s, "hidden");
            if ((mode && ee_r8(mode)) || (hidden && string_equals(hidden, "yes", true))) continue;
            if (string_equals(value(s, "track"), track, true)) return s;
        }
    }
    return 0;
}

// Adds a multiplayer Eve for a track that only has an Action Eve (Welch), from
// that Eve's own section: track/name/date/time/class as the native loader
// would record them. It holds no native contest; only the Battle pair follows.
void add_multiplayer_eve(const char *track) {
    const uint32_t eves = ee_r32(kEveCount);
    if (eves >= kEveCapacity || eve_for_track(eves, track)) return;
    const uint32_t section = action_eve_section(track);
    if (!section) return;
    const uint32_t eve = kEves + eves * kEveStride;
    for (uint32_t i = 0; i < kEveStride; i += 4) ee_w32(eve + i, 0);
    ee_w32(eve, eves);
    const std::string name = text(value(section, "name"), 31), place = text(value(section, "track"), 31);
    for (size_t i = 0; i < place.size(); ++i) ee_w8(eve + 8 + static_cast<uint32_t>(i), static_cast<uint8_t>(place[i]));
    for (size_t i = 0; i < name.size(); ++i) ee_w8(eve + 0x28 + static_cast<uint32_t>(i), static_cast<uint8_t>(name[i]));
    unsigned month = 8, day = 4, hour = 12, minute = 0;
    std::sscanf(text(value(section, "date")).c_str(), "%u/%u", &month, &day);
    std::sscanf(text(value(section, "time")).c_str(), "%u:%u", &hour, &minute);
    const float time = static_cast<float>(hour) + static_cast<float>(minute) / 60.0f;
    uint32_t time_bits;
    std::memcpy(&time_bits, &time, 4);
    ee_w32(eve + 0x68, month);
    ee_w32(eve + 0x6C, day);
    ee_w32(eve + 0x70, 0);
    ee_w32(eve + 0x74, 1); // car_class absent -> native default midsize
    ee_w32(eve + 0x78, time_bits);
    ee_w32(eve + 0x80, section);
    ee_w32(kEveCount, eves + 1);
}

// Career progression is the player's rank (100 at the start, 0 = champion).
// Every Career Eve is scheduled by a [max_rank, min_rank] window in the native
// career_eves.val; the window midpoint says how far into the Career the Eve is.
struct CareerWindow { const char *name; uint8_t max_rank, min_rank; };
constexpr CareerWindow kCareerWindows[] = {
    {"Ashland Smackfest", 90, 100}, {"Woodly Crash Test", 85, 100}, {"Eagan County Derby", 80, 100},
    {"Crunch Time at Hampton", 75, 90}, {"Hastings Hell on Wheels", 70, 85}, {"Dennison Derby", 65, 80},
    {"Kenyon Crash Up", 60, 75}, {"Destruction at Dundas", 55, 70}, {"Mankato Mayhem", 50, 65},
    {"Anoka Anarchy", 45, 60}, {"Gilroy Blood and Guts", 40, 55}, {"Baxter Bust Up", 40, 50},
    {"Ashland Speedfest", 37, 47}, {"Woodly Apocalypse", 34, 44}, {"Eagan Tri County Derby", 30, 40},
    {"Hampton Battles", 27, 37}, {"Havoc at Hastings", 24, 34}, {"Dennison Daredevils", 20, 30},
    {"Kenyon Hard Knocks", 17, 27}, {"Dundas Rust and Bust", 13, 23}, {"Mankato Madness", 8, 18},
    {"Anoka T-Bone Royale", 5, 15}, {"Gilroy Smash and Pass", 2, 10}, {"Baxter Big One", 2, 7},
};
constexpr uint32_t kCareerSeed = 0x7D0B6A11u;
} // namespace

uint32_t battle_events_career_ai(const char *eve_name) {
    if (!eve_name) return 0;
    const CareerWindow *window = nullptr;
    for (const CareerWindow &w : kCareerWindows) if (std::strcmp(w.name, eve_name) == 0) window = &w;
    if (!window) return 0; // unknown or non-schedule Eve (championship, Homebase, modded): native
    uint32_t hash = 2166136261u ^ kCareerSeed;
    for (const char *c = eve_name; *c; ++c) hash = (hash ^ static_cast<uint8_t>(*c)) * 16777619u;
    hash ^= hash >> 15; hash *= 2246822519u; hash ^= hash >> 13;
    if (hash % 5 == 0) return 0; // about one Eve in five stays purely native
    const uint32_t rank = (window->max_rank + window->min_rank) / 2;
    uint32_t ai = rank >= 68 ? 4 : rank >= 36 ? 6 : 8; // early / mid / late Career
    if (ai < 8 && (hash >> 8) % 3 == 0) ai += 2;       // a harder draw now and then
    return ai;
}

namespace {
bool hidden_eve(uint32_t section) {
    const uint32_t hidden = value(section, "hidden");
    return hidden && string_equals(hidden, "yes", true);
}

void append_battles() {
    const uint32_t mode = ee_r32(kMode);
    // A native mode load restarts the table at zero; only a repeated callback on
    // the table this module just completed may keep its variant records.
    if (mode != produced_mode || ee_r32(kContestCount) != produced_total) {
        variant_ai.fill(0);
        variant_section.fill(0);
    }
    produced_mode = mode;
    produced_total = ee_r32(kContestCount);
    if (!g_config.sp_battle_mode || (mode != kArcade && mode != kMulti && mode != kCareerMode)) return;
    const GuestText t = write_texts();
    if (mode == kMulti && alternate_available("Welch")) add_multiplayer_eve("Welch");
    const uint32_t eves = ee_r32(kEveCount);
    uint32_t contests = ee_r32(kContestCount);
    if (!eves || eves > kEveCapacity || contests > kContestCapacity) return;
    bool synthesized = false;
    for (uint32_t i = 0; i < eves; ++i) {
        const uint32_t eve = kEves + i * kEveStride;
        const uint32_t id = ee_r32(eve), current = ee_r32(eve + 0x70), eve_section = ee_r32(eve + 0x80);
        // Multiplayer also offers head-to-head; single player has no 1v1 without AI.
        // Career gets at most one Battle per Eve, chosen by battle_events_career_ai.
        std::vector<uint32_t> variants = mode == kMulti ? std::vector<uint32_t>{kHeadToHead, 4, 8}
                                                        : std::vector<uint32_t>{4, 8};
        if (mode == kCareerMode) {
            variants.clear();
            if (const uint32_t career_ai = battle_events_career_ai(text(eve + 0x28, 40).c_str())) variants.push_back(career_ai);
        }
        const uint32_t added = static_cast<uint32_t>(variants.size());
        if (!added) continue;
        if (current + added > kPerEve || contests + added > kContestCapacity) continue;
        if (!object(eve_section, 0x1C) || !valid_section_chain(eve_section) || hidden_eve(eve_section)) continue;
        bool present = false;
        for (uint32_t c = 0; c < contests && !present; ++c) {
            const uint32_t cp = kContestTable + c * kContestStride;
            present = variant_ai[c] && ee_r32(cp + 4) == id && ee_r32(cp + 8) == kBattle;
        }
        if (present) continue;
        uint32_t section = native_battle_section(eve_section);
        if (!section) {
            const std::string track = text(eve + 8);
            if (synthesized || !alternate_available(track)) continue; // no Battle data: leave the Eve native
            section = synthesized_battle_section(t, ee_r32(eve_section + 0x18));
            synthesized = true;
        }
        // Numeric fields use exactly the native 0x123B98 defaults/terrain rules.
        uint32_t terrain = native_integer(section, 0x003EF2C0, -1); // terrain_mask
        if (terrain == UINT32_MAX) {
            const uint32_t option = native_integer(section, 0x003EF230, 0); // opt_terrain
            if (option > 32) continue;
            terrain = option ? 1u << (option - 1) : 0;
        }
        terrain |= native_integer(eve_section, 0x003EF2C0, 0);
        terrain &= ~native_integer(section, 0x003EF3E0, 0); // terrain_disable_mask
        const uint32_t laps = native_integer(section, 0x003EF220, 0);
        const uint32_t rounds = native_integer(section, 0x003EF3F8, 1);
        uint32_t purse = native_integer(section, 0x003EF250, 1000);
        if (mode == kCareerMode) {
            // The Battle descriptor only carries Action's default; Career pays by
            // Eve tier, so a Career Battle pays what its Eve's native contests pay.
            for (uint32_t c = 0; c < contests; ++c) {
                const uint32_t cp = kContestTable + c * kContestStride;
                if (!variant_ai[c] && ee_r32(cp + 4) == id) { purse = ee_r32(cp + 0x1C); break; }
            }
        }
        for (uint32_t ai : variants) {
            const uint32_t cars = ai == kHeadToHead ? 2 : std::min(ai + 2, kMaxCars);
            const std::array<uint32_t, 10> contest = {
                contests, id, kBattle, laps, cars, terrain, rounds, purse, section, 1};
            const uint32_t cp = kContestTable + contests * kContestStride;
            for (uint32_t word = 0; word < contest.size(); ++word) ee_w32(cp + word * 4, contest[word]);
            variant_ai[contests] = static_cast<uint8_t>(ai);
            variant_section[contests] = section;
            ++contests;
        }
        ee_w32(eve + 0x70, current + added);
    }
    ee_w32(kContestCount, contests);
    produced_total = contests;
}

uint32_t variant_name(uint32_t code) {
    return code == 8 ? texts.name8 : code == 6 ? texts.name6 : code == 4 ? texts.name4 : texts.name1;
}

uint32_t contest_index(uint32_t contest) {
    if (contest < kContestTable || contest >= kContestTable + kContestCapacity * kContestStride ||
        (contest - kContestTable) % kContestStride) return UINT32_MAX;
    return (contest - kContestTable) / kContestStride;
}
} // namespace

uint32_t battle_events_ai_for_contest(uint32_t contest) {
    const uint32_t index = contest_index(contest);
    if (index == UINT32_MAX || index >= ee_r32(kContestCount) || !variant_ai[index]) return 0;
    if (ee_r32(contest) != index || ee_r32(contest + 8) != kBattle || ee_r32(contest + 0x20) != variant_section[index])
        return 0;
    return variant_ai[index];
}

void battle_events_set_alternate_root(const char *path) { alternate_root_override = path ? path : ""; }

extern "C" void f_001244A8(void) {
    ++load_depth;
    f_001244A8_orig();
    --load_depth;
    // Includes call 124870 -> 1244A8 recursively. Wait until all native Eves
    // exist; the caller subsequently sorts/remaps them through 1248E0 and
    // records their final counts. Native contests are never replaced.
    if (load_depth == 0) append_battles();
}

// Contest display name (0x12C364 is the only caller). Added contests show their
// variant; every native contest keeps its event-type name.
extern "C" void f_001252C0(void) {
    const uint32_t index = arg_u32(0);
    if (index < kContestCapacity &&
        battle_events_ai_for_contest(kContestTable + index * kContestStride)) {
        ret_s32(static_cast<int32_t>(variant_name(variant_ai[index])));
        return;
    }
    f_001252C0_orig();
}

// Event-type display name (0x10AFA0). The Action/multiplayer event lists call it
// with the type loaded from a contest they just fetched through 0x1252F0 /
// 0x1253B0, and that contest pointer is still live in $v0 (0x2C6E38, 0x2C8C48)
// or $s2 (0x2C6598, 0x2C6908). Only when that pointer is an added Battle
// contest of the requested type does the list show the variant's name; every
// other request, including native Battle, gets the native name.
extern "C" void f_0010AFA0(void) {
    const uint32_t type = arg_u32(0);
    if (type == kBattle) {
        for (const int reg : {2, 18}) {
            const uint32_t contest = g_ee.r[reg].uw[0];
            const uint32_t index = contest_index(contest);
            if (index == UINT32_MAX || ee_r32(contest + 8) != type) continue;
            if (const uint32_t ai = battle_events_ai_for_contest(contest)) {
                ret_s32(static_cast<int32_t>(variant_name(ai)));
                return;
            }
            break;
        }
    }
    f_0010AFA0_orig();
}

// External layout table for the contest being started (0x20E404 -> 0x1515B8).
// Retail tracks resolve "*<track>.trk/c_battle.va0" natively. Only a Battle
// contest on a track that has a composed alternate (Welch) takes the
// runtime-owned copy instead; the native path is never asked for the missing
// resource, and every other event is native.
extern "C" void f_001515B8(void) {
    const uint32_t contest = arg_u32(0);
    if (object(contest, 0x28) && ee_r32(contest + 8) == kBattle) {
        const uint32_t eve_index = ee_r32(contest + 4);
        const uint32_t eve = eve_index < ee_r32(kEveCount) ? kEves + eve_index * kEveStride : 0;
        const std::string track = eve ? text(eve + 8) : std::string();
        uint32_t layout = 0;
        if (eve && !native_battle_section(ee_r32(eve + 0x80))) {
            const std::vector<uint8_t> image = read_alternate(track);
            if (!image.empty()) {
                for (size_t i = 0; i < image.size(); ++i) ee_w8(kExternalTable + static_cast<uint32_t>(i), image[i]);
                layout = relocate_valt(kExternalTable, static_cast<uint32_t>(image.size()));
            }
        }
        if (layout) {
            ee_w32(kExternalRoot, layout);
            ee_w32(kExternalSection, ee_r32(contest + 0x20));
            const std::string path = "*" + track + ".trk/c_battle.va0";
            for (size_t i = 0; i <= path.size(); ++i)
                ee_w8(kExternalPath + static_cast<uint32_t>(i), static_cast<uint8_t>(i < path.size() ? path[i] : 0));
            return;
        }
    }
    f_001515B8_orig();
}
