# Modding Scope: Test Drive: Eve of Destruction (feasibility study)

> **Status: planning only.** Modding is parked until the macOS port of this PS2
> recompilation reaches 1:1 parity and passes the 2-player split-screen
> requirement (see `TDEOD RECOMP/GOALS.md` §0 and `docs/MODDING_IDEAS.md` there).
> Nothing here has been implemented. Every mod must be opt-in and must leave the
> default game unchanged.
>
> Research date: 2026-10-16. Data inspected: the owner's **Xbox** disc
> (`TDEOD RECOMP/game/`). The **PS2** disc (`SLUS_209.10` + `DATA/`) was not
> available, so every PS2 data claim below is a **guess** until it is checked.
> Format findings come from throwaway parsers in `/private/tmp/claude-501/eodmod/`
> (outside both repos). Nothing was extracted into a repo.

---


> **Note (2026-10-16):** this study was made from the Xbox disc only. The
> owner's PS2 files were in fact available (outside the repo) and the macOS
> build now runs them; PS2-side claims below are still unverified guesses.
> Modding stays parked until the macOS port reaches 1:1 parity.

## 0. Summary

**Main finding: almost all game content and rules are stored as data, in a
key/value table format we can already decode.** Event rules, AI tuning, car
physics, damage bones, prop placement, arena bounds, camera fields of view and
the driver roster are all `VALT` tables (§2.3). Graphics are DXT textures and
indexed meshes on Xbox. All of it sits in one simple archive format (`0TSR`,
§2.1). Code changes are needed only for these things: raising vehicle caps,
adding new event rule types, adding new rendering effects, and anything that
touches the PS2 GS/VU1 path.

**Engine lineage is confirmed. The closest sibling is NASCAR: Dirt to Daytona
(2002, PS2/GameCube), not Heat 2002.** Evidence is in §6.1.

| # | Idea | Feasibility | Rough effort* | Main prerequisite |
|---|---|---|---|---|
| F1 | **Mod overlay folder** (`mods/<name>/DATA/...` merged over `DATA/` in the virtual disc) | **High** | 1–2 days | none (runtime-only change in `runtime/iop/stream.cpp`) |
| F2 | `0TSR` archive unpack/repack tool | **High** | 2–4 days | verify that the entry hashes are not checked (§9) |
| F3 | `VALT` ⇄ text decompiler/compiler (round-trip) | **High** | 3–5 days | F2 |
| G1 | Higher internal resolution | **Done** on D3D11 (1–6×); needs to be redone in the Mac renderer | part of the port | Metal GS backend |
| G2 | Widescreen (true 16:9 field of view) | **High** if the PS2 build keeps the game's own widescreen mode; **Med** otherwise | 2 days – 2 weeks | PS2 ELF strings check (§1.2) |
| G3 | Texture replacement / HD pack (hash-keyed, at GS upload) | **Med-High** | 2–3 weeks | Metal backend, dump tool |
| G4 | Xbox-texture to PS2 "HD pack" (use the owner's Xbox DXT textures in the PS2 port) | **Med** | 2–4 weeks | G3 + PS2 `.tex` format known |
| G5 | New post effects (CRT filter, bloom, sharpening, AA) | **High** | days each | Metal backend |
| G6 | New per-material shaders (e.g. real environment maps, normal maps) | **Low-Med** | weeks–months | GS-level material identification |
| G7 | 60→120 FPS rendering with the simulation unchanged | **Low-Med** | weeks+ | game-loop reverse engineering |
| M1 | Re-tune existing cars (physics, durability, damage bones) | **High** | hours per car | F1–F3 |
| M2 | Re-skin cars / tracks (texture swaps in archives) | **High** on Xbox data, **Med** on PS2 data | days | F2 + PS2 `.tex` decode |
| M3 | Add a **new car** (new mesh) | **Med** | 3–6 weeks for the first car, then days | MODL exporter, bone rigging tool, slot/registry research |
| T1 | Edit existing tracks' events (props, bounds, laps, car counts) | **High** | hours | F1–F3, frame64 codec (done, §2.3) |
| T2 | **New track** (custom map) via importer | **Med-Low** | 2–4 months | GRF/SOL/BSC/BPP formats (partly unknown) |
| T3 | In-game/standalone track editor | **Low** (as a full editor); **Med** as a "Blender plus exporter" pipeline | months | T2 |
| A1 | Re-tune AI (lookahead, skill, speed factors) | **High** | hours | F3 |
| A2 | Auto-generate AI racing lines (`.ild`) for new tracks | **Med** | 2–4 weeks | `.ild` layout fully decoded (mostly done, §4) |
| A3 | Replace the AI | **Low** | months | deep code reverse engineering |
| MP1 | New multiplayer **variants** (existing rules, new parameters/props) | **High** | hours per mode | F1–F3 |
| MP2 | New multiplayer **rule types** (Reverse Only, Juggernaut, etc.) | **Med** (simple ones as HLE hooks) / **Low** (Chain Gang) | 1–4 weeks each | find player-input/damage functions in the PS2 ELF |
| MP3 | More than 20 vehicles; 3–4 player split screen | **Low-Med** | weeks–months | memory pools, AI arrays, VU1/GS performance |
| N1 | NASCAR Heat (PC 2000) **car** import | **Med-Low** | 1–2 months | MODL v? to v9 converter, re-rig damage bones |
| N2 | NASCAR Heat / Dirt to Daytona **track** import | **Low-Med** | 2–4 months for the first track | T2 + scale/AI lines |
| N3 | Physics value transplant (Heat `.val` to EoD `physics.val`) | **Med-High** | days | F3 (key names look shared) |

\*Effort is a rough single-developer estimate after parity is reached. It
assumes the existing recompilation tooling and gets cheaper with each repeat.

---

## 1. Graphics

### 1.1 What exists
* The PS2 port renders by emulating the PS2's Graphics Synthesizer (GS). It has
  a software GS and, upstream, a Direct3D 11 GS (`runtime/gs/d3d11/`) with an
  **internal resolution of 1–6×** (`RenderScale` in `runtime/core/config.cpp:109`)
  and an `AspectRatio` option (`config.cpp:108`). Right now the aspect option
  only stretches the 4:3 picture. The D3D11 backend is Windows-only, so the Mac
  port needs a Metal (or other) GS backend before any of G1/G3/G5 apply.
* The D3D11 texture cache is already **content-hashed**:
  `hash_level(tbp,tbw,psm,w,h)` at `runtime/gs/d3d11/gs_d3d11.cpp:899-912`. The
  software path keys on (tbp, tbw, psm, w, h, palette hash)
  (`runtime/gs/gs_draw.cpp:100-180`). That hash is the natural key for a
  PCSX2-style texture dump/replace system.
* The VU1 microcode is compiled ahead of time (`config/vu/*.bin`,
  `tools/vurecomp.py`). Geometry reaches the GS already transformed, so
  "new shaders" can only work on GS primitives (screen-space triangles with
  texture/alpha/fog state), not on game materials.

### 1.2 Widescreen: the game already has it (on Xbox)
The Xbox XBE contains `Widescreen (16:9)`, `+Widescreen (%1.2f)`,
`mr: widescreen mode`, `mr: letterbox mode`, `Aspect_constant`,
`Aspect_ramp`, `fov_a`, `new_fov`, `mirror_fov: …` (from `strings default.xbe`).
So the shared engine has a real 16:9 projection path. **If the same code and
strings are in `SLUS_209.10`**, widescreen is a matter of forcing that mode on
(an HLE `wrap` on the option getter, or a one-word memory poke at boot) and
widening the GS display output. Cost: about 2 days. If the PS2 build compiled it
out, the fallback is to patch the projection setup: find the function that
builds the projection from `fov` (camera tables hold `fov = 67.50` etc., see
§2.3) and HLE-`wrap` it to scale the horizontal FOV. The HUD would also need to
be pinned to the screen edges, either by finding the 2D overlay draw routines or
by a GS-level heuristic. Cost: 1–2 weeks. **First check once the PS2 disc is
present:** `strings SLUS_209.10 | grep -i widescreen`.

### 1.3 Hooks: HLE wrapping vs data edits
* **HLE** (`config/hle.txt`: `<EE address> wrap|replace`, implemented in
  `src/hle_*.cpp`; e.g. `0033D4F8 wrap # StrmFindFile`). Best for: forcing
  options (widescreen, FOV scale, frame-rate pacing), logging which texture or
  model name is loaded (for mapping GS hashes back to `.tex` names), and
  replacing whole functions. Cost is low per hook. The real work is finding the
  addresses: only 30 symbols are named so far (`config/symbols.txt`).
* **GS-level** (renderer). Best for texture replacement, upscaling, post
  effects and AA. It does not depend on game code. Risk: the game reinterprets
  depth buffers as colour for shadows (README §How it works), so replacement
  must skip render-target textures.
* **Data edits** (overlay + archive repack). Best for camera FOV tables,
  texture swaps in the game's own format, and LOD distances (`lod.val`:
  `lod1 = 10 … lod4 = 500`). Raising the LOD distances is a cheap, faithful
  "draw distance" mod.

### 1.4 Feasibility
* G1 resolution: done upstream; port it. **High.**
* G2 widescreen: **High/Med** (see §1.2).
* G3 HD texture pack keyed by GS hash: **Med-High**. 2–3 weeks: dump PNGs per
  hash, load replacements at a higher resolution, handle CLUT (palette)
  textures by hashing the palette too (already in the software key).
* G4 Xbox textures on the PS2 port: **Med**. The Xbox textures are DXT1/DXT3/DXT5,
  usually 256×256 (sometimes 512×256) with full mip chains (§2.4). PS2 versions
  are probably smaller and palettized (guess). Because the archives share entry
  names (guess, highly likely, since the data pipeline was shared; see `ps2butns.img`
  inside the Xbox `raceday.res` and GameCube `gc_bnr.cvs` inside `genesis.res`), a name-to-name
  mapping is possible. That needs an HLE hook at texture *load* time (or a
  name-to-GS-hash table built by logging loads), so that the GS-hash replacer
  knows which Xbox texture to use. Legal: only from the owner's own Xbox disc,
  generated locally, never shipped.
* G5 post effects (CRT, bloom, FXAA/SMAA, sharpen): **High**, a few days each,
  done in the Metal presenter.
* G6 per-material shaders: **Low-Med**. Materials have to be identified from GS
  state and texture hash. Possible for a few targets (car paint with an
  environment map: `envmap2.tex` exists on Xbox), not as a general system.
* G7 high refresh rate: **Low-Med**. Needs the game loop's frame-step split from
  rendering, plus interpolation. `config/hle.txt` already marks the frame loop
  idle points (`0035C460`, `0035C528`), which is a starting point. Physics must
  stay at the original tick (GOALS 1:1 rule).

---

## 2. Formats (cars, tracks, resources)

### 2.1 `0TSR` archive container (`.res`, `.car`, `.trk`), all little-endian

The format was confirmed on every `.res/.car/.trk` file on the Xbox disc.

```
0x00 char[4] "0TSR"          0x04 u32 0x2C (header size?)   0x08 u32 7 (version)
0x0C u32 total file size     0x10 u32 Unix build time (e.g. 0x41057A74 = 2004-07-27)
0x14 u32 dir_offset (0x80)   0x18 u32 entry count
0x1C u32 offset of a 2nd block (rel. dir_offset; .trk: start of the c_*.va0 event block, else 0)
0x20 u32 size of that block  0x24 u32,u32 two 32-bit hashes (not CRC32 of the directory; unknown)
dir_offset: entries, 0x44 bytes each:
  +0x00 char[32] name   +0x20 fourcc, byte-reversed ("LDOM" = MODL, "XET " = TEX, "TLAV" = VALT)
  +0x24 u32 chunk version  +0x28 u32 size  +0x2C u32 offset (rel. dir_offset, 0x80-aligned)
  +0x30 u32 hash (not plain CRC32 of data or name; unknown)  +0x34 4×u32 0
```
The existing QuickBMS script (`TDEOD RECOMP/testdriveevo.bms`) agrees with this
layout (it reads 9 longs after the name, so the stride is 0x44). Chunk types
seen across the whole disc (count, fourcc, extension): 3309 TEX `.tex`, 490 MODL
`.mod`, 354+338 VALT `.val/.va0`, 263 IMAG `.img`, 200 LAYT `.lyt`, 211 ILIN
`.ild/.ili`, 112 SFX0 `.sfx/.sf_`, 74 STAB `.tab`, 67 TEX `.art`, 64 SETU `.gar`,
62 GTEX `.gtx`, 36 VALT `.mvl`, 35 CANV `.cvs`, 27 VALT `.pln`, 18 Anim `.anm`,
15 each of BSPT `.bsp`, BPPT `.bpp`, SCOL `.bsc`, SOBL `.sol`, GRAF `.grf`,
BATS `.obt`, and a few STMP `.stp`, XFSE `.ens`, TBOP `.opt`, LANG `.lng`.

**Open risk:** the per-entry and header hashes. If the loader checks them, a
repacker must reproduce the hash function. Test this first (§9).

### 2.2 PS2 vs Xbox data (mostly guesses until the PS2 disc is added)
* The PS2 runtime builds a virtual ISO from `DATA/` (`runtime/iop/stream.cpp`).
  The TOC holds **at most 256 entries** (`kMaxToc`, including `.`/`..` per
  directory) with **names of at most 15 characters** (TOC entry layout in the
  file header comment). New files must fit under both limits. The limit lives
  in our HLE and in the size of the game's EE buffer. Raising it is possible
  only if the EE side allows it (unknown).
* Expected to be **identical** across platforms: the container, every `VALT`
  table (event rules, physics, AI, bones, camera, driver lists), and `.ild/.ili`
  racing lines. These are platform-neutral serialized tables (the vtable words
  `0x0040C218/0x0040C0D8` inside them are pointers from the Win32 tool that
  wrote them, not from the console).
* Expected to **differ**: `TEX` (Xbox: DXT; PS2: probably PSMT8/PSMT4 + CLUT in
  GS layout), possibly `MODL`/`GRAF` (PS2 meshes may be pre-packed VIF/VU1
  packets for the four VU1 programs in `config/vu/`), and audio (`.wma`/`.xmv`
  on Xbox vs `.pss`/SPU ADPCM banks on PS2). Movies are `data\video\*.pss`
  (`src/hle_movie.cpp`).
* Consequence: **data mods that touch only VALT/ILIN files should work on the
  PS2 port with no format work at all.** Mesh and texture mods need the PS2
  formats decoded first.

### 2.3 `VALT` key/value tables (`.val`, `.va0`, `.mvl`, `.pln`), fully decoded

```
0x00 u32 tool vtable (0x40C218)  0x04 u32 section count  0x08 u32 ?  0x0C 0
0x10 section[n], 0x1C bytes: {vtable 0x40C0D8, name_off, parent_name_off, n_pairs, pairs_off, 0, 0}
pairs_off: n_pairs × {key_off, value_off}; all values are C strings in a pool at the end
```
* `parent` implements **inheritance**: a contest section `[$0040$contest]
  parent='knockout_contest'` inherits its template, and eves inherit
  `afton_eve_defaults`. The engine errors confirm this: `valtab: table "%s" has
  missing parent "%s"` / `ambiguous parent` / `parented to self!`.
* `@name = $NNNN$name` keys are references to child sections (lists of objects).
* `include` sections chain files (`eves.val` includes the 14 `*_eves.val`).
* `frame64` / `unfloat_frame64` / `house_car_frame64`: 64 characters in the
  alphabet `a-zA-Z0-9,.` (6 bits each, MSB first) give 48 bytes = 12 LE floats =
  3×3 rotation + XYZ translation. Verified: decoding Afton's derby
  semi-trailer gives an orthonormal rotation and a position of (-37.3, 3.9, 188.6),
  consistent with that arena's bound polygon. Decoder:
  `/private/tmp/claude-501/eodmod/` (scratch).
* The XBE contains `Attempt to call ValueFileLoadHumanReadable() from non-PC
  platform???` (string at Xbox VA 0x1FC190, referenced at VA 0x52311). The
  engine had a **text val loader on PC**. NASCAR Heat PC's
  `drivers.txt` ("table Driver … end", per the RaceSimCentral guide) is that
  text form. A text-to-binary compiler (F3) puts us back on Monster Games' own
  authoring path. With an HLE hook, the port could even load text `.val` files
  directly.

### 2.4 Textures (`TEX` v10, Xbox)
Header: `u32 width, u32 height, u32 mip_count, u32 format, u32 flags, …`, then
mip offsets from +0x1C; data starts at 0x80. Seen formats: `0x0010` and
`0x30010` = DXT1 (0.5 bytes/pixel), `0x0211` = DXT3 or DXT5 (1 byte/pixel; which one
is to be confirmed). Example: `Afton.trk/cliff3.tex` is 256×256 with 6 mips,
mip0 at 0x80 and 0x10000 bytes long. Version `0x4000000A` on a few (`obswall`,
`plywood4`, `bbl_burn`, `pointer`) is probably a flag bit (alpha/cutout?). These
convert directly to and from DDS. PS2 `.tex`: unknown (§2.2).

### 2.5 Models (`MODL` v9) and damage
* Header: `u32 0x1F (sub-version?), 0x44, 1, data size, part count, …, bbox
  floats`, then a u32 table of part offsets. Each part is a `"DNIS"` chunk:
  `size`, 16-byte texture name padded with `0xE3`, `u32 0xE30X00YY` (YY = part or
  group id; the X bit separates body and interior), vertex count/offset, index
  count/offset. Xbox vertex stride is 36 bytes: position float3, normal float3,
  UV float2, plus 4 bytes (packed colour or bone index; to be confirmed).
* Cars (`*.car`): `Low0..Low4.mod` (LOD chain, distances in `lod.val`),
  `shadow1..3.mod`, `physics.val`, `efx.val` (engine smoke, radiator steam
  emitters, siren, flag mount), `Bone.val` (42 deformation bones:
  `link_center`, `travel_min/max` in inches, `weakness`) and `Bone.mvl`
  (157 mesh bones in 2 `bonesurf` groups with `group_mask`, `dmg_dist`,
  `weakness`, bounds). **Crumple damage is bone/vertex-weight based. Detachable panels are
  separate part groups** (`part.res` has `Hood.mod`, `LFlap.mod`, `RFlap.mod`,
  axle arms, steering wheel; `HobbySet.res` has `damage.tex`). Car liveries go
  through `paintkit.res` (same name as NASCAR Heat's Paintkit).
* `physics.val` is a full sim description in imperial units: Pacejka "magic
  formula" tyres (`magic_b/c/d/e`), aero (`drag`, `frontal_area`, spoilers),
  suspension (`bump`, `rebound`, `camber`, `caster`, `anti_pitch`, `sway`), engine
  (`power_max`, `torque_rpm`, `redline`), transmission, `damage_scale`,
  `damage_impulse`, `removal_impulse` (wheel loss), `mass = 2800`.
* PS2 meshes: unknown. The PS2 `.mod` may carry VU1 DMA packets, which would
  make a mesh exporter harder (guess).

### 2.6 Tracks (`*.trk`): see §3.

---

## 3. Custom tracks / map maker

### 3.1 What a track contains (Afton.trk, 224 entries; EmptyTrack.trk, 19)
| Entry | Type | Contents (evidence / **guess**) |
|---|---|---|
| `track.grf` | GRAF v18 (3.5 MB) | Render scene: mesh parts reusing the `DNIS` chunk with texture names (`dwall.tex`, `rd-5.tex`…). 3DSimED reads and writes "GRF (Viper Racing and Nascar Heat)", so this is the same lineage (version gap unknown). |
| `track.sol` | SOBL v3 | Solid collision primitives: contains `"BOX "`-tagged records with planes and extents (**guess**: walls and barriers). |
| `track.bsc` | SCOL v4 | Surface collision, quantized (0x7FFF-normalised) records (**guess**: terrain/road triangles with surface type). |
| `track.bsp` / `track.bpp` | BSPT / BPPT | Spatial partitions. `.bpp` contains Win32 heap pointers (0x77F5xxxx), i.e. a memory dump from the tool. |
| `track.obt` | BATS | Small object table. |
| `track.val` | VALT | `Info` (house-car frame), `AI` (`min/max_lookahead`, `pit_speed`, `rtt_*_lon`), `Physics` (`dirt_track`, `restrictor_plate`, `qualify_laps`), `Graphics` (`wearline_m/k`, the tyre wear line), `Skill` / `AIEasySimDirt`… |
| `*.ild` | ILIN v27 | AI racing lines / centerlines per layout: `track`, `oval`, `fig8`, `bw` / `bw_oval` / `bw_fig8` (backwards), `rr*` (road race), shortcut lines. |
| `lineinfo.val` | VALT | Per layout: `lookahead {curv_dist,min_t,max_t}`, `exceptions` (`on_lon`/`off_lon` ranges with `lookahead_f`), `exit_path` polygon. |
| `c_<event>.va0` | VALT | One per event type: placed `dyno` props with `unfloat_frame64`, `bound` polygon, `ringout` polygon, `ewob_cam`, respawn points, etc. |
| `def_objects.val`, `obstacle.val`, `*.pln` | VALT | Ambient sound emitters; prop definitions (`mass`, `friction`, `model`, `type = polyhedra`, `fabric`); convex collision hulls as plane lists. |
| `camera2/3.val`, `camera.tab` | VALT | TV/replay cameras: `region_sphere` triggers per event type, `fov`. |
| `intrlude.val` | VALT | Scripted pre-race "interludes" (camera moves, teleports, sounds). The retail XBE still has an **Interlude Editor** (strings: `INTERLUDE EDITOR`, `Set camera here...`, `Teleport cars...`). |
| `trkefx.val`, `shadow.val`, `*.sf_`, sky `*.tex`, prop `*.mod`, `*.gar` | various | Effects, baked shadows, ambient sounds, 15 skies, props, car setups (`default.gar`/`Advanced.gar`, NASCAR Heat "garage" setups). |

Global registration (in `common.res`): `tracks.val` (id → display name; it also
lists the cut tracks `Cedar` and `Shakopee`, which have no `.trk`), the
`<track>_eves.val` files (career/challenge/multi events per track), and the
hidden test eve `test: Empty Track` in `eves.val`.

### 3.2 What an importer or editor needs
1. **Easy (data only):** new events on existing tracks: move props, change
   bounds or ring-out, add obstacles (`c_*.va0` + frame64 codec). Rated **High**.
2. **Start from `EmptyTrack.trk`.** It is a 139 KB minimal track that the game
   already knows how to load. Its contents are a tiny grf/sol/bsc/bsp/bpp/obt,
   `oval.ild`, `fig8.ild` and the camera/interlude tables. Un-hide the test eve
   to load it.
3. **Medium-hard (binary):** writers for GRAF (render mesh), SOBL/SCOL/BSPT/BPPT
   (collision and partitions) and ILIN (lines). GRAF has a head start because
   3DSimED already writes NASCAR Heat GRF. SCOL/BPPT have no public
   documentation (**unknown**) and would be reverse-engineered from the loader
   code in the recompiled ELF.
4. Pipeline proposal: Blender scene (road, terrain and wall meshes tagged by
   material/surface) → exporter for GRAF + collision → spline-based `.ild`
   generator (§4) → event `.va0` placement from Blender empties → repack `0TSR` →
   register in `tracks.val` + `<new>_eves.val` → overlay.
5. The trkedit leftovers (`trkedit.res`: `checkpt.mod`, `point.mod`,
   `Back.stp`/`empty.stp` "stamps", `road.tex`; `uiedit.res`: Open/Save dialogs,
   property dialog, editor arrows; an options section named `TRKEDIT`) suggest
   an internal or cut track editor. The retail XBE has almost no strings that
   name these files, so **the editor code is probably compiled out** (guess;
   check the PS2 ELF). Do not plan on reviving it.

Feasibility: T1 **High**, T2 **Med-Low** (2–4 months for the first working
track, dominated by the collision formats), T3 full in-game editor **Low**.

---

## 4. AI on new maps

### 4.1 How the AI works (from data and strings)
* **Race AI follows precomputed lines.** `ILIN` (`.ild`) is a header
  (`lap length` ×3, e.g. 542.23 m for Afton's oval, which is about ⅓ mile; a scale
  factor; node count) plus a table of 60-byte nodes. From decoding Afton
  `oval.ild` (378 nodes), each node holds: segment length, cumulative `lon`
  (distance along the track), width (35.6), curvature, world X/Z, heading
  cos/sin, and an index. Lines are selected by `line_basename` in the event (`rr`
  → `rroval.ild` …). Code strings: `ai: Can't load centerline`, `ai: no backward
  racing line found, fwd/bwd-race will not function`, `ai: no shortcut racing
  line found`, `CenterLine::convert_meters_to_ilpos`, `Centerline: Can't load
  centerline (type 0x%x). Could an event req bw.ild?` (Xbox VA 0x35747).
* **Ghost lines** (`ghostcar.res`, 86 `.ili`: `<track>_e.ili`, `oval_<track>.ili`,
  `d_*`) are recorded laps. The Gauntlet needs them (`ghost: Can't run gauntlet
  event without backwards ghostcar`).
* **Arena/derby AI is algorithmic.** It is bounded by the `bound`/`ringout`
  polygons in `c_kill_*.va0` and states such as `ai kod: unhandled mode %d` and
  `Wrangle: unrecognized ai strength`. No lines are needed. So **new arenas get
  working derby AI for free** once bounds are written (guess, but strongly
  implied).
* Tuning: `track.val [AI]`, `lineinfo.val` lookahead and exceptions, per-event
  `speed_factor/skill_range` (+ `easy/medium/hard_sfd/srd`), and `dirt.val`
  (the driver roster: name, number, chassis, `rank`; `max_pack_size = 20`).

### 4.2 New tracks
* **A2, auto-generating `.ild`: Med, 2–4 weeks.** Fit a smooth closed spline
  to the road mesh centreline (or let the author draw one in Blender), sample
  it at about 1.5 m, compute heading/curvature/width/`lon`, then optionally
  optimise toward a racing line (minimum-curvature solve). Remaining unknowns:
  4 node fields and the header's 4th float. Learn them by regenerating
  Afton's `oval.ild` from its own centreline and diffing.
* Exceptions, `exit_path` and lookahead values can be copied from a similar
  existing track.
* Ghost `.ili` for Gauntlet: record by driving (the game writes ghosts: `ghost
  save`) or synthesise from the `.ild`.
* **A3, replacing the AI: Low.** It is not needed if lines can be generated.

---

## 5. Multiplayer game modes

### 5.1 How modes are defined
* **Rule types are code.** 26 `event_type` values appear in data: `normal_race,
  figure8_race, figure8_jump_race, jump_race, suicide_race, fwd_bwd_race,
  whip_around_race, trailer_race, school_bus, flagpole_race, stop_and_go_race,
  last_man_race, shortcut_race, chain_race, pointtopoint, gauntlet, detention,
  red_rover, save_ring, kill_derby, kill_knock, kill_push, battle, capture,
  wrangle, soccer`. A cut `king_of_ring` appears in `camera3.val`. Each has a
  manager class name in the XBE (`DemoDerbyMaster`, `GauntletMaster`,
  `TrailerMaster`, `CaptureMaster`, `BattleMaster`, `KillDerbyMaster`,
  `WrangleMaster`, `FlagpoleMgr`, `P2PMaster` (point-to-point) …).
* **Everything else is data.** A contest sets `laps, rounds, max_cars,
  damage_factor, player_damage_tweak, opt_terrain/terrain_mask, external_table`
  (which `c_*.va0` prop layout to use), `line_basename`, start/arena positions,
  AI skill and purse. All of these keys were found in the `*_eves.val` files.
* **The 2-player menu is data.** Each track's `<track>_eves.val` has one
  `[eve] mode = multi` with ~25 contests (decoded: every track except
  HomeBase offers jump/normal/figure-8/derby/battle/capture/gauntlet/…, and
  `soccer` appears only in MP on 5 tracks). Adding a contest to that list adds an MP option.
* Split screen: `Multiplayer requires at least two controllers!` (Xbox VA
  0xF51C2). Options sections `PLAYER0..3`/`CONTROL..3` exist (4 slots in the
  options system, inherited from Heat), but EoD ships 2-player only. There is no
  network code in the XBE (only Heat-era leftovers such as `EscapeMenuMulti:Disconnect`).

### 5.2 Feasibility
* **MP1, data-driven variants: High.** Examples: a "Reverse-arena Survival"
  using `kill_derby` with a new prop layout, a `school_bus` gauntlet with
  `max_cars` 14 and higher `damage_factor`, or a "Glass Cannon"
  approximated with `player_damage_tweak`/`damage_factor` plus a
  per-mode car `physics.val` override (guess: whether per-mode physics overrides
  are possible needs checking).
* **MP2, new rules: Med.** "Reverse Only" (input filter on the player's
  throttle mapping: HLE wrap of the pad-to-controls function, or done in our own
  pad layer), "Juggernaut" (bus chassis + damage scaling per car) and "Glass
  Cannon" (engine power ×2) are small HLE hooks once the functions are found.
  "Chain Gang" (tow cable between cars) might reuse the trailer hitch
  (`trailer: regluing to car...`), but this is a guess. **Low-Med.**
* **MP3, bigger fields: Low-Med.** The data maximum is `max_cars = 14`. Code
  enforces `cars + trailers ≤ 20` (Xbox: `lea edx,[edi+edi]; cmp edx,0x14` near
  VA 0x23B77 before `Eve: %s has too many vehicles (%d cars + %d trailers > %d
  max)`), and `dirt.val max_pack_size = 20`. Raising this touches pools
  (`Panic: out of memory in pool(%s)`), AI arrays, VU1/GS load and the PS2's
  32 MB memory map (the recomp can give the EE more memory, but fixed-size
  pools inside the game binary still need patching). 32–64 cars (MODDING_IDEAS §2)
  is a multi-month project with real risk. 3–4 player split screen needs
  viewport code that does not exist in the retail game: **Low**.
* LAN/online (MODDING_IDEAS §4): no engine support. It would need deterministic
  lockstep or state sync built from scratch: **Low**, a separate large project.

---

## 6. NASCAR asset import

### 6.1 Engine lineage (verified)
In EoD's own files:
* `Copyright (C) 2000 Monster Games, Inc.`, `heat.log`, `heat.bak`,
  `Heatman: Warning: empty results`, `Beat the Heat`, `TT_CANT_TAKE_THE_HEAT`,
  `options.cfg`/`default.opt` (XBE strings).
* NASCAR keys left in track data: `restrictor_plate`, `pit_speed`,
  `qualify_laps`, `yellow_flags`, `can_pit`, `rtt_greenflag_lon`, `Daytona`.
* Dirt to Daytona markers: `dirt_track`, `League = Dirt`, `AIEasySimDirt`,
  `FrontDirt/RearDirt`, `dirt.val`. **GameCube** banner/icon (`gc_bnr.cvs`,
  `gc_icon.cvs`) in `genesis.res`. D2D was the studio's GameCube title.
* NASCAR Heat (PC) tooling and data names match EoD's: `Paintkit` / `paintkit.res`,
  `.tex`, `.cvs`, `.trk`, `drivers.txt` "table Driver … end" with `number, name,
  chassis, rank` (same fields as EoD's `dirt.val [Driver]`) [RSC guide].
* Monster Games' shipped titles: Viper Racing (1998 PC), NASCAR Heat (2000,
  PC + PS1), NASCAR Heat 2002 (2001, PS2 + Xbox; **no PC version**), NASCAR: Dirt to
  Daytona (2002, PS2 + GameCube), EoD (2004) [Wikipedia]. Test Drive Off-Road:
  Wide Open is **not** a Monster Games title (it was Angel Studios), so it is
  not related.

**Conclusion:** one continuous in-house engine, Viper Racing → Heat (PC) →
Heat 2002 → Dirt to Daytona → EoD. Data formats evolved by version number
(EoD: container v7, MODL v9, GRAF v18, VALT v11, ILIN v27).

### 6.2 Which NASCAR versions to use
* **NASCAR Heat (PC, 2000)** has by far the best tooling. 3DSimED3 **imports
  and exports GRF and MOD "(Viper Racing and Nascar Heat)"** and unpacks "RES,
  TRK, CAR (Viper Racing and Nascar Heat)" [sim-garage.co.uk]. The community
  (The Mod Squad, HeatFinder, FFS Racing) has "hundreds of mods … over one
  thousand tracks", Restools (compile/decompile), Mod Launcher 2 and HeatCore.
  It is the best source of format knowledge, but it is the oldest format version.
* **NASCAR Heat 2002 (PS2/Xbox)** and **Dirt to Daytona (PS2/GC)** are closer in
  time to EoD and probably closer in format (guess). They have no known tools.
  The owner's Heat 2002 Xbox disc (if owned) would be the best like-for-like
  comparison: same platform, same texture style.

### 6.3 Mismatch analysis
| Aspect | Issue | Severity |
|---|---|---|
| Container | Probably the same `0TSR` family; Heat PC archives may be compressed (3DSimED mentions "decompression") | Low |
| Mesh (MOD/GRF) | Version gap (Heat ? → EoD v9/v18); vertex layout may differ | Med |
| Textures | Heat PC: its own `.tex` (palettized 16/256-colour era, "paint256.res"); EoD Xbox: DXT | Low-Med (convert through PNG) |
| **Damage** | Heat cars have `vertex_damage` (mod.cfg) but no EoD 42+157-bone rig, no detachable part groups, no radiator/steam emitters | **High**: every imported car must be re-rigged |
| Physics | Key names look shared (Pacejka `magic_*`, imperial units); stock-car numbers (≈3400 lb, 750 hp) go straight into `physics.val` | Low-Med |
| Scale | Same units (inches in physics, metres in world, guess); but superspeedway tracks are 2.5 mi against EoD's ⅓–½ mi short tracks: draw distance, LOD, AI `lon` ranges, skybox and memory are all stressed | Med-High for big ovals; Low for short tracks |
| AI lines | Heat tracks carry their own lines (likely an older ILIN version) or can use the A2 generator | Med |
| Track collision | Heat collision format (unknown) → EoD SOBL/SCOL/BPPT | High |

### 6.4 Feasibility
* N3 physics transplant: **Med-High**, days.
* N1 cars (Heat PC → EoD): **Med-Low**. Mesh through 3DSimED/Blender → EoD
  MODL v9 writer → bone rig + part groups (could be semi-automatic: copy
  bone layout from a similar EoD car, e.g. a muscle-class chassis, and
  skin by proximity) → `physics.val` → `carclass.val`/`car_stats.val`/
  `buycar.val` registration. The first car takes about 1–2 months, later ones days.
* N2 tracks: **Low-Med**. It depends entirely on T2 (EoD collision writers).
  Short tracks (Bristol-size, dirt bullrings) are realistic. Daytona-size ovals
  are a stretch on PS2-derived memory budgets.

### 6.5 Legal note
Only for content converted locally from discs/installs **the owner owns**. No
converted NASCAR/EoD assets go into any repository or release. NASCAR,
driver and sponsor marks are licensed to the original publishers. Tools may be
published, but they should read from the user's own files, the same way as the
port. (General guidance, not legal advice.)

---

## 7. Existing community knowledge and tools
* **EoD:** none found. There is no Xentax/ZenHAX/GitHub/ModDB work on the
  `0TSR` formats beyond the owner's BMS script. (A GitHub repo
  `catsandsoup/Test-Drive-Eve-Of-Destruction-Recomp-MacOS` is private and
  unrelated as far as can be seen.) The format notes in §2 appear to be the
  first public documentation.
* **NASCAR Heat (PC):**
  * 3DSimED3 (GRF/MOD import + export, RES/TRK/CAR unpack): https://www.sim-garage.co.uk/3dsimed-features/
  * The Mod Squad (mods, tracks, HeatCore, forum): https://www.themodsquad.info/; install guide (`*.trk` copied into `run/`, Mod Launcher 2, UI Control Center): https://www.themodsquad.info/index.php?topic=3946.0
  * HeatFinder / FFS Racing archives: https://heatfinder.net/downloads?c=16 , https://ffsracing.com/
  * RaceSimCentral driver guide (`drivers.txt` table syntax, `.tex/.cvs`, Paintkit, Restools): https://bhms.racesimcentral.net/guide/42
  * beatcracker/Nascar_Heat (reverse-engineering notes, unpacked Mod Launcher / UI Control Center): https://github.com/beatcracker/Nascar_Heat ; Vogons thread "Nascarheat (2002) – Digging Deeper": https://www.vogons.org/viewtopic.php?t=43104 (blocked to automated fetch; read manually)
* **NASCAR Heat 2002 GBA** has a matching decompilation (`shohamc1/heat2002`),
  but that version is by Crawfish, a different engine, so it is irrelevant.
* Background: https://en.wikipedia.org/wiki/Monster_Games , https://en.wikipedia.org/wiki/NASCAR_Heat_2002

---

## 8. Recommended order (after macOS parity + 2P split-screen pass)
1. **F1 mod overlay** in `runtime/iop/stream.cpp` `build_disc()`: merge
   `mods/*/DATA` over `DATA`, with a per-mod enable list in the ini. It needs no
   game code, is reversible, and keeps the default game byte-identical.
2. **F2 + F3 tooling** (Python, in `tools/`): `rst0.py unpack/pack`,
   `valt.py decompile/compile` (text form modelled on Heat's `table … end`
   syntax), and a frame64 codec. Run the hash test first (§9).
3. **Quick data wins** (each a few hours, all opt-in): LOD/draw distance,
   AI difficulty tuning, MP variants (MP1), car re-tunes (M1), prop/arena
   layout edits (T1), un-hiding the `EmptyTrack` test eve as a sandbox.
4. **Widescreen (G2)**: check the PS2 ELF for the widescreen strings, then
   flag-force or wrap the FOV.
5. **Renderer extras** on the Mac backend: post effects (G5), then the hash-keyed
   texture dump/replace (G3), then the Xbox-to-PS2 texture mapping (G4).
6. **Format reverse engineering** in the PS2 ELF (named via `tools/dis.py` +
   `config/symbols.txt`): the loaders for TEX/MODL/GRAF/SOBL/SCOL/ILIN. This
   unlocks M3, A2 and T2.
7. **`.ild` generator (A2)**, validated by regenerating Afton's oval.
8. **First new car (M3/N1)**, then the **first new short track (T2/N2)**.
9. Only then the risky items: >20 vehicles (MP3), new rule types (MP2),
   high refresh rate (G7).

## 9. Open questions / unknowns
1. **Are the `0TSR` header/entry hashes checked at load?** Test: overlay a
   repacked archive with one changed `.val` value and watch for a load error. If
   they are checked, the hash function has to be found in the loader.
2. **PS2 `DATA/` layout:** same archive names? PS2 `TEX`/`MODL`/`GRAF` chunk
   versions and encodings? (Run `tools`-side `rst.py` over the PS2 disc on day 1.)
3. Does `SLUS_209.10` contain the `Widescreen (16:9)` path and
   `ValueFileLoadHumanReadable`?
4. PS2 TOC limit (256 entries, 15-char names): how large is the EE-side buffer,
   and can extra tracks/cars fit?
5. Meaning of the remaining fields: VALT header word 2, ILIN header float 4 and
   4 node fields, MODL vertex last 4 bytes, the TEX `0x40000000` flag, and the
   `.trk` second block (`0x1C/0x20`; probably the event `.va0` group, loaded
   separately).
6. SOBL/SCOL/BSPT/BPPT semantics: these are the critical path for new tracks.
7. How cars are registered (chassis lists in `carclass.val`, `car_stats.val`,
   `buycar.val`, `catalog.val`): is there also a hard-coded chassis table in the
   code? (`ghost: %8x replacing illegal chassis %s w/%s` suggests a validity list.)
8. NASCAR Heat PC chunk versions vs EoD's. One look at a Heat `.car` with the
   §2.1 parser settles how far apart they are.
9. Can per-event car physics overrides be expressed in data (for "Glass Cannon"
   etc.), or do they need HLE hooks?
