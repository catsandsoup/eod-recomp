#!/usr/bin/env python3
"""Reimport edited vehicle OBJ/textures as native EOD data.

patch: edited vehicle_export OBJ (same topology) + optional PNG textures of an existing
       retail car -> Kit mod package (eodmod.json, native/<archive>/<member>) via vif_patch.
new:   new named chassis cloned from a retail base (create_custom_car overlay), optionally with
       a body PNG and same-topology geometry edits applied to the clone.
Arbitrary new topology, new wheel/marker layouts and trailers-as-new-cars are refused,
never approximated. See docs/VEHICLE_OBJ_PIPELINE.md.
"""
import argparse
import contextlib
import io
import json
import re
import shutil
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import modding_backend as mb
import mod_package
import vehicle_export as ve
import vif_patch
from rst0 import pack_archive, unpack_archive
from tex import compile_tex, read_png
from texture_preview import decode_preview


class ImportError_(ValueError):
    pass


def split_obj(path):
    """{model prefix: OBJ text} from an exported vehicle OBJ. Object names are '<model>__slotNN_...'."""
    V, VT, VN, objs, cur = [], [], [], {}, None
    for line in Path(path).read_text().splitlines():
        t = line.split()
        if not t:
            continue
        if t[0] in ("o", "g"):
            name = " ".join(t[1:])
            if "__" not in name:
                raise ImportError_(f"OBJ object '{name}' is not a vehicle_export object (expected '<model>__slotNN_...')")
            cur = objs.setdefault(name, dict(v0=len(V), nv=0, faces=[]))
        elif t[0] == "v":
            if cur is None:
                raise ImportError_("OBJ vertex before any object")
            V.append(t[1:4])
            cur["nv"] += 1
        elif t[0] == "vt":
            VT.append(t[1:3])
        elif t[0] == "vn":
            VN.append(t[1:4])
        elif t[0] == "f":
            cur["faces"].append([(c.split("/") + ["", ""])[:3] for c in t[1:]])
    by_model = {}
    for name, o in objs.items():
        by_model.setdefault(name.split("__")[0], []).append((name, o))
    texts = {}
    for model, items in by_model.items():
        body, nv, vtmap, vnmap = [], 0, {}, {}
        for name, o in items:
            body.append("o " + name)
            body += ["v " + " ".join(v) for v in V[o["v0"]:o["v0"] + o["nv"]]]
            for f in o["faces"]:
                corner = []
                for v, vt, vn in f:
                    ti = vtmap.setdefault(int(vt), len(vtmap) + 1) if vt else ""
                    ni = vnmap.setdefault(int(vn), len(vnmap) + 1) if vn else ""
                    corner.append(f"{nv + int(v) - o['v0']}/{ti}/{ni}")
                body.append("f " + " ".join(corner))
            nv += o["nv"]
        vts = ["vt " + " ".join(VT[i - 1]) for i in sorted(vtmap, key=vtmap.get)]
        vns = ["vn " + " ".join(VN[i - 1]) for i in sorted(vnmap, key=vnmap.get)]
        texts[model] = "\n".join(vts + vns + body) + "\n"
    return texts


def patch_models(disc, car_path, obj, clamp=False, channels=("pos", "uv", "nrm")):
    """Patch each OBJ model against the car's retail member. Returns ({member: new bytes}, report)."""
    entries = mb.directory(car_path)
    byname = {e["name"].lower(): e["name"] for e in entries if e["type"] == "MODL"}
    archive = ve.rel(disc, car_path)
    changes, report, ignored = {}, {}, []
    with tempfile.TemporaryDirectory(prefix="eod-vimport-") as tmp:
        for model, text in split_obj(obj).items():
            if model.lower().startswith("wheel_"):
                ignored.append(model)
                continue
            member = byname.get(model.lower() + ".mod")
            if member is None:
                raise ImportError_(f"OBJ model '{model}' is not a model of {archive}")
            data = mb.payload(car_path, member, entries)[0]
            edited = Path(tmp) / (model + ".obj")
            edited.write_text(text)
            mp = vif_patch.export_model(data, str(Path(tmp) / "unused.obj"), member, archive)
            try:
                new, rep = vif_patch.patch_member(data, mp, str(edited), channels, clamp)
            except vif_patch.PatchError as error:
                raise ImportError_(f"{archive}#{member}: {error}") from None
            report[member] = dict(rep, changed=new != data)
            if new != data:
                changes[member] = new
    return changes, dict(models=report, ignoredWheelModels=ignored)


def compile_replacement(original, png):
    """PNG -> TEX with the original member's exact header parameters; size must not change."""
    w, h, lv, fmt, flags, aux = struct.unpack_from("<6I", original)
    pw, ph, rgba = read_png(png)
    if (pw, ph) != (w, h):
        raise ImportError_(f"{Path(png).name} is {pw}x{ph}; native texture is {w}x{h} (resize externally)")
    data = compile_tex(w, h, rgba, fmt, flags, aux)
    if len(data) != len(original) or struct.unpack_from("<6I", data)[:5] != (w, h, lv, fmt, flags):
        raise ImportError_(f"{Path(png).name}: compiled TEX layout differs from the retail member (unsupported shape)")
    decode_preview(data)
    return data


def resolve_texture_target(disc, car_path, key):
    """'Bus1.tex' (member of the car) or 'DATA/RES/CT.RES#790.tm0' -> (archive path, member name)."""
    archive, _, member = key.rpartition("#")
    path = car_path if not archive else mod_package.retail_archive(disc, archive)[0]
    entries = mb.directory(path)
    hits = [e["name"] for e in entries if e["name"].lower() == member.lower() and e["type"] == "TEX"]
    if len(hits) != 1:
        raise ImportError_(f"No TEX member '{member}' in {ve.rel(disc, path)}")
    return path, hits[0]


def build_package(disc, out, package_id, version, name, edits):
    """edits: [(archive path, member, new bytes)] -> verified Kit package directory."""
    out = Path(out).expanduser().resolve()
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        raise ImportError_(f"Package folder already exists and is not empty: {out}")
    manifest = mod_package.new_manifest(disc, package_id, version, name)
    with tempfile.TemporaryDirectory(prefix="eod-vpkg-") as tmp:
        stage = Path(tmp) / "pkg"
        for path, member, data in edits:
            retail, entry, _, spelled = mod_package.retail_member(disc, ve.rel(disc, path), member)
            file = f"native/{spelled}/{entry['name']}"
            (stage / file).parent.mkdir(parents=True, exist_ok=True)
            (stage / file).write_bytes(data)
            manifest["native"].append(dict(archive=spelled, member=entry["name"], sourceArchiveSha256=mod_package.cached_file_sha(path),
                                           sourceMemberSha256=mod_package.sha(retail), file=file, sha256=mod_package.sha(data)))
        mod_package.write_manifest(stage, manifest)
        result = mod_package.verify(stage, disc)
        out.parent.mkdir(parents=True, exist_ok=True)
        if out.exists():
            out.rmdir()
        shutil.move(str(stage), str(out))
    return dict(result, path=str(out))


def import_patch(disc, car, out, package_id, version="1.0.0", name=None, obj=None, textures=(), clamp=False):
    """Same-topology edit of an existing retail car. `textures` = ['member-or-ARCHIVE#member=PNG']."""
    disc = ve.disc_root(disc)
    car_path = ve.find_car(disc, car)
    edits, report = [], {}
    if obj:
        changes, report = patch_models(disc, car_path, obj, clamp)
        edits += [(car_path, member, data) for member, data in changes.items()]
    for spec in textures:
        key, _, png = spec.rpartition("=")
        if not key or not png:
            raise ImportError_(f"--texture needs MEMBER=PNG, got '{spec}'")
        path, member = resolve_texture_target(disc, car_path, key)
        original = mb.payload(path, member)[0]
        edits.append((path, member, compile_replacement(original, png)))
    if not edits:
        raise ImportError_("No edits detected (geometry identical to retail and no textures given); no package written")
    result = build_package(disc, out, package_id, version, name or package_id, edits)
    return dict(result, report=report, changedMembers=[f"{ve.rel(disc, p)}#{m}" for p, m, _ in edits])


def import_new(disc, out, base, name, number="777", car_class="midsize", body_png=None, obj=None, **options):
    """New named chassis: create_custom_car overlay (+ body PNG, + same-topology geometry on the clone)."""
    import create_custom_car as ccc
    disc = ve.disc_root(disc)
    out = Path(out).expanduser().resolve()
    if out.exists() and any(out.iterdir()):
        raise ImportError_(f"Output folder is not empty: {out}")
    classes = dict(compact=0, midsize=1, muscle=2)
    if car_class not in classes:
        raise ImportError_("class must be compact, midsize or muscle")
    patched, notes = [], []
    with tempfile.TemporaryDirectory(prefix="eod-vnew-") as tmp, contextlib.redirect_stdout(io.StringIO()):
        built = Path(tmp) / "overlay"
        ccc.build_custom_car(str(disc / "DATA"), str(built), name=name, base=base, number=str(number),
                             classes=[classes[car_class]], **options)
        if body_png or obj:
            car = built / "DATA/CAR" / ccc.car_archive_filename(name)
            stage = Path(tmp) / "car"
            unpack_archive(str(car), str(stage))
            files = {p.name.lower(): p for p in stage.iterdir()}
            if body_png:
                tm0 = files[name.lower() + ".tm0"]
                tm0.write_bytes(compile_replacement(tm0.read_bytes(), body_png))
                patched.append(tm0.name)
                notes.append("Name.tex (low-resolution) keeps the generated livery; only Name.tm0 uses the PNG")
            if obj:
                base_car = ve.find_car(disc, ccc.car_archive_filename(base))
                for model, text in split_obj(obj).items():
                    if model.lower().startswith("wheel_"):
                        continue
                    target = files.get(model.lower() + ".mod")
                    if target is None:
                        raise ImportError_(f"OBJ model '{model}' is not a model of the new car")
                    data = target.read_bytes()
                    objfile = Path(tmp) / (model + ".obj")
                    objfile.write_text(text)
                    mp = vif_patch.export_model(data, str(Path(tmp) / "unused.obj"), target.name, name)
                    try:
                        new, _ = vif_patch.patch_member(data, mp, str(objfile))
                    except vif_patch.PatchError as error:
                        raise ImportError_(f"{model}: {error}") from None
                    if new != data:
                        target.write_bytes(new)
                        patched.append(target.name)
            pack_archive(str(stage), str(car))
        out.mkdir(parents=True, exist_ok=True)
        shutil.copytree(built, out, dirs_exist_ok=True)
    return dict(package=str(out), kind="overlay (EOD_MOD_DIR), not an eodmod.json package", patchedMembers=patched, notes=notes)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("patch", help="same-topology edit of a retail car -> Kit mod package")
    p.add_argument("--disc", required=True)
    p.add_argument("--car", required=True)
    p.add_argument("--obj", help="edited OBJ from vehicle_export")
    p.add_argument("--texture", action="append", default=[], help="MEMBER=PNG or DATA/RES/CT.RES#790.tm0=PNG")
    p.add_argument("--out", required=True)
    p.add_argument("--id", required=True)
    p.add_argument("--version", default="1.0.0")
    p.add_argument("--name")
    p.add_argument("--clamp", action="store_true")
    n = sub.add_parser("new", help="new named chassis from a retail base (overlay)")
    n.add_argument("--disc", required=True)
    n.add_argument("--out", required=True)
    n.add_argument("--base", required=True)
    n.add_argument("--name", required=True)
    n.add_argument("--number", default="777")
    n.add_argument("--class", dest="car_class", default="midsize")
    n.add_argument("--body-png")
    n.add_argument("--obj")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "patch":
            r = import_patch(a.disc, a.car, a.out, a.id, a.version, a.name, a.obj, a.texture, a.clamp)
        else:
            r = import_new(a.disc, a.out, a.base, a.name, a.number, a.car_class, a.body_png, a.obj)
    except (ValueError, FileNotFoundError) as error:
        print("error:", error, file=sys.stderr)
        return 2
    print(json.dumps({k: v for k, v in r.items() if k != "report"}, indent=1, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
