#!/usr/bin/env python3
"""Import NASCAR: Dirt to Daytona (SLUS-20441) vehicles into Test Drive: Eve of Destruction.

Both games are Monster Games titles on one engine lineage. Verified shared formats:
  * MODL v9 meshes: identical 0x58 header, 50 part slots and VU memory layout, but the VIF
    packets target different VU1 microcode; see transcode_modl() (needed for anything to draw).
    The first header word varies per model (1, 7, 12, 29, 32): it is not a version.
  * TEX: v9 (NASCAR) differs from v10 (EOD) only by header size (0x60 vs 0x80).
    Format/flag words (0x30001/0x320500, 0x200/0x320600, ...) occur in both games.
  * Damage skeleton (bone.val): 42 sections in both; EOD values derive from NASCAR's.
  * physics.val: same sections/keys; EOD adds a few keys (grip, damage_scale,
    wheel_class, gear ratios, FrontDirt/RearDirt, speedometer).

Pipeline: build a registered custom chassis with create_custom_car (tables, dealer,
roster, CT/UICAR entries), then replace its meshes, textures, livery and physics
values with the NASCAR originals.

Bone.mvl and shadow geometry are generated from the NASCAR body. This remains a
prototype: donor transmission adapters, audio, effects, body LOD distances
and menu art still require replacement; a dedicated class is not implemented.
"""

import argparse
import json
import os
import re
import shutil
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# The fork's tools (create_custom_car, modl, rst0, valt) are looked up next to this file
# first, then in EOD_TOOLS (path to the recomp checkout's tools/ directory).
if os.environ.get("EOD_TOOLS"):
    sys.path.insert(1, os.environ["EOD_TOOLS"])

import math  # noqa: E402
import nascar_gen as gen  # noqa: E402

from create_custom_car import build_custom_car, car_archive_filename  # noqa: E402
from modl import parse_modl, retexture_modl  # noqa: E402
from rst0 import pack_archive, unpack_archive  # noqa: E402
from valt import compile_binary_valt, parse_binary_valt  # noqa: E402

# NASCAR archive -> (body template texture in its models, default paint archive entry)
NASCAR_VEHICLES = {
    "stock": ("STOCK.CAR", "Stock1.tex", "Car24.tex"),
}

# --- VIF transcoding -------------------------------------------------------------
# The MODL container and the per-batch VU1 memory layout are shared, but the two
# games' resident VU1 microcode differs. Verified on STOCK.CAR Low0-Low3 vs retail
# EOD cars (Algar, Prince): every NASCAR batch is
#   S-32 hdr@0, V3-16 pos@2, V4-5 normal@1, V2-16 uv@3, V4-8 bones@0x96, V4-32 x2@0xC6, MSCAL 0x2D
# while EOD batches are
#   S-32 hdr@0, V3-16 pos@2, STROW(65536.0 x3, 0x7F80) + V4-8 normal@1, V2-16 uv@3 (row w 0x8000),
#   V4-8 bones@0x96, V4-32 x2@0xC6, MSCAL 0x5C.
# EOD has no VU1 entry at 0x2D*8, so untranscoded NASCAR parts draw nothing in game.
# Both normal encodings are offset binary: NASCAR (c-16)/16 from 5 bits, EOD (b-128)/128
# from 8 bits, so b = c << 3 (exactly what the VIF V4-5 unpacker produces). Bit 15
# (strip-restart/ADC flag) maps to b_w = 0x80, matching EOD's 0x7F80 + {0, 0x80} row trick.
NASCAR_MSCAL, EOD_MSCAL = 0x2D, 0x5C
EOD_NORMAL_ROW = struct.pack("<4I", 0x47800000, 0x47800000, 0x47800000, 0x7F80)
EOD_UV_ROW_W = 0x8000


def _unpack_words(cmd: int, num: int) -> int:
    vn, vl = (cmd >> 2) & 3, cmd & 3
    n = num or 256
    if vl == 3:  # V4-5: 16 bits per vertex
        return (n * 16 + 31) // 32
    return (n * (vn + 1) * (32 >> vl) + 31) // 32


def transcode_vif_stream(stream: bytes) -> bytes:
    """Rewrite one 4SPS VIF stream from NASCAR's VU1 conventions to EOD's."""
    out = bytearray()
    p, mask, uv_row_pending, batches = 0, None, False, 0
    while p + 4 <= len(stream):
        code = struct.unpack_from("<I", stream, p)[0]
        cmd, num, imm = (code >> 24) & 0x7F, (code >> 16) & 0xFF, code & 0xFFFF
        if cmd >= 0x60:  # UNPACK
            words = _unpack_words(cmd, num)
            body = stream[p + 4:p + 4 + words * 4]
            if len(body) != words * 4:
                raise ValueError("Truncated UNPACK in VIF stream")
            fmt = cmd & 0x6F
            if fmt == 0x6F:  # V4-5 normals -> EOD V4-8 normals with its offset row
                if (imm & 0x3FF) != 1:
                    raise ValueError(f"V4-5 unpack at unexpected VU address {imm & 0x3FF:#x}")
                n = num or 256
                conv = bytearray()
                for (v,) in struct.iter_unpack("<H", body[:n * 2]):
                    conv += bytes(((v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3, (v >> 15) << 7))
                out += struct.pack("<I", 0x30000000) + EOD_NORMAL_ROW
                out += struct.pack("<I", (code & 0x80000000) | ((0x6E | (cmd & 0x10)) << 24) | (num << 16) | imm)
                out += conv
            elif fmt == 0x62:
                # NASCAR low-LOD single-bone skin (V1-8 @150, STMOD normal). Rewrite as EOD's
                # weighted V4-8 skin (offset row 32768.0 on z/w like every EOD part); the
                # bytes are regenerated by the Bone.mvl skinning pass anyway.
                if (imm & 0x3FF) != 150:
                    raise ValueError("S-8 unpack at unexpected VU address")
                n = num or 256
                out += struct.pack("<I", 0x30000000) + struct.pack("<4I", 0, 0, 0x47000000, 0x47000000)
                out += struct.pack("<I", 0x05000001)
                out += struct.pack("<I", (code & 0x80000000) | (0x6E << 24) | (num << 16) | imm)
                out += b"".join(bytes((b, 0, 255, 0)) for b in body[:n])
                out += struct.pack("<I", 0x05000000)
            else:
                out += stream[p:p + 4] + body
            p += 4 + words * 4
            continue
        if cmd == 0x20:  # STMASK
            mask = struct.unpack_from("<I", stream, p + 4)[0]
            uv_row_pending = mask == 0x50505050
            out += stream[p:p + 8]
            p += 8
            continue
        if cmd in (0x30, 0x31):  # STROW / STCOL
            row = bytearray(stream[p + 4:p + 20])
            if cmd == 0x30 and uv_row_pending:
                struct.pack_into("<I", row, 12, EOD_UV_ROW_W)
                uv_row_pending = False
            out += stream[p:p + 4] + row
            p += 20
            continue
        if cmd in (0x14, 0x15):  # MSCAL / MSCALF
            if imm not in gen.MSCAL_MAP:
                raise ValueError(f"Unsupported NASCAR VU1 entry {imm:#x}")
            out += struct.pack("<I", (code & 0xFFFF0000) | gen.MSCAL_MAP[imm])
            batches += 1
            p += 4
            continue
        if cmd in (0x50, 0x51, 0x4A):
            raise ValueError(f"Unexpected VIF command {cmd:#x} in mesh stream")
        out += stream[p:p + 4]  # NOP, STCYCL, STMOD, flushes ...
        p += 4
    if p != len(stream):
        raise ValueError("VIF stream did not end on a command boundary")
    while len(out) % 16:
        out += b"\0\0\0\0"  # VIF NOP padding to whole quadwords
    if not batches:
        raise ValueError("VIF stream has no MSCAL")
    return bytes(out)


def transcode_modl(data: bytes) -> bytes:
    """Transcode every 4SPS part of a NASCAR MODL v9 for EOD's VU1 microcode.

    Layout (verified on both games): part = 0x38-byte header + VIF stream of qwc
    quadwords (size field = 0x38 + 16*qwc, qwc at +0x34), followed by 8 bytes
    before the next part; the TJRM helper block is last; u32 @0x40 = file size.
    """
    nslots = struct.unpack_from("<I", data, 0x10)[0]
    offsets = list(struct.unpack_from(f"<{nslots}I", data, 0x58))
    blocks = sorted({o for o in offsets if o != 0xE3E3E3E3 and data[o:o + 4] in (b"4SPS", b"TJRM")})
    if not blocks or data[blocks[-1]:blocks[-1] + 4] != b"TJRM":
        raise ValueError("Expected 4SPS parts followed by a final TJRM block")
    declared = struct.unpack_from("<I", data, 0x40)[0]
    out = bytearray(data[:blocks[0]])
    remap = {}
    for i, o in enumerate(blocks):
        remap[o] = len(out)
        if data[o:o + 4] == b"TJRM":
            tail = data[o:]
            size = struct.unpack_from("<I", data, o + 4)[0]
            out += tail + b"\0" * max(0, size - len(tail))  # NASCAR files stop 4 bytes short
            continue
        size = struct.unpack_from("<I", data, o + 4)[0]
        qwc = struct.unpack_from("<I", data, o + 0x34)[0]
        if size != 0x38 + 16 * qwc:
            raise ValueError(f"4SPS at {o:#x}: size {size:#x} != 0x38 + 16*{qwc}")
        stream = transcode_vif_stream(data[o + 0x38:o + size])
        header = bytearray(data[o:o + 0x38])
        if header[0x1C] == 4:   # single-bone low-LOD part, now carries a weighted V4-8 skin
            header[0x1C] = 3
        struct.pack_into("<I", header, 4, 0x38 + len(stream))
        struct.pack_into("<I", header, 0x34, len(stream) // 16)
        out += header + stream + data[o + size:blocks[i + 1]]  # keep the 8 inter-part bytes
    if declared != blocks[-1] + struct.unpack_from("<I", data, blocks[-1] + 4)[0]:
        raise ValueError("MODL declared size does not match its layout")
    struct.pack_into(f"<{nslots}I", out, 0x58, *[remap.get(o, o) for o in offsets])
    struct.pack_into("<I", out, 0x40, len(out))
    return bytes(out)


# --- A2: group remap, damage skin, Bone.mvl, helpers, shadows ---------------------------
# NASCAR Cup Low1-3 group ids are not TDEOD part groups.  Verified by bounding boxes and
# renders (A2_REPORT.md): 5/7/9 are three manufacturer variants of the greenhouse+roof,
# 6/8/10 three variants of the rear clip (decklid, spoiler, rear fascia), 11 the hood,
# 12 the front clip, 0 the main body plus interior (cage, air hose, driver, net).
# TDEOD gives fixed meaning to some groups (EE table at 0x0018DBF8: 2 hood, 5/6 doors,
# 11 decklid, 12/13 bumpers are detachable panels hung from TJRM helpers).
STOCK_DROP_GROUPS = {7, 8, 9, 10}           # keep variant 5/6, drop the other two
STOCK_GROUP_MAP = {5: 9, 6: 10, 11: 2, 12: 1}
STOCK_INTERIOR_TEXTURES = {"rollwht.tex", "airhose.tex", "template01c.tex", "padding.tex", "swindnet.tex"}
SURF_GROUPS = ({0, 1, 2, 10}, {9})          # bonesurf_1 (front + rear clips), bonesurf_2 (body)


def remap_stock_groups(m):
    keep = []
    for part in m["parts"]:
        raw = gen.part_group(part)
        g = raw & 0xFFFF
        if g in STOCK_DROP_GROUPS:
            continue
        if g == 0:
            g = 0 if part_texture_l(part) in STOCK_INTERIOR_TEXTURES else 9
        else:
            g = STOCK_GROUP_MAP.get(g, g)
        struct.pack_into("<I", part["hdr"], 0x18, (raw & 0xFFFF0000) | g)
        keep.append(part)
    m["parts"] = keep


def part_texture_l(part):
    return gen.part_texture(part).lower()


def group_points(m):
    pts = {}
    for part in m["parts"]:
        pts.setdefault(gen.part_group(part) & 0xFFFF, []).extend(gen.part_points(part))
    return pts


def make_bones(gpts):
    """Bone list (1-based, dummies padded to bone_80), bonesurfs and per-surf local bone tables."""
    surf_pts = [[p for g in SURF_GROUPS[0] if g != 0 for p in gpts.get(g, [])],
                [p for g in SURF_GROUPS[1] for p in gpts.get(g, [])]]
    b1, cell1 = gen.place_bones(surf_pts[0], 79)
    b2, cell2 = gen.place_bones(surf_pts[1], 79)
    bones = [gen.bone_params(p, 0.0) for p in b1]
    bones += [dict(gen.DUMMY_BONE) for _ in range(80 - len(b1))]
    bones += [gen.bone_params(p, 0.0) for p in b2]
    present = set(gpts)
    mask = [sum(1 << g for g in sg if g in present or g == 0) for sg in SURF_GROUPS]
    surfs = [(0, len(b1), mask[0]), (80, 80 + len(b2), mask[1])]
    local = [[(i + 1, p) for i, p in enumerate(b1)], [(i + 1, p) for i, p in enumerate(b2)]]
    print(f"Bone.mvl: {len(b1)} + {len(b2)} bones (cell {cell1:.3f} / {cell2:.3f} m), masks "
          f"{mask[0]:X} / {mask[1]:X}")
    return bones, surfs, local


def skin_modl(m, local, rigid_all=False):
    for part in m["parts"]:
        g = gen.part_group(part) & 0xFFFF
        if rigid_all or g == 0:
            gen.write_part_skin(part, lambda p: (0, 0, 255, 0))
            continue
        si = 0 if g in SURF_GROUPS[0] else 1
        bl = local[si]
        gen.write_part_skin(part, lambda p, bl=bl: gen.compute_skin(p, bl))


def hood_helpers(gpts):
    """TJRM helpers for the hood (group 2), placed like Algar's: hinges on the rear corners,
    latch at the front centre; plus a trailer hitch at the rear centre."""
    hood = gpts.get(2)
    out = []
    if hood:
        zmin = min(p[2] for p in hood)
        zmax = max(p[2] for p in hood)
        xmax = max(abs(p[0]) for p in hood)
        rear = [p for p in hood if p[2] < zmin + 0.06]
        front = [p for p in hood if p[2] > zmax - 0.06]
        yr = max(p[1] for p in rear)
        yf = min(p[1] for p in front)
        out += [("left hood hinge", gen.IDENTITY, (-xmax, yr, zmin)),
                ("right hood hinge", gen.IDENTITY, (xmax, yr, zmin)),
                ("hood latch", gen.IDENTITY, (0.0, yf, zmax))]
    body = [p for g in (1, 9, 10) for p in gpts.get(g, [])]
    if body:
        zmin = min(p[2] for p in body)
        low = [p for p in body if p[2] < zmin + 0.15]
        out.append(("hitch", gen.IDENTITY, (0.0, min(p[1] for p in low) + 0.12, zmin + 0.05)))
    return out


def add_helpers(m, helpers):
    if not m["tjrm"]:
        return
    recs = [r for r in gen.tjrm_records(m["tjrm"]) if r[0] not in {h[0] for h in helpers}]
    m["tjrm"] = gen.build_tjrm(recs + helpers)


def roof_liner_height(roof, z):
    near = [q[1] for q in roof if abs(q[2] - z) < 0.35 and abs(q[0]) < 0.6] or [q[1] for q in roof]
    # Windows and pillars share group 9 with the roof. Its lower envelope can
    # sit at driver-eye height, exposing exterior paint above the liner.
    return max(near) - 0.02


def fix_cockpit_roof(low0, gpts):
    """TDEOD draws the exterior LOD around the cockpit (Algar's Low1 carries its own
    interior trim); NASCAR's cockpit roof liner (Cockpit.tex quad at y=1.35) sits above
    the exterior roof (y<=1.23), so from inside one sees the underside of the livery roof.
    Lower the liner just under the local exterior ceiling, retaining its source
    footprint so the driver does not see exterior paint around the edges."""
    roof = [p for p in gpts.get(9, []) if p[1] > 1.0]
    if not roof:
        return 0
    moved = 0
    for part in low0["parts"]:
        if part_texture_l(part) != "cockpit.tex":
            continue
        stream = bytearray(part["stream"])
        for b in gen.walk_vif(stream):
            pts = gen.batch_positions(stream, b)
            for i, p in enumerate(pts):
                if p[1] < 1.30:
                    continue
                y = roof_liner_height(roof, p[2])
                x = p[0]  # retain NASCAR lining width; narrowing exposes exterior roof
                gen.set_batch_position(stream, b, i, (x, y, p[2]))
                moved += 1
        part["stream"] = bytes(stream)
    return moved


def make_shadows(gpts, donor_shadows):
    """shadow1-3.mod in the retail format (type-7 closed volumes, MSCAL 4)."""
    ext = [p for g in (1, 2, 9, 10) for p in gpts.get(g, [])]
    out = {}
    floor = min(p[1] for p in ext) + 0.12      # retail volumes stop ~0.1 m above the sill
    ext = [(p[0], max(p[1], floor), p[2]) for p in ext]
    lo = [min(p[i] for p in ext) for i in range(3)]
    hi = [max(p[i] for p in ext) for i in range(3)]
    for name, cell in (("shadow1.mod", 0.6), ("shadow2.mod", 1.2)):
        reps = gen._mirror(gen._cluster(ext, cell))
        tris = gen.hull_triangles(reps)
        out[name] = tris
    y = 0.35
    w, z0, z1 = hi[0] + 0.07, lo[2] - 0.02, hi[2] + 0.02
    a, b, c, d = (-w, y, z1), (w, y, z1), (w, y, z0), (-w, y, z0)
    out["shadow3.mod"] = [((a, b, c), (0, 1, 0)), ((a, c, d), (0, 1, 0)),
                          ((a, c, b), (0, -1, 0)), ((a, d, c), (0, -1, 0))]
    files = {}
    for name, tris in out.items():
        donor = gen.read_modl(donor_shadows[name])
        dp = donor["parts"][0]
        first_z = gen.walk_vif(dp["stream"])[0]["q198"][2]
        f30 = struct.unpack_from("<f", dp["hdr"], 0x30)[0]
        part = gen.encode_shadow_part(tris, first_z, f30)
        m = {"head": donor["head"], "parts": [part], "tjrm": None, "nrec": donor["nrec"]}
        gen.set_modl_bounds(m, [p for t in tris for p in t[0]])
        files[name] = gen.write_modl(m)
        print(f"{name}: {len(tris)} triangles")
    return files


def convert_tex_v9_to_v10(data: bytes, aux: int = 0) -> bytes:
    """NASCAR TEX v9 (0x60 header) -> EOD TEX v10 (0x80 header). Pixel data is unchanged."""
    w, h, mips, fmt, flags, _aux, pal = struct.unpack("<7I", data[:28])
    if pal != 0x60:
        raise ValueError(f"Unexpected v9 palette offset {pal:#x}")
    if not (1 <= mips <= 9) or w > 2048 or h > 2048:
        raise ValueError("Implausible TEX v9 header")
    offs = list(struct.unpack(f"<{mips}I", data[28:28 + 4 * mips]))
    header = bytearray(0x80)
    struct.pack_into("<7I", header, 0, w, h, mips, fmt, flags, aux, 0x80)
    struct.pack_into(f"<{mips}I", header, 28, *[o + 0x20 for o in offs])
    # zero sentinel after last mip offset is already present (header is zero-filled)
    return bytes(header) + data[0x60:]


def drop_top_mip(v10: bytes) -> bytes:
    """Make a half-resolution TEX from a v10 TEX by discarding its first mip level."""
    w, h, mips, fmt, flags, aux, pal = struct.unpack("<7I", v10[:28])
    if mips < 2:
        raise ValueError("Cannot drop the only mip")
    offs = list(struct.unpack(f"<{mips}I", v10[28:28 + 4 * mips]))
    palette = v10[pal:pal + 0x400]
    payload = v10[offs[1]:]
    shift = offs[1] - (0x80 + 0x400)
    header = bytearray(0x80)
    struct.pack_into("<7I", header, 0, w // 2, h // 2, mips - 1, fmt, flags, aux, 0x80)
    struct.pack_into(f"<{mips - 1}I", header, 28, *[o - shift for o in offs[1:]])
    return bytes(header) + palette + payload


def find_entry(directory: Path, name: str) -> Path:
    hits = [p for p in directory.iterdir() if p.name.lower() == name.lower()]
    if len(hits) != 1:
        raise FileNotFoundError(f"{name} not found (or ambiguous) in {directory}")
    return hits[0]


def merge_physics(eod_model: dict, nascar_model: dict) -> dict:
    """Preserve every NASCAR value and translate the prototype's EOD-only fields.

    Gear, speedometer and engine-audio adapters still require the
    prototype template. This function does not establish a donor-free import.
    """
    target = {s["name"]: s for s in eod_model["sections"]}
    source = {s["name"]: dict(s["pairs"]) for s in nascar_model["sections"]}
    # Reuse EOD table serialization metadata, while retaining source-only sections
    # (e.g. the restrictor engine) and source-only gauge/setup keys.
    metadata = {k: v for k, v in eod_model["sections"][0].items()
                if k not in ("name", "parent", "pairs")}
    for name, values in source.items():
        if name not in target:
            section = dict(metadata, name=name, parent="", pairs=[])
            target[name] = section
            eod_model["sections"].append(section)
        pairs = dict(target[name]["pairs"])
        pairs.update(values)
        target[name]["pairs"] = list(pairs.items())
    chassis = target["Chassis"]
    chassis["pairs"] = list(dict(chassis["pairs"], grip="1.0", damage_scale="1.0").items())
    for axle in ("Front", "Rear"):
        if axle not in source:
            continue
        # Asphalt Cup tyres stay asphalt tyres on dirt until the Career dirt-tyre
        # upgrade exists; never silently give the car the donor's dirt settings.
        name = axle + "Dirt"
        if name not in target:
            section = dict(metadata, name=name, parent="", pairs=[])
            target[name] = section
            eod_model["sections"].append(section)
        target[name]["pairs"] = list(source[axle].items())
        normal = target[axle]
        normal["pairs"] = list(dict(normal["pairs"], wheel_class="ncu").items())
    return eod_model


def merge_bones(eod_model: dict, nascar_model: dict) -> dict:
    src = {s["name"]: dict(s["pairs"]) for s in nascar_model["sections"]}
    for sec in eod_model["sections"]:
        values = src.get(sec["name"])
        if values:
            sec["pairs"] = [(k, values.get(k, v)) for k, v in sec["pairs"]]
    return eod_model


def set_entry(manifest: dict, name: str, size: int, version: int, fourcc: str, extractable: bool):
    manifest["entries"] = [e for e in manifest["entries"] if e["name"].lower() != name.lower()]
    manifest["entries"].append(dict(name=name, fourcc=fourcc, version=version, size=size,
                                    is_extractable=extractable))


def import_vehicle(kind, nascar_data, eod_data, output, name, number, base, car_class, paint):
    car_file, body_template, default_paint = NASCAR_VEHICLES[kind]
    paint = paint or default_paint
    nascar_data, eod_data, output = Path(nascar_data), Path(eod_data), Path(output)
    with tempfile.TemporaryDirectory(prefix="eod-nascar-") as tmp:
        tmp = Path(tmp)
        pkg = tmp / "pkg"
        build_custom_car(str(eod_data), str(pkg), name=name, base=base, number=str(number),
                         classes=[car_class],
                         description="NASCAR Winston Cup stock car imported from Dirt to Daytona")

        src = tmp / "nascar_car"
        unpack_archive(str(nascar_data / car_file), str(src))
        paints = tmp / "paint"
        unpack_archive(str(nascar_data / "RES" / "PAINT256.RES"), str(paints))

        car_dir = tmp / "car"
        unpack_archive(str(pkg / "DATA" / "CAR" / car_archive_filename(name)), str(car_dir))
        manifest = json.loads((car_dir / "manifest.json").read_text())

        # 1. Meshes: drop donor body LODs, bring NASCAR Low0..Low4 (EOD lod.val uses five).
        for p in list(car_dir.iterdir()):
            if re.fullmatch(r"low\d+\.mod", p.name, re.I):
                p.unlink()
                manifest["entries"] = [e for e in manifest["entries"] if e["name"] != p.name]
        body_tex = f"{name}.tex"
        used_textures = set()
        # NASCAR Low0..Low4 are all its own meshes now: Low4's single-bone/no-bone VU1
        # programs (MSCAL 0x2D with S-8 bones, MSCAL 0x26) are transcoded too.
        lods = {}
        for i in range(5):
            data = transcode_modl(find_entry(src, f"Low{i}.mod").read_bytes())
            data = retexture_modl(data, body_template, body_tex)
            # NASCAR's driver figure (suit texture template01c.tex is generated at run time
            # in NASCAR). EOD draws no separate driver for this car (A1 run: hiding this
            # part leaves the seat empty), so keep it and give it an existing texture.
            data = retexture_modl(data, "template01c.tex", "Padding.tex")
            lods[i] = gen.read_modl(data)
        for i in range(1, 5):
            remap_stock_groups(lods[i])
        gpts = group_points(lods[1])
        bones, surfs, local = make_bones(gpts)
        for i in range(1, 5):
            skin_modl(lods[i], local)
        skin_modl(lods[0], local, rigid_all=True)    # retail cockpits (Low0) are rigid
        helpers = hood_helpers(gpts)
        for i in range(1, 5):
            add_helpers(lods[i], helpers)
        print(f"Helpers added: {[h[0] for h in helpers]}")
        print(f"Cockpit roof liner vertices lowered: {fix_cockpit_roof(lods[0], gpts)}")
        for i in range(5):
            data = gen.write_modl(lods[i])
            for part in parse_modl(data)["parts"]:
                if part["valid"] and part["magic"] == "4SPS" and part["texture"].lower().endswith(".tex"):
                    used_textures.add(part["texture"].lower())
            out = car_dir / f"Low{i}.mod"
            out.write_bytes(data)
            set_entry(manifest, out.name, len(data), 9, "MODL", False)

        # Damage deformation map generated from the NASCAR mesh (no donor data left).
        template = None
        for p in list(car_dir.iterdir()):
            if p.name.lower() == "bone.mvl":
                template = parse_binary_valt(p.read_bytes())
                p.unlink()
                manifest["entries"] = [e for e in manifest["entries"] if e["name"] != p.name]
        bounds = {g: ([min(q[k] for q in v) for k in range(3)], [max(q[k] for q in v) for k in range(3)])
                  for g, v in gpts.items()}
        mvl = compile_binary_valt(gen.build_mvl_model(template, bounds, surfs, bones))
        (car_dir / "Bone.mvl").write_bytes(mvl)
        set_entry(manifest, "Bone.mvl", len(mvl), 11, "VALT", False)

        # Shadow volumes generated from the NASCAR body.
        donor_sh = {}
        for p in list(car_dir.iterdir()):
            if re.fullmatch(r"shadow\d\.mod", p.name, re.I):
                donor_sh[p.name.lower()] = p.read_bytes()
                p.unlink()
                manifest["entries"] = [e for e in manifest["entries"] if e["name"] != p.name]
        for fname, blob in make_shadows(gpts, donor_sh).items():
            (car_dir / fname).write_bytes(blob)
            set_entry(manifest, fname, len(blob), 9, "MODL", False)

        # 2. Detail textures (cockpit, dash, windows, roll cage...), converted to TEX v10.
        for p in src.iterdir():
            if p.suffix.lower() == ".tex" and p.name.lower() in used_textures:
                conv = convert_tex_v9_to_v10(p.read_bytes())
                (car_dir / p.name).write_bytes(conv)
                set_entry(manifest, p.name, len(conv), 10, "TEX", False)
        missing = used_textures - {p.name.lower() for p in car_dir.iterdir()}
        if missing:
            raise ValueError(f"Mesh references textures with no source: {sorted(missing)}")

        # 3. Livery: real NASCAR paint scheme. tm0 = 512x256, tex = 256x128 (next mip down).
        livery_tm0 = convert_tex_v9_to_v10(find_entry(paints, paint).read_bytes(), aux=0x100)
        livery_tex = drop_top_mip(livery_tm0)
        for fname, blob, extract in ((f"{name}.tex", livery_tex, False), (f"{name}.tm0", livery_tm0, True)):
            (car_dir / fname).write_bytes(blob)
            set_entry(manifest, fname, len(blob), 10, "TEX", extract)

        # 4. lod.val: point the paintable base texture at the imported body.
        lod_path = find_entry(car_dir, "lod.val")
        lod = parse_binary_valt(lod_path.read_bytes())
        for sec in lod["sections"]:
            if sec["name"] == "low":
                sec["pairs"] = [(k, body_tex.lower() if k == "base_tex" else v) for k, v in sec["pairs"]]
        lod_path.write_bytes(compile_binary_valt(lod))

        # 5. Physics and damage skeleton values from NASCAR.
        phys_path = find_entry(car_dir, "physics.val")
        phys = merge_physics(parse_binary_valt(phys_path.read_bytes()),
                             parse_binary_valt(find_entry(src, "physics.val").read_bytes()))
        phys_path.write_bytes(compile_binary_valt(phys))
        bone_path = find_entry(car_dir, "Bone.val")
        bones = merge_bones(parse_binary_valt(bone_path.read_bytes()),
                            parse_binary_valt(find_entry(src, "bone.val").read_bytes()))
        bone_path.write_bytes(compile_binary_valt(bones))

        for e in manifest["entries"]:
            e.pop("orig_offset", None)
            e.pop("offset", None)
            e["size"] = (car_dir / e["name"]).stat().st_size
        manifest.pop("original_layout", None)
        (car_dir / "manifest.json").write_text(json.dumps(manifest, indent=2))
        pack_archive(str(car_dir), str(pkg / "DATA" / "CAR" / car_archive_filename(name)))

        # 6. CT.RES: every paint number the carousel/AI can show on this chassis needs a
        # livery laid out for the NASCAR UVs. create_custom_car reassigns two retail AI
        # drivers (keeping their numbers) and adds a dealer number; their retail
        # liveries are mapped for the donor body and render garbled on the Cup mesh.
        common = tmp / "common"
        unpack_archive(str(pkg / "DATA" / "RES" / "COMMON.RES"), str(common))
        numbers = set()
        for table in ("dirt.val", "buycar.val"):
            for sec in parse_binary_valt(find_entry(common, table).read_bytes())["sections"]:
                pairs = dict(sec["pairs"])
                if str(pairs.get("chassis", "")).lower() == name.lower() and "number" in pairs:
                    numbers.add(str(pairs["number"]))
        numbers.add(str(number))
        spare = [p.name for p in sorted(paints.iterdir(), key=lambda p: p.name.lower())
                 if re.fullmatch(r"car\d+\.tex", p.name, re.I) and p.name.lower() != paint.lower()]
        ct = tmp / "ct"
        unpack_archive(str(pkg / "DATA" / "RES" / "CT.RES"), str(ct))
        ctm = json.loads((ct / "manifest.json").read_text())
        for num in sorted(numbers, key=int):
            if num == str(number):
                tm0, tex = livery_tm0, livery_tex
            else:
                src_paint = spare.pop(0)
                tm0 = convert_tex_v9_to_v10(find_entry(paints, src_paint).read_bytes(), aux=0x100)
                tex = drop_top_mip(tm0)
            print(f"CT.RES livery {num} <- {paint if num == str(number) else src_paint}")
            for fname, blob, extract in ((f"{num}.tex", tex, False), (f"{num}.tm0", tm0, True)):
                (ct / fname).write_bytes(blob)
                set_entry(ctm, fname, len(blob), 10, "TEX", extract)
        for e in ctm["entries"]:
            e.pop("orig_offset", None)
            e.pop("offset", None)
        ctm.pop("original_layout", None)
        (ct / "manifest.json").write_text(json.dumps(ctm, indent=2))
        pack_archive(str(ct), str(pkg / "DATA" / "RES" / "CT.RES"))

        # 7. Namespaced source Cup wheels coexist with every native RACE resource.
        from nascar_wheels import build_cup_wheels
        race = tmp / "race"
        unpack_archive(str(eod_data / "RES" / "RACE.RES"), str(race))
        source_race = tmp / "source_race"
        unpack_archive(str(nascar_data / "RES" / "RACE.RES"), str(source_race))
        wheel_resources, wheel_provenance = build_cup_wheels(
            source_race, parse_binary_valt(find_entry(src, "physics.val").read_bytes()))
        race_manifest = json.loads((race / "manifest.json").read_text())
        for fname, (blob, version, fourcc) in wheel_resources.items():
            (race / fname).write_bytes(blob)
            set_entry(race_manifest, fname, len(blob), version, fourcc, False)
        for entry in race_manifest["entries"]:
            entry.pop("orig_offset", None)
            entry.pop("offset", None)
        race_manifest.pop("original_layout", None)
        (race / "manifest.json").write_text(json.dumps(race_manifest, indent=2))
        pack_archive(str(race), str(pkg / "DATA" / "RES" / "RACE.RES"))
        (pkg / "wheel-provenance.json").write_text(json.dumps(wheel_provenance, indent=2))

        if output.exists() and any(output.iterdir()):
            raise FileExistsError(f"{output} is not empty")
        shutil.copytree(pkg, output, dirs_exist_ok=True)
    print(f"NASCAR {kind} -> {name} (#{number}) written to {output}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nascar-data", required=True, help="NASCAR disc DATA directory")
    ap.add_argument("--eod-data", default=str(Path.home() / "Library/Application Support/TDEODRecomp/disc/DATA"))
    ap.add_argument("--output", required=True)
    ap.add_argument("--kind", choices=sorted(NASCAR_VEHICLES), default="stock")
    ap.add_argument("--name", default="Cupcar")
    ap.add_argument("--number", type=int, default=777)
    ap.add_argument("--base", default="Algar")
    ap.add_argument("--class", dest="car_class", choices=("compact", "midsize", "muscle"), default="midsize")
    ap.add_argument("--paint", help="PAINT256.RES entry, e.g. Car24.tex")
    a = ap.parse_args()
    import_vehicle(a.kind, a.nascar_data, a.eod_data, a.output, a.name, a.number, a.base,
                   ("compact", "midsize", "muscle").index(a.car_class), a.paint)


if __name__ == "__main__":
    main()
