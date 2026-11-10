# Project: Test Drive: Eve of Destruction — Subsystem Reverse-Engineering & Native Architecture

## Architecture
- **Runtime Model**: Static binary recompilation of `SLUS_209.10` (R5900 EE core translated to C via `tools/recomp.py`), linked into native executable `eod`.
- **Memory Subsystem**: 32 MB linear EE RAM (`EEState g_ee`, `g_ram`), 16 KB Scratchpad (`0x70000000`), 2 MB IOP RAM.
- **Hardware Compatibility Layers**:
  - VU0/VU1: Macro mode inline COP2; microcode precompiled via `tools/vurecomp.py` into `gen/vu_rec.inc` with fallback to `vu_interp`.
  - GS: GIF packet decoder (PATH1/2/3), 4 MB VRAM emulation with physical/swizzled addressing, Vulkan / D3D11 backends.
  - IOP: HLE Sony SDK 2.8 modules (`loadfile`, `fio`, `padman`, `mcman`, `libsd`) and Monster Games `stream.irx` (`MGI_Streamer`, RPC `0x8000021A`).
- **Data & Asset Subsystem**:
  - Archive Container: `0TSR` v6 (`.car`, `.res`, `.trk`), 0x44 directory entries, CRC32 table polynomial `0x04C11DB7`.
  - 3D Geometry: `MODL` v9 with `4SPS` VIF DMA packets, triangle strips with ADC flags, skeletal markers (`TJRM`).
  - Textures: `TEX` v10 (PSMT8 / PSMT4 with CLUT palettes), 1,547 2x resolution `.tm0` files, GS swizzling with `upload_addr8` and `height < 32` linear branch.
  - Tables: `VALT` v11 (`.val`, `.va0`, `.mvl`) driving vehicle tuning, physics, and bone mesh deformation.
  - Audio: SPU2 ADPCM soundbanks (`Bank` v1 `SPEECH.BNK` with 13 licensed music tracks + 2,584 speech cues, `SFX0`), MPEG-2 `.PSS` video streams.
- **Game Subsystems**:
  - Vehicles: 32 cars + 5 trailers, 8-row detachable panel table (`0x0044AE68`), detached wheel pool (`0x00484E68`), detached panel debris pool (`0x00494D60`), 157-bone mesh deformation (`Bone.mvl`), 20-car world cap.
  - Gameplay Objects & Projectiles: Explosive chicken projectile (`0x228` bytes, vtable `0x003F9528`), 3 ballistic launch regimes, 3.0s cooldown machine, radial blast shockwave ($7.5\text{ m}$, $25{,}000\text{ N}$ impulse) on 100x100 spatial hash grid (`0x0048D2B4`).
  - Tracks: Dual-layer representation (`track.grf` visual VIF meshes vs `track.sol` SOBL box primitives, `track.bsc` SCOL surface triangle mesh, `BSPT`/`BPPT` trees, `ILIN` 60-byte centerline splines).
  - AI & Events: AI input register injection in `f_001A2D20`, master event registry (`0x003C3A70`) with 26 race event types, `BattleMaster` match progression.
  - Career: `CareerSaveInfo` (`0xE7C` bytes), `CDBupgr` (`0xC68` bytes), 20 parts, 4 garage slots, `{79, 49, 0}` rank gates, `HSCM` v3 memory card container.
  - UI: `MenuRun` (`0x00270C10`), `.lyt` widget tree, HUD widescreen offset queues.

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | Baseline Architecture Map | System dependency graph, EE runtime, IOP/VU/GS emulation layers, and build pipeline | M6 | Survey R1 |
| 2 | Archive Container Unpacker/Repacker | Complete extraction & repacking of 70 `0TSR` v6 archives (`.car`, `.res`, `.trk`) | M1 | Survey R2 |
| 3 | Texture Decompilation & Atlas Mapping | Decompilation of 4,923 textures (`TEX` v10, PSMT8/4, CLUTs, `.tm0` 2x mip0) to PNG | M1 | Survey R2 |
| 4 | 3D Mesh & Geometry Conversion | VIF DMA packet decoding (`MODL` v9, `4SPS`, ADC strips, LODs, props, skyboxes) to OBJ/MTL | M1 | Survey R2 |
| 5 | Audio & Soundtrack Extraction | Extraction of 13 music tracks, 2,584 speech cues, 112 SFX, and 24 video PSS streams to WAV | M1 | Survey R2 |
| 6 | Vehicle Model & Hierarchy Specification | 32 cars + 5 trailers, `TJRM` attachments, wheel sockets, exhaust emitters, and component groups | M2 | Survey R2 |
| 7 | Detachable Panels & Debris Pools | 8-panel detachment table (`0x0044AE68`), detached wheel/panel pool lifecycles | M2 | Survey R2 |
| 8 | Vehicle Damage & Crumple Deformation | 157 mesh bones (`Bone.mvl`), `wa/256*D[a] + wb/256*D[b]` skinning, and chassis health | M2 | Survey R2 |
| 9 | Vehicle Physics & Participant Lifecycle | Pacejka parameters, 20-car world pool (`0x00485230`), and memory cars allocation | M2 | Survey R2 |
| 10 | Explosive Chicken Weapon & Projectile | Complete weapon lifecycle (`0x228` bytes, vtable `0x003F9528`), 3D models, textures, animations | M2 | Survey R2 |
| 11 | Weapon Kinematics & Blast Shockwave | Straight/Mortar/Rain regimes, 3.0s cooldown, radial shockwave ($7.5\text{ m}$, $25{,}000\text{ N}$) on spatial grid | M2 | Survey R2 |
| 12 | Track Visual Geometry & Scenery Spawning | `track.grf` (`GRAF` v18, SPS4 VIF meshes), scenery prop instances, and LOD rendering | M3 | Survey R2 |
| 13 | Track Collision Geometry & Spatial Trees | `track.sol` (SOBL oriented boxes), `track.bsc` (SCOL surface triangle mesh), `BSPT`/`BPPT` trees | M3 | Survey R2 |
| 14 | Indestructible & Immovable Track Props | Barrier properties, height restrictions, track configs, and race-mode environments | M3 | Survey R2 |
| 15 | AI Driving & Pathfinding Navigation | Course waypoint nodes (`ILIN` 60-byte splines `g_PrimaryCourseCache`), lead-pursuit math | M3 | Survey R2 |
| 16 | AI Control Injection & Aggression | Input injection in `f_001A2D20` (steer, throttle, brake, clutch, gear), opponent targeting | M3 | Survey R2 |
| 17 | Race Event Engine & Match Progression | 26 event descriptors (`0x003C3A70`), `BattleMaster` lifecycle, kill/death rules, win states | M3 | Survey R2 |
| 18 | Career Progression & Upgrade Economy | `CareerSaveInfo` (`0xE7C` bytes), `CDBupgr` (`0xC68` bytes), 20 parts, 4 garage slots, valuation math | M4 | Survey R2 |
| 19 | Career Rank Gates & Dealership Stock | Rank gate table `{79, 49, 0}`, event eligibility `f_002E5200`, dealer lot inventory | M4 | Survey R2 |
| 20 | Memory Card Persistence & Container | `HSCM` v3 save container format, CRC32 (`0x04C11DB7`), and modern file I/O replacement | M4 | Survey R2 |
| 21 | UI Architecture & Modal Dialog Engine | `MenuRun` (`0x00270C10`), `.lyt` widget tree, input dispatch jump table (`0x004111B0`) | M4 | Survey R2 |
| 22 | Replaceable UI & HUD Specification | Decoupled UI interface contract (virtual list model), HUD widescreen offset queues | M4 | Survey R2 |
| 23 | Startup, Race Lifecycle & Memory Architecture | Full lifecycle of startup, menus, career, track/car loading, memory cars, and 32MB limits | M4 | Survey R2 |
| 24 | PS2 Hardware Dependency Inventory | Full catalog of GS, VU, DMA, GIF, VIF, SPU2 dependencies and 58 VRAM readbacks/frame bottleneck | M5 | Survey R2 |
| 25 | PC-Native GPU & Rendering Roadmap | Technical spec for modern GPU pipeline (Vulkan/Metal/D3D12 native vertex buffers, shaders, decoupled threads) | M5 | Survey R5 |
| 26 | PC-Native Audio & I/O Replacement Roadmap | Migration roadmap for native spatial audio (OpenAL/miniaudio) and PC memory/streaming | M5 | Survey R5 |
| 27 | Central Integration & Searchable Catalogue | Unified architecture map, cross-subsystem dependency graph, searchable function/asset index | M6 | Survey R4 |
| 28 | Modding Suite Architectural Foundations | Data structures & specs for custom vehicles, damage hierarchies, track imports, and event scripting | M6 | Survey R3 |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| M1 | Asset Extraction & Tooling Track | Features 2, 3, 4, 5 (Archives, Textures, Meshes, Audio, Tools) | none | DONE |
| M2 | Vehicles, Physics, Debris & Objects Track | Features 6, 7, 8, 9, 10, 11 (Cars, Detachable Panels, Deformation, Explosive Chicken) | none | DONE |
| M3 | Environment, Tracks, Collision & AI Track | Features 12, 13, 14, 15, 16, 17 (Tracks, SOBL/SCOL Collision, AI Waypoints, Events) | none | DONE |
| M4 | UI, Career, Loading & Memory Architecture | Features 18, 19, 20, 21, 22, 23 (UI, Career, Save/Load, Lifecycle, 32MB Memory) | none | DONE |
| M5 | Rendering & Native Architecture Roadmap | Features 24, 25, 26 (GS/VU/DMA Hardware Bottlenecks, GPU Pipeline, Migration Specs) | none | DONE |
| M6 | Central Integration & Modding Foundations | Features 1, 27, 28 (Unified Architecture Map, Searchable Catalogue, Modding Suite Specs) | M1, M2, M3, M4, M5 | IN_PROGRESS |

## Interface Contracts

### M1 (Asset Extraction) ↔ M2/M3 (Vehicles & Tracks)
- `MODL` v9 Part Hierarchy: Output standard Wavefront OBJ + MTL with vertex group naming convention `Group_<N>_<part_name>` corresponding to VIF 4SPS component IDs (0..31).
- Textures: PNG output indexed by original FourCC and archive path; palette CLUT metadata preserved in JSON sidecar.

### M2 (Vehicles & Debris) ↔ M3 (Tracks & Collision)
- Collision Impulse Interface: `0x0017B918` (`CarContactEvaluateObstacleImpulse`) accepts world position, surface normal, and impulse vector, applying deformation to `Bone.mvl` and detachment evaluation against `0x0044AE68`.
- Spatial Hash Interface: 100x100 2D spatial grid at `0x0048D2B4` (8m cell size) queried by `f_0017DA58` for explosive chicken radial blast.

### M3 (AI & Events) ↔ M4 (UI & Career)
- Event State Interface: Relocated contest table at `0x00090000` providing 12-byte event descriptors to `CareerSaveInfo` for progression tracking and race selection.
- Victory Evaluation: `BattleMaster` (`0x5AC` bytes) writes match outcome to `*(0x004A1AC4)` for consumption by `MenuRun` modal transitions.

### M4 (UI & Lifecycle) ↔ M5/M6 (Native Architecture & Modding)
- Replaceable UI Interface: `VirtualListModel` and `MenuRun` command callback wrapper accepting `(dialog_id, item_index)` without requiring legacy `.lyt` rendering.
- Memory Model: Decoupled asset streaming interface replacing 32MB physical PS2 memory boundaries with host virtual memory pools.

## Code Layout
- `config/`: Symbol databases (`symbols.txt`), HLE function specifications (`hle.txt`), VU microcode (`vu/`).
- `runtime/`: Core EE/IOP emulation, memory management (`core/`), GS Vulkan/D3D11 backends (`gs/`), VU0/VU1 execution (`vu/`), kernel fibers (`kernel/`).
- `src/`: Native implementations, hooks, and wrappers (`game_patches.cpp`, `battle_driver.cpp`, `hle_hud.cpp`, `career_upgrades.h`, etc.).
- `tools/`: Reverse-engineering, extraction, and compilation CLI tools (`rst0.py`, `tex.py`, `vif_mesh.py`, `audio.py`, `valt.py`, `trk.py`, `nascar_parts.py`, `recomp.py`, `analyze.py`).
- `docs/`: Technical specifications, reverse-engineering notes, modding scope, and developer handoffs.
