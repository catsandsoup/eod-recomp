// decomp_physics_collision.h - Decompiled EE out-of-bounds recovery countdown,
// timed-damage feat / race director update, wheel ground query, and vehicle
// physics step + dual-probe wheel ground sampler from SLUS_209.10.
//
// Each entry point runs through diff_run() against its recompiled f_XXXXXXXX_orig()
// counterpart so EOD_DIFF can prove bit-exact equivalence while preserving all
// recovery, mutator/invincibility, and custom-map collision hooks in
// src/hle_recovery.cpp, src/game_patches.cpp, and src/custom_map_physics.cpp.
//
// Full instruction-level ledger & struct maps: docs/DECOMP_PHYSICS_COLLISION.md.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void eod_physics_001B8370(void);  // CarUpdateOutOfBoundsRecovery (0x001B8370)
void eod_physics_001B5840(void);  // TimedDamageFeatUpdate (0x001B5840)
void eod_physics_00212500(void);  // WheelGroundQuery (0x00212500)
void eod_physics_001DE7B0(void);  // ChassisSweptBoxCollide / VehiclePhysicsStep (0x001DE7B0)

#ifdef __cplusplus
}
#endif
