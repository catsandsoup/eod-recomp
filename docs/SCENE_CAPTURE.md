# Scene capture: the game's 3D frame for a modern renderer

## Player success

A player turns on the modern renderer and sees the same race (same track pieces, cars,
props and sky in the same places, same camera) lit with sun shadows, a physical sky and
modern materials. The HUD, menus and 2D overlays look exactly as today. Original mode stays
pixel-identical. This document establishes that the game's real 3D scene can be taken from
the running game each frame and records the data a renderer such as Filament needs.

Status: the capture spike works in the real game (Afton, No-Rules Race, Venom, headless,
muted). One race frame was captured, rebuilt in world space with its real textures, and
re-projected onto the game's own screenshot of that frame. The production-shaped
**decoded-batch emitter** (`EOD_SCENE_EMIT`, design steps 1-2) now runs every frame of a live
race at full speed, and a test viewer renders its output from the captured camera with the
game's light, lined up with the game's screenshot (section "Decoded-batch emitter"). No
modern renderer exists yet.

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

## Decoded-batch emitter (design steps 1-2, built)

`runtime/vu/scene_emit.{h,cpp}`, hooked next to the spike (`vu_execute`, `gif_transfer`,
`gif_process`, `gs_frame_end`, the producer vblank, `setup_env`). Off unless `EOD_SCENE_EMIT`
is set; every hook is behind `scene_emit::g_on`.

Per frame, on the producer (VIF1/VU1 worker) thread:

* **Meshes** (`GEOM`), once per distinct content: model-space positions and s, t with the
  TOP+198/199 offsets applied, normals (int8) or vertex colours, authored strip flags (ADC).
  The id hashes the batch's vertex block, so static meshes are sent once and then referenced
  (design step 2's cache, keyed by content instead of VIF address).
* **Batches** (`BATC`): (entry, ITOP), kick range, mesh id, world matrix
  `M = inv(camera_xyw) * batch_xyw`, the light direction (q17) and vf12 rotated to world,
  q17.w, and the batch's model->screen matrix. Runs that kick nothing are dropped. Programs:
  the geometry entries of section 1 plus 0x350 (facing sprites); shadows, particles and state
  programs are left out (`EOD_SCENE_EMIT_ALL=1` admits every program, for diagnosis).
* **Camera** (`CAMR`): the most common x/y/w rows of the frame's batches.

On the GS thread, one row per textured PATH1 draw state: kick number, TEX0 pointer, FRAME,
XYOFFSET and the host-texture keys. The index key (a decode of every texel) is cached by a
linear hash of the VRAM pages the texture occupies.

`tools/scene_view.py` is the test viewer: `stats`, `verify` (reprojection against the kicks
of an `EOD_SCENE_EMIT_VERIFY` frame), `render` (the frame from the emitted camera and light:
texel x (q17.w ambient + diffuse max(0, N.L)), or texel x vertex colour; beside the game's
screenshot with an edge overlay) and `strip`. It reads only emitted data; textures come from
the disc through the keys.

Results (Windows, D3D11, headless, muted; Afton/No-Rules Race/Venom, which the session start
resolves to Eve 16 "Woodly", 14 cars; 3,000-frame runs):

| Measure | Result | Conf. |
|---|---|---|
| Reprojection, all 1:1 batches of the verify frame (2,319 batches, 73,481 on-screen vertices), emitted camera x emitted M | median 0.056 px, 94.1% under 0.5 px | V |
| Same without the skinned driver (0x24b0, 0x318) | median 0.054 px, p99 0.34 px, max 2.3 px, 99.3% under 0.5 px | V |
| Camera x M vs the batch's own matrix | identical: the decomposition adds no error | V |
| On-screen vertices over 1 px | all from the driver programs 0x24b0 and 0x318 (open question 1) | V |
| Viewer vs game screenshot | car, livery, wheels, ground and road layers, tyre stacks, fences, trucks and skyline line up; 128/129 texture keys resolve to disc textures | V |
| Producer (VU1 worker) cost per drawn frame | mean 1.25 ms, p50 1.00, p95 2.11, max 18.1 (2,024 frames, including the stream write) | V |
| GS-thread cost | mean 1.99 ms, p50 1.64, p95 3.32, max 14.0; the first frame after a load decodes every key (~16 ms, cold cache) | V |
| Stream | 276 KB per drawn frame (~16 MB/s); 1,540 batches per frame on average, 2,400 in busy frames; 19,700 distinct meshes over 2,100 frames | V |
| Frame rate | 59.9 vblank/s off and on; game frames/s 55.6 and 56.6 (off), 54.9 (on), 53.2 (on, decode only); EE busy 70-74% off, 78% on; GS busy 35-37% off, 42-45% on. Within run-to-run spread (runs are not deterministic) | V |
| Original mode | GS replay of a race dump (vblank 1500, 4 vblanks): VRAM after the replay byte-identical with the emitter off and on (md5 `f30d64c0...`), while the emitter logged texture rows | V |

Frame numbering: the emitter names a frame by the vblank that ends it; `--screenshot-every`
frame N shows the work emitted as frame N-1 (frame 997 vs `frame_00998.png`: edge agreement
40%, vs 34% against `frame_00997.png`). Saving a screenshot every vblank slows the game, so
around the shots some vblanks carry no draw work; elsewhere the game draws nearly every vblank.

The comparison image (game | viewer | viewer edges on the game) is kept outside Git in the
session notes (`scene-capture/emitter_compare_frame997.png`).

Viewer gaps (renderer, not data): no fog, no blend equations for the car's multi-pass layers,
nearest texels, no shadow pass, no particles. The start-line flag is not in the emitted set
(its program is not identified yet).

## Modern renderer, first live pass (`EOD_MODERN=1`)

The emitter now also feeds a live modern pass in the Direct3D 11 backend. The GS keeps the
HUD, menus, 2D, particles and the game's own sky dome; the modern pass draws the race's
world, cars and props from the decoded batches and is switched live: **F9**, or
**L1+R1+R3** on the controller (with an on-screen banner). The original renderer keeps running
underneath, so the switch is instant.

How it composes (`runtime/gs/modern_scene.{h,cpp}`, `runtime/gs/d3d11/modern_d3d11.inc`, hooks in
`scene_emit.cpp`, `gs.cpp`, `gs_draw.cpp`):

* Producer: for each replaced batch it queues a batch marker into the GS command stream
  (`CMD_MODERN`, live only, never written to GS dumps); the GS skips the draws of that batch's
  PATH1 kicks and hands the backend the batch's texture, blend equation, alpha test and vertex
  alpha. At the HUD (program 0x270 in ITOP mode 0x200 with the screen-facing matrix: w row
  (0, 0, 1, 365); the same program also draws camera-space batches earlier in the frame) the
  producer queues the frame (camera, model matrices, roles, world layers, new meshes).
* Backend: copies the GS frame so far (the sky) into a 4x MSAA target at the internal
  resolution, renders a 2048² sun shadow map from the game's light direction, draws the
  world in draw order (layers by the batch's GS depth row) and the objects with reverse
  depth (near / camera w), resolves and copies the result into the game's frame buffer; the
  GS then draws the HUD on top.
* Shading: linear-space lighting from the game's light and ambient (q17), baked vertex
  colours for the terrain under the sun shadow, glossy car paint (specular and a sky
  reflection with Fresnel), distance haze in the game's fog colour, filmic tone mapping,
  16x anisotropic filtering. Blend equations follow the GS (mix / add / subtract / fixed);
  the GS alpha test and source alpha (MODULATE / DECAL / TCC) are honoured per batch.
* Safety: if the backend cannot start the pass (no shader compiler, device failure) it turns
  itself off and the GS draws everything.

Results (Windows, D3D11, Afton/No-Rules Race/Venom, headless, muted):

| Measure | Result | Conf. |
|---|---|---|
| Same-moment comparison (`EOD_MODERN_FLIP_EVERY=50` alternates the renderers) at 3x (1920x1344) | world, props, cones, fences, trucks and the car line up with the original frame; HUD and messages intact; car lit with gloss and reflections; image kept in the session notes (`scene-capture/modern_vs_original_3x.png`) | V |
| Original mode | GS replay of the race dump with this build: VRAM after the replay byte-identical to the build before the modern pass (md5 `f30d64c0...`); every hook is behind `modern::g_on` | V |
| Frame rate at 3x (3,000 frames each) | 59.9 vblank/s in both modes; game frames/s 58.7 (modern off) and 56.0 (modern on), inside the run-to-run spread measured before (53-57); EE busy 77% -> 84%, GS busy 37% -> 35% (the GS skips the replaced draws) | V |
| Failure path | the shader-compile failure seen during development switched the pass off; the GS then draws everything | V |

Since the first pass (commits after `8007e86`):

* Per-vertex alpha and per-primitive blending from the game's own PATH1 kicks: the car damage
  layers fade in vertex by vertex as in the original; terrain blends per primitive.
* The modern pass runs before the particles (at the first particle program after the scene) and
  writes the depth the GS would have written, so particles, the start flag and the late
  camera-space batches are drawn by the GS over it and hidden correctly.
* Sun shadows: a 4096x2048 atlas with two cascades, sizes smoothed per track, centres snapped
  to texels. Only what is visible casts (blended primitives by their GS alpha, alpha-tested
  texels). Percentage-closer soft shadows: the blocker distance sets the filter width (soft
  away from the caster, sharp at contact), 16 rotated disk taps.
* The game's own projected car shadow (programs 0x0020 / 0x0338, a hard dark quad on the
  ground) is not drawn while the modern pass is live (`EOD_MODERN_BLOBS=1` keeps it).
* Shading follows Filament's standard model (docs: Filament "Physically Based Rendering in
  Filament"): GGX distribution, height-correlated Smith visibility, Schlick Fresnel, a clear
  coat layer on car paint (Kelemen visibility), split-sum image light from the game's sky with
  an analytic DFG fit. Post: ambient occlusion from depth with a surface-plane test, bloom,
  filmic grade.
* Settings in the in-game menu (Graphics, MODERN RENDERER; saved in `eod.ini` [Video]):
  RENDERER (MODERN / ORIGINAL, also F9 and L1+R1+R3), SUN SHADOWS, SOFT SHADOWS, REFLECTIONS,
  AMBIENT OCCLUSION, BLOOM, EXPOSURE, DISTANCE HAZE, COLOUR GRADE, VIGNETTE (off by default).
  All live. The renderer is available in every Windows D3D11 build (`EOD_MODERN=0` removes it,
  `EOD_MODERN=1` starts in it); while ORIGINAL is chosen the emitter does no work.

Results of this round (Windows, D3D11, Afton/No-Rules Race/Venom, headless, muted):

| Measure | Result | Conf. |
|---|---|---|
| Original mode | GS replay of the race dump: VRAM byte-identical with the modern path available and with `EOD_MODERN=0` (md5 `f30d64c0...`, as before) | V |
| Same-moment pair at 3x (1920x1344) | modern frame: soft car-shaped shadow under the car, no hard quad; original frame shows the game's quad shadow | V |
| Run-to-run determinism | two runs with identical settings give different screenshots (the game is not frame-deterministic headless); pixel identity is therefore proved by GS replay, not screenshots | V |

### Effects audit: what the game has, and where it stands in Modern

| Effect | In the original game | Modern pass today |
|---|---|---|
| Car body, paint, numbers | VU1 programs 0x2e0 (body), 0xd48 (detail), 0x15a8 (damage) | redrawn: lit, clear coat, sun shadow, damage fades per vertex |
| Car shadow | 0x0020 / 0x0338 project a hard quad onto the ground | replaced by real cascaded soft shadows |
| Driver | 0x24b0 / 0x318 (skinned) | redrawn; about 1 px off (bone matrices not decoded) |
| Tyre smoke, dust, exhaust and engine flames, sparks, fire | particle programs 0x1b8 / 0x8e0 / 0x5e0, textured sprites drawn by the GS | still the game's sprites, drawn by the GS over the modern image with correct depth; not relit, no soft-particle fade, no light emitted |
| Explosions | the same particle programs (billboard fireballs and debris sprites) | as above: original sprites; no flash light, no heat haze |
| Headlights / tail lights | none: the races are in daylight; lamps are only painted in the texture | none. A light list exists nowhere yet |
| Street and track lights | none lit; light poles are plain props | drawn as props; no emissive or light |
| Metal poles, fences, barrels | props lit by the vertex colours | lit by the sun with a rough dielectric response; no metal material (the game has no material ids) |
| Track surface reflections | none (dirt, no wet tracks) | none; the dirt is matte. Screen-space reflections would only matter for wet or polished surfaces |
| Glass | the window frames are open cages, no glass | n/a |
| Skid marks | late camera-space 0x270 batches before the HUD | drawn by the GS over the modern image |
| Sky | dome 0x270 in ITOP mode 0x280 | kept from the GS; also used as the reflected environment |
| HUD, menus, movies | GS | unchanged |

### Filament techniques mapped onto this pass

The Filament library itself is not linked: it has Vulkan, OpenGL and Metal backends but no
Direct3D 11 one, and its materials need its offline compiler. Its documented techniques are
implemented directly in the pass (no Filament code copied):

| Filament feature | Here |
|---|---|
| Standard lit model (GGX, Smith-GGX correlated, Schlick, Lambert) | yes |
| Clear coat layer | yes (car paint) |
| Image-based lighting (split sum, DFG) | yes, from the analytic sky (no captured cube map yet) |
| Cascaded shadow maps, PCSS soft shadows | yes (2 cascades) |
| SSAO | yes (surface-plane AO) |
| Bloom, ACES tone mapping, colour grading, vignette | yes, all in the settings |
| Fog | distance haze in the game's fog colour |
| MSAA / supersampling | 4x MSAA at the render scale |
| Punctual lights (point, spot), clustered forward | no: no light sources in the game's data; headlights would need car-front positions |
| Screen-space reflections | no |
| TAA, depth of field, motion blur | no |
| Anisotropic, cloth, subsurface materials | no material ids to drive them |

Known gaps (each a follow-up):

* **Vulkan**: the pass is Direct3D 11 only. The Vulkan backend (macOS) is a separate port with
  its own precompiled shaders and is not built on Windows. Moving Windows to Vulkan (one backend
  for both platforms) and porting this pass to it is the next platform step.
* Particles, fire and explosions are the original sprites; rebuilding them (lit, soft, emitting
  light) needs the particle programs decoded like the geometry ones.
* Driver skinning (0x24b0 bone matrices).
* Textures are still the game's (from VRAM).
* Shaders are compiled at start with `d3dcompiler_47.dll`; shipping needs them precompiled.
* GPU mesh cache has no eviction yet (about 25 MB after a race).

## Tools

```
EOD_SCENE_CAPTURE=scene.bin EOD_SCENE_CAPTURE_AT=1000 [EOD_SCENE_CAPTURE_N=3]   # game run
python3 tools/scene_capture.py stats scene.bin
python3 tools/scene_capture.py runs scene.bin --pc 0270 --kicks 2
python3 tools/scene_capture.py reconstruct scene.bin --out DIR [--disc GAME] [--screenshot PNG]
```

```
EOD_SCENE_EMIT=scene.emit [EOD_SCENE_EMIT_FROM=900] [EOD_SCENE_EMIT_N=200] [EOD_SCENE_EMIT_VERIFY=996]
python3 tools/scene_view.py stats scene.emit [--frame N]
python3 tools/scene_view.py verify scene.emit
python3 tools/scene_view.py render scene.emit --frame 997 --disc GAME --screenshot frame_00998.png --out DIR
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
5. Done: the decoded-batch emitter and test viewer (section "Decoded-batch emitter").
6. Identify the start-line flag's program (`EOD_SCENE_EMIT_ALL=1` lists 0x0790 ITOP 0/1,
   0x0020 and the particle programs as the remaining programs that kick).
7. Car damage moves vertices through the bone offsets (qwords 143+i), so a damaged car's
   mesh gets a new content id each frame. Emit the undeformed mesh plus the offsets.
8. Move the stream write off the producer thread (it is inside the measured 1.25 ms) and
   warm the texture-key cache at track load to remove the 14-16 ms first-frame spike.

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

## STAR handoff: decoded-batch emitter (2026-11-13)

**Situation.** The spike proved the 3D scene is available at the VU1 boundary but
snapshotted 16 KB per run (40 MB per frame, producer stalls). A modern renderer needs a
compact per-frame feed.

**Task.** Build design steps 1-2 as an opt-in emitter and a test viewer that renders the live
race from the captured camera with the game's light; measure accuracy and cost with Original
rendering unchanged.

**Action.** `runtime/vu/scene_emit.{h,cpp}` (new); hooks in `runtime/vu/vu.cpp`,
`runtime/gs/gs.cpp` and `runtime/gs/gs_draw.cpp` (`emit_texture`); `tools/scene_view.py`
(new); this document. Real game runs on Windows (headless, muted, `EOD_DIRECT_START`
Afton/No-Rules Race/Venom): emitter off/on pairs of 3,000 frames with `EOD_FPS`, a verify
frame, screenshots 990-1010, a decode-only run, and a GS dump replayed off and on.

**Result.** Verified: reprojection median 0.056 px (0.054 px and 99.3% under 0.5 px without
the skinned driver); the viewer frame lines up with the game's screenshot; producer 1.25 ms
and GS 2.0 ms mean per frame, 276 KB per frame; 59.9 vblank/s and the game frame rate within
run-to-run spread; GS replay VRAM byte-identical off and on. Not done: the modern renderer;
the skinned driver; damage deformation; the flag's program; an asynchronous write; the
HUD-boundary rule on menus, split screen and the garage.

**Next decisive test.** Feed this stream to an embedded modern renderer (Vulkan) that draws
world and cars into a depth buffer shared with the GS HUD, in a live race at full speed,
with Original mode toggleable and unchanged.
