#pragma once
// Host-side spring-damper chain tether & mid-match trailer detach controller
// (RF_CHAIN_TETHER / RF_TRAILER).
#include "mutator_schema.h"
#include <cstdint>
#include <vector>

namespace mutator {

class ChainSystem {
public:
    void reset();
    void setup_pairs(uint32_t slot_count, float rest_len_m = 11.0f,
                     float stiffness = 18000.0f, float damping = 1800.0f);
    void set_pairs(const std::vector<ChainPair> &pairs);

    // Apply spring-damper restoring forces into RigidBody::force (+0x90) for taut pairs
    void step_host(uint32_t slot_count);

    bool has_chains() const { return !pairs_.empty(); }
    const std::vector<ChainPair> &pairs() const { return pairs_; }

    // Mid-match trailer detach & hitch replication helpers
    static bool detach_trailer_for_slot(uint32_t slot);
    static uint16_t snapshot_hitch_mask(uint32_t slot_count);
    static void apply_remote_hitch_mask(uint16_t host_hitch_mask, uint32_t slot_count);

private:
    std::vector<ChainPair> pairs_;
};

} // namespace mutator
