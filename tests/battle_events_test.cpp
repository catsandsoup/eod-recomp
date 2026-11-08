// Synthetic native-resource fixtures for additive Battle (4 AI)/(8 AI) contests.
#include "battle_events.h"
#include "battle_layout.h"
#include "core/config.h"
#include "core/cpu.h"
#include "core/memory.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

EEState g_ee{};
uint8_t *g_ram = nullptr;
Config::Config() = default;
Config g_config;

namespace {
using namespace battle_layout;
constexpr uint32_t eves = 0x3C4D08, contests = kContestTable;
constexpr uint32_t battle = 0x18000224;
// Two VAL roots: Anoka (Eve + native Battle descriptor) and Welch (Eve only).
constexpr uint32_t anoka_root = 0x10000, anoka_sections = 0x10010;
constexpr uint32_t anoka_eve = anoka_sections, anoka_battle = anoka_sections + 0x1C;
constexpr uint32_t welch_root = 0x11000, welch_sections = 0x11010, welch_eve = welch_sections;
std::vector<uint8_t> ram(EE_RAM_SIZE);
int original_calls, integer_calls, name_calls, external_calls;
bool recursive, inside_original;
uint32_t cursor;
std::filesystem::path alternates;

void require(bool okay, const char *message) {
    if (!okay) { std::fprintf(stderr, "Battle events: %s\n", message); std::exit(1); }
}
uint32_t text(const char *s) {
    const uint32_t at = cursor;
    std::memcpy(g_ram + at, s, std::strlen(s) + 1);
    cursor += static_cast<uint32_t>((std::strlen(s) + 4) & ~3u);
    return at;
}
std::string read(uint32_t a) { return reinterpret_cast<const char *>(g_ram + a); }
void section(uint32_t s, uint32_t root, const char *name, std::vector<std::pair<const char *, const char *>> pairs) {
    const uint32_t table = cursor;
    cursor += static_cast<uint32_t>(pairs.size() * 8);
    for (size_t i = 0; i < pairs.size(); ++i) {
        const uint32_t k = text(pairs[i].first), v = text(pairs[i].second);
        ee_w32(table + static_cast<uint32_t>(i) * 8, k);
        ee_w32(table + static_cast<uint32_t>(i) * 8 + 4, v);
    }
    ee_w32(s, 0x3F21B0); ee_w32(s + 4, text(name)); ee_w32(s + 0xC, static_cast<uint32_t>(pairs.size()));
    ee_w32(s + 0x10, table); ee_w32(s + 0x18, root);
}
void root(uint32_t r, uint32_t sections, uint32_t count, uint32_t top) {
    ee_w32(r, 0x3F2190); ee_w32(r + 4, count); ee_w32(r + 8, top); ee_w32(r + 0xC, sections);
}
void eve(uint32_t index, const char *track, uint32_t section, uint32_t count) {
    const uint32_t e = eves + index * 0x84;
    ee_w32(e, index); std::memcpy(g_ram + e + 8, track, std::strlen(track) + 1);
    std::memcpy(g_ram + e + 0x28, track, std::strlen(track) + 1);
    ee_w32(e + 0x70, count); ee_w32(e + 0x80, section);
}
void contest(uint32_t index, uint32_t eve_index, uint32_t type) {
    const uint32_t c = contests + index * 0x28;
    ee_w32(c, index); ee_w32(c + 4, eve_index); ee_w32(c + 8, type); ee_w32(c + 0x24, 1);
}
// Action fixture: Anoka with two native races, Welch with one; root list as
// recorded by native 0x124870.
void reset(uint32_t mode = 0x12F) {
    std::fill(ram.begin(), ram.end(), 0); g_ram = ram.data(); g_ee = {};
    original_calls = integer_calls = name_calls = external_calls = 0;
    recursive = inside_original = false; cursor = 0x12000;
    g_config.sp_battle_mode = true;
    root(anoka_root, anoka_sections, 2, 1);
    section(anoka_eve, anoka_root, "eve", {{"track", "Anoka"}, {"name", "Anoka"}});
    section(anoka_battle, anoka_root, "$0061$contest", {{"event_type", "battle"}, {"max_cars", "2"}});
    root(welch_root, welch_sections, 1, 1);
    section(welch_eve, welch_root, "eve", {{"track", "Welch"}, {"name", "Welch"}, {"date", "8/4"}, {"time", "12:00"}});
    ee_w32(0x3C4C70, anoka_root); ee_w32(0x3C4C74, welch_root); ee_w32(0x3C4CF0, 2);
    ee_w32(0x3C4C04, mode);
    if (mode == 0x131) {
        ee_w32(0x3C4CF4, 1); ee_w32(0x3C4CF8, 2);
        eve(0, "Anoka", anoka_eve, 2); contest(0, 0, 0x2110); contest(1, 0, battle);
    } else {
        ee_w32(0x3C4CF4, 2); ee_w32(0x3C4CF8, 3);
        eve(0, "Anoka", anoka_eve, 2); eve(1, "Welch", welch_eve, 1);
        contest(0, 0, 0x2110); contest(1, 0, 0x8125); contest(2, 1, 0x2110);
    }
    battle_events_set_alternate_root(alternates.u8string().c_str());
}
uint32_t name_of(uint32_t index) { g_ee.r[4].ud[0] = index; f_001252C0(); return g_ee.r[2].uw[0]; }

void actionAppendsBothVariantsEverywhere() {
    reset();
    std::array<uint8_t, 3 * 0x28> native;
    std::memcpy(native.data(), g_ram + contests, native.size());
    f_001244A8();
    require(original_calls == 1 && ee_r32(0x3C4CF8) == 7, "two Battle contests per track Eve follow the native ones");
    require(std::memcmp(native.data(), g_ram + contests, native.size()) == 0, "native contests are unchanged");
    require(ee_r32(eves + 0x70) == 4 && ee_r32(eves + 0x84 + 0x70) == 3, "each Eve counts its two new contests");
    for (uint32_t i = 3; i < 7; ++i) {
        const uint32_t c = contests + i * 0x28;
        require(ee_r32(c) == i && ee_r32(c + 8) == battle && ee_r32(c + 0x24) == 1, "added contest is an enabled Battle");
        require(battle_events_ai_for_contest(c) == ((i & 1) ? 4u : 8u), "variants alternate 4 then 8 AI");
        require(read(name_of(i)) == ((i & 1) ? "Battle (4 AI)" : "Battle (8 AI)"), "menu shows both variant names");
    }
    require(ee_r32(contests + 3 * 0x28 + 4) == 0 && ee_r32(contests + 3 * 0x28 + 0x20) == anoka_battle,
            "Anoka reuses its native Battle descriptor");
    require(ee_r32(contests + 3 * 0x28 + 0x10) == 6 && ee_r32(contests + 4 * 0x28 + 0x10) == 10, "max_cars covers humans plus AI");
    const uint32_t welch_battle = ee_r32(contests + 5 * 0x28 + 0x20);
    require(welch_battle == kSections && ee_r32(contests + 5 * 0x28 + 4) == 1, "Welch gets the runtime-owned descriptor");
    require(read(ee_r32(ee_r32(welch_battle + 0x10))) == "event_type" && read(ee_r32(ee_r32(welch_battle + 0x10) + 4)) == "battle",
            "Welch descriptor is a native-shaped Battle section");
    for (uint32_t i = 0; i < 3; ++i) {
        require(!battle_events_ai_for_contest(contests + i * 0x28), "native contests are not variants");
        const int before = name_calls; name_of(i);
        require(name_calls == before + 1, "native contest names come from the native function");
    }
    // Frontend lists: type in $a0, the fetched contest still in $v0 or $s2.
    for (const int reg : {2, 18}) {
        g_ee = {}; g_ee.r[4].ud[0] = battle; g_ee.r[reg].ud[0] = contests + 4 * 0x28; f_0010AFA0();
        require(read(g_ee.r[2].uw[0]) == "Battle (8 AI)", "event list names the variant from the live contest pointer");
    }
    ee_w32(contests + 0x28 + 8, battle); // a native Battle contest
    g_ee = {}; g_ee.r[4].ud[0] = battle; g_ee.r[2].ud[0] = contests + 0x28; g_ee.r[18].ud[0] = contests + 3 * 0x28;
    int before = name_calls; f_0010AFA0();
    require(name_calls == before + 1, "a native Battle contest in $v0 keeps the native name even if $s2 is stale");
    ee_w32(contests + 0x28 + 8, 0x8125);
    g_ee = {}; g_ee.r[4].ud[0] = 0x2110; g_ee.r[2].ud[0] = contests + 3 * 0x28;
    before = name_calls; f_0010AFA0();
    require(name_calls == before + 1, "other event types are always native");
    f_001244A8();
    require(ee_r32(0x3C4CF8) == 7 && battle_events_ai_for_contest(contests + 3 * 0x28) == 4,
            "a repeated callback neither duplicates nor forgets the variants");
}

void multiplayerAddsWelch() {
    reset(0x131);
    f_001244A8();
    require(ee_r32(0x3C4CF4) == 2, "multiplayer gains a Welch Eve");
    const uint32_t welch = eves + 0x84;
    require(read(welch + 8) == "Welch" && read(welch + 0x28) == "Welch" && ee_r32(welch) == 1 &&
            ee_r32(welch + 0x68) == 8 && ee_r32(welch + 0x6C) == 4 && ee_r32(welch + 0x80) == welch_eve,
            "Welch Eve comes from its Action Eve section");
    require(ee_r32(welch + 0x70) == 3 && ee_r32(0x3C4CF8) == 8, "Anoka and Welch each gain 1v1, 4 AI and 8 AI");
    const char *expected[3] = {"Battle (1v1)", "Battle (4 AI)", "Battle (8 AI)"};
    for (uint32_t i = 2; i < 8; ++i) {
        require(battle_events_ai_for_contest(contests + i * 0x28) == std::array<uint32_t, 3>{1, 4, 8}[(i - 2) % 3],
                "multiplayer variants are 1v1, 4 AI, 8 AI");
        require(read(name_of(i)) == expected[(i - 2) % 3], "each multiplayer variant has a distinct sized name");
    }
    require(ee_r32(contests + 2 * 0x28 + 0x10) == 2, "1v1 holds two cars");
    require(ee_r32(contests + 1 * 0x28 + 8) == battle && !battle_events_ai_for_contest(contests + 0x28),
            "native multiplayer Battle stays native");
    battle_events_set_alternate_root("/nonexistent");
    reset(0x131); battle_events_set_alternate_root("/nonexistent");
    f_001244A8();
    require(ee_r32(0x3C4CF4) == 1 && ee_r32(0x3C4CF8) == 5, "without the Welch alternate nothing is invented");
}

void gates() {
    reset(); g_config.sp_battle_mode = false; f_001244A8();
    require(ee_r32(0x3C4CF8) == 3 && integer_calls == 0, "disabled setting stays native");
    for (uint32_t mode : {0x130u, 0x132u, 0x133u}) {
        reset(mode); f_001244A8();
        require(ee_r32(0x3C4CF8) == 3 && integer_calls == 0,
                "demo and challenge catalogs, and Career Eves outside the native schedule, are never changed");
    }
    reset(0x130); g_config.sp_battle_mode = false; std::memcpy(g_ram + eves + 0x28, "Dennison Derby", 15); f_001244A8();
    require(ee_r32(0x3C4CF8) == 3, "the setting also switches Career Battles off");
    reset(); ee_w32(0x3C4CF8, kContestCapacity - 1); f_001244A8();
    require(ee_r32(0x3C4CF8) == kContestCapacity - 1, "table capacity is respected");
    reset(); ee_w32(eves + 0x70, 31); f_001244A8();
    require(ee_r32(eves + 0x70) == 31, "per-Eve capacity of 32 is respected");
    reset(); section(welch_eve, welch_root, "eve", {{"track", "Welch"}, {"hidden", "yes"}}); f_001244A8();
    require(ee_r32(eves + 0x84 + 0x70) == 1, "hidden test Eves are skipped");
    reset(); battle_events_set_alternate_root("/nonexistent"); f_001244A8();
    require(ee_r32(eves + 0x84 + 0x70) == 1 && ee_r32(0x3C4CF8) == 5, "a track without Battle data is unchanged");
    reset(); ee_w32(anoka_root + 4, 0xFFFFFFFF); f_001244A8();
    require(ee_r32(eves + 0x70) == 2, "invalid section bounds are rejected");
    reset(); ee_w32(anoka_battle + 0x14, anoka_battle); f_001244A8();
    require(ee_r32(eves + 0x70) == 2, "cyclic inheritance never enters native lookup");
    reset(); recursive = true; f_001244A8();
    require(original_calls == 2 && ee_r32(0x3C4CF8) == 7, "only the outer include callback appends");
    // A later native load reuses indices: stale variants must not survive it.
    reset(); f_001244A8(); ee_w32(0x3C4CF8, 3); contest(3, 0, battle); ee_w32(contests + 3 * 0x28 + 0x20, anoka_battle);
    ee_w32(0x3C4C04, 0x130); f_001244A8();
    require(!battle_events_ai_for_contest(contests + 3 * 0x28), "a new native load clears earlier variant records");
}

const char *kEarly[] = {"Ashland Smackfest", "Woodly Crash Test", "Eagan County Derby", "Crunch Time at Hampton",
                        "Hastings Hell on Wheels", "Dennison Derby"};
const char *kLate[] = {"Eagan Tri County Derby", "Hampton Battles", "Dennison Daredevils", "Kenyon Hard Knocks",
                       "Dundas Rust and Bust", "Mankato Madness", "Anoka T-Bone Royale", "Gilroy Smash and Pass", "Baxter Big One"};

void careerAddsOneScaledBattle() {
    unsigned early_none = 0, early_sum = 0, late_sum = 0, late_none = 0;
    for (const char *n : kEarly) { const uint32_t a = battle_events_career_ai(n); require(a == 0 || a == 4 || a == 6, "early Career is 4 AI (6 on a harder draw)"); early_none += !a; early_sum += a; }
    for (const char *n : kLate) { const uint32_t a = battle_events_career_ai(n); require(a == 0 || a == 8, "late Career is always 8 AI"); late_none += !a; late_sum += a; }
    require(early_none < 6 && late_none < 9, "most Eves get a Battle");
    require(early_sum * 9 < late_sum * 6 * 2 && early_sum / (6 - early_none) < 8, "AI count rises with progression");
    require(battle_events_career_ai("Homebase") == 0 && battle_events_career_ai("Destruction Championship") == 0 &&
            battle_events_career_ai("Welch") == 0 && !battle_events_career_ai(nullptr), "non-schedule Eves stay native");
    require(battle_events_career_ai("Baxter Big One") == battle_events_career_ai("Baxter Big One"), "deterministic");
    const char *picked = nullptr;
    for (const char *n : kEarly) if (battle_events_career_ai(n)) { picked = n; break; }
    require(picked != nullptr, "an early Eve gets a Battle");
    const uint32_t ai = battle_events_career_ai(picked);
    reset(0x130);
    std::memcpy(g_ram + eves + 0x28, picked, std::strlen(picked) + 1);
    ee_w32(contests + 0x1C, 85);
    std::array<uint8_t, 3 * 0x28> native;
    std::memcpy(native.data(), g_ram + contests, native.size());
    f_001244A8();
    require(ee_r32(0x3C4CF8) == 4 && ee_r32(eves + 0x70) == 3 && ee_r32(eves + 0x84 + 0x70) == 1,
            "one Battle joins the scheduled Career Eve; the other Eve stays native");
    require(std::memcmp(native.data(), g_ram + contests, native.size()) == 0, "native Career contests are unchanged");
    const uint32_t c = contests + 3 * 0x28;
    require(ee_r32(c) == 3 && ee_r32(c + 4) == 0 && ee_r32(c + 8) == battle && ee_r32(c + 0x20) == anoka_battle &&
            ee_r32(c + 0x10) == ai + 2 && battle_events_ai_for_contest(c) == ai, "Career Battle uses the native Battle descriptor");
    char expected[32]; std::snprintf(expected, sizeof expected, "Battle (%u AI)", ai);
    require(read(name_of(3)) == expected, "Career menu names the Battle by AI count");
    require(ee_r32(c + 0x1C) == ee_r32(contests + 0x1C), "Career Battle pays its Eve's native purse");
    f_001244A8();
    require(ee_r32(0x3C4CF8) == 4, "a repeated callback does not add a second Battle");
}

void welchExternalTable() {
    reset(); f_001244A8();
    const uint32_t c = contests + 5 * 0x28;
    g_ee.r[4].ud[0] = c; g_ee.r[16].ud[0] = 99; f_001515B8();
    require(external_calls == 0, "missing native Welch table is never requested");
    const uint32_t layout = ee_r32(0x3CD5C8);
    require(layout == kExternalTable && ee_r32(layout) == 0x3F2190 && ee_r32(layout + 0xC) == layout + 0x10,
            "alternate VAL root is relocated like native 0x134A70");
    const uint32_t s0 = layout + 0x10, s1 = s0 + 0x1C;
    require(ee_r32(s0) == 0x3F21B0 && read(ee_r32(s0 + 4)) == "respawn" && ee_r32(s0 + 0x18) == layout,
            "sections get native vtable, names and root");
    require(read(ee_r32(ee_r32(s1 + 0x10))) == "pt" && read(ee_r32(ee_r32(s1 + 0x10) + 4)) == "1, 2" &&
            ee_r32(s1 + 0x14) == s0, "pairs are absolute and parent names resolve");
    require(ee_r32(0x3CD5CC) == ee_r32(c + 0x20) && read(0x3CD5D0) == "*Welch.trk/c_battle.va0" && g_ee.r[16].ud[0] == 99,
            "native external-table globals are set");
    g_ee.r[4].ud[0] = contests + 3 * 0x28; f_001515B8();
    require(external_calls == 1, "a track with native Battle data uses the native table");
    g_ee.r[4].ud[0] = contests + 2 * 0x28; f_001515B8();
    require(external_calls == 2, "non-Battle events are native");
}

// Minimal VALT: two sections, "respawn" and "$0000$a" : respawn with pt.
void writeAlternate() {
    alternates = std::filesystem::temp_directory_path() / "battle_events_test_alternates";
    std::filesystem::create_directories(alternates / "TRACKS" / "WELCH");
    std::vector<uint8_t> image(0x100, 0);
    auto w32 = [&](size_t o, uint32_t v) { std::memcpy(image.data() + o, &v, 4); };
    auto str = [&](size_t o, const char *s) { std::memcpy(image.data() + o, s, std::strlen(s) + 1); };
    w32(0, 0x40C218); w32(4, 2); w32(8, 1);
    // section 0 at 0x10: name 0x80 "respawn", no pairs (table 0x60)
    w32(0x10, 0x40C0D8); w32(0x14, 0x80); w32(0x18, 0); w32(0x1C, 0); w32(0x20, 0x60);
    // section 1 at 0x2C: name 0x88 "$0000$a", parent 0x80, one pair at 0x60
    w32(0x2C, 0x40C0D8); w32(0x30, 0x88); w32(0x34, 0x80); w32(0x38, 1); w32(0x3C, 0x60);
    w32(0x60, 0x98); w32(0x64, 0x9C);
    str(0x80, "respawn"); str(0x88, "$0000$a"); str(0x98, "pt"); str(0x9C, "1, 2");
    std::ofstream(alternates / "TRACKS" / "WELCH" / "c_battle.va0", std::ios::binary)
        .write(reinterpret_cast<const char *>(image.data()), static_cast<std::streamsize>(image.size()));
}
} // namespace

extern "C" uint32_t ee_read32_slow(uint32_t) { std::abort(); }
extern "C" void ee_write32_slow(uint32_t, uint32_t) { std::abort(); }
extern "C" uint8_t ee_read8_slow(uint32_t) { std::abort(); }
extern "C" void ee_write8_slow(uint32_t, uint8_t) { std::abort(); }
extern "C" uint16_t ee_read16_slow(uint32_t) { std::abort(); }
extern "C" void ee_write16_slow(uint32_t, uint16_t) { std::abort(); }
extern "C" void f_001244A8_orig() {
    ++original_calls;
    if (recursive && !inside_original) {
        inside_original = true; f_001244A8(); inside_original = false;
        require(ee_r32(0x3C4CF8) == 3 && integer_calls == 0, "include return must not append before remaining native Eves");
    }
}
extern "C" void f_001252C0_orig() { ++name_calls; g_ee.r[2].ud[0] = 0x3EC300; }
extern "C" void f_0010AFA0_orig() { ++name_calls; g_ee.r[2].ud[0] = 0x3EC300; }
extern "C" void f_001515B8_orig() { ++external_calls; }
extern "C" void f_00135BB0() {
    ++integer_calls;
    const uint32_t s = arg_u32(0), fallback = arg_u32(2);
    require(s == anoka_eve || s == anoka_battle || s == welch_eve || s == kSections, "integer query uses an owned section");
    std::memset(&g_ee, 0xA5, sizeof(g_ee)); g_ee.r[2].ud[0] = fallback;
}

int main() {
    writeAlternate();
    actionAppendsBothVariantsEverywhere(); multiplayerAddsWelch(); gates(); careerAddsOneScaledBattle(); welchExternalTable();
    std::puts("Battle event exposure checks passed");
}
