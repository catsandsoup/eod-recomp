#pragma once
#include "mutator_chain.h"
#include "mutator_schema.h"
#include "mutator_triggers.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mutator {

struct ResolvedVehicle {
    uint32_t table_index = 0;
    uint32_t vehicle_number = 0;
    uint32_t vehicle_class = 0;
    std::string chassis_name;
};

class Engine {
public:
    bool load_json(const std::string &json_text, std::string &err);
    bool load_file(const std::string &path, std::string &err);
    void set_config(const GametypeConfig &cfg);
    void clear_config();

    bool has_config() const { return has_config_; }
    const GametypeConfig &config() const { return config_; }

    // Called once when a match/world binds (solo or netplay GO).
    // authority=true for solo & netplay Host; false for netplay Guest.
    void on_world_bound(uint32_t event_id, uint32_t seed, bool authority = true, int local_slot = 0);
    // Called every 60 Hz world simulation step
    void tick(uint32_t sim_tick);
    // Called on match teardown / return to menu
    void reset_match();

    // Micro-round & role replication (Host -> Guests via K_MUTATOR_ROUND)
    bool has_round_packet() const { return round_serial_ > 0; }
    uint32_t round_serial() const { return round_serial_; }
    const RoundPacket &latest_round_packet() const { return latest_round_; }
    // Guest-side application of host's K_MUTATOR_ROUND; returns true if newly applied.
    bool apply_remote_round(const RoundPacket &pkt, uint32_t current_sim_tick);
    // Host-side manual round jump (also usable for Infection / Battle Royale phase flips)
    void advance_round(uint32_t sim_tick);

    // Dynamic trigger/storm/flag/chain replication (Host -> Guests via K_MUTATOR_TRIGGER)
    bool has_live_sync_state() const;
    TriggerSyncPacket build_trigger_packet(uint32_t sim_tick) const;
    void apply_remote_triggers(const TriggerSyncPacket &pkt);
    const TriggerSystem &triggers() const { return triggers_; }
    const ChainSystem &chains() const { return chains_; }

    // Direct per-slot setters (also used by Fiesta rerolls, Infection tag & tests)
    void apply_slot_role(uint32_t slot, const RoleOverride &role);
    void set_slot_direction(uint32_t slot, int8_t direction_sign);
    void set_slot_traits(uint32_t slot, const PhysicsTraits &traits);
    void set_slot_feats(uint32_t slot, uint32_t feat_bits);
    void set_slot_score(uint32_t slot, int32_t score);

    // Apply cached per-slot traits to live guest RigidBody / Tuning / Car RAM
    void sync_live_physics();

    // Resolve a string vehicle key (e.g. "bus", "hearse") against kVehicleTable (0x003CBEE4)
    static bool resolve_vehicle_key(const std::string &key, ResolvedVehicle &out);
    // Verify that a set of vehicle numbers does not exceed kMaxLoadedChassis (7-chassis EE cap)
    static uint32_t count_distinct_chassis(const std::vector<uint32_t> &vehicle_numbers);
    static bool within_chassis_budget(const std::vector<uint32_t> &vehicle_numbers);

    // Resolve Track-Anchored coordinates (WorldXYZ / ArenaCenter / SplinePct)
    static bool resolve_anchor(AnchorKind anchor, const float origin[3], float spline_pct, float out_xyz[3]);

    uint32_t active_round() const { return current_round_; }
    uint32_t fiesta_epoch() const { return fiesta_epoch_; }
    uint8_t warp_gen() const { return latest_round_.warp_gen; }

private:
    struct BodyBaseline {
        uint32_t car_ptr = 0;
        uint32_t body_ptr = 0;
        uint32_t tuning_ptr = 0;
        float inv_inertia[4] = {};
        float grip[4] = {};
        float damage_mult = 1.0f;
        bool valid = false;
    };

    void begin_micro_round(uint32_t order_pos, uint32_t sim_tick);
    void apply_fiesta_reroll(uint32_t epoch);
    void apply_round_defaults(uint32_t active_flags, uint32_t active_feats, const PhysicsTraits &traits);
    void capture_baseline(uint32_t slot, uint32_t car, uint32_t body_idx);
    void build_round_packet(uint16_t def_index, uint32_t sim_tick, uint32_t duration_ticks,
                            uint32_t active_flags, uint32_t active_feats, const PhysicsTraits &traits);
    void refresh_hud(uint32_t sim_tick);
    const WarpZone *find_warp_zone(const std::string &name) const;

    GametypeConfig config_{};
    bool has_config_ = false;
    bool match_active_ = false;
    bool is_authority_ = true;
    int local_slot_ = 0;
    uint32_t event_id_ = 0;
    uint32_t seed_ = 0;
    uint32_t current_round_ = 0;
    uint32_t round_serial_ = 0;
    uint32_t round_start_tick_ = 0;
    uint32_t round_duration_ticks_ = 0;
    uint32_t fiesta_epoch_ = 0;
    uint32_t last_tick_ = 0;
    std::vector<uint16_t> round_order_;
    std::string active_round_title_;
    RoundPacket latest_round_{};
    std::array<BodyBaseline, kMaxSlots> baselines_{};
    TriggerSystem triggers_{};
    ChainSystem chains_{};
};

Engine &engine();

} // namespace mutator
