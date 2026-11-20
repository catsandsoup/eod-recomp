# EE 3D render submission and skinned bone palettes

Status: call graph traced with `tools/ee_dis.py`, the generated function list (`tools/analyze.py`) and a
Ghidra 12.2 decompile (Emotion Engine processor module);
bone path confirmed in the running game (dealer scene, windowed). The draw queue runs as readable
C++ (src/render_queue.cpp, section "Readable replacement"); `0x35DBD8`/`0x35A810` likewise
(src/bone_pose.cpp).

## Model submission is a queue, not a DMA

`ModelSubmitVu1Packets` `0x0034BDA8 (model, const Mat *m, a2, a3)` does not send anything:

1. Bounding sphere: model table `*(0x4C96C0) + model*0x14`, header at `+8`: centre `+0x1C`,
   radius `+0x18`. The centre is transformed by `m` (3x3 at `m+0`, translation `m+0x24`).
2. `FrustumTestSphere 0x34E308`; culled models return.
3. `0x35CD28(m)` (matrix registration), then `0x34B140(model, mreg, a2, a3, f12)`.
4. `0x34B140` walks the model's batches (`0x34D060`), checks the batch layer bit against
   `*(0x3DD634)` (temporarily set to `a3`), and appends a 20-byte draw record to the pool:

| offset | field |
| --- | --- |
| 0x00 | batch pointer |
| 0x04 | next record in the bucket |
| 0x08 | flags (`a3`) |
| 0x0C | matrix handle |
| 0x10 | material/texture id (`a2`, or -1 when `0x35DF40` says the batch has none) |

   The original guest pool is at `*(0x4C96C4)`, with the count at `*(0x4C96C8)` and space for
   `0xC00` records. Original rejects the append that would raise the count to `0xC00`.
   Buckets: `0x4C96D0 + 4*(key+1) + 0x800*(*(0x4CA6D0))` (two bucket sets, double-buffered by
   `0x4CA6D0`).
5. Flush `0x34AF98(set, ...)` walks the buckets. Per record: matrix change -> `0x35E018`,
   material change -> `0x35DDC8` (last values cached at `0x4CA6D8` / `0x4CA6DC`), then `0x365288(flags)`
   and `0x35CB88(batch)` -> `0x35BA10` + `0x3613C0(batch+0x38, *(batch+0x34))` (the packet DMA).

Direct callers of `0x34BDA8` (17 sites in 12 functions): `0x1E4610`, `0x20DE68`, `0x213640`,
`VisualCarSubmitModel 0x2189C8` (4), `0x219C48` (3), `0x21A9B0`, `0x21DC18`,
`WheelSelectSlowOrFastModel 0x220788`, `0x223858` (sky), `0x235D98`, `0x2AE250`, `0x2B0FB8`.
`0x34BDA8` is **not** the single 3D entry point, and the queue does not draw the track: the track
world has its own renderer (`0x358BD0`, section "Every VU1 program start by sender"). There are
three paths into VU1 for 3D models:

| path | entry | callers | notes |
| --- | --- | --- | --- |
| queued | `0x34BDA8` | the 12 functions above | record keeps `a2` as material id |
| queued, no material | `0x34BF90 (model, m, layer)` | `0x3067B8`, `0x32DFA8` | same pool; flags come from the frustum test; flushes at once (`0x34AF98(-1, 0)`) when no frame is open (`*(0x4CA6D0) == 0`) |
| direct (projected shadows) | `0x34C208`, `0x34C4B0` | `0x30A910`, `0x30A608` | bypass the pool: `0x35D9C8(size)`, `0x35CD28(m)`, then per batch `0x35D148(1)`/`0x35D298` and `0x35D148(0)`/`0x35D298`; render target opened by `0x34C7D0` -> `0x35DA30` (`0x363218`, at most 320x512) |

## Frame flush

Source: Ghidra 12.2 decompile of SLUS_209.10 (EE module), callers from Ghidra references. Static only;
not yet confirmed with a runtime call trace.

The three frame renderers have the same shape: `0x20F2D8` (race view; per split-screen viewport),
`0x20F4A0` (visual-car slots from `0x3D1C90`, each drawn by `0x21A9B0`) and `0x22D7F0` (list at
`*(0x3D2138)`, each drawn by `0x22ECB8`):

```
0x34C9C0  FrameBegin: open set 1 (*(0x4CA6D0) = 1), pool count 0, cached matrix/material = -1
  ... draw functions queue records ...
0x34C930  flush layers 0..8 of set 1 (0x34AF98(layer, 1) each), then *(0x4CA6D0) = 0
0x34C860  flush layer 9 with the identity matrix (0x35E018(0)), then reset the pool and caches
```

`0x20F2D8` sets the viewport first (`0x34D8A0` -> `0x35E200`), runs `0x212AC8` and `0x20F0D8` (the
queuing), then after the flush `0x2E7368(0)`, `0x304D50` (single viewport only) and every object in
`0x495CB4[0..*(0x495CAC))` through vtable slot `+0x1C`.

`0x34AF98(layer, set)` per layer: `0x34F988(layer)` (layer state, `0x355068`/`0x3514F0`), layer
`*(0x3DD680)` brackets its records with `0x35A4D0(1)` / `0x35A4D0(0)`, then for each record:
matrix -> `0x35E018`, material (if not -1) -> `0x35DDC8`, `0x365288(flags)` (ITOP bit 0x200), and
`0x35CB88(batch)`. `0x34F888` closes the layer. The layer list comes from `0x354C00(layer)` (tables
at `0x4CD4B8 + layer*0x804`) or `0x354BB8` for layer < 0.

## What each record puts in the VU1 DMA chain

`0x35CB88(batch)` -> `0x35BA10` then `0x3613C0(batch + 0x38, *(batch + 0x34))`. `0x35BA10` emits, only
when the value changed since the last batch:

1. camera packet (when `*(0x3E57E4)` is set by a viewport change): `0x35B3D0`
2. DMA REF to the current matrix packet `*(0x4D7C48)` (qwc 9) with `STCYCL 1,1`, `STMOD 0`
3. DMA REF to the current material packet `*(0x4D7C54)` (qwc = its byte 2)
4. scissor/clear (`0x365350`, `0x35B8B8` -> `0x365208`) when the viewport changed

`0x3613C0` then adds the batch's own prebuilt VIF data as a DMA REF (`batch+0x38`, qwc `batch+0x34`)
with `ITOP = *(0x4D8BC0) | *(0x4D8BC4)`.

### Camera packet `0x35B3D0`: VU1 qwords 1..16

`UNPACK V4-32` 16 qwords to VU1 address 1 (`0x6C104001`), preceded by `MSCAL 0x24D8` twice.

| VU1 qword | contents | inputs |
| --- | --- | --- |
| 1-4 | viewport matrix: x/y scale, GS depth range (`-32749.5` .. `32750.5` as `0xC6FFDB00` / `0x46FFDD00`), screen centre `+2048 - 320/224` (GS XYOFFSET), w 1 | viewport `0x4D7C20..2C` (`0x35E200`), aspect `0x3DD658`, fov scale `0x4D7C1C`, near/far `0x4D7C10`/`0x4D7C14` |
| 5-8 | projection * viewport (`0x38A5D0` multiply) | same |
| 9-12 | clip-space projection (guard-band frustum for `clip`) | same, `0x4D7C60`/`0x4D7C64` (0.75 x width, 0.825 x height) |
| 13-16 | view matrix: camera rotation `0x3E57A8..` transposed, translation `-R^T t` (t at `0x3E57CC`); q16.w = `0x4D7C1C * width / 2` | camera at `0x3E57A8` |

### Matrix packet `0x35CD28(m)`: VU1 TOPS+0..6

Built in the packet buffer (`0x366C88(9)`, 9 qwords): three marker NOPs, `UNPACK V4-32` 7 qwords
to TOPS+0 (`0x6C07C000`: FLG and USN), then `MSCAL 0x0000` and `MSCAL 0x24D8`.

| TOPS+ | contents | setter (game side) |
| --- | --- | --- |
| 0-3 | model matrix `m` (3x4 floats, rows widened with w = 0,0,0,1) | the caller's matrix |
| 4 | colour `rgb * 128/255`, w 0 (`0x4D7C90`) | `0x35E388` <- `0x34E8A8(0xRRGGBB)`; called from the sky/world object `0x223800` (with -1, white) |
| 5 | colour `rgb * 128/255` (`0x4D7C70`), w = `*(0x4D7D18)` (shadow size, set by `0x35D9C8`) | `0x35E420` <- `0x34F5A0(0xRRGGBB)`, 15 call sites (cars `0x2189C8`, `0x218E68`, ..., `0x213640`, `0x11C4A0`) |
| 6 | light direction xyz (`0x4CD2E8`), w = `-*(0x3DD648)` | `0x35E3F0` <- `0x34E868`, from `0x34E880(vec3)` (`0x218850`, `0x2237A8`, `0x2AF44C`) and `0x34F570(intensity)` (15 sites) |

### VU1 setup programs (main image `vu1_fd0ce30ab0ec263c`)

- `0x24D8`: ends immediately (`nop [E]`). Used as a no-op MSCAL so the next packet waits for the
  previous program.
- `0x0000` (per object, from the matrix packet): reads TOPS+0..6 and VU1 q5..16 and leaves in
  registers for the geometry programs:
  - `vf4..vf7` = q5..8 x q13..16 x TOPS+0..3: model -> GS screen (matches the measured meaning in
    docs/SCENE_CAPTURE.md);
  - `vf8..vf11` = q9..12 x q13..16 x TOPS+0..3: model -> clip space;
  - `vf1` = TOPS+5 xyz, `vf2` = TOPS+4 xyz (the two colours);
  - `vf12.xyz` = `R^T L * L.w` (light direction in model space, scaled by `-*(0x3DD648)`), w 0;
  - q17.xyz = `-normalize(R^T (V + L))` with V = camera forward (q13.z, q14.z, q15.z) and L =
    TOPS+6: a model-space half vector. q17.w is not written here.

  Read from the disassembly (`tools/vudis.py`); the `sq` operand decoding of vudis is wrong in
  places, so the q17 store is inferred from data flow. Check against `EOD_MODERN` captures.

### Material packet

`*(0x4D7C54)` is set per record from the material id (`0x35DDC8`), or by `0x35CBD8` (shadow plane:
`0x35A810(…, 4, 0x1D, 3, 4, quad)` -> V3-32 4 vectors to VU1 qword 0x8F + 0x1D). The default material
`0x35A6E0` is `UNPACK` of zeros to VU1 0x8F with `MSCAL 0x24D8`. The materials themselves (textures,
TEX0/TEX1 A+D, the source of VU1 qwords 20-23) belong to step 3 (texture manager) and are not traced yet.

## Runtime evidence (EOD_RENDER_TRACE)

`src/render_trace.cpp` wraps the functions above and only observes (`EOD_RENDER_TRACE=<vblank,...>` logs
every event of the next rendered frame; `EOD_RENDER_TRACE_EVERY=<n>` prints per-vblank counters).
Headless run, Afton Jump Race, Original mode, vblanks 341-1500:

- Every batch emitted (`0x35CB88`) happened inside `0x34AF98`: 0 outside a flush in every sample.
- Records appended by `0x34B140` equal batches emitted (1182 = 1182 at vb 341, 944 = 944 at vb 1202),
  so in the race the queue is the only path for non-shadow 3D batches.
- `0x34BF90` and `0x34CC98` were never called in the race; projected shadows (`0x34C208` ~30/frame,
  `0x34C4B0` ~3/frame) ran after the frame flush and emitted 30-56 batches through `0x35D298`.
- About 655 `0x34BDA8` calls per frame, about 250 of them kept (the rest frustum-culled).
- Two frames per rendered vblank: `0x20F2D8` (race view) and `0x22D7F0` (the second list, 170 calls
  through the tail call in `0x22ECB8`, ra `0x22D820`). Both close with `0x34C930` then `0x34C860`.
- After the close, `0x34BDA8` calls draw at once (`0x34B140` flushes layer -1 when no frame is open):
  the sky object `0x223858` and `0x233400` objects (ra `0x233428`).
- Callers seen at runtime, by containing function (ra -> function): `0x2068F8` -> `0x2068C0` (via
  thunk `0x34BF78`; called from `0x235D98`, `0x21D1C0`, `0x219C48`, `0x2B17F8`, `0x217CE0`),
  `0x237538` -> `0x235D98`, `0x218B18/B98/BC0` -> `VisualCarSubmitModel 0x2189C8`, `0x220BF8` ->
  `0x220788`, `0x20F124` -> object vtable slot +0x18 called from `0x20F0D8`, `0x213718` -> `0x213640`,
  `0x22DB68` -> `0x22DB40`, `0x223A14` -> `0x223858`, `0x233428` -> `0x233400`.
- Fourth entry found: `0x34CC98(model)` (12 world call sites, `0x222730`..`0x232FC8`) tests visibility
  and calls `0x34B140` directly. Not exercised in this race.

### Still open

- Same trace in the career free roam, the dealer and split screen (`0x34BF90`, `0x34CC98` users).
- Where q17.w, VU1 qwords 20-23 and the TOP+198/199 offsets come from (batch data at `batch+0x38`
  is prebuilt at load time; see docs/VIF_GEOMETRY_PATCHING.md).
- HUD (PATH2/PATH3) and particles do not go through `0x34AF98`; their entry points are steps 4 and 8.
- `0x25F0` is a short setup program: loads `vf14` = TOP+0 and `vf13` = TOP+1, two stores, ends at
  `0x2610`. The program after it (`0x2630`) adds `vf13.xy` to its inputs (a UV/screen offset). Its
  EE-side sender is not traced yet.

## Skinned bone palettes (pedestrians, drivers)

Verified path:

```
0x20DE68 (pedestrian draw, called through a pointer; also calls 0x34BDA8)
  -> 0x20B8D0 (pose evaluation, inline; no calls except the upload)
       -> 0x34C9F8 BonePaletteUpload(first, const float bones[count][12], count)
            -> 0x35DBD8: widens each 3x4 (stride 0x30) to 4x4 on the stack, reserves packet space
               (0x366C88), writes VIF UNPACK V4-32 count*4 qwords to VU1 0x90 + 4*first (0x35A810)
0x1E4610 / 0x235D98 -> 0x20C800 (second pose evaluator, drivers) -> 0x34C9F8
```

Evidence (dealer scene, `START="mode=career;place=dealer"`):

- `EOD_BONE_SRC=1` VIF probe: VU1 receives `UNPACK 6c504090` (V4-32, 80 qwords = 20 bones) at qword
  0x90, sourced from the DMA packet in EE RAM (0x0065xxxx, one per character). No VU1 program
  computes bones; the microcode only reads them (`skin_vertices` in runtime/vu/scene_emit.cpp).
- `EOD_FUNC_WATCH` (trace build, `EOD_TRACE_CALLS=ON`) on a palette word: every change has
  `ra = 0x35DD04` inside `0x35DBD8`, call chain `0x20DE68 -> 0x20B8D0 -> 0x34C9F8 -> 0x35DBD8`.
- `EOD_FREEZE_POSE=800` (src/bone_pose.cpp): the bone matrices VU1 programs 0x0318 and 0x24b0 read
  vary at every sample before vblank 800 (8 distinct poses) and are bit-identical at every sample
  from 840 to 1680. All of these pedestrian poses therefore pass through `0x34C9F8`.

Car damage dents (VU1 0x2e0/0xd48/0x15a8) arrive the same way as V3-32 unpacks to qword 0x90
(source buffer seen at 0x005BF288); their writer is not traced yet.

Correction to older docs: skin weights are floats biased by 32768 (`w - 32768`), not `w/256`.

## Tools

- `EOD_BONE_SRC=1`: logs VU1 unpacks into qwords 0x90..0xDF with their EE source address, and
  each skinned batch's bone matrices (every 120 vblanks).
- `EOD_FREEZE_POSE=<vblank>`: replays each upload's pose from that vblank (diagnostic).
- Trace build: `cmake -DEOD_TRACE_CALLS=ON`, then `EOD_FUNC_WATCH=<hex EE address>`.

## Readable replacement (src/render_queue.cpp)

| address | name | equivalence (`EOD_DIFF`, headless, 2026-11-18) |
| --- | --- | --- |
| `0x34BDA8` | RenderQueueSubmitModel | race, free roam, dealer: 0 mismatches (>= 512 checked each) |
| `0x34B140` | RenderQueueAppendBatches | race, free roam, dealer: 0 mismatches (>= 128 checked each) |
| `0x34C9C0` | RenderQueueBeginFrame | race: 0 mismatches |
| `0x34C930` | RenderQueueFlushLayers0to8 | race: 0 mismatches (every call checked) |
| `0x34C860` | RenderQueueFlushLayer9 | race: 0 mismatches (every call checked) |
| `0x34AF98` | RenderQueueFlush | race, free roam, dealer: 0 mismatches |
| `0x34BF90` | RenderQueueSubmitModelKey | not called in race, free roam or dealer: unchecked |
| `0x34CC98` | RenderQueueSubmitStatic | not called in race, free roam or dealer: unchecked |

Counts are the last power-of-two report (the game exits without the final summary). Each
replacement makes the same guest calls in the same order with the same return addresses, the same
frame size and the same `$v0`, so wrappers and traces behave as before.

### Native draw-record capacity

Both `0x34B140` and `0x34BF90` continue into an adaptive host list once the guest pool fills.
Host links are indices within the immediate or view queue. A flush follows the layer's original
texture key order and draws each key's newer host records before its older guest records. The
scene tap gets the same batch, clip, matrix and material values for either storage location.

Beginning a view and resetting after layer 9 clear pending links. Set 0 also retires its host
records after the last pending immediate bucket is drained. Underused storage is released after
64 resets at no more than one-quarter capacity. The guest allocation and teardown hooks
(`0x34D1F8` and `0x34D088`) release native overflow when the model renderer changes lifetime.
The alternate generated reset entry points, `0x34C828` and `0x34C8C0`, clear pending host links
alongside the guest queue. Original and configured `EOD_DIFF` runs retain the guest pool and its
original capacity so the generated EE readers and diff snapshots see the expected layout.

Record fields in game terms: `+0x08` is the frustum test's "partly outside" flag (it selects the
clipping path, ITOP bit 0x200), not a caller flag; `+0x0C` is the matrix packet pointer returned by
`0x35CD28` (its model matrix is at packet `+0x10`); `+0x10` is a material packet pointer (the
caller's third argument, kept only for batch types 3 and 5); the bucket key is the batch's texture
slot (table `*(0x4D2D30)`, 0x44 bytes per slot; `0x353470` lowers the slot's distance at `+0x30`
for mip selection). So `0x34BDA8`'s arguments are (model, matrix, material packet, layer mask).

Free roam without the diff slowdown (vblank 900-1800): about 155 `0x34BDA8` calls, 112 kept,
536 batches per frame, all inside a flush. Race: about 960 batches. `0x34BF90` and `0x34CC98`
were not called in either.

VU1 program starts per frame (`[rtrace] ... vu1 programs:`, race vblank 1200): setup `0x0000`
582, `0x24D8` 1061; geometry and effects `0x0270` 405, `0x02A8` 553, `0x02E0` 363, `0x0D48` 1269,
`0x15A8` 88, `0x24B0` 1106, `0x1F90` 203, `0x0618` 198, `0x0920` 123, `0x2538` 175, others under
60. A batch's own VIF data issues several calls, so these are not one per batch; attributing every
start to its sender is milestone M1 in docs/NATIVE_RENDERER_SUCCESS.md.

## Every VU1 program start by sender (milestone M1)

Method (`EOD_RENDER_TRACE_EVERY=<n>`, src/render_trace.cpp): every packet the EE reserves through
the packet managers (`0x366C88`/`0x366D30` on `*(0x3E5978)`, `0x366D78`/`0x366DD8` on
`*(0x3E597C)`) is tagged with the reserving call's return address and the emitter it ran under.
When VIF1 starts a VU1 program (MSCAL/MSCALF/MSCNT), the DMA tag VIF1 is reading
(`g_vif1_dma_tag`, runtime/hw/hw.cpp) is looked up in those tags. Model data a DMA tag REFs is
attributed through the tag, so one model drawn normally and as a shadow counts under each sender.
`[rtrace] vb N vu1 starts T attributed A unattributed U` and one `vu1 sender` line per packet
builder report each sample; unattributed tags are listed by address.

Result (headless, Original mode):

| scene | VU1 starts per sample | unattributed |
| --- | --- | --- |
| Afton Jump Race, vblank 600 and 1200 | about 6,260 | 0 |
| career free roam, vblank 900-1800 | 7,461-7,756 | 0 |
| dealer, vblank 339-1800 | 2,569-7,596 | 0 |
| 2-player split screen, Dennison (scripted pads), vblank 4802-6301 | 4,077-7,148 | 0 |

Packet builders (the reserving function) and the game-level code that calls them, race vblank 1200:

| builder (reserve ra) | caller chain | VU1 programs (starts) | what it is |
| --- | --- | --- | --- |
| `0x3613C0` (`0x3613E0`) | queue flush `0x34AF98` -> `0x35CB88` | `0x02A8` 553, `0x02E0` 363, `0x0D48` 1269, `0x15A8` 88, `0x24B0` 1106, `0x1F90` 203, `0x0270` 16, `0x0318` 14, `0x0920` 4 | queued model batches: cars, characters, props (the batch's own VIF data, REF'd) |
| `0x35BA10` (`0x35BA6C`, `0x35BAE8`) | `0x35CB88` and other emitters | `0x0000` 485+37, `0x24D8` 276+12+485+37 | per-batch matrix/material packet REFs and the setup programs |
| `0x35B3D0` (`0x35B428`) | viewport change | `0x24D8` 18 | camera packet |
| `0x35D170` (`0x35D1B4`, `0x35D204`) | shadows `0x34C208`/`0x34C4B0` -> `0x35D298` | `0x0000` 56, `0x24D8` 56, `0x0020` 56, `0x0790` 6 | projected shadows |
| `0x35DEE0` (`0x35DEFC`) | `0x214B68` (frame) -> `0x358BD0(pass 0..4)` | `0x0270` 349, `0x0618` 197, `0x0920` 119, `0x0350` 28, `0x0000`/`0x24D8` 4 | **track world renderer**, five passes around the queued frame `0x22D7F0`; set up by `0x3582B8` with track data `0x3D1B28`/`0x3D1B2C` |
| `0x35F278` (`0x35F824`) | texture layer begin `0x34F988` -> `0x3514F0` -> `0x360680`; also `0x35DAD0`, `0x3551C0` | `0x2538` 126 | texture manager (step 3) |
| `0x3620A8` (`0x362104`) | `0x35BA10`, `0x35DAD0`, `0x361318`, `0x361ED8`, `0x362E70`, `0x363B98`, `0x363D78`, `0x363F48`, `0x35D4C8` | `0x24D8` 83+90, `0x2538` 17+30, `0x25F0` 7+8 | shared state/program setup used by several emitters |
| `0x361468` (`0x3614CC`) | `0x35DF10` <- many game functions (`0x207150`, `0x219608`, `0x229B30`, `0x29A2B8`, `0x2EEC70`, `0x304D50`, `0x30E368`, `0x310760`, `0x325728`, `0x327610`, `0x32DB68`, ...) | `0x0270` 32 | immediate geometry API (begin) |
| `0x364D18` (`0x364DF0`) | `0x361708` <- the same callers as `0x35DF10` | `0x0270` 5 | immediate geometry API (vertices) |
| `0x361C30` (`0x361C48`) | `0x361ED8` <- `0x34EE70`, `0x34EEB8` (sphere-tested draws) | `0x0270` 1 | immediate sphere-tested draw |
| `0x363FF8` (`0x364034`) | `0x3639D0`, `0x363B98` <- `0x2F2020`, `0x2F2268`, `0x2F24B0`, `0x2F2AC8` | `0x08E0` 8 | particle systems |
| `0x364548` (`0x364584`) | `0x363DE8`, `0x363F48` <- `0x2F0FD0`, `0x2F8008`, `0x2F9BA0`, `0x307390`, `0x307800`, `0x314B60`, `0x328450` | `0x01B8` 10 | effect sprites |

So the native renderer needs these entry points, each with a Filament owner: the draw queue, the
track world renderer (`0x358BD0`), shadows (`0x35D298`), the immediate geometry API (`0x35DF10` /
`0x361708`, `0x361ED8`), particles (`0x3639D0`/`0x363B98`) and effect sprites
(`0x363DE8`/`0x363F48`), plus the camera, matrix, material and texture state they consume. HUD,
menus and fonts do not start VU1 programs; they are PATH2/PATH3 GS work and need their own count
(milestone M6).
