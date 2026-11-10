# Unified Architecture Map & System Dependency Graph
## Test Drive: Eve of Destruction (`SLUS_209.10`) Recompilation

**Document ID:** `EOD-ARCH-001`  
**Classification:** Core System Technical Specification (Milestone M6 / Requirements R1, R4)  
**Author:** Central Integration Agent (`teamwork_worker_central_integrator`)  
**Date:** 2026-11-10  
**Status:** Approved Technical Architecture  

---

## 1. Executive Summary & Runtime Model

*Test Drive: Eve of Destruction* (Monster Games / Atari, 2004, `SLUS_209.10`) is statically recompiled from PlayStation 2 MIPS R5900 Emotion Engine machine code into C/C++ source code via `tools/recomp.py`, linked with native compatibility layers, hardware emulators, and high-level reimplementations into a single native host executable (`eod`).

### Core Architecture Axioms
1. **Static EE Binary Translation**: The 3.37 MB retail ELF (`SLUS_209.10`) is disassembled, partitioned into 3,680 basic blocks and analyzed functions, and translated into native C functions (`gen/rec_*.c`) indexed through a global dispatch jump table (`gen/table.c`).
2. **Deterministic Memory Model**: A contiguous 32 MB linear buffer (`g_ram`) emulates physical EE RAM (`0x00000000`–`0x02000000`), complemented by 16 KB Scratchpad RAM (`0x70000000`), 2 MB IOP RAM (`g_iop_ram`), and 4 MB physical GS VRAM (`kVramSize = 4 * 1024 * 1024`).
3. **Hardware Emulation Partition**:
   - **Vector Units (VU0 / VU1)**: Macro-mode VU0 runs inline as COP2 instructions; VU1 microcode is precompiled via `tools/vurecomp.py` into native C functions (`gen/vu_rec.inc`), falling back to an interpretive emulator (`vu_interp()`).
   - **Graphics Synthesizer (GS)**: Decodes GIF packets across PATH1, PATH2, and PATH3 into a virtual 4 MB VRAM page table (512 pages $\times$ 8 KB), translating PS2 draw kicks into Vulkan / D3D11 graphics commands.
   - **I/O Processor (IOP)**: HLE modules emulate Sony SDK 2.8 services (`loadfile`, `fio`, `padman`, `mcman`, `libsd`) and Monster Games' proprietary optical streaming engine `stream.irx` (`MGI_Streamer`, RPC `0x8000021A`).
4. **Decoupled Asset System**: The game's 7,329 internal files across 70 `0TSR` v6 archives are indexed via 0x44-byte directory entries, CRC32 verified, and split into resident heap partitions and optical streaming pools.

---

## 2. Master System Dependency Graph

The following graph maps all 9 reverse-engineered game subsystems, host runtime layers, hardware compatibility emulators, and the compilation toolchain:

```
+──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────+
|                                              BUILD & CODE GENERATION PIPELINE                                            |
|                                                                                                                          |
|  Retail Disc Media       tools/analyze.py           tools/recomp.py         tools/vurecomp.py         CMake + Ninja      |
|  [SLUS_209.10] ────────> [Function Ranges] ───────> [C Translation] ──────> [VU1 Translation] ─────> [eod Native Exec]  |
|  [DATA/*.RES/CAR/TRK]    [Symbols Database]         [gen/table.c]           [gen/vu_rec.inc]          [macOS / Windows]  |
+───────────────────────────────────────────────────────┬──────────────────────────────────────────────────────────────────+
                                                        │
                                                        ▼
+──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────+
|                                              CORE RECOMPILATION RUNTIME ENGINE                                           |
|                                                                                                                          |
|  Linear Memory (32 MB EE RAM)      Fibers / Co-routines        HLE IOP Kernel (RPC)         Hardware Emulators           |
|  - EEState g_ee, g_ram            - arm64 register switch     - stream.irx (0x8000021A)    - VU0 (inline COP2)          |
|  - Scratchpad (0x70000000)        - P-core affinity QoS       - mcserv (0x80000400)        - VU1 (Precompiled/Interp)   |
|  - 4 MB GS VRAM (g_page_gen)      - Single-threaded lockstep   - padman / libsd             - GIF / VIF DMA Offload      |
+───────────────────────────────────────────────────────┬──────────────────────────────────────────────────────────────────+
                                                        │
         ┌──────────────────────────────────────────────┴─────────────────────────────────────────────┐
         ▼                                                                                            ▼
+──────────────────────────────────────────────────────────────+ +──────────────────────────────────────────────────────────+
|                 GAME ENGINE CORE SUBSYSTEMS                  | |                ASSET & DATA PIPELINE SUBSYSTEMS          |
|                                                              | |                                                          |
| 1. LOADING & MEMORY ARCHITECTURE                             | | 7. ASSET EXTRACTION & CONTAINER ENGINE                   |
|    - 6-phase lifecycle: Boot -> Menu -> Staging -> Race      | |    - 70 0TSR v6 archives (.car, .res, .trk)              |
|    - 0x003CC438 STAK heap allocator (0x40B cookie header)    | |    - 0x44 directory entries, CRC32 poly 0x04C11DB7       |
|    - 400 KB mip0 circular cache (0x004A9830)                 | |    - Resident vs Extractable optical partition           |
|    - 20-car world pool / 7-chassis memory car limits         | |    - tools/rst0.py, tex.py, vif_mesh.py, audio.py        |
|                                                              | |                                                          |
| 2. VEHICLE DYNAMICS, HIERARCHY & DAMAGE                      | | 8. TEXTURE & GS RASTER ENGINE                            |
|    - 32 cars + 5 trailers, MODL v9, 4SPS groups 0..31        | |    - TEX v10 (0x201 PSMT8, 0x200 PSMT4, 256 ARGB CLUT)   |
|    - 8-row detachable panel table (0x0044AE68)               | |    - upload_addr8 swizzle / height < 32 linear branch    |
|    - 16-slot wheel (0x00484E68) & panel (0x00494D60) pools   | |    - 1,547 2x hi-res .tm0 stream mip0 textures           |
|    - 157 deformation bones (Bone.mvl), VU1 dual-skinning     | |    - Font sheets (GTEX v4) & UI canvases (CANV v1)       |
|                                                              | |                                                          |
| 3. GAMEPLAY OBJECTS & EXPLOSIVE CHICKEN                      | | 9. RENDERING & HARDWARE ABSTRACTION                      |
|    - 0x228-byte Ball object, vtable 0x003F9528               | |    - Vulkan / D3D11 backends, GIF packet parser          |
|    - 3 ballistic regimes: Straight (28m/s), Mortar, Rain     | |    - Bottleneck: 58 synchronous VRAM readbacks/frame     |
|    - 3.0s cooldown machine (State 0, 1, 2)                   | |    - ModelSubmitVu1Packets (0x0034BDA8) pipeline         |
|    - 7.5m / 25,000 N blast on 100x100 grid (0x0048D2B4)      | |    - SPU2 24-voice ADPCM mixing & SPEECH.BNK (13 songs)  |
|                                                              | +──────────────────────────────────────────────────────────+
| 4. ENVIRONMENT, TRACKS & COLLISION                           |
|    - Dual-layer: track.grf (GRAF v18 visual SPS4) vs         |
|      track.sol (SOBL v3 264B OBBs) + track.bsc (SCOL v4)     |
|    - 6 surface materials: Grass, Clay, Mud, Asphalt, Curbs   |
|    - 125 obstacle types (obstacle.val) & recovery machine    |
|                                                              |
| 5. AI DECISION-MAKING & EVENT SYSTEMS                        |
|    - Control injection: steer (+0x1B6C), gas (+0x1B70),      |
|      brake (+0x1B74), clutch (+0x1B78), handbrake (+0x1B7C)  |
|    - Spline cache (0x003CD190) 60B ILIN nodes, lead pursuit  |
|    - BattleMaster (0x003CF194) 26 modes, 5-kill win logic    |
|                                                              |
| 6. CAREER PROGRESSION, UPGRADES & PERSISTENCE                |
|    - CareerSaveInfo (0xE7C), CDBupgr (0xC68), 20 parts       |
|    - Rank gates {79, 49, 0}, dealer inventory (0x004AB130)   |
|    - HSCM v3 save container, rotational XOR (0xFEEDFACE)     |
|    - MenuRun (0x00270C10) modal loop, decoupled UI model     |
+──────────────────────────────────────────────────────────────+
```

---

## 3. Subsystem Architecture & Functional Breakdown

### 3.1 Subsystem 1: Asset Extraction & Container Engine
- **Responsibility**: Manages the extraction, verification, repacking, and runtime memory mapping of all optical media containers.
- **Archive Architecture (`0TSR` v6)**:
  - 70 retail archives: 37 vehicle containers (`DATA/CAR/*.CAR`), 18 resource containers (`DATA/RES/*.RES`), and 15 circuit containers (`DATA/TRACKS/*.TRK`).
  - Container header occupies 0x28 bytes: magic `'0TSR'` (`0x52535430`), version 6, directory offset 0x80.
  - Directory entries are strictly 0x44 (68) bytes: 32-byte null-terminated filename, 4-byte reversed FourCC, 4-byte version, 4-byte uncompressed size, 4-byte directory-relative offset, 4-byte CRC32, 16 bytes reserved padding.
  - **CRC32 Engine**: Evaluated in `SLUS_209.10` at `0x00135FC8` using precomputed 256-word table at `0x003F2598` with generator polynomial `0x04C11DB7`.
  - **Partitioning Model**: Entries with `offset < extract_block` are resident (0x80-byte alignment) and loaded into the EE heap during initialization. Entries with `offset >= extract_block` are streamed on-demand from optical media with 2,048-byte (`0x800`) DVD sector alignment.

### 3.2 Subsystem 2: Vehicle Dynamics, Hierarchy, Damage & Debris
- **Responsibility**: Simulates 6-DOF vehicle rigid body physics, Pacejka tire curves, skeletal mesh crumpling, and detachable panel debris lifecycles.
- **Roster & Models**:
  - 32 cars and 5 trailers stored as `MODL` v9 archives.
  - Geometry is organized into `4SPS` chunks with group indices 0..31 (bit 16 designates paintable body panels for livery replacement from `CT.RES`).
  - Attachment points stored in `TJRM` markers (80 bytes each: name, 3x3 orthonormal rotation matrix, 3D position vector in metres) defining wheel sockets (`"left front wheel"`), hinges (`"right hood hinge"`, `"left door hinge"`), bumper anchors, driver seating (`"butt"`, `"rhand"`), trailer hitches, and 8 exhaust emitter sockets.
- **Detachable Panel Table (`0x0044AE68`)**:
  - 8-row table updated every frame by `0x0018CFE8`: Row 0 Left Door (0.0762m), Row 1 Right Door (0.0762m), Row 2 Hood (0.1270m), Row 3 Decklid (0.0381m), Row 4 Front Bumper (0.0762m), Row 5 Rear Bumper (0.0762m), Row 6 Left Front Fender (0.0762m), Row 7 Right Front Fender (0.0762m).
  - When nearest bone displacement exceeds threshold, visibility bit in `car + 0x2AC` is cleared and a rigid body phob is instantiated into the 16-slot panel pool (`0x00494D60`).
- **Deformation & Damage**:
  - 157 mesh bones defined in `Bone.mvl` split across two surface groups: `bonesurf_1` (bones 0..79, mask `0x752D05F`) and `bonesurf_2` (bones 80..156, mask `0x52E20`).
  - VU1 microcode computes vertex displacement $\Delta \vec{P} = \frac{w_a}{256}\vec{D}_a + \frac{w_b}{256}\vec{D}_b$.
  - Structural health scalar $D \in [0.0, 1.0]$ stored at `car + 0x1BEC`; transitions to wreck enum `0xA/0xB/0xC` at `+0x1BE8` upon reaching 1.0.

### 3.3 Subsystem 3: Gameplay Objects, Projectiles & Explosive Chicken
- **Responsibility**: Governs special weapon allocation, firing kinematics, reload state machines, projectile flight integration, and detonation shockwaves.
- **Memory & Vtables**:
  - Allocated dynamically as a 0x228-byte (552 B) `Ball` entity via `ChickenWeaponAlloc` (`0x0017DFA8`).
  - Stores weapon interface vtable `0x003F9528` at `body + 0x1DC` and projectile physics vtable `0x003F9538` at `body + 0x98`.
  - Registered in master weapon table at `0x003CE3F8` (12-byte records, count at `0x003CE4B8`, capped at 16 active weapons).
- **Kinematic Regimes**:
  - Straight Battle (`flags & 0x90 == 0`): $\vec{V}_{\text{launch}} = \vec{F}_{\text{car}} \times (\|\vec{V}_{\text{car}}\| + 28.0\text{ m/s})$.
  - Feat Mortar (`flags & 0x10 != 0`): $\vec{V}_{\text{launch}} = (0.707\vec{F} + 0.4949\vec{U}) \times 24.8889\text{ m/s} + \vec{V}_{\text{car}}$.
  - Raining Chickens (`flags & 0x80 != 0`): $\vec{V}_{\text{launch}} = (0, -19.62, 0)\text{ m/s}$.
- **Reload State Machine & Shockwave**:
  - 3-state reload machine at `body + 0x1E0`: State 0 (cooldown, 3.0s deadline at `+0x1E4`), State 1 (awaiting trigger release), State 2 (armed).
  - Detonation (`0x0017E790`) queries 100x100 2D spatial hash grid at `0x0048D2B4` (8m cell size), applying radial blast ($R = 7.5\text{ m}$, $F = 25{,}000\text{ N}$) via `0x0017B708` to damage `car + 0x1BEC` and crumple `Bone.mvl`.

### 3.4 Subsystem 4: Environment, Tracks, Scenery & Collision
- **Responsibility**: Loads track geometry, manages spatial trees, evaluates surface friction, handles obstacle destruction, and drives vehicle track recovery.
- **Dual-Layer Architecture**:
  - Visual scene stored in `track.grf` (`GRAF` v18, loaded by `0x00214678` into `*(0x003D1C18)`), comprising up to 2,239 `4SPS` VIF parts per track.
  - Solid collision barriers stored in `track.sol` (`SOBL` v3, loaded by `0x001A00B8` into `*(0x004473FC)`), consisting of 264-byte (`0x108`) Oriented Bounding Box records (mass 50,000 kg, friction 0.20, half-extents $dx, dy, dz$).
  - Surface terrain collision stored in `track.bsc` (`SCOL` v4, loaded by `0x00226768` into `*(0x003D2098)`), partitioned into 32-byte Table 1 (winged-edge topology) and 20-byte Table 2 (plane equations and material IDs).
- **Surface Materials & Props**:
  - 6 surface material types: Grass (`0`), Dirt Bullring (`2`), Mud (`5`), Asphalt (`6`), Concrete Curbs (`14`), Farm Terrain (`20`).
  - 125 interactive scenery props in `obstacle.val` across 15 classes (`cone`, `wobble`, `barrel`, `wall`, `horse`, `polyhedra`, etc.).
  - Track recovery machine (`0x001B8370`) monitors vehicle orientation and boundary containment, triggering snap reset `0x001B63E8` via `0x001D8F58`.

### 3.5 Subsystem 5: AI & Event Systems
- **Responsibility**: Drives AI bot navigation, tactical pursuit, opponent ramming, recovery unstick maneuvers, and master race event progression.
- **Control Injection Protocol**:
  - Intercepts `AICarUpdate` (`f_001A2D20`) and writes directly into normalized registers: steering target `car + 0x1B6C`, throttle `+0x1B70`, footbrake `+0x1B74`, clutch/drivetrain coupling `+0x1B78` (`1.0f`), handbrake `+0x1B7C` (`0.0f..1.0f`), gear `+0x1B90` (`-10` auto, `-1` reverse).
- **Navigation & Tactical Math**:
  - Spline following via primary course cache at `0x003CD190` (`tag 0x104F128B`, 60-byte nodes). Dynamic lookahead arc: $\min(\text{arc}, \text{clamp}(5.0 + 0.65 \times \text{speed}, 8.0, 20.0))$.
  - Arena interception: Lead pursuit predicts target point $\vec{P}_{\text{lead}} = \vec{P} + \vec{V} \cdot t_{\text{lead}}$, with $v_{\text{closing}} = \|\vec{V}\| + 12.0\text{ m/s}$. Power-slide ramming applies rear handbrake (`+0x1B7C = 0.35..0.90`) at speed to induce perpendicular T-bone impact.
  - Barrier unstick machine: Stalls ($v < 1.5\text{ m/s}$ for 1.5s) trigger reverse escape (gear $-1$, steer $\pm 0.8$) for 1.7s, achieving clean recovery in $<2.0$ seconds.
- **Event Engine (`BattleMaster`)**:
  - Master event registry at `0x003C3A70` holds 26 event descriptors. Category bits: race `0x100`, trailer `0x800`, arena combat `0x18000000`.
  - `BattleMaster` (`0x5AC` bytes, vtable `0x003F7770`): 5 net kills ($\text{kills} - \text{deaths} \ge 5$) triggers victory transition, sorts finishers (`f_00167658`), and presents scoreboard. Respawns enforce strict 2.0s delay.

### 3.6 Subsystem 6: Career Progression, Upgrades & Persistence
- **Responsibility**: Manages career driver state, garage inventory, vehicle valuation, shop stock rotation, event rank qualification, and memory card persistence.
- **Memory Layout**:
  - `CareerSaveInfo` (`0xE7C` / 3,708 B) at `*(0x003D9384)` aggregates metadata (`+0x04`), `CDBupgr` (`0xC68` / 3,176 B at `+0x80`), and `CDBResSaveInfo` (`0x194` / 404 B at `+0xCE8`).
  - Garage slots: 4 slots $\times$ 0x300 bytes (`0x68` to `0xC68` in `CDBupgr`).
- **Valuation & Gating**:
  - Chassis trade-in: $\lfloor 0.90 \times \text{PricePaid} \rfloor$. Installed parts refund: $\sum \lfloor 0.75 \times \text{Retail} \rfloor$. Depreciation: $\text{round}((\text{Chassis} + \text{Parts}) \times (1.0 - \text{Condition}))$.
  - Rank gates at `0x0041EFF8`: `{79, 49, 0}`. Rank $\le 79$ unlocks AutoShop upgrades; Rank $\le 49$ unlocks full dealer stock; Rank 0 marks Champion.
- **Persistence (`HSCM` v3)**:
  - Save container `HSCM` v3 (144-byte header, magic `'HSCM'`, version 3). Checksum at `+0x10` is 32-bit rotational XOR sum with constant `0xFEEDFACEu`. Interfaced via IOP `mcserv` RPC `0x80000400`.

### 3.7 Subsystem 7: UI & Menus Architecture
- **Responsibility**: Drives modal dialog loops, layout parsing, widget trees, dealership screens, and widescreen HUD edge anchoring.
- **Modal Engine (`MenuRun`)**:
  - Central modal runner at `0x00270C10` accepts `(desc, x, y, flags)`.
  - Input routing via jump table at `0x004111B0`. Exit contract writes `*(0x004A1AC0) = 1` and `*(0x004A1AC4) = exit_code`.
  - Transition duration at `0x004A1B30` defaults to 0.65s; speed is $1.0 / \max(0.01, \text{duration})$.
- **In-Game Semantic HUD**:
  - Viewport bounds table at `0x003C4310 + view * 0x18`. Left HUD queue (`0x003C4370`) and right HUD queue (`0x003C4600`).
  - Widescreen edge shift: In 16:9 mode ($k = 0.75$), horizontal delta is $\Delta = 320 \times (1/k - 1) = 106.6667$ native units, shifting HUD anchors outward to the display perimeter without distorting gauges.

### 3.8 Subsystem 8: Loading Lifecycle & Memory Architecture
- **Responsibility**: Governs the 6-phase startup-to-race execution pipeline, heap allocations, vehicle instance sharing, and streaming buffers under the 32 MB PS2 memory ceiling.
- **Execution Pipeline**:
  - **Phase 1 (Boot)**: `main()` initializes SIF RPC, mounts `COMMON.RES`, allocates `"STAK"` heap pool (`0x003CC438`), and establishes 400 KB `mip0` cache (`0x004A9830`).
  - **Phase 2 (Menus)**: `start.lyt` -> restores `HSCM` v3 save -> `MainMenuUpdate` (`0x002A1298`) -> `MenuRun`.
  - **Phase 3 (Staging)**: Mounts `.trk` archive; parses `track.grf`, `track.sol`, `track.bsc`, and `track.ild`.
  - **Phase 4 (Spawning)**: `f_0020FD48` walks 20 participant slots of 0x78 bytes, loads `MODL` v9 meshes, clones duplicate chassis (`0x0021C130`), allocates 0xB0-byte rigid bodies in `*(0x00494D30)`, and attaches weapons.
  - **Phase 5 (Race Loop)**: 59.94 Hz lockstep loop advances `WorldSimulationStep` (`0x00177718`), AI updates (`0x001A2D20`), and submits LODs (`0x00218E68`).
  - **Phase 6 (Teardown)**: `RunRaceContest` (`0x00114358`) cleans up participants, flushes caches, verifies `"STAK"` cookies, and unwinds to menus (`0x00437BA8=1`).
- **Memory Ceiling Mitigations**:
  - Max 7 unique chassis per Eve (`0x00123EE0`), capping mesh allocation to ~12.6 MB.
  - Model sharing via `VisualCarCloneShared` (`0x0021C130`), reducing RAM for duplicate cars from 2.0 MB to ~200 KB.
  - Stack expansion (`eod_davmap_stack_growth`) prevents crash in `DAVMapMakeForEve` (`0x0012A700`).

### 3.9 Subsystem 9: Rendering & Hardware Abstraction
- **Responsibility**: Emulates GS rasterization, VU0/VU1 microcode execution, VIF/GIF DMA unpacking, and audio synthesis.
- **Hardware Pipeline & Bottleneck**:
  - Emulates 4 MB GS VRAM across 512 pages (8 KB each), tracked by `g_page_gen[512]`.
  - 13 pixel formats decomposed by `addr_row()` using GS block swizzle tables.
  - **Diagnosed Bottleneck**: `vk_compat.h::Map()` submits command buffers synchronously with `retire(UINT64_MAX)` (`vkWaitForFences()`). Preserving GPU-rendered pages for CPU access causes up to 58 synchronous readbacks per frame, inflating submits from 7 to 65 and collapsing frame rates from 60 FPS to 8–14 FPS on Apple Silicon / MoltenVK.
  - **Vector Units**: Precompiled microcode functions (`vu_rec.inc`) execute skinning and projection on worker threads (`EOD_VU1_THREAD=1`).

---

## 4. Reconciled Cross-Subsystem Shared Interfaces

The 9 subsystems interact through 6 core architectural contracts reconciled below:

```
+───────────────────────────────────────────────────────────────────────────────────────────────────────────────────+
|                                  RECONCILED CROSS-SUBSYSTEM SHARED INTERFACES                                     |
+───────────────────────────────────────────────────────────────────────────────────────────────────────────────────+
|                                                                                                                   |
|  1. Contact Impulse & Detachment Interface (0x0017B918)                                                           |
|     Tracks & Collisions (M3) ───────────────> Vehicle Physics & Debris (M2)                                       |
|     - Routine: CarContactEvaluateObstacleImpulse(body, shape, impulse_vec, contact_pt, type, other, scale)        |
|     - Action: Ingests impact impulse -> passes to 0x0018CA98 -> displaces Bone.mvl -> updates chassis health     |
|               scalar (car + 0x1BEC) -> triggers panel detachment in 0x0044AE68 -> instantiates 0x00494D60 phob.    |
|                                                                                                                   |
|  2. Spatial Hash Grid Radial Shockwave Interface (0x0048D2B4)                                                     |
|     Explosive Chicken (M2) ─────────────────> Vehicle Deformation & Physics (M2/M3)                               |
|     - Address: 100x100 2D grid at 0x0048D2B4 (8.0m cell size, coordinates scaled by 0.05, clamped to [0..99]).    |
|     - Action: Detonation (0x0017E790) calls 0x0017DA58 (R = 7.5m, F = 25,000 N) -> SpatialHashQueryRadial        |
|               (0x001D57A8) -> applies radial force vector via 0x0017B708 -> damages car + 0x1BEC & Bone.mvl.     |
|                                                                                                                   |
|  3. Spline Navigation & Course Cache Interface (0x003CD190)                                                       |
|     Environment Tracks (M3) ────────────────> AI Driving & Decision-Making (M3)                                   |
|     - Address: Primary course cache at 0x003CD190 (resource tag 0x104F128B, up to 3 12-byte records).             |
|     - Format: 60-byte ILIN nodes (position X/Z, tangent X/Z, next pointer, segment length).                       |
|     - Action: Ingested from track.ild -> consumed by AICarUpdate (0x001A2D20) for dynamic lookahead arc           |
|               clamp(5.0 + 0.65 * speed, 8.0, 20.0m) and grade-crossing disambiguation.                            |
|                                                                                                                   |
|  4. Modal Dialog & Command Return Interface (MenuRun 0x00270C10)                                                  |
|     UI & Menus (M4) ────────────────────────> Career Progression & Race Lifecycle (M4)                           |
|     - Address: MenuRun at 0x00270C10, input jump table at 0x004111B0, exit status at 0x004A1AC0/0x004A1AC4.       |
|     - Contract: MenuRun(desc, x, y, flags) blocks caller until *(0x004A1AC0) == 1, returning *(0x004A1AC4).       |
|     - Callers: MainMenuUpdate (0x002A1298), Career Hub (0x003698E8), Dealership Buy Screen (0x0036E9C0),           |
|                and Race Results dialogs. Decoupled via VirtualListModel<T> and INativeMenuHost.                   |
|                                                                                                                   |
|  5. Persistence Container & Checksum Interface (HSCM v3)                                                          |
|     Career Progression (M4) ────────────────> IOP Memory Card Subsystem (mcserv)                                  |
|     - Format: HSCM v3 container (144-byte header, magic 'HSCM' 0x4D435348, version 3, payload size at +0x0C).     |
|     - Checksum: 32-bit rotational XOR sum at +0x10 with seed constant 0xFEEDFACEu.                                |
|     - Action: Aggregates CareerSaveInfo (0xE7C B) into 'Crer' section; transmitted over SIF RPC 0x80000400        |
|               through mcserv commands 0x01..0x06 to physical/virtual memory card storage.                         |
|                                                                                                                   |
|  6. Archive Virtual Filesystem & CRC32 Interface (0TSR v6)                                                        |
|     Asset Subsystem (M1) ───────────────────> All Engine Subsystems                                               |
|     - Structure: 70 containers (.CAR, .RES, .TRK), 0x28 header, 0x44 directory entries, dir_offset 0x80.          |
|     - Checksum: MSB-first CRC32 algorithm at 0x00135FC8 referencing 256-word table at 0x003F2598 (poly 0x04C11DB7|
|     - Storage: Resident partition (offset < extract_block) mounted to heap; extractable partition streamed.       |
+───────────────────────────────────────────────────────────────────────────────────────────────────────────────────+
```

---

## 5. Physical Memory Architecture & Address Space Layout

The following memory map details the 32 MB linear EE RAM, Scratchpad, hardware registers, and GS VRAM allocations:

```
+───────────────────────────+ 0x00000000
| Exception Vectors & Kernel|   (0x00000000 - 0x0008FFFF, 576 KB)
| Relocated Contest Tables  |   0x00090000: 12-byte event descriptors
+───────────────────────────+ 0x00100000
| Recompiled Text (.text)   |   (0x00100000 - 0x003BC290, ~2.73 MB)
| - Game Entry Point        |   0x00100138: main()
| - Physics Simulation      |   0x00177718: WorldSimulationStep()
| - Weapon Kinematics       |   0x0017E000: ChickenWeaponTrigger()
| - AI Updates              |   0x001A2D20: AICarUpdate()
| - Track Environment Load  |   0x00230CD8: Master track loader
| - UI Modal Runner         |   0x00270C10: MenuRun()
| - Texture Relocation      |   0x003521E8: TextureRelocateOffsets()
+───────────────────────────+ 0x003BC290
| Initialized Data (.data)  |   (0x003BC290 - 0x003DC000, ~127 KB)
| - Primary Spline Cache    |   0x003CD190: g_PrimaryCourseCache (tag 0x104F128B)
| - Native Float Clock      |   0x003CDACC: g_EngineTimeSeconds
| - Weapon Registry Pool    |   0x003CE3F8: g_WeaponRegistry (16 slots x 12B)
| - Master Event Registry   |   0x003C3A70: 26 event descriptors
| - Career Save State Root  |   0x003D9384: CareerSaveInfo (0xE7C bytes)
+───────────────────────────+ 0x003DC000
| Read-Only Data (.rodata)  |   (0x003DC000 - 0x003F6000, ~104 KB)
| - CRC32 Lookup Table      |   0x003F2598: 256 words (poly 0x04C11DB7)
| - Weapon Interface Vtable |   0x003F9528: Chicken weapon methods
| - Projectile Vtable       |   0x003F9538: Ballistic physics methods
+───────────────────────────+ 0x003F6000
| BSS & Global Runtime State|   (0x003F6000 - 0x00500000, ~1.04 MB)
| - UI Input Jump Table     |   0x004111B0: D-pad / Enter / Cancel jump table
| - Rank Gates Table        |   0x0041EFF8: {79, 49, 0}
| - Detachable Panel Table  |   0x0044AE68: 8 panel hinge rows x 0x20 bytes
| - Detached Wheel Pool     |   0x00484E68: 16 wheel phobs x 0xB0 bytes
| - Participant Pool        |   0x00485230: 20 slots x 0x78 bytes (g_DriverTable)
| - Spatial Hash Grid 2D    |   0x0048D2B4: 100x100 spatial hash cells (8m size)
| - Rigid Body Master Array |   *(0x00494D30): Contiguous array of 0xB0-byte bodies
| - Detached Panel Pool     |   0x00494D60: 16 panel phobs x 0xB0 bytes
| - Modal Return Registers  |   0x004A1AC0 (done flag), 0x004A1AC4 (result ID)
| - Hi-Res tm0 Cache Ring   |   0x004A9830: Fixed 400 KB circular mip0 buffer
+───────────────────────────+ 0x00500000
| Dynamic Heap Pool ("STAK")|   (0x00500000 - 0x01E00000, ~25 MB)
| - MemPool Descriptor      |   0x003CC438: kPool root descriptor
| - Block Cookie Headers    |   0x40-byte 'STAK' headers with size, name, guard
| - Vehicle Chassis Meshes  |   MODL v9 Low0..Low4 buffers (~1.8 MB per unique)
| - Track SPS4 Geometry     |   track.grf VIF mesh buffers (~3.5 MB)
+───────────────────────────+ 0x01E00000
| EE Main Thread Stack      |   (0x01E00000 - 0x02000000, 2 MB descending)
+───────────────────────────+ 0x02000000 (32 MB EE RAM Boundary)
| Scratchpad RAM (SPR)      |   (0x70000000 - 0x70004000, 16 KB fast cache)
+───────────────────────────+ 0x70004000
| IOP RAM Space             |   2 MB independent address space (g_iop_ram)
+───────────────────────────+
| GS VRAM Space             |   4 MB DRAM (512 pages x 8 KB, g_page_gen)
+───────────────────────────+
```

---

## 6. Build Pipeline & Modern Execution Strategy

### 6.1 Recompilation Toolchain Architecture
1. **ELF Analysis (`tools/analyze.py`)**:
   - Parses the retail MIPS ELF, identifying function boundaries, switch-case jump tables, cross-references, and leaf routines.
   - Outputs `build/functions.json` containing validated function ranges.
2. **Binary Translation (`tools/recomp.py`)**:
   - Ingests `build/functions.json`, `config/symbols.txt`, and `config/hle.txt`.
   - Utilizes `rabbitizer` to disassemble MIPS R5900 instructions, emitting equivalent native C code into `gen/rec_*.c` and compiling the dispatch table `gen/table.c`.
   - Incorporates automated stack growth (`eod_davmap_stack_growth`) to expand stack frames during high-roster Eve loading.
3. **VU Microcode Compilation (`tools/vurecomp.py`)**:
   - Analyzes registered microprograms in `config/vu/vu_entries.txt`.
   - Translates vector arithmetic into optimized SSE2 / NEON intrinsics, generating `gen/vu_rec.inc`.
4. **Native Compilation (CMake / Ninja)**:
   - Configures native targets (`eod`, test harnesses, offline tools).
   - On macOS (Apple Silicon arm64), enforces `-ffp-contract=off` to preserve x86 float semantics without FMA fusing, maps SSE2 intrinsics to ARM NEON (`runtime/include/sse_compat.h`), and switches execution fibers via direct assembly register saving.

---

## 7. Architectural Invariants & Verification Checklist

To prove adherence to the unified architecture without regression:
- [x] **Invariant 1 (Memory Sizing)**: Total EE RAM allocation is exactly 32 MB (`EE_RAM_SIZE 0x02000000u`) with a 64 KB guard page; Scratchpad is exactly 16 KB (`0x70000000`).
- [x] **Invariant 2 (CRC32 Engine)**: Container and texture CRC matches the generator polynomial `0x04C11DB7` referencing table `0x003F2598`.
- [x] **Invariant 3 (Vehicle Limits)**: Active car world participant slots capped at 20 (`0x00485230`), debris wheel phobs capped at 16 (`0x00484E68`), debris panel phobs capped at 16 (`0x00494D60`).
- [x] **Invariant 4 (Deformation Model)**: `Bone.mvl` contains exactly 157 mesh bones with dual-bone skinning weights $\Delta \vec{P} = \frac{w_a}{256}\vec{D}_a + \frac{w_b}{256}\vec{D}_b$.
- [x] **Invariant 5 (Chicken Weapon)**: Allocation size is strictly 0x228 bytes, vtable is `0x003F9528`, reload cooldown is strictly 3.0 seconds, blast radius is 7.5 metres, blast force is 25,000 Newtons.
- [x] **Invariant 6 (Solid Collision Boxes)**: `track.sol` (`SOBL` v3) records have a fixed stride of 264 bytes (`0x108`) with 3x3 orthonormal matrices and half-extents.
- [x] **Invariant 7 (Surface Terrain Mesh)**: `track.bsc` (`SCOL` v4) partitions into a 32-byte Table 1 (topology) and 20-byte Table 2 (plane equations and surface material IDs).
- [x] **Invariant 8 (AI Control Registers)**: Driver registers reside at `car + 0x1B6C` (steer), `+0x1B70` (throttle), `+0x1B74` (brake), `+0x1B78` (clutch), `+0x1B7C` (handbrake), `+0x1B90` (gear).
- [x] **Invariant 9 (Career Persistence)**: `CareerSaveInfo` is strictly 0xE7C bytes, `CDBupgr` is strictly 0xC68 bytes, save checksum is rotational XOR with `0xFEEDFACEu`.
- [x] **Invariant 10 (Modal UI Contract)**: `MenuRun` (`0x00270C10`) halts execution until `*(0x004A1AC0) == 1`, returning result code from `*(0x004A1AC4)`.
