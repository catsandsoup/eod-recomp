# VIF geometry patching (`tools/vif_patch.py`)

Edit native MODL geometry in Blender through OBJ and write it back **in place**. Positions, UVs and normals are
re-encoded into the original UNPACK slots; the output has the same length and every untouched byte is identical.
Topology changes (vertex/face counts, connectivity, UV seams) are rejected.

```
python3 tools/vif_patch.py list --archive DATA/TRACKS/ANOKA.TRK
python3 tools/vif_patch.py export --archive DATA/CAR/BUS.CAR --member Low1.mod --obj bus.obj    # + bus.obj.map.json
python3 tools/vif_patch.py apply --archive DATA/CAR/BUS.CAR --member Low1.mod --obj bus.obj --map bus.obj.map.json --out Low1.mod.new
python3 tools/vif_patch.py apply-archive --archive DATA/CAR/BUS.CAR --member Low1.mod --obj bus.obj --map bus.obj.map.json --out BUS.CAR.new
```
`--member` may be a raw `.mod` path without `--archive`. `apply-archive` patches the same-size member in the archive and
rewrites its directory CRC (as `modding_backend.save_single_archive`); chain it for several members. `--clamp` saturates
instead of rejecting; `--channels pos,uv,nrm` limits what is written; `--report` saves the JSON report.

## Format (verified against `vif_mesh.py` on all 490 retail MODL members)
Per VU batch (MSCAL): `V1-32@0` count|0x8000; positions `V3-16`/`V3-8`/`V3-32` at address 2, normals `V4-8` at 1,
UVs `V2-16`/`V2-8` at 3, offsets `V4-32@198` (q198 = s,t; q199 = x,y,z). Offset-mode (STMOD 1) values decode as
`f32(STROW bits + u) + q199` (UV: `+ q198`); `V3-32`/`V2-32` lanes are raw floats (STROW 0). Normals decode as `(byte-128)/127`.
Vertex order is the native unpack order (including ADC restart vertices); vertex j of a batch is at `slot.off + j*slot.stride`
(`stride` 6/3/12 pos, 4 uv/nrm; the sidecar map lists these per part/batch plus format bits, STROW and q198/q199 offsets).
Faces are the strips as `vif_mesh` triangulates them. UV `t` is exported as `1-t`.

Unsupported: `V4-5` packed normals (2 retail models: envsky, hlparrow; their normals are left untouched), skin
bone/weight data and ADC flags (never changed), vertex colours (no colour channel exists in retail EOD VIF data),
TJRM attachment markers (not moved), fill-mode STCYCL.

## Precision, range and re-basing
Unchanged vertices keep their bytes (OBJ values within 2e-6 of the decoded value are treated as unedited). For edited
batches the encoder tries, in order: same STROW/offset; shift q199/q198 by whole ulps; raise the STROW exponent (coarser
ulp, e.g. 2^-15 -> 2^-14 m) when the batch no longer fits (a 16-bit lane covers 65535 ulp, 2 m at the retail base 256).
Re-based/refit batches rewrite all of their vertices (small requantisation error, reported as `max_err`). A shared STROW
is never re-based. Unrepresentable values are rejected unless `--clamp`. NaN/inf is always rejected.

## Bounds
MODL header radius (+0x18), centre (+0x1C) and bbox (+0x28, min/max per axis) match decoded vertex extents on all 490 models
(bbox within <0.01, radius never smaller than the farthest vertex). `apply` grows bbox sides and radius by exactly how far the
decoded geometry grew (never shrinks; retail slack preserved; no-op leaves them byte-identical). Not updated, because
unverified: part header words +0x1C..+0x37, TJRM markers, and any track sector/layout culling bounds outside the model.
Whether the game culls with the header bounds is unverified (no game run).

## Census
490 MODL members: 278 CAR, 158 RES, 54 TRK (HOMEBASE 29, GILROY 3, other 12 tracks 2 each: `*_stfn.mod` start/finish gantry and
`sfshad_*.mod`). Track terrain/ramps are not MODL members (not covered). Tests: `tools/tests/test_vif_patch.py`
(`EOD_TEST_RETAIL_MODELS=1` runs the all-490 byte-identical no-op round trip). Native staging: `work/claude-2026-11-10/vif-patch/`.
