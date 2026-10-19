#include "battle_events.h"

#include "core/config.h"
#include "core/cpu.h"
#include "core/memory.h"

#include <array>
#include <algorithm>
#include <cstring>

extern "C" void f_001244A8_orig(void);
extern "C" void f_00135BB0(void);

namespace {
constexpr uint32_t kEves = 0x003C4D08;
constexpr uint32_t kContests = 0x003C61A8;
constexpr uint32_t kBattle = 0x18000224;
constexpr uint32_t kSectionVtable = 0x003F21B0;
constexpr uint32_t kRootVtable = 0x003F2190;
unsigned load_depth = 0;

bool object(uint32_t address, uint32_t size) {
    return address && !(address & 3) && size <= EE_RAM_SIZE && address <= EE_RAM_SIZE - size;
}

bool string_equals(uint32_t address, const char *expected) {
    const size_t length = std::strlen(expected);
    if (!address || address >= EE_RAM_SIZE || length >= EE_RAM_SIZE - address) return false;
    for (size_t i = 0; i <= length; ++i)
        if (ee_r8(address + static_cast<uint32_t>(i)) != static_cast<uint8_t>(expected[i])) return false;
    return true;
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

// This mirrors the native VAL lookup's parent traversal (135C80) without
// calling any virtual method while discovering candidate resource sections.
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

uint32_t native_battle_section(uint32_t eve_section) {
    if (!object(eve_section, 0x1C) || !valid_section_chain(eve_section)) return 0;
    const uint32_t root = ee_r32(eve_section + 0x18);
    if (!object(root, 0x10) || ee_r32(root) != kRootVtable) return 0;
    // 134A70 relocates every section, including those excluded by mode filters.
    // +4 is ALL sections; +8 is only the top-level section count.
    const uint32_t count = ee_r32(root + 4), sections = ee_r32(root + 0xC);
    if (!count || count > 4096 || !object(sections, count * 0x1C)) return 0;
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

void append_battles() {
    const uint32_t mode = ee_r32(0x003C4C04);
    if (!g_config.sp_battle_mode || (mode != 0x12F && mode != 0x130)) return;
    const uint32_t eves = ee_r32(0x003C4CF4);
    uint32_t contests = ee_r32(0x003C4CF8);
    if (!eves || eves > 40 || contests > 320) return;
    // Capacity is shared with all other Action contests. Allocate remaining
    // slots in display-name order, matching the track menu (not VAL file order).
    std::array<uint32_t,40> order{};
    for(uint32_t i=0;i<eves;++i) order[i]=i;
    std::stable_sort(order.begin(),order.begin()+eves,[](uint32_t a,uint32_t b){
        for(uint32_t j=0;j<32;++j){
            const uint8_t x=ee_r8(kEves+a*0x84+0x28+j),y=ee_r8(kEves+b*0x84+0x28+j);
            if(x!=y) return x<y;
            if(!x) return false;
        }
        return false;
    });
    for (uint32_t n = 0; n < eves && contests < 320; ++n) {
        const uint32_t i=order[n];
        const uint32_t eve = kEves + i * 0x84;
        const uint32_t id = ee_r32(eve), current = ee_r32(eve + 0x70);
        if (!current || current >= 32) continue;
        bool present = false;
        for (uint32_t c = 0; c < contests; ++c) {
            const uint32_t cp = kContests + c * 0x28;
            if (ee_r32(cp + 4) == id && ee_r32(cp + 8) == kBattle) { present = true; break; }
        }
        if (present) continue;
        const uint32_t eve_section = ee_r32(eve + 0x80);
        const uint32_t section = native_battle_section(eve_section);
        if (!section) continue; // No native Battle data for this track: leave it intact.

        // The numeric fields use exactly the native 123B98 defaults/terrain
        // rules. Its Battle section remains owned by the already loaded VAL;
        // no new allocation, mutable shared string or synthetic vtable is needed.
        uint32_t terrain = native_integer(section, 0x003EF2C0, -1); // terrain_mask
        if (terrain == UINT32_MAX) {
            const uint32_t option = native_integer(section, 0x003EF230, 0); // opt_terrain
            if (option > 32) continue;
            terrain = option ? 1u << (option - 1) : 0;
        }
        terrain |= native_integer(eve_section, 0x003EF2C0, 0);
        terrain &= ~native_integer(section, 0x003EF3E0, 0); // terrain_disable_mask
        const uint32_t max_cars = native_integer(section, 0x003EF240, 14);
        if (!max_cars || max_cars > 4) continue;
        const std::array<uint32_t, 10> contest = {
            contests, id, kBattle,
            native_integer(section, 0x003EF220, 0), // laps
            max_cars, terrain,
            native_integer(section, 0x003EF3F8, 1), // rounds
            native_integer(section, 0x003EF250, 1000), // purse
            section, 1
        };
        const uint32_t cp = kContests + contests * 0x28;
        for (uint32_t word = 0; word < contest.size(); ++word) ee_w32(cp + word * 4, contest[word]);
        ee_w32(eve + 0x70, current + 1);
        ee_w32(0x003C4CF8, ++contests);
    }
}
} // namespace

extern "C" void f_001244A8(void) {
    ++load_depth;
    f_001244A8_orig();
    --load_depth;
    // Includes call 124870 -> 1244A8 recursively. Wait until all native Eves
    // exist; the caller subsequently sorts/remaps them through1248E0 and
    // records their final counts. Existing first races are never replaced.
    if (load_depth == 0) append_battles();
}
