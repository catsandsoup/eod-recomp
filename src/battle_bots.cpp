// Native Battle participant and weapon construction bridges. These hooks run
// during session creation; they do not advance physics, weapons or the clock.
#include "battle_bots.h"
#include "battle_driver.h"

#include "core/config.h"
#include "core/cpu.h"
#include "core/memory.h"

#include <algorithm>

extern "C" {
void f_0012C158_orig(void);
void f_0020FD48_orig(void);
void f_0017DE78(void);
void f_00227EE8(void);
void f_002CE180(void);
}

namespace {
constexpr uint32_t kBattle = 0x18000224;
constexpr uint32_t kEvent = 0x003CF19C;
constexpr uint32_t kHumanCount = 0x00439858;
constexpr uint32_t kWeaponCount = 0x003CE4B8;
constexpr uint32_t kCarScenes = 0x003D1C90;
constexpr uint32_t kWeaponScenes = 0x003D20A8;
constexpr uint32_t kWorldCount = 0x00495CAC;
constexpr uint32_t kWorldVisibleCount = 0x00495CB0;
constexpr uint32_t kWorldObjects = 0x00495CB4;
constexpr uint32_t kWorldVisibleObjects = 0x00495CB8;
uint32_t ai_slots = 0;

bool ram_object(uint32_t address, uint32_t size) {
    return address && !(address & 3) && size <= EE_RAM_SIZE && address <= EE_RAM_SIZE - size;
}

bool world_has_weapon_capacity() {
    // 20E360 allocates each object pointer array with 0x12C0 bytes (1200
    // entries); 2110D8 appends without checking capacity. 17DE30 likewise
    // appends to sixteen 12-byte weapon records before its count at +0xC0.
    return ee_r32(kWeaponCount) < 16 &&
           ee_r32(kWorldCount) < 1200 && ee_r32(kWorldVisibleCount) < 1200 &&
           ram_object(ee_r32(kWorldObjects), 0x12C0) &&
           ram_object(ee_r32(kWorldVisibleObjects), 0x12C0);
}
} // namespace

bool battle_bots_is_ai_slot(uint32_t slot) {
    // Ownership is established at world construction. The settings UI promises
    // changes on the next event; disabling live must not strand latched inputs.
    return slot < 4 && ee_r32(kEvent) == kBattle &&
           (ai_slots & (1u << slot));
}

extern "C" void f_0012C158(void) {
    const uint32_t contest = arg_u32(0);
    const uint32_t humans = ee_r32(kHumanCount);
    const bool fill = g_config.battle_bots && humans >= 1 && humans <= 4 &&
                      ram_object(contest, 0x14) && ee_r32(contest + 8) == kBattle;
    f_0012C158_orig();
    // This is before 12AEF0 fills the DAVMap and 111E10/1116E0 resolve
    // driver/vehicle IDs and Human/AI types. Do not append blank car records.
    if (fill) ret_s32(4);
}

extern "C" void f_0020FD48(void) {
    const uint32_t description = arg_u32(0);
    ai_slots = 0;
    battle_driver_reset();
    f_0020FD48_orig();
    if (!g_config.battle_bots || !ram_object(description, 0x9A8) ||
        ee_r32(description + 0x9A4) != kBattle) return;
    const uint32_t count = ee_r32(description + 0x98C);
    if (!count || count > 4) return; // Native BattleMaster has four records.

    // Keep the original function's complete return context. Constructor calls
    // use its restored stack and preserve their native memory side effects.
    const EEState saved = g_ee;
    f_002CE180(); // same native suppression gate used at 210010
    const bool suppressed = g_ee.r[2].uw[0] != 0;
    g_ee = saved;
    // 227EE8 creates the scene now, but 213578/1758B8 queues physical creation.
    // Registry count may not grow until after this bridge returns, so each
    // queued constructor must reserve one of its sixteen entries here.
    uint32_t reserved_weapon_count = ee_r32(kWeaponCount);
    for (uint32_t slot = 0; slot < count; ++slot) {
        const uint32_t record = description + 0x2C + slot * 0x78;
        if (ee_r32(record) != 0x51) continue;
        ai_slots |= 1u << slot;
        if (suppressed || reserved_weapon_count >= 16 ||
            !ram_object(ee_r32(kCarScenes + 4 * slot), 0x654) ||
            ee_r32(kWeaponScenes + 4 * slot) || !world_has_weapon_capacity()) continue;

        g_ee.r[4].ud[0] = slot;
        g_ee.r[5].ud[0] = 0;
        f_0017DE78();
        const bool has_weapon = g_ee.r[2].uw[0] != 0;
        g_ee = saved;
        if (has_weapon) continue;

        // 210028 calls this for Human records only. Its native factory derives
        // launch offsets from the car model, creates the scene, and queues
        // physical weapon initialization with the proper owner ID.
        g_ee.r[4].ud[0] = slot;
        g_ee.r[5].ud[0] = 0x51;
        g_ee.r[6].ud[0] = record + 0x44;
        f_00227EE8();
        g_ee = saved;
        reserved_weapon_count = std::max(reserved_weapon_count + 1, ee_r32(kWeaponCount));
    }
}
