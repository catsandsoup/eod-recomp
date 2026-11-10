#!/usr/bin/env python3
"""Export a retail track world (GRAF display list) to OBJ+MTL+PNG and placements.json,
render a top-down preview, and apply verified placement edits back to a .TRK.

    world_export.py export --disc DISC --track DATA/TRACKS/ANOKA.TRK --dest DIR
                           [--lod N] [--props-only|--terrain-only] [--blender]
                           [--include-sky] [--include-unspawned] [--preview PNG]
    world_export.py place  --disc DISC --track DATA/TRACKS/ANOKA.TRK
                           --edits edits.json --out NEW.TRK

Coordinates are native: metres, +Y up. Static world models are stored
pre-transformed in world space (identity instance transform); spawned Dyno
records carry a 4x3 row-vector matrix. Format evidence: docs/WORLD_FORMAT.md.
Read-only on the disc; run with `python3 -I`.
"""
import argparse
import hashlib
import json
import math
import os
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import world_graf as wg  # noqa: E402
from modding_backend import directory, payload, cached_directory  # noqa: E402
from rst0 import eod_crc  # noqa: E402
from texture_preview import decode_preview  # noqa: E402
from vif_mesh import write_png  # noqa: E402

TOOL_VERSION = "1"
SHARED_RES = ("HOBBYSET.RES", "RACE.RES", "PART.RES", "COMMON.RES", "GENESIS.RES")
BILLBOARDS = {"Point-Facing", "Axis-Facing", "Multi-Facing", "Facing Test"}
IDENTITY = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]


def sha(data):
    return hashlib.sha256(data).hexdigest()


# --------------------------------------------------------------------------- loading

def load_track(disc, track):
    disc = Path(disc).expanduser().resolve()
    rel = Path(track)
    path = (disc / rel).resolve()
    if rel.is_absolute() or ".." in rel.parts or not str(path).startswith(str(disc / "DATA")):
        raise ValueError("Track must be a disc-relative path inside DATA/")
    raw = path.read_bytes()
    entries = directory(path)
    graf = [e for e in entries if e["type"] == "GRAF"]
    if len(graf) != 1:
        raise ValueError("Expected exactly one GRAF member in %s" % track)
    gentry = graf[0]
    dir_off = struct.unpack_from("<I", raw, 20)[0]
    index = entries.index(gentry)
    version = struct.unpack_from("<I", raw, dir_off + index * 68 + 36)[0]
    if version != wg.GRAF_VERSION:
        raise ValueError("bad graf version %d (native loader requires 18)" % version)
    data, _ = payload(path, gentry["name"], entries)
    return dict(disc=disc, rel=rel.as_posix(), path=path, raw=raw, entries=entries,
                graf_entry=gentry, graf_index=index, graf=data, dir_offset=dir_off)


def resolve_texture(track, name, cache):
    """Same lookup order as modding_backend.resolve_material for a track:
    own archive, then HOBBYSET/RACE/PART/COMMON/GENESIS .RES; `name` before
    `<stem>.tm0` within each archive. Returns (assetId, bytes) or (None, None)."""
    key = name.lower()
    if key in cache:
        return cache[key]
    result = (None, None)
    if name and name.lower() not in ("-", ".tex"):
        candidates = [(track["path"], track["rel"])] + [
            (track["disc"] / "DATA/RES" / f, "DATA/RES/" + f) for f in SHARED_RES]
        stem = os.path.splitext(name)[0]
        for path, rel in candidates:
            if not path.is_file():
                continue
            st = path.stat()
            entries = cached_directory(str(path), st.st_mtime_ns, st.st_size)
            match = next((e for cand in (name, stem + ".tm0") for e in entries
                          if e["name"].lower() == cand.lower()), None)
            if match:
                data, _ = payload(path, match["name"], entries)
                result = ("%s#%s" % (rel, match["name"]), data)
                break
    cache[key] = result
    return result


# --------------------------------------------------------------------------- scene

def classify(leaf, parts):
    if leaf.get("kind") is None:
        return "empty"
    if leaf["kind"] == 0:
        texs = " ".join(p["texture"].lower() for p in parts)
        return "static-ramp" if "ramp" in texs else "static"
    name = leaf.get("subtype_name", "?")
    if name == "Sky":
        return "sky"
    if name in BILLBOARDS:
        return "billboard"
    if name == "Optional Terrain":
        return "optional-terrain"
    if name == "Dyno":
        if leaf["dyno"]["mode"] == -1:
            return "dyno-unspawned"
        return "dyno"
    return "display-object"


def native_rows_to_matrix(m12):
    """Native 4x3 (rows r0,r1,r2 basis, row 3 position; world = x*r0+y*r1+z*r2+p)
    -> 4x4 row-major matrix M for column vectors (p_world = M p_local)."""
    r0, r1, r2, p = m12[0:3], m12[3:6], m12[6:9], m12[9:12]
    return [r0[0], r1[0], r2[0], p[0],
            r0[1], r1[1], r2[1], p[1],
            r0[2], r1[2], r2[2], p[2],
            0.0, 0.0, 0.0, 1.0]


def matrix_to_native_rows(M):
    return [M[0], M[4], M[8], M[1], M[5], M[9], M[2], M[6], M[10], M[3], M[7], M[11]]


def apply(M, p):
    return (M[0] * p[0] + M[1] * p[1] + M[2] * p[2] + M[3],
            M[4] * p[0] + M[5] * p[1] + M[6] * p[2] + M[7],
            M[8] * p[0] + M[9] * p[1] + M[10] * p[2] + M[11])


def leaf_selected(leaf, lod):
    for step in leaf["path"]:
        if step["kind"] == "lod" and step["count"] > 1:
            if step["index"] != min(lod, step["count"] - 1):
                return False
    return True


def build_scene(track, lod=0, mode="all", include_sky=False, include_unspawned=False):
    data = track["graf"]
    graf = wg.parse_graf(data)
    base = track["graf_entry"]["offset"]
    member = track["graf_entry"]["name"]
    instances = []
    for i, leaf in enumerate(graf["leaves"]):
        model = parts = None
        if leaf.get("model_offset"):
            model = wg.parse_model(data, leaf["model_offset"])
            parts = wg.decode_model(data, model)
        category = classify(leaf, parts or [])
        selected = leaf_selected(leaf, lod)
        if category == "sky" and not include_sky:
            selected = False
        if category == "dyno-unspawned" and not include_unspawned:
            selected = False
        if mode == "terrain" and category not in ("static", "static-ramp", "optional-terrain"):
            selected = False
        if mode == "props" and category in ("static", "static-ramp", "optional-terrain", "sky"):
            selected = False
        M = IDENTITY
        confidence = "verified-world-space"
        editable = {}
        if category == "dyno":
            M = native_rows_to_matrix(leaf["dyno"]["matrix"])
            confidence = "matrix-verified-read; row-vector convention inferred"
            editable = dict(matrix=True, translate=True, rotate=True, scale=False)
        elif category == "dyno-unspawned":
            confidence = "not instantiated natively (mode -1); placement unknown, exported at model coordinates"
        elif category in ("static", "static-ramp"):
            editable = dict(translate=True, rotate=False, scale=False)
        elif category == "sky":
            confidence = "camera-relative at runtime (Sky display object)"
        elif category == "billboard":
            confidence = "world-space anchor; faces camera at runtime"
        geom = []
        tris = 0
        lo, hi = [math.inf] * 3, [-math.inf] * 3
        for part in parts or []:
            verts = [apply(M, v["p"]) for v in part["verts"]]
            for v in verts:
                for a in range(3):
                    lo[a] = min(lo[a], v[a])
                    hi[a] = max(hi[a], v[a])
            geom.append(dict(texture=part["texture"], verts=verts, uvs=[v["uv"] for v in part["verts"]],
                             tris=part["tris"]))
            tris += len(part["tris"])
        rec = leaf["record"]
        inst = dict(
            id="i%04d" % i,
            name="%s@%x_i%04d" % (re.sub(r"[^A-Za-z0-9_.-]", "_", member), leaf.get("model_offset") or 0, i),
            assetId="%s#%s" % (track["rel"], member),
            modelId="%s#%s@0x%x" % (track["rel"], member, leaf.get("model_offset") or 0),
            category=category,
            subtype=leaf.get("subtype_name", "Simple Model" if leaf.get("kind") == 0 else None),
            subtypeIndex=leaf.get("index"),
            selected=selected,
            lodPath=[dict(node="0x%x" % s["node"], child=s["index"], of=s["count"],
                          maxDistance=math.sqrt(s["max_dist2"]) if s["max_dist2"] > 0 else 0.0)
                     for s in leaf["path"] if s["kind"] == "lod"],
            cullSpheres=["0x%x" % s["node"] for s in leaf["path"] if s["kind"] == "sphere"],
            conditions=[dict(node="0x%x" % s["node"], type="0x%x" % s["type"], value=s["value"])
                        for s in leaf["path"] if s["kind"] == "cond"],
            transform=M,
            transformConvention="row-major 4x4, column vectors (p_world = M * p_local), native metres, +Y up",
            confidence=confidence,
            editable=editable,
            bbox=dict(min=lo, max=hi) if tris else None,
            pivot=model["centre"] if model else None,
            triangles=tris,
            textures=sorted(set(p["texture"] for p in parts or [])),
            source=dict(member=member, nodeOffset=leaf["node"], recordOffset=rec,
                        modelOffset=leaf.get("model_offset"),
                        archiveRecordOffset=base + rec if rec else None,
                        archiveModelOffset=base + leaf["model_offset"] if leaf.get("model_offset") else None),
        )
        if category in ("dyno", "dyno-unspawned"):
            dy = dict(leaf["dyno"])
            if "matrix_offset" in dy:
                dy["archiveMatrixOffset"] = base + dy["matrix_offset"]
            inst["dyno"] = dy
        inst["_geom"] = geom
        instances.append(inst)
    return graf, instances


# --------------------------------------------------------------------------- export

def export_textures(track, names, dest):
    cache, out = {}, {}
    tdir = dest / "textures"
    tdir.mkdir(parents=True, exist_ok=True)
    for name in sorted(names):
        rec = dict(name=name, status="unresolved")
        asset, data = resolve_texture(track, name, cache)
        if asset:
            rec.update(assetId=asset, memberSha256=sha(data))
            try:
                w, h, rgba, meta = decode_preview(data)
            except ValueError as exc:
                rec.update(status="undecodable", error=str(exc))
            else:
                alphas = rgba[3::4]
                amin = min(alphas)
                fn = re.sub(r"[^A-Za-z0-9_.-]", "_", asset.split("#", 1)[1]) + ".png"
                write_png(str(tdir / fn), w, h, bytes(rgba))
                n = w * h
                tot = sum(alphas) or 1
                avg = tuple(sum(rgba[c + 4 * i] * alphas[i] for i in range(0, n, 7)) /
                            max(1, sum(alphas[i] for i in range(0, n, 7))) for c in range(3)) if tot else (128, 128, 128)
                rec.update(status="exported", png="textures/" + fn, width=w, height=h,
                           alphaMin=amin, format=meta["format"], flags=meta["flags"],
                           averageRgb=[round(x) for x in avg])
        out[name] = rec
    return out


def to_blender(p):
    # Native +Y up -> Blender +Z up, native +Z -> Blender -Y (same as Blender's
    # OBJ importer "Forward -Z, Up Y" default). Handedness is unchanged.
    return (p[0], -p[2], p[1])


def write_obj(instances, textures, dest, stem, blender=False):
    obj, mtl = dest / (stem + ".obj"), dest / (stem + ".mtl")
    mats = {}
    lines = ["# world_export.py %s; native metres, %s" % (TOOL_VERSION, "Blender Z-up (x,-z,y)" if blender else "+Y up"),
             "mtllib %s" % mtl.name]
    vbase = 1
    for inst in instances:
        if not inst["selected"] or not inst["_geom"]:
            continue
        lines.append("o %s" % inst["name"])
        lines.append("g %s %s" % (inst["name"], inst["category"]))
        for part in inst["_geom"]:
            mat = re.sub(r"[^A-Za-z0-9_.-]", "_", os.path.splitext(part["texture"])[0] or "none")
            mats[mat] = part["texture"]
            lines.append("usemtl %s" % mat)
            for v in part["verts"]:
                v = to_blender(v) if blender else v
                lines.append("v %.5f %.5f %.5f" % v)
            for uv in part["uvs"]:
                lines.append("vt %.5f %.5f" % (uv[0], 1.0 - uv[1]))
            for (a, b, c) in part["tris"]:
                ia, ib, ic = a + vbase, b + vbase, c + vbase
                lines.append("f %d/%d %d/%d %d/%d" % (ia, ia, ib, ib, ic, ic))
            vbase += len(part["verts"])
    obj.write_text("\n".join(lines) + "\n")
    ml = ["# world_export.py materials; map_d = same PNG when palette alpha < 255"]
    unresolved = []
    for mat, tex in sorted(mats.items()):
        ml += ["", "newmtl %s" % mat, "Kd 1 1 1", "Ka 0 0 0", "Ks 0 0 0", "illum 1"]
        t = textures.get(tex)
        if t and t["status"] == "exported":
            ml.append("map_Kd %s" % t["png"])
            if t["alphaMin"] < 255:
                ml.append("map_d %s" % t["png"])
            ml.append("# source %s flags 0x%x alphaMin %d" % (t["assetId"], t["flags"], t["alphaMin"]))
        else:
            unresolved.append(tex)
            ml.append("# unresolved texture %r" % tex)
    mtl.write_text("\n".join(ml) + "\n")
    return obj, mtl, sorted(set(unresolved))


def render_topdown(instances, textures, out_png, size=1400, bg=(24, 26, 30)):
    """Orthographic view from +Y looking down. Image right = +X, image down = +Z
    (this is how a right-handed Y-up world looks from above)."""
    sel = [i for i in instances if i["selected"] and i["_geom"]]
    lo = [min(i["bbox"]["min"][a] for i in sel) for a in range(3)]
    hi = [max(i["bbox"]["max"][a] for i in sel) for a in range(3)]
    span = max(hi[0] - lo[0], hi[2] - lo[2]) or 1.0
    scale = (size - 20) / span
    W = int((hi[0] - lo[0]) * scale) + 20
    H = int((hi[2] - lo[2]) * scale) + 20
    zbuf = [-math.inf] * (W * H)
    img = bytearray(bytes(bg) + b"\xff") * (W * H)
    light = (0.35, 0.85, 0.4)
    ln = math.sqrt(sum(x * x for x in light))
    light = [x / ln for x in light]
    for inst in sel:
        for part in inst["_geom"]:
            t = textures.get(part["texture"]) or {}
            col = t.get("averageRgb", (255, 0, 255))
            V = part["verts"]
            P = [((v[0] - lo[0]) * scale + 10, (v[2] - lo[2]) * scale + 10, v[1]) for v in V]
            for (a, b, c) in part["tris"]:
                A, B, C = V[a], V[b], V[c]
                e1 = [B[k] - A[k] for k in range(3)]
                e2 = [C[k] - A[k] for k in range(3)]
                n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
                nl = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2) or 1.0
                shade = 0.35 + 0.65 * abs(n[0] * light[0] + n[1] * light[1] + n[2] * light[2]) / nl
                rgb = bytes(min(255, int(x * shade)) for x in col)
                pa, pb, pc = P[a], P[b], P[c]
                area = (pb[0] - pa[0]) * (pc[1] - pa[1]) - (pc[0] - pa[0]) * (pb[1] - pa[1])
                minx = max(int(min(pa[0], pb[0], pc[0])), 0)
                maxx = min(int(max(pa[0], pb[0], pc[0])) + 1, W - 1)
                miny = max(int(min(pa[1], pb[1], pc[1])), 0)
                maxy = min(int(max(pa[1], pb[1], pc[1])) + 1, H - 1)
                if abs(area) < 1e-9:
                    # Edge-on (vertical) triangle: plot its vertices so walls/poles stay visible.
                    for q in (pa, pb, pc):
                        x, y = int(q[0]), int(q[1])
                        if 0 <= x < W and 0 <= y < H and q[2] > zbuf[y * W + x]:
                            zbuf[y * W + x] = q[2]
                            img[(y * W + x) * 4:(y * W + x) * 4 + 3] = rgb
                    continue
                inv = 1.0 / area
                for y in range(miny, maxy + 1):
                    py = y + 0.5
                    for x in range(minx, maxx + 1):
                        px = x + 0.5
                        w0 = ((pb[0] - px) * (pc[1] - py) - (pc[0] - px) * (pb[1] - py)) * inv
                        w1 = ((pc[0] - px) * (pa[1] - py) - (pa[0] - px) * (pc[1] - py)) * inv
                        w2 = 1.0 - w0 - w1
                        if w0 < 0 or w1 < 0 or w2 < 0:
                            continue
                        z = w0 * pa[2] + w1 * pb[2] + w2 * pc[2]
                        idx = y * W + x
                        if z <= zbuf[idx]:
                            continue
                        zbuf[idx] = z
                        img[idx * 4:idx * 4 + 3] = rgb
    write_png(str(out_png), W, H, bytes(img))
    return dict(width=W, height=H, metresPerPixel=1.0 / scale, origin=[lo[0], lo[2]],
                axes="image right = +X, image down = +Z, viewed from +Y")


def export(args):
    track = load_track(args.disc, args.track)
    dest = Path(args.dest).expanduser().resolve()
    if str(dest).startswith(str(track["disc"])):
        raise ValueError("Destination must be outside the disc")
    dest.mkdir(parents=True, exist_ok=True)
    mode = "props" if args.props_only else "terrain" if args.terrain_only else "all"
    graf, instances = build_scene(track, lod=args.lod, mode=mode, include_sky=args.include_sky,
                                  include_unspawned=args.include_unspawned)
    names = set(p["texture"] for i in instances if i["selected"] for p in i["_geom"])
    textures = export_textures(track, names, dest)
    stem = Path(args.track).stem.lower() + "_world"
    obj, mtl, unresolved = write_obj(instances, textures, dest, stem, blender=args.blender)
    sel = [i for i in instances if i["selected"]]
    stats = dict(instances=len(instances), selected=len(sel),
                 selectedWithGeometry=sum(1 for i in sel if i["_geom"]),
                 objects=sum(1 for i in sel if i["_geom"]),
                 triangles=sum(i["triangles"] for i in sel),
                 textures=len(names), texturesExported=sum(1 for t in textures.values() if t["status"] == "exported"),
                 unresolvedMaterials=unresolved,
                 categories={}, nodeTypes=wg.summarize(graf)["node_types"])
    for i in instances:
        c = stats["categories"].setdefault(i["category"], dict(total=0, selected=0, triangles=0))
        c["total"] += 1
        c["selected"] += int(i["selected"])
        c["triangles"] += i["triangles"] if i["selected"] else 0
    preview = None
    if args.preview is not False:
        png = Path(args.preview) if args.preview else dest / "preview_topdown.png"
        preview = render_topdown(instances, textures, png)
        preview["file"] = str(png.name)
    doc = dict(schemaVersion=1, tool="world_export.py", toolVersion=TOOL_VERSION,
               archive=track["rel"], archiveSha256=sha(track["raw"]),
               member=track["graf_entry"]["name"], memberSha256=sha(track["graf"]),
               memberArchiveOffset=track["graf_entry"]["offset"],
               coordinates=dict(units="metres (native)", up="+Y",
                                obj="Blender Z-up via (x,-z,y)" if args.blender else "native",
                                handedness="see docs/WORLD_FORMAT.md (Coordinates)"),
               options=dict(lod=args.lod, mode=mode, includeSky=args.include_sky,
                            includeUnspawned=args.include_unspawned),
               stats=stats, preview=preview, textures=textures,
               instances=[{k: v for k, v in i.items() if k != "_geom"} for i in instances])
    (dest / "placements.json").write_text(json.dumps(doc, indent=1) + "\n")
    print(json.dumps(dict(obj=str(obj), mtl=str(mtl), placements=str(dest / "placements.json"),
                          preview=preview, stats={k: v for k, v in stats.items() if k != "nodeTypes"}), indent=1))
    return doc


# --------------------------------------------------------------------------- write-back

def vif_q199_offsets(data, part):
    """Byte offsets of every qword-199 (batch position offset xyz) UNPACK target in a 4SPS
    part. VU1 world programs add mem[199].xyz to every decoded position."""
    found = []
    p, end = part["offset"] + 0x38, part["offset"] + part["size"]
    cl = wl = 1
    while p + 4 <= end:
        w = struct.unpack_from("<I", data, p)[0]
        cmd, num, imm = (w >> 24) & 0x7F, (w >> 16) & 0xFF, w & 0xFFFF
        p += 4
        if cmd >= 0x60:
            vn, vl = (cmd >> 2) & 3, cmd & 3
            n = num or 256
            bpe = 2 if vl == 3 else [4, 2, 1][vl] * (vn + 1)
            if vn == 3 and vl == 0:
                for i in range(n):
                    addr = (imm & 0x3FF) + (i // wl) * cl + (i % wl)
                    if addr == 199:
                        found.append(p + 16 * i)
            p += (n * bpe + 3) & ~3
        elif cmd == 0x01:
            cl, wl = imm & 0xFF, (imm >> 8) & 0xFF
        elif cmd == 0x20:
            p += 4
        elif cmd in (0x30, 0x31):
            p += 16
        elif cmd == 0x4A:
            p += (num or 256) * 8
        elif cmd in (0x50, 0x51):
            p += imm * 16
    return found


def _add_f32(buf, off, delta):
    if delta == 0.0:
        return
    v = struct.unpack_from("<f", buf, off)[0]
    struct.pack_into("<f", buf, off, v + delta)


def place(disc, track_rel, edits, out_path):
    track = load_track(disc, track_rel)
    data = bytearray(track["graf"])
    original = bytes(data)
    graf = wg.parse_graf(original)
    by_id = {"i%04d" % i: leaf for i, leaf in enumerate(graf["leaves"])}
    by_record = {leaf["record"]: leaf for leaf in graf["leaves"]}
    mlists = [info for info in graf["mlist_info"] if info]
    report = []
    for edit in edits:
        leaf = by_id.get(edit.get("instance")) if "instance" in edit else by_record.get(edit.get("recordOffset"))
        if leaf is None:
            raise ValueError("Unknown instance in edit: %r" % edit)
        model = wg.parse_model(original, leaf["model_offset"]) if leaf.get("model_offset") else None
        category = classify(leaf, model["parts"] if model else [])
        if "matrix" in edit:
            if category != "dyno":
                raise ValueError("matrix edits are only verified for spawned Dyno records, not %s" % category)
            M = [float(x) for x in edit["matrix"]]
            if len(M) != 16 or any(abs(M[12 + k] - (0.0, 0.0, 0.0, 1.0)[k]) > 1e-6 for k in range(4)):
                raise ValueError("matrix must be a 4x4 affine row-major list")
            R = [[M[0], M[1], M[2]], [M[4], M[5], M[6]], [M[8], M[9], M[10]]]
            for a in range(3):
                for b in range(3):
                    dot = sum(R[k][a] * R[k][b] for k in range(3))
                    if abs(dot - (1.0 if a == b else 0.0)) > 1e-3:
                        raise ValueError("Dyno matrix must be a pure rotation + translation (no scale/shear)")
            det = (R[0][0] * (R[1][1] * R[2][2] - R[1][2] * R[2][1]) - R[0][1] * (R[1][0] * R[2][2] - R[1][2] * R[2][0])
                   + R[0][2] * (R[1][0] * R[2][1] - R[1][1] * R[2][0]))
            if det < 0:
                raise ValueError("Dyno matrix must not mirror")
            rows = matrix_to_native_rows(M)
            off = leaf["record"] + wg.DYNO_MATRIX
            old = list(struct.unpack_from("<12f", original, off))
            for k in range(12):
                if rows[k] != old[k]:
                    struct.pack_into("<f", data, off + 4 * k, rows[k])
            report.append(dict(instance=edit.get("instance"), kind="dyno-matrix", offset=off))
            continue
        if edit.get("rotate") or edit.get("scale"):
            raise ValueError("rotate/scale are not verified for %s; only translate is supported" % category)
        delta = [float(x) for x in edit.get("translate", (0, 0, 0))]
        if len(delta) != 3 or not all(math.isfinite(x) for x in delta):
            raise ValueError("translate must be three finite numbers")
        if category not in ("static", "static-ramp"):
            raise ValueError("translate is only verified for static (kind 0) world models, not %s" % category)
        touched = []
        for part in model["parts"]:
            for q in vif_q199_offsets(original, part):
                for a in range(3):
                    _add_f32(data, q + 4 * a, delta[a])
                touched.append(q)
        if not touched:
            raise ValueError("model has no position-offset qwords to translate")
        mo = leaf["model_offset"]
        for a in range(3):
            _add_f32(data, mo + 0x1C + 4 * a, delta[a])          # centre
            _add_f32(data, mo + 0x28 + 8 * a, delta[a])          # bbox min (x,y,z interleaved)
            _add_f32(data, mo + 0x2C + 8 * a, delta[a])          # bbox max
        moved = math.sqrt(sum(x * x for x in delta))
        # mlist per-model sphere copies: exact (centre, radius) matches inside each mlist.
        key = struct.pack("<4f", *model["centre"], model["radius"])
        mhits = []
        if moved:
            for info in mlists:
                lo_, hi_ = info["offset"], info["offset"] + info["batches"]["offset"]
                start = lo_
                while True:
                    k = original.find(key, start, hi_)
                    if k < 0:
                        break
                    if (k - lo_) % 4 == 0:
                        for a in range(3):
                            _add_f32(data, k + 4 * a, delta[a])
                        mhits.append(k)
                    start = k + 4
        # Ancestor cull spheres: grow the radius to keep covering the moved model.
        grown = []
        for step in leaf["path"]:
            if step["kind"] != "sphere" or not moved:
                continue
            n = step["node"]
            cx, cy, cz, r = struct.unpack_from("<4f", original, n + 0xC)
            c2 = [model["centre"][a] + delta[a] for a in range(3)]
            need = math.sqrt(sum((c2[a] - (cx, cy, cz)[a]) ** 2 for a in range(3))) + model["radius"]
            if need > r:
                struct.pack_into("<f", data, n + 0x18, need * 1.001)
                grown.append(n)
        report.append(dict(instance=edit.get("instance"), kind="translate", delta=delta,
                           q199=len(touched), mlistSpheres=len(mhits), cullSpheresGrown=len(grown),
                           warnings=["visual only: collision (track.bsc/track.bpp/track.sol) is unchanged",
                                     "LOD group centres unchanged (draw-distance origin stays at the old place)"]))
    new_payload = bytes(data)
    if len(new_payload) != len(original):
        raise AssertionError("payload size changed")
    raw = bytearray(track["raw"])
    ge = track["graf_entry"]
    raw[ge["offset"]:ge["offset"] + len(new_payload)] = new_payload
    if new_payload != original:
        struct.pack_into("<I", raw, track["dir_offset"] + track["graf_index"] * 68 + 48, eod_crc(new_payload))
    out_path = Path(out_path).expanduser().resolve()
    if str(out_path).startswith(str(track["disc"])):
        raise ValueError("Output must be outside the disc")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    tmp = out_path.with_suffix(out_path.suffix + ".tmp")
    tmp.write_bytes(bytes(raw))
    # Verify: directory/CRC valid, every other member byte-identical.
    built = directory(tmp)
    for e_old, e_new in zip(track["entries"], built):
        if e_old["name"] != e_new["name"] or e_old["offset"] != e_new["offset"]:
            raise AssertionError("directory layout changed")
        if e_old["name"] == ge["name"]:
            payload(tmp, e_new["name"], built, verify_crc=True)
            continue
        a = track["raw"][e_old["offset"]:e_old["offset"] + e_old["size"]]
        b = bytes(raw[e_new["offset"]:e_new["offset"] + e_new["size"]])
        if a != b or e_old["crc"] != e_new["crc"]:
            raise AssertionError("unedited member changed: " + e_old["name"])
    os.replace(tmp, out_path)
    changed = [i for i in range(len(original)) if original[i] != new_payload[i]]
    return dict(out=str(out_path), sha256=sha(bytes(raw)), edits=report, changedPayloadBytes=len(changed),
                identical=bytes(raw) == track["raw"])


# --------------------------------------------------------------------------- CLI

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd")
    ex = sub.add_parser("export")
    for p in (ap, ex):
        p.add_argument("--disc")
        p.add_argument("--track")
        p.add_argument("--dest")
        p.add_argument("--lod", type=int, default=0)
        g = p.add_mutually_exclusive_group()
        g.add_argument("--props-only", action="store_true")
        g.add_argument("--terrain-only", action="store_true")
        p.add_argument("--blender", action="store_true", help="write OBJ in Blender Z-up axes (x,-z,y)")
        p.add_argument("--include-sky", action="store_true")
        p.add_argument("--include-unspawned", action="store_true",
                       help="also export Dyno models whose spawn mode is -1 (not instantiated natively)")
        p.add_argument("--preview", nargs="?", const=None, default=None,
                       help="top-down preview PNG path (default DEST/preview_topdown.png)")
        p.add_argument("--no-preview", dest="preview", action="store_const", const=False)
    pl = sub.add_parser("place")
    pl.add_argument("--disc", required=True)
    pl.add_argument("--track", required=True)
    pl.add_argument("--edits", required=True)
    pl.add_argument("--out", required=True)
    args = ap.parse_args(argv)
    if args.cmd == "place":
        edits = json.loads(Path(args.edits).read_text())
        edits = edits.get("edits", edits) if isinstance(edits, dict) else edits
        print(json.dumps(place(args.disc, args.track, edits, args.out), indent=1))
        return
    if not (args.disc and args.track and args.dest):
        ap.error("export needs --disc, --track and --dest")
    export(args)


if __name__ == "__main__":
    main()
