"""Procedural 3D road mesh, barriers, bridge piers & organic island builder."""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import math
from typing import Callable

import numpy as np

from road_spline import (
    RoadControlPoint,
    RoadPlan,
    catmull_rom_sample,
    compute_station_frames,
    grade_limited_profile,
    make_banked_oval_plan,
    make_bridge_plan,
    make_figure8_plan,
    make_jump_ramp_plan,
    resample_polyline,
)
from world_loader import MeshPart, WorldModel

__all__ = [
    "RoadControlPoint",
    "RoadPlan",
    "BuiltRoad",
    "catmull_rom_sample",
    "resample_polyline",
    "grade_limited_profile",
    "compute_station_frames",
    "make_banked_oval_plan",
    "make_figure8_plan",
    "make_jump_ramp_plan",
    "make_bridge_plan",
    "build_road_mesh",
    "build_organic_island",
]

_BARRIER_PROFILES: dict[str, list[tuple[float, float]]] = {
    "jersey": [(-0.30, 0.0), (-0.30, 0.15), (-0.15, 0.35), (-0.10, 1.05), (0.10, 1.05), (0.15, 0.35), (0.30, 0.15), (0.30, 0.0)],
    "guardrail": [(-0.12, 0.0), (-0.12, 0.85), (0.12, 0.85), (0.12, 0.0)],
    "derby": [(-0.35, 0.0), (-0.35, 2.20), (0.35, 2.20), (0.35, 0.0)],
}


@dataclass
class BuiltRoad:
    plan: RoadPlan
    model: WorldModel
    stations_xyz: np.ndarray
    arc_s: np.ndarray
    tangents: np.ndarray
    rights: np.ndarray
    ups: np.ndarray
    widths: np.ndarray
    bank_deg: np.ndarray
    ground_quads: list[np.ndarray]
    wall_boxes: list[dict]
    junction_mouths: list[tuple[np.ndarray, float]]


class _MeshAccum:
    def __init__(self, texture: str) -> None:
        self.texture = texture
        self.verts: list[np.ndarray] = []
        self.inds: list[np.ndarray] = []
        self.vcount = 0

    def add(self, verts: np.ndarray, inds: np.ndarray) -> None:
        if len(verts) == 0 or len(inds) == 0:
            return
        self.verts.append(np.asarray(verts, dtype=np.float32))
        self.inds.append(np.asarray(inds, dtype=np.uint32) + np.uint32(self.vcount))
        self.vcount += len(verts)

    def add_quad(self, p0: np.ndarray, p1: np.ndarray, p2: np.ndarray, p3: np.ndarray, normal: np.ndarray, uvs: tuple[tuple[float, float], ...]) -> None:
        n = np.asarray(normal, dtype=np.float64)
        nl = float(np.linalg.norm(n))
        n = n / nl if nl > 1e-8 else np.array([0.0, 1.0, 0.0])
        pts = (p0, p1, p2, p3)
        v = np.empty((4, 8), dtype=np.float32)
        for k in range(4):
            v[k, 0:3] = pts[k]
            v[k, 3:6] = n
            v[k, 6:8] = uvs[k]
        self.add(v, np.array([0, 1, 2, 0, 2, 3], dtype=np.uint32))

    def to_part(self, pivot: np.ndarray) -> MeshPart | None:
        if self.vcount == 0:
            return None
        v = np.vstack(self.verts)
        v[:, 0:3] -= pivot.astype(np.float32)
        idx = np.concatenate(self.inds)
        return MeshPart(texture=self.texture, vertices=v, indices=idx, tri_count=len(idx) // 3)


def _extrude_ribbon(
    mb: _MeshAccum,
    pts_grid: np.ndarray,
    norms_grid: np.ndarray,
    u_coords: np.ndarray,
    v_coords: np.ndarray,
    flip: bool = False,
) -> None:
    """Extrude a (R, C, 3) grid of points and normals into quads."""
    rows, cols = pts_grid.shape[:2]
    if rows < 2 or cols < 2:
        return
    verts = np.empty((rows, cols, 8), dtype=np.float32)
    verts[:, :, 0:3] = pts_grid
    nl = np.linalg.norm(norms_grid, axis=2, keepdims=True)
    verts[:, :, 3:6] = np.where(nl > 1e-8, norms_grid / np.maximum(nl, 1e-8), np.array([0.0, 1.0, 0.0]))
    verts[:, :, 6] = u_coords[None, :]
    verts[:, :, 7] = v_coords[:, None]
    r_idx = np.arange(rows - 1, dtype=np.uint32)[:, None]
    c_idx = np.arange(cols - 1, dtype=np.uint32)[None, :]
    i00 = r_idx * cols + c_idx
    i10 = i00 + 1
    i01 = i00 + cols
    i11 = i01 + 1
    tris = np.stack([i00, i10, i11, i00, i11, i01] if not flip else [i00, i11, i10, i00, i01, i11], axis=-1)
    mb.add(verts.reshape(-1, 8), tris.reshape(-1))


def _add_barrier_segment(
    mb: _MeshAccum,
    p0: np.ndarray,
    p1: np.ndarray,
    r0: np.ndarray,
    r1: np.ndarray,
    u0: np.ndarray,
    u1: np.ndarray,
    lat0: float,
    lat1: float,
    s0: float,
    s1: float,
    btype: str,
) -> dict:
    """Extrude one barrier segment with crisp face normals and return its OBB dict."""
    prof = _BARRIER_PROFILES.get(btype, _BARRIER_PROFILES["jersey"])
    n_p = len(prof)
    v0, v1 = s0 / 4.0, s1 / 4.0
    xs = [p[0] for p in prof]
    ys = [p[1] for p in prof]
    cx, cy = 0.5 * (min(xs) + max(xs)), 0.5 * (min(ys) + max(ys))
    half_w, half_h = 0.5 * (max(xs) - min(xs)), 0.5 * (max(ys) - min(ys))

    for k in range(n_p - 1):
        x_a, y_a = prof[k]
        x_b, y_b = prof[k + 1]
        q00 = p0 + r0 * (lat0 + x_a) + u0 * y_a
        q10 = p0 + r0 * (lat0 + x_b) + u0 * y_b
        q11 = p1 + r1 * (lat1 + x_b) + u1 * y_b
        q01 = p1 + r1 * (lat1 + x_a) + u1 * y_a
        dx, dy = x_b - x_a, y_b - y_a
        r_m = 0.5 * (r0 + r1)
        u_m = 0.5 * (u0 + u1)
        n = -dy * r_m + dx * u_m
        seg_u0, seg_u1 = k / (n_p - 1), (k + 1) / (n_p - 1)
        mb.add_quad(q00, q10, q11, q01, n, ((seg_u0, v0), (seg_u1, v0), (seg_u1, v1), (seg_u0, v1)))

    c0 = p0 + r0 * (lat0 + cx) + u0 * cy
    c1 = p1 + r1 * (lat1 + cx) + u1 * cy
    centre = 0.5 * (c0 + c1)
    seg_vec = c1 - c0
    seg_len = max(float(np.linalg.norm(seg_vec)), 1e-4)
    t_ax = seg_vec / seg_len
    u_ax = 0.5 * (u0 + u1)
    u_ax = u_ax - np.dot(u_ax, t_ax) * t_ax
    u_len = float(np.linalg.norm(u_ax))
    u_ax = u_ax / u_len if u_len > 1e-8 else np.array([0.0, 1.0, 0.0])
    r_ax = np.cross(u_ax, t_ax)
    r_len = float(np.linalg.norm(r_ax))
    r_ax = r_ax / r_len if r_len > 1e-8 else np.array([1.0, 0.0, 0.0])
    return {
        "centre": [float(centre[0]), float(centre[1]), float(centre[2])],
        "radii": [float(half_w), float(half_h), float(0.5 * seg_len)],
        "axes": [r_ax.tolist(), u_ax.tolist(), t_ax.tolist()],
        "surface": btype,
    }


def _add_pier(mb: _MeshAccum, cx: float, top_y: float, bot_y: float, cz: float, fwd: np.ndarray, size: float = 1.8) -> None:
    """Emit a vertical square support pier extending from top_y down to bot_y."""
    if bot_y >= top_y - 0.05:
        return
    f = np.array([float(fwd[0]), 0.0, float(fwd[2])], dtype=np.float64)
    fl = float(np.linalg.norm(f))
    f = f / fl if fl > 1e-6 else np.array([0.0, 0.0, 1.0])
    r = np.array([f[2], 0.0, -f[0]], dtype=np.float64)
    h = 0.5 * size
    base = np.array([cx, bot_y, cz], dtype=np.float64)
    top = np.array([cx, top_y, cz], dtype=np.float64)
    corners = [(-h, -h), (h, -h), (h, h), (-h, h)]
    normals = [-f, r, f, -r]
    v_top = (top_y - bot_y) / 4.0
    for k in range(4):
        a0, b0 = corners[k]
        a1, b1 = corners[(k + 1) % 4]
        p0 = base + r * a0 + f * b0
        p1 = base + r * a1 + f * b1
        p2 = top + r * a1 + f * b1
        p3 = top + r * a0 + f * b0
        mb.add_quad(p0, p1, p2, p3, normals[k], ((0.0, 0.0), (1.0, 0.0), (1.0, v_top), (0.0, v_top)))


def build_road_mesh(plan: RoadPlan, ground_fn: Callable[[float, float], float] | None = None) -> BuiltRoad:
    """Build procedural 3D road geometry, barriers, bridge structure, piers & collision data."""
    if len(plan.points) < 2:
        raise ValueError("RoadPlan requires at least 2 control points")

    sps = 20
    cp_xyz = np.array([[p.x, p.y, p.z] for p in plan.points], dtype=np.float64)
    sampled = catmull_rom_sample(cp_xyz, closed=plan.closed, samples_per_seg=sps)
    poly = np.vstack([sampled, sampled[:1]]) if plan.closed else sampled
    cum = np.concatenate([[0.0], np.cumsum(np.linalg.norm(np.diff(poly, axis=0), axis=1))])
    total_len = max(float(cum[-1]), 1e-6)
    n_cp = len(plan.points)
    cp_t = np.array([cum[min(k * sps, len(cum) - 1)] / total_len for k in range(n_cp)], dtype=np.float64)

    stations_xyz, arc_s, t_params = resample_polyline(sampled, step_m=plan.step_m, closed=plan.closed)
    n_st = len(stations_xyz)

    def _interp_attr(vals: list[float]) -> np.ndarray:
        if plan.closed:
            return np.interp(t_params, np.append(cp_t, 1.0), np.append(vals, vals[0]))
        return np.interp(t_params, cp_t, vals)

    widths = _interp_attr([p.width for p in plan.points])
    bank_deg = _interp_attr([p.bank_deg for p in plan.points])
    bridge_f = _interp_attr([1.0 if p.is_bridge else 0.0 for p in plan.points])
    is_bridge = bridge_f >= 0.5

    # Nearest control point per station for discrete barrier strings
    cp_t_ext = np.append(cp_t, 1.0) if plan.closed else cp_t
    bl_vals = [p.barrier_left for p in plan.points] + ([plan.points[0].barrier_left] if plan.closed else [])
    br_vals = [p.barrier_right for p in plan.points] + ([plan.points[0].barrier_right] if plan.closed else [])
    nearest_cp = np.argmin(np.abs(t_params[:, None] - cp_t_ext[None, :]), axis=1)
    barrier_left = [bl_vals[i] for i in nearest_cp]
    barrier_right = [br_vals[i] for i in nearest_cp]

    # Pin endpoints and junction stations before smoothing elevation
    pinned = np.zeros(n_st, dtype=bool)
    is_junction_st = np.zeros(n_st, dtype=bool)
    if not plan.closed:
        pinned[0] = True
        pinned[-1] = True
    junc_cp_pairs: list[tuple[int, RoadControlPoint]] = []
    for k, cp in enumerate(plan.points):
        if cp.is_junction:
            j_idx = int(np.argmin(np.abs(t_params - cp_t[k])))
            pinned[j_idx] = True
            is_junction_st[j_idx] = True
            junc_cp_pairs.append((j_idx, cp))

    stations_xyz = stations_xyz.copy()
    stations_xyz[:, 1] = grade_limited_profile(
        arc_s, stations_xyz[:, 1], pinned, max_grade=plan.max_grade, smooth_passes=plan.smooth_passes
    )
    tangents, rights, ups = compute_station_frames(stations_xyz, bank_deg, closed=plan.closed)

    junction_mouths = [
        (stations_xyz[j_idx].astype(np.float32), float(cp.width * 0.5 + 4.0)) for j_idx, cp in junc_cp_pairs
    ]
    junc_indices = np.flatnonzero(is_junction_st)

    # Wrap row sequence for seamless closed UV extrusion
    row_idx = np.append(np.arange(n_st), 0) if plan.closed else np.arange(n_st)
    s_wrap = np.append(arc_s, total_len) if plan.closed else arc_s
    P, R, U = stations_xyz[row_idx], rights[row_idx], ups[row_idx]
    HW = 0.5 * widths[row_idx]
    v_road = s_wrap / 12.0

    surface_mb = _MeshAccum(plan.surface_tex)
    wear_mb = _MeshAccum(plan.wear_tex)
    deck_mb = _MeshAccum(plan.deck_tex)
    barrier_mb = _MeshAccum(plan.barrier_tex)

    # 1. Driving surface with +0.08m center crown
    crown = 0.08
    drv_pts = np.stack([P - R * HW[:, None], P + U * crown, P + R * HW[:, None]], axis=1)
    drv_nrm = np.broadcast_to(U[:, None, :], drv_pts.shape)
    _extrude_ribbon(surface_mb, drv_pts, drv_nrm, np.array([0.0, 0.5, 1.0]), v_road)

    # Ground quads for every driving deck quad
    ground_quads: list[np.ndarray] = []
    n_segs = len(row_idx) - 1
    for i in range(n_segs):
        for c in (0, 1):
            q = np.array([drv_pts[i, c], drv_pts[i, c + 1], drv_pts[i + 1, c + 1], drv_pts[i + 1, c]], dtype=np.float32)
            ground_quads.append(q)

    # 2. Tire-wear decal overlay (+0.03m normal offset across inner 70% width)
    wear_hw = 0.70 * HW
    wear_edge_h = 0.03 + crown * 0.30
    wear_pts = np.stack(
        [P - R * wear_hw[:, None] + U * wear_edge_h, P + U * (crown + 0.03), P + R * wear_hw[:, None] + U * wear_edge_h],
        axis=1,
    )
    _extrude_ribbon(wear_mb, wear_pts, np.broadcast_to(U[:, None, :], wear_pts.shape), np.array([0.0, 0.5, 1.0]), v_road)

    # 3. Shoulders / kerbs (1.5m outer bands)
    sh_w = 1.5
    sh_l = np.stack([P - R * (HW + sh_w)[:, None] - U * 0.04, P - R * HW[:, None]], axis=1)
    sh_r = np.stack([P + R * HW[:, None], P + R * (HW + sh_w)[:, None] - U * 0.04], axis=1)
    _extrude_ribbon(deck_mb, sh_l, np.broadcast_to(U[:, None, :], sh_l.shape), np.array([0.0, 1.0]), v_road)
    _extrude_ribbon(deck_mb, sh_r, np.broadcast_to(U[:, None, :], sh_r.shape), np.array([0.0, 1.0]), v_road)

    # 4. Barriers + 5. Bridge deck underside & fascia per segment
    wall_boxes: list[dict] = []
    deck_thick = 1.2
    for i in range(n_segs):
        a, b = int(row_idx[i]), int(row_idx[i + 1])
        seg_mid = 0.5 * (stations_xyz[a] + stations_xyz[b])
        seg_hw = 0.25 * (widths[a] + widths[b])
        near_junction = any(
            float(np.linalg.norm(seg_mid - stations_xyz[j])) <= max(seg_hw, 0.5 * widths[j]) + 2.0
            for j in junc_indices
        )
        if not near_junction:
            bl = barrier_left[a]
            if bl and bl != "none":
                wall_boxes.append(
                    _add_barrier_segment(
                        barrier_mb, stations_xyz[a], stations_xyz[b], rights[a], rights[b], ups[a], ups[b],
                        -(0.5 * widths[a] + 1.15), -(0.5 * widths[b] + 1.15), s_wrap[i], s_wrap[i + 1], bl,
                    )
                )
            br = barrier_right[a]
            if br and br != "none":
                wall_boxes.append(
                    _add_barrier_segment(
                        barrier_mb, stations_xyz[a], stations_xyz[b], rights[a], rights[b], ups[a], ups[b],
                        0.5 * widths[a] + 1.15, 0.5 * widths[b] + 1.15, s_wrap[i], s_wrap[i + 1], br,
                    )
                )
            if plan.median_barrier:
                wall_boxes.append(
                    _add_barrier_segment(
                        barrier_mb, stations_xyz[a], stations_xyz[b], rights[a], rights[b], ups[a], ups[b],
                        0.0, 0.0, s_wrap[i], s_wrap[i + 1], "jersey",
                    )
                )

        if is_bridge[a] or is_bridge[b]:
            ow_a, ow_b = 0.5 * widths[a] + sh_w, 0.5 * widths[b] + sh_w
            lt_a, rt_a = stations_xyz[a] - rights[a] * ow_a, stations_xyz[a] + rights[a] * ow_a
            lt_b, rt_b = stations_xyz[b] - rights[b] * ow_b, stations_xyz[b] + rights[b] * ow_b
            lb_a, rb_a = lt_a - ups[a] * deck_thick, rt_a - ups[a] * deck_thick
            lb_b, rb_b = lt_b - ups[b] * deck_thick, rt_b - ups[b] * deck_thick
            va, vb = s_wrap[i] / 6.0, s_wrap[i + 1] / 6.0
            u_m, r_m = 0.5 * (ups[a] + ups[b]), 0.5 * (rights[a] + rights[b])
            deck_mb.add_quad(lb_a, lb_b, rb_b, rb_a, -u_m, ((0.0, va), (0.0, vb), (1.0, vb), (1.0, va)))
            deck_mb.add_quad(lt_a, lb_a, lb_b, lt_b, -r_m, ((0.0, va), (1.0, va), (1.0, vb), (0.0, vb)))
            deck_mb.add_quad(rt_a, rt_b, rb_b, rb_a, r_m, ((0.0, va), (0.0, vb), (1.0, vb), (1.0, va)))

    # 5b. Bridge support piers spaced every plan.pier_spacing_m on contiguous bridge spans
    spacing = max(float(plan.pier_spacing_m), 4.0)
    idx = 0
    while idx < n_st:
        if not is_bridge[idx]:
            idx += 1
            continue
        start = idx
        while idx < n_st and is_bridge[idx]:
            idx += 1
        end = idx - 1
        span_len = float(arc_s[end] - arc_s[start])
        target_s = float(arc_s[start]) + min(0.5 * spacing, 0.5 * span_len)
        while target_s <= float(arc_s[end]) + 1e-6:
            st_i = int(np.argmin(np.abs(arc_s[start : end + 1] - target_s))) + start
            px, py, pz = stations_xyz[st_i]
            bot_y = float(ground_fn(float(px), float(pz))) - 2.0 if ground_fn is not None else float(py) - 12.0
            _add_pier(deck_mb, float(px), float(py) - 1.1, bot_y, float(pz), tangents[st_i], size=1.8)
            target_s += spacing

    # Compute world centroid pivot and assemble recentered WorldModel
    all_pts = np.vstack([v[:, 0:3] for mb in (surface_mb, wear_mb, deck_mb, barrier_mb) for v in mb.verts])
    pivot = all_pts.mean(axis=0).astype(np.float32)
    parts = [p for mb in (surface_mb, wear_mb, deck_mb, barrier_mb) if (p := mb.to_part(pivot)) is not None]
    local_pts = np.vstack([p.vertices[:, 0:3] for p in parts])
    bbox_min = local_pts.min(axis=0).astype(np.float32)
    bbox_max = local_pts.max(axis=0).astype(np.float32)
    textures = list(dict.fromkeys(p.texture for p in parts))
    tri_count = sum(p.tri_count for p in parts)
    hash_hex = hashlib.sha256(json.dumps(plan.to_dict(), sort_keys=True).encode("utf-8")).hexdigest()[:16]

    model = WorldModel(
        key=f"road:{plan.name}",
        name=plan.name,
        category="Roads",
        model_store="procedural:road",
        hash_hex=hash_hex,
        parts=parts,
        tri_count=tri_count,
        textures=textures,
        bbox_min=bbox_min,
        bbox_max=bbox_max,
        pivot=pivot,
        placed_by_map=True,
        model_offset=None,
        subtype="Procedural Road",
    )
    return BuiltRoad(
        plan=plan,
        model=model,
        stations_xyz=stations_xyz,
        arc_s=arc_s,
        tangents=tangents,
        rights=rights,
        ups=ups,
        widths=widths,
        bank_deg=bank_deg,
        ground_quads=ground_quads,
        wall_boxes=wall_boxes,
        junction_mouths=junction_mouths,
    )


def build_organic_island(
    center_xz: tuple[float, float],
    radius_x: float,
    radius_z: float,
    height: float = 6.0,
    rings: int = 8,
    sectors: int = 24,
    seed: int = 1,
    tex: str = "W_dirt.tex",
) -> MeshPart:
    """Build an organic dome island with radial sine-noise coastline, unit normals & planar UVs."""
    cx, cz = float(center_xz[0]), float(center_xz[1])
    rx, rz = max(float(radius_x), 0.5), max(float(radius_z), 0.5)
    rings = max(int(rings), 2)
    sectors = max(int(sectors), 6)
    rng = np.random.default_rng(seed)
    phases = rng.uniform(0.0, 2.0 * math.pi, size=4)
    amps = np.array([0.14, 0.09, 0.06, 0.03], dtype=np.float64)
    freqs = np.array([2.0, 3.0, 5.0, 7.0], dtype=np.float64)

    positions = [np.array([cx, float(height), cz], dtype=np.float64)]
    for r_i in range(1, rings + 1):
        u_r = r_i / rings
        dome = (math.cos(0.5 * math.pi * u_r) ** 1.5) * float(height)
        for s_i in range(sectors):
            theta = 2.0 * math.pi * s_i / sectors
            noise = float(np.sum(amps * np.sin(freqs * theta + phases)))
            scale = max(0.65, min(1.35, 1.0 + noise * (0.35 + 0.65 * u_r)))
            px = cx + math.cos(theta) * rx * u_r * scale
            pz = cz + math.sin(theta) * rz * u_r * scale
            py = dome * (1.0 + 0.08 * noise * (1.0 - u_r))
            positions.append(np.array([px, py, pz], dtype=np.float64))

    pos_arr = np.asarray(positions, dtype=np.float64)
    tris: list[tuple[int, int, int]] = []
    for s_i in range(sectors):
        s_next = (s_i + 1) % sectors
        tris.append((0, 1 + s_next, 1 + s_i))
    for r_i in range(rings - 1):
        b0 = 1 + r_i * sectors
        b1 = b0 + sectors
        for s_i in range(sectors):
            s_next = (s_i + 1) % sectors
            a0, a1 = b0 + s_i, b0 + s_next
            c0, c1 = b1 + s_i, b1 + s_next
            tris.append((a0, a1, c1))
            tris.append((a0, c1, c0))

    idx_arr = np.asarray(tris, dtype=np.uint32).reshape(-1)
    normals = np.zeros_like(pos_arr)
    t_mat = idx_arr.reshape(-1, 3)
    v0, v1, v2 = pos_arr[t_mat[:, 0]], pos_arr[t_mat[:, 1]], pos_arr[t_mat[:, 2]]
    fn = np.cross(v1 - v0, v2 - v0)
    # Ensure upward outward normal orientation in LH coordinates
    flip_mask = fn[:, 1] < 0.0
    if np.any(flip_mask):
        fn[flip_mask] = -fn[flip_mask]
        t_mat[flip_mask, 1], t_mat[flip_mask, 2] = t_mat[flip_mask, 2].copy(), t_mat[flip_mask, 1].copy()
        idx_arr = t_mat.reshape(-1)
    for col in range(3):
        np.add.at(normals, t_mat[:, col], fn)
    nl = np.linalg.norm(normals, axis=1, keepdims=True)
    normals = np.where(nl > 1e-8, normals / np.maximum(nl, 1e-8), np.array([0.0, 1.0, 0.0]))

    verts = np.empty((len(pos_arr), 8), dtype=np.float32)
    verts[:, 0:3] = pos_arr
    verts[:, 3:6] = normals
    verts[:, 6] = (pos_arr[:, 0] - cx) / 16.0
    verts[:, 7] = (pos_arr[:, 2] - cz) / 16.0
    return MeshPart(texture=tex, vertices=verts, indices=idx_arr, tri_count=len(idx_arr) // 3)
