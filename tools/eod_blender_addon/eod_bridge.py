"""Pure-Python EOD <-> Blender 4.2+ geometry, track, and vehicle bridge.

Zero ``bpy`` dependency so CI, unittest, and World Studio can import directly.
Coordinate convention matches ``project_state.export_object_obj`` 100%:
  - EOD LH Y-up ``(x, y, z)`` <-> Blender RH Z-up ``(x, z, y)`` (self-inverse)
  - Triangle winding reverses ``(a, b, c) <-> (a, c, b)`` across LH <-> RH
  - UV V flips ``v <-> 1.0 - v`` between DirectX/PS2 top-left and Blender bottom-left
"""

from __future__ import annotations

import hashlib
import json
import math
import re
import sys
from pathlib import Path
from typing import Any, Sequence

import numpy as np

_THIS_DIR = Path(__file__).resolve().parent
_TOOLS_DIR = _THIS_DIR.parent
_WORLD_EDITOR_DIR = _TOOLS_DIR / "world_editor"
for _p in (_TOOLS_DIR, _WORLD_EDITOR_DIR):
    if _p.is_dir() and str(_p) not in sys.path:
        sys.path.insert(0, str(_p))

from world_loader import MeshPart, WorldModel  # noqa: E402


# ---------------------------------------------------------------------------
# Coordinate-space helpers (100% inverse of project_state.export_object_obj)
# ---------------------------------------------------------------------------
def eod_lh_to_blender_rh(xyz: Sequence[float]) -> tuple[float, float, float]:
    """Convert EOD left-handed Y-up (x, y, z) to Blender right-handed Z-up (x, z, y)."""
    return (float(xyz[0]), float(xyz[2]), float(xyz[1]))


def blender_rh_to_eod_lh(xyz: Sequence[float]) -> tuple[float, float, float]:
    """Convert Blender right-handed Z-up (x, y, z) back to EOD left-handed Y-up (x, z, y)."""
    return (float(xyz[0]), float(xyz[2]), float(xyz[1]))


def _resolve_mtl_tex_name(mat_name: str, map_kd: str | None, default_tex: str) -> str:
    raw = (map_kd or "").strip()
    if raw:
        p = Path(raw.replace("\\", "/"))
        if p.suffix.lower() in (".tex", ".tm0"):
            return p.name
        if p.stem:
            return f"{p.stem}.tex"
    clean = mat_name.strip()
    if not clean or clean.lower() in ("untextured", "default", "none"):
        return default_tex
    return clean if clean.lower().endswith((".tex", ".tm0")) else f"{clean}.tex"


def parse_mtl_textures(mtl_path: Path) -> tuple[dict[str, str], dict[str, Path]]:
    """Parse Wavefront .mtl material -> texture filename and optional image file path."""
    mat_to_tex: dict[str, str] = {}
    mat_to_img: dict[str, Path] = {}
    if not mtl_path.is_file():
        return mat_to_tex, mat_to_img
    cur_mat: str | None = None
    for raw in mtl_path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) < 2:
            continue
        key, val = parts[0].lower(), parts[1].strip()
        if key == "newmtl":
            cur_mat = val
        elif key == "map_kd" and cur_mat is not None:
            # Skip optional Wavefront texture options (-s, -o, -bm, etc.)
            tokens = val.split()
            tex_ref = tokens[-1]
            for idx, tok in enumerate(tokens):
                if not tok.startswith("-") and (idx == 0 or not tokens[idx - 1].startswith("-")):
                    tex_ref = " ".join(tokens[idx:])
                    break
            mat_to_tex[cur_mat] = tex_ref
            img_p = (mtl_path.parent / tex_ref.replace("\\", "/")).resolve()
            if img_p.is_file():
                mat_to_img[cur_mat] = img_p
    return mat_to_tex, mat_to_img


def _resolve_obj_index(idx_str: str, count: int) -> int:
    if not idx_str:
        return -1
    idx = int(idx_str)
    return (count + idx) if idx < 0 else (idx - 1)


def _compute_smooth_normals(pos: np.ndarray, indices: np.ndarray) -> np.ndarray:
    """Compute area-weighted unit vertex normals in EOD LH space."""
    norms = np.zeros_like(pos, dtype=np.float32)
    tris = indices.reshape(-1, 3)
    v0 = pos[tris[:, 0]]
    v1 = pos[tris[:, 1]]
    v2 = pos[tris[:, 2]]
    fn = np.cross(v1 - v0, v2 - v0)
    np.add.at(norms, tris[:, 0], fn)
    np.add.at(norms, tris[:, 1], fn)
    np.add.at(norms, tris[:, 2], fn)
    lens = np.linalg.norm(norms, axis=1, keepdims=True)
    valid = lens[:, 0] > 1e-8
    norms[valid] /= lens[valid]
    norms[~valid] = np.array([0.0, 1.0, 0.0], dtype=np.float32)
    return norms


def _parse_obj_internal(
    obj_path: Path, default_tex: str = "rd-1.tex"
) -> tuple[list[MeshPart], dict[str, Path]]:
    obj_p = Path(obj_path)
    positions: list[tuple[float, float, float]] = []
    uvs: list[tuple[float, float]] = []
    normals: list[tuple[float, float, float]] = []
    mat_to_raw: dict[str, str] = {}
    mat_to_img: dict[str, Path] = {}

    buckets: dict[str, list[tuple[tuple[int, int, int], tuple[int, int, int], tuple[int, int, int]]]] = {}
    cur_mat = "untextured"

    for raw in obj_p.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("mtllib "):
            mtl_rel = line[7:].strip()
            m_tex, m_img = parse_mtl_textures(obj_p.parent / mtl_rel)
            mat_to_raw.update(m_tex)
            mat_to_img.update(m_img)
        elif line.startswith("usemtl "):
            cur_mat = line[7:].strip() or "untextured"
        elif line.startswith("v "):
            toks = line.split()
            if len(toks) >= 4:
                positions.append(blender_rh_to_eod_lh((float(toks[1]), float(toks[2]), float(toks[3]))))
        elif line.startswith("vt "):
            toks = line.split()
            if len(toks) >= 3:
                uvs.append((float(toks[1]), 1.0 - float(toks[2])))
        elif line.startswith("vn "):
            toks = line.split()
            if len(toks) >= 4:
                normals.append(blender_rh_to_eod_lh((float(toks[1]), float(toks[2]), float(toks[3]))))
        elif line.startswith("f "):
            toks = line.split()[1:]
            if len(toks) < 3:
                continue
            face: list[tuple[int, int, int]] = []
            for tok in toks:
                seg = tok.split("/")
                vi = _resolve_obj_index(seg[0], len(positions))
                vti = _resolve_obj_index(seg[1], len(uvs)) if len(seg) > 1 else -1
                vni = _resolve_obj_index(seg[2], len(normals)) if len(seg) > 2 else -1
                if 0 <= vi < len(positions):
                    face.append((vi, vti, vni))
            if len(face) < 3:
                continue
            tri_list = buckets.setdefault(cur_mat, [])
            # Fan triangulation + LH winding reversal (0, i+1, i)
            for i in range(1, len(face) - 1):
                tri_list.append((face[0], face[i + 1], face[i]))

    parts: list[MeshPart] = []
    tex_png_map: dict[str, Path] = {}
    for mat_name, tris in buckets.items():
        if not tris:
            continue
        tex_name = _resolve_mtl_tex_name(mat_name, mat_to_raw.get(mat_name), default_tex)
        if mat_name in mat_to_img:
            tex_png_map[tex_name] = mat_to_img[mat_name]

        vert_map: dict[tuple[int, int, int], int] = {}
        verts_out: list[list[float]] = []
        idx_out: list[int] = []
        has_all_normals = True

        for tri in tris:
            for key in tri:
                vid = vert_map.get(key)
                if vid is None:
                    vi, vti, vni = key
                    px, py, pz = positions[vi]
                    if 0 <= vni < len(normals):
                        nx, ny, nz = normals[vni]
                    else:
                        nx, ny, nz = 0.0, 1.0, 0.0
                        has_all_normals = False
                    u, v = uvs[vti] if 0 <= vti < len(uvs) else (0.0, 0.0)
                    vid = len(verts_out)
                    vert_map[key] = vid
                    verts_out.append([px, py, pz, nx, ny, nz, u, v])
                idx_out.append(vid)

        v_arr = np.asarray(verts_out, dtype=np.float32)
        i_arr = np.asarray(idx_out, dtype=np.uint32)
        if not has_all_normals and len(i_arr) >= 3:
            v_arr[:, 3:6] = _compute_smooth_normals(v_arr[:, 0:3], i_arr)
        parts.append(MeshPart(texture=tex_name, vertices=v_arr, indices=i_arr, tri_count=len(i_arr) // 3))

    return parts, tex_png_map


def parse_obj_to_mesh_parts(obj_path: Path | str, default_tex: str = "rd-1.tex") -> list[MeshPart]:
    """Parse a Blender RH Z-up Wavefront .obj/.mtl into EOD LH Y-up MeshParts."""
    parts, _ = _parse_obj_internal(Path(obj_path), default_tex=default_tex)
    return parts


def build_world_model_from_obj(
    obj_path: Path | str,
    default_tex: str = "rd-1.tex",
    key_prefix: str = "procedural:blender",
) -> tuple[WorldModel, tuple[float, float, float], dict[str, Path]]:
    """Parse an OBJ into a pivot-centered WorldModel ready for World Studio & .TRK compile."""
    obj_p = Path(obj_path)
    parts, tex_png_map = _parse_obj_internal(obj_p, default_tex=default_tex)
    if not parts:
        raise ValueError(f"OBJ contains no valid triangle geometry: {obj_p}")

    all_pts = np.vstack([p.vertices[:, 0:3] for p in parts])
    lo_w = all_pts.min(axis=0)
    hi_w = all_pts.max(axis=0)
    pivot_arr = np.array(
        [(lo_w[0] + hi_w[0]) * 0.5, lo_w[1], (lo_w[2] + hi_w[2]) * 0.5], dtype=np.float32
    )

    centered_parts: list[MeshPart] = []
    h = hashlib.sha1()
    for p in parts:
        v = p.vertices.copy()
        v[:, 0:3] -= pivot_arr
        h.update(v.tobytes())
        h.update(p.indices.tobytes())
        centered_parts.append(MeshPart(texture=p.texture, vertices=v, indices=p.indices, tri_count=p.tri_count))

    stem = re.sub(r"[^A-Za-z0-9_.-]+", "_", obj_p.stem).strip("_") or "blender_mesh"
    hash_hex = h.hexdigest()[:12]
    model = WorldModel(
        key=f"{key_prefix}:{stem}_{hash_hex[:6]}",
        name=stem,
        category="prop",
        model_store="procedural:blender_obj",
        hash_hex=hash_hex,
        parts=centered_parts,
        tri_count=sum(p.tri_count for p in centered_parts),
        textures=sorted({p.texture for p in centered_parts if p.texture}),
        bbox_min=(lo_w - pivot_arr).astype(np.float32),
        bbox_max=(hi_w - pivot_arr).astype(np.float32),
        pivot=(float(pivot_arr[0]), float(pivot_arr[1]), float(pivot_arr[2])),
        subtype="Blender Mesh",
    )
    return model, model.pivot, tex_png_map


# ---------------------------------------------------------------------------
# Track (.TRK / .eodworld) & Vehicle (.CAR) structured scene payloads
# ---------------------------------------------------------------------------
def _infer_game_dir(asset_path: Path, explicit: Path | None) -> Path | None:
    if explicit is not None and Path(explicit).is_dir():
        return Path(explicit).resolve()
    p = asset_path.resolve()
    for parent in p.parents:
        if (parent / "DATA").is_dir():
            return parent
    from world_loader import default_game_dir

    gd = default_game_dir()
    return gd if gd.is_dir() else None


def _parts_to_blender_payload(parts: list[dict[str, Any]]) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for part in parts:
        verts = part.get("verts") or []
        tris = part.get("tris") or []
        if not verts or not tris:
            continue
        b_verts = [eod_lh_to_blender_rh(v["p"]) for v in verts]
        b_uvs = [(float(v["uv"][0]), 1.0 - float(v["uv"][1])) for v in verts]
        b_tris = [(int(a), int(c), int(b)) for a, b, c in tris]
        out.append({"texture": str(part.get("texture") or ""), "verts": b_verts, "uvs": b_uvs, "tris": b_tris})
    return out


def load_trk_scene_payload(trk_path: Path | str, game_dir: Path | None = None) -> dict[str, Any]:
    """Load an EOD .TRK or .eodworld project into a Blender RH Z-up scene dictionary."""
    src = Path(trk_path).resolve()
    overlay_data: dict[str, Any] | None = None
    target_trk = src
    if src.suffix.lower() == ".eodworld":
        overlay_data = json.loads(src.read_text(encoding="utf-8"))
        trk_name = str(overlay_data.get("track_name") or "").upper()
        gd = _infer_game_dir(src, game_dir)
        if gd is not None and trk_name:
            cand = gd / "DATA" / "TRACKS" / f"{trk_name}.TRK"
            if cand.is_file():
                target_trk = cand
    else:
        gd = _infer_game_dir(src, game_dir)

    from world_loader import load_track_world

    world = load_track_world(gd or target_trk.parent, target_trk.stem.upper(), trk_path=target_trk)
    if overlay_data is not None:
        try:
            from project_state import apply_project_overlay

            apply_project_overlay(world, overlay_data)
        except Exception:
            pass

    models_out: dict[str, Any] = {}
    for mkey, wm in world.models.items():
        mparts = []
        for p in wm.parts:
            b_verts = [(float(v[0]), float(v[2]), float(v[1])) for v in p.vertices]
            b_uvs = [(float(v[6]), 1.0 - float(v[7])) for v in p.vertices]
            tris = p.indices.reshape(-1, 3)
            b_tris = [(int(a), int(c), int(b)) for a, b, c in tris]
            mparts.append({"texture": p.texture, "verts": b_verts, "uvs": b_uvs, "tris": b_tris})
        models_out[mkey] = {"name": wm.name, "category": wm.category, "parts": mparts}

    objects_out = [
        {
            "obj_id": o.obj_id,
            "name": o.name,
            "model_key": o.model_key,
            "category": o.category,
            "position_blender": eod_lh_to_blender_rh(o.position),
            "rotation_hpr_deg": tuple(float(x) for x in o.rotation_hpr),
            "scale_blender": (float(o.scale[0]), float(o.scale[2]), float(o.scale[1])),
        }
        for o in world.objects
        if not o.deleted and o.visible
    ]
    textures_out = {
        name: {"width": t.width, "height": t.height, "alpha_kind": t.alpha_kind, "rgba": t.rgba}
        for name, t in world.textures.items()
    }
    return {
        "track_name": world.track_name,
        "source_path": str(src),
        "coordinate_space": "blender_rh_zup",
        "models": models_out,
        "objects": objects_out,
        "textures": textures_out,
    }


def load_car_assembly_payload(car_path: Path | str, game_dir: Path | None = None) -> dict[str, Any]:
    """Load an EOD .CAR archive with body LOD, RACE.RES wheel geometry/offsets & Bone.mvl bones."""
    import modding_backend as mb
    import vehicle_export as veh_exp
    from texture_preview import decode_preview
    from valt import parse_binary_valt

    cp = Path(car_path).resolve()
    gd = _infer_game_dir(cp, game_dir)
    entries = mb.directory(cp)
    by_low = {e["name"].lower(): e for e in entries}

    # 1. Body LOD (highest detail first)
    mod_entries = {n: e for n, e in by_low.items() if e["type"] == "MODL"}
    pick = (
        mod_entries.get("low1.mod")
        or mod_entries.get("low2.mod")
        or mod_entries.get("low3.mod")
        or next((e for n, e in mod_entries.items() if not n.startswith("shadow")), None)
    )
    if pick is None:
        raise ValueError(f"No MODL mesh found in {cp}")
    body_raw, _ = mb.payload(cp, pick["name"], entries, verify_crc=False)
    body_dec = veh_exp.decode_model(body_raw)
    body_parts = _parts_to_blender_payload(body_dec["parts"])

    # 2. Wheel offsets from physics.val (matching tools/nascar_preview.py wheel_centers)
    wheels: list[dict[str, Any]] = []
    wheel_model_name = ""
    wheel_parts: list[dict[str, Any]] = []
    phys_e = by_low.get("physics.val")
    if phys_e is not None:
        phys_raw, _ = mb.payload(cp, phys_e["name"], entries, verify_crc=False)
        phys = {s["name"]: dict(s["pairs"]) for s in parse_binary_valt(phys_raw)["sections"]}
        chassis = phys.get("Chassis") or {}
        if "wheelbase" in chassis and "ftrack" in chassis and "rtrack" in chassis:
            wb = float(chassis["wheelbase"]) * 0.0254 * 0.5
            for axle, sign, key, tags in (
                ("Front", 1.0, "ftrack", ("FL", "FR")),
                ("Rear", -1.0, "rtrack", ("RL", "RR")),
            ):
                sec = phys.get(axle) or {}
                hx = float(chassis[key]) * 0.0254 * 0.5
                radius = float(sec.get("tire_diameter", 26.0)) * 0.0254 * 0.5
                z = sign * wb
                for side_sign, tag in ((-1.0, tags[0]), (1.0, tags[1])):
                    eod_pos = (side_sign * hx, radius, z)
                    wheels.append(
                        {
                            "name": f"Wheel_{tag}",
                            "axle": axle,
                            "side": "left" if side_sign < 0 else "right",
                            "radius_m": radius,
                            "position_eod": eod_pos,
                            "position_blender": eod_lh_to_blender_rh(eod_pos),
                            "flip_side": side_sign > 0,
                        }
                    )
            wheel_model_name = str((phys.get("Front") or {}).get("model", "")).strip()

    if wheel_model_name and gd is not None:
        race_res = gd / "DATA" / "RES" / "RACE.RES"
        if race_res.is_file():
            r_ents = mb.directory(race_res)
            w_hit = next(
                (e for e in r_ents if e["name"].lower() in (f"{wheel_model_name.lower()}1.mod", f"{wheel_model_name.lower()}.mod")),
                None,
            )
            if w_hit is not None:
                w_raw, _ = mb.payload(race_res, w_hit["name"], r_ents, verify_crc=False)
                wheel_parts = _parts_to_blender_payload(veh_exp.decode_model(w_raw)["parts"])

    # 3. Damage bones from Bone.mvl (matching tools/dent_preview.py)
    bones: list[dict[str, Any]] = []
    bonesurfs: list[dict[str, Any]] = []
    bone_e = by_low.get("bone.mvl")
    if bone_e is not None:
        bone_raw, _ = mb.payload(cp, bone_e["name"], entries, verify_crc=False)
        sections = {s["name"]: dict(s["pairs"]) for s in parse_binary_valt(bone_raw)["sections"]}
        for k, v in sections.items():
            if k.startswith("bone_") and "link_center" in v:
                idx = int(k[5:])
                eod_xyz = tuple(float(c) / 39.37 for c in v["link_center"].split(","))
                bones.append(
                    {
                        "index": idx,
                        "name": k,
                        "position_eod": eod_xyz,
                        "position_blender": eod_lh_to_blender_rh(eod_xyz),
                    }
                )
            elif k.startswith("bonesurf") and "first_bone" in v:
                bonesurfs.append(
                    {
                        "name": k,
                        "first_bone": int(v["first_bone"]),
                        "last_bone": int(v["last_bone"]),
                        "group_mask": int(v["group_mask"], 16),
                    }
                )
        bones.sort(key=lambda b: b["index"])

    # 4. Resolve textures referenced by body + wheel parts
    textures_out: dict[str, Any] = {}
    paint = None
    if gd is not None:
        try:
            paint = mb.vehicle_paint(gd, cp, include_embedded=True)
        except Exception:
            paint = None
    for part in body_parts + wheel_parts:
        tname = part["texture"]
        if not tname or tname in textures_out or gd is None:
            continue
        try:
            tdata = mb.resolve_material(gd, cp, tname, paint=paint, request=veh_exp.KIT_REQUEST)
            if tdata:
                w, h, rgba = decode_preview(tdata)
                textures_out[tname] = {"width": w, "height": h, "rgba": rgba}
        except Exception:
            pass

    return {
        "car_name": cp.stem.upper(),
        "source_path": str(cp),
        "lod_name": pick["name"],
        "coordinate_space": "blender_rh_zup",
        "body_parts": body_parts,
        "wheel_model": wheel_model_name,
        "wheel_parts": wheel_parts,
        "wheels": wheels,
        "bones": bones,
        "bonesurfs": bonesurfs,
        "textures": textures_out,
    }
