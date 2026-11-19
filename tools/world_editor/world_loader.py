"""Clean-room EOD .TRK World Loader.

Imports and reuses the repository's native reverse-engineered parsers in tools/:
  - tools/world_graf.py      (GRAF v18 display list, mlists, embedded mrModel)
  - tools/vif_mesh.py        (4SPS VIF mesh decoder, MODL v9 parser)
  - tools/world_export.py    (scene classification, billboards, textures, event props, write-back)
  - tools/texture_preview.py (PS2 PSMT4/PSMT8 swizzled texture decoder)
  - tools/world_collision.py (track.bsc SCOL terrain + track.sol SOBL solids + Terrain raycast)
  - tools/formats/ilin.py    (ILIN .ild racing lines)
  - tools/modding_backend.py (0TSR v6 directory + cached member payloads)

Never modifies retail .TRK files on disk.
"""

from __future__ import annotations

import hashlib
import json
import math
import os
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np

# Ensure repo tools/ is importable
REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_DIR = REPO_ROOT / "tools"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

import modding_backend as mb  # noqa: E402
import texture_identity as tex_id  # noqa: E402
import vehicle_export as veh_exp  # noqa: E402
import vif_mesh  # noqa: E402
import world_collision as wc  # noqa: E402
import world_export as we  # noqa: E402
import world_graf as wg  # noqa: E402
from formats import ilin as ilin_fmt  # noqa: E402
from modding_backend import payload  # noqa: E402
from texture_preview import decode_preview  # noqa: E402
from valt import parse_binary_valt  # noqa: E402

DEFAULT_GAME_DIR = Path(r"D:\Test Drive - Eve of Destruction (USA)")

# In-memory cache of decoded DATA/CAR/*.CAR models + textures per game_dir
_CAR_CATALOG_CACHE: dict[str, tuple[dict[str, "WorldModel"], dict[str, "TextureAsset"]]] = {}


def default_game_dir() -> Path:
    env = os.environ.get("EOD_GAME_DIR") or os.environ.get("EOD_DISC")
    if env:
        p = Path(env).expanduser()
        if p.is_dir():
            return p
    local_app = os.environ.get("LOCALAPPDATA")
    if local_app:
        cand = Path(local_app) / "TDEODRecomp" / "disc"
        if (cand / "DATA" / "TRACKS").is_dir():
            return cand
    return DEFAULT_GAME_DIR


def discover_tracks(game_dir: Path | str) -> list[Path]:
    """Return sorted list of retail .TRK files under <game_dir>/DATA/TRACKS."""
    base = Path(game_dir)
    tracks_dir = base / "DATA" / "TRACKS"
    if not tracks_dir.is_dir():
        return []
    return sorted(
        (p for p in tracks_dir.iterdir() if p.is_file() and p.suffix.upper() == ".TRK"),
        key=lambda p: p.name.upper(),
    )


def discover_cars(game_dir: Path | str) -> list[Path]:
    """Return sorted list of retail .CAR archives under <game_dir>/DATA/CAR."""
    base = Path(game_dir)
    cars_dir = base / "DATA" / "CAR"
    if not cars_dir.is_dir():
        return []
    return sorted(
        (p for p in cars_dir.iterdir() if p.is_file() and p.suffix.upper() == ".CAR"),
        key=lambda p: p.name.upper(),
    )


# Canonical event display names matching src/session_start.cpp canonical_contest_alias()
_EVENT_TYPE_TO_DISPLAY: dict[str, str] = {
    "battle": "Battle (Free Roam)",
    "c_battle": "Battle (Free Roam)",
    "normal": "No-Rules Race",
    "normal_race": "No-Rules Race",
    "c_normal_race": "No-Rules Race",
    "fig8jump": "Figure-8 Jump Race",
    "figure8_jump_race": "Figure-8 Jump Race",
    "c_figure8_jump_race": "Figure-8 Jump Race",
    "fig8": "Figure-8",
    "figure8_race": "Figure-8",
    "c_figure8_race": "Figure-8",
    "road_race": "Jump Race",
    "jump_race": "Jump Race",
    "c_road_race": "Jump Race",
    "c_jump_race": "Jump Race",
    "demo_derby_contest": "Demolition Derby",
    "kill_derby": "Demolition Derby",
    "c_kill_derby": "Demolition Derby",
    "knockout_contest": "Knockout",
    "kill_knock": "Knockout",
    "c_kill_knock": "Knockout",
    "pushoff_contest": "Push-Off",
    "kill_push": "Push-Off",
    "c_pushoff": "Push-Off",
    "save_ring": "Suicide Race",
    "c_save_ring": "Suicide Race",
    "suicide": "Suicide Race",
    "suicide_race": "Suicide Race",
    "c_suicide_race": "Suicide Race",
    "stop_and_go": "Stop-and-Go",
    "stop_and_go_race": "Stop-and-Go",
    "c_stop_and_go_race": "Stop-and-Go",
    "shortcut": "Shortcut Race",
    "shortcut_race": "Shortcut Race",
    "c_shortcut_race": "Shortcut Race",
    "flag_pole": "Flagpole Race",
    "flagpole_race": "Flagpole Race",
    "c_flagpole_race": "Flagpole Race",
    "whip": "Whip-Around",
    "whip_around_race": "Whip-Around",
    "c_whip_around_race": "Whip-Around",
    "chain_race": "Chain Race",
    "fwd_bwd_race": "Forward-Backward",
    "c_fwd_bwd_race": "Forward-Backward",
    "last_man_race": "Last Man",
    "c_last_man_race": "Last Man",
    "school_bus": "School Bus",
    "c_school_bus": "School Bus",
    "trailer_race": "Trailer Race",
    "c_trailer_race": "Trailer Race",
    "pointtopoint": "Point-to-Point",
    "c_pointtopoint": "Point-to-Point",
    "detention": "Detention",
    "c_detention": "Detention",
    "gauntlet": "Gauntlet",
    "c_gauntlet": "Gauntlet",
    "red_rover": "Red Rover",
    "c_red_rover": "Red Rover",
    "wrangle": "Wrangler",
    "c_wrangle": "Wrangler",
    "capture": "Downtown",
    "c_capture": "Downtown",
    "stuntman": "Daredevil",
    "c_stuntman": "Daredevil",
}


@dataclass
class EventTemplateInfo:
    """Metadata linking a canonical EOD event template to its .TRK props, opt_terrain, and ILIN line."""
    display_name: str
    contest_key: str
    va0_member: str | None = None
    opt_terrain_modes: tuple[int, ...] = ()
    ilin_member: str | None = None


def _pick_ilin_for_event(event_type: str, line_basename: str, avail_ilds: list[str]) -> str | None:
    if not avail_ilds:
        return None
    by_l = {n.lower(): n for n in avail_ilds}
    et = (event_type or "").lower()
    lb = (line_basename or "").lower()
    candidates: list[str] = []
    if lb:
        candidates.extend([f"{lb}oval.ild", f"{lb}track.ild", f"{lb}fig8.ild", f"{lb}.ild"])
    if "fig8" in et or "figure8" in et:
        candidates.extend(["fig8.ild", "bw_fig8.ild"])
    elif "oval" in et or "suicide" in et or "whip" in et:
        candidates.extend(["oval.ild", "track.ild"])
    candidates.extend(["track.ild", "oval.ild", "fig8.ild"])
    for c in candidates:
        if c in by_l:
            return by_l[c]
    return avail_ilds[0]


def discover_track_event_templates(
    game_dir: Path | str,
    display_name: str,
    trk_entries: list[dict[str, Any]] | None = None,
) -> dict[str, EventTemplateInfo]:
    """Read real contest/event templates from COMMON.RES#<track>_eves.val and match them to .TRK members."""
    templates: dict[str, EventTemplateInfo] = {}
    va0_by_l: dict[str, str] = {}
    avail_ilds: list[str] = []
    if trk_entries:
        for e in trk_entries:
            nl = e["name"].lower()
            if e["type"] == "VALT" and nl.startswith("c_") and nl.endswith(".va0"):
                va0_by_l[nl] = e["name"]
            elif e["type"] == "ILIN" and nl.endswith(".ild") and e["size"] >= 48:
                avail_ilds.append(e["name"])

    # Always include Battle (Free Roam) first if c_battle.va0 exists or as universal free-roam preset
    battle_va0 = va0_by_l.get("c_battle.va0")
    templates["Battle (Free Roam)"] = EventTemplateInfo(
        display_name="Battle (Free Roam)",
        contest_key="battle",
        va0_member=battle_va0,
        opt_terrain_modes=(),
        ilin_member=_pick_ilin_for_event("battle", "", avail_ilds),
    )

    common = Path(game_dir) / "DATA" / "RES" / "COMMON.RES"
    default_modes: set[int] = set()
    if common.is_file():
        try:
            entries = mb.directory(common)
            want = f"{display_name.lower()}_eves.val"
            hit = next((e for e in entries if e["name"].lower() == want), None)
            if hit:
                raw, _ = mb.payload(common, hit["name"], entries, verify_crc=False)
                table = parse_binary_valt(raw)
                # First pass: track default terrain_mask bit(s) (e.g. terrain_mask = 1024 -> mode 11)
                for sec in table.get("sections", []):
                    pairs = dict(sec.get("pairs", []))
                    tmask = pairs.get("terrain_mask")
                    if tmask:
                        try:
                            mval = int(str(tmask).strip(), 0)
                            for bit in range(16):
                                if mval & (1 << bit):
                                    default_modes.add(bit + 1)
                        except ValueError:
                            pass
                if default_modes:
                    templates["Battle (Free Roam)"].opt_terrain_modes = tuple(sorted(default_modes))

                # Second pass: real contest definitions
                for sec in table.get("sections", []):
                    sname = sec.get("name", "")
                    if not sname or sname.startswith("$"):
                        continue
                    sl = sname.lower()
                    if sl in ("contest_defaults", "eve", "header") or sl.endswith("_eve_defaults"):
                        continue
                    pairs = dict(sec.get("pairs", []))
                    et = str(pairs.get("event_type", "")).strip()
                    if not et and sl not in _EVENT_TYPE_TO_DISPLAY:
                        continue
                    canon = (
                        _EVENT_TYPE_TO_DISPLAY.get(sl)
                        or _EVENT_TYPE_TO_DISPLAY.get(et.lower())
                        or str(pairs.get("display_name") or pairs.get("name") or sname)
                    )
                    ext_tbl = str(pairs.get("external_table", "")).strip().lower()
                    va0_cand = None
                    for cand in (
                        f"{ext_tbl}.va0" if ext_tbl else "",
                        f"c_{et.lower()}.va0" if et else "",
                        f"c_{sl}.va0",
                    ):
                        if cand and cand in va0_by_l:
                            va0_cand = va0_by_l[cand]
                            break
                    modes = set(default_modes)
                    opt_t = pairs.get("opt_terrain")
                    if opt_t:
                        for tok in str(opt_t).replace(",", " ").split():
                            try:
                                mv = int(tok, 0)
                                if 1 <= mv <= 16:
                                    modes.add(mv)
                            except ValueError:
                                pass
                    ilin_m = _pick_ilin_for_event(et or sl, str(pairs.get("line_basename", "")), avail_ilds)
                    if canon not in templates:
                        templates[canon] = EventTemplateInfo(
                            display_name=canon,
                            contest_key=sname,
                            va0_member=va0_cand,
                            opt_terrain_modes=tuple(sorted(modes)),
                            ilin_member=ilin_m,
                        )
        except Exception:
            pass

    # Add any remaining c_*.va0 tables present in .TRK that weren't explicitly named in _eves.val
    for vl, vname in sorted(va0_by_l.items()):
        stem = vl.removesuffix(".va0")
        canon = _EVENT_TYPE_TO_DISPLAY.get(stem)
        if canon and canon not in templates:
            templates[canon] = EventTemplateInfo(
                display_name=canon,
                contest_key=stem,
                va0_member=vname,
                opt_terrain_modes=tuple(sorted(default_modes)),
                ilin_member=_pick_ilin_for_event(stem, "", avail_ilds),
            )

    return templates


def discover_track_events(game_dir: Path | str, display_name: str, trk_entries: list[dict[str, Any]] | None = None) -> list[str]:
    """Return canonical SLUS_209.10 contest/event names for a track."""
    templates = discover_track_event_templates(game_dir, display_name, trk_entries)
    names = [k for k in templates.keys() if k != "Battle (Free Roam)"]
    return names or ["No-Rules Race", "Figure-8 Jump Race", "Jump Race", "Demolition Derby", "Gauntlet"]


def normal_to_pitch_roll(normal: tuple[float, float, float], yaw_deg: float = 0.0) -> tuple[float, float, float]:
    """Compute (Heading, Pitch, Roll) in degrees aligning local +Y to terrain `normal` at `yaw_deg`."""
    n = np.array(normal, dtype=np.float32)
    nl = float(np.linalg.norm(n))
    if nl < 1e-5:
        return (yaw_deg, 0.0, 0.0)
    n = n / nl
    if n[1] < 0.0:
        n = -n
    # Rotate normal into local yaw frame (inverse Ry(yaw))
    y_r = math.radians(-yaw_deg)
    cy, sy = math.cos(y_r), math.sin(y_r)
    lx = cy * float(n[0]) + sy * float(n[2])
    ly = float(n[1])
    lz = -sy * float(n[0]) + cy * float(n[2])
    # Pitch rotates around +X (tilting +Y toward +Z -> +pitch), Roll rotates around +Z (tilting +Y toward -X -> +roll)
    pitch = math.degrees(math.atan2(lz, max(1e-4, ly)))
    roll = math.degrees(math.atan2(-lx, max(1e-4, math.hypot(ly, lz))))
    return (yaw_deg, max(-55.0, min(55.0, pitch)), max(-55.0, min(55.0, roll)))


# Environment / Skybox presets for live OpenGL 3.3 Core atmosphere + mod export
ENVIRONMENT_PRESETS: dict[str, dict[str, Any]] = {
    "Track Default": {
        "sky_zenith": (0.11, 0.21, 0.36),
        "sky_mid": (0.28, 0.45, 0.66),
        "sky_horizon": (0.56, 0.68, 0.79),
        "sky_ground": (0.14, 0.16, 0.20),
        "sun_azimuth": 32.0,
        "sun_elevation": 52.0,
        "sun_color": (0.98, 0.95, 0.88),
        "ambient_intensity": 1.0,
        "fog_color": (0.44, 0.55, 0.68),
        "fog_start": 350.0,
        "fog_end": 2200.0,
        "fog_density": 0.72,
    },
    "Clear Day": {
        "sky_zenith": (0.08, 0.28, 0.62),
        "sky_mid": (0.26, 0.54, 0.84),
        "sky_horizon": (0.72, 0.84, 0.94),
        "sky_ground": (0.18, 0.21, 0.24),
        "sun_azimuth": 45.0,
        "sun_elevation": 64.0,
        "sun_color": (1.00, 0.98, 0.92),
        "ambient_intensity": 1.12,
        "fog_color": (0.64, 0.78, 0.90),
        "fog_start": 480.0,
        "fog_end": 2800.0,
        "fog_density": 0.52,
    },
    "Golden Sunset": {
        "sky_zenith": (0.12, 0.11, 0.28),
        "sky_mid": (0.52, 0.26, 0.34),
        "sky_horizon": (0.95, 0.52, 0.22),
        "sky_ground": (0.14, 0.10, 0.10),
        "sun_azimuth": 255.0,
        "sun_elevation": 14.0,
        "sun_color": (1.00, 0.62, 0.32),
        "ambient_intensity": 0.88,
        "fog_color": (0.76, 0.44, 0.28),
        "fog_start": 260.0,
        "fog_end": 1800.0,
        "fog_density": 0.78,
    },
    "Anoka Night": {
        "sky_zenith": (0.02, 0.03, 0.08),
        "sky_mid": (0.05, 0.08, 0.16),
        "sky_horizon": (0.12, 0.15, 0.24),
        "sky_ground": (0.03, 0.04, 0.06),
        "sun_azimuth": 210.0,
        "sun_elevation": 28.0,
        "sun_color": (0.45, 0.56, 0.82),
        "ambient_intensity": 0.48,
        "fog_color": (0.08, 0.10, 0.17),
        "fog_start": 160.0,
        "fog_end": 1250.0,
        "fog_density": 0.88,
    },
    "Overcast Storm": {
        "sky_zenith": (0.18, 0.21, 0.25),
        "sky_mid": (0.32, 0.36, 0.41),
        "sky_horizon": (0.48, 0.52, 0.56),
        "sky_ground": (0.13, 0.14, 0.16),
        "sun_azimuth": 120.0,
        "sun_elevation": 36.0,
        "sun_color": (0.74, 0.78, 0.84),
        "ambient_intensity": 0.92,
        "fog_color": (0.44, 0.48, 0.52),
        "fog_start": 180.0,
        "fog_end": 1350.0,
        "fog_density": 0.85,
    },
    "Desert Noon": {
        "sky_zenith": (0.14, 0.34, 0.62),
        "sky_mid": (0.42, 0.60, 0.76),
        "sky_horizon": (0.86, 0.78, 0.62),
        "sky_ground": (0.24, 0.19, 0.14),
        "sun_azimuth": 180.0,
        "sun_elevation": 74.0,
        "sun_color": (1.00, 0.94, 0.80),
        "ambient_intensity": 1.18,
        "fog_color": (0.80, 0.72, 0.58),
        "fog_start": 320.0,
        "fog_end": 2100.0,
        "fog_density": 0.65,
    },
}


# ---------------------------------------------------------------------------
# Math helpers (Native EOD Left-Handed: +X right, +Y up, +Z forward)
# ---------------------------------------------------------------------------
def compose_transform(
    pos: tuple[float, float, float],
    rot_hpr: tuple[float, float, float],
    scale: tuple[float, float, float] = (1.0, 1.0, 1.0),
) -> np.ndarray:
    """Build 4x4 row-major transform matrix M (p_world = M @ p_local).
    Heading (H) = yaw around +Y, Pitch (P) = rotation around +X, Roll (R) = rotation around +Z.
    """
    h, p, r = (math.radians(a) for a in rot_hpr)
    ch, sh = math.cos(h), math.sin(h)
    cp, sp = math.cos(p), math.sin(p)
    cr, sr = math.cos(r), math.sin(r)

    # Ry(h) @ Rx(p) @ Rz(r)
    ry = np.array(
        [[ch, 0.0, sh], [0.0, 1.0, 0.0], [-sh, 0.0, ch]],
        dtype=np.float32,
    )
    rx = np.array(
        [[1.0, 0.0, 0.0], [0.0, cp, -sp], [0.0, sp, cp]],
        dtype=np.float32,
    )
    rz = np.array(
        [[cr, -sr, 0.0], [sr, cr, 0.0], [0.0, 0.0, 1.0]],
        dtype=np.float32,
    )
    rot = ry @ rx @ rz
    sx, sy, sz = scale
    rot[:, 0] *= sx
    rot[:, 1] *= sy
    rot[:, 2] *= sz

    m = np.eye(4, dtype=np.float32)
    m[:3, :3] = rot
    m[0, 3], m[1, 3], m[2, 3] = pos
    return m


def decompose_matrix_hpr(m4: np.ndarray) -> tuple[tuple[float, float, float], tuple[float, float, float], tuple[float, float, float]]:
    """Decompose 4x4 column-vector matrix into (pos, rot_hpr_deg, scale)."""
    pos = (float(m4[0, 3]), float(m4[1, 3]), float(m4[2, 3]))
    c0, c1, c2 = m4[:3, 0], m4[:3, 1], m4[:3, 2]
    sx = float(np.linalg.norm(c0)) or 1.0
    sy = float(np.linalg.norm(c1)) or 1.0
    sz = float(np.linalg.norm(c2)) or 1.0
    r = np.column_stack((c0 / sx, c1 / sy, c2 / sz))
    if abs(sx - 1.0) < 1e-4:
        sx = 1.0
    if abs(sy - 1.0) < 1e-4:
        sy = 1.0
    if abs(sz - 1.0) < 1e-4:
        sz = 1.0
    # Ry(h) Rx(p) Rz(r):
    # r[1, 2] = -sin(p)
    sp = max(-1.0, min(1.0, float(-r[1, 2])))
    pitch = math.asin(sp)
    if abs(math.cos(pitch)) > 1e-5:
        heading = math.atan2(float(r[0, 2]), float(r[2, 2]))
        roll = math.atan2(float(r[1, 0]), float(r[1, 1]))
    else:
        heading = math.atan2(float(-r[2, 0]), float(r[0, 0]))
        roll = 0.0
    return (
        pos,
        (math.degrees(heading), math.degrees(pitch), math.degrees(roll)),
        (sx, sy, sz),
    )


def _compute_smooth_normals(verts: np.ndarray, indices: np.ndarray) -> np.ndarray:
    """Compute per-vertex normals from triangle geometry when stored normals are zero/flat."""
    norms = np.zeros_like(verts, dtype=np.float32)
    if len(indices) == 0:
        norms[:, 1] = 1.0
        return norms
    tris = indices.reshape(-1, 3)
    v0 = verts[tris[:, 0]]
    v1 = verts[tris[:, 1]]
    v2 = verts[tris[:, 2]]
    fn = np.cross(v1 - v0, v2 - v0)
    # Orient flat/terrain fallback normals upward when winding is inconsistent
    flip = fn[:, 1] < -1e-4
    fn[flip] = -fn[flip]
    np.add.at(norms, tris[:, 0], fn)
    np.add.at(norms, tris[:, 1], fn)
    np.add.at(norms, tris[:, 2], fn)
    lens = np.linalg.norm(norms, axis=1, keepdims=True)
    bad = (~np.isfinite(lens[:, 0])) | (lens[:, 0] < 1e-6)
    norms[~bad] /= lens[~bad]
    norms[bad] = np.array([0.0, 1.0, 0.0], dtype=np.float32)
    return norms


# ---------------------------------------------------------------------------
# Data classes
# ---------------------------------------------------------------------------
@dataclass
class MeshPart:
    texture: str
    # Interleaved float32: [px, py, pz, nx, ny, nz, u, v] (8 floats per vert)
    vertices: np.ndarray  # shape (N, 8), float32
    indices: np.ndarray   # shape (M,), uint32
    tri_count: int


@dataclass
class WorldModel:
    key: str
    name: str
    category: str
    model_store: str
    hash_hex: str
    parts: list[MeshPart]
    tri_count: int
    textures: list[str]
    bbox_min: np.ndarray  # shape (3,), local space
    bbox_max: np.ndarray  # shape (3,), local space
    pivot: tuple[float, float, float]
    placed_by_map: bool = True
    model_offset: int | None = None
    subtype: str = "Simple Model"


@dataclass
class WorldObject:
    obj_id: str
    name: str
    model_key: str
    source: str  # "Retail" | "Placed by the editor"
    category: str
    subtype: str
    section_mlist: str
    model_store: str
    hash_hex: str
    tri_count: int
    textures: list[str]
    position: tuple[float, float, float]
    rotation_hpr: tuple[float, float, float] = (0.0, 0.0, 0.0)
    scale: tuple[float, float, float] = (1.0, 1.0, 1.0)
    written: bool = True
    visible: bool = True
    deleted: bool = False
    has_custom_collision: bool = False
    # Retail provenance for mod write-back
    retail_instance_id: str | None = None
    retail_record_offset: int | None = None
    retail_model_offset: int | None = None
    retail_initial_pos: tuple[float, float, float] = (0.0, 0.0, 0.0)
    retail_initial_rot: tuple[float, float, float] = (0.0, 0.0, 0.0)
    retail_event_member: str | None = None
    retail_event_section: str | None = None
    retail_event_prop_index: int | None = None
    retail_original_model_key: str | None = None
    opt_terrain_mode: int | None = None

    def is_transformed(self) -> bool:
        if self.source != "Retail":
            return True
        moved = any(abs(a - b) > 1e-4 for a, b in zip(self.position, self.retail_initial_pos))
        rotated = any(abs(a - b) > 1e-4 for a, b in zip(self.rotation_hpr, self.retail_initial_rot))
        scaled = any(abs(s - 1.0) > 1e-4 for s in self.scale)
        return moved or rotated or scaled

    def world_matrix(self) -> np.ndarray:
        return compose_transform(self.position, self.rotation_hpr, self.scale)

    def world_aabb(self, model: WorldModel) -> tuple[np.ndarray, np.ndarray]:
        lo, hi = model.bbox_min, model.bbox_max
        corners = np.array(
            [
                [lo[0], lo[1], lo[2], 1.0],
                [hi[0], lo[1], lo[2], 1.0],
                [lo[0], hi[1], lo[2], 1.0],
                [hi[0], hi[1], lo[2], 1.0],
                [lo[0], lo[1], hi[2], 1.0],
                [hi[0], lo[1], hi[2], 1.0],
                [lo[0], hi[1], hi[2], 1.0],
                [hi[0], hi[1], hi[2], 1.0],
            ],
            dtype=np.float32,
        )
        m = self.world_matrix()
        w = (corners @ m.T)[:, :3]
        return w.min(axis=0), w.max(axis=0)


@dataclass
class TextureAsset:
    name: str
    width: int
    height: int
    rgba: bytes
    alpha_kind: str  # "opaque" | "cutout" | "blend"
    average_rgb: tuple[int, int, int]
    asset_id: str
    orig_width: int = 0
    orig_height: int = 0
    orig_rgba: bytes = b""
    raw_tex_bytes: bytes | None = None
    index_key: str | None = None
    palette_key: str | None = None
    is_replaced: bool = False
    replaced_png_path: str | None = None

    def __post_init__(self) -> None:
        if not self.orig_width:
            self.orig_width = self.width
        if not self.orig_height:
            self.orig_height = self.height
        if not self.orig_rgba:
            self.orig_rgba = self.rgba

    def ensure_identity(self) -> tuple[str | None, str | None]:
        """Lazily compute FNV-1a 64 runtime indexKey & paletteKey for HOSTTEX export."""
        if self.index_key is not None:
            return self.index_key, self.palette_key
        if self.raw_tex_bytes:
            try:
                ident = tex_id.texture_identity(self.raw_tex_bytes)
                self.index_key = ident["indexKey"]
                self.palette_key = ident["paletteKey"]
            except Exception:
                self.index_key = None
                self.palette_key = None
        return self.index_key, self.palette_key


@dataclass
class RoadNode:
    node_id: int
    x: float
    y: float
    z: float
    line_name: str = "track.ild"
    is_side: bool = False
    retail_index: int | None = None
    lanes: int = 2
    width: float = 14.0
    two_way: bool = False


@dataclass
class RoadEdge:
    a: int
    b: int
    lanes: int = 2
    width: float = 14.0
    two_way: bool = False


@dataclass
class RoadGraph:
    line_name: str
    available_lines: list[str] = field(default_factory=list)
    nodes: dict[int, RoadNode] = field(default_factory=dict)
    edges: list[RoadEdge] = field(default_factory=list)
    next_id: int = 1
    dirty: bool = False

    def neighbors(self, node_id: int) -> list[int]:
        out: list[int] = []
        for e in self.edges:
            if e.a == node_id:
                out.append(e.b)
            elif e.b == node_id:
                out.append(e.a)
        return out

    def node_kind(self, node_id: int) -> str:
        deg = len(self.neighbors(node_id))
        if deg <= 1:
            return "road end"
        if deg == 2:
            return "waypoint"
        return "junction"

    def node_direction(self, node_id: int) -> tuple[float, float, float]:
        n = self.nodes.get(node_id)
        if not n:
            return (0.0, 0.0, 1.0)
        outs = [e.b for e in self.edges if e.a == node_id and e.b in self.nodes]
        ins = [e.a for e in self.edges if e.b == node_id and e.a in self.nodes]
        dx = dy = dz = 0.0
        if outs:
            nb = self.nodes[outs[0]]
            dx += nb.x - n.x
            dy += nb.y - n.y
            dz += nb.z - n.z
        if ins:
            pa = self.nodes[ins[0]]
            dx += n.x - pa.x
            dy += n.y - pa.y
            dz += n.z - pa.z
        length = math.sqrt(dx * dx + dy * dy + dz * dz)
        if length < 1e-5:
            return (0.0, 0.0, 1.0)
        return (dx / length, dy / length, dz / length)

    def add_node(
        self,
        x: float,
        y: float,
        z: float,
        lanes: int = 2,
        width: float = 14.0,
        two_way: bool = False,
        line_name: str | None = None,
        retail_index: int | None = None,
        is_side: bool = False,
    ) -> RoadNode:
        nid = self.next_id
        self.next_id += 1
        node = RoadNode(
            node_id=nid,
            x=float(x),
            y=float(y),
            z=float(z),
            line_name=line_name or self.line_name,
            is_side=is_side,
            retail_index=retail_index,
            lanes=lanes,
            width=width,
            two_way=two_way,
        )
        self.nodes[nid] = node
        return node

    def add_edge(self, a: int, b: int, lanes: int = 2, width: float = 14.0, two_way: bool = False) -> bool:
        if a == b or a not in self.nodes or b not in self.nodes:
            return False
        for e in self.edges:
            if (e.a == a and e.b == b) or (e.a == b and e.b == a):
                return False
        self.edges.append(RoadEdge(a=a, b=b, lanes=lanes, width=width, two_way=two_way))
        return True

    def remove_node(self, node_id: int) -> tuple[RoadNode | None, list[RoadEdge]]:
        node = self.nodes.pop(node_id, None)
        removed_edges = [e for e in self.edges if e.a == node_id or e.b == node_id]
        self.edges = [e for e in self.edges if e.a != node_id and e.b != node_id]
        return node, removed_edges


@dataclass
class WorldMarker:
    marker_id: str
    name: str
    kind: str  # "Marker" | "Car" | "Breaker" | "Spawn" | "Start Grid" | "Chicken" | "Trigger"
    position: tuple[float, float, float]
    heading_deg: float = 0.0
    source: str = "Placed by the editor"
    note: str = ""


@dataclass
class EnvironmentState:
    preset_name: str = "Track Default"
    sky_zenith: tuple[float, float, float] = (0.11, 0.21, 0.36)
    sky_mid: tuple[float, float, float] = (0.28, 0.45, 0.66)
    sky_horizon: tuple[float, float, float] = (0.56, 0.68, 0.79)
    sky_ground: tuple[float, float, float] = (0.14, 0.16, 0.20)
    sun_azimuth: float = 32.0
    sun_elevation: float = 52.0
    sun_color: tuple[float, float, float] = (0.98, 0.95, 0.88)
    ambient_intensity: float = 1.0
    fog_color: tuple[float, float, float] = (0.44, 0.55, 0.68)
    fog_start: float = 350.0
    fog_end: float = 2200.0
    fog_density: float = 0.72
    borrowed_sky_track: str | None = None

    def apply_preset(self, preset_name: str) -> None:
        cfg = ENVIRONMENT_PRESETS.get(preset_name)
        if not cfg:
            return
        self.preset_name = preset_name
        self.sky_zenith = tuple(cfg["sky_zenith"])
        self.sky_mid = tuple(cfg["sky_mid"])
        self.sky_horizon = tuple(cfg["sky_horizon"])
        self.sky_ground = tuple(cfg["sky_ground"])
        self.sun_azimuth = float(cfg["sun_azimuth"])
        self.sun_elevation = float(cfg["sun_elevation"])
        self.sun_color = tuple(cfg["sun_color"])
        self.ambient_intensity = float(cfg["ambient_intensity"])
        self.fog_color = tuple(cfg["fog_color"])
        self.fog_start = float(cfg["fog_start"])
        self.fog_end = float(cfg["fog_end"])
        self.fog_density = float(cfg["fog_density"])

    def sun_direction(self) -> tuple[float, float, float]:
        az = math.radians(self.sun_azimuth)
        el = math.radians(self.sun_elevation)
        ce = math.cos(el)
        return (math.sin(az) * ce, math.sin(el), math.cos(az) * ce)

    def to_dict(self) -> dict[str, Any]:
        return {
            "preset_name": self.preset_name,
            "sky_zenith": list(self.sky_zenith),
            "sky_mid": list(self.sky_mid),
            "sky_horizon": list(self.sky_horizon),
            "sky_ground": list(self.sky_ground),
            "sun_azimuth": self.sun_azimuth,
            "sun_elevation": self.sun_elevation,
            "sun_color": list(self.sun_color),
            "ambient_intensity": self.ambient_intensity,
            "fog_color": list(self.fog_color),
            "fog_start": self.fog_start,
            "fog_end": self.fog_end,
            "fog_density": self.fog_density,
            "borrowed_sky_track": self.borrowed_sky_track,
        }

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "EnvironmentState":
        st = cls()
        if not isinstance(d, dict):
            return st
        st.preset_name = str(d.get("preset_name", st.preset_name))
        for k in ("sky_zenith", "sky_mid", "sky_horizon", "sky_ground", "sun_color", "fog_color"):
            if k in d and len(d[k]) == 3:
                setattr(st, k, tuple(float(v) for v in d[k]))
        for k in ("sun_azimuth", "sun_elevation", "ambient_intensity", "fog_start", "fog_end", "fog_density"):
            if k in d:
                setattr(st, k, float(d[k]))
        st.borrowed_sky_track = d.get("borrowed_sky_track")
        return st


@dataclass
class CustomRaceConfig:
    event_name: str = "No-Rules Race"
    event_type: str = "No-Rules Race"
    laps: int = 5
    ai_count: int = 8
    player_car: str = "Algar"
    ai_speed_scale: float = 1.0
    ilin_line: str = "track.ild"
    free_roam_place: str = "home"
    arcade_mode: str = "free_roam"
    sync_all_events: bool = True
    include_lod_twins: bool = True

    def to_dict(self) -> dict[str, Any]:
        return {
            "event_name": self.event_name,
            "event_type": self.event_type,
            "laps": self.laps,
            "ai_count": self.ai_count,
            "player_car": self.player_car,
            "ai_speed_scale": self.ai_speed_scale,
            "ilin_line": self.ilin_line,
            "free_roam_place": self.free_roam_place,
            "arcade_mode": self.arcade_mode,
            "sync_all_events": self.sync_all_events,
            "include_lod_twins": self.include_lod_twins,
        }

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "CustomRaceConfig":
        cfg = cls()
        if not isinstance(d, dict):
            return cfg
        cfg.event_name = str(d.get("event_name", cfg.event_name))
        cfg.event_type = str(d.get("event_type", cfg.event_type))
        cfg.laps = int(d.get("laps", cfg.laps))
        cfg.ai_count = int(d.get("ai_count", cfg.ai_count))
        cfg.player_car = str(d.get("player_car", cfg.player_car))
        cfg.ai_speed_scale = float(d.get("ai_speed_scale", cfg.ai_speed_scale))
        cfg.ilin_line = str(d.get("ilin_line", cfg.ilin_line))
        cfg.free_roam_place = str(d.get("free_roam_place", cfg.free_roam_place))
        cfg.arcade_mode = str(d.get("arcade_mode", cfg.arcade_mode))
        cfg.sync_all_events = bool(d.get("sync_all_events", cfg.sync_all_events))
        cfg.include_lod_twins = bool(d.get("include_lod_twins", cfg.include_lod_twins))
        return cfg


def parse_bppt(data: bytes) -> dict[int, dict[str, Any]]:
    """Parse EOD `track.bpp` (`BPPT` v7) optional-terrain 2D BSP collision trees.

    Returns dict mapping 1-based `opt_terrain_mode` (1..16) -> slot metadata
    (matching SLUS_209.10 `f_00216F60` / `f_00217240` / `f_002176C8`).
    """
    slots: dict[int, dict[str, Any]] = {}
    if not data or len(data) < 68:
        return slots
    slot_offs = struct.unpack_from("<16I", data, 4)
    for k, tree_off in enumerate(slot_offs):
        if tree_off == 0 or tree_off + 32 > len(data):
            continue
        hdr = struct.unpack_from("<8I", data, tree_off)
        n_planes, n_nodes = hdr[1], hdr[2]
        planes_off = tree_off + hdr[4]
        nodes_off = tree_off + hdr[5]
        root = hdr[7]
        if (
            n_planes == 0
            or n_nodes == 0
            or planes_off + n_planes * 20 > len(data)
            or nodes_off + n_nodes * 20 > len(data)
            or root >= n_nodes
        ):
            continue
        slots[k + 1] = {
            "mode": k + 1,
            "slot_index": k,
            "tree_off": tree_off,
            "n_planes": n_planes,
            "n_nodes": n_nodes,
            "planes_off": planes_off,
            "nodes_off": nodes_off,
            "root": root,
        }
    return slots


def query_bppt_plane(data: bytes, slot: dict[str, Any], x: float, z: float) -> int | None:
    """Walk a `track.bpp` 2D BSP tree at (x, z) and return the leaf plane index, or None."""
    nodes_off = slot["nodes_off"]
    n_nodes = slot["n_nodes"]
    n_planes = slot["n_planes"]
    cur = slot["root"]
    for _ in range(min(n_nodes + 4, 2048)):
        if cur < 0 or cur >= n_nodes:
            return None
        nx, nz, d, left, right = struct.unpack_from("<3f2i", data, nodes_off + cur * 20)
        child = left if (nx * x + nz * z + d) <= 0.0 else right
        if child < 0:
            leaf = child & 0x7FFFFFFF
            if leaf == 0x7FFFFFFF or leaf >= n_planes:
                return None
            return int(leaf)
        cur = child
    return None


class _LazyTerrainDescriptor:
    """Descriptor that lazily builds wc.Terrain from raw SCOL bytes on first access."""

    def __get__(self, obj: Any, objtype: Any = None) -> wc.Terrain | None:
        if obj is None:
            return None
        if obj._terrain is None and obj._scol_raw:
            try:
                obj._terrain = wc.Terrain(wc.parse_scol(obj._scol_raw))
            except Exception:
                obj._terrain = None
        return obj._terrain

    def __set__(self, obj: Any, val: wc.Terrain | None) -> None:
        obj._terrain = val


@dataclass
class CollisionData:
    scol_count: int
    sobl_count: int
    sobl_boxes: int
    sobl_tubes: int
    terrain: wc.Terrain | None = _LazyTerrainDescriptor()  # type: ignore[assignment]
    scol_line_verts: np.ndarray = field(default_factory=lambda: np.zeros((0, 3), dtype=np.float32))
    sobl_line_verts: np.ndarray = field(default_factory=lambda: np.zeros((0, 3), dtype=np.float32))
    sobl_line_colors: np.ndarray = field(default_factory=lambda: np.zeros((0, 4), dtype=np.float32))
    sobl_records: list[dict[str, Any]] = field(default_factory=list)
    sobl_edits: dict[int, dict[str, Any]] = field(default_factory=dict)  # rec_idx -> {"field", "value", "ry", "centre", ...}
    custom_boxes: list[dict[str, Any]] = field(default_factory=list)
    bppt_slots: dict[int, dict[str, Any]] = field(default_factory=dict)
    bppt_line_verts: np.ndarray = field(default_factory=lambda: np.zeros((0, 3), dtype=np.float32))
    scol_base_line_verts: np.ndarray = field(default_factory=lambda: np.zeros((0, 3), dtype=np.float32))
    _scol_raw: bytes | None = field(default=None, repr=False)
    _terrain: wc.Terrain | None = field(default=None, init=False, repr=False)

    def __post_init__(self) -> None:
        if len(self.scol_base_line_verts) == 0 and len(self.scol_line_verts) > 0:
            self.scol_base_line_verts = self.scol_line_verts.copy()

    def rebuild_dynamic_ramp_collision(self, world: "TrackWorld") -> None:
        """Sync live green collision wireframe for optional-terrain & edited static ramp meshes."""
        ramp_segs: list[np.ndarray] = []
        for obj in world.objects:
            if obj.deleted or not obj.visible:
                continue
            is_opt_ramp = obj.category == "optional-terrain"
            is_edited_static_ramp = (
                obj.category == "static"
                and obj.is_transformed()
                and any(tok in obj.name.lower() for tok in ("ramp", "dirt", "jump", "bridge", "hill"))
            )
            if not is_opt_ramp and not is_edited_static_ramp:
                continue
            model = world.models.get(obj.model_key)
            if model is None:
                continue
            m4 = obj.world_matrix()
            for part in model.parts:
                if len(part.indices) < 3:
                    continue
                pts = part.vertices[:, 0:3]
                ones = np.ones((len(pts), 1), dtype=np.float32)
                wpts = (np.hstack([pts, ones]) @ m4.T)[:, :3]
                wpts[:, 1] += 0.11  # lift slightly above visual ramp so green wireframe reads cleanly
                tris = part.indices.reshape(-1, 3)
                segs = np.empty((len(tris) * 6, 3), dtype=np.float32)
                segs[0::6] = wpts[tris[:, 0]]
                segs[1::6] = wpts[tris[:, 1]]
                segs[2::6] = wpts[tris[:, 1]]
                segs[3::6] = wpts[tris[:, 2]]
                segs[4::6] = wpts[tris[:, 2]]
                segs[5::6] = wpts[tris[:, 0]]
                ramp_segs.append(segs)

        if ramp_segs:
            self.bppt_line_verts = np.vstack(ramp_segs)
            if len(self.scol_base_line_verts) > 0:
                self.scol_line_verts = np.vstack([self.scol_base_line_verts, self.bppt_line_verts])
            else:
                self.scol_line_verts = self.bppt_line_verts.copy()
        else:
            self.bppt_line_verts = np.zeros((0, 3), dtype=np.float32)
            self.scol_line_verts = self.scol_base_line_verts.copy()

    def ground_height(self, x: float, z: float, below: float | None = None) -> float:
        if self.terrain is None:
            return 0.0
        res = self.terrain.ground(x, z, below=below)
        return float(res["y"])

    def ground_sample(
        self, x: float, z: float, below: float | None = None
    ) -> tuple[float, tuple[float, float, float]]:
        """Return (ground_y, unit_upward_normal) from SCOL terrain triangle plane."""
        if self.terrain is None:
            return 0.0, (0.0, 1.0, 0.0)
        res = self.terrain.ground(x, z, below=below)
        y = float(res["y"])
        tri_idx = res.get("tri", -1)
        if 0 <= tri_idx < len(self.terrain.tris):
            a, b, c, _d = self.terrain.tris[tri_idx]["plane"]
            if b < 0.0:
                a, b, c = -a, -b, -c
            nl = math.sqrt(a * a + b * b + c * c)
            if nl > 1e-6:
                return y, (float(a / nl), float(b / nl), float(c / nl))
        return y, (0.0, 1.0, 0.0)

    def rebuild_sobl_lines(self) -> None:
        """Rebuild height-color-coded SOBL wireframe buffers (Red >=10m, Orange 3-10m, Yellow <3m)."""
        pts: list[list[float]] = []
        cols: list[list[float]] = []
        for rec in self.sobl_records:
            if rec.get("kind") != "box":
                continue
            corners = wc.box_corners(rec)
            above = float(rec.get("above_ground", 0.0))
            if above >= 10.0:
                c = [0.98, 0.24, 0.22, 0.92]  # Red: tall invisible wall
            elif above >= 3.0:
                c = [0.98, 0.58, 0.16, 0.88]  # Orange: medium barrier
            elif above >= 0.0:
                c = [0.96, 0.88, 0.22, 0.82]  # Yellow: low curb
            else:
                c = [0.30, 0.78, 0.42, 0.45]  # Dim green: flattened below ground
            for a, b in wc.BOX_EDGES:
                pts.append(corners[a])
                pts.append(corners[b])
                cols.append(c)
                cols.append(c)
        if pts:
            self.sobl_line_verts = np.array(pts, dtype=np.float32)
            self.sobl_line_colors = np.array(cols, dtype=np.float32)
        else:
            self.sobl_line_verts = np.zeros((0, 3), dtype=np.float32)
            self.sobl_line_colors = np.zeros((0, 4), dtype=np.float32)

    def apply_barrier_height_in_memory(self, rec_index: int, target_top_above_ground: float) -> dict[str, Any]:
        """Compute & apply a valid shrink-only SOBL height edit in memory, updating wireframe."""
        rec = self.sobl_records[rec_index]
        # Build original-state proxy so we always plan against retail bounds safely
        orig_rec = dict(rec)
        orig_rec["half_extents"] = list(rec["orig_half_extents"])
        orig_rec["centre"] = list(rec["orig_centre"])
        orig_rec["sphere_radius"] = float(rec["orig_sphere_radius"])
        try:
            plan = wc.plan_box_edit(orig_rec, "top-above-ground", float(target_top_above_ground), "bottom", self.terrain)
            field_used = "top-above-ground"
            val_used = float(target_top_above_ground)
        except ValueError:
            # Fallback: shrink ry to safe minimum 0.02 m (flattens barrier below ground!)
            plan = wc.plan_box_edit(orig_rec, "ry", 0.02, "bottom", self.terrain)
            field_used = "ry"
            val_used = 0.02
        rec["half_extents"][1] = float(plan["ry"])
        rec["centre"] = list(plan["centre"])
        rec["sphere_radius"] = float(plan["sphere_radius"])
        lo, hi = wc.vertical_span(rec)
        rec["bottom_y"] = lo
        rec["top_y"] = hi
        rec["above_ground"] = hi - float(rec["ground_y"])
        rec["modified"] = True
        edit_info = {
            "index": rec_index,
            "field": field_used,
            "value": val_used,
            "ry": float(plan["ry"]),
            "centre": list(plan["centre"]),
            "sphere_radius": float(plan["sphere_radius"]),
        }
        self.sobl_edits[rec_index] = edit_info
        return edit_info

    def restore_barrier_state(self, rec_index: int, state: dict[str, Any] | None) -> None:
        """Restore a barrier to either a saved edit dict or its original retail geometry."""
        rec = self.sobl_records[rec_index]
        if state is None:
            rec["half_extents"] = list(rec["orig_half_extents"])
            rec["centre"] = list(rec["orig_centre"])
            rec["sphere_radius"] = float(rec["orig_sphere_radius"])
            rec["modified"] = False
            self.sobl_edits.pop(rec_index, None)
        else:
            rec["half_extents"][1] = float(state["ry"])
            rec["centre"] = list(state["centre"])
            rec["sphere_radius"] = float(state["sphere_radius"])
            rec["modified"] = True
            self.sobl_edits[rec_index] = dict(state)
        lo, hi = wc.vertical_span(rec)
        rec["bottom_y"] = lo
        rec["top_y"] = hi
        rec["above_ground"] = hi - float(rec["ground_y"])


@dataclass
class TrackWorld:
    game_dir: Path
    trk_path: Path
    trk_rel: str
    track_name: str
    display_name: str
    entries: list[dict[str, Any]]
    graf_summary: dict[str, Any]
    mlist_info: list[dict[str, Any] | None]
    models: dict[str, WorldModel]
    objects: list[WorldObject]
    textures: dict[str, TextureAsset]
    collision: CollisionData
    road_graph: RoadGraph
    all_road_graphs: dict[str, RoadGraph]
    markers: list[WorldMarker]
    world_center: tuple[float, float, float]
    world_radius: float
    environment: EnvironmentState = field(default_factory=EnvironmentState)
    custom_race: CustomRaceConfig = field(default_factory=CustomRaceConfig)
    available_events: list[str] = field(default_factory=list)
    event_templates: dict[str, EventTemplateInfo] = field(default_factory=dict)
    active_event_template: str = "All Templates (Combined)"
    sync_all_events: bool = True
    include_lod_twins: bool = True
    car_catalog_loaded: bool = False
    car_count: int = 0
    loaded_from_cache: bool = False
    load_ms: float = 0.0
    cache_path: str | None = None
    _cache_backing: bytes | None = field(default=None, repr=False)

    @property
    def total_triangles(self) -> int:
        return sum(o.tri_count for o in self.objects if not o.deleted and o.visible)

    @property
    def active_objects(self) -> list[WorldObject]:
        return [o for o in self.objects if not o.deleted]

    def object_by_id(self, obj_id: str) -> WorldObject | None:
        for o in self.objects:
            if o.obj_id == obj_id and not o.deleted:
                return o
        return None

    def object_visible_in_event_template(self, obj: WorldObject, template_name: str | None = None) -> bool:
        """Return whether `obj` belongs to the active Event Template filter in the 3D viewport."""
        if obj.deleted or not obj.visible:
            return False
        if obj.source != "Retail":
            return True
        tpl_name = template_name if template_name is not None else self.active_event_template
        if not tpl_name or tpl_name.lower().startswith("all templates") or tpl_name.lower().startswith("all ramps"):
            # In combined view, show all geometry + Battle / primary event props (avoiding 25-way duplicates)
            if obj.category == "event-prop" and obj.retail_event_member:
                preferred_va0 = None
                if "Battle (Free Roam)" in self.event_templates and self.event_templates["Battle (Free Roam)"].va0_member:
                    preferred_va0 = self.event_templates["Battle (Free Roam)"].va0_member
                elif self.event_templates:
                    preferred_va0 = next((t.va0_member for t in self.event_templates.values() if t.va0_member), None)
                if preferred_va0 and obj.retail_event_member.lower() != preferred_va0.lower():
                    return obj.is_transformed()
            return True

        tpl = self.event_templates.get(tpl_name)
        if tpl is None:
            return True
        if obj.category == "event-prop":
            if not tpl.va0_member:
                return obj.is_transformed()
            return (obj.retail_event_member or "").lower() == tpl.va0_member.lower()
        if obj.category == "optional-terrain":
            if obj.opt_terrain_mode is None:
                return True
            if obj.opt_terrain_mode in tpl.opt_terrain_modes:
                return True
            # When Sync All Events is ON, edited optional-terrain ramps stay visible in every template!
            if self.sync_all_events and obj.is_transformed():
                return True
            return False
        return True

    def find_similar_objects(self, seed: WorldObject) -> list[WorldObject]:
        """Find all visible active objects sharing the same model, primary texture, or prefix family."""
        if seed is None or seed.deleted:
            return []
        seed_tex = seed.textures[0].lower() if seed.textures else ""
        seed_prefix = seed.name.split("_")[0].split("(")[0].strip().lower()
        out: list[WorldObject] = []
        for obj in self.objects:
            if not self.object_visible_in_event_template(obj):
                continue
            if obj.model_key == seed.model_key:
                out.append(obj)
                continue
            if seed_tex and obj.textures and obj.textures[0].lower() == seed_tex and obj.category == seed.category:
                out.append(obj)
                continue
            if (
                not seed_tex
                and seed_prefix
                and len(seed_prefix) >= 3
                and obj.category == seed.category
                and obj.name.split("_")[0].split("(")[0].strip().lower() == seed_prefix
            ):
                out.append(obj)
        return out

    def find_overlapping_lod_twins_and_barriers(
        self, targets: list[WorldObject]
    ) -> tuple[list[WorldObject], list[int]]:
        """Find co-located LOD/billboard twins and overlapping vertical SOBL barriers for `targets`."""
        target_ids = {o.obj_id for o in targets}
        target_boxes: list[tuple[np.ndarray, np.ndarray, WorldObject]] = []
        for o in targets:
            m = self.models.get(o.model_key)
            if m is None:
                continue
            lo, hi = o.world_aabb(m)
            target_boxes.append((lo, hi, o))

        twins: list[WorldObject] = []
        seen_twins: set[str] = set()
        for other in self.objects:
            if other.deleted or other.obj_id in target_ids or other.obj_id in seen_twins:
                continue
            if other.source != "Retail" or other.category not in ("billboard", "static", "optional-terrain"):
                continue
            om = self.models.get(other.model_key)
            if om is None:
                continue
            olo, ohi = other.world_aabb(om)
            other_low = other.name.lower()
            is_lod_like = (
                other.category == "billboard"
                or other_low.startswith("lod")
                or other.subtype in ("Point-Facing", "Axis-Facing", "Multi-Facing")
            )
            for lo, hi, src in target_boxes:
                src_low = src.name.lower()
                # Check tight 3D proximity / overlapping footprint
                dx = max(0.0, float(olo[0] - hi[0]), float(lo[0] - ohi[0]))
                dy = max(0.0, float(olo[1] - hi[1]), float(lo[1] - ohi[1]))
                dz = max(0.0, float(olo[2] - hi[2]), float(lo[2] - ohi[2]))
                dist_p = math.hypot(other.position[0] - src.position[0], other.position[2] - src.position[2])
                if is_lod_like and (dist_p <= 4.5 or (dx <= 1.2 and dz <= 1.2 and dy <= 2.5)):
                    # Only grab LOD/billboard twin if footprint is similar scale (don't grab giant terrain!)
                    other_span = max(float(ohi[0] - olo[0]), float(ohi[2] - olo[2]))
                    src_span = max(float(hi[0] - lo[0]), float(hi[2] - lo[2]), 4.0)
                    if other_span <= src_span * 2.5 + 4.0:
                        twins.append(other)
                        seen_twins.add(other.obj_id)
                        break
                elif (
                    (src_low.startswith("lod") or src.category == "billboard")
                    and dist_p <= 3.5
                    and abs(other.position[1] - src.position[1]) <= 3.5
                ):
                    other_span = max(float(ohi[0] - olo[0]), float(ohi[2] - olo[2]))
                    if other_span <= 30.0:
                        twins.append(other)
                        seen_twins.add(other.obj_id)
                        break

        barrier_indices: list[int] = []
        seen_b: set[int] = set()
        for rec in self.collision.sobl_records:
            if rec.get("kind") != "box" or not rec.get("vertical"):
                continue
            if float(rec.get("above_ground", 0.0)) <= -0.4:
                continue
            ridx = int(rec["index"])
            if ridx in seen_b:
                continue
            cx, cy, cz = rec["centre"]
            fx, fz = wc.footprint(rec)
            rlo_x, rhi_x = cx - fx, cx + fx
            rlo_z, rhi_z = cz - fz, cz + fz
            for lo, hi, _src in target_boxes:
                if (
                    rhi_x >= float(lo[0]) - 1.2
                    and rlo_x <= float(hi[0]) + 1.2
                    and rhi_z >= float(lo[2]) - 1.2
                    and rlo_z <= float(hi[2]) + 1.2
                ):
                    barrier_indices.append(ridx)
                    seen_b.add(ridx)
                    break

        return twins, barrier_indices


# ---------------------------------------------------------------------------
# Mesh packing & texture decoding
# ---------------------------------------------------------------------------
def _pack_decoded_parts(
    decoded_parts: list[dict[str, Any]],
    subtract_pivot: tuple[float, float, float] | None = None,
) -> tuple[list[MeshPart], int, np.ndarray, np.ndarray, list[str]]:
    px_off, py_off, pz_off = subtract_pivot or (0.0, 0.0, 0.0)
    mesh_parts: list[MeshPart] = []
    total_tris = 0
    lo = np.array([math.inf, math.inf, math.inf], dtype=np.float32)
    hi = np.array([-math.inf, -math.inf, -math.inf], dtype=np.float32)
    tex_names: set[str] = set()

    for part in decoded_parts:
        verts = part.get("verts") or []
        tris = part.get("tris") or []
        if not verts or not tris:
            continue
        pos_arr = np.array(
            [(v["p"][0] - px_off, v["p"][1] - py_off, v["p"][2] - pz_off) for v in verts],
            dtype=np.float32,
        )
        uv_arr = np.array([v["uv"] for v in verts], dtype=np.float32)
        idx_arr = np.array(tris, dtype=np.uint32).reshape(-1)
        # Compute consistent upward-oriented geometric normals (PS2 GRAF strips have mixed winding
        # and often store packed lighting/color bytes in the VIF normal lane).
        nrm_arr = _compute_smooth_normals(pos_arr, idx_arr)

        interleaved = np.empty((len(verts), 8), dtype=np.float32)
        interleaved[:, 0:3] = pos_arr
        interleaved[:, 3:6] = nrm_arr
        interleaved[:, 6:8] = uv_arr

        lo = np.minimum(lo, pos_arr.min(axis=0))
        hi = np.maximum(hi, pos_arr.max(axis=0))

        tex = part.get("texture") or ""
        if tex:
            tex_names.add(tex)
        mesh_parts.append(
            MeshPart(
                texture=tex,
                vertices=interleaved,
                indices=idx_arr,
                tri_count=len(tris),
            )
        )
        total_tris += len(tris)

    if total_tris == 0:
        lo = np.array([-0.5, -0.5, -0.5], dtype=np.float32)
        hi = np.array([0.5, 0.5, 0.5], dtype=np.float32)
    return mesh_parts, total_tris, lo, hi, sorted(tex_names)


_WATER_TEX_TOKENS = ("sween", "water", "h2o", "pond", "puddle", "river", "lake")


def _load_textures(track_dict: dict[str, Any], tex_names: set[str]) -> dict[str, TextureAsset]:
    """Decode all referenced EOD textures using tools/world_export + tools/texture_preview."""
    cache: dict[str, tuple[str | None, bytes | None]] = {}
    out: dict[str, TextureAsset] = {}
    for name in sorted(tex_names):
        if not name or name.lower() in ("-", ".tex"):
            continue
        asset_id, raw = we.resolve_texture(track_dict, name, cache)
        if not asset_id or not raw:
            continue
        try:
            w, h, rgba, _meta = decode_preview(raw)
        except Exception:
            continue
        alphas = rgba[3::4]
        low_name = name.lower()
        is_water_tex = any(tok in low_name for tok in _WATER_TEX_TOKENS)
        max_a = max(alphas) if alphas else 255
        min_a = min(alphas) if alphas else 255
        # PS2 water/ripple overlays (e.g. Afton's `sween.tex`, raw alpha 18..73, zero==0%)
        # carry genuine translucent blend alpha rather than opaque road detail weight.
        if is_water_tex or (0 < max_a <= 128 and min_a < 120):
            kind = "blend"
            arr_w = np.frombuffer(rgba, dtype=np.uint8).reshape(-1, 4).copy()
            if is_water_tex:
                # PS2 GS scales alpha by 2x (0..128 -> 0..255); convert monochrome ripple overlays
                # into clean translucent blue-teal pond water instead of opaque black/white blobs.
                luma = arr_w[:, :3].mean(axis=1)
                chroma = np.abs(arr_w[:, 0].astype(np.int16) - arr_w[:, 2].astype(np.int16)).mean()
                if chroma < 14.0:
                    norm_l = np.clip(luma / 255.0, 0.0, 1.0)
                    arr_w[:, 0] = np.clip(38.0 + norm_l * 135.0, 0, 255).astype(np.uint8)
                    arr_w[:, 1] = np.clip(78.0 + norm_l * 145.0, 0, 255).astype(np.uint8)
                    arr_w[:, 2] = np.clip(112.0 + norm_l * 135.0, 0, 255).astype(np.uint8)
                arr_w[:, 3] = np.clip(arr_w[:, 3].astype(np.float32) * 2.1, 45.0, 185.0).astype(np.uint8)
            else:
                arr_w[:, 3] = np.clip(arr_w[:, 3].astype(np.float32) * 2.0, 24.0, 225.0).astype(np.uint8)
            rgba_clean = arr_w.tobytes()
            arr = arr_w
        else:
            kind, _zero = we.alpha_treatment(alphas, "cutout")
            rgba_clean = we.apply_alpha(rgba, kind)
            arr = np.frombuffer(rgba_clean, dtype=np.uint8).reshape(-1, 4)
        opaque_mask = arr[:, 3] > 0
        if np.any(opaque_mask):
            avg = tuple(int(v) for v in arr[opaque_mask, :3].mean(axis=0))
        else:
            avg = (140, 140, 140)
        out[name] = TextureAsset(
            name=name,
            width=w,
            height=h,
            rgba=rgba_clean,
            alpha_kind=kind,
            average_rgb=avg,
            asset_id=asset_id,
            orig_width=w,
            orig_height=h,
            orig_rgba=rgba_clean,
            raw_tex_bytes=raw,
        )
    return out


def _load_collision(raw_trk: bytes, _entries: list[dict[str, Any]]) -> CollisionData:
    scol_tris = []
    sobl_records = []
    sobl_boxes = 0
    sobl_tubes = 0
    terrain = None
    bppt_slots: dict[int, dict[str, Any]] = {}

    try:
        col_entries = wc.read_directory(raw_trk)
    except Exception:
        col_entries = []

    bsc_raw_saved: bytes | None = None
    try:
        bsc_raw, bsc_e = wc.member(raw_trk, col_entries, "track.bsc", verify=False)
        if bsc_e["type"] == "SCOL" and bsc_e["version"] == wc.SCOL_VERSION:
            bsc_raw_saved = bytes(bsc_raw)
            scol = wc.parse_scol(bsc_raw)
            scol_tris = scol["tris"]
            terrain = wc.Terrain(scol)
    except Exception:
        pass

    try:
        bpp_raw, bpp_e = wc.member(raw_trk, col_entries, "track.bpp", verify=False)
        if bpp_e["type"] == "BPPT":
            bppt_slots = parse_bppt(bpp_raw)
    except Exception:
        pass

    try:
        sol_raw, sol_e = wc.member(raw_trk, col_entries, "track.sol", verify=False)
        if sol_e["type"] == "SOBL" and sol_e["version"] == wc.SOBL_VERSION:
            sobl = wc.parse_sobl(sol_raw)
            sobl_records = sobl["records"]
            sobl_boxes = sum(1 for r in sobl_records if r["kind"] == "box")
            sobl_tubes = sum(1 for r in sobl_records if r["kind"] == "tube")
    except Exception:
        pass

    # Enrich SOBL records with ground height, verticality, and original snapshots for undoable editing
    for rec in sobl_records:
        lo, hi = wc.vertical_span(rec)
        gy = terrain.ground(rec["centre"][0], rec["centre"][2], below=rec["centre"][1])["y"] if terrain else 0.0
        rec["ground_y"] = float(gy)
        rec["bottom_y"] = float(lo)
        rec["top_y"] = float(hi)
        rec["above_ground"] = float(hi - gy)
        rec["modified"] = False
        if rec["kind"] == "box":
            rec["vertical"] = abs(rec["rows"][1][1]) >= wc.VERTICAL_LIMIT
            rec["orig_half_extents"] = list(rec["half_extents"])
            rec["orig_centre"] = list(rec["centre"])
            rec["orig_sphere_radius"] = float(rec["sphere_radius"])
        else:
            rec["vertical"] = False

    # Build GL_LINES vertex arrays for collision overlay
    if scol_tris:
        scol_lines = np.empty((len(scol_tris) * 6, 3), dtype=np.float32)
        for i, t in enumerate(scol_tris):
            v0, v1, v2 = t["verts"]
            base = i * 6
            scol_lines[base] = (v0[0], v0[1] + 0.08, v0[2])
            scol_lines[base + 1] = (v1[0], v1[1] + 0.08, v1[2])
            scol_lines[base + 2] = (v1[0], v1[1] + 0.08, v1[2])
            scol_lines[base + 3] = (v2[0], v2[1] + 0.08, v2[2])
            scol_lines[base + 4] = (v2[0], v2[1] + 0.08, v2[2])
            scol_lines[base + 5] = (v0[0], v0[1] + 0.08, v0[2])
    else:
        scol_lines = np.zeros((0, 3), dtype=np.float32)

    col = CollisionData(
        scol_count=len(scol_tris),
        sobl_count=len(sobl_records),
        sobl_boxes=sobl_boxes,
        sobl_tubes=sobl_tubes,
        terrain=terrain,
        scol_line_verts=scol_lines,
        sobl_line_verts=np.zeros((0, 3), dtype=np.float32),
        sobl_records=sobl_records,
        bppt_slots=bppt_slots,
        _scol_raw=bsc_raw_saved,
    )
    col.rebuild_sobl_lines()
    return col


# ---------------------------------------------------------------------------
# Retail Vehicle / Wrecked Car Library (DATA/CAR/*.CAR)
# ---------------------------------------------------------------------------
def load_car_catalog(
    game_dir: Path | str,
    *,
    token: Any = None,
    progress: Any = None,
    use_cache: bool = True,
) -> tuple[dict[str, WorldModel], dict[str, TextureAsset]]:
    """Decode all retail .CAR archives under <game_dir>/DATA/CAR into WorldModels + textures.

    Uses Low3.mod (falling back to Low2.mod / Low1.mod) so all 37 retail vehicles
    decode cleanly in ~1 second and render fast even when dozens of wrecks are scattered.
    """
    gdir = Path(game_dir).resolve()
    cache_key = str(gdir).lower()
    if use_cache and cache_key in _CAR_CATALOG_CACHE:
        c_models, c_tex = _CAR_CATALOG_CACHE[cache_key]
        return dict(c_models), dict(c_tex)

    car_paths = discover_cars(gdir)
    models: dict[str, WorldModel] = {}
    textures: dict[str, TextureAsset] = {}
    shared_res = [
        gdir / "DATA" / "RES" / f
        for f in ("HOBBYSET.RES", "RACE.RES", "PART.RES", "COMMON.RES", "GENESIS.RES")
        if (gdir / "DATA" / "RES" / f).is_file()
    ]
    shared_dirs = {p: mb.directory(p) for p in shared_res}

    total = max(1, len(car_paths))
    for idx, cp in enumerate(car_paths):
        if token is not None and token.is_cancelled:
            return {}, {}
        if progress is not None:
            progress(int((idx / total) * 100), f"Scanning car {cp.stem} ({idx + 1}/{total})...")
        try:
            entries = mb.directory(cp)
            by_l = {e["name"].lower(): e for e in entries if e["type"] == "MODL"}
            pick = (
                by_l.get("low3.mod")
                or by_l.get("low2.mod")
                or by_l.get("low1.mod")
                or next((e for n, e in by_l.items() if not n.startswith("shadow")), None)
            )
            if pick is None:
                continue
            raw_mod, _ = mb.payload(cp, pick["name"], entries, verify_crc=False)
            dec = veh_exp.decode_model(raw_mod)

            # Namespace textures per car to avoid colliding with track texture names
            car_stem = cp.stem.upper()
            paint = None
            try:
                paint = mb.vehicle_paint(gdir, cp, include_embedded=True)
            except Exception:
                paint = None

            remapped_parts: list[dict[str, Any]] = []
            for p in dec["parts"]:
                orig_tex = p.get("texture") or ""
                ns_tex = f"car/{car_stem}/{orig_tex}" if orig_tex and orig_tex.lower() not in ("", "-", ".tex") else ""
                remapped_parts.append(
                    {
                        "texture": ns_tex,
                        "verts": p.get("verts") or [],
                        "tris": p.get("tris") or [],
                    }
                )
                if ns_tex and ns_tex not in textures:
                    tdata = None
                    asset_id = f"DATA/CAR/{cp.name}#{orig_tex}"
                    try:
                        tdata = mb.resolve_material(
                            gdir, cp, orig_tex, paint=paint, request=veh_exp.KIT_REQUEST
                        )
                    except Exception:
                        tdata = None
                    if tdata is None:
                        # Quick fallback check inside shared archives
                        for sp, sents in shared_dirs.items():
                            hit = next((e for e in sents if e["name"].lower() == orig_tex.lower()), None)
                            if hit:
                                tdata, _ = mb.payload(sp, hit["name"], sents, verify_crc=False)
                                asset_id = f"DATA/RES/{sp.name}#{hit['name']}"
                                break
                    if tdata:
                        try:
                            w, h, rgba, _meta = decode_preview(tdata)
                            kind, _z = we.alpha_treatment(rgba[3::4], "cutout")
                            rgba_clean = we.apply_alpha(rgba, kind)
                            arr = np.frombuffer(rgba_clean, dtype=np.uint8).reshape(-1, 4)
                            op = arr[:, 3] > 0
                            avg = tuple(int(v) for v in arr[op, :3].mean(axis=0)) if np.any(op) else (150, 145, 140)
                            textures[ns_tex] = TextureAsset(
                                name=ns_tex,
                                width=w,
                                height=h,
                                rgba=rgba_clean,
                                alpha_kind=kind,
                                average_rgb=avg,
                                asset_id=asset_id,
                                orig_width=w,
                                orig_height=h,
                                orig_rgba=rgba_clean,
                                raw_tex_bytes=tdata,
                            )
                        except Exception:
                            pass

            parts, tris, lo, hi, tnames = _pack_decoded_parts(remapped_parts)
            if tris == 0:
                continue
            is_trailer = car_stem.startswith("TRAILER")
            pretty = car_stem.replace("_1", "").title()
            subtype = "Trailer / Wreck" if is_trailer else "Car / Wreck"
            mkey = f"car:{cp.name.upper()}"
            hhex = hashlib.sha1(raw_mod[:256]).hexdigest()[:10].upper()
            models[mkey] = WorldModel(
                key=mkey,
                name=f"{pretty} ({'Trailer' if is_trailer else 'Car'})",
                category="car-wreck",
                model_store=f"DATA/CAR/{cp.name}::{pick['name']}",
                hash_hex=f"0x{hhex}",
                parts=parts,
                tri_count=tris,
                textures=tnames,
                bbox_min=lo,
                bbox_max=hi,
                pivot=(0.0, 0.0, 0.0),
                placed_by_map=False,
                subtype=subtype,
            )
        except Exception:
            continue

    if use_cache and models:
        _CAR_CATALOG_CACHE[cache_key] = (dict(models), dict(textures))
    return models, textures


def merge_car_catalog_into_world(
    world: TrackWorld,
    car_models: dict[str, WorldModel],
    car_textures: dict[str, TextureAsset],
) -> int:
    """Attach decoded retail DATA/CAR models & textures into an active TrackWorld."""
    added = 0
    for k, m in car_models.items():
        if k not in world.models:
            world.models[k] = m
            added += 1
    for tk, ta in car_textures.items():
        if tk not in world.textures:
            world.textures[tk] = ta
    world.car_catalog_loaded = True
    world.car_count = sum(1 for m in world.models.values() if m.category == "car-wreck")
    return added


def _load_road_graphs(
    trk_path: Path,
    entries: list[dict[str, Any]],
    collision: CollisionData,
) -> tuple[RoadGraph, dict[str, RoadGraph]]:
    ild_entries = sorted(
        (e for e in entries if e["type"] == "ILIN" and e["name"].lower().endswith(".ild") and e["size"] >= 48),
        key=lambda e: (e["name"].lower() != "track.ild", -e["size"], e["name"].lower()),
    )
    available = [e["name"] for e in ild_entries]
    graphs: dict[str, RoadGraph] = {}

    for e in ild_entries:
        try:
            raw, _ = payload(trk_path, e["name"], entries, verify_crc=False)
            layout = ilin_fmt._layout(raw)
            base = layout["nodes_offset"]
            count = layout["count"]
            primary = layout["primary"]
            order, closed = ilin_fmt.main_loop(raw)
        except Exception:
            continue

        g = RoadGraph(line_name=e["name"], available_lines=list(available))
        idx_to_nid: dict[int, int] = {}
        for i in range(count):
            off = base + i * ilin_fmt.NODE_SIZE
            x, z = struct.unpack_from("<2f", raw, off)
            y = collision.ground_height(x, z) + 0.35
            is_side = i >= primary
            node = g.add_node(
                x=x,
                y=y,
                z=z,
                lanes=1 if is_side else 2,
                width=9.0 if is_side else 14.0,
                line_name=e["name"],
                retail_index=i,
                is_side=is_side,
            )
            idx_to_nid[i] = node.node_id

        # Main loop edges
        for k in range(len(order) - 1):
            a = idx_to_nid.get(order[k])
            b = idx_to_nid.get(order[k + 1])
            if a and b:
                g.add_edge(a, b, lanes=2, width=14.0)
        if closed and len(order) > 2:
            a = idx_to_nid.get(order[-1])
            b = idx_to_nid.get(order[0])
            if a and b:
                g.add_edge(a, b, lanes=2, width=14.0)

        # Side line edges via next pointer
        for i in range(primary, count):
            off = base + i * ilin_fmt.NODE_SIZE
            nxt_off = struct.unpack_from("<I", raw, off + 20)[0]
            nxt_idx = ilin_fmt._node_index(layout, nxt_off)
            if nxt_idx is not None and nxt_idx in idx_to_nid:
                g.add_edge(idx_to_nid[i], idx_to_nid[nxt_idx], lanes=1, width=9.0)

        graphs[e["name"]] = g

    if available and available[0] in graphs:
        primary_graph = graphs[available[0]]
    else:
        primary_graph = RoadGraph(line_name="custom.ild", available_lines=available)
        graphs["custom.ild"] = primary_graph
    return primary_graph, graphs


# ---------------------------------------------------------------------------
# Turbo Binary Track Cache (%LOCALAPPDATA%\EODWorldEditor\cache\<track>_<sha256>.eodcache)
# Uncompressed numpy + struct buffers for ~50-100x faster repeat track opens.
# ---------------------------------------------------------------------------
_CACHE_MAGIC = b"EODCACHE"
_CACHE_VERSION = 3
_CACHE_HEADER_FMT = "<8sIIIQQ"
_CACHE_HEADER_SIZE = struct.calcsize(_CACHE_HEADER_FMT)
_TRK_SHA_STAT_CACHE: dict[str, tuple[int, int, str]] = {}


def track_cache_dir() -> Path:
    """Return local binary track cache directory (respects EOD_EDITOR_APPDATA override)."""
    override = os.environ.get("EOD_EDITOR_APPDATA")
    if override:
        base = Path(override)
    else:
        local = os.environ.get("LOCALAPPDATA")
        base = (Path(local) / "EODWorldEditor") if local else (Path.home() / ".eod_world_editor")
    cdir = base / "cache"
    cdir.mkdir(parents=True, exist_ok=True)
    return cdir


def _trk_sha256_prefix(trk_path: Path, prefix_len: int = 16) -> str:
    """Fast stat-memoized SHA-256 prefix for retail .TRK files."""
    p = Path(trk_path).resolve()
    st = p.stat()
    key = str(p)
    cached = _TRK_SHA_STAT_CACHE.get(key)
    if cached is not None and cached[0] == st.st_size and cached[1] == st.st_mtime_ns:
        return cached[2][:prefix_len]
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    digest = h.hexdigest()
    _TRK_SHA_STAT_CACHE[key] = (st.st_size, st.st_mtime_ns, digest)
    return digest[:prefix_len]


def track_cache_path(trk_path: Path | str, lod: int = 0, include_sky: bool = False) -> Path:
    p = Path(trk_path).resolve()
    sha = _trk_sha256_prefix(p, 16)
    stem = p.stem.upper()
    if lod == 0 and not include_sky:
        return track_cache_dir() / f"{stem}_{sha}.eodcache"
    return track_cache_dir() / f"{stem}_{sha}_lod{lod}_sky{int(include_sky)}.eodcache"


def track_cache_exists(trk_path: Path | str, lod: int = 0, include_sky: bool = False) -> bool:
    try:
        return track_cache_path(trk_path, lod=lod, include_sky=include_sky).is_file()
    except Exception:
        return False


def _save_track_cache(world: TrackWorld, cache_file: Path, lod: int = 0, include_sky: bool = False) -> Path:
    """Serialize TrackWorld into uncompressed numpy/struct binary cache atomically."""
    blob = bytearray()

    def _append_bytes(raw: bytes | bytearray | memoryview, align: int = 4) -> tuple[int, int]:
        if not raw:
            return 0, 0
        rem = len(blob) % align
        if rem:
            blob.extend(b"\x00" * (align - rem))
        off = len(blob)
        blob.extend(raw)
        return off, len(raw)

    def _append_arr(arr: np.ndarray, align: int = 4) -> tuple[int, list[int]]:
        if arr.size == 0:
            return 0, list(arr.shape)
        c_arr = np.ascontiguousarray(arr)
        off, _ = _append_bytes(memoryview(c_arr).cast("B"), align=align)
        return off, list(c_arr.shape)

    models_meta: list[list[Any]] = []
    for m in world.models.values():
        if m.category == "car-wreck":
            continue  # car catalog is merged dynamically via in-memory _CAR_CATALOG_CACHE
        parts_meta: list[list[Any]] = []
        for part in m.parts:
            v_off, v_shape = _append_arr(part.vertices.astype(np.float32, copy=False))
            i_off, i_shape = _append_arr(part.indices.astype(np.uint32, copy=False))
            parts_meta.append([part.texture, v_off, v_shape, i_off, i_shape, int(part.tri_count)])
        models_meta.append(
            [
                m.key,
                m.name,
                m.category,
                m.model_store,
                m.hash_hex,
                parts_meta,
                int(m.tri_count),
                list(m.textures),
                [float(x) for x in m.bbox_min],
                [float(x) for x in m.bbox_max],
                list(m.pivot),
                bool(m.placed_by_map),
                m.model_offset,
                m.subtype,
            ]
        )

    # Pack all WorldObject float transforms into a single contiguous (N, 15) float32 numpy table
    n_objs = len(world.objects)
    obj_xforms = np.empty((n_objs, 15), dtype=np.float32)
    objects_meta: list[list[Any]] = []
    for idx_o, o in enumerate(world.objects):
        obj_xforms[idx_o, 0:3] = o.position
        obj_xforms[idx_o, 3:6] = o.rotation_hpr
        obj_xforms[idx_o, 6:9] = o.scale
        obj_xforms[idx_o, 9:12] = o.retail_initial_pos
        obj_xforms[idx_o, 12:15] = o.retail_initial_rot
        wm = world.models.get(o.model_key)
        # Omit name/subtype when identical to WorldModel (saves ~2 MB of metadata JSON!)
        name_override = None if (wm is not None and o.name == wm.name) else o.name
        sub_override = None if (wm is not None and o.subtype == wm.subtype) else o.subtype
        orig_mk_override = None if (o.retail_original_model_key == o.model_key) else o.retail_original_model_key
        flags = (
            (1 if o.written else 0)
            | (2 if o.visible else 0)
            | (4 if o.deleted else 0)
            | (8 if o.has_custom_collision else 0)
            | (16 if o.source == "Retail" else 0)
        )
        objects_meta.append(
            [
                o.obj_id,
                o.model_key,
                o.category,
                o.section_mlist,
                flags,
                name_override,
                sub_override,
                o.retail_instance_id,
                o.retail_record_offset,
                o.retail_model_offset,
                o.retail_event_member,
                o.retail_event_section,
                o.retail_event_prop_index,
                orig_mk_override,
                o.opt_terrain_mode,
            ]
        )
    obj_xforms_off, obj_xforms_shape = _append_arr(obj_xforms)

    textures_meta: list[list[Any]] = []
    for name, ta in world.textures.items():
        if name.startswith("car/"):
            continue
        rgba_off, rgba_len = _append_bytes(ta.rgba, align=4)
        raw_off, raw_len = _append_bytes(ta.raw_tex_bytes or b"", align=4)
        textures_meta.append(
            [
                ta.name,
                int(ta.width),
                int(ta.height),
                rgba_off,
                rgba_len,
                ta.alpha_kind,
                list(ta.average_rgb),
                ta.asset_id,
                raw_off,
                raw_len,
            ]
        )

    col = world.collision
    scol_line_off, scol_line_shape = _append_arr(col.scol_line_verts.astype(np.float32, copy=False))
    scol_base_off, scol_base_shape = _append_arr(col.scol_base_line_verts.astype(np.float32, copy=False))
    bppt_line_off, bppt_line_shape = _append_arr(col.bppt_line_verts.astype(np.float32, copy=False))
    sobl_line_off, sobl_line_shape = _append_arr(col.sobl_line_verts.astype(np.float32, copy=False))
    sobl_col_off, sobl_col_shape = _append_arr(col.sobl_line_colors.astype(np.float32, copy=False))
    scol_raw_off, scol_raw_len = _append_bytes(col._scol_raw or b"", align=4)

    col_meta = {
        "scol_count": int(col.scol_count),
        "sobl_count": int(col.sobl_count),
        "sobl_boxes": int(col.sobl_boxes),
        "sobl_tubes": int(col.sobl_tubes),
        "sobl_records": col.sobl_records,
        "bppt_slots": {str(k): v for k, v in col.bppt_slots.items()},
        "scol_line": [scol_line_off, scol_line_shape],
        "scol_base": [scol_base_off, scol_base_shape],
        "bppt_line": [bppt_line_off, bppt_line_shape],
        "sobl_line": [sobl_line_off, sobl_line_shape],
        "sobl_col": [sobl_col_off, sobl_col_shape],
        "scol_raw": [scol_raw_off, scol_raw_len],
    }

    roads_meta: dict[str, Any] = {}
    for lname, g in world.all_road_graphs.items():
        roads_meta[lname] = {
            "line_name": g.line_name,
            "available_lines": list(g.available_lines),
            "next_id": int(g.next_id),
            "nodes": [
                [n.node_id, n.x, n.y, n.z, n.line_name, n.is_side, n.retail_index, n.lanes, n.width, n.two_way]
                for n in g.nodes.values()
            ],
            "edges": [[e.a, e.b, e.lanes, e.width, e.two_way] for e in g.edges],
        }

    markers_meta = [
        [m.marker_id, m.name, m.kind, list(m.position), float(m.heading_deg), m.source, m.note]
        for m in world.markers
    ]

    templates_meta = {
        k: [t.display_name, t.contest_key, t.va0_member, list(t.opt_terrain_modes), t.ilin_member]
        for k, t in world.event_templates.items()
    }

    meta_doc = {
        "trk_rel": world.trk_rel,
        "track_name": world.track_name,
        "display_name": world.display_name,
        "entries": world.entries,
        "graf_summary": world.graf_summary,
        "mlist_info": world.mlist_info,
        "models": models_meta,
        "objects": objects_meta,
        "obj_xforms": [obj_xforms_off, obj_xforms_shape],
        "textures": textures_meta,
        "collision": col_meta,
        "active_road_line": world.road_graph.line_name,
        "roads": roads_meta,
        "markers": markers_meta,
        "world_center": list(world.world_center),
        "world_radius": float(world.world_radius),
        "available_events": list(world.available_events),
        "event_templates": templates_meta,
        "custom_race": world.custom_race.to_dict(),
    }
    meta_bytes = json.dumps(meta_doc, separators=(",", ":")).encode("utf-8")
    header = struct.pack(
        _CACHE_HEADER_FMT,
        _CACHE_MAGIC,
        _CACHE_VERSION,
        int(lod),
        int(include_sky),
        len(meta_bytes),
        len(blob),
    )

    cache_file.parent.mkdir(parents=True, exist_ok=True)
    tmp = cache_file.with_name(cache_file.name + ".tmp")
    with open(tmp, "wb") as f:
        f.write(header)
        f.write(meta_bytes)
        f.write(blob)
    os.replace(tmp, cache_file)
    return cache_file


def _load_track_cache(
    cache_file: Path,
    trk_p: Path,
    gdir: Path,
    lod: int = 0,
    include_sky: bool = False,
) -> TrackWorld | None:
    """Fast binary deserialization of TrackWorld from an uncompressed .eodcache file."""
    if not cache_file.is_file():
        return None
    try:
        raw = cache_file.read_bytes()
        if len(raw) < _CACHE_HEADER_SIZE:
            return None
        magic, ver, c_lod, c_sky, meta_len, blob_len = struct.unpack_from(_CACHE_HEADER_FMT, raw, 0)
        if (
            magic != _CACHE_MAGIC
            or ver != _CACHE_VERSION
            or c_lod != int(lod)
            or c_sky != int(include_sky)
            or _CACHE_HEADER_SIZE + meta_len + blob_len != len(raw)
        ):
            return None

        meta_start = _CACHE_HEADER_SIZE
        blob_start = meta_start + meta_len
        meta = json.loads(raw[meta_start:blob_start])
        mv = memoryview(raw)[blob_start:]

        def _read_arr(spec: list[Any], dtype: Any) -> np.ndarray:
            off, shape = int(spec[0]), tuple(int(s) for s in spec[1])
            count = 1
            for s in shape:
                count *= s
            if count == 0:
                return np.zeros(shape, dtype=dtype)
            return np.frombuffer(mv, dtype=dtype, count=count, offset=off).reshape(shape).copy()

        models: dict[str, WorldModel] = {}
        for m in meta["models"]:
            parts: list[MeshPart] = []
            for p in m[5]:
                v_off, v_shape = int(p[1]), tuple(p[2])
                i_off, i_shape = int(p[3]), tuple(p[4])
                v_cnt = v_shape[0] * v_shape[1]
                i_cnt = i_shape[0]
                verts = np.frombuffer(mv, dtype=np.float32, count=v_cnt, offset=v_off).reshape(v_shape)
                inds = np.frombuffer(mv, dtype=np.uint32, count=i_cnt, offset=i_off)
                parts.append(MeshPart(texture=p[0], vertices=verts, indices=inds, tri_count=int(p[5])))
            models[m[0]] = WorldModel(
                key=m[0],
                name=m[1],
                category=m[2],
                model_store=m[3],
                hash_hex=m[4],
                parts=parts,
                tri_count=int(m[6]),
                textures=m[7],
                bbox_min=np.array(m[8], dtype=np.float32),
                bbox_max=np.array(m[9], dtype=np.float32),
                pivot=(float(m[10][0]), float(m[10][1]), float(m[10][2])),
                placed_by_map=bool(m[11]),
                model_offset=m[12],
                subtype=m[13],
            )

        obj_xforms = _read_arr(meta["obj_xforms"], np.float32).tolist()
        objects: list[WorldObject] = []
        for o, xf in zip(meta["objects"], obj_xforms):
            mk = o[1]
            wm = models.get(mk)
            flags = int(o[4])
            objects.append(
                WorldObject(
                    obj_id=o[0],
                    name=o[5] if o[5] is not None else (wm.name if wm else mk),
                    model_key=mk,
                    source="Retail" if (flags & 16) else "Placed by the editor",
                    category=o[2],
                    subtype=o[6] if o[6] is not None else (wm.subtype if wm else "Simple Model"),
                    section_mlist=o[3],
                    model_store=wm.model_store if wm else "",
                    hash_hex=wm.hash_hex if wm else "",
                    tri_count=wm.tri_count if wm else 0,
                    textures=list(wm.textures) if wm else [],
                    position=(xf[0], xf[1], xf[2]),
                    rotation_hpr=(xf[3], xf[4], xf[5]),
                    scale=(xf[6], xf[7], xf[8]),
                    written=bool(flags & 1),
                    visible=bool(flags & 2),
                    deleted=bool(flags & 4),
                    has_custom_collision=bool(flags & 8),
                    retail_instance_id=o[7],
                    retail_record_offset=o[8],
                    retail_model_offset=o[9],
                    retail_initial_pos=(xf[9], xf[10], xf[11]),
                    retail_initial_rot=(xf[12], xf[13], xf[14]),
                    retail_event_member=o[10],
                    retail_event_section=o[11],
                    retail_event_prop_index=o[12],
                    retail_original_model_key=o[13] if o[13] is not None else mk,
                    opt_terrain_mode=o[14],
                )
            )

        textures: dict[str, TextureAsset] = {}
        for t in meta["textures"]:
            rgba = bytes(mv[int(t[3]) : int(t[3]) + int(t[4])]) if int(t[4]) > 0 else b""
            raw_b = bytes(mv[int(t[8]) : int(t[8]) + int(t[9])]) if int(t[9]) > 0 else None
            textures[t[0]] = TextureAsset(
                name=t[0],
                width=int(t[1]),
                height=int(t[2]),
                rgba=rgba,
                alpha_kind=t[5],
                average_rgb=(int(t[6][0]), int(t[6][1]), int(t[6][2])),
                asset_id=t[7],
                orig_width=int(t[1]),
                orig_height=int(t[2]),
                orig_rgba=rgba,
                raw_tex_bytes=raw_b,
            )

        cm = meta["collision"]
        scol_raw_off, scol_raw_len = int(cm["scol_raw"][0]), int(cm["scol_raw"][1])
        scol_raw = bytes(mv[scol_raw_off : scol_raw_off + scol_raw_len]) if scol_raw_len > 0 else None
        collision = CollisionData(
            scol_count=int(cm["scol_count"]),
            sobl_count=int(cm["sobl_count"]),
            sobl_boxes=int(cm["sobl_boxes"]),
            sobl_tubes=int(cm["sobl_tubes"]),
            terrain=None,
            scol_line_verts=_read_arr(cm["scol_line"], np.float32),
            sobl_line_verts=_read_arr(cm["sobl_line"], np.float32),
            sobl_line_colors=_read_arr(cm["sobl_col"], np.float32),
            sobl_records=cm["sobl_records"],
            bppt_slots={int(k): v for k, v in cm.get("bppt_slots", {}).items()},
            bppt_line_verts=_read_arr(cm["bppt_line"], np.float32),
            scol_base_line_verts=_read_arr(cm["scol_base"], np.float32),
            _scol_raw=scol_raw,
        )

        all_road_graphs: dict[str, RoadGraph] = {}
        for lname, rd in meta["roads"].items():
            nodes_d: dict[int, RoadNode] = {}
            for nd in rd["nodes"]:
                rn = RoadNode(
                    node_id=int(nd[0]),
                    x=float(nd[1]),
                    y=float(nd[2]),
                    z=float(nd[3]),
                    line_name=nd[4],
                    is_side=bool(nd[5]),
                    retail_index=nd[6],
                    lanes=int(nd[7]),
                    width=float(nd[8]),
                    two_way=bool(nd[9]),
                )
                nodes_d[rn.node_id] = rn
            edges_l = [RoadEdge(a=int(e[0]), b=int(e[1]), lanes=int(e[2]), width=float(e[3]), two_way=bool(e[4])) for e in rd["edges"]]
            all_road_graphs[lname] = RoadGraph(
                line_name=rd["line_name"],
                available_lines=rd["available_lines"],
                nodes=nodes_d,
                edges=edges_l,
                next_id=int(rd["next_id"]),
                dirty=False,
            )

        active_line = meta.get("active_road_line")
        road_graph = (
            all_road_graphs.get(active_line)
            or next(iter(all_road_graphs.values()), RoadGraph(line_name="track.ild"))
        )

        markers = [
            WorldMarker(
                marker_id=m[0],
                name=m[1],
                kind=m[2],
                position=(float(m[3][0]), float(m[3][1]), float(m[3][2])),
                heading_deg=float(m[4]),
                source=m[5],
                note=m[6],
            )
            for m in meta["markers"]
        ]

        event_templates = {
            k: EventTemplateInfo(
                display_name=v[0],
                contest_key=v[1],
                va0_member=v[2],
                opt_terrain_modes=tuple(int(x) for x in v[3]),
                ilin_member=v[4],
            )
            for k, v in meta.get("event_templates", {}).items()
        }

        wc_pt = meta["world_center"]
        return TrackWorld(
            game_dir=gdir,
            trk_path=trk_p,
            trk_rel=meta["trk_rel"],
            track_name=meta["track_name"],
            display_name=meta["display_name"],
            entries=meta["entries"],
            graf_summary=meta["graf_summary"],
            mlist_info=meta["mlist_info"],
            models=models,
            objects=objects,
            textures=textures,
            collision=collision,
            road_graph=road_graph,
            all_road_graphs=all_road_graphs,
            markers=markers,
            world_center=(float(wc_pt[0]), float(wc_pt[1]), float(wc_pt[2])),
            world_radius=float(meta["world_radius"]),
            environment=EnvironmentState(),
            custom_race=CustomRaceConfig.from_dict(meta.get("custom_race", {})),
            available_events=meta.get("available_events", []),
            event_templates=event_templates,
            active_event_template="All Templates (Combined)",
            sync_all_events=True,
            include_lod_twins=True,
            loaded_from_cache=True,
            cache_path=str(cache_file),
            _cache_backing=raw,
        )
    except Exception:
        return None


# ---------------------------------------------------------------------------
# Main Track World Loader
# ---------------------------------------------------------------------------
def load_track_world(
    trk_path: Path | str,
    game_dir: Path | str | None = None,
    lod: int = 0,
    include_sky: bool = False,
    include_cars: bool = True,
    token: Any = None,
    progress: Any = None,
    use_cache: bool = True,
) -> TrackWorld:
    """Load an EOD retail .TRK archive into an interactive TrackWorld (with Turbo Binary Cache)."""
    t_start_ns = time.perf_counter_ns()
    trk_p = Path(trk_path).resolve()
    gdir = Path(game_dir).resolve() if game_dir else trk_p.parents[2]
    try:
        trk_rel = trk_p.relative_to(gdir).as_posix()
    except ValueError:
        trk_rel = f"DATA/TRACKS/{trk_p.name}"

    cpath: Path | None = None
    if use_cache:
        try:
            cpath = track_cache_path(trk_p, lod=lod, include_sky=include_sky)
            cached_world = _load_track_cache(cpath, trk_p, gdir, lod=lod, include_sky=include_sky)
            if cached_world is not None:
                if include_cars:
                    if progress is not None:
                        progress(75, "Loading retail DATA/CAR vehicle library...")
                    c_models, c_tex = load_car_catalog(gdir, token=token)
                    merge_car_catalog_into_world(cached_world, c_models, c_tex)
                cached_world.load_ms = (time.perf_counter_ns() - t_start_ns) / 1_000_000.0
                if progress is not None:
                    progress(100, f"Loaded {cached_world.display_name} from Turbo Cache ({cached_world.load_ms:.1f} ms)")
                return cached_world
        except Exception:
            pass

    track_dict = we.load_track(gdir, trk_rel)
    graf_data = track_dict["graf"]
    graf = wg.parse_graf(graf_data)
    graf_summary = wg.summarize(graf)
    graf_member = track_dict["graf_entry"]["name"]
    entries = track_dict["entries"]

    # 1. Load collision (SCOL + SOBL)
    collision = _load_collision(track_dict["raw"], entries)

    # 2. Decode GRAF leaves into deduplicated WorldModels + placed WorldObjects
    models: dict[str, WorldModel] = {}
    objects: list[WorldObject] = []
    markers: list[WorldMarker] = []
    all_tex_names: set[str] = set()

    # Track mlist sections
    mlist_labels = []
    for idx_m, minfo in enumerate(graf["mlist_info"]):
        if minfo:
            mlist_labels.append(f"mlist_{idx_m} ({minfo['batches']['count']} batches)")
    default_mlist = mlist_labels[0] if mlist_labels else "mlist_0"

    for i, leaf in enumerate(graf["leaves"]):
        if not we.leaf_selected(leaf, lod):
            continue
        mod_off = leaf.get("model_offset")
        if not mod_off:
            continue

        model_hdr = wg.parse_model(graf_data, mod_off)
        decoded = wg.decode_model(graf_data, model_hdr)
        category = we.classify(leaf, decoded)
        if category == "sky" and not include_sky:
            continue
        if category == "dyno-unspawned":
            continue
        if category == "billboard":
            decoded = we.rebuild_billboard(graf_data, model_hdr)

        subtype_name = leaf.get("subtype_name", "Simple Model" if leaf.get("kind") == 0 else "Unknown")
        if subtype_name == "NamedPoint":
            c = model_hdr["centre"]
            markers.append(
                WorldMarker(
                    marker_id=f"m_graf_{i}",
                    name=f"NamedPoint_{i}",
                    kind="Marker",
                    position=(float(c[0]), float(c[1]), float(c[2])),
                    source="Retail",
                )
            )

        is_dyno = category == "dyno"
        pivot = tuple(float(v) for v in model_hdr["centre"])
        mkey = f"{graf_member}@0x{mod_off:x}"

        if mkey not in models:
            # Static/billboard models store world coords -> recenter around pivot for clean placement/gizmos
            subtract = None if is_dyno else pivot
            parts, tris, lo, hi, tnames = _pack_decoded_parts(decoded, subtract_pivot=subtract)
            if tris == 0:
                continue
            all_tex_names.update(tnames)
            # Clean readable model name from primary texture + offset
            primary_tex = Path(tnames[0]).stem if tnames else subtype_name.lower().replace(" ", "_")
            mname = f"{primary_tex}_{mod_off:05x}"
            hhex = hashlib.sha1(f"{trk_p.name}:{mkey}".encode("utf-8")).hexdigest()[:10].upper()
            models[mkey] = WorldModel(
                key=mkey,
                name=mname,
                category=category,
                model_store=f"{trk_p.name}::{graf_member}",
                hash_hex=f"0x{hhex}",
                parts=parts,
                tri_count=tris,
                textures=tnames,
                bbox_min=lo,
                bbox_max=hi,
                pivot=pivot,
                placed_by_map=True,
                model_offset=mod_off,
                subtype=subtype_name,
            )

        wm = models[mkey]
        if is_dyno:
            m12 = leaf["dyno"]["matrix"]
            m4 = np.array(we.native_rows_to_matrix(m12), dtype=np.float32).reshape(4, 4)
            pos, rot_hpr, scl = decompose_matrix_hpr(m4)
        else:
            pos = pivot
            rot_hpr = (0.0, 0.0, 0.0)
            scl = (1.0, 1.0, 1.0)

        # Determine section / mlist tag
        sec_tag = default_mlist
        opt_mode = None
        if leaf.get("optional_terrain"):
            opt_mode = int(leaf["optional_terrain"]["mode"])
            sec_tag = f"opt_terrain (mode {opt_mode})"
        elif leaf["path"]:
            spheres = [s["node"] for s in leaf["path"] if s["kind"] == "sphere"]
            if spheres:
                sec_tag = f"{default_mlist} / cull_0x{spheres[-1]:x}"

        objects.append(
            WorldObject(
                obj_id=f"i{i:04d}",
                name=wm.name,
                model_key=mkey,
                source="Retail",
                category=category,
                subtype=subtype_name,
                section_mlist=sec_tag,
                model_store=wm.model_store,
                hash_hex=wm.hash_hex,
                tri_count=wm.tri_count,
                textures=list(wm.textures),
                position=pos,
                rotation_hpr=rot_hpr,
                scale=scl,
                written=True,
                retail_instance_id=f"i{i:04d}",
                retail_record_offset=leaf.get("record"),
                retail_model_offset=mod_off,
                retail_initial_pos=pos,
                retail_initial_rot=rot_hpr,
                retail_original_model_key=mkey,
                opt_terrain_mode=opt_mode,
            )
        )

    # 3. Load event props from ALL c_*.va0 event tables (c_battle.va0 barriers, jump race props, gauntlet, etc.)
    va0_entries = sorted(
        (e for e in entries if e["name"].lower().startswith("c_") and e["type"] == "VALT"),
        key=lambda e: (e["name"].lower() != "c_battle.va0", e["name"].lower()),
    )
    ev_global_idx = 0
    seen_ev_markers: set[tuple[str, int, int]] = set()
    for ev_entry in va0_entries:
        try:
            ev_name = ev_entry["name"]
            for k, prop in enumerate(we.event_props(track_dict, ev_name)):
                rows = we.compose_rows(prop["frame"], prop["localFrame"]) if prop["localFrame"] else prop["frame"]
                m4 = np.array(we.native_rows_to_matrix(rows), dtype=np.float32).reshape(4, 4)
                pos, rot_hpr, scl = decompose_matrix_hpr(m4)
                mod_name = prop.get("model")
                if not mod_name:
                    mk_key = (prop["dyno"].lower(), int(round(pos[0])), int(round(pos[2])))
                    if mk_key not in seen_ev_markers:
                        seen_ev_markers.add(mk_key)
                        markers.append(
                            WorldMarker(
                                marker_id=f"m_ev_{ev_global_idx}",
                                name=prop["dyno"],
                                kind="Marker",
                                position=pos,
                                heading_deg=rot_hpr[0],
                                source="Retail",
                                note=ev_name,
                            )
                        )
                    ev_global_idx += 1
                    continue
                mkey = f"mod:{mod_name.lower()}"
                if mkey not in models:
                    rel, entry, mdata = we.find_member(track_dict, mod_name, kinds=("MODL",))
                    if mdata:
                        try:
                            mhdr = wg.parse_model(mdata, 0)
                            dec = wg.decode_model(mdata, mhdr)
                            parts, tris, lo, hi, tnames = _pack_decoded_parts(dec)
                            if tris > 0:
                                all_tex_names.update(tnames)
                                hhex = hashlib.sha1(mdata[:256]).hexdigest()[:10].upper()
                                models[mkey] = WorldModel(
                                    key=mkey,
                                    name=Path(mod_name).stem,
                                    category="event-prop",
                                    model_store=f"{rel}::{entry['name']}",
                                    hash_hex=f"0x{hhex}",
                                    parts=parts,
                                    tri_count=tris,
                                    textures=tnames,
                                    bbox_min=lo,
                                    bbox_max=hi,
                                    pivot=(0.0, 0.0, 0.0),
                                    placed_by_map=True,
                                    subtype=prop.get("dynoType") or "event-prop",
                                )
                        except Exception:
                            pass
                if mkey in models:
                    wm = models[mkey]
                    objects.append(
                        WorldObject(
                            obj_id=f"e{ev_global_idx:04d}",
                            name=f"{prop['dyno']} ({wm.name})",
                            model_key=mkey,
                            source="Retail",
                            category="event-prop",
                            subtype=wm.subtype,
                            section_mlist=ev_name,
                            model_store=wm.model_store,
                            hash_hex=wm.hash_hex,
                            tri_count=wm.tri_count,
                            textures=list(wm.textures),
                            position=pos,
                            rotation_hpr=rot_hpr,
                            scale=scl,
                            written=True,
                            retail_initial_pos=pos,
                            retail_initial_rot=rot_hpr,
                            retail_event_member=prop["member"],
                            retail_event_section=prop["section"],
                            retail_event_prop_index=k,
                            retail_original_model_key=mkey,
                        )
                    )
                    ev_global_idx += 1
        except Exception:
            continue

    # 4. Discover standalone .mod members inside this .TRK not yet placed (for Explorer catalog)
    for e in entries:
        if e["type"] == "MODL" and e["name"].lower().endswith(".mod"):
            mkey = f"mod:{e['name'].lower()}"
            if mkey in models:
                continue
            try:
                mdata, _ = payload(trk_p, e["name"], entries, verify_crc=False)
                mhdr = wg.parse_model(mdata, 0)
                dec = wg.decode_model(mdata, mhdr)
                parts, tris, lo, hi, tnames = _pack_decoded_parts(dec)
                if tris == 0:
                    continue
                all_tex_names.update(tnames)
                hhex = hashlib.sha1(mdata[:256]).hexdigest()[:10].upper()
                models[mkey] = WorldModel(
                    key=mkey,
                    name=Path(e["name"]).stem,
                    category="prop-catalog",
                    model_store=f"{trk_p.name}::{e['name']}",
                    hash_hex=f"0x{hhex}",
                    parts=parts,
                    tri_count=tris,
                    textures=tnames,
                    bbox_min=lo,
                    bbox_max=hi,
                    pivot=(0.0, 0.0, 0.0),
                    placed_by_map=False,
                    subtype="Standalone MODL",
                )
            except Exception:
                continue

    # 5. Decode textures
    textures = _load_textures(track_dict, all_tex_names)

    # 6. Load ILIN racing lines
    road_graph, all_road_graphs = _load_road_graphs(trk_p, entries, collision)

    # Add Start / Finish Grid slots along ILIN centerline (4 staggered grid markers)
    if road_graph.nodes:
        nodes_list = list(road_graph.nodes.values())
        n0 = nodes_list[0]
        dx, _dy, dz = road_graph.node_direction(n0.node_id)
        hdg = math.degrees(math.atan2(dx, dz))
        # Perpendicular right vector in LH (+X right, +Z forward)
        rx, rz = dz, -dx
        grid_markers: list[WorldMarker] = []
        for slot in range(4):
            back = slot * 5.2
            lat = -2.2 if (slot % 2 == 0) else 2.2
            gx = n0.x - dx * back + rx * lat
            gz = n0.z - dz * back + rz * lat
            gy = collision.ground_height(gx, gz) + 0.25
            grid_markers.append(
                WorldMarker(
                    marker_id=f"m_start_{slot}",
                    name="Start / Finish Grid P1" if slot == 0 else f"Start Grid P{slot + 1}",
                    kind="Start Grid" if slot > 0 else "Spawn",
                    position=(gx, gy, gz),
                    heading_deg=hdg,
                    source="Retail",
                    note=f"Derived from {road_graph.line_name} node 0",
                )
            )
        markers = grid_markers + markers

    # 7. Compute core world center & radius for initial camera framing
    if road_graph.nodes:
        pts = np.array([(n.x, n.y, n.z) for n in road_graph.nodes.values()], dtype=np.float32)
        center = tuple(float(v) for v in pts.mean(axis=0))
        radius = float(np.linalg.norm(pts - np.array(center), axis=1).max()) if len(pts) > 1 else 120.0
    elif objects:
        pts = np.array([o.position for o in objects], dtype=np.float32)
        center = tuple(float(v) for v in np.median(pts, axis=0))
        radius = max(80.0, float(np.percentile(np.linalg.norm(pts - np.array(center), axis=1), 85)))
    else:
        center = (0.0, 0.0, 0.0)
        radius = 100.0

    display = wc.track_display_name(trk_p)
    event_templates = discover_track_event_templates(gdir, display, entries)
    events = [k for k in event_templates.keys() if k != "Battle (Free Roam)"]
    if not events:
        events = ["No-Rules Race", "Figure-8 Jump Race", "Jump Race", "Demolition Derby", "Gauntlet"]
    race_cfg = CustomRaceConfig(
        event_name=events[0] if events else "No-Rules Race",
        event_type=events[0] if events else "No-Rules Race",
        ilin_line=road_graph.line_name,
    )

    world = TrackWorld(
        game_dir=gdir,
        trk_path=trk_p,
        trk_rel=trk_rel,
        track_name=trk_p.stem.upper(),
        display_name=display,
        entries=entries,
        graf_summary=graf_summary,
        mlist_info=graf["mlist_info"],
        models=models,
        objects=objects,
        textures=textures,
        collision=collision,
        road_graph=road_graph,
        all_road_graphs=all_road_graphs,
        markers=markers,
        world_center=center,
        world_radius=max(40.0, radius),
        environment=EnvironmentState(),
        custom_race=race_cfg,
        available_events=events,
        event_templates=event_templates,
        active_event_template="All Templates (Combined)",
        sync_all_events=True,
        include_lod_twins=True,
    )
    world.collision.rebuild_dynamic_ramp_collision(world)

    if use_cache and cpath is not None:
        try:
            _save_track_cache(world, cpath, lod=lod, include_sky=include_sky)
            world.cache_path = str(cpath)
        except Exception:
            pass

    if include_cars:
        if progress is not None:
            progress(75, "Loading retail DATA/CAR vehicle library...")
        c_models, c_tex = load_car_catalog(gdir, token=token)
        merge_car_catalog_into_world(world, c_models, c_tex)

    world.load_ms = (time.perf_counter_ns() - t_start_ns) / 1_000_000.0
    if progress is not None:
        progress(100, f"Loaded {world.display_name} ({world.load_ms:.1f} ms)")
    return world
