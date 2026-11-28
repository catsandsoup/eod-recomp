# MMO Headless Dedicated-Server Simulation Core Decompilation

Behaviorally exact C++ decompilation of the 6 authoritative simulation-core functions called inside `WorldStepTick` (`0x00177718`) from `SLUS_209.10`, implemented in [`src/decomp_mmo_sim.cpp`](../src/decomp_mmo_sim.cpp) and [`src/decomp_mmo_sim.h`](../src/decomp_mmo_sim.h).

All 6 functions are registered as `wrap` entries in [`config/hle.txt`](../config/hle.txt) and execute via `diff_run(0xXXXXXXXXu, decomp_XXXXXXXX, f_XXXXXXXX_orig)`, verified bit-exact with `EOD_DIFF` and `EOD_HEAP_CHECK=1` across 3 gameplay scenes (Woodly Jump Race, Dundas No-Rules Race, Dennison Battle 8 AI).

---

## Function Ledger & `EOD_DIFF` Verification (`EOD_HEAP_CHECK=1`, 1800 frames)

| Address | Symbol | Role | `rec_*.c` Lines | Direct Callees | `afton` (Woodly Jump) | `dundas` (No-Rules) | `battle` (8 AI) | Total Checked / Mismatched |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| `0x001779C0` | `EntityMatrixOrthonormalizeRoundRobin` | Round-robin Gram-Schmidt orthonormalization of one active dynamic entity's 3x3 orientation matrix (`+0x64`) per tick | 98 | 0 (+1 vtable) | 107 / 0 | 120 / 0 | 134 / 0 | **361 / 0** |
| `0x00177B40` | `SimSnapshotHistoryStep` | Shifts the 8-slot x `0x108`-byte simulation snapshot history ring (`0x0044A4C0`) when oldest entry > 5.0s, time rewinds, or forced | 84 | 1 (`0x003A5FD0`) | 107 / 0 | 120 / 0 | 135 / 0 | **362 / 0** |
| `0x00177CA0` | `DynamicEntitiesPhysicsStep` | Main dynamic-entity collision step over `g_ActiveDynamicEntities` (`0x003CDBE8`): collider pre-step, spatial-hash broadphase (`0x001D57A8`), and dual static track quadtrees (`0x004473FC` / `0x00447400`) | 362 | 1 (`0x001D57A8`) (+vtable) | 106 / 0 | 120 / 0 | 134 / 0 | **360 / 0** |
| `0x00178258` | `DynamicEntitiesPostStep` | Post-physics contact flush (`0x001ADDD0`), entity `vt[+0x1C]`, post-physics snapshot capture (`0x001BF3F8`) vs. replay playback (`0x001BEF40`), and final entity `vt[+0x20]` sync | 74 | 4 (+vtable) | 106 / 0 | 120 / 0 | 134 / 0 | **360 / 0** |
| `0x001792E8` | `TrackRacePreStep` | Applies pending pause/unpause requests (`0x003CDAC8` -> `0x003CDAC4`), respects locked replay-end state (`0x00203238`/`0x001918D0`), and pauses 5 channels (`0x00333398`) on pause entry | 50 | 3 | 107 / 0 | 120 / 0 | 135 / 0 | **362 / 0** |
| `0x001793B8` | `TrackRaceMidStep` | Drains the 20-slot deferred event/collision pointer queue (`0x00447458`) via `0x00189E80` when `0x0044743C != 0`, then clears `0x0044743C` | 73 | 1 (`0x00189E80`) | 107 / 0 | 120 / 0 | 134 / 0 | **361 / 0** |

All 3 runs exited with code `0`, **2,166 / 2,166 total checked calls**, **0 mismatches**, and **0 heap errors**.

---

## Verified Globals, Struct Offsets & Constants

### Global Simulation Tables & Flags
- `0x003CDAC4` (`int32_t`): `g_WorldSimPaused` — nonzero when world simulation is paused.
- `0x003CDAC8` (`int32_t`): `g_PauseRequest` — pending pause state transition (`0` = none, `1` = pause, `2` = unpause); cleared to `0` at the end of `TrackRacePreStep`.
- `0x003CDACC` (`float`): `g_SimTimeSeconds` — current simulation clock in seconds (used by `SimSnapshotHistoryStep` for 5.0s expiry and rewind detection).
- `0x003CDAE4` (`int32_t`): `g_ActiveDynamicEntityCount` — number of active dynamic entities in `g_ActiveDynamicEntities`.
- `0x003CDBE8` (`uint32_t[]`): `g_ActiveDynamicEntities` — scratch array of active dynamic entity pointers built by `WorldStepTick` (`0x00177718`) and consumed by `DynamicEntitiesPhysicsStep` (`0x00177CA0`) and `DynamicEntitiesPostStep` (`0x00178258`).
- `0x004473FC` (`StaticQuadtree*`): `g_StaticQuadtreeMain` — primary static track collision quadtree.
- `0x00447400` (`StaticQuadtree*`): `g_StaticQuadtreeAux` — optional secondary static collision quadtree (traversed when non-null).
- `0x00447404` (`uint32_t*`): `g_WorldEntityArrayPtr` — pointer array of all registered world entities.
- `0x00447408` (`int32_t`): `g_WorldEntityCount` — total count of registered world entities.
- `0x00447428` (`int32_t`): `g_OrthonormalizeCursor` — round-robin index into `g_WorldEntityArrayPtr`, incremented once per tick by `EntityMatrixOrthonormalizeRoundRobin`.
- `0x0044743C` (`int32_t`): `g_DeferredEventPending` — non-zero when `g_DeferredEventSlots` has entries to drain in `TrackRaceMidStep`.
- `0x00447458` (`uint32_t[20]`): `g_DeferredEventSlots` — 20-slot (`0x00447458..0x004474A4`, 80 bytes) deferred event pointer table, immediately preceding `g_WorldStepScratchFlag` (`0x004474A8`).
- `0x0044A4C0` (`SimSnapshotEntry[8]`): `g_SimSnapshotHistory` — 8-entry ring/shift buffer (`0x108` = 264 bytes per entry; tail slot 7 at `0x0044ABF8`):
  - `+0x00` (`int8_t`): active/occupied flag
  - `+0x100` (`float`): simulation timestamp in seconds

### Entity & Collider Struct Layout
- `ent + 0x10` (`uint32_t[]`): inline array of collider object pointers attached to the entity.
- `ent + 0x20` (`int32_t`): `num_colliders` — active collider count.
- `ent + 0x64` (`float[9]`): 3x3 world orientation matrix (row 0 = X at `+0x64..+0x6C`, row 1 = Y at `+0x70..+0x78`, row 2 = Z at `+0x7C..+0x84`).
- `ent + 0x88` (`float[3]`): world position vector (`x` at `+0x88`, `y` at `+0x8C`, `z` at `+0x90`), passed directly to `BroadphaseQueryCells` (`0x001D57A8`) and scaled by `10.0f` (`0x41200000`) to integer decimeters for static quadtree descent.
- `ent + 0x98` (`uint32_t`): entity physics/simulation vtable pointer:
  - `vt + 0x1C`: `PostPhysicsUpdate(ent)` — called per active dynamic entity in `DynamicEntitiesPostStep` (live simulation path)
  - `vt + 0x20`: `PostStepFinalize(ent)` — called per active dynamic entity at the end of `DynamicEntitiesPostStep` (both live and replay paths)
  - `vt + 0x3C`: `bool IsActiveDynamic(ent)` — checked in `WorldStepTick` and `EntityMatrixOrthonormalizeRoundRobin`
  - `vt + 0x40`: `bool IsDynamicPairFilter(ent)` — used in `DynamicEntitiesPhysicsStep` Pass 2A for `(ent, other)` unordered pair deduplication (`ent < other` skips calling `vt[+0x40]`; if `ent >= other` and `other->vt[+0x40](other) != 0`, skip pair because `(other, ent)` was already processed)
- `col + 0x1C` (`uint32_t`): collider vtable pointer:
  - `col_vt + 0x0C`: `Collider::PreStep(col)` — updates world-space bounds/shapes in `DynamicEntitiesPhysicsStep` Phase 1
  - `col_vt + 0x14`: `Collider::CollideWith(col, other_col)` — narrow-phase collision test & contact generation in `DynamicEntitiesPhysicsStep` Passes 2A, 2B, 2C

### Static Track Quadtree Layout (`0x004473FC` / `0x00447400`)
- `qt + 0x08` (`uint32_t`): `props_base` — array of `0x108`-byte (264-byte) static collision prop records; each record stores its collider pointer at `+0x3C` (`+60`).
- `qt + 0x0C` (`int16_t*`): `index_table` — indirection table mapping node index ranges `[start, end)` to prop indices in `props_base`.
- `qt + 0x10` (`QuadtreeNode*`): array of 8-byte nodes:
  - `+0x00` (`int16_t`): `start_idx`
  - `+0x02` (`int16_t`): `end_idx`
  - `+0x04` (`int16_t`): `child_base` (`0` = leaf; otherwise 4 children at `child_base + {0, 1, 2, 3}` indexed by `(mid_z < qz ? 2 : 0) | (mid_x < qx ? 1 : 0)`)
- Root domain: `[-40000000, +40000000]` (`0xFD9DA600 .. 0x02625A00`) on both X and Z axes (decimeters).

---

## STAR Handoff — Next Hottest Physics / Car-Pool Callees

With all 6 outer simulation-core functions inside `WorldStepTick` now decompiled and verified bit-exact, the next highest-value targets for the headless authoritative MMO server and >16-car scaling are the direct callees of `0x00177CA0` and `0x00178258`:

1. **`0x001D57A8` (`BroadphaseQueryCells`, 154 lines, 0 callees)** — called once per active dynamic entity inside `0x00177CA0` Pass 2A; maps `ent+0x88` world position into up to 4 spatial-hash broadphase grid cell pointers. Pure leaf function, zero callees, critical for scaling dynamic entity count beyond the PS2's fixed grid/pool limits.
2. **`0x001ADDD0` (`PostPhysicsContactFlush`, 4 lines, 1 callee)** — thin dispatcher called at the start of `0x00178258` live path; its single callee solves/flushes accumulated contacts from `0x00177CA0`.
3. **`0x001BF3F8` (`SnapshotCaptureEntities`, 6 lines, 0 callees)** & **`0x001BEF40` (`ReplayApplyEntities`, 194 lines, 9 callees)** — the exact post-physics state capture / playback pair in `0x00178258`; ideal anchor for authoritative MMO server snapshot serialization and client state reconciliation.
4. **`0x001BFA30` (`IsReplayOrOverrideActive`, 6 lines, 0 callees)** — tiny leaf predicate called by both `WorldStepTick` and `DynamicEntitiesPostStep`.
5. **`0x00189E80` (`ProcessDeferredEventSlot`, 51 lines, 6 callees)** — drains each non-null slot in `TrackRaceMidStep` (`0x001793B8`).
6. **Virtual Physics & Collider Targets** dispatched from `0x00177CA0` / `0x00178258`:
   - `ent->vt[+0x1C]` (`PostPhysicsUpdate` — rigid-body / vehicle integration)
   - `ent->vt[+0x20]` (`PostStepFinalize` — post-step transform & wheel/chassis sync)
   - `col->vt[+0x0C]` (`Collider::PreStep`) and `col->vt[+0x14]` (`Collider::CollideWith`)
