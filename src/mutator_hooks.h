#pragma once
// Lightweight header-only bridge between native game hooks (game_patches.cpp,
// hle_recovery.cpp, car_damage.cpp) and the MutatorEngine. Mirrors
// src/netplay_hooks.h so standalone unit tests need no link dependency on
// mutator_engine.cpp.
#include "mutator_schema.h"
#include "core/memory.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mutator_hooks {

struct SlotState {
    mutator::RoleId role = mutator::RoleId::Default;
    int8_t direction_sign = 1; // +1 forward, -1 reversed (RF_SUICIDE / RF_WHIP_AROUND)
    uint32_t feat_bits = 0;    // Per-slot FeatBits overrides
    mutator::PhysicsTraits traits{};
    uint8_t warp_gen = 0;      // Incremented on atomic warp so Remote::push snaps instantly
    int32_t score = 0;         // Round / custom objective score
    bool active = false;
};

struct WorldCamera {
    float position[3] = {};
    float axes[3][3] = {};
    float horizontal_scale = 0.0f;
    float vertical_scale = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    bool valid = false;
};

struct HudSnapshot {
    bool visible = false;
    std::string gametype_name;
    std::string round_title;
    uint32_t round_number = 0;       // 1-based display
    uint32_t total_rounds = 0;       // 0 if endless/single continuous mode
    float remaining_sec = 0.0f;      // <=0 if untimed
    uint32_t active_rule_flags = 0;
    uint32_t local_feat_bits = 0;
    mutator::RoleId local_role = mutator::RoleId::Default;
    int8_t local_direction_sign = 1;
    float local_gravity_scale = 1.0f;
    float local_mass_scale = 1.0f;
    std::string banner_text;
    uint64_t banner_until_us = 0;
    std::string zone_warning;        // e.g. "OUTSIDE SAFE ZONE!" or "HOLDING FLAG!"
    uint8_t survivors_alive = 0;
    uint8_t infected_count = 0;
    float storm_radius_m = 0.0f;
    uint16_t eliminated_mask = 0;
    uint8_t tips_in_slot = 0xFF;
    float tips_fuse_sec = 0.0f;
    uint8_t tips_alive_count = 0;
};

inline bool active = false;
inline bool suppress_weapons = false;
inline uint32_t active_rule_flags = 0;
inline uint8_t global_warp_gen = 0;
inline std::array<SlotState, mutator::kMaxSlots> slots{};
inline std::array<bool, mutator::kMaxSlots> tips_immune{};
inline HudSnapshot hud{};
inline std::vector<mutator::ActiveTriggerState> active_triggers{};
inline std::vector<mutator::ChainPair> active_chains{};
inline uint16_t active_hitch_mask = 0;
inline WorldCamera world_camera{};

// Optional per-frame tick hook installed by MutatorEngine (null in unit tests)
inline void (*on_world_tick)() = nullptr;
// Optional native EE callbacks installed by mutator_native_bridge.cpp in full eod runtime
inline void (*on_world_view)() = nullptr;
inline void (*on_offline_world_construct)(uint32_t description) = nullptr;
inline bool (*ground_snap_y)(float x, float z, float &out_y) = nullptr;
inline void (*native_repair_car)(uint32_t car) = nullptr;
inline void (*native_set_car_pose)(uint32_t car, const float pose_4x3[12]) = nullptr;
inline void (*native_reset_slot_hud)(uint32_t slot) = nullptr;
inline void (*native_detach_trailer)(uint32_t trailer_car) = nullptr;
inline void (*play_pa_speech)(int16_t speech_sid) = nullptr;
inline void (*play_sfx_cue)(const char *sfx_name) = nullptr;
inline void (*show_hud_banner)(const char *text, int duration_ms) = nullptr;

inline void reset() {
    active = false;
    suppress_weapons = false;
    active_rule_flags = 0;
    global_warp_gen = 0;
    for (auto &s : slots) s = SlotState{};
    tips_immune.fill(false);
    hud = HudSnapshot{};
    active_triggers.clear();
    active_chains.clear();
    active_hitch_mask = 0;
    world_camera = WorldCamera{};
}

// Resolve participant slot [0..15] from a guest Car pointer via 0x00447458
// (falling back to car+0x1DC participant index when in range).
inline int slot_for_car(uint32_t car) {
    if (!car || (car & 3) || car > EE_RAM_SIZE - 0x1E4) return -1;
    const uint32_t count = ee_r32(0x0048522C);
    const uint32_t limit = (count > 0 && count <= mutator::kMaxSlots) ? count : mutator::kMaxSlots;
    for (uint32_t i = 0; i < limit; ++i) {
        if (ee_r32(0x00447458 + i * 4) == car) return static_cast<int>(i);
    }
    const uint32_t idx = ee_r32(car + 0x1DC);
    if (idx < mutator::kMaxSlots && ee_r32(0x00447458 + idx * 4) == 0) {
        // Standalone test fixture without populated 0x447458 table
        return static_cast<int>(idx);
    }
    return -1;
}

inline const SlotState *state_for_slot(int slot) {
    if (!active || slot < 0 || slot >= static_cast<int>(mutator::kMaxSlots)) return nullptr;
    return slots[static_cast<size_t>(slot)].active ? &slots[static_cast<size_t>(slot)] : nullptr;
}

inline const SlotState *state_for_car(uint32_t car) {
    if (!active) return nullptr;
    return state_for_slot(slot_for_car(car));
}

inline bool car_invincible(uint32_t car) {
    const auto *s = state_for_car(car);
    return s && ((s->feat_bits & mutator::FB_INVINCIBLE) != 0);
}

inline bool slot_has_feat(int slot, uint32_t bit) {
    const auto *s = state_for_slot(slot);
    return s && ((s->feat_bits & bit) != 0);
}

inline bool car_has_feat(uint32_t car, uint32_t bit) {
    const auto *s = state_for_car(car);
    return s && ((s->feat_bits & bit) != 0);
}

// True when this car's slot is driving in reversed direction (RF_SUICIDE /
// RF_WHIP_AROUND), so hle_recovery.cpp suppresses false wrong-way auto-resets.
inline bool suppress_wrong_way(uint32_t car) {
    const auto *s = state_for_car(car);
    return s && s->direction_sign < 0;
}

inline uint8_t warp_gen_for_car(uint32_t car) {
    if (!active) return 0;
    const auto *s = state_for_car(car);
    return s ? s->warp_gen : global_warp_gen;
}

inline bool is_tips_active() {
    return active && ((active_rule_flags & mutator::RF_TIPS_TAG) != 0);
}

inline uint8_t tips_tagger_slot() {
    return is_tips_active() ? hud.tips_in_slot : 0xFFu;
}

inline bool slot_tips_immune(uint32_t slot) {
    return is_tips_active() && slot < mutator::kMaxSlots && tips_immune[slot];
}

} // namespace mutator_hooks
