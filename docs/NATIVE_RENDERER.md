# Native renderer (plume)

Decision (2026-11-18): the game's own render data feeds a renderer this project owns, built on
[plume](https://github.com/renderbag/plume) (MIT; the render interface behind RT64 and Unleashed
Recompiled). Filament was rejected: it reorders draws itself, its materials cannot choose a blend
equation (the PS2 look needs both), it supports only MSVC on Windows, and no shipped game uses it.
Research and sources: docs/RENDERER_REFERENCES.md. Success measures: docs/NATIVE_RENDERER_SUCCESS.md.

## What it is

One renderer, two looks in the same scene, switchable live:

- **PS2 look**: draws in the game's own submission order, GS alpha blend equations as real blend
  states (including reverse subtract), texture x vertex colour, the game's fog per vertex. Proven
  against GS replay frames of the same scene.
- **Modern look**: PBR (Filament's published BRDF and IBL maths, not its runtime), sun from the
  game's light setters, cascaded shadows, IBL from the sky, screen-space reflections, water with
  refraction. Material values from the surface tags (docs/MATERIAL_TAGGING.md).

Split screen is one scene drawn into one viewport per player: every view gets the same objects,
LOD, passes and effects as single player (P8).

## Data in (game side, decompiled)

| Source | Entry point | What the renderer gets |
| --- | --- | --- |
| Draw queue (cars, props, characters) | `0x34AF98` flush, records from `0x34B140` (src/render_queue.cpp) | batch (static VIF data -> mesh), model matrix (matrix packet +0x10), material packet, texture slot, layer, clip flag, draw order |
| Track world | `0x358BD0(pass 0..4)` via `0x214B68` | world leaves visible this view (VU0 mlist results), per pass |
| Skinned characters | `0x34C9F8` bone palette (20 bones) | 3x4 bone matrices per draw; weights from the batch data (float - 32768) |
| Car damage | V3-32 offsets to VU1 qword 0x90 (writer not traced yet) | per-bone translation (up to 157) |
| Shadows | `0x34C208`, `0x34C4B0` -> `0x35D298` | caster batches, shadow plane |
| Particles, sprites | `0x3639D0`/`0x363B98`, `0x363DE8`/`0x363F48`, `0x3642F4` | emitter quads |
| Sun glare, flares, glows | `0x304D50` (single viewport only today) | flare positions, intensities |
| Water | `0x325728` (surface `*(0x3DC134)`), simulation `0x325E40` | water plane, waves |
| Immediate geometry | `0x35DF10` / `0x361708` | primitive lists |
| Camera, light, colour, fog | camera packet `0x35B3D0`; `0x35E388`, `0x35E420`, `0x34E880`; fog `0x4D8B80` | view/projection, light direction, colours, fog range and colour |
| Textures | texture slots `*(0x4D2D30)`, manager `0x3514F0` -> `0x360680` | texture uploads (CLUT, mips) |
| HUD, menus, fonts, movies | PATH2/PATH3 GS work (not mapped yet) | 2D layer |

## GPU side

- plume Vulkan backend on Windows and Linux, Metal on macOS. plume's D3D12 backend does not
  build with MinGW's DirectX headers and is not needed.
- One device. plume creates the instance and device; until the GS emulator leaves the frame
  path (M6), the legacy Vulkan code (runtime/gs/vulkan) borrows plume's handles instead of
  creating its own, so 3D from plume and 2D from the GS compose on one device with no copies.
- Static geometry is decoded once (port of tools/vif_mesh.py) and kept in GPU buffers; per frame
  only matrices, bone palettes and dynamic effect vertices are uploaded.
- Shaders: HLSL compiled offline with DXC to SPIR-V (and to Metal with SPIRV-Cross), checked in
  as generated .inc files, as runtime/gs/vulkan already does.

## Stages (each verified in the real game, borderless or exclusive fullscreen, before the next)

| Stage | Result the player sees | Proof |
| --- | --- | --- |
| R0 | plume owns the device; the game looks and runs exactly as before | Original GS replay md5 unchanged; fullscreen race and menus |
| R1 | cars, props and characters drawn by plume from the draw queue (PS2 look, untextured, then textured) over the GS frame | every queue record drawn (counts equal); frame comparison against the GS |
| R2 | track world, sky and water from the world renderer | world leaf counts equal; Dundas water visible in 1P and 2P |
| R3 | skinned characters and car damage from bone palettes | posed vertices within 1/16 px of the GS |
| R4 | shadows, particles, sprites, flares, glare | effect counts equal; visual comparison |
| R5 | PS2 look matches GS replay (P3 thresholds) | comparison tool |
| R6 | Modern look: PBR, shadows, IBL, reflections, water; live toggle | P4, P5 |
| R7 | HUD, menus, fonts, movies, render targets native; GS and VU1 out of the frame path | S4 = 0 |
| R8 | 60 fps at RenderScale 6, max LOD, Modern look on the target box; floors on the minimum box | S1, S2 with Tracy and EOD_GPU_PROF |
