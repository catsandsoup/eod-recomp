// decomp_race_lifecycle.h - Decompiled EE race lifecycle, countdown, RNG seeding,
// crash-orbit pause, and Eve event-completion functions from SLUS_209.10.
//
// Each entry point runs through diff_run() against its recompiled f_XXXXXXXX_orig()
// counterpart so EOD_DIFF can prove bit-exact equivalence while existing netplay,
// session-start, and solo editor hooks remain in their original wrapper files.
//
// Full instruction-level ledger & struct maps: docs/DECOMP_RACE_LIFECYCLE.md.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void eod_race_00176698(void);  // RaceSessionInitPhase (0x00176698)
void eod_race_00176798(void);  // RaceGetModeState / CountdownReleaseCheck (0x00176798)
void eod_race_00176918(void);  // RaceSessionSeedRng (0x00176918)
void eod_race_001D3FB8(void);  // StartCrashOrbitPause (0x001D3FB8)
void eod_race_00120FB8(void);  // EveHasMoreEvents (0x00120FB8)
void eod_race_00114358(void);  // RunRaceContest (0x00114358)

#ifdef __cplusplus
}
#endif
