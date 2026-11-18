# VIF mesh decoder (`runtime/render/vif_mesh`)

C++17 port of `tools/vif_mesh.py`: turns a model part's prebuilt VIF1 stream into a
renderer-ready mesh (positions, normals or vertex colours, UVs, skin bones/weights, triangle
list, texture name and group per part). No game/runtime dependencies, no exceptions, one
reusable `Decoder` + `Mesh` per thread. Test: `tools/tests/vif_mesh_test.cpp`.

Confidence: **V** = checked against all retail data or read in the VU1 microcode;
**I** = inferred, not verified in a game run.

## API

| Call | Input |
|---|---|
| `Decoder::decode_vif(vif, bytes, mesh, opt, texture, group, slot)` | a raw VIF stream: draw-queue batch `+0x38`, `qwc*16` bytes (`qwc` at `+0x34`) |
| `decode_part(part, avail, mesh, opt, slot)` | a `4SPS` part header |
| `decode_modl(data, size, mesh, opt)` | a stand-alone `.mod` member |
| `decode_mr_model(data, size, model_offset, mesh, opt)` | an embedded mrModel (`track.grf`) |
| `decode_part_guest(read, ctx, addr, mesh, opt, slot)` | a `4SPS` part in guest memory through `bool read(ctx, addr, dst, n)` |

Every call appends `Part`s, `Batch`es, `Vertex`es and part-relative `uint32_t` indices (3 per
triangle) to the `Mesh` and returns a `Status` (`Ok`, `Truncated`, `UnknownCommand`,
`FillMode`, `BadAddress`, `BadHeader`, `NotPart`, `ReadFailed`); a failed part keeps the
batches decoded before the error and records its status. `Options::fix_winding` (default on,
the Python preview contract) and `drop_degenerate` (default on).

## Containers

* `4SPS` part: `+4` size, `+8` texture[16], `+0x18` group (low 16 bits damage/visibility
  group, bit 16 body paint), `+0x34` VIF qword count, VIF from `+0x38`. `qwc == (size-0x38)/16`
  on all 44,584 retail parts. **V**
* `.mod`: part slots from `0x58` to header word `0x14`; `0xE3E3E3E3` = empty, non-`4SPS`
  slots (TJRM markers) skipped. mrModel: `+0` nParts, `+4` model-relative part table.
  (`docs/WORLD_FORMAT.md`) **V**

## Batch layout (one MSCAL)

| VU qword | UNPACK (retail) | Decoded |
|---|---|---|
| 0 | V1-32 | vertex count `& 0x7FFF` (bit 15 set) |
| 2+3i | V3-16 / V3-8 / V3-32, offset mode | position = `f32(row + u) + q199.xyz` |
| 1+3i | V4-8 offset, STROW `65536.0f` x3, `0x7F80` | normal `(b-128)/127`, ADC = byte 3 bit 7 |
| 1+3i | V4-5 (or V4-8 without the normal row) | vertex colour RGB (`(v>>5k & 31) << 3`), ADC = bit 15 |
| 3+3i | V2-16 offset | uv = `f32(row + u) + q198.xy` |
| 150.. | V4-8 offset, STROW `(0,0,32768.0f,32768.0f)` / V1-8 | per-vertex skin word (below) |
| 198/199 | V4-32 | q198 = (s off, t off, flags, flags), q199 = position offset |

Program 4 (shadow source, `MSCAL 4`) uses a 2-qword stride (attribute, position) and no UV.
A normal qword is recognised as in `runtime/vu/scene_emit.cpp`: unpacked x lane
`& 0xFFFFFF00 == 0x47800000`. Retail: 601,688 normal vertices (cars, props, drivers),
1,367,745 colour vertices (all world programs `MSCAL 0x4E`/`MSCNT`, `paint.mod`, `envsky`,
`sci_wall`). `docs/VIF_GEOMETRY_PATCHING.md`'s "no colour channel" holds for MODL members
except those three; the GRAF world geometry is prelit V4-5 colour. **V** (data)

* Colour bytes are kept on the PS2 scale (`0x80` = 1.0, V4-5 max 248); alpha is written as
  `0x80`, because the fourth lane is the ADC flag, not alpha (as `scene_emit` `modern_vertex`).
  Whether the world program kicks a constant alpha: **I**.
* Normals: `(b-128)/127` matches the Python. The VU (body program at VU1 `0x948`) computes
  `(65536 + b/128) - 65537 = (b-128)/128` (constant `loi 65537`, `subx`). **V** (microcode)
* Triangles: strip with ADC restart as `strip_triangles`, then `fix_winding` (face normal vs
  summed vertex normals, in double), then degenerate drop (equal double positions). For colour
  batches the Python, and therefore this port, reads the colour bytes as normals for the
  winding test; use `fix_winding = false` for raw strip parity. World geometry is drawn without
  culling.

## Skin word (qword 150..)

VU1 microcode (`config/vu/vu1_fd0ce30ab0ec263c.bin`, body loop at `0x948`): `vi3 = count`,
`lq vf22, 149(vi3 + TOP)`, decrementing `vi3` per vertex: vertex k reads **TOP+149+n-k**, i.e.
the skin block is stored in reverse vertex order. `mtir vi6, vf22x` / `vf22y` then
`lq 143(vi6)`: bone fields are absolute VU1 qword bases. `suby.zw vf13, vf22, vf3y` with
`vf3.y = 32768`: weights = lane minus 32768 = byte/256. **V** (microcode; runtime skin check in
`docs/SCENE_CAPTURE.md` "Bones")

| Program (file MSCAL) | Data | Decoder | Meaning |
|---|---|---|---|
| `0x5C` (VU1 `0x2e0`, cars, 466,057 verts) | V4-8 | `Skin::Offset`, bone 0..79 | dent translation at qword `bone+143`; dent uploads start at `0x90` = bone 1, so bone 0 reads qword 143 (no dent) **I** |
| `0x63` (VU1 `0x318`, drivers, 41,760 verts) | V4-8 | `Skin::Matrix`, bone 0..72, all multiples of 4 | 3x4 matrix columns at `bone+144..147` = palette entry `bone/4` (uploads at `0x90 + 4*first`) **V** |
| `0x5C`, one byte (bus, debris, 21,817 verts) | V1-8 | `Skin::Partial`, `bone[0]` only | the program still reads `.y/.z/.w`; their contents are stale VU memory, not in the file. Open |
| `0x6A` (VU1 `0x350`, facing sprites, 9,824 verts) | V4-8 | `aux` only | not skin; meaning not decoded |

Weight byte sums: 254 (374,235 vertices), 255 (133,579), other 3. VU-order pairing gives
tighter same-bone vertex clusters than forward pairing in 9,342 of 9,416 decidable batches.

`tools/vif_mesh.py`'s `skin` field pairs vertex i with qword 150+i (reversed relative to the
VU) and also reports vertex or offset qwords as skin when `3n+1 >= 150` or `n >= 49`
(467,492 such vertices). The decoder only reports words written by an UNPACK at 150 and pairs
them as the VU does; its raw bytes equal the Python lanes under that pairing on all 539,458
vertices.

## Test

```
EOD_DISC=<disc> python tools/tests/vif_mesh_ref/dump_ref.py ref.bin
                                # tools/vif_mesh.py decode_part + triangulate_batch + degenerate
                                # filter for every MODL/GRAF part; f32 values, raw skin lanes
c++ -std=c++17 -O2 -I runtime tools/tests/vif_mesh_test.cpp runtime/render/vif_mesh.cpp -o vif_mesh_test
vif_mesh_test "<disc>/DATA" ref.bin
```

The dumper walks every `.CAR/.RES/.TRK` (`modding_backend.directory`), decodes MODL members
with `vif_mesh.parse_modl` and GRAF models with `world_graf.parse_graf/parse_model`, and writes
per part `'PREF', key, status, texture, group, (mscal, nverts) per batch, per vertex
(p[3], uv[2], n[3] as f32, adc, skin lane mask, skin lanes[4]), triangles`. Python doubles
round to the same f32 the C++ computes (f32+f32 and int/127 are exact or correctly rounded in
double).

Result (2026-11-18, retail disc, LLVM-MinGW clang -O2, x86-64):

* 72 archives, 505 MODL/GRAF members, 34,564 models, 44,584 parts, 67,551 batches,
  1,969,433 vertices, 1,113,759 triangles; 0 decoder errors, 0 Python failures.
* Mismatching parts 0: positions, UVs, normals, colour bytes and ADC bitwise equal, triangle
  lists identical, texture/group/batch program and counts equal.
* Guest-reader path equals the buffer path (222 parts).
* Whole disc decodes in 157-160 ms single-threaded (12.3 M vertices/s); the Python reference
  takes 28.5 s.

Not verified: native game rendering from this decoder (no game run); vertex-colour alpha;
`Skin::Partial` weights; the `0x6A` per-vertex word; TJRM markers are not exposed.
