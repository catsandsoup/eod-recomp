"""Car Studio 3D Assembled Showroom, Bone.mvl Damage Deformation & Texture/HOSTTEX Studio.

Provides a headless-safe 3D car assembly engine (MODL chassis + positioned 4-wheel
suspension adapter + Bone.mvl crumple deformation + CT.RES livery discovery +
2x-supersampled software studio renderer + PNG/EODHTX1 texture pipeline) paired
with an interactive dark-theme PySide6 turntable panel.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image, ImageFilter
from PySide6.QtCore import QPoint, QPointF, QRectF, Qt, Signal
from PySide6.QtGui import (
    QBrush,
    QColor,
    QImage,
    QLinearGradient,
    QMouseEvent,
    QPainter,
    QPen,
    QPixmap,
    QPolygonF,
    QRadialGradient,
)
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QFileDialog,
    QFrame,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPushButton,
    QScrollArea,
    QSlider,
    QSplitter,
    QVBoxLayout,
    QWidget,
)

import modding_backend as mb
import project_state
import tex as tex_codec
import valt
import vif_mesh
from world_loader import MeshPart, TextureAsset, discover_cars

# ---------------------------------------------------------------------------
# Constants & Class / Wheel Mappings
# ---------------------------------------------------------------------------

_INCH_TO_M = 1.0 / 39.37

_WHEEL_CLASS_MOD: dict[str, str] = {
    "com": "compact1.mod",
    "mid": "mid1.mod",
    "mus": "muscle1.mod",
    "bus": "busfrnt1.mod",
    "busrear": "busrear1.mod",
}

_FALLBACK_CAR_CLASSES: dict[str, str] = {
    "algar": "Compact", "blitz": "Compact", "bulldog": "Compact", "carrere": "Compact",
    "desio": "Compact", "digit": "Compact", "hurricane": "Compact", "moth": "Compact",
    "palo": "Compact", "top": "Compact",
    "arrow": "Midsize", "cannon": "Midsize", "empire": "Midsize", "guerilla": "Midsize",
    "hunter": "Midsize", "limo": "Midsize", "ocelot": "Midsize", "prince": "Midsize",
    "coyote": "Muscle", "durand": "Muscle", "fillmore": "Muscle", "gladiator": "Muscle",
    "gladia": "Muscle", "mongoose": "Muscle", "orbital": "Muscle", "outlaw": "Muscle",
    "rocket": "Muscle", "venom": "Muscle",
    "ambulance": "Special", "ambula": "Special", "bus": "Special", "clown": "Special",
    "copcar": "Special", "hearse": "Special", "postal": "Special", "taxi": "Special",
    "tow": "Special",
}


# ---------------------------------------------------------------------------
# Data Model
# ---------------------------------------------------------------------------

@dataclass
class CarShowroomAssembly:
    car_name: str
    archive_path: Path
    lod_name: str
    livery_names: list[str]
    active_livery: str
    chassis_parts: list[MeshPart]
    wheel_parts: list[MeshPart]
    textures: dict[str, TextureAsset]
    physics_fields: dict[str, Any]
    bones: dict[int, tuple[float, float, float]]
    car_class: str = "Special"
    available_lods: list[str] = field(default_factory=lambda: ["Low0"])
    body_texture_key: str = ""
    livery_sources: dict[str, tuple[Path, str]] = field(default_factory=dict)


# ---------------------------------------------------------------------------
# Pure Parsing, Assembly & Deformation Engine (Headless-Safe)
# ---------------------------------------------------------------------------

def _normalize_car_key(stem: str) -> str:
    s = stem.lower().strip()
    if s.endswith(".car"):
        s = s[:-4]
    if s.endswith("_1"):
        s = s[:-2]
    return s


def _safe_float(val: Any, default: float) -> float:
    try:
        token = str(val).split(",")[0].strip().split()[0]
        v = float(token)
        return v if math.isfinite(v) else default
    except (TypeError, ValueError, IndexError):
        return default


def _parse_vec3(val: str) -> tuple[float, float, float] | None:
    parts = [p.strip() for p in str(val).replace("\t", " ").split(",") if p.strip()]
    if len(parts) < 3:
        parts = [p.strip() for p in str(val).split() if p.strip()]
    if len(parts) < 3:
        return None
    try:
        x, y, z = float(parts[0]), float(parts[1]), float(parts[2])
        if all(math.isfinite(v) for v in (x, y, z)):
            return (x, y, z)
    except ValueError:
        pass
    return None


def _decode_modl_bytes(raw: bytes) -> list[MeshPart]:
    """Decode raw MODL/VIF binary bytes into interleaved MeshPart instances."""
    parsed = vif_mesh.parse_modl(raw)
    out: list[MeshPart] = []
    for part in parsed.get("parts", []):
        batches = vif_mesh.decode_part(raw, part)
        verts: list[list[float]] = []
        indices: list[int] = []
        for bt in batches:
            base = len(verts)
            bverts = bt.get("verts", [])
            for a, b, c in vif_mesh.triangulate_batch(bt):
                pa, pb, pc = bverts[a]["p"], bverts[b]["p"], bverts[c]["p"]
                if pa == pb or pb == pc or pa == pc:
                    continue
                indices.extend((a + base, b + base, c + base))
            for v in bverts:
                px, py, pz = v["p"]
                nx, ny, nz = v.get("n", (0.0, 1.0, 0.0))
                u, tv = v.get("uv", (0.0, 0.0))
                verts.append([px, py, pz, nx, ny, nz, u, tv])
        if not verts or not indices:
            continue
        v_arr = np.asarray(verts, dtype=np.float32)
        i_arr = np.asarray(indices, dtype=np.uint32)
        out.append(
            MeshPart(
                texture=str(part.get("texture") or ""),
                vertices=v_arr,
                indices=i_arr,
                tri_count=len(indices) // 3,
            )
        )
    return out


def _make_texture_asset(
    name: str,
    width: int,
    height: int,
    rgba: bytes,
    raw_tex_bytes: bytes | None = None,
    asset_id: str = "",
) -> TextureAsset:
    arr = np.frombuffer(rgba, dtype=np.uint8).reshape((height, width, 4))
    alpha = arr[:, :, 3]
    if int(alpha.min()) >= 250:
        alpha_kind = "opaque"
    elif np.any((alpha > 10) & (alpha < 245)):
        alpha_kind = "blend"
    else:
        alpha_kind = "cutout"
    rgb_mean = arr[:, :, :3].mean(axis=(0, 1))
    avg_rgb = (int(round(rgb_mean[0])), int(round(rgb_mean[1])), int(round(rgb_mean[2])))
    return TextureAsset(
        name=name,
        width=width,
        height=height,
        rgba=rgba,
        alpha_kind=alpha_kind,
        average_rgb=avg_rgb,
        asset_id=asset_id or name.lower(),
        raw_tex_bytes=raw_tex_bytes,
    )


def _decode_tex_asset(name: str, raw: bytes, asset_id: str = "") -> TextureAsset | None:
    try:
        w, h, rgba, _info = tex_codec.decompile_tex(raw)
        if w > 0 and h > 0 and len(rgba) == w * h * 4:
            return _make_texture_asset(name, w, h, rgba, raw_tex_bytes=raw, asset_id=asset_id or name.lower())
    except Exception:
        return None
    return None


def _parse_bones(raw: bytes) -> dict[int, tuple[float, float, float]]:
    """Parse Bone.mvl / Bone.val VALT binary into bone_id -> (x, y, z) meters."""
    bones: dict[int, tuple[float, float, float]] = {}
    parsed = valt.parse_binary_valt(raw)
    next_id = 1
    for sec in parsed.get("sections", []):
        sname = str(sec.get("name", "")).lower()
        if not sname.startswith("bone_"):
            continue
        pairs = dict(sec.get("pairs", []))
        center_str = pairs.get("link_center")
        if not center_str:
            continue
        vec = _parse_vec3(center_str)
        if vec is None:
            continue
        try:
            bone_id = int(sname.split("_", 1)[1])
        except ValueError:
            bone_id = next_id
        next_id = max(next_id, bone_id + 1)
        bones[bone_id] = (vec[0] * _INCH_TO_M, vec[1] * _INCH_TO_M, vec[2] * _INCH_TO_M)
    return bones


def _parse_physics_sections(raw: bytes) -> dict[str, dict[str, str]]:
    parsed = valt.parse_binary_valt(raw)
    out: dict[str, dict[str, str]] = {}
    for sec in parsed.get("sections", []):
        name = str(sec.get("name", ""))
        if not name or name.startswith("$"):
            continue
        out[name] = {str(k): str(v) for k, v in sec.get("pairs", [])}
    return out


def _procedural_wheel_parts(radius: float, width: float = 0.24, segments: int = 18) -> list[MeshPart]:
    """Generate a clean procedural tire + metallic rim wheel centered at origin."""
    verts: list[list[float]] = []
    indices: list[int] = []
    half_w = width * 0.5
    angles = [2.0 * math.pi * i / segments for i in range(segments)]

    # Tread cylinder
    for i in range(segments):
        a0, a1 = angles[i], angles[(i + 1) % segments]
        cy0, cz0 = math.cos(a0), math.sin(a0)
        cy1, cz1 = math.cos(a1), math.sin(a1)
        b = len(verts)
        verts.extend([
            [-half_w, cy0 * radius, cz0 * radius, 0.0, cy0, cz0, i / segments, 0.0],
            [half_w, cy0 * radius, cz0 * radius, 0.0, cy0, cz0, i / segments, 1.0],
            [half_w, cy1 * radius, cz1 * radius, 0.0, cy1, cz1, (i + 1) / segments, 1.0],
            [-half_w, cy1 * radius, cz1 * radius, 0.0, cy1, cz1, (i + 1) / segments, 0.0],
        ])
        indices.extend([b, b + 1, b + 2, b, b + 2, b + 3])

    # Outer and inner sidewalls
    for side_x in (-half_w, half_w):
        nx = -1.0 if side_x < 0 else 1.0
        center_idx = len(verts)
        verts.append([side_x, 0.0, 0.0, nx, 0.0, 0.0, 0.5, 0.5])
        ring_base = len(verts)
        for a in angles:
            cy, cz = math.cos(a), math.sin(a)
            verts.append([side_x, cy * radius, cz * radius, nx, 0.0, 0.0, 0.5 + 0.45 * cy, 0.5 + 0.45 * cz])
        for i in range(segments):
            j = (i + 1) % segments
            if side_x > 0:
                indices.extend([center_idx, ring_base + i, ring_base + j])
            else:
                indices.extend([center_idx, ring_base + j, ring_base + i])

    return [
        MeshPart(
            texture="__procedural_wheel__",
            vertices=np.asarray(verts, dtype=np.float32),
            indices=np.asarray(indices, dtype=np.uint32),
            tri_count=len(indices) // 3,
        )
    ]


def _position_wheel_parts(
    base_parts: list[MeshPart],
    pos: tuple[float, float, float],
    target_radius: float,
) -> list[MeshPart]:
    """Place a wheel mesh at pos=(x, y, z), mirroring opposite side & fitting tire radius."""
    px, py, pz = pos
    side = -1.0 if px > 0.0 else 1.0
    # Compute source wheel radius on YZ to match tire_diameter cleanly if needed
    all_yz = np.concatenate([p.vertices[:, 1:3] for p in base_parts], axis=0)
    src_r = float(np.sqrt((all_yz ** 2).sum(axis=1)).max()) if len(all_yz) else target_radius
    scale = (target_radius / src_r) if (src_r > 1e-4 and abs(target_radius - src_r) / target_radius > 0.12) else 1.0

    placed: list[MeshPart] = []
    for part in base_parts:
        v = part.vertices.copy()
        v[:, 0] = side * v[:, 0] * scale + px
        v[:, 1] = v[:, 1] * scale + py
        v[:, 2] = side * v[:, 2] * scale + pz
        v[:, 3] *= side
        v[:, 5] *= side
        placed.append(
            MeshPart(
                texture=part.texture,
                vertices=v,
                indices=part.indices.copy(),
                tri_count=part.tri_count,
            )
        )
    return placed


def _discover_car_classes(game_dir: Path) -> dict[str, str]:
    """Load chassis -> class mapping from COMMON.RES/carclass.val with built-in fallback."""
    mapping = dict(_FALLBACK_CAR_CLASSES)
    common = game_dir / "DATA" / "RES" / "COMMON.RES"
    if not common.is_file():
        return mapping
    try:
        entries = mb.directory(common)
        hit = next((e["name"] for e in entries if e["name"].lower() == "carclass.val"), None)
        if not hit:
            return mapping
        raw, _ = mb.payload(common, hit, entries, verify_crc=False)
        parsed = valt.parse_binary_valt(raw)
        secs = {s["name"]: dict(s["pairs"]) for s in parsed.get("sections", [])}
        for cls_name in ("Compact", "Midsize", "Muscle", "Special"):
            ref = secs.get(cls_name, {}).get("@chassis")
            if ref and ref in secs:
                for chassis_name in secs[ref]:
                    key = _normalize_car_key(chassis_name)
                    mapping[key] = cls_name
                    mapping[key[:6]] = cls_name
    except Exception:
        pass
    return mapping


def _discover_ct_liveries(
    game_dir: Path,
    car_stem: str,
    car_key: str,
) -> tuple[list[str], dict[str, tuple[Path, str]]]:
    """Find CT.RES liveries matching this car (by COMMON.RES vehicle/driver catalog or prefix)."""
    ct_path = game_dir / "DATA" / "RES" / "CT.RES"
    if not ct_path.is_file():
        return [], {}
    try:
        ct_entries = mb.directory(ct_path)
    except Exception:
        return [], {}

    ct_by_low = {e["name"].lower(): e["name"] for e in ct_entries}
    livery_names: list[str] = []
    livery_sources: dict[str, tuple[Path, str]] = {}

    def _register_number(num: str, label_suffix: str = "") -> None:
        num_s = str(num).strip()
        if not num_s:
            return
        # Prefer high-res .tm0, fall back to .tex
        member = ct_by_low.get(f"{num_s}.tm0") or ct_by_low.get(f"{num_s}.tex")
        if not member:
            return
        label = f"#{num_s}" + (f" — {label_suffix}" if label_suffix else "")
        if label not in livery_sources:
            livery_names.append(label)
            livery_sources[label] = (ct_path, member)

    # 1. Match prefix directly in CT.RES (supports custom/modded named liveries)
    prefixes = {car_stem.lower(), car_key}
    for e in ct_entries:
        low = e["name"].lower()
        if not low.endswith((".tm0", ".tex")):
            continue
        stem = Path(low).stem
        if any(stem.startswith(p) and len(p) >= 3 for p in prefixes):
            label = Path(e["name"]).stem
            if label not in livery_sources:
                livery_names.append(label)
                livery_sources[label] = (ct_path, e["name"])

    # 2. Match retail vehicle/driver numbers in COMMON.RES VALTs
    common = game_dir / "DATA" / "RES" / "COMMON.RES"
    if common.is_file():
        try:
            ce = mb.directory(common)
            ce_low = {e["name"].lower(): e["name"] for e in ce}
            seen_nums: set[str] = set()
            for val_name in ("buycar.val", "dirt.val", "truck.val", "modified.val", "cup.val"):
                member = ce_low.get(val_name)
                if not member:
                    continue
                raw, _ = mb.payload(common, member, ce, verify_crc=False)
                parsed = valt.parse_binary_valt(raw)
                for sec in parsed.get("sections", []):
                    pairs = {str(k).lower(): str(v) for k, v in sec.get("pairs", [])}
                    chassis = _normalize_car_key(pairs.get("chassis", ""))
                    if not chassis:
                        continue
                    if chassis == car_key or chassis.startswith(car_key) or car_key.startswith(chassis):
                        num = pairs.get("number", "").strip()
                        if num and num not in seen_nums:
                            seen_nums.add(num)
                            driver_name = pairs.get("name", "")
                            _register_number(num, driver_name)
        except Exception:
            pass

    return livery_names, livery_sources


def load_car_showroom_assembly(
    game_dir: Path | str,
    car_archive_name: str,
    lod_name: str = "Low0",
) -> CarShowroomAssembly:
    """Load an assembled 3D vehicle (chassis MODL + 4 wheels + physics + Bone.mvl + textures)."""
    root = Path(game_dir)
    cand = Path(car_archive_name)
    if cand.is_file():
        archive_path = cand
    elif (root / cand).is_file():
        archive_path = root / cand
    else:
        fname = cand.name if cand.name.lower().endswith(".car") else f"{cand.name}.CAR"
        archive_path = root / "DATA" / "CAR" / fname
        if not archive_path.is_file():
            # Case-insensitive lookup inside DATA/CAR
            car_dir = root / "DATA" / "CAR"
            if car_dir.is_dir():
                hit = next((p for p in car_dir.iterdir() if p.name.lower() == fname.lower()), None)
                if hit is not None:
                    archive_path = hit

    if not archive_path.is_file():
        raise FileNotFoundError(f"Car archive not found: {archive_path}")

    entries = mb.directory(archive_path)
    by_low = {e["name"].lower(): e["name"] for e in entries}

    # Discover available LODs
    available_lods = [
        f"Low{i}" for i in range(5) if f"low{i}.mod" in by_low
    ] or [Path(e["name"]).stem for e in entries if e["name"].lower().endswith(".mod")]
    chosen_lod = lod_name
    lod_member = by_low.get(f"{lod_name.lower()}.mod") or by_low.get(lod_name.lower())
    if lod_member is None:
        for fallback in ("low0.mod", "low1.mod", "low2.mod", "low3.mod", "low4.mod"):
            if fallback in by_low:
                lod_member = by_low[fallback]
                chosen_lod = Path(lod_member).stem
                break
    if lod_member is None:
        raise ValueError(f"No .mod geometry found in {archive_path.name}")

    mod_raw, _ = mb.payload(archive_path, lod_member, entries, verify_crc=False)
    chassis_parts = _decode_modl_bytes(mod_raw)

    # Parse physics.val
    physics_fields: dict[str, Any] = {}
    if "physics.val" in by_low:
        phys_raw, _ = mb.payload(archive_path, by_low["physics.val"], entries, verify_crc=False)
        physics_fields = _parse_physics_sections(phys_raw)

    # Parse Bone.mvl (fallback Bone.val)
    bones: dict[int, tuple[float, float, float]] = {}
    bone_member = by_low.get("bone.mvl") or by_low.get("bone.val")
    if bone_member:
        bone_raw, _ = mb.payload(archive_path, bone_member, entries, verify_crc=False)
        bones = _parse_bones(bone_raw)

    # Decode embedded .CAR textures (prefer .tm0 over .tex when both share stem)
    textures: dict[str, TextureAsset] = {}
    for ext_order in (".tex", ".tm0"):
        for e in entries:
            low = e["name"].lower()
            if not low.endswith(ext_order):
                continue
            raw_t, _ = mb.payload(archive_path, e["name"], entries, verify_crc=False)
            asset = _decode_tex_asset(e["name"], raw_t, asset_id=f"{archive_path.name}#{e['name']}")
            if asset is not None:
                stem = Path(low).stem
                textures[stem] = asset

    # Position 4 wheels using physics.val dimensions + RACE.RES wheel mesh (or procedural fallback)
    chassis_sec = physics_fields.get("Chassis", {})
    front_sec = physics_fields.get("Front", {})
    rear_sec = physics_fields.get("Rear", {})
    ftrack_m = _safe_float(chassis_sec.get("ftrack"), 62.0) * 0.0254
    rtrack_m = _safe_float(chassis_sec.get("rtrack"), 62.0) * 0.0254
    wheelbase_m = _safe_float(chassis_sec.get("wheelbase"), 102.0) * 0.0254
    front_r = _safe_float(front_sec.get("tire_diameter"), 28.0) * 0.0254 * 0.5
    rear_r = _safe_float(rear_sec.get("tire_diameter"), 28.0) * 0.0254 * 0.5

    race_path = root / "DATA" / "RES" / "RACE.RES"
    race_entries = mb.directory(race_path) if race_path.is_file() else []
    race_low = {e["name"].lower(): e["name"] for e in race_entries}

    def _load_axle_wheel(sec: dict[str, str], radius: float) -> list[MeshPart]:
        wclass = str(sec.get("wheel_class", "")).strip().lower()
        wmodel = str(sec.get("model", "")).strip().lower()
        candidates = []
        if wclass in _WHEEL_CLASS_MOD:
            candidates.append(_WHEEL_CLASS_MOD[wclass])
        if wmodel:
            candidates.extend([f"{wmodel}1.mod", f"{wmodel}.mod"])
        for cname in candidates:
            member = race_low.get(cname.lower())
            if member:
                try:
                    wraw, _ = mb.payload(race_path, member, race_entries, verify_crc=False)
                    parts = _decode_modl_bytes(wraw)
                    if parts:
                        return parts
                except Exception:
                    pass
        return _procedural_wheel_parts(radius)

    front_base = _load_axle_wheel(front_sec, front_r)
    rear_base = _load_axle_wheel(rear_sec, rear_r)

    wheel_parts: list[MeshPart] = []
    wheel_specs = [
        (front_base, (-ftrack_m * 0.5, front_r, +wheelbase_m * 0.5), front_r),
        (front_base, (+ftrack_m * 0.5, front_r, +wheelbase_m * 0.5), front_r),
        (rear_base, (-rtrack_m * 0.5, rear_r, -wheelbase_m * 0.5), rear_r),
        (rear_base, (+rtrack_m * 0.5, rear_r, -wheelbase_m * 0.5), rear_r),
    ]
    for base, pos, rad in wheel_specs:
        wheel_parts.extend(_position_wheel_parts(base, pos, rad))

    # Load any referenced textures from RACE.RES and COMMON.RES
    common_path = root / "DATA" / "RES" / "COMMON.RES"
    common_entries = mb.directory(common_path) if common_path.is_file() else []
    common_low = {e["name"].lower(): e["name"] for e in common_entries}

    needed_stems = {Path(p.texture).stem.lower() for p in chassis_parts + wheel_parts if p.texture}
    for stem in needed_stems:
        if stem in textures or stem.startswith("__"):
            continue
        for res_path, res_entries, res_low in (
            (race_path, race_entries, race_low),
            (common_path, common_entries, common_low),
        ):
            member = res_low.get(f"{stem}.tm0") or res_low.get(f"{stem}.tex")
            if member:
                raw_t, _ = mb.payload(res_path, member, res_entries, verify_crc=False)
                asset = _decode_tex_asset(member, raw_t, asset_id=f"{res_path.name}#{member}")
                if asset is not None:
                    textures[stem] = asset
                    break

    # Discover CT.RES liveries & resolve primary body texture slot
    car_stem = archive_path.stem
    car_key = _normalize_car_key(car_stem)
    car_name = car_key.title()
    class_map = _discover_car_classes(root)
    car_class = class_map.get(car_key) or class_map.get(car_key[:6], "Special")

    body_tex_key = ""
    for part in chassis_parts:
        st = Path(part.texture).stem.lower()
        if st and (st == car_key or st.startswith(car_key[:4])):
            body_tex_key = st
            break
    if not body_tex_key and chassis_parts:
        body_tex_key = Path(chassis_parts[0].texture).stem.lower()

    livery_names, livery_sources = _discover_ct_liveries(root, car_stem, car_key)
    active_livery = ""
    if livery_names:
        active_livery = livery_names[0]
        ct_path, member = livery_sources[active_livery]
        try:
            ct_entries = mb.directory(ct_path)
            raw_t, _ = mb.payload(ct_path, member, ct_entries, verify_crc=False)
            asset = _decode_tex_asset(member, raw_t, asset_id=f"{ct_path.name}#{member}")
            if asset is not None and body_tex_key:
                textures[body_tex_key] = asset
        except Exception:
            pass
    elif body_tex_key in textures:
        active_livery = textures[body_tex_key].name
        livery_names = [active_livery]

    # Ensure procedural wheel texture exists if procedural fallback was used
    if any(p.texture == "__procedural_wheel__" for p in wheel_parts):
        pw = np.full((16, 16, 4), [38, 42, 48, 255], dtype=np.uint8)
        pw[4:12, 4:12, :3] = [155, 164, 178]
        textures["__procedural_wheel__"] = _make_texture_asset(
            "ProceduralWheel.tex", 16, 16, pw.tobytes(), asset_id="procedural_wheel"
        )

    return CarShowroomAssembly(
        car_name=car_name,
        archive_path=archive_path,
        lod_name=chosen_lod,
        livery_names=livery_names,
        active_livery=active_livery,
        chassis_parts=chassis_parts,
        wheel_parts=wheel_parts,
        textures=textures,
        physics_fields=physics_fields,
        bones=bones,
        car_class=car_class,
        available_lods=available_lods,
        body_texture_key=body_tex_key,
        livery_sources=livery_sources,
    )


def switch_car_livery(assembly: CarShowroomAssembly, livery_name: str) -> TextureAsset | None:
    """Swap active CT.RES livery onto the assembly's body texture slot."""
    src = assembly.livery_sources.get(livery_name)
    if src is None:
        return None
    ct_path, member = src
    entries = mb.directory(ct_path)
    raw, _ = mb.payload(ct_path, member, entries, verify_crc=False)
    asset = _decode_tex_asset(member, raw, asset_id=f"{ct_path.name}#{member}")
    if asset is not None:
        key = assembly.body_texture_key or Path(member).stem.lower()
        assembly.textures[key] = asset
        assembly.active_livery = livery_name
    return asset


def apply_damage_preview(
    parts: list[MeshPart],
    bones: dict[int, tuple[float, float, float]],
    damage_pct: float,
    detached_panels: set[int] | None = None,
) -> list[MeshPart]:
    """Deterministically deform chassis MeshParts toward Bone.mvl centers (0% -> 100%).

    Never mutates input parts. Optional `detached_panels` hides parts by index.
    """
    detached = detached_panels or set()
    dmg = max(0.0, min(1.0, float(damage_pct) / 100.0))

    # Compute overall chassis center for inward crush vectors
    kept_verts = [p.vertices[:, :3] for i, p in enumerate(parts) if i not in detached and len(p.vertices)]
    if not kept_verts:
        return []
    all_pts = np.concatenate(kept_verts, axis=0)
    center = all_pts.mean(axis=0).astype(np.float32)
    span = np.maximum(all_pts.max(axis=0) - all_pts.min(axis=0), 0.25).astype(np.float32)

    bone_pts = (
        np.asarray(list(bones.values()), dtype=np.float32)
        if bones
        else np.array([
            [center[0], center[1], all_pts[:, 2].max()],
            [center[0], center[1], all_pts[:, 2].min()],
            [all_pts[:, 0].min(), center[1], center[2]],
            [all_pts[:, 0].max(), center[1], center[2]],
        ], dtype=np.float32)
    )

    out: list[MeshPart] = []
    for idx, part in enumerate(parts):
        if idx in detached:
            continue
        v = part.vertices.copy()
        ind = part.indices.copy()
        if dmg > 1e-6 and len(v) > 0:
            pos = v[:, :3]
            # Vectorized nearest-bone weighting
            diff = pos[:, None, :] - bone_pts[None, :, :]  # (N, B, 3)
            dist_sq = np.sum(diff * diff, axis=2)          # (N, B)
            nearest_idx = np.argmin(dist_sq, axis=1)
            nearest_dist = np.sqrt(dist_sq[np.arange(len(pos)), nearest_idx])
            nearest_bone = bone_pts[nearest_idx]

            # Smooth bone zone proximity weight (stronger near exterior bones)
            bone_weight = np.exp(-nearest_dist * 1.65).astype(np.float32)
            rel = pos - center[None, :]
            # Exterior exposure factor: front/rear bumpers & outer fenders crush deepest
            ext_x = np.abs(rel[:, 0]) / (span[0] * 0.5 + 1e-4)
            ext_z = np.abs(rel[:, 2]) / (span[2] * 0.5 + 1e-4)
            zone_amp = np.clip(0.35 + 0.65 * np.maximum(ext_x, ext_z), 0.25, 1.35).astype(np.float32)

            # Inward impact vector toward chassis interior + bone pull
            inward = -rel
            inward_len = np.linalg.norm(inward, axis=1, keepdims=True)
            inward_dir = inward / np.maximum(inward_len, 1e-4)
            bone_pull = nearest_bone - pos
            bp_len = np.linalg.norm(bone_pull, axis=1, keepdims=True)
            bp_dir = bone_pull / np.maximum(bp_len, 1e-4)

            # Deterministic multi-frequency crumple crease ripple
            crease_x = np.sin(pos[:, 2] * 7.3 + pos[:, 1] * 5.1 + idx * 0.7)
            crease_y = np.cos(pos[:, 0] * 6.4 - pos[:, 2] * 4.7 + idx * 1.1)
            crease_z = np.sin(pos[:, 0] * 5.9 + pos[:, 1] * 6.8 - idx * 0.5)
            crease = np.stack([crease_x, crease_y * 0.65, crease_z], axis=1).astype(np.float32)

            crush_mag = (0.24 * dmg * zone_amp * (0.45 + 0.55 * bone_weight))[:, None]
            ripple_mag = (0.075 * (dmg ** 1.15) * zone_amp)[:, None]
            delta = (inward_dir * 0.78 + bp_dir * 0.22) * crush_mag + crease * ripple_mag
            # Slight downward sag on rear/front overhang, keep floor above ground
            delta[:, 1] -= 0.04 * dmg * ext_z * bone_weight

            new_pos = pos + delta
            # Guard against NaN/Inf
            v[:, :3] = np.nan_to_num(new_pos, nan=0.0, posinf=0.0, neginf=0.0)

            # Perturb normals slightly to highlight crumpled metal reflections
            nrm = v[:, 3:6] + crease * (0.45 * dmg)
            nlen = np.linalg.norm(nrm, axis=1, keepdims=True)
            v[:, 3:6] = nrm / np.maximum(nlen, 1e-4)

        out.append(MeshPart(texture=part.texture, vertices=v, indices=ind, tri_count=part.tri_count))
    return out


# ---------------------------------------------------------------------------
# 2x-Supersampled Shaded 3D Turntable Studio Renderer
# ---------------------------------------------------------------------------

def _resolve_texture(assembly: CarShowroomAssembly, tex_name: str) -> TextureAsset | None:
    if not tex_name:
        return None
    low = tex_name.lower()
    stem = Path(low).stem
    return assembly.textures.get(stem) or assembly.textures.get(low)


def render_showroom_frame(
    assembly: CarShowroomAssembly,
    width: int = 420,
    height: int = 260,
    yaw_deg: float = -35.0,
    pitch_deg: float = 18.0,
    damage_pct: float = 0.0,
    detached_panels: set[int] | None = None,
) -> QImage:
    """Render a 2x-supersampled shaded 3D turntable studio frame as a QImage."""
    w = max(64, int(width))
    h = max(64, int(height))
    sw, sh = w * 2, h * 2

    img = QImage(sw, sh, QImage.Format.Format_ARGB32)
    img.fill(QColor("#0b0f17"))
    painter = QPainter(img)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)

    # GT7 / Forza dark studio cyclorama backdrop
    bg_grad = QLinearGradient(0.0, 0.0, 0.0, float(sh))
    bg_grad.setColorAt(0.0, QColor("#0d121c"))
    bg_grad.setColorAt(0.56, QColor("#141c2b"))
    bg_grad.setColorAt(0.70, QColor("#111722"))
    bg_grad.setColorAt(1.0, QColor("#070a10"))
    painter.fillRect(0, 0, sw, sh, bg_grad)

    # Overhead studio softbox glow
    glow = QRadialGradient(sw * 0.5, sh * 0.44, sw * 0.48)
    glow.setColorAt(0.0, QColor(72, 136, 224, 52))
    glow.setColorAt(0.55, QColor(36, 62, 108, 22))
    glow.setColorAt(1.0, QColor(0, 0, 0, 0))
    painter.fillRect(0, 0, sw, sh, glow)

    # Turntable stage disc & subtle neon ring
    disc_rect = QRectF(sw * 0.14, sh * 0.62, sw * 0.72, sh * 0.25)
    disc_grad = QRadialGradient(disc_rect.center(), disc_rect.width() * 0.5)
    disc_grad.setColorAt(0.0, QColor("#1d2636"))
    disc_grad.setColorAt(0.75, QColor("#111723"))
    disc_grad.setColorAt(1.0, QColor(12, 16, 24, 0))
    painter.setBrush(QBrush(disc_grad))
    painter.setPen(QPen(QColor(56, 189, 248, 70), 2.0))
    painter.drawEllipse(disc_rect)

    # Assemble active chassis + 4 wheels
    chassis = apply_damage_preview(assembly.chassis_parts, assembly.bones, damage_pct, detached_panels)
    all_parts = chassis + list(assembly.wheel_parts)

    if not all_parts:
        painter.end()
        return img.scaled(w, h, Qt.AspectRatioMode.IgnoreAspectRatio, Qt.TransformationMode.SmoothTransformation)

    # Bounding sphere for consistent framing
    all_xyz = np.concatenate([p.vertices[:, :3] for p in all_parts if len(p.vertices)], axis=0)
    bmin = all_xyz.min(axis=0)
    bmax = all_xyz.max(axis=0)
    center = (bmin + bmax) * 0.5
    radius = max(float(np.linalg.norm(bmax - bmin)) * 0.5, 0.5)

    # Contact shadow ellipse under car
    shadow_rect = QRectF(sw * 0.24, sh * 0.66, sw * 0.52, sh * 0.15)
    sh_grad = QRadialGradient(shadow_rect.center(), shadow_rect.width() * 0.5)
    sh_grad.setColorAt(0.0, QColor(0, 0, 0, 175))
    sh_grad.setColorAt(0.65, QColor(0, 0, 0, 90))
    sh_grad.setColorAt(1.0, QColor(0, 0, 0, 0))
    painter.setPen(Qt.PenStyle.NoPen)
    painter.setBrush(QBrush(sh_grad))
    painter.drawEllipse(shadow_rect)

    # Orbit camera matrix (yaw around Y, pitch around X)
    yaw = math.radians(yaw_deg)
    pitch = math.radians(max(-65.0, min(75.0, pitch_deg)))
    cy, sy = math.cos(yaw), math.sin(yaw)
    cp, sp = math.cos(pitch), math.sin(pitch)
    r_yaw = np.array([[cy, 0.0, sy], [0.0, 1.0, 0.0], [-sy, 0.0, cy]], dtype=np.float32)
    r_pitch = np.array([[1.0, 0.0, 0.0], [0.0, cp, -sp], [0.0, sp, cp]], dtype=np.float32)
    rot = r_pitch @ r_yaw

    cam_dist = radius * 2.55
    focal = min(sw, sh) * 1.18
    cx_s, cy_s = sw * 0.5, sh * 0.54

    # Key, fill, rim light directions in view space
    l_key = np.array([0.42, 0.72, 0.55], dtype=np.float32)
    l_key /= np.linalg.norm(l_key)
    l_fill = np.array([-0.65, 0.25, 0.72], dtype=np.float32)
    l_fill /= np.linalg.norm(l_fill)
    l_half = l_key + np.array([0.0, 0.0, 1.0], dtype=np.float32)
    l_half /= np.linalg.norm(l_half)

    # Pre-unpack textures once as uint8 RGBA views for fast UV sampling
    tex_views: dict[int, tuple[np.ndarray, int, int]] = {}
    for asset in assembly.textures.values():
        tex_views[id(asset)] = (
            np.frombuffer(asset.rgba, dtype=np.uint8).reshape((asset.height, asset.width, 4)),
            asset.width,
            asset.height,
        )

    tri_polys: list[tuple[float, np.ndarray, tuple[int, int, int, int]]] = []
    for part in all_parts:
        if len(part.vertices) == 0 or len(part.indices) < 3:
            continue
        asset = _resolve_texture(assembly, part.texture)
        tview = tex_views.get(id(asset)) if asset is not None else None
        fallback_rgb = np.array(asset.average_rgb if asset is not None else (172, 180, 194), dtype=np.float32)

        pts_local = part.vertices[:, :3] - center[None, :]
        pts_cam = pts_local @ rot.T
        z_cam = cam_dist - pts_cam[:, 2]
        inv_z = focal / np.maximum(z_cam, 0.15)
        sx = cx_s + pts_cam[:, 0] * inv_z
        sy = cy_s - pts_cam[:, 1] * inv_z
        screen_xy = np.stack([sx, sy], axis=1)

        tris = part.indices.reshape(-1, 3)
        p0 = pts_cam[tris[:, 0]]
        p1 = pts_cam[tris[:, 1]]
        p2 = pts_cam[tris[:, 2]]
        e1 = p1 - p0
        e2 = p2 - p0
        fn = np.cross(e1, e2)
        fn_len = np.linalg.norm(fn, axis=1)
        valid = fn_len > 1e-7
        if not np.any(valid):
            continue
        tris = tris[valid]
        fn = fn[valid] / fn_len[valid, None]
        # Double-sided illumination: flip normal toward camera if needed
        flip = fn[:, 2] < 0.0
        fn[flip] = -fn[flip]

        depth = (pts_cam[tris[:, 0], 2] + pts_cam[tris[:, 1], 2] + pts_cam[tris[:, 2], 2]) / 3.0
        ndotl = np.clip(fn @ l_key, 0.0, 1.0)
        nfill = np.clip(fn @ l_fill, 0.0, 1.0)
        spec = np.power(np.clip(fn @ l_half, 0.0, 1.0), 18.0) * 70.0
        shade = 0.28 + 0.62 * ndotl + 0.18 * nfill

        # Sample real livery/texture pixels at each triangle UV centroid
        if tview is not None:
            tarr, tw, th = tview
            uvs = part.vertices[:, 6:8]
            uv_c = (uvs[tris[:, 0]] + uvs[tris[:, 1]] + uvs[tris[:, 2]]) / 3.0
            uu = np.mod(uv_c[:, 0], 1.0)
            vv = np.mod(uv_c[:, 1], 1.0)
            tx = np.clip((uu * (tw - 1)).astype(np.int32), 0, tw - 1)
            ty = np.clip((vv * (th - 1)).astype(np.int32), 0, th - 1)
            sampled = tarr[ty, tx].astype(np.float32)
            base_rgb = sampled[:, :3]
            alphas = sampled[:, 3]
        else:
            base_rgb = np.broadcast_to(fallback_rgb, (len(tris), 3)).copy()
            alphas = np.full(len(tris), 255.0, dtype=np.float32)

        lit = np.clip(base_rgb * shade[:, None] + spec[:, None], 0.0, 255.0).astype(np.uint8)
        sxy_tri = screen_xy[tris]  # (T, 3, 2)

        for k in range(len(tris)):
            a_val = int(alphas[k])
            if a_val < 20:
                continue
            r_c, g_c, b_c = int(lit[k, 0]), int(lit[k, 1]), int(lit[k, 2])
            tri_polys.append((float(depth[k]), sxy_tri[k], (r_c, g_c, b_c, min(255, max(180, a_val)))))

    # Painter's algorithm: far -> near (smaller cam-space z is farther)
    tri_polys.sort(key=lambda item: item[0])
    painter.setPen(Qt.PenStyle.NoPen)
    for _z, pts2d, rgba_col in tri_polys:
        col = QColor(*rgba_col)
        painter.setBrush(col)
        painter.setPen(QPen(col, 0.45))
        poly = QPolygonF([
            QPointF(float(pts2d[0, 0]), float(pts2d[0, 1])),
            QPointF(float(pts2d[1, 0]), float(pts2d[1, 1])),
            QPointF(float(pts2d[2, 0]), float(pts2d[2, 1])),
        ])
        painter.drawPolygon(poly)

    painter.end()
    return img.scaled(w, h, Qt.AspectRatioMode.IgnoreAspectRatio, Qt.TransformationMode.SmoothTransformation)


# ---------------------------------------------------------------------------
# Texture Studio & HOSTTEX (EODHTX1) Export Pipeline
# ---------------------------------------------------------------------------

def _get_texture_key(assembly: CarShowroomAssembly, tex_name: str) -> str:
    low = tex_name.lower()
    stem = Path(low).stem
    if tex_name in assembly.textures:
        return tex_name
    if stem in assembly.textures:
        return stem
    for k, asset in assembly.textures.items():
        if asset.name.lower() == low or k.lower() == low or k.lower() == stem:
            return k
    raise KeyError(f"Texture '{tex_name}' not found in {assembly.car_name} assembly")


def export_car_texture_png(assembly: CarShowroomAssembly, tex_name: str, out_png: Path | str) -> Path:
    """Export a car/livery TextureAsset as a straight-RGBA PNG."""
    key = _get_texture_key(assembly, tex_name)
    asset = assembly.textures[key]
    out_path = Path(out_png)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    Image.frombytes("RGBA", (asset.width, asset.height), asset.rgba).save(out_path, format="PNG")
    return out_path


def replace_car_texture_png(assembly: CarShowroomAssembly, tex_name: str, png_path: Path | str) -> TextureAsset:
    """Replace an in-memory TextureAsset with a user PNG (preserving HOSTTEX identity)."""
    key = _get_texture_key(assembly, tex_name)
    old = assembly.textures[key]
    src = Path(png_path)
    w, h, rgba = project_state.load_png_rgba(src)
    updated = _make_texture_asset(old.name, w, h, rgba, raw_tex_bytes=old.raw_tex_bytes, asset_id=old.asset_id)
    updated.orig_width = old.orig_width
    updated.orig_height = old.orig_height
    updated.orig_rgba = old.orig_rgba
    updated.index_key = old.index_key
    updated.palette_key = old.palette_key
    updated.is_replaced = True
    updated.replaced_png_path = str(src)
    assembly.textures[key] = updated
    return updated


def upscale_car_texture_hosttex(
    assembly: CarShowroomAssembly,
    tex_name: str,
    out_dir: Path | str,
    scale: int = 2,
) -> Path:
    """Upscale a car texture and write a validated EODHTX1 `.eodhtx` Vulkan HOSTTEX file."""
    key = _get_texture_key(assembly, tex_name)
    asset = assembly.textures[key]
    factor = max(2, int(scale))
    img = Image.frombytes("RGBA", (asset.width, asset.height), asset.rgba)
    up_w, up_h = asset.width * factor, asset.height * factor
    up = img.resize((up_w, up_h), Image.Resampling.LANCZOS)
    up = up.filter(ImageFilter.UnsharpMask(radius=1.0, percent=85, threshold=2))

    data, _tw, _th = project_state.build_eodhtx_from_rgba(
        up.tobytes("raw", "RGBA"),
        up.width,
        up.height,
        asset.orig_width or asset.width,
        asset.orig_height or asset.height,
    )
    out_root = Path(out_dir)
    out_root.mkdir(parents=True, exist_ok=True)
    stem = Path(asset.name).stem
    idx_key, pal_key = asset.ensure_identity()
    fname = f"{idx_key}_{pal_key}.eodhtx" if (idx_key and pal_key) else f"{assembly.car_name.lower()}_{stem}.eodhtx"
    out_path = out_root / fname
    out_path.write_bytes(data)
    return out_path


# ---------------------------------------------------------------------------
# Interactive PySide6 Turntable & Texture Studio Widget
# ---------------------------------------------------------------------------

class _OrbitPreviewLabel(QLabel):
    """Interactive drag-to-orbit 360-degree showroom viewport label."""

    orbitChanged = Signal(float, float)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.yaw_deg = -35.0
        self.pitch_deg = 18.0
        self._drag_pos: QPoint | None = None
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.setMinimumSize(420, 260)
        self.setCursor(Qt.CursorShape.OpenHandCursor)
        self.setStyleSheet(
            "QLabel { background: #0b0f17; border: 1px solid #273042; border-radius: 8px; }"
        )

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self._drag_pos = event.position().toPoint()
            self.setCursor(Qt.CursorShape.ClosedHandCursor)
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self._drag_pos is not None:
            cur = event.position().toPoint()
            dx = cur.x() - self._drag_pos.x()
            dy = cur.y() - self._drag_pos.y()
            self._drag_pos = cur
            self.yaw_deg = (self.yaw_deg + dx * 0.75) % 360.0
            self.pitch_deg = max(-35.0, min(70.0, self.pitch_deg + dy * 0.45))
            self.orbitChanged.emit(self.yaw_deg, self.pitch_deg)
            event.accept()
            return
        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        if self._drag_pos is not None:
            self._drag_pos = None
            self.setCursor(Qt.CursorShape.OpenHandCursor)
            event.accept()
            return
        super().mouseReleaseEvent(event)

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:
        self.yaw_deg = -35.0
        self.pitch_deg = 18.0
        self.orbitChanged.emit(self.yaw_deg, self.pitch_deg)
        event.accept()


class CarShowroomPanel(QWidget):
    """3D Assembled Car Showroom, Bone.mvl Damage Slider & Texture/HOSTTEX Studio."""

    carChanged = Signal(str, dict)

    def __init__(self, game_dir: Path | str, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.game_dir = Path(game_dir)
        self.assembly: CarShowroomAssembly | None = None
        self._all_cars: list[tuple[str, str, str]] = []  # (display, rel_archive, car_class)
        self._panel_checks: list[QCheckBox] = []
        self._detached_panels: set[int] = set()
        self._loading = False

        root = QVBoxLayout(self)
        root.setContentsMargins(6, 6, 6, 6)
        root.setSpacing(8)

        # Top control bar: Class Filter, Car, LOD, Livery
        bar = QHBoxLayout()
        bar.setSpacing(6)
        self.cmb_class = QComboBox()
        self.cmb_class.addItems(["All", "Compact", "Midsize", "Muscle", "Special"])
        self.cmb_car = QComboBox()
        self.cmb_lod = QComboBox()
        self.cmb_livery = QComboBox()

        bar.addWidget(QLabel("Class:"))
        bar.addWidget(self.cmb_class)
        bar.addWidget(QLabel("Car:"))
        bar.addWidget(self.cmb_car, 1)
        bar.addWidget(QLabel("LOD:"))
        bar.addWidget(self.cmb_lod)
        bar.addWidget(QLabel("Livery:"))
        bar.addWidget(self.cmb_livery, 1)
        root.addLayout(bar)

        # Main split: Left = 3D Turntable + Damage & Panel toggles; Right = Texture/HOSTTEX Studio
        split = QSplitter(Qt.Orientation.Horizontal)

        left = QWidget()
        l_lay = QVBoxLayout(left)
        l_lay.setContentsMargins(0, 0, 0, 0)
        l_lay.setSpacing(6)

        self.viewport = _OrbitPreviewLabel(self)
        l_lay.addWidget(self.viewport, 1)

        self.lbl_meta = QLabel("Drag to orbit 360° · Double-click to reset camera")
        self.lbl_meta.setStyleSheet("color: #8c9bb3; font-size: 8.5pt;")
        l_lay.addWidget(self.lbl_meta)

        # Damage slider
        dmg_row = QHBoxLayout()
        dmg_row.addWidget(QLabel("Damage Level:"))
        self.sld_damage = QSlider(Qt.Orientation.Horizontal)
        self.sld_damage.setRange(0, 100)
        self.sld_damage.setValue(0)
        self.lbl_damage = QLabel("0% Pristine")
        self.lbl_damage.setMinimumWidth(96)
        dmg_row.addWidget(self.sld_damage, 1)
        dmg_row.addWidget(self.lbl_damage)
        l_lay.addLayout(dmg_row)

        # Panel detach toggles
        self._panel_scroll = QScrollArea()
        self._panel_scroll.setWidgetResizable(True)
        self._panel_scroll.setMaximumHeight(68)
        self._panel_scroll.setFrameShape(QFrame.Shape.NoFrame)
        self._panel_host = QWidget()
        self._panel_row = QHBoxLayout(self._panel_host)
        self._panel_row.setContentsMargins(2, 2, 2, 2)
        self._panel_row.setSpacing(8)
        self._panel_scroll.setWidget(self._panel_host)
        l_lay.addWidget(self._panel_scroll)
        split.addWidget(left)

        # Right: Material / Texture Studio
        right = QWidget()
        r_lay = QVBoxLayout(right)
        r_lay.setContentsMargins(4, 0, 0, 0)
        r_lay.setSpacing(6)
        r_lay.addWidget(QLabel("<b>Material &amp; Texture Studio</b>"))
        self.tex_list = QListWidget()
        r_lay.addWidget(self.tex_list, 1)

        self.btn_export_png = QPushButton("Export PNG")
        self.btn_replace_png = QPushButton("Replace PNG")
        self.btn_upscale_hd = QPushButton("Upscale HD (HOSTTEX)")
        r_lay.addWidget(self.btn_export_png)
        r_lay.addWidget(self.btn_replace_png)
        r_lay.addWidget(self.btn_upscale_hd)

        self.lbl_tex_status = QLabel("")
        self.lbl_tex_status.setWordWrap(True)
        self.lbl_tex_status.setStyleSheet("color: #8c9bb3; font-size: 8.5pt;")
        r_lay.addWidget(self.lbl_tex_status)
        split.addWidget(right)
        split.setStretchFactor(0, 3)
        split.setStretchFactor(1, 1)
        root.addWidget(split, 1)

        # Wire signals
        self.viewport.orbitChanged.connect(lambda _y, _p: self._render_viewport())
        self.cmb_class.currentTextChanged.connect(self._apply_class_filter)
        self.cmb_car.currentIndexChanged.connect(self._on_car_selected)
        self.cmb_lod.currentTextChanged.connect(self._on_lod_selected)
        self.cmb_livery.currentTextChanged.connect(self._on_livery_selected)
        self.sld_damage.valueChanged.connect(self._on_damage_changed)
        self.btn_export_png.clicked.connect(self._on_export_png)
        self.btn_replace_png.clicked.connect(self._on_replace_png)
        self.btn_upscale_hd.clicked.connect(self._on_upscale_hosttex)

        self._populate_cars()

    def _populate_cars(self) -> None:
        class_map = _discover_car_classes(self.game_dir)
        self._all_cars.clear()
        for path in discover_cars(self.game_dir):
            key = _normalize_car_key(path.stem)
            cls = class_map.get(key) or class_map.get(key[:6], "Special")
            rel = path.relative_to(self.game_dir).as_posix()
            self._all_cars.append((key.title(), rel, cls))
        self._apply_class_filter(self.cmb_class.currentText())

    def _apply_class_filter(self, cls_filter: str) -> None:
        prev = self.cmb_car.currentData()
        self.cmb_car.blockSignals(True)
        self.cmb_car.clear()
        for display, rel, cls in self._all_cars:
            if cls_filter == "All" or cls == cls_filter:
                self.cmb_car.addItem(f"{display} ({cls})", rel)
        idx = self.cmb_car.findData(prev)
        if idx < 0:
            idx = next((i for i in range(self.cmb_car.count()) if "DIGIT.CAR" in str(self.cmb_car.itemData(i)).upper()), 0)
        if self.cmb_car.count() > 0:
            self.cmb_car.setCurrentIndex(max(0, idx))
        self.cmb_car.blockSignals(False)
        self._on_car_selected(self.cmb_car.currentIndex())

    def select_car_by_archive(self, archive_rel: str) -> None:
        if not archive_rel:
            return
        idx = self.cmb_car.findData(archive_rel)
        if idx < 0 and self.cmb_class.currentText() != "All":
            self.cmb_class.setCurrentText("All")
            idx = self.cmb_car.findData(archive_rel)
        if idx >= 0 and idx != self.cmb_car.currentIndex():
            self.cmb_car.setCurrentIndex(idx)

    def _on_car_selected(self, _index: int) -> None:
        rel = self.cmb_car.currentData()
        if not rel:
            return
        self._load_assembly(str(rel), "Low0")

    def _on_lod_selected(self, lod_name: str) -> None:
        if self._loading or not lod_name or self.assembly is None:
            return
        rel = self.cmb_car.currentData()
        if rel:
            self._load_assembly(str(rel), lod_name)

    def _load_assembly(self, rel_archive: str, lod_name: str) -> None:
        self._loading = True
        try:
            self.assembly = load_car_showroom_assembly(self.game_dir, rel_archive, lod_name=lod_name)
        except Exception as exc:
            self.assembly = None
            self.lbl_meta.setText(f"Could not load {rel_archive}: {exc}")
            self._loading = False
            return

        self._detached_panels.clear()
        self.cmb_lod.blockSignals(True)
        self.cmb_lod.clear()
        self.cmb_lod.addItems(self.assembly.available_lods)
        l_idx = self.cmb_lod.findText(self.assembly.lod_name)
        if l_idx >= 0:
            self.cmb_lod.setCurrentIndex(l_idx)
        self.cmb_lod.blockSignals(False)

        self.cmb_livery.blockSignals(True)
        self.cmb_livery.clear()
        if self.assembly.livery_names:
            self.cmb_livery.addItems(self.assembly.livery_names)
            v_idx = self.cmb_livery.findText(self.assembly.active_livery)
            if v_idx >= 0:
                self.cmb_livery.setCurrentIndex(v_idx)
        else:
            self.cmb_livery.addItem("Default")
        self.cmb_livery.blockSignals(False)

        self._rebuild_panel_checks()
        self._rebuild_texture_list()
        self._loading = False
        self._render_viewport()
        self.carChanged.emit(rel_archive, self.assembly.physics_fields)

    def _rebuild_panel_checks(self) -> None:
        while self._panel_row.count():
            item = self._panel_row.takeAt(0)
            if item and item.widget():
                item.widget().deleteLater()
        self._panel_checks.clear()
        if self.assembly is None:
            return
        for i, part in enumerate(self.assembly.chassis_parts):
            label = Path(part.texture).stem or f"Part {i}"
            chk = QCheckBox(f"P{i}: {label}")
            chk.setChecked(True)
            chk.toggled.connect(lambda checked, idx=i: self._toggle_panel(idx, checked))
            self._panel_row.addWidget(chk)
            self._panel_checks.append(chk)
        self._panel_row.addStretch(1)

    def _toggle_panel(self, idx: int, visible: bool) -> None:
        if visible:
            self._detached_panels.discard(idx)
        else:
            self._detached_panels.add(idx)
        self._render_viewport()

    def _rebuild_texture_list(self) -> None:
        self.tex_list.clear()
        if self.assembly is None:
            return
        for key, asset in sorted(self.assembly.textures.items()):
            tag = " [Replaced]" if asset.is_replaced else ""
            item = QListWidgetItem(f"{asset.name} ({asset.width}×{asset.height}){tag}")
            item.setData(Qt.ItemDataRole.UserRole, key)
            self.tex_list.addItem(item)
        if self.tex_list.count() > 0:
            self.tex_list.setCurrentRow(0)

    def _on_livery_selected(self, livery_name: str) -> None:
        if self._loading or self.assembly is None or not livery_name:
            return
        switch_car_livery(self.assembly, livery_name)
        self._rebuild_texture_list()
        self._render_viewport()

    def _on_damage_changed(self, value: int) -> None:
        tag = "Pristine" if value == 0 else ("Wrecked" if value >= 95 else ("Heavy" if value >= 60 else "Scuffed"))
        self.lbl_damage.setText(f"{value}% {tag}")
        self._render_viewport()

    def _render_viewport(self) -> None:
        if self.assembly is None:
            return
        frame = render_showroom_frame(
            self.assembly,
            width=max(420, self.viewport.width()),
            height=max(260, self.viewport.height()),
            yaw_deg=self.viewport.yaw_deg,
            pitch_deg=self.viewport.pitch_deg,
            damage_pct=float(self.sld_damage.value()),
            detached_panels=self._detached_panels,
        )
        self.viewport.setPixmap(QPixmap.fromImage(frame))
        tris = sum(p.tri_count for p in self.assembly.chassis_parts) + sum(p.tri_count for p in self.assembly.wheel_parts)
        self.lbl_meta.setText(
            f"{self.assembly.car_name} [{self.assembly.car_class}] · {self.assembly.lod_name} · "
            f"{tris:,} tris · {len(self.assembly.bones)} bones · Drag to orbit 360°"
        )

    def _selected_tex_key(self) -> str | None:
        item = self.tex_list.currentItem()
        if item is None:
            return None
        return str(item.data(Qt.ItemDataRole.UserRole))

    def _on_export_png(self) -> None:
        key = self._selected_tex_key()
        if self.assembly is None or not key:
            return
        out_path, _ = QFileDialog.getSaveFileName(self, "Export Car Texture PNG", f"{key}.png", "PNG (*.png)")
        if not out_path:
            return
        saved = export_car_texture_png(self.assembly, key, Path(out_path))
        self.lbl_tex_status.setText(f"Exported {saved.name}")

    def _on_replace_png(self) -> None:
        key = self._selected_tex_key()
        if self.assembly is None or not key:
            return
        in_path, _ = QFileDialog.getOpenFileName(self, "Replace Car Texture PNG", "", "Images (*.png *.jpg *.bmp)")
        if not in_path:
            return
        try:
            asset = replace_car_texture_png(self.assembly, key, Path(in_path))
            self._rebuild_texture_list()
            self._render_viewport()
            self.lbl_tex_status.setText(f"Replaced {asset.name} ({asset.width}×{asset.height})")
        except Exception as exc:
            QMessageBox.warning(self, "Texture Replace Error", str(exc))

    def _on_upscale_hosttex(self) -> None:
        key = self._selected_tex_key()
        if self.assembly is None or not key:
            return
        out_dir = QFileDialog.getExistingDirectory(self, "Select HOSTTEX Output Folder")
        if not out_dir:
            return
        try:
            out_file = upscale_car_texture_hosttex(self.assembly, key, Path(out_dir), scale=2)
            self.lbl_tex_status.setText(f"Wrote HOSTTEX {out_file.name}")
        except Exception as exc:
            QMessageBox.warning(self, "HOSTTEX Upscale Error", str(exc))
