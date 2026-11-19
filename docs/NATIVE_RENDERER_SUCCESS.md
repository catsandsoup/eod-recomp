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
