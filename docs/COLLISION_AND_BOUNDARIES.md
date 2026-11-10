# Collision, invisible walls and boundaries

## Player success

A modder can see why a car cannot jump over a wall or leave the course, find
the exact record responsible, lower it in a copy of the track, and confirm in the
native game that a car now clears that spot while everything else on the track
behaves as before. Inspection and the data edit are done; the native game check
in "Next decisive test" has not been run yet.

## Short answer

On every retail track except EmptyTrack, the "invisible walls" are static
collision boxes in `track.sol` (SOBL). Most of them are thin wall slabs with
a half-height of **15.0 m**, centred about 1 m above the road. Their top edge
sits about **16 m above the ground**, while the visible concrete is about
1–2 m tall. On Anoka, 574 of the 709 boxes reach 10 m or more above the
terrain (maximum 18.67 m). The engine applies no separate height limit:
the broadphase ignores Y, so the box itself is the limit. Lowering a box's top
in the data raises the limit at that spot. `tools/world_collision.py set-limit`
makes this edit safely.

## Mechanism ledger

Confidence: **H** = loader/consumer code read and checked against all retail
data; **M** = code path identified, but some fields or effects are inferred;
**L** = lead only.

| # | Mechanism | Where | Data-driven? | Confidence |
|---|---|---|---|---|
| 1 | Static solid boxes/tubes (`track.sol`, SOBL v3) | loader `f_001A00B8` from world setup at `0x00176BC4`; global `0x004473FC`; broadphase `f_00177CA0` | yes, per record | H (layout), M (narrow-phase response) |
| 2 | Event wall props (`wall`, `wall2`, `arrow_wall`, `obwal1`, `tank`, `dumper`, `Semi_Trailer`) | placed by `c_<event>.va0` `dyno` + `unfloat_frame64`; dimensions `dx/dy/dz` in `obstacle.val` (RACE.RES, plus the track's own); compiled by `SolBuilder::StaticObjectList` `f_001C0878` → setter `f_00179AF8` → `0x00447400`; tested by the same broadphase | yes (VALT) | M (half or full extent of `dy` not traced) |
| 3 | Terrain surface (`track.bsc`, SCOL v4) | loader `f_00226768` (`0x003D2098`); point lookup `f_00226F40`/`f_002268D8`; height `f_00227AE0`; surface `f_00227878`/`f_00227950` | yes | H |
| 4 | Optional terrain (ramps/crossings switched per event) | triangle bits `+0x1E` (`f_00227830`) AND event mask `0x003D1C70` (`f_00217220`, set by `f_00217230` from world object `+0x9CC` at `0x00230E04`); `opt_terrain` in `COMMON.RES/<track>_eves.val` | yes | M (link from `opt_terrain` to `+0x9CC` not traced) |
| 5 | Event X/Z polygons `bound`, `ringout`, `safe_bound`, `play_area`, endzones | `c_<event>.va0`; strings `bound%d`, `arenabnd: … emptying bound`, `went out of bounds` (`0x004026A8`), `SoccerMaster: resetting ball - out of bounds` | yes (VALT) | M: 2D event rules (ring-out, scoring, AI limits), not collision solids. No collision use found. |
| 6 | Recovery countdown ("Get back in the race!") and auto-reset | `f_001B8370` evaluates; reset `f_001B63E8` (auto at RA `0x001B8884`, gate word `0x00485098`; SELECT at `0x0018EA04`) | no | H for gate/reset (docs/MANUAL_RECOVERY.md); predicates partly decoded (L) |
| 7 | Beyond the terrain mesh | brute-force lookup `f_00226F40` returns the least-outside triangle and extrapolates its plane; no wall found | no | M |
| 8 | `track.bsp` (BSPT, 108 bytes) | one giant ground triangle; segment query `f_00216D98` (docs/BATTLE_NAVIGATION.md) | not a limit | H |
| 9 | `track.bpp` (BPPT) | contains serialized Win32 heap pointers; "using bpp terrain" path not used by retail tracks seen here | opaque, preserved | L |
| 10 | Global Y ceiling / world clamp | none found in the SOBL broadphase, the SCOL lookup or the recovery gate | — | L (search was not exhaustive) |

### 1. SOBL static solids: the invisible walls

`f_001A00B8` loads member `track.sol` of type SOBL (directory version 3). It relocates
three pointers and builds a shape in place in every record with `f_001A18E8`.

```
header  u32 count, u32 index_len, 3 x u32 runtime pointers (0 in file)
record  0x108 bytes, count of them, starting at 0x14
  +0x00 9 x f32 rows: local X, Y, Z axes in world space (row convention: neighbouring
        wall segments are spaced along row 2 with 0.976 alignment vs 0.68 for columns)
  +0x24 vec3 centre (m)
  +0x30 s32 link (-1 normally; stored by the ctor, meaning unknown)
  +0x34 u32 surface_type → shape+0x18 via vtable+0x2C (0x001A1EE8)
  +0x38 u32 tag: 'BOX ' f_001A06D0, 'TUBE' f_0019E368, 'POLY' f_001A03A0,
        'SPHR' f_001A15F0, 'RING' f_001A0518 (retail uses BOX and TUBE only);
        tag 0 → no shape, surface 1000
  +0x40 shape object (Win32 tool image; vptr moved to shape+0x1C on PS2)
BOX     shape+0x20 rx, +0x24 ry, +0x28 rz half extents (m), +0x2C sphere_radius,
        +0x30 box_flags. Names come from serializer f_0019E820 ("rx", "ry",
        "rz", "sphere_radius", "box_flags", "spring", "friction", "type",
        "surface_type"). sphere_radius = |(rx,ry,rz)| + 0.2 in every retail box
        (tested).
TUBE    shape+0x20 radius, +0x24 half_length along local Z; two cached
        end spheres at shape+0x28/+0x78 (local and world centres)
index   u16[index_len] record numbers
tree    8-byte nodes {u16 first, u16 end, u16 child (0 = leaf), u16 0}
```

Surface ids come from the name table at `0x004090F0`: default 100, concrete 101,
metal 105, plastic 106, rubber 107, wood 108, wall 110, hay 111. Retail
records use 110 (wall), 100, 107, 108, and 0.

Broadphase `f_00177CA0`: for each moving body it multiplies X and Z by 10
(decimetres), starts at root bounds ±40,000,000 and descends with
`child + 2*(Z>midZ) + (X>midX)`. It calls the dynamic shape's collide
(vtable+0x14) against every static shape listed on each visited node. **Y is
never tested here**, so a box's vertical extent is the only height limit. Shapes
are registered with up to ~4.1 m of slack around their footprint (measured on
Anoka). That is why edits are shrink-only: a smaller box stays inside the
cells it was registered in.

Retail census (all exact; `world_collision.py census`):

| Track | records | BOX | TUBE | index | nodes | SCOL tris |
|---|---:|---:|---:|---:|---:|---:|
| Afton | 1245 | 1203 | 42 | 2635 | 3477 | 9519 |
| Anoka | 713 | 709 | 4 | 1540 | 1537 | 4930 |
| Baxter | 995 | 492 | 503 | 1998 | 1657 | 5455 |
| Bemidji | 626 | 597 | 29 | 1411 | 1573 | 4622 |
| Dennison | 903 | 750 | 153 | 1923 | 2125 | 6600 |
| Dundas | 764 | 747 | 17 | 1676 | 1705 | 4834 |
| Eagan | 783 | 643 | 140 | 1582 | 1905 | 5446 |
| EmptyTrack | 0 | 0 | 0 | 0 | 1 | 2 |
| Gilroy | 1268 | 987 | 281 | 2512 | 2885 | 6148 |
| Hastings | 851 | 616 | 235 | 1875 | 2253 | 4938 |
| HomeBase | 1520 | 1446 | 74 | 2798 | 2965 | 11434 |
| Kenyon | 1102 | 842 | 260 | 2134 | 2209 | 7098 |
| Mankato | 1703 | 1423 | 280 | 3536 | 3433 | 9167 |
| Springfield | 926 | 926 | 0 | 1770 | 2525 | 11416 |
| Welch | 1457 | 1457 | 0 | 2983 | 3669 | 7021 |

Not yet verified: what the `box_flags` bits do (retail values 0–3; on Anoka's
outer wall the two overlapping layers use flags 2 and 3), and how the car's
narrow phase responds (bounce, friction or damage).

### 2. Event wall props

Each event's `c_<event>.va0` places props of type wall. Their dimensions come
from `RACE.RES/obstacle.val`: `wall` and `arrow_wall` use dx .1, dy 1, dz 5;
`wall2` uses dy 3.096; `obwal1` uses dy 2.58; `tank` uses 8.408/3.026/2.505;
`Semi_Trailer` uses 12/3.75/3. At run time `SolBuilder` builds these into a
second static set at `0x00447400`, which the same broadphase tests (second
loop at `0x00178068`). The `dx/dy/dz` values are read into a descriptor at
+88/+92/+96, by `f_0022F1B0` (near `0x0022F678`). Whether they are half or full
extents is not traced. `list-limits` reports them; `set-limit` refuses to edit
them.

### 3–4. Terrain (SCOL) and optional terrain

```
header u32 count, u32 table1 (=12), u32 table2
table1 32 B: 3 x {i16 nx, i16 nz, f32 d}  inside ⇔ nx/32767·x + nz/32767·z + d ≥ 0
             3 x i16 neighbour (-1 open edge), u16 optional-terrain bits
table2 20 B: f32 a,b,c,d  (y = -(a·x + c·z + d)/b), i16 surface id,
             u8, u8 (per-triangle bytes read by f_002277E8; meaning unverified)
```

Every retail triangle rebuilds from its three edge lines with the centroid
inside (tested). Terrain surface ids seen: 0, 2, 5, 6, 14, 20. The id→name
table for terrain was not found, so the exports colour them by id. A triangle
whose bits are non-zero is used only when `bits & mask(0x003D1C70)` is
non-zero. On Anoka the bits are 2 (793 triangles), 4 (64) and 6 (28). The events
set `opt_terrain` as follows: fig8_jump 3, fig8 6, knockout 2, demo_derby 4,
shortcut 7.

## Editable fields

| Field | File | Units | Tool support |
|---|---|---|---|
| BOX `ry` (half-height) | track.sol record +0x64 | m | `set-limit --field ry` (shrink-only) |
| BOX top above ground | derived: ry and centre (+0x24) | m | `set-limit --field top-above-ground` (keeps the bottom face; recomputes sphere_radius at +0x6C) |
| BOX `rx`, `rz`, `box_flags`, `surface_type`; TUBE | track.sol | m / bits / id | read and exported; edits refused (unverified effect) |
| `bound`/`ringout`/… vertices | c_<event>.va0 | m (X, Z) | read; edit through the existing VALT path (`modding_backend` save) |
| prop `dx/dy/dz` | obstacle.val | m (scale unverified) | read; VALT path after a native check |
| `opt_terrain` | COMMON.RES `<track>_eves.val` | bit mask | read |

The write-back copies the archive and keeps its size. It patches only the
edited records' centre (12 bytes), `ry` (4) and `sphere_radius` (4), and
recomputes the `track.sol` CRC in the directory. The tool checks that no other
byte changed. A no-op edit is byte-identical. It refuses to: grow a box; edit a
tilted box (|local Y · world Y| < 0.85); leave `ry` below 0.01 m; edit a
record whose sphere radius does not follow the |r| + 0.2 rule; or write
inside the disc folder.

## Recipes: raise or remove the height limit so cars can jump over object X

1. **Static wall (SOBL), data edit.** Run `list-limits <track>` or
   `export` and open `boundaries.obj` or `topdown.png` to find the records
   at the object. Then run
   `set-limit <track> --records A-B,C --field top-above-ground --value 1.5 --out <mod>/DATA/TRACKS/<TRACK>.TRK`
   and launch with `EOD_MOD_DIR=<mod>` (runtime/iop/stream.cpp merges
   `<mod>/DATA`). The bottom face stays put. If the original bottom was below
   ground, the result is a low kerb whose top is 1.5 m above the terrain at
   the box centre. To "remove" a wall, lower it until its top is below the
   ground. That is allowed when the requested `ry` stays at 0.01 m or more;
   for walls with a 15 m half-height the bottom is about 14 m below ground, so
   it usually is.
2. **Event wall prop, VALT edit (unverified scale).** Lower `dy` for that
   prop's `[dyno]` in an overlay `obstacle.val`, or remove or move the
   `[object]` in `c_<event>.va0` (placement is owned by world_export/H1
   tools). Native test first.
3. **Event ring-out or out-of-bounds polygon, VALT edit.** Move or enlarge the
   `boundN` vertices. These are 2D rules (elimination, scoring), not walls, so
   changing them does not let a car pass a solid.
4. **Runtime alternatives (not implemented).**
   - A global cap: wrap `f_001A00B8` and post-process its return value (`v0`,
     the relocated SOBL) with the same bottom-anchored `ry`/centre/sphere
     math, before world setup stores it at `0x004473FC`.
   - Per-shape disable: in `f_00177CA0`'s static loops (`0x00177F30` and
     `0x001780C8`), skip the vtable+0x14 call when the record pointer
     (`shape+4`) is in a disabled set.
   - Event props: post-process the set stored by `f_00179AF8`.
   - Automatic recovery is already switchable with
     `[Gameplay] ManualRecovery` (`f_001B8370` wrap).

## Tool

```
python3 -I tools/world_collision.py census
python3 -I tools/world_collision.py list-limits Anoka [--json]
python3 -I tools/world_collision.py export Anoka work/<dir>        # collision.obj/mtl, boundaries.obj/mtl, manifest.json, topdown.png
python3 -I tools/world_collision.py render Anoka out.png --bounds=-140,-80,110,75
python3 -I tools/world_collision.py set-limit Anoka --records 357-379,383-442 \
    --field top-above-ground --value 1.5 --out work/<dir>/mod/DATA/TRACKS/ANOKA.TRK [--dry-run]
python3 -m unittest tools/tests/test_world_collision.py         # retail: EOD_TEST_RETAIL_MODELS=1
```

Exports use native world coordinates (metres, Y up) and stay separate from the
visual GRAF export. In `collision.obj`, terrain is grouped by surface id and
optional bits, and solids by surface type (MTL `Kd`). `boundaries.obj`
contains line wireframes of every box, classed by height above ground
(≥10 m, 3–10 m, <3 m), plus every event polygon extruded vertically.

## Anoka preview (2026-11-10, CPU only)

Files are in `work/claude-2026-11-10/collision-anoka/`:

- `topdown.png` covers the whole map. A long point-to-point road leads
  north-west from a small oval. Both sides of the road and the whole outer ring
  of the oval are red, meaning their walls are at least 10 m above ground.
- `topdown_oval.png` is the oval close-up. The oval interior has no walls.
  The figure-8 and crossing optional terrain shows blue. The derby, knockout,
  wrangle, save-ring and red-rover polygons are drawn in the infield. Around the
  pit and paddock entrance there are yellow (<3 m) and orange (3–10 m) solids.
- `topdown_oval_edited.png` shows the oval's +Z straight after the edit:
  records 357–379 and 383–442, two overlapping layers, x −35…+35, z ≈ 61,
  lowered to 1.5 m above ground (yellow).

The preview is a data rendering. It does not prove what happens in the game.

## Next decisive test (native, not run)

1. Use the overlay `work/claude-2026-11-10/collision-anoka/mod`
   (`DATA/TRACKS/ANOKA.TRK`, sha256 `17ab559c…239a`, same size as retail,
   1,210 changed bytes, all in track.sol records 357–379/383–442 and its CRC).
   Run Anoka **Normal Race** (its event table places no props near that
   straight). Confirm from the HUD or a capture that the race runs on the oval.
2. Baseline without `EOD_MOD_DIR`, then with it. Get a car moving outward
   across the +Z straight at 1.5–5 m above the road: use a TurboBoost or
   chicken-blast launch, or write the car body position/velocity directly
   in a debug run.
3. Expected: in the baseline the car stops or bounces at the line z ≈ 61 even
   while airborne (an invisible plane up to about 17 m). With the overlay it
   passes over once it is more than about 1.5 m above the road. Below that it
   still hits the wall. Every other wall, for example the −Z straight, still
   stops it. Also record what happens beyond the terrain edge (no terrain for
   z > ~68): limit 7 predicts plane extrapolation, and the recovery countdown
   may fire.

## STAR handoff

**Situation:** players and modders see invisible walls far above the visible
barriers; earlier documents guessed at SOBL and SCOL.

**Task:** find every height and boundary mechanism, separate data from code,
and make collision inspectable and safely editable.

**Action:** traced the loaders, the broadphase, the shape serializer and the
terrain lookup in SLUS_209.10. Parsed all 15 retail tracks. Built
`tools/world_collision.py` with census, list-limits, export, render and a
shrink-only, byte-checked set-limit. Added synthetic and retail tests.

**Result:** the height limit comes from data (SOBL box `ry`, usually 15 m),
and the edit tool is verified at the byte level. Native player behaviour after
an edit is **not yet observed**. Still open: the `box_flags` semantics, the
`dy` scale of event props, the terrain surface names, and the
`opt_terrain` → mask link.
