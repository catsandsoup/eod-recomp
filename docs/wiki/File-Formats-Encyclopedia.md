# File Formats Encyclopedia — All 24 FourCC Formats, 72 Archives & Host Extensions

Nothing on the retail *Test Drive: Eve of Destruction* (`SLUS_209.10`) disc is encrypted. The disc contains **72 `0TSR` v6 archives** (`DATA/CAR/*.CAR`, `DATA/RES/*.RES`, `DATA/TRACKS/*.TRK`) holding **7,329 internal members** across **24 distinct FourCC asset types**, plus external media (`DATA/SPEECH.BNK`, 40 `.PSS`/`.M2V` video streams, and 13 `.IRX` IOP modules).

This encyclopedia documents the binary layout, `SLUS_209.10` loader address, and Python/C++ tooling for every format on the disc—plus our modern `.eodhtx` (`HOSTTEX`), `.eodworld.json`, `.eodmod`, and `HSCM` v3 save containers.

---

## 1. Master Census of All 24 Disc FourCC Types

| # | FourCC | Extensions | Ver | Disc Count | `SLUS_209.10` Loader | Decoder / Tool (`tools/`) | Modding & Edit Status |
|---|---|---|---:|---:|---|---|---|
| **1** | **`0TSR`** | `.trk`, `.car`, `.res` | 6 | **72 archives** | `0x0033D408` (`StrmMountArchive`), CRC `0x00135FC8` | `tools/rst0.py` | **Full Read/Write:** Byte-exact unchanged repack, in-place same-size member patch, or full resized repack with MSB-first CRC-32 (`0x04C11DB7`). |
| **2** | **`TEX `** | `.tex` (3,309), `.tm0` (1,547), `.art` (67) | 10 | **4,923** | `0x003521E8` (`TextureRelocateOffsets`), `0x003545B8` (`.tm0`) | `tools/tex.py`, `tools/texture_edit.py`, `tools/texture_identity.py` | **100% Decode (4,923/4,923) + Full Write:** Native indexed PNG re-import **AND** 1x–32x (up to 4K) true-color Vulkan `.eodhtx` (`HOSTTEX`). |
| **3** | **`VALT`** | `.val` (354), `.va0` (338), `.mvl` (36), `.pln` (27) | 11 (v6) | **755** | `0x00134A70` (`ValtOpen`), `0x00135C80` (`ValtFindKey`) | `tools/valt.py`, `tools/formats/valt_schema.py` | **Full Read/Write (v11):** 2,851 key patterns schema-labeled across 25 families (`physics.val`, `c_*.va0`, `*_eves.val`, `shadow.val`, `obstacle.val`). |
| **4** | **`MODL`** | `.mod` | 9 | **490** | `0x0034CEF0` (`ModelLoadFromArchive`), `0x0034B720` | `runtime/render/vif_mesh.cpp`, `tools/vif_mesh.py`, `tools/vif_patch.py`, `tools/modl.py` | **100% 3D Decode + Patch:** Full OBJ/MTL export, texture rebinding, same-topology vertex/UV/normal patching (`vif_patch.py`), and NASCAR VIF transcoding. |
| **5** | **`IMAG`** | `.img` | 3 | **264** | `0x00349078` / `0x0034A0F8` | `tools/formats/imag.py` | **Full Read/Write (264/264):** 1–20 frames of 8-bit indexed RGBA (max 512 px); replace any frame with a PNG. |
| **6** | **`ILIN`** | `.ild` (AI line), `.ili` (ghost) | 27 | **211** | `0x00137A68` (`AICenterlineLoad`), `0x001174D0` (ghost) | `tools/formats/ilin.py`, `tools/trk.py` | **Full Read/Write (190 non-empty + 21 0-byte `.ili`):** Interactive 3D Road Tool in World Editor; auto-recomputes `dirX/dirZ`, `segLen`, `dist`, and header lap lengths. |
| **7** | **`LAYT`** | `.lyt` | 5 | **200** | `0x00270F10` (`LayoutLoad`), builders `0x0026D5E8` | `tools/formats/layt.py` | **Full Read/Write (200/200):** 640×480 menu widget trees (art, labels, buttons, glyphs, steppers, tables); edit widget positions, sizes, fonts, and bindings. |
| **8** | **`SFX0`** | `.sfx` (87), `.sf_` (25) | 3 | **112** | `0x0033ACB8` (`SfxLoad`) | `tools/audio.py` | **Full Read/Write:** PS2 SPU2 ADPCM mono sound effects ↔ standard `.WAV` round-trip conversion. |
| **9** | **`STAB`** | `.tab` (74), `.obt` (15) | 0 | **89** | `0x00132E98` (`StringTableLoad`), `0x00128AF0` | `tools/modding_backend.py`, `tools/nascar_wheels.py` | **Read / Table Decode:** Lookup and string tables (`ranked_list.tab`, `camera.tab`, `Rtire.tab` 5×2 table with `0x4C` header and 33B cells, `track.obt`). |
| **10** | **`SETU`** | `.gar` | 2 | **64** | **None** *(verified unused by `SLUS_209.10`!)* | `tools/formats/setu.py` | **Full Read/Write (64/64):** 196-byte (49-float) legacy NASCAR garage setup sheets; flagged in UI as unused by runtime (edit `physics.val` instead). |
| **11** | **`GTEX`** | `.gtx` | 4 | **62** | UI font renderer | `tools/modding_backend.py` | **Inspected (38,912 B fixed):** Monospaced & proportional UI font glyph sheets and kerning tables. |
| **12** | **`CANV`** | `.cvs` | 1 | **35** | UI canvas loader | `tools/modding_backend.py` | **Inspected (up to 524,324 B):** Full-screen 512×512 UI background canvases (garage, dealership, title screen). |
| **13** | **`Anim`** | `.anm` | 2 | **18** | Skeletal animation loader | `tools/modding_backend.py` | **Inspected (4,128 B):** Driver & character skeletal animation headers (25 human bones, 7 chicken bones). |
| **14** | **`Anm0`** | `.an0` | 2 | **15** | Keyframe stream evaluator | `tools/modding_backend.py` | **Inspected (20–166 KB):** Skeletal keyframe transform streams (`Drv_winA`, `spec_03A`). |
| **15** | **`GRAF`** | `.grf` (`track.grf`) | 18 | **15** | `0x00214678` (`TrackLoadVisualScene`) | `tools/world_graf.py`, `tools/world_export.py` | **Full Read + In-Place / Model-Swap Write:** Display tree (`0x0`, `0x101..103`, `0x4`), 34 display-object subtypes, 2 `mlist` draw lists, embedded `mrModel` + `4SPS` VIF batches. |
| **16** | **`SOBL`** | `.sol` (`track.sol`) | 3 | **15** | `0x001A00B8` (`TrackLoadSolidCollision`), broadphase `0x00177CA0` | `tools/world_collision.py` | **Full Read + Barrier Height/Flatten Write:** 264-byte (`0x108`) OBB `BOX ` / `TUBE` records + 2D XZ quadtree; shrink/flatten 16m invisible walls safely in place. |
| **17** | **`SCOL`** | `.bsc` (`track.bsc`) | 4 | **15** | `0x00226768` (`TrackLoadSurfaceCollision`), lookup `0x00226F40` | `tools/world_collision.py` | **Full Read + Terrain Raycast / Plane Sync:** 32-byte winged-edge Table 1 + 20-byte plane equation & surface-ID Table 2 + optional-terrain mask bits (`+0x1E`). |
| **18** | **`BSPT`** | `.bsp` (`track.bsp`) | 0 | **15** | `0x00216D98` / `0x00216ED0` | `tools/world_collision.py` | **Full Read (108 B):** Single bounding ground triangle for fast segment queries; preserved byte-for-byte. |
| **19** | **`BPPT`** | `.bpp` (`track.bpp`) | — | **15** | `0x00216F60` (`"Indiv BPPTree"`) | `tools/world_collision.py` | **Preserved:** Tool-serialized partition tree with Win32 pointers; unused by retail tracks at runtime. |
| **20** | **`ESFX`** | `.ens` | 2 | **6** | Engine audio synthesizer | `tools/nascar_engine_audio.py` | **Inspected (356 B – 4 KB):** Engine RPM pitch/blend sound definitions (`PART.RES#eng1e.ens`). |
| **21** | **`STMP`** | `.stp` | 3 | **4** | Paint shop stamp loader | `tools/modding_backend.py` | **Inspected (135 KB):** Custom paint-kit decal stamp layers (`PAINTKIT.RES#base_layer.stp`). |
| **22** | **`LANG`** | `.lng` | 1 | **2** | `0x001342A8` (`LangLoad`), lookup `0x00134620` | `tools/formats/lang.py` | **Full Read/Write:** `LOCALE.RES#usenglish_lang.lng` (38,751 B, **877 `Group:Name` entries**); supports longer strings with `%d` & Latin-1 validation. |
| **23** | **`BOPT`** | `.opt` | 1 | **2** | Options preset loader | `tools/modding_backend.py` | **Inspected (612 B):** Binary option presets (`GENESIS.RES#tune.opt`, `default.opt`). |
| **24** | **`XDSP` / `ICON`** | `.dsp` (1), `.icn` (1) | 1 | **2** | DSP / PS2 Memory Card icon | `tools/modding_backend.py` | **Inspected:** `GENESIS.RES#xtalk.dsp` (4,068 B) and `mem_icon.icn` (75,736 B 3D save icon). |

---

## 2. Deep-Dive Format Specifications

### 2.1 `0TSR` v6 Archive Container (`.TRK`, `.CAR`, `.RES`)
- **Tool:** `tools/rst0.py` · **Loader:** `StrmMountArchive` (`0x0033D408`), `StrmFindFile` (`0x0033D4F8`)
- **Header (`0x28` / 40 bytes at `0x00`, padded to `0x80`):**
  - `+0x00`: Magic `'0TSR'` (`0x52535430`, reversed `"RST0"`).
  - `+0x04`: `u32 version` (`6` on all 70 EOD archives; *NASCAR: Dirt to Daytona* uses `2`).
  - `+0x08`: `u32 dir_offset` (`0x80`).
  - `+0x0C`: `u32 entry_count`.
  - `+0x10`: `u32 extract_block` — **Resident vs. Streamed Partition Boundary**:
    - Entries with `offset < extract_block` are **resident** (`0x80`-byte aligned) and loaded into the EE `"STAK"` heap when the archive is mounted.
    - Entries with `offset >= extract_block` are **streamed on demand** via `stream.irx` (`0x800`-byte / 2,048-byte DVD sector aligned, such as `.tm0` hi-res mip0 textures).
- **Directory Entries (`0x44` / 68 bytes each, starting at `0x80`):**
  - `+0x00`: `char name[32]` (null-terminated ASCII filename, e.g. `"track.grf"`).
  - `+0x20`: `char fourcc_rev[4]` (byte-reversed FourCC, e.g. `'FARG'` for `GRAF`, `' XET'` for `TEX `).
  - `+0x24`: `u32 type_version` (e.g. `18` for `GRAF`, `10` for `TEX `, `9` for `MODL`).
  - `+0x28`: `u32 size` (uncompressed byte length).
  - `+0x2C`: `u32 data_offset` (byte offset relative to directory start `0x80`).
  - `+0x30`: `u32 eod_crc` — **Hardware MSB-First CRC-32** computed by `f_00135FC8` using the 256-word table at `0x003F2598` (generator polynomial `0x04C11DB7`, initial accumulator `0x00000000`, shifted MSB-first `(crc << 8) ^ table[((crc >> 24) ^ byte) & 0xFF]`, bitwise inverted `~crc` at return).
  - `+0x34..0x43`: `16 bytes` reserved (`0`).

---

### 2.2 `GRAF` v18 Track Visual Scene Graph (`track.grf`)
- **Tools:** `tools/world_graf.py`, `tools/world_export.py` · **Loader:** `TrackLoadVisualScene` (`0x00214678` → `*(0x003D1C18)`)
- **Top-Level Header (`12 bytes`):**
  - `+0x00`: `u32 display_list_root_offset` (relocated by `0x002156C8` & `0x00214258`, initialized by `0x00214468`).
  - `+0x04`: `u32 mlist_a_offset` (fine single-player draw list, relocated by `0x00358F50`).
  - `+0x08`: `u32 mlist_b_offset` (coarse split-screen draw list; bypassed when `SplitScreenParity=1`).
- **Display Tree Nodes:**
  - `type 0x0` (**LOD / Group**): `+0x04 next`, `+0x08 vec3 centre`, `+0x14 u32 count`, followed by `count × {f32 maxDistanceSq, u32 child_offset}` (child 0 = highest detail).
  - `type 0x101 / 0x103` (**Mode Condition**): `+0x04 next`, `+0x08 child`, `+0x0C / +0x1C s32 value` (checked against session flag `0x3D1B34`).
  - `type 0x102` (**Cull Sphere**): `+0x04 next`, `+0x08 child`, `+0x0C vec3 centre`, `+0x18 f32 radius`.
  - `type 0x4` (**Leaf**): `+0x04 next`, `+0x08 u32 model_record_offset`.
- **Model Record (`leaf + 0x08`):**
  - `+0x00`: `u32 kind` (`0` = simple static model, `1` = display object).
  - `+0x08`: `u32 mrModel_offset` (relative to model record).
  - `+0x10`: `u32 subtype` (for `kind == 1`, index `0..33` into constructor table `0x00406B40`: `0` Simple, `1` Point-Facing, `2` Axis-Facing, `3` Dyno `0x221E80`, `4` Sky `0x221E00`, `9` Sun, `15` Ambient, `16` Shadow, `17` SunBright, `18` FogColor, `19` NamedPoint, `20` Optional Terrain `0x221BC8`, `28` Water `0x223E00`).
  - **Subtype 20 (`Optional Terrain`, ramps/crossings):** `+0x18 / +0x1C` = `mlist` toggle indices, `+0x2C` = 1-based mode index converted to bitmask `1 << (mode - 1)` by `0x002230F8` and tested against `session(0x495BE0) + 0x3C`.
  - **Subtype 3 (`Dyno`, invisible guardrail wobble boxes):** `+0x2C` = spawn mode (`-1` = unspawned), `+0x30` = dyno kind (`0..7`), `+0x34` = `'dyno'`, `+0x38..0x67` = `4×3` rigid world matrix, `+0x68..0x73` = half-extents.
- **`mlist` Draw Lists (`0x94`-byte header, evaluated by `0x003582B8 MlistVisibilityCalc` & `0x00358BD0 WorldRenderPass`):**
  - Contains `16B` frustum spheres (`+0x10/+0x14`), `32B` LOD groups (`+0x18/+0x1C`), `32B` model descriptors (`+0x20/+0x24`), `12B` world batches (`+0x28/+0x2C`: `{u16 scratchpad_idx, u16 req_val, u16 matrix_idx, u16 qwc, u32 part_ptr}`), `0x1C`-byte texture buckets (`+0x30/+0x34`), and 5 render-pass ranges (`+0x38 + 16*pass`: passes `0..4`).

---

### 2.3 `MODL` v9 & Embedded `mrModel` (`*.mod` & `track.grf`)
- **Tools:** `runtime/render/vif_mesh.cpp`, `tools/vif_mesh.py`, `tools/vif_patch.py`, `tools/modl.py`
- **Header Layout:**
  - `+0x00`: `u32 nParts` · `+0x04`: `u32 partTableOffset` (`0x58` in standalone `.mod`, `+0x50/+0x54` in embedded `mrModel`).
  - `+0x08`: `u32 nExtras` · `+0x0C`: `u32 extraTableOffset` (`'TJRM'` 80-byte attachment sockets: 32-byte name like `"left front wheel"`, `"right hood hinge"`, `"butt"`, `"rhand"`, `3×3` rotation matrix, `vec3` position in metres).
  - `+0x10`: `u32 nInstances` · `+0x14`: `u32 instanceTableOffset`.
  - `+0x18`: `f32 bounding_radius` · `+0x1C`: `vec3 centre` · `+0x28..0x3F`: `f32 bbox[6]` (`minX, maxX, minY, maxY, minZ, maxZ`).
- **`4SPS` Geometry Chunks:** See **[Native Plume Renderer §2](Native-Plume-Renderer#2-c17-vif-mesh-decoder-runtimerendervif_meshcpp)** for the complete VIF1 `UNPACK` and `MSCAL` specification.

---

### 2.4 `TEX ` v10 Native PS2 Textures & `.eodhtx` (`HOSTTEX`)
- **Tools:** `tools/tex.py`, `tools/texture_edit.py`, `tools/texture_identity.py` · **Runtime:** `0x003521E8`, `0x0035EE38`, `runtime/gs/texture_replacements.cpp`
- **Native `TEX ` v10 (`0x80`-byte header; NASCAR v9 uses `0x60`):**
  - Formats: `0x201` (`PSMT8`, 8-bit indexed with 256-entry 32-bit RGBA CLUT) and `0x200` (`PSMT4`, 4-bit indexed with 16-entry CLUT), plus direct color variants.
  - **GS Swizzle Rule (`0x0035EE38` / `0x0035F140`):** Mip levels with `height >= 32` use PS2 GS compact page/block swizzling (`upload_addr8`); small mip tails with `height < 32` branch to linear storage (`0x0035F140`).
  - **Hi-Res `.tm0` Streamed Mip0 (`1,547` files):** Streamed dynamically from the archive's extractable partition into the 400 KB circular ring buffer at `0x004A9830` by `TextureLoadHiResTm0` (`0x003545B8`) and bound to the parent `TEX` descriptor via `0x003537F0`.
- **Vulkan Host Texture Replacement (`.eodhtx` & `HOSTTEX/replacements.tsv`):**
  - Identity keys (`tools/texture_identity.py`): FNV-1a 64-bit **`indexKey`** (hashes PSM, width, height, and unswizzled mip-0 index plane) and **`paletteKey`** (hashes expanded 256×RGB CLUT, or `*` wildcard for CLUT-animated textures).
  - `.eodhtx` binary container: stores straight-alpha `R8G8B8A8` pixels at a shared power-of-two scale, up to 8192 pixels per side, with a complete 2×2 box-filtered mipmap chain. Textures are uploaded on demand under `EOD_HOST_TEXTURE_BUDGET_MB` (default `1536 MB`).

---

### 2.5 `ILIN` v27 AI Racing & Ghost Lines (`*.ild`, `*.ili`)
- **Tools:** `tools/formats/ilin.py`, `tools/trk.py` · **Loaders:** `0x00137A68` (AI centerline → `0x003CD190`), `0x0013D9A8` (layout mapper), `0x001174D0` (ghost line)
- **Header (`48 bytes`):**
  - `+0x00`: `u32 runtime_tag` (`0` on disc; stamped with `0x104F128B` in memory after pointer relocation).
  - `+0x04`: `u32 header_size` (`48`) · `+0x08`: `u32 node_size` (`60`).
  - `+0x0C`, `+0x10`, `+0x14`: `3 × f32 total_lap_length_m` (three identical copies equal to $\sum \text{segLen}_i$).
  - `+0x1C`: `u16 reserved (0)`, `u16 main_node_count`.
  - `+0x20`: `u32 node_array_offset` · `+0x24`: `u32 first_side_node_offset` (`0` if none).
  - `+0x28`: `u32 ghost_record_offset` (`0` in `.ild`; in `.ili`, points to recorded ghost run & 20-byte lap summary at 59.94 Hz).
  - `+0x2C`: `u32 pointer_fixup_count` (followed immediately at `+0x30` by `count × u32` file offsets of every pointer in the file).
- **60-Byte Node Record (`node_size = 60`):**
  - `+0x00`: `f32 x`, `+0x04`: `f32 z` (world X/Z position in metres; no Y stored).
  - `+0x08`: `f32 dir_x`, `+0x0C`: `f32 dir_z` (unit tangent vector toward next node).
  - `+0x10`: `u32 index_or_merge` (`index << 16` on main loop nodes; `16.16` fixed-point main-line merge parameter on side-branch nodes).
  - `+0x14`: `u32 next_node_offset` (file offset; last main node wraps back to node 0).
  - `+0x18`: `f32 seg_len` (Euclidean distance in metres to next node).
  - `+0x1C`: `f32 cumulative_dist` (distance in metres from node 0).
  - `+0x20..0x30`: `5 × f32` speed/width metadata (`+0x20` = `35.556f` on main lines, half on side lines).
  - `+0x34`: `u32 flags` (`256` on main nodes) · `+0x38`: `u32 side_line_list_ptr`.

---

### 2.6 `SOBL` v3 Static Collision & Invisible Walls (`track.sol`)
- **Tool:** `tools/world_collision.py` · **Loader:** `TrackLoadSolidCollision` (`0x001A00B8` → `*(0x004473FC)`), broadphase `0x00177CA0`
- **Header (`0x14` / 20 bytes):** `u32 count`, `u32 index_len`, `3 × u32` runtime pointers (`0` on disc).
- **Solid Records (`0x108` / 264 bytes each, starting at `0x14`):**
  - `+0x00..0x23`: `9 × f32` orthonormal `3×3` rotation rows (local X, Y, Z axes in world space).
  - `+0x24..0x2F`: `vec3 centre` (world X, Y, Z in metres).
  - `+0x30`: `s32 link` (`-1`) · `+0x34`: `u32 surface_type` (`100` default, `101` concrete, `105` metal, `106` plastic, `107` rubber, `108` wood, `110` wall, `111` hay; table at `0x004090F0`).
  - `+0x38`: `u32 shape_tag` (`'BOX '` ctor `0x001A06D0` or `'TUBE'` ctor `0x0019E368`).
  - `+0x40..0x107`: Embedded shape object (`shape + 0x20` = `+0x60` in record):
    - **`'BOX '`:** `+0x60 f32 rx`, `+0x64 f32 ry` (**half-height**, typically **`15.0 m`** on perimeter walls → 16 m invisible ceiling!), `+0x68 f32 rz`, `+0x6C f32 sphere_radius` ($\|\vec{r}\| + 0.2\text{ m}$), `+0x70 u32 box_flags` (`0..3`).
    - **`'TUBE'`:** `+0x60 f32 radius`, `+0x64 f32 half_length` along local Z, plus two cached end-spheres.
- **2D XZ Broadphase Quadtree (After Records):**
  - `u16 index[index_len]` record numbers followed by 8-byte quadtree nodes `{u16 first, u16 end, u16 child, u16 pad}`.
  - **Crucial Discovery:** `f_00177CA0` subdivides only **X and Z** (in decimetres, `±40,000,000` root bounds) and **never tests Y**! Therefore, shrinking/lowering `ry` and adjusting `centre.y` + `sphere_radius` in place (via `world_collision.py set-limit` or the World Editor's **Flatten Barriers** tool) removes 16 m invisible walls without invalidating a single quadtree cell!

---

### 2.7 `SCOL` v4 Drivable Terrain Surface Mesh (`track.bsc`)
- **Tool:** `tools/world_collision.py` · **Loader:** `TrackLoadSurfaceCollision` (`0x00226768` → `*(0x003D2098)`), point lookup `0x00226F40`, height `0x00227AE0`, surface `0x00227878`
- **Header (`12 bytes`):** `u32 triangle_count` (`4,622` on Bemidji up to `11,434` on HomeBase), `u32 table1_offset` (`12`), `u32 table2_offset`.
- **Table 1 — 2D Winged-Edge Topology (`32 bytes` per triangle):**
  - `3 × {i16 nx, i16 nz, f32 d}`: 2D half-space edge planes in X/Z (point $(x, z)$ is inside edge $k \iff \frac{nx_k}{32767}x + \frac{nz_k}{32767}z + d_k \ge 0$).
  - `3 × i16 neighbour_tri_index` (`-1` = open boundary edge).
  - `u16 optional_terrain_bits` (`+0x1E`): When non-zero, the triangle is only active when `(bits & *(u32*)0x003D1C70) != 0` (set per event from `opt_terrain` in `COMMON.RES#<track>_eves.val`—e.g. jump ramps and figure-8 crossovers!).
- **Table 2 — 3D Plane Equations & Surface Materials (`20 bytes` per triangle):**
  - `f32 a, b, c, d`: 3D plane equation ($y = -(a x + c z + d) / b$, where $\vec{n} = (a, b, c)$ is the surface normal).
  - `i16 surface_id`: Physics traction/dust material (`0` Grass, `2` Dirt Bullring, `5` Mud, `6` Asphalt, `14` Concrete Curbs, `20` Farm Terrain).
  - `u8 aux0, u8 aux1`: Per-triangle flags read by `0x002277E8`.

---

### 2.8 `VALT` v11 Key/Value Tables, `unfloat_frame64`, `LANG`, `LAYT`, `IMAG` & `HSCM` v3 Saves
- **`VALT` v11 (`.val`, `.va0`, `.mvl`, `.pln` — 755 files):**
  - Hierarchical binary sections with case-insensitive key lookup (`0x00135C80`, first match wins) and parent-section inheritance (`section + 20`). Child tables are named `$NNNN$name` and referenced via `@name` links.
  - **`unfloat_frame64` (64-character base-64 4×3 transform in `c_<event>.va0`):** Encoded by `f_00133A50` over the 64-character alphabet at `0x003CCFC0` (`a-z A-Z 0-9 , .`), packing 4 symbols per 3 bytes (MSB 6 bits first) into 48 bytes = `12 × f32` (`3×3` rotation rows X, Y, Z + `vec3` position in metres). Decodes and re-encodes bit-exactly via `tools/world_export.py`.
- **`LANG` v1 (`LOCALE.RES#usenglish_lang.lng`):** Decoded by `tools/formats/lang.py` (`0x001342A8`). Contains **877 `Group:Name` localized text entries**; supports in-place or resized string replacement with format-specifier (`%d`, `%s`) validation.
- **`LAYT` v5 (`*.lyt`, 200 files) & `IMAG` v3 (`*.img`, 264 files):** Decoded by `tools/formats/layt.py` (`0x00270F10`) and `tools/formats/imag.py` (`0x00349078`). `LAYT` defines `640×480` UI widget trees; `IMAG` stores 1–20 frames of 8-bit indexed RGBA interface sprites.
- **`HSCM` v3 Memory Card Save Container (`CareerSaveInfo`):**
  - 144-byte (`0x90`) header: magic `'HSCM'` (`0x4D435348`), version `3`, payload size at `+0x0C`, and a **32-bit rotational XOR checksum** at `+0x10` seeded with constant **`0xFEEDFACEu`**.
  - Wraps the `'Crer'` section containing `CareerSaveInfo` (`0xE7C` / 3,708 bytes at `*(0x003D9384)`), `CDBupgr` (`0xC68` / 3,176 bytes at `+0x80` with player cash, parts inventory, and 4 garage car slots × `0x300` B), and `CDBResSaveInfo` (`0x194` / 404 bytes at `+0xCE8` with player rank and Eve trophies).
