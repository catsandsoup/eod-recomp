# Native renderer: success measures

Scope: replace the PS2 graphics path (VU1 microcode, GS emulation) in the shipped frame with this
project's own renderer on plume (docs/NATIVE_RENDERER.md), fed directly by the game's own,
decompiled render code, with two looks in the same scene (Modern, PS2) switchable live from the menu. The GS emulator remains an offline
reference only.

Nothing below counts as met without the evidence named in its row. Earlier results count only
after they are re-run against the current build. A passing unit test proves only that the code
compiles.

## 0. The bar

A Halo 2 Anniversary (MCC)-class remaster: the classic game exactly as it was and a remastered look
that holds up in 2026 screenshots and video, switchable instantly mid-race, at any resolution the
GPU can carry. The player judges it by what they can capture and share. Rows P9-P12 state that.

| # | Outcome | Measure | Pass |
| --- | --- | --- | --- |
| P9 | Remastered frames are share-worthy | Scene suite at RenderScale 6 and 8, Modern look: dynamic sun shadows on cars, track and characters; reflections on paint, glass and water; image-based sky lighting; no aliasing shimmer, no texture or object pop-in, no missing effects | side-by-side capture sheet per scene reviewed by the owner; no listed defect present |
| P10 | Any resolution, full detail everywhere | RenderScale 1-8 (and above where the GPU allows), Detail=maximum: highest car and world LOD at every visible distance, no PS2 culling or distance cut, identical in 1P and every split-screen view | LOD/cull trace shows highest level selected for every visible object; P8 holds at every scale |
| P11 | Photo mode | Pause, free camera (orbit, fly, roll, FOV, depth of field), HUD hidden, time of day unchanged, capture at a chosen resolution up to 8K and above independent of the window | a capture at 7680x4320 from a race and from a crash, written in under 2 s |
| P12 | Video-ready motion | Locked 60 fps (and higher on capable hardware) with even frame pacing, no hitches over 33 ms, while recording with a standard capture tool | Tracy capture over a 5-minute race with recording active: 0 missed vblanks, p99 frame <= 16.7 ms |
| P13 | Replaced art is first-class | 4K/8K texture replacements and higher-poly models from the Creation Kit load into both looks with the Modern look's materials applied | a replaced car and track texture set shown in both looks with no seams or fallback |

## 1. What the player sees

| # | Outcome | Measure | Pass |
| --- | --- | --- | --- |
| P1 | Every 3D object is drawn | Per frame, objects the game submits (render queue records, shadows, particles) vs objects the native renderer draws | equal on every frame of the scene suite (section 4) |
| P2 | Characters animate, damage shows | Bone and dent matrices the native renderer uses vs the matrices the game's pose code wrote (0x34C9F8 input) | bit-identical; skinned vertices within 1/16 px of the GS reference at 1x |
| P3 | PS2 look matches the original | The same frame drawn by the native renderer (PS2 look, 1x) vs GS replay of that frame | mean abs error <= 1/255 per channel, 99th percentile <= 8/255, SSIM >= 0.98; fog and sky pixels exact within 2/255; no missing or extra object (P1) |
| P4 | Modern look adds only what is wanted | Modern frame: lit materials, shadows, IBL. No vignette, colour grade, tone curve or haze. Fog colour, fog distances and night levels come from the game's values | review of every scene-suite frame, plus fog/sky sample pixels within 4/255 of the PS2 look |
| P5 | The look switch is instant | Toggle Modern <-> PS2 in the menu during a race | next frame shows the new look; no frame over 33 ms; GPU memory flat after 100 toggles |
| P6 | Original mode is unchanged | GS replay of a recorded race dump with the current build | VRAM md5 equal to the reference build (`f30d64c0...`, docs/SCENE_CAPTURE.md) |
| P8 | Split screen looks like single player | 2-player frame vs 1-player frame of the same track and camera position: same track passes (water, reflections, detail), same LOD and draw distance, no extra culling, every look and renderer | identical object and pass lists per view (render trace); water visible on Woodly (AFTON.TRK) in both views |
| P7 | Simulation and netplay are unchanged | Two-instance netplay race plus a solo race with recorded input | no desync; race result and lap times identical to the reference build |

## 2. Speed (borderless or exclusive fullscreen only)

Sign-off runs are borderless fullscreen or exclusive fullscreen (never windowed; vsync on, the
player's settings file), with a Tracy capture and the Vulkan GPU timer (`EOD_GPU_PROF`): a 5-minute
scripted race plus 2 minutes of career free roam. Headless runs render every frame on the GPU and
are used for day-to-day image comparisons, correctness and per-pass GPU timings; they cannot
measure presentation, vsync pacing or the fullscreen display path, so S1, S2 and P12 are signed
off only from fullscreen runs.

| # | Hardware | Settings | Pass |
| --- | --- | --- | --- |
| S1 | Target: i5-7600K + Radeon Pro 580 | RenderScale 6, max LOD, Modern look | every frame presented at 60 fps (0 missed vblanks), game speed 100% (vblank count / wall time), p99 CPU frame <= 12 ms, p99 GPU frame <= 14 ms |
| S2 | Minimum: Windows 10, 2015-era Vulkan GPU incl. integrated (Intel UHD 620 class) | RenderScale 1-2, PS2 look, standard LOD | same 60 fps / 100% speed / 0 missed vblanks rule |
| S3 | Any capable GPU | RenderScale 6, max LOD | S1 rule |

Per-pass GPU budget at 6x on the target (set once the native passes exist, then held): shadows,
opaque, transparent, post, UI. A pass over budget is a failure even if the frame fits.

CPU budget: the frame path runs no VU1 microcode and no GS emulation (S4). Today compiled VU1 is
about half of EE time; removing it is part of meeting S1.

| # | Measure | Pass |
| --- | --- | --- |
| S4 | Emulated graphics in the frame path | per frame: 0 VU1 microprogram runs, 0 GS primitives rasterised by the emulator, in every scene of the suite |

## 3. Decompilation (how each subsystem is proven)

A subsystem is done when all of these hold:

| # | Measure | Pass |
| --- | --- | --- |
| D1 | Readable C++ replaces each listed function (hle.txt `wrap`, `diff_run`) | every function in the subsystem's doc table |
| D2 | Equivalence on live game state (`EOD_DIFF`) | 0 mismatches; every function checked at least 1,000 times, and on every code path the scene suite reaches (paths never reached are listed in the doc) |
| D3 | Original mode | P6 and P7 hold with the replacement active |
| D4 | Real game run | build, install over the game (backup kept), boot, menus, race, career, exit cleanly |
| D5 | Documentation | docs/<SUBSYSTEM>.md: addresses, structs, call graph, evidence, what is still open |

Subsystem order: 3D submission, skeletal animation and deformation, textures/materials/light/fog,
particles, track loading/LOD/culling, allocator, frame loop/PATH2/render targets/speed blur, HUD,
car and asset loading, collision/world data, camera. Physics, AI, race rules and netplay stay
recompiled.

Ownership: a GS job counts as owned only when a native path draws it in both looks. The frame path
has no emulator left (S4) only when every GS job below is owned:

| GS job | Native owner |
| --- | --- |
| 3D models (render queue 0x34AF98) | native renderer, fed from the queue records |
| Projected shadows (0x34C208, 0x34C4B0, render target 0x34C7D0) | native shadow pass |
| Particles and effects | native particle pass |
| HUD, menus, fonts (PATH2/PATH3) | native UI pass |
| Render-to-texture, speed blur, off-screen buffers (e.g. FBP 0x1180), VRAM readbacks | native render targets |
| Movies, loading screens, memory-card screens | native UI pass |

## 4. Scene suite

Every measure above runs on all of these, Original and native, with the same scripted input:
Afton Jump Race (1 player and 2-player split screen), career free roam (day and night), dealer,
garage, front end menus, loading, memory card screen, a movie, a crash replay, a scene with
projected shadows and one with speed blur.

## 5. Milestones

Each milestone is one goal for a working session: a fixed end state, the check that proves it,
and what it must not break. A milestone is met only when its check passes on the current build;
"should work" or partial passes do not count. Every milestone also keeps the standing guards:
P6 (Original GS replay md5), P7 (determinism), D4 (real game run, clean exit), no PS2 limit kept.

| M | End state | Proof (all must pass) | Out of scope |
| --- | --- | --- | --- |
| M0 | The render queue (8 functions) runs as readable C++ | `EOD_DIFF` 0 mismatches in race, free roam, dealer; render trace: batches outside a flush = 0 | flush emission internals |
| M1 | Every VU1 program start in a frame has a known EE source | `EOD_RENDER_TRACE` attributes 100% of VU1 program starts (by program address, from the `vu1 programs` line) to a named sender (queue batch, shadow, particle feeder, HUD, setup) in race, 2-player split screen, free roam, dealer; unattributed = 0. Every entry function either checked by `EOD_DIFF` (>= 1,000 calls) or listed with the scene that reaches it | rewriting the senders |
| M2 | A per-frame scene snapshot built only from game data (queue records, camera packet, light/colour setters, bone palettes, dent offsets) | a check mode compares each snapshot object with what VU1 receives for it: model matrix, camera, colours, light, bones bit-identical; object count = batch count; 0 mismatches over 10,000 frames of the scene suite | drawing anything |
| M3 | Textures, materials, light and fog decompiled | D1-D5 for the texture slot / material packet / fog functions; snapshot carries texture id, mip set, TEX0/TEX1/ALPHA/fog values matching the GS registers they produce (0 mismatches) | the renderer |
| M4 | The native renderer (plume, docs/NATIVE_RENDERER.md) draws the snapshot in the PS2 look beside the GS path | P1 equality on every frame of the scene suite; fullscreen run boots, races, exits cleanly | Modern look, HUD |
| M5 | PS2 look matches GS replay | P3 thresholds on every scene-suite frame, with the comparison tool committed | performance |
| M6 | No emulated graphics in the frame path | S4 = 0 in every scene: shadows, particles, HUD/menus/fonts, render targets, speed blur, movies, loading and memory-card screens all drawn natively; GS emulator only in the offline reference tool | Modern look |
| M7 | Modern look and the live toggle | P4 and P5 | new art |
| M8 | Speed floors | S1 and S2 with Tracy captures and `EOD_GPU_PROF` logs from fullscreen runs, per-pass GPU budget recorded and held | DLSS |

Goal text for a session (example, M1): "Every VU1 program start in race, 2-player split screen,
free roam and dealer frames is attributed by `EOD_RENDER_TRACE` to a named EE sender, with 0
unattributed starts; each sender's entry point is in docs/EE_RENDER_SUBMISSION.md with its scene;
the Original GS replay md5 is unchanged and the build runs in borderless fullscreen through a race and exits
cleanly. Stop and report if a sender needs a design decision."

## 6. Status (2026-11-18)

| Item | State | Evidence |
| --- | --- | --- |
| Render queue (0x34BDA8, 0x34B140, 0x34BF90, 0x34CC98, 0x34C9C0, 0x34C930, 0x34C860, 0x34AF98) | M0 met for 6 of 8: `EOD_DIFF` 0 mismatches in race, free roam and dealer (headless). 0x34BF90 and 0x34CC98 were never called in those scenes, so they are unchecked | docs/EE_RENDER_SUBMISSION.md |
| M1 | met for VU1: every VU1 program start attributed to a named sender, 0 unattributed, in race, free roam, dealer and 2-player split screen. Senders: draw queue, track world renderer `0x358BD0`, shadows, immediate geometry API, particles, effect sprites, texture manager, setup. HUD/PATH2/PATH3 GS work is not VU1 and is counted under M6 | docs/EE_RENDER_SUBMISSION.md, "Every VU1 program start by sender" |
| Bone palette write (0x35DBD8, 0x35A810) | D1 done; D2 race: 0 mismatches | docs/EE_RENDER_SUBMISSION.md |
| Native renderer | decided: own renderer on plume (Filament rejected, docs/NATIVE_RENDERER.md); plume's Vulkan backend builds with the project toolchain | stage R0 next |
| P3 comparison tool | not started | needed before any native PS2-look work is judged |
| S4 counters | not started | per-frame VU1 program and emulated GS primitive counts |

## 7. Stage R2: track world, sky, water (2026-11-18)

Build `ninja -C build eod`, headless and muted (`EOD_HEADLESS=1 EOD_MUTE=1 --headless`),
`FrameSkip=0`, RenderScale 1, Radeon Pro 580. Code: `src/native_world.cpp`,
`runtime/render/native_feed.cpp`, `runtime/render/native_renderer.cpp`; spec in
docs/NATIVE_RENDERER.md "Stage R2". Every run logged `[SESSION] start: Eve N ... contest M`.

### D2: EOD_DIFF (0 mismatches each; counts are the harness's last power-of-two report)

| function | checked | mismatched | run |
| --- | --- | --- | --- |
| `0x214B68` WorldFrameRender | 2,048 | 0 | Woodly (Afton.trk) Jump Race, `EOD_SPLIT_PARITY=0` |
| `0x3582B8` MlistVisibilityCalc | 1,024 | 0 | same |
| `0x358BD0` WorldRenderPass | 4,096 | 0 | same |
| `0x35DEE0` WorldPacketBegin | 2,048 | 0 | same |
| `0x35CBB8` PacketCommit | 2,048 | 0 | same |
| `0x223858` SkyDraw | 1,024 | 0 | same |
| `0x223800` SkyDestroy | 1,024 | 0 | deterministic replay `EOD_R2_REPLAY_SKY_DESTROY=2` (it runs only at track unload) |
| `0x326168` WaterDrawIfPresent | 2,048 | 0 | same race |
| `0x325728` WaterDraw | 1,024 | 0 | deterministic replay `EOD_R2_REPLAY_WATER=2` (surface seldom in view) |
| `0x325E40` WaterSimulate | 3,072 | 0 | same race |

Not reached by `EOD_DIFF`: the split-screen branch of `0x214B68` with parity off (the shipped
default runs the single-view branch, which is the one checked).

### P1: world leaves and water submitted == drawn (every frame, `EOD_NATIVE=1`)

| scene | frames | world leaves drawn / submitted | water drawn / submitted | heap (`EOD_HEAP_CHECK=1`) / exit |
| --- | --- | --- | --- | --- |
| Woodly (Afton.trk) Jump Race 1P, 3000 vb | 1,634 / 1,634 equal | 705,071 / 705,071 | 1,232 / 1,232 | 0 corruptions / clean |
| Career free roam (`mode=career;place=home`), 3000 vb | 2,310 / 2,310 | 291,231 / 291,231 | 0 / 0 | 0 / clean |
| Dundas No-Rules Race 1P, 3000 vb | 1,884 / 1,884 | 1,139,087 / 1,139,087 | 0 / 0 (no water body) | 0 / clean |
| Dundas No-Rules Race 2P split, parity on, 3000 vb | 1,835 / 1,835 | 1,404,456 / 1,404,456 | 0 / 0 | 0 / clean |
| Carnage: Anoka Battle (8 AI), chickens, x from 600, l2 every 30, 3600 vb | 2,761 / 2,761 | 808,882 / 808,882 | 0 / 0 | 0 / clean |
| `EOD_NATIVE_ONLY=1` Woodly, 1500 vb | 476 / 476 | 185,011 / 185,011 | 424 / 424 | 818,236 GS primitives dropped |

0 world records dropped for packet space in every run; camera-facing parts (VU1 `0x350`) drawn
natively with the sun quad found every time (`no sun quad 0`).

### P8 and Dundas water (finding)

- Retail data: of the 15 `track.grf` files only AFTON.TRK (in-game Eve 16 'Woodly') has a Water
  display object (subtype 28); DUNDAS.TRK has none, and its mlists have no water texture, so
  `0x325728` never draws on Dundas in 1P or 2P (`*(0x3DC134) == 0`, logged by
  `EOD_NATIVE_WATER_LOG=1`). Water on Woodly is drawn natively in every frame it is in view.
- Split screen: both the PS2 (`0x214B68`) and the Xbox (`FUN_000a8390`) skip world passes 1, 2, 4
  and use the coarse mlist, and `0x326188` creates the water surface only for one view
  (`*(0x439858) < 2`). `SplitScreenParity=1` (default) gives split views the single-view
  sequence, mlist and water. 2-player Battle on Woodly (AFTON.TRK) is available in retail; split-screen
  water verification on Woodly Battle is deferred/pending.

### Still open after R2

Per-vertex fog and mip levels (R5); the projected-shadow, particle and immediate-geometry passes
that `0x214B68` calls are still drawn by the GS (R4); PS2 caps owned here are tracked, not
removed (docs/PS2_LIMITS.md).

## 8. Stage R3: skinned characters and car deformation (2026-11-19)

Build `ninja -C build eod`, headless and muted (`EOD_HEADLESS=1 EOD_MUTE=1 --headless`),
`Detail=maximum`, `DrawDistance=8`, `FrameSkip=0`, RenderScale 1, Radeon Pro 580. Code:
`src/bone_pose.cpp`, `src/car_damage.cpp`, `src/debris_pool.cpp`, `src/hle_camera.cpp`,
`runtime/render/native_feed.cpp`, `runtime/render/native_renderer.cpp`; spec in
docs/NATIVE_RENDERER.md "Stage R3".

### D2: EOD_DIFF (0 mismatches across Phase A3 and Phase B)

| function | Phase A3 checked | Phase B checked (`EOD_DIFF_EVERY=32`) | mismatched | notes |
| --- | --- | --- | --- | --- |
| `0x35DBD8` BonePaletteWrite | (R1/M0) | 184 (5,535 live calls) | 0 | 6 scenes + P2 gate |
| `0x35A810` BonePaletteUpload | (R1/M0) | 28 (752 live calls) | 0 | same |
| `0x35DD28` PoseWrite | (R1/M0) | 264 (7,388 live calls) | 0 | same |
| `0x34CA18` PoseSubmit | (R1/M0) | 267 (7,422 live calls) | 0 | same |
| `0x17B918` ContactRecord | — | 25 (699 live calls) | 0 | collisions in Battle / Derby / 2P |
| `0x18CA98` CarApplyCollisionDamage | >= 1,374 | 3 (19 live calls) | 0 | Phase A3 + Dundas/Derby |
| `0x18AFE0` CarSetupDamageAndPanels | >= 1,068 | 7 (65 live calls) | 0 | Phase A3 + all 6 scenes |
| `0x1E1E88` 'Panl' ctor | >= 2,088 | 6 (72 live calls) | 0 | Phase A3 + all 6 scenes |
| `0x1E29C0` 'Panl' dtor | 1,112 | — | 0 | Phase A3 Eagan derby teardown |
| `0x1B3190` wheel pool reset | 3,005 | 6 (6 live calls) | 0 | Phase A3 + all 6 scenes |
| `0x1B3180` wheel count reset | 1,002 | — | 0 | Phase A3 teardown |
| `0x1B3068` DetachedWheelSpawn | >= 5,152 | — | 0 | Phase A3 Gilroy night / Dundas wheel replay (nested under `0x18CA98` when both watched) |

### P2: bone palettes, dent offsets, and posed vertices vs GS reference (1x)

Run: Dundas 'Battle (8 AI)', Hearse (`vehicle 502`), `EOD_CHICKENS=1` (L2 every 30 vb), 1,800 vb,
`EOD_NATIVE=1 EOD_NATIVE_SKIN_CHECK=1`.

| check | compared | pass / matched | error / detail |
| --- | --- | --- | --- |
| EE bone palette vs `0x34C9F8` input | 756 checks (241,920 words) | 241,920 / 241,920 (0 mismatched) | bit-identical |
| VU1 bone & dent words vs VU1 data memory | 8,192 records (581,652 words) | 581,652 / 581,652 (0 mismatched) | bit-identical |
| Posed vertices vs GS (1x, non-clip) | 9,364 objects (4,672,296 vertices) | 4,667,998 within `1/16 px` (99.91%) | p99 `0.0625 px`, max `0.0672 px`, mean `0.0386 px` (bind pose off > `1/16 px` at 3,277,330 vertices) |
| Posed vertices vs GS (1x, clip path) | 764 objects (247,134 vertices) | 232,170 within `1/16 px` (93.94%) | CPU frustum re-clip vs VU1 clipper |

### R3.2a Classic static queue projection at 1×

Identical recorded Dundas GS inputs and submitted transforms replayed on the
Mac M2 Pro through MoltenVK. Red and green used the same forward samples and
the same native vertex indices inside the original reverse scissor margin.

| Input | Forward misses before / after | Reverse misses before / after | Max error after |
| --- | ---: | ---: | ---: |
| Moving camera | 4,087 / 0 of 3,689,031 | 1,981 / 0 of 1,954,037 | 0.00005261 px |
| Batch `0x00924DD8` | 21 / 0 of 14,148 | 12 / 0 of 8,864 | 0.00003702 px |
| Fixed-position TV camera | 905 / 0 of 728,688 | 457 / 0 of 358,704 | 0.00002758 px |

The TV camera held its position within 0.000025 world units across 82 captured
frames. Clip Z/W remained bit-identical across 2,035,964 moving GPU samples
against the original default arithmetic. The Plume graphics fixture failed
on the old projection and passed with the new vertex entry and a nonzero
instance index. Existing Classic and Modern fixture colors matched in both
builds. The excluded posed control had 0 of 118,680 forward and 0 of 76,679
reverse misses. The clipped control still had 5,740 of 53,376 forward misses.
Late dent-input integrity, clipping-generated geometry and topology remain
open in R3.1 and R3.2.

The exact shared Mac build passed 97/97 tests. The 1× live Classic TV and
moving Dundas runs each recorded zero non-clip forward and reverse misses
across 10,663,728 and 22,525,389 GS samples. A Modern-start smoke completed
eight switches with 22 pipelines at first switch and exit. Windows x64
cross-linked. Windows runtime verification remains open.

### P1: multi-scene suite (`EOD_NATIVE=1 EOD_DIFF=... EOD_HEAP_CHECK=1`)

Special vehicles rotated without Venom or back-to-back repeats; camera views cycled via Triangle
(pad event 0) and `EOD_CAMERA_KEYS` (`KeyPush 0x33C168`).

| scene | vehicle | frames equal (queue / world) | queue drawn / submitted | world drawn / submitted | `EOD_DIFF` / heap / exit | camera views covered |
| --- | --- | --- | --- | --- | --- | --- |
| Dundas 'Battle (8 AI)', max cars, `DrawDistance=8`, `EOD_CHICKENS=1` (L2/30 vb), 2200 vb | Bus (`790`) | 448/448 / 448/448 | 11,970 / 11,970 | 73,490 / 73,490 | 0 / 0 / clean | Cockpit, NearChase, Chase, FarChase, Rear, RearChase, Overhead, TV |
| Gilroy 'Battle (8 AI)' night, `EOD_CHICKENS=1` (L2/30 vb), 2200 vb | Taxi (`508`) | 399/399 / 399/399 | 21,607 / 21,607 | 134,980 / 134,980 | 0 / 0 / clean | Cockpit, NearChase, FarChase, Rear, RearChase, Overhead, TV |
| Career free roam (`mode=career;place=home`), 2000 vb | Career car | 371/371 / 371/371 | 5,330 / 5,330 | 28,001 / 28,001 | 0 / 0 / clean | Cockpit, NearChase (plus front-end physcam 31) |
| Car dealer (`mode=career;place=dealer`), 1500 vb | Career car | 217/217 / 217/217 | 11,423 / 11,423 | 28,231 / 28,231 | 0 / 0 / clean | Cockpit, NearChase (plus front-end physcam 31) |
| Eagan 'Demolition Derby', 2200 vb | Copcar (`504`) | 434/434 / 434/434 | 21,673 / 21,673 | 26,293 / 26,293 | 0 / 0 / clean | Cockpit, NearChase, Chase, FarChase, Rear, RearChase, Overhead, TV |
| Dundas 'No-Rules Race' 2P split-screen (`humans=2`), 2200 vb | Postal (`503`) | 490/490 / 490/490 | 28,279 / 28,279 | 58,380 / 58,380 | 0 / 0 / clean | Cockpit (plus lobby physcam 19) |
| `EOD_NATIVE_ONLY=1` Dundas 'Battle (8 AI)', 1200 vb | Ambulance (`601`) | 363/363 / 363/363 | 11,644 / 11,644 | 76,726 / 76,726 | — / 0 / clean | 133,823 GS primitives dropped |

Chassis (2) and Bumper (1) views were additionally verified in Phase A3 (`r3a3_cockpit`, Bus, Mankato
Demolition Derby: 81/81 Chassis frames and 49/49 Bumper frames P1 equal; see docs/NATIVE_RENDERER.md).

### Open items for R4 (and deferred R2 items)

- **R4 — non-queue cockpit quad:** `non-queue quad in 0x219C48 via 0x35CAA0` (built on the stack when
  `0x2114A8() != -1`, between `0x34F928` / `0x34F888`, outside the draw queue).
- **R4 — `EOD_NATIVE_ONLY=1` dropped GS primitives:** 133,823 GS primitives dropped in 1,200 vb of
  Dundas 'Battle (8 AI)' (and in Phase A3 `r3a3_cockpit_only`: Chase 1,181/frame, Chassis 781/frame,
  Bumper 872/frame, Cockpit 932/frame) — projected shadows, particles, effect sprites, immediate
  geometry, and 2D/HUD still drawn by the GS emulator.
- **Deferred from R2:** `0x214B68` split-screen parity-off gate and split-screen water verification on
  Woodly (AFTON.TRK) 2P Battle.

## 9. Stage R4: shadows, particles, sprites, flares, immediate geometry (2026-11-19)

Build `ninja -C build eod`, headless and muted (`EOD_HEADLESS=1 EOD_MUTE=1 --headless`),
`Detail=maximum`, `DrawDistance=8`, `FrameSkip=0`, RenderScale 1 (and RenderScale 2 for adaptive
shadow RT proof), Radeon Pro 580. Code: `src/native_shadows_flares.cpp` (Phase A1, `d72b320`),
`src/native_effects_imm.cpp` (Phase A2, `6734733`), `runtime/render/native_feed.cpp`,
`runtime/render/native_renderer.cpp`; spec in `docs/NATIVE_RENDERER.md` "Stage R4".

### D2: EOD_DIFF (0 mismatches across all 28 live-reachable R4 functions; `>= 1,000` checked each)

Live multi-scene runs (`EOD_DIFF_EVERY=64` across Dundas Battle, Gilroy Night, Woodly Jump Race,
Career free roam, Car dealer) plus deterministic `EOD_DIFF_REPLAY` (`1,000` replays/function on
Woodly Jump Race Postal `503` and `0x363C18` on Gilroy Night Bus `790`):

| function | role (phase) | multi-scene live calls / checked | D2 replay checked | mismatched | notes |
| --- | --- | --- | --- | --- | --- |
| `0x34C7D0` | ShadowBegin (A1) | 135 / 5 | 1,001 (368 live) | 0 | all 5 diff scenes + replay |
| `0x34C798` | ShadowEnd (A1) | 135 / 5 | 1,001 (368 live) | 0 | same |
| `0x34C208` | ShadowSubmit (A1) | 1,951 / 34 | 1,001 (13,208 live) | 0 | same |
| `0x34C4B0` | ShadowSubmitMasked (A1) | 131 / 3 | 1,001 (2,347 live) | 0 | Gilroy Night, Woodly + replay |
| `0x35D170` | ShadowEmitBatchPass (A1) | 1,741 / 30 | 1,001 (20,085 live) | 0 | all 5 diff scenes + replay |
| `0x35D298` | ShadowEmitBatch (A1) | 1,772 / 31 | 1,001 (20,086 live) | 0 | same |
| `0x304D50` | FlareRender (A1) | 135 / 5 | 1,001 (368 live) | 0 | same (`0x20F2D8` lifts 2P gate under `SplitScreenParity`) |
| `0x363B98` | ParticleBegin (A2) | 105 / 3 | 1,001 (1,232 live) | 0 | Dundas, Gilroy, Woodly + replay |
| `0x3639D0` | ParticleAdd (A2) | 814 / 14 | 1,001 (18,612 live) | 0 | same |
| `0x363B68` | ParticleEnd (A2) | 105 / 3 | 1,001 (1,232 live) | 0 | same |
| `0x363FF8` | ParticlePacketReserve (A2) | 131 / 3 | 1,001 (2,073 live) | 0 | same |
| `0x363F48` | SpriteBegin (A2) | 441 / 11 | 1,001 (2,507 live) | 0 | all 5 diff scenes + replay |
| `0x363DE8` | SpriteAdd (A2) | 1,569 / 26 | 1,001 (49,641 live) | 0 | same |
| `0x363F18` | SpriteEnd (A2) | 441 / 11 | 1,001 (2,507 live) | 0 | same |
| `0x364548` | SpritePacketReserve (A2) | 430 / 10 | 1,001 (3,236 live) | 0 | same |
| `0x363D78` | AtlasSpriteBegin (A2) | 42 / 2 | 1,001 (360 live) | 0 | Gilroy, Woodly + replay |
| `0x363C18` | AtlasSpriteAdd (A2) | 454 / 8 | 1,001 (2,268 live) | 0 | Gilroy, Woodly + Gilroy replay |
| `0x363D48` | AtlasSpriteEnd (A2) | 42 / 2 | 1,001 (360 live) | 0 | Gilroy, Woodly + replay |
| `0x3642B8` | AtlasSpritePacketReserve (A2) | 40 / 2 | 1,001 (359 live) | 0 | same |
| `0x35DF10` | ImmediateBegin (A2) | 16,553 / 262 | 1,001 (13,837 live) | 0 | all 5 diff scenes + replay |
| `0x361468` | ImmediatePacketBegin (A2) | 16,291 / 258 | 1,001 (13,836 live) | 0 | same |
| `0x361708` | ImmediateVertex (A2) | 104,956 / 1,642 | (1,642 live) | 0 | `>= 1,000` live checks directly |
| `0x361BC8` | ImmediateEnd (A2) | 16,553 / 262 | 1,001 (13,837 live) | 0 | all 5 diff scenes + replay |
| `0x364D18` | ImmediateVertexContinue (A2) | 138 / 4 | 1,001 (3,088 live) | 0 | Dundas, Gilroy, Woodly + replay |
| `0x34EE70` | SphereImmSubmit (A2) | 18 / 1 | 1,001 (368 live) | 0 | Woodly + replay |
| `0x34EEB8` | SphereImmSubmitTransformed (A2) | 0 / 0 | — | 0 | unreferenced in retail `SLUS_209.10` (0 callers in `gen/`) |
| `0x361ED8` | ImmStaticDraw (A2) | 13 / 1 | 1,001 (367 live) | 0 | Woodly + replay |
| `0x361C30` | ImmStaticEmitRef (A2) | 12 / 1 | 1,001 (366 live) | 0 | Woodly + replay |
| `0x35CAA0` | ImmediateDrawIndexedTris (A2) | 16,323 / 257 | 1,001 (11,761 live) | 0 | all 5 diff scenes + replay (incl. cockpit quad path) |

### P1: multi-scene suite (`EOD_NATIVE=1 EOD_HEAP_CHECK=1`, all 9 native kinds)

Special vehicles rotated without Venom or back-to-back repeats (Hearse `502`, Bus `790`, Ambulance
`601`, Career car, Taxi `508`, Copcar `504`, plus Postal `503` for D2 replay); camera views cycled
via `EOD_CAMERA_KEYS` (`KeyPush 0x33C168`) and Triangle pad events, covering all 10 gameplay views
including Cockpit (`0`), Bumper (`1`), and Chassis (`2`). Every frame in every scene was 100% P1
equal across all 9 kinds (`queue`, `world`, `water`, `shadow`, `flare`, `particle`, `sprite`,
`immediate`, `cockpit`), with 0 `EOD_DIFF` mismatches, 0 heap corruptions, and clean exit (`0`).

| scene | vehicle | P1 frames equal (all 9 kinds) | queue / world / water | shadows / flares | particles / sprites / imm / cockpit | diff / heap / exit | camera views covered |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Dundas 'Battle (8 AI)', max cars, `DrawDistance=8`, `EOD_CHICKENS=1` (L2/30 vb), 2150 vb | Hearse (`502`) | 1,112 / 1,112 | 8,192 / 56,969 / 0 | 358 / 49 | 13 / 104 / 3,164 / 0 | 0 / 0 / clean | Cockpit, Bumper, Chassis, NearChase, Chase, FarChase, Rear, RearChase, Overhead, TV |
| Gilroy 'Battle (8 AI)' night, `EOD_CHICKENS=1` (L2/30 vb), 2150 vb | Bus (`790`) | 933 / 933 | 9,895 / 72,654 / 0 | 288 / 0 | 14 / 113 / 2,747 / 0 | 0 / 0 / clean | Cockpit, Bumper, Chassis, NearChase, Chase, FarChase, Rear, RearChase, Overhead, TV |
| Woodly 'Jump Race' 1P, 2150 vb | Ambulance (`601`) | 913 / 913 | 12,656 / 13,642 / 10 | 496 / 10 | 18 / 24 / 2,896 / 0 | 0 / 0 / clean | Cockpit, NearChase, FarChase, Rear, RearChase, Overhead, TV |
| Career free roam (`mode=career;place=home`), 1800 vb | Career car | 426 / 426 | 5,954 / 28,157 / 0 | 160 / 74 | 0 / 10 / 27,566 / 0 | 0 / 0 / clean | Cockpit, NearChase, physcam 31 |
| Car dealer (`mode=career;place=dealer`), 1800 vb | Career car | 873 / 873 | 4,230 / 21,577 / 0 | 168 / 9 | 0 / 6 / 2,877 / 0 | 0 / 0 / clean | Cockpit, NearChase, physcam 31 |
| Dundas 'No-Rules Race' 2P split-screen (`humans=2`, `SplitScreenParity=1`), 1800 vb | Taxi (`508`) | 514 / 514 | 18,887 / 41,461 / 0 | 3,078 / 0 | 50 / 153 / 53,604 / 0 | — / 0 / clean | Cockpit, 2P lobby physcam 19 |
| `EOD_NATIVE_ONLY=1` Dundas 'Battle (8 AI)', `EOD_CHICKENS=1`, 1500 vb | Copcar (`504`) | 352 / 352 | 9,501 / 57,966 / 0 | 406 / 53 | 0 / 167 / 14,742 / 0 | — / 0 / clean | Cockpit, Bumper, NearChase, Chase, FarChase, TV; **12,337** GS prims dropped |

Notes on specific paths:
- **Cockpit quad (`0x219C48` -> `0x34DB70` -> `0x35CAA0`):** `0x35CAA0` is replaced in readable C++
  and diff-verified (`16,323` live calls + `1,001` replay checks, `0` mismatches) with `KIND_COCKPIT`
  wired for `ra == 0x21A064`. In these runs `0x2114A8()` returned `-1` (no active overlay texture
  slot), so `0x219C48` skipped the optional quad (`cockpit = 0/0`) while all cockpit interior and
  gauge meshes drew via the queue.
- **2P split-screen flares (`0x20F2D8`/`0x304D50`):** `0x20F2D8` lifts the single-viewport gate
  (`*(0x3D1A74) == 1`) under `SplitScreenParity=1` (when `EOD_DIFF` is off). In the Dundas 2P native
  lobby (`physcam 19`), both split viewports render shadows (`3,078/3,078`), world, queue, particles,
  and sprites natively, while `0x304D50` returns early on `0x11F9A0() != 0` (menu/lobby overlay).

### PS2 limits removed & `0x25F0` -> `0x2630` finding

- **PS2 `320x512` shadow RT cap removed (`0x35DA30`):** Verified on Dundas 'Battle (8 AI)' (Hearse
  `502`, 2150 vb) at `--scale 2` (`1280x896`, exceeding PS2's `320x512` cap on both axes): Plume
  allocated its host shadow accumulation buffer (`R8G8B8A8_UNORM`) at full scaled resolution
  depth-tested against the full-resolution `D32` depth buffer and rendered `224 / 224` shadows
  (`865/865` frames P1 equal, `0` diff mismatches, `0` heap corruptions).
- **PS2 VU1 chunk caps removed (P10):** Particles (`16`/chunk in `0x363FF8`), sprites (`33`/chunk in
  `0x364548`/`0x3642B8`), and immediate geometry (`60` vertices/chunk in `0x361468`; high-water `96`
  observed in Dundas Battle) each stream into a single dynamically growing Plume mesh per pass.
- **VU1 `0x25F0` -> `0x2630` resolved:** Sent by `0x3620A8` (shared GS/VU1 state flush). `0x25F0` is
  a 5-instruction state-setup program storing two GIFtag PRIM templates into VU1 data memory qwords
  `18` and `19` (0 GS primitives emitted); `0x2630` is an internal `bal` subroutine called from the
  existing main mesh microcode (`0x05B8`, `0x1058`, `0x1878`, `0x1B60`), not a standalone VIF entry.
- **`EOD_NATIVE_ONLY=1` remaining 2D count:** `12,337` dropped GS primitives over 1,500 vb (352
  frames, ~35 prims/frame) in Dundas 'Battle (8 AI)' vs `133,823` in R3 (1,200 vb, 363 frames,
  ~369 prims/frame) — a **90.8% reduction**, leaving only 2D/HUD/menus for R7.

### Open items for R5–R7 (and deferred R2 items)

- **R5 (visual parity & shading):** Per-vertex fog (`FGE`), mipmap levels (`TEX1`), region
  clamp/repeat, destination-alpha blend factor, `AFAIL FB_ONLY/RGB_ONLY`.
- **R6 (performance / VU1 bypass):** Skip redundant VU1 microcode execution for native-drawn
  batches once GS-free parity is complete.
- **R7 (2D, HUD & menus):** Move the remaining `12,337` dropped GS primitives (~35/frame in race)
  to Plume so `EOD_NATIVE_ONLY=1` reaches 0 dropped primitives.
- **Deferred from R2:** `0x214B68` split-screen parity-off gate and split-screen water verification
  on Woodly (AFTON.TRK) 2P Battle.

## 10. Stage R6: Modern look materials, Phase B suite (2026-11-20)

Numbers only (owner rule: no image comparisons until the end of R7). Build: `build/eod.exe` at
`85d8aff` (`ninja -C build eod`: no work to do, no stale objects). Every run: headless, muted,
`RenderScale=1`, `Detail=maximum`, `DrawDistance=8`, `EOD_NATIVE=1 EOD_LOOK=modern` (Modern VFX on
by default), look toggled every 30 vb from vb 1500 (24 toggles, `EOD_LOOK_TOGGLE_*`), Triangle on
the pad script at vb 900 / 1300 / 1700 / 1900 / 2100 (camera cycle Cockpit -> NearChase -> Chase
-> Chassis -> Bumper -> Cockpit -> NearChase), `EOD_CHICKENS=1`, `EOD_HEAP_CHECK=1`,
`EOD_GPU_TIMING=1`, 2,300 vb. Runs in `D:/eod-runs/r6b` (`rb.sh`, `pair.py`, `sum.sh`).

Pairs (`pair.py`, from `EOD_LOOK_FRAME_LOG`): old look = the capture at T-1 (flushed before the
toggle), new look = the first new game frame at T+1..T+8 (1 vblank after the toggle, plus the
game's own frame step); toggles within -10..+12 vb of a camera change skipped.
Guardrails = `tools/modern_guardrails.py` rows 4/5, 6, 7, 8 (row 1 on the native-only run).

| Scene | `[SESSION] start:` | Views | Pairs | Rows 4/5 whole / region worst | Row 6 | Row 7 | Row 8 Modern - Classic | P5 pipelines (1st toggle -> end), sets late, textures MB | P1 (all 9 kinds) | Heap | Exit |
|---|---|---|---|---|---|---|---|---|---|---|---|
| (a) Dundas day, Bus `790`, `ai=12` | Eve 4 'Dundas' contest 323 'Battle (8 AI)' | 5 of 5 pad views | 20 | 1.032 / 1.117 | 0.0031% | 0% | +0.035 ms (3.906 / 3.941). PASS | 19 -> 19, 0, 205.3 -> 111.0 | 1510/1510 | 0 | 0 |
| (b) Gilroy night, Taxi `508` | Eve 6 'Gilroy' contest 325 'Battle (8 AI)' | 5 of 5 | 22 | 1.023 / 1.038 | 0% | 0% | +0.482 ms. PASS | 19 -> 19, 0, 171.0 -> 249.6 | 1056/1056 | 0 | 0 |
| (c) Baxter night (flares 744 in 300 vb), Limo `136` | Eve 2 'Baxter' contest 331 'Battle (8 AI)' | 5 of 5 | 19 | 1.022 / 1.078 | 0% | 0% | -0.216 ms. PASS | 17 -> 17, 0, 123.9 -> 127.6 | 1522/1522 | 0 | 0 |
| (d) Career free roam (`mode=career;place=malt`) | career start, TJ's Diner (Malt Shop) | 5 of 5 | 20 | 1.042 / 1.103 | 0.0209% | 0% | -0.076 ms. PASS | 17 -> 17, 0, 71.8 -> 73.7 | 1355/1355 | 0 | 0 |
| (e) 2P, Kenyon, Hearse `502`, `humans=2` (both pad scripts) | Eve 10 'Kenyon' contest 329 'Battle (8 AI)' humans 2 | Cockpit only (see open) | 21 | 1.009 / 1.017 | 0% | 0% | -0.968 ms. PASS | 17 -> 17, 0, 32.0 -> 32.0 | 1312/1312 | 0 | 0 |
| (f) `EOD_NATIVE_ONLY=1`, Hampton night, Postal `503` | Eve 7 'Hampton' contest 317 'Battle (8 AI)' | 5 of 5 | row 1: 19 frames | lum mean min 9.41 avg 11.27, nonzero min 44.3% avg 52.1%: **FAIL** (50% bar) | - | - | Modern 1.418 ms | 17 -> 17 | 1201/1201 | 0 | 0 |

P5: pipelines never grow after the first toggle in any scene; "sets made for a texture that
already had its Classic and Modern sets" 0 everywhere; the texture MB moves with streaming (Dundas
falls, Gilroy rises), not with the toggle. `EOD_NATIVE_ONLY=1` still drops 60,934 GS primitives in
2,300 vb (2D / HUD / menus, R7).

EOD_DIFF (starts the race): `EOD_DIFF=00214b68`, Dundas 'Battle (8 AI)' Bus, Look=Modern toggling
every 97 vb, 1,500 vb: `[SESSION] start:` reached, 506 calls, 506 checked, **0 mismatched**; EE
state vs GS immediate 21,060 objects, 0 mismatches. The known `0x214B68` first-call field (two words
0x70 apart at `0x772C60` / `0x772CD0`, upper halfword `0x0A30`) is accepted: it shows on Gilroy
Limo only (A5, Classic too), not on Dundas Bus.

### Open items (R6 -> R7)

- Hampton night Row 1 passed on the Apple M2 Pro Vulkan/MoltenVK build with Modern native-only at
  1×. The 48 race captures taken from vblank 400 onward had at least 58.20% non-black pixels and
  mean luminance of 11.21 or greater. At GS frame 1501, Plume drew all 948,607 submitted world
  leaves across 1,367 measured frames. A legacy `ModernRenderer=1` setting had drawn zero native
  world geometry before the two rendering paths were made mutually exclusive. The passing run
  kept the legacy setting and `EOD_MODERN=1` to exercise that fix. Native-only GS drops remained
  and the full R6 and R7 scene gates still require separate runs.
- 2P (`humans=2`): the pad Triangle did not change the camera (`[camera]` logs Cockpit only) and
  the textures stay at 32.0 MB; split-screen (view count `0x3D1A74` = 2) is not proven by this run.
- `0x214B68` first-call field on Gilroy Limo (stride 0x70 at `0x772C60`): accepted, not fixed.
- `EOD_DIFF=all` (and some multi-function lists) do not reach `[SESSION] start:` on Gilroy Limo
  (Classic too): bisect the stalling function.
- `metal_painted` metalness (A4).
- R5 open items: oval-track grandstand wedge, upper-right blurred rectangle (`FBP 0x1180`), 27 fps
  under `EOD_NATIVE_ONLY=1`, near-plane clip vs GS (depth clamp), P2 `0x318` back-face culling.

## R7.1d retail movie framebuffer handoff (2026-11-20)

On Apple M2 Pro with the borrowed Plume Vulkan device, the opt-in handoff
was compared off and on in 1280×960 at 4:3 and 1×, and 1280×720 at 16:9
and 3×. The runs used private saves, strict native level 1 and R7.1c
direct movie scanout. In each natural and Start comparison, the first
`logos.pss` crop at (40,16), 640×448 CT32, dropped from 165 synchronous
display-page reads to zero. Both settings measured zero movie-window
mid-frame submits. Enabled runs retired the two boundary commands in
order for each movie that played. The wide runs restored 16:9 at the
front end after presenting the movie at 4:3.

Natural playback completed both `logos.pss` and `campsiz.pss`. Start
ended `logos.pss` after about 2.8 seconds. Startup skip bypassed playback
and issued no boundary commands. All twelve window runs reached the
retail main menu. SDL opened the MacBook Pro speakers at 48 kHz, and
the stereo mixer captures matched 512-frame PCM segments at 30 sampled
positions in the 4:3 natural pair and 32 in the wide natural pair.
Offline software GS replay also matched VRAM, decoded depth and scanout
on the guarded clear cases and preserved the portable original GIF packet.

These results cover the tested Mac movie transitions and their ordering
and readback counters. Physical audio-to-display latency, native Windows
playback and the broader R7/S4 scene gates remain open.

## R7 VU1 MSCAL no-op retirement (2026-11-21)

The native renderer now retires the verified `0x24D8` VU1 no-op for ordinary
MSCALs from all producers. The VIF command still updates the double-buffer
registers. The live instruction check and the original dispatcher remain
available for other programs and VU diagnostics. Set
`EOD_NATIVE_CAMERA_NOP=0` for the reference dispatch.

On Apple M2 Pro with Plume Vulkan, Dundas Battle with the Bus, 12 AI, maximum
detail and draw distance at 6× Modern gave these render-trace samples:

| Vblank | VU1 starts with retirement off | VU1 starts with retirement on | No-ops retired |
|---|---:|---:|---:|
| 120 | 4,902 | 4,292 | 612 |
| 240 | 4,948 | 4,332 | 615 |
| 360 | 4,888 | 4,217 | 662 |
| 480 | 4,299 | 3,738 | 590 |

Counts are per sampled render frame. The independent runs can reach slightly
different simulation states at the same vblank. Native GPU time averaged
9.39 ms over 567 measured frames with retirement on. A separate strict-native
run at the same scale averaged 8.84 ms and still suppressed 226 cumulative GS
primitives by GS frame 480. These are headless diagnostics. The owner-PC S5 run
and fullscreen pacing remain open.

Full VU state, scoreboard and data memory comparisons passed with
`EOD_NATIVE_CAMERA_NOP_CHECK=1` on Dundas Modern, Gilroy night with the
interpreter forced on, and Kenyon Classic with two active viewports.
The Dundas `EOD_DIFF=00214B68` run checked 1,337 calls with zero mismatches.
The Mac executable built and CTest passed 97 of 97 tests. An observer run
on Woodly with `EOD_VU_STATS=1` kept `0x24D8` in normal dispatch.
The changed Windows translation units cross-compiled. Windows runtime
verification remains open.

S4 remains open. At vblank 240 in strict-native Dundas, the dominant remaining
world starts were `0x0270`, `0x0618` and `0x0920`, largely from the
`0x35DEFC` reservation. Batch work from `0x3613E0` follows, especially
`0x0D48` and `0x24B0`. The existing single-leaf retirement only covers
one exact opaque payload and still emits compatibility GIF data.

## R7 non-clipped world REF retirement (2026-11-21)

`EOD_NATIVE_WORLD_RETIRE=1 EOD_NATIVE_ONLY=1` now recognizes eligible
committed world REFs by the live packet and strip layout. It covers
variable vertex counts and origin/UV bases and retains the exact live
micro-image guard. The opt-in path still emits the compatibility GS
packets.

With `EOD_NATIVE_WORLD_BENCH_CHECK=1`, a 7,000-call Dundas VU capture
compared 1,128 non-clipped `0x0270` entries to the reference. All 1,128
matched the entire VU state and memory, active scoreboard and the bytes
and order of both GIF kicks. Live `EOD_NATIVE_WORLD_RETIRE_CHECK=1`
also passed 670 vblanks of Dundas day, 550 of Gilroy night and 650 of
two-view Kenyon. Sampled live retirements reached 1,080, 706 and 2,663
per frame, respectively. The independent `EOD_DIFF=00358BD0` run
checked 2,168 calls with zero mismatches.

At 6× in strict-native Dundas, vblank 240 recorded 4,332 VU1 dispatches
with retirement off and 3,280 with it on. The 1,052 difference equals
the retirement counter. The GPU means were 9.21 and 9.83 ms in separate
runs. This comparison measures removed VU1 dispatches and establishes
no GPU time improvement. S4 and S5 remain open, as does the requirement
to link the release without the VU1 and GS backends.

## R7 lifecycle audit baseline (2026-11-21)

R7 and S4 remain open. A rebuilt macOS executable on Apple M2 Pro
(Plume Vulkan over MoltenVK) logged these totals from boot through
worker drain. The headless runs used private saves, direct race
entry, movie skip, maximum detail, DrawDistance 8 and vsync off.
Dundas used the Bus with 12 AI. Strict mode also enabled the opt-in
non-clipped world REF retirement. Gilroy forced the VU interpreter.

| Run | Vblanks | VU1 dispatches | Compatibility GIF qwords | GS raster | GS suppressed | Sync VRAM | Deferred waits | CLUT waits |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Dundas Modern 6× | 520 | 1,759,320 | 263,550,084 | 677 | 0 | 138 | 0 | 0 |
| Dundas Modern 6×, strict | 520 | 1,341,365 | 268,029,890 | 0 | 657 | 0 | 0 | 0 |
| Kenyon Classic 1×, 2P | 500 | 2,044,135 | 285,815,663 | 24,857 | 0 | 138 | 87 | 0 |
| Gilroy Classic 1×, interpreted VU | 275 | 159,791 | 23,644,555 | 1,809 | 0 | 138 | 1 | 0 |

All four game processes exited cleanly and emitted exactly one summary.
The audit checker rejected all four. A 60-vblank game process with the
audit flag unset exited cleanly and emitted no summary. A synthetic
offline replay and a failed startup also emitted no audit summary
with the flag set. Checker fixtures exercised all-zero, missing,
malformed, duplicate, strict suppression and nonzero wait reports.

The Mac build and all 97 CTest cases passed. With the audit flag unset,
a separate Dundas `EOD_DIFF=00358BD0` smoke run checked 2,283 calls
with zero mismatches. The renderer's separate EE/GS queue comparison
reported one batch blend mismatch, a category also seen before this
audit. The seven changed Windows translation units cross-compiled.
Windows runtime behavior remains unverified.

The periodic Vulkan stats recorded 73 of Kenyon's 87 deferred waits
through vblank 480, with 2,330 preservation copies scheduled by that
point. The shutdown count includes the final interval. Gilroy recorded
33 lazy CLUT snapshots through vblank 240 with no CLUT wait, so these
runs establish deferred wait detection. A positive CLUT wait remains
unverified. The first 120 vblanks of normal Dundas logged 138 texture
readbacks and two completed deferred preservation copies. Trace those
boot and loading texture consumers to identify the next synchronous
readback path to remove.

These cases cover a limited boot, race and bounded shutdown sequence on
one Mac. The wider lifecycle still requires fresh install and upgrade,
movie playback and manual skip, menu transitions, Career save/load,
4P and the rest of the split-screen rotation, netplay, player-driven
quit and reopen. Graphics-specific VU0 work remains outside these
counters. World visibility at `0x3582B8` still calls VU0 `vcallms 0x40`,
which requires replacement and separate verification.

Package gates remain open. The exact published binary and runtime
libraries must exclude VU1 and GS in the build graph and link map and
match the approved local Mac visuals, settings and content. Actual
fullscreen 6× presentation at 60 FPS requires target-hardware runs.
The independent unpaced S5 workload must also meet 3.33 ms.

## R7 native world visibility (2026-11-21)

The native MlistVisibilityCalc at `0x3582B8` now performs camera setup,
sphere and model clipping, model depth, and group LOD on the CPU. The
retail visibility upload, both VU0 entry `0x40` calls and the group's
direct COP2 macros have been removed from the regular path.

On the Apple M2 Pro, the full-RAM `EOD_DIFF` rotation matched the
legacy EE reference on all 2,949 live calls. It compares the guest
visibility bytes, matrix table, depth words and camera state. Only the
documented cull-private VF registers are excluded.

| Scene | Look | Detail | Checked `0x3582B8` calls | Mismatches |
|---|---|---|---:|---:|
| Dundas day, Bus, 12 AI | Modern | Maximum | 1,934 | 0 |
| Gilroy night, Taxi, 12 AI | Classic | Original | 362 | 0 |
| Dundas, two joined players | Classic | Original | 365 | 0 |
| Career, HomeBase and dealer | Modern | Maximum | 288 | 0 |

A separate Dundas `EOD_DIFF=00214B68` run matched 82 enclosing world-frame
calls. The direct oracle smoke also counted 115,335 launches of the
retail visibility image, confirming the audit detects the reference path.

Offline replay matched the two six-bit CLIP groups and VF31 depth bits for
2,097 Dundas records and 778 Career records against the compiled VU
reference, and again against the interpreter. Each replay had zero
mismatches and zero unsupported images.

A separate native-only rotation at scale 1 recorded zero retail Mlist
VU0 dispatches on all four scenes. The split race logged two joined
players and both 640×224 viewports. The full R7 audit still rejected the
runs because compatibility VU1, GIF and GS-suppressed work was present.
This check covers cull/depth parity. It leaves player visual sign-off,
release link exclusions, install/upgrade and the two performance gates
open.

CMake regeneration initially included six tracked numbered source copies.
Stale GS and Vulkan copies failed to compile against the current renderer
interfaces. CMake now excludes those exact paths from its source lists and
the normal macOS build passes. The build retains the existing duplicate
`-lm` linker warning. All 99 macOS CTest tests passed. The Windows
cross-build linked `eod.exe` and built the CPU boundary test.

## R7 bootstrap/loading icon readbacks (2026-11-21)

The replacement for 0x35BE08 matched all 1,117 checked live calls in
EOD_DIFF. The comparison covers the generated compatibility packet and
the return value. A separate 6× Modern smoke checked 308 calls with
zero mismatches.

With movie skip and ordinary Plume, 160-vblank Dundas Battle runs across
Classic and Modern at 1× and 6× each removed 138 synchronous readbacks.
The reference used EOD_NATIVE_BOOT_QUAD=0. The native runs reported zero
synchronous reads, 13–15 replacement icon draws, and zero suppressed GS
draws. The optional VU1-worker case used 13 native draws with zero
synchronous reads or suppressed draws. Other legacy work remained,
including deferred waits in the Classic race runs.

Frame comparisons used visible loading-icon images. At 1× Classic,
13 same-vblank pairs differed in 1–5 icon pixels per frame, with a
maximum difference of one RGB code value. At 6× Modern, two matched
rotation poses captured at 640×448 differed in three and five icon
pixels, also by at most one code value. These are headless comparison
runs. The separately paced boot transitions differed in vblank timing.

Ordinary startup played logos.pss in both arms of a 230-vblank A/B run.
The reference recorded 279 synchronous readbacks and Plume recorded 210.
A longer trace attributed all 210 remaining reads to target acquisition.
A separate explicit scene capture produced both files and retained the
reference draw and its 138 reads. The macOS build passed 99 of 99 CTest
tests. The Windows cross-build linked eod.exe.

The change still uses the compatibility transfer and GS state bridge.
Movie handoff, the full R7 release build, player visual parity, and
performance remain open.

## R7 split-screen shadow target experiment (2026-11-21)

Apple M2 Pro, Kenyon Battle (8 AI), two players, headless normal Plume.
At 1× Classic, the default run still recorded 90 deferred waits. The
independent opt-in `EOD_NATIVE_SPLIT_SHADOW_PASS=1` drew 358 native
prepare/composite pairs for each of the two views and recorded zero
synchronous requests, deferred waits, and suppressed GS draws. At
6× Modern, forcing both sun cascades and the full-screen shadow selector
off also left all three counts at zero with 356 pairs per view. The
default retains the original split path.

Exact same-dump software GS replay and a composite-off replay establish
shadowed regions in both views. At the stationary 1× pair ending at
vblank 187, the live candidate's mask MAE (RGB code values) was 1.89
versus GS shadows and 25.85 versus no shadows in the upper view, and
4.62 versus 18.74 in the lower view. The original path at comparable
stationary poses gave 1.98 versus 25.71 and 4.63 versus 18.71. At 6×,
the candidate's 640×448 scanout at the same checkpoint gave 6.23 versus
27.16 and 7.98 versus 19.46 respectively. Forcing only the native
composite off inverted those comparisons in each view. In a moving
lower-view control the off image measured 20.76 against GS shadows and
1.44 against the no-shadow replay, demonstrating that the native
composite accounts for the shadows in that view.

Two moving 1× lower-view candidate masks remained closer to no-shadow
than shadow replay (14.45 versus 14.00 at vblank 362, 13.98 versus 13.22
at vblank 402), and a 6× moving lower-view mask had a similar result.
Nearby original-path frames cover different poses and raster masks,
leaving parity and regression status unresolved in those windows. A
±5-pixel shift search did not explain the two 1× residuals. The captures and run logs support visible shadows in both
views and cleaner memory ownership in this experiment. The moving
lower-view parity, default retirement and cross-platform runtime gates
remain open. A repeat using the EE camera-position watch found equivalent
stationary positions and close moving positions sampled at different
vblanks. At 362 and 402 the repeat candidate's moving lower-view MAE
favored shadows (1.32 versus 18.84, and 1.31 versus 18.10), but the
default arm's same-vblank replay windows had no shadow-affected pixels.
Those repeats do not isolate the cause of the outliers.

The 1,650-vblank differential run checked 1,044 calls each to ShadowBegin
(0x34C7D0) and ShadowEnd (0x34C798) without a mismatch. Ordinary 1P
Dundas day retained its native full-screen sun path. Gilroy night
retained its original full-screen route. macOS CTest passed 99 of 99.
The MinGW cross-build linked `eod.exe`. Windows runtime testing remains open.

## R7 integration with Remaster 1.1 source (2026-11-21)

The upstream launcher defaults, package checks, and removal of tracked
captured VU tables are integrated. The renderer conflicts were against
files that already matched upstream before the R7 work. The merge keeps
the R7 changes on those files and keeps split-screen shadow ownership
opt-in. The upstream cleanup also removes the stale editor copies from
the normal CMake source graph.

A fresh macOS Plume build with `EOD_PUBLIC_VU_INTERPRETER=ON` and the
matching MinGW build both linked. Neither VU object depended on
`vu_rec.inc`. The private Mac comparison build still depended on it.
The public Mac executable had the VU interpreter symbol and no captured
dispatch table symbols. The known 0x24D8 no-op still retires against the
live instruction pairs. In the public build the exact-state interpreter
comparison passed and the trace reported 577 and 613 retirements at
vblanks 123 and 241. Disabling retirement removed both entries.

In headless EOD_DIFF smoke runs, the local and public Mac builds matched
383 and 387 loading-quad calls, and 290 and 82 visibility calls. All four
checks reported zero mismatches. Ordinary 6× Dundas Modern loads and
races in both builds recorded zero synchronous requests, deferred
waits, CLUT waits, suppressed draws and visibility VU0 executions.
The opt-in 1× Classic Kenyon split-screen smoke also recorded zeros for
those fields. The lifecycle gate correctly rejected both 6× Dundas
logs because VU1 dispatches, GIF packets and GS raster work remained.
The counters span each run, and the two binaries progressed through
different numbers of rendered frames. The local Mac build passed all 99
CTest cases. These are functional checks.
Package identity, player visual parity, sustained presentation, the
3.33 ms budget, and Windows runtime verification remain open.

## R7 public non-clipped world dispatch (2026-11-21)

The guarded `0x0270` world-leaf replacement runs by default in ordinary
Plume, including public builds with no captured VU table. An archived
capture exercised 1,128 admitted leaves. Public interpreter, compiled
local reference, and forced interpreter in the local build each matched
all 1,128. Each comparison covered both GIF kicks, the final registers
and flags, all VU data memory and the live scoreboard slots. It failed
all 1,128 cases before the interpreter flag reconstruction was added,
even though both kicks already matched. Changing one byte of each
recorded VU1 image rejected all 1,128 entries. The public live world-pass
`EOD_DIFF=00358BD0` ran 835 checks with zero mismatches.

Ordinary public runs with live checking accepted the guarded path in
1× Modern Dundas day, 1× Classic Gilroy night and both views in 1× Classic
Kenyon. These had no VU differential failures, synchronous reads or
suppressed draws. A/B deferred-wait counts matched in Gilroy (1 each)
and Kenyon (6 each). In the 6× Modern Dundas case, the reference at
vblank 242 dispatched `0x0270` 1,237 times. With the shipping default
at the same sampled vblank, it dispatched 204 times and retired
1,033 leaves on the host. The 360-vblank reference audit reported
212,973 VU1 dispatches. The default candidate reported 173,686,
34,409,721 compatibility GIF qwords and 88 GS raster submissions.
Both had zero synchronous reads, deferred waits, CLUT waits and
suppressed draws. The independently paced runs progressed through
different numbers of gameplay frames.

The separately paced 640×448 Modern scanout comparison found a closest
A/B pair at 0.755 mean RGB code values of difference. Two independent
runs with the reference path enabled had a closest pair at 1.023.
This visual check is supplementary to the same-slot kick differential.
The reference and replacement both still need the VU scratch state
and GS bridge for this stage. S4 and build-level elimination remain open.

## R7 typed world surface and texture LOD transport (2026-11-21)

The world REF now carries its color and depth targets, scissor, and
screen origin from EE state. Compare that snapshot with the first
primitive using `EOD_NATIVE_WORLD_SURFACE_CHECK=1`. The surface proof
reported zero mismatches and zero unobserved objects in the following
audited ordinary public runs.

| Scene and mode | Observed typed surfaces | TEX1 updates sent as typed commands | PATH1 fallbacks |
| --- | ---: | ---: | ---: |
| Dundas 6× Modern | 51,641 | 46,361 | 0 |
| Dundas 1× Modern | 54,115 | 48,580 | 0 |
| Gilroy night 1× Classic | 46,255 | 42,471 | 0 |
| Kenyon 2P 1× Classic | 56,257 upper plus 57,960 lower | 106,023 | 0 |

The admitted TEX1 operation overwrites the same compatibility register
and preserves PATH1's final tag state. It bypasses two GIF qwords per
typed update. In bounded 6× Dundas, `EOD_NATIVE_WORLD_LOD=0` kept the
ordinary TEX1 kick and counted 31,718,217 GIF qwords. Default counted
31,654,029, with 92,722 qwords skipped specifically by the matched
transport. These separate runs reached different render-frame counts,
so the 64,188 total difference combines the transport change with the
workload difference. Sampled world object parity showed 44,597 of
44,597 leaves drawn on default and 45,777 of 45,777 on opt-out, with
zero losses in both. The state comparison showed zero K/filter or
other world-state mismatches. The 6× pair reported zero synchronous
reads, deferred/CLUT waits, and suppressed draws.

Night and split retained deferred waits. Gilroy was 2 on default and
1 on opt-out in both the initial and traced pairs. The traces identify
the same upload merge sources, including pages 360 and 382. Kenyon
recorded 6 and 7 waits, respectively. All four runs still reported
zero synchronous reads and suppression. A non-threaded GS added
33,765 matched typed updates and a portable capture issued no typed
updates and replayed in the software reference. The Dundas producer
comparison checked 143 live `0x358BD0` calls with zero mismatches.
Separately paced 640×448 screenshots gave closest-pair RGB MAE
1.189/255 for opt-out versus typed and 0.722/255 for opt-out versus
repeat opt-out. This image sample does not establish a pixel-exact
same-pose comparison.

The local compiled-VU control matched 322,505 observed draw surfaces
and used 289,548 typed TEX1 updates with zero PATH1 fallbacks. The public
and local Mac builds linked, all 99 macOS CTests passed, and the Windows
public executable cross-linked. Windows runtime remains unverified.

A separate public headless performance comparison kept this typed
transport disabled in both arms and toggled only the already-committed
world VU1 retirement. Four serial 810-vblank Dundas 6× Modern runs
were run off, on, on, off. The 5–10 s game-draw samples were 12.9
and 13.6 FPS off and 14.0 and 14.3 FPS on (13.25 → 14.15, +6.8%).
Average whole-process CPU time was 13.525 → 13.620 s and wall time
was 13.635 → 13.630 s. Mean native GPU time was 7.959 → 8.197 ms,
within the spread of repeat off runs. These figures describe paced
headless rendering only. The default public binary still executes
VU1 and the GS bridge. The typed TEX1 work removes a packet rather
than the renderer family. In particular the strip, texture/CLUT
contents and remaining compatibility state still depend on GS.
## R7 world texture binding from EE assets (2026-11-21)

The public Plume build uses an immutable EE binding for transient
128×128 PSMT4 world textures on verified non-clipped `0x0270` draws.
The draw must match the reference's PSM, dimensions, texture function,
TCC, active mip count, each mip base and pitch, and all 16 palette
entries. This copies the active palette during the EE world record and
uses it for the renderer's cache identity, material and host-texture
lookup, and pixel expansion. The mip addresses likewise come from the
EE binding when it passes the comparison. `EOD_NATIVE_WORLD_TEXTURE=0`
selects the reference and `EOD_NATIVE_WORLD_TEXTURE_CHECK=1` reports
comparisons and distinct palette hashes by view and bank.

| Public headless case | Matched | Mismatched | Native bindings selected | Sync/deferred/CLUT waits | GS suppressed |
| --- | ---: | ---: | ---: | --- | ---: |
| Dundas Modern at 6× | 24,656 | 0 | 24,656 | 0/0/0 | 0 |
| Dundas Modern at 6× binding disabled | 22,493 | 0 | 0 | 0/0/0 | 0 |
| Gilroy night Classic at 1× | 26,202 | 0 | 26,202 | 0/2/0 | 0 |
| Gilroy night Classic at 1× binding disabled | 34,650 | 0 | 0 | 0/1/0 | 0 |
| Kenyon Classic at 1×, two players | 69,897 | 0 | 69,897 | 0/7/0 | 0 |
| Kenyon Classic at 1×, two players, binding disabled | 70,916 | 0 | 0 | 0/6/0 | 0 |

Dundas observed 78 distinct palette hashes in each of the reused
`0x2BC4` and `0x2FC8` banks. Kenyon independently saw 50 and 49 in the
upper view and 46 and 46 in the lower view. Reported P1 samples drew all
submitted world leaves. Two separately paced Dundas Classic 1× scanout
windows had a closest nearby comparison of RGB MAE 0.230/255. The
scanouts were independently timed and did not match exactly.

A longer public Dundas differential checked 1,150 calls to
`0x358BD0` with zero mismatches and matched 124,069 bindings. The
public and local Mac builds linked, 99 macOS CTests passed, and the
Windows public cross-build linked.

The compatibility strip still supplies the draw reference. GS upload
storage still supplies the indexed mip texels. This is a binding change,
and the whole-run VU1, GIF and GS counters still fail R7. Both arms of
the separately timed night and split runs recorded legacy deferred
waits, and the binding adds no wait operation.

## R7 world indexed mip bytes from EE assets (2026-11-21)

The admitted non-clipped `0x0270` draws can decode their transient
128×128 PSMT4 mips from an immutable snapshot of the prepared EE texture.
The native texture key uses the bytes through the selected mip level and
the EE palette. The GS still provides the draw descriptor and the existing
base and mip availability decisions. Unsupported sources retain the
compatibility path.

`EOD_NATIVE_WORLD_INDICES_CHECK=1` compares the entire chain once for each
observable version, destination bank and view. Checks with an unavailable
reference mip count as unobserved attempts and may be retried. They do not
contribute to the texel count. With that check enabled,
`EOD_NATIVE_WORLD_INDICES=0` compares without selecting the EE byte
source. `EOD_R7_AUDIT=1` also reports selections with the comparison unset.

| Public macOS headless case | Complete checks | Texels compared | Mismatches | EE source selections | Unobserved attempts |
| --- | ---: | ---: | ---: | ---: | ---: |
| Dundas, Modern 6× | 158 | 3,448,192 | 0 | 31,768 | 0 |
| Dundas, Modern 6×, EE indices disabled | 158 | 3,448,192 | 0 | 0 | 0 |
| Dundas, Modern 6×, comparison unset | 0 | 0 | 0 | 33,920 | 0 |
| Gilroy night, Classic 1× | 54 | 1,178,496 | 0 | 33,274 | 1,714 |
| Gilroy night, Classic 1×, EE indices disabled | 54 | 1,178,496 | 0 | 0 | 1,712 |
| Kenyon head-to-head, Classic 1× | 189 | 4,124,736 | 0 | 67,085 | 2,725 |
| Kenyon head-to-head, Classic 1×, EE indices disabled | 189 | 4,124,736 | 0 | 0 | 2,754 |

The three enabled comparison runs totaled 401 complete checks and
8,751,424 texels, an average of 21,824 per check. That corresponds to
the size of a five-level chain. The comparison records do not establish
individual live coverage for 4×4 or 2×2 mips. Kenyon joined two players
and reported `viewcount3D1A74 2`.
At banks `0x2BC4` and `0x2FC8` it verified 49 and 49 source versions in the
upper view and 45 and 46 in the lower view. All P1 counters printed in these
samples showed submitted world leaves drawn. The Dundas differential
checked 1,176 calls to `0x358BD0` with zero mismatches. The public and
local Mac builds linked, all 99 macOS CTests passed, and the Windows
public executable cross-linked. No authorized Windows game install path
was available for replacement with backup or runtime testing.

An independent rerun matched 8,707,776 texels over 399 complete checks
in the same enabled scene set, and all 1,208 Dundas differential calls
matched. The ordinary 6× run selected EE indices 32,971 times. An
isolated, ad-hoc signed macOS bundle passed strict signature
verification and ran the 6× Dundas Modern headless smoke with Plume
required, selecting EE indices 32,516 times. The bundle still failed
the R7 runtime gate.

Independently paced 640×448 Classic scanouts included two disabled runs
per fixture. The median RGB MAE (out of 255) at the same capture vblank
was 1.749 enabled/disabled and 2.468 disabled/disabled for Dundas, 0.036
and 0.012 for Gilroy, and 3.121 and 3.498 for Kenyon. The closest Kenyon
same-vblank pair had MAE 0.084 in the upper view and 0.148 in the lower
view. The separately paced runs do not establish equal simulation poses
at a given capture vblank or pixel-exact parity.

The ordinary 6× run retained 171,371 VU1 starts, 33,833,479 GIF qwords and
94 GS raster submissions. Gilroy and Kenyon retained one and six deferred
waits in their checked runs. These runs recorded zero synchronous VRAM
readbacks, CLUT waits and suppressed GS primitives. The R7 runtime gate still
fails. The world strip, transient upload banks and availability of GS mip
storage remain dependencies of this slice.

## R7 non-clipped world strip terminal oracle (2026-11-21)

The oracle compares the exact retired `0x0270` strip's terminal payload
against the GS worker's next ordered PATH1 parse. It covers the packet
cursor, PRIM, ST/Q/RGBAQ, inherited UV/FOG/TEX0/TEX1, attribute taint
and written masks, the three FIFO vertices, and triangle attempts after
ADC. These attempts precede draw filtering and `note_draw`. The parser
still processes the same packet and performs the same draw effects.

| Public macOS Plume headless case | Strips checked | Triangle attempts | ADC vertices | GIF qwords inspected and retained |
| --- | ---: | ---: | ---: | ---: |
| Dundas, Modern 6× (Phase A) | 48,408 | 511,557 | 472,687 | 3,001,140 |
| Dundas day, Classic 1× | 23,196 | 242,696 | 222,661 | 1,419,267 |
| Gilroy night, Classic 1× | 33,112 | 343,897 | 274,027 | 1,886,884 |
| Kenyon 2P, Classic 1× | 118,017 | 879,698 | 622,373 | 4,624,230 |
| Dundas, Modern 1×, world differential | 223,671 | 2,326,161 | 2,151,803 | 13,657,563 |

All 446,404 checked strips matched across these runs. There were zero
partial PATH1 packets, zero strips without triangle attempts, and zero
discarded strip qwords. These counts cover the exact retired microcode
case, including strips outside the EE indexed texture class. Kenyon
reported two live views and matched 58,209 upper-view and 59,808
lower-view EE surfaces with zero mismatches or unobserved surfaces.
The checked and unset Classic runs reported zero lost world leaves in
the sampled frames and zero suppressed GS primitives. Capture at the
same vblank in independently paced runs did not align rendered frames
or audit totals.

For an exact unset control, the same four-vblank recorded Dundas GS
stream replayed on the preceding build and the new build. Both software
replays recorded 69,349 primitive attempts, 2,352,271 pixels and one
game frame. The final scanout and GS VRAM matched byte for byte. This
replay covers the parser and its draw effects on a fixed stream. Live
Plume evidence comes from the ordered comparisons above.

The public Release interpreter-enabled executable SHA-256 was
`8e839f2502e2d602ea598d703dd2cca141506c4566fcc2e0669b1817d40cfc4f`.
The Dundas `EOD_DIFF=00358BD0` run matched all 1,168 live calls.
The public and local Mac builds linked, all 99 macOS CTests passed,
and the Windows executable cross-linked. Windows installation with
backup and runtime testing were unavailable.

The strict R7 audit remains red. The 6× sample retained 151,738 VU1
executions, 29,966,467 GIF qwords and 91 GS raster submissions. The
Classic checked runs recorded one deferred wait each in Dundas and
Gilroy and six in Kenyon, with zero synchronous VRAM reads, CLUT
waits and suppressed draws. No fullscreen or performance result
was measured for this prerequisite.

The worker still needs an independent admission decision from the
ordered EE target, scissor, material, immutable mip/palette snapshot,
LOD and mesh. Mip availability, trimming around live attachment
aliases and allocation failures are unresolved. The first native
triangle still takes its draw descriptor through GS. Target dirty-page
ownership and propagation through `note_draw` and `raster_mark_pages`
remain live compatibility dependencies. Verify these together before
dropping the first strip packet.

## R7 ordered world-strip omission (2026-11-21)

On Apple M2 Pro, the public Release executable with the interpreter
enabled recorded the following native draws at 6× headless. Mode 1
omits a worker-side PATH1 parse after admission. Mode 2 runs the
original parser once and checks its first-draw environment generation,
XYOFFSET, native triangle ownership and dirty flag. It then verifies
that normal finalization and recording use the prepared texture,
pipeline and sampler for a single indexed draw.

| Case | Omitted and recorded | GIF qwords that bypassed the parser | Original-path reference matches | Option-unset strips terminal-checked |
| --- | ---: | ---: | ---: | ---: |
| Classic Dundas day | 16,846 | 811,147 | 16,600 | 56,716 |
| Classic Gilroy night | 23,167 | 1,076,551 | 22,968 | 60,240 |
| Classic Kenyon 2P | 50,020 (24,451 upper, 25,569 lower) | 2,111,797 | 47,561 (23,184 upper, 24,377 lower) | 113,929 |
| Modern Dundas day | 16,203 | 776,121 | 15,979 | 48,468 |
| Modern Kenyon 2P | 50,141 (24,520 upper, 25,621 lower) | 2,114,282 | 50,057 (24,449 upper, 25,608 lower) | 152,234 |
| Total | 156,377 | 6,889,898 | 153,165 | 431,587 |

The omitted runs forwarded all 326,886 declined strips, with
15,706,620 qwords verified by the terminal oracle. The reference
admissions matched 1,226,429 triangle attempts. The omission runs
compared 16,193,408 indexed texels over 742 complete version, bank
and view checks with zero mismatches. With EE indices
disabled in Classic Kenyon, the index gate declined 49,178 candidates
and all 152,071 strips reached the original parser. There were zero
omissions. All sampled P1 world leaves were drawn in the matrix.
With the texture, index, surface and strip terminal checks unset,
Modern Kenyon separately recorded 24,880 omitted and indexed draws in
the upper view and 25,996 in the lower view.

Two 1× Classic Dundas repeats used the same controller input and
synchronous GS execution. Each compared 35 scanouts at vblanks
152–198. In the final repeat, the median 8-bit RGB MAE was 0.306 for
omission/reference, 0.474 for omission/unset and 0.427 for
reference/unset. Completed frame counts varied, and no scanouts were
byte-identical in either repeat. The per-draw
ordered reference proves the local draw-state checks. Exact live
scanout parity remains open.

The Modern Dundas omission run still recorded 169,952 VU1
executions, 32,818,103 parsed GIF qwords and 91 GS raster submissions.
The 6× omission matrix reported 0–13 deferred waits per run and zero
synchronous VRAM reads, CLUT waits, suppressed GS draws or VU0
matrix-list executions. The strict R7 audit rejected every run. GIF
construction, XGKICK extraction and queue copying still occur.
The native draw still depends on GS mip availability, attachment
ownership and the terminal compatibility bridge.

The mode stays opt-in. A preflight rejection leaves the original
packet available to the GS worker. Unexpected loss of prepared GPU
resources after admission aborts the run because its draw no longer
has an ordered recovery path. Pixel-exact matched-input presentation
and recovery under late attachment failure remain open for default
enablement.

The Modern Dundas differential matched all 1,178 calls to
`0x358BD0`. The local macOS build passed all 99 CTests, the public
macOS executable linked and ran the matrix, and the Windows
executable cross-linked. Windows runtime and installation with
backup were unavailable. No release or fullscreen performance
measurement was made.

## R7 bounded world-strip producer transport (2026-11-21)

`EOD_NATIVE_WORLD_STRIP_DIRECT=1` bypasses the producer's generic XGKICK
tag walk for the exact retired opaque `0x0270/0x80` leaf. The validated
PRE/EOP PACKED tag fixes the non-wrapping range to `1 + 3 * NLOOP`
qwords in either TOP bank. The retirement oracle compared both kick
payloads, final VU state and data memory, and the live scoreboard
against the same-state interpreter for each retired leaf in these
headless public runs. TEX1 and the terminal check retain their place
before the original strip bytes in the PATH1 order.

| 6× headless case | Mode 1 direct producer packets | Mode 1 direct producer qwords | Mode 1 native recordings | Mode 2 direct packets parsed |
| --- | ---: | ---: | ---: | ---: |
| Classic Dundas day | 22,716 | 1,196,967 | 11,794 | 23,380 |
| Classic Gilroy night | 24,799 | 1,215,337 | 15,423 | 30,081 |
| Classic Kenyon 2P | 60,522 | 2,943,294 | 36,433 (17,733 upper, 18,700 lower) | 68,432 |
| Modern Kenyon 2P | 51,013 | 2,517,385 | 30,819 (14,778 upper, 16,041 lower) | 68,463 |
| Modern Dundas day | 22,023 | 1,159,431 | 11,550 | 28,052 |
| Total | 181,073 | 9,032,414 | 106,019 | 218,408 |

Mode 1 transported 90,981 packets at TOP `0x0E0` and 90,092 at
`0x270`. All 206,910 worker declines matched the original parser's
terminal state. At least 75,054 directly transported packets reached
that fallback. Mode 2 called the original parser on all strips,
including 10,851,380 directly transported qwords. It verified the
terminal state of 378,697 strips and recorded all 126,026 matched
admissions. Five mode-2 controls with direct transport unset checked
401,947 terminals and recorded 131,576 admissions with no direct
packets. The 6× Modern Dundas control's periodic P1 lines ended before
race leaves appeared, though the recording counter covered race draws.
All worker-path checks reported zero partial PATH1 packets.

With the GS worker disabled, Classic Dundas and Classic Kenyon sent
85,089 direct packets, totaling 4,199,970 qwords. All 152,520 terminals
matched and 50,821 admitted draws recorded, including 20,037 in the upper
and 21,185 in the lower Kenyon view. The portable Dundas GS capture
produced 14,061,460 bytes with the direct option enabled and zero direct
packets. Its software replay matched the archived reference's 69,349
primitive attempts, 2,352,271 pixels, final scanout and VRAM. This
diagnostic capture incurred 411 synchronous readbacks.

The initial mode-1 6× Modern Kenyon run reported 114,393 of 114,546
world leaves drawn in its cumulative P1 samples, with one parity
frame short. The same interval reported deficits for queue records,
shadows, sprites and immediate draws. All 30,819 admitted strips
recorded and all 63,770 declined terminals matched. Two alternated
mode-1 repeats with parity logging recorded all 171,539 and 114,639
sampled world leaves with direct transport on, and all 139,912 and
171,758 with it unset. The cause of the initial parity discrepancy
remains open. These separately paced runs leave full P1 and
pixel-exact live scanout parity open.

At 1× Modern Dundas, all 1,030 live EE differential calls to
`0x00358BD0` matched. The interval transported 135,138 direct
packets and the original parser matched their terminal states as part
of 204,698 checks. The Apple M2 Pro public executable SHA-256 was
`c89cf0a538221f20984ba463f4a3748b8237e06629b0aef774698b7e14283b4a`.
The local macOS build passed all 99 CTests. Windows cross-linked to
SHA-256 `6dcd5e64acda5883066f50ab4e60668ab9614b977d77c3c6dab97bb879d05495`.
Windows runtime and installation with backup were unavailable.

The strict runtime audit rejected the 6× mode-1 matrix. Modern Dundas
reported 105,473 VU1 starts, 20,470,962 parsed GIF qwords and 87 GS
raster submissions. The five runs retained zero to seven deferred
waits each, with zero suppressed GS primitives, synchronous VRAM
readbacks, CLUT waits or VU0 matrix-list starts. Strip construction,
VU writes and PATH1 transport remain live. The option stays opt-in.
The headless runs provide no fullscreen pacing or performance claim.

| Executable | SHA-256 |
| --- | --- |
| Public macOS | `ac846f7e6641a45f788c298430c5f5b41633cace8d3d0c65cb18a0aa2350f301` |
| Local macOS | `0cc30716599dab9f08784adbdf6ccec1fab684f370b394803fea5462a352151d` |
| Windows cross-link | `22992d4a75f0389e4ce2f7a7f455f376f0a716b9424a34978b80b77b5b4e50c1` |

## R7 whole-run VU census and audit v2 (2026-11-21)

The release audit now counts VU0 micro dispatch, COP2 macros and COP2
control accesses, alongside VU1 and the existing GS counters. An opt-in
census records actual dispatches by launch route, byte PC, micro-memory
fingerprint and context. It keeps the exact native VIF retirements
separate. VU0 `vu0_mlist` remains a subset. The parser rejects old schemas,
missing and duplicate reports, census overflow, and census records after
shutdown. All 17 successful Phase B census runs reconciled with zero
dropped rows. Every one still failed the strict emulator-work gate.

The public interpreter Vulkan/Plume executable on Apple M2 Pro had
SHA-256 `8cdd91db27c589c512f6ebe0620d18d8dfbc77165d7ed2293befef81d2507ae7`.
These are whole-process counts from public headless runs with movie
skip, heap checking, default native admission and the stated vblank
windows. The two Kenyon runs each showed two stacked 640×224 viewports.
Their counts combine both views.

| Scene | Vblanks | VU1 MSCAL / MSCNT | World / NOP retired | VU0 micro | VU0 macro | COP2 reads / writes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Modern Dundas day, 6× | 360 | 44,060 / 86,850 | 42,268 / 25,164 | 2,420 | 315,541 | 17 / 18,273 |
| Classic Dundas day, 6× | 360 | 47,233 / 93,015 | 45,416 / 26,988 | 2,447 | 320,101 | 17 / 18,487 |
| Classic Gilroy night, 6× | 475 | 90,151 / 131,882 | 47,472 / 40,853 | 3,610 | 493,849 | 17 / 27,851 |
| Classic Kenyon 2P, 6× | 485 | 83,872 / 113,273 | 130,517 / 35,695 | 2,833 | 382,453 | 17 / 21,461 |
| Modern Kenyon 2P, 6× | 485 | 84,469 / 116,492 | 127,326 / 36,198 | 2,920 | 393,643 | 17 / 22,159 |
| Classic main menu, 1× | 360 | 21,747 / 0 | 0 / 9,679 | 0 | 25,057 | 17 / 1 |
| Classic Career Home, 1× | 1,800 | 912,753 / 161,553 | 46,257 / 444,820 | 22,737 | 2,482,156 | 17 / 139,625 |
| Classic Career garage zone, 1× | 2,200 | 299,056 / 206,810 | 169,963 / 141,848 | 26,135 | 2,350,735 | 17 / 160,325 |
| Classic Career dealer zone, 1× | 2,200 | 218,496 / 315,031 | 166,863 / 101,741 | 10,227 | 980,215 | 17 / 64,877 |
| Modern Dundas differential, 6× | 2,200 | 204,229 / 353,664 | 209,026 / 120,094 | 10,840 | 1,521,541 | 17 / 85,633 |

The Career fixtures used separate copies of a saved Career. Two early
warps stopped at a pending zone. Delaying the warp to 240 Home Driver
updates let the game settle into the garage and dealer zones. Garage
and dealer menu actions remain unverified. Across the five 6× race runs,
the audit measured 25,919,907–43,633,827 parsed GIF qwords, 92–4,186 GS
raster submissions and 0–14 deferred waits per run. Career Home alone
had 183,405,394 parsed qwords, 68,620 GS raster submissions and 249
deferred waits. All completed runs had zero VU0 matrix-list dispatches,
strict suppressions, synchronous VRAM reads and CLUT waits.

The sampled VIF1 reserve-site trace attributed all 42,270 sampled starts
across the five 6× races. At `0x003613E0`, the Modern Dundas sample attributed
2,739 starts of `0x0D48` and 2,133 of `0x24B0` to queue batches. The whole
Modern Dundas run counted 35,606 and 27,729 respective continuations,
both at ITOP `0x080` on image `fd0ce30ab0ec263c`. Together they supplied
25.2–48.4% of actual VU1 starts in the five race runs. The Modern sample
also attributed clipped/world starts to `0x0035DEFC`, shared `0x0270`
starts to immediate reserve `0x003614CC`, and `0x2538` / `0x25F0` starts to
texture and state reserves `0x0035F824` / `0x00362104`. These reserve
samples do not identify the exact model or caller mix across the full
run. The menu's whole-run histogram independently logged 13,456 starts
of `0x0270` at ITOP `0x200`, 4,142 of `0x2538` at `0x200`, and 346 of
`0x25F0` at `0x200`.

Every VU0 dispatch in the displayed matrix was VCALLMS to PC 0 on image
`dc7d103a5a6e1a99`. All used the retained EE return registers
`0x001DE864` and `0x001DEBA4` inside `0x001DE7B0`. Those sites accounted
for 91.5–94.2% of VU0 macros across the five race runs and 1,928,151 in
Career Home. The census observed no direct, VIF0, MSCALF, VCALLMSR or
CMSAR1 launches in these fixtures. Generated code still contains 475
other macro sites, seven VCALLMS sites, 30 CTC2, 12 CFC2 and 19 no-op
sync helper calls. The 164 direct VF loads, 120 stores and 29 register
copies, and the mapped VU memory routes, require separate source and
build/link verification.

The public differential matched all 1,041 live `0x00358BD0` calls
with zero replayed. The earlier Phase A differential matched 1,179.
Two worker tests with sender tracing unset reconciled after drain and
join, including 168,640 VU1 dispatches in Modern Dundas. An earlier
worker run with sender tracing active hit a drawing DMA timeout and
emitted no shutdown audit. The disabled control exited normally and
emitted neither diagnostic. With both settings unset, the runtime
skips hashing, row updates and atomic increments at the audited sites.
Disabled-mode timing overhead was not measured.

The local macOS build linked with reference VU tables and passed 99/99
CTests. Its own Dundas smoke reconciled 849,872 VU1 and 2,650 VU0 dispatches.
The Windows executable cross-linked with SHA-256
`5c202c8e261e9d8aa0ab69c401ec2dc50cdd180ccf18cadf28d08e4d4114b24c`.
Windows installation and runtime were unavailable. Movie play, manual
Career save/load and menu actions, 4P, netplay, full lifecycle parity,
packaged VU/GS absence and fullscreen performance remain open.

The next replacement is the gameplay producer `0x001DE7B0`, called at
`0x00187E08`. Recover the DMA0 image staged at `0x003BC590` and cross-check
the observed PC 0 fingerprint and the local reference's `0x0000` program
range. Trace the EE prelude, VF/VI and VU memory inputs, and the outputs and
consumers before replacing the upload, macros and VCALLMS together. Use
same-state comparisons and verify that the replaced producer's launches
and support operations disappear for the same workload. The largest
mapped graphics follow-on is the exact `0x35CB88 → 0x3613C0` queue class
with `0x02E0 → MSCNT 0x0D48`. Prove TOP ownership, posed/dented geometry
and terminal effects before dropping its packet. Treat `0x24B0` and
the other queue variants as separate admissions.
