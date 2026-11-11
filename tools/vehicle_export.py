#!/usr/bin/env python3
"""Export retail EOD vehicles (DATA/CAR/*.CAR) to OBJ + MTL + PNG + vehicle.json.

Geometry comes from vif_mesh (native vertex order is preserved per part, so the
OBJ can be fed back to vehicle_import / vif_patch). Materials resolve through
modding_backend.resolve_material exactly as the Creation Kit model inspector does
(model archive, then HOBBYSET/RACE/PART/COMMON/GENESIS, numbered CT.RES liveries);
unresolved materials are listed and left without map_Kd, never substituted.
Wheels are exported from RACE.RES at the origin: no native wheel socket position
is verified (see docs/VEHICLE_OBJ_PIPELINE.md).
"""
import argparse
import hashlib
import json
import re
import sys
import tempfile
import traceback
from functools import lru_cache
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import modding_backend as mb
import vif_mesh
from tex import write_png
from texture_preview import decode_preview
from valt import parse_binary_valt

SCHEMA = 1
LOW = re.compile(r"low(\d+)\.mod", re.I)
SHADOW = re.compile(r"shadow(\d+)\.mod", re.I)
NO_TEXTURE = ("", "-", ".tex")
# Placeholder project path: nonexistent, so the Kit's saved-edit lookup yields no edits.
KIT_REQUEST = dict(project=str(Path(tempfile.gettempdir()) / "eod-vehicle-export-no-project.json"),
                   archive="", asset="", textureEdits={})


@lru_cache(maxsize=64)
def file_sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def disc_root(disc):
    root = Path(disc).expanduser().resolve()
    if not (root / "DATA" / "CAR").is_dir():
        raise ValueError(f"Not an EOD disc folder (no DATA/CAR): {root}")
    return root


def car_files(disc):
    return sorted((p for p in (disc / "DATA" / "CAR").iterdir() if p.suffix.lower() == ".car"),
                  key=lambda p: p.name.upper())


def find_car(disc, car):
    name = Path(str(car)).name
    hits = [p for p in car_files(disc) if p.name.lower() == name.lower()]
    if len(hits) != 1:
        raise ValueError(f"Car archive not found on disc: {car}")
    return hits[0]


def rel(disc, path):
    return Path(path).resolve().relative_to(disc).as_posix()


def parse_lods(spec):
    """Returns (set of LOD numbers or None for all, include_shadows)."""
    parts = [p.strip().lower() for p in str(spec).split(",") if p.strip()]
    if not parts:
        raise ValueError("--lod needs 'all', a number list such as 1,2, and/or 'shadow'")
    if "all" in parts:
        return None, True
    lods, shadows = set(), False
    for p in parts:
        if p == "shadow":
            shadows = True
        elif p.isdigit():
            lods.add(int(p))
        else:
            raise ValueError(f"Unknown --lod value: {p}")
    return lods, shadows


def decode_model(data):
    """Parts with native-order vertices and triangles (degenerates dropped, as vif_mesh.load_mesh)."""
    model = vif_mesh.parse_modl(data)
    for part in model["parts"]:
        verts, tris, mscal = [], [], set()
        batches = vif_mesh.decode_part(data, part)
        for batch in batches:
            base = len(verts)
            mscal.add(batch["mscal"])
            for a, b, c in vif_mesh.triangulate_batch(batch):
                pa, pb, pc = (batch["verts"][i]["p"] for i in (a, b, c))
                if pa != pb and pb != pc and pa != pc:
                    tris.append((a + base, b + base, c + base))
            verts.extend(batch["verts"])
        part.update(verts=verts, tris=tris, batches=len(batches), mscal=sorted(mscal))
    return model


def valt_dump(raw):
    return [dict(section=s["name"], pairs=[list(p) for p in s["pairs"]]) for s in parse_binary_valt(raw)["sections"]]


def pick_paint(disc, car_path, number=None):
    paint = mb.vehicle_paint(disc, car_path, include_embedded=True)
    if number is None:
        return paint
    if paint is None:
        raise ValueError(f"{car_path.name} has no native numbered-livery mapping; --paint is unavailable")
    if not re.fullmatch(r"[0-9]+", str(number)):
        raise ValueError("--paint must be a racing number")
    ct = disc / "DATA/RES/CT.RES"
    entries = mb.directory(ct)
    asset = next((e["name"] for suffix in (".tm0", ".tex") for e in entries
                  if e["name"].lower() == str(number) + suffix), None)
    if asset is None:
        raise ValueError(f"CT.RES has no livery {number}; no other paint is substituted")
    return dict(paint, number=str(number), asset=asset, driver="(explicit --paint)")


def locate(disc, archive, name, digest, paint):
    """assetId of the member whose bytes the resolver returned (None if none matches)."""
    shared = [disc / "DATA/RES" / f for f in ("HOBBYSET.RES", "RACE.RES", "PART.RES", "COMMON.RES", "GENESIS.RES")]
    cands = [(archive, (name, Path(name).stem + ".tm0"))]
    if paint:
        cands.insert(0, (paint["archive"], (paint["asset"], "79a.tm0")))
    cands += [(a, (name, Path(name).stem + ".tm0")) for a in shared]
    for path, names in cands:
        if not Path(path).is_file():
            continue
        entries = mb.directory(Path(path))
        for e in entries:
            if e["name"].lower() in {n.lower() for n in names} and sha(mb.payload(Path(path), e["name"], entries, False)[0]) == digest:
                return f"{rel(disc, path)}#{e['name']}"
    return None


# See world_export.alpha_treatment: palette alpha is mostly a blend weight, so only textures with
# >=1% alpha==0 texels (retail BUS: only wheel_bustire, 20.7%) are exported as cutouts.
CUTOUT_ZERO_FRACTION = 0.01
ALPHA_MODES = ("cutout", "blend", "opaque")


def alpha_treatment(alphas, mode):
    zero = alphas.count(0) / max(1, len(alphas))
    if mode == "blend":
        return ("blend" if min(alphas) < 255 else "opaque"), zero
    if mode == "cutout" and zero >= CUTOUT_ZERO_FRACTION:
        return "cutout", zero
    return "opaque", zero


def apply_alpha(rgba, kind):
    if kind == "blend":
        return bytes(rgba)
    out = bytearray(rgba)
    if kind == "opaque":
        out[3::4] = b"\xff" * (len(out) // 4)
    else:
        out[3::4] = bytes(0 if a == 0 else 255 for a in out[3::4])
    return bytes(out)


class Materials:
    """Resolve once per (scope, texture name); write PNGs; render MTL."""

    def __init__(self, disc, out_dir, paint, alpha="cutout"):
        self.disc, self.dir, self.paint, self.alpha = disc, Path(out_dir), paint, alpha
        self.items, self.pngs = {}, {}

    def get(self, scope, archive, name):
        key = (scope, name.lower())
        if key in self.items:
            return self.items[key]
        stem = Path(name).stem or "none"
        mat = dict(name=(scope + "_" if scope != "car" else "") + stem, texture=name, scope=scope)
        if name.lower() in NO_TEXTURE:
            mat.update(status="no-texture-field")
        else:
            try:
                data = mb.resolve_material(self.disc, archive, name, paint=self.paint if scope == "car" else None,
                                           request=KIT_REQUEST)
            except Exception as error:  # reported, never substituted
                data, mat["error"] = None, f"{type(error).__name__}: {error}"
            if data is None:
                mat.update(status="unresolved", reason=mat.pop("error", "not found in model or resident archives"))
            else:
                try:
                    w, h, rgba, meta = decode_preview(data)
                    png = self.dir / "textures" / (mat["name"] + ".png")
                    png.parent.mkdir(parents=True, exist_ok=True)
                    kind, zero = alpha_treatment(rgba[3::4], self.alpha)
                    write_png(str(png), w, h, apply_alpha(rgba, kind))
                    mat.update(status="resolved", source=locate(self.disc, archive, name, sha(data), self.paint if scope == "car" else None),
                               sourceSha256=sha(data), png=f"textures/{mat['name']}.png", pngSha256=file_sha_nocache(png),
                               width=w, height=h, levels=meta["levels"],
                               zeroAlphaFraction=round(zero, 4), alphaKind=kind)
                    self.pngs[mat["name"].lower()] = (w, h, rgba)
                except Exception as error:
                    mat.update(status="unresolved", reason=f"decode failed: {error}")
        self.items[key] = mat
        return mat

    def mtl(self):
        lines = ["# vehicle_export materials; map_Kd is the native texture as the Creation Kit resolves it"]
        for mat in sorted(self.items.values(), key=lambda m: m["name"]):
            lines += [f"newmtl {mat['name']}", "Kd 1.0 1.0 1.0"]
            if mat["status"] == "resolved":
                lines.append(f"map_Kd {mat['png']}")
                if mat["alphaKind"] in ("cutout", "blend"):
                    lines.append(f"map_d {mat['png']}")
                lines.append(f"# source {mat['source']}")
            elif mat["status"] == "unresolved":
                lines += ["Kd 1.0 0.0 1.0", f"# UNRESOLVED {mat['texture']}: {mat['reason']} (not substituted)"]
            else:
                lines.append("# part has no texture field")
        return "\n".join(lines) + "\n"


def file_sha_nocache(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def wheel_specs(disc, physics, lods, want_all):
    """Front/Rear wheel classes -> RACE.RES wheel models (class .val 'model.base' + N)."""
    race = disc / "DATA/RES/RACE.RES"
    entries = mb.directory(race)
    byname = {e["name"].lower(): e for e in entries}
    sections = {s["section"]: dict(s["pairs"]) for s in physics}
    found = {}
    notes = []
    for axle in ("Front", "Rear"):
        cls = sections.get(axle, {}).get("wheel_class")
        if not cls:
            notes.append(f"{axle}: physics.val has no wheel_class")
            continue
        entry = byname.get(cls.lower() + ".val")
        if entry is None:
            notes.append(f"{axle}: wheel class {cls} has no .val in RACE.RES")
            continue
        table = {s["name"]: dict(s["pairs"]) for s in parse_binary_valt(mb.payload(race, entry["name"], entries)[0])["sections"]}
        base = table.get("model", {}).get("base")
        if not base:
            notes.append(f"{axle}: {cls}.val has no model base")
            continue
        rec = found.setdefault((cls, base), dict(wheelClass=cls, base=base, axles=[],
                                                  dimensions=table.get("dimensions", {}), lod=table.get("lod", {})))
        rec["axles"].append(axle)
    out = []
    for (cls, base), rec in found.items():
        members = sorted((int(m.group(1)), e["name"]) for e in entries
                         if (m := re.fullmatch(re.escape(base) + r"(\d+)\.mod", e["name"], re.I)))
        pick = [(n, name) for n, name in members if want_all or lods is None or n in lods]
        if not pick:
            notes.append(f"{cls}: no {base}N.mod for the selected LODs")
        out.append(dict(rec, models=pick))
    return out, notes


def export_car(disc, car, dest, lod="1", paint=None, blender=False, wheels=True, keep_mesh=False,
               alpha="cutout", double_sided=False):
    """Export one car. Returns the vehicle.json dict (plus '_mesh' when keep_mesh)."""
    disc = disc_root(disc)
    car_path = find_car(disc, car)
    stem = car_path.stem
    out = Path(dest).expanduser().resolve() / stem
    out.mkdir(parents=True, exist_ok=True)
    lods, shadows = parse_lods(lod)
    entries = mb.directory(car_path)
    archive_id = rel(disc, car_path)
    chosen = []
    for e in entries:
        m, s = LOW.fullmatch(e["name"]), SHADOW.fullmatch(e["name"])
        if e["type"] != "MODL":
            continue
        if m and (lods is None or int(m.group(1)) in lods):
            chosen.append((0, int(m.group(1)), e))
        elif s and shadows:
            chosen.append((1, int(s.group(1)), e))
    chosen.sort(key=lambda t: t[:2])
    missing = sorted(n for n in (lods or ()) if not any(k == 0 and i == n for k, i, _ in chosen))
    members = []
    for e in entries:
        data = mb.payload(car_path, e["name"], entries, verify_crc=False)[0]
        members.append(dict(member=e["name"], type=e["type"], size=e["size"], crc=e["crc"],
                            crcOk=mb.eod_crc(data) == e["crc"], sha256=sha(data)))
    raw = {e["name"].lower(): mb.payload(car_path, e["name"], entries)[0] for e in entries
           if e["name"].lower() in ("physics.val", "lod.val", "bone.val")}
    physics = valt_dump(raw["physics.val"]) if "physics.val" in raw else []
    lodval = valt_dump(raw["lod.val"]) if "lod.val" in raw else []
    painted = pick_paint(disc, car_path, paint)
    materials = Materials(disc, out, painted, alpha)
    lod_dist = {}
    for sec in lodval:
        if sec["section"] == "low":
            for k, v in sec["pairs"]:
                if re.fullmatch(r"lod\d+", k):
                    lod_dist[int(k[3:])] = v.split()
    obj = [f"# {archive_id}  exported by vehicle_export.py (schema {SCHEMA})",
           "# +Y up, +Z forward, metres. Vertex order inside each object is the native VIF order: do not add/remove/reorder vertices.",
           f"mtllib {stem}.mtl"]
    parts, vbase, nbase, mesh_parts, bbox_pts = [], 1, 1, [], []
    models = []

    def emit(label, kind, data, scope, mat_archive, assetid, extra):
        nonlocal vbase, nbase
        model = decode_model(data)
        info = dict(label=label, kind=kind, assetId=assetid, size=len(data), sha256=sha(data), markers=model["markers"],
                    bbox=model["bbox"], radius=model["radius"], **extra)
        n_tri = n_v = 0
        for part in model["parts"]:
            if not part["verts"]:
                continue
            mat = materials.get(scope, mat_archive, part["texture"])
            name = "%s__slot%02d_g%d_%s%s" % (label, part["slot"], part["group"] & 0xFFFF, Path(part["texture"]).stem or "none",
                                              "_body" if part["group"] & 0x10000 else "")
            obj.append(f"# assetId={assetid} slot={part['slot']} damageGroup={part['group'] & 0xFFFF} "
                       f"bodyPanel={bool(part['group'] & 0x10000)} groupRaw={part['group']:#x} texture={part['texture']}")
            obj.extend([f"o {name}", f"usemtl {mat['name']}"])
            obj.extend(["v %.7f %.7f %.7f" % v["p"] for v in part["verts"]])
            obj.extend(["vt %.7f %.7f" % (v["uv"][0], 1.0 - v["uv"][1]) for v in part["verts"]])
            obj.extend(["vn %.6f %.6f %.6f" % v["n"] for v in part["verts"]])
            nv = len(part["verts"])
            if double_sided:  # back faces reuse the vertices with flipped normals appended after the native ones
                obj.extend(["vn %.6f %.6f %.6f" % tuple(-x for x in v["n"]) for v in part["verts"]])
            for a, b, c in part["tris"]:
                va, vb, vc = a + vbase, b + vbase, c + vbase
                na, nb, nc = a + nbase, b + nbase, c + nbase
                obj.append(f"f {va}/{va}/{na} {vb}/{vb}/{nb} {vc}/{vc}/{nc}")
                if double_sided:
                    na, nb, nc = na + nv, nb + nv, nc + nv
                    obj.append(f"f {va}/{va}/{na} {vc}/{vc}/{nc} {vb}/{vb}/{nb}")
            nbase += nv * (2 if double_sided else 1)
            parts.append(dict(object=name, assetId=assetid, model=label, kind=kind, slot=part["slot"],
                              damageGroup=part["group"] & 0xFFFF, bodyPanel=bool(part["group"] & 0x10000),
                              groupRaw=part["group"], texture=part["texture"], material=mat["name"],
                              vertices=len(part["verts"]), triangles=len(part["tris"]), batches=part["batches"],
                              mscal=part["mscal"], **extra))
            n_v += len(part["verts"])
            n_tri += len(part["tris"])
            vbase += len(part["verts"])
            bbox_pts.extend(v["p"] for v in part["verts"])
            if keep_mesh:
                mesh_parts.append(dict(slot=part["slot"], texture=part["texture"], group=part["group"], scope=scope,
                                       verts=part["verts"], tris=part["tris"], model=label, kind=kind))
        models.append(dict(info, parts=sum(1 for p in parts if p["model"] == label), vertices=n_v, triangles=n_tri))
        return model

    markers = []
    for kind_i, n, e in chosen:
        stem_name = Path(e["name"]).stem
        data = mb.payload(car_path, e["name"], entries)[0]
        extra = dict(lod=n, lodDistance=lod_dist.get(n)) if kind_i == 0 else dict(shadowIndex=n)
        model = emit(stem_name, "body-lod" if kind_i == 0 else "shadow", data, "car", car_path, f"{archive_id}#{e['name']}", extra)
        if model["markers"] and not markers:
            markers = [dict(name=m["name"], position=list(m["pos"]), rotation=list(m["rot"]), source=f"{archive_id}#{e['name']}")
                       for m in model["markers"]]
    wheel_notes = []
    wheel_out = []
    if wheels:
        race = disc / "DATA/RES/RACE.RES"
        specs, wheel_notes = wheel_specs(disc, physics, lods, lods is None)
        for spec in specs:
            for n, member in spec["models"]:
                data = mb.payload(race, member)[0]
                emit("Wheel_" + Path(member).stem, "wheel", data, "wheel", race, f"{rel(disc, race)}#{member}",
                     dict(wheelClass=spec["wheelClass"], axles=spec["axles"], lod=n))
            wheel_out.append({k: spec[k] for k in ("wheelClass", "base", "axles", "dimensions", "lod")} |
                             dict(models=[m for _, m in spec["models"]]))
    (out / f"{stem}.obj").write_text("\n".join(obj) + "\n")
    (out / f"{stem}.mtl").write_text(materials.mtl())
    mats = sorted(materials.items.values(), key=lambda m: m["name"])
    unresolved = [dict(material=m["name"], texture=m["texture"], reason=m["reason"]) for m in mats if m["status"] == "unresolved"]
    info = dict(
        schemaVersion=SCHEMA, tool="vehicle_export.py", car=stem, archive=archive_id, archiveSha256=file_sha(str(car_path)),
        slusSha256=file_sha(str(disc / "SLUS_209.10")) if (disc / "SLUS_209.10").is_file() else None,
        coordinateSystem="native: +Y up, +Z forward, metres", lodSelection=lod, uvConvention="vt v = 1 - native t",
        paint=None if painted is None else dict(number=painted["number"], driver=painted["driver"], asset=painted["asset"],
                                               archive=rel(disc, painted["archive"]), explicit=paint is not None),
        members=members, physics=physics, lod=lodval, models=models, parts=parts, markers=markers, materials=mats,
        unresolvedMaterials=unresolved, missingLods=missing,
        wheels=dict(placement="origin", note="Wheels are RACE.RES wheel-class meshes at the origin. Native wheel sockets are not verified: "
                    "left/right front/rear markers and physics tracks do not match wheel centres.", classes=wheel_out, notes=wheel_notes),
        totals=dict(models=len(models), parts=len(parts), vertices=sum(p["vertices"] for p in parts),
                    triangles=sum(p["triangles"] for p in parts), materials=len(mats), unresolved=len(unresolved)),
        files=dict(obj=f"{stem}.obj", mtl=f"{stem}.mtl", textures=[m["png"] for m in mats if m["status"] == "resolved"]))
    (out / "vehicle.json").write_text(json.dumps(info, indent=1) + "\n")
    if blender:
        (out / "import_blender.py").write_text(BLENDER.format(stem=stem))
    if keep_mesh:
        pts = bbox_pts or [(0, 0, 0)]
        info["_mesh"] = dict(parts=[p for p in mesh_parts if p["scope"] == "car" and p["kind"] == "body-lod"],
                             textures=materials.pngs,
                             bbox={"min": tuple(min(p[k] for p in pts) for k in range(3)), "max": tuple(max(p[k] for p in pts) for k in range(3))})
    return info


BLENDER = '''"""Blender 3.6+/4.x: run from the Scripting tab. Imports {stem}.obj with native axes and adds marker empties.
Edit mesh positions/UVs only; export with Keep Vertex Order, Triangulate, forward NEGATIVE_Z, up Y, same
object names, then run tools/vehicle_import.py. Untested in Blender by the exporter."""
import json, os
import bpy
here = os.path.dirname(os.path.abspath(__file__))
bpy.ops.wm.obj_import(filepath=os.path.join(here, "{stem}.obj"), forward_axis="NEGATIVE_Z", up_axis="Y")
for m in json.load(open(os.path.join(here, "vehicle.json")))["markers"]:
    x, y, z = m["position"]
    e = bpy.data.objects.new("marker " + m["name"], None)
    e.location = (x, -z, y)  # Y-up/-Z-forward import -> Blender Z-up
    bpy.context.collection.objects.link(e)
'''


def render_preview(info, png, yaw=35.0, pitch=18.0, width=720, height=432, lod_label=None):
    """3/4 flat-textured render of one body LOD (default Low1, else the first) with Kit-resolved textures."""
    mesh = info["_mesh"]
    labels = list(dict.fromkeys(p["model"] for p in mesh["parts"]))
    label = lod_label or next((l for l in labels if l.lower() == "low1"), labels[0])
    parts = [p for p in mesh["parts"] if p["model"] == label]
    pts = [v["p"] for p in parts for v in p["verts"]]
    bbox = dict(min=tuple(min(p[k] for p in pts) for k in range(3)), max=tuple(max(p[k] for p in pts) for k in range(3)))
    vif_mesh.render(dict(parts=parts, bbox=bbox), str(png), {k.lower(): v for k, v in mesh["textures"].items()},
                    width=width, height=height, yaw_deg=yaw, pitch_deg=pitch)


def export_all(disc, dest, **options):
    """Export every retail car; returns exact accounting. Per-car exceptions are recorded, not hidden."""
    disc = disc_root(disc)
    dest = Path(dest).expanduser().resolve()
    cars, failures = [], []
    for car in car_files(disc):
        try:
            info = export_car(disc, car.name, dest, **options)
            cars.append(dict(car=info["car"], archive=info["archive"], archiveSha256=info["archiveSha256"], **info["totals"],
                             paint=info["paint"], unresolvedMaterials=info["unresolvedMaterials"], missingLods=info["missingLods"]))
        except Exception as error:
            failures.append(dict(car=car.name, error=f"{type(error).__name__}: {error}", trace=traceback.format_exc(limit=3)))
    total = {k: sum(c[k] for c in cars) for k in ("models", "parts", "vertices", "triangles", "materials", "unresolved")}
    summary = dict(schemaVersion=SCHEMA, discCars=len(car_files(disc)), exported=len(cars), failed=len(failures),
                   options={k: v for k, v in options.items() if k != "keep_mesh"}, totals=total, cars=cars, failures=failures)
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "export-summary.json").write_text(json.dumps(summary, indent=1) + "\n")
    return summary


def main(argv=None):
    ap = argparse.ArgumentParser(description="Export retail EOD cars to OBJ+MTL+PNG+vehicle.json")
    ap.add_argument("--disc", required=True, help="disc folder containing DATA/")
    ap.add_argument("--car", help="e.g. DATA/CAR/BUS.CAR")
    ap.add_argument("--all", action="store_true", help="export every DATA/CAR/*.CAR with exact accounting")
    ap.add_argument("--dest", required=True)
    ap.add_argument("--lod", default="1", help="'1' (default), '1,2', 'all' (every LowN and shadow), 'shadow'")
    ap.add_argument("--paint", help="CT.RES racing number for the livery (default: first retail driver of the chassis)")
    ap.add_argument("--blender", action="store_true", help="also write import_blender.py")
    ap.add_argument("--no-wheels", action="store_true")
    ap.add_argument("--alpha", choices=ALPHA_MODES, default="cutout",
                    help="cutout (default): map_d + binary-alpha PNG only for textures with >=1%% alpha==0 texels, "
                         "all others opaque; blend: map_d for any palette alpha<255; opaque: never")
    ap.add_argument("--double-sided", action="store_true",
                    help="emit every triangle twice with reversed winding and flipped normals (for back-face-culling "
                         "viewers such as SceneKit); doubles f lines and vn lines; view-only, do not feed to vehicle_import")
    ap.add_argument("--preview", help="write a 3/4 software-render PNG (single car)")
    a = ap.parse_args(argv)
    if bool(a.car) == bool(a.all):
        ap.error("give exactly one of --car or --all")
    opts = dict(lod=a.lod, paint=a.paint, blender=a.blender, wheels=not a.no_wheels, alpha=a.alpha, double_sided=a.double_sided)
    try:
        if a.all:
            if a.paint:
                ap.error("--paint applies to a single --car")
            s = export_all(a.disc, a.dest, **opts)
            print(f"cars {s['exported']}/{s['discCars']} failed {s['failed']} " + " ".join(f"{k} {v}" for k, v in s["totals"].items()))
            for f in s["failures"]:
                print("FAILED", f["car"], f["error"])
            return 1 if s["failed"] else 0
        info = export_car(a.disc, a.car, a.dest, keep_mesh=bool(a.preview), **opts)
        if a.preview:
            render_preview(info, a.preview)
        print(json.dumps(info["totals"]) + (f" unresolved: {[u['texture'] for u in info['unresolvedMaterials']]}" if info["unresolvedMaterials"] else ""))
    except ValueError as error:
        print("error:", error, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
