# Physics, Collision, Recovery & Timed-Damage Decompilation

Behaviorally exact C++ decompilation of the 4 core vehicle recovery, timed-damage feat / race director update, wheel ground query, and vehicle physics step + dual-probe wheel sampling functions from `SLUS_209.10`, implemented in `src/decomp_physics_collision.cpp` and `src/decomp_physics_collision.h`.

All 4 functions preserve 100% of existing mod, mutator, recovery, and custom-map hooks (`src/hle_recovery.cpp`, `src/game_patches.cpp`, `src/custom_map_physics.cpp`), replacing direct `f_XXXXXXXX_orig()` calls with `diff_run`-backed `eod_physics_XXXXXXXX()` entry points.

---

## Function Ledger

| Address | Symbol | Role | Instructions | Existing Hook Owner | `EOD_DIFF` Calls / Mismatches (`EOD_HEAP_CHECK=1`) |
| --- | --- | --- | ---: | --- | --- |
| `0x001B8370` | `CarUpdateOutOfBoundsRecovery` (`CarRecoveryCountdownUpdate`) | Evaluates wrong-way, off-track, rollover, and stuck (`<4` wheels) recovery sub-timers, sets HUD countdown deadline, and triggers auto-reset (`0x001B63E8`) | 348 | `src/hle_recovery.cpp` (manual-only human countdown recovery + reversed-lane mutator suppression) | **905 checked / 1,493 calls, 0 mismatched** (Woodly Jump Race: 317/317; Dundas No-Rules Race: 588/1,176) |
| `0x001B5840` | `TimedDamageFeatUpdate` | Per-tick race director & feat update: spectator cycling, finish grace timer, inline Deadly Ambition timed-damage feat, start countdown release, time-limit expiration, standings | 523 | `src/game_patches.cpp` (invincibility + per-slot `FB_DEADLY_AMBITION` mutator gate + `on_world_tick`) | **66 checked / 108 calls, 0 mismatched** (Woodly Jump Race: 23/23; Dundas No-Rules Race: 43/85) |
| `0x00212500` | `WheelGroundQuery` | Queries static track SCOL ground (`0x00227950`) and dynamic/overlay collision (`0x00217240`), keeping whichever hit has the higher Y coordinate | 64 | `src/custom_map_physics.cpp` (merges portable custom-map triangle ground samples into native wheel query) | **566 checked / 2,263 calls, 0 mismatched** (Woodly Jump Race dedicated inner pass, `EOD_DIFF_EVERY=4`) |
| `0x001DE7B0` | `ChassisSweptBoxCollide` (`VehiclePhysicsStep`) | Uploads VU0 vehicle microprogram via VIF0 DMA, runs `eod_vehicle_physics_step` per car pipelined with 4-wheel fore/aft (`±0.5m`) dual-probe ground sampling via `WheelGroundQuery` (`0x00212500`) | 576 | `src/custom_map_physics.cpp` (scopes `wheel_mesh` mount and resolves swept chassis boxes against portable map triangles) | **65 checked / 107 calls, 0 mismatched** (Woodly Jump Race: 23/23; Dundas No-Rules Race: 42/84) |

Note on nested `EOD_DIFF`: `0x001DE7B0` calls `0x00212500` 8 times per active vehicle per physics step (2 fore/aft probes × 4 wheels). Because `diff_run` suppresses nested comparisons while an outer `diff_run` is active, `0x00212500` was verified directly in a dedicated second pass (`EOD_DIFF=00212500`, `EOD_DIFF_EVERY=4`) without `0x001DE7B0` in `EOD_DIFF`. All 3 headless runs used `EOD_HEAP_CHECK=1`, exited with code `0`, and reported **0 mismatches** and **0 heap errors**.

Unit tests verified (`eod_recovery_test`, `eod_live_cheats_test`, `custom_map_physics_test`, `mutator_engine_test`): all 4 built and passed.

---

## Verified Globals, Struct Offsets & Bit-Exact Details

### `CarUpdateOutOfBoundsRecovery` (`0x001B8370`)
- Signature: `void CarUpdateOutOfBoundsRecovery(uint32_t timer, uint32_t person, float now)` (`$a0`, `$a1`, `$f12`).
- Globals:
  - `0x00484FEC` (`int32_t`): `g_EventTypeFlags` (`0x4214` special mode check, `0x00402112` reversed lanes, `0x00020000` arena no-wrong-way bit, `0x00042119` special off-track).
  - `0x00485098` (`uint32_t`): `g_RecoveryAutoResetEnabled` (`g_RecoveryConfigFlag` — toggles automatic relocation and HUD countdown deadline).
  - `0x0048509C` (`uint32_t`): `g_RecoveryCheckEnabled`.
  - `0x004850B4` (`uint32_t`): `g_RaceModeControllerPtr` (vtable `+0x14` recovery eligibility, `+0x30` wrong-way override).
  - `0x004852C4` (`uint32_t`): `g_RecoverySuppressedFlag`.
- `timer` struct (`0x54` bytes):
  - `+0x00`, `+0x10`, `+0x20`, `+0x30`: 16-byte `RecoveryConditionTimer` blocks for wrong-way, off-track, rollover (`car+0x74 < 0.1f`), and stuck (`car+0x1BB8 < 4 && car+0x1B64 < 4/3 m/s`).
  - `+0x40`: master recovery countdown (`+0x44` start time, `+0x48` duration, `+0x4C` active flag; `0.9f` grace window).
  - `+0x50`: linked-list head of condition timers (`node+0x00` next, `+0x04` start, `+0x08` duration, `+0x0C` active).
- `person` struct:
  - `+0x04`: `CarBase*` (magic `'PCAR'` = `0x50434152` at `+0x0C`; `+0x74` up-vector Y; `+0x104` forward vector; `+0x1B5C` forward speed compared against `20/9` m/s = `0x400E38E4`; `+0x1B64` speed; `+0x1BB8` grounded wheel count).
  - `+0xC0` (`+192`): HUD recovery warning flag (`0xFFFFFFFF` active, `0` clear).
  - `+0xC4` (`+196`): SELECT manual-recovery / countdown deadline (`-1.0f` when inactive).
- Exact `$v0` exit paths: `0` (mode controller rejected), `0x50434152` (non-`PCAR`), `0x00480000` (early bail or not triggered), `-1` (triggered, warning active), or callee return value from `0x001B63E8`.

### `TimedDamageFeatUpdate` (`0x001B5840`)
- Globals:
  - `0x003C4190`: `g_GlobalCheatFlags` (bit `0x01` master + bit `0x40` Deadly Ambition timed-damage feat, adding `0x39DAAC09` = `0.0004170838f` per tick to `car+0x1BEC`, wrecking via `driver->vt[+0x34](driver, -1)` at `1.0f`).
  - `0x003CDACC`: `g_GlobalRaceTimeSec` (`float`).
  - `0x003CE910`: `g_GlobalCamCycleDelta` (cleared on read; tail-calls `0x001D4190` in replay mode).
  - `0x0048522C` / `0x00485230`: `g_DriverCount` / `g_DriverArray[]`.
  - `0x00485280`: `g_SelectedDriverIndex`; `0x004852A0`: `g_HumanPlayerCount`; `0x004852F0`: `g_StandingsOrder[]`.
  - `0x00485390`..`0x004853A0`: countdown deadline, race time limit (`0x003F0FF4` NaN sentinel), finish grace deadline (`+100000.0f` = `0x47C35000`), finish grace active, countdown armed.
- Bit-exact exit detail: when `g_DriverCount > 0`, the final loop back-edge delay slot leaves `$v0 = 0x00485230` (`kDriverArrayBase`); when `g_DriverCount <= 0`, `$v0 = g_DriverCount`.

### `WheelGroundQuery` (`0x00212500`)
- Signature: `int32_t WheelGroundQuery(uint32_t scol_cache, uint32_t pos_xyz, uint32_t normal_out, uint32_t material_out)`.
- Calls `0x00227950` (static SCOL mesh query); restores `int16_t` triangle cache at `scol_cache` and returns `0` on miss.
- Computes dynamic collision bitmask `0x00227830(scol_cache) & 0x00217220()` and queries `0x00217240`. Keeps the dynamic/overlay hit only if `saved_y < new_y`; otherwise restores `pos_xyz+4`, `normal_out[0..11]`, and `material_out`, returning `1`.

### `ChassisSweptBoxCollide` / `VehiclePhysicsStep` (`0x001DE7B0`)
- Uploads VU0 vehicle physics microprogram (`0x003BC590`) via VIF0 DMA channel 0 (`0x10008000`..`0x10008030`, `0x1000E010`/`0x1000E020`, wait `0x001E0830`).
- Global physics SoA arrays:
  - `0x00494D28`: `g_PhysicsCarCount`
  - `0x00494D30`: `g_PhysicsBodyArray` (stride `0xB0` = 11 qwords; `+0x30` = chassis forward axis)
  - `0x00494D34`: `g_PhysicsWheelArray` (stride `0x1F0` = 31 qwords; `+0x40..+0x7F` = 4 world hub positions, `+0x110..+0x14F` = 4 contact normals + height, `+0x150..+0x18F` = 4 contact points)
  - `0x00494D38`: `g_PhysicsParamArray` (stride `0x180` = 24 qwords)
  - `0x00494D3C`: `g_PhysicsDisabledArray` (stride `4`; nonzero skips VU0 collect writeback)
  - `0x00494D18`: `g_PhysicsScolCacheArray` (stride `0x14`: `+0x00` SCOL cache header, `+0x04..+0x13` 4 wheel material IDs)
- Pipelined loop: stages car `i` into VU0 (`[0..11)` body, `[11..42)` wheels, `[42..66)` params), invokes `eod_vehicle_physics_step()`, runs 4-wheel dual-probe ground sampling (`hub ± 0.5 * axis_z`) for car `i - 1` via `0x00212500` (fallback on miss: normal `(0, 1, 0)`, `y = -100.0f` = `0xC2C80000`), collects car `i` back if enabled, and runs an epilogue ground-sample pass for car `count - 1`.
- Bit-exact details:
  - `wheel_slot + 0x11C` stores `fmul_ps2(fadd_ps2(pa_y, pb_y), 0.5f)` whereas `wheel_slot + 0x154` stores `fadd_ps2(fmul_ps2(pa_y, 0.5f), fmul_ps2(pb_y, 0.5f))`.
  - `wheel_slot + 0x15C` (`pos_q.uw[3]`) is the 32-to-64 sign-extension of `avg_pz` (`0xFFFFFFFF` when `avg_pz < 0`, `0` otherwise), matching the original MMI `pextlw` + `pcpyld` packing.

---

## Call Graphs

- `0x001B8370` (`CarUpdateOutOfBoundsRecovery`) -> `ctrl->vt[+0x14]`, `0x00210F98`, `0x001741C0`, `0x00179888`, `0x00179868`, `0x001F0D78`, `ctrl->vt[+0x30]`, `0x0013B0D0`, `0x001B63E8`.
- `0x001B5840` (`TimedDamageFeatUpdate`) -> `0x001BFA30`, `0x001D3F98`, `0x001D4190`, `0x001766E8`, `0x001BAD10`, `0x00210F98`, `0x002CE180`, `0x00179920`, `driver->vt[+0x10/+0x34/+0x3C]`, `0x00210F88`, `0x001B7198`, `start_hook->vt[+0x1C]`, `0x0018A950`, `0x00201BB8`, `0x001767A8`, `0x001741D8`, `0x00257BB8`, `0x001BB538`, `0x001BB708`, `0x001741C0`, `0x0039BD40`, `0x0039BC30`, `0x001B7E58`, `0x001BB8A0`, `0x001BB848`, `0x001BB850`.
- `0x00212500` (`WheelGroundQuery`) -> `0x00227950`, `0x00227830`, `0x00217220`, `0x00217240`.
- `0x001DE7B0` (`ChassisSweptBoxCollide` / `VehiclePhysicsStep`) -> `0x001E0830`, `eod_vehicle_physics_step()`, `0x00212500` (`WheelGroundQuery`, 8× per vehicle).

---

## Key Collision/Physics Callees Recommended for the MMO Dedicated-Server Physics Wave

1. **`0x00227950` (`ScolQueryStaticGround`) & `0x00227830` (`ScolGetSectorDynamicMask`)** — Static track collision BVH/triangle ground sampler; decompiling these unlocks pure C++ server-side terrain queries without EE RAM SCOL caches and supports >20 cars with zero PS2 pool caps.
2. **`0x00217240` (`QueryDynamicOverlayGround`) & `0x00217220` (`GetGlobalDynamicCollisionMask`)** — Dynamic bridges, ramps, and moving prop surface collision queried every wheel tick; required for authoritative jump-race and stunt-arena servers.
3. **`0x00177CA0` (`DynamicEntitiesPhysicsStep`)** — Top-level physics coordinator calling `0x001DE7B0`, car-to-car narrowphase/impulse solver, and rigid-body constraints; the primary hub for scaling beyond the 20-car PS2 physics SoA (`0x00494D28`).
4. **`0x001B63E8` (`CarTriggerAutoRecovery`) & `0x0013B0D0` (`TrackSplineIsWrongWay`)** — Server-authoritative vehicle respawn placement and spline progress/direction checks (critical for anti-cheat and MMO free-roam/event recovery).
5. **`0x001B7E58` (`UpdateDriverStandings`) & `0x001BAD10` (`DriverStatusPreUpdate`)** — Authoritative race standings, lap/checkpoint progression, and finish eligibility across all 26 event modes.

---

## STAR Handoff

- **Situation**: Four core physics/collision/recovery functions (`0x001B8370`, `0x001B5840`, `0x00212500`, `0x001DE7B0`) were wrapped directly around recompiled `f_XXXXXXXX_orig()` originals in `src/hle_recovery.cpp`, `src/game_patches.cpp`, and `src/custom_map_physics.cpp`.
- **Task**: Decompile all 4 functions into clean, readable, malleable, bit-exact `diff_run`-backed C++ in `src/decomp_physics_collision.cpp`/`.h`, preserve 100% of existing recovery/mutator/custom-map hooks, keep all unit tests green, and verify 0 `EOD_DIFF` mismatches and 0 heap errors (`EOD_HEAP_CHECK=1`).
- **Action**: Implemented `src/decomp_physics_collision.h` and `src/decomp_physics_collision.cpp` with named constants for all globals, SoA strides, thresholds, and EE float bit patterns; rewired `src/hle_recovery.cpp`, `src/game_patches.cpp`, and `src/custom_map_physics.cpp` to call `eod_physics_XXXXXXXX()`; added forwarding stubs to `tools/tests/recovery_test.cpp`, `tests/mutator_engine_test.cpp`, `tools/tests/live_cheats_test.cpp` (also fixing pre-existing Task 1 stubs), and `tests/custom_map_physics_test.cpp`; updated `config/hle.txt` comments; built `eod` and all 4 unit tests cleanly; verified all 4 functions across 3 live headless `EOD_DIFF` + `EOD_HEAP_CHECK=1` passes (Woodly Jump Race outer + inner wheel pass, Dundas No-Rules Race).
- **Result**: All 4 unit tests pass, and all 4 functions achieved **0 mismatches** and **0 heap errors** (`0x001B8370`: 905/1,493; `0x001B5840`: 66/108; `0x00212500`: 566/2,263; `0x001DE7B0`: 65/107).
- **Next decisive test**: Decompile `0x00227950` (`ScolQueryStaticGround`), `0x00217240` (`QueryDynamicOverlayGround`), and `0x00177CA0` (`DynamicEntitiesPhysicsStep`) to complete the authoritative server-side terrain + vehicle physics pipeline.
