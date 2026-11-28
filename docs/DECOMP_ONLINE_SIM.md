# Online Simulation Step, Active Driver, Replay Input & DetentionMaster Decompilation

Behaviorally exact C++ decompilation of the 4 core online simulation-step, active driver switching, 6-byte human input replay recording, and DetentionMaster participant setup functions from `SLUS_209.10`, implemented in `src/decomp_online_sim.cpp` and `src/decomp_online_sim.h`.

All 4 functions preserve 100% of existing netplay hooks (`src/netplay_trace.cpp`, `src/game_patches.cpp`), replacing direct `f_XXXXXXXX_orig()` calls with `diff_run`-backed `eod_online_sim_XXXXXXXX()` entry points.

---

## Function Ledger

| Address | Symbol | Role | Instructions | Existing Hook Owner | `EOD_DIFF` Calls / Mismatches (`EOD_HEAP_CHECK=1`) |
| --- | --- | --- | ---: | --- | --- |
| `0x00177718` | `WorldStepTick` (`WorldSimulationStep`) | Main per-tick world physics, entity, event-rules, and clock simulation step; netplay lockstep boundary & future 24/7 headless dedicated-server tick root | 169 | `src/netplay_trace.cpp` (`step_begin`/`step_allowed`/`before_step`/`after_step`/`step_end` + JSONL trace) | **1,097 / 1,097 checked, 0 mismatched** (Woodly Jump Race, 1500 frames) |
| `0x00190A38` | `CarSetActiveDriver` (`DriverWrapperSetActive`) | Switches `car+0x1C1C` driver wrapper active driver between pace-lap autopilot (`+0x08`) and human/remote player (`+0x0C`) with `OnDeactivate`/`OnActivate` vtable hooks | 26 | `src/netplay_trace.cpp` (`EOD_NETPLAY_DEBUG` driver-switch logger) | **663 / 663 checked, 0 mismatched** across 4 runs (17 Woodly w/ outer diff, 172 Dundas No-Rules Race, 287 Dennison Detention, 187 Woodly Jump Race) |
| `0x00191548` | `HumanReplayRecordTick` (`ReplayRecordHumanTick`) | Quantizes and packs live per-tick human controller channels into a 6-byte replay record; pauses & resyncs phase at end of replay playback | 216 | `src/netplay_trace.cpp` (`EOD_NETPLAY_TEST_CONTROLS` deterministic input injection + `driver_input` trace) | **591 / 591 checked, 0 mismatched** across 3 runs (155 Dundas No-Rules Race, 265 Dennison Detention, 171 Woodly Jump Race) |
| `0x00165E10` | `DetentionMasterSetupRole` (`DetentionSetupParticipants`) | Resets all Detention participant records (16-byte stride via `0x00165EF0`) and clears DetentionMaster scoring/role state words | 28 | `src/game_patches.cpp` (syncs `0x003CD8F4` bus slot across online peers before participant setup) | **4 / 4 checked, 0 mismatched** (Dennison Detention, 2500 frames) |

Note on nested `EOD_DIFF`: `0x00191548` and in-race calls to `0x00190A38` execute inside `0x00177718`. Because `diff_run` suppresses nested comparisons while an outer `diff_run` is active, `0x00177718` was verified in its own pass (`EOD_DIFF=00177718,...`) and `0x00190A38`, `0x00191548`, and `0x00165E10` were verified directly in dedicated passes without `0x00177718` in `EOD_DIFF`. All runs used `EOD_HEAP_CHECK=1` and exited cleanly with code `0` and **0 heap errors**.

---

## Verified Globals, Struct Offsets & Constants

### `WorldStepTick` (`0x00177718`)
- `0x003CDA8C` (`int32_t`): `g_RaceTick` — incremented by `+1` via `RaceSessionInitPhase` (`0x00176698`) at the end of every unpaused tick.
- `0x003CDAC4` (`int32_t`): `g_WorldSimPaused` — nonzero pauses physics/entity stepping and tick advancement (only 30Hz effects `0x00180A00` still tick on odd half-rate phases).
- `0x003CDADC` (`uint32_t`): `g_WorldProfileTag` — tag handle passed to profiler begin/end stubs (`0x00335D08` / `0x00335D00`).
- `0x003CDAE4` (`int32_t`): `g_ActiveDynamicEntityCount` — number of active dynamic entities collected into `0x003CDBE8[]` this tick.
- `0x003CDAE8` (`uint32_t`): `g_WorldHalfRateParity` — toggled `^= 1` every tick; drives the 30Hz (`0x3D08AB86` = `0.033366702f` = `2 / 59.94` s) effects/debris pass `0x00180A00`. Also cleared to `0` by `netplay_trace.cpp` on aligned race-start epochs.
- `0x003CDBE8` (`uint32_t[]`): `g_ActiveDynamicEntities` — scratch pointer table of entities passing both `ent->vtable_at_0x98[+0x40](ent)` and `ent->vtable_at_0x98[+0x3C](ent)`, consumed by `0x00177CA0` and `0x00178258`.
- `0x003CE3EC` (`uint32_t`): `g_WorldPostStepFlag` — cleared to `0` after `0x00178258`.
- `0x00447404` (`uint32_t*`): `g_WorldEntityArrayPtr` — pointer array of all registered world entities.
- `0x00447408` (`int32_t`): `g_WorldEntityCount` — count of registered world entities.
- `0x00447448` / `0x0044744C` and `0x00447450` / `0x00447454`: current and previous-tick counter snapshots copied before `0x00177B40(0)`.
- `0x004474A8` (`uint32_t`) & `0x004494B8` (`uint32_t`): per-tick scratch flags cleared before subsystem and mid-step updates.
- Entity vtable indirection: `ent + 0x98` -> vtable pointer; `+0x40` (`PreTick`), `+0x3C` (`IsActiveDynamic`).

### `CarSetActiveDriver` (`0x00190A38`)
- Signature: `void CarSetActiveDriver(uint32_t wrapper, uint32_t new_driver)` (`$a0`, `$a1`).
- `wrapper` (`car + 0x1C1C`):
  - `+0x08`: pace-lap autopilot driver pointer
  - `+0x0C`: human/player driver pointer
  - `+0x10`: currently active driver pointer (compared against `new_driver`; no-op if unchanged)
- Driver vtable (`*(uint32_t*)driver`):
  - `+0x30`: `OnDeactivate(prev_driver)` (called with `$v0 = fn_ptr`, `$ra = 0x00190A74`)
  - `+0x2C`: `OnActivate(new_driver)` (called with `$v0 = fn_ptr`, `$ra = 0x00190A8C`)
- Exact `$v0` detail proven by `EOD_DIFF`: the original MIPS loads the virtual method address into `$v0` (`R[2]`) before `jalr $v0`. When a driver's `OnActivate` is a `void` no-op (e.g., `0x00139188`), `$v0` retains that target function address on return.

### `HumanReplayRecordTick` (`0x00191548`)
- Signature: `void HumanReplayRecordTick(uint32_t replay_input, uint32_t car)` (`$a0`, `$a1`).
- `replay_input` struct:
  - `+0x04`: live input controller pointer (vtable queries: `+0x0C` steer, `+0x10` throttle, `+0x14` brake, `+0x18` handbrake, `+0x1C` clutch, `+0x20` gear, `+0x2C` btn15, `+0x30` btn12, `+0x34` btn13, `+0x38` btn14)
  - `+0x08`: pointer to current tick's 6-byte record (`buf_base + clamp(g_RaceTick, 0, max_ticks - 1) * 6`)
  - `+0x0C`: high-water recorded tick count (mirrored to global `0x00482560`)
  - `+0x24`: base address of 6-byte replay record array
  - `+0x2C`: buffer capacity in ticks
- `car + 0x1B64` (`float`): steering speed parameter passed in `$f12` to `ctrl->vtable[+0x0C]`.
- 6-byte record layout (`rec_ptr`):
  - `+0x00` (`uint16_t`): steering `[-3.0f, +3.0f]` (`0xC0400000`, span `6.0f` = `0x40C00000`) normalized to `[0, 1]` and quantized with `65535.0f` (`0x477FFF00`) + `0.5f` (`0x3F000000`) via `fcvtw_ps2`.
  - `+0x02` (`uint8_t`): throttle `[0, 1]` quantized with `255.0f` (`0x437F0000`) + `0.5f`.
  - `+0x03` (`uint8_t`): brake `[0, 1]` quantized with `255.0f` + `0.5f`.
  - `+0x04` (`uint16_t` bitfield):
    - bits `[3:0]`: handbrake (`q8 >> 4`)
    - bits `[7:4]`: clutch (`q8 & 0xF0`)
    - bits `[11:8]`: `((gear + 1) & 0xF)`
    - bit `12`: `vtable[+0x30] != 0`
    - bit `13`: `vtable[+0x34] != 0`
    - bit `14`: `vtable[+0x38] != 0`
    - bit `15`: `vtable[+0x2C] != 0`
- Replay playback end: when `IsReplayPlaybackActive()` (`0x00203238`) is nonzero and `tick_idx >= recorded_count - 1`, calls `RequestPhysicsPause()` (`0x00176710`) and tail-calls `RaceSessionInitPhase(g_RaceTick)` (`0x00176698`).

### `DetentionMasterSetupRole` (`0x00165E10`)
- Signature: `void DetentionMasterSetupRole(uint32_t master)` (`$a0`).
- `0x003CD8F4` (`int32_t`): global Detention bus slot (overridden by `game_patches.cpp` before `0x00165E10` so all online peers protect and score the same bus).
- `DetentionMaster` struct:
  - `+0x04`: participant record array base (16-byte stride `0x10`)
  - `+0x08`: `int32_t` participant count
  - `+0x0C`: `int32_t` keep-score flag passed in `$a1` to `0x00165EF0` (which clears `entry+0x04`, `entry+0x0C`, and when `keep_flag == 0`, clears `entry+0x08`)
  - `+0x10`, `+0x14`, `+0x18`, `+0x1C`: DetentionMaster role/scoring state words cleared to `0`

---

## Call Graphs

- `0x00177718` (`WorldStepTick`) -> `0x00335D08`, `0x00175958`, `0x001792E8`, `0x00177B40(0)`, `0x0021F140`, `0x002CE130`, `0x001B5840` (`TimedDamageFeatUpdate`), `0x00173120`, `0x00202FB0`, `0x00268CD0`, `0x001BFA30`, [`0x001DA9F0`, `0x001F0D78`, `0x001DA720`, `0x001E8AD8`, `0x00138170` / `0x001380B0`, `0x001779C0`, `0x001793B8`, `0x00180A00(2/59.94f)`, entity `vt[+0x40]` & `vt[+0x3C]`, `0x00177CA0`, `0x00178258`], `0x001B6070`, `0x002CE128`, `0x00176698` (`RaceSessionInitPhase`), `0x0012D6A0`, tail-call `0x00335D00`.
- `0x00190A38` (`CarSetActiveDriver`) -> indirect `prev_driver->vt[+0x30]`, indirect `new_driver->vt[+0x2C]`.
- `0x00191548` (`HumanReplayRecordTick`) -> `0x00203248` (`IsReplayRecordingActive`), indirect controller vtable (`+0x0C`, `+0x14`, `+0x10`, `+0x18`, `+0x1C`, `+0x20`, `+0x30`, `+0x34`, `+0x38`, `+0x2C`), `0x00203238` (`IsReplayPlaybackActive`), `0x00176710` (`RequestPhysicsPause`), tail-call `0x00176698` (`RaceSessionInitPhase`).
- `0x00165E10` (`DetentionMasterSetupRole`) -> `0x00165EF0` (6-instruction leaf participant reset, called `count` times).

---

## Key `WorldStepTick` (`0x00177718`) Callees Recommended for the MMO Dedicated-Server Wave

For a 24/7 headless authoritative MMO dedicated server, `WorldStepTick` cleanly separates **server-authoritative simulation** from **client-only presentation** (HUD, audio, camera, particles). Recommended next decompilation wave in priority order:

1. **`0x00177CA0` (`DynamicEntitiesPhysicsStep`, 365 instrs)** — iterates `g_ActiveDynamicEntities` (`0x003CDBE8`), drives rigid-body / vehicle / constraint integration and collision resolution. Highest-value target for deterministic server-side physics and >16-car scaling.
2. **`0x00178258` (`DynamicEntitiesPostStep`, 72 instrs)** — compact coordinator calling `0x001ADDD0` (or `0x001BEF40` in replay), entity `vt[+0x1C]`, `0x001BF3F8(entities, count)` (state/snapshot capture), and entity `vt[+0x20]`. Ideal hook point for authoritative server snapshot serialization.
3. **`0x00173120` (`EventModeRulesStep`, 469 instrs)** & **`0x001B5840` (`TimedDamageFeatUpdate`, 523 instrs)** — per-tick contest/event rule evaluation (laps, zones, Detention/Gauntlet/Derby/Flag scoring) and timed feat/damage rules. Required for authoritative 26-mode MMO event state.
4. **`0x001779C0` (`EntityMatrixOrthonormalizeRoundRobin`, 96 instrs)** & **`0x00177B40` (`SimSnapshotHistoryStep`, 87 instrs)** — small, self-contained math/history helpers; quick wins for eliminating PS2-fixed-size entity array assumptions.
5. **`0x001792E8` (51 instrs) / `0x001793B8` (71 instrs)** — track/race state pre-step and mid-step updates.
6. **`0x00268CD0` (`WorldPropsAndTriggersStep`, 251 instrs)** — dynamic track props/obstacles and world triggers (needed for syncing breakable props & ring-out zones).
7. **Client-only skip candidates on headless server:** `0x001DA9F0`/`0x001DA720` (camera), `0x001380B0`/`0x00138170` (HUD), `0x00180A00` (1319 instrs, 30Hz debris/particle FX), `0x002CE130`/`0x002CE128` (audio).

---

## STAR Handoff

- **Situation**: The 4 core online simulation-step, driver-switch, replay-input, and Detention setup functions (`0x00177718`, `0x00190A38`, `0x00191548`, `0x00165E10`) were wrapped directly around recompiled `f_XXXXXXXX_orig()` originals in `src/netplay_trace.cpp` and `src/game_patches.cpp`.
- **Task**: Decompile all 4 functions into readable, malleable, bit-exact `diff_run`-backed C++ in `src/decomp_online_sim.cpp` while preserving 100% of existing netplay hooks and verifying 0 `EOD_DIFF` mismatches and 0 heap errors (`EOD_HEAP_CHECK=1`).
- **Action**: Implemented `src/decomp_online_sim.h` and `src/decomp_online_sim.cpp` with named constants for all globals, struct offsets, vtable slots, and EE float bit patterns; rewired `src/netplay_trace.cpp` and `src/game_patches.cpp` to call `eod_online_sim_XXXXXXXX()`; added the `eod_online_sim_00165E10` forwarding stub to `tools/tests/live_cheats_test.cpp`; updated `config/hle.txt` comments; built `eod` with zero warnings in touched files; ran 4 live headless `EOD_DIFF` + `EOD_HEAP_CHECK=1` passes across 3 tracks and 3 event types (Woodly Jump Race, Dundas No-Rules Race, Dennison Detention). Caught and fixed a `$v0` register preservation edge case in `0x00190A38` where `jalr $v0` leaves the vtable function pointer in `$v0` when `OnActivate` is a `void` no-op (`0x00139188`).
- **Result**: All 4 functions verified with **0 mismatches** and **0 heap errors** (`0x00177718`: 1,097/1,097; `0x00190A38`: 663/663; `0x00191548`: 591/591; `0x00165E10`: 4/4).
- **Next decisive test**: Decompile `0x00177CA0` (`DynamicEntitiesPhysicsStep`) and `0x00178258` (`DynamicEntitiesPostStep`) as the next MMO dedicated-server wave, and run a replay-playback `EOD_DIFF=00191548` pass to exercise the end-of-replay tail-call branch (`0x00191848` -> `0x00176698`).
