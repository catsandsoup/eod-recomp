#pragma once
// Dynamic Trigger Volume, Shrinking Battle Royale Storm, CTF Flag &
// Halo Infection (Tag) evaluator for MutatorEngine.
#include "mutator_schema.h"
#include <array>
#include <cstdint>
#include <vector>

namespace mutator {

class TriggerSystem {
public:
    struct TickResult {
        bool round_dirty = false;    // Role/score changed -> host should push K_MUTATOR_ROUND immediately
        bool triggers_dirty = false; // Trigger/storm/flag state changed -> host replicates K_MUTATOR_TRIGGER
    };

    void reset();
    void spawn_from_defs(const std::vector<TriggerDef> &defs);
    void init_tips(uint32_t seed, uint32_t slot_count, float fuse_sec,
                   const std::vector<RoleOverride> &roles);

    TickResult tick_host(float dt, uint32_t sim_tick, uint32_t slot_count, int local_slot,
                         uint32_t active_rule_flags, const std::vector<RoleOverride> &roles);

    void apply_remote(const TriggerSyncPacket &pkt, int local_slot, uint32_t slot_count);

    TriggerSyncPacket build_packet(uint32_t sim_tick, const std::vector<ChainPair> &chains,
                                   uint16_t hitch_mask) const;

    bool has_triggers() const { return !states_.empty(); }
    const std::vector<ActiveTriggerState> &triggers() const { return states_; }
    uint16_t eliminated_mask() const { return eliminated_mask_; }
    uint8_t tips_in_slot() const { return tips_in_slot_; }
    float tips_fuse_remaining() const { return tips_fuse_remaining_; }
    float tips_slot_fuse(uint32_t slot) const { return slot < kMaxSlots ? tips_fuse_[slot] : 0.0f; }

private:
    std::vector<TriggerDef> defs_;
    std::vector<ActiveTriggerState> states_;
    std::array<std::array<float, kMaxSlots>, kMaxTriggersPerRound> dwell_{};
    std::array<float, kMaxSlots> tag_cooldown_{};
    std::array<float, kMaxSlots> tips_fuse_{};
    uint16_t eliminated_mask_ = 0;
    uint8_t tips_in_slot_ = 0xFF;
    float tips_fuse_remaining_ = 0.0f;
    uint32_t tips_rng_ = 0;
    bool tips_winner_announced_ = false;
};

} // namespace mutator
