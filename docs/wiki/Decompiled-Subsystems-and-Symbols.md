# Decompiled Subsystems & Symbols — Function Addresses, HLE Hooks, `EOD_DIFF`, Physics, Weapons & AI

This page catalogs the reverse-engineered engine subsystems, canonical guest function addresses (`config/symbols.txt`), High-Level Emulation (HLE) hooks (`config/hle.txt`), the **`EOD_DIFF`** runtime equivalence harness, and the mathematical models governing vehicle deformation, explosive chickens, AI bot drivers, career economy, and deterministic netplay in *Test Drive: Eve of Destruction* (`SLUS_209.10`).

---

## 1. The Symbol Database (`config/symbols.txt`) & `EOD_DIFF` Harness

### 1.1 Canonical Symbol Registry (`config/symbols.txt`)
Every verified (`[V]`) or inferred (`[I]`) function and global variable in `SLUS_209.10` is recorded in [`config/symbols.txt`](https://github.com/catsandsoup/eod-recomp/blob/main/config/symbols.txt):
```text
<hex_addr> <SymbolName> [V|I]  # <description> -> docs/<file>.md
```
- **`tools/recomp.py`** embeds symbol names, `[V]`/`[I]` tags, and doc references directly into generated `gen/rec_*.c` function headers and inline `/* SymbolName */` comments at every `jal` call site.
- **`tools/ee_dis.py`** resolves function and global operand names inline during MIPS R5900 disassembly.

### 1.2 The `EOD_DIFF` & `EOD_DIFF_REPLAY` Verification Harness (`runtime/core/diff_harness.cpp`)
To guarantee **zero behavioral drift** when replacing recompiled MIPS functions (`gen/rec_*.c`) with readable C++ (`src/*.cpp` registered as `diff_run` or `wrap` in `config/hle.txt`):
1. Before calling a watched function, `diff_harness` snapshots the entire guest execution state (32 general-purpose 128-bit EE registers, FPU `f0..f31`/`ACC`, VU0 `vf0..vf31`/`vi0..vi15`, 32 MB EE RAM writes, and 16 KB Scratchpad).
2. It runs **both** the readable C++ implementation (`src/*.cpp`) and the original recompiled MIPS function (`gen/rec_*.c`) from the exact same entry state and compares every register and memory write bit-for-bit (`diff_in_original_side()` isolates host-side reserve vector bookkeeping via `HostUndo`).
3. **`EOD_DIFF_REPLAY=<hex_addr>:<n>`:** Re-executes a watched function `n` additional times from its captured live entry state so rarely called functions (like `SkyDestroy 0x223800` or `WheelPoolReset 0x1B3180`) still pass the **Gate D2 requirement (`>= 1,000` verified calls with `0` mismatches)**.

---

## 2. Subsystem Deep Dives & Decompiled Math

### 2.1 Vehicle Physics, 157-Bone Deformation & Detachable Debris Pools (`src/car_damage.cpp`, `src/bone_pose.cpp`, `src/debris_pool.cpp`)
- **6-DOF Rigid Body & Control Registers (`Car` struct):**
  - Steering target: `car + 0x1B6C` (`[-1.0, +1.0]`, `CarSetSteering 0x0018AA80`)
  - Throttle: `car + 0x1B70` (`[0.0, 1.0]`, `CarSetThrottle 0x0018AA70`)
  - Footbrake: `car + 0x1B74` (`[0.0, 1.0]`, `CarSetFootbrake 0x0018AA68`)
  - Clutch engagement: `car + 0x1B78` (`1.0f` coupled, `0.0f` disengaged, `CarSetClutchEngagement 0x0018AA60`)
  - Handbrake: `car + 0x1B7C` (`[0.0, 1.0]`, multiplied by `8.0` in rear-axle physics, `CarSetHandbrake 0x0018AA58`)
  - Gear: `car + 0x1B90` (`-10` automatic forward, `-1` reverse, `0` neutral, `1..4`, `CarSetGear 0x0018AA50`)
- **157-Bone Mesh Crumpling (`CarApplyCollisionDamage 0x0018CA98`):**
  - Each vehicle carries up to **157 deformation bones** in `Bone.mvl` (`bonesurf_1` bones `0..79` mask `0x752D05F`, `bonesurf_2` bones `80..156` mask `0x52E20`), with current 3D dent translations stored at `car + 0x4B8` and uploaded to VU1 `qword 0x90 + bone` by `DentOffsetWrite` (`0x0035DD28`).
  - When `CarContactEvaluateObstacleImpulse` (`0x0017B918`) delivers an impact:
    1. **Nearest Free Bone Search:** Finds the nearest free bone (`word +0x30 == 0`) whose rest position $+ 1.8 \times \text{dent}$ lies within **`1.5 m`** ($d^2 < 2.25$, `0x40100000`) of the contact point and dents it along the impulse via `0x0018C6B8`.
    2. **Radial Spread:** Every bone behind the nearest bone's dent plane shifts along $-\text{impulse}$ by $\min(\text{depth}, \text{behind}) \times \min(0.2 / \max(d^2, 0.1), 1.0)$, clamped to its local dent bounding box (`bone + 0x04..0x18`). The low byte of `dent.z` encodes vertex darkening $|\text{offset}| / \text{length} \times 128 \in [0x20, 0x80]$.
    3. **Dual-Bone Skinning Equation (`VU1 0x02E0` / `native_feed.cpp`):**
       $$\vec{P}' = \vec{P} + w_a \vec{D}_{a} + w_b \vec{D}_{b}, \quad w = f_{\text{skin}} - 32768.0$$
- **8-Row Detachable Panel Table (`0x0044AE68`, stride `0x40` B, states at `car + 0x13E8`):**
  - Initialized per car by `CarSetupDamageAndPanels` (`0x0018AFE0`), which matches each panel's helper socket (`TJRM` node via `0x00205A58`) and origin to the nearest `Bone.mvl` bones (`state[1]`, `state[2]`).
  - The 8 detachable body groups (`car + 0x2AC` visibility mask): **Row 0** Left Door (`0.0762m`), **Row 1** Right Door (`0.0762m`), **Row 2** Hood (`0.1270m`), **Row 3** Decklid/Trunk (`0.0381m`), **Row 4** Front Bumper (`0.0762m`), **Row 5** Rear Bumper (`0.0762m`), **Row 6** Left Front Fender (`0.0762m`), **Row 7** Right Front Fender (`0.0762m`).
  - Past threshold, the panel's state transitions from attached (`0/1`) → hanging loose (`2`) → sheared off (`3`), spawning a `'Panl'` rigid body phob (`0x001E0DC8` → `0x00494D60`).
- **Structural Health & Wheel Shearing (`car + 0x1BEC`):**
  - Any impact with $\|\text{impulse}\| > 300$ adds $\min(0.35\text{ car} / 0.10\text{ other}, \|\text{impulse}\| \times 6.6667 \times 10^{-6} \times \text{damage\_tweak})$ to `car + 0x1BEC` (capped at `0.90` in Career Free Roam `0x001741C0`).
  - Reaching `1.0` transitions `car + 0x1BE8` to wreck state `10 / 11 / 12` and, with a 1-in-4 chance (`rng(4) == 0`), shears off the wheel on the impact side via `DetachedWheelSpawn` (`0x001B3068` → `0x00484E68`).
  - With **`ExpandDebris=1`** (`src/debris_pool.cpp`), the 16-slot wheel (`0x00484E68`) and panel (`0x00494D60`) guest rings spill live phobs into a dynamic host `std::vector` reserve (up to 512 scene phobs) so detached wheels and hoods remain on the track for the entire match!

---

### 2.2 Explosive Chicken Weapon System (`0x0017DFA8`–`0x0017E718`)
- **Allocation & Vtables:** `ChickenWeaponAlloc` (`0x0017DFA8`) allocates a `0x228`-byte (552 B) `Ball` entity with weapon interface vtable `0x003F9528` (`body + 0x1DC`) and ballistic physics vtable `0x003F9538` (`body + 0x98`), registered in `g_WeaponRegistry` (`0x003CE3F8`, 16 entries × 12 B).
- **Three Ballistic Launch Regimes (`ChickenWeaponTrigger 0x0017E000`, evaluated against cheat mask `0x003C4190`):**
  1. **Straight Battle Shot (`(flags & 0x90) == 0`):** Enforced by our Straight-Shot Chicken Fight mod (`cheats = (cheats | 0x09) & ~0x90`):
     $$\vec{V}_{\text{launch}} = \vec{F}_{\text{car}} \times (\|\vec{V}_{\text{car}}\| + 28.0\text{ m/s})$$
  2. **Feat Mortar Arc (`(flags & 0x10) != 0`, vanilla `horn_ball` cheat):**
     $$\vec{V}_{\text{launch}} = (0.7071\vec{F}_{\text{car}} + 0.4949\vec{U}_{\text{car}}) \times 24.8889\text{ m/s} + \vec{V}_{\text{car}}$$
  3. **Raining Chickens (`(flags & 0x80) != 0`):** Drops vertically from high altitude above active cars at $\vec{V}_{\text{launch}} = (0, -19.62, 0)\text{ m/s}$.
- **3-State Reload Machine (`body + 0x1E0`):** State `0` (Cooldown for `3.0 s` against deadline `body + 0x1E4` set by `0x0017E4D8`) → State `1` (Awaiting `L2` trigger release) → State `2` (Armed).
- **Spatial Hash Grid Shockwave (`ChickenWeaponDetonate 0x0017E718` → `0x0017DA58`):**
  - Queries the 100×100 2D spatial hash grid at `0x0048D2B4` (8.0m cells, coordinates scaled by `0.05`) via `SpatialHashQueryRadial` (`0x001D57A8`).
  - Applies radial blast force $F = 25{,}000\text{ N} \times \text{ChickenForceScale}$ within radius $R = 7.5\text{ m} \times \text{ChickenRadiusScale}$ with linear distance falloff $(1 - d/R)$.

---

### 2.3 AI Spline Navigation, Battle Bots & `BattleMaster` (`0x001A2D20`, `0x00166C38`)
- **Spline Following (`src/battle_navigation.cpp`):** Reads the primary course cache at `0x003CD190` (`tag 0x104F128B`, 60-byte `ILIN` nodes) with speed-adaptive lookahead arc $\min(\text{arc}, \text{clamp}(5.0 + 0.65 \times v, 8.0, 20.0\text{ m}))$.
- **Arena Interception & Unstick Machine (`src/battle_driver.cpp` intercepting `AICarUpdate 0x001A2D20`):**
  - Predicts opponent lead point $\vec{P}_{\text{lead}} = \vec{P} + \vec{V} \cdot t_{\text{lead}}$ with closing speed $\|\vec{V}\| + 12.0\text{ m/s}$ and applies rear handbrake (`+0x1B7C = 0.35..0.90`) for perpendicular T-bone impacts.
  - If stalled ($v < 1.5\text{ m/s}$ for `1.5 s`), engages reverse escape (`gear = -1`, `steer = ±0.8`, `clutch = 1.0f`) for `1.7 s`.
- **`BattleMaster` (`0x003CF194`, `0x5AC` bytes, vtable `0x003F7770`):** Manages 4 participant slots (`master + 0x454`, stride `0x50`), 2.0-second wreck respawn timers (`0x001671E8` → `0x001675C0`), and triggers match finish (`0x00167D58` → `0x00167658`) when any driver reaches **5 net kills** ($\text{kills} - \text{deaths} \ge 5$).

---

### 2.4 Career Economy, Rank Gates & Valuation (`0x002C0000`–`0x002EFFFF`)
- **Save State Layout (`CareerSaveInfo` at `*(0x003D9384)`, `0xE7C` bytes):** Aggregates header metadata (`+0x04`), `CDBupgr` (`0xC68` B at `+0x80`: cash, 20 upgrade parts, 4 garage slots × `0x300` B, expanded up to 16 slots via HLE wrap `0x002E1BB0`), and `CDBResSaveInfo` (`0x194` B at `+0xCE8`: driver rank and open Eve list).
- **Rank Gates (`0x0041EFF8 = {79, 49, 0}`):**
  - $\text{Rank} \le 79$ (`0x0037ECB8`): Unlocks AutoShop performance upgrades.
  - $\text{Rank} \le 49$ (`0x002E4230`, `0x0016EF58`): Unlocks Tier-3 Dealership stock (`0x004AB130`) and Highway free-roam Dare challenges.
  - $\text{Rank} = 0$ (`0x002E6C78`): Crowns player Eve Champion (wrapped in HLE to clamp at Rank 1 so all Career Eves remain infinitely replayable!).
- **Trade-In Valuation Math (`0x002E3BC8`–`0x002E3F20`):**
  $$\text{ChassisRefund} = \lfloor 0.90 \times \text{PricePaid} \rfloor, \quad \text{PartsRefund} = \sum \lfloor 0.75 \times \text{RetailPartPrice} \rfloor$$
  $$\text{NetTradeIn} = (\text{ChassisRefund} + \text{PartsRefund}) - \text{round}\big((\text{ChassisRefund} + \text{PartsRefund}) \times (1.0 - \text{Condition})\big)$$

---

## 3. Master Decompiled & HLE Function Reference Table

| Guest EE Address | Function Name | Source File / HLE Status | Subsystem | Verified Role & Contract |
|---|---|---|---|---|
| `0x00100008` | `_start` | `gen/rec_000.c` `[V]` | System / Boot | PS2 CRT entry point; initializes stack, clears BSS, calls `main`. |
| `0x00100138` | `main` | `gen/rec_000.c` `[V]` | System / Boot | Initializes SIF RPC, mounts `COMMON.RES`, allocates `"STAK"` heap, runs main loop. |
| `0x00114020` | `RaceFrameLoop` | `gen/rec_004.c` `[V]` | Frame Loop | 59.94 Hz active in-race simulation and render submission loop (`0x00437BB8`). |
| `0x00114358` | `RunRaceContest` | `src/` wrap `[V]` | Lifecycle | Master race teardown, participant cleanup, and `"STAK"` cookie verification. |
| `0x00119888` | `HudDrawLeftRaceRows` | `src/hle_hud.cpp` `[V]` | UI / HUD | Draws lap, position, and status text; shifted by $-\Delta$ (`-106.67`) in 16:9 widescreen. |
| `0x00119F40` | `HudDrawRightSpeedRows` | `src/hle_hud.cpp` `[V]` | UI / HUD | Draws tachometer and speedometer gauges; shifted by $+\Delta$ (`+106.67`) in 16:9. |
| `0x00123EE0` | `EveValidateChassisLimit` | `gen/rec_007.c` `[V]` | Memory / Load | Enforces PS2 7-unique-chassis ceiling per Eve (~12.6 MB mesh cap). |
| `0x001244A8` | `EveLoadContests` | `src/battle_events.cpp` `[V]` | Battle / Events | Loads contest descriptors into `0x003C61A8`; wrapped to inject solo Battle events. |
| `0x0012A700` | `DAVMapMakeForEve` | `tools/recomp.py` `[V]` | Memory / Load | Builds Driver-And-Vehicle asset map; patched with `eod_davmap_stack_growth`. |
| `0x0012C940` | `PointsRosterLoad` | `src/game_patches.cpp` `[V]` | Unlocks | Loads 16 points-gated track/car rows at `0x003CBF48` (unlocked by `UnlockAll=1`). |
| `0x0012DB78` | `MemFree` | `src/hle_heapcheck.cpp` `[V]` | Memory | Frees `"STAK"` heap block; checked for double-free and guard corruption. |
| `0x0012DDB8` | `MemAlloc` | `src/hle_heapcheck.cpp` `[V]` | Memory | Allocates 64-byte-aligned block with `0x40`-byte `"STAK"` header (`0x003CC438`). |
| `0x001323E8` | `ResLoadAsset` | `gen/rec_010.c` `[V]` | StreamFS | Resolves and loads member from mounted `0TSR` v6 archive into EE RAM. |
| `0x001342A8` | `LangLoad` | `gen/rec_010.c` `[V]` | Localization | Parses `LANG` v1 string table (`LOCALE.RES#usenglish_lang.lng`, 877 entries). |
| `0x00134A70` | `ValtOpen` | `gen/rec_010.c` `[V]` | Assets / VALT | Opens loaded binary `VALT` v11 key/value table in place (vtable `0x003F2190`). |
| `0x00135FC8` | `EodCrc32` | `gen/rec_011.c` `[V]` | Assets / CRC | Hardware MSB-first CRC-32 using 256-word table at `0x003F2598` (`0x04C11DB7`). |
| `0x00137A68` | `AICenterlineLoad` | `gen/rec_011.c` `[V]` | AI / ILIN | Loads `.ild` racing line into `0x003CD190` and stamps `0x104F128B` at `+0x00`. |
| `0x00166C38` | `BattleCreate` | `gen/rec_017.c` `[V]` | Battle Mode | Allocates and initializes `0x5AC`-byte `BattleMaster` (`0x003CF194`). |
| `0x001675C0` | `BattleRespawnCar` | `gen/rec_017.c` `[V]` | Battle Mode | Resets wrecked participant after 2.0s timer, snaps to terrain via `0x001D8F58`. |
| `0x00167D58` | `BattleUpdate` | `gen/rec_017.c` `[V]` | Battle Mode | Per-frame Battle match controller; triggers victory at 5 net kills. |
| `0x001741C0` | `CareerIsFreeRoamActive` | `src/game_patches.cpp` `[V]` | Career | Returns non-zero in Career Free Roam (caps car damage at `0.90` unless overridden). |
| `0x00176918` | `RaceSessionSeedRng` | `gen/rec_020.c` `[V]` | Netplay / Sim | Seeds deterministic session PRNG (`0x0012F3C8..0x0012F4F0`) for lockstep replay. |
| `0x00177718` | `WorldSimulationStep` | `gen/rec_020.c` `[V]` | Physics | Master 59.94 Hz rigid-body, suspension raycast, and broadphase step. |
| `0x00177CA0` | `SoblBroadphaseQuery` | `gen/rec_020.c` `[V]` | Collision | Traverses `track.sol` 2D XZ quadtree (in decimetres; ignores Y coordinate). |
| `0x0017B918` | `ContactRecord` | `src/car_damage.cpp` `[V]` | Physics / Damage | **Decompiled (`EOD_DIFF` 0 mismatch):** Records `0x24`-byte collision contacts (`0x3CE3F0`). |
| `0x0017DA58` | `ChickenExplosionApplyShockwave` | `src/game_patches.cpp` `[V]` | Weapons | Queries `0x0048D2B4` 100×100 grid and applies radial blast ($R=7.5\text{m}, F=25\text{kN}$). |
| `0x0017DFA8` | `ChickenWeaponAlloc` | `gen/rec_022.c` `[V]` | Weapons | Allocates `0x228`-byte `Ball` projectile with vtables `0x003F9528`/`0x003F9538`. |
| `0x0017E000` | `ChickenWeaponTrigger` | `gen/rec_022.c` `[V]` | Weapons | Evaluates Straight, Mortar (`0x10`), or Rain (`0x80`) launch velocity vector. |
| `0x0017E718` | `ChickenWeaponDetonate` | `gen/rec_022.c` `[V]` | Weapons | Plays `explode.sfx`, spawns fireball sprites, and invokes `0x0017DA58`. |
| `0x00185470` | `CarPhysicsInit` | `gen/rec_023.c` `[V]` | Vehicle Physics | Reads `physics.val` (`mass`, `grip`, `slip`, `spring_factor`, `rear_end_ratio`, `g1..g4`). |
| `0x0018AFE0` | `CarSetupDamageAndPanels` | `src/car_damage.cpp` `[V]` | Vehicle Damage | **Decompiled (`EOD_DIFF` 0 mismatch):** Binds 8 detachable panels (`0x44AE68`) to `Bone.mvl`. |
| `0x0018CA98` | `CarApplyCollisionDamage` | `src/car_damage.cpp` `[V]` | Vehicle Damage | **Decompiled (`EOD_DIFF` 0 mismatch):** Dents 157 bones, detaches panels, tosses wheels. |
| `0x0018F2A8` | `HumanDriverUpdate` | `src/game_patches.cpp` `[V]` | Input / Physics | Samples controller, writes `car + 0x1B6C..0x1B90`, scales L2 Turbo Boost ceiling. |
| `0x001A00B8` | `TrackLoadSolidCollision` | `gen/rec_028.c` `[V]` | Track / SOBL | Loads `track.sol` (`SOBL` v3, 264B records) into `*(0x004473FC)`. |
| `0x001A2D20` | `AICarUpdate` | `src/battle_driver.cpp` `[V]` | AI Driver | Master AI tick; wrapped to inject Battle Bot spline navigation and pursuit controls. |
| `0x001B3068` | `DetachedWheelSpawn` | `src/debris_pool.cpp` `[V]` | Debris Pools | **Decompiled (`EOD_DIFF` 0 mismatch):** Launches sheared wheel phob (`ExpandDebris=1`). |
| `0x001B8370` | `CarRecoveryCountdownUpdate` | `src/hle_recovery.cpp` `[V]` | Track Recovery | Evaluates rollover/off-track timer; wrapped for `ManualRecovery=1` (`Select` only). |
| `0x001E1E88` | `DetachedPanelCtor` | `src/debris_pool.cpp` `[V]` | Debris Pools | **Decompiled (`EOD_DIFF` 0 mismatch):** Constructs `0x264`-byte `'Panl'` phob (`0x494D60`). |
| `0x00212B58` | `CameraBuildViewportProjection` | `src/hle_lod.cpp` `[V]` | Camera / Fog | Builds view frustum and fog range; wrapped to restore full `2300m` far plane in 2P. |
| `0x00214678` | `TrackLoadVisualScene` | `gen/rec_050.c` `[V]` | Track / GRAF | Loads `track.grf` (`GRAF` v18) into `*(0x003D1C18)` and relocates display tree. |
| `0x00214B68` | `WorldFrameRender` | `src/native_world.cpp` `[V]` | Plume R2 | **Decompiled (`EOD_DIFF` 0 mismatch):** Drives per-view `mlist` visibility & passes 0..4. |
| `0x00218E68` | `VisualCarBatchSelectAndSubmit` | `gen/rec_051.c` `[V]` | LOD / Render | Selects `Low1..4.mod` LOD and submits vehicle parts to the render queue. |
| `0x00223858` | `SkyDraw` | `src/native_world.cpp` `[V]` | Plume R2 | **Decompiled (`EOD_DIFF` 0 mismatch):** Cone-culls and submits 32-part track sky dome. |
| `0x00226768` | `TrackLoadSurfaceCollision` | `gen/rec_055.c` `[V]` | Track / SCOL | Loads `track.bsc` (`SCOL` v4 terrain mesh) into `*(0x003D2098)`. |
| `0x00270C10` | `MenuRun` | `src/` wrap `[V]` | UI / Menus | Modal dialog loop; blocks until `*(0x004A1AC0) == 1`, returns `*(0x004A1AC4)`. |
| `0x00304D50` | `FlareRender` | `src/native_shadows_flares.cpp` `[V]` | Plume R4 | **Decompiled (`EOD_DIFF` 0 mismatch):** Updates & emits sun glare and point-light flares. |
| `0x00325728` | `WaterDraw` | `src/native_world.cpp` `[V]` | Plume R2 | **Decompiled (`EOD_DIFF` 0 mismatch):** Emits visible water surface tiles on Woodly. |
| `0x00325E40` | `WaterSimulate` | `src/native_world.cpp` `[V]` | Plume R2 | **Decompiled (`EOD_DIFF` 0 mismatch):** Simulates car wake ripples ($v > 2.222\text{ m/s}$). |
| `0x0033D408` | `StrmMountArchive` | `gen/rec_095.c` `[V]` | StreamFS | Mounts `.TRK`/`.CAR`/`.RES` `0TSR` v6 archive via `stream.irx` (`0x8000021A`). |
| `0x0033ECB8` | `FogConfigureRange` | `src/native_fog.cpp` `[V]` | Plume R5 | **Decompiled (`EOD_DIFF` 0 mismatch):** Thunk to `GsFogWriteTables` (`0x003653F8`). |
| `0x0034AF98` | `RenderQueueFlush` | `src/render_queue.cpp` `[V]` | Plume R1 | **Decompiled (`EOD_DIFF` 0 mismatch):** Flushes sorted draw-queue buckets to Plume/GS. |
| `0x0034BDA8` | `ModelSubmitVu1Packets` | `src/render_queue.cpp` `[V]` | Plume R1 | **Decompiled (`EOD_DIFF` 0 mismatch):** Queues `4SPS` model batches for rendering. |
| `0x0034C208` | `ShadowSubmit` | `src/native_shadows_flares.cpp` `[V]` | Plume R4 | **Decompiled (`EOD_DIFF` 0 mismatch):** Culls & submits front/back extruded shadow volumes. |
| `0x0034C7D0` | `ShadowBegin` | `src/native_shadows_flares.cpp` `[V]` | Plume R4 | **Decompiled (`EOD_DIFF` 0 mismatch):** Opens full-resolution Plume shadow accumulation pass. |
| `0x0034C9F8` | `BonePaletteUpload` | `src/bone_pose.cpp` `[V]` | Plume R3 | **Decompiled (`EOD_DIFF` 0 mismatch):** Uploads 20-bone character skinning matrices (`3×4`). |
| `0x0034CEF0` | `ModelLoadFromArchive` | `src/` wrap `[V]` | Assets / MODL | Loads `.mod` `MODL` v9 mesh; wrapped to probe independent fast `ncuNf.mod` Cup wheels. |
| `0x0034D328` | `DisplaySetAspectMode` | `src/hle_camera.cpp` `[V]` | Camera / 16:9 | Sets display aspect scale (`0x003DD658 = 0.75f` in 16:9) and Hor+ FOV expansion. |
| `0x003582B8` | `MlistVisibilityCalc` | `src/native_world.cpp` `[V]` | Plume R2 | **Decompiled (`EOD_DIFF` 0 mismatch):** Evaluates frustum & LOD visibility into Scratchpad. |
| `0x00358BD0` | `WorldRenderPass` | `src/native_world.cpp` `[V]` | Plume R2 | **Decompiled (`EOD_DIFF` 0 mismatch):** Submits visible world leaves for pass `0..4`. |
| `0x0035CAA0` | `ImmediateDrawIndexedTris` | `src/native_effects_imm.cpp` `[V]` | Plume R4 | **Decompiled (`EOD_DIFF` 0 mismatch):** Emits indexed immediate triangles & cockpit quad. |
| `0x0035DD28` | `DentOffsetWrite` | `src/bone_pose.cpp` `[V]` | Plume R3 | **Decompiled (`EOD_DIFF` 0 mismatch):** Uploads `V3-32` 157-bone car dent offsets to VU1 `0x90`. |
| `0x0035DF10` | `ImmediateBegin` | `src/native_effects_imm.cpp` `[V]` | Plume R4 | **Decompiled (`EOD_DIFF` 0 mismatch):** Opens dynamic immediate primitive stream. |
| `0x00361708` | `ImmediateVertex` | `src/native_effects_imm.cpp` `[V]` | Plume R4 | **Decompiled (`EOD_DIFF` 0 mismatch):** Appends `(xyz, st, rgba, adc)` immediate vertex. |
| `0x003639D0` | `ParticleAdd` | `src/native_effects_imm.cpp` `[V]` | Plume R4 | **Decompiled (`EOD_DIFF` 0 mismatch):** Packs 16B rotated particle quad (`VU1 0x08E0`). |
| `0x00363C18` | `AtlasSpriteAdd` | `src/native_effects_imm.cpp` `[V]` | Plume R4 | **Decompiled (`EOD_DIFF` 0 mismatch):** Appends UV-rect atlas effect sprite (`VU1 0x05E0`). |
| `0x00363DE8` | `SpriteAdd` | `src/native_effects_imm.cpp` `[V]` | Plume R4 | **Decompiled (`EOD_DIFF` 0 mismatch):** Appends simple effect sprite (`VU1 0x01B8`). |
| `0x003652A8` | `TextureSetLodState` | `src/native_texstate.cpp` `[V]` | Plume R5 | **Decompiled (`EOD_DIFF` 0 mismatch):** Configures `TEX1` `K` template and `MXL` mip count. |
| `0x003653F8` | `GsFogWriteTables` | `src/native_fog.cpp` `[V]` | Plume R5 | **Decompiled (`EOD_DIFF` 0 mismatch):** Stores fog start/end/density at `0x4D8B84..8C`. |
| `0x003654E0` | `BlendSelect` | `src/native_texstate.cpp` `[V]` | Plume R5 | **Decompiled (`EOD_DIFF` 0 mismatch):** Selects `ALPHA_1` standard (`0x44`) vs additive (`0x48`). |
| `0x00365540` | `RenderFlagsCommit` | `src/native_fog.cpp` `[V]` | Plume R5 | **Decompiled (`EOD_DIFF` 0 mismatch):** Commits render state flags to `*0x4D8BC8`. |
