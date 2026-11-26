#pragma once
// Built-in Online Play Gametype Presets for MutatorEngine.
// Selectable directly by the lobby host via the "GAMETYPE" option in Online Play.
// All spatial zones use Track-Anchored coordinates ("arena_center" / "spline_pct")
// so every preset runs cleanly across Homebase and all 16 retail tracks.
#include "mutator_schema.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace mutator {

enum PresetId : uint8_t {
    PRESET_STANDARD = 0,
    PRESET_BATTLE_ROYALE = 1,
    PRESET_TIPS_TAG = 2,
    PRESET_INFECTION = 3,
    PRESET_DESTRUCTIONWARE = 4,
    PRESET_FIESTA_ROULETTE = 5,
    PRESET_CHAIN_GANG = 6,
    PRESET_CUSTOM_JSON = 7,
    PRESET_COUNT = 8,
};

inline uint8_t preset_count() { return PRESET_COUNT; }

inline const char *preset_label(uint8_t id) {
    switch (id) {
    case PRESET_STANDARD:        return "STANDARD (VANILLA)";
    case PRESET_BATTLE_ROYALE:   return "BATTLE ROYALE";
    case PRESET_TIPS_TAG:        return "TIPS (TAG)";
    case PRESET_INFECTION:       return "INFECTION (TAG)";
    case PRESET_DESTRUCTIONWARE: return "DESTRUCTIONWARE";
    case PRESET_FIESTA_ROULETTE: return "FIESTA ROULETTE";
    case PRESET_CHAIN_GANG:      return "CHAIN GANG";
    case PRESET_CUSTOM_JSON:     return "CUSTOM JSON";
    default:                     return "STANDARD (VANILLA)";
    }
}

inline const char *preset_description(uint8_t id) {
    switch (id) {
    case PRESET_STANDARD:        return "Vanilla retail rules with no custom mutator overlay.";
    case PRESET_BATTLE_ROYALE:   return "Shrinking safe-zone storm anchored to arena center. Stay inside or burn!";
    case PRESET_TIPS_TAG:        return "Hot Potato Elimination: 1 random car starts IN with a 25s fuse. Tap to pass it (3.5s immunity); hit 0s and you're out!";
    case PRESET_INFECTION:       return "Halo-style Infection tag: Alpha Infected hunts Survivors. Contact converts!";
    case PRESET_DESTRUCTIONWARE: return "Fast 25s micro-rounds: Low-G Chicken Sumo, One-Hit Suicide & Raining Chickens.";
    case PRESET_FIESTA_ROULETTE: return "Randomizes weapons, gravity, mass, grip & speed traits every 20 seconds.";
    case PRESET_CHAIN_GANG:      return "Pairs drivers together with live spring-damper chain tethers + Turbo.";
    case PRESET_CUSTOM_JSON:     return "Loads EOD_GAMETYPE_JSON or mods/gametypes/custom.json (falls back to DestructionWare).";
    default:                     return "";
    }
}

inline bool preset_uses_weapons(uint8_t id) {
    switch (id) {
    case PRESET_TIPS_TAG:
    case PRESET_CHAIN_GANG:
        return false;
    default:
        return true;
    }
}

inline bool config_uses_weapons(const GametypeConfig &cfg) {
    constexpr uint32_t kWeaponBits = FB_CHICKEN_STRAIGHT | FB_CHICKEN_MORTAR | FB_RAIN_CHICKENS;
    if ((cfg.base_rule_flags & RF_FIESTA_ROULETTE) != 0) return true;
    if ((cfg.base_feats & kWeaponBits) != 0) return true;
    for (const auto &r : cfg.roles) {
        if ((r.feat_bits & kWeaponBits) != 0) return true;
    }
    for (const auto &mr : cfg.micro_rounds) {
        if ((mr.rule_flags & RF_FIESTA_ROULETTE) != 0) return true;
        if ((mr.global_feats & kWeaponBits) != 0) return true;
    }
    return (cfg.base_rule_flags & (RF_TIPS_TAG | RF_CHAIN_TETHER)) == 0;
}

inline GametypeConfig make_battle_royale_preset() {
    GametypeConfig cfg;
    cfg.id = "homebase_battle_royale";
    cfg.name = "Battle Royale";
    cfg.default_track = "Homebase";
    cfg.base_rule_flags = RF_PUSH_OFF;
    cfg.base_feats = FB_CHICKEN_STRAIGHT | FB_TURBO;
    cfg.base_traits.damage_taken_scale = 0.85f;
    cfg.base_traits.chicken_reload_sec = 2.5f;

    TriggerDef storm;
    storm.id = 1;
    storm.kind = TriggerKind::RingOut;
    storm.anchor = AnchorKind::ArenaCenter;
    storm.extents[0] = 95.0f;
    storm.extents[1] = 20.0f;
    storm.extents[2] = 95.0f;
    storm.shrink_rate_mps = 1.15f;
    storm.min_radius_m = 12.0f;
    storm.dps_outside = 0.14f;
    storm.color_rgba = 0xFF3A7BFFu; // Glowing storm blue-violet (ABGR/RGBA handled in overlay)
    cfg.base_triggers.push_back(storm);

    cfg.sanitize();
    return cfg;
}

inline GametypeConfig make_tips_tag_preset() {
    GametypeConfig cfg;
    cfg.id = "tips_tag_hot_potato";
    cfg.name = "Tips (Tag)";
    cfg.default_track = "Anoka";
    cfg.base_rule_flags = RF_TIPS_TAG;
    cfg.base_feats = FB_NONE;
    cfg.tips_fuse_sec = 25.0f;

    // Role 0 (Tagger / "IN"): slight speed/torque/grip edge + Turbo + Invincible
    RoleOverride tagger;
    tagger.role = RoleId::Tagger;
    tagger.feat_bits = FB_TURBO | FB_INVINCIBLE;
    tagger.traits.top_speed_scale = 1.12f;
    tagger.traits.torque_scale = 1.18f;
    tagger.traits.grip_scale = 1.10f;
    tagger.repair_on_assign = true;
    cfg.roles.push_back(tagger);

    // Role 1 (Survivor / Runner): Invincible, default traits
    RoleOverride survivor;
    survivor.role = RoleId::Survivor;
    survivor.feat_bits = FB_INVINCIBLE;
    survivor.repair_on_assign = true;
    cfg.roles.push_back(survivor);

    cfg.sanitize();
    return cfg;
}

inline GametypeConfig make_infection_preset() {
    GametypeConfig cfg;
    cfg.id = "halo_infection_tag";
    cfg.name = "Infection (Tag)";
    cfg.default_track = "Anoka";
    cfg.base_rule_flags = RF_INFECTION_TAG;
    cfg.base_feats = FB_NONE;

    // Role 0 (assigned to slot 0 at round start): Alpha Infected
    RoleOverride alpha;
    alpha.role = RoleId::Infected;
    alpha.feat_bits = FB_TURBO;
    alpha.traits.torque_scale = 1.35f;
    alpha.traits.top_speed_scale = 1.25f;
    alpha.traits.grip_scale = 1.20f;
    alpha.traits.damage_taken_scale = 0.35f;
    alpha.repair_on_assign = true;
    cfg.roles.push_back(alpha);

    // Role 1 (assigned to slots 1..N-1): Survivors
    RoleOverride survivor;
    survivor.role = RoleId::Survivor;
    survivor.feat_bits = FB_CHICKEN_STRAIGHT;
    survivor.traits.chicken_reload_sec = 3.5f;
    survivor.traits.chicken_force_scale = 1.40f;
    survivor.repair_on_assign = true;
    cfg.roles.push_back(survivor);

    MicroRoundDef rd;
    rd.title = "SURVIVE THE INFECTION!";
    rd.duration_sec = 180.0f;
    rd.rule_flags = RF_INFECTION_TAG;
    rd.repair_all = true;
    rd.sfx_cue = "arena_horn.sfx";
    cfg.micro_rounds.push_back(rd);

    cfg.sanitize();
    return cfg;
}

inline GametypeConfig make_destructionware_preset() {
    GametypeConfig cfg;
    cfg.id = "destructionware_3round";
    cfg.name = "DestructionWare";
    cfg.default_track = "Anoka";
    cfg.shuffle_rounds = true;
    cfg.rounds_to_win = 5;

    WarpZone center;
    center.name = "arena_bowl";
    center.anchor = AnchorKind::ArenaCenter;
    center.radius_m = 26.0f;
    center.face_center = true;
    cfg.warp_zones.push_back(center);

    // Round 1: Low-G Chicken Sumo with shrinking ring-out
    {
        MicroRoundDef r;
        r.title = "LOW-G CHICKEN SUMO!";
        r.duration_sec = 25.0f;
        r.rule_flags = RF_PUSH_OFF;
        r.global_feats = FB_CHICKEN_STRAIGHT | FB_TURBO;
        r.traits.gravity_scale = 0.45f;
        r.traits.chicken_force_scale = 2.60f;
        r.traits.chicken_reload_sec = 1.25f;
        r.repair_all = true;
        r.warp_zone = "arena_bowl";
        r.sfx_cue = "arena_horn.sfx";
        TriggerDef ring;
        ring.id = 10;
        ring.kind = TriggerKind::RingOut;
        ring.anchor = AnchorKind::ArenaCenter;
        ring.extents[0] = 48.0f;
        ring.extents[1] = 12.0f;
        ring.extents[2] = 48.0f;
        ring.shrink_rate_mps = 1.2f;
        ring.min_radius_m = 14.0f;
        ring.dps_outside = 0.25f;
        ring.color_rgba = 0xFFFF5533u;
        r.triggers.push_back(ring);
        cfg.micro_rounds.push_back(r);
    }

    // Round 2: One-Hit Suicide Turbo
    {
        MicroRoundDef r;
        r.title = "ONE-HIT SUICIDE TURBO!";
        r.duration_sec = 25.0f;
        r.rule_flags = RF_SUICIDE;
        r.global_feats = FB_ONE_HIT_KILLS | FB_TURBO;
        r.traits.top_speed_scale = 1.35f;
        r.traits.torque_scale = 1.30f;
        r.repair_all = true;
        r.sfx_cue = "arena_horn.sfx";
        cfg.micro_rounds.push_back(r);
    }

    // Round 3: Raining Chickens + Hold the Safe Box (Stop-and-Go)
    {
        MicroRoundDef r;
        r.title = "CHICKEN RAIN: HOLD THE BOX!";
        r.duration_sec = 25.0f;
        r.rule_flags = RF_STOP_AND_GO;
        r.global_feats = FB_RAIN_CHICKENS | FB_DEADLY_AMBITION;
        r.traits.gravity_scale = 0.85f;
        r.repair_all = true;
        r.sfx_cue = "arena_horn.sfx";
        TriggerDef box;
        box.id = 20;
        box.kind = TriggerKind::Cylinder;
        box.anchor = AnchorKind::ArenaCenter;
        box.extents[0] = 16.0f;
        box.extents[1] = 8.0f;
        box.extents[2] = 16.0f;
        box.dwell_sec = 1.5f;
        box.color_rgba = 0xFF33EE66u;
        r.triggers.push_back(box);
        cfg.micro_rounds.push_back(r);
    }

    cfg.sanitize();
    return cfg;
}

inline GametypeConfig make_fiesta_roulette_preset() {
    GametypeConfig cfg;
    cfg.id = "fiesta_roulette_derby";
    cfg.name = "Fiesta Roulette";
    cfg.default_track = "Anoka";
    cfg.base_rule_flags = RF_FIESTA_ROULETTE;
    cfg.fiesta_interval_sec = 20.0f;
    cfg.sanitize();
    return cfg;
}

inline GametypeConfig make_chain_gang_preset() {
    GametypeConfig cfg;
    cfg.id = "chain_gang_demolition";
    cfg.name = "Chain Gang";
    cfg.default_track = "Anoka";
    cfg.base_rule_flags = RF_CHAIN_TETHER;
    cfg.base_feats = FB_TURBO;
    cfg.base_traits.damage_taken_scale = 0.75f;
    cfg.base_traits.torque_scale = 1.25f;
    cfg.sanitize();
    return cfg;
}

// Build GametypeConfig for a preset ID. Returns false for PRESET_STANDARD (vanilla).
inline bool build_preset(uint8_t id, GametypeConfig &out) {
    switch (id) {
    case PRESET_STANDARD:
        return false;
    case PRESET_BATTLE_ROYALE:
        out = make_battle_royale_preset();
        return true;
    case PRESET_TIPS_TAG:
        out = make_tips_tag_preset();
        return true;
    case PRESET_INFECTION:
        out = make_infection_preset();
        return true;
    case PRESET_DESTRUCTIONWARE:
        out = make_destructionware_preset();
        return true;
    case PRESET_FIESTA_ROULETTE:
        out = make_fiesta_roulette_preset();
        return true;
    case PRESET_CHAIN_GANG:
        out = make_chain_gang_preset();
        return true;
    case PRESET_CUSTOM_JSON: {
        const char *env_path = std::getenv("EOD_GAMETYPE_JSON");
        const char *candidates[2] = {env_path, "mods/gametypes/custom.json"};
        for (const char *path : candidates) {
            if (!path || !*path) continue;
            std::ifstream in(path, std::ios::binary);
            if (!in) continue;
            std::ostringstream ss;
            ss << in.rdbuf();
            std::string err;
            if (parse_gametype_json(ss.str(), out, err)) return true;
        }
        out = make_destructionware_preset();
        return true;
    }
    default:
        return false;
    }
}

} // namespace mutator
