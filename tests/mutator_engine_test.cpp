// Milestone 1 unit tests: Mutator schema JSON/wire codec, Track-Anchored zones,
// string vehicle key resolution & 7-chassis budget, per-body gravity in
// vehicle_dynamics.cpp, per-slot physics/Feat mutators, Fiesta deterministic
// reroll, and RF_SUICIDE / RF_WHIP_AROUND direction_sign wrong-way suppression.
#include "mutator_engine.h"
#include "mutator_hooks.h"
#include "mutator_presets.h"
#include "mutator_schema.h"
#include "mutator_warp.h"
#include "battle_layout.h"
#include "netplay_car.h"
#include "netplay_wire.h"
#include "vehicle_dynamics.h"
#include "core/config.h"
#include "core/cpu.h"
#include "core/memory.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

EEState g_ee{};
uint8_t *g_ram = nullptr;

extern "C" uint8_t ee_read8_slow(uint32_t a) { return g_ram[a]; }
extern "C" void ee_write8_slow(uint32_t a, uint8_t v) { g_ram[a] = v; }
extern "C" uint16_t ee_read16_slow(uint32_t a) { uint16_t v; std::memcpy(&v, g_ram + a, 2); return v; }
extern "C" void ee_write16_slow(uint32_t a, uint16_t v) { std::memcpy(g_ram + a, &v, 2); }
extern "C" uint32_t ee_read32_slow(uint32_t a) { uint32_t v; std::memcpy(&v, g_ram + a, 4); return v; }
extern "C" void ee_write32_slow(uint32_t a, uint32_t v) { std::memcpy(g_ram + a, &v, 4); }
extern "C" uint64_t ee_read64_slow(uint32_t a) { uint64_t v; std::memcpy(&v, g_ram + a, 8); return v; }
extern "C" void ee_write64_slow(uint32_t a, uint64_t v) { std::memcpy(g_ram + a, &v, 8); }

extern "C" void f_001B8370();
static unsigned g_recovery_resets = 0;
static constexpr uint32_t kAutoReset = 0x00485098;
extern "C" void f_001B8370_orig() {
    const uint32_t person = g_ee.r[5].uw[0];
    MEM_SW(person + 192, 0xFFFFFFFFu);
    if (MEM_LW(kAutoReset)) {
        ++g_recovery_resets;
        float t = 15.0f; uint32_t u; std::memcpy(&u, &t, 4);
        MEM_SW(person + 196, u);
    }
}
extern "C" void eod_physics_001B8370() { f_001B8370_orig(); }

namespace {

void require(bool cond, const char *msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        std::exit(1);
    }
}

void wf(uint32_t addr, float v) {
    uint32_t u; std::memcpy(&u, &v, 4); ee_w32(addr, u);
}

float rf(uint32_t addr) {
    uint32_t u = ee_r32(addr); float v; std::memcpy(&v, &u, 4); return v;
}

bool near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

void write_cstr(uint32_t addr, const char *s) {
    for (size_t i = 0;; ++i) {
        ee_w8(addr + static_cast<uint32_t>(i), static_cast<uint8_t>(s[i]));
        if (!s[i]) break;
    }
}

void test_json_and_wire_codec() {
    require(netplay::kProtocol == 15, "kProtocol bumped cleanly to 15");
    require(netplay::K_MUTATOR_CONFIG != netplay::K_MUTATOR_CONFIG_ACK, "distinct mutator packet IDs");

    const std::string json = R"({
      "id": "destructionware_fiesta",
      "name": "DestructionWare: Fiesta Roulette",
      "base_event_type": "0x18000224",
      "default_track": "Anoka",
      "base_rule_flags": ["RF_FIESTA_ROULETTE", "RF_SUICIDE"],
      "base_feats": ["FB_CHICKEN_STRAIGHT", "FB_TURBO"],
      "base_traits": { "gravity_scale": 0.65, "mass_scale": 1.25, "grip_scale": 1.1 },
      "shuffle_rounds": true,
      "rounds_to_win": 4,
      "fiesta_interval_sec": 15.0,
      "warp_zones": [
        { "name": "infield_bowl", "anchor": "arena_center", "radius_m": 26.0, "face_center": true },
        { "name": "backstretch",  "anchor": "spline_pct", "pct": 0.50, "radius_m": 18.0, "face_center": false },
        { "name": "custom_pad",   "anchor": "world", "origin": [10.0, 2.0, -5.0], "radius_m": 12.0 }
      ],
      "roles": [
        {
          "role": "Juggernaut",
          "forced_vehicle_key": "bus",
          "direction_sign": 1,
          "feat_bits": ["FB_CHICKEN_STRAIGHT", "FB_INVINCIBLE"],
          "traits": { "mass_scale": 2.2, "damage_taken_scale": 0.35, "torque_scale": 1.35 }
        },
        {
          "role": "Infected",
          "forced_vehicle_key": "hearse",
          "direction_sign": -1,
          "feat_bits": ["FB_ONE_HIT_KILLS", "FB_DEADLY_AMBITION"],
          "traits": { "gravity_scale": 0.45, "grip_scale": 0.6 }
        }
      ],
      "micro_rounds": [
        {
          "title": "LOW-G CHICKEN SUMOS!",
          "duration_sec": 25.0,
          "rule_flags": ["RF_PUSH_OFF"],
          "global_feats": ["FB_CHICKEN_STRAIGHT"],
          "traits": { "gravity_scale": 0.45, "chicken_force_scale": 2.5, "chicken_reload_sec": 1.2 },
          "sfx_cue": "arena_horn.sfx",
          "warp_zone": "infield_bowl",
          "triggers": [
            { "id": 1, "kind": "RingOut", "anchor": "arena_center", "extents": [34.0, 12.0, 34.0] },
            { "id": 2, "kind": "Cylinder", "anchor": "spline_pct", "pct": 0.25, "extents": [10.0, 6.0, 10.0], "dwell_sec": 3.0 }
          ]
        }
      ]
    })";

    mutator::GametypeConfig parsed;
    std::string err;
    require(mutator::parse_gametype_json(json, parsed, err), "parse valid gametype JSON");
    require(parsed.id == "destructionware_fiesta", "parsed id");
    require(parsed.base_event_type == 0x18000224u, "parsed hex event_type");
    require((parsed.base_rule_flags & (mutator::RF_FIESTA_ROULETTE | mutator::RF_SUICIDE)) ==
                (mutator::RF_FIESTA_ROULETTE | mutator::RF_SUICIDE), "parsed rule flags");
    require(parsed.roles.size() == 2, "parsed 2 roles");
    require(parsed.roles[0].forced_vehicle_key == "bus", "string vehicle key preserved");
    require(parsed.roles[1].direction_sign == -1, "reversed direction_sign preserved");
    require(parsed.warp_zones.size() == 3, "parsed 3 warp zones");
    require(parsed.warp_zones[0].anchor == mutator::AnchorKind::ArenaCenter, "arena_center anchor");
    require(parsed.warp_zones[1].anchor == mutator::AnchorKind::SplinePct && near(parsed.warp_zones[1].spline_pct, 0.5f), "spline_pct anchor");
    require(parsed.warp_zones[2].anchor == mutator::AnchorKind::WorldXYZ && near(parsed.warp_zones[2].origin[0], 10.0f), "world anchor");
    require(parsed.micro_rounds.size() == 1 && parsed.micro_rounds[0].triggers.size() == 2, "parsed micro-round triggers");
    require(parsed.micro_rounds[0].triggers[1].anchor == mutator::AnchorKind::SplinePct, "trigger spline_pct anchor");

    // Wire round-trip
    netplay::Writer w;
    mutator::put_mutator_config(w, parsed);
    netplay::Reader r(w.b.data(), w.b.size());
    mutator::GametypeConfig wire_out;
    require(mutator::get_mutator_config(r, wire_out) && r.p == r.e, "wire round-trip succeeds");
    require(wire_out.id == parsed.id && wire_out.base_traits == parsed.base_traits, "wire base fields match");
    require(wire_out.roles.size() == 2 && wire_out.roles[0].forced_vehicle_key == "bus", "wire role key matches");
    require(wire_out.roles[1].direction_sign == -1, "wire direction_sign matches");
    require(wire_out.micro_rounds.size() == 1 && wire_out.micro_rounds[0].triggers.size() == 2, "wire triggers match");

    // Truncated packet rejected
    netplay::Reader trunc(w.b.data(), w.b.size() - 3);
    mutator::GametypeConfig bad;
    require(!mutator::get_mutator_config(trunc, bad), "truncated K_MUTATOR_CONFIG rejected");

    // Invalid JSON rejected
    std::string bad_err;
    require(!mutator::parse_gametype_json(R"({"name":"missing id"})", bad, bad_err), "missing id rejected");
}

void test_vehicle_key_and_chassis_budget() {
    constexpr uint32_t kTable = 0x00100000;
    const char *names[] = {"CompactA", "MidsizeB", "MuscleC", "SchoolBus", "Hearse", "Gladiator", "MonsterTruck"};
    ee_w32(0x003CBEE4, kTable);
    ee_w32(0x00439704, 7);
    for (uint32_t i = 0; i < 7; ++i) {
        const uint32_t rec = kTable + i * 0x54u;
        ee_w32(rec + 0, i);
        ee_w32(rec + 4, 100u + i);
        write_cstr(rec + 8, names[i]);
        ee_w32(rec + 0x40, i % 4);
    }

    mutator::ResolvedVehicle v;
    require(mutator::Engine::resolve_vehicle_key("bus", v) && v.vehicle_number == 103u && v.chassis_name == "SchoolBus",
            "substring key 'bus' resolves SchoolBus");
    require(mutator::Engine::resolve_vehicle_key("HEARSE", v) && v.vehicle_number == 104u,
            "case-insensitive exact key 'HEARSE' resolves Hearse");
    require(!mutator::Engine::resolve_vehicle_key("nonexistent_tank", v), "unknown key returns false");

    // 16 players sharing 6 distinct chassis stays within kMaxLoadedChassis (6)
    std::vector<uint32_t> ok_16p = {100,101,102,103,104,105,100,101,102,103,104,105,100,101,102,103};
    require(mutator::Engine::count_distinct_chassis(ok_16p) == 6, "6 distinct chassis across 16 seats");
    require(mutator::Engine::within_chassis_budget(ok_16p), "6 distinct chassis within EE heap budget");

    // 7 distinct chassis exceeds safety cap
    std::vector<uint32_t> over = {100,101,102,103,104,105,106};
    require(mutator::Engine::count_distinct_chassis(over) == 7, "7 distinct chassis detected");
    require(!mutator::Engine::within_chassis_budget(over), "7 distinct chassis flagged over budget");
}

void test_track_anchored_zones() {
    // 1. Mock BattleMaster respawn table -> ArenaCenter centroid
    constexpr uint32_t kMaster = 0x00120000;
    ee_w32(0x003CF194, kMaster);
    ee_w32(kMaster, battle_layout::kMasterVtable);
    ee_w32(kMaster + battle_layout::kRespawnCount, 2);
    wf(kMaster + 0x10 + 0 * 0x44 + 0x24, 10.0f); wf(kMaster + 0x10 + 0 * 0x44 + 0x28, 1.0f); wf(kMaster + 0x10 + 0 * 0x44 + 0x2C, -20.0f);
    wf(kMaster + 0x10 + 1 * 0x44 + 0x24, 30.0f); wf(kMaster + 0x10 + 1 * 0x44 + 0x28, 3.0f); wf(kMaster + 0x10 + 1 * 0x44 + 0x2C, 40.0f);

    mutator_hooks::ground_snap_y = [](float, float, float &out_y) { out_y = 4.25f; return true; };
    float out[3] = {};
    const float zero[3] = {0.0f, 0.0f, 0.0f};
    require(mutator::Engine::resolve_anchor(mutator::AnchorKind::ArenaCenter, zero, 0.0f, out), "ArenaCenter resolved");
    require(near(out[0], 20.0f) && near(out[1], 4.25f) && near(out[2], 10.0f), "ArenaCenter centroid + ground snap");

    // 2. Mock ILIN primary course cache -> SplinePct interpolation
    constexpr uint32_t kCourseRes = 0x00130000;
    constexpr uint32_t kNodes = 0x00131000;
    ee_w32(0x003CD1B4, 1);
    ee_w32(0x003CD190, kCourseRes);
    ee_w32(0x003CD194, 0x101u);
    ee_w32(kCourseRes + 0, 0x104F128Bu);
    ee_w32(kCourseRes + 8, 60u);
    ee_w32(kCourseRes + 28, 4u << 16); // 4 nodes
    ee_w32(kCourseRes + 32, kNodes);
    const float pts[4][2] = {{0.0f, 0.0f}, {100.0f, 0.0f}, {100.0f, 100.0f}, {0.0f, 100.0f}};
    for (uint32_t i = 0; i < 4; ++i) {
        wf(kNodes + i * 60u + 0, pts[i][0]);
        wf(kNodes + i * 60u + 4, pts[i][1]);
    }
    require(mutator::Engine::resolve_anchor(mutator::AnchorKind::SplinePct, zero, 0.375f, out), "SplinePct resolved");
    require(near(out[0], 100.0f) && near(out[1], 4.25f) && near(out[2], 50.0f), "SplinePct midpoint on segment 1->2");
    mutator_hooks::ground_snap_y = nullptr;
}

void test_per_body_gravity_and_live_physics() {
    // 1. Direct VU0 dynamics solver gravity check
    alignas(16) std::array<uint8_t, 4096> mem_a{}, mem_b{};
    auto init_state = [](VUState &vu, uint8_t *m) {
        vu = {};
        vu.mem = m;
        // RigidBody at qword 0..10: pos=(0,10,0), identity axes, quat=(0,0,0,1), invMass=1/1000
        auto putf = [&](size_t q, size_t lane, float v) {
            std::memcpy(m + q * 16 + lane * 4, &v, 4);
        };
        putf(0, 1, 10.0f);
        putf(1, 0, 1.0f); putf(2, 1, 1.0f); putf(3, 2, 1.0f);
        putf(4, 3, 1.0f);
        putf(8, 0, 1e-3f); putf(8, 1, 1e-3f); putf(8, 2, 1e-3f); putf(8, 3, 1e-3f);
    };
    VUState normal_vu{}, lowg_vu{};
    init_state(normal_vu, mem_a.data());
    init_state(lowg_vu, mem_b.data());
    vehicle_dynamics_step(normal_vu);
    vehicle_dynamics_step_with_gravity(lowg_vu, 9.81f * 0.45f);
    float vy_normal = 0.0f, vy_lowg = 0.0f;
    std::memcpy(&vy_normal, mem_a.data() + 5 * 16 + 4, 4);
    std::memcpy(&vy_lowg, mem_b.data() + 5 * 16 + 4, 4);
    require(vy_lowg > vy_normal && near(vy_lowg / vy_normal, 0.45f, 0.02f),
            "per-body gravity scales vertical acceleration accurately");

    // 2. Per-slot RigidBody / Tuning / car+0x4B0 baseline + non-compounding writes
    constexpr uint32_t kCar0 = 0x00140000, kCar1 = 0x00144000;
    constexpr uint32_t kBodies = 0x00150000, kTunings = 0x00152000;
    ee_w32(0x0048522C, 2);
    ee_w32(0x00447458, kCar0);
    ee_w32(0x0044745C, kCar1);
    ee_w32(0x003CE548, 2);
    ee_w32(0x003CE4F8, kCar0);
    ee_w32(0x003CE4FC, kCar1);
    ee_w32(0x00494D28, 2);
    ee_w32(0x00494D30, kBodies);
    ee_w32(0x00494D38, kTunings);

    for (uint32_t i = 0; i < 2; ++i) {
        const uint32_t car = i == 0 ? kCar0 : kCar1;
        ee_w32(car + 0x1DC, i);
        wf(car + 0x4B0, 1.0f);
        for (uint32_t k = 0; k < 4; ++k) {
            wf(kBodies + i * 0xB0 + 0x80 + k * 4, 0.001f);
            wf(kTunings + i * 0x180 + 0xA0 + k * 4, 1.2f);
        }
    }

    mutator::GametypeConfig cfg;
    cfg.id = "asymmetric_fiesta";
    cfg.base_rule_flags = mutator::RF_SUICIDE;
    mutator::RoleOverride r0;
    r0.role = mutator::RoleId::Juggernaut;
    r0.direction_sign = 1;
    r0.feat_bits = mutator::FB_INVINCIBLE | mutator::FB_CHICKEN_STRAIGHT;
    r0.traits.gravity_scale = 0.5f;
    r0.traits.mass_scale = 2.0f;
    r0.traits.grip_scale = 1.5f;
    r0.traits.damage_taken_scale = 0.25f;
    mutator::RoleOverride r1;
    r1.role = mutator::RoleId::Infected;
    r1.direction_sign = -1;
    r1.feat_bits = mutator::FB_ONE_HIT_KILLS;
    r1.traits.gravity_scale = 1.5f;
    r1.traits.mass_scale = 0.5f;
    r1.traits.grip_scale = 0.5f;
    r1.traits.damage_taken_scale = 2.0f;
    cfg.roles = {r0, r1};

    auto &eng = mutator::engine();
    eng.set_config(cfg);
    eng.on_world_bound(7, 0x12345678u);
    eng.sync_live_physics();
    eng.sync_live_physics(); // Second call must NOT compound!

    require(near(vehicle_dynamics_get_body_gravity(0), 9.81f * 0.5f), "slot 0 low-G applied");
    require(near(vehicle_dynamics_get_body_gravity(1), 9.81f * 1.5f), "slot 1 high-G applied");
    require(near(rf(kBodies + 0 * 0xB0 + 0x8C), 0.0005f), "slot 0 mass x2 halves inverse mass without compounding");
    require(near(rf(kBodies + 1 * 0xB0 + 0x8C), 0.0020f), "slot 1 mass x0.5 doubles inverse mass");
    require(near(rf(kTunings + 0 * 0x180 + 0xA0), 1.8f), "slot 0 grip scaled x1.5");
    require(near(rf(kCar0 + 0x4B0), 0.25f) && near(rf(kCar1 + 0x4B0), 2.0f), "per-slot damage_mult applied");
    require(mutator_hooks::car_invincible(kCar0) && !mutator_hooks::car_invincible(kCar1), "per-slot invincibility");
    require(!mutator_hooks::suppress_wrong_way(kCar0) && mutator_hooks::suppress_wrong_way(kCar1), "per-slot direction_sign");

    // 3. Verify hle_recovery.cpp suppresses wrong-way auto-reset for reversed AI/human slot
    constexpr uint32_t kTimer = 0x00160000, kPerson0 = 0x00160100, kPerson1 = 0x00160300;
    ee_w32(kAutoReset, 1);
    ee_w32(kCar0 + 12, 0x50434152); ee_w32(kCar0 + 480, 0x51); // AI normal
    ee_w32(kCar1 + 12, 0x50434152); ee_w32(kCar1 + 480, 0x51); // AI reversed
    ee_w32(kPerson0 + 4, kCar0);
    ee_w32(kPerson1 + 4, kCar1);
    g_recovery_resets = 0;

    g_ee = {}; g_ee.r[4].uw[0] = kTimer; g_ee.r[5].uw[0] = kPerson0;
    f_001B8370();
    require(g_recovery_resets == 1 && ee_r32(kPerson0 + 192) == 0xFFFFFFFFu, "normal slot keeps native recovery");

    g_ee = {}; g_ee.r[4].uw[0] = kTimer; g_ee.r[5].uw[0] = kPerson1;
    f_001B8370();
    require(g_recovery_resets == 1 && ee_r32(kPerson1 + 192) == 0 && ee_r32(kAutoReset) == 1,
            "reversed slot suppresses false wrong-way auto-reset & HUD warning");

    // 4. Fiesta deterministic reroll across ticks
    cfg.roles.clear();
    cfg.base_rule_flags = mutator::RF_FIESTA_ROULETTE;
    cfg.fiesta_interval_sec = 10.0f;
    eng.set_config(cfg);
    eng.on_world_bound(8, 0xCAFEBABEu);
    const float g_epoch0 = vehicle_dynamics_get_body_gravity(0);
    eng.tick(599);
    require(eng.fiesta_epoch() == 0 && near(vehicle_dynamics_get_body_gravity(0), g_epoch0), "no reroll before interval");
    eng.tick(600);
    const float g_epoch1 = vehicle_dynamics_get_body_gravity(0);
    require(eng.fiesta_epoch() == 1, "Fiesta epoch advanced at 600 ticks (10s)");

    // Re-running with same seed reproduces exact epoch 1 gravity
    eng.reset_match();
    eng.set_config(cfg);
    eng.on_world_bound(8, 0xCAFEBABEu);
    eng.tick(600);
    require(near(vehicle_dynamics_get_body_gravity(0), g_epoch1), "Fiesta trait reroll is deterministic from seed+epoch");

    eng.clear_config();
    require(!mutator_hooks::active && near(vehicle_dynamics_get_body_gravity(0), 9.81f), "clear_config restores vanilla defaults");
}

void test_atomic_warp_and_puppet_snap() {
    constexpr uint32_t kCar0 = 0x00140000, kCar1 = 0x00144000;
    constexpr uint32_t kBodies = 0x00150000, kWheels = 0x00154000, kTunings = 0x00152000;
    constexpr uint32_t kPerson0 = 0x00160100, kPerson1 = 0x00160300;
    ee_w32(0x0048522C, 2);
    ee_w32(0x00447458, kCar0); ee_w32(0x0044745C, kCar1);
    ee_w32(0x00485230, kPerson0); ee_w32(0x00485234, kPerson1);
    ee_w32(0x003CE548, 2);
    ee_w32(0x003CE4F8, kCar0); ee_w32(0x003CE4FC, kCar1);
    ee_w32(0x00494D28, 2);
    ee_w32(0x00494D30, kBodies);
    ee_w32(0x00494D34, kWheels);
    ee_w32(0x00494D38, kTunings);

    // Seed dirty velocity, suspension travel, damage, tossed wheel, and wrong-way warning
    wf(kCar0 + 0x1BEC, 0.95f);
    ee_w32(kCar0 + 0x1BE8, 10u);
    ee_w32(kCar0 + 0x1A50, 0x80u);
    ee_w32(kCar0 + 0x13E8 + 0x14, 3u);
    wf(kCar0 + netplay_car::kVel, 30.0f);
    wf(kBodies + 0x50, 30.0f);
    wf(kWheels + 0x00, 0.4f); wf(kWheels + 0x90, 5.0f);
    ee_w32(kPerson0 + 192, 0xFFFFFFFFu);

    mutator_hooks::active = true;
    mutator_hooks::slots[0].active = true;
    mutator_hooks::slots[0].warp_gen = 0;

    const auto pose = mutator::make_yaw_pose(50.0f, 3.0f, -25.0f, 1.5707963f);
    require(mutator::warp_slot(0, pose, true), "atomic warp_slot succeeds");
    require(near(rf(kBodies + 0x00), 50.0f) && near(rf(kBodies + 0x04), 3.0f) && near(rf(kBodies + 0x08), -25.0f),
            "RigidBody position updated");
    require(near(rf(kBodies + 0x50), 0.0f) && near(rf(kCar0 + netplay_car::kVel), 0.0f), "velocities zeroed on warp");
    require(near(rf(kWheels + 0x00), 0.0f) && near(rf(kWheels + 0x90), 0.0f), "WheelSet travel and slip zeroed");
    require(near(rf(kCar0 + 0x1BEC), 0.0f) && ee_r32(kCar0 + 0x1BE8) == 0u && (ee_r32(kCar0 + 0x1A50) & 0x80u) == 0u,
            "repair cleared damage, wreck state, and tossed wheel");
    require(ee_r32(kPerson0 + 192) == 0u, "wrong-way warning cleared on warp");
    require(mutator_hooks::slots[0].warp_gen == 1u, "warp_gen incremented");

    // Verify netplay_car::Remote snaps immediately when warp_gen changes (no 8m blend streak)
    netplay_car::Remote rem;
    netplay_car::State s1{};
    s1.pose[9] = 0.0f; s1.pose[10] = 1.0f; s1.pose[11] = 0.0f; s1.warp_gen = 0;
    rem.push(10, s1);
    auto shown1 = rem.advance(0, 1.0f, 0);
    require(near(shown1.pose[9], 0.0f), "initial puppet at origin");

    // Normal large move with same warp_gen blends smoothly (~halfway)
    netplay_car::State s_blend = s1;
    s_blend.pose[9] = 100.0f; s_blend.warp_gen = 0;
    rem.push(12, s_blend);
    auto shown_blend = rem.advance(0, 1.0f, 0);
    require(shown_blend.pose[9] > 1.0f && shown_blend.pose[9] < 95.0f, "same warp_gen blends smoothly");

    // Warp with bumped warp_gen snaps instantly to (250, 2, -80) on first advance
    netplay_car::State s_warp = s1;
    s_warp.pose[9] = 250.0f; s_warp.pose[10] = 2.0f; s_warp.pose[11] = -80.0f; s_warp.warp_gen = 1;
    rem.push(14, s_warp);
    auto shown_warp = rem.advance(100, 0.0f, 20);
    require(near(shown_warp.pose[9], 250.0f) && near(shown_warp.pose[11], -80.0f),
            "bumped warp_gen snaps remote puppet instantly without streaking");

    // Verify put_car / get_car preserves warp_gen on the wire
    netplay::Writer w;
    netplay::put_car(w, 0, 14, s_warp);
    netplay::Reader r(w.b.data(), w.b.size());
    uint8_t dec_slot = 99; uint32_t dec_tick = 0; netplay_car::State dec_state{};
    require(netplay::get_car(r, dec_slot, dec_tick, dec_state) && r.p == r.e && dec_state.warp_gen == 1u,
            "put_car/get_car round-trips warp_gen");
}

void test_micro_rounds_and_round_packet() {
    // Set up 2 cars + BattleMaster ArenaCenter
    constexpr uint32_t kCar0 = 0x00140000, kCar1 = 0x00144000;
    constexpr uint32_t kMaster = 0x00120000;
    ee_w32(0x003CF194, kMaster);
    ee_w32(kMaster, battle_layout::kMasterVtable);
    ee_w32(kMaster + battle_layout::kRespawnCount, 2);
    wf(kMaster + 0x10 + 0 * 0x44 + 0x24, 0.0f); wf(kMaster + 0x10 + 0 * 0x44 + 0x28, 1.0f); wf(kMaster + 0x10 + 0 * 0x44 + 0x2C, 0.0f);
    wf(kMaster + 0x10 + 1 * 0x44 + 0x24, 20.0f); wf(kMaster + 0x10 + 1 * 0x44 + 0x28, 1.0f); wf(kMaster + 0x10 + 1 * 0x44 + 0x2C, 0.0f);
    ee_w32(0x0048522C, 2);
    ee_w32(0x00447458, kCar0); ee_w32(0x0044745C, kCar1);

    mutator::GametypeConfig cfg;
    cfg.id = "destructionware_3r";
    cfg.name = "DestructionWare 3-Round";
    cfg.shuffle_rounds = false;
    mutator::WarpZone wz;
    wz.name = "infield_bowl";
    wz.anchor = mutator::AnchorKind::ArenaCenter;
    wz.radius_m = 20.0f;
    cfg.warp_zones.push_back(wz);

    mutator::MicroRoundDef r1;
    r1.title = "LOW-G CHICKEN SUMOS!";
    r1.duration_sec = 10.0f;
    r1.rule_flags = mutator::RF_PUSH_OFF;
    r1.global_feats = mutator::FB_CHICKEN_STRAIGHT;
    r1.traits.gravity_scale = 0.45f;
    r1.warp_zone = "infield_bowl";
    r1.sfx_cue = "arena_horn.sfx";
    r1.pa_speech_sid = 42;

    mutator::MicroRoundDef r2;
    r2.title = "ONE-HIT SUICIDE!";
    r2.duration_sec = 10.0f;
    r2.rule_flags = mutator::RF_SUICIDE;
    r2.global_feats = mutator::FB_ONE_HIT_KILLS | mutator::FB_TURBO;
    r2.traits.gravity_scale = 1.0f;

    mutator::MicroRoundDef r3;
    r3.title = "DEADLY AMBITION!";
    r3.duration_sec = 10.0f;
    r3.global_feats = mutator::FB_DEADLY_AMBITION;
    cfg.micro_rounds = {r1, r2, r3};

    static int banner_count = 0, sfx_count = 0, pa_count = 0;
    banner_count = sfx_count = pa_count = 0;
    mutator_hooks::show_hud_banner = [](const char *, int) { ++banner_count; };
    mutator_hooks::play_sfx_cue = [](const char *) { ++sfx_count; };
    mutator_hooks::play_pa_speech = [](int16_t) { ++pa_count; };

    auto &eng = mutator::engine();
    eng.set_config(cfg);
    eng.on_world_bound(9, 0x13579BDFu, true, 0);

    require(eng.round_serial() == 1 && mutator_hooks::hud.visible, "round 1 started and HUD visible");
    require(mutator_hooks::hud.round_title == "LOW-G CHICKEN SUMOS!", "round 1 HUD title");
    require(near(mutator_hooks::hud.remaining_sec, 10.0f, 0.05f), "round 1 duration 10s");
    require(banner_count == 1 && sfx_count == 1 && pa_count == 1, "round 1 fired HUD banner, SFX, and PA speech");
    require(eng.warp_gen() == 1u, "round 1 warped formation and bumped warp_gen");

    // Verify 2 cars placed on non-overlapping circle around ArenaCenter (10, 1, 0)
    const float dx = rf(kCar0 + netplay_car::kPose + 36) - rf(kCar1 + netplay_car::kPose + 36);
    const float dz = rf(kCar0 + netplay_car::kPose + 44) - rf(kCar1 + netplay_car::kPose + 44);
    require(std::hypot(dx, dz) >= 35.0f, "formation warped 2 cars on opposite sides of 20m circle");

    // Advance 300 ticks (5s) -> remaining 5s, still round 1
    eng.tick(300);
    require(eng.round_serial() == 1 && near(mutator_hooks::hud.remaining_sec, 5.0f, 0.05f), "mid-round countdown");

    // Advance to 600 ticks (10s) -> transitions to Round 2 (RF_SUICIDE + ONE-HIT)
    eng.tick(600);
    require(eng.round_serial() == 2 && mutator_hooks::hud.round_title == "ONE-HIT SUICIDE!", "auto-advanced to round 2");
    require(mutator_hooks::slots[0].direction_sign == 1 && mutator_hooks::slots[1].direction_sign == -1,
            "round 2 RF_SUICIDE flipped odd slot direction_sign");

    // Serialize Round 2 packet -> Guest applies it cleanly
    netplay::Writer rw;
    mutator::put_mutator_round(rw, eng.latest_round_packet());
    netplay::Reader rr(rw.b.data(), rw.b.size());
    mutator::RoundPacket decoded{};
    require(mutator::get_mutator_round(rr, decoded) && rr.p == rr.e, "K_MUTATOR_ROUND wire round-trip");
    require(decoded.round_serial == 2 && decoded.round_def_index == 1 && decoded.slot_direction[1] == -1,
            "K_MUTATOR_ROUND fields preserved");

    // Guest receives Round 2 packet
    eng.reset_match();
    eng.set_config(cfg);
    eng.on_world_bound(9, 0x13579BDFu, false, 1);
    require(eng.round_serial() == 0, "guest waits for host round packet");
    require(eng.apply_remote_round(decoded, 600), "guest applies host Round 2 packet");
    require(eng.round_serial() == 2 && mutator_hooks::hud.local_direction_sign == -1,
            "guest HUD reflects local slot 1 reversed sign");
    require(!eng.apply_remote_round(decoded, 601), "duplicate round serial ignored");

    // Advance host to Round 3
    eng.reset_match();
    eng.set_config(cfg);
    eng.on_world_bound(9, 0x13579BDFu, true, 0);
    eng.tick(600);
    eng.tick(1200);
    require(eng.round_serial() == 3 && mutator_hooks::hud.round_title == "DEADLY AMBITION!", "auto-advanced to round 3");

    mutator_hooks::show_hud_banner = nullptr;
    mutator_hooks::play_sfx_cue = nullptr;
    mutator_hooks::play_pa_speech = nullptr;
    eng.clear_config();
}

void test_presets_and_trigger_wire_codec() {
    mutator::GametypeConfig cfg;
    require(!mutator::build_preset(mutator::PRESET_STANDARD, cfg), "Standard preset yields no mutator config");
    for (uint8_t id = mutator::PRESET_BATTLE_ROYALE; id < mutator::PRESET_COUNT; ++id) {
        mutator::GametypeConfig p;
        require(mutator::build_preset(id, p), "built-in preset constructs cleanly");
        netplay::Writer w;
        mutator::put_mutator_config(w, p);
        netplay::Reader r(w.b.data(), w.b.size());
        mutator::GametypeConfig dec;
        require(mutator::get_mutator_config(r, dec) && r.p == r.e && dec.id == p.id,
                "preset K_MUTATOR_CONFIG wire round-trip");
    }

    // K_MUTATOR_TRIGGER round-trip + corruption rejection
    mutator::TriggerSyncPacket pkt;
    pkt.sim_tick = 1234;
    pkt.hitch_mask = 0x0005u;
    pkt.eliminated_mask = 0x0002u;
    mutator::ActiveTriggerState tr;
    tr.id = 7; tr.kind = mutator::TriggerKind::RingOut;
    tr.center[0] = 10.0f; tr.center[1] = 1.0f; tr.center[2] = -5.0f;
    tr.radius = 42.5f; tr.extents[0] = 42.5f; tr.extents[1] = 15.0f; tr.extents[2] = 42.5f;
    tr.carrier_slot = 0xFF; tr.color_rgba = 0xFF3A7BFFu; tr.active = true;
    pkt.triggers.push_back(tr);
    mutator::ChainPair cp{0, 1, 11.0f, 18000.0f, 1800.0f};
    pkt.chains.push_back(cp);

    netplay::Writer tw;
    mutator::put_mutator_triggers(tw, pkt);
    netplay::Reader trd(tw.b.data(), tw.b.size());
    mutator::TriggerSyncPacket dec_pkt;
    require(mutator::get_mutator_triggers(trd, dec_pkt) && trd.p == trd.e, "K_MUTATOR_TRIGGER wire round-trip");
    require(dec_pkt.sim_tick == 1234 && dec_pkt.hitch_mask == 0x0005u && dec_pkt.eliminated_mask == 0x0002u,
            "K_MUTATOR_TRIGGER header fields preserved");
    require(dec_pkt.triggers.size() == 1 && near(dec_pkt.triggers[0].radius, 42.5f), "trigger radius preserved");
    require(dec_pkt.chains.size() == 1 && dec_pkt.chains[0].slot_b == 1, "chain pair preserved");

    // Truncated packet must fail gracefully
    netplay::Reader bad(tw.b.data(), tw.b.size() - 4);
    mutator::TriggerSyncPacket dummy;
    require(!mutator::get_mutator_triggers(bad, dummy), "truncated K_MUTATOR_TRIGGER rejected");
}

void test_battle_royale_shrinking_storm() {
    std::memset(g_ram, 0, EE_RAM_SIZE);
    constexpr uint32_t kCar0 = 0x00140000, kCar1 = 0x00144000;
    constexpr uint32_t kMaster = 0x00120000;
    ee_w32(0x003CF194, kMaster);
    ee_w32(kMaster, battle_layout::kMasterVtable);
    ee_w32(kMaster + battle_layout::kRespawnCount, 1);
    wf(kMaster + 0x10 + 0x24, 0.0f); wf(kMaster + 0x10 + 0x28, 0.0f); wf(kMaster + 0x10 + 0x2C, 0.0f);
    ee_w32(0x0048522C, 2);
    ee_w32(0x00447458, kCar0); ee_w32(0x0044745C, kCar1);

    // Slot 0 inside safe zone (5, 0, 0), Slot 1 outside safe zone (120, 0, 0)
    wf(kCar0 + 0x88, 5.0f); wf(kCar0 + 0x8C, 0.0f); wf(kCar0 + 0x90, 0.0f);
    wf(kCar1 + 0x88, 120.0f); wf(kCar1 + 0x8C, 0.0f); wf(kCar1 + 0x90, 0.0f);
    wf(kCar0 + 0x1BEC, 0.0f); ee_w32(kCar0 + 0x1BE8, 0);
    wf(kCar1 + 0x1BEC, 0.0f); ee_w32(kCar1 + 0x1BE8, 0);

    auto &eng = mutator::engine();
    eng.set_config(mutator::make_battle_royale_preset());
    eng.on_world_bound(20, 0xBEEFu, true, 1);

    const float initial_r = eng.triggers().triggers()[0].radius;
    require(near(initial_r, 95.0f), "Battle Royale storm starts at 95m");

    // Simulate 8 seconds (480 ticks at 60 Hz; dps_outside=0.14 -> >1.0 damage wrecks slot 1)
    for (uint32_t tick = 1; tick <= 480; ++tick) eng.tick(tick);

    const float shrunk_r = eng.triggers().triggers()[0].radius;
    require(shrunk_r < initial_r - 8.0f && shrunk_r >= 12.0f, "Battle Royale storm shrinks monotonically");
    require(near(rf(kCar0 + 0x1BEC), 0.0f) && ee_r32(kCar0 + 0x1BE8) == 0u, "car inside safe zone takes zero storm damage");
    require(ee_r32(kCar1 + 0x1BE8) >= 10u && (eng.triggers().eliminated_mask() & 2u) != 0u,
            "car outside storm wrecks and is marked eliminated");

    eng.clear_config();
}

void test_halo_infection_tag() {
    std::memset(g_ram, 0, EE_RAM_SIZE);
    constexpr uint32_t kCar0 = 0x00140000, kCar1 = 0x00144000, kCar2 = 0x00148000;
    ee_w32(0x0048522C, 3);
    ee_w32(0x00447458, kCar0); ee_w32(0x0044745C, kCar1); ee_w32(0x00447460, kCar2);

    // Slot 0 = Alpha Infected at (0,0,0), Slot 1 = Survivor at (3,0,0) (within 5m tag range), Slot 2 = Survivor at (80,0,0)
    wf(kCar0 + 0x88, 0.0f); wf(kCar1 + 0x88, 3.0f); wf(kCar2 + 0x88, 80.0f);
    wf(kCar1 + 0x1BEC, 0.7f);

    auto &eng = mutator::engine();
    eng.set_config(mutator::make_infection_preset());
    eng.on_world_bound(21, 0xCAFEu, true, 0);

    require(mutator_hooks::slots[0].role == mutator::RoleId::Infected, "slot 0 starts as Alpha Infected");
    require(mutator_hooks::slots[1].role == mutator::RoleId::Survivor &&
            mutator_hooks::slots[2].role == mutator::RoleId::Survivor, "slots 1-2 start as Survivors");
    const uint32_t serial_before = eng.round_serial();

    eng.tick(1);
    require(mutator_hooks::slots[1].role == mutator::RoleId::Infected, "contact converted Survivor slot 1 to Infected");
    require(mutator_hooks::slots[2].role == mutator::RoleId::Survivor, "distant Survivor slot 2 remains Survivor");
    require(mutator_hooks::slots[0].score == 1, "Alpha Infected awarded +1 tag score");
    require(near(rf(kCar1 + 0x1BEC), 0.0f), "newly infected car repaired on conversion");
    require(eng.round_serial() == serial_before + 1u, "infection conversion bumps round_serial for immediate K_MUTATOR_ROUND push");
    require(mutator_hooks::hud.survivors_alive == 1 && mutator_hooks::hud.infected_count == 2,
            "HUD infection counts updated");

    // Verify 1.5s tag cooldown prevents instant chain-tag by slot 1
    wf(kCar1 + 0x88, 78.0f);
    eng.tick(2);
    require(mutator_hooks::slots[2].role == mutator::RoleId::Survivor, "newly infected slot respects 1.5s tag cooldown");

    // After 100 ticks (~1.67s), slot 1 can tag slot 2
    for (uint32_t t = 3; t <= 105; ++t) eng.tick(t);
    require(mutator_hooks::slots[2].role == mutator::RoleId::Infected, "slot 1 tags slot 2 once cooldown expires");

    eng.clear_config();
}

void test_ctf_flag_and_stop_and_go() {
    std::memset(g_ram, 0, EE_RAM_SIZE);
    constexpr uint32_t kCar0 = 0x00140000;
    ee_w32(0x0048522C, 1);
    ee_w32(0x00447458, kCar0);

    mutator::GametypeConfig cfg;
    cfg.id = "flag_and_stop";
    cfg.name = "Flag & Stop";
    cfg.base_rule_flags = mutator::RF_STOP_AND_GO;

    mutator::TriggerDef flag;
    flag.id = 1; flag.kind = mutator::TriggerKind::Flag; flag.anchor = mutator::AnchorKind::WorldXYZ;
    flag.origin[0] = 10.0f; flag.origin[1] = 0.0f; flag.origin[2] = 0.0f;
    cfg.base_triggers.push_back(flag);

    mutator::TriggerDef stop_box;
    stop_box.id = 2; stop_box.kind = mutator::TriggerKind::Cylinder; stop_box.anchor = mutator::AnchorKind::WorldXYZ;
    stop_box.origin[0] = 50.0f; stop_box.origin[1] = 0.0f; stop_box.origin[2] = 0.0f;
    stop_box.extents[0] = 10.0f; stop_box.extents[1] = 5.0f; stop_box.extents[2] = 10.0f;
    stop_box.dwell_sec = 0.5f;
    cfg.base_triggers.push_back(stop_box);

    auto &eng = mutator::engine();
    eng.set_config(cfg);
    eng.on_world_bound(22, 0x1234u, true, 0);

    // Move car onto flag at (10, 0, 0)
    wf(kCar0 + 0x88, 11.0f); wf(kCar0 + 0x8C, 0.0f); wf(kCar0 + 0x90, 0.0f);
    eng.tick(1);
    require(eng.triggers().triggers()[0].carrier_slot == 0u, "car picked up CTF flag");

    // Drive car to (50, 0, 0) at high speed (15 m/s) -> flag follows, Stop-and-Go rejects high speed
    wf(kCar0 + 0x88, 50.0f); wf(kCar0 + 0x104, 15.0f);
    for (uint32_t t = 2; t <= 40; ++t) eng.tick(t);
    require(near(eng.triggers().triggers()[0].center[0], 50.0f), "flag follows carrier position");
    require(mutator_hooks::slots[0].score == 0, "Stop-and-Go does not score while speeding");

    // Come to a stop inside Stop-and-Go box for 0.6s -> scores!
    wf(kCar0 + 0x104, 0.5f);
    for (uint32_t t = 41; t <= 80; ++t) eng.tick(t);
    require(mutator_hooks::slots[0].score >= 1, "Stop-and-Go scores after full stop dwell");

    // Wreck carrier -> flag drops
    ee_w32(kCar0 + 0x1BE8, 10u);
    eng.tick(81);
    require(eng.triggers().triggers()[0].carrier_slot == 0xFFu, "wrecking carrier drops flag");

    eng.clear_config();
}

void test_chain_tether_and_trailer_detach() {
    std::memset(g_ram, 0, EE_RAM_SIZE);
    constexpr uint32_t kCar0 = 0x00140000, kCar1 = 0x00144000, kTrailer0 = 0x00148000;
    constexpr uint32_t kBodies = 0x00220000;
    ee_w32(0x0048522C, 2);
    ee_w32(0x00447458, kCar0); ee_w32(0x0044745C, kCar1);
    ee_w32(0x003CE548, 3); ee_w32(0x00494D28, 3);
    ee_w32(0x003CE4F8, kCar0); ee_w32(0x003CE4FC, kCar1); ee_w32(0x003CE500, kTrailer0);
    ee_w32(0x00494D30, kBodies);

    // Trailer attached to Car0
    ee_w32(kTrailer0 + 0x1E0, 0x56u);
    ee_w32(kTrailer0 + 0x1C18, kCar0);

    // Put Car0 at x=0, Car1 at x=5 (within 11m rest_len -> slack, zero force)
    wf(kBodies + 0x00, 0.0f); wf(kBodies + 0xB0 + 0x00, 5.0f);

    mutator::ChainSystem cs;
    cs.setup_pairs(2, 11.0f, 10000.0f, 0.0f);
    cs.step_host(2);
    require(near(rf(kBodies + 0x90), 0.0f) && near(rf(kBodies + 0xB0 + 0x90), 0.0f),
            "slack chain exerts zero restoring force");

    // Stretch Car1 to x=14 (3m past 11m rest_len -> 30000 N equal and opposite)
    wf(kBodies + 0xB0 + 0x00, 14.0f);
    cs.step_host(2);
    require(near(rf(kBodies + 0x90), 30000.0f, 1.0f), "taut chain pulls slot 0 toward slot 1 (+30000 N)");
    require(near(rf(kBodies + 0xB0 + 0x90), -30000.0f, 1.0f), "taut chain pulls slot 1 toward slot 0 (-30000 N)");

    // Trailer hitch mask & detach
    require(mutator::ChainSystem::snapshot_hitch_mask(2) == 1u, "hitch mask detects trailer on slot 0");
    mutator::ChainSystem::apply_remote_hitch_mask(0u, 2);
    require(mutator::ChainSystem::snapshot_hitch_mask(2) == 0u && ee_r32(kTrailer0 + 0x1C18) == 0u,
            "remote hitch mask cleanly detaches trailer");
}

void test_16_player_battle_layout() {
    require(battle_layout::kCapacity == 16u && battle_layout::kMaxCars == 16u,
            "BattleMaster lifted to 16-player capacity");
    require(battle_layout::kRanking == 0x954u && battle_layout::kSpawnIndex == 0x994u &&
            battle_layout::kCount == 0x998u && battle_layout::kMasterSize == 0x99Cu,
            "BattleMaster 16-slot offsets verified");
}

void test_tips_hot_potato_tag() {
    std::memset(g_ram, 0, EE_RAM_SIZE);
    constexpr uint32_t kCars[4] = {0x00140000, 0x00144000, 0x00148000, 0x0014C000};
    ee_w32(0x0048522C, 4);
    for (uint32_t i = 0; i < 4; ++i) {
        ee_w32(0x00447458 + i * 4, kCars[i]);
        wf(kCars[i] + 0x88, static_cast<float>(i) * 40.0f);
        wf(kCars[i] + 0x8C, 0.0f);
        wf(kCars[i] + 0x90, 0.0f);
        wf(kCars[i] + 0x1BEC, 0.0f);
        ee_w32(kCars[i] + 0x1BE8, 0u);
    }

    auto &eng = mutator::engine();
    mutator::GametypeConfig cfg = mutator::make_tips_tag_preset();
    require(cfg.id == "tips_tag_hot_potato" && (cfg.base_rule_flags & mutator::RF_TIPS_TAG) != 0 &&
            near(cfg.tips_fuse_sec, 25.0f) && cfg.base_triggers.empty(),
            "Tips preset configured as open-field 25s Hot Potato");

    // Verify random initial "IN" selection varies across seeds (can pick any human or AI slot)
    uint32_t seen_mask = 0;
    for (uint32_t s = 1; s <= 32; ++s) {
        eng.set_config(cfg);
        eng.on_world_bound(30 + s, s * 0x9E3779B9u, true, 0);
        const uint8_t picked = mutator_hooks::hud.tips_in_slot;
        require(picked < 4u, "Tips initial IN slot in [0..3]");
        seen_mask |= (1u << picked);
    }
    require(seen_mask == 0xFu, "Tips random initial IN covers all 4 slots (human + AI)");

    // Run full deterministic Tips scenario with 4 cars
    eng.set_config(cfg);
    eng.on_world_bound(100, 0x12345678u, true, 0);
    require(eng.has_live_sync_state(), "Tips enables 10 Hz live trigger sync");
    const uint8_t in0 = mutator_hooks::hud.tips_in_slot;
    const uint8_t other = static_cast<uint8_t>((in0 + 1) % 4);
    require(in0 < 4u && mutator_hooks::slots[in0].role == mutator::RoleId::Tagger, "initial IN slot has Tagger role");
    require(mutator_hooks::slots[other].role == mutator::RoleId::Survivor, "other slot starts as Survivor");
    require(mutator_hooks::hud.tips_alive_count == 4u && near(mutator_hooks::hud.tips_fuse_sec, 25.0f),
            "Tips starts with 4 alive and 25s fuse");

    // Drain 5 seconds (300 ticks at 60 Hz) while separated -> only in0's fuse drains
    for (uint32_t t = 1; t <= 300; ++t) eng.tick(t);
    require(mutator_hooks::hud.tips_in_slot == in0, "same slot remains IN while separated");
    require(near(eng.triggers().tips_slot_fuse(in0), 20.0f, 0.1f), "IN player's personal fuse drained to ~20s");
    require(near(eng.triggers().tips_slot_fuse(other), 25.0f, 0.05f), "runner's personal fuse untouched at 25s");

    // Light bumper tap (< 5.2m) between in0 and other -> swaps IN + grants 3.5s No-Tip-Backs immunity
    wf(kCars[other] + 0x88, rf(kCars[in0] + 0x88) + 3.0f);
    const uint32_t serial_before_swap = eng.round_serial();
    eng.tick(301);
    require(mutator_hooks::hud.tips_in_slot == other, "light tap swapped IN to other slot");
    require(mutator_hooks::slots[other].role == mutator::RoleId::Tagger &&
            mutator_hooks::slots[in0].role == mutator::RoleId::Survivor,
            "roles swapped cleanly on tap");
    require(mutator_hooks::slots[in0].score == 1, "former IN awarded +1 tag score");
    require(mutator_hooks::slot_tips_immune(in0), "former IN has 3.5s No-Tip-Backs immunity");
    require(eng.round_serial() == serial_before_swap + 1u, "Tips swap bumps round_serial");

    // Keep cars touching for 2 seconds (120 ticks) -> immunity prevents instant tag-back; in0 fuse stays paused!
    for (uint32_t t = 302; t <= 421; ++t) eng.tick(t);
    require(mutator_hooks::hud.tips_in_slot == other, "3.5s No-Tip-Backs immunity prevents instant tag-back");
    require(near(eng.triggers().tips_slot_fuse(in0), 20.0f, 0.15f), "former IN player's personal fuse paused on swap");
    require(eng.triggers().tips_slot_fuse(other) < 23.2f, "new IN player's personal fuse is draining");

    // Wire round-trip of K_MUTATOR_TRIGGER with tips_in_slot & tips_fuse_remaining
    const auto sync_pkt = eng.build_trigger_packet(421);
    netplay::Writer tw;
    mutator::put_mutator_triggers(tw, sync_pkt);
    netplay::Reader trd(tw.b.data(), tw.b.size());
    mutator::TriggerSyncPacket dec{};
    require(mutator::get_mutator_triggers(trd, dec) && trd.p == trd.e, "Tips K_MUTATOR_TRIGGER round-trip");
    require(dec.tips_in_slot == other && near(dec.tips_fuse_remaining, eng.triggers().tips_fuse_remaining(), 0.01f),
            "Tips K_MUTATOR_TRIGGER preserves tips_in_slot and tips_fuse_remaining");

    // Separate cars and drain remaining ~23s on `other` -> eliminates `other` at 0.0s & picks new surviving IN
    wf(kCars[other] + 0x88, 500.0f);
    for (uint32_t t = 422; t <= 1900; ++t) eng.tick(t);
    require((eng.triggers().eliminated_mask() & static_cast<uint16_t>(1u << other)) != 0u,
            "slot whose fuse hits 0s is marked eliminated");
    require(ee_r32(kCars[other] + 0x1BE8) >= 10u && near(rf(kCars[other] + 0x1BEC), 1.0f),
            "eliminated slot is wrecked (damage=1.0, stage=10)");
    require(mutator_hooks::hud.tips_alive_count == 3u, "alive count drops to 3 after elimination");
    require(mutator_hooks::hud.tips_in_slot < 4u && mutator_hooks::hud.tips_in_slot != other,
            "new random surviving IN selected after elimination");

    eng.clear_config();
}

void test_mode_weapon_suppression_policy() {
    // Pure driving/tag/chain modes suppress weapon launchers; Fiesta/BR/Infection/DestructionWare enable them.
    require(!mutator::preset_uses_weapons(mutator::PRESET_TIPS_TAG), "Tips (Tag) suppresses weapon launchers");
    require(!mutator::preset_uses_weapons(mutator::PRESET_CHAIN_GANG), "Chain Gang suppresses weapon launchers");
    require(mutator::preset_uses_weapons(mutator::PRESET_BATTLE_ROYALE), "Battle Royale enables weapon launchers");
    require(mutator::preset_uses_weapons(mutator::PRESET_INFECTION), "Infection enables weapon launchers");
    require(mutator::preset_uses_weapons(mutator::PRESET_DESTRUCTIONWARE), "DestructionWare enables weapon launchers");
    require(mutator::preset_uses_weapons(mutator::PRESET_FIESTA_ROULETTE), "Fiesta Roulette enables weapon launchers");

    auto &eng = mutator::engine();
    eng.set_config(mutator::make_tips_tag_preset());
    require(mutator_hooks::suppress_weapons, "Engine sets suppress_weapons for Tips (Tag)");
    eng.set_config(mutator::make_fiesta_roulette_preset());
    require(!mutator_hooks::suppress_weapons, "Engine enables weapons for Fiesta Roulette");
    eng.set_config(mutator::make_chain_gang_preset());
    require(mutator_hooks::suppress_weapons, "Engine sets suppress_weapons for Chain Gang");
    eng.clear_config();
    require(!mutator_hooks::suppress_weapons, "clear_config resets suppress_weapons");
}

void test_json_serialize_roundtrip() {
    require(mutator::slugify_gametype_id("My Crazy Tag! #2") == "my_crazy_tag_2", "slugify_gametype_id normalizes cleanly");
    require(mutator::slugify_gametype_id("   ") == "custom_mode", "slugify_gametype_id handles blank input");

    for (uint8_t pid = mutator::PRESET_BATTLE_ROYALE; pid <= mutator::PRESET_CHAIN_GANG; ++pid) {
        mutator::GametypeConfig orig;
        require(mutator::build_preset(pid, orig), "build_preset succeeds");
        const std::string json = mutator::serialize_gametype_json(orig);
        mutator::GametypeConfig parsed;
        std::string err;
        require(mutator::parse_gametype_json(json, parsed, err), "serialize -> parse roundtrip succeeds");
        require(parsed.id == orig.id, "roundtrip id matches");
        require(parsed.name == orig.name, "roundtrip name matches");
        require(parsed.base_event_type == orig.base_event_type, "roundtrip base_event_type matches");
        require(parsed.base_rule_flags == orig.base_rule_flags, "roundtrip base_rule_flags matches");
        require(parsed.base_feats == orig.base_feats, "roundtrip base_feats matches");
        require(parsed.base_traits == orig.base_traits, "roundtrip base_traits matches");
        require(parsed.roles.size() == orig.roles.size(), "roundtrip roles count matches");
        require(parsed.warp_zones.size() == orig.warp_zones.size(), "roundtrip warp_zones count matches");
        require(parsed.base_triggers.size() == orig.base_triggers.size(), "roundtrip base_triggers count matches");
        require(parsed.micro_rounds.size() == orig.micro_rounds.size(), "roundtrip micro_rounds count matches");
        require(near(parsed.tips_fuse_sec, orig.tips_fuse_sec), "roundtrip tips_fuse_sec matches");
        require(near(parsed.fiesta_interval_sec, orig.fiesta_interval_sec), "roundtrip fiesta_interval_sec matches");
    }
}

} // namespace

int main() {
    std::vector<uint8_t> ram(EE_RAM_SIZE, 0);
    g_ram = ram.data();
    test_json_and_wire_codec();
    test_vehicle_key_and_chassis_budget();
    test_track_anchored_zones();
    test_per_body_gravity_and_live_physics();
    test_atomic_warp_and_puppet_snap();
    test_micro_rounds_and_round_packet();
    test_presets_and_trigger_wire_codec();
    test_battle_royale_shrinking_storm();
    test_halo_infection_tag();
    test_ctf_flag_and_stop_and_go();
    test_chain_tether_and_trailer_detach();
    test_16_player_battle_layout();
    test_tips_hot_potato_tag();
    test_mode_weapon_suppression_policy();
    test_json_serialize_roundtrip();
    std::puts("mutator_engine_test passed");
    return 0;
}
