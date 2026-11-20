# EOD-RECOMP master specification and operating rules

The owner's binding specification for the native remaster. Stage status and gate
numbers live in `docs/NATIVE_RENDERER_SUCCESS.md`; step 0 cleanup is in
`docs/STEP0_CLEANUP.md`.

## 1. Product vision and player jobs

A commercial-grade native PC (Windows x64, Vulkan) and Apple Silicon macOS (Metal)
remaster of *Test Drive: Eve of Destruction* (`SLUS_209.10`, PS2 2004), statically
recompiled into `eod`. Quality bar: Halo 2 Anniversary (MCC).

- **JTBD 1 — instant Classic/Modern toggle:** one button (`F9`, `L1+R1+R3` or menu)
  mid-race switches in 1 frame (`<= 33 ms`), flat GPU memory, between a pixel-faithful
  PS2 Classic look and a 2026 Remastered PBR look.
- **JTBD 2 — photo mode and video:** pause anywhere, hide HUD, free camera with FOV/DOF,
  export 8K (`7680x4320`) in `< 2 s`; record 60+ fps video with no hitch (`> 33 ms`),
  shimmer or pop-in.
- **JTBD 3 — unthrottled scale, LOD and multiplayer parity:** any `RenderScale` (1x–8x+),
  any frame rate the display supports, maximum car/world LOD at every visible distance,
  no PS2 culling, identical visuals across 1P, 2P and 4P split-screen.
- **JTBD 4 — modding:** 4K/8K textures and higher-poly cars/tracks load in both looks
  without PS2 memory, pool or streaming ceilings; Single & Multiplayer Arcade rosters
  display a clean `[MOD]` badge on any track or vehicle overridden by an active mod.
- **JTBD 5 — instant PC boot (default for all installations):** once the native pipeline is
  complete, every installation skips intro/boot `.PSS` movies, splash/loading delays, and the
  PS2 memory-card check by default, booting straight to the main menu (keep an opt-in
  env/config override for the `.PSS`/memory-card test suite scenes).

## 2. Acceptance gates (nothing is done without this evidence)

### Player and visual gates
- **P1 object parity:** per frame, objects the game submits (queue records, world leaves,
  shadows, particles, sprites, UI) == objects Plume draws, every frame of the scene suite.
- **P2 animation and damage parity:** bone palettes (`0x34C9F8`) and car dent offsets
  (`Bone.mvl` / `0x005BF288`) fed to Plume are bit-identical to EE pose outputs;
  skinned/dented vertices within `<= 1/16 px` of GS reference at 1x.
- **P3 Classic parity:** Plume PS2 look at 1x vs offline GS replay of the same frame:
  MAE `<= 1/255` per channel, p99 `<= 8/255`, SSIM `>= 0.98`, fog and sky `<= 2/255`.
- **P4 clean Modern look:** Filament's published BRDF/IBL math in our own HLSL (never the
  Filament runtime, never legacy `modern_vk.hlsl`): cascaded sun shadows, sky IBL, SSR,
  clear-coat paint, chrome, glass, water refraction from `material_table.json`. No
  unrequested vignette, grade, tone curve or haze; fog/sky within `<= 4/255` of the
  game's setters.
- **P5 instant toggle:** 1 frame, no frame `> 33 ms`, 0 MB GPU growth over 100 toggles.
- **P6 reference integrity:** offline GS replay of recorded race dumps reproduces the
  reference VRAM MD5s (`f30d64c0...` / `1ae45b16...` at 1x, `8a86f217...` at 6x).
- **P7 determinism:** solo replay and 2-instance netplay give identical lap times,
  physics and results, 0 desyncs.
- **P8 split-screen parity:** 2P/4P views have the 1P object lists, world passes, draw
  distance, LOD, sun glare/flares (`0x304D50`) and Woodly (`AFTON.TRK`) water (`0x325728`).
- **P9:** 6x and 8x Modern captures: shadows, reflections, sky IBL with no shimmer/pop-in.
- **P10:** `Detail=maximum` forces highest LOD at every visible distance, 1P and split.
- **P11:** Photo Mode exports `7680x4320` PNG in `< 2 s` (free camera, roll, FOV, DOF, no HUD).
- **P12:** 5-minute Tracy race capture with video recording: 0 missed vblanks, p99 frame
  `<= 16.7 ms`.
- **P13:** Creation Kit 4K/8K car and track textures render in both looks, no seams or
  fallbacks.

### Speed gates
- **S1 target floor:** i5-7600K + Radeon Pro 580, RenderScale 6, max LOD, Modern,
  fullscreen, vsync: 60 fps, 0 missed vblanks, p99 CPU `<= 12 ms`, p99 GPU `<= 14 ms`
  over 5-min race + 2-min free roam (Tracy + `EOD_GPU_PROF`).
- **S2 minimum floor:** Intel UHD 620-class Vulkan iGPU, RenderScale 1–2, Classic: 60 fps,
  0 missed vblanks.
- **S3 modern hardware / M2 Pro:** RenderScale 6–8+, max LOD, 60–240+ fps, no synchronous
  VRAM readback stalls.
- **S4 no emulated graphics in the frame path:** 0 VU1 microprogram executions, 0 emulated
  GS rasterizations, 0 synchronous VRAM readbacks per frame across the scene suite.
- **S5 efficiency (owner, 2026-11-20):** on the owner's PC at RenderScale 6, max LOD, Modern,
  Dundas with max cars and draw distance, the per-frame cost is `<= 3.33 ms`. That is the
  300 fps budget: the mean of CPU frame work (EE + native feed + record/submit) and GPU frame
  time, measured unpaced with Tracy + `EOD_GPU_TIMING`. Night flood-lit tracks and 2P
  split-screen must also meet it. The game still simulates at 60 Hz; this gate is about
  efficiency headroom, not the displayed frame rate.

### Decompilation gates
- **D1:** readable C++ replaces every function in the subsystem (`config/hle.txt` `wrap` / `diff_run`).
- **D2:** `EOD_DIFF` shows 0 mismatches over `>= 1,000` live calls on every path the suite reaches.
- **D3:** P6 and P7 hold with the replacement active.
- **D4:** `ninja -C build eod`, installed over the game with a backup, boots through menus,
  race, career and exits cleanly with 0 heap errors (`EOD_HEAP_CHECK=1`).
- **D5:** documented in `docs/<SUBSYSTEM>.md` with addresses, structs, call graphs, open edges.

### Scene suite (11 scenes, every gate)
1 Afton Jump Race (1P) · 2 Afton/Dennison/Dundas 2P and 4P split-screen · 3 Career free
roam (day) · 4 Career free roam (night / Anoka night) · 5 Car dealer · 6 Garage / paint /
upgrades · 7 Front-end menus · 8 Loading screen · 9 Memory card screen · 10 `.PSS`
movie / cutscene · 11 Heavy demolition crash replay (projected shadows + speed blur).

## 3. PS2 limits must go

Keeping a PS2 limit is a failed task. Touching a subsystem that owns a fixed cap replaces
it with adaptive host memory that grows on demand and releases when unused.
1. Draw record pool (`0x4C96C4` / `0x4C96C8`): `0xC00` (3,072) record cap.
2. Shadow render target (`0x34C7D0` -> `0x363218`): `320x512`; scale with RenderScale /
   4096+ cascade atlases.
3. Plume texture cache (`runtime/render/native_renderer.cpp`): no 256 MB hard cap.
4. Frame renderer list (`0x20F4A0` / `0x3D1C90`): 20 entries.
5. Engine pools (`UNIFIED_ARCHITECTURE_MAP.md`): 32 MB EE RAM / `STAK` heap (`0x003CC438`),
   400 KB `mip0` ring (`0x004A9830`), 7 unique chassis (`0x00123EE0`), 20-car world pool
   (`0x00485230`), 16 wheel debris (`0x00484E68`), 16 panel debris (`0x00494D60`),
   16-weapon registry (`0x003CE3F8`) — expanded as their owners are decompiled.

## 4. State on `main` (do not re-derive)

- **R0:** `runtime/render/plume_device.cpp` owns the Vulkan device; legacy GS borrows it
  (`EOD_PLUME=0` fallback).
- **VIF mesh decoder** (`runtime/render/vif_mesh.cpp`): all 44,584 retail `4SPS` parts
  (1,969,433 verts) in 160 ms, 0 mismatches vs `tools/vif_mesh.py`.
- **Draw queue and bone upload C++** (`src/render_queue.cpp`, `src/bone_pose.cpp`):
  `0x34BDA8`, `0x34B140`, `0x34C9C0`, `0x34C930`, `0x34C860`, `0x34AF98`, `0x34C9F8` ->
  `0x35DBD8`/`0x35A810`, 0 `EOD_DIFF` mismatches.
- **M1:** all ~6,260–7,756 VU1 starts/frame attributed to EE packet builders.
- **R1:** `EOD_NATIVE=1` draws queued cars, props, characters (`0x34AF98`) via Plume into
  the shared frame/depth buffers (1.3e-6 max projection error vs VU1 `vf4..vf7`).

## 5. Roadmap R2–R8

- **Step 0:** clean up leftover worktrees — done, see `docs/STEP0_CLEANUP.md`.
- **R2 track world, sky, water:** world renderer `0x358BD0` (passes 0..4 from `0x214B68`,
  setup `0x3582B8`, track data `0x3D1B28`/`0x3D1B2C`, builder `0x35DEE0`, VU1
  `0x0270`/`0x0618`/`0x0920`/`0x0350`), sky (`0x223858`/`0x223800`), water (`0x325728`,
  surface `*(0x3DC134)`, sim `0x325E40`). `track.grf` uses prelit `V4-5` colours
  (1,367,745 verts): decode with `fix_winding = false` and no backface culling. Fix Woodly
  (`AFTON.TRK`) water in 1P and 2P/4P (P8).
- **R3 skinned characters and car deformation:** character palettes (`0x34C9F8`, 20 bones at
  VU1 `0x90 + 4*first`, programs `0x0318`/`0x24B0`, weights `float - 32768`, reverse vertex
  order `TOP+149+n-k`). Car dents (`0x005BF288` -> VU1 `0x90`, `Skin::Offset` programs
  `0x02E0`/`0x0D48`/`0x15A8`, 157 bones in `Bone.mvl`, `bonesurf_1` mask `0x752D05F`,
  `bonesurf_2` mask `0x52E20`), starting from `CarContactEvaluateObstacleImpulse`
  (`0x0017B918`) -> `CarApplyCollisionDamage` (`0x0018CA98`), `CarSetupDamageAndPanels`
  (`0x0018AFE0`), `DeformResetMeshVertices` (`0x001DF9A0`). Resolve `Skin::Partial`
  (`V1-8`, 21,817 verts) and `0x6A` (VU1 `0x350` facing sprites, 9,824 verts).
- **R4 shadows, particles, sprites, flares, immediate geometry:** shadows `0x34C208`,
  `0x34C4B0` -> `0x35D298` (`0x35D170`, target `0x34C7D0`); particles `0x3639D0`/`0x363B98`
  (`0x363FF8`, VU1 `0x08E0`); sprites `0x363DE8`/`0x363F48` (`0x364548`, VU1 `0x01B8`),
  `0x3642F4`; glare/flares `0x304D50` without the single-viewport check; immediate geometry
  `0x35DF10`/`0x361708` (`0x361468`/`0x364D18`), sphere-tested `0x361ED8` (`0x361C30` <-
  `0x34EE70`, `0x34EEB8`); trace the sender of UV/screen-offset program `0x25F0` -> `0x2630`.
- **R5 Classic parity and unthrottled LOD:** P3 comparison tool (SSIM, MAE, p99) vs offline
  GS replay; per-vertex fog (`FogConfigureRange 0x0033ECB8`, `GsFogWriteTables 0x003653F8`,
  state `0x4D8B80`), mipmaps (`0x353470`, `.tm0` stream `0x003545B8`), region clamp/repeat,
  destination-alpha blend, `AFAIL FB_ONLY/RGB_ONLY`, speed-blur RTT (`FBP 0x1180`). P10 via
  `DrawDistanceApplyOption` (`0x00212808`), `VisualCarSelectLodIndex` (`0x0021C6E0`),
  `g_ViewCountLodFactors` (`0x004062A8`), `g_DrawDistanceFar` (`0x00495D58`). Trigger and
  verify `0x34BF90` (`0x3067B8`, `0x32DFA8`) and `0x34CC98` (`0x222730..0x232FC8`).
- **R6 Modern PBR & VFX:** connect the 2,728 `material_table.json` records
  (`docs/MATERIAL_TAGGING.md`). The table has no blend field: blend equations always come
  from the game's render state, so `fx` / `emissive_light` cards never render as solid
  walls. Resolve the 318 low-confidence (`< 0.6`) textures with in-context renders.
  **Modern VFX (R&D first before coding):** replace flat 2004 billboard cards (exhaust
  flames, exploding-chicken fireballs, smoke, dust, sparks) in the Modern look with genuine
  modern GPU particle/VFX implementations — soft depth-faded particles, multi-layered /
  volumetric-style fire & smoke simulation, GPU embers/sparks, HDR emissive + bloom, and
  dynamic point-light flashes on nearby cars and terrain (Classic look keeps the original
  PS2 quads for P3 parity).
- **R7 native 2D and emulator removal:** PATH2/PATH3 GS transfers (`MenuRun 0x00270C10`,
  `HudDrawLeftRaceRows 0x00119888`, `HudDrawRightSpeedRows 0x00119F40`, HUD queues
  `0x003C4370`/`0x003C4600`, widescreen edge shift, `GTEX` v4 fonts, `CANV` v1 canvases,
  `.PSS` movies, loading / memory card) to Plume; cut VU1 and GS emulation from the frame
  loop (S4 = 0), removing all 58 synchronous VRAM readbacks per frame. R7 is done only once
  S5 (`<= 3.33 ms`/frame at 6x) is met: profile and remove every remaining PS2 leftover in
  the frame path (emulated DMA/VIF/GIF paths, the GS shim, the old `ModernRenderer` pass,
  per-frame PS2 cap bookkeeping).
- **R8 macOS, speed sign-off, photo mode, Creation Kit:** `plume_metal.cpp` on macOS; P11,
  P13; sign off S1–S3 and P12 with fullscreen Tracy + `EOD_GPU_PROF`.
- **R9 Creation Kit world editor (clean-room, workflow modelled on modern community map
  editors; no third-party code, assets or names):** per-track editing in the Kit, with
  the viewport drawn by the Plume renderer (depends on R4 only, so it may run in parallel
  with R5–R8 in its own worktree when the owner allows). Edits are saved as a named project
  layered over the retail `.TRK`; a separate "write" step builds the game files.
  - **R9a viewport and objects:** fly camera; model browser with thumbnails from
    `vif_mesh` (filter "only models this track places"); select / move / rotate (heading,
    pitch, roll) / scale gizmos; snap to ground; delete; per-object properties (model,
    archive member, placement record, triangles, textures, retail vs editor-placed, written
    or not); status bar with camera position, instances, triangles, draws, frame ms;
    Launch game (START string). Writer: `tools/world_export.py place`, proven in a real run.
  - **R9b gameplay markers:** spawn points, event triggers, chicken spawns, checkpoints,
    breakable props (hay bales, cones, barrels) with their physics and debris data.
  - **R9c new content:** place new objects and models (archive rebuild; per-track object
    pools grow adaptively, `docs/PS2_LIMITS.md`). The model browser lists every model from
    every retail track, not just the open one: any geometry from any map can be inserted
    into any other, with its textures, materials, LODs and collision copied into the edited
    track (owner rule, 2026-11-19); "Make collision" compiles collision
    from a mesh (`tools/world_collision.py` read side exists).
  - **R9d racing lines and events:** ILIN racing-line editor (the Roads tool: nodes,
    branches, lanes, width, direction), AI start grids, event and race definitions for
    the 26 event types.
  - **R9e terrain and mesh:** ground-mesh sculpting (decode the GRAF ground first) and a
    Blender round trip (OBJ export plus `tools/vif_patch.py` / import pipeline).
  - **Gates:** each edit type survives save, reopen and write. Unedited archive members
    stay byte-identical. The edited track loads and plays in the real game (headless run
    plus one owner session). Edited objects have collision and AI drives the edited lines.
    No PS2 cap limits object count.

## 6. Engineering rules

1. **Never guess.** Before any shader or C++ replacement check `docs/RENDERER_REFERENCES.md`
   (OpenGOAL, RT64, RoadTripAdventure-recomp, Gitaroo Man Native, paraLLEl-GS oracle), the
   PS2 Ghidra decompile + `config/symbols.txt`, and the Xbox `.xbe` Ghidra project
   (read-only; `ghidra-xbe` + XbSymbolDatabase D3D8 signatures) for exact blend modes, fog,
   water, particle and lighting constants. Never write D3D8 wrapper code.
2. **Spec before code.** Addresses, struct layouts and state tables go into `docs/` before
   `.cpp`/`.hlsl` edits. Still ambiguous after PS2 and Xbox: stop and ask.
3. **No images in context until R5/R6 visual sign-off.** R2–R4 are validated with numbers
   (`EOD_DIFF`, `EOD_RENDER_TRACE`, `EOD_NATIVE_CHECK=1`, vertex/pixel diff tools).
4. **One subagent at a time.** Complete, verify, commit to `main` and install one task
   before starting the next.
5. **Log filtering.** Build and game logs go to files and are queried with
   `grep -E "error:|warning:|FAIL|mismatch|\[rtrace\]|\[native\]" -C 3`.
6. **Headless by default** (`EOD_HEADLESS=1 EOD_MUTE=1 --headless`) for the vblank window
   the scene needs; fullscreen (never windowed) once per milestone for speed and pacing.
7. **STAR handoffs on disk** (Situation, Task, Action, Result, Next decisive test). Never
   commit disc assets, captures, binaries, personal paths or names.

### Subagent and context-budget rules (mandatory)

8. **No upfront bulk reads.** Never pre-read a long list of docs or sources at the start of
   a task. Run `git status --short` and `git diff --stat` first, then read only the specific
   section lines (`sed -n` / `grep -n`) of the 2–4 files the current micro-task modifies.
9. **Two-phase micro-sprints per stage.** Never pack spec, multi-subsystem decompilation,
   pool expansion and 5-scene verification into one continuous agent run. Split every stage:
   - **Phase A (spec and implementation):** decompile the target functions, wire the Plume
     feed, compile (`ninja -C build eod > build.log 2>&1` + `grep -E "error:|warning:"`), pass
     one fast headless `EOD_DIFF` smoke test. Persist findings to disk immediately.
   - **Phase B (multi-scene verification and install):** run the multi-scene headless suite,
     update `docs/NATIVE_RENDERER_SUCCESS.md` and `docs/STEP0_CLEANUP.md`, commit to `main`,
     install to `<game install dir>\eod.exe` with backup.
10. **Delta-only resumes.** After a usage limit, never re-verify or re-read passing work.
    Check `git diff` and `docs/`, list only the unfinished delta in <= 5 bullets, and do
    strictly what remains.

## 7. Failure guardrails (revert immediately)

1. **Fake renderer:** guessing cameras, skies, UI layers or surfaces from VU1/GS traffic
   (`scene_emit.cpp`, GIF packets, VRAM) or layering effects over the emulator. Every Plume
   draw originates in a decompiled EE function (`src/*.cpp`) verified by `EOD_DIFF`; an
   undecompiled caller is decompiled first.
2. **Happy-path decompilation:** before closing a stage, a coverage sweep (`EOD_COV_LOG=1` /
   `EOD_RENDER_TRACE`) across career and economy (HomeBase, garage, AutoShop, dealer
   turntable `0x0036E9C0`, buy/sell/trade, liveries `CT.RES`), all 26 event types
   (`0x003C3A70`: Race `0x100`, Trailer `0x800`, Arena/Demolition `0x18000000`, Gauntlet,
   Flag, Explosive Chicken `0x0017DFA8` Straight/Mortar/Rain), 2P/4P on Dundas, Dennison,
   Afton, extreme damage (head-ons, rollovers, 8 panels `0x0044AE68`, 16 wheels
   `0x00484E68`, wreck states `+0x1BE8 = 0xA/0xB/0xC`, out-of-bounds `0x001B8370`), and 2D
   paths (`MenuRun` dialogs, `HSCM` v3 memory card, loading, `.PSS`, cheats). Any graphics,
   UI or deformation function with 0 calls gets a deterministic replay verified by `EOD_DIFF`.
3. **Silent emulator masking:** `EOD_NATIVE_ONLY=1` suppresses all emulated GS draws so a
   missing Plume path shows as missing geometry.
4. **Invented constants:** every constant cites a PS2 EE address / VU1 instruction, a matched
   Xbox D3D8 function address, or a `material_table.json` record.
5. **Completion without live proof:** no stage advances until every row of its gates is
   recorded with numbers in `docs/NATIVE_RENDERER_SUCCESS.md`.

Install target: `<game install dir>\eod.exe`, previous copy backed up first.
