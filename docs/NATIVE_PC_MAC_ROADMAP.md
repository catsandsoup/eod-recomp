# Native PC/Mac Modern Architecture & Migration Roadmap
## Test Drive: Eve of Destruction (`SLUS_209.10`) Recompilation

**Document ID:** `EOD-ROAD-001`  
**Classification:** Phased Engineering Migration Specification (Milestone M6 / Requirements R1, R5)  
**Author:** Central Integration Agent (`teamwork_worker_central_integrator`)  
**Date:** 2026-11-10  
**Status:** Approved Technical Migration Roadmap  

---

## 1. Executive Summary & Migration Principles

This roadmap establishes the phased engineering plan to transition *Test Drive: Eve of Destruction* from its current hybrid recompiled/emulated state into a high-performance, native PC/Mac title. It details the complete elimination of PlayStation 2 hardware emulation bottlenecks (GS VRAM readbacks, VU1 microcode, 32 MB EE RAM ceilings, SPU2 ADPCM limitations) and their replacement with modern, scalable PC architectures (Vulkan / Metal GPU pipelines, Jolt Physics, and Miniaudio spatial audio).

### Owner Mandate & Priority Order
In strict accordance with repository operational rules:
1. **P0 (Baseline Correctness)**: Establish accurate 1:1 native 1x PS2-resolution rendering, original 4:3 presentation, original timing, rock-solid stability, and full 59.94 FPS speed without frame skipping first.
2. **P1 (Presentation & Fidelity)**: Improve LOD and draw distance, deliver native widescreen 16:9 / 16:10 HUD edge anchoring, and widescreen camera projection.
3. **P2 (Resolution Scaling)**: Validate higher resolutions sequentially ($2\times$, $4\times$, $6\times$) on modern high-DPI displays.
4. **P3 (High Refresh Rates)**: Uncap game frame rates ($120\text{ Hz}$, $144\text{ Hz}$, $240\text{ Hz}$) by interpolating rigid body poses across fixed-rate 60 Hz physics ticks.
5. **P4 (Post-Processing & Upscaling)**: DLSS / FSR / MetalFX upscaling is explicitly last.
6. **Integrity Mandate**: No synthetic test shortcuts; every milestone must be verified against the real game running retail disc assets (`DATA/`, `SLUS_209.10`).

---

## 2. PS2 Hardware Bottleneck Diagnosis & Elimination

```
+─────────────────────────────────────────────────────────────────────────────────────────────────────────────────+
|                                    HARDWARE BOTTLENECK ELIMINATION MATRIX                                       |
+─────────────────────────────────────────────────────────────────────────────────────────────────────────────────+
| Bottleneck Area        | PS2 Emulation Mechanism          | Performance Cost      | Native Replacement Strategy  |
|────────────────────────|──────────────────────────────────|───────────────────────|──────────────────────────────|
| 1. Synchronous VRAM    | vk_compat.h::Map() invokes       | 58 stalls/frame       | GPU Render-Target Retain     |
|    Readbacks (P0)      | vkWaitForFences() to sync VRAM   | (14.5 ms on M2 Pro;   | + Async Transfer Queue;      |
|                        | pages with host CPU.             | collapses to 8-14 FPS)| zero mid-frame CPU readbacks.|
|────────────────────────|──────────────────────────────────|───────────────────────|──────────────────────────────|
| 2. GS Texture Block    | upload_addr8() swizzles textures | Heavy CPU memory      | Direct BC1/BC3/BC7 GPU       |
|    Swizzling (P1)      | into 256-byte GS pages in DRAM   | bandwidth; duplicate  | textures permanently         |
|                        | with height < 32 branch.         | texture copies.       | resident in VRAM.            |
|────────────────────────|──────────────────────────────────|───────────────────────|──────────────────────────────|
| 3. VU1 Microcode &     | ModelSubmitVu1Packets() unpacks  | CPU thread contention | Direct 4SPS decode to        |
|    VIF DMA Strips (P1) | VIF STCYCL streams; microcode    | and single-threaded   | VkBuffer / MTLBuffer VBOs;   |
|                        | runs on worker fibers.           | pipeline serialization| GPU compute shader skinning. |
|────────────────────────|──────────────────────────────────|───────────────────────|──────────────────────────────|
| 4. 32 MB Memory        | STAK pool at 0x003CC438 forces   | 7-chassis Eve cap;    | 64-bit Virtual Memory;       |
|    Ceiling (P2)        | 20-car world pool & car sharing; | 20-car world limit;   | unbounded participant pools; |
|                        | 400 KB mip0 circular cache.      | texture evictions.    | persistent asset cache.      |
|────────────────────────|──────────────────────────────────|───────────────────────|──────────────────────────────|
| 5. Optical Streamer    | stream.irx RPC 0x8000021A stalls | Artificial 50 ms      | Asynchronous NVMe background |
|    Delays (P0)         | 3 Vblanks per chunk; artificial  | pauses; 2-4 s post-   | streaming; memory-mapped     |
|                        | sleep loops at 0x00369FA8.       | load wait loops.      | 0TSR containers.             |
|────────────────────────|──────────────────────────────────|───────────────────────|──────────────────────────────|
| 6. SPU2 24-Voice       | 2 MB Sound RAM, 24 hardware      | CPU ADPCM decode;     | Miniaudio / OpenAL Soft      |
|    ADPCM Limit (P2)    | voices, manual ADSR envelopes,   | voice voice-stealing; | 3D spatial mixing; unlimited |
|                        | Sony K_FILTER prediction math.   | mono downmixing.      | voices; 7.1 surround sound.  |
|────────────────────────|──────────────────────────────────|───────────────────────|──────────────────────────────|
| 7. Legacy UI Engine    | MenuRun (0x00270C10) parses .lyt | Buffer overflows;     | Decoupled VirtualListModel   |
|    and .lyt Trees (P1) | binary trees; draws via GS GIF.  | re-entrancy bugs.     | & ImGui / Slint frontend.    |
+─────────────────────────────────────────────────────────────────────────────────────────────────────────────────+
```

---

## 3. Detailed Technical Modernization Specifications

### 3.1 Modern GPU Pipeline Architecture (Vulkan / Metal)
The modern rendering subsystem bypasses the PlayStation 2 Graphics Synthesizer by intercepting high-level geometry submission calls:

```
+─────────────────────────────────────────────────────────────────────────────────────────────────────────+
|                                    NATIVE VERTEX & RENDER PIPELINE                                      |
+─────────────────────────────────────────────────────────────────────────────────────────────────────────+
|                                                                                                         |
|  Legacy 4SPS VIF DMA Stream                         Native GPU Geometry Pipeline                        |
|  [ModelSubmitVu1Packets 0x0034BDA8]                 [Direct Memory Ingestion]                           |
|       │                                                  │                                              |
|       ├── Quantized V3-16 Positions (x, y, z) ─────────> ├── Packed Float32 Vertex Buffer (VBO)         |
|       ├── Quantized V2-16 UVs (s, t) ──────────────────> ├── Float32 UV Coordinate Buffer               |
|       ├── V4-8 Normals + ADC Flag (bit 7) ─────────────> ├── Float32 Normal & Index Buffer (IBO)        |
|       └── V4-8 Skinning Weights (wa, wb, ba, bb) ──────> ├── Bone Index & Weight Attributes (uint8x4)   |
|                                                                  │                                      |
|                                                                  ▼                                      |
|                                                     [GPU Compute Skinning Pass]                         |
|                                                     - Dispatches 160 bone matrices                      |
|                                                     - Evaluates: P' = wa*Da + wb*Db                     |
|                                                                  │                                      |
|                                                                  ▼                                      |
|                                                     [Vulkan / Metal Forward+ Rasterizer]                |
|                                                     - High-DPI Resolution (1x, 2x, 4x, 6x, 4K)          |
|                                                     - True 16:9 / 16:10 Perspective Projection          |
|                                                     - Native Multisampling (4x/8x MSAA)                 |
+─────────────────────────────────────────────────────────────────────────────────────────────────────────+
```

#### Synchronous VRAM Readback Elimination (`vk_compat.h`)
1. **Diagnosis**: In `runtime/gs/vulkan/vk_compat.h:733-745`, calling `Map(Texture*)` commands a full GPU pipeline barrier followed by `submit()`, executing `vkWaitForFences()` and blocking the CPU thread. This occurs up to 58 times per frame to read back temporary minimap renders and sync CPU-written pages.
2. **Solution**:
   - **Render Target Retainment**: Retain GPU render targets permanently in GPU memory. When the minimap or HUD requires a texture copy, execute a GPU-to-GPU `vkCmdBlitImage()` without reading bytes back to host RAM.
   - **Double-Buffered Staging Buffers**: For CPU texture writes, utilize a host-visible, non-blocking ring buffer. Writes are recorded via asynchronous DMA transfer barriers (`VK_ACCESS_TRANSFER_WRITE_BIT`) that do not stall the rendering fiber.
   - **Zero-Fence Invariant**: Command submission during gameplay must never invoke `retire(UINT64_MAX)`. Fences are signaled and retired asynchronously at Vblank presentation boundaries.

### 3.2 Multithreaded Simulation & Physics Architecture (Jolt Physics)
1. **Thread Partitioning**:
   - **Main / Render Thread**: Drives window events, SDL3 input polling, audio mixing, and GPU command encoding at display refresh rate (60 Hz, 120 Hz, 144 Hz).
   - **Simulation Thread**: Executes deterministic 59.94 Hz fixed-step physics (`WorldSimulationStep`), vehicle dynamics, AI decision-making, and track bounds queries.
2. **Jolt Physics Integration**:
   - Replace `track.sol` (264-byte OBBs) and `track.bsc` (winged-edge triangles) with a single `Jolt::MeshShape` generated from track collision geometry.
   - Replace 100x100 clamped spatial hash grid (`0x0048D2B4`) with an unbounded dynamic Bounding Volume Hierarchy (`Jolt::BroadPhaseLayerInterface`), eradicating the $\pm 400\text{ m}$ boundary distortion for explosive chicken shockwaves.
   - Decouple rigid body state from legacy 0xB0-byte arrays, expanding world capacity beyond 20 vehicles and 16 debris phobs.

### 3.3 Miniaudio Spatial Audio Pipeline
1. **Architecture**:
   - Integrate `miniaudio` as a single-header, zero-dependency audio backend.
   - Replace SPU2 24-voice hardware limit with an unbounded software mixer.
   - Convert `DATA/SPEECH.BNK`'s 2,584 speech cues and 13 licensed music tracks to 16-bit 48 kHz stereo PCM / Ogg Vorbis streams.
   - Implement 3D HRTF spatial audio positioning based on vehicle camera matrices (`0x003C4310`).

---

## 4. Phased Engineering Roadmap

```
+───────────────────────────────────────────────────────────────────────────────────────────────────────────────────+
|                                              PHASED ROADMAP OVERVIEW                                              |
+───────────────────────────────────────────────────────────────────────────────────────────────────────────────────+
|                                                                                                                   |
|   Phase 1: Baseline Stability & Performance (P0 Focus)                                                           |
|   ├── Objective: Lock rock-solid 59.94 FPS at native 1x / 4:3 with zero frame skipping.                           |
|   ├── Target: Eliminate 58 synchronous VRAM readbacks in vk_compat.h; bypass NVMe vblank sleep loops.             |
|   └── Acceptance: Verified headless/muted run; 0 stalls; full game speed in 2-player split screen.               |
|                                                                                                                   |
|   Phase 2: Presentation & Geometry Modernization (P1 Focus)                                                       |
|   ├── Objective: Widescreen HUD/camera, draw distance expansion, native VBO pipeline.                            |
|   ├── Target: Shift HUD edge anchors (Δ = 106.6667); decode 4SPS geometry directly to VkBuffer/MTLBuffer.         |
|   └── Acceptance: Flawless 16:9 / 16:10 presentation without gauge stretching; LOD culling expanded to 250m.     |
|                                                                                                                   |
|   Phase 3: High-DPI Resolution & High-Refresh Scaling (P2 & P3 Focus)                                             |
|   ├── Objective: Scale resolution sequentially (2x, 4x, 6x, 4K); uncap refresh rate to 120/144/240 Hz.           |
|   ├── Target: Decouple 60 Hz physics tick from render swapchain; interpolate rigid body poses via quaternions.   |
|   └── Acceptance: Crisp 4K rendering; ultra-smooth 144 FPS motion without physics desynchronization.              |
|                                                                                                                   |
|   Phase 4: Complete PS2 Hardware Retirement & Modding Engine                                                      |
|   ├── Objective: Replace SPU2 with Miniaudio; replace custom solver with Jolt Physics; glTF 2.0 asset loader.    |
|   ├── Target: 64-bit virtual memory model (no 32 MB cap); unbounded car rosters (>20 cars); JSON mod loader.     |
|   └── Acceptance: Custom tracks, vehicles, and weapons loaded seamlessly from external directories.               |
+───────────────────────────────────────────────────────────────────────────────────────────────────────────────────+
```

### Phase 1: Baseline Correctness & Performance Locking (P0 Focus)
- **Goal**: Attain flawless 59.94 FPS gameplay execution on native hardware without frame skipping, regressions, or emulation pauses.
- **Key Tasks**:
  1. Patch `runtime/gs/vulkan/vk_compat.h` to replace synchronous `Map()` calls with GPU blit queues and double-buffered host staging buffers.
  2. Eliminate 3-vblank artificial stalls in `hle_stream.cpp` (`f_002C1A20` and `f_002C2188`) to enable instant loading on NVMe SSDs.
  3. Bypass artificial 2–4 second sleep loops in `f_00337718` (`TimerGetMs`).
- **Acceptance Criteria**:
  - Replay of 5-frame retail race capture executes with 0 synchronous fence stalls.
  - Headless 2-player split-screen race maintains exactly 59.94 FPS throughout.
  - Zero crashes, zero memory allocator corruptions, and clean session teardown.

### Phase 2: Presentation, Widescreen & Geometry Modernization (P1 Focus)
- **Goal**: Modernize visual presentation and geometry pipelines while preserving authentic game simulation.
- **Key Tasks**:
  1. Implement native widescreen HUD edge anchoring (`src/hle_hud.cpp`) using verified shift deltas ($\Delta = 106.6667$ for 16:9; $\Delta = 64.0$ for 16:10).
  2. Implement high-level vertex pipeline bypass in `ModelSubmitVu1Packets` (`0x0034BDA8`), unpacking `4SPS` VIF geometry into host VBOs/IBOs.
  3. Expand camera frustum culling (`0x0034E308`) and vehicle LOD thresholds (`lod.val`) to eliminate pop-in on modern wide displays.
- **Acceptance Criteria**:
  - HUD elements anchored to physical display perimeter without gauge distortion or text overlap.
  - Geometry rendered directly from native vertex buffers without VU1 CPU emulation overhead.

### Phase 3: Resolution Scaling & High-Refresh Pipeline (P2 & P3 Focus)
- **Goal**: Enable sequential resolution scaling ($2\times$, $4\times$, $6\times$, 4K) and high-refresh gameplay ($120\text{ Hz}$, $144\text{ Hz}$, $240\text{ Hz}$).
- **Key Tasks**:
  1. Validate sequential resolution scaling multipliers in GS framebuffer allocation, confirming zero edge-bleeding or alpha-depth fighting.
  2. Decouple fixed 59.94 Hz physics simulation from host display presentation via linear pose interpolation:
     $$\vec{P}(t) = (1 - \alpha)\vec{P}_k + \alpha \vec{P}_{k+1}, \quad \mathbf{q}(t) = \text{slerp}(\mathbf{q}_k, \mathbf{q}_{k+1}, \alpha)$$
  3. Convert palettized `.tex` and `.tm0` textures to native BC1/BC3/BC7 GPU formats.
- **Acceptance Criteria**:
  - Stable 4K ($3840 \times 2160$) rendering with crisp UI and shadow volume parity.
  - 144 FPS smooth rendering validated on high-refresh PC/Mac monitors with identical vehicle physics.

### Phase 4: Full PS2 Hardware Retirement & Modding Suite Engine
- **Goal**: Retire all remaining legacy PS2 emulation layers and activate the native Modding Suite.
- **Key Tasks**:
  1. Replace SPU2 ADPCM emulation with `miniaudio`, streaming 48 kHz stereo music and 3D spatial sound effects.
  2. Replace legacy PS2 rigid body physics and spatial hash grid (`0x0048D2B4`) with multithreaded Jolt Physics.
  3. Transition from 32 MB linear EE RAM (`g_ram`) to standard 64-bit virtual memory allocations.
  4. Ingest custom glTF 2.0 models, JSON manifest files, and declarative game events according to `docs/MODDING_SUITE_FOUNDATIONS.md`.
- **Acceptance Criteria**:
  - Game runs entirely free of PS2 emulation code (no VU, no GS, no SPU2, no IOP RPC).
  - Custom vehicle models with $>20$ participants and custom track circuits load and race natively.

---

## 5. Architectural Quality Gates & Verification Standards

Every code change committed during this modernization roadmap must pass through three rigorous quality gates:

1. **Gate 1: Headless Automated Parity Test**:
   - Command: `EOD_HEADLESS=1 EOD_MUTE=1 ./build/eod --headless --data <disc_dir> --frames 600`.
   - Requirement: Exactly 600 simulation ticks completed with zero assert panics, identical vehicle positions against deterministic golden replays, and zero memory leaks.
2. **Gate 2: Real Game In-Game Verification (Mandatory AGENTS.md Rule)**:
   - Build native executable `eod`.
   - Launch with real retail disc assets (`DATA/`, `SLUS_209.10`).
   - Validate boot, menus, 3D world state, collisions, and clean exit without relying solely on unit test harnesses.
3. **Gate 3: Performance Regression Profiling**:
   - Compare frame times against baseline captures; no change may re-introduce synchronous GPU fences or drop frame rates below 59.94 FPS.
