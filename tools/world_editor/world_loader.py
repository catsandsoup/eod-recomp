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
import math
import os
import struct
import sys
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


def discover_track_events(game_dir: Path | str, display_name: str) -> list[str]:
    """Read real contest/event names for a track from DATA/RES/COMMON.RES#<track>_eves.val."""
    common = Path(game_dir) / "DATA" / "RES" / "COMMON.RES"
    if not common.is_file():
        return ["No-Rules Race", "Demolition Derby", "Figure-8 Jump", "Battle (8 AI)"]
    try:
        entries = mb.directory(common)
        want = f"{display_name.lower()}_eves.val"
        hit = next((e for e in entries if e["name"].lower() == want), None)
        if not hit:
            return ["No-Rules Race", "Demolition Derby", "Figure-8 Jump", "Battle (8 AI)"]
        raw, _ = mb.payload(common, hit["name"], entries, verify_crc=False)
        table = parse_binary_valt(raw)
        names: list[str] = []
        for sec in table.get("sections", []):
            pairs = dict(sec.get("pairs", []))
            sname = sec.get("name", "")
            if sname.startswith("$") or sname.lower() in ("contest_defaults", "eve", "header"):
                continue
            label = pairs.get("name") or pairs.get("display_name") or sname
            if label and label not in names:
                names.append(str(label))
        return names or ["No-Rules Race", "Demolition Derby", "Figure-8 Jump", "Battle (8 AI)"]
    except Exception:
        return ["No-Rules Race", "Demolition Derby", "Figure-8 Jump", "Battle (8 AI)"]


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
    retail_original_model_key: str | None = None

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
        return cfg


@dataclass
class CollisionData:
    scol_count: int
    sobl_count: int
    sobl_boxes: int
    sobl_tubes: int
    terrain: wc.Terrain | None
    scol_line_verts: np.ndarray  # shape (M, 3), float32 line segments for GL_LINES
    sobl_line_verts: np.ndarray  # shape (K, 3), float32 line segments for GL_LINES
    sobl_line_colors: np.ndarray = field(default_factory=lambda: np.zeros((0, 4), dtype=np.float32))
    sobl_records: list[dict[str, Any]] = field(default_factory=list)
    sobl_edits: dict[int, dict[str, Any]] = field(default_factory=dict)  # rec_idx -> {"field", "value", "ry", "centre", ...}
    custom_boxes: list[dict[str, Any]] = field(default_factory=list)

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
    car_catalog_loaded: bool = False
    car_count: int = 0

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

    try:
        col_entries = wc.read_directory(raw_trk)
    except Exception:
        col_entries = []

    try:
        bsc_raw, bsc_e = wc.member(raw_trk, col_entries, "track.bsc", verify=False)
        if bsc_e["type"] == "SCOL" and bsc_e["version"] == wc.SCOL_VERSION:
            scol = wc.parse_scol(bsc_raw)
            scol_tris = scol["tris"]
            terrain = wc.Terrain(scol)
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
) -> TrackWorld:
    """Load an EOD retail .TRK archive into an interactive TrackWorld."""
    trk_p = Path(trk_path).resolve()
    gdir = Path(game_dir).resolve() if game_dir else trk_p.parents[2]
    try:
        trk_rel = trk_p.relative_to(gdir).as_posix()
    except ValueError:
        trk_rel = f"DATA/TRACKS/{trk_p.name}"

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
        if leaf.get("optional_terrain"):
            sec_tag = f"opt_terrain (mode {leaf['optional_terrain']['mode']})"
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
            )
        )

    # 3. Also load event props from first c_*.va0 event table (if present)
    va0_entries = [e for e in entries if e["name"].lower().startswith("c_") and e["type"] == "VALT"]
    if va0_entries:
        try:
            ev_name = va0_entries[0]["name"]
            for k, prop in enumerate(we.event_props(track_dict, ev_name)):
                rows = we.compose_rows(prop["frame"], prop["localFrame"]) if prop["localFrame"] else prop["frame"]
                m4 = np.array(we.native_rows_to_matrix(rows), dtype=np.float32).reshape(4, 4)
                pos, rot_hpr, scl = decompose_matrix_hpr(m4)
                mod_name = prop.get("model")
                if not mod_name:
                    markers.append(
                        WorldMarker(
                            marker_id=f"m_ev_{k}",
                            name=prop["dyno"],
                            kind="Marker",
                            position=pos,
                            heading_deg=rot_hpr[0],
                            source="Retail",
                        )
                    )
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
                            obj_id=f"e{k:04d}",
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
                            retail_original_model_key=mkey,
                        )
                    )
        except Exception:
            pass

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
    events = discover_track_events(gdir, display)
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
    )

    if include_cars:
        if progress is not None:
            progress(75, "Loading retail DATA/CAR vehicle library...")
        c_models, c_tex = load_car_catalog(gdir, token=token)
        merge_car_catalog_into_world(world, c_models, c_tex)

    if progress is not None:
        progress(100, f"Loaded {world.display_name}")
    return world
