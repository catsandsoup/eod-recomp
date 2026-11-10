# Track world format (GRAF scene, display objects, event props)

## Player success

A modder exports a retail track to OBJ/MTL with its real textures and every
object where the game draws it, opens it in Blender, moves one existing piece
(a ramp, a wall prop), writes a copy of the `.TRK` that differs only in that
piece's placement bytes, and drives the edited track in the native game with
everything else unchanged.

Status: export, previews and byte-exact write-back work (tests below). No
native game run has been done for an edited placement yet; see "Next decisive
test".

Tools: `tools/world_graf.py` (parser), `tools/world_export.py`
(`export` / `place`), tests `tools/tests/test_world_export.py`. Vertex-level
edits of an existing model (same topology) belong to `tools/vif_patch.py`
(another worker's tool); this document covers whole-object placement.
Collision is documented in `docs/COLLISION_AND_BOUNDARIES.md`
(`tools/world_collision.py`).

Confidence labels: **V** = loader/consumer code read and checked against all
retail data; **D** = derived from data consistency across all retail tracks
(no contradicting case found) but the consuming code was not fully traced;
**H** = hypothesis.

## Archive member ledger (all 15 retail `.TRK`)

Counts per directory FourCC (directory field `+0x20`, reversed).

| Track | members | TEX | VALT | ILIN | SFX0 | SETU | MODL | GRAF | SCOL | SOBL | BPPT | BSPT | STAB | LAYT | GTEX | IMAG | Anim | Anm0 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| AFTON | 262 | 203 | 37 | 9 | 3 | 2 | 2 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 |
| ANOKA | 369 | 311 | 37 | 7 | 2 | 4 | 2 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 |
| BAXTER | 330 | 274 | 38 | 7 | 2 | 1 | 2 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 |
| BEMIDJI | 290 | 229 | 38 | 7 | 1 | 6 | 2 | 1 | 1 | 1 | 1 | 1 | 2 | 0 | 0 | 0 | 0 | 0 |
| DENNISON | 302 | 244 | 36 | 7 | 1 | 6 | 2 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 |
| DUNDAS | 378 | 318 | 34 | 9 | 1 | 7 | 2 | 1 | 1 | 1 | 1 | 1 | 2 | 0 | 0 | 0 | 0 | 0 |
| EAGAN | 295 | 238 | 37 | 7 | 1 | 4 | 2 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 |
| EMPTYT_1 | 20 | 3 | 4 | 4 | 0 | 2 | 0 | 1 | 1 | 1 | 1 | 1 | 2 | 0 | 0 | 0 | 0 | 0 |
| GILROY | 298 | 242 | 36 | 7 | 3 | 1 | 3 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 |
| HASTINGS | 288 | 227 | 37 | 7 | 2 | 6 | 2 | 1 | 1 | 1 | 1 | 1 | 2 | 0 | 0 | 0 | 0 | 0 |
| HOMEBASE | 449 | 250 | 27 | 29 | 9 | 6 | 29 | 1 | 1 | 1 | 1 | 1 | 7 | 44 | 6 | 12 | 14 | 11 |
| KENYON | 289 | 230 | 37 | 7 | 1 | 5 | 2 | 1 | 1 | 1 | 1 | 1 | 2 | 0 | 0 | 0 | 0 | 0 |
| MANKATO | 334 | 279 | 33 | 7 | 1 | 6 | 2 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 |
| SPRING_1 | 275 | 221 | 37 | 7 | 1 | 1 | 2 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 |
| WELCH | 200 | 177 | 7 | 4 | 1 | 5 | 0 | 1 | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 |

| FourCC | Members | What it is | Loader evidence | Conf. |
|---|---|---|---|---|
| GRAF | `track.grf` | The whole visual world: display list, embedded models, two draw lists | `f_00214678` (requires directory version 18, "bad graf version %d") | V |
| TEX | `*.tex`, `*.tm0` | Indexed/direct textures for world, sky, props | `tools/texture_preview.py`, `docs/CREATION_KIT_TEXTURE_CONTRACTS.md` | V |
| MODL | `a_stfn.mod`/`b_stfn.mod` (start/finish), `sfshad_*.mod`, `blades.mod` (Gilroy), HomeBase UI/garage models | Stand-alone models placed by code, not by GRAF | `f_0034B720` | V (format), H (placement) |
| VALT | `track.val`, `shadow.val`, `obstacle.val`, `def_objects.val`, `camera*.val`, `c_<event>.va0` (27 on Anoka) | Tables; `c_<event>.va0` hold per-event prop placements (below) | `f_00134A70` | V |
| SCOL | `track.bsc` | Drivable terrain surface + optional-terrain bits | `f_00226768` ("bad scol version, run mkworld") | V (see collision doc) |
| SOBL | `track.sol` | Static collision boxes/tubes (invisible walls) | `f_001A00B8` | V (see collision doc) |
| BPPT | `track.bpp` | Tool-serialised partition tree ("Indiv BPPTree") | `f_00216F60`/`f_00217610` | H, preserved |
| BSPT | `track.bsp` | 108-byte one-triangle tree | `f_00216D98` | V (collision doc) |
| STAB | `track.obt`, `*.tab` | String tables ("Wrong string table version") | `f_00132E98` | V |
| ILIN | `*.ild`, `roadrace.ili` | AI racing lines; used here only as a preview overlay | `f_0013E070`, `tools/trk.py` | V |
| SETU | `*.gar` | Car garage setups | — | not world |
| SFX0 | `*.sf_` | Ambient sounds | `f_0033ACB8` | not world |
| LAYT/GTEX/IMAG/Anim/Anm0 | HomeBase only | UI layouts, fonts, garage characters | — | not world |

No `SOBL`/`SCOL`/`BPPT` record names a visual model, and GRAF names no
collision record: visuals and collision are separate and must be edited
separately.

## GRAF display list (track.grf)

Loader `f_00214678`: header word 0 = display-list root offset, words 1/2 =
two "mlist" draw lists. Root is relocated by `f_002156C8`, the tree by
`f_00214258` (fixup_display_list), then the display objects are constructed
by `f_00214468` (init_display_list). Every pointer in the file is an offset
from the start of the member (0 = null). **V**

```
node +0 u32 type, +4 u32 next sibling
type 0     LOD / group     +8 vec3 centre, +0x14 u32 count,
                           count x { +0x18+8i f32 maxDistance^2, +0x1C+8i u32 child }
type 0x101 condition       +8 child, +0xC s32 value
type 0x102 cull sphere     +8 child, +0xC vec3 centre, +0x18 f32 radius
type 0x103 condition       +8 child, +0x1C s32 value
type 4     leaf            +8 u32 model record (0 allowed)
other      Panic "init_display_list: unexpected display node type"
```

* Group children are LOD levels: in every multi-child group on Anoka (49),
  Spring (91) and Gilroy (21), triangle counts fall monotonically from
  child 0 to the last child, and thresholds read 50/100/300 m (Anoka),
  100/250 m/infinite (Spring). Child 0 = nearest/highest detail. Single-child
  groups carry a draw distance (400 m, 500 m, 1e7 m). **D** for the
  distance semantics (the per-frame mlist walk `f_003582B8` was not traced).
* 0x101/0x103: `f_00214468` includes the child when the value is not -500
  if session flag `0x3D1B34` (set when `f_00211150()+0xC == 2`) is set,
  otherwise when the value is -500 or >= 0. 0x102 has no condition. Which game
  modes those are is **H**. Retail use: Baxter 2, Hastings 2, Kenyon 1,
  Mankato 2, Dennison 1.
* Parsed counts (exact; Anoka matches the earlier independent traversal in
  `work/track-environment-research/findings.md`):

| Track | groups 0x0 | leaves 0x4 | 0x101 | 0x102 | 0x103 | simple | Optional Terrain | Axis/Point-Facing | Dyno (spawned/visible-unspawned) | other display objects |
|---|---|---|---|---|---|---|---|---|---|---|
| AFTON | 315 | 1907 | 0 | 286 | 0 | 1591 | 28 | 217 | 42/0 | 29 |
| ANOKA | 108 | 2517 | 0 | 39 | 0 | 1800 | 75 | 447 | 98/4 | 93 |
| BAXTER | 793 | 2943 | 2 | 1 | 0 | 2392 | 62 | 241 | 72/14 | 162 |
| BEMIDJI | 392 | 1960 | 0 | 1 | 0 | 1468 | 66 | 162 | 87/25 | 152 |
| DENNISON | 198 | 1523 | 0 | 128 | 1 | 1359 | 13 | 69 | 44/1 | 37 |
| DUNDAS | 256 | 2488 | 0 | 1 | 0 | 2095 | 63 | 41 | 286/1 | 2 |
| EAGAN | 647 | 1595 | 0 | 3 | 0 | 1422 | 60 | 28 | 72/4 | 9 |
| EMPTYT_1 | 0 | 1 | 0 | 0 | 0 | 1 | 0 | 0 | 0/0 | 0 |
| GILROY | 141 | 3858 | 0 | 2 | 0 | 3064 | 169 | 329 | 170/32 | 94 |
| HASTINGS | 82 | 1773 | 1 | 244 | 1 | 1428 | 39 | 159 | 42/7 | 98 |
| HOMEBASE | 758 | 3503 | 0 | 0 | 0 | 3176 | 56 | 213 | 21/0 | 37 |
| KENYON | 219 | 2297 | 1 | 176 | 0 | 2009 | 9 | 164 | 0/8 | 107 |
| MANKATO | 72 | 3290 | 2 | 0 | 0 | 3186 | 21 | 56 | 25/0 | 2 |
| SPRING_1 | 1067 | 2155 | 0 | 0 | 0 | 2055 | 41 | 0 | 50/0 | 9 |
| WELCH | 113 | 2264 | 0 | 0 | 0 | 1885 | 0 | 364 | 0/0 | 15 |

No two leaves share a model record or an embedded model (no instancing in
GRAF). **V** (checked on all tracks by the parser: one model per leaf).

### Model record (leaf +8)

```
+0x00 u32 kind            0 = simple model, 1 = display object
+0x04 u32 runtime handle   stale tool pointer in file; f_0034BB10("graf") writes it
+0x08 u32 model offset     relative to the record (fixed up by f_0034B530 "simple_model")
+0x0C u32 0
+0x10 u32 subtype          kind 1 only; index into ELF table 0x406B40 (34 entries)
+0x14 u32 index            running index within the subtype
+0x18.. subtype-specific
```

Subtype names/constructors from 0x406B40 (name, ctor, pool size): Simple
Model, Point-Facing, Axis-Facing, Dyno 0x221E80, Sky 0x221E00, Multi-Facing,
Old Flag, Rotator, Crowd, Sun, People, People-Zone, Animation, Pit-Arm,
Unknown, Ambient, Shadow, SunBright, FogColor, NamedPoint, Optional Terrain
0x221BC8, Old Banner, Linear Path, Linear Drone, Leader Board, Cross Fader,
Flare Source, Fireworks, Water, Effect source, Hermite path, Hermite drone,
Fabric, Facing Test. Fixup calls vtable slot 3, init slots 4 and 11
(`f_00214468`). **V**

Subtype-specific fields used by the exporter:

| Subtype | Field | Meaning | Evidence | Conf. |
|---|---|---|---|---|
| Optional Terrain (20) | +0x2C s32 | 1-based event mode; fixup turns it into `1<<(mode-1)` | `f_002230F8` | V |
| | +0x18 / +0x1C u32 | entry index toggled in mlist B / mlist A | `f_00223008` → `f_00359248(graf[1], +0x1C)`, `(graf[2], +0x18)` | V |
| | draw | drawn only when `session(0x495BE0)+0x3C & mask` | `f_00223090` | V |
| Dyno (3) | +0x2C s32 | spawn mode bit (1-based); -1 = never spawned | `f_00223128` | V |
| | +0x30 u32 | dyno kind, jump table 0x408560 (0..7) | `f_00225808` | V |
| | +0x34 | tag `'dyno'` | data | D |
| | +0x38..+0x67 | 4x3 frame: rows X,Y,Z axes then position (m) | copied whole into the WobbleObject init by `f_00225330` (kind 5) / `f_002256A8` (kind 7) | V (read), D (row convention: guard-rail proxies are spaced along row 2, slope matches row 2 y) |
| | +0x68..+0x73 | 3 f32 copied to init +0x58/+0x5C/+0x60 (0.1, 4.0, length) | same | V (copy), H (half extents) |
| | +0x20..+0x28 | 3 f32, e.g. (-244.8, -718.4, -64.8) on unspawned Dynos | not read by any traced path | unknown |
| Point/Axis-Facing (1/2) | +0x20 vec3 | equals the model centre | data | D |

Retail Dynos: every spawned one (mode >= 0, kind 5) has an **empty model**
(0 parts): they are invisible physics proxies (guard-rail wobble boxes).
Every Dyno with geometry has mode -1 (never spawned, never drawn: Dyno's draw
slot is a no-op). They are excluded from the OBJ unless
`--include-unspawned`.

### Embedded model (mrModel)

```
+0x00 nParts  +0x04 partTable  +0x08 nExtras  +0x0C extraTable ('NBRM'/'TJRM')
+0x10 nInstances (must be > 0) +0x14 instanceTable   (offsets relative to model)
+0x18 f32 radius  +0x1C vec3 centre  +0x28 bbox minX,maxX,minY,maxY,minZ,maxZ
part = 4SPS (texture[16] at +8, group at +0x18, VIF from +0x38), as tools/vif_mesh.py
```

`f_0034B530` (V). Stand-alone `.mod` members (track/RES props) use the same
header (`obswall.mod`: parts at 0x58). `vif_mesh.parse_modl` assumes the
part table at 0x58; embedded models put it at `+4` (0x50/0x54), so the world
parser reads `+0/+4` instead. Suggested shared fix: make `parse_modl` use
header words 0 and 1.

World VU programs: MSCAL 0x4E (2644 parts on Anoka), 0x6A (447, facing
sprites), 0x55 (10). For all of them every decoded vertex lies inside the
model's header bbox (checked on Anoka). **Positions are world space**: the
static models' header centres are world coordinates (e.g. `light02` at
(-4.2, 0.4, 53.0)), and each batch's qword 199 (`UNPACK V4-32 @198`, second
qword) holds the world offset that the VU adds to every position (e.g.
(-2056.43, -127.99, -4051.0) with rows 2048/128/4096). There is no
per-instance transform for static or display-object geometry.

Facing sprites (0x6A) store each pair of quad corners at one point; the VU
widens them toward the camera. The exporter rebuilds the authored quad from
the model bbox width and each corner's U (Point-Facing also from V). **D**
(bbox = authored quad), runtime expansion not decoded.

### mlist draw lists (header words 1 and 2)

Self-relative; relocated by `f_00358F50`; drawn by `f_003582B8` ("mlist:
calc") with word 2 chosen when `0x3D1B0C != 0` (H: split-screen/low detail).

```
+0x00 0x94  +0x08 flags (|1 after relocation)  +0x0C -1
+0x10 count / +0x14 offset   16-byte spheres = copies of the 0x102 nodes (40 on Anoka)
+0x18 count / +0x1C offset   32-byte group records {maxDist^2, 4e16, 4e16, 0, centre, 0}
+0x20 count / +0x24 offset   16-byte per-model spheres {centre, radius} (exact copies of model headers)
+0x28 count / +0x2C offset   12-byte batches {u32 id, u32 qwc<<16, s32 rel ptr into a 4SPS VIF stream}
+0x30 count / +0x34 offset   0x1C-byte texture records {name[16], u32, u32 first, u32 end}
+0x8C count / +0x90 offset   unknown (0xE3 fill on Anoka)
```

Batches point into the same VIF data as the display list (2443 of 2934 on
Anoka land inside a leaf part; the rest are unexplained). Editing a VIF
qword 199 therefore changes what the mlist draws. Unknown: the table between
the per-model spheres and the batches (records like {vec3, f32} and
{0x10014, 0, 0, 0} pairs), the meaning of batch `id`, the texture record's
first u32.

## How terrain, road, props and ramps are told apart

* There is no road/terrain flag in GRAF. Road, terrain, buildings,
  bridges and fixed scenery are all kind-0 simple models in world space;
  only texture names separate them (`an-road1`, `an-grs02`, ...). **V**
  (absence of a type field).
* **Ramps and other event-only geometry are Optional Terrain** display
  objects (subtype 20), switched by event mode. On Anoka all 27 objects that
  use `Ramp*`/`rampdrt1` textures are Optional Terrain with modes 2/3/4/6.
  Their collision is in SCOL with matching per-triangle bits (collision doc,
  mechanism 4). `placements.json` flags `rampHint` from the texture names.
* Billboards: Point/Axis/Multi-Facing. Sky: one Sky object
  (camera-relative; excluded unless `--include-sky`).
* **Movable event props** (walls, cones, hay bales, tanks, trailers,
  ramps like `W_Jump4`) are not in GRAF. They are placed per event by
  `c_<event>.va0`.

## Event props (c_<event>.va0 + obstacle.val)

Each `$NNNN$object` section has `dyno` (prop name) and `unfloat_frame64`.

* `unfloat_frame64` is 64 symbols over the ELF alphabet at 0x3CCFC0
  (`a-z A-Z 0-9 , .`). The encoder `f_00133A50` emits 4 symbols per 3 bytes,
  most significant 6 bits first; the 48 bytes are 12 LE f32 = 4x3 frame
  (rows X,Y,Z then position). **V** (encoder read; all 2697 Anoka frames
  decode to orthonormal rotations and re-encode bit-exactly).
* The prop name resolves to a `dyno`/`dyno_<x>` section of `obstacle.val`
  (RACE.RES, then the track's own, later wins: **H**), giving `model`,
  `type`, dimensions. A `dyno` without `model` takes it from the preceding
  template section (e.g. `base_fun_barrel`): **H**. Composite dynos
  (`haywagon`, `barrel_stack`, `smtire_stack`, `small_bale_wall`) are not
  expanded yet (no geometry).
* Physics local frame: `conc_wall_phys2` carries a `@local_frame` with
  basis_x=(0,0,-1), basis_z=(1,0,0). Applied to `wall`/`wall2`/`arrow_wall`/
  `obwal1` (type wall, no own `dx`), the wall chains become continuous
  barriers; without it every wall stands across its chain (Anoka, 662 walls:
  chain tangent · row 0 = 0.88, · row 2 = 0.25 while the model is long in Z).
  Hay bales, sawhorses and mike barriers already line up without it.
  `world = eventFrame x localFrame` is **D**; the selection rule (same-name
  template, or a `conc_wall_phys*` template for type wall) is **H**.
* Runtime settling (`drop_height`, physics) is not modelled; frames are
  exported as stored.

## Coordinates

Native units are metres, +Y up, **left-handed**: +X right, +Z forward.
Evidence: the Anoka soda machine (`sodavend.tex`, model `i0489`) rendered
under a right-handed reading shows "SODA POP" and the coin strip mirrored;
under the left-handed reading it is correct. Placement matrices are rigid
(det +1) in native space.

* OBJ default `native`: coordinates verbatim (mirror-image in right-handed
  viewers).
* `--rh`: (x, y, -z), winding reversed — right-handed Y-up (glTF, Maya).
* `--blender`: (x, z, y), winding reversed — Blender Z-up, native forward
  (+Z) becomes Blender +Y. Use Blender's OBJ importer with Forward = Y,
  Up = Z (no further rotation).
* `placements.json` always uses native space. `transform` is a row-major
  4x4 for column vectors (`p_world = M p_local`) built from native rows
  r0,r1,r2,p as `world = x r0 + y r1 + z r2 + p`.
* Top-down previews are drawn from +Y with +X right and +Z up, i.e. an
  unmirrored map of the left-handed world, with the largest ILIN line in red.

## Export (`world_export.py export`)

```
python3 -I tools/world_export.py export --disc DISC --track DATA/TRACKS/ANOKA.TRK --dest DIR \
    [--lod N] [--props-only|--terrain-only] [--blender|--rh] [--event c_jump_race] \
    [--include-sky] [--include-unspawned] [--preview PNG|--no-preview]
```

Writes `<track>_world.obj/.mtl`, `textures/*.png` (mip 0 via
`texture_preview.decode_preview`; lookup = track archive, then
HOBBYSET/RACE/PART/COMMON/GENESIS .RES, `name` before `<stem>.tm0`, as in
`modding_backend.resolve_material`), `placements.json`,
`preview_topdown.png` (central 96 % of objects), `_full.png`, and with
`--event` `_event.png`. One `o`/`g` per instance named
`<member>@<modelOffset>_i<index>` (event props `<va0>_<dyno>_e<index>`).
`map_d` is written when a palette alpha is below 255; note that many opaque
terrain textures carry palette alpha < 255 (e.g. `an-grs02` 67–190), which
the native renderer may not treat as transparency (texture `flags` are in the
MTL comment and placements.json).
`--lod N` keeps child `min(N, count-1)` of every multi-child group.
`--terrain-only` = static + Optional Terrain; `--props-only` = everything
else except sky.

`placements.json` per instance: `id`, `name`, `assetId`
(`<archive>#track.grf`, or the prop model `<archive>#<member>`), `modelId`
(`<archive>#track.grf@0x<modelOffset>` for embedded models), `category`
(static, optional-terrain, billboard, display-object, sky, dyno,
dyno-unspawned, event-prop), `subtype`, `lodPath`, `cullSpheres`,
`conditions`, `transform`, `confidence`, `editable`, `bbox`, `pivot`,
`triangles`, `textures`, `rampHint`, `optionalTerrain`, `dyno`, and `source`
offsets (member- and archive-relative) for write-back.

### Export results (2026-11-10, retail disc)

| Track | instances (selected) | objects in OBJ | triangles | textures (exported) | unresolved |
|---|---|---|---|---|---|
| Anoka | 2517 (2441) | 2338 | 52 921 | 156 (155 + 1 untextured) | 0 |
| Anoka + `c_jump_race` | 2681 (2603) | 2500 | 59 697 | 176 (174 + 2 untextured) | 0 |
| Gilroy | 3858 (3791) | 3620 | 74 557 | 120 (119 + 1) | 0 |
| Baxter | 2943 (2850) | 2762 | 58 745 | 162 (161 + 1) | 0 |

"Untextured" = parts whose texture name is empty or `.tex` (power lines,
wires); they get a flat grey material.

Previews (`work/claude-2026-11-10/world-*/preview_topdown*.png`, ignored):
Anoka shows the road from the south-west, the long straight north, the
highway overpass and the figure-8 arena with its stands; the `oval.ild`
racing line lies on the road and around the arena for its whole length.
Gilroy shows the winding road, pond, airfield with aircraft and the oval
stadium, racing line on the road. Baxter shows the desert road, power lines,
a crossroad and the figure-8 stadium, racing line on the road. With
`c_jump_race` the Anoka arena shows a closed wall circuit with hay bales,
cones, a tank and a trailer.

## Write-back (`world_export.py place`)

```
python3 -I tools/world_export.py place --disc DISC --track DATA/TRACKS/ANOKA.TRK --edits edits.json --out NEW.TRK
edits.json: {"edits": [
  {"instance": "i1352", "translate": [3, 0, -2]},              # static / Optional Terrain
  {"instance": "i<n>", "matrix": [16 floats]},                  # spawned Dyno (category dyno)
  {"event": "c_jump_race", "prop": "e0003", "translate": [0, 0, 4]},
  {"event": "c_jump_race", "prop": "e0003", "matrix": [16 floats]}]}   # transform as exported
```

The output is the source archive with the same size and layout. Only the
edited bytes and the edited members' directory CRC (`eod_crc`, entry +48)
change; every other member is compared byte for byte before the file is
renamed into place. A zero edit gives a byte-identical archive. Output inside
the disc folder is refused.

| Target | Bytes written | Verified | Refused |
|---|---|---|---|
| Static / Optional Terrain translate | every batch's qword-199 x/y/z (`+= d`, only non-zero axes), model centre and bbox (+0x1C..+0x3F), matching per-model sphere copies in both mlists; ancestor 0x102 spheres only grow | VU offset mechanism (vif_mesh VU1 analysis); decoded vertices move by exactly `d` (test) | rotate, scale (positions are quantised per batch; needs `vif_patch.py`) |
| Spawned Dyno matrix | 48 bytes at record +0x38 | read by `f_00225330`/`f_002256A8` | non-rigid matrices, unspawned or non-Dyno targets |
| Event prop | the 64-byte `unfloat_frame64` string in `c_<event>.va0` | encoder `f_00133A50`; round-trip test | scale; composite internals |
| Sky, billboards, other display objects | — | — | all edits |

Known effects not handled: collision does not move with a static or
Optional Terrain translate (SCOL triangles, SOBL boxes; use the collision
tools); LOD group centres stay put; the optional-terrain collision for a
moved ramp stays at the old place. Event props carry their own physics, so
their collision moves with the frame (H, not run).

## Unknown bytes (explicit)

* Model record +0x0C (always 0 seen), +0x18/+0x1C for subtypes other than
  Optional Terrain (0xE3E3E3E3 or ints), Dyno +0x20..+0x28 and +0x70..+0x73,
  Point/Axis-Facing +0x24.. after the anchor.
* mlist: table between per-model spheres and batches, batch `id`, texture
  record first word, `+0x8C/+0x90` table, which game state sets `0x3D1B0C`.
* Embedded model: instance records (16 bytes, zero in file), header
  +0x40..+0x4F (e.g. 0x184, 0x84, 0x74 on `light02`), part-table tail entries
  beyond nParts (e.g. 0x280, 0x380 on `trshbin2`).
* 4SPS header +0x1C..+0x37 (vif_mesh: "counts / a float").
* 0x101/0x103 condition values (1, 2) → which game modes.
* Optional Terrain / Dyno mode numbers → event names (Anoka OT modes
  2,3,4,6; the collision doc maps `opt_terrain` masks per event).
* `track.bpp`, `track.obt` contents (preserved, not used).

## Next decisive test (native)

1. `place` Anoka with `{"event":"c_jump_race","prop":<a wall on the main
   straight>,"translate":[0,0,4]}` into an overlay, run the jump race with
   `EOD_MOD_DIR`, capture the wall at its new place and drive into it
   (physics follows the frame?).
2. `place` Anoka Optional Terrain ramp `i1352` by `[3,0,-2]`, run the event
   whose mode enables it: the ramp must draw at the new place (visual) and
   the car must still hit the old ramp surface (collision unchanged) — this
   confirms both the VIF-offset write-back and the documented limit.
