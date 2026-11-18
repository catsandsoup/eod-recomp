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
| P8 | Split screen looks like single player | 2-player frame vs 1-player frame of the same track and camera position: same track passes (water, reflections, detail), same LOD and draw distance, no extra culling, every look and renderer | identical object and pass lists per view (render trace); water visible on Dundas in both views |
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
