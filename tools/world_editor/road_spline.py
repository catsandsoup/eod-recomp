"""Road spline sampling, grade-limited elevation profiles, banked LH frames & presets.

Coordinate convention (left-handed): +X right, +Y up, +Z forward.
"""
from __future__ import annotations

from dataclasses import asdict, dataclass
import math
from typing import Any

import numpy as np


@dataclass
class RoadControlPoint:
    x: float
    y: float
    z: float
    width: float = 14.0
    bank_deg: float = 0.0
    is_bridge: bool = False
    is_junction: bool = False
    barrier_left: str = "jersey"
    barrier_right: str = "jersey"

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> RoadControlPoint:
        return cls(
            x=float(data["x"]),
            y=float(data["y"]),
            z=float(data["z"]),
            width=float(data.get("width", 14.0)),
            bank_deg=float(data.get("bank_deg", 0.0)),
            is_bridge=bool(data.get("is_bridge", False)),
            is_junction=bool(data.get("is_junction", False)),
            barrier_left=str(data.get("barrier_left", "jersey")),
            barrier_right=str(data.get("barrier_right", "jersey")),
        )


@dataclass
class RoadPlan:
    name: str
    points: list[RoadControlPoint]
    closed: bool = False
    step_m: float = 6.0
    max_grade: float = 0.08
    smooth_passes: int = 30
    surface_tex: str = "rd-1.tex"
    wear_tex: str = "af_rdwr.tex"
    barrier_tex: str = "wall01.tex"
    deck_tex: str = "conc01.tex"
    median_barrier: bool = False
    pier_spacing_m: float = 48.0
    style: str = "highway"

    def to_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "points": [p.to_dict() for p in self.points],
            "closed": self.closed,
            "step_m": self.step_m,
            "max_grade": self.max_grade,
            "smooth_passes": self.smooth_passes,
            "surface_tex": self.surface_tex,
            "wear_tex": self.wear_tex,
            "barrier_tex": self.barrier_tex,
            "deck_tex": self.deck_tex,
            "median_barrier": self.median_barrier,
            "pier_spacing_m": self.pier_spacing_m,
            "style": self.style,
        }

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> RoadPlan:
        return cls(
            name=str(data["name"]),
            points=[RoadControlPoint.from_dict(p) for p in data["points"]],
            closed=bool(data.get("closed", False)),
            step_m=float(data.get("step_m", 6.0)),
            max_grade=float(data.get("max_grade", 0.08)),
            smooth_passes=int(data.get("smooth_passes", 30)),
            surface_tex=str(data.get("surface_tex", "rd-1.tex")),
            wear_tex=str(data.get("wear_tex", "af_rdwr.tex")),
            barrier_tex=str(data.get("barrier_tex", "wall01.tex")),
            deck_tex=str(data.get("deck_tex", "conc01.tex")),
            median_barrier=bool(data.get("median_barrier", False)),
            pier_spacing_m=float(data.get("pier_spacing_m", 48.0)),
            style=str(data.get("style", "highway")),
        )


def _catmull_rom_segment(
    p0: np.ndarray, p1: np.ndarray, p2: np.ndarray, p3: np.ndarray, samples: int
) -> np.ndarray:
    """Centripetal Catmull-Rom interpolation (alpha=0.5) on [t1, t2)."""
    def _dt(a: np.ndarray, b: np.ndarray) -> float:
        return max(float(np.linalg.norm(b - a)) ** 0.5, 1e-6)

    t0 = 0.0
    t1 = t0 + _dt(p0, p1)
    t2 = t1 + _dt(p1, p2)
    t3 = t2 + _dt(p2, p3)
    t = np.linspace(t1, t2, samples, endpoint=False)[:, None]

    a1 = (t1 - t) / (t1 - t0) * p0 + (t - t0) / (t1 - t0) * p1
    a2 = (t2 - t) / (t2 - t1) * p1 + (t - t1) / (t2 - t1) * p2
    a3 = (t3 - t) / (t3 - t2) * p2 + (t - t2) / (t3 - t2) * p3
    b1 = (t2 - t) / (t2 - t0) * a1 + (t - t0) / (t2 - t0) * a2
    b2 = (t3 - t) / (t3 - t1) * a2 + (t - t1) / (t3 - t1) * a3
    return (t2 - t) / (t2 - t1) * b1 + (t - t1) / (t2 - t1) * b2


def catmull_rom_sample(
    pts: np.ndarray, closed: bool = False, samples_per_seg: int = 20
) -> np.ndarray:
    """Sample a centripetal Catmull-Rom spline through 3D (or N-D) control points."""
    arr = np.asarray(pts, dtype=np.float64)
    n = len(arr)
    if n <= 1:
        return arr.copy()
    sps = max(1, int(samples_per_seg))

    if closed:
        segs = [
            _catmull_rom_segment(
                arr[(i - 1) % n], arr[i], arr[(i + 1) % n], arr[(i + 2) % n], sps
            )
            for i in range(n)
        ]
        return np.vstack(segs)

    ext = np.vstack([2.0 * arr[0] - arr[1], arr, 2.0 * arr[-1] - arr[-2]])
    segs = [
        _catmull_rom_segment(ext[i], ext[i + 1], ext[i + 2], ext[i + 3], sps)
        for i in range(n - 1)
    ]
    segs.append(arr[-1:])
    return np.vstack(segs)


def resample_polyline(
    pts: np.ndarray, step_m: float = 6.0, closed: bool = False
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Resample a polyline by uniform arc length, returning (stations_xyz, arc_s, t_params)."""
    arr = np.asarray(pts, dtype=np.float64)
    if len(arr) == 0:
        empty = np.zeros((0, 3), dtype=np.float64)
        z = np.zeros((0,), dtype=np.float64)
        return empty, z, z
    if len(arr) == 1:
        return arr.copy(), np.zeros(1, dtype=np.float64), np.zeros(1, dtype=np.float64)

    poly = np.vstack([arr, arr[:1]]) if closed else arr
    seg_lens = np.linalg.norm(np.diff(poly, axis=0), axis=1)
    cum = np.concatenate([[0.0], np.cumsum(seg_lens)])
    total = float(cum[-1])
    if total <= 1e-9:
        return arr[:1].copy(), np.zeros(1, dtype=np.float64), np.zeros(1, dtype=np.float64)

    step = max(float(step_m), 1e-3)
    if closed:
        count = max(3, int(round(total / step)))
        arc_s = np.linspace(0.0, total, count, endpoint=False)
    else:
        count = max(2, int(round(total / step)) + 1)
        arc_s = np.linspace(0.0, total, count, endpoint=True)

    stations = np.column_stack([np.interp(arc_s, cum, poly[:, d]) for d in range(poly.shape[1])])
    t_params = arc_s / total
    return stations, arc_s, t_params


def grade_limited_profile(
    arc_s: np.ndarray,
    y_vals: np.ndarray,
    pinned_mask: np.ndarray,
    max_grade: float = 0.08,
    smooth_passes: int = 30,
) -> np.ndarray:
    """Smooth elevation profile while clamping |dy/ds| <= max_grade and holding pinned indices fixed."""
    s = np.asarray(arc_s, dtype=np.float64)
    y0 = np.asarray(y_vals, dtype=np.float64)
    pinned = np.asarray(pinned_mask, dtype=bool)
    n = len(y0)
    if n <= 1:
        return y0.copy()

    ds = np.maximum(np.diff(s), 0.0)
    max_dy = max(float(max_grade), 0.0) * ds
    y = y0.copy()

    # Exact lower/upper feasible bounds induced by pinned stations.
    lo = np.full(n, -np.inf, dtype=np.float64)
    hi = np.full(n, np.inf, dtype=np.float64)
    lo[pinned] = y0[pinned]
    hi[pinned] = y0[pinned]
    for i in range(n - 1):
        lo[i + 1] = max(lo[i + 1], lo[i] - max_dy[i])
        hi[i + 1] = min(hi[i + 1], hi[i] + max_dy[i])
    for i in range(n - 2, -1, -1):
        lo[i] = max(lo[i], lo[i + 1] - max_dy[i])
        hi[i] = min(hi[i], hi[i + 1] + max_dy[i])
    feasible = lo <= hi + 1e-12

    def _project(vals: np.ndarray) -> np.ndarray:
        v = vals.copy()
        v[pinned] = y0[pinned]
        clip_idx = (~pinned) & feasible
        v[clip_idx] = np.clip(v[clip_idx], lo[clip_idx], hi[clip_idx])
        u = v.copy()
        l = v.copy()
        for i in range(n - 1):
            u[i + 1] = min(u[i + 1], u[i] + max_dy[i])
            l[i + 1] = max(l[i + 1], l[i] - max_dy[i])
        for i in range(n - 2, -1, -1):
            u[i] = min(u[i], u[i + 1] + max_dy[i])
            l[i] = max(l[i], l[i + 1] - max_dy[i])
        out = 0.5 * (u + l)
        out[pinned] = y0[pinned]
        return out

    y = _project(y)
    free_interior = np.flatnonzero(~pinned[1:-1]) + 1
    for _ in range(max(0, int(smooth_passes))):
        if len(free_interior):
            nxt = y.copy()
            for i in free_interior:
                wl, wr = ds[i - 1], ds[i]
                denom = wl + wr
                target = (wr * y[i - 1] + wl * y[i + 1]) / denom if denom > 1e-12 else 0.5 * (y[i - 1] + y[i + 1])
                nxt[i] = 0.5 * y[i] + 0.5 * target
            y = _project(nxt)
        else:
            break
    return y


def compute_station_frames(
    stations_xyz: np.ndarray, bank_deg: np.ndarray, closed: bool = False
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Compute orthonormal LH frames (tangents, rights, ups) rolled by bank_deg around tangent."""
    pts = np.asarray(stations_xyz, dtype=np.float64)
    n = len(pts)
    if n == 0:
        z = np.zeros((0, 3), dtype=np.float64)
        return z, z, z

    banks = np.broadcast_to(np.asarray(bank_deg, dtype=np.float64), (n,))
    tangents = np.zeros((n, 3), dtype=np.float64)
    if n == 1:
        tangents[0] = np.array([0.0, 0.0, 1.0])
    elif closed:
        tangents = np.roll(pts, -1, axis=0) - np.roll(pts, 1, axis=0)
    else:
        tangents[0] = pts[1] - pts[0]
        tangents[-1] = pts[-1] - pts[-2]
        if n > 2:
            tangents[1:-1] = pts[2:] - pts[:-2]

    norms = np.linalg.norm(tangents, axis=1)
    last_t = np.array([0.0, 0.0, 1.0])
    for i in range(n):
        if norms[i] > 1e-9:
            tangents[i] /= norms[i]
            last_t = tangents[i]
        else:
            tangents[i] = last_t

    world_up = np.array([0.0, 1.0, 0.0])
    fallback_up = np.array([0.0, 0.0, -1.0])
    rights0 = np.zeros((n, 3), dtype=np.float64)
    ups0 = np.zeros((n, 3), dtype=np.float64)
    for i in range(n):
        t = tangents[i]
        ref_up = fallback_up if abs(float(np.dot(t, world_up))) > 0.999 else world_up
        r = np.cross(ref_up, t)
        r /= np.linalg.norm(r)
        u = np.cross(t, r)
        u /= np.linalg.norm(u)
        rights0[i] = r
        ups0[i] = u

    rad = np.deg2rad(banks)
    c = np.cos(rad)[:, None]
    s = np.sin(rad)[:, None]
    rights = rights0 * c + ups0 * s
    ups = -rights0 * s + ups0 * c
    return tangents, rights, ups


def make_banked_oval_plan(
    center: tuple[float, float, float] = (0.0, 0.0, 0.0),
    rx: float = 90.0,
    rz: float = 55.0,
    width: float = 18.0,
    bank_deg: float = 14.0,
) -> RoadPlan:
    cx, cy, cz = center
    pts = []
    for k in range(16):
        a = 2.0 * math.pi * k / 16.0
        pts.append(
            RoadControlPoint(
                x=cx + rx * math.cos(a),
                y=cy,
                z=cz + rz * math.sin(a),
                width=width,
                bank_deg=bank_deg * (math.cos(a) ** 2),
            )
        )
    return RoadPlan(name="banked_oval", points=pts, closed=True, style="oval")


def make_figure8_plan(
    center: tuple[float, float, float] = (0.0, 0.0, 0.0),
    rx: float = 85.0,
    rz: float = 50.0,
    width: float = 15.0,
) -> RoadPlan:
    cx, cy, cz = center
    pts = []
    for k in range(16):
        a = 2.0 * math.pi * k / 16.0
        elev = 4.0 * (1.0 - math.cos(a))
        pts.append(
            RoadControlPoint(
                x=cx + rx * math.sin(a),
                y=cy + elev,
                z=cz + rz * math.sin(2.0 * a),
                width=width,
                bank_deg=-6.0 * math.sin(a),
                is_bridge=elev > 5.5,
            )
        )
    return RoadPlan(name="figure8", points=pts, closed=True, style="figure8")


def make_jump_ramp_plan(
    start: tuple[float, float, float] = (0.0, 0.0, 0.0),
    length: float = 40.0,
    height: float = 5.5,
    width: float = 14.0,
) -> RoadPlan:
    sx, sy, sz = start
    fracs = (0.0, 0.25, 0.5, 0.75, 1.0)
    pts = [
        RoadControlPoint(
            x=sx,
            y=sy + height * (f * f),
            z=sz + length * f,
            width=width,
        )
        for f in fracs
    ]
    slope = max(0.25, (height / max(length, 1.0)) * 2.5)
    return RoadPlan(name="jump_ramp", points=pts, closed=False, max_grade=slope, style="ramp")


def make_bridge_plan(
    start: tuple[float, float, float] = (0.0, 0.0, 0.0),
    end: tuple[float, float, float] = (0.0, 0.0, 160.0),
    deck_height: float = 10.0,
    width: float = 14.0,
) -> RoadPlan:
    p0 = np.asarray(start, dtype=np.float64)
    p1 = np.asarray(end, dtype=np.float64)
    profile = (
        (0.0, 0.0, False),
        (0.15, 0.5, False),
        (0.30, 1.0, True),
        (0.50, 1.0, True),
        (0.70, 1.0, True),
        (0.85, 0.5, False),
        (1.0, 0.0, False),
    )
    pts = []
    for f, h_frac, bridge in profile:
        pos = p0 + (p1 - p0) * f
        pts.append(
            RoadControlPoint(
                x=float(pos[0]),
                y=float(pos[1] + deck_height * h_frac),
                z=float(pos[2]),
                width=width,
                is_bridge=bridge,
            )
        )
    return RoadPlan(name="bridge", points=pts, closed=False, style="bridge")
