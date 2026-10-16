# Modding Scope: Test Drive: Eve of Destruction (feasibility study)

## Owner priority update — October 16, 2026

LOD, draw distance and correct widescreen/ultrawide culling are now a **high
priority workstream**, explicitly promoted by the owner from the otherwise
deferred mod list. Target very distant highest-available-detail rendering and
eliminate visible model/quality pop-in where the engine/assets permit. Support
scalable Maximum/Extreme settings for faster Macs and high-end 8K PCs, beyond
what the owner's M2 Pro can run at full speed. Retain a playable Mac preset.
DLSS is explicitly the very last priority. Preserve
original physics, AI and simulation timing; maintain a faithful baseline for
comparison. Other mods and 120 Hz remain deferred.

Do not equate disabling all culling with correct wide-aspect visibility. Trace
frustum construction, per-camera aspect, bounding-volume tests, distance/LOD
selection, scene submission and asset streaming separately. Correct edge
visibility while retaining valid visibility tests; measure costs per player in
split screen. Use reproducible camera sweeps, distant opponent/track objects,
wide-edge captures and frame-time/geometry/memory measurements. A dedicated
fresh specialist agent is appropriate for this research/implementation stream.

Zero pop-in is an acceptance ambition, **not an established feasible guarantee**.
First establish whether highest-detail assets exist, remain resident, and can
be submitted beyond original engine limits. More internal render pixels alone
do not create higher-resolution textures or more detailed meshes. Document
engine bounds and any loading/streaming limits before promising removal.

The historical planning-only note below remains context for other mod ideas;
it does not override this newer LOD/culling priority.

> **Status: planning only.** Modding is parked until the macOS port of this PS2
> recompilation reaches 1:1 parity and passes the 2-player split-screen
> requirement (see `TDEOD RECOMP/GOALS.md` §0 and `docs/MODDING_IDEAS.md` there).
> Nothing here has been implemented. Every mod must be opt-in and must leave the
> default game unchanged.
>
> **Verified against the PS2 data on 2026-10-16.** First written from the owner's
> **Xbox** disc (`TDEOD RECOMP/game/`), then checked against the owner's **PS2**
> disc files (`SLUS_209.10` + `DATA/`, read in place from
> `~/Library/Application Support/TDEODRecomp/disc`; the ELF is byte-identical to
> the one in this repo). Format findings come from throwaway parsers in
> `/private/tmp/claude-501/eodmod/` (Xbox) and `/private/tmp/claude-501/eodmod-ps2/`
> (PS2, plus `xs.py`, a string/address cross-reference helper for the ELF).
> Nothing was extracted into a repo. Claims still unverified are marked
> **(guess)** or **(unverified)**.

---

## 0. Summary

**Main finding: almost all game content and rules are stored as data in a
key/value table format we can decode, and that data is the same on PS2 and
Xbox.** All 755 `VALT` tables (event rules, AI tuning, car physics, damage
bones, prop placement, arena bounds, camera FOVs, driver roster) are
semantically identical on both discs, and so are all `ILIN` racing/ghost lines
and the track collision files (`SOBL/SCOL/BSPT/BPPT`, byte-identical). Only
graphics (`TEX`, `MODL`, `GRAF`, UI images) and audio/video differ. The archive
format (`0TSR`, §2.1) is a slightly older version on PS2 (v6 vs v7) and its
checksums are now **solved**: they are a CRC32 found in the ELF, so repacking
is safe.

Three PS2-specific findings change the plan:
* **Widescreen is built into the PS2 game** (options menu "Widescreen (16:9)",
  a real Hor+ projection). With the port's `AspectRatio=16:9` it may already
  work; it needs a check, not a mod (§1.2).
* **PS2 textures are 4/8-bit palettized and GS-swizzled, at the same resolution
  as Xbox's DXT textures.** The Xbox disc is not a source of *higher-resolution*
  textures, only of full-colour ones (64% of PS2 textures have 16 colours) (§2.4).
* **PS2 meshes are pre-built VIF/VU1 packets** (`SPS4` parts), so new cars and
  tracks need a VIF packet writer. The Xbox meshes (`DNIS` parts, plain
  vertex/index arrays) are a clean source for that conversion: the part lists
  match one to one (§2.5).

Code changes are needed only for these things: raising vehicle caps, adding new
event rule types, more than 2 split-screen views, new rendering effects, and
frame interpolation.

**Engine lineage is confirmed. The closest sibling is NASCAR: Dirt to Daytona
(2002, PS2/GameCube), not Heat 2002.** Evidence is in §6.1.

| # | Idea | Feasibility (PS2-based port) | Rough effort* | Main prerequisite |
|---|---|---|---|---|
| F1 | **Mod overlay folder** (`mods/<name>/DATA/...` merged over `DATA/` in the virtual disc) | **High** | 1–2 days | none (`build_disc()` in `runtime/iop/stream.cpp`); names must be ISO 8.3 (§2.2) |
| F2 | `0TSR` archive unpack/repack tool | **High** (CRC solved) | 1–3 days | none; CRC = `f_00135FC8` (§2.1) |
| F3 | `VALT` ⇄ text decompiler/compiler (round-trip) | **High** | 3–5 days | F2 |
| G1 | Higher internal resolution | **Done** on D3D11 (1–6×); needs to be redone in the Mac renderer | part of the port | Mac GPU GS backend |
| G2 | Widescreen (true 16:9 field of view) | **High: already in the game** | 0.5–2 days (verify, optional auto-select) | test in-game option + `AspectRatio=16:9` (§1.2) |
| G3 | Texture replacement / HD pack (keyed at GS upload, names from the texture loader) | **Med-High** | 2–3 weeks | Mac GPU backend, dump tool, hook `f_00350F58` |
| G4 | Xbox textures in the PS2 port | **Med** (easy mapping, modest gain) | 1–3 weeks | G3 (true-colour path) or a PS2 `TEX` writer |
| G5 | New post effects (CRT filter, bloom, sharpening, AA) | **High** | days each | Mac GPU backend |
| G6 | New per-material shaders (e.g. real environment maps, normal maps) | **Low-Med** | weeks–months | GS-level material identification |
| G7 | 120 Hz output (opt-in), simulation unchanged | **Low-Med** | weeks+ | transform interpolation hooks, or renderer frame generation |
| M1 | Re-tune existing cars (physics, durability, damage bones) | **High** | hours per car | F1–F3 |
| M2 | Re-skin cars / tracks (texture swaps in archives) | **Med-High** | days | F2 + PS2 `TEX` writer (8bpp verified, 4bpp swizzle to confirm) |
| M3 | Add a **new car** (new mesh) | **Med-Low** | 4–8 weeks for the first car, then days | `SPS4` VIF mesh writer, bone rigging, 8.3 name, chassis limits |
| T1 | Edit existing tracks' events (props, bounds, laps, car counts) | **High** | hours | F1–F3, frame64 codec (done, §2.3) |
| T2 | **New track** (custom map) via importer | **Med-Low** | 2–4 months | collision writers (platform-neutral) + `GRAF` with `SPS4` parts |
| T3 | In-game/standalone track editor | **Low** (as a full editor); **Med** as a "Blender plus exporter" pipeline | months | T2 |
| A1 | Re-tune AI (lookahead, skill, speed factors) | **High** | hours | F3 |
| A2 | Auto-generate AI racing lines (`.ild`) for new tracks | **Med** | 2–4 weeks | `.ild` layout (same on both platforms, §4) |
| A3 | Replace the AI | **Low** | months | deep code reverse engineering |
| MP1 | New multiplayer **variants** (existing rules, new parameters/props) | **High** | hours per mode | F1–F3 |
| MP2 | New multiplayer **rule types** (Reverse Only, Juggernaut, etc.) | **Med** (simple ones as HLE hooks) / **Low** (Chain Gang) | 1–4 weeks each | find player-input/damage functions in the ELF |
| MP3a | More than 20 vehicles | **Low** | months | the race setup holds exactly 20 car slots (§5.2) |
| MP3b | 3–4 player split screen | **Low** (input side is ready) | months | viewport/camera/HUD code for >2 views does not exist |
| N1 | NASCAR Heat (PC 2000) **car** import | **Med-Low** (after M3) | 1–2 months | M3 pipeline, re-rig damage bones |
| N2 | NASCAR Heat / Dirt to Daytona **track** import | **Low-Med** | 2–4 months for the first track | T2 + scale/AI lines |
| N3 | Physics value transplant (Heat `.val` to EoD `physics.val`) | **Med-High** | days | F3 (key names look shared) |

\*Effort is a rough single-developer estimate after parity is reached. It
assumes the existing recompilation tooling and gets cheaper with each repeat.

### 0.1 PS2 hook points found in this pass
Addresses are EE addresses in `SLUS_209.10` (view with `tools/dis.py -f <addr>`).
None of these are in `config/symbols.txt` yet; the names are descriptive guesses
from strings and code shape. HLE `wrap`/`replace` entries go in `config/hle.txt`.

| Address | What it is (evidence) | Mod use |
|---|---|---|
| `00135FC8` | Resource CRC32 (MSB-first, poly 0x04C11DB7, table at `003F2598`) | reproduce in the repacker (done in Python) |
| `0012FEA0` | Resource-set header check (cookie `0TSR`, version 6, header size 0x28, file size, header CRC) | none needed |
| `00130980` / `00130C30` / `00130170` | `load_resource_set`, entry extraction with CRC check, `check_all_resident_crcs` | debug logging of loaded sets |
| `00331708` | Path to ISO 8.3 name mangler (`EmptyTrack.trk` → `EMPTYT_1.TRK`) | naming rule for new files |
| `0033D4F8` | `StrmFindFile` (already wrapped) | log missing mod files |
| `00350F58` / `003545B8` | Texture load by name, then optional `<name>.tm0` high-detail variant | **G3/G4 name ↔ GS upload mapping** |
| `0034D328` | `SetDisplayMode(0 normal, 1 widescreen, 2 letterbox)`; saves option `display_type` | G2 |
| `0032EC58` | Boot-time "console is set to 16:9" query; `main` calls `SetDisplayMode(1)` if non-zero | G2: wrap to follow the port's `AspectRatio` |
| `0034E530` | Projection setup (near, far, FOV in radians; divides by the x-scale at `003DD658`); called with (1, 1000, 67.5°) from `002127B0` | FOV mods, G2 |
| `00212808` | Detail setup: option `draw_distance` (0–2) → factor at `00495D58`; also `detail_level` | draw-distance mod |
| `00137838` / `00137750` / `001377C0` | Options get-int / get / set (key + section id) | force any option |
| `00123B98` | Eve/contest loader (validates contests; chassis ≤ 7, trailer-race vehicles ≤ 20) | MP1/MP3 |
| `0020FD48` | `create_cars`: 20 slots × 0x78 bytes, count at slot base + 0x960 | MP3a |
| `00331FD0` | Pad init: loads `mtapman.irx` + `padman.irx`, opens up to 5 pad ports | MP3b input |
| `00178670` / `001796E0` | Physics task (catch-up ticks, clamp 12) / tick-time conversion (tick = 16683 µs ≈ 59.94 Hz) | G7 |
| `0035C3F8` | Frame flip (`drFlip`, waits for vblank); idle points `0035C460`/`0035C528` already in `hle.txt` | G7 |

---

## 1. Graphics

### 1.1 What exists
* The PS2 port renders by emulating the PS2's Graphics Synthesizer (GS). It has
  a software GS and, upstream, a Direct3D 11 GS (`runtime/gs/d3d11/`) with an
  **internal resolution of 1–6×** (`RenderScale` in `runtime/core/config.cpp`)
  and an `AspectRatio` option (`4:3 | 16:9 | stretch`), which sets the shape of
  the displayed image (`platform_sdl3.cpp`). The D3D11 backend is Windows-only,
  so the Mac port needs its own GPU GS backend (`docs/MACOS.md`: Vulkan on
  MoltenVK first) before any of G1/G3/G5 apply.
* The D3D11 texture cache is already **content-hashed**:
  `hash_level(tbp,tbw,psm,w,h)` in `runtime/gs/d3d11/gs_d3d11.cpp`. The
  software path keys on (tbp, tbw, psm, w, h, palette hash)
  (`runtime/gs/gs_draw.cpp`). That hash is the natural key for a
  PCSX2-style texture dump/replace system.
* The VU1 microcode is compiled ahead of time (`config/vu/*.bin`,
  `tools/vurecomp.py`). Geometry reaches the GS already transformed, so
  "new shaders" can only work on GS primitives (screen-space triangles with
  texture/alpha/fog state), not on game materials.

### 1.2 Widescreen: built into the PS2 game (verified in the ELF)
`SLUS_209.10` has the same widescreen strings as the Xbox XBE
(`Widescreen (16:9)`, `mr: widescreen mode`, `mr: letterbox mode`,
`Aspect_constant`, `new_fov`, `mirror_fov`) and the code behind them:
* The options screen (`002A3CF0`) offers **"Standard (4:3)" / "Widescreen
  (16:9)"** next to Mono/Stereo/Dolby Pro Logic II, and applies a change with
  `SetDisplayMode` (`0034D328`, call at `002A39E4`).
* `SetDisplayMode(1)` sets a widescreen flag (`003DD64C`) and an x-scale of
  **0.75** (`003DD658`/`003DD65C`); mode 2 is letterbox (`003DD650`), mode 0
  resets to 1.0. The value is saved as option `display_type`.
* The projection setup (`0034E530`) divides the horizontal half-FOV tangent by
  that x-scale, so widescreen widens the view by 4/3: a true Hor+ 16:9 picture,
  rendered anamorphically into the normal 640-wide frame.
* At boot `main` calls `SetDisplayMode(1)` when `f_0032EC58` (the console's
  16:9 system setting) returns non-zero.

**Consequence:** G2 is mostly free. Pick "Widescreen (16:9)" in the game's
options and set `AspectRatio=16:9` in the port, which stretches the anamorphic
frame to 16:9. Remaining work (0.5–2 days): verify it in a race, in the menus
and in 2-player split screen (unverified); check that the HUD is drawn for the
squeeze (unverified); optionally wrap `f_0032EC58` so the game starts in
widescreen whenever `AspectRatio=16:9`. Ultra-wide (21:9) would be a small
extra: wrap `0034D328` to write a smaller x-scale (0.5625 for 21:9) plus a wider
presenter.

### 1.3 Hooks: HLE wrapping vs data edits
* **HLE** (`config/hle.txt`: `<EE address> wrap|replace`, implemented in
  `src/hle_*.cpp`; e.g. `0033D4F8 wrap # StrmFindFile`). Best for: forcing
  options (widescreen, FOV scale, draw distance: the getters in §0.1), logging
  which texture or model name is loaded (texture loader `00350F58`, for mapping
  GS hashes back to `.tex` names), and replacing whole functions. Cost is low
  per hook. Only 30 symbols are named in `config/symbols.txt`; §0.1 adds about
  20 candidates.
* **GS-level** (renderer). Best for texture replacement, upscaling, post
  effects and AA. It does not depend on game code. Risk: the game reinterprets
  depth buffers as colour for shadows (README §How it works), so replacement
  must skip render-target textures.
* **Data edits** (overlay + archive repack). Best for camera FOV tables,
  texture swaps in the game's own format, and LOD distances (`lod.val`:
  `lod1 = 10 … lod4 = 500`, identical on both platforms). Raising the LOD
  distances is a candidate model-retention change. **Correction from the PS2
  investigation:** `draw_distance` is an enum: 0→1150, 1→725, 2/default→300
  (`1700*factor+300` at `00212808`, output `00495D58`). Raising the enum past
  its supported values cannot extend distance. Car LOD, projection/fog range,
  scene submission and residency require separate changes and validation.
  See [current investigation](LOD_CULLING_INVESTIGATION.md).

### 1.4 Feasibility
* G1 resolution: done upstream; port it. **High.**
* G2 widescreen: **High**, already in the game (§1.2).
* G3 HD texture pack keyed by GS hash: **Med-High**. 2–3 weeks: dump PNGs per
  hash, load replacements at a higher resolution, handle CLUT (palette)
  textures by hashing the palette too (already in the software key). The PS2
  texture loader (`00350F58`) receives the texture name, so a `wrap` can log
  name → GS upload address, which gives dumps human-readable names and makes
  packs stable across renderer changes.
* G4 Xbox textures on the PS2 port: **Med**. Checked on all 3,376 shared
  texture names: **every Xbox texture has the same size as the best PS2 copy**
  (the PS2's `.tm0` variant where one exists, else `.tex`). So the Xbox disc
  gives no extra resolution. What it gives is colour: Xbox textures are DXT1/3/5
  (full colour, smooth alpha), while 64% of PS2 textures are 4-bit (16 colours)
  and the rest 8-bit (256 colours). Two routes:
  1. **Renderer route:** decode the Xbox DXT to RGBA and substitute it as a
     true-colour texture at GS upload, keyed by the name logged from
     `00350F58`. Best quality; needs G3 plus a renderer able to replace a CLUT
     texture with a 32-bit one.
  2. **Data route:** re-quantize the Xbox image to an 8-bit palette and write a
     PS2 `TEX` (§2.4). Works with any renderer, but gains only 16 → 256 colours
     on the 4-bit textures.
  Mapping is trivial: entry names match one to one in matching archives. Value
  is modest (DXT has its own block artifacts), so this is a "nice to have", not
  an HD pack. A real HD pack needs new artwork or AI upscaling (G3). Legal: only
  from the owner's own Xbox disc, generated locally, never shipped.
* G5 post effects (CRT, bloom, FXAA/SMAA, sharpen): **High**, a few days each,
  done in the presenter.
* G6 per-material shaders: **Low-Med**. Materials have to be identified from GS
  state and texture name/hash. Possible for a few targets (car paint with an
  environment map: `envmap2.tex` exists), not as a general system.
* G7 120 Hz as an opt-in mod: **Low-Med**. Facts from the ELF: physics runs on
  a fixed tick of 16,683 µs (≈59.94 Hz, `f_001796E0`) in a task that catches up
  missed ticks (`f_00178670`, at most 12), and the game draws one frame per tick
  (`docs/MACOS.md`: 59.9 game frames/s). Two routes:
  1. **Game-side interpolation:** keep the tick, render a second frame between
     ticks with car/camera transforms interpolated. Needs the object transform
     and camera state found and a way to re-run the render pass without
     advancing the game (render list replay at GS level, or a split of the
     frame function). Weeks to months; must stay off by default (GOALS 1:1 rule).
  2. **Renderer frame generation:** synthesize in-between frames from two GS
     frames plus the depth buffer (motion estimation). Independent of game code,
     but HUD and alpha effects ghost. Days to weeks for a prototype, quality
     uncertain (guess).

---

## 2. Formats (cars, tracks, resources)

### 2.1 `0TSR` archive container (`.res`, `.car`, `.trk`), all little-endian

Verified on all 72 PS2 archives (7,329 entries) and all Xbox archives. The two
platforms use two versions of the same container; the directory layout is the
same.

```
0x00 char[4] "0TSR"           0x04 u32 header size: PS2 0x28, Xbox 0x2C
0x08 u32 version: PS2 6, Xbox 7   0x0C u32 total file size
0x10 u32 Unix build time (PS2 2004-07-13, Xbox 2004-07-27)
0x14 u32 dir_offset (0x80)    0x18 u32 entry count
0x1C u32 start of the "extractable" block, rel. dir_offset (= end of data if none)
0x20 u32 unknown (0 when no extractable block; probably a buffer size)
PS2:  0x24 u32 header CRC = crc(bytes 0x00..0x23)
Xbox: 0x24 u32 directory CRC = crc(entry table)   0x28 u32 header CRC = crc(bytes 0x00..0x27)
dir_offset: entries, 0x44 bytes each:
  +0x00 char[32] name   +0x20 fourcc, byte-reversed ("LDOM" = MODL, "XET " = TEX, "TLAV" = VALT)
  +0x24 u32 chunk version  +0x28 u32 size  +0x2C u32 offset (rel. dir_offset)
  +0x30 u32 CRC of the entry data   +0x34 4×u32 0
```
* **The checksums are solved.** `crc` is the game's CRC32 at `f_00135FC8`:
  MSB-first, polynomial 0x04C11DB7 (table at `003F2598`); the first 4 bytes are
  loaded big-endian and inverted, then each further byte is shifted in
  (`c = ((c << 8) | byte) ^ T[c >> 24]`), and the result is inverted. It matches
  every PS2 header and entry and every Xbox entry, header and directory checksum
  tested (`/private/tmp/claude-501/eodmod-ps2/crc.py`).
* **They are checked.** `f_0012FEA0` rejects a set whose cookie, version (6),
  header size (0x28), file size or header CRC is wrong, and extraction
  (`f_00130C30`) and `check_all_resident_crcs` (`f_00130170`) compare entry
  CRCs. A repacker therefore must write PS2-version headers and recompute the
  CRCs. That is now easy.
* The extractable block (`0x1C`) holds entries loaded on demand rather than
  kept resident: on PS2 the `.tm0` textures, `.an0` animations and the event
  `c_*.va0` tables; on Xbox the `c_*.va0` tables.
* The existing QuickBMS script (`TDEOD RECOMP/testdriveevo.bms`) reads the
  directory correctly on both versions (stride 0x44).

Chunk types (PS2 count; Xbox counts in brackets where they differ): 3,309 TEX
`.tex`, **1,547 TEX `.tm0` [0]**, 490 MODL `.mod`, 354+338 VALT `.val/.va0`,
264 IMAG `.img` [263], 200 LAYT `.lyt`, 211 ILIN `.ild/.ili`, 112 SFX0
`.sfx/.sf_`, 89 STAB `.tab/.obt`, 67 TEX `.art`, 64 SETU `.gar`, 62 GTEX
`.gtx`, 36 VALT `.mvl`, 35 CANV `.cvs`, 27 VALT `.pln`, 18 Anim `.anm`, **15
Anm0 `.an0` [0]**, 15 each of BSPT/BPPT/SCOL/SOBL/GRAF, and a few STMP, ESFX,
BOPT, LANG, ICON, XDSP. Chunk versions are the same on both platforms.

### 2.2 PS2 vs Xbox data (verified)
Every PS2 archive has an Xbox counterpart with the same entry names; the PS2
adds only `.tm0`/`.an0` variants and `genesis.res/badmedia.img`. (The PS2
`common.res` even ships `xbbutns.img`, the Xbox button art: one shared
pipeline.)

| Chunk | Same on both? | Notes |
|---|---|---|
| `VALT` (`.val .va0 .mvl .pln`) | **Yes** (755/755 semantically identical) | byte diffs only in unused section words (tool heap pointers) |
| `ILIN` (`.ild .ili`) | **Yes** (211/211 byte-identical) | AI lines and ghosts |
| `SOBL SCOL BSPT BPPT SETU LAYT BOPT ICON XDSP` | **Yes** (byte-identical) | track collision/partitions, garage setups, UI layouts |
| `TEX` (`.tex .art`) | No | PS2: palettized + GS-swizzled (§2.4) |
| `MODL`, `GRAF` | No | PS2: VIF packet parts `SPS4` (§2.5) |
| `IMAG CANV GTEX STAB Anim LANG SFX0 ESFX` | No (same names) | UI images, fonts, tables, animations, sound (PS2 ADPCM) |
| Video | No | PS2 `DATA/VIDEO/*.PSS` and `*.M2V` (41 files, 1.4 GB); Xbox `.xmv` |

* **Consequence: data mods that touch only VALT/ILIN files work on the PS2
  port with no format work at all, and a mod made from Xbox data files of those
  types is valid on PS2 as-is.** Mesh and texture mods need PS2 writers.
* Disc limits for new files (F1):
  * The runtime's virtual ISO TOC holds **256 entries** (`kMaxToc` in
    `runtime/iop/stream.cpp`, mirroring the 256 × 24-byte table that
    `stream.irx` returns). The retail disc uses **148** (7 directories with
    their `.`/`..` records plus 131 files), so **about 100 new files fit**. The
    EE side receives a fixed 256-entry table, so raising the limit needs EE
    changes (unknown).
  * Names are **ISO 8.3, upper case** (TOC field 15 characters). The game maps
    long names itself (`f_00331708`): `EmptyTrack.trk` → `EMPTYT_1.TRK`,
    `Hurricane.car` → `HURRIC_1.CAR`. A new long name must use the mangled form
    on disc, and two long names that share their first 6 characters would
    collide **(guess, from the `_1` suffix pattern)**. Short names (≤ 8
    characters) avoid the issue.
  * Code-side tables: up to **40 tracks** (`f_00107E60`, 15 used), **40 eves**
    and 40 make-your-own eves (`f_001244A8`), **80 events** (`f_00108350`).

### 2.3 `VALT` key/value tables (`.val`, `.va0`, `.mvl`, `.pln`), fully decoded, platform-neutral

```
0x00 u32 tool vtable (0x40C218)  0x04 u32 section count  0x08 u32 ?  0x0C 0
0x10 section[n], 0x1C bytes: {vtable 0x40C0D8, name_off, parent_name_off, n_pairs, pairs_off, 0, tool pointer}
pairs_off: n_pairs × {key_off, value_off}; all values are C strings in a pool at the end
```
* The last section word is a leftover heap pointer from the Win32 tool. It is
  the only thing that differs between the PS2 and Xbox copies of a table.
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
* Both executables contain `Attempt to call ValueFileLoadHumanReadable() from
  non-PC platform???` (PS2: string `003F2148`, used in `f_00134D88`). The
  engine had a **text val loader on PC**, compiled out on consoles. NASCAR Heat
  PC's `drivers.txt` ("table Driver … end") is that text form. A
  text-to-binary compiler (F3) puts us back on Monster Games' own authoring
  path. An HLE `replace` of `f_00134D88` could even let the port load text
  `.val` files directly **(unverified: the function's arguments are not traced)**.

### 2.4 Textures (`TEX` v10)

**Xbox:** header `u32 width, height, mip_count, format, flags, …`, mip offsets
from +0x1C, data at 0x80. Formats: `0x10`/`0x30010` = DXT1, `0x211`/`0x411` =
DXT3/DXT5 (which is which: to be confirmed). Converts directly to and from DDS.

**PS2 (verified):**
```
0x00 u32 width, height, level count (palette + mips), format, flags (same value as the Xbox copy)
0x14 u32 0   0x18 u32 palette offset (0x80)   0x1C.. u32 offsets of each mip, then the end offset
0x80 palette: 256 × 4 bytes, byte order [A, R, G, B], alpha 0–255 (not GS 0–128)
mip 0 follows at 0x480
```
* `format` bit 0: 1 = 8-bit indices, 0 = 4-bit indices (16 colours; the palette
  area is still 0x400 bytes). Bit 9 (0x200) is set on most textures (alpha
  present? guess); bits 16–17 (0x30000) as on Xbox.
* **8-bit pixels are stored GS-swizzled** (the PSMT8 layout as written through a
  32-bit upload). Verified by decoding `ct.res/1.tm0` (512×256 car livery) with
  the standard PSMT8 unswizzle and a linear (not CSM-swizzled) palette: the
  image is clean. 4-bit pixels are presumably the matching PSMT4 swizzle
  **(unverified)**. Decoder: `/private/tmp/claude-501/eodmod-ps2/tex2png.py`.
* Sizes: unique textures range 16 to 512 px (most 128–256). Each `<name>.tm0`
  is the same texture at twice the size of `<name>.tex` (1,547 of them: all car
  liveries in `ct.res`/`uicar.res`, car interiors in `hobbyset.res`, and about
  40–100 per track). The loader (`f_00350F58`, `f_003545B8`) loads `.tex` and
  then the `.tm0` on demand ("high detail" texture). When it is used (always,
  1-player only, or by the detail option) is **unverified**.
* Counts: 2,166 unique textures are 4-bit, 1,210 are 8-bit.
* Writing PS2 `TEX` needs: quantize to 16 or 256 colours, swizzle, build mips.
  About 1 week including the 4-bit swizzle check.

### 2.5 Models (`MODL` v9) and damage
* **Shared:** the header's bounding values and part count match between
  platforms (e.g. `Algar.car/low0.mod`: 50 parts, identical floats), and the
  damage data is in VALT (`Bone.val`, `Bone.mvl`), so it is identical.
* **Xbox:** header `u32 0x1F (sub-version?), 0x44, 1, data size, part count, …,
  bbox floats`, then a u32 table of part offsets. Each part is a `"DNIS"` chunk:
  `size`, 16-byte texture name padded with `0xE3`, `u32 0xE30X00YY` (YY = part
  or group id; the X bit separates body and interior), vertex count/offset,
  index count/offset. Vertex stride is 36 bytes: position float3, normal float3,
  UV float2, plus 4 bytes (packed colour or bone index; to be confirmed).
* **PS2 (verified):** header size 0x58 instead of 0x44, same bounds, then a
  part table; each part is an `"SPS4"` chunk (`4SPS` on disk) with the texture
  name (e.g. `Algar.tex`) followed by **VIF command streams**: `STCYCL`,
  `STMOD`, `STMASK`, `STROW` (offset 256.0 for 16-bit quantized positions),
  `UNPACK` V4-8/V3-16/V4-16/V4-32, and `MSCAL` calls into the VU1 programs in
  `config/vu/`. So PS2 meshes are ready-to-send DMA packets, quantized, with
  per-vertex colours packed as bytes.
* `GRAF` (track render scene) is the same story: the top-level header is the
  same on both platforms and Afton's `track.grf` has **2,239 parts on both**,
  `DNIS` on Xbox and `SPS4` on PS2.
* Consequence: a mesh importer for the PS2 port needs a **VIF packet writer**
  that reproduces what the VU1 microcode expects (vertex format per program,
  batch sizes, `STROW` quantization). The VU1 programs are known and
  recompiled, so this is reverse engineering with a test harness, not guesswork.
  The Xbox `DNIS` parts are the easiest source geometry (plain floats, one part
  per PS2 part), and a converter can be validated by turning an Xbox car into
  PS2 packets and comparing against the PS2 original. Estimate: 3–5 weeks.
* Cars (`*.car`): `Low0..Low4.mod` (LOD chain, distances in `lod.val`),
  `shadow1..3.mod`, `physics.val`, `efx.val` (engine smoke, radiator steam
  emitters, siren, flag mount), `Bone.val` (42 deformation bones:
  `link_center`, `travel_min/max` in inches, `weakness`) and `Bone.mvl`
  (157 mesh bones in 2 `bonesurf` groups with `group_mask`, `dmg_dist`,
  `weakness`, bounds). **Crumple damage is bone/vertex-weight based. Detachable
  panels are separate part groups** (`part.res` has `Hood.mod`, `LFlap.mod`,
  `RFlap.mod`, axle arms, steering wheel; `HobbySet.res` has `damage.tex` and,
  on PS2, `damage.tm0`). Car liveries are in `ct.res` (285 liveries, `.tex` +
  `.tm0`) and `paintkit.res` (same name as NASCAR Heat's Paintkit). How bone
  weights are encoded in the PS2 packets is **unknown**.
* `physics.val` is a full sim description in imperial units: Pacejka "magic
  formula" tyres (`magic_b/c/d/e`), aero (`drag`, `frontal_area`, spoilers),
  suspension (`bump`, `rebound`, `camber`, `caster`, `anti_pitch`, `sway`), engine
  (`power_max`, `torque_rpm`, `redline`), transmission, `damage_scale`,
  `damage_impulse`, `removal_impulse` (wheel loss), `mass = 2800`.

### 2.6 Tracks (`*.trk`): see §3.

---

## 3. Custom tracks / map maker

### 3.1 What a track contains (Afton.trk: 262 entries on PS2, 224 on Xbox; EmptyTrack.trk 20/19)
| Entry | Type | Contents (evidence / **guess**) |
|---|---|---|
| `track.grf` | GRAF v18 (3.5 MB) | Render scene: 2,239 mesh parts with texture names (`dwall.tex`, `rd-5.tex`…); `DNIS` on Xbox, VIF `SPS4` on PS2 (§2.5). 3DSimED reads and writes "GRF (Viper Racing and Nascar Heat)", so this is the same lineage (version gap unknown). |
| `track.sol` | SOBL v3 | Solid collision primitives: contains `"BOX "`-tagged records with planes and extents (**guess**: walls and barriers). Byte-identical on both platforms. |
| `track.bsc` | SCOL v4 | Surface collision, quantized (0x7FFF-normalised) records (**guess**: terrain/road triangles with surface type). Byte-identical. |
| `track.bsp` / `track.bpp` | BSPT / BPPT | Spatial partitions. `.bpp` contains Win32 heap pointers (0x77F5xxxx), i.e. a memory dump from the tool. Byte-identical. |
| `track.obt` | STAB | Small object table (differs between platforms). |
| `track.val` | VALT | `Info` (house-car frame), `AI` (`min/max_lookahead`, `pit_speed`, `rtt_*_lon`), `Physics` (`dirt_track`, `restrictor_plate`, `qualify_laps`), `Graphics` (`wearline_m/k`, the tyre wear line), `Skill` / `AIEasySimDirt`… |
| `*.ild` | ILIN v27 | AI racing lines / centerlines per layout: `track`, `oval`, `fig8`, `bw` / `bw_oval` / `bw_fig8` (backwards), `rr*` (road race), shortcut lines. |
| `lineinfo.val` | VALT | Per layout: `lookahead {curv_dist,min_t,max_t}`, `exceptions` (`on_lon`/`off_lon` ranges with `lookahead_f`), `exit_path` polygon. |
| `c_<event>.va0` | VALT | One per event type (extractable block): placed `dyno` props with `unfloat_frame64`, `bound` polygon, `ringout` polygon, `ewob_cam`, respawn points, etc. |
| `def_objects.val`, `obstacle.val`, `*.pln` | VALT | Ambient sound emitters; prop definitions (`mass`, `friction`, `model`, `type = polyhedra`, `fabric`); convex collision hulls as plane lists. |
| `camera2/3.val`, `camera.tab` | VALT / STAB | TV/replay cameras: `region_sphere` triggers per event type, `fov`. |
| `intrlude.val` | VALT | Scripted pre-race "interludes" (camera moves, teleports, sounds). Both executables still have an **Interlude Editor** (PS2: `INTERLUDE EDITOR` used in `f_001EBE78`, `Set camera here...`, `Teleport cars...`). |
| `*.tm0`, `*.an0` | TEX, Anm0 | PS2 only: high-detail texture variants (38 on Afton) and animation variants (HomeBase). |
| `trkefx.val`, `shadow.val`, `*.sf_`, sky `*.tex`, prop `*.mod`, `*.gar` | various | Effects, baked shadows, ambient sounds, 15 skies, props, car setups (`default.gar`/`Advanced.gar`, NASCAR Heat "garage" setups). |

Global registration (in `common.res`, identical on both platforms): `tracks.val`
(id → display name; it also lists the cut tracks `Cedar` and `Shakopee`, which
have no `.trk`), the `<track>_eves.val` files (career/challenge/multi events per
track), and the hidden test eve `test: Empty Track` in `eves.val`. The code
accepts up to 40 tracks (§2.2).

### 3.2 What an importer or editor needs
1. **Easy (data only):** new events on existing tracks: move props, change
   bounds or ring-out, add obstacles (`c_*.va0` + frame64 codec). Rated **High**.
2. **Start from `EmptyTrack.trk`** (`EMPTYT_1.TRK` on PS2, 150 KB). It is a
   minimal track that the game already knows how to load: a tiny
   grf/sol/bsc/bsp/bpp/obt, `oval.ild`, `fig8.ild` and the camera/interlude
   tables. Un-hide the test eve to load it.
3. **Medium-hard (binary):** writers for GRAF (render mesh, PS2 `SPS4` parts),
   SOBL/SCOL/BSPT/BPPT (collision and partitions) and ILIN (lines). Good news
   from the PS2 check: **the collision, partition and line formats are
   byte-identical across platforms**, so one set of writers serves both, and
   the Xbox and PS2 builds can be used to cross-check the loaders. SCOL/BPPT
   have no public documentation (**unknown**) and would be reverse-engineered
   from the loader code in the recompiled ELF (`scol: tracing path took too many
   tris`, `using bpp terrain` are entry points).
4. Pipeline proposal: Blender scene (road, terrain and wall meshes tagged by
   material/surface) → exporter for GRAF (`SPS4` VIF writer, §2.5) + collision →
   spline-based `.ild` generator (§4) → event `.va0` placement from Blender
   empties → repack `0TSR` v6 with CRCs → register in `tracks.val` +
   `<new>_eves.val` → overlay with an 8.3 file name.
5. The trkedit leftovers (`trkedit.res`: `checkpt.mod`, `point.mod`,
   `Back.stp`/`empty.stp` "stamps", `road.tex`/`road.tm0`; `uiedit.res`:
   Open/Save dialogs, property dialog, editor arrows; an options section named
   `TRKEDIT`, present in the PS2 ELF too). Neither executable has strings that
   name the editor's files or dialogs, so **the editor code is compiled out**
   (strong guess). Do not plan on reviving it.

Feasibility: T1 **High**, T2 **Med-Low** (2–4 months for the first working
track, dominated by the collision formats and the VIF writer), T3 full in-game
editor **Low**.

---

## 4. AI on new maps

### 4.1 How the AI works (from data and strings; identical on both platforms)
* **Race AI follows precomputed lines.** `ILIN` (`.ild`) is a header
  (`lap length` ×3, e.g. 542.23 m for Afton's oval, which is about ⅓ mile; a scale
  factor; node count) plus a table of 60-byte nodes. From decoding Afton
  `oval.ild` (378 nodes), each node holds: segment length, cumulative `lon`
  (distance along the track), width (35.6), curvature, world X/Z, heading
  cos/sin, and an index. Lines are selected by `line_basename` in the event (`rr`
  → `rroval.ild` …). Code strings (all present in `SLUS_209.10`): `ai: Can't load
  centerline`, `Centerline: Can't load centerline (type 0x%x). Could an event req
  bw.ild?`, `CenterLine::convert_meters_to_ilpos`, `IdealLineGetCenterlineLength()`.
* **Ghost lines** (`ghostcar.res`, 86 `.ili`, byte-identical on both platforms:
  `<track>_e.ili`, `oval_<track>.ili`, `d_*`) are recorded laps. The Gauntlet
  needs them (`ghost: Can't run gauntlet event without backwards ghostcar`).
* **Arena/derby AI is algorithmic.** It is bounded by the `bound`/`ringout`
  polygons in `c_kill_*.va0` and states such as `ai kod: unhandled mode %d` and
  `Wrangle: unrecognized ai strength`. No lines are needed. So **new arenas get
  working derby AI for free** once bounds are written (guess, but strongly
  implied).
* Tuning: `track.val [AI]`, `lineinfo.val` lookahead and exceptions, per-event
  `speed_factor/skill_range` (+ `easy/medium/hard_sfd/srd`), and `dirt.val`
  (the driver roster: name, number, chassis, `rank`; `max_pack_size = 20`, read
  in `f_0013CEE0`).

### 4.2 New tracks
* **A2, auto-generating `.ild`: Med, 2–4 weeks.** Fit a smooth closed spline
  to the road mesh centreline (or let the author draw one in Blender), sample
  it at about 1.5 m, compute heading/curvature/width/`lon`, then optionally
  optimise toward a racing line (minimum-curvature solve). Remaining unknowns:
  4 node fields and the header's 4th float. Learn them by regenerating
  Afton's `oval.ild` from its own centreline and diffing. Because `.ild` files
  are identical on both platforms, one generator serves both.
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
  manager class (`DemoDerbyMaster`, `GauntletMaster`, `TrailerMaster`,
  `CaptureMaster`, `BattleMaster`, `KillDerbyMaster`, `WrangleMaster`,
  `FlagpoleMgr`, `P2PMaster` …).
* **Everything else is data.** A contest sets `laps, rounds, max_cars,
  damage_factor, player_damage_tweak, opt_terrain/terrain_mask, external_table`
  (which `c_*.va0` prop layout to use), `line_basename`, start/arena positions,
  AI skill and purse. All of these keys were found in the `*_eves.val` files.
* **The 2-player menu is data, and it is the same data on PS2.** Each track's
  `<track>_eves.val` has one `[eve] mode = multi` with ~25 contests (every track
  except HomeBase offers jump/normal/figure-8/derby/battle/capture/gauntlet/…,
  and `soccer` appears only in MP on 5 tracks). These files are semantically
  identical on both discs, so the Xbox decode applies to the PS2 port. Adding a
  contest to that list adds an MP option.
* Split screen and pads on PS2: `Multiplayer requires at least two
  controllers!` is in the ELF. The pad layer loads **`mtapman.irx`** (on the
  disc in `DATA/IOP/`) and opens up to five pad ports (`f_00331FD0`, "joy: Can't
  open port %d"), and there is a "Please reconnect the multitap" message. The
  options keep `PLAYER0..3`/`CONTROL1..4` sections, and the standings screen
  has `player_info_1..4` slots (Heat heritage). So **input and results UI
  already know about four players**; rendering does not (no split-screen
  strings for more than two views were found). There is no network code.

### 5.2 Feasibility
* **MP1, data-driven variants: High.** Examples: a "Reverse-arena Survival"
  using `kill_derby` with a new prop layout, a `school_bus` gauntlet with
  `max_cars` 14 and higher `damage_factor`, or a "Glass Cannon"
  approximated with `player_damage_tweak`/`damage_factor` plus a
  per-mode car `physics.val` override (guess: whether per-mode physics overrides
  are possible needs checking).
* **MP2, new rules: Med.** "Reverse Only" (input filter on the player's
  throttle mapping: do it in the port's own pad layer, the cleanest place),
  "Juggernaut" (bus chassis + damage scaling per car) and "Glass Cannon"
  (engine power ×2) are small HLE hooks once the functions are found.
  "Chain Gang" (tow cable between cars) might reuse the trailer hitch
  (`trailer: regluing to car...`), but this is a guess. **Low-Med.**
* **MP3a, more than 20 vehicles: Low.** Found on PS2:
  * The equivalent of the Xbox check near VA 0x23B77 is at **`001243F0`** in
    the eve loader `f_00123B98`: `sll v0,a2,1; slti v0,v0,0x15`, then
    `panic("Eve: %s has too many vehicles (%d cars + %d trailers > %d max)", …, 20)`.
    It runs only for contests with flag bit 0x800 (trailer events: cars ×2 ≤ 20).
  * The real cap is structural: `create_cars` (`f_0020FD48`) walks **20 car
    slots of 0x78 bytes** and the car count sits right after them (slot base +
    0x960 = 20 × 0x78). Raising it means relocating that array and every user
    of it, plus the AI arrays, the memory pools (`Panic: out of memory in
    pool(%s)`), and `dirt.val max_pack_size = 20`.
  * Related limits: at most **7 chassis per eve** (`00123EE0`) and **30
    chassis requests** in the texture/model map per race (`davmap`, `0012AB38`).
  * The data maximum is `max_cars = 14`. 32–64 cars (MODDING_IDEAS §2) remains
    a multi-month project with real risk.
* **MP3b, 3–4 player split screen: Low.** Input is ready (multitap, 4+ pads,
  4 player option sections, 4-slot standings UI). Missing: viewports, one
  camera and HUD per extra player, and GS/VU1 load for four scene passes. Next
  step if pursued: trace how the 2-player split sets its two scissor/viewport
  regions (from a GS trace of a 2P race) and find the code that loops over
  views.
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
  `gc_icon.cvs`) in `genesis.res` (on both discs). D2D was the studio's
  GameCube title.
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
(EoD: container v6 on PS2 / v7 on Xbox, MODL v9, GRAF v18, VALT v11, ILIN v27).

### 6.2 Which NASCAR versions to use
* **NASCAR Heat (PC, 2000)** has by far the best tooling. 3DSimED3 **imports
  and exports GRF and MOD "(Viper Racing and Nascar Heat)"** and unpacks "RES,
  TRK, CAR (Viper Racing and Nascar Heat)" [sim-garage.co.uk]. The community
  (The Mod Squad, HeatFinder, FFS Racing) has "hundreds of mods … over one
  thousand tracks", Restools (compile/decompile), Mod Launcher 2 and HeatCore.
  It is the best source of format knowledge, but it is the oldest format version.
* **NASCAR Heat 2002 (PS2/Xbox)** and **Dirt to Daytona (PS2/GC)** are closer in
  time to EoD and probably closer in format (guess). They have no known tools.
  A **PS2** copy of either would be the best like-for-like source for this
  port: probably the same `SPS4`/VIF mesh and palettized texture family
  (guess), which would avoid the mesh conversion step entirely.

### 6.3 Mismatch analysis
| Aspect | Issue | Severity |
|---|---|---|
| Container | Probably the same `0TSR` family; Heat PC archives may be compressed (3DSimED mentions "decompression"). EoD's CRC is now known, so repacking is no obstacle. | Low |
| Mesh (MOD/GRF) | Version gap (Heat ? → EoD v9/v18); and for the PS2 port, every mesh must become `SPS4` VIF packets (§2.5) | Med-High |
| Textures | Heat PC: its own `.tex` (palettized 16/256-colour era, "paint256.res"), close in spirit to EoD's PS2 format | Low-Med (convert through PNG) |
| **Damage** | Heat cars have `vertex_damage` (mod.cfg) but no EoD 42+157-bone rig, no detachable part groups, no radiator/steam emitters | **High**: every imported car must be re-rigged |
| Physics | Key names look shared (Pacejka `magic_*`, imperial units); stock-car numbers (≈3400 lb, 750 hp) go straight into `physics.val` | Low-Med |
| Scale | Same units (inches in physics, metres in world, guess); but superspeedway tracks are 2.5 mi against EoD's ⅓–½ mi short tracks: draw distance, LOD, AI `lon` ranges, skybox and the PS2's 32 MB budget are all stressed | Med-High for big ovals; Low for short tracks |
| AI lines | Heat tracks carry their own lines (likely an older ILIN version) or can use the A2 generator | Med |
| Track collision | Heat collision format (unknown) → EoD SOBL/SCOL/BPPT (platform-neutral) | High |

### 6.4 Feasibility
* N3 physics transplant: **Med-High**, days.
* N1 cars (Heat PC → EoD): **Med-Low**, after M3. Mesh through
  3DSimED/Blender → `SPS4` VIF writer (M3) → bone rig + part groups (could be
  semi-automatic: copy bone layout from a similar EoD car, e.g. a muscle-class
  chassis, and skin by proximity) → `physics.val` →
  `carclass.val`/`car_stats.val`/`buycar.val` registration → 8.3 file name.
  The first car takes about 1–2 months on top of M3, later ones days.
* N2 tracks: **Low-Med**. It depends entirely on T2. Short tracks
  (Bristol-size, dirt bullrings) are realistic. Daytona-size ovals are a stretch
  on PS2 memory budgets.

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
* PS2 texture swizzling (PSMT8/PSMT4) is standard, well-documented GS
  knowledge; the port's own GS code (`runtime/gs/`) already implements the
  layouts.
* Background: https://en.wikipedia.org/wiki/Monster_Games , https://en.wikipedia.org/wiki/NASCAR_Heat_2002

---

## 8. Recommended order (after macOS parity + 2P split-screen pass)
1. **Widescreen check (G2)**: pick "Widescreen (16:9)" in the game options with
   `AspectRatio=16:9`; check races, menus and 2P. If good, optionally wrap
   `f_0032EC58` so it is selected automatically. Hours, not days.
2. **F1 mod overlay** in `runtime/iop/stream.cpp` `build_disc()`: merge
   `mods/*/DATA` over `DATA`, with a per-mod enable list in the ini. Enforce
   8.3 upper-case names and warn near the 256-entry TOC limit. It needs no game
   code, is reversible, and keeps the default game byte-identical.
3. **F2 + F3 tooling** (Python, in `tools/`): `rst0.py unpack/pack` (PS2 v6
   header + the CRC from §2.1), `valt.py decompile/compile` (text form modelled
   on Heat's `table … end` syntax), and a frame64 codec. Test: repack an archive
   with one changed `.val` value and confirm the game loads it.
4. **Quick data wins** (each a few hours, all opt-in): LOD/draw distance,
   AI difficulty tuning, MP variants (MP1), car re-tunes (M1), prop/arena
   layout edits (T1), un-hiding the `EmptyTrack` test eve as a sandbox.
5. **Renderer extras** on the Mac backend: post effects (G5), then the
   texture dump/replace (G3) with names logged from `f_00350F58`, then the
   Xbox true-colour substitution (G4) if still wanted.
6. **PS2 graphics writers**: `TEX` writer (confirm the 4-bit swizzle first),
   then the `SPS4` VIF mesh format, validated by converting Xbox `DNIS` parts
   and comparing with the PS2 originals. This unlocks M2, M3 and T2.
7. **`.ild` generator (A2)**, validated by regenerating Afton's oval.
8. **First new car (M3/N1)**, then the **first new short track (T2/N2)**.
9. Only then the risky items: >20 vehicles (MP3a), 3–4 player split screen
   (MP3b), new rule types (MP2), 120 Hz (G7).

## 9. Open questions / unknowns
1. Does the game's widescreen mode look right in the port (HUD, menus, 2P
   split, movies), and does the 2P split already use its own x-scale?
2. 4-bit texture swizzle (PSMT4) and the meaning of format bit 0x200 and flag
   bits 0x30000.
3. When are `.tm0` high-detail textures used (always on demand, 1-player only,
   or by `detail_level`/`high_detail_car`)? Relevant for G3 and for memory in
   3–4 player modes.
4. `SPS4` packet details: vertex formats per VU1 program, how bone weights for
   crumple damage are stored, batch limits.
5. The TOC limit: can the EE side take more than 256 entries? How does
   `f_00331708` resolve two long names with the same first 6 characters?
6. Meaning of the remaining fields: archive header `0x20`, VALT header word 2,
   ILIN header float 4 and 4 node fields, Xbox MODL vertex last 4 bytes, the
   TEX `0x40000000`/`0x80000000` version flags.
7. SOBL/SCOL/BSPT/BPPT semantics: these are the critical path for new tracks
   (same on both platforms, so either executable can be studied).
8. How cars are registered (chassis lists in `carclass.val`, `car_stats.val`,
   `buycar.val`, `catalog.val`): is there also a hard-coded chassis table in the
   code? (`ghost: %8x replacing illegal chassis %s w/%s` suggests a validity
   list; `world: Can't map CarType %d` suggests a type table.)
9. NASCAR Heat PC chunk versions vs EoD's. One look at a Heat `.car` with the
   §2.1 parser settles how far apart they are.
10. Can per-event car physics overrides be expressed in data (for "Glass Cannon"
    etc.), or do they need HLE hooks?
11. Where is the per-view loop for split screen, and how much of it assumes two
    views (MP3b)?
