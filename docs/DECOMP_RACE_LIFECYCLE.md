# Race Lifecycle, Countdown, RNG & Crash Orbit Decompilation

Behaviorally exact C++ decompilation of the 6 race lifecycle, countdown, RNG seeding, crash-orbit pause, and Eve event-completion functions from `SLUS_209.10`, implemented in `src/decomp_race_lifecycle.cpp` and `src/decomp_race_lifecycle.h`.

All 6 functions preserve 100% of existing netplay, session-start, and solo editor map-preview hooks (`src/netplay_trace.cpp`, `src/custom_map_physics.cpp`, `src/session_start.cpp`), replacing direct `f_XXXXXXXX_orig()` calls with `diff_run`-backed `eod_race_XXXXXXXX()` entry points.

---

## Function Ledger

| Address | Symbol | Role | Instructions | Existing Hook Owner | `EOD_DIFF` Calls / Mismatches |
| --- | --- | --- | ---: | --- | --- |
| `0x00176698` | `RaceSessionInitPhase` | Sets `g_RaceTick` and `g_RaceClockSeconds = tick * (1/59.94f)` | 12 | `src/netplay_trace.cpp` (netplay start-phase & RNG epoch) | **1024 / 1024 checked, 0 mismatched** (1-lap race) |
| `0x00176798` | `RaceGetModeState` | Reads `g_RaceModeState` (`0x004472C0`); checked at countdown release (`0x001B8DA8`) & race teardown (`0x00114590`) | 3 | `src/custom_map_physics.cpp` (solo editor map preview countdown) | **480 / 480 checked, 0 mismatched** (1500-frame race); **239 / 239 checked, 0 mismatched** (3500-frame derby) |
| `0x00176918` | `RaceSessionSeedRng` | Generates 2-word RNG seed pair modulo `30000` from `TimerGetMs()` + `g_RngSeedEntropy`, tail-calls `DebugPrintf("seed: %d, %d", s0, s1)` | 36 | `src/netplay_trace.cpp` (agreed netplay session seeds) | **9 / 9 checked, 0 mismatched** (across race & derby runs) |
| `0x001D3FB8` | `StartCrashOrbitPause` | Requests physics pause (`0x00176710`) and arms 3-second crash-orbit camera timer when single-viewport & qualifying chase camera | 48 | `src/netplay_trace.cpp` (bypassed during online events) | **0 / 0 live calls** (not triggered in headless autodrive; static instruction-for-instruction verified against `ee_dis.py`, `gen/rec_035.c`, and Ghidra) |
| `0x00120FB8` | `EveHasMoreEvents` | Returns `0` in Single Event mode (`g_EveMode == 3`), else dispatches `EveTracker::vtable[+0x2C]()` | 16 | `src/session_start.cpp` (returns `0` to unwind online events) | **1 / 1 checked, 0 mismatched** (per headless run) |
| `0x00114358` | `RunRaceContest` | Full contest lifecycle: overlay load, retry loop (`0x00114020`), post-race replay/scoring, and cleanup | 284 | `src/session_start.cpp` (sets `g_ContestExitFlag = 1` for online unwind) | **Live end-to-end verified** (ran `decomp_00114358` directly across 5 headless runs with exit 0; cannot be snapshot-diffed in `EOD_DIFF` because it wraps the multi-frame VBlank/DMA loop `0x00114020`) |

---

## Verified Globals, Struct Offsets & Constants

### Race Timing & Mode State (`0x003CDA8C` / `0x004472C0`)
- `0x003CDA8C` (`int32_t`): `g_RaceTick` — native simulation frame counter.
- `0x003CDAC8` (`int32_t`): `g_RacePhysicsPauseReq` — physics pause state (`1` = requested by `0x00176710`, `2` = set by `0x001766F8`).
- `0x003CDACC` (`float`): `g_RaceClockSeconds` — `fmul_ps2((float)g_RaceTick, 0x3C88AB86)` (`0x3C88AB86` = `0.016683351f` = `1 / 59.94` s NTSC frame period).
- `0x004472C0` (`int32_t`): `g_RaceModeState` — race mode state (`0` = inactive, `2` = active countdown/race).

### Race RNG Seeding (`0x00447370`)
- `0x00447370` (`int32_t`): `g_RngSeedEntropy` — running entropy accumulator incremented by `0x9947` (`39239`) for seed 0 and `0x0002F1CD` (`192973`) for seed 1.
- `0x7530` (`30000`): signed 32-bit remainder modulus (`MIPS div` -> `HI`).
- `0x003F8F38` (`const char*`): `"seed: %d, %d"` format string passed to `DebugPrintf` (`0x00331E48`).
- `dst` (`$a0`): `int32_t seed_pair[2]` (e.g. `0x004473B0`, `0x004473B8`, `0x004473C0` for native RNG streams 4/3/5).

### Crash Orbit Camera Pause (`0x001D3FB8`)
- `0x0048CB90` (`int32_t`): `g_PhysCamType` — active camera view mode. Qualifying views for crash orbit pause: `4`, `5`, `6`, `0x15` (`21`).
- `0x0048D280` (`float`): `g_CrashOrbitStartSec` — `fmul_ps2((float)TimerGetMs(), 0x3A83126F)` (`0x3A83126F` = `0.001f`).
- `0x003CE9D8` (`uint32_t`): `g_CrashCamCtrlPtr` — pointer to camera controller struct:
  - `+0x54` (`int32_t`): `orbit_pause_active` (set to `1`)
  - `+0x58` (`int32_t`): `orbit_timer_a` (cleared to `0`)
  - `+0x5C` (`int32_t`): `orbit_timer_b` (cleared to `0`)

### Eve Event Tracker (`0x00120FB8`)
- `0x004394F0` (`int32_t`): `g_EveMode` — `3` indicates Single Event (no subsequent Eve contests).
- `0x003C4BF8` (`uint32_t`): `g_EveTrackerPtr` — pointer to `EveTracker` object:
  - `+0x00` (`uint32_t`): `vtable`
  - `vtable + 0x2C`: `int32_t (*HasMoreEvents)(EveTracker* this)`

### Contest Lifecycle & Cleanup (`0x00114358`)
- Signature: `void RunRaceContest(uint32_t session_ptr, uint32_t abort_flag_ptr, uint32_t completed_out_ptr)` (`$a0`, `$a1`, `$a2`).
- `session_ptr` (`$a0`):
  - `+0x2C`: contest info / result descriptor passed to `0x00115B70`, `0x00120740`, `0x00120E78`.
  - `+0x9A4` (`2468`): track/race ID passed to `0x00200B10`.
- `0x003EDB20` (`const char*`): `"overlays.res"` loaded via `0x00132890` and unloaded via `0x00132798`.
- `0x003EDB30` (`uint32_t[9]`): static table of 9 overlay object pointers (`0x003C3E48` .. `0x003C40C8`), registered via `0x00116B38` and unregistered via `0x00116B30`.
- `0x003C4118`: player viewport descriptor table (`+0x04`, `+0x20`, `+0x3C`, `+0x58`, stride `0x1C`), registered for slots `0..3` via `0x001D41B0(ptr, slot, 0)`.
- `0x003C3CE8` (`int32_t`): `g_RacePhaseIndex` (`0` normally, `0xB` when `0x002B7610() != 0`).
- `0x003C3CF8`: phase callback table (stride `0x18`, `+0x04` = `void (*phase_init)(void)`).
- `0x00437BA8` (`int32_t`): `g_ContestExitFlag` — cleared on `*abort_flag_ptr != 0` teardown; set back to `1` by `session_start.cpp` for online unwind.
- `0x00437BAC` (`int32_t`): `g_RaceQuitFlag` — cleared each retry, set to `1` if `0x0022DA58() != 0`, prevents retry and selects `0x00120E78` instead of `0x00120740`.
- `0x00437BB0` (`int32_t`): `g_RaceStateFlag` — cleared each retry.
- Guest stack frame (`0x2D0` bytes):
  - `sp + 0x00 .. 0x20`: 9 overlay pointers (36 bytes).
  - `sp + 0x30 .. 0x12F`: 256-byte `result_buf` (`0x001D9B60` / `0x00115B70` / `0x00120740` / `0x00120E78`).
  - `sp + 0x130 .. 0x22F`: 256-byte `extra_buf` (`0x001D9C78(extra_buf, 0x14)`).

---

## Call Graphs

- `0x00176698` (`RaceSessionInitPhase`): leaf.
- `0x00176798` (`RaceGetModeState`): leaf.
- `0x00176918` (`RaceSessionSeedRng`) -> `0x00337718` (`TimerGetMs`, x2), tail-call `0x00331E48` (`DebugPrintf`).
- `0x001D3FB8` (`StartCrashOrbitPause`) -> `0x00203238`, `0x00211378` (`ViewGetActiveViewportCount`), `0x00176710` (`RequestPhysicsPause`, 5-instr leaf), `0x00337718` (`TimerGetMs`).
- `0x00120FB8` (`EveHasMoreEvents`) -> indirect `EveTracker::vtable[+0x2C]`.
- `0x00114358` (`RunRaceContest`) -> `0x002DE1F8`, `0x0011F4A8`, `0x0011F4A0`, `0x00200B10`, `0x00132890`, `0x002A11E8`, `0x00105E90`, `0x00173118`, `0x00116C08`, `0x00116B38` (x9), `0x001D41B0` (x4), `0x001D4078`, `0x0012E6E0`, `0x002393A8`, `0x002B7610`, `0x001155C8`, `phase_table[idx].init()`, `0x00114020` (main race loop), `0x0022DA58`, `0x00120AC8`, `0x002393B8`, `0x0012E6D0`, `0x00115758`, `0x00176798`, `0x00175758`, `0x001B04A0`, `0x002CE1D0`, `0x002CE160`, `0x0012D730`, `0x00106900`, `0x002CE120`, `0x0032EDA0`, `0x002031F8`, `0x0010BBA0`, `0x00115578`, `0x00200DA8`, `0x00174258`, `0x00105FF8`, `0x002A11A8`, `0x00116B30` (x9), `0x00116BB8`, `0x00237760`, `0x00132798`, `0x00120F30`, `0x00120A80`, `0x00120C28`, `0x0013D460`, `0x001D9B60`, `0x003A5FD0`, `0x00115B70`, `0x001D9C78`, `0x00120E78`, `0x00120740`, `0x00200DB8`.

Note on `EOD_DIFF` for `0x00114358`: because `RunRaceContest` executes the full multi-frame race loop `0x00114020` (which performs VBlank waits and GS/DMA kicks), `diff_run` with `EOD_DIFF` active on `0x00114358` cannot hold interrupts across the entire race loop (`diff_harness.h` hardware/IRQ limit). `0x00114358` is still wired through `diff_run` (so it runs the C++ replacement directly during live execution) and verified end-to-end by running a complete headless race lifecycle through `decomp_00114358`.

---

## STAR Handoff

- **Situation**: The 6 online-critical race lifecycle, countdown, RNG seeding, crash-orbit pause, and Eve event-completion functions (`0x00176698`, `0x00176798`, `0x00176918`, `0x001D3FB8`, `0x00120FB8`, `0x00114358`) were wrapped directly around recompiled `f_XXXXXXXX_orig()` originals in `src/netplay_trace.cpp`, `src/custom_map_physics.cpp`, and `src/session_start.cpp`.
- **Task**: Decompile all 6 functions into clean, readable, malleable, `diff_run`-backed C++ in `src/decomp_race_lifecycle.cpp` while preserving 100% of existing netplay, session-start, and editor preview hooks and verifying bit-exactness with live headless `EOD_DIFF`.
- **Action**: Created `src/decomp_race_lifecycle.h` / `src/decomp_race_lifecycle.cpp` with named constants for all globals, struct offsets, and EE float bit patterns (`0x3C88AB86`, `0x3A83126F`); rewired the 3 wrapper files to call `eod_race_XXXXXXXX()`; added forwarding stubs to `tests/session_start_test.cpp` and `tests/custom_map_physics_test.cpp`; updated `config/hle.txt` comments; built `eod`, `session_start_test`, and `custom_map_physics_test` with zero warnings in touched files; ran both unit tests and 5 headless `EOD_DIFF` scenarios (No-Rules Race, 1-lap autodrive Race, Demolition Derby, Suicide Race).
- **Result**: Both unit tests pass (`session_start_test=0`, `custom_map_physics_test=0`). Live `EOD_DIFF` reported **0 mismatches** on all exercised functions (`00176698`: 1024/1024, `00176798`: 480/480 race + 239/239 derby, `00176918`: 9/9, `00120FB8`: 1/1 per run), and `decomp_00114358` ran end-to-end across all 5 headless runs with exit 0. `001D3FB8` had 0 live calls in headless autodrive (requires a qualifying single-view wreck incident from `0x002577F0`).
- **Next decisive test**: Run a scripted/interactive single-player severe wreck incident with `EOD_DIFF=001D3FB8` to record live `001D3FB8` call counts, and consider decompiling the small leaf/helper callees (`0x00176710` `RequestPhysicsPause`, `0x00114020` race frame loop, `0x00120AC8`/`0x00120A80`/`0x00120C28` Eve state predicates) in a follow-up batch.
