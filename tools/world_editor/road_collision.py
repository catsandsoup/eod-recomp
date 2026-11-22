"""Junction surgery, terrain corridor sculpting, 32-bit .eodcol collision, AI ILIN & minimap synthesis."""
from __future__ import annotations

from dataclasses import dataclass
import math
import struct

import numpy as np
from PIL import Image, ImageDraw

from road_builder import BuiltRoad
from world_loader import RoadEdge, RoadGraph, RoadNode, WorldModel, WorldObject

EODCOL_MAGIC = b"EODCOL32"
EODCOL_VERSION = 1
_HEADER_STRUCT = struct.Struct("<8sIIII6f")
_BOX_STRUCT = struct.Struct("<15f")

SURFACE_TERRAIN = 0
SURFACE_ROAD = 1
SURFACE_OBJECT = 2

__all__ = [
    "EODCOL_MAGIC",
    "EODCOL_VERSION",
    "SURFACE_TERRAIN",
    "SURFACE_ROAD",
    "SURFACE_OBJECT",
    "EodCollisionMesh32",
    "open_eod_junction",
    "sculpt_terrain_along_corridor",
    "build_unified_collision_32bit",
    "pack_eodcol",
    "unpack_eodcol",
    "query_ground_y_32bit",
    "synthesize_road_graph",
    "rasterize_minimap_rgba",
]


@dataclass
class EodCollisionMesh32:
    vertices: np.ndarray      # (V, 3) float32
    triangles: np.ndarray     # (T, 3) uint32
    surface_ids: np.ndarray   # (T,) uint8
    wall_boxes: list[dict]
    aabb_min: np.ndarray      # (3,) float32
    aabb_max: np.ndarray      # (3,) float32


def open_eod_junction(
    sobl_records: list[dict],
    sobl_edits: dict[int, dict],
    junction_mouths: list[tuple[np.ndarray, float]],
) -> list[int]:
    """Flatten SOBL box barriers overlapping any junction mouth within |dy| <= 12m."""
    if not sobl_records or not junction_mouths:
        return []
    mouths = [(np.asarray(pos, dtype=np.float64), float(rad)) for pos, rad in junction_mouths]
    opened: set[int] = set()
    for rec in sobl_records:
        if rec.get("kind") != "box":
            continue
        idx = int(rec["index"])
        cx, cy, cz = (float(v) for v in rec["centre"])
        rx, ry, rz = (float(v) for v in rec["radii"])
        box_r_xz = max(abs(rx), abs(rz))
        for m_pos, m_rad in mouths:
            if abs(cy - float(m_pos[1])) > 12.0:
                continue
            if math.hypot(cx - float(m_pos[0]), cz - float(m_pos[2])) <= m_rad + box_r_xz:
                entry = dict(sobl_edits.get(idx, {}))
                entry.setdefault("orig_ry", ry)
                entry["ry"] = 0.05
                entry["flattened_by_junction"] = True
                sobl_edits[idx] = entry
                opened.add(idx)
                break
    return sorted(opened)


def _station_bridge_mask(road: BuiltRoad) -> np.ndarray:
    """Map control-point is_bridge flags onto resampled road stations."""
    n = len(road.stations_xyz)
    mask = np.zeros(n, dtype=bool)
    pts = road.plan.points
    if n == 0 or not pts:
        return mask
    if len(pts) == 1:
        mask[:] = bool(pts[0].is_bridge)
        return mask
    cp_xyz = np.array([[p.x, p.y, p.z] for p in pts], dtype=np.float64)
    cp_flags = np.array([1.0 if p.is_bridge else 0.0 for p in pts], dtype=np.float64)
    if road.plan.closed:
        cp_xyz = np.vstack([cp_xyz, cp_xyz[0:1]])
        cp_flags = np.append(cp_flags, cp_flags[0])
    seg_len = np.linalg.norm(np.diff(cp_xyz, axis=0), axis=1)
    cp_s = np.concatenate([[0.0], np.cumsum(seg_len)])
    total_cp = float(cp_s[-1])
    total_st = float(road.arc_s[-1]) if len(road.arc_s) else 0.0
    if total_cp <= 1e-6 or total_st <= 1e-6:
        return mask
    u_st = np.asarray(road.arc_s, dtype=np.float64) / total_st
    u_cp = cp_s / total_cp
    return np.interp(u_st, u_cp, cp_flags) >= 0.5


def sculpt_terrain_along_corridor(
    terrain_verts: np.ndarray,
    road: BuiltRoad,
    shoulder_skirt_m: float = 8.0,
    clearance_m: float = 0.08,
) -> tuple[np.ndarray, int]:
    """Smoothly blend terrain Y beneath non-bridge road stations with cosine shoulder falloff."""
    out = np.array(terrain_verts, dtype=np.float32, copy=True)
    if out.size == 0 or len(road.stations_xyz) == 0:
        return out, 0

    bridge_mask = _station_bridge_mask(road)
    st_xyz = np.asarray(road.stations_xyz, dtype=np.float64)
    st_hw = np.asarray(road.widths, dtype=np.float64) * 0.5
    n_st = len(st_xyz)

    # Build non-bridge segments (a -> b) so sculpting stays smooth across stations and skips bridges.
    seg_pairs: list[tuple[int, int]] = []
    for i in range(n_st - 1):
        if not bridge_mask[i] and not bridge_mask[i + 1]:
            seg_pairs.append((i, i + 1))
    if road.plan.closed and n_st >= 3 and not bridge_mask[n_st - 1] and not bridge_mask[0]:
        seg_pairs.append((n_st - 1, 0))
    if not seg_pairs:
        non_bridge_idx = np.flatnonzero(~bridge_mask)
        if len(non_bridge_idx) == 0:
            return out, 0
        seg_pairs = [(int(i), int(i)) for i in non_bridge_idx]

    idx_a = np.array([a for a, _ in seg_pairs], dtype=np.int64)
    idx_b = np.array([b for _, b in seg_pairs], dtype=np.int64)
    ax, ay, az = st_xyz[idx_a, 0], st_xyz[idx_a, 1], st_xyz[idx_a, 2]
    bx, by, bz = st_xyz[idx_b, 0], st_xyz[idx_b, 1], st_xyz[idx_b, 2]
    hwa, hwb = st_hw[idx_a], st_hw[idx_b]
    dx, dz = bx - ax, bz - az
    len2 = dx * dx + dz * dz

    skirt = max(float(shoulder_skirt_m), 1e-4)
    max_reach = float(np.max(st_hw)) + skirt
    min_x, max_x = float(np.min(st_xyz[:, 0]) - max_reach), float(np.max(st_xyz[:, 0]) + max_reach)
    min_z, max_z = float(np.min(st_xyz[:, 2]) - max_reach), float(np.max(st_xyz[:, 2]) + max_reach)

    cand_idx = np.flatnonzero(
        (out[:, 0] >= min_x) & (out[:, 0] <= max_x) & (out[:, 2] >= min_z) & (out[:, 2] <= max_z)
    )
    if len(cand_idx) == 0:
        return out, 0

    orig_y = out[:, 1].copy()
    chunk = 2048
    for start in range(0, len(cand_idx), chunk):
        sub = cand_idx[start : start + chunk]
        vx = out[sub, 0].astype(np.float64)[:, None]
        vz = out[sub, 2].astype(np.float64)[:, None]
        px = vx - ax[None, :]
        pz = vz - az[None, :]
        u = np.where(len2[None, :] > 1e-9, (px * dx[None, :] + pz * dz[None, :]) / np.maximum(len2[None, :], 1e-9), 0.0)
        u = np.clip(u, 0.0, 1.0)
        cx = ax[None, :] + u * dx[None, :]
        cz = az[None, :] + u * dz[None, :]
        dist = np.hypot(vx - cx, vz - cz)
        best = np.argmin(dist, axis=1)
        row = np.arange(len(sub))
        d_min = dist[row, best]
        u_best = u[row, best]
        hw = hwa[best] + u_best * (hwb[best] - hwa[best])
        road_y = ay[best] + u_best * (by[best] - ay[best])

        inside = d_min < (hw + skirt)
        if not np.any(inside):
            continue
        t = np.clip((d_min[inside] - hw[inside]) / skirt, 0.0, 1.0)
        weight = 0.5 * (1.0 + np.cos(np.pi * t))
        target_y = road_y[inside] - float(clearance_m)
        vy = out[sub[inside], 1].astype(np.float64)
        out[sub[inside], 1] = (vy * (1.0 - weight) + target_y * weight).astype(np.float32)

    modified = int(np.count_nonzero(np.abs(out[:, 1] - orig_y) > 1e-6))
    return out, modified


def _normalize_wall_box(box: dict) -> dict:
    centre = [float(np.float32(v)) for v in box["centre"][:3]]
    radii = [float(np.float32(v)) for v in box["radii"][:3]]
    axes = [[float(np.float32(v)) for v in row[:3]] for row in box["axes"][:3]]
    return {"centre": centre, "radii": radii, "axes": axes}


def _extract_object_obb(world_pts: np.ndarray, rot_scale: np.ndarray) -> dict:
    """Compute a tight oriented bounding box aligned with the object's local frame."""
    cols = []
    for c in range(3):
        vec = rot_scale[:, c].astype(np.float64)
        n = float(np.linalg.norm(vec))
        cols.append(vec / n if n > 1e-8 else np.eye(3, dtype=np.float64)[c])
    # Gram-Schmidt orthonormalization for skewed/scaled transforms
    u0 = cols[0]
    u1 = cols[1] - np.dot(cols[1], u0) * u0
    u1 = u1 / np.linalg.norm(u1) if np.linalg.norm(u1) > 1e-8 else np.array([0.0, 1.0, 0.0])
    u2 = np.cross(u0, u1)
    u2 = u2 / np.linalg.norm(u2) if np.linalg.norm(u2) > 1e-8 else np.array([0.0, 0.0, 1.0])
    axes = np.vstack([u0, u1, u2])
    proj = world_pts.astype(np.float64) @ axes.T
    p_min = proj.min(axis=0)
    p_max = proj.max(axis=0)
    mid = 0.5 * (p_min + p_max)
    radii = np.maximum(0.5 * (p_max - p_min), 0.05)
    centre = mid @ axes
    return _normalize_wall_box({"centre": centre.tolist(), "radii": radii.tolist(), "axes": axes.tolist()})


def build_unified_collision_32bit(
    built_roads: list[BuiltRoad],
    edited_objects: list[WorldObject],
    models: dict[str, WorldModel],
    base_scol_tris: np.ndarray | None = None,
) -> EodCollisionMesh32:
    """Assemble 32-bit indexed ground triangles and OBB walls from base terrain, roads & edited objects."""
    vert_chunks: list[np.ndarray] = []
    tri_chunks: list[np.ndarray] = []
    surf_chunks: list[np.ndarray] = []
    wall_boxes: list[dict] = []
    vert_offset = 0

    if base_scol_tris is not None and len(base_scol_tris) > 0:
        arr = np.asarray(base_scol_tris, dtype=np.float32).reshape(-1, 3, 3)
        k = len(arr)
        vert_chunks.append(arr.reshape(-1, 3))
        tri_chunks.append(np.arange(vert_offset, vert_offset + 3 * k, dtype=np.uint32).reshape(k, 3))
        surf_chunks.append(np.full(k, SURFACE_TERRAIN, dtype=np.uint8))
        vert_offset += 3 * k

    for road in built_roads:
        if road.ground_quads:
            quads = np.asarray(road.ground_quads, dtype=np.float32).reshape(-1, 4, 3)
            q_count = len(quads)
            vert_chunks.append(quads.reshape(-1, 3))
            base = (np.arange(q_count, dtype=np.uint32) * 4 + vert_offset)[:, None]
            t1 = np.hstack([base, base + 1, base + 2])
            t2 = np.hstack([base, base + 2, base + 3])
            tri_chunks.append(np.vstack([t1, t2]))
            surf_chunks.append(np.full(2 * q_count, SURFACE_ROAD, dtype=np.uint8))
            vert_offset += 4 * q_count
        for box in road.wall_boxes:
            wall_boxes.append(_normalize_wall_box(box))

    for obj in edited_objects:
        if obj.deleted:
            continue
        model = models.get(obj.model_key)
        if model is None or not model.parts:
            continue
        mat = obj.world_matrix()
        rot_scale = mat[:3, :3]
        trans = mat[:3, 3]
        steep_pts: list[np.ndarray] = []
        for part in model.parts:
            if part.vertices.size == 0 or part.indices.size == 0:
                continue
            local_v = np.asarray(part.vertices[:, :3], dtype=np.float32)
            world_v = (local_v @ rot_scale.T + trans).astype(np.float32)
            idx = np.asarray(part.indices, dtype=np.uint32).reshape(-1, 3)
            v0, v1, v2 = world_v[idx[:, 0]], world_v[idx[:, 1]], world_v[idx[:, 2]]
            cross = np.cross(v1 - v0, v2 - v0)
            lens = np.linalg.norm(cross, axis=1)
            valid = lens > 1e-7
            if not np.any(valid):
                continue
            ny = np.abs(cross[valid, 1] / lens[valid])
            valid_idx = idx[valid]
            ground_tris = valid_idx[ny >= 0.5]
            steep_tris = valid_idx[ny < 0.5]
            if len(ground_tris) > 0:
                vert_chunks.append(world_v)
                tri_chunks.append(ground_tris + np.uint32(vert_offset))
                surf_chunks.append(np.full(len(ground_tris), SURFACE_OBJECT, dtype=np.uint8))
                vert_offset += len(world_v)
            if len(steep_tris) > 0:
                steep_pts.append(world_v[np.unique(steep_tris)])
        if steep_pts:
            wall_boxes.append(_extract_object_obb(np.vstack(steep_pts), rot_scale))

    if vert_chunks:
        vertices = np.ascontiguousarray(np.vstack(vert_chunks), dtype=np.float32)
        triangles = np.ascontiguousarray(np.vstack(tri_chunks), dtype=np.uint32)
        surface_ids = np.ascontiguousarray(np.concatenate(surf_chunks), dtype=np.uint8)
        aabb_min = vertices.min(axis=0).astype(np.float32)
        aabb_max = vertices.max(axis=0).astype(np.float32)
    else:
        vertices = np.zeros((0, 3), dtype=np.float32)
        triangles = np.zeros((0, 3), dtype=np.uint32)
        surface_ids = np.zeros((0,), dtype=np.uint8)
        aabb_min = np.zeros(3, dtype=np.float32)
        aabb_max = np.zeros(3, dtype=np.float32)

    return EodCollisionMesh32(
        vertices=vertices,
        triangles=triangles,
        surface_ids=surface_ids,
        wall_boxes=wall_boxes,
        aabb_min=aabb_min,
        aabb_max=aabb_max,
    )


def pack_eodcol(mesh: EodCollisionMesh32) -> bytes:
    """Serialize EodCollisionMesh32 into a compact little-endian EODCOL32 binary blob."""
    verts = np.ascontiguousarray(mesh.vertices, dtype="<f4").reshape(-1, 3)
    tris = np.ascontiguousarray(mesh.triangles, dtype="<u4").reshape(-1, 3)
    surfs = np.ascontiguousarray(mesh.surface_ids, dtype="u1").reshape(-1)
    a_min = np.asarray(mesh.aabb_min, dtype="<f4").reshape(3)
    a_max = np.asarray(mesh.aabb_max, dtype="<f4").reshape(3)
    header = _HEADER_STRUCT.pack(
        EODCOL_MAGIC,
        EODCOL_VERSION,
        len(verts),
        len(tris),
        len(mesh.wall_boxes),
        float(a_min[0]),
        float(a_min[1]),
        float(a_min[2]),
        float(a_max[0]),
        float(a_max[1]),
        float(a_max[2]),
    )
    box_buf = bytearray()
    for box in mesh.wall_boxes:
        c = box["centre"]
        r = box["radii"]
        ax = box["axes"]
        box_buf.extend(
            _BOX_STRUCT.pack(
                float(c[0]), float(c[1]), float(c[2]),
                float(r[0]), float(r[1]), float(r[2]),
                float(ax[0][0]), float(ax[0][1]), float(ax[0][2]),
                float(ax[1][0]), float(ax[1][1]), float(ax[1][2]),
                float(ax[2][0]), float(ax[2][1]), float(ax[2][2]),
            )
        )
    return header + verts.tobytes() + tris.tobytes() + surfs.tobytes() + bytes(box_buf)


def unpack_eodcol(data: bytes) -> EodCollisionMesh32:
    """Deserialize an EODCOL32 binary blob back into EodCollisionMesh32."""
    if len(data) < _HEADER_STRUCT.size:
        raise ValueError("EODCOL32 buffer too short for header")
    magic, ver, v_cnt, t_cnt, b_cnt, minx, miny, minz, maxx, maxy, maxz = _HEADER_STRUCT.unpack_from(data, 0)
    if magic != EODCOL_MAGIC:
        raise ValueError(f"Invalid EODCOL magic: {magic!r}")
    if ver != EODCOL_VERSION:
        raise ValueError(f"Unsupported EODCOL version: {ver}")
    off = _HEADER_STRUCT.size
    v_bytes = v_cnt * 12
    t_bytes = t_cnt * 12
    s_bytes = t_cnt
    b_bytes = b_cnt * _BOX_STRUCT.size
    if len(data) != off + v_bytes + t_bytes + s_bytes + b_bytes:
        raise ValueError("EODCOL32 payload size mismatch")

    vertices = np.frombuffer(data, dtype="<f4", count=v_cnt * 3, offset=off).reshape(v_cnt, 3).astype(np.float32, copy=True)
    off += v_bytes
    triangles = np.frombuffer(data, dtype="<u4", count=t_cnt * 3, offset=off).reshape(t_cnt, 3).astype(np.uint32, copy=True)
    off += t_bytes
    surface_ids = np.frombuffer(data, dtype="u1", count=t_cnt, offset=off).copy()
    off += s_bytes

    wall_boxes: list[dict] = []
    for _ in range(b_cnt):
        vals = _BOX_STRUCT.unpack_from(data, off)
        off += _BOX_STRUCT.size
        wall_boxes.append({
            "centre": [vals[0], vals[1], vals[2]],
            "radii": [vals[3], vals[4], vals[5]],
            "axes": [
                [vals[6], vals[7], vals[8]],
                [vals[9], vals[10], vals[11]],
                [vals[12], vals[13], vals[14]],
            ],
        })

    return EodCollisionMesh32(
        vertices=vertices,
        triangles=triangles,
        surface_ids=surface_ids,
        wall_boxes=wall_boxes,
        aabb_min=np.array([minx, miny, minz], dtype=np.float32),
        aabb_max=np.array([maxx, maxy, maxz], dtype=np.float32),
    )


def query_ground_y_32bit(
    mesh: EodCollisionMesh32,
    x: float,
    z: float,
    ref_y: float = 1e6,
) -> float | None:
    """Vectorized vertical raycast returning highest triangle Y <= ref_y + 2.0 at (x, z)."""
    if len(mesh.triangles) == 0:
        return None
    eps = 1e-4
    if (
        x < float(mesh.aabb_min[0]) - eps
        or x > float(mesh.aabb_max[0]) + eps
        or z < float(mesh.aabb_min[2]) - eps
        or z > float(mesh.aabb_max[2]) + eps
    ):
        return None

    v0 = mesh.vertices[mesh.triangles[:, 0]]
    v1 = mesh.vertices[mesh.triangles[:, 1]]
    v2 = mesh.vertices[mesh.triangles[:, 2]]
    min_x = np.minimum(np.minimum(v0[:, 0], v1[:, 0]), v2[:, 0])
    max_x = np.maximum(np.maximum(v0[:, 0], v1[:, 0]), v2[:, 0])
    min_z = np.minimum(np.minimum(v0[:, 2], v1[:, 2]), v2[:, 2])
    max_z = np.maximum(np.maximum(v0[:, 2], v1[:, 2]), v2[:, 2])
    cand = (x >= min_x - eps) & (x <= max_x + eps) & (z >= min_z - eps) & (z <= max_z + eps)
    if not np.any(cand):
        return None

    c0, c1, c2 = v0[cand].astype(np.float64), v1[cand].astype(np.float64), v2[cand].astype(np.float64)
    dx1, dz1 = c1[:, 0] - c0[:, 0], c1[:, 2] - c0[:, 2]
    dx2, dz2 = c2[:, 0] - c0[:, 0], c2[:, 2] - c0[:, 2]
    det = dx1 * dz2 - dz1 * dx2
    non_deg = np.abs(det) > 1e-9
    if not np.any(non_deg):
        return None

    c0, c1, c2 = c0[non_deg], c1[non_deg], c2[non_deg]
    dx1, dz1, dx2, dz2, det = dx1[non_deg], dz1[non_deg], dx2[non_deg], dz2[non_deg], det[non_deg]
    px, pz = float(x) - c0[:, 0], float(z) - c0[:, 2]
    u = (px * dz2 - pz * dx2) / det
    v = (dx1 * pz - dz1 * px) / det
    bary_eps = 1e-5
    hit = (u >= -bary_eps) & (v >= -bary_eps) & ((u + v) <= 1.0 + bary_eps)
    if not np.any(hit):
        return None

    ys = (1.0 - u[hit] - v[hit]) * c0[hit, 1] + u[hit] * c1[hit, 1] + v[hit] * c2[hit, 1]
    below = ys[ys <= float(ref_y) + 2.0]
    if len(below) == 0:
        return None
    return float(np.max(below))


def synthesize_road_graph(
    road: BuiltRoad,
    line_name: str = "custom.ild",
) -> tuple[RoadGraph, list[float]]:
    """Build an AI RoadGraph and per-station curvature + banking target speeds (m/s)."""
    n = len(road.stations_xyz)
    nodes: dict[int, RoadNode] = {}
    edges: list[RoadEdge] = []
    closed = bool(road.plan.closed)
    two_way = not closed

    for i in range(n):
        x, y, z = (float(v) for v in road.stations_xyz[i])
        w = float(road.widths[i])
        lanes = max(1, int(round(w / 4.0)))
        nodes[i] = RoadNode(
            node_id=i,
            x=x,
            y=y,
            z=z,
            line_name=line_name,
            is_side=False,
            retail_index=None,
            lanes=lanes,
            width=w,
            two_way=two_way,
        )

    for i in range(n - 1):
        w = 0.5 * (float(road.widths[i]) + float(road.widths[i + 1]))
        edges.append(RoadEdge(a=i, b=i + 1, lanes=max(1, int(round(w / 4.0))), width=w, two_way=two_way))
    if closed and n >= 3:
        w = 0.5 * (float(road.widths[n - 1]) + float(road.widths[0]))
        edges.append(RoadEdge(a=n - 1, b=0, lanes=max(1, int(round(w / 4.0))), width=w, two_way=two_way))

    speeds: list[float] = []
    if n > 0:
        t_xz = np.asarray(road.tangents[:, [0, 2]], dtype=np.float64)
        t_len = np.linalg.norm(t_xz, axis=1, keepdims=True)
        t_xz = np.where(t_len > 1e-8, t_xz / np.maximum(t_len, 1e-8), np.array([0.0, 1.0]))
        arc_s = np.asarray(road.arc_s, dtype=np.float64)
        total_s = float(arc_s[-1]) + (
            float(np.linalg.norm(road.stations_xyz[0] - road.stations_xyz[-1])) if closed and n >= 2 else 0.0
        )
        mu, g = 1.15, 9.81
        for i in range(n):
            if n < 2:
                kappa = 0.0
            elif closed:
                i_prev = (i - 1) % n
                i_next = (i + 1) % n
                ds = (arc_s[i_next] - arc_s[i_prev]) % max(total_s, 1e-3)
                dtheta = math.acos(max(-1.0, min(1.0, float(np.dot(t_xz[i_prev], t_xz[i_next])))))
                kappa = dtheta / max(ds, 1e-3)
            else:
                i_prev = max(0, i - 1)
                i_next = min(n - 1, i + 1)
                ds = float(arc_s[i_next] - arc_s[i_prev])
                dtheta = math.acos(max(-1.0, min(1.0, float(np.dot(t_xz[i_prev], t_xz[i_next])))))
                kappa = dtheta / max(ds, 1e-3)
            bank_rad = math.radians(min(abs(float(road.bank_deg[i])), 45.0))
            tan_b = math.tan(bank_rad)
            a_lat = g * (mu + tan_b) / max(1.0 - 0.35 * tan_b, 0.3)
            v_ms = 62.0 if kappa < 1e-4 else math.sqrt(a_lat / kappa)
            speeds.append(max(12.0, min(62.0, v_ms)))

    graph = RoadGraph(
        line_name=line_name,
        available_lines=[line_name],
        nodes=nodes,
        edges=edges,
        next_id=n,
        dirty=True,
    )
    return graph, speeds


def rasterize_minimap_rgba(
    built_roads: list[BuiltRoad],
    road_graphs: list[RoadGraph] | None = None,
    size: int = 256,
) -> tuple[bytes, tuple[float, float, float, float]]:
    """Render a crisp top-down RGBA minimap and return (rgba_bytes, (min_x, max_x, min_z, max_z))."""
    xs: list[float] = []
    zs: list[float] = []
    max_half_w = 8.0
    for road in built_roads:
        if len(road.stations_xyz) > 0:
            xs.extend(road.stations_xyz[:, 0].tolist())
            zs.extend(road.stations_xyz[:, 2].tolist())
            max_half_w = max(max_half_w, float(np.max(road.widths)) * 0.5)
    for graph in road_graphs or ():
        for node in graph.nodes.values():
            xs.append(float(node.x))
            zs.append(float(node.z))

    if xs and zs:
        raw_min_x, raw_max_x = min(xs), max(xs)
        raw_min_z, raw_max_z = min(zs), max(zs)
        span = max(raw_max_x - raw_min_x, raw_max_z - raw_min_z, 20.0)
        pad = max(span * 0.12, max_half_w * 1.5)
        cx, cz = 0.5 * (raw_min_x + raw_max_x), 0.5 * (raw_min_z + raw_max_z)
        half = 0.5 * span + pad
        min_x, max_x = cx - half, cx + half
        min_z, max_z = cz - half, cz + half
    else:
        min_x, max_x, min_z, max_z = -100.0, 100.0, -100.0, 100.0

    world_span = max(max_x - min_x, 1e-3)
    px_per_m = (size - 1) / world_span

    def to_px(x: float, z: float) -> tuple[float, float]:
        return ((x - min_x) * px_per_m, (max_z - z) * px_per_m)

    img = Image.new("RGBA", (size, size), (12, 16, 22, 185))
    draw = ImageDraw.Draw(img, "RGBA")

    for graph in road_graphs or ():
        for edge in graph.edges:
            na, nb = graph.nodes.get(edge.a), graph.nodes.get(edge.b)
            if na is not None and nb is not None:
                stroke = max(2, int(round(edge.width * px_per_m)))
                draw.line([to_px(na.x, na.z), to_px(nb.x, nb.z)], fill=(86, 94, 108, 220), width=stroke)

    for road in built_roads:
        if len(road.stations_xyz) < 2:
            continue
        pts = [to_px(float(p[0]), float(p[2])) for p in road.stations_xyz]
        if road.plan.closed:
            pts.append(pts[0])
        road_w = max(3, int(round(float(np.mean(road.widths)) * px_per_m)))
        draw.line(pts, fill=(28, 32, 40, 255), width=road_w + 2, joint="curve")
        draw.line(pts, fill=(196, 202, 212, 255), width=road_w, joint="curve")
        # Dashed centerline
        for i in range(0, len(pts) - 1, 2):
            draw.line([pts[i], pts[i + 1]], fill=(245, 210, 72, 240), width=max(1, road_w // 5))
        sx, sy = pts[0]
        r = max(3.0, size / 48.0)
        draw.ellipse([sx - r, sy - r, sx + r, sy + r], fill=(44, 224, 96, 255), outline=(255, 255, 255, 255), width=1)

    return img.tobytes(), (min_x, max_x, min_z, max_z)
