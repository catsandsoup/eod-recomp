// Native Battle participant and weapon construction bridges. These hooks run
// during session creation; they do not advance physics, weapons or the clock.
#include "battle_bots.h"
#include "vfs/custom_map.h"
#include "netplay_hooks.h"
#include "mutator_hooks.h"
#include "mutator_presets.h"
#include "battle_driver.h"
#include "battle_events.h"
#include "battle_layout.h"
#include "session_start.h"
#include "visual_car_list.h"

#include "core/config.h"
#include "core/cpu.h"
#include "core/memory.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <vector>
#include <cmath>
#include <cstring>

extern "C" {
void f_0012C158_orig(void);
void f_0020FD48_orig(void);
void f_00167C70_orig(void);
void f_00167A88_orig(void);
void f_001673B8_orig(void);
void f_0012AEF0_orig(void);
void f_001116E0_orig(void);
void f_00119428_orig(void);
void f_00166B50_orig(void);
void f_001D8F58(void);
void f_0017DE78(void);
void f_00227EE8(void);
void f_002CE180(void);
}

namespace {
constexpr uint32_t kBattle = 0x18000224;
constexpr uint32_t kEvent = 0x003CF19C;
constexpr uint32_t kHumanCount = 0x00439858;
constexpr uint32_t kWeaponCount = 0x003CE4B8;
constexpr uint32_t kWeaponScenes = 0x003D20A8;
constexpr uint32_t kWorldCount = 0x00495CAC;
constexpr uint32_t kWorldVisibleCount = 0x00495CB0;
constexpr uint32_t kWorldObjects = 0x00495CB4;
constexpr uint32_t kWorldVisibleObjects = 0x00495CB8;
uint32_t ai_slots = 0;
uint32_t battle_description = 0; // session description of the Battle world built last
bool drawing_message = false;     // inside the native kill-message draw (0x166B50)

bool ram_object(uint32_t address, uint32_t size) {
    return address && !(address & 3) && size <= EE_RAM_SIZE && address <= EE_RAM_SIZE - size;
}

// Native special-chassis event table (0x003CBEE8, 4 entries x 0x14 bytes, read by f_0012C580/f_0012C398).
bool is_special_chassis_event(uint32_t event_type) {
    if (!event_type) return false;
    constexpr uint32_t kSpecialTable = 0x003CBEE8;
    for (uint32_t i = 0; i < 4; ++i) {
        if (ee_r32(kSpecialTable + i * 0x14) == event_type) return true;
    }
    return false;
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
    return slot < battle_layout::kCapacity && ee_r32(kEvent) == kBattle &&
           (ai_slots & (1u << slot)) &&
           !(netplay_hooks::chicken_player_slot && netplay_hooks::chicken_player_slot(slot));
}

extern "C" void f_0012C158(void) {
    const uint32_t contest = arg_u32(0);
    const uint32_t contest_id = ram_object(contest, 4) ? ee_r32(contest) : 0xFFFFFFFFu;
    const uint32_t target = session_single_event_contest();
    if (target != 0xFFFFFFFFu && contest_id != target) {
        ret_s32(0);
        return;
    }
    const uint32_t humans = ee_r32(kHumanCount);
    const uint32_t event_type = ram_object(contest, 0x14) ? ee_r32(contest + 8) : 0;
    const bool battle = humans >= 1 && humans <= 4 && event_type == kBattle;
    const bool trailer_race = (event_type & 0x800u) != 0;
    const uint32_t max_drivers = trailer_race ? (battle_layout::kMaxCars / 2) : battle_layout::kMaxCars;
    // "Battle (4 AI)" / "Battle (8 AI)": the humans plus that many AI, within the
    // ten cars native 0x12C158 already allows two humans. Their participant
    // records fit the enlarged BattleMaster (battle_layout.h).
    const uint32_t ai = battle ? battle_events_ai_for_contest(contest) : 0;
    const bool fill = !ai && battle && g_config.battle_bots;
    const int override_ai = session_ai_override();
    f_0012C158_orig();
    const uint32_t orig_count = g_ee.r[2].uw[0];
    // Explicit EOD_DIRECT_START ai=<0..15> override: works on Battle AND any Race/Derby/Gauntlet
    // template so "Force Free Roam" (ai=0) spawns only the player car on that exact map variation,
    // and ai=1..15 spawns the requested AI count.
    if (override_ai >= 0 && humans >= 1 && humans <= 4) {
        const uint32_t remote = netplay_hooks::remote_humans ? netplay_hooks::remote_humans() : 0;
        const uint32_t total = std::min(humans + remote + static_cast<uint32_t>(override_ai), max_drivers);
        ret_s32(static_cast<int32_t>(total));
        return;
    }
    const bool online = netplay_hooks::event_active && netplay_hooks::event_active();
    if (!online && g_config.offline_ai_bots > 0 && humans >= 1 && humans <= 4) {
        uint32_t total = std::min(humans + static_cast<uint32_t>(g_config.offline_ai_bots), max_drivers);
        if (target == 0xFFFFFFFFu && (trailer_race || is_special_chassis_event(event_type))) {
            total = std::min(total, orig_count);
        }
        ret_s32(static_cast<int32_t>(total));
        return;
    }
    // This is before 12AEF0 fills the DAVMap and 111E10/1116E0 resolve
    // driver/vehicle IDs and Human/AI types. Do not append blank car records.
    // Variant 1 is "Battle (1v1)": the humans only, never AI.
    if (ai) {
        // online: remote players are extra cars in the same world (puppets driven by their owners)
        const uint32_t remote = netplay_hooks::remote_humans ? netplay_hooks::remote_humans() : 0;
        ret_s32(static_cast<int32_t>(std::min(humans + remote + (ai == 1 ? 0 : ai), max_drivers)));
    }
    else if (fill) ret_s32(4);
}

extern "C" void f_0020FD48(void) {
    const uint32_t description = arg_u32(0);
    ai_slots = 0;
    battle_description = description;
    battle_driver_reset();
    f_0020FD48_orig();
    if (!ram_object(description, 0x9A8)) return;
    const bool battle = ee_r32(description + 0x9A4) == kBattle;
    const uint32_t count = ee_r32(description + 0x98C);
    const bool online = netplay_hooks::event_active && netplay_hooks::event_active();
    // Run offline mutator construction BEFORE want_weapons evaluation so custom JSON
    // and launch-scoped PendingOfflineMatch modes set mutator_hooks::suppress_weapons in time.
    if (!online && mutator_hooks::on_offline_world_construct) {
        mutator_hooks::on_offline_world_construct(description);
    }
    const bool offline_suppress_weapons = !online && battle && !mutator_hooks::active && g_config.offline_gametype > 0 &&
        !mutator::preset_uses_weapons(static_cast<uint8_t>(
            std::clamp(g_config.offline_gametype, 0, static_cast<int>(mutator::preset_count()) - 1)));
    const bool want_weapons = !mutator_hooks::suppress_weapons && !offline_suppress_weapons;
    const bool online_weapons = online && want_weapons && (battle || netplay_hooks::rule(netplay_hooks::Chickens, false));
    const bool battle_ai = battle && count <= battle_layout::kCapacity &&
        (g_config.battle_bots || (!online && g_config.offline_ai_bots > 0) || count > 4 || session_ai_override() > 0 || online);
    // Trailer rows have no launchers. The native scene table holds twenty slots.
    if (count && count <= kLegacyVisualCarSlots && (battle_ai || online_weapons)) {
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
        uint32_t scenes = 0;
        for (uint32_t slot = 0; slot < kLegacyVisualCarSlots; ++slot)
            if (ee_r32(kWeaponScenes + 4 * slot)) ++scenes;
        reserved_weapon_count = std::max(reserved_weapon_count, scenes);
        for (uint32_t slot = 0; slot < count; ++slot) {
            const uint32_t record = description + 0x2C + slot * 0x78;
            if (ee_r32(record) != 0x51) continue;
            if (battle_ai) ai_slots |= 1u << slot;
            if (!want_weapons) continue;
            // A remote human uses an AI-type car on this machine; the original
            // constructor skips it even when Chicken Fight is enabled.
            if (!battle_ai && !(netplay_hooks::chicken_player_slot && netplay_hooks::chicken_player_slot(slot))) continue;
            if (suppressed || reserved_weapon_count >= 16 ||
                !ram_object(eod_visual_car_slot_read(kVisualCarSlots + 4 * slot), 0x654) ||
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
}

// Kill messages ("You killed %s.", "You were killed by %s.", ...). The native
// draw (0x166B50) names the other car with 0x119428(slot), which indexes the
// four 18-byte profile names at 0x437F9C by participant slot. That is right
// for humans in a native Battle, but an AI slot would show a profile name
// (slots 0-3) or whatever follows the table (slots 4 and up: "Basher", "A").
// While a Battle that has AI is drawing a message, a slot is named from its
// session description record (+4: the driver name 0x1116E0 resolved, the same
// native driver names the races show). Everything else keeps the native lookup.
extern "C" void f_00166B50(void) {
    drawing_message = true;
    f_00166B50_orig();
    drawing_message = false;
}

extern "C" void f_00119428(void) {
    const uint32_t slot = arg_u32(0);
    if (drawing_message && ai_slots && ee_r32(kEvent) == kBattle && slot < battle_layout::kCapacity &&
        ram_object(battle_description, 0x9A8) && ee_r32(battle_description + 0x9A4) == kBattle &&
        slot < ee_r32(battle_description + 0x98C)) {
        const uint32_t name = battle_description + 0x2C + slot * 0x78 + 4;
        if (ee_r8(name)) {
            static const bool trace = std::getenv("EOD_BATTLE_TRACE") != nullptr;
            if (trace) {
                char text[0x41] = {};
                for (uint32_t i = 0; i < 0x40 && ee_r8(name + i); ++i) text[i] = static_cast<char>(ee_r8(name + i));
                std::fprintf(stderr, "[BATTLE_MESSAGE] slot %u named '%s'\n", slot, text);
            }
            ret_s32(static_cast<int32_t>(name));
            return;
        }
    }
    f_00119428_orig();
}

namespace {
void write_map_pose(uint32_t out, const eod::vfs::MapSpawn &spawn) {
    using Point = eod::vfs::MapPoint;
    const Point &up = spawn.normal;
    const double yaw = spawn.heading * (3.141592653589793 / 180.);
    auto cross = [](const Point &a, const Point &b) -> Point {
        return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
    };
    Point right = cross(up, {float(std::sin(yaw)), 0, float(std::cos(yaw))});
    float length = std::hypot(right[0], right[1], right[2]);
    if (length < .00001f) {
        right = cross(up, {0, 1, 0});
        length = std::hypot(right[0], right[1], right[2]);
    }
    for (float &v : right) v /= length;
    const Point forward = cross(right, up);
    // The native transform adds 0.376m of clearance along the surface normal.
    Point position = spawn.position;
    for (size_t i = 0; i < 3; ++i) position[i] += .376f * up[i];
    for (const Point &row : {right, up, forward, position}) {
        for (float v : row) {
            uint32_t bits;
            std::memcpy(&bits, &v, 4);
            ee_w32(out, bits);
            out += 4;
        }
    }
    for (unsigned i = 0; i < 0x14; i += 4) ee_w32(out + i, 0);
}
}

// These are the three BattleMaster methods that construct and consume the
// fixed-size guest records. Map spawns live in the host-side, immutable mount.
extern "C" void f_00167A88(void) {
    auto map = eod::vfs::active_map();
    if (!map || !map->replace_retail_spawns) { f_00167A88_orig(); return; }
    uint32_t master = arg_u32(0);
    ee_w32(master + battle_layout::kRespawnCount, 0);
    ee_w32(master + battle_layout::kSpawnIndex, 0);
}

extern "C" void f_001673B8(void) {
    auto map = eod::vfs::active_map();
    if (!map || !map->replace_retail_spawns) { f_001673B8_orig(); return; }
    const auto &poses = map->battle_respawns.empty() ? map->grid_starts : map->battle_respawns;
    uint32_t master = arg_u32(0);
    uint32_t sequence = ee_r32(master + battle_layout::kSpawnIndex);
    ee_w32(master + battle_layout::kSpawnIndex, sequence + 1);
    // The caller consumes this guest pointer synchronously.
    uint32_t out = battle_layout::kScratch + 0x100;
    write_map_pose(out, poses[(uint64_t(sequence) + arg_u32(1)) % poses.size()]);
    g_ee.r[2].ud[0] = out;
}

// Initial Battle placement (vtable +0x64): native places car i on respawn
// record (rotation + i) % count. Tracks ship five to eight records, so in a
// larger Battle two cars would share one pose. The first `count` cars keep the
// native pose; each later car takes the same record moved sideways by whole
// car-widths and re-grounded by the native respawn builder 0x1D8F58 that made
// the record from (pt, dir) in the first place.
extern "C" void f_00167C70(void) {
    const uint32_t master = arg_u32(0), out = arg_u32(1), index = arg_u32(2);
    auto map = eod::vfs::active_map();
    if (map && map->replace_retail_spawns && ram_object(out, 0x48)) {
        const auto &poses = map->grid_starts.empty() ? map->battle_respawns : map->grid_starts;
        write_map_pose(out, poses[index % poses.size()]);
        ee_w32(out + 0x44, 2);
        g_ee.r[2].ud[0] = 1;
        return;
    }
    f_00167C70_orig();
    if (!ram_object(master, battle_layout::kMasterSize) || ee_r32(master) != battle_layout::kMasterVtable ||
        !ram_object(out, 0x48)) return;
    auto f = [](uint32_t a) { const uint32_t u = ee_r32(a); float v; std::memcpy(&v, &u, 4); return v; };
    auto put = [](uint32_t a, float v) { uint32_t u; std::memcpy(&u, &v, 4); ee_w32(a, u); };
    const uint32_t records = ee_r32(master + battle_layout::kRespawnCount);
    if (!records || records > 16 || index < records || index >= battle_layout::kCapacity) return;
    const uint32_t lap = index / records; // 1, 2, ...
    const float side = (lap & 1 ? 1.0f : -1.0f) * static_cast<float>((lap + 1) / 2) * 6.0f;
    const float rx = f(out), rz = f(out + 8), fx = f(out + 0x18), fz = f(out + 0x20);
    const float px = f(out + 0x24) + rx * side, pz = f(out + 0x2C) + rz * side;
    if (!std::isfinite(px) || !std::isfinite(pz) || !std::isfinite(fx) || !std::isfinite(fz)) return;
    const EEState saved = g_ee;
    put(battle_layout::kScratch, px); put(battle_layout::kScratch + 4, pz);
    put(battle_layout::kScratch + 8, fx); put(battle_layout::kScratch + 12, fz);
    g_ee.r[4].ud[0] = out;
    g_ee.r[5].ud[0] = battle_layout::kScratch;
    g_ee.r[6].ud[0] = battle_layout::kScratch + 8;
    f_001D8F58();
    g_ee = saved;
}

namespace {
// Native vehicle/driver tables (0x12A508 / 0x12A560): 0x54-byte records.
// Vehicle: +4 number, +8 chassis name[32], +0x40 class (0 compact, 1 midsize,
// 2 muscle, 3 special). Driver: +4 number, +8 name.
constexpr uint32_t kVehicleTable = 0x003CBEE4, kVehicleCount = 0x00439704;
constexpr uint32_t kDriverTable = 0x003CBEE0, kDriverCount = 0x00439700;
constexpr uint32_t kRecordStride = 0x54;
constexpr uint32_t kHumanTable = 0x00439708; // humans' chosen vehicle numbers at +4, count 0x439858
constexpr uint32_t kMaxDistinctChassis = 6; // native races load up to eight
uint32_t mix_seed = 0x2545F491;

uint32_t next_random() { mix_seed = mix_seed * 1664525u + 1013904223u; return mix_seed >> 8; }

bool same_text(uint32_t a, uint32_t b, uint32_t size) {
    for (uint32_t i = 0; i < size; ++i) {
        const uint8_t x = ee_r8(a + i), y = ee_r8(b + i);
        if (x != y) return false;
        if (!x) return true;
    }
    return true;
}
} // namespace

// Mixed classes for an added Battle (N AI) contest. Native 0x12AEF0 picks the
// event's drivers from the Eve's DAVMap, whose drivers all match the Eve's
// class, and 0x1116E0 then resolves each description record from its driver
// number (+0x68) and vehicle number (+0x6C): names, chassis, paint and the
// Human/AI type (0x12A220 matches the humans' numbers). Native events keep
// exactly that. For a Battle variant only, each AI record's numbers are set,
// just before 0x1116E0 runs, to another native driver with that driver's own
// vehicle (the same number in both native tables), drawn in turn from every
// class. Humans, record order and count, the DAVMap and every native event are
// untouched; each vehicle keeps its own physics and damage.
void battle_bots_mix_classes(uint32_t description) {
    if (!ram_object(description, 0x9A8)) return;
    const uint32_t count = ee_r32(description + 0x98C);
    const uint32_t vehicles = ee_r32(kVehicleTable), vehicle_count = ee_r32(kVehicleCount);
    const uint32_t drivers = ee_r32(kDriverTable), driver_count = ee_r32(kDriverCount);
    const uint32_t humans = ee_r32(kHumanCount);
    if (!count || count > battle_layout::kCapacity || humans > 4 || !vehicle_count || vehicle_count > 4096 ||
        !driver_count || driver_count > 4096 || !ram_object(vehicles, vehicle_count * kRecordStride) ||
        !ram_object(drivers, driver_count * kRecordStride)) return;
    std::vector<uint32_t> used, human_numbers;
    std::vector<uint32_t> chassis; // vehicle records whose chassis is already in this event
    uint32_t human_class = 1;
    auto vehicle_for = [&](uint32_t number) -> uint32_t {
        for (uint32_t i = 0; i < vehicle_count; ++i)
            if (ee_r32(vehicles + i * kRecordStride + 4) == number) return vehicles + i * kRecordStride;
        return 0;
    };
    auto note_chassis = [&](uint32_t vehicle) {
        for (uint32_t c : chassis) if (same_text(c + 8, vehicle + 8, 32)) return;
        chassis.push_back(vehicle);
    };
    for (uint32_t i = 0; i < humans; ++i) {
        const uint32_t number = ee_r32(kHumanTable + i * kRecordStride + 4);
        human_numbers.push_back(number);
        used.push_back(number);
        if (const uint32_t v = vehicle_for(number)) { human_class = ee_r32(v + 0x40) & 3; note_chassis(v); }
    }
    // Candidate pairs per class: a vehicle with a driver of the same number.
    std::array<std::vector<uint32_t>, 4> pool;
    for (uint32_t i = 0; i < vehicle_count; ++i) {
        const uint32_t v = vehicles + i * kRecordStride, number = ee_r32(v + 4), cls = ee_r32(v + 0x40);
        if (cls > 3 || !ee_r8(v + 8)) continue;
        for (uint32_t j = 0; j < driver_count; ++j) {
            const uint32_t d = drivers + j * kRecordStride;
            if (ee_r32(d + 4) == number && ee_r8(d + 8)) { pool[cls].push_back(v); break; }
        }
    }
    for (auto &candidates : pool)
        for (size_t i = candidates.size(); i > 1; --i) std::swap(candidates[i - 1], candidates[next_random() % i]);
    uint32_t turn = human_class + 1;
    for (uint32_t slot = 0; slot < count; ++slot) {
        const uint32_t record = description + 0x2C + slot * 0x78;
        if (std::find(human_numbers.begin(), human_numbers.end(), ee_r32(record + 0x68)) != human_numbers.end()) continue;
        uint32_t chosen = 0;
        for (uint32_t attempt = 0; attempt < 4 && !chosen; ++attempt) {
            for (const uint32_t v : pool[(turn + attempt) & 3]) {
                if (std::find(used.begin(), used.end(), ee_r32(v + 4)) != used.end()) continue;
                bool known = false;
                for (uint32_t c : chassis) known = known || same_text(c + 8, v + 8, 32);
                if (!known && chassis.size() >= kMaxDistinctChassis) continue;
                chosen = v;
                break;
            }
        }
        ++turn;
        if (!chosen) { used.push_back(ee_r32(record + 0x68)); continue; } // keep the native pick
        used.push_back(ee_r32(chosen + 4));
        note_chassis(chosen);
        ee_w32(record + 0x68, ee_r32(chosen + 4));
        ee_w32(record + 0x6C, ee_r32(chosen + 4));
    }
}

namespace { uint32_t pending_mix = 0; }

extern "C" void f_0012AEF0(void) {
    const uint32_t contest = arg_u32(1);
    f_0012AEF0_orig();
    mix_seed ^= ee_r32(0x003CDACC) ^ (contest * 0x9E3779B9u);
    // The description for this roster is resolved next (0x112D48 -> 0x111E10 ->
    // 0x1116E0); any other roster build cancels a pending mix.
    const uint32_t cp = contest < battle_layout::kContestCapacity
        ? battle_layout::kContestTable + contest * battle_layout::kContestStride : 0;
    const uint32_t variant = cp ? battle_events_ai_for_contest(cp) : 0;
    const bool is_battle_cp = cp && ram_object(cp, 0x14) && ee_r32(cp + 8) == kBattle;
    const bool online = netplay_hooks::event_active && netplay_hooks::event_active();
    pending_mix = (variant > 1 || (is_battle_cp && (session_ai_override() > 0 || (!online && g_config.offline_ai_bots > 0)))) ? 1 : 0;
}

extern "C" void f_001116E0(void) {
    const uint32_t description = arg_u32(0);
    if (pending_mix) {
        pending_mix = 0;
        const EEState saved = g_ee;
        battle_bots_mix_classes(description);
        g_ee = saved;
        static const bool trace = std::getenv("EOD_BATTLE_TRACE") != nullptr;
        if (trace && ram_object(description, 0x9A8)) {
            std::fprintf(stderr, "[BATTLE_ROSTER] mixed:");
            for (uint32_t i = 0; i < ee_r32(description + 0x98C) && i < battle_layout::kCapacity; ++i)
                std::fprintf(stderr, " %u", ee_r32(description + 0x2C + i * 0x78 + 0x68));
            std::fprintf(stderr, "\n");
        }
    }
    if (netplay_hooks::event_active && netplay_hooks::event_active()) netplay_hooks::roster_pre(description);
    f_001116E0_orig();
    if (netplay_hooks::event_active && netplay_hooks::event_active()) netplay_hooks::roster_post(description);
}
