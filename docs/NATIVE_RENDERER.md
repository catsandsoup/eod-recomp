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

### Phase A3: coverage of the wheel paths, teardown, camera views, night

**What makes a wheel come off.** Only 0x18CA98 calls 0x1B3068 (0x18D680, `toss_wheel` in
src/car_damage.cpp): a car in damage state 1..4 whose damage (car+0x1BEC) passes 1.0 on a hit with
|impulse| > 300 is wrecked (state 10/11/12 from rng(3)), and with rng(4) = 0 (one wreck in four) the
wheel on the side of the hit (`hit_side` in src/car_damage.cpp) comes off unless its flag (wheel+0x50 & 0x80) is
set already. Free roam caps damage at 0.9 (0x1741C0(4)), so no wheel comes off there; the one-hit
cheat (0x101) sets 1.0 without this path. A wheel stays live (+0x1DC = spawn clock) until its vtable
+0x10 (0x1B2DD8) parks it at y = -500; nothing in a race calls that, so every tossed wheel stays live
until the scene ends.

**Replay tools (test only).**
- `EOD_DIFF_REPLAY=<hex addr>:<n>,...` (runtime/core/diff_harness.cpp): each checked call of those
  functions is checked n more times from the same recorded entry state (registers, RAM, scratchpad,
  VU0); counted as "replayed". The game keeps the original's results of the live run.
- `EOD_WHEEL_REPLAY=<n>` (src/debris_pool.cpp): after each real wheel toss, the same toss (car,
  matrix, velocity, hub position the caller built) runs n more times on the car's wheels in turn,
  through the wrapper (expansion and EOD_DIFF), so the pool goes past 16.
- 0x18CA98 calls 0x1B3068 and 0x1E0DC8 inside its own comparison; nested calls are not checked, and
  the wrappers' host bookkeeping on the original side is now undone (`HostUndo`, using
  `diff_in_original_side()`), so a run with 0x18CA98 under EOD_DIFF no longer counts a reserve or a
  made phob twice. 0x1B3068 is therefore checked in runs without 0x18CA98 in EOD_DIFF.
- `diff_report()` prints the [diff] totals at app_exit (it leaves through `_exit`, so atexit never ran).

**Measured (headless, muted, EOD_NATIVE=1, Detail=maximum, DrawDistance=8).**

| run | event | vehicle | result |
| --- | --- | --- | --- |
| r3a3_night | Gilroy 'Battle (8 AI)' (night, fog 0 0 0), chickens | Ambulance | 5 real wheel tosses + 100 replayed: wheel pool live peak **105** (scene phobs 5, PS2 array 16), 100 made, 0 live wheels taken; panels peak 34, 22 made, 0 taken |
| r3a3_ps2pool | Dundas 'Battle (8 AI)', `ExpandDebris=0` | Taxi | 2 real tosses + 40 replayed: at most 5 live wheels (the scene's 5 'WHLP'; demand 6), 37 live wheels taken by the ring, as on the PS2 |
| r3a3_derby | Eagan 'Demolition Derby' | Copcar | player wrecked, results, a second round: real teardown (0x1B3180 1, 0x1E29C0 12, 0x1B3190 2) |
| r3a3_views | Mankato 'Demolition Derby' | Postal | camera views (below), 0x18CA98 live calls |

EOD_DIFF totals (0 mismatches in every run):

| function | r3a3_night | r3a3_ps2pool | r3a3_derby | r3a3_views | checked in all |
| --- | --- | --- | --- | --- | --- |
| 0x18CA98 CarApplyCollisionDamage | not watched | not watched | >= 512 live (log at powers of two) | 862 live | >= 1374 live, 0 mismatched |
| 0x18AFE0 CarSetupDamageAndPanels | 9 live, >= 1024 checked (replay 120) | | 16 live | 28 live | >= 1068, 0 mismatched |
| 0x1E1E88 'Panl' ctor | 23 live (scene and made phobs), >= 2048 checked (replay 90) | | 16 live | 24 live | >= 2088, 0 mismatched |
| 0x1E29C0 'Panl' dtor | | | 12 live, 1092 checked (replay 90) | 20 live | 1112, 0 mismatched |
| 0x1B3190 wheel pool reset (scene) | 1 live, 1001 checked (replay 1000) | | 2 live, 2002 checked | 2 live | 3005, 0 mismatched |
| 0x1B3180 wheel count reset (teardown) | | | 1 live, 1001 checked (replay 1000) | 1 live | 1002, 0 mismatched |
| 0x1B3068 DetachedWheelSpawn | 105 spawns (5 real + 100 wheel replay), >= 5120 checked (replay 48) | 42 spawns (2 real + 40 wheel replay), >= 32 logged | nested in 0x18CA98 | nested | >= 5152, 0 mismatched |

The night and ps2pool runs predate `diff_report()`: their totals are the last power-of-two line.
Every run printed 0 mismatched.

**Camera views.** View-change input: the human driver's "view" input (binding name 0x3F97F0, pad
Triangle in the default layout, not Circle) raises pad event 0 (0x184280: 0x1AA070 edge of the input
at controller+0x248 -> 0x179608 writes event word 0x4473D8 + 8p); 0x177400 copies it into the frame
record, 0x1136C0 reads it (0x211258) and, unless an override is pending (0x3C4118 + 0x1C p != 0x20),
calls 0x1151B0(p, 0, 0x3C411C + 0x1C p), which steps through 0x3EDC30 = {1, 0, 4, 5, 2} (a view not in
the table goes to 5). 0x113D90 applies 0x3C411C each frame through 0x1D41B0 (requested view at
0x48CB90 + 0x1B8 p); 0x1C31A0 copies it to +0xC, the view in effect, and 0x1D10C0 switches on it.
The race start view is the setting "race_camera%d" (0x115760: 0..3 and 7..9 kept, 4..6 or >9 become
4, or 5 with flags 0x8800). FarChase (6), Rear (3), RearChase (7), Overhead (8) and TV (9) are not in
the pad cycle: the game reaches them only through 0x1151B0's development-keyboard keys (0x374..0x37A,
0x170..0x173; no retail code queues them) and the replay viewers (0x10FE90 / 0x10FE6C cycle 0..9,
0x10C8D8 / 0x374698 the TV cameras 9..13). `EOD_CAMERA_KEYS` queues those keys through the game's own
KeyPush 0x33C168, so the game's own handler sets the view. `EOD_CAMERA_TRACE` logs every change of
the view in effect by the game's names (0x108D28 / 0x109288 "CameraNames:*").

r3a3_views (Mankato 'Demolition Derby', Postal), `EOD_CAMERA_TRACE` lines, view in effect = 0x48CB8C:

| vblank | view (value) | how |
| --- | --- | --- |
| 384 | NearChase (4) | race start: race_camera0 default 4 (0x115760) |
| 902 | FarChase (6) | key 0x376 (0x1151B0) |
| 1103 | Rear (3) | key 0x374 |
| 1316 | RearChase (7) | key 0x377 |
| 1512 | Overhead (8) | key 0x378 |
| 1700 | TV (9), the trackside cinematic cameras (0x1CC380), also the replay viewers' start view | key 0x37A |
| 2103 | Chase (5) | Triangle (pad event 0): 9 is not in 0x3EDC30, so 5 |
| 2319 | Chassis (2), the under-car suspension / wheel view | Triangle |
| 2501 | Bumper (1) | Triangle |
| 2703 | Cockpit (0) | Triangle |
| 2911 | NearChase (4) | Triangle |
| 3257 | physcam type 30 (0x1E, 0x1CAB50) | the player's car wrecked (event over) |
| 5670 | physcam type 19 (0x13) | results screen frame (0x36E568 sets 0x13) |
| 5979 | NearChase (4) | second round (race_camera0) |

The same sequence ran in r3a3_derby (Triangle cycle) and r3a3_cockpit (all views). The replay
viewers (0x10C8D8 after an event, 0x10D338 from the menu) were not opened: their views are the
same physcam types (0..9 and the TV cameras 9..13).

**Interior, bumper and chassis views on the native renderer** (r3a3_cockpit, Mankato derby, Bus,
`ModernRenderer=0`, `EOD_NATIVE_LOG=1`; r3a3_cockpit_tr adds `EOD_RENDER_TRACE` on one frame per view;
r3a3_cockpit_only runs `EOD_NATIVE_ONLY=1`):

| view | frames | queue records submitted / drawn natively (P1 frames equal) | records tagged | 0x218E68 car batch: Low0 cars, records | extra draw-queue callers in a traced frame | native-only dropped GS prims (per frame) |
| --- | --- | --- | --- | --- | --- | --- |
| Chase (5) | 89 | 11362 / 11362 (89/89) | 11543 of 11543 | 0, 7929 | (reference) | 157120 (1181) |
| Chassis (2) | 81 | 3380 / 3380 (81/81) | 3376 of 3376 | 0, 1632 | ra 0x21D684 x2 (0x21D1C0 via 0x34BF78), ra 0x21DECC x1 (0x21DC18, from 0x21A378): suspension and wheel assembly | 97584 (781) |
| Bumper (1) | 49 | 1744 / 1744 (49/49) | 1762 of 1762 | 0, 0 (own car not drawn) | none | 72351 (872) |
| Cockpit (0) | 43 | 2442 / 2442 (43/43) | 2439 of 2439 | 0, 0 | ra 0x21A248 and ra 0x21A270 (0x219C48, from 0x219560): the cockpit model car+0x228 (0x34BDA8 flags 5) and, when 0x3D1C84 != 0, the posed gauge/steering model (0x34CA18 bones then 0x34BDA8) | 40989 (932) |

So the interior and the exposed suspension / wheel assembly go through the draw queue (0x34BDA8)
and are drawn natively (every queue record of every frame in these views drawn, all tagged). The
cockpit model is not a Low0 LOD selection: 0x218E68 picks no Low0 (+0x54C = 0) in any view (Detail
maximum keeps the loader minimum, 1), and in Cockpit and Bumper it submits nothing for the player's
car. The GS primitives that `EOD_NATIVE_ONLY=1` drops are in every view (Chase drops more than
Cockpit), so they are the 2D/effects paths still drawn by the emulator, not the interior; which
paths they are is R4 work. 0x219C48 has one non-queue draw: when 0x2114A8() != -1 it builds a quad on
the stack and sends it through 0x35CAA0 (between 0x34F928 / 0x34F888), outside the draw queue: open
(below).


**Night events (data).** The Eve records (COMMON.RES `*_eves.val`, field `time`) put Anoka (17:00 to
18:00), Baxter, Gilroy, Hampton (Bemidji), Hastings and Kenyon (17:00 to 18:30) in the evening; those
runs measure fog 0 0 0 (`[modern] ... fog`), the day Eves (Dundas 10:00 to 12:30, Dennison, Eagan,
Mankato, Woodly) 0.35 to 0.79. Gilroy carries the stadium light textures gglights/light/light2/
redlight (GILROY.TRK). Night run: Gilroy 'Battle (8 AI)', Ambulance.

### Not done in R3 Phase A (open)

- 0x219C48 (cockpit draw, from 0x219560): the stack quad through 0x35CAA0 when 0x2114A8() != -1 is
  not a draw-queue record; decompile 0x219C48 / 0x35CAA0 and feed it to plume (R3/R4).
- Name the GS primitives `EOD_NATIVE_ONLY=1` still drops in a race (about 780 to 1180 per frame in
  every view) and move each producer to plume (R4).
- 18CA98 reached >= 1000 live checks over two runs (512+, 862), not in one run.
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

---

## Stage R5: Classic parity (Phase A1: P3 tool and fog; Phase A2: texture LOD, alpha test, blend)

Phase A1 builds the P3 comparison tool and moves fog onto Plume from decompiled EE code. Mipmaps,
region clamp/repeat, destination-alpha blend and AFAIL modes follow in Phase A2.

### Fog: what both binaries do (spec before code)

| item | PS2 `SLUS_209.10` | Xbox `default.xbe` (matched) | meaning |
| --- | --- | --- | --- |
| fog range setter | `GsFogWriteTables 0x3653F8`: `*0x4D8B84 = f12` (start), `*0x4D8B88 = f13` (end), `*0x4D8B8C = f14`, `*0x4D8B74 \|= 0x800000` | `0x2ACA7C/80/84` stores, `*0x2ACA88 \|= 0x4000` (`0x9E230`, `0x9E130`, `0x132760`) | same three floats, a dirty bit |
| `FogConfigureRange 0x33ECB8` | jump thunk to `0x3653F8` (`gen/rec_095.c` `f_0033ECB8`) | inlined | |
| per-view range | `CameraBuildViewportProjection 0x212B58` (call at `0x212C34`): start = `far / views * 0.1`, end = `far / views * 2`, `0.5`; far = `g_DrawDistanceFar 0x495D58`, views clamped 1..4; second call `0x2129D0` (`0x212AAC`) | `0x9E230`: start = `far / views * 0.1`, end = `far + far`, `0.5` | identical formula |
| boot / screen values | `0x33EB30`: `(0, 1.0, 0.7)` and fog colour `0x34E848(0xFF00FF00)`; `0x360E18` init: colour `0xFF`, `200.0`, `1000.0`, `0.5` | `0x132760` / `0x9E130`: colour `0xFF00`, `0`, `1.0`, `0.7`; `0x13D620`: `100.0`, `1000.0`, `0.5`; `0x131560`: colour `0xC9C9EB` | |
| fog colour (state `0x4D8B80`) | `0x365428(r, g, b)`: `rgb = r & 0xFF \| (g & 0xFF) << 8 \| (b & 0xFF) << 16`; if changed, store and `*0x4D8B74 \|= 0x10000`; `0x3620A8` (`0x3623CC`) emits it as GS `FOGCOL` (A+D reg `0x3D`) | `0x2ACA78` -> `D3DRS_FOGCOLOR` (`0x13D320` state `0x8A`) | GS `FOGCOL` |
| readers of the range | **none**: `RefsTo 0x4D8B70..0x4D8BC8` finds only the writes at `0x3653F8` and `0x360E18`; no code tests `0x800000` (`0x3620A8` drops it with `*0x4D8B74 = 0` at `0x362930`) | `0x13DA30` `AND EBX, 0xFFFFA5F9` (`0x13DA4D`) clears `0x4000`, so the `FOGSTART/END/DENSITY` block at `0x13DD5E` (`0x13D320` states `0x5E/0x5F/0x60`) is unreachable | the range is dead state in both |
| fog enable | GS `PRIM.FGE` = bit 3 of the render flags `*0x4D8BC8` (`0x3620A8`: `(flags & 8) << 2` into both GIFtag templates); flags come from `*0x4CD398` via `0x34F028` -> `0x365540` (bit 3 cleared when `*0x4CD3A0 == 1`) | `D3DRS_FOGENABLE` (`0x1DAE18`, state `0x5C`) = `(*0x2ACA5C >> 3) & 1` (`0x13DD01`) | same bit |
| who sets that bit | only `RenderStateEnable(2) 0x34DEC0` (`\|= 8`); no call site passes 2. `RenderStateDisable(2) 0x34DFE0` is called at `0x2105F8`, `0x214C98`, `0x304D50`; init `0x34F0C0` sets `0x630` (bit 3 clear); `0x34F7C8` only touches `0x10`/`0x20` (`0x351410`) | game code never writes `0x1DAE18`; `0x13D620` sets `D3DRS_FOGTABLEMODE` (`0x1DAE1C`) = 3 (linear) only | fog never turns on |
| GS fog arithmetic | `C = FOGCOL + (((C - FOGCOL) * F + 128) >> 8)` per channel (`runtime/gs/gs_pixel_math.h:4`, verified against paraLLEl-GS, docs/RENDERER_REFERENCES.md) | D3D linear table fog `f = (end - d) / (end - start)` (`0x1D31F0`: bias `end / (end - start) + 1`, scale `-1 / (end - start)`), `C = f C + (1 - f) FOGCOL` | |
| per-vertex F | VU1 writes `XYZF2` with the w word built from integers (`mfir.w` at `0x05E0`, `0x06C0`, `0x0718`; `mul.w vf21, vf2, vf1` at `0x0770`): no distance maths and no read of `0x4D8B84..8C`. Measured in a Dundas replay: `F = 255` on 11,102 vertices, `0` on 6,789, other values on the rest | n/a (fog unit computes f from eye depth) | F is a by-product, not a fog factor |

Measured (GS replay of the live command stream, `EOD_GS_REPLAY_TRACE`): Gilroy 'Battle (8 AI)' night
vb 1200, `0` of `41,962` primitives with `FGE = 1`; Dundas 'Battle (8 AI)' day vb 1200, `0` of
`6,743`. Classic parity therefore means: no fog on any retail frame. The renderer still carries the
exact GS stage so a mod or a future path that sets the bit gets the GS result.

### Fog: native implementation

- `src/native_fog.cpp` (readable C++, `EOD_DIFF`): `0x33ECB8`, `0x3653F8`, `0x365428`, `0x365540`
  (render flags commit). Each replacement also hands its values to the native feed
  (`native::ee_fog_range`, `native::ee_fog_colour`, `native::ee_render_flags`); `0x360E18` is wrapped
  and publishes its init constants (`0xFF`, `200.0`, `1000.0`, `0.5`) after the original runs.
  Nothing is read back from GS registers.
- Every packet (`fill_camera`) carries `fge` (flags bit 3), `fogcol` and the range at queue time.
- `native_shaders.hlsl`: mode bit `M_FGE 0x800`; `mode.y` bits 8..31 = `FOGCOL`; `mode.z` bits
  8..15 = F. The pixel stage applies `fog + ((c - fog) * F + 128) >> 8` after texture function and
  before alpha test, as the GS does. Push constants stay 128 bytes.
- F per vertex: VU1's w word is not a fog coefficient (above), so there is no F to pass. A draw
  with `FGE` set is counted (`[native] fog: FGE draws`) and left unfogged; FOGCOL is put in
  `mode.y` only then, so retail push constants are bit-identical to R4. Measured: 0 such draws.

### P3 tool (`tools/p3_compare.py`, `tools/p3_run.sh`)

`tools/p3_run.sh NAME TRACK EVENT CAR` runs one headless, muted race with `EOD_NATIVE_ONLY=2` (new:
the GS still draws sprites, i.e. clears, HUD and the copy of the 3D buffer `FBP 0x1180` to the
display buffer, so the 3D on screen is Plume's alone), records the GS command stream at each vblank V
(`EOD_GS_DUMP_AT`, `EOD_GS_DUMP_N=6`), takes screenshots only near V+6 (`EOD_SCREENSHOT_AT`, new) and
cycles the camera with Triangle. `tools/p3_compare.py` replays each window in the software GS
(`--gs-replay`, `Renderer=software`) and reports, numbers only:
- per-channel MAE, p99 of the per-pixel max channel error, SSIM (luma, 8x8 windows, stride 4);
- fog-pixel error: pixels that change when the replay's FOGCOL is replaced
  (`EOD_GS_REPLAY_FOGCOL`, new, two colours);
- sky-pixel error: pixels whose final depth under the display is 0 (cleared), from
  `EOD_GS_REPLAY_ZSHOT` (new: the Z buffer through the same DISPFB/DISPLAY mapping as the picture).
The live frame is the best match in V+3..V+9. A window is skipped unless the replay drew at least
100,000 triangle pixels: the dump starts from VRAM as the live renderer left it, so a window
without a whole GS frame shows Plume's pixels again (first runs gave false MAE 0.00 this way). A slow
EE (EOD_DIFF on a function called 1,000 times a race, e.g. `0x365540`) causes exactly that, so the
EOD_DIFF smoke and the P3 run are separate runs.

### Phase A1 results (headless, RenderScale 1, Classic, `EOD_NATIVE_ONLY=2`)

EOD_DIFF (separate runs): Gilroy 'Battle (8 AI)' night, Hearse: `0x33ECB8` 7/7, `0x365428` 4/4,
`0x365540` 1,177/1,177 checked, 0 mismatched; Dundas 'Battle (8 AI)' day, Taxi: 8/8, 4/4,
1,011/1,011, 0 mismatched. `0x3653F8` is reached only through the thunk (its readable body runs
inside the `0x33ECB8` check). FGE draws 0 in every run.

| scene | vehicle | frames | MAE (worst) | p99 (worst) | SSIM (worst) | fog px | sky MAE (px) | views |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Gilroy 'Battle (8 AI)' night | Hearse (`502`) | 22 | 3.79 (15.37) | 30 (90) | 0.9338 (0.6475) | 0 | 3.29 (1,829,056) | Cockpit, Bumper, Chassis, NearChase, Chase |
| Dundas 'Battle (8 AI)' day | Taxi (`508`) | 20 | 6.61 (20.41) | 74 (146) | 0.9087 (0.7836) | 0 | 8.96 (1,115,348) | Cockpit, Bumper, Chassis, NearChase, Chase |

P3 fails as expected before mip levels, blend and the remaining A2 items. Fog contributes nothing:
no retail pixel is fogged.

### Phase A2: what the GS state of a native draw is (spec before code)

Census of the Gilroy night and Dundas day replays (`EOD_GS_CENSUS`, now with TEX1 L/K/LCM): every
3D triangle has TEX1 `MMAG 1, MMIN 4` (LINEAR_MIPMAP_NEAREST), `MXL 0..6`, `L 0`, `LCM 0` and a K that
changes from draw to draw; CLAMP `WMS/WMT` 0 or 1 only; TEST `ATE 0` or `ATE 1, GEQUAL, AREF 0x60,
AFAIL 1` (FB_ONLY); ALPHA `(Cs, Cd, As, Cd)` or `(Cs, 0, As, Cd)`; `DATE 0`; no ALPHA with C = Ad.
The EE code that sets each of these:

| GS field | PS2 `SLUS_209.10` | value |
| --- | --- | --- |
| TEX1 K template, MXL | `TextureSetLodState 0x3652A8(a0, a1)` (texture manager `0x35F278` at `0x35F8D0`, `0x35EE38` at `0x35EE94`; a0 = log2 height `+0x1A`, a1 = levels - 1 `+0x1C`) | `*0x4D8BAC = a0 << 4`, `*0x4D8BB0 = a0`, `*0x4D8BBC = a1`, dirty `0x400` on change |
| TEX1 register | `RenderStateCommit 0x3620A8`, dirty `0x400` | `(K & 0xFFF) << 32 \| 0x120 \| (MXL & 7) << 2` (LCM 0, MMAG 1, MMIN 4, L 0), UNPACK V4-32 x3 to VU1 qword 20 (`0x6C034014`: A+D GIFtag, qword 21 TEX1_1 kicked copy, qword 22 template), then `MSCAL 0x49B` (VU1 `0x24D8`: empty program) |
| per-batch K | VU1 batch programs (starts `0x0388`, `0x0D70`, `0x15D0`, `0x18F0`; kick sites `0x0400`, `0x08D0`, `0x0CF8`, `0x0DE8`, `0x1558`, `0x1648`, `0x1968`, `0x1F40`, `0x2460`) | `z = mtir vf13z` of batch qword 198 (EE VIF data, `vifmesh::Batch::q198_zw[0]`); `z <= 0`: qword 21.y = qword 22.y + z + 1000 (`iaddiu 0x3E8`, `iadd`, `isw.y 21`), `xgkick 20`; `z > 0`: no kick, the GS keeps the last TEX1 |
| particle / sprite K | header word `*0x4D8BB8` (`0x364068`, `0x364328`, `0x3645B8`) = `(int)(bias * 16 - 1000)` both halves (`LodBiasSet 0x365308`; `0x365578` and `0x360E18` set bias 1.0) | K = template + word (the overlay programs are not in the VU1 image; equal to the GS TEX1 in every draw measured) |
| immediate / flare K | the same word (`0x361468`, `0x361708`, `0x364D18`) | K = template + word + 1000 (equal to the GS TEX1 in every draw measured) |
| MIPTBP1/2 | `0x35F9A0` (A+D `0x34`, `0x36`) from the level offsets of `0x35F278` | level l at slot base + the 256-byte blocks of the levels below it, `TBW_l = ceil((w >> l) / 64)` |
| LOD input Q | `0x35B3D0`: `fVar7 = *0x4D7C1C * width * 0.5` | GS `1 / Q` = native screen w / fVar7 (`EOD_NATIVE_QCHECK`: ratio 1.0000, p5..p95 1.0000..1.0002 over 20,000 vertices) |
| CLAMP_1 | `0x3620A8` dirty `0x30` | `WMS = !(flags & 0x10)`, `WMT = !(flags & 0x20)`, MINU..MAXV 0: REPEAT or CLAMP. No setter writes REGION_CLAMP / REGION_REPEAT (census: modes 0 and 1 only) |
| TEST_1 | `0x3620A8` dirty `0x241` | ZTST `flags & 0x40 ? GEQUAL : ALWAYS`; `!(flags & 0x200)`: `0x12001` (ATE, NEVER, AFAIL ZB_ONLY: depth only); `flags & 0x201`: `0x1160B` (ATE, GEQUAL, AREF 0x60, AFAIL FB_ONLY); else `0x10000`. DATE is 0 in all three |
| ZBUF_1.ZMSK | `0x3620A8` dirty `0x80` | `!(flags & 0x80)` |
| PRIM.ABE | `0x3620A8` dirty `0x109` (GIFtag templates, VU1 qword `0x2C`) | `flags & 1` |
| ALPHA_1 | `BlendSelect 0x3654E0(sel)` -> `*0x4D8B78`, dirty `0x80000`; `0x3620A8` | `sel ? 0x44 (Cs, Cd, As, Cd) : 0x48 (Cs, 0, As, Cd)`, FIX 0. The other ALPHA writers (`0x362E70` shadow composite `0x44` with FIX = intensity, `0x35BE08` `0x44` FIX `0x80`) do not use C = Ad either: no destination-alpha blend in the retail game |
| render flags | `RenderFlagsCommit 0x365540` -> `*0x4D8BC8` (A1) | |
| frame copy / speed blur | the copy of `FBP 0x1180` to the display buffer: sprite, ALPHA `(Cs, Cd, FIX, Cd)`, FIX `0x60` (census) | drawn by the GS on both sides of P3 (2D path); its EE setter is not located yet, not changed |

### Phase A2: native implementation

- `src/native_texstate.cpp` (readable C++, `EOD_DIFF`): `0x3652A8`, `0x3654E0`; `0x365308` and
  `0x365578` wrapped (the original runs, the feed gets what it stored); `0x360E18` publishes them too.
  `fill_camera` puts the render flags, blend select, K template, MXL and the bias word in every packet.
- The batch decoder records each VU1 run's qword 198 z (`Mesh::k_delta`, `k_kick`, `k_runs`); a batch
  whose runs have different K is drawn run by run, each with its own K (`run K check`: the GS K
  sequence of every such object is a subsequence of the native one).
- `native_renderer.cpp` builds TEST, ALPHA, ABE, CLAMP, ZMSK, ZTST and TEX1 from those values
  (`ee_state`); the frame/depth surfaces, scissor and TEX0 (texture content, TFX/TCC) stay those of the
  object's first GS draw (R1). `compare_state` counts EE-vs-GS differences per kind (diagnostic only).
- Textures carry MXL levels (decoded at MIPTBP; a level in GPU-rendered pages ends the chain there).
  The shader is the GS texture unit: u, v in 1/16 texel, `LOD = -log2approx(Q) << L + K` with the
  exponent-and-mantissa log2 the software GS uses, rounded, at most MXL, MMIN above level 0 and MMAG at
  level 0, REPEAT/CLAMP/region addressing, bilinear with 4-bit weights truncated (`>> 8`). The clip
  matrix is scaled by `1 / fVar7` so clip w is the VU1 w (a uniform scale draws the same pixels).
- Alpha test: passing pixels with the state as is; failing pixels as a second pass with the test
  inverted and AFAIL's writes (FB_ONLY colour, ZB_ONLY depth, RGB_ONLY RGB); FB_ONLY without depth
  write is one pass. Before A2, FB_ONLY failing pixels also wrote depth.
- Blending: the full `(A - B) C + D` set as fixed-function factors (`blend_classes`, pipeline key bit
  15): each colour's coefficient `k + j C`; `1 + C` is counted and drawn as 1; C = Ad uses DST_ALPHA
  (Ad / 255, not Ad / 128) and is counted. Neither occurs in the retail scenes measured.
- Diagnostics: `EOD_NATIVE_QCHECK=1` (GS Q vs native w), `EOD_R5A2_OFF` (A/B: 1 no mip levels, 2 R4
  alpha test, 4 hardware sampler, 8 no per-run K), and `tools/p3_compare.py --breakdown` with
  `EOD_GS_REPLAY_FORCE=mip0|keep|nearest|noblend|nortt` (replay with one feature off: the live frame's
  MAE against it; lower than against the reference means the live frame lacks that feature).

### Phase A2 results (headless, RenderScale 1, Classic, `EOD_NATIVE_ONLY=2`)

EE state vs the GS registers of each object's first draw (Dundas Copcar run): world 1,861,498
objects, particle 6,643, sprite 8,681, immediate 76,401, flare 2,134: 0 differences in test, blend,
clamp, z, K and MXL; queue 262,401 objects: K and MXL 0, test 5, blend 31, z 5 (flags read when the
record is queued). Run K check 400,934 objects, 0 not matching. EOD_DIFF (separate run, Gilroy
'Battle (8 AI)', Postal): `0x3652A8` 289/289, `0x3654E0` 406/406 checked, 0 mismatched. P1 equal in
every frame of both P3 runs.

Before tracking the w scale the LOD used the native w (274 times the VU1 w), so every texture showed
its last level; the A/B runs (Gilroy, Hearse, 8 frames) found it: mip levels on 4.16 to 4.83 MAE, off
2.65, then on with the scale 2.64 (SSIM 0.978).

| scene | vehicle | frames | MAE (worst) | p99 (worst) | SSIM (worst) | sky MAE | A1 MAE / p99 / SSIM / sky |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Gilroy 'Battle (8 AI)' night | Limo (`136`) | 24 | 2.44 (4.46) | 28 (49) | 0.9561 (0.9213) | 1.79 | 3.79 / 30 / 0.9338 / 3.29 |
| Dundas 'Battle (8 AI)' day | Copcar (`504`) | 22 | 3.89 (18.81) | 32 (141) | 0.9563 (0.7448) | 5.62 | 6.61 / 74 / 0.9087 / 8.96 |

Views cycled in both: Cockpit, Bumper, Chassis, NearChase, Chase. Breakdown (Gilroy vb 1050, 1470,
1820, 2240): against the mip0, keep and nearest replays the live frame is now further away (+0.11,
+11.84, +0.70): it has those features. Against nortt (no projected-shadow composite) it is closer
(-0.28; Dundas A1 frames up to -4.45): the shadow composite is the largest remaining error source,
then the Cockpit/Bumper frames (Dundas vb 1402 Cockpit 18.81, vb 2104 Bumper 14.32: closer to the
no-blend replay by 4.32, i.e. a blended surface near the camera, see "camera inside another car").
P3 still fails.

### Phase A3: projected-shadow composite and P10 maximum LOD (spec)

| item | PS2 source | native |
| --- | --- | --- |
| shadow target | `0x35DA30` / `0x363218`: `min(view w, 320) x min(view h, 512)` PSMCT16 + PSMZ16 (`0x4D8BD8`, view `0x4D8C00..0x4D8C0C`) | same size times RenderScale (`Packet::shadow_rt`) |
| Z copy | `0x362968`: main Z16S through a nearest UV sprite, complemented, target cleared to 0 | shader reads the main D32 at `vx + (x + 0.9375) vw / w` |
| volume passes | `0x363708`: pass 1 RGB 15 added, pass 0 RGB 7 subtracted (one 5-bit step), `TEST_1 0x50000` GEQUAL, `ZMSK 1`, COLCLAMP 1 (GS census of the replay: `clamp=1`) | `B_ADD` / `B_REVSUB` 8/255, test in the shader (`z <= scene z + 2`, VU1 writes `65537 - z`, `0x3BE4C0` 0x00B0 / 0x0218) |
| z clipping | the VU1 clips only the guard band; the GS has no z clip | `K_NOCLIP` (depth clamp) on volume pipelines |
| extrusion | VU1 `0x3BE4C0` 0x0130..0x0170: `e = min(max(0, n . vf1) 65535, extrude)`, colour `0x363708` RGB 15 | `native_feed.cpp` `shadow_extruded_mesh` |
| composite | `0x362E70`: strips over the view, TEX1 0x60 bilinear, `TEXA` TA0 = intensity (`*0x4D8BF8`, `0x363480`), AEM 1, `ALPHA 0x44` with RGBAQ (0, 0, 0, 0x80) | `B_SHADOW`: `dst (1 - As / 128)` |
| draw distance | `0x212808` DrawDistanceApplyOption: `*0x495D58 = {0, 0.25, 0.5}[draw_distance option 0x4064D8] * 1700 + 300`, `0x495D5C = 4`, `0x34E530(1.0, far, 0x3F278D37)` | `src/native_lod.cpp`, EOD_DIFF |
| car LOD | `0x21C6E0` VisualCarSelectLodIndex: distance (`0x34FA98`) times `*0x3DD638` (`0x34F620`) times `g_ViewCountLodFactors 0x4062A8 = {1, 1, 2, 2}[views - 1]` (`0x211318`); first threshold (`+0x220`, stride 16) above it, clamped to `[+0x550, count - 1]` | `src/native_lod.cpp`, EOD_DIFF; `Detail=maximum` then sets `+0x54C = +0x550` (the highest level, `src/hle_lod.cpp`); Original/Classic keep the value |

`0x21C6E0` has no caller in SLUS_209.10 (0x218E68 inlines it): `EOD_LOD_SELFTEST=1` runs the readable
function through its EOD_DIFF wrapper on every car 0x218E68 selected and restores `+0x548/+0x54C`.

### Phase A3 results (headless, RenderScale 1, Classic, `EOD_NATIVE_ONLY=2`)

Shadow parity (`tools/p3_compare.py --shadow`, new: MAE on the pixels the GS composite decides, i.e.
replay vs `EOD_GS_REPLAY_FORCE=nortt`) and `EOD_NATIVE_SHADOW_OFF=2` (native shadow pixels white)
on Gilroy 'Battle (8 AI)' Limo, vb 700/770/840: GS 3,093..10,247 / 1,812 / 1,419 shadow pixels,
native 2,142 / 5,354 / 5,436, overlap 2,142 / 0 / 0. The native shadow lands below the GS one (GS y
211..320, native y 238..436). Variants, same frames: depth test z-pass (`EOD_NATIVE_SHADOW_TEST=2`)
0 native pixels; volume cull swapped 0..136 px, overlap 0; extrusion sign flipped 0 px. Both
combinations that would make a shadow on a valid scene depth draw nothing, so the volume pass most
likely reads no scene depth (the sampled main D32 behaves as cleared; only depth-clamped far faces
pass the z-fail test). **Not fixed: the shadow composite does not match the GS yet.**

P3 (24 windows each, live vs GS replay of the same frame) fails far beyond the shadow pixels:

| scene | vehicle | MAE all | SSIM all | Chase/NearChase/FarChase MAE / SSIM | A2 |
| --- | --- | --- | --- | --- | --- |
| Gilroy 'Battle (8 AI)' night | Bus (`790`) | 10.07 | 0.7768 | 12.79 / 0.7136 (15 windows) | 2.44 / 0.956 (Limo) |
| Gilroy 'Battle (8 AI)' night (A2 scene) | Limo (`136`) | 15.40 | 0.6745 | | 2.44 / 0.956 |
| Dundas 'Battle (8 AI)' day | Hearse (`502`) | 44.57 | 0.5113 | 55.00 / 0.4849 (12 windows) | 3.89 / 0.956 (Copcar) |

The breakdown (`--breakdown`) puts the error on blended pixels covering most of the screen
(`noblend` 258,344 px, +26..+49) with the shadow mask under 4,000 px (`nortt` +0.1..+0.9), and the
same Gilroy scene with the native composite off (`EOD_NATIVE_SHADOW_OFF=1`) is as bad (15.74): a
regression after A2 (the A3 work-in-progress runs went from 2.11 to 7.2+ on the same 4 windows
between 18:13 and 21:11 on 2026-11-19), not the shadow. Fixed, see below.

**Regression fixed.** Cause: `shadow_ps` is selected by `mode.w` bit 31 (`S_ON`, 3d7c0d6), but every
draw stored the car-highlight half vector's z in `mode.w` (`native_renderer.cpp`, q17 block), so each
non-`M_SPEC` draw with a negative z went through the shadow shader. `mode.w` is now 0 unless `M_SPEC`.
P3, same 24 windows (vb 700..2310 step 70), A2 build (12e7044) vs fixed HEAD: Gilroy night Limo MAE
3.04 / SSIM 0.9618 vs 2.68 / 0.9545 (was 15.40 / 0.6745); Dundas day Copcar 3.42 / 0.9573 vs 3.88 /
0.9518 (second run; the first, 4.97, had two more Cockpit windows of the near-plane case above at sky
MAE 43, as A2 has at vb 1330). EOD_DIFF (Gilroy Hearse, 1,500 frames): `0x3652A8` 287/287,
`0x3654E0` 403/403, `0x212808` 1/1, `0x21C6E0` 81/81, 0 mismatched.

P10 / EOD_DIFF (Dennison 'Battle (8 AI)', Taxi `508`, 1,500 frames, `Detail=maximum`): `0x212808` 1/1 checked, `0x21C6E0` 9,747/9,747 checked (`EOD_LOD_SELFTEST=1`), 0 mismatched. LOD index histogram (1P, every car of every selection): faithful (`EOD_CAR_LOD_MULTIPLIER=1`) i1 18,090, i2 3,888, i3 4,599, i4 67,869; maximum i1 19,494 of 19,494 (i1 = `+0x550`, level 0 is the parts-only cockpit model). The 2P runs (Dundas 'Demolition Derby', `humans 2`) read view count 1 at `0x3D1A74` in the histogram and are not a split-screen proof; the 2P P10 run was skipped (scope cut). `0x34BF90` (callers
`0x3067B8`, `0x32DFA8`) and `0x34CC98` (`0x222730..0x232FC8`): **not reached**; the trigger hunt was
dropped by the owner (scope cut 2026-11-19), the existing `diff_run` wrappers stay unverified.

### Open (R5)

- Yellow-and-black wedge over the grandstand on an oval track: not investigated.
- Translucent blurred rectangle in the upper right (speed-blur / RTT `FBP 0x1180` suspected): not investigated.
- 27 fps with `EOD_NATIVE_ONLY=1`: not investigated.
- Camera inside another car (e.g. an AI bus): Plume clips at the near plane (`depthClipEnabled`) and
  the GS does not; a depth-clamp A/B on Dundas Cannon gave Cockpit MAE 38.5 -> 7.9. Not changed in A1.
- P2 check: the R3/R4 suites set `EOD_VERTEX_CHECK=1`, which the check did not read
  (`EOD_NATIVE_SKIN_CHECK`), so no run ever printed it. It now reads both, covers every queue object
  and adds a geometric triangle match: Dundas Bus run, 0 GS triangles missing from the native meshes;
  native draws triangles the GS never receives for `0x318` batches (51%, likely VU1 back-face culling).

## Stage R6: Modern look materials (Phase A1, specification)

Built on the native Plume path only (never the old `ModernRenderer=1` GS-traffic pass, see
`docs/MODERN_RENDERER_RCA.md` and the rules in `docs/MODERN_LOOK_GUARDRAILS.md`).

### Look switch

| Item | Rule |
|---|---|
| `Look=Classic` (default) | the R5 shaders (`native_shaders.hlsl` `vs_main` / `ps_main`, SPIR-V unchanged) and the same pipeline keys: byte-identical to R5 |
| `Look=Modern` | `native_modern.hlsl` `vs_modern` / `ps_modern` (own SPIR-V file); pipeline key bit 24 (`K_MODERN`) selects them; every other key bit (blend, z, mask, cull) is the Classic item's |
| Switch | `EOD_LOOK=modern` at start; F9 / L1+R1+R3 (`modern::toggle`, routed to `native::toggle_look` when the native renderer runs); read once per flush, so the next frame is fully in the new look |
| No stall on toggle | every pipeline is created with its twin (Classic and Modern) and every texture descriptor set with its Modern sampler set, so a toggle creates nothing |
| Test hook | `EOD_LOOK_TOGGLE_EVERY=N` toggles every N vblanks and logs `[look]` frame times (P5) |

### Material record -> shader inputs

`tools/material_runtime.py` binds each of the 2,728 `material_table.json` records to the runtime
texture identity of the disc TEX it was tagged from (indexKey, paletteKey, w, h;
`docs/CREATION_KIT_TEXTURE_CONTRACTS.md` section 2) and writes `runtime/render/material_table.inc`
(2,694 records bound; the 34 left are direct-colour car paint and tyre TEX, which have no runtime key).
At startup the renderer loads every record. A native texture computes its indexKey / paletteKey once,
when it is decoded (`texture()`), and takes the record with the same keys and size (else the same
indexKey and size).

| Record field | Shader input (Modern only) |
|---|---|
| `metalness`, `roughness` | 4-bit each in `mode.z` 16..23: Filament standard model `f0 = mix(0.04, albedo, metal)`, `D_GGX`, `V_SmithGGXCorrelated`, `F_Schlick`, Lambert diffuse `(1 - metal) albedo` |
| `surface` class | `mode.z` 27..30: unlit (`fx`, `emissive_light`, `sky`, `backdrop`, `crowd_sprite`, `fake_shadow`, `ui`), car paint (Filament clear-coat lobe, roughness 0.1, f0 0.04), glass / water (f0 0.02..0.04, low roughness), chrome / metal, generic |
| `emissive` | `mode.z` bit 24: the Classic colour is kept as it is (no lighting added) |
| `detail != none` (relief material named) | `mode.z` bit 25: normal from height (texture luminance, screen-space surface gradient) |
| no record / no texture | generic dielectric, roughness 0.8 |

### What stays from the game's render state (never from the table)

| State | Source |
|---|---|
| Blend equation, alpha test, AFAIL, colour mask, z test / write, cull, scissor | the item's Classic pipeline key and `mode.x` / `mode.y` (GS state of the native draw, R5 A2) |
| Alpha value | the Classic texture function (TCC, TFX, vertex alpha) |
| Fog | the Classic GS fog after shading (`mode.y` FOGCOL, F) |
| Light direction and colours | VU1 light (`light`, `diffuse`, `ambient`) of lit objects; prelit world keeps its baked vertex light as the irradiance (multiplies albedo once, never relit) |
| Sun for prelit draws | the last lit queue object's world light direction and colour of this frame |
| Sky IBL | the texture the table tags `sky`: its mean upper and lower colours, decoded once; a hemisphere irradiance plus a roughness-blurred specular term (Filament `Fd_Lambert` irradiance, Karis analytic DFG in place of the DFG LUT); before a sky is seen, FOGCOL ; daylight: the sky colours and the prelit sun are scaled by FOGCOL / sky-horizon luminance (at most 1), because a night sky texture's mean is far brighter than the sky the game draws |

So `fx` / `emissive_light` cards keep the game's additive / mix blend and are drawn unlit: they
never become solid walls. Modern adds no vignette, grade, tone curve or haze.

### Results (R6 Phase A1)

Headless, muted, `RenderScale=1`, `Detail=maximum`, Battle (8 AI) unless named.

| Check | Result |
|---|---|
| Materials attached | 2,694 of 2,728 records keyed; draws with a record: Dundas day 69.9%, Gilroy night 72.1..74.0%, Career free roam 71.8%, Dennison 96.2..97.9% (the rest: untagged textures, generic dielectric) |
| Classic unchanged | Classic keeps the R5 SPIR-V, pipeline keys and push constants (`modern_patch` runs only when Look=Modern). P3 Gilroy night, 24 windows vb 700..2310: MAE 2.73 / SSIM 0.9533 vs R5 2.68 / 0.9545 (run-to-run noise) |
| P5 toggle | Look read once per flush: the next frame is fully in the new look. 100 toggles (every vblank, Dundas / Gilroy / Career): pipelines 19 -> 19, 17 -> 17, texture sets made late 0; texture memory grows only with textures streamed in while driving (no set or pipeline is made by a toggle). Frames > 33 ms: toggle frames 1 of 61 (1.6%) vs 23 of 1,740 (1.3%) without a toggle (Dundas, toggle every 60); the headless runs have the same paced-host hitches in Classic-only stretches (up to 142 ms), so the toggle adds none |
| Guardrails 4..7 (`tools/modern_guardrails.py`) | Dundas day (5 pairs, +-5 vblanks of a toggle): whole M/C 1.042 worst, region 1.085 worst; Gilroy night: 1.011 / 1.081 (was 1.045 / 1.165, FAIL, before the daylight scale); Career free roam: 1.017 / 1.058. New pixels at 1.0: 0. Opaque card blocks: 0. PASS |
| Guardrail 8 | Same items and draw count as Classic (one item list, two pipelines); paced frame mean Dundas Modern 16.60 ms vs Classic 16.76 ms (toggle every 60). No GPU timer on the native path: the unpaced GPU delta is not measured |
| Guardrails 1, 2, 3, 9 | 1: Modern draws only native items (no GS 3D, no classification from GS traffic); 2: classes come from the material table by texture identity, checked on Career free roam + 2 tracks; 3: Modern adds no shadow or AO (the shadow pipeline has no Modern twin); 9: the native path has no MSAA |
| EOD_DIFF | Gilroy Taxi 508, `EOD_DIFF=all`, Look=Modern toggling every 97 vblanks, 1,500 frames: 9 functions, 1,223 checked, 0 mismatched |

### Open (R6 A1)

- Guardrail 1 check with `EOD_NATIVE_ONLY=1`: headless screenshots read the GS frame buffer, which is empty in that mode (black frames), so no native-only capture exists yet.
- Guardrail 8 needs GPU timestamps on the native path (`EOD_GPU_PROF` does not exist there).
- Headless captures every vblank lag the look by a few frames (identical Classic / Modern halves); the checker skips such pairs.
- Camera views were not cycled in the guardrail runs (default chase view only).

### Results (R6 Phase A2)

R6 A2 by Antigravity "Neon", verified and finished by Claude. Headless, muted, `RenderScale=1`,
`Detail=maximum`, `DrawDistance=8`, Battle (8 AI) with `EOD_CHICKENS=1`, camera view changed by the
pad script at vb 900 / 1300 / 1700 / 2100, look toggled every 30 vblanks from vb 1500; pairs are
vb T-5 / T+5 of each toggle, skipping pairs in a game fade or cut (a frame more than 5% off its
same-look neighbour 5 vblanks away).

| Check | Result |
|---|---|
| Materials (`tools/material_runtime.py`) | the 318 `needs_human` records are resolved by deterministic name rules (`AUTO_NAME_RULES`; blend cards stay unlit `fx`, PBR bounds on resolved metal); `.tm0` high-res members inherit their `.tex` record by stem; unmatched indexed TEX members get an auto record. 3,167 records (3,133 keyed) vs 2,728 (2,694); draws with a record: Dundas 96.3..96.7% (was 69.9%), Gilroy 88.2..89.7% (was 72.1..74.0%), Career free roam 85.9..87.4% (was 71.8%); low-confidence records seen 0 |
| Row 8 (`EOD_GPU_TIMING=1`) | Vulkan timestamps around each native main and shadow pass (8-slot query ring, read back late); `[r6] GPU timing` line, counted from the first toggle on. Modern - Classic: Dundas +0.09..+0.23 ms (1.62 / 1.71..1.85 ms), Gilroy +0.04..+0.12 ms, Career -0.19..+0.02 ms. PASS. Modern issues fewer API draws than Classic (Classic draws once per VU1 TEX1.K run of an item; same item list) |
| Row 1 (`EOD_NATIVE_ONLY=1`) | Plume clears its own target from the dropped GS clear sprites, and the display reads Plume's last 3D target, so native-only captures are no longer black: Dundas 10 frames lum mean 122..125, 100% non-zero; Gilroy night 12.0..12.8, 59..63% non-zero; Career 8 of 9 frames lum 116..141 (one frame at a game fade, black) |
| Rows 4..7 | Career free roam: whole 0.999, region 0.899, new 1.0 px 0, opaque blocks 0: PASS. Dundas: whole 1.052, region 1.159 (one pair), new 1.0 px 0.016%, opaque 0.089%. Gilroy night (Postal, cycled views): whole 0.61, region 0.33, opaque 0 - the same with the A1 material table (0.66 / 0.32..2.10), so it comes from the cycled camera views, not from A2 |
| Modern GPU VFX | `vfx_fire` / `vfx_smoke` / `vfx_spark` classes (soft radial mask on particle / sprite quads, fBM flame / wisp, ember core; tint and mask only, never brighter than Classic). Opt-in with `EOD_MODERN_VFX=1`: with it on, row 7 fails (opaque blocks Dundas 0.18%, Gilroy 1.79%; 0.09% / 0% with it off), so by default these records are unlit `fx` cards |
| Classic | Classic SPIR-V (`native_shaders_spv.inc`), pipelines and push constants unchanged; all A2 code runs only for Look=Modern, `EOD_NATIVE_ONLY=1` or `EOD_GPU_TIMING`. P3 Gilroy night (Taxi, 7 windows vb 1050..2250): MAE 3.50 / SSIM 0.928, per window 1.5..7.1, inside A1's per-window range 1.18..8.82 (A1 24 windows: 2.73 / 0.953) |
| P5 toggle | 24 toggles per scene: pipelines 19 -> 19 (Dundas, Gilroy), 17 -> 17 (Career) |
| EOD_DIFF | Gilroy Limo, `EOD_DIFF=all`, Look=Modern toggling every 97 vblanks, 1,500 frames: 28 functions, 1,279 checked, 1 mismatched (`0x214B68`, camera / LOD EE code that A2 does not touch); EE state vs GS: 0 mismatches |

### Open (R6 A2)

- Modern GPU VFX makes flat bright blocks (row 7 FAIL with `EOD_MODERN_VFX=1`): find which cards (KIND_QUEUE / immediate fire meshes without the radial mask are the suspects) and fix before turning it on by default.
- Gilroy night with cycled camera views: Modern region luminance 0.33..0.63 of Classic (rows 4/5), pre-existing with the A1 table; find which view and region.
- `EOD_DIFF` mismatch in `0x214B68` (1 of 5 calls): check whether it reproduces in Classic.
- P3 at the A1 window set (24 windows vb 700..2310) for a like-for-like Classic number.

---

## Stage R4: shadows, particles, sprites, flares and immediate geometry

Stage R4 moves the remaining non-queue 3D/effect GS draws to readable C++ (verified by `EOD_DIFF`)
and native Plume rendering. R4 is split into two implementation micro-sprints:

- **Phase A1 (this spec & implementation):** Projected shadows (`0x34C7D0`, `0x34C798`, `0x34C208`,
  `0x34C4B0`, `0x35D170`, `0x35D298`) and sun glare / light flares (`0x304D50`, plus the
  single-viewport gate in `0x20F2D8`).
- **Phase A2 (next):** Particles (`0x3639D0`, `0x363B98`, `0x363FF8`, VU1 `0x08E0`), effect
  sprites (`0x363DE8`, `0x363F48`, `0x364548`, `0x3642F4`, VU1 `0x01B8`), general immediate
  geometry (`0x35DF10`, `0x361468`, `0x361708`, `0x361BC8`, `0x364D18`, sphere-tested `0x361ED8` /
  `0x361C30` <- `0x34EE70`, `0x34EEB8`), the non-queue cockpit quad (`0x219C48` -> `0x35CAA0`),
  and the sender of VU1 UV/screen-offset program `0x25F0` -> `0x2630`.

### Full R4 function inventory (`SLUS_209.10`)

| Subsystem | Address | Role | VU1 program | Phase |
|---|---|---|---|---|
| Shadows | `0x30A580` | Per-viewport shadow pass driver (`0x34C7D0` -> `0x30A608` -> `0x30A910` -> `0x34C798`) | — | A1 |
| Shadows | `0x34C7D0` | `ShadowBegin(dir*, alpha)`: copy shadow dir to `0x4C96A8`, open RT via `0x35DA30` | — | A1 |
| Shadows | `0x34C798` | `ShadowEnd()`: close & composite shadow RT via `0x35D9F0`, unbind slots 0/1, clear `0x4C96B4` | — | A1 |
| Shadows | `0x35DA30` | `GsShadowBegin(alpha)`: clamp viewport to `320x512`, call `0x363218`, upload shadow VU1 (`0x35A5D8(1)`) | — | A1 |
| Shadows | `0x35D9F0` | `GsShadowEnd()`: restore main VU1 (`0x35A5D8(0)`), composite & restore via `0x363640` | — | A1 |
| Shadows | `0x363218` | `GsShadowTargetOpen(w, h, alpha)`: allocate VRAM target (`0x4D8BD8`), copy Z & clear (`0x362968`) | — | A1 |
| Shadows | `0x363640` | `GsShadowTargetClose()`: restore aspect/viewport, composite onto main FB (`0x362E70`) | — | A1 |
| Shadows | `0x34C208` | `ShadowSubmit(modelId, mat*, extrudeLen)`: sphere-cull extruded volume, emit front/back batches | `0x0020` | A1 |
| Shadows | `0x34C4B0` | `ShadowSubmitMasked(modelId, mat*, mask, extrudeLen)`: same with temporary layer mask override | `0x0020` | A1 |
| Shadows | `0x35D9C8` | `ShadowSetExtrude(len)`: store extrude length at `0x4D7D18`, set dirty bit `0x4` in `0x4D7D10` | — | A1 |
| Shadows | `0x35D148` | `ShadowSetPass(pass)`: store pass (`1`=front/+ , `0`=back/-) at `0x4D7D14`, emit GS state (`0x363708`) | — | A1 |
| Shadows | `0x35D298` | `ShadowEmitBatch(batch, clipFlag, matPkt)`: forward with `pass = *(0x4D7D14)` to `0x35D170` | `0x0020` | A1 |
| Shadows | `0x35D170` | `ShadowEmitBatchPass(batch, pass, clipFlag, matPkt)`: emit matrix REF + batch DMA REF (`ITOP`) | `0x0020` | A1 |
| Flares | `0x20F2D8` | Per-viewport render finish; gates `0x304D50` on `*(0x3D1A74) == 1` (single-viewport check) | — | A1 |
| Flares | `0x304D50` | `FlareRender()`: update sun (`0x304608`/`0x304970`) & lights (`0x304818`/`0x304B18`), flush buckets | `0x0270` | A1 |
| Flares | `0x303CB8` | Emit one rotated 2D flare quad (4 vertices) through `0x361708` (`ImmediateVertex`) | `0x0270` | A1 |
| Particles | `0x3639D0`, `0x363B98`, `0x363FF8` | Particle stream submission & VU1 packet build | `0x08E0` | A2 |
| Sprites | `0x363DE8`, `0x363F48`, `0x364548`, `0x3642F4` | Effect sprite submission & VU1 packet build | `0x01B8` | A2 |
| Immediate | `0x35DF10`, `0x361468`, `0x361708`, `0x361BC8`, `0x364D18` | General immediate geometry API | `0x0270` | A2 |
| Sphere-imm | `0x361ED8`, `0x361C30` (`0x34EE70`, `0x34EEB8`) | Sphere-tested immediate geometry | `0x0270` | A2 |
| Cockpit quad | `0x219C48` -> `0x35CAA0` | Non-queue cockpit quad | `0x0270` | A2 |
| UV-offset | `0x25F0` -> `0x2630` | Trace EE sender & move to Plume | `0x25F0` | A2 |

---

### Phase A1.1: Projected shadows (`0x34C7D0`, `0x34C798`, `0x34C208`, `0x34C4B0`, `0x35D170`, `0x35D298`)

#### Call graph

```
WorldFrameRender (0x214B68)
  └─ ShadowPassRun (0x30A580)
       ├─ ShadowBegin (0x34C7D0)(dir = 0x4AE970, alpha = *(int*)0x3DC064)
       │    └─ GsShadowBegin (0x35DA30)(alpha)
       │         ├─ GsShadowTargetOpen (0x363218)(min(w,320), min(h,512), alpha)
       │         │    └─ GsShadowClearAndCopyZ (0x362968)()
       │         └─ Vu1LoadMicrocode (0x35A5D8)(1)   // 0x3BE4C0..0x3BFC80
       ├─ ShadowDrawWorldProps (0x30A608)()  ─┐
       ├─ ShadowDrawDynamicCars (0x30A910)() ─┴─> ShadowSubmit (0x34C208) / ShadowSubmitMasked (0x34C4B0)
       │                                            ├─ FrustumCullSphere (0x34F650)
       │                                            ├─ ShadowSetExtrude (0x35D9C8)(extrudeLen)
       │                                            ├─ MatrixPacketBuild (0x35CD28)(mat) -> matPkt
       │                                            ├─ ModelGetBatchList (0x34D060)(modelId)
       │                                            └─ for each layer-visible batch:
       │                                                 ├─ ShadowSetPass(1) -> 0x363708(1)
       │                                                 ├─ ShadowEmitBatch (0x35D298) -> 0x35D170(batch, 1, clip, matPkt)
       │                                                 ├─ ShadowSetPass(0) -> 0x363708(0)
       │                                                 └─ ShadowEmitBatch (0x35D298) -> 0x35D170(batch, 0, clip, matPkt)
       └─ ShadowEnd (0x34C798)()
            ├─ GsShadowEnd (0x35D9F0)()
            │    ├─ Vu1LoadMicrocode (0x35A5D8)(0)   // restore main microcode 0x3BFC80..0x3C3450
            │    └─ GsShadowTargetClose (0x363640)()
            │         └─ GsShadowComposite (0x362E70)()
            └─ TextureBind(0), TextureBind(1)
```

#### Guest state & struct layouts (verified from `gen/rec_098.c`, `gen/rec_100.c`, `gen/rec_101.c`)

- **`0x4C96A8`** (`0x4D0000 - 26968`): `float shadowDir[3]` — world-space extrusion direction copied by `0x34C7D0`.
- **`0x4C96B4`** (`0x4C96A8 + 12`): `int32_t shadowOpen` — return value of `0x35DA30` (1 while shadow target is active, cleared to 0 by `0x34C798`).
- **`0x4C96C0`** (`0x4D0000 - 26944`): pointer to model descriptor table (entry stride `0x14` bytes; `entry + 8` -> model header `hdr`, where `hdr + 0x18` = `float radius`, `hdr + 0x1C` = `float center[3]`).
- **`0x3DD634`** (`0x3E0000 - 10700`): `uint32_t g_LayerMask` — bitmask of visible model layers; batch `b` is drawn when `(g_LayerMask & (1 << *(int16_t*)(b + 0x18))) != 0`. `0x34C4B0` saves, overrides, and restores this word around its batch loop.
- **`0x4D7C48`** / **`0x4D7C4C`** (`0x4D0000 + 31816 / 31820`): current / last-emitted matrix packet pointer. `0x35D170` emits a 9-qword DMA REF (`0x30000009`) to `matPkt` whenever `matPkt != -1`, then caches it at `0x4D7C4C`.
- **`0x4D7D10`** (`0x4D0000 + 32016`): shadow dirty flags (`bit 0` = active, `bit 1` = pass set, `bit 2` = extrude set).
- **`0x4D7D14`** (`0x4D7D10 + 4`): `int32_t shadowPass` (`1` = front faces / increment, `0` = back faces / decrement).
- **`0x4D7D18`** (`0x4D7D10 + 8`): `float shadowExtrudeLen` — copied into matrix packet `q[5].w` (`colour_b[3]`) by `0x35CD28` (lines 5195-5206 of `gen/rec_100.c`).
- **`0x4D8BD8`** (`0x4E0000 - 29736`): GS shadow render-target descriptor written by `0x363218`:
  - `+0x00`: `int32_t width` (PS2 clamped `<= 320`), `+0x04`: `int32_t height` (PS2 clamped `<= 512`)
  - `+0x08`: `int32_t tw` (`ceil(log2(width))`), `+0x0C`: `int32_t th` (`ceil(log2(height))`)
  - `+0x10`: `uint32_t fbp`, `+0x14`: `uint32_t psm = 2` (`PSMCT16`)
  - `+0x18`: `uint32_t zbp`, `+0x1C`: `uint32_t zpsm = 0x32` (`PSMZ16`)
  - `+0x20`: `int32_t intensity = (alpha >> 1) + (alpha >> 7)` (0..128 GS alpha units; `0x363480`)
  - `+0x24`: `int32_t active` (`1` when open)
  - `+0x28..+0x34`: saved main viewport `{x, y, w, h}` from `0x35E300`
  - `+0x38`: saved aspect float from `0x34F498`, `+0x3C`: saved fog enable from `0x4D8B7C`

#### VU1 shadow extrusion & GS blend state (verified from `0x3BE4C0` and `0x363708` / `0x362E70`)

- **VU1 setup (`0x3BE4C0 + 0x1568..0x17B8`, `MSCAL 0`):**
  Reads the 7-qword matrix packet at `xtop`: `q[0..3]` = model matrix `M`, `q[5].w` = `extrudeLen` (`vf2.w`), `q[6]` = `light` (`vf24`). Computes model-space extrusion vector `vf1 = -M^T * (light.xyz * light.w)`.
- **VU1 extrusion (`0x3BE4C0 + 0x0020..0x02F8`, `MSCAL 4`, `kProgShadow`):**
  Each batch vertex (`vif_mesh.cpp` `kProgShadow`, 2-qword stride: `q0` = V4-8 normal + ADC in `w`, `q1` = position + `q199` offset) is extruded in model space:
  - `n = (normal_byte - 128)`
  - `d = dot(n, vf1.xyz)`
  - `e = min(max(0.0, d) * 65535.0, extrudeLen)`
  - `p' = p + vf1.xyz * e`
  Face culling uses the triangle normal against camera position `vf7` and `pass = ITOP & 1`: `pass == 1` keeps front faces (culls back faces), `pass == 0` keeps back faces (culls front faces).
- **GS blend state per pass (`0x363708`, `gen/rec_101.c:5911-5934`):**
  - Depth test `GEQUAL` (`TEST_1 = 0x50000`), depth writes disabled (`ZBUF_1.ZMSK = 1`, set by `0x362968`).
  - `pass == 1` (front faces): `ALPHA_1 = (0x80 << 32) | 0x29` -> `(Cd - 0) * FIX(1.0) + Cs = Cd + Cs` (**additive**), `RGBAQ = (15, 15, 15, 0)`.
  - `pass == 0` (back faces): `ALPHA_1 = (0x80 << 32) | 0xA1` -> `(Cd - Cs) * FIX(1.0) + 0 = Cd - Cs` (**subtractive**), `RGBAQ = (7, 7, 7, 0)`.
- **GS composite (`0x362E70`, `gen/rec_101.c:5440-5496`):**
  - `ALPHA_1 = 0x44` (`(Cs - Cd) * As + Cd`, standard alpha blend), `ZMSK = 1`, `TEST_1 = 0x30000` (depth always, no Z write).
  - `TEXA = TA0(0) | AEM(1) | TA1(intensity)` (`0x8000` sets `AEM = 1`: texels with `RGB == 0` become alpha 0; non-zero shadow texels get alpha `intensity` from `0x4D8BD8 + 0x20`).
  - Draws a full-viewport quad with `RGBAQ = (0, 0, 0, 0x80)` over the main framebuffer, darkening pixels inside any shadow volume by `intensity / 128.0`.

#### PS2 limits removed in Plume (P10 / adaptive host target)

- **PS2 `320x512` shadow RT cap (`0x35DA30`):** On PS2, `0x35DA30` clamps shadow buffer dimensions to `min(w, 320) x min(h, 512)` in `PSMCT16`/`PSMZ16` to fit 4 MB GS VRAM, causing blocky shadows and 16-bit depth quantization artifacts.
- **Native Plume implementation:** Plume allocates a dedicated host shadow accumulation texture (`R8G8B8A8_UNORM`) at the **main framebuffer's full scaled resolution** (`s.width x s.height`, scaling dynamically with `RenderScale` — e.g. `3840x2688` at 6x) and depth-tests directly against the main framebuffer's full-resolution `D32` depth buffer (`s.depth`). `ee_shadow_begin()` snapshots the main-viewport camera/projection before `0x363218` overwrites `0x4D7C20` with the clamped `320x512` viewport. Each `KIND_SHADOW` batch is extruded in `vs_main` (`M_SHADOW`) and rendered into the host shadow buffer (pass 1 additive with back-face cull, pass 0 subtractive with front-face cull). On `CMD_SHADOW_COMPOSITE` (`0x34C798`), Plume composites the full-resolution shadow mask onto `s.color` using `intensity / 128.0` (`ALPHA_1 = 0x44`, `AEM = 1`). Meanwhile, because `KIND_SHADOW` suppresses the GS emulator's shadow volume draws, the GS VRAM shadow RT stays all-zero so `0x362E70` (`AEM = 1`) is a transparent no-op in hybrid mode and `EOD_NATIVE_ONLY=1` renders full shadows natively.

---

### Phase A1.2: Sun glare and point-light flares (`0x304D50`, `0x20F2D8`)

#### Call graph

```
ViewportRenderFinish (0x20F2D8)
  ├─ RenderQueueFlush (0x34C860)
  ├─ EffectsPostFlush (0x2E7368)(0)
  ├─ [PS2 gate: if (*(int*)0x3D1A74 == 1)] -> lifted under SplitScreenParity (P8)
  └─ FlareRender (0x304D50)()
       ├─ if (0x11F9A0() != 0) return    // menu/pause gate (*(int*)0x3C4BDC)
       ├─ dt = clamp(TimerGetMs() - *(0x3DBF6C), 1, 100) * 0.001f
       ├─ RenderStatePush (0x34F760); Clear(4, 8, 2); Set(0x80)
       ├─ if (*(0x3DBB40) != 0): SunUpdate (0x304608)(dt); SunEmit (0x304970)()
       ├─ for i in 0 .. *(0x3DBF68)-1: LightUpdate (0x304818)(light[i], dt); LightEmit (0x304B18)(light[i])
       ├─ for b in 0 .. *(0x3D94F0)-1:
       │    if (bucket[b].count > 0):
       │      Begin2DTexture (0x34D9A8)(bucket[b].texSlot)
       │      if (ImmediateBegin (0x35DF10)(bucket[b].count * 4)):
       │        for j in 0 .. bucket[b].count-1:
       │          FlareEmitQuad (0x303CB8)(&entry[j].x, entry[j].rgb, entry[j].size, entry[j].intensity)
       │            └─ 4x ImmediateVertex (0x361708)(sx, sy, 0.0f, u, v, argb, strip = k >= 2)
       │        ImmediateEnd (0x361BC8)()
       │      TextureSlotEnd (0x34F888)()
       │      bucket[b].count = 0
       └─ RenderStatePop (0x34F710)
```

#### Guest state & struct layouts (verified from `gen/rec_045.c`, `gen/rec_085.c`, `gen/rec_086.c`)

- **`0x3D1A74`** (`0x3D0000 + 6772`): `int32_t g_ViewCount` (`1` = 1P, `2` = 2P, `4` = 4P). `0x20F2D8` skips `0x304D50` when `g_ViewCount != 1`. Under `SplitScreenParity` (P8, disabled during `EOD_DIFF` to keep bit-identical parity checks), `0x304D50` runs for every viewport.
- **`0x3C4BDC`** (`0x3C0000 + 19420`): read by `0x11F9A0()`; nonzero suppresses flare rendering.
- **`0x3DBF6C`** (`0x3E0000 - 16532`): `uint32_t g_FlareLastMs` (previous `TimerGetMs()` timestamp).
- **`0x3DBB40`** (`0x3E0000 - 17600`): `int32_t g_SunFlareEnabled`.
- **`0x3DBF68`** (`0x3E0000 - 16536`): `int32_t g_PointLightFlareCount`; entries at `0x3DBB48` (stride `0x2C` bytes).
- **`0x3D94F0`** (`0x3E0000 - 27408`): `int32_t g_FlareBucketCount`; buckets at `0x3D94F4` (stride `0x308` bytes):
  - `+0x000`: `int32_t count` (max 32 entries per bucket)
  - `+0x004`: `FlareEntry entries[32]` (stride `0x18` = 24 bytes: `float x, y, z, size, intensity; uint32_t rgb`)
  - `+0x304`: `uint32_t texSlot`

#### Native Plume feed

- `0x34D9A8(texSlot)` saves the 3D camera (`0x4CD2F8` -> `0x4CD328`), installs an orthographic-equivalent 2D camera at `z = -halfW / (fov * aspect)` + identity model matrix (`0x34CC38`), and binds `texSlot` (`0x3514F0`).
- Therefore, calling `native::ee_flare_begin(texSlot)` immediately after `0x34D9A8` and before `0x35DF10` captures the exact 2D camera & matrix packet via `fill_camera` / `read_matrix_packet`, and uses the exact same `screen_matrix(p)` and `ImmediateVertex (0x361708)` capture mechanism as `KIND_WATER` (`KIND_FLARE = 4`).
- `0x304D50` sets render flags (`Clear(4, 8, 2)`, `Set(0x80)`), which configure additive blending and Z-test off on the GS; Plume reads the live `gs::Env` state (blend, depth test/write, texture) at `finalize()` just like `KIND_WATER`, so flares render natively in both `EOD_NATIVE=1` and `EOD_NATIVE_ONLY=1` with P1 parity tracking (`flares submitted == drawn`).

---

### Phase A2.1: Particles (`0x363B98`, `0x3639D0`, `0x363B68`, `0x363FF8`, VU1 `0x08E0`)

#### Call graph & VU1 microcode (`vu1_029b295eacf3f666.bin`, base `0x3BD670`)

```
Particle emitters (0x2F2020, 0x2F2268, 0x2F24B0, 0x2F2AC8, ...)
  ├─ ParticleBegin (0x363B98)(int32_t count, float scale)
  │    ├─ DentOffsetWrite (0x35DD28)(1, 0x4D87B4, 0x4F) -> upload 79-entry sin/cos table to VU1 0x90
  │    ├─ MaterialPacketSet (0x35DDC8)(pkt)
  │    ├─ Vu1SetParticleMicrocode (0x35CA98)()
  │    ├─ GsStateFlush (0x3620A8)()
  │    └─ ParticlePacketReserve (0x363FF8)(0x4D8798) -> PacketReserve(min(rem, 16) + 7) [RA 0x364034]
  ├─ N x ParticleAdd (0x3639D0)(int32_t col_rgb, int32_t col_alpha, float size, float x, float y, float z, float angle)
  │    ├─ if (count == cap): ParticlePacketFlush (0x364068) + ParticlePacketReserve (0x363FF8)
  │    └─ pack 16-byte particle qword at data_ptr + count * 16
  └─ ParticleEnd (0x363B68)()
       └─ ParticlePacketFlush (0x364068)(0x4D8798) -> MSCAL 0x08E0 ((0x3BDF50 - 0x3BD670) >> 3), PacketCommit
```

#### Guest state & struct layout (`0x4D8798` = `0x4E0000 - 30824`, verified from `gen/rec_101.c`, `gen/rec_102.c`)

- `+0x00`: `int32_t active` (return value of `0x363FF8`; cleared to 0 by `0x363B68`).
- `+0x04`: `int32_t count` (particles written in current VU1 chunk, `0..16`).
- `+0x08`: `int32_t cap` (capacity of current VU1 chunk, `min(remaining, 16)`).
- `+0x0C`: `int32_t remaining` (remaining requested particles).
- `+0x10`: `float scale` (`f12` passed to `0x363B98`; `0x364068` writes `0.5f * scale` into `TOP+3.z` for vertical top-to-bottom lighting gradient).
- `+0x14`: `uint32_t pkt_base` (DMA packet returned by `0x366D30`).
- `+0x18`: `uint32_t data_ptr` (`pkt_base + 0x60`, 16 bytes per particle).
- `+0x1C` (`0x4D87B4`): 79-entry `(cos, sin)` lookup table (`0..2pi` in 78 steps; `0x3E22F983 = 1/(2pi)`, `0x429C0000 = 78.0f`, `0x41800000 = 16.0f` fractional sub-step).

#### Packed particle qword & VU1 `0x08E0` math

- Word 0 (`x`): `(float_bits(x) & ~0xFF) | frac`
- Word 1 (`y`): `(float_bits(y) & ~0xFF) | idx`
- Word 2 (`z`): `(float_bits(z) & ~0xFF) | (col_rgb >> 1)` (GS 0..127 brightness)
- Word 3 (`w`): `(float_bits(size) & ~0xFF) | (col_alpha >> 1)` (GS 0..127 alpha)
- `0x364068` writes 4 header qwords (`TOP+0` = 4-vertex strip GIFtag `PRIM=0xC02E4000`, `TOP+1` = `cam_up - cam_right`, `TOP+2` = `cam_up + cam_right`, `TOP+3` = `{cam_right.y, cam_up.y, 0.5f * scale, *(0x4D8BB8)}`).
- **Native Plume feed (`KIND_PARTICLE`):** Builds a single dynamically growing `Mesh` per `ParticleBegin..ParticleEnd` stream (removing the PS2 16-particle VU1 chunk cap, P10). Each particle emits a 4-vertex (2-triangle) camera-aligned rotated quad around `(x, y, z)` using `R' = cos(angle)*R - sin(angle)*U`, `U' = sin(angle)*R + cos(angle)*U`, half-extent `size`, UVs `(0,0)..(1,1)`, and per-corner vertical sky-gradient shading `rgb * (1 - 0.5*scale*(1 - up_dot))`.

---

### Phase A2.2: Effect sprites (`0x363F48`/`0x363DE8`/`0x363F18`/`0x364548` VU1 `0x01B8`, and `0x363D78`/`0x363C18`/`0x363D48`/`0x3642B8` [RA `0x3642F4`] VU1 `0x05E0`)

#### Two parallel sprite emitters (both in `vu1_029b295eacf3f666.bin`, base `0x3BD670`)

1. **Simple sprites (`0x4D8760` = `0x4E0000 - 30880`, VU1 `0x01B8` = `0x3BD828 - 0x3BD670`):**
   - `0x363F48(count)` -> `0x35CA98()`, `0x3620A8()`, `0x364548(0x4D8760)` (`PacketReserve(min(rem, 33) + 6)` at RA `0x364584`).
   - `0x363DE8(r128 col, int32_t flip, float size, float x, float y, float z)`: packs `{x, y, z, size}` masked `& ~0xFF` ORed with 128-bit GS-scale RGBA (`col.uw[0..3] & 0xFF`) and `flip` bits (`bit 0` in word 0 selects `U+R` vs `U-R`; `bit 1` in word 1 flips sign). Flushes via `0x3645B8` + `0x364548` every 33 sprites.
   - `0x363F18()`: calls `0x3645B8`, clears `active = 0`.
   - `0x3645B8`: emits GS `SPRITE` (`PRIM=0x60AF4000`, `REGS=0x00434312`), `TOP+1 = U - R`, `TOP+2 = U + R`, `MSCAL 0x01B8`.
2. **Atlas/grid sprites (`0x4D8778` = `0x4E0000 - 30856`, VU1 `0x05E0` = `0x3BDC50 - 0x3BD670`):**
   - `0x363D78(count, gridSize)` -> `0x35CA98()`, `0x3620A8()`, `0x364500(state, gridSize)` (`gridSize` in `{4, 16, 64}` -> `grid_shift` in `{1, 2, 3}`, `grid_mask = (1 << shift) - 1`), `0x3642B8(0x4D8778)` (`PacketReserve(min(rem, 33) + 5)` at RA `0x3642F4`).
   - `0x363C18(r128 col, int32_t tile, float size, float x, float y, float z)`: packs `{x, y, z, size}` ORed with 128-bit GS-scale RGBA and atlas tile coords `tx = tile & grid_mask` (word 0), `ty = tile >> grid_shift` (word 2). Flushes via `0x364328` + `0x3642B8` every 33 sprites.
   - `0x363D48()`: calls `0x364328`, clears `active = 0`.
   - `0x364328`: emits GS `SPRITE` (`PRIM=0x60AF4000`, `REGS=0x00434132`), `TOP+1 = {U - R, tex_size / (1 << grid_shift)}`, `MSCAL 0x05E0`.
- **Native Plume feed (`KIND_SPRITE`):** Both simple and atlas sprites emit camera-aligned 4-vertex (2-triangle) quads into a dynamically growing `Mesh` per stream (removing the PS2 33-sprite VU1 chunk cap, P10), applying exact flip UVs for `0x363DE8` and exact `(tx, ty) / (1 << grid_shift)` atlas UVs for `0x363C18`.

---

### Phase A2.3: Immediate geometry (`0x35DF10`, `0x361468`, `0x361708`, `0x361BC8`, `0x364D18`) & sphere-tested immediate (`0x34EE70`, `0x34EEB8`, `0x361ED8`, `0x361C30`)

#### State struct (`0x3E5848` = `0x3E0000 + 0x5848`, VU1 `0x0270` in `vu1_fd0ce30ab0ec263c.bin`)

- `+0x00..+0x17`: saved previous vertex `{x, y, z, u, v_with_strip_lsb, raw_colour}` for 2-vertex strip continuation when splitting across 60-vertex (`0x3C`) PS2 VU1 chunks.
- `+0x18`: `int32_t active`
- `+0x1C`: `int32_t total_requested`
- `+0x20`: `int32_t remaining`
- `+0x24`: `int32_t chunk_written` (`0..chunk_cap`)
- `+0x28`: `int32_t chunk_cap` (`min(remaining, 60)`)
- `+0x2C`: `uint32_t col_ptr` (4 bytes/vert: `(adc << 31) | (B << 16) | (G << 8) | R` in GS 0..128 scale)
- `+0x30`: `uint32_t vert_ptr` (24 bytes/vert: `float x, y, z, u, v; uint32_t alpha`)
- `+0x34`: `uint32_t giftag_nloop_ptr` (patched by `0x3651D8` to `chunk_written | 0x8000`)
- `+0x38`: `uint32_t pkt_qwc` (committed by `0x366CD0` on chunk flush / `0x3651D8`)

#### Functions & native Plume feed (`KIND_IMMEDIATE`)

- `0x35DF10(numVerts)`: calls `0x35BA10()` then `0x361468(numVerts)`. When not already inside `WaterDraw` (`KIND_WATER`), `FlareRender` (`KIND_FLARE`), or `0x219C48` (`KIND_COCKPIT`), opens a `KIND_IMMEDIATE` stream.
- `0x361468(numVerts)`: reserves first chunk (`<= 60` verts, RA `0x3614CC` to `0x366D30`), writes VIF header + `ITOP 0x200` + `MSCAL 0x4E` (`0x0270 >> 3`).
- `0x361708(colour, strip, x, y, z, u, v)` & `0x364D18(state*, colour, strip, x, y, z, u, v)`: appends vertex to current chunk (flushing + repeating the last 2 vertices when chunk reaches 60 verts, RA `0x3617E4` / `0x364DF0`). Feeds `ee_water_vertex`, `ee_flare_vertex`, `ee_cockpit_vertex`, or `ee_imm_vertex` (single dynamically growing mesh, removing the PS2 60-vertex chunk cap, P10).
- `0x361BC8()`: tail-calls `0x3651D8(0x3E5848)` and closes `ee_imm_end()`.
- **Sphere-tested pre-built immediate geometry:**
  - `0x34EE70(obj*)`: `FrustumTestSphere(obj + 4, 0, 0, *(float*)(obj + 16))`; if visible, tail-calls `0x361ED8(*(uint32_t*)obj)`.
  - `0x34EEB8(obj*, mat*)`: transforms `obj + 4` by 3x4 `mat`, calls `FrustumTestSphere`; if visible, calls `0x361ED8(*(uint32_t*)obj)`.
  - `0x361ED8(bufObj*)`: calls `0x3620A8()` then tail-calls `0x361C30(bufObj)`.
  - `0x361C30(bufObj*)`: reserves 1 qword (`RA 0x361C48`), writes DMA REF (`0x30000000 | *(int16_t*)(bufObj + 16)`, `addr = *(uint32_t*)bufObj`, `ITOP 0x200`), commits. Native feed decodes the pre-built `0x0270` VIF buffer (`0x361C98` format: `UNPACK V4-8` colours+ADC + `UNPACK V3-32` `{x,y,z,u,v,a}`) into a cached `Mesh` and tags the REF as `KIND_IMMEDIATE`.

---

### Phase A2.4: Cockpit non-queue quad (`0x219C48` -> `0x34DB70` -> `0x35CAA0`)

- Inside `0x219C48` (`gen/rec_046.c:6116-6352`), when `0x2114A8() != -1`, it builds 4 vertices on the stack (32 bytes each: `{x, y, z, pad, colour=0xFFFFFFFF, pad, u, v}`) and 6 `int16_t` triangle indices `{0, 1, 2, 0, 2, 3}` (copied from `0x407008`), binds texture slot `0x2114A8()` via `0x34F928`, and calls `0x34DB70(verts, 4, indices, 6)` (RA `0x21A064`), which tail-calls `0x35CAA0`.
- `0x35CAA0(verts*, numVerts, indices*, numIndices)` (`gen/rec_100.c:4946-5006`) is `ImmediateDrawIndexedTris`: calls `0x35DF10(numIndices)`, loops `i = 0 .. numIndices - 1` calling `0x361708` on `verts[indices[i]]` with `strip = (i % 3 == 2)`, and calls `0x361BC8()`.
- Replacing `0x35CAA0` in readable C++ (`diff_run`) and classifying `ra == 0x21A064` as `KIND_COCKPIT` (all other `0x35CAA0` callers go through `KIND_IMMEDIATE`) renders the cockpit rear-view quad natively on Plume with P1 parity tracking (`cockpit submitted == drawn`) and 0 leftover emulated 3D geometry in cockpit view.

---

### Phase A2.5: VU1 program `0x25F0` -> `0x2630` (traced)

- **EE sender of `0x25F0`:** `0x3620A8` (`gen/rec_101.c:4845-4995`), the shared GS/VU1 state flush invoked by `0x35BA10`, `0x35DAD0`, `0x361318`, `0x361ED8`, `0x362E70`, `0x363B98`, `0x363D78`, `0x363F48`, and `0x35D4C8`. Whenever dirty state bits `& 0x109` in `(*(0x4D8BC8) ^ *(0x4D8BCC)) | *(0x4D8B74)` are nonzero (setting internal flag `0x400000`), `0x3620A8` unpacks two 128-bit GIFtag templates (computed from `0x42A190`/`0x42A1A0` and render flags `*(0x4D8BC8)`) to `TOP+0..1` and executes `MSCAL 0x25F0` (`(0x3C2270 - 0x3BFC80) >> 3 = 0x04BE`) followed immediately by `MSCAL 0x24D8` (`0x049B`).
- **What `0x25F0` does (`vu1_fd0ce30ab0ec263c.bin`):** 5 instructions (`0x25F0..0x2610`): `xtop vi1; lq vf14, 0(vi1); lq vf13, 1(vi1); sq vf14, 19(vi0); sq vf13, 18(vi0) [E]`. It is a **state-setup program** (not a draw program) that stores the two GIFtag PRIM templates into VU1 data memory qwords `18` and `19`.
- **What `0x2630` is:** Not a standalone VIF/MSCAL entry point (`0x25F0` halts at `0x2610` with the `E` bit). `0x2630` is an internal VU1 subroutine called via `bal` (`0x21`) from four sites inside the existing main mesh microcode (`0x05B8`, `0x1058`, `0x1878`, `0x1B60`), where the caller loads `vf13.xy` before branching.
- **Native renderer impact:** None needed — `0x25F0` emits zero GS primitives and only updates VU1's GIFtag PRIM state (fog/alpha/AA/prim flags that Plume already reads directly from live `gs::Env` on every native draw).


