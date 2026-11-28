// decomp_mmo_sim.h - Behaviorally exact C++ decompilation of SLUS_209.10's
// authoritative simulation core inside WorldStepTick (0x00177718) — the pure
// headless dedicated-server physics, collision broadphase/quadtree, snapshot
// history, and race-step loop:
//
//   0x001779C0  EntityMatrixOrthonormalizeRoundRobin
//   0x00177B40  SimSnapshotHistoryStep
//   0x00177CA0  DynamicEntitiesPhysicsStep
//   0x00178258  DynamicEntitiesPostStep
//   0x001792E8  TrackRacePreStep
//   0x001793B8  TrackRaceMidStep
//
// Each entry point is registered as `wrap` in config/hle.txt and runs through
// diff_run() against its recompiled f_XXXXXXXX_orig() counterpart so EOD_DIFF
// proves bit-exact equivalence across all scenes.
//
// Full ledger, struct maps & STAR handoff: docs/DECOMP_MMO_SIM.md.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void f_001779C0(void);  // EntityMatrixOrthonormalizeRoundRobin
void f_00177B40(void);  // SimSnapshotHistoryStep
void f_00177CA0(void);  // DynamicEntitiesPhysicsStep
void f_00178258(void);  // DynamicEntitiesPostStep
void f_001792E8(void);  // TrackRacePreStep
void f_001793B8(void);  // TrackRaceMidStep

#ifdef __cplusplus
}
#endif
