#pragma once
// Grounded Gametype & Mutator Schema (SLUS_209.10 + eod-recomp).
//
// All bitflags mirror verified retail constants:
//   - RuleFlags low/upper bits: Master Event Registry (0x003C3A70)
//   - FeatBits: g_ActiveCheatMask (0x003C4190) + host per-slot extensions
//   - Vehicle overrides: resolved by string key against kVehicleTable (0x003CBEE4)
//   - Spatial zones: Track-Anchored (arena_center / spline_pct) or raw world XYZ
#include "netplay_wire.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace mutator {

// Verified EE heap safety cap: retail 0x00123EE0 caps distinct chassis per Eve
// at 7 (src/battle_bots.cpp:302 uses 6 to leave headroom for trailers/specials).
// Tier 1 Fiesta swaps stay within loaded chassis; Tier 2 reloads between rounds.
constexpr uint32_t kMaxLoadedChassis = 6;
constexpr uint32_t kMaxSlots = 16;
constexpr uint32_t kMaxRoles = 8;
constexpr uint32_t kMaxWarpZones = 16;
constexpr uint32_t kMaxTriggersPerRound = 16;
constexpr uint32_t kMaxMicroRounds = 16;

// Grounded in SLUS_209.10 0x003C3A70 event_type bitflags + host extensions
enum RuleFlags : uint32_t {
    RF_NONE            = 0u,
    RF_TRAILER         = 0x00000800u, // Native TRLR hitch
    RF_CASKET_CARGO    = 0x00001000u, // Native egg/casket fragile load
    RF_CHAIN_TETHER    = 0x00008000u, // Host spring-damper chain pair
    RF_SUICIDE         = 0x00010000u, // Split-direction racing (direction_sign = +/-1)
    RF_FLAGPOLE        = 0x00020000u, // Dynamic flagpole turn-around
    RF_WHIP_AROUND     = 0x00040000u, // Alternating forward/reverse laps
    RF_LAST_MAN        = 0x00080000u, // Eliminate last-place per lap/round
    RF_STOP_AND_GO     = 0x00400000u, // Mandatory full-stop trigger box
    RF_BACKWARD        = 0x02000000u, // Reverse-gear only
    RF_PUSH_OFF        = 0x04000000u, // Host extension: ring-out elimination
    RF_INSTANT_RESPAWN = 0x08000000u, // Host extension: BattleRespawnCar (0x1675C0)
    RF_INFECTION_TAG   = 0x10000000u, // Host extension: contact converts role
    RF_FIESTA_ROULETTE = 0x20000000u, // Host extension: periodic trait/feat reroll
    RF_TIPS_TAG        = 0x40000000u, // Host extension: 1-player "IN" Hot Potato Tips (Tag)
};

// Grounded in SLUS_209.10 0x003C4190 g_ActiveCheatMask bits + host extensions
enum FeatBits : uint32_t {
    FB_NONE             = 0u,
    FB_TURBO            = 0x00000002u, // Turbo boost (L2, f_0018F2A8)
    FB_HARDCORE         = 0x00000004u, // Hardcore damage scaling
    FB_CHICKEN_STRAIGHT = 0x00000008u, // Straight Battle chicken shot
    FB_CHICKEN_MORTAR   = 0x00000018u, // Weapon master (0x08) | mortar arc (0x10)
    FB_AI_VENGEANCE     = 0x00000020u, // Aggressive AI ramming
    FB_DEADLY_AMBITION  = 0x00000040u, // 1st-place health drain (0x1B5BD0)
    FB_RAIN_CHICKENS    = 0x00000080u, // Overhead raining chickens
    FB_ONE_HIT_KILLS    = 0x00000100u, // Instant total on qualifying hit (f_0018CA98)
    FB_SEPIA            = 0x00000200u, // Vintage sepia post-filter
    FB_INVINCIBLE       = 0x00010000u, // Host per-slot gate on f_0018CA98 / f_0018AD20
};

struct PhysicsTraits {
    float gravity_scale        = 1.0f; // Multiplies 9.81f in vehicle_dynamics.cpp
    float mass_scale           = 1.0f; // Divides RigidBody::inverseInertia (0x494D30)
    float grip_scale           = 1.0f; // Scales Tuning::grip (0x494D38 + 0xA0)
    float torque_scale         = 1.0f; // Scales drive acceleration / boost impulse
    float top_speed_scale      = 1.0f; // Scales 44.444447f m/s cap (f_0018F2A8)
    float damage_taken_scale   = 1.0f; // Scales car+0x4B0 (CarSetupDamageAndPanels)
    float chicken_force_scale  = 1.0f; // Scales f_0017DA58 $f13 (default 25000 N)
    float chicken_radius_scale = 1.0f; // Scales f_0017DA58 $f12 (default 7.5 m)
    float chicken_reload_sec   = 3.0f; // Overrides f_0017E4D8 reload deadline

    void sanitize() {
        auto clamp_f = [](float v, float lo, float hi, float def) {
            return std::isfinite(v) ? std::clamp(v, lo, hi) : def;
        };
        gravity_scale        = clamp_f(gravity_scale, 0.10f, 4.00f, 1.0f);
        mass_scale           = clamp_f(mass_scale, 0.25f, 5.00f, 1.0f);
        grip_scale           = clamp_f(grip_scale, 0.20f, 3.00f, 1.0f);
        torque_scale         = clamp_f(torque_scale, 0.25f, 3.00f, 1.0f);
        top_speed_scale      = clamp_f(top_speed_scale, 0.40f, 3.00f, 1.0f);
        damage_taken_scale   = clamp_f(damage_taken_scale, 0.00f, 5.00f, 1.0f);
        chicken_force_scale  = clamp_f(chicken_force_scale, 0.25f, 8.00f, 1.0f);
        chicken_radius_scale = clamp_f(chicken_radius_scale, 0.25f, 4.00f, 1.0f);
        chicken_reload_sec   = clamp_f(chicken_reload_sec, 0.25f, 10.00f, 3.0f);
    }

    bool operator==(const PhysicsTraits &o) const {
        return gravity_scale == o.gravity_scale && mass_scale == o.mass_scale &&
               grip_scale == o.grip_scale && torque_scale == o.torque_scale &&
               top_speed_scale == o.top_speed_scale && damage_taken_scale == o.damage_taken_scale &&
               chicken_force_scale == o.chicken_force_scale &&
               chicken_radius_scale == o.chicken_radius_scale &&
               chicken_reload_sec == o.chicken_reload_sec;
    }
};

enum class RoleId : uint8_t { Default = 0, Survivor = 1, Infected = 2, Juggernaut = 3, VIP = 4, Bus = 5, Tagger = 6 };

struct RoleOverride {
    RoleId role = RoleId::Default;
    // Resolved dynamically against kVehicleTable (0x003CBEE4) chassis key string
    // (e.g. "bus", "hearse", "gladiator"). Empty = keep player/random choice.
    std::string forced_vehicle_key;
    int8_t direction_sign = 1; // +1 forward, -1 reversed (wired into hle_recovery.cpp)
    uint32_t feat_bits = 0;
    PhysicsTraits traits{};
    bool repair_on_assign = true;
};

enum class AnchorKind : uint8_t { WorldXYZ = 0, ArenaCenter = 1, SplinePct = 2 };
enum class TriggerKind : uint8_t { Sphere = 0, Cylinder = 1, OBB = 2, RingOut = 3, Flag = 4 };

struct TriggerDef {
    uint16_t id = 0;
    TriggerKind kind = TriggerKind::Cylinder;
    AnchorKind anchor = AnchorKind::ArenaCenter;
    float origin[3] = {0.0f, 0.0f, 0.0f};   // World XYZ when WorldXYZ, or local offset
    float spline_pct = 0.0f;                // [0, 1) along g_PrimaryCourseCache (0x003CD190)
    float extents[3] = {15.0f, 5.0f, 15.0f};
    float yaw_rad = 0.0f;
    float dwell_sec = 0.0f;
    float shrink_rate_mps = 0.0f;           // Battle Royale storm shrink rate (m/s), 0 = static
    float min_radius_m = 8.0f;              // Minimum radius when shrinking
    float dps_outside = 0.0f;               // Damage fraction/sec outside RingOut (0 = dwell ring-out)
    uint32_t color_rgba = 0xFF33AAFFu;
};

struct WarpZone {
    std::string name;
    AnchorKind anchor = AnchorKind::ArenaCenter;
    float origin[3] = {0.0f, 0.0f, 0.0f};
    float spline_pct = 0.0f;
    float radius_m = 28.0f;
    bool face_center = true;
};

struct MicroRoundDef {
    std::string title;
    float duration_sec = 30.0f;
    uint32_t rule_flags = RF_NONE;
    uint32_t global_feats = FB_NONE;
    PhysicsTraits traits{};
    bool reroll_hitch = false;
    bool repair_all = true;
    int16_t pa_speech_sid = -1; // SPEECH.BNK SID for f_00266AD8
    std::string sfx_cue;        // e.g. "arena_horn.sfx" for f_00238630
    std::string warp_zone;      // Optional WarpZone::name
    std::vector<TriggerDef> triggers;
};

struct GametypeConfig {
    std::string id;
    std::string name;
    uint32_t base_event_type = 0x18000224u; // Default: retail Battle
    std::string default_track = "Anoka";
    uint32_t base_rule_flags = RF_NONE;
    uint32_t base_feats = FB_NONE;
    PhysicsTraits base_traits{};
    std::vector<RoleOverride> roles;
    std::vector<WarpZone> warp_zones;
    std::vector<TriggerDef> base_triggers;
    std::vector<MicroRoundDef> micro_rounds;
    bool shuffle_rounds = false;
    uint8_t rounds_to_win = 5;
    float fiesta_interval_sec = 20.0f; // Trait reroll cadence when RF_FIESTA_ROULETTE is active
    float tips_fuse_sec = 25.0f;       // Personal Hot Potato fuse (seconds) when RF_TIPS_TAG is active

    static void sanitize_trigger(TriggerDef &t) {
        t.spline_pct = std::isfinite(t.spline_pct) ? (t.spline_pct - std::floor(t.spline_pct)) : 0.0f;
        t.dwell_sec = std::isfinite(t.dwell_sec) ? std::clamp(t.dwell_sec, 0.0f, 30.0f) : 0.0f;
        t.shrink_rate_mps = std::isfinite(t.shrink_rate_mps) ? std::clamp(t.shrink_rate_mps, 0.0f, 50.0f) : 0.0f;
        t.min_radius_m = std::isfinite(t.min_radius_m) ? std::clamp(t.min_radius_m, 2.0f, 200.0f) : 8.0f;
        t.dps_outside = std::isfinite(t.dps_outside) ? std::clamp(t.dps_outside, 0.0f, 2.0f) : 0.0f;
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(t.origin[i])) t.origin[i] = 0.0f;
            if (!std::isfinite(t.extents[i]) || t.extents[i] <= 0.0f) t.extents[i] = 15.0f;
        }
        if (!std::isfinite(t.yaw_rad)) t.yaw_rad = 0.0f;
    }

    void sanitize() {
        base_traits.sanitize();
        if (roles.size() > kMaxRoles) roles.resize(kMaxRoles);
        for (auto &r : roles) {
            r.direction_sign = r.direction_sign < 0 ? -1 : 1;
            r.traits.sanitize();
        }
        if (warp_zones.size() > kMaxWarpZones) warp_zones.resize(kMaxWarpZones);
        for (auto &z : warp_zones) {
            z.spline_pct = std::isfinite(z.spline_pct) ? (z.spline_pct - std::floor(z.spline_pct)) : 0.0f;
            z.radius_m = std::isfinite(z.radius_m) ? std::clamp(z.radius_m, 4.0f, 120.0f) : 28.0f;
        }
        if (base_triggers.size() > kMaxTriggersPerRound) base_triggers.resize(kMaxTriggersPerRound);
        for (auto &t : base_triggers) sanitize_trigger(t);
        if (micro_rounds.size() > kMaxMicroRounds) micro_rounds.resize(kMaxMicroRounds);
        for (auto &mr : micro_rounds) {
            mr.duration_sec = std::isfinite(mr.duration_sec) ? std::clamp(mr.duration_sec, 5.0f, 300.0f) : 30.0f;
            mr.traits.sanitize();
            if (mr.triggers.size() > kMaxTriggersPerRound) mr.triggers.resize(kMaxTriggersPerRound);
            for (auto &t : mr.triggers) sanitize_trigger(t);
        }
        rounds_to_win = std::clamp<uint8_t>(rounds_to_win, 1, 20);
        fiesta_interval_sec = std::isfinite(fiesta_interval_sec) ? std::clamp(fiesta_interval_sec, 5.0f, 120.0f) : 20.0f;
        tips_fuse_sec = std::isfinite(tips_fuse_sec) ? std::clamp(tips_fuse_sec, 5.0f, 120.0f) : 25.0f;
    }
};

// ---------------------------------------------------------------------------
// String <-> Enum / Bitmask helpers
// ---------------------------------------------------------------------------
inline std::string to_lower_ascii(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline uint32_t parse_rule_flag_name(const std::string &raw) {
    const std::string s = to_lower_ascii(raw);
    if (s == "rf_trailer" || s == "trailer") return RF_TRAILER;
    if (s == "rf_casket_cargo" || s == "casket_cargo" || s == "casket") return RF_CASKET_CARGO;
    if (s == "rf_chain_tether" || s == "chain_tether" || s == "chain") return RF_CHAIN_TETHER;
    if (s == "rf_suicide" || s == "suicide") return RF_SUICIDE;
    if (s == "rf_flagpole" || s == "flagpole") return RF_FLAGPOLE;
    if (s == "rf_whip_around" || s == "whip_around") return RF_WHIP_AROUND;
    if (s == "rf_last_man" || s == "last_man") return RF_LAST_MAN;
    if (s == "rf_stop_and_go" || s == "stop_and_go") return RF_STOP_AND_GO;
    if (s == "rf_backward" || s == "backward") return RF_BACKWARD;
    if (s == "rf_push_off" || s == "push_off") return RF_PUSH_OFF;
    if (s == "rf_instant_respawn" || s == "instant_respawn") return RF_INSTANT_RESPAWN;
    if (s == "rf_infection_tag" || s == "infection_tag" || s == "infection") return RF_INFECTION_TAG;
    if (s == "rf_fiesta_roulette" || s == "fiesta_roulette" || s == "fiesta") return RF_FIESTA_ROULETTE;
    if (s == "rf_tips_tag" || s == "tips_tag" || s == "tips" || s == "tag") return RF_TIPS_TAG;
    return 0u;
}

inline uint32_t parse_feat_bit_name(const std::string &raw) {
    const std::string s = to_lower_ascii(raw);
    if (s == "fb_turbo" || s == "turbo") return FB_TURBO;
    if (s == "fb_hardcore" || s == "hardcore") return FB_HARDCORE;
    if (s == "fb_chicken_straight" || s == "chicken_straight" || s == "chickens") return FB_CHICKEN_STRAIGHT;
    if (s == "fb_chicken_mortar" || s == "chicken_mortar" || s == "mortar") return FB_CHICKEN_MORTAR;
    if (s == "fb_ai_vengeance" || s == "ai_vengeance") return FB_AI_VENGEANCE;
    if (s == "fb_deadly_ambition" || s == "deadly_ambition") return FB_DEADLY_AMBITION;
    if (s == "fb_rain_chickens" || s == "rain_chickens" || s == "raining_chickens") return FB_RAIN_CHICKENS;
    if (s == "fb_one_hit_kills" || s == "one_hit_kills") return FB_ONE_HIT_KILLS;
    if (s == "fb_sepia" || s == "sepia") return FB_SEPIA;
    if (s == "fb_invincible" || s == "invincible") return FB_INVINCIBLE;
    return 0u;
}

inline RoleId parse_role_id(const std::string &raw) {
    const std::string s = to_lower_ascii(raw);
    if (s == "survivor") return RoleId::Survivor;
    if (s == "infected") return RoleId::Infected;
    if (s == "juggernaut") return RoleId::Juggernaut;
    if (s == "vip") return RoleId::VIP;
    if (s == "bus") return RoleId::Bus;
    if (s == "tagger" || s == "in" || s == "it") return RoleId::Tagger;
    return RoleId::Default;
}

inline AnchorKind parse_anchor_kind(const std::string &raw) {
    const std::string s = to_lower_ascii(raw);
    if (s == "arena_center" || s == "arenacenter") return AnchorKind::ArenaCenter;
    if (s == "spline_pct" || s == "splinepct" || s == "spline") return AnchorKind::SplinePct;
    return AnchorKind::WorldXYZ;
}

inline TriggerKind parse_trigger_kind(const std::string &raw) {
    const std::string s = to_lower_ascii(raw);
    if (s == "sphere") return TriggerKind::Sphere;
    if (s == "obb" || s == "box") return TriggerKind::OBB;
    if (s == "ringout" || s == "ring_out") return TriggerKind::RingOut;
    if (s == "flag") return TriggerKind::Flag;
    return TriggerKind::Cylinder;
}

// ---------------------------------------------------------------------------
// Wire Serialization (netplay::Writer / netplay::Reader, big-endian, bounded)
// ---------------------------------------------------------------------------
inline void put_physics_traits(netplay::Writer &w, const PhysicsTraits &t) {
    w.f32(t.gravity_scale);
    w.f32(t.mass_scale);
    w.f32(t.grip_scale);
    w.f32(t.torque_scale);
    w.f32(t.top_speed_scale);
    w.f32(t.damage_taken_scale);
    w.f32(t.chicken_force_scale);
    w.f32(t.chicken_radius_scale);
    w.f32(t.chicken_reload_sec);
}

inline bool get_physics_traits(netplay::Reader &r, PhysicsTraits &t) {
    t.gravity_scale = r.f32();
    t.mass_scale = r.f32();
    t.grip_scale = r.f32();
    t.torque_scale = r.f32();
    t.top_speed_scale = r.f32();
    t.damage_taken_scale = r.f32();
    t.chicken_force_scale = r.f32();
    t.chicken_radius_scale = r.f32();
    t.chicken_reload_sec = r.f32();
    if (!r.ok) return false;
    auto finite_pos = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    return finite_pos(t.gravity_scale, 0.10f, 4.00f) &&
           finite_pos(t.mass_scale, 0.25f, 5.00f) &&
           finite_pos(t.grip_scale, 0.20f, 3.00f) &&
           finite_pos(t.torque_scale, 0.25f, 3.00f) &&
           finite_pos(t.top_speed_scale, 0.40f, 3.00f) &&
           finite_pos(t.damage_taken_scale, 0.00f, 5.00f) &&
           finite_pos(t.chicken_force_scale, 0.25f, 8.00f) &&
           finite_pos(t.chicken_radius_scale, 0.25f, 4.00f) &&
           finite_pos(t.chicken_reload_sec, 0.25f, 10.00f);
}

inline void put_trigger_def(netplay::Writer &w, const TriggerDef &t) {
    w.u16(t.id);
    w.u8(static_cast<uint8_t>(t.kind));
    w.u8(static_cast<uint8_t>(t.anchor));
    for (int i = 0; i < 3; ++i) w.f32(t.origin[i]);
    w.f32(t.spline_pct);
    for (int i = 0; i < 3; ++i) w.f32(t.extents[i]);
    w.f32(t.yaw_rad);
    w.f32(t.dwell_sec);
    w.f32(t.shrink_rate_mps);
    w.f32(t.min_radius_m);
    w.f32(t.dps_outside);
    w.u32(t.color_rgba);
}

inline bool get_trigger_def(netplay::Reader &r, TriggerDef &t) {
    t.id = static_cast<uint16_t>(r.u16());
    const uint32_t tk = r.u8(), ta = r.u8();
    if (tk > static_cast<uint32_t>(TriggerKind::Flag) ||
        ta > static_cast<uint32_t>(AnchorKind::SplinePct))
        return false;
    t.kind = static_cast<TriggerKind>(tk);
    t.anchor = static_cast<AnchorKind>(ta);
    for (int k = 0; k < 3; ++k) t.origin[k] = r.f32();
    t.spline_pct = r.f32();
    for (int k = 0; k < 3; ++k) t.extents[k] = r.f32();
    t.yaw_rad = r.f32();
    t.dwell_sec = r.f32();
    t.shrink_rate_mps = r.f32();
    t.min_radius_m = r.f32();
    t.dps_outside = r.f32();
    t.color_rgba = r.u32();
    return r.ok &&
           std::isfinite(t.origin[0]) && std::isfinite(t.origin[1]) && std::isfinite(t.origin[2]) &&
           std::isfinite(t.extents[0]) && std::isfinite(t.extents[1]) && std::isfinite(t.extents[2]) &&
           std::isfinite(t.spline_pct) && std::isfinite(t.yaw_rad) && std::isfinite(t.dwell_sec) &&
           std::isfinite(t.shrink_rate_mps) && std::isfinite(t.min_radius_m) && std::isfinite(t.dps_outside);
}

inline void put_mutator_config(netplay::Writer &w, const GametypeConfig &cfg) {
    GametypeConfig c = cfg;
    c.sanitize();
    w.str(c.id, 32);
    w.str(c.name, 48);
    w.u32(c.base_event_type);
    w.str(c.default_track, 32);
    w.u32(c.base_rule_flags);
    w.u32(c.base_feats);
    put_physics_traits(w, c.base_traits);
    w.u8(c.shuffle_rounds ? 1u : 0u);
    w.u8(c.rounds_to_win);
    w.f32(c.fiesta_interval_sec);
    w.f32(c.tips_fuse_sec);

    w.u8(static_cast<uint32_t>(c.roles.size()));
    for (const auto &role : c.roles) {
        w.u8(static_cast<uint8_t>(role.role));
        w.str(role.forced_vehicle_key, 32);
        w.u8(role.direction_sign < 0 ? 0u : 1u);
        w.u32(role.feat_bits);
        put_physics_traits(w, role.traits);
        w.u8(role.repair_on_assign ? 1u : 0u);
    }

    w.u8(static_cast<uint32_t>(c.warp_zones.size()));
    for (const auto &z : c.warp_zones) {
        w.str(z.name, 32);
        w.u8(static_cast<uint8_t>(z.anchor));
        for (int i = 0; i < 3; ++i) w.f32(z.origin[i]);
        w.f32(z.spline_pct);
        w.f32(z.radius_m);
        w.u8(z.face_center ? 1u : 0u);
    }

    w.u8(static_cast<uint32_t>(c.base_triggers.size()));
    for (const auto &t : c.base_triggers) put_trigger_def(w, t);

    w.u8(static_cast<uint32_t>(c.micro_rounds.size()));
    for (const auto &mr : c.micro_rounds) {
        w.str(mr.title, 64);
        w.f32(mr.duration_sec);
        w.u32(mr.rule_flags);
        w.u32(mr.global_feats);
        put_physics_traits(w, mr.traits);
        w.u8((mr.reroll_hitch ? 1u : 0u) | (mr.repair_all ? 2u : 0u));
        w.u16(static_cast<uint16_t>(mr.pa_speech_sid));
        w.str(mr.sfx_cue, 32);
        w.str(mr.warp_zone, 32);
        w.u8(static_cast<uint32_t>(mr.triggers.size()));
        for (const auto &t : mr.triggers) put_trigger_def(w, t);
    }
}

inline bool get_mutator_config(netplay::Reader &r, GametypeConfig &out) {
    GametypeConfig c;
    c.id = r.str();
    c.name = r.str();
    c.base_event_type = r.u32();
    c.default_track = r.str();
    c.base_rule_flags = r.u32();
    c.base_feats = r.u32();
    if (!get_physics_traits(r, c.base_traits)) return false;
    const uint32_t shuf = r.u8();
    if (shuf > 1) return false;
    c.shuffle_rounds = (shuf != 0);
    c.rounds_to_win = static_cast<uint8_t>(r.u8());
    c.fiesta_interval_sec = r.f32();
    c.tips_fuse_sec = r.f32();
    if (!r.ok || c.rounds_to_win == 0 || c.rounds_to_win > 20 ||
        !std::isfinite(c.fiesta_interval_sec) || c.fiesta_interval_sec < 5.0f || c.fiesta_interval_sec > 120.0f ||
        !std::isfinite(c.tips_fuse_sec) || c.tips_fuse_sec < 5.0f || c.tips_fuse_sec > 120.0f)
        return false;

    const uint32_t role_count = r.u8();
    if (role_count > kMaxRoles) return false;
    c.roles.reserve(role_count);
    for (uint32_t i = 0; i < role_count && r.ok; ++i) {
        RoleOverride role;
        const uint32_t rid = r.u8();
        if (rid > static_cast<uint32_t>(RoleId::Tagger)) return false;
        role.role = static_cast<RoleId>(rid);
        role.forced_vehicle_key = r.str();
        const uint32_t dir = r.u8();
        if (dir > 1) return false;
        role.direction_sign = dir ? 1 : -1;
        role.feat_bits = r.u32();
        if (!get_physics_traits(r, role.traits)) return false;
        const uint32_t rep = r.u8();
        if (rep > 1) return false;
        role.repair_on_assign = (rep != 0);
        c.roles.push_back(std::move(role));
    }

    const uint32_t wz_count = r.u8();
    if (wz_count > kMaxWarpZones) return false;
    c.warp_zones.reserve(wz_count);
    for (uint32_t i = 0; i < wz_count && r.ok; ++i) {
        WarpZone z;
        z.name = r.str();
        const uint32_t anc = r.u8();
        if (anc > static_cast<uint32_t>(AnchorKind::SplinePct)) return false;
        z.anchor = static_cast<AnchorKind>(anc);
        for (int k = 0; k < 3; ++k) z.origin[k] = r.f32();
        z.spline_pct = r.f32();
        z.radius_m = r.f32();
        const uint32_t fc = r.u8();
        if (fc > 1 || !std::isfinite(z.origin[0]) || !std::isfinite(z.origin[1]) || !std::isfinite(z.origin[2]) ||
            !std::isfinite(z.spline_pct) || !std::isfinite(z.radius_m))
            return false;
        z.face_center = (fc != 0);
        c.warp_zones.push_back(std::move(z));
    }

    const uint32_t bt_count = r.u8();
    if (bt_count > kMaxTriggersPerRound) return false;
    c.base_triggers.reserve(bt_count);
    for (uint32_t j = 0; j < bt_count && r.ok; ++j) {
        TriggerDef t;
        if (!get_trigger_def(r, t)) return false;
        c.base_triggers.push_back(t);
    }

    const uint32_t mr_count = r.u8();
    if (mr_count > kMaxMicroRounds) return false;
    c.micro_rounds.reserve(mr_count);
    for (uint32_t i = 0; i < mr_count && r.ok; ++i) {
        MicroRoundDef mr;
        mr.title = r.str();
        mr.duration_sec = r.f32();
        mr.rule_flags = r.u32();
        mr.global_feats = r.u32();
        if (!get_physics_traits(r, mr.traits)) return false;
        const uint32_t flags = r.u8();
        if (flags > 3) return false;
        mr.reroll_hitch = (flags & 1u) != 0;
        mr.repair_all = (flags & 2u) != 0;
        mr.pa_speech_sid = static_cast<int16_t>(r.u16());
        mr.sfx_cue = r.str();
        mr.warp_zone = r.str();
        const uint32_t tc = r.u8();
        if (tc > kMaxTriggersPerRound || !std::isfinite(mr.duration_sec) ||
            mr.duration_sec < 5.0f || mr.duration_sec > 300.0f)
            return false;
        mr.triggers.reserve(tc);
        for (uint32_t j = 0; j < tc && r.ok; ++j) {
            TriggerDef t;
            if (!get_trigger_def(r, t)) return false;
            mr.triggers.push_back(t);
        }
        c.micro_rounds.push_back(std::move(mr));
    }

    if (!r.ok) return false;
    c.sanitize();
    out = std::move(c);
    return true;
}

// Replicated micro-round & role state transition (K_MUTATOR_ROUND, host -> guests)
struct RoundPacket {
    uint32_t round_serial = 0;      // Monotonic 1, 2, 3...
    uint16_t round_def_index = 0;   // Index into GametypeConfig::micro_rounds
    uint8_t warp_gen = 0;           // Synchronized warp generation counter
    uint32_t start_tick = 0;        // Host sim tick at round entry
    uint32_t duration_ticks = 0;    // 0 = untimed continuous round
    uint32_t rule_flags = 0;        // Active RuleFlags
    uint32_t global_feats = 0;      // Active FeatBits
    PhysicsTraits traits{};         // Active round PhysicsTraits
    uint8_t slot_count = 0;
    std::array<RoleId, kMaxSlots> slot_roles{};
    std::array<int8_t, kMaxSlots> slot_direction{};
    std::array<int32_t, kMaxSlots> slot_scores{};
};

inline void put_mutator_round(netplay::Writer &w, const RoundPacket &p) {
    w.u32(p.round_serial);
    w.u16(p.round_def_index);
    w.u8(p.warp_gen);
    w.u32(p.start_tick);
    w.u32(p.duration_ticks);
    w.u32(p.rule_flags);
    w.u32(p.global_feats);
    PhysicsTraits t = p.traits;
    t.sanitize();
    put_physics_traits(w, t);
    const uint8_t count = std::min<uint8_t>(p.slot_count, static_cast<uint8_t>(kMaxSlots));
    w.u8(count);
    for (uint8_t i = 0; i < count; ++i) {
        w.u8(static_cast<uint8_t>(p.slot_roles[i]));
        w.u8(p.slot_direction[i] < 0 ? 0u : 1u);
        w.u32(static_cast<uint32_t>(p.slot_scores[i]));
    }
}

inline bool get_mutator_round(netplay::Reader &r, RoundPacket &out) {
    RoundPacket p;
    p.round_serial = r.u32();
    p.round_def_index = static_cast<uint16_t>(r.u16());
    p.warp_gen = static_cast<uint8_t>(r.u8());
    p.start_tick = r.u32();
    p.duration_ticks = r.u32();
    p.rule_flags = r.u32();
    p.global_feats = r.u32();
    if (!get_physics_traits(r, p.traits)) return false;
    const uint32_t count = r.u8();
    if (!r.ok || !p.round_serial || count > kMaxSlots) return false;
    p.slot_count = static_cast<uint8_t>(count);
    for (uint32_t i = 0; i < count && r.ok; ++i) {
        const uint32_t rid = r.u8();
        const uint32_t dir = r.u8();
        const int32_t sc = static_cast<int32_t>(r.u32());
        if (rid > static_cast<uint32_t>(RoleId::Tagger) || dir > 1u) return false;
        p.slot_roles[i] = static_cast<RoleId>(rid);
        p.slot_direction[i] = dir ? 1 : -1;
        p.slot_scores[i] = sc;
    }
    if (!r.ok) return false;
    out = p;
    return true;
}

// Replicated live trigger zones, storm radii, flag carriers & chain tethers (K_MUTATOR_TRIGGER)
struct ActiveTriggerState {
    uint16_t id = 0;
    TriggerKind kind = TriggerKind::Cylinder;
    float center[3] = {0.0f, 0.0f, 0.0f};
    float radius = 15.0f;
    float extents[3] = {15.0f, 5.0f, 15.0f};
    float yaw_rad = 0.0f;
    uint8_t carrier_slot = 0xFF; // 0xFF = dropped / no carrier
    uint32_t color_rgba = 0xFF33AAFFu;
    bool active = true;
};

struct ChainPair {
    uint8_t slot_a = 0;
    uint8_t slot_b = 1;
    float rest_len_m = 11.0f;
    float stiffness = 18000.0f;
    float damping = 1800.0f;
};

constexpr uint32_t kMaxChainPairs = 8;

struct TriggerSyncPacket {
    uint32_t sim_tick = 0;
    uint16_t hitch_mask = 0;
    uint16_t eliminated_mask = 0;
    uint8_t tips_in_slot = 0xFF;     // Current "IN" tagger slot (0xFF = none)
    float tips_fuse_remaining = 0.0f; // Seconds remaining on current "IN" player's fuse
    std::vector<ActiveTriggerState> triggers;
    std::vector<ChainPair> chains;
};

inline void put_mutator_triggers(netplay::Writer &w, const TriggerSyncPacket &p) {
    w.u32(p.sim_tick);
    w.u16(p.hitch_mask);
    w.u16(p.eliminated_mask);
    w.u8(p.tips_in_slot);
    w.f32(std::isfinite(p.tips_fuse_remaining) ? std::max(0.0f, p.tips_fuse_remaining) : 0.0f);
    const size_t tc = std::min<size_t>(p.triggers.size(), kMaxTriggersPerRound);
    w.u8(static_cast<uint32_t>(tc));
    for (size_t i = 0; i < tc; ++i) {
        const auto &t = p.triggers[i];
        w.u16(t.id);
        w.u8(static_cast<uint8_t>(t.kind));
        w.u8(t.active ? 1u : 0u);
        for (int k = 0; k < 3; ++k) w.f32(t.center[k]);
        w.f32(t.radius);
        for (int k = 0; k < 3; ++k) w.f32(t.extents[k]);
        w.f32(t.yaw_rad);
        w.u8(t.carrier_slot);
        w.u32(t.color_rgba);
    }
    const size_t cc = std::min<size_t>(p.chains.size(), kMaxChainPairs);
    w.u8(static_cast<uint32_t>(cc));
    for (size_t i = 0; i < cc; ++i) {
        const auto &ch = p.chains[i];
        w.u8(ch.slot_a);
        w.u8(ch.slot_b);
        w.f32(ch.rest_len_m);
        w.f32(ch.stiffness);
        w.f32(ch.damping);
    }
}

inline bool get_mutator_triggers(netplay::Reader &r, TriggerSyncPacket &out) {
    TriggerSyncPacket p;
    p.sim_tick = r.u32();
    p.hitch_mask = static_cast<uint16_t>(r.u16());
    p.eliminated_mask = static_cast<uint16_t>(r.u16());
    p.tips_in_slot = static_cast<uint8_t>(r.u8());
    p.tips_fuse_remaining = r.f32();
    const uint32_t tc = r.u8();
    if (!r.ok || tc > kMaxTriggersPerRound ||
        (p.tips_in_slot != 0xFF && p.tips_in_slot >= kMaxSlots) ||
        !std::isfinite(p.tips_fuse_remaining) || p.tips_fuse_remaining < 0.0f || p.tips_fuse_remaining > 300.0f)
        return false;
    p.triggers.reserve(tc);
    for (uint32_t i = 0; i < tc && r.ok; ++i) {
        ActiveTriggerState t;
        t.id = static_cast<uint16_t>(r.u16());
        const uint32_t tk = r.u8();
        const uint32_t act = r.u8();
        if (tk > static_cast<uint32_t>(TriggerKind::Flag) || act > 1u) return false;
        t.kind = static_cast<TriggerKind>(tk);
        t.active = (act != 0);
        for (int k = 0; k < 3; ++k) t.center[k] = r.f32();
        t.radius = r.f32();
        for (int k = 0; k < 3; ++k) t.extents[k] = r.f32();
        t.yaw_rad = r.f32();
        t.carrier_slot = static_cast<uint8_t>(r.u8());
        t.color_rgba = r.u32();
        if (!std::isfinite(t.center[0]) || !std::isfinite(t.center[1]) || !std::isfinite(t.center[2]) ||
            !std::isfinite(t.radius) || t.radius < 0.0f || t.radius > 5000.0f ||
            !std::isfinite(t.extents[0]) || !std::isfinite(t.extents[1]) || !std::isfinite(t.extents[2]) ||
            !std::isfinite(t.yaw_rad) || (t.carrier_slot != 0xFF && t.carrier_slot >= kMaxSlots))
            return false;
        p.triggers.push_back(t);
    }
    const uint32_t cc = r.u8();
    if (!r.ok || cc > kMaxChainPairs) return false;
    p.chains.reserve(cc);
    for (uint32_t i = 0; i < cc && r.ok; ++i) {
        ChainPair ch;
        ch.slot_a = static_cast<uint8_t>(r.u8());
        ch.slot_b = static_cast<uint8_t>(r.u8());
        ch.rest_len_m = r.f32();
        ch.stiffness = r.f32();
        ch.damping = r.f32();
        if (ch.slot_a >= kMaxSlots || ch.slot_b >= kMaxSlots || ch.slot_a == ch.slot_b ||
            !std::isfinite(ch.rest_len_m) || ch.rest_len_m < 1.0f || ch.rest_len_m > 200.0f ||
            !std::isfinite(ch.stiffness) || ch.stiffness < 0.0f || ch.stiffness > 500000.0f ||
            !std::isfinite(ch.damping) || ch.damping < 0.0f || ch.damping > 100000.0f)
            return false;
        p.chains.push_back(ch);
    }
    if (!r.ok) return false;
    out = std::move(p);
    return true;
}

// ---------------------------------------------------------------------------
// Self-contained JSON Parser (zero third-party dependencies)
// ---------------------------------------------------------------------------
namespace detail {

struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    const JsonValue *field(const char *key) const {
        if (type != Object) return nullptr;
        for (const auto &kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string &src) : s_(src) {}

    bool parse(JsonValue &out, std::string &err) {
        skip_ws();
        if (!parse_value(out, err, 0)) return false;
        skip_ws();
        if (pos_ != s_.size()) {
            err = "Unexpected trailing characters at offset " + std::to_string(pos_);
            return false;
        }
        return true;
    }

private:
    const std::string &s_;
    size_t pos_ = 0;

    void skip_ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n'))
            ++pos_;
    }

    bool parse_value(JsonValue &out, std::string &err, int depth) {
        if (depth > 32) { err = "JSON nesting too deep"; return false; }
        skip_ws();
        if (pos_ >= s_.size()) { err = "Unexpected end of JSON"; return false; }
        const char c = s_[pos_];
        if (c == '{') return parse_object(out, err, depth + 1);
        if (c == '[') return parse_array(out, err, depth + 1);
        if (c == '"') { out.type = JsonValue::String; return parse_string(out.str, err); }
        if (c == 't' && s_.compare(pos_, 4, "true") == 0) { pos_ += 4; out.type = JsonValue::Bool; out.b = true; return true; }
        if (c == 'f' && s_.compare(pos_, 5, "false") == 0) { pos_ += 5; out.type = JsonValue::Bool; out.b = false; return true; }
        if (c == 'n' && s_.compare(pos_, 4, "null") == 0) { pos_ += 4; out.type = JsonValue::Null; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out, err);
        err = std::string("Unexpected character '") + c + "' at offset " + std::to_string(pos_);
        return false;
    }

    bool parse_string(std::string &out, std::string &err) {
        if (pos_ >= s_.size() || s_[pos_] != '"') { err = "Expected '\"'"; return false; }
        ++pos_;
        out.clear();
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= s_.size()) { err = "Unterminated escape"; return false; }
                const char e = s_[pos_++];
                switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: out.push_back(e); break;
                }
            } else {
                out.push_back(c);
            }
        }
        err = "Unterminated string";
        return false;
    }

    bool parse_number(JsonValue &out, std::string &err) {
        const char *start = s_.c_str() + pos_;
        char *end = nullptr;
        const double v = std::strtod(start, &end);
        if (end == start || !std::isfinite(v)) {
            err = "Invalid number at offset " + std::to_string(pos_);
            return false;
        }
        pos_ = static_cast<size_t>(end - s_.c_str());
        out.type = JsonValue::Number;
        out.num = v;
        return true;
    }

    bool parse_array(JsonValue &out, std::string &err, int depth) {
        ++pos_; // '['
        out.type = JsonValue::Array;
        out.arr.clear();
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
        while (pos_ < s_.size()) {
            JsonValue elem;
            if (!parse_value(elem, err, depth)) return false;
            out.arr.push_back(std::move(elem));
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
            err = "Expected ',' or ']' at offset " + std::to_string(pos_);
            return false;
        }
        err = "Unterminated array";
        return false;
    }

    bool parse_object(JsonValue &out, std::string &err, int depth) {
        ++pos_; // '{'
        out.type = JsonValue::Object;
        out.obj.clear();
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
        while (pos_ < s_.size()) {
            skip_ws();
            std::string key;
            if (!parse_string(key, err)) return false;
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != ':') { err = "Expected ':' after key '" + key + "'"; return false; }
            ++pos_;
            JsonValue val;
            if (!parse_value(val, err, depth)) return false;
            out.obj.emplace_back(std::move(key), std::move(val));
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
            err = "Expected ',' or '}' at offset " + std::to_string(pos_);
            return false;
        }
        err = "Unterminated object";
        return false;
    }
};

inline uint32_t parse_u32_field(const JsonValue *v, uint32_t fallback) {
    if (!v) return fallback;
    if (v->type == JsonValue::Number) return static_cast<uint32_t>(v->num);
    if (v->type == JsonValue::String && !v->str.empty()) {
        char *end = nullptr;
        const unsigned long parsed = std::strtoul(v->str.c_str(), &end, 0);
        if (end && *end == '\0') return static_cast<uint32_t>(parsed);
    }
    return fallback;
}

inline uint32_t parse_flag_array(const JsonValue *v, uint32_t (*lookup)(const std::string &)) {
    if (!v) return 0u;
    if (v->type == JsonValue::Number || v->type == JsonValue::String)
        return v->type == JsonValue::String ? (lookup(v->str) ? lookup(v->str) : parse_u32_field(v, 0u)) : static_cast<uint32_t>(v->num);
    if (v->type != JsonValue::Array) return 0u;
    uint32_t mask = 0u;
    for (const auto &el : v->arr) {
        if (el.type == JsonValue::String) mask |= lookup(el.str);
        else if (el.type == JsonValue::Number) mask |= static_cast<uint32_t>(el.num);
    }
    return mask;
}

inline void parse_vec3(const JsonValue *v, float out[3]) {
    if (!v || v->type != JsonValue::Array || v->arr.size() < 3) return;
    for (int i = 0; i < 3; ++i)
        if (v->arr[i].type == JsonValue::Number) out[i] = static_cast<float>(v->arr[i].num);
}

inline void parse_traits(const JsonValue *v, PhysicsTraits &t) {
    if (!v || v->type != JsonValue::Object) return;
    auto rd = [&](const char *k, float &dst) {
        if (const auto *f = v->field(k); f && f->type == JsonValue::Number) dst = static_cast<float>(f->num);
    };
    rd("gravity_scale", t.gravity_scale);
    rd("mass_scale", t.mass_scale);
    rd("grip_scale", t.grip_scale);
    rd("torque_scale", t.torque_scale);
    rd("top_speed_scale", t.top_speed_scale);
    rd("damage_taken_scale", t.damage_taken_scale);
    rd("chicken_force_scale", t.chicken_force_scale);
    rd("chicken_radius_scale", t.chicken_radius_scale);
    rd("chicken_reload_sec", t.chicken_reload_sec);
}

} // namespace detail

inline bool parse_gametype_json(const std::string &json_text, GametypeConfig &out, std::string &err) {
    detail::JsonValue root;
    detail::JsonParser parser(json_text);
    if (!parser.parse(root, err)) return false;
    if (root.type != detail::JsonValue::Object) {
        err = "Gametype root must be a JSON object";
        return false;
    }

    GametypeConfig cfg;
    if (const auto *v = root.field("id"); v && v->type == detail::JsonValue::String) cfg.id = v->str;
    if (const auto *v = root.field("name"); v && v->type == detail::JsonValue::String) cfg.name = v->str;
    cfg.base_event_type = detail::parse_u32_field(root.field("base_event_type"), 0x18000224u);
    if (const auto *v = root.field("default_track"); v && v->type == detail::JsonValue::String) cfg.default_track = v->str;
    cfg.base_rule_flags = detail::parse_flag_array(root.field("base_rule_flags"), parse_rule_flag_name);
    if (const auto *rf = root.field("rule_flags")) cfg.base_rule_flags |= detail::parse_flag_array(rf, parse_rule_flag_name);
    cfg.base_feats = detail::parse_flag_array(root.field("base_feats"), parse_feat_bit_name);
    if (const auto *gf = root.field("global_feats")) cfg.base_feats |= detail::parse_flag_array(gf, parse_feat_bit_name);
    detail::parse_traits(root.field("base_traits"), cfg.base_traits);
    detail::parse_traits(root.field("traits"), cfg.base_traits);
    if (const auto *v = root.field("shuffle_rounds"); v && v->type == detail::JsonValue::Bool) cfg.shuffle_rounds = v->b;
    if (const auto *v = root.field("rounds_to_win"); v && v->type == detail::JsonValue::Number)
        cfg.rounds_to_win = static_cast<uint8_t>(std::clamp(static_cast<int>(v->num), 1, 20));
    if (const auto *v = root.field("fiesta_interval_sec"); v && v->type == detail::JsonValue::Number)
        cfg.fiesta_interval_sec = static_cast<float>(v->num);
    if (const auto *v = root.field("tips_fuse_sec"); v && v->type == detail::JsonValue::Number)
        cfg.tips_fuse_sec = static_cast<float>(v->num);

    if (const auto *roles = root.field("roles"); roles && roles->type == detail::JsonValue::Array) {
        for (const auto &rv : roles->arr) {
            if (rv.type != detail::JsonValue::Object) continue;
            RoleOverride role;
            if (const auto *f = rv.field("role"); f && f->type == detail::JsonValue::String) role.role = parse_role_id(f->str);
            if (const auto *f = rv.field("forced_vehicle_key"); f && f->type == detail::JsonValue::String)
                role.forced_vehicle_key = f->str;
            if (const auto *f = rv.field("direction_sign"); f && f->type == detail::JsonValue::Number)
                role.direction_sign = f->num < 0 ? -1 : 1;
            role.feat_bits = detail::parse_flag_array(rv.field("feat_bits"), parse_feat_bit_name);
            detail::parse_traits(rv.field("traits"), role.traits);
            if (const auto *f = rv.field("repair_on_assign"); f && f->type == detail::JsonValue::Bool)
                role.repair_on_assign = f->b;
            cfg.roles.push_back(std::move(role));
        }
    }

    if (const auto *zones = root.field("warp_zones"); zones && zones->type == detail::JsonValue::Array) {
        for (const auto &zv : zones->arr) {
            if (zv.type != detail::JsonValue::Object) continue;
            WarpZone z;
            if (const auto *f = zv.field("name"); f && f->type == detail::JsonValue::String) z.name = f->str;
            if (const auto *f = zv.field("anchor"); f && f->type == detail::JsonValue::String) {
                z.anchor = parse_anchor_kind(f->str);
            } else if (zv.field("origin")) {
                z.anchor = AnchorKind::WorldXYZ;
            }
            detail::parse_vec3(zv.field("origin"), z.origin);
            if (const auto *f = zv.field("pct"); f && f->type == detail::JsonValue::Number) z.spline_pct = static_cast<float>(f->num);
            if (const auto *f = zv.field("spline_pct"); f && f->type == detail::JsonValue::Number) z.spline_pct = static_cast<float>(f->num);
            if (const auto *f = zv.field("radius_m"); f && f->type == detail::JsonValue::Number) z.radius_m = static_cast<float>(f->num);
            if (const auto *f = zv.field("face_center"); f && f->type == detail::JsonValue::Bool) z.face_center = f->b;
            cfg.warp_zones.push_back(std::move(z));
        }
    }

    auto parse_trigger_obj = [](const detail::JsonValue &tv) -> TriggerDef {
        TriggerDef t;
        t.id = static_cast<uint16_t>(detail::parse_u32_field(tv.field("id"), 0));
        if (const auto *f = tv.field("kind"); f && f->type == detail::JsonValue::String) t.kind = parse_trigger_kind(f->str);
        if (const auto *f = tv.field("anchor"); f && f->type == detail::JsonValue::String) {
            t.anchor = parse_anchor_kind(f->str);
        } else if (tv.field("center") || tv.field("origin")) {
            t.anchor = AnchorKind::WorldXYZ;
        }
        detail::parse_vec3(tv.field("center"), t.origin);
        detail::parse_vec3(tv.field("origin"), t.origin);
        if (const auto *f = tv.field("pct"); f && f->type == detail::JsonValue::Number) t.spline_pct = static_cast<float>(f->num);
        if (const auto *f = tv.field("spline_pct"); f && f->type == detail::JsonValue::Number) t.spline_pct = static_cast<float>(f->num);
        detail::parse_vec3(tv.field("extents"), t.extents);
        if (const auto *f = tv.field("radius"); f && f->type == detail::JsonValue::Number) t.extents[0] = t.extents[2] = static_cast<float>(f->num);
        if (const auto *f = tv.field("yaw_rad"); f && f->type == detail::JsonValue::Number) t.yaw_rad = static_cast<float>(f->num);
        if (const auto *f = tv.field("dwell_sec"); f && f->type == detail::JsonValue::Number) t.dwell_sec = static_cast<float>(f->num);
        if (const auto *f = tv.field("shrink_rate_mps"); f && f->type == detail::JsonValue::Number) t.shrink_rate_mps = static_cast<float>(f->num);
        if (const auto *f = tv.field("min_radius_m"); f && f->type == detail::JsonValue::Number) t.min_radius_m = static_cast<float>(f->num);
        if (const auto *f = tv.field("dps_outside"); f && f->type == detail::JsonValue::Number) t.dps_outside = static_cast<float>(f->num);
        t.color_rgba = detail::parse_u32_field(tv.field("color_rgba"), 0xFF33AAFFu);
        return t;
    };

    if (const auto *trigs = root.field("triggers"); trigs && trigs->type == detail::JsonValue::Array) {
        for (const auto &tv : trigs->arr) {
            if (tv.type != detail::JsonValue::Object) continue;
            cfg.base_triggers.push_back(parse_trigger_obj(tv));
        }
    }

    if (const auto *rounds = root.field("micro_rounds"); rounds && rounds->type == detail::JsonValue::Array) {
        for (const auto &mv : rounds->arr) {
            if (mv.type != detail::JsonValue::Object) continue;
            MicroRoundDef mr;
            if (const auto *f = mv.field("title"); f && f->type == detail::JsonValue::String) mr.title = f->str;
            if (const auto *f = mv.field("duration_sec"); f && f->type == detail::JsonValue::Number) mr.duration_sec = static_cast<float>(f->num);
            mr.rule_flags = detail::parse_flag_array(mv.field("rule_flags"), parse_rule_flag_name);
            mr.global_feats = detail::parse_flag_array(mv.field("global_feats"), parse_feat_bit_name);
            detail::parse_traits(mv.field("traits"), mr.traits);
            if (const auto *f = mv.field("reroll_hitch"); f && f->type == detail::JsonValue::Bool) mr.reroll_hitch = f->b;
            if (const auto *f = mv.field("repair_all"); f && f->type == detail::JsonValue::Bool) mr.repair_all = f->b;
            if (const auto *f = mv.field("pa_speech_sid"); f && f->type == detail::JsonValue::Number) mr.pa_speech_sid = static_cast<int16_t>(f->num);
            if (const auto *f = mv.field("sfx_cue"); f && f->type == detail::JsonValue::String) mr.sfx_cue = f->str;
            if (const auto *f = mv.field("warp_zone"); f && f->type == detail::JsonValue::String) mr.warp_zone = f->str;
            if (const auto *trigs = mv.field("triggers"); trigs && trigs->type == detail::JsonValue::Array) {
                for (const auto &tv : trigs->arr) {
                    if (tv.type != detail::JsonValue::Object) continue;
                    mr.triggers.push_back(parse_trigger_obj(tv));
                }
            }
            cfg.micro_rounds.push_back(std::move(mr));
        }
    }

    if (cfg.id.empty()) {
        err = "Gametype JSON missing required non-empty 'id' field";
        return false;
    }
    if (cfg.name.empty()) cfg.name = cfg.id;
    cfg.sanitize();
    out = std::move(cfg);
    return true;
}

// ---------------------------------------------------------------------------
// JSON Serialization & Controller-Friendly Slug Helpers (header-only)
// ---------------------------------------------------------------------------
inline const char *role_id_name(RoleId r) {
    switch (r) {
    case RoleId::Survivor:   return "Survivor";
    case RoleId::Infected:   return "Infected";
    case RoleId::Juggernaut: return "Juggernaut";
    case RoleId::VIP:        return "VIP";
    case RoleId::Bus:        return "Bus";
    case RoleId::Tagger:     return "Tagger";
    case RoleId::Default:
    default:                 return "Default";
    }
}

inline const char *anchor_kind_name(AnchorKind a) {
    switch (a) {
    case AnchorKind::ArenaCenter: return "arena_center";
    case AnchorKind::SplinePct:   return "spline_pct";
    case AnchorKind::WorldXYZ:
    default:                      return "world";
    }
}

inline const char *trigger_kind_name(TriggerKind k) {
    switch (k) {
    case TriggerKind::Sphere:   return "sphere";
    case TriggerKind::OBB:      return "obb";
    case TriggerKind::RingOut:  return "ring_out";
    case TriggerKind::Flag:     return "flag";
    case TriggerKind::Cylinder:
    default:                    return "cylinder";
    }
}

inline std::vector<const char *> rule_flag_names(uint32_t mask) {
    static const struct { uint32_t bit; const char *name; } kMap[] = {
        {RF_TRAILER,         "RF_TRAILER"},
        {RF_CASKET_CARGO,    "RF_CASKET_CARGO"},
        {RF_CHAIN_TETHER,    "RF_CHAIN_TETHER"},
        {RF_SUICIDE,         "RF_SUICIDE"},
        {RF_FLAGPOLE,        "RF_FLAGPOLE"},
        {RF_WHIP_AROUND,     "RF_WHIP_AROUND"},
        {RF_LAST_MAN,        "RF_LAST_MAN"},
        {RF_STOP_AND_GO,     "RF_STOP_AND_GO"},
        {RF_BACKWARD,        "RF_BACKWARD"},
        {RF_PUSH_OFF,        "RF_PUSH_OFF"},
        {RF_INSTANT_RESPAWN, "RF_INSTANT_RESPAWN"},
        {RF_INFECTION_TAG,   "RF_INFECTION_TAG"},
        {RF_FIESTA_ROULETTE, "RF_FIESTA_ROULETTE"},
        {RF_TIPS_TAG,        "RF_TIPS_TAG"},
    };
    std::vector<const char *> out;
    for (const auto &e : kMap) {
        if ((mask & e.bit) == e.bit) out.push_back(e.name);
    }
    return out;
}

inline std::vector<const char *> feat_bit_names(uint32_t mask) {
    std::vector<const char *> out;
    if (mask & FB_TURBO) out.push_back("FB_TURBO");
    if (mask & FB_HARDCORE) out.push_back("FB_HARDCORE");
    if ((mask & FB_CHICKEN_MORTAR) == FB_CHICKEN_MORTAR) out.push_back("FB_CHICKEN_MORTAR");
    else if (mask & FB_CHICKEN_STRAIGHT) out.push_back("FB_CHICKEN_STRAIGHT");
    if (mask & FB_AI_VENGEANCE) out.push_back("FB_AI_VENGEANCE");
    if (mask & FB_DEADLY_AMBITION) out.push_back("FB_DEADLY_AMBITION");
    if (mask & FB_RAIN_CHICKENS) out.push_back("FB_RAIN_CHICKENS");
    if (mask & FB_ONE_HIT_KILLS) out.push_back("FB_ONE_HIT_KILLS");
    if (mask & FB_SEPIA) out.push_back("FB_SEPIA");
    if (mask & FB_INVINCIBLE) out.push_back("FB_INVINCIBLE");
    return out;
}

inline std::string slugify_gametype_id(const std::string &name) {
    std::string out;
    bool prev_us = false;
    for (unsigned char c : name) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
            prev_us = false;
        } else if (!out.empty() && !prev_us) {
            out.push_back('_');
            prev_us = true;
        }
        if (out.size() >= 32) break;
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? std::string("custom_mode") : out;
}

namespace detail {

inline std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) >= 0x20) out.push_back(c);
            break;
        }
    }
    out.push_back('"');
    return out;
}

inline std::string json_float(float v) {
    if (!std::isfinite(v)) return "0";
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.4f", static_cast<double>(v));
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.push_back('0');
    }
    if (s == "-0.0") s = "0.0";
    return s;
}

inline void write_flag_array(std::ostringstream &os, const std::vector<const char *> &names) {
    os << "[";
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) os << ", ";
        os << json_escape(names[i]);
    }
    os << "]";
}

inline void write_traits(std::ostringstream &os, const PhysicsTraits &t) {
    os << "{ "
       << "\"gravity_scale\": " << json_float(t.gravity_scale) << ", "
       << "\"mass_scale\": " << json_float(t.mass_scale) << ", "
       << "\"grip_scale\": " << json_float(t.grip_scale) << ", "
       << "\"torque_scale\": " << json_float(t.torque_scale) << ", "
       << "\"top_speed_scale\": " << json_float(t.top_speed_scale) << ", "
       << "\"damage_taken_scale\": " << json_float(t.damage_taken_scale) << ", "
       << "\"chicken_force_scale\": " << json_float(t.chicken_force_scale) << ", "
       << "\"chicken_radius_scale\": " << json_float(t.chicken_radius_scale) << ", "
       << "\"chicken_reload_sec\": " << json_float(t.chicken_reload_sec)
       << " }";
}

inline void write_trigger(std::ostringstream &os, const TriggerDef &t) {
    char hex_col[16];
    std::snprintf(hex_col, sizeof(hex_col), "0x%08X", t.color_rgba);
    os << "{ "
       << "\"id\": " << t.id << ", "
       << "\"kind\": " << json_escape(trigger_kind_name(t.kind)) << ", "
       << "\"anchor\": " << json_escape(anchor_kind_name(t.anchor)) << ", "
       << "\"origin\": [" << json_float(t.origin[0]) << ", " << json_float(t.origin[1]) << ", " << json_float(t.origin[2]) << "], "
       << "\"spline_pct\": " << json_float(t.spline_pct) << ", "
       << "\"extents\": [" << json_float(t.extents[0]) << ", " << json_float(t.extents[1]) << ", " << json_float(t.extents[2]) << "], "
       << "\"yaw_rad\": " << json_float(t.yaw_rad) << ", "
       << "\"dwell_sec\": " << json_float(t.dwell_sec) << ", "
       << "\"shrink_rate_mps\": " << json_float(t.shrink_rate_mps) << ", "
       << "\"min_radius_m\": " << json_float(t.min_radius_m) << ", "
       << "\"dps_outside\": " << json_float(t.dps_outside) << ", "
       << "\"color_rgba\": " << json_escape(hex_col)
       << " }";
}

} // namespace detail

inline std::string serialize_gametype_json(const GametypeConfig &in_cfg) {
    GametypeConfig c = in_cfg;
    c.sanitize();
    if (c.id.empty()) c.id = slugify_gametype_id(c.name);
    if (c.name.empty()) c.name = c.id;

    char hex_evt[16];
    std::snprintf(hex_evt, sizeof(hex_evt), "0x%08X", c.base_event_type);

    std::ostringstream os;
    os << "{\n";
    os << "  \"id\": " << detail::json_escape(c.id) << ",\n";
    os << "  \"name\": " << detail::json_escape(c.name) << ",\n";
    os << "  \"base_event_type\": " << detail::json_escape(hex_evt) << ",\n";
    os << "  \"default_track\": " << detail::json_escape(c.default_track) << ",\n";
    os << "  \"base_rule_flags\": "; detail::write_flag_array(os, rule_flag_names(c.base_rule_flags)); os << ",\n";
    os << "  \"base_feats\": "; detail::write_flag_array(os, feat_bit_names(c.base_feats)); os << ",\n";
    os << "  \"base_traits\": "; detail::write_traits(os, c.base_traits); os << ",\n";
    os << "  \"shuffle_rounds\": " << (c.shuffle_rounds ? "true" : "false") << ",\n";
    os << "  \"rounds_to_win\": " << static_cast<unsigned>(c.rounds_to_win) << ",\n";
    os << "  \"fiesta_interval_sec\": " << detail::json_float(c.fiesta_interval_sec) << ",\n";
    os << "  \"tips_fuse_sec\": " << detail::json_float(c.tips_fuse_sec) << ",\n";

    os << "  \"roles\": [";
    for (size_t i = 0; i < c.roles.size(); ++i) {
        const auto &r = c.roles[i];
        os << (i ? ",\n    {" : "\n    {");
        os << " \"role\": " << detail::json_escape(role_id_name(r.role))
           << ", \"forced_vehicle_key\": " << detail::json_escape(r.forced_vehicle_key)
           << ", \"direction_sign\": " << static_cast<int>(r.direction_sign)
           << ", \"feat_bits\": "; detail::write_flag_array(os, feat_bit_names(r.feat_bits));
        os << ", \"traits\": "; detail::write_traits(os, r.traits);
        os << ", \"repair_on_assign\": " << (r.repair_on_assign ? "true" : "false") << " }";
    }
    os << (c.roles.empty() ? "],\n" : "\n  ],\n");

    os << "  \"warp_zones\": [";
    for (size_t i = 0; i < c.warp_zones.size(); ++i) {
        const auto &z = c.warp_zones[i];
        os << (i ? ",\n    {" : "\n    {");
        os << " \"name\": " << detail::json_escape(z.name)
           << ", \"anchor\": " << detail::json_escape(anchor_kind_name(z.anchor))
           << ", \"origin\": [" << detail::json_float(z.origin[0]) << ", " << detail::json_float(z.origin[1]) << ", " << detail::json_float(z.origin[2]) << "]"
           << ", \"spline_pct\": " << detail::json_float(z.spline_pct)
           << ", \"radius_m\": " << detail::json_float(z.radius_m)
           << ", \"face_center\": " << (z.face_center ? "true" : "false") << " }";
    }
    os << (c.warp_zones.empty() ? "],\n" : "\n  ],\n");

    os << "  \"triggers\": [";
    for (size_t i = 0; i < c.base_triggers.size(); ++i) {
        os << (i ? ",\n    " : "\n    ");
        detail::write_trigger(os, c.base_triggers[i]);
    }
    os << (c.base_triggers.empty() ? "],\n" : "\n  ],\n");

    os << "  \"micro_rounds\": [";
    for (size_t i = 0; i < c.micro_rounds.size(); ++i) {
        const auto &mr = c.micro_rounds[i];
        os << (i ? ",\n    {" : "\n    {");
        os << " \"title\": " << detail::json_escape(mr.title)
           << ", \"duration_sec\": " << detail::json_float(mr.duration_sec)
           << ", \"rule_flags\": "; detail::write_flag_array(os, rule_flag_names(mr.rule_flags));
        os << ", \"global_feats\": "; detail::write_flag_array(os, feat_bit_names(mr.global_feats));
        os << ", \"traits\": "; detail::write_traits(os, mr.traits);
        os << ", \"reroll_hitch\": " << (mr.reroll_hitch ? "true" : "false")
           << ", \"repair_all\": " << (mr.repair_all ? "true" : "false")
           << ", \"pa_speech_sid\": " << mr.pa_speech_sid
           << ", \"sfx_cue\": " << detail::json_escape(mr.sfx_cue)
           << ", \"warp_zone\": " << detail::json_escape(mr.warp_zone)
           << ", \"triggers\": [";
        for (size_t j = 0; j < mr.triggers.size(); ++j) {
            if (j) os << ", ";
            detail::write_trigger(os, mr.triggers[j]);
        }
        os << "] }";
    }
    os << (c.micro_rounds.empty() ? "]\n" : "\n  ]\n");
    os << "}\n";
    return os.str();
}

} // namespace mutator
