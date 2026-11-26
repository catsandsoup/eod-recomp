#pragma once
// Atomic in-place vehicle warp & repair (Milestone 2).
// Relocates RigidBody, WheelSet, Car pose/velocity, scene bounds, HUD latch,
// and wrong-way recovery timers in one step without reloading the venue.
#include "mutator_schema.h"
#include <cstdint>

namespace mutator {

struct WarpPose {
    // 4x3 row-major pose: right[0..2], up[3..5], forward[6..8], position[9..11]
    float pose[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
};

// Construct an upright world pose at (x, y, z) facing yaw_rad around +Y.
WarpPose make_yaw_pose(float x, float y, float z, float yaw_rad);

// Atomically teleport (and optionally full-repair) one participant slot.
// Increments mutator_hooks::slots[slot].warp_gen so K_SNAP snaps remote puppets.
bool warp_slot(uint32_t slot, const WarpPose &target, bool repair);

// Resolve a Track-Anchored WarpZone and arrange all active slots in a clean,
// ground-snapped non-overlapping circle formation. Returns number of cars warped.
uint32_t warp_formation_to_zone(const WarpZone &zone, uint32_t slot_count, bool repair, uint32_t seed = 0);

} // namespace mutator
