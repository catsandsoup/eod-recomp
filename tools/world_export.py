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
from tex import write_png  # noqa: E402

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
        return "static"
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


def rebuild_billboard(data, model):
    """Facing sprites (VU program 0x6A) store each quad corner pair at one point; the
    VU expands it toward the camera at run time. Rebuild the authored quad from the
    model bbox (its widest horizontal axis) and each corner's U coordinate. The
    width source is the model header bbox (verified equal to the authored quad);
    the runtime expansion itself is not decoded."""
    bb = model["bbox"]
    wx, wz = bb["max"][0] - bb["min"][0], bb["max"][2] - bb["min"][2]
    axis, width = (0, wx) if wx >= wz else (2, wz)
    out = []
    for part in model["parts"]:
        verts, tris = [], []
        for bt in wg.vif_mesh.decode_part(data, part):
            base = len(verts)
            vs = []
            ys = [v["p"][1] for v in bt["verts"]]
            flat_y = bool(ys) and max(ys) - min(ys) < 1e-4   # Point-Facing: both axes collapsed
            for v in bt["verts"]:
                p = list(v["p"])
                p[axis] = (bb["min"][axis] + bb["max"][axis]) / 2 + (v["uv"][0] - 0.5) * width
                if flat_y:
                    p[1] = (bb["min"][1] + bb["max"][1]) / 2 + (0.5 - v["uv"][1]) * (bb["max"][1] - bb["min"][1])
                vs.append(dict(v, p=tuple(p)))
            for (a, b, c) in wg.vif_mesh.strip_triangles(vs):
                if len({vs[a]["p"], vs[b]["p"], vs[c]["p"]}) == 3:
                    tris.append((a + base, b + base, c + base))
            verts.extend(vs)
        out.append(dict(texture=part["texture"], group=part["group"], verts=verts, tris=tris, mscal=[]))
    return out


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


def build_scene(track, lod=0, mode="all", include_sky=False, include_unspawned=False, event=None):
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
        if category == "billboard" and model:
            parts = rebuild_billboard(data, model)
        selected = leaf_selected(leaf, lod)
        if category == "sky" and not include_sky:
            selected = False
        if category == "dyno-unspawned" and not include_unspawned:
            selected = False
        if mode == "terrain" and category not in ("static", "optional-terrain"):
            selected = False
        if mode == "props" and category in ("static", "optional-terrain", "sky"):
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
        elif category in ("static", "optional-terrain"):
            editable = dict(translate=True, rotate=False, scale=False)
        elif category == "sky":
            confidence = "camera-relative at runtime (Sky display object)"
        elif category == "billboard":
            confidence = "camera-facing sprite; quad rebuilt from model bbox + corner U (authored orientation)"
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
            rampHint=any("ramp" in p["texture"].lower() for p in parts or []),
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
        if "optional_terrain" in leaf:
            inst["optionalTerrain"] = leaf["optional_terrain"]
        inst["_geom"] = geom
        instances.append(inst)
    if event:
        instances.extend(build_event_instances(track, event, mode))
    return graf, instances


def build_event_instances(track, event, mode):
    out, models = [], {}
    for k, prop in enumerate(event_props(track, event)):
        rows = compose_rows(prop["frame"], prop["localFrame"]) if prop["localFrame"] else prop["frame"]
        M = native_rows_to_matrix(rows)
        geom, tris, archive_rel = [], 0, None
        lo, hi = [math.inf] * 3, [-math.inf] * 3
        if prop["model"]:
            if prop["model"].lower() not in models:
                rel, entry, data = find_member(track, prop["model"], kinds=("MODL",))
                parts = wg.decode_model(data, wg.parse_model(data, 0)) if data else None
                models[prop["model"].lower()] = (rel, entry, parts)
            archive_rel, entry, parts = models[prop["model"].lower()]
            for part in parts or []:
                verts = [apply(M, v["p"]) for v in part["verts"]]
                for v in verts:
                    for a in range(3):
                        lo[a], hi[a] = min(lo[a], v[a]), max(hi[a], v[a])
                geom.append(dict(texture=part["texture"], verts=verts, uvs=[v["uv"] for v in part["verts"]],
                                 tris=part["tris"], archive=archive_rel))
                tris += len(part["tris"])
        model_id = "%s#%s" % (archive_rel, entry["name"]) if archive_rel else None
        out.append(dict(
            id="e%04d" % k, name="%s_%s_e%04d" % (re.sub(r"[^A-Za-z0-9_.-]", "_", prop["member"]),
                                                    re.sub(r"[^A-Za-z0-9_.-]", "_", prop["dyno"]), k),
            assetId=model_id, modelId=model_id, category="event-prop", subtype=prop["dynoType"],
            dyno=dict(name=prop["dyno"], obstacleSource=prop["obstacleSource"], section=prop["section"],
                      model=prop["model"], modelInherited=prop["modelInherited"],
                      localFrame=prop["localFrame"], localFrameFrom=prop["localFrameFrom"],
                      frame=prop["frame"]),
            selected=mode != "terrain" and bool(geom), lodPath=[], cullSpheres=[], conditions=[],
            transform=M, transformConvention="row-major 4x4, column vectors (p_world = M * p_local), native metres, +Y up; "
                                             "= event frame x obstacle local_frame when present",
            confidence="frame decoded from unfloat_frame64 (alphabet verified in ELF, bit order inferred); "
                       "runtime drop/settle not modelled",
            editable=dict(matrix=True, translate=True, rotate=True, scale=False),
            bbox=dict(min=lo, max=hi) if tris else None, pivot=prop["frame"][9:12], triangles=tris,
            textures=sorted(set(p["texture"] for p in geom)), rampHint=False,
            source=dict(member=prop["member"], frameMemberOffset=prop["memberOffset"],
                        frameArchiveOffset=prop["archiveOffset"], frameLength=64),
            _geom=geom))
    return out



# --------------------------------------------------------------------------- event props

# ELF 0x3CCFC0 holds this 64-symbol alphabet next to the "unfloat_frame64" key
# (0x4094B0). 64 symbols x 6 bits = 48 bytes = 12 little-endian f32, symbols
# packed most-significant-bit first (inferred: every retail frame then decodes
# to an orthonormal rotation + plausible position; encode(decode(s)) == s).
FRAME64_ALPHABET = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789,."


def decode_frame64(text):
    if len(text) != 64:
        raise ValueError("frame64 must be 64 symbols")
    n = 0
    for ch in text:
        n = (n << 6) | FRAME64_ALPHABET.index(ch)
    return list(struct.unpack("<12f", n.to_bytes(48, "big")))


def encode_frame64(values):
    n = int.from_bytes(struct.pack("<12f", *values), "big")
    return "".join(FRAME64_ALPHABET[(n >> (6 * (63 - i))) & 63] for i in range(64))


def _members(track, rel_path):
    path = track["disc"] / rel_path
    st = path.stat()
    return path, cached_directory(str(path), st.st_mtime_ns, st.st_size)


def find_member(track, name, kinds=None):
    """Track archive first, then the shared .RES list (same order as textures)."""
    for path, rel in [(track["path"], track["rel"])] + [
            (track["disc"] / "DATA/RES" / f, "DATA/RES/" + f) for f in SHARED_RES]:
        if not path.is_file():
            continue
        st = path.stat()
        entries = cached_directory(str(path), st.st_mtime_ns, st.st_size)
        e = next((e for e in entries if e["name"].lower() == name.lower()
                  and (kinds is None or e["type"] in kinds)), None)
        if e:
            data, _ = payload(path, e["name"], entries)
            return rel, e, data
    return None, None, None


def _local_frame(sections_by_name, ref):
    """Resolve an obstacle.val '@local_frame' child: basis_x/y/z + origin, or frame64."""
    sec = sections_by_name.get(ref)
    if not sec:
        return None
    pairs = dict(sec["pairs"])
    if "frame64" in pairs:
        return decode_frame64(pairs["frame64"])
    try:
        rows = [float(x) for k in ("basis_x", "basis_y", "basis_z") for x in pairs[k].split(",")]
        return rows + [float(x) for x in pairs.get("origin", "0,0,0").split(",")]
    except (KeyError, ValueError):
        return None


def compose_rows(outer, inner):
    """inner maps model -> parent space, outer parent -> world (both native rows)."""
    Mo, Mi = native_rows_to_matrix(outer), native_rows_to_matrix(inner)
    M = [sum(Mo[r * 4 + k] * Mi[k * 4 + c] for k in range(4)) for r in range(4) for c in range(4)]
    return matrix_to_native_rows(M)


def obstacle_table(track):
    """name -> merged [dyno] pairs: track obstacle.val overrides RACE.RES (hypothesis order).
    Inherited model / physics local frame come from the nearest preceding template
    section (e.g. base_fun_barrel, conc_wall_phys2); that rule is inferred from
    data, not from the loader."""
    from valt import parse_binary_valt
    table = {}
    for path, rel in ((track["disc"] / "DATA/RES/RACE.RES", "DATA/RES/RACE.RES"), (track["path"], track["rel"])):
        if not path.is_file():
            continue
        st = path.stat()
        entries = cached_directory(str(path), st.st_mtime_ns, st.st_size)
        e = next((e for e in entries if e["name"].lower() == "obstacle.val"), None)
        if not e:
            continue
        data, _ = payload(path, e["name"], entries)
        sections = parse_binary_valt(data)["sections"]
        by_name = {sec["name"]: sec for sec in sections}
        prev, phys = None, None
        for sec in sections:
            raw = sec["pairs"]
            pairs = dict((k, v) for k, v in raw if not k.startswith("@"))
            refs = dict((k, v) for k, v in raw if k.startswith("@"))
            is_dyno = sec["name"] == "dyno" or sec["name"].startswith("dyno_")
            if not is_dyno and not sec["name"].startswith("$"):
                # any named template section replaces the current one ($NNNN$ children do not)
                phys = dict(pairs, _name=sec["name"], _frame=_local_frame(by_name, refs.get("@local_frame")))
            if is_dyno and "name" in pairs:
                rec = dict(pairs, _source="%s#%s" % (rel, e["name"]), _section=sec["name"])
                if "model" not in rec and prev and "model" in prev:
                    # e.g. base_fun_barrel precedes dyno fun_barrel; inheritance rule unverified.
                    rec.update(model=prev["model"], _modelInherited=True)
                    rec.setdefault("type", prev.get("type"))
                own = _local_frame(by_name, refs.get("@local_frame"))
                if own:
                    rec["_localFrame"], rec["_localFrameFrom"] = own, "own"
                elif phys and phys.get("_frame") and phys["_name"].lower() == pairs["name"].lower():
                    rec["_localFrame"], rec["_localFrameFrom"] = phys["_frame"], "template %s (inferred)" % phys["_name"]
                elif (phys and phys.get("_frame") and rec.get("type") == "wall" and "dx" not in rec
                      and phys["_name"].startswith("conc_wall_phys")):
                    rec["_localFrame"], rec["_localFrameFrom"] = phys["_frame"], "template %s (inferred)" % phys["_name"]
                table[pairs["name"].lower()] = rec
            prev = None if is_dyno else pairs
    return table


def event_props(track, event):
    """Objects of one event table (c_<event>.va0): dyno name + unfloat_frame64 frame."""
    from valt import parse_binary_valt
    name = event if event.lower().endswith(".va0") else event + ".va0"
    e = next((e for e in track["entries"] if e["name"].lower() == name.lower()), None)
    if e is None:
        raise ValueError("No event table %s in %s" % (name, track["rel"]))
    data, _ = payload(track["path"], e["name"], track["entries"])
    obstacles = obstacle_table(track)
    out = []
    for sec in parse_binary_valt(data)["sections"]:
        pairs = dict(sec["pairs"])
        if "unfloat_frame64" not in pairs or "dyno" not in pairs:
            continue
        frame_s = pairs["unfloat_frame64"]
        raw = frame_s.encode("latin1")
        if data.count(raw) != 1:
            raise ValueError("frame string not unique in %s" % e["name"])
        obst = obstacles.get(pairs["dyno"].lower(), {})
        out.append(dict(section=sec["name"], dyno=pairs["dyno"], frame=decode_frame64(frame_s),
                        member=e["name"], memberOffset=data.find(raw), archiveOffset=e["offset"] + data.find(raw),
                        model=obst.get("model"), dynoType=obst.get("type"), obstacleSource=obst.get("_source"),
                        modelInherited=bool(obst.get("_modelInherited")),
                        localFrame=obst.get("_localFrame"), localFrameFrom=obst.get("_localFrameFrom")))
    return out

# --------------------------------------------------------------------------- export

def export_textures(track, names, dest):
    cache, out = {}, {}
    tdir = dest / "textures"
    tdir.mkdir(parents=True, exist_ok=True)
    for name in sorted(names):
        rec = dict(name=name, status="unresolved")
        if name.lower() in ("", "-", ".tex"):
            out[name] = dict(name=name, status="untextured")
            continue
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


# Native space is LEFT-handed: +X right, +Y up, +Z forward (verified by
# rendering a textured world prop: text reads mirrored under a right-handed
# interpretation; docs/WORLD_FORMAT.md "Coordinates").
AXES = {
    "native": (lambda p: p, False),                       # LH, Y-up, verbatim
    "rh-yup": (lambda p: (p[0], p[1], -p[2]), True),      # RH Y-up (glTF/Maya); mirror => reverse winding
    "blender": (lambda p: (p[0], p[2], p[1]), True),      # RH Z-up: native +Z forward -> Blender +Y
}


def write_obj(instances, textures, dest, stem, axes="native"):
    obj, mtl = dest / (stem + ".obj"), dest / (stem + ".mtl")
    mats = {}
    conv, mirror = AXES[axes]
    lines = ["# world_export.py %s; metres; axes=%s (native is left-handed +X right, +Y up, +Z forward)"
             % (TOOL_VERSION, axes), "mtllib %s" % mtl.name]
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
                lines.append("v %.5f %.5f %.5f" % tuple(conv(v)))
            for uv in part["uvs"]:
                lines.append("vt %.5f %.5f" % (uv[0], 1.0 - uv[1]))
            for (a, b, c) in part["tris"]:
                if mirror:
                    b, c = c, b
                ia, ib, ic = a + vbase, b + vbase, c + vbase
                lines.append("f %d/%d %d/%d %d/%d" % (ia, ia, ib, ib, ic, ic))
            vbase += len(part["verts"])
    obj.write_text("\n".join(lines) + "\n")
    ml = ["# world_export.py materials; map_d = same PNG when palette alpha < 255"]
    unresolved = []
    for mat, tex in sorted(mats.items()):
        ml += ["", "newmtl %s" % mat, "Kd 1 1 1", "Ka 0 0 0", "Ks 0 0 0", "illum 1"]
        t = textures.get(tex)
        if tex.lower() in ("", "-", ".tex"):
            ml.append("Kd 0.25 0.25 0.25")
            ml.append("# untextured native part (empty texture name)")
            continue
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


def core_bounds(sel, keep=0.96):
    """Bounds of the central `keep` fraction of instance centres (drops far backdrops)."""
    cs = sorted(((i["bbox"]["min"][0] + i["bbox"]["max"][0]) / 2, (i["bbox"]["min"][2] + i["bbox"]["max"][2]) / 2)
                for i in sel)
    k = int(len(cs) * (1 - keep) / 2)
    xs = sorted(c[0] for c in cs)[k:len(cs) - k]
    zs = sorted(c[1] for c in cs)[k:len(cs) - k]
    pad = 0.08 * max(xs[-1] - xs[0], zs[-1] - zs[0])
    return [xs[0] - pad, 0, zs[0] - pad], [xs[-1] + pad, 0, zs[-1] + pad]


def racing_line(track, name=None):
    """X/Z of ILIN nodes (layout as tools/trk.py decompile_ild: floats +0/+4).
    Default: track.ild, else the largest .ild member."""
    ilds = sorted((e for e in track["entries"] if e["type"] == "ILIN" and e["name"].lower().endswith(".ild")),
                  key=lambda e: (e["name"].lower() != "track.ild", -e["size"]))
    e = next((e for e in ilds if name is None or e["name"].lower() == name), None)
    if e is None or e["size"] < 48:
        return []
    d, _ = payload(track["path"], e["name"], track["entries"], verify_crc=False)
    node_sz, nodes_off = struct.unpack_from("<I", d, 8)[0], struct.unpack_from("<I", d, 32)[0]
    if node_sz < 8 or nodes_off >= len(d):
        return []
    return [struct.unpack_from("<2f", d, o) for o in range(nodes_off, len(d) - node_sz + 1, node_sz)]


def render_topdown(instances, textures, out_png, size=1400, bg=(24, 26, 30), core=False, overlay=(), bounds=None):
    """Orthographic view from +Y looking down at the left-handed native world:
    image right = +X, image up = +Z (an unmirrored map)."""
    sel = [i for i in instances if i["selected"] and i["triangles"]]
    lo = [min(i["bbox"]["min"][a] for i in sel) for a in range(3)]
    hi = [max(i["bbox"]["max"][a] for i in sel) for a in range(3)]
    if core:
        lo, hi = core_bounds(sel)
    if bounds:
        lo, hi = bounds
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
            col = t.get("averageRgb", (90, 90, 90) if part["texture"].lower() in ("", "-", ".tex") else (255, 0, 255))
            V = part["verts"]
            P = [((v[0] - lo[0]) * scale + 10, (hi[2] - v[2]) * scale + 10, v[1]) for v in V]
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
    for (x, z) in overlay:   # racing line: 3x3 red dots
        px, py = int((x - lo[0]) * scale + 10), int((hi[2] - z) * scale + 10)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if 0 <= px + dx < W and 0 <= py + dy < H:
                    o = ((py + dy) * W + px + dx) * 4
                    img[o:o + 3] = b"\xff\x30\x30"
    write_png(str(out_png), W, H, bytes(img))
    return dict(overlay="ILIN racing line nodes (red)" if overlay else None, width=W, height=H, metresPerPixel=1.0 / scale, topLeftXZ=[lo[0] - 10 / scale, hi[2] + 10 / scale],
                axes="image right = +X, image up = +Z, viewed from +Y (unmirrored for the left-handed native space)")


def export(args):
    track = load_track(args.disc, args.track)
    dest = Path(args.dest).expanduser().resolve()
    if str(dest).startswith(str(track["disc"])):
        raise ValueError("Destination must be outside the disc")
    dest.mkdir(parents=True, exist_ok=True)
    mode = "props" if args.props_only else "terrain" if args.terrain_only else "all"
    graf, instances = build_scene(track, lod=args.lod, mode=mode, include_sky=args.include_sky,
                                  include_unspawned=args.include_unspawned, event=getattr(args, "event", None))
    names = set(p["texture"] for i in instances if i["selected"] for p in i["_geom"])
    textures = export_textures(track, names, dest)
    stem = Path(args.track).stem.lower() + "_world"
    axes = "blender" if args.blender else "rh-yup" if args.rh else "native"
    obj, mtl, unresolved = write_obj(instances, textures, dest, stem, axes=axes)
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
        line = racing_line(track)
        preview = render_topdown(instances, textures, png, core=True, overlay=line)
        preview["file"] = str(png.name)
        full = png.with_name(png.stem + "_full.png")
        preview["full"] = render_topdown(instances, textures, full, overlay=line)
        preview["full"]["file"] = full.name
        props = [i for i in instances if i["category"] == "event-prop" and i["triangles"] and i["selected"]]
        if props:
            lo = [min(i["bbox"]["min"][a] for i in props) - 15 for a in range(3)]
            hi = [max(i["bbox"]["max"][a] for i in props) + 15 for a in range(3)]
            ev = png.with_name(png.stem + "_event.png")
            preview["event"] = render_topdown(instances, textures, ev, size=1200, overlay=line, bounds=(lo, hi))
            preview["event"]["file"] = ev.name
    doc = dict(schemaVersion=1, tool="world_export.py", toolVersion=TOOL_VERSION,
               archive=track["rel"], archiveSha256=sha(track["raw"]),
               member=track["graf_entry"]["name"], memberSha256=sha(track["graf"]),
               memberArchiveOffset=track["graf_entry"]["offset"],
               coordinates=dict(units="metres (native)", up="+Y",
                                obj=axes, handedness="native is left-handed: +X right, +Y up, +Z forward",
                                objConversions={"native": "(x,y,z)", "rh-yup": "(x,y,-z), winding reversed",
                                                "blender": "(x,z,y), winding reversed"},
                                placementsSpace="native"),
               options=dict(lod=args.lod, mode=mode, event=getattr(args, "event", None), includeSky=args.include_sky,
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
    members = {}          # other member name -> bytearray (event tables)
    for edit in edits:
        if "event" in edit:
            report.append(_place_event_prop(track, edit, members))
            continue
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
        if category not in ("static", "optional-terrain"):
            raise ValueError("translate is only verified for world-space static/optional-terrain models, not %s" % category)
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
    patched = {ge["name"]: (new_payload, original)}
    for name, buf in members.items():
        e = next(e for e in track["entries"] if e["name"] == name)
        patched[name] = (bytes(buf), track["raw"][e["offset"]:e["offset"] + e["size"]])
    for index, e in enumerate(track["entries"]):
        if e["name"] not in patched:
            continue
        new, old = patched[e["name"]]
        if len(new) != len(old):
            raise AssertionError("payload size changed: " + e["name"])
        raw[e["offset"]:e["offset"] + len(new)] = new
        if new != old:
            struct.pack_into("<I", raw, track["dir_offset"] + index * 68 + 48, eod_crc(new))
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
        if e_old["name"] in patched:
            payload(tmp, e_new["name"], built, verify_crc=patched[e_old["name"]][0] != patched[e_old["name"]][1])
            continue
        a = track["raw"][e_old["offset"]:e_old["offset"] + e_old["size"]]
        b = bytes(raw[e_new["offset"]:e_new["offset"] + e_new["size"]])
        if a != b or e_old["crc"] != e_new["crc"]:
            raise AssertionError("unedited member changed: " + e_old["name"])
    os.replace(tmp, out_path)
    changed = [i for name, (new, old) in patched.items() for i in range(len(old)) if old[i] != new[i]]
    return dict(out=str(out_path), sha256=sha(bytes(raw)), edits=report, changedPayloadBytes=len(changed),
                identical=bytes(raw) == track["raw"])


def _place_event_prop(track, edit, members):
    """Rewrite one unfloat_frame64 string in place (same 64 bytes) in c_<event>.va0."""
    props = event_props(track, edit["event"])
    pid = edit.get("prop")
    k = int(pid[1:]) if isinstance(pid, str) and pid.startswith("e") else int(edit.get("index", -1))
    if not 0 <= k < len(props):
        raise ValueError("Unknown event prop: %r" % edit)
    prop = props[k]
    rows = list(prop["frame"])
    if "matrix" in edit:
        M = [float(x) for x in edit["matrix"]]
        _check_rigid(M)
        new_rows = matrix_to_native_rows(M)
        if prop["localFrame"]:
            # placements.json transforms include the obstacle local frame; store the event frame.
            L = native_rows_to_matrix(prop["localFrame"])
            R = [[L[0], L[1], L[2]], [L[4], L[5], L[6]], [L[8], L[9], L[10]]]
            t = [L[3], L[7], L[11]]
            inv = [R[0][0], R[1][0], R[2][0], -(R[0][0] * t[0] + R[1][0] * t[1] + R[2][0] * t[2]),
                   R[0][1], R[1][1], R[2][1], -(R[0][1] * t[0] + R[1][1] * t[1] + R[2][1] * t[2]),
                   R[0][2], R[1][2], R[2][2], -(R[0][2] * t[0] + R[1][2] * t[1] + R[2][2] * t[2]),
                   0.0, 0.0, 0.0, 1.0]
            new_rows = compose_rows(new_rows, matrix_to_native_rows(inv))
    elif "translate" in edit:
        d = [float(x) for x in edit["translate"]]
        new_rows = rows[:9] + [rows[9 + a] + d[a] if d[a] != 0.0 else rows[9 + a] for a in range(3)]
    else:
        raise ValueError("event prop edits support matrix or translate")
    if edit.get("rotate") or edit.get("scale"):
        raise ValueError("use a full matrix for rotation; scale is not supported")
    text = encode_frame64(new_rows) if new_rows != rows else None
    if text is not None:
        if decode_frame64(text) != [struct.unpack("<f", struct.pack("<f", x))[0] for x in new_rows]:
            raise AssertionError("frame64 roundtrip failed")
        name = prop["member"]
        if name not in members:
            e = next(e for e in track["entries"] if e["name"] == name)
            members[name] = bytearray(track["raw"][e["offset"]:e["offset"] + e["size"]])
        buf = members[name]
        if bytes(buf[prop["memberOffset"]:prop["memberOffset"] + 64]) != encode_frame64(rows).encode():
            raise AssertionError("frame bytes do not match the decoded frame")
        buf[prop["memberOffset"]:prop["memberOffset"] + 64] = text.encode("latin1")
    return dict(prop="e%04d" % k, kind="event-frame", member=prop["member"], offset=prop["memberOffset"],
                changed=text is not None,
                warnings=["runtime may settle/drop the prop (drop_height); verify in game"])


def _check_rigid(M):
    if len(M) != 16 or any(abs(M[12 + k] - (0.0, 0.0, 0.0, 1.0)[k]) > 1e-6 for k in range(4)):
        raise ValueError("matrix must be a 4x4 affine row-major list")
    R = [[M[0], M[1], M[2]], [M[4], M[5], M[6]], [M[8], M[9], M[10]]]
    for a in range(3):
        for b in range(3):
            if abs(sum(R[k][a] * R[k][b] for k in range(3)) - (1.0 if a == b else 0.0)) > 1e-3:
                raise ValueError("matrix must be a pure rotation + translation (no scale/shear)")
    det = (R[0][0] * (R[1][1] * R[2][2] - R[1][2] * R[2][1]) - R[0][1] * (R[1][0] * R[2][2] - R[1][2] * R[2][0])
           + R[0][2] * (R[1][0] * R[2][1] - R[1][1] * R[2][0]))
    if det < 0:
        raise ValueError("matrix must not mirror")


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
        p.add_argument("--blender", action="store_true",
                       help="write OBJ for Blender (right-handed Z-up): (x,z,y), winding reversed")
        p.add_argument("--rh", action="store_true", help="write OBJ right-handed Y-up: (x,y,-z), winding reversed")
        p.add_argument("--include-sky", action="store_true")
        p.add_argument("--event", help="also place one event table's props, e.g. c_jump_race")
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
