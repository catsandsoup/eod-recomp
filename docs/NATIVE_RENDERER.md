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

### R3.2a Classic static queue projection

On the `fd0ce30ab0ec263c` VU1 image, `0x02A8` reads ITOP bit 9 and enters
the non-clipping loop at `0x1BD8` when the bit is clear. The first vertex adds
TOP+199 to the decoded position at `0x1C28`. The loop applies vf4–vf7 in
the MULAX, MADDAY, MADDAZ, MADDW order at `0x1C30–0x1C48`, followed by
DIV, MULQ and FTOI4 at `0x1C60–0x1CA8`. Setup `0x0000` formed those rows
from the camera and model packets (`0x35B3D0`, `0x35CD28`).

On the same Dundas vertices, both the double-precision P2 check and the
submitted float clip-space transform crossed fixed-point boundaries. The
existing VU-ordered matrix construction matched 371,280 recorded vf4–vf7
words in the moving trace, and 232,023 checked native source positions
matched the VU inputs with homogeneous W equal to 1.

For unskinned, unposed Classic queue draws with ITOP bit 9 clear,
`vs_static_queue` reads the rounded X/Y/W rows and GS viewport mapping
from a per-flush instance stream. It applies VU-order projection and
FTOI4 to XY, retaining the existing vertex shader's clip Z/W, texture Q
and shading. The draw order, indices, scissor and depth state use the
same path. The pre-FTOI4 P2 comparison retains its 1/16 px threshold.
The shader requires Vulkan 1.2 or higher and advertised float32
signed-zero/Inf/NaN preservation. The SPIR-V execution mode selects
precise arithmetic for this stage on MoltenVK, where NoContraction alone
misses the GS grid. Modern first-use warms the Classic variant for later
switches. See [R3.2a results](NATIVE_RENDERER_SUCCESS.md#r32a-classic-static-queue-projection-at-1).
[MoltenVK shader float controls](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Configuration_Parameters.md#mvk_config_fast_math_enabled)

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
| `Look=Classic` (default) | the R5 shaders (`vs_main` / `ps_main`) and draw state, with the R3.2a vertex entry for supported unclipped, unskinned `0x02A8` queue draws |
| `Look=Modern` | `native_modern.hlsl` `vs_modern` / `ps_modern` (own SPIR-V file); pipeline key bit 24 (`K_MODERN`) selects them; every other key bit (blend, z, mask, cull) is the Classic item's |
| Switch | Set `EOD_LOOK=modern` to start in Modern. During play on macOS and Windows, press F9, or hold L1 + R1 and click the right stick on player 1's controller in an offline race. The game displays the selected look. Plume reads it once per flush. |
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

### Results (R6 Phase A3, partial)

| Check | Result |
|---|---|
| Classic P3, A1 window set | Gilroy night, Battle (8 AI), Limo (vehicle 136, as A1), 24 windows vb 700..2310 step 70, HEAD 7d43b3d: MAE 2.38 / SSIM 0.9640 over 16 windows (8 skipped: no whole GS frame in the replay window), worst 4.03 / 0.9389, vs A1 2.73 / 0.9533. Classic unchanged by A2: no bisect needed. (`tools/p3_run.sh` with a relative `EXE` fails at the compare step: pass an absolute `--exe`.) |
| Night darkness, located | Gilroy night, Postal, cycled views (pairs T-5 / T+5 of toggles every 30 from vb 1500): Chase / Chassis / Bumper views have the top regions at 0.63..0.72 of Classic, the Cockpit view (vb 2101+) has whole 0.65, middle row 0.33 / 0.47 / 0.33 (uniform grey 84 -> 27 per channel). |

### Results (R6 Phase A4)

Rows 4/5 at night. Headless, muted, `RenderScale=1`, `DrawDistance=8`, Battle (8 AI) + `EOD_CHICKENS=1`,
Triangle at vb 900 / 1300 / 1700 / 2100 (Chase, Chassis, Bumper, Cockpit), look toggled every 30 vb
from 1500, captures every vblank 1490..2210. Pair = last old-look frame and first new-look frame
(1 vblank apart; the game draws a new frame every 3..4 vblanks, so T-5 / T+5 pairs mixed in up to
0.66..1.75 of region motion); toggles within -10..+12 vb of a camera change are skipped.

| Fix | What |
|---|---|
| Baked-light floor (`ps_modern`) | `col = max(col, albedo * E)`: the game's baked (prelit) or VU1 light times albedo is the floor; PBR only adds on top. Metal records lost `metal` of their diffuse before (T1119 / T1082 / T1058 / T1075 are fences, poles and railings, `metal_painted` 0.6..0.7, records not changed) |
| Metal reflection | indirect radiance `lerp(sky * occl, E, metal)`: a metal mirrors the game's light here, not only the dark night sky; without it the floor alone left Chassis regions at 1.22 (bright pixels kept, dark ones raised) |
| Cockpit alpha | the M_SPEC highlight alpha is Classic's exactly: VU1 0x948 per vertex with the game's q17 half vector (`modern_patch` packs it into `ambient.w` bits 8..31, octahedral 12+12, ITOP alpha in 0..7), not per pixel from `L + V` |

| Scene | Before (A3) | After |
|---|---|---|
| Gilroy night, Postal, views cycled (2 runs, 18 + 5 pairs) | Chase / Chassis / Bumper top regions 0.63..0.72; Cockpit whole 0.65, middle row 0.33 / 0.47 / 0.33 | whole 0.98..1.02 (Cockpit 1.00..1.01), region worst 1.096; new 1.0 px 0; opaque blocks 0. PASS |
| Dundas day, Hearse, views cycled (10 pairs) | region 1.159 | whole worst 1.023, region worst 0.914. PASS |
| Career hub (`place=home`, 3 non-identical pairs) | - | whole 1.009, region 1.035. PASS (hub only, not free roam) |
| Classic | - | Classic SPIR-V, pipelines and push constants untouched (all changes in `ps_modern` / `vs_modern` / `modern_patch`). P3 Gilroy night Limo, 4 windows vb 1050 / 1470 / 1750 / 2030: MAE 4.16, per window 2.1..8.1, inside A1's per-window range 1.18..8.82 |

### Open (R6 A4)

- `metal_painted` records at 0.6..0.7 metalness (paint is a dielectric): retag via `tools/material_runtime.py` if a day scene shows them too shiny; not needed for rows 4/5.
- Career free roam (Time Challenge gate 1) rerun: only the hub was run in A4 (done in A5).
- Modern VFX opaque blocks (row 7 with `EOD_MODERN_VFX=1`): not started; VFX stays opt-in (A5: harness artefact, VFX on by default).
- `EOD_DIFF` mismatch in `0x214B68`: not started (check Classic and f0ef513, R2 split-screen parity-off gate) (A5: reproduces in Classic, see A5).


### Results (R6 Phase A5)

Modern GPU VFX on by default. Same harness as A4 (headless, muted, `RenderScale=1`,
`DrawDistance=8`, Battle (8 AI) + `EOD_CHICKENS=1`, Triangle at vb 900 / 1300 / 1700 / 2100, look
toggled every 30 vb from 1500, 1-vblank pairs, toggles within -10..+12 vb of a camera change skipped).

| Item | Result |
|---|---|
| Where the A2 "blocks" came from | Re-scoring the A2 captures pair by pair: Gilroy's 1.786% is the one T-5 / T+5 pair at vb 2100, which straddles the Cockpit camera cut at vb 2101 (Classic half Bumper view, Modern half Cockpit view: whole 1.71, region 2.78); Dundas' 0.179% / 0.089% are single T-5 / T+5 pairs with motion (the VFX-off set has 0.089% too). With A4's 1-vblank pairs the A2 shader (HEAD) with VFX on already gives 0% on Gilroy and Dundas. No VFX draw is opaque: every VFX draw seen uses a standard mix (`Cs As + Cd (1 - As)`) or an additive blend (`Cs As + Cd`) from the game's state (`EOD_VFX_LOG=1`: particles, sprites and immediate fire / smoke / spark, records 2442 / 2486 / 2491 / 2492 / 2532 / 2535 / 381) |
| Shader hardening (`ps_modern`) | coverage = the game's alpha (texture alpha x vertex alpha, the TCC / TFX combine) x the procedural falloff, applied the way the game's GS blend fades a card (`modern_patch` puts the fade in mode.z bits 20..21): source alpha for a mix, source colour when Cd is kept whole (additive / subtractive), C -> 1 for an inverse mix, and no change at all for opaque, FIX or destination-alpha blends (they draw exactly as Classic). The tint stays <= 1 per channel; the colour is no longer also darkened by the mask on mix blends; the alpha test uses the game's alpha, not the masked one |
| Default | Modern VFX is on in Look=Modern; `EOD_MODERN_VFX=0` turns it off (records then unlit `fx` cards) |

| Scene (VFX on) | Rows 4/5 whole / region worst | Row 6 | Row 7 before (HEAD shader) -> after | Row 8 Modern - Classic |
|---|---|---|---|---|
| Gilroy night, Postal, views cycled (19 pairs) | 0.965 / 0.881 | 0 | 0% -> 0% (A2 T-5 / T+5 scoring: 1.79%, camera-cut pair) | - |
| Dundas day, Hearse, views cycled (13 pairs; HEAD 20) | 1.021 / 0.902 | 0.0014% | 0% -> 0% (A2: 0.18%) | +0.33 ms (5.26 / 5.59 ms; HEAD run +0.22 ms). PASS |
| Career free roam (`place=malt`, Drive Around, 17 pairs) | 1.035 / 1.072 | 0.0049% | -> 0% | +0.12 ms. PASS |

Classic: Classic SPIR-V, pipelines and push constants untouched (`ps_modern` and `modern_patch`
only). `EOD_DIFF=00214b68` Gilroy Limo, Look=Modern toggling every 97 vb, VFX on by default,
2,000 frames: 678 checked, 1 mismatched (the first call, see below); EE state vs GS immediate:
29,036 objects, 0 mismatches.

`0x214B68`: the mismatch reproduces in Classic (`ModernRenderer=0`, no toggle, 2,400 frames: 800
checked, 1 mismatched), so it is not renderer state. It is only the first call of the race: two
words 0x70 apart (`0x772C60`, `0x772CD0`) where the original leaves the upper halfword `0x0A30`
and the readable `0x214B68` writes 0 (`0x00006BEF` vs `0x0A306BEF`), every later call matches;
single view, so not the split-screen parity gate. Not fixed (not trivial in the time box).

### Open (R6 A5)

- `0x214B68` first-call mismatch: find which field (stride 0x70 at `0x772C60`) the original fills
  in its upper halfword on the first frame and the readable path does not.
- `EOD_DIFF=all` (and some multi-function lists) on Gilroy Limo did not reach the race in 2,400
  vblanks (no `[SESSION] start:`), where A2 did in 1,500; `EOD_DIFF=00214b68` alone does. Classic
  as well, so not A5: bisect which checked function stalls the direct start.
- `metal_painted` metalness (from A4) still open.

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



---

## Stage R7: native 2D and emulator removal

### VU1 MSCAL no-op retirement

The live VU1 instruction pairs at byte address `0x24D8` are identical in
all four captured retail images. The first pair has the E bit and the
second is its delay pair. Both are no-ops and have no GS output. The
Dundas trace observed ordinary MSCALs from reserves at `0x35B428`
(camera), `0x35BA6C` and `0x35BAE8` (batch), `0x35D1B4` (shadow),
`0x35DEFC` (world) and `0x362104` (shared state flush).

Check the two live pairs at the MSCAL boundary after VIF updates TOP,
ITOP, DBF and TOPS. In Plume mode, exact matches can use the existing
retirement regardless of the source packet. A different entry, changed
instruction pair, or observed VU dispatch uses ordinary execution.
MSCALF and MSCNT continue through ordinary execution.
`EOD_NATIVE_CAMERA_NOP=0` retains the ordinary MSCAL path for comparison.
Compare the full VU state, scoreboard and data memory against the
compiled and interpreter paths using `EOD_NATIVE_CAMERA_NOP_CHECK=1`.

### Original camera-only retirement evidence

`EOD_NATIVE_CAMERA_NOP=1` with `EOD_NATIVE_ONLY=1` bypasses VU1 dispatch for the
two `0x24D8` launches in the camera packet. The EE producer at `0x35B3D0`
reserves 18 qwords at return address `0x35B428`. Its CNT payload begins with
two MSCALs, mark `0x6666`, and the 16-qword UNPACK. VIF checks the reservation
and those four words before attempting retirement. Both microcode pairs must
match `8000033c 400002ff` and `8000033c 000002ff` in lower/upper word order.

TOP, ITOP, DBF and TOPS update through the normal MSCAL handler. The VU
retirement advances two pairs, completes Q/P and flag updates, and leaves TPC
at `0x24E0`. The interpreted path also synchronizes pending pipes per pair,
matching the interpreter's behavior. The scoreboard and VU data memory are
unmodified by these pairs. `EOD_NATIVE_CAMERA_NOP_CHECK=1` compares the entire
VU state, scoreboard and data memory against live VU dispatch before accepting
the retirement. VU recording, VU logging, VU check/stats, and active scene
capture or emission continue through normal dispatch. An idle legacy emitter
permits retirement. A live scene-emission window restored all 18 camera
dispatches and produced its capture file. The render trace lists dispatches
and camera retirements separately.

On macOS, Dundas at 1× Classic with maximum detail and draw distance dispatched
2,842 VU1 starts at vblank 841, down from 2,860. Normal-dispatch and forced-
interpreter diagnostic runs validated 13,556 and 3,988 retirements respectively.
Seven live scanout pairs at vblanks 598–604 were pixel-exact. At 838–844,
121–219 near-field pixels per pair differed by at most two channel values.
Separate reference runs also varied. Replaying the same retirement-on capture
with the pre-change and current builds produced identical scanout bytes for
each of the software and Vulkan backends. The Dennison retail-menu 2P run
retired 34–68 launches per sampled trace interval with full VU-state checks
and the idle legacy emitter enabled. Both Dundas 1P variants changed camera
at vblanks 682 and 776. Independently timed live captures had different
command streams. Pixel-exact comparison of those switches and the 2P race,
and native Windows runtime validation, remain open. This optimization
covers only the measured camera producer.

`EOD_VU_STATS=1` reports elapsed microseconds per compiled VU1 entry in five-second
windows. It uses the same wall-clock units on Apple Silicon and x86. In a 1,600-vblank
Dundas Modern diagnostic at 6×, entries `0x0270` and `0x0618` together represented
41.1–45.9% of measured compiled VU1 time across five windows. The known senders
include world draws and the shared immediate path. Entry `0x0D48` represented
14.5–16.0%, and `0x24B0` represented 13.6–16.4%. These are per-entry diagnostic
shares. Interpreter time, surrounding VIF work, and frame presentation have
separate measurements.

### Dundas world-leaf VU1 retirement (opt-in)

Set `EOD_NATIVE_WORLD_RETIRE=1` with `EOD_NATIVE_ONLY=1` to enable retirement
for the Dundas opaque pass-0, slot-35 leaf at `0x0109D018`. The committed DMA
REF is eligible once at its exact tag address, after the native object marker
enters the ordered stream. VIF requires the 32-qword EE payload at `0x0109D050`,
its exact tag words and MSCAL position, ITOP `0x080`, and the expected double
buffer state. The VU checks XXH3-128 fingerprints of the full 512-byte payload
(`2494700ae9738e9be4e1e3a046e75426`) and the live 16 KiB micro image
(`c8c64eae4bb1b7eb2329b43f09230e29`). The existing recompiler lookup checks
the compiled `0x0270` entry against its exact microcode ranges.

The host derives TEX1 and the 20-vertex strip from live VU inputs. It restores
the affected registers and VU data words, preserves the existing ST padding,
and retires 312 cycles to TPC `0x0920`. The two original-format PATH1 packets
follow the object marker through the same GIF submission routine. A mismatch,
forced interpreter, portable GS/VU recording, or active VU diagnostics uses
the original dispatcher. `EOD_RENDER_TRACE_EVERY` counts the host retirements
separately from actual `0x0270` dispatches. GS still consumes the compatibility
packets for ordered state and geometry.

### R7 opaque world-strip differential

At the EE call `0x358E8C`, WorldRenderPass opens the texture record with
`0x35DEE0`. It writes the leaf REF from `part + 0x38`. The VIF codes
mark non-clipped leaves with ITOP `0x80` and clipped leaves with `0x280`.
For `0x0270` and ITOP `0x80`, the live VU image branches to `0x0640`.
It gets the vertex count from the tag at TOP, adds the position base at
TOP+199 and uses the UV base at TOP+198. The vertex loop at `0x07B8`
advances one vertex per iteration. For non-positive signed K, the
epilogue writes the TEX1 word at q21 and sends q20 before the strip at
TOP+200. The E-bit pair and delay at `0x0918` and `0x0920` retire
this branch. An independent dispatch at `0x0920` has different control
flow.

For broader retirement, compare against the compiled reference offline
with the same VU state, data memory, TOP, ITOP and active scoreboard.
Compare final VU state, full data memory and the live scoreboard in
`--vu-bench` before changing runtime admission. The recorded Dundas
sample had 1,471 invocations of `0x0270`, including 400 four-vertex
and 37 twenty-vertex invocations with ITOP `0x80`. A single payload
address is insufficient to cover this producer.

Ordinary Plume admits a committed non-clipped world REF by default.
It checks the actual DMA tag, source address, payload size, first MSCAL
and top-bank layout. Later VIF commands remain in the stream. The host
checks the strip layout and exact 16 KiB live VU image fingerprint. It
revalidates the image after each microcode generation change. The public
build has no compiled VU table. For interpreted execution, the host
derives each vertex's ten FMAC flag contributions and preserves the
final MAC, sticky flags and eight-entry flag ring from the epilogue
at `0x0848..0x0888`. Both ordered kicks and the full state are compared
against the reference in `EOD_NATIVE_WORLD_RETIRE_CHECK=1`. The layout
guards, simulated VU state and GS compatibility packets still bridge
to other legacy programs. `EOD_NATIVE_WORLD_RETIRE=0` restores the
reference for ordinary draws. Forced interpretation and portable
capture also retain the reference dispatch.

### Phase A1: race HUD producer table (`SLUS_209.10`, Ghidra `DecompileAddrs`, 2026-11-20)

`0x0011CA48(view)` (per player from `0x00101C00` at `0x00101D34`; full screen view -1 from
`0x001063DC`) calls `HudDrawLeftRaceRows`, `HudDrawRightSpeedRows`, then the centred `0x0011ACC0`.

| Producer | Address | Role | Callees that reach the GS | GS state |
|---|---|---|---|---|
| `HudDrawLeftRaceRows` | `0x00119888` | left queue rows (`0x003C4370` font, count `0x003C45F8`, 0x40-byte rows at `0x003C4378`, type `+0x38`, slide `+0x3C`) | `0x0011C278` backing rect (colour `0x66000000`), `0x0011B960` text, `0x001194C0` bar row, `0x0029CA40`/`0x0029E138` frame sprites | see leaves |
| `HudDrawRightSpeedRows` | `0x00119F40` | right queue rows (`0x003C4600`) | same leaves (`0x0011C278` at `0x0011A0DC`, `0x0011A414`) | see leaves |
| X anchors | `0x0011DC50` / `0x0011DC30` | left/right endpoint `0x003C4310 + view*0x18 + 4/8` (`0x0011D2C8` table, overscan `0x00297498`) | none | widescreen edge shift: `src/hle_hud.cpp` -106.667 / +106.667 (16:9), -64 / +64 (16:10) native units, restored after the call |
| rect leaf | `0x0011C278` -> `0x0011C2E8` (`0x0034F8D0(-1)`: no texture) -> `0x0011E730` -> `0x0035CAA0` | 4 verts by `0x00297298` (z 1.0), indices `0x003EE718` | `ImmediateDrawIndexedTris` -> `0x35DF10`/`0x361708`/`0x361BC8` (VU1 `0x0270`) | untextured, vertex alpha blend, then `0x0034F888` state flush |
| sprite leaf | `0x0029E138` | 4 verts by `0x00297B60` (`{x,y,z,u,v,colour}`), indices `0x00413AE0`, texture `0x0034F928(font+0x84, 0x00294D40())` | `0x0035CAA0` (same VU1 `0x0270` path) | textured, alpha `0xFF000000` template, `0x0034F888` |
| text leaf | `0x0011B960` -> `0x0029A9E8` (twice: drop shadow at +2,-2 with colour & `0xFF000000`, then text) | queues 0x54-byte entries at `*0x003D7440`, count `0x004A7E3C`, flushed by `0x0029A2B8` when > `0x5F` (PS2 cap: 96 strings, P10 item) | `0x0029A2B8` -> `0x0034F888` (`0x0029A648`) | font texture per glyph |

HUD matrix `0x00297A88` is identity with Z = -365 under the active projection; all HUD geometry
reaches the GS through the immediate path, which R4 already feeds to Plume as `KIND_IMMEDIATE`.

### Results (R7 Phase A1, measured 2026-11-20, `EOD_NATIVE_ONLY=1`, Classic, RenderScale 1)

- Dundas 'Battle (8 AI)' (vehicle 790, Cockpit -> NearChase via Select), 1,300 vb: **~80 dropped
  GS primitives per GS frame**, all full-screen sprite strips, **0 HUD triangles**: fb 0 tex 1180
  (20, display copy), fb 1180 tex 33e0 (20), fb 1180 untextured (20), fb 2be0 tex 2300 (10, half
  width), fb 2be0 untextured (10); 1 stray tri (fb 1180 tex 2fc8) per 100 frames. P1 immediate
  1116/1116. The R4 "~35/frame race" figure counted EE frames, not GS frames.
- So the race HUD is not in the dropped set: the remaining drops are full-screen post / copy
  passes (frame path, R7 A4 scope), not the HUD.
- P3 (`tools/p3_run.sh`, `ONLY=1`, Dundas Battle, Limo vehicle 136, vb 850/1250/1650) whole frame MAE 35.03, SSIM 0.670;
  corner regions TL MAE 6.70 / SSIM 0.850, TR 29.73 / 0.763, BL 50.17 / 0.633, BR 32.09 / 0.752.
  Not HUD-isolated: the dropped full-screen passes change every pixel, so these numbers do not
  measure HUD parity.

### macOS R7 residue check

Apple M2 Pro, Plume Vulkan over MoltenVK, Dundas Battle (8 AI), Bus, 725
vblanks. The run used Classic at RenderScale 1, maximum detail, DrawDistance 8,
`EOD_NATIVE_ONLY=1`, `EOD_NATIVE_LOG=120` and `EOD_RENDER_TRACE_EVERY=120`.
It exited cleanly. At vblank 720, the EE trace listed 3,967 VU1 program starts.
The native renderer reported 55,688 dropped GS primitives cumulatively and
9,900 of 9,900 tagged HUD draws across 689 of 689 measured frames. The drop
report still includes the five fullscreen or half-width sprite-strip groups
targeting frame buffers 0, 0x1180 and 0x2BE0, plus triangles from two texture
sources. HUD object counts and native-only draw suppression measure separate
things. S4 still fails because VU1 runs and GS jobs remain unowned.

### R7 6× readback trace on macOS

Apple M2 Pro, Dundas Battle (8 AI), Bus, Modern, RenderScale 6, maximum
detail, DrawDistance 8, 480 headless vblanks. The capture ran with
`EOD_GS_READBACK_TRACE=50000` and logged GS counters every 60 vblanks.
Two steady windows each recorded 420 deferred CPU-write preservations,
120 merges and 300 overwrites, with zero waits in those windows.
Later race windows recorded 300–420 preservations and 0–6 waits.
The preservation counter includes copies that finish asynchronously and
copies whose saved payload is discarded.

The steady merge requests came from uploads to `0x2BE0` PSMZ32, the shadow
storage already mapped in R7 A4. The common request rectangles were
`(0,256,64,64)` and `(64,384,256,64)`. Texture uploads at `0x2BC4` and
`0x2FC8` overlapped pages under the shadow owner. Native shadow preparation
must keep its temporary depth storage out of the emulated texture ring
to remove those preservation copies and waits.

### R7 Classic scanout pairing

`tools/p3_run.sh` pairs each replay window with the live image captured on
its terminal GS vblank. `--exact-scanout` reads the boundary from the
capture sidecar and requires the software replay to stop on that vblank.
Strict mode checks every color channel against the 1/255 MAE limit.
The capture readback is diagnostic and is off during performance runs.

On the Mac Dundas Battle run at 1× with `EOD_NATIVE_ONLY=2`, exact pairs at
vblanks 856 and 1056 gave mean MAE 4.73, median p99 52 and mean SSIM 0.9464.
With `EOD_NATIVE_ONLY=1`, the vblank 856 pair failed with MAE 20.47, p99
149 and SSIM 0.7442. These are software GS replay comparisons. Original
PS2 photo-level sign-off and the rest of the scene suite remain open.

With the typed scene clear enabled, a fresh display-copy off/on check on
the same build paired Dundas Battle Classic at exact vblanks 856 and 1056.
Enabling the native display copy reduced mean RGB MAE from 6.23 to 4.42 and
raised mean SSIM from 0.9416 to 0.9536. Median p99 moved from 48 to 50.
Each pair still fails strict P3.

The original depth diagnostic read the GS context at the terminal vblank.
That could be the next scene, even while FBP0 still held the prior display
copy. Two replays cut after the display copy and after the next scene clear
had identical color and different depth files. The software replay diagnostic
now samples Z16S at the verified `0x35BBD0` copy boundary for this retail
layout. The two cuts now have byte-identical depth, with 92,358 zero-depth
pixels and 194,362 populated pixels. The corrected display-copy-on pairs
had 123,873 and 92,358 sky-proxy pixels, measuring sky MAE 0.49 and 0.74.
The full-frame P3 metrics and projected-shadow discrepancies remain.

At vblank 1056, the broad `nortt` replay diagnostic reduced
whole-frame MAE from 6.87 to 1.20. The narrower `--shadow` diagnostic
then suppressed the `0x362E70` composite state (FBP `0x1180`, TBP
`0x33e0`, ALPHA `0x44`) and reproduced the same 42,687 affected
pixels. Within that mask, MAE was 39.05 against the normal replay
and 0.95 against composite-off. At vblank 856 the narrower diagnostic
isolated 2,509 pixels and MAE moved from 30.12 to 10.58. These capture
pairs show that counting submitted Plume shadow draws is insufficient
to prove shadow coverage.

### Open (R7 A1)

- Readable C++ for `0x00119888` / `0x00119F40` under `diff_run` + `EOD_DIFF` (>= 1,000 calls, 0
  mismatches) not written yet; the leaves already reach Plume through `KIND_IMMEDIATE`.
- Decide what the HUD 2D pass is: move HUD `KIND_IMMEDIATE` draws (RA inside `0x0011E730` /
  `0x0029E138` -> `0x0035CAA0`) to a new `KIND_HUD` orthographic pass after 3D in both looks.
- Verify projected shadows in the remaining viewport layouts and scene suite.
- HUD-only P3 needs a HUD pixel mask (for example a replay with `0x0011CA48` skipped) before parity can be scored.
- Night flood-lit track, 2P split-screen, odd-vehicle rotation runs for A1 are not done yet.

### R7 A4 strip producers

The Dundas Classic `dundas-classic.gsd.850` capture repeats the five groups below. The
order starts just after the GS vblank marker, with the display copy for the preceding
scene. The clear starts the next scene, and that scene reaches the display copy after
the next marker. All five groups arrive through VIF1 DIRECT on GIF PATH2. The trace
matches exact GIF tags and register writes to their EE emitters. The strip numbers are
32-pixel-wide sprites and exclude intervening geometry and register-only packets.

| Order | Target and operation | EE packet evidence |
|---|---|---|
| 1 | `FBP 0 <- TBP 0x1180`, 20 × 32×448. Blend uses `FIX=0x60` in this capture. | `0x35BBD0`, `gen/rec_100.c`. 88 DIRECT qwords, `FRAME_1=0xA0000`, `ALPHA_1=0x6000000064`, tag `40ab400000008014/5353`. |
| 2 | `FBP 0x1180` scene color and Z clear, 20 × 32×448. Observed RGBA `64 64 74 FF`, Z 0. | `0x361098` (aligned-width branch, reserve RA `0x361154`), `gen/rec_101.c`. 45 DIRECT qwords, initial tag `3003400000000001/01ee`, sprite tag `2000000000008014/0055`. The unaligned-width branch tails to `0x360EF0`. |
| 3 | `FBP 0x2BE0` shadow target white clear, 10 × 32×448. Z writes masked. | `0x362968` (reserve RA `0x362A34`), called from shadow begin `0x363218` (RA `0x36356C`), `gen/rec_101.c`. `FRAME_1=0x3205015F`, tag `200340000000000a/0055` at qword 5. |
| 4 | `FBP 0x2BE0 <- TBP 0x2300`, 10 × 32×448. This pass samples the scene Z area and enables writes to the shadow Z area. | Same 77-qword command as row 3, tag `40ab40000000000a/5e5e` at qword 32. `ALPHA_1=0x80000000A1` and the Z write mask clears before this subpass. Shadow volume draws follow. |
| 5 | `FBP 0x1180 <- TBP 0x33E0`, 20 × 32×448. Shadow composite, `ALPHA_1=0x44`. | `0x362E70` (reserve RA `0x362F34`), called from shadow end `0x363640` (RA `0x3636DC`), `gen/rec_101.c`. 89 DIRECT qwords, tag `40ab400000008014/5e5e`. The native shadow pass already implements its own accumulation and composite, so retain a single owner for that output. |

The capture carries GIF path and packet contents. The live Dundas copy called
`0x35BBD0` from `0x35E948` (RA `0x35E978`). The EE wrote the tag via
`0x204D70C0` and VIF1 consumed it at physical `0x004D70C0`. The other static
caller `0x35C138` (RA `0x35C158`) remains on its original path. A bounded EE
entry trace can establish the higher-level caller of `0x361098`; the existing
`EOD_RENDER_TRACE` reserve attribution sees VU1 starts and misses DIRECT-only
senders.

**Aligned scene clear.** The EE reserve at `0x361154` tags a typed operation from
`0x361098`'s saved rectangle and color inputs. The matching VIF1 DIRECT consumes
it at the original DMA position. Plume clears only the 640×448 color and depth
rectangle after checking the live target, registers and offsets. Unsupported cases,
software and `EOD_NATIVE_SCENE_CLEAR=0` use the original packet. Portable GS dumps
retain that packet for reference replay.

On Apple M2 Pro in Dundas Battle at 1×, the five steady native-only groups totalled
80 residual strips with the feature off and 60 with it on. A separate same-run
diagnostic at GS vblank 120 started from populated buffers and compared this clear
against the original sprite pass. All 1,146,880 CT32 bytes and 573,440 Z16S bytes
matched, and each range changed from its pre-clear contents. This measures the
clear boundary. The shadow preparation and composite strips still remain.

**Display-copy contract.** `0x35BBD0` takes an EE packet destination in `r4`
and returns `0x59` qwords in `r2`, including one DMA/VIF tag and `0x58`
DIRECT qwords. Guest word `0x3E57F8` supplies the source base in pages,
`TBP = (word << 5) & 0x3FE0`. The low byte of `0x3E57FC` supplies FIX.
If signed word `0x3E57A4` is positive, the routine decrements it once and
caps the signed FIX input to 16 before taking the low byte. Preserve this
countdown, the builder's return contract or both caller contracts, the enclosing
DMA submission and follow-on state, and the display-copy position in GS order.
The observed packet sets `FRAME_1=0xA0000`, `ZBUF_1=0x10A000118`,
`TEST_1=0x30000`, `ALPHA_1=0x6000000064`, `TEX1_1=0`,
`TEX0_1=0x2000000E68029180` and `PRIM=0x156`. When hybrid draws follow,
check which of these GS register values they inherit before removing the packet.
The packet selects the fixed CT32 display framebuffer at base 0, width 640,
with a 640×448 destination rectangle, point-sampled CT32 source, DECAL with
source alpha, and no Z writes. For the observed state, each RGB byte is
`Cd + (((Cs - Cd) * FIX) >> 7)`, using the previous display target contents.
Reproduce GS clamp/wrap and output alpha rules. UNORM fixed-function blending
can round differently from these integer operations.

**Typed display copy.** The EE hook at the live caller retains the original
builder for the guest countdown, return value and portable reference packets.
The matching 88-qword VIF1 DIRECT executes a Vulkan composition command in its
place. The 640×448 integer blend reads the scene texture and a GPU snapshot of
the previous CT32 display target. It uses ordinary GS flip accounting and the
existing `DISPFB` scanout. A skipped frame reuses existing display history.
The original packet remains available for other callers, unsupported GS state
or aliases, and `EOD_NATIVE_DISPLAY_COPY=0`. The scaled path also keeps the
legacy packet if the scissor extends below the 448-row retail target.

The original sprite's XYZ offset starts at −15/16 GS pixel with UV zero. The
Vulkan vertex shader adds half a GS pixel, then nearest sampling floors the
scaled interpolants at each physical pixel center. This reduces to an offset
of `floor((8 + 7 * RenderScale) / 16)` texels on both axes. The final
scissor-edge slivers clamp sampling to the last physical texel. Destination
history always uses the unshifted physical pixel.

Apple M2 Pro at 1×, Dundas Battle, native-only Classic, reported 60 residual
strips before this pass and 40 after at GS frame 181. Frame 125 with FIX
`0x60` had a same-live-pass comparison of source and previous destination
readbacks against the composed bytes. All 1,146,880 CT32 bytes matched the
integer equation. The previous destination differed in 1,025,873 bytes.
Software replay of the original 88-qword copy separately matched the same
equation for all 1,146,880 CT32 bytes. The live trace observed FIX `0x80` at
boot and `0x60` in-race.

The 6× run also reported 40 strips at frame 181. Same-pass GPU checks at 1×,
2×, 3× and 6× used the GS sprite edge helper and the integer blend on readbacks
of both inputs. Each checked CT32 byte matched at steady FIX `0x60` and during
a forced countdown at FIX `0x10`. At 6× the check covered 41,287,680 bytes per
pass, including the first copy after FIX returned to `0x60`. The original
builder decremented the test countdown from 2 to 1 to 0. An actual skipped GS
frame at 6× preserved all 41,287,680 display bytes from the previous frame.

The fixed 640×512 scanout allocation now alternates its GPU attachments. The
shader reads the previous display directly while writing the next one. Only
the 64 rows outside the display rectangle are copied to preserve surrounding
GS storage. The scanout surface keeps its logical page ownership. The prior
full snapshot remains available with `EOD_NATIVE_DISPLAY_SNAPSHOT=1`.

Two consecutive live copies at 1× and 6× matched the integer blend for every
active pixel and preserved every trailing byte. At 6× the GPU-to-GPU history
transfer fell from 41,287,680 to 5,898,240 bytes per copy. The last reported
GPU-timing windows of two 960-vblank Dundas Modern runs measured 0.89 ms for
the full snapshot and 0.69 ms for alternating attachments. Both drew 59.9
frames per second in their last headless window. Fullscreen pacing and
end-to-end efficiency remain open.

**Ordered projected shadows.** The full-screen retail shadow pass uses
320×448 shadow storage for the 640×448 scene. Its color base is `0x33E0`
and its Z base is `0x2BE0`. During preparation, the guest temporarily
binds the Z base as FRAME, then binds the color base as FRAME before
shadow volumes. The EE reserve at `0x362A34` tags the 77-qword prepare
DIRECT, and the reserve at `0x362F34` tags the 89-qword composite DIRECT.
VIF1 carries each tag to its original PATH2 position. Plume flushes the
scene at preparation and the accumulated shadow volumes at composite.
The GS register writes and final RGBA, PRIM and UV state are retained.
Plume samples scene depth into its private shadow pass and composites onto
scene color without using the temporary GS pages. Portable replay retains
both original DIRECT packets. Hybrid mode, unrecognized layouts, and
`EOD_NATIVE_SHADOW_PASS=0` keep the original GS path.

On Apple M2 Pro at 6×, Dundas Battle (8 AI), Bus, native-only Classic
(`EOD_NATIVE_ONLY=2`), the four steady 60-vblank windows ending at
180, 240, 300 and 360 each went from 2,400 emulated GS raster primitives
and 180 Vulkan raster draws to zero. Deferred preservations went from
353–361 to zero, merges from 120 to zero, and preservation waits from
55–60 to zero. At level 1 the three shadow strip groups disappeared from
the dropped-primitive report. These checks cover the full-screen race
layout. VU1 execution and unsupported layouts still keep S4 open.

At 1×, exact native-only Dundas Bus pairs at vblanks 856 and 1056 measured
whole-frame RGB MAE 1.09, median p99 21 and mean SSIM 0.9805 against
software GS replay. The replay diagnostic isolated 44,545 shadow pixels.
Live MAE in that mask was 0.65 against normal replay and 33.29 against
replay with the shadow composite disabled. Gilroy Battle (8 AI), Limo, at
the same scanout boundaries measured whole-frame MAE 0.99. The blue channel
exceeded the strict threshold in both pairs. Across its 22,423 replay shadow
pixels, live MAE was 1.11 against normal replay and 31.67 against
composite-off replay. Strict P3 and the original PS2 comparison are still
open.

The two-player Dundas No-Rules Race direct-start control reached the
race and halted with `Didn't find player 0 in event 0` with the ordered
replacement enabled and disabled. Kenyon Battle reached the same panic.
The two-player lobby exercised both boundaries, but its replay showed no
pixels affected by the selected composite, so the split-screen race
shadow comparison remains open.

### R7 lifecycle counters

Set `EOD_R7_AUDIT=1` to count compatibility work from boot through GS worker
shutdown. The option defaults to off. A successful game exit emits one
`[r7-audit]` line after the VIF/GIF, GS and raster workers drain. Run
`python3 tools/r7_audit.py RUN.log [RUN.log ...]` on one combined-output log
per successful game process. An absent, malformed, duplicate or nonzero
summary fails the check.

`vu1` counts dispatches to compiled or interpreted VU1 microprograms.
`vu0_mlist` counts VU0 dispatches to entry `0x40` with MlistVisibilityCalc's
retail microcode block (the bytes from `0x40` through `0x117`, including the
E-bit delay slot). It excludes gameplay VU0 programs. Use a fresh native
run without `EOD_DIFF` or the offline reference for this counter. Logs from
before the field was added lack the required counter.
`gif_qwords` counts qwords consumed by the compatibility parser.
`gs_raster` counts primitives queued for hardware rasterization or
submitted to the software rasterizer. `gs_suppressed` counts primitives
that strict native mode would otherwise drop, including its compatibility
clear interception. It remains a separate gate when raster submissions
reach zero.

`sync_vram` counts synchronous VRAM copy/map requests. `deferred_wait`
and `clut_wait` count consumer paths that force a synchronous submission
or a fence wait for an earlier copy. Copies already complete when consumed
add no wait count. Run explicit screenshots and offline replay separately
from ordinary shipped-play checks.

Run the exact release candidate through fresh install and upgrade,
startup and menus, movie play and skip, loading, Career including save/load,
the race and split-screen rotation, netplay, return to menus, quit, and
reopen. Require zero graphics audit counts and the separate release build
graph, link map and package inspection for VU1 and GS exclusion. Verify
the packaged visuals, settings and content against the approved local
Mac build. Measure actual presented frame cadence on target displays at
fullscreen 6× for the 60 FPS gate and run the independent unpaced S5
workload for the 3.33 ms budget. Any other graphics-specific VU0 work
requires separate verification.

### R7 native world visibility

Replace MlistVisibilityCalc's VIF0 upload and local COP2 operations with CPU
arithmetic at `0x3582B8`, including the group LOD helper `0x357E60`.
The retail executable's chain at `0x3BD4D0` is a CNT tag with 24 payload
qwords and VIF command `0x4A300000` (MPG, 48 instruction pairs at address 0),
followed by an empty END tag at `0x3BD660`. MPG writes the VU instruction
memory only. Both EE launches of the visibility entry
are in this function, at `0x358804` and `0x358AD4`. The other VIF0 wait
callers at `0x1DE85C`, `0x1DFB14`, `0x1DFD8C` and `0x2EF784` clear the
channel 0 completion bit for their own request before waiting. They
install their own code for entry 0. The additional entry `0x20` call at
`0x1E00F0` also uses its enclosing routine's own MPG upload from
`0x3BC290`. The display-object callbacks and scratchpad DMA in the
epilogue remain part of the existing EE path.

| Source | CPU contract |
|---|---|
| `0x3584E8..0x358714` | View rotation columns are the transpose of the camera rows. Translation is the negated dot of the camera position with each row in X, Y, Z order. Preserve the four guest camera quadwords and the existing EE projection-factor call. |
| VU0 `0x0040..0x0068` | Accumulate centre in view space in column order X, Y, Z, translation. The plus and minus radius endpoints add or subtract `(0, 0, 1 / y)` times record W. Round products before adding. |
| VU0 `0x0080..0x00E0` | Each endpoint keeps unprojected Z as clip W. Multiply XYZ by `(1 / x, 1 / x, (far+near)/(far-near))` and add `far * (1-ratio)` to Z. Clip the plus endpoint first and the minus endpoint second using strict comparisons against ±abs(W). Depth is `max(1, 0 + minus.z)` from before projection. |
| EE `0x358810..0x358840`, `0x358AF0..0x358B20` | The two six-bit CLIP groups give `0` when any bit in `0xBD0` is set, `0x81` when any bit in `0x42F` is set, or `1` otherwise. Older CLIP history does not enter either mask. |
| EE `0x357E60` | For a group with a nonzero parent visibility byte, subtract group XYZ at +0x10 from camera XYZ. Square and sum in X, Y, Z order. Apply the original EE `(distance² + f12) * f13` and compare the three thresholds at +0x00/+0x04/+0x08 to its absolute value. Each threshold at or below that value contributes bit 0, 2, or 4 respectively. |
| Guest output | `0x70000000[0] = 0x81`, then spheres (stride 16), groups (stride 32) and models (stride 32). A model evaluates only if `(parent & 0x7F) == required` and writes depth for every evaluated model, including one rejected by clipping. A model skipped by its parent writes visibility 0 and leaves depth untouched. The depth index is the same combined sphere/group/model index as the visibility byte. |
| Guest aliases | Retain the `0x4D6F00` quadword copies from sphere `+0x20` and evaluated model `+0x40`, including aligned EE access and the associated EE `v0` high word. The destination ends immediately before the current-list pointer at `0x4D6F10`. Preserve the identity/matrix-table loop and its overwrite at entry `count`. |

The old sphere VF31 store at stack `+0x80` had no consumer and the
group count and pointer overwrite its first eight bytes. The model VF31
store at `+0xA0` only fed its X depth word, now written directly to the
guest depth table. Both stores and the old matrix temporaries were in
the callee's ordinary EE stack frame.

Use `EOD_DIFF=003582B8 EOD_DIFF_RAM_MB=128` to compare full guest RAM and
scratchpad, including the projection and camera globals, visibility and
matrix entries, the copied quadword, and stale depth words. When the native
function runs on the replacement side, the VF comparison ignores exactly
`vf1–5`, `vf20–24`, `vf26` and `vf28–31`. These are the registers
written by the retired camera setup, group clip and sphere/model microcode.
This applies if the function is nested inside a frame comparison too.
The generated EE uses VF20 and above in this cull routine alone. Other
VF registers, the checked EE registers, VU integer registers, and VU data
RAM still compare. The group reads its newly written low CLIP group, and the
three EE status-flag readers at `0x38B9F8`, `0x38BA78` and `0x38BB28` clear
the status register before use.

Use `EOD_MLIST_VISIBILITY_BENCH_CHECK=1 eod --vu-bench CAPTURE` to compare
the two low CLIP groups and the raw VF31 depth bits against the reference
on recorded VU inputs. A different image at that entry, a comparison
failure, or no matching records fails the check. Repeat with
`EOD_VU_INTERP=1` for the interpreter reference.

Sources: `gen/rec_100.c`, `gen/rec_099.c`, the retail VIF0 tag and VU0
instruction disassembly, and the CLIP definition in the
[Sony VU User’s Manual, §3.3.3](https://www.manuallib.com/download/VU-USER-MANUAL.PDF).

### R7 bootstrap/loading icon

The fixed display chain at EE physical 0x004D70C0 calls 0x35BE08 from
0x35E948 with return address 0x35E994. It adds a 15-qword CNT at 0x004D7650,
including the DMA/VIF header and 14 DIRECT qwords. The readable builder
uses center, radii and UV bounds at 0x4D7CD0, the phase word at 0x3E579C
and the signed quadrant lookup at 0x429BF0. It preserves the compatibility
packet and its return value. The builder supplies the typed quad to the
native renderer. Metadata is ordered with DMA even when the optional
VU1 worker is active.

The retail TEX0 describes a 1024×512 CT32 texture at 0x2300. Across the
fixed U/V interval 1–31, bilinear taps only touch a 32×32 CT32 region
with the original TBW and swizzle. A full texture decode can synchronously
read unrelated pages from the Z16S surface that shares that address.
The Plume path decodes the cropped texture only when all sampled blocks
are current in CPU memory and outside any pending preservation region.
Destination acquisition also rejects page ownership that would require
a readback or deferred merge. Unsupported packets and capture/replay can
use the compatibility path.

The old renderer's support flag is enabled by default even when that
renderer is idle. The bootstrap selector checks for a live old renderer
or an explicitly requested scene capture. Set EOD_NATIVE_BOOT_QUAD=0 to
run the reference raster path. Bootstrap history and clear experiments
retain their own selector.

### R7 ordered 2P shadow targets

The retail 640×224 views at y=0 and y=224 share the main frame and
Z buffer. From the decompiled `0x34C7D0` / `0x34C798` boundary,
`0x363218` saves the main viewport at `0x4D8C00` and sets the native
shadow target dimensions at `0x4D8BD8`. The 2P target is 320×224.
`0x362968` and `0x362E70` reserve the 77- and 89-qword DIRECTs at
RA `0x362A34` and `0x362F34` respectively (generated retail decompile
`gen/rec_101.c`). The stored main viewport tags both packets in order.

| State | Upper 2P | Lower 2P |
| --- | --- | --- |
| Prepare depth-copy V at first strip | 0 | 224 |
| Prepare scratch SCISSOR | 0,0–319,223 | 0,0–319,223 |
| Composite TEX0 (shadow mask) | `0x20000006242173e0` | same |
| Composite SCISSOR | 0,0–639,223 | 0,224–639,447 |
| Composite first packed Y | `0x71F0` | `0x7FF0` |

Both views use `XYOFFSET_1=0x71FF00006BFF`, and the prepare depth-copy
TEX0 matches the 640×448 layout. These values were observed in three
Kenyon split-screen captures and follow the register and strip formulas
at the two packet producers. The native renderer can keep color
accumulation private and sample main depth in place. It must match the
current main scissor and pair a composite with the accepted prepare for
the same view. The split path uses the existing projected volume math
and supports Classic and Modern, including when sun cascades are off.
Keep the existing 640×448 selection and unsupported packet fallback.

Restrict this dispatch to the retail `0x30A59C` ShadowBegin return site
with `0x3D1A74 == 2` and active viewport `0x3D1A78 == 0` or `1`.
`0x363218` saves the view for both `0x362968` and `0x362E70`, so both
reservations must target the same 224-row view. Drain pending shadow
meshes before preparing a view and flush its composite before allowing
the next view to change the main target. Distinct packet seals and
per-view counts expose a missing or mismatched half.

The 2P path is a diagnostic opt-in via `EOD_NATIVE_SPLIT_SHADOW_PASS=1`
while the moving lower-view comparison remains open. The full-screen
selector retains the original `EOD_NATIVE_SHADOW_PASS` and cascade gates.
A valid pair is the two tagged retail producers in one view and one GS
frame. If preparation falls back, the native composite falls back too.
Measured captures confirm all tagged pairs were completed. If a matching
composite were corrupted after its prepare was accepted, its original
scratch contents could not be reconstructed from the native target.
