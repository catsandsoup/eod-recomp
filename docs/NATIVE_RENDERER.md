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
| R2 | track world, sky and water from the world renderer (done: section "Stage R2", evidence in docs/NATIVE_RENDERER_SUCCESS.md section 7) | world leaf counts equal; water visible in 1P and split screen (only Afton.trk has a water body) |
| R3 | skinned characters and car damage from bone palettes (Phase A: section "Stage R3") | posed vertices within 1/16 px of the GS |
| R4 | shadows, particles, sprites, flares, glare | effect counts equal; visual comparison |
| R5 | PS2 look matches GS replay (P3 thresholds) | comparison tool |
| R6 | Modern look: PBR, shadows, IBL, reflections, water; live toggle | P4, P5 |
| R7 | HUD, menus, fonts, movies, render targets native; GS and VU1 out of the frame path | S4 = 0 |
| R8 | 60 fps at RenderScale 6, max LOD, Modern look on the target box; floors on the minimum box | S1, S2 with Tracy and EOD_GPU_PROF |

## Stage R3: skinned characters and car dents (specification)

Written with the code (rule 6.2). Sources: PS2 Ghidra decompile and `tools/ee_dis.py` of
SLUS_209.10, the VU1 image `config/vu/vu1_fd0ce30ab0ec263c.bin` (`tools/vudis.py`). The native
renderer never reads VU1 memory: it keeps its own copy of every upload the readable EE C++ builds
and poses the vertices on the CPU (runtime/render/native_feed.cpp `posed_mesh`).

### Data path

| What | EE producer (readable C++, EOD_DIFF) | Packet | VU1 | Programs |
| --- | --- | --- | --- | --- |
| Driver / pedestrian palettes | 0x34C9F8 BonePaletteUpload -> 0x35DBD8 BonePaletteWrite (src/bone_pose.cpp); pose arrays from 0x20C800 (drivers) and 0x20B8D0 (pedestrians) | 0x35A810 UNPACK V4-32 + MSCAL 0x24D8 | qword 0x90 + 4*first, 3x4 per bone, 20 bones | 0x0318 (MSCAL 0x63), 0x24B0 |
| Car dent offsets | 0x18CA98 CarApplyCollisionDamage writes car+0x4B8 (src/car_damage.cpp); 0x2189C8 VisualCarSubmitModel -> 0x34CA18 -> 0x35DD28 DentOffsetWrite (src/bone_pose.cpp) | 0x35A810 UNPACK V3-32 + MSCAL 0x24D8 | qword 0x90 + bone (xyz) | 0x02E0 (MSCAL 0x5C), 0x0D48, 0x15A8 |
| Sun-facing quad | 0x35CBD8 (material packet `*(0x4D7C54)`) | V3-32 | 0xAC..0xAF | 0x350 (MSCAL 0x6A) |

Skin word: the program reads it at TOP+149+n-k (vertices in reverse order); the bone fields are VU1
qword bases, weights are lane - 32768 (float).
- Matrix (0x318/0x24B0): p' = wa (Ma p) + wb (Mb p), M = columns at qwords bone+144..147, w lane of
  the position = translation weight; normals turn with the 3x3.
- Offset (0x2E0/0xD48/0x15A8, V4-8 skin unpack): p' = p + wa D[a] + wb D[b], D = qword bone+143 xyz.
- Partial (0x2E0 with a V1-8 skin unpack, 21,817 vertices in the retail cars): the VIF repeats the
  byte in all four lanes (+ STROW), so bone a = bone b and both weights come from the row.
- Camera-facing (0x350): p' = p + wa C[a] + wb C[b], C = sun quad corners (bone 29..32).
The upload in effect for a draw is the one whose packet is the material `*(0x4D7C54)` when the
record is tagged; P2 (`EOD_NATIVE_SKIN_CHECK=1`) compares the bone qwords used against VU1 data
memory and the posed vertices against the GS (1/16 px).

### Car damage (0x18CA98, readable C++)

Called by 0x189CD0 after the collision response (f_0018CA98 keeps the invincibility gate,
src/game_patches.cpp). Contacts come through 0x17B918 ContactRecord (pool 0x3CE3F0, 0x24-byte
records, 16 per object at obj+0x190).
1. Nearest bone: free bones (M+0x20+0x34*i, word +0x30 = 0) whose rest + 1.8 x dent lies within
   1.5 (2.25 squared, 0x40100000) of the contact; 0x18C6B8 dents it along the impulse.
2. Spread: every bone behind the nearest bone's dent plane moves along -impulse by
   min(depth, behind) x min(0.2 / max(d^2, 0.1), 1), clamped to its dent box (bone +4..+0x18). The
   low byte of the dent z is the shade |offset| / length(+0x28) x 128, kept in 0x20..0x80.
3. Panels: 8 per car (table 0x44AE68, 0x40 bytes; states car+0x13E8, 0x18 bytes). Past the
   panel's distance (+0x30) a panel hangs loose (2) or comes off (3, 0x1E0DC8 with its box centre).
4. Damage: impulse > 300 adds min(0.35 car / 0.1 other, |impulse| x 6.6666666e-06) to car+0x1BEC;
   states 2/3 past 0.6 (or 0.5 with a hit > 0.03), wrecked 10/11/12 past 1.0 with a wheel
   tossed (0x1B3068) on the side of the hit; 0.9 cap in free roam (0x1741C0).

### Car setup (0x18AFE0 CarSetupDamageAndPanels, readable C++, Phase A2)

Called once per car by 0x185470 (jal at 0x186174); f_0018AFE0 (src/game_patches.cpp) keeps the
CarDurability damage_scale patch after it.
1. memset(car+0x13E8, 0, 0xC0) (0x3A5FD0), then for each of the 8 panels of 0x44AE68 (0x40 bytes):
   0x205A58(car+0x3D0, name = entry+4, &state[0]) finds the helper node (state = car+0x13E8 +
   0x18*i). Missing: DebugPrintf 0x3F9FD0 ("car: can't find helper %s"), state stays 0.
2. Helper point p = R v + T: v = entry+0x24..0x2C, R = node[0..8] (x = v0 m0 + v1 m3 + v2 m6,
   mula/madda/madd), T = node+0x24..0x2C. state[1] = the bone 1..n-1 (n = M+0x1C) whose rest
   position (bone+0x1C) is nearest p (sqrt.s distance, strict <, start 1e7 = 0x4B189680, 0 = none).
3. Node origin o = R (0,0,0) + T (the EE does the 0 x m products, so -0/NaN follow the matrix);
   state[2] = the bone nearest o, same rule.
4. car+0x4B4 = M+0x1C; car+0x4B0 = (*M)->vtable[0x28](1.0, "damage_tweak" 0x3FA018) x 1818.0
   (0x44E34000) / (mass par+8 x 0.4545 0x3EE8B439); then x upgrades car+0x2EC when it is >= -1e-4
   (0xB8D1B717), else |car+0x2EC|. Human (car+0x1E0 = 0x50): x player_damage_tweak (0x3FA028,
   through 0x176768 / 0x125388). Human, or 0x56 with 0x1DB330(car+0x1DC) != 0: game rule 0x3CE4F0
   0 -> 0, 1 -> x0.5 (0x3F000000).
5. Matrix: car+0x300 ? 0x2DD5B8(car+0x300, car+0x14A8) : 0x2DD448(car+0x14A8); then 0x2AC bytes
   car+0x14A8 -> car+0x1754 ($v0 = the dword at car+0x1738, as the copy loop leaves it).

### 0x1DF9A0 (not a function)

config/symbols.txt names 0x1DF9A0 DeformResetMeshVertices; it is an address inside 0x1DF7A8, whose
strings are "+collide_test", "mass_list", "edge_list", "index_list" (0x400B48..0x400B88): a VU0
mass-spring debug test (256 masses, its own VIF0 DMA kicks at 0x10008030 and its own render loop).
Its only caller is 0x1E0558, which nothing calls or points to (no jal/j, no data word in the ELF).
Dead in retail: no EE draw or dent comes from it, so it is not decompiled (0 calls by
construction; the dent reset the renderer needs is the car+0x4B8 clear of the respawn path).

### Debris phob pools (0x484E68 wheels, 0x494D60 panels)

Owners found from the data xrefs (RefsTo) of 0x484E60..0x484EB4 and 0x494D58..0x494DAC; nothing
else reads the arrays.
- Wheels ('WHLP', factory 0x179158 -> ctor 0x1B3028, vtable 0x3FCE48): Init 0x1B2868 appends the
  phob (array 0x484E68, count 0x484EA8); 0x1B3068 DetachedWheelSpawn takes the ring slot 0x484EAC
  whether or not it is still flying (0x1B2D58 launches it, +0x1DC = race clock 0x3CDACC, +0x1E0
  car, +0x1E4 wheel, the hub radius car+0x1A00+0x54 w+0x38 to the model at +0x10); 0x1B2DD8 (vtable
  +0x10) clears +0x1DC and the ring index; 0x1B3180 / 0x1B3190 zero the count (and index) at scene
  teardown (0x185378 / 0x185320).
- Panels ('Panl', ctor 0x1E1E88, vtable 0x400EE0): the ctor allocates 0x264 bytes, appends (array
  0x494D60, count 0x494DA0) and sets +0x200 = 0x1E6C10(4, 6, 2); dtor 0x1E29C0 decrements the count.
  0x1E0DC8 (from 0x18CA98 at 0x18D1C8) takes the first phob with +0x1DC = 0 (car), else the one
  with the oldest +0x204 (spawn clock).
- Both kinds are scene objects, created by 0x2100C0 through the network create stream (0x1758B8 ->
  0x1783E0: factory, 0x17AEC0 id, vtable+8 Init, 0x178378 into phob_list 0x447404, which is 0x800
  bytes = 512 entries). The arrays are 16 words; the scene fills them: 12 'Panl' (4 x 3 creates,
  0x210444..0x2105A4; measured 12 in Anoka Battle), the wheel ring from its own loop at 0x21014C.

Phase A2: 0x1B3068, 0x1E1E88, 0x1E29C0, 0x1B3180, 0x1B3190 as readable C++ (src/debris_pool.cpp,
EOD_DIFF); 0x1E0DC8 and 0x1B2868 wrapped (bodies recompiled). The scene makes fewer phobs than the
16-word arrays: 12 'Panl' (0x210444..0x2105A4) and 5 'WHLP' (generators at 0x2100C0, measured 5),
so the real PS2 cap is the scene count.

Original behaviour (PS2, and `ExpandDebris=0`, and every online event): the wheel ring 0x484EAC
takes its next slot even when that wheel still flies; the panel spawn takes the first phob with
+0x1DC = 0, else the oldest +0x204. A taken phob is relaunched: the earlier piece vanishes.

Expanded behaviour (`ExpandDebris=1`, default, offline only; `netplay_hooks::race_rules() >= 0`
forces the original so all peers simulate the same): the guest array stays a 16-word window that
the recompiled readers see. Before 0x1B3068 / 0x1E0DC8 run, when the slot they would take holds a
live phob (wheel: the ring slot; panel: window full, slot 0 is used), the wrapper moves that live
phob to a host reserve (`std::vector`) and puts a free phob in the slot: a reserve phob whose
+0x1DC is 0, else a new phob made exactly as the scene makes them (0x1783E0): factory (wheel
0x179158, panel 0x1E1E88), Init through vtable+8 (0x1B2868 with the scene's captured 'WHLP' create
record / 0x1E28B8 with {'Panl', 8} from 0x2100C0), then 0x178378 into phob_list. The factory and
Init append at the guest count, so the count is parked on slot 0 and restored; MemAlloc's lock
panic (0x12E870: 0x3CC458 = 0 and 0x3CC460 != 0) is lifted by 0x3CC458 = 1 for that call only. The
spawn itself then runs unchanged (and is the code EOD_DIFF checks). Nothing in flight or on the
ground is taken while phob_list (MemAlloc(0x800) at 0x1769A8 = 512 entries, count 0x447408) has
room; past that the original steal applies and is counted. Release: reserve phobs that come free
are reused first; the scene's teardown destroys every phob in phob_list (the STAK heap 0x12DF20 pops
freed blocks in any order), the panel dtor wrapper gives back the decrement of a phob it made (so
0x494DA0 returns to 0 as on the PS2), and the host lists are dropped at 0x1B3180 / 0x1B3190 and when
the panel count reaches 0. Made phobs keep the PhobConstruct id (no create-stream id, 0x17AEC0).

### Dent loop proof (0x18CA98)

`[dent]` counters in src/car_damage.cpp (replacement side only): calls, calls past
CarCanTakeDamage 0x18AAD0, bones visited by the nearest-bone search, hits with a nearest bone
(0x18C6B8 called) and bones moved by the spread; printed at 1, 2, 4, ... and every 1024 calls.

### Functions replaced (config/hle.txt `wrap`, `diff_run`)

0x34C9F8, 0x35DBD8, 0x35A810, 0x35DD28, 0x34CA18 (src/bone_pose.cpp); 0x18CA98, 0x17B918,
0x18AFE0 (src/car_damage.cpp); 0x1B3068, 0x1E1E88, 0x1E29C0, 0x1B3180, 0x1B3190
(src/debris_pool.cpp).

### PS2 limits (docs/PS2_LIMITS.md)

Contacts per object (16) and the contact pool are tracked by `limit_note`; removing them needs the
readers of obj+0x190 decompiled. Wheel debris (16, 0x484E68) and panel debris (16, 0x494D60) are
removed: grow on demand with ExpandDebris (above), `[limit]` with capacity 0.

### Not done in R3 Phase A (open)

- 0x1E0DC8 body (panel launch) and 0x1B2868 (wheel Init) are still recompiled (wrapped).
- Phase B: multi-scene suite (P2 within 1/16 px on every posed vertex, crashes, rollovers, 8
  panels, 16 wheels), split screen, docs/NATIVE_RENDERER_SUCCESS.md.

## Stage R2: track world, sky and water (specification)

Written before the code (rule 6.2). Sources: PS2 Ghidra decompile and `tools/ee_dis.py` of
SLUS_209.10, the VU1 image `config/vu/vu1_fd0ce30ab0ec263c.bin` (`tools/vudis.py`), and the Xbox
`default.xbe` Ghidra project (`FUN_000a8390` = `0x214B68`, `FUN_00138840` = `0x358BD0`,
`FUN_00138280` = `0x3582B8`). Readable C++: `src/native_world.cpp`; native side:
`runtime/render/native_feed.cpp`.

### Functions replaced (config/hle.txt `wrap`, `diff_run`)

| PS2 | name (this project) | Xbox | what it does |
| --- | --- | --- | --- |
| `0x214B68` | WorldFrameRender | `0x0A8390` | per view: `0x3D1B30 = 0x212DD0()`, sun quad `0x34C1F0(0x2217A8())`, mlist calc `0x3582B8(f12 *(0x3D1B28), f13 *(0x3D1B2C), mlist, *(0x3D1B30), 0, 0)` with mlist `*(*(0x3D1C18)+4)` (1 view) or `+8` (split, `*(0x3D1B0C) != 0`); pass 0; if not split: flags `0x34DFE0(8)`, `0x34DEC0(4)`, near/far x `1.1` (`0x3F8CCCCD`, `0x34E4D8`), pass 1, `0x32E638`, `0x34E518`, `0x34DEC0(8)`, pass 2, effects `0x2E7300(0)`; queued frame `0x22D7F0`; sun quad again; pass 3; if not split: shadows `0x30A580(0)`, `0x2283D0`, pass 4; `0x359238` (mlist = 0) |
| `0x3582B8` | MlistVisibilityCalc | `0x138280` | uploads VU0 microcode (VIF0 DMA from `0x3BD4D0`), matrix table `0x70001C00[0..255]` (identity packet `0x35CD28`), camera rows `0x4D2E90..0x4D2EC0` from `0x34FC68` (`0x4CD2F8`), VU0 frustum constants vf20..vf26, then per mlist sphere / group / model a visibility byte in the scratchpad (`[0] = 0x81`, spheres from 1, then groups, then models): VU0 `vcallms 0x40` clip flags `& 0xBD0` -> 0, `& 0x42F` -> `0x81` (partly outside), else 1; groups through `0x357E60` (LOD); model depth to `0x3DE6E8[i]`; then each mlist object's vtable `+0xC` |
| `0x358BD0` | WorldRenderPass(pass) | `0x138840` | identity matrix packet into `0x70001C00[0]`; for texture records `[first,last)` of pass (`mlist + 0x38 + 16*pass`), batches whose scratchpad byte `& 0x7F` equals the batch's required value: texture `0x34F8D0(slot)`, packet `0x35DEE0(2*n)`, per batch DMA REF to the matrix packet when it changes and a DMA REF to the part's VIF data, then `0x35CBB8(qwords)`; then the pass's display objects (vtable `+0x14`) |
| `0x35DEE0` | WorldPacketBegin(qwc) | (inline) | `0x35BA10` (camera/matrix/material REFs), `0x366D30(qwc)` |
| `0x35CBB8` | PacketCommit(qwc) | (inline) | jump to `0x366CD0`: commit `qwc` qwords of the open packet; 0 when the buffer is full |
| `0x223858` | SkyDraw (vtable `0x408350 + 0x18`) | | sky dome at the camera (`0x4CD2F8 + 0x24`); type 1 (`+8`): part k (32 parts, centre/radius at `+0x2C + 16k`) visible when `cos(r + 0.625 fov) < dir . centre` (`0x39BB28` = cosf, fov `0x2111D0(-1)`), else all 32; `0x34BDA8(model *(+0x3C)+4, matrix +0xC, -1, mask)` |
| `0x223800` | SkyDestroy (vtable `0x408318 + 8`) | | vtable `0x408318`, colour reset `0x34E8A8(0xFFFFFFFF)`, base `0x223D00` |
| `0x326168` | WaterDrawIfPresent | | `0x325728(*(0x3DC134))` when the track has water |
| `0x325728` | WaterDraw(surface) | | when `surface+0x38` (visible, set by display object `0x223E00` through `0x359198`): texture `0x34F8D0(+0x3C)`, immediate begin `0x35DF10(0x3000)`, per tile (list `+0x40`, next `+0x1B8`) inside bounds `0x423788..94`: 4 vertices `0x326B80(tile, v, 0|8, 0|8)`, colour/uv `0x31E7E0(tile, v, 0x4AEFA8)`, `0x361708(xyz, st, colour, ADC = k < 2)`; else `0x324928(tile)`; `0x361BC8` |
| `0x325E40` | WaterSimulate | | per car (`0x2E87B0`, `0x2E8768`) faster than `2.2222223` (`0x400E38E4`): splash strength `(v - 2.2222223) * 0.09`, find the tile (`0x31BFA0`, `0x31C7E8`), when visible push `0x31CA30(-min(1, s))`; then `0x325AD0` and every tile `0x31C7E0` |

### Structures

| address / record | layout |
| --- | --- |
| mlist (`*(0x4D6F10)` while drawing) | `+0x10` n spheres, `+0x14` spheres (16 B: centre, radius), `+0x18` n groups, `+0x1C` groups (32 B), `+0x20` n models, `+0x24` models (32 B: sphere, `+0x10` u16 parent, `+0x12` u16 required), `+0x28` n batches, `+0x2C` batches, `+0x30` n texture records, `+0x34` texture records, `+0x38 + 16*pass` {first, last texture record, first, last object}, `+0x88` n objects, `+0x90` objects |
| texture record (0x1C B) | `+0x10` texture slot, `+0x14` first batch, `+0x18` end batch |
| world batch (12 B) | u16 scratchpad index, u16 required value, u16 matrix index (`0x70001C00[i]`), u16 qwc, part pointer (`4SPS`, VIF at `+0x38`) |
| scratchpad byte | bit 7: partly outside the frustum (VU1 clip path, ITOP `0x280`), low 7 bits compared with the batch's required value |
| world packet qwords (`0x358BD0`) | matrix: `{0x30000000 | *(u16*)0x429A98, matrix packet & 0x0FFFFFFF}`, `0x01000101` STCYCL 1,1, `0x05000000` STMOD 0; batch: `{0x30000000 | qwc, (part + 0x38) & 0x7FFFFFFF}`, `0x04000080` / `0x04000280` ITOP, `0x05000000` STMOD 0 |
| sun quad (`0x34C1F0` -> `0x35CBD8`) | material packet `*(0x4D7C54)`: `0x35A810(p, 4, 0x1D, 3, 4)` = V3-32 x4 to VU1 qwords `0xAC..0xAF` = `+10 r`, `-10 r`, `+10 u`, `-10 u` (r, u from `0x2217A8`: camera right / up), z bits `& 0xFFFFFF00 | 0x80` |
| water surface (`*(0x3DC134)`) | `+0x24` position, `+0x38` visible, `+0x3C` texture slot, `+0x40` tile list, `+0x50` lists; tile `+0x160` axis A, `+0x178` axis B, `+0x184` centre, `+0x190`/`+0x194` sizes, `+0x1A4..0x1B0` bounds, `+0x1B8` next |

### VU1 programs (what the native renderer does instead)

| program | entry (VU1) | read in the microcode | native |
| --- | --- | --- | --- |
| `0x0270` (MSCAL `0x4E`) | clip `0x388` (ITOP & `0x200`), else `0x640` | colour qword = V4-5 colour, `mfir.w vf18, vi1` = ITOP (alpha), `itof0.w` ADC; no lighting, `maxw.z` q = 1 | prelit colour, alpha = ITOP low byte (`0x80`, from the EE packet) |
| `0x0618`, `0x0920` | continuation entries (MSCNT) into `0x388` / `0x650` of `0x0270` | same | same as `0x0270` |
| `0x0350` (MSCAL `0x6A`) | `0x10D0` (non-clip), `0x15D0` (clip) | skin word at TOP+149+n-k: `p + wa C[a] + wb C[b]` with `C` = qword `143 + bone`, weights `lane - 32768`; lit `min(TOPS+5 + max(0, n.vf12) TOPS+4, 128)`; alpha `mfir.w` of `C[a].z` low 16 bits | camera-facing quads: `C` = the sun quad (`0xAC..0xAF` = bone 29..32) read from the EE material packet; expanded on the CPU per frame |
| water (MSCAL `0x4E`) | `0x0270` through the immediate API (`0x361468` ITOP `0x200`, `0x361708`, `0x364D18`) | positions/st given by `0x361708`, colour `(c >> 1)` rounded, R/B swapped | the same vertices from the replaced `0x325728`, strip per tile (ADC on the first 2) |

`track.grf` geometry is decoded with `fix_winding = false` (raw strip parity) and drawn with no
culling (the VU1 programs have no backface test). Textures, blend, alpha test and depth state
come from the GS state the texture manager (`0x34F8D0`) sets for the leaf, as in R1.

### Split screen (P8)

PS2 and Xbox both skip passes 1, 2 and 4 (and their water display objects, effects and
projected shadows) and use the coarse mlist (`+8`) when `*(0x3D1B0C) != 0`. With
`[Video] SplitScreenParity=1` (default; `EOD_SPLIT_PARITY=0` gives the original) the readable
`0x214B68` runs the single-view sequence in every split view. `EOD_DIFF` equivalence is checked
with parity off.

`EOD_NATIVE_ONLY=1` (implies `EOD_NATIVE=1`) drops every primitive the GS emulator would draw, so a
path not yet native shows as missing geometry. `EOD_NATIVE_WORLD_LIST=1` prints each mlist's
pass objects and texture names; `EOD_NATIVE_WATER_LOG=1` the water surface state;
`EOD_NATIVE_PARITY_LOG=1` every frame whose world/water counts differ.

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
