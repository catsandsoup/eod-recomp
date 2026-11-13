# Scene capture: the game's 3D frame for a modern renderer

## Player success

A player turns on the modern renderer and sees the same race (same track pieces, cars,
props and sky in the same places, same camera) lit with sun shadows, a physical sky and
modern materials. The HUD, menus and 2D overlays look exactly as today. Original mode stays
pixel-identical. This document establishes that the game's real 3D scene can be taken from
the running game each frame and records the data a renderer such as Filament needs.

Status: the capture spike works in the real game (Afton, No-Rules Race, Venom, headless,
muted). One race frame was captured, rebuilt in world space with its real textures, and
re-projected onto the game's own screenshot of that frame. No modern renderer exists yet.

Confidence labels: **V** verified against the captured frame (numbers below), **D** derived
from reading the micro program or from data consistency, **H** hypothesis.

## Method

`EOD_SCENE_CAPTURE` (below) records, for a few vblanks: the entry state of every VU1 micro
program run (pc, VIF TOP/ITOP, all 32 VF and 16 VI registers, the 16 KB data memory), every
GIF packet on every path in order (PATH1 = XGKICK), and on the GS thread the texture each
draw used with its host-texture identity keys. `tools/scene_capture.py` decodes it.

Each claim below was checked by computing, from the recorded inputs alone, what the micro
program should have kicked and comparing it with the packet it actually kicked.

## 1. Which micro program draws what

MSCAL immediates are in units of 8 bytes: MSCAL 0x4E starts at byte 0x270. Every program
entry is a 7-instruction stub that picks one of two bodies by ITOP: ITOP < 0x200 runs one
body, ITOP >= 0x200 the other (D, `0x270`-`0x2a0`). So the same entry can draw sky, world or
HUD; the pair (entry, ITOP) is the real program identity.

Frame captured: vblank 996, 2,776 XGKICKs, 1,309 VU1 runs that kick.

| Entry (MSCAL) | ITOP | Draws | Evidence | Conf. |
|---|---|---|---|---|
| 0x2538 | any | A+D state packet from TOP (TEX0, MIPTBP, CLAMP, TEST, ZBUF, FRAME, ALPHA, FOGCOL, SCISSOR) | kicks contain only A+D | V |
| 0x24d8, 0x25f0, 0x0000 | any | uploads/setup, no kick | 0 kicks in the frame | V |
| 0x270 (0x4E) | 0x280 | sky dome and horizon cards (`docs/SKY_EXPORT.md` "MSCAL 78" = decimal 78 = 0x4E) | model matrix = camera translation, identity rotation; textures sky09a..sky14a | V |
| 0x618 (0xC3) | 0x280 | terrain, road layers; one sky horizon card | camera matrix, `m-grnd`, `rd-*`, `dirt01` | V |
| 0x270 (0x4E) | 0x80 | distant terrain and track parts | camera matrix | V |
| 0x920 | 0x80 | distant terrain | camera matrix | V |
| 0x2a8 (0x55) | 0x80 | event props (fences, cones, barrels, hay), AI-car wheels | own rigid model matrix per batch | V |
| 0x1f90 | 0x80 | props, AI cars | own model matrix | V |
| 0x2e0 (0x5C), 0xd48, 0x15a8 | 0x80 | player car body: three passes over the same mesh (paint, detail, damage) | car model matrix, `HOBBYSET.RES` textures | V |
| 0x24b0, 0x0318 | 0x80 | driver figure and steering parts | `RACE.RES:Drvr_hlm`, `PART.RES:steer1` | V |
| 0x0020 (MSCAL 4), 0x0338 | 0/1/0x200/0x201 | shadow casters into off-screen buffer FBP 0x19f | FRAME_1 = 0x19f | V |
| 0x01b8, 0x08e0, 0x05e0 | 0x80/0x200 | particles: one input point becomes a sprite or a quad (2x / 4x outputs) | output count multiples, sprite prims | V |
| 0x270 (0x4E) | 0x200 | HUD panels, text, icons | own 2D matrix (scale 365, no perspective) | V |

The world document's MSCAL 0x4E/0x6A/0x55 (Anoka, from the archives) and the car document's
0x5C apply. 0x350 (0x6A, facing sprites) did not run in this Afton frame.

## 2. Where each program reads its data

Batch layout, relative to VIF TOP (V for every geometry program in the table except particles
and shadows, which use other layouts):

| Qword | Content | Conf. |
|---|---|---|
| TOP+0 `.x` | vertex count \| 0x8000 | V |
| TOP+1+3k | normal (offset-mode V4-8, STROW 65536: component = `(byte-128)/127`) or vertex colour (V4-8, 0x80 = 1.0); `.w` = ADC (strip restart) | V (normals D) |
| TOP+2+3k | position (model space) | V |
| TOP+3+3k | s, t | V |
| TOP+198 `.xy` | UV offset added to every s, t | V |
| TOP+199 `.xyz` | position offset added to every position | V |
| TOP+149/150+k | skin: bone a, bone b, weight a, weight b (car body) | D |

Evidence: positions decoded this way and projected through the registers below reproduce the
kicked XYZ to a median of 0.06 px (the GS 12.4 fixed-point step); 84% of the batches whose
kicks map 1:1 to their inputs are under 0.5 px. Decoded s, t equal the kicked S/Q, T/Q to 1e-7 in every geometry program. The
batches over 0.5 px are the skinned driver (about 1 px, see open questions), shadow programs
(another matrix), and vertices the program culled in ITOP mode 0x280.

Registers and constants at entry:

| Where | What | Conf. |
|---|---|---|
| vf4, vf5, vf6, vf7 (columns) | model -> GS screen matrix: `screen = [vf4 vf5 vf6 vf7] (p, 1)`, x/w and y/w in GS pixels (XYOFFSET 1728,1824 for 640x448), z/w = GS depth | V |
| vf8 - vf11 | same transform in clip space, for the guard-band test (`clip vf14`) | D |
| x, y, w rows of vf4-7 | the camera: identical for every sky/terrain batch in the frame | V |
| z row of vf4-7 | per-layer depth range: differs between terrain groups that share a camera | V |
| model matrix | `M = inv(camera_xyw) * batch_xyw` is rigid (R^T R = I) for cars and props; identity for world batches; camera translation for the sky | V |
| VU1 qword 17 `.xyz` | the game's light direction, already rotated into the batch's model space. `R * q17` gives world (-0.116, 0.705, 0.700) for every world and object batch | V |
| VU1 qword 17 `.w` | 0.1858 in every batch (ambient level) | H |
| vf12 `.xyz` | second direction in model space; world (-0.704, 0.707, -0.069) for every batch | V (value), H (meaning: second light or environment vector) |
| vf1, vf2 | (72.28 x3, -1), (69.77 x3, ...): light colour or intensity scales | H |
| VU1 qwords 143+i | car "bones": per-bone translation offsets blended by the skin weights (damage dents; about 0 on an undamaged car) | D (program `0xd70`-`0xea0`) |
| VU1 qwords 20-23 | the A+D packet the program kicks before geometry (TEX1 etc.) | V |
| registers persist across runs | the matrices are set by setup programs and survive between MSCALs | V |

So a renderer can get, per batch, world-space positions `M*p`, world normals `R*n`, UVs and
the camera. Fog colour comes from FOGCOL in the frame's state packet (0xEBC9C9 here).

## 3. Pairing a draw with its source batch and texture

* Kick -> batch: every XGKICK happens inside the run that issued it; in the producer stream
  the PATH1 packets after a `VU1R` record belong to it. V.
* Kick -> texture: per batch the game streams the texture into one of two alternating VRAM
  slots (TBP 0x2bc4 / 0x2fc8) over PATH3, kicks TEX0 from program 0x2538, then draws. V.
  Pairing must therefore use the TEX0 base pointer in force for the batch and the GS-side key
  logged for that pointer, not "the last texture logged".
* Texture identity: at draw time the GS thread computes the host-texture keys
  (`texture_replacements.h`: index key over palette indices, palette key over RGB) from VRAM.
  79 of 80 keys in the frame resolve to named disc textures (`AFTON.TRK`, `RACE.RES`,
  `HOBBYSET.RES`, `OVERLAYS.RES`, `PART.RES`, `CT.RES`), so captured draws pair with the
  material table of the texture-tagging work (keyed the same way). V.
* Strip connectivity: use the input ADC flags (authored). The kicked ADC flags also carry the
  program's culling (off-screen and back faces); a renderer with its own camera must not
  inherit those. V (zenith sky tiles: all kicked vertices ADC, input strips intact).

## 4. Where the 3D world ends and the HUD begins

Order in the captured frame (kick numbers):

| Kicks | Content |
|---|---|
| 0-1 | PATH2 clear sprites |
| 3-124 | sky (0x270/0x280) |
| 140-1548 | terrain and track (0x618, 0x270, 0x920), some props |
| 1550-1655 | shadow casters into FBP 0x19f, PATH2 state between each |
| 1657 | PATH2 sprite: shadow composite onto the ground |
| 1662-2592 | cars, driver, props, wheels |
| 2595-2638 | particles; five late camera-space batches |
| 2642-2774 | HUD: 0x270 with ITOP 0x200 and its own 2D matrix |

Boundary rule: the first VU1 run in ITOP mode 0x200 whose matrix is not the camera (D: holds
in this frame; check menus, split screen and the pause overlay before relying on it). Off-screen
work is identified by FRAME_1 (FBP 0x19f), not by order.

## Proof

`EOD_SCENE_CAPTURE=... EOD_SCENE_CAPTURE_AT=1000 EOD_SCENE_CAPTURE_N=4` on Afton, then
`tools/scene_capture.py reconstruct CAPTURE --out DIR --disc GAME --screenshot frame_00998.png`.
The rebuilt frame (1,162 batches: 375 world, 633 rigid objects, 36 scaled objects, 13 sky;
26,880 triangles; 79/80 textures) renders from the captured camera with the car, its livery,
fences, cones, trucks, terrain and road layers where the game drew them, and the triangle
wireframe sits on the game's screenshot of the same frame. Images and `scene.json` are kept
outside Git in the session notes (`scene-capture/compare_frame998.png`).

Known differences in the checker render (renderer gaps, not capture gaps): no GS fog (distant
cliffs read grey instead of fading into the sky), no GS blend equations for the car's
multi-pass layers, nearest texel sampling, no shadow pass.

Original mode: every hook is behind `scene_capture::g_on` (read once from the environment).
A GS dump of the same race (`EOD_GS_DUMP_AT=995`, 4 vblanks) replayed through the hardware
renderer gives byte-identical output with the capture off and on (md5 `63b6db1f...` both),
while the GS-side hook logged 642 texture states during the "on" replay. Live runs cannot be
compared image to image: two runs of the same build already differ by frame 990.

## Proposed capture design (for the modern renderer)

The spike snapshots 16 KB of VU memory per run (40 MB per frame, stalls the producer for about
five vblanks). The production path keeps the same hook points and emits decoded batches:

1. **Hook at `vu_execute` for VU1** (both interpreter and compiled programs pass through it):
   look up (entry, ITOP) in a small table of known geometry programs (section 1). For a
   geometry program, read count, positions, normals/colours, UVs, ADC, offsets (section 2) and
   vf4-vf7; for the car programs also the skin weights and qwords 143+i.
2. **Per frame, the camera** = the x, y, w rows shared by the world batches; per batch
   `M = inv(camera) * batch` (rigid for objects; identity for world). Emit `{entry, itop,
   M, vertex range, light q17 / vf12 rotated to world}`. Static world batches can be cached
   by the VIF source address (the GRAF mlist batch pointer) and only referenced each frame.
3. **Hook at `xgkick`** only to confirm the batch drew (some kicks are culled) and to record
   the FRAME_1 target, so off-screen passes (shadows) are skipped or handled separately.
4. **GS thread**: the texture key per TEX0 base pointer, as the spike does, joined by the
   kick count. The modern renderer resolves the key to its material.
5. **Composition**: the GS renderer keeps drawing everything from the HUD boundary (section 4)
   and all 2D, into the same depth buffer as the modern renderer's world. While the modern
   renderer is on, the GS skips the 3D kicks it replaces (sky, world, objects, particles
   optional), keyed by the same (entry, ITOP, FRAME) rule.
6. Off unless the renderer is selected; Original mode keeps today's path untouched.

Reuse check (AGENTS.md): RT64 (N64) gets its 3D from the game's display lists, and its
extended GBI lets a port tag matrices with group IDs for interpolation; that is the same
"tag transforms at the source" idea, applied here at the VU1 entry. Road Trip's frame
generation pairs VU1 per-object matrices with PATH1 packets (`MotionTracker::packetReproject`)
but replaces no renderer and captures no world-space scene. Details in
`docs/RENDERER_REFERENCES.md`.

## Tools

```
EOD_SCENE_CAPTURE=scene.bin EOD_SCENE_CAPTURE_AT=1000 [EOD_SCENE_CAPTURE_N=3]   # game run
python3 tools/scene_capture.py stats scene.bin
python3 tools/scene_capture.py runs scene.bin --pc 0270 --kicks 2
python3 tools/scene_capture.py reconstruct scene.bin --out DIR [--disc GAME] [--screenshot PNG]
```

`reconstruct` writes `scene.obj`/`.mtl` (world space, one group per batch, texture PNGs when
`--disc` is given), `scene.json` (camera matrix, per-batch role, model matrix, texture keys,
projection error) and `reconstruction.png`/`compare.png`. Capture files contain game data:
never commit them.

## Open questions and next decisive tests

1. Driver program 0x24b0: qword 144 reads (1, 0, 0, 0), consistent with bone matrices (true
   skeletal animation). Decode its skin path; the batch matrix alone reprojects to about 1 px.
2. Light colours (vf1, vf2, q17.w) and the meaning of vf12: test by changing time of day or
   track (Anoka, Spring) and compare with each track's `SunBright`/`Ambient` display objects.
3. The five camera-matrix batches drawn in ITOP mode 0x200 just before the HUD (kicks
   2632-2638): identify them (skid marks, sun flare, mirror?).
4. Repeat the capture on a menu, split screen, a crash replay and the garage before fixing the
   HUD boundary rule.
5. Next build step: the decoded-batch emitter (design step 1-2) feeding a test viewer that
   renders the live race from the captured camera, measured for per-frame cost.

## STAR handoff (2026-11-13)

**Situation.** The GS renderer only sees pre-lit 2D triangles, so modern lighting is
impossible there. The agreed plan: capture the 3D scene at the VU1 boundary, let a modern
renderer (Filament candidate) draw world and cars, keep the GS for HUD/2D with shared depth.

**Task.** Prove the real 3D scene can be taken from the running game each frame and paired
with textures/materials, without changing Original mode; propose the production capture.

**Action.**
- Files: `runtime/vu/scene_capture.{h,cpp}` (new, opt-in recorder), hooks in
  `runtime/vu/vu.cpp` (`vu_execute`), `runtime/gs/gs.cpp` (`gif_transfer`, `gif_process`,
  `gs_frame_end`, vblank), `runtime/gs/gs_draw.cpp` (`capture_texture` in `setup_env`),
  `tools/scene_capture.py` (stats / runs / reconstruct), this document,
  `docs/RENDERER_REFERENCES.md` (RT64 and Road Trip reuse check), `docs/INDEX.md`.
- Separate commit: Windows build fix (`src/hle_movie.cpp` missing `<cstdio>`) and Win32
  `EOD_MUTE=1` support (`runtime/platform/win32/platform_win32.cpp`).
- Real game runs: Afton, No-Rules Race, Venom, headless, muted, frame 1000 window; GS dump at
  vblank 995 replayed with capture off/on.

**Result.**
- Verified: program roles by (entry, ITOP); batch layout (count, normal/colour + ADC,
  position, s/t, TOP+198/199 offsets); vf4-vf7 = model->screen (median 0.06 px); camera =
  shared x/y/w rows; rigid per-object model matrices; light direction at VU1 qword 17 (model
  space, one world vector for all batches); UVs exact to 1e-7; texture pairing via the TEX0
  slot (two alternating VRAM slots) with 79/80 keys resolved to disc textures; frame order and
  the HUD start (0x270 ITOP 0x200); rebuilt frame lines up with the game's screenshot;
  Original mode byte-identical in GS replay.
- Derived: vf8-vf11 clip matrix; car skin = translation offsets at qwords 143+i; HUD boundary
  rule beyond this one frame.
- Hypothesis: q17.w ambient; meaning of vf12 (second light/environment); vf1/vf2 light
  colours; driver program 0x24b0 uses bone matrices.
- Not done: the modern renderer; the compact per-batch emitter (the spike writes about 40 MB
  per frame and stalls the producer for ~5 vblanks); menus/split screen/garage captures.

**Next decisive test.** Implement design steps 1-2 (decoded-batch emitter at `vu_execute`,
no 16 KB snapshots), run a full race lap with it on at native speed, and render the live
camera's world+objects in a test viewer next to the GS image every frame: success is matching
geometry each frame at no frame-time loss, with Original mode unchanged. Then repeat the
capture on a menu, split screen and the garage to confirm the HUD boundary rule.
