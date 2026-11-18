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
| R1 | cars, props and characters drawn by plume from the draw queue (PS2 look, textured) in place of the GS's drawing of them (done: section "Stage R1") | every queue record drawn (counts equal); frame comparison against the GS |
| R2 | track world, sky and water from the world renderer | world leaf counts equal; Dundas water visible in 1P and 2P |
| R3 | skinned characters and car damage from bone palettes | posed vertices within 1/16 px of the GS |
| R4 | shadows, particles, sprites, flares, glare | effect counts equal; visual comparison |
| R5 | PS2 look matches GS replay (P3 thresholds) | comparison tool |
| R6 | Modern look: PBR, shadows, IBL, reflections, water; live toggle | P4, P5 |
| R7 | HUD, menus, fonts, movies, render targets native; GS and VU1 out of the frame path | S4 = 0 |
| R8 | 60 fps at RenderScale 6, max LOD, Modern look on the target box; floors on the minimum box | S1, S2 with Tracy and EOD_GPU_PROF |

## Stage R1: queued 3D objects drawn by plume

Setting: `[Video] NativeRenderer=1` in eod.ini or `EOD_NATIVE=1` (default off; Vulkan renderer
only). With it off nothing below runs and the GS command stream is unchanged.

### What the player sees

Cars, props and characters (every record the draw queue `0x34AF98` emits) are drawn by the
native renderer at the game's RenderScale, in the game's draw order, into the game's own frame
and depth buffers. The GS no longer draws those batches; it still draws the track world, sky,
projected shadows, particles, HUD and menus, so native objects and GS geometry occlude each other
through the shared depth buffer, and the HUD lands on top.

### Data path (files)

| step | thread | where |
| --- | --- | --- |
| record handed over before its batch DMA is built (`scene_tap` values, camera packet inputs, mesh id) | EE | `src/render_queue.cpp` -> `native::ee_object` (runtime/render/native_feed.cpp) |
| batch decoded once (vifmesh), cached by batch address, re-checked each frame with XXH3 of header + VIF data | EE | `mesh_for` |
| the packet `0x3613C0` reserves for the batch's DMA REF (reserve ra `0x3613E0`) is tagged with the record; any other packet reserved over it removes the tag | EE | `ee_packet_reserved`, from the reserve wrappers in src/render_trace.cpp |
| VU1 program start: if the DMA tag VIF1 reads is a tagged REF that still points at `batch + 0x38`, the record (OBJ) goes into the GS stream; the next other start sends OFF | VIF1 (offload worker) | `vu1_start`, from runtime/vu/vif.cpp |
| between OBJ and OFF the GS drops the record's PATH1 primitives (register writes still apply) and keeps the GS state of its first primitive | GS | `native::gs_draw` (runtime/render/native_renderer.cpp), runtime/gs/gs_draw.cpp |
| pending objects are drawn just before the GS's next own primitive, a transfer (TRXDIR) or the vblank | GS | `flush` |
| the backend ends its work, takes ownership of the frame/depth surfaces and hands over its command buffer | GS | runtime/gs/vulkan/native_bridge.inc |

GS command `CMD_NATIVE` (runtime/gs/gs.cpp) is live-only, like `CMD_MODERN`: never written to GS
dumps, so replays and their md5s do not change. If the Modern renderer is live it keeps drawing
these batches and the native renderer stands aside.

### Renderer (plume)

- One plume `RenderPipelineLayout` (128-byte push constants, set 0 = texture + sampler), shaders
  `runtime/render/native_shaders.hlsl` compiled by `tools/build_native_shaders.py` (DXC 1.8.2407,
  the build plume pins: renderbag/dxc-bin `7810655`) into `native_shaders_spv.inc`.
- Pipelines (plume `createGraphicsPipeline`) per GS state class: blend equation, depth test
  (ALWAYS/GEQUAL/GREATER/NEVER), depth write, colour write mask from FBMSK.
- Meshes: one plume `RenderBuffer` (upload heap) per decoded batch, vertices then 32-bit indices.
  A batch whose data changed gets a new mesh; the old buffer is released 8 vblanks later.
- Textures: plume `RenderTexture` (RGBA8) decoded from GS memory with `gs::tex_fetch` (the GS
  front end's decoder: PSMT8/PSMT4 through the CLUT, CT16/24/32 through TEXA) with TEX0 as the
  object's first GS primitive had it, uploaded through a plume staging buffer and
  `copyTextureRegion`. See "Textures" below.
- Target: the backend's frame buffer and depth buffer `VkImage`s are wrapped as plume
  `VulkanTexture`s (plume's foreign-image constructor) with a plume framebuffer, moved from
  `GENERAL` to attachment layouts with plume barriers and back afterwards.
- Command recording: a plume `VulkanCommandList` whose `vk` is pointed at the backend's command
  buffer for the duration of a flush, so plume's commands sit at the exact GS command position
  with no extra submit or semaphore. plume's render passes `LOAD`/`STORE` the surfaces.

### Projection (camera packet 0x35B3D0, from its inputs)

Model -> GS screen = P x V x M (column vectors), as the VU1 setup program `0x0000` combines VU1
q5..8, q13..16 and TOPS+0..3 into vf4..vf7 (native_feed.cpp `screen_matrix`):

- V: rows of the camera rotation `0x3E57A8`, translation `-R c` (c = `0x3E57CC`); the packet's
  q16.w (`fov * width / 2`) is replaced by 1 in the program (`max.w vf4, vf0, vf0`).
- P (q5..8, the packet's projection times its viewport matrix):
  `x' = f x + cx z`, `y' = sy f y + cy z`, `z' = zk + zb z`, `w' = z` with
  `f = fov * W / 2`, `cx = vx + 2047.9375 + W/2 - 320`, `cy = vy + 2047.9375 + H/2 - 224`,
  `sy = -0.93333 (0xBF6EEEF0) / aspect`, `zk = far near 65499.4 (0x477FDB00) / (far - near)`,
  `zb = (far - 65500 near) / (far - near)`; inputs `0x4D7C10..2C`, aspect `0x3DD658`.
- GS screen -> surface clip space: `ndc = (X - XYOFFSET + 0.5) * 2 / size - 1` (the GS backend's
  convention), depth `Z / 2^bits` of ZBUF's format (how the backend stores GS depth).

`EOD_NATIVE_CHECK=1` compares this matrix with vf4..vf7 at every record's first VU1 program
start: worst relative error 1.3e-6 over 172,470 records (race, 900 frames).

### Shading (PS2 look, from the VU1 microcode)

- Lit vertices (body loops at VU1 `0x1BD8` and `0x948`): `n = (b - 128) / 128`,
  `rgb = min(TOPS+5 + max(0, n . vf12) * TOPS+4, 128)`, `vf12 = M^T L * L.w`, truncated. The GS's
  first vertex of each record matched (`EOD_NATIVE_PASSES=<vblank>` logs both per record).
- Vertex alpha: ITOP's low byte (`mfir.w vf22, vi1`) for programs `0x270`, `0x2A8`, `0x318`.
  For the car program `0x2E0` it is the highlight `max(0, 1152 (n . h) - 1056)` with
  `h = q17 = -normalize(M^T (camera forward + L))` (`loi 1152` / `loi 1056` in the `0x948` loop).
  Car bodies use TFX HIGHLIGHT2, which adds this alpha to the colour.
- Prelit (V4-5 colour) vertices: the colour as decoded.
- GS texture functions (modulate, decal, highlight, highlight2, TCC), alpha test (discard for
  AFAIL KEEP / ZB_ONLY), blending `(A - B) C + D` for the cases the game uses, through dual-source
  blending (C = As or FIX from the shader); GS alpha is written unblended.

### Textures (what the game does)

- Each bucket key is a texture slot (`*(0x4D2D30)`, 0x44 bytes); the layer begin `0x34F988`
  makes the texture manager (`0x3514F0` -> `0x360680` -> `0x35F278`, VU1 program `0x2538`) upload
  the slot's texture into one of two shared GS areas before the bucket's records. In races TEX0
  alternates between TBP `0x2BC4` and `0x2FC8` (PSMT8 and PSMT4 with a CLUT, 32x32 to 512x256).
- TEX0 therefore does not identify a texture within a frame. The native cache is keyed by
  content: the 256-byte GS blocks the texture covers plus the TEX0 fields that decide decoding
  (block offset, TBW, PSM, size, CLUT hash, TEXA). An address whose pages were not written since
  its last lookup (`g_page_gen`) skips the hash. Hashing whole 8 KB pages instead kept missing
  (other textures and CLUTs share the pages): 6,025 decodes in 1,200 vblanks; per block, 182
  decodes in total for the race, none after the first frames.
- Pages a GPU render target holds newer than the CPU copy (render-to-texture) are not decoded;
  such objects are drawn untextured and counted (`render-target texture untextured`): 0 in race
  and free roam.
- The cache grows with what the loaded content uses; above 256 MB, textures unused for 120
  vblanks are released.

### Draw batching

Pending objects are drawn in one plume render pass until the GS draws a primitive of its own,
a GS memory transfer touches their frame or depth buffer pages, or the vblank. The texture
manager's uploads between buckets do not end the pass (each object's texture was decoded when
it was queued). Race: 4 render passes for about 965 objects per frame.

### Evidence (2026-11-18, headless unless stated, Radeon Pro 580)

- Counts, per EE frame (FrameBegin to the next in a later vblank, as `EOD_RENDER_TRACE`):
  records queued = records tagged = records the GS received = drawn + skipped by the GS's frame
  skip, 0 lost, on every frame of 3000-vblank runs at RenderScale 6: Afton Jump Race 1,215
  frames (798,780 records), career free roam 785 frames (383,802 records). The render trace's
  emitted batches equal the native record count at every sample after the first (first sample:
  1182 vs 1195 race, 536 vs 550 free roam, a frame-boundary difference at start-up).
- Projection: `EOD_NATIVE_CHECK=1`, worst relative error 1.3e-6 against VU1 vf4..vf7.
- Original identity: GS replay of the R0 race dump, VRAM md5 `1ae45b16...` (1x) and
  `8a86f217...` (6x), unchanged; `CMD_NATIVE` never enters dumps.
- Borderless fullscreen, installed settings, 2400 vblanks of the race: game 19.2 frames/s off,
  20.1 on (EE-bound in both); GS thread busy 87% off, 56% on; GPU 12.8 ms off, 9.8 ms on per
  presented frame.

### Not done in R1 (open)

- Skinning: driver and pedestrian bone palettes (`0x318`, `0x24B0`) and car dent offsets (`0x2E0`
  `Skin::Offset`) are drawn in bind pose (R3). Damaged cars show their undamaged shape; driver
  arms are in the bind pose.
- Mip levels: level 0 only (bilinear where TEX1 asks for it). The GS uses coarser levels at a
  distance, so distant props are sharper than in the original.
- Fog (FGE), region clamp/repeat (approximated by clamp), destination-alpha blend factor
  (approximated by As), AFAIL FB_ONLY / RGB_ONLY (drawn as pass).
- The world, sky, projected shadows, particles and HUD are still the GS's (R2, R4, R7); the VU1
  programs of queue batches still run (their GS output is dropped), so S4 is unchanged.
