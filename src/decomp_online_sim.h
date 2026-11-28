// decomp_online_sim.h - Decompiled EE online simulation-step, active driver switch,
// 6-byte human input replay recorder, and DetentionMaster participant setup functions
// from SLUS_209.10.
//
// Each entry point runs through diff_run() against its recompiled f_XXXXXXXX_orig()
// counterpart so EOD_DIFF can prove bit-exact equivalence while preserving all
// netplay hooks in src/netplay_trace.cpp and src/game_patches.cpp.
//
// Full instruction-level ledger & struct maps: docs/DECOMP_ONLINE_SIM.md.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void eod_online_sim_00177718(void);  // WorldStepTick / WorldSimulationStep (0x00177718)
void eod_online_sim_00190A38(void);  // CarSetActiveDriver / DriverWrapperSetActive (0x00190A38)
void eod_online_sim_00191548(void);  // HumanReplayRecordTick / ReplayRecordHumanTick (0x00191548)
void eod_online_sim_00165E10(void);  // DetentionMasterSetupRole / DetentionSetupParticipants (0x00165E10)

#ifdef __cplusplus
}
#endif
