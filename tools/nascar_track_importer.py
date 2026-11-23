#!/usr/bin/env python3
"""NASCAR: Dirt to Daytona (0TSR v2) -> EOD (0TSR v6) Track (.TRK) Binary Transcoder.

Converts all 32 NASCAR: Dirt to Daytona tracks from an ISO9660 disc image or extracted
disc directory into valid retail-compatible EOD 0TSR v6 .TRK archives, 32-bit .eodcol
collision meshes, top-down minimap previews, and validated Schema v2 .eodmod bundles.
"""

from __future__ import annotations

from contextlib import contextmanager
from dataclasses import dataclass, field
import json
import math
import os
from pathlib import Path
import struct
import sys
import tempfile
import time
from typing import Any, Iterator

import numpy as np
from PIL import Image, ImageDraw

_TOOLS_DIR = Path(__file__).resolve().parent
_EDITOR_DIR = _TOOLS_DIR / "world_editor"
for _p in (str(_TOOLS_DIR), str(_EDITOR_DIR)):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import import_nascar_cars as inc  # noqa: E402
import iso9660_reader as iso_mod  # noqa: E402
import mod_package_v2 as mp2  # noqa: E402
import rst0  # noqa: E402
import valt  # noqa: E402
import vif_mesh  # noqa: E402
import world_collision as wc  # noqa: E402
import world_graf as wg  # noqa: E402
from formats import ilin as ilin_fmt  # noqa: E402

try:
    import road_collision as rc  # noqa: E402
except Exception:
    rc = None  # type: ignore[assignment]


def _get_profiler() -> Any:
    try:
        import perf_profiler

        return perf_profiler.get_profiler()
    except Exception:
        return None


@contextmanager
def span(name: str, category: str = "nascar_track", timings: dict[str, float] | None = None) -> Iterator[None]:
    """Record a Tracy/Perfetto span via tools/world_editor/perf_profiler.py."""
    prof = _get_profiler()
    t0 = time.perf_counter_ns()
    cm = None
    if prof is not None and hasattr(prof, "span"):
        cm = prof.span(name, category)
    elif prof is not None and hasattr(prof, "zone"):
        cm = prof.zone(name, category)
    if cm is not None:
        with cm:
            try:
                yield
            finally:
                if timings is not None:
                    timings[name] = timings.get(name, 0.0) + (time.perf_counter_ns() - t0) / 1_000_000.0
    else:
        try:
            yield
        finally:
            if timings is not None:
                timings[name] = timings.get(name, 0.0) + (time.perf_counter_ns() - t0) / 1_000_000.0


@dataclass(frozen=True)
class NascarTrackInfo:
    track_key: str        # e.g. "BRISTOL", "DAYTONA", "TALLAD_1"
    trk_filename: str     # e.g. "BRISTOL.TRK"
    iso_path: str         # e.g. "DATA/TRACKS2/BRISTOL.TRK"
    display_name: str     # e.g. "Bristol"
    longname: str         # e.g. "Motor Speedway"
    surface_class: str    # ShortDirt | ShortAsphalt | RoadCourse | Speedway | Superspeedway
    archive_size: int
    has_night_sky: bool


@dataclass
class ImportTrackResult:
    track_key: str
    display_name: str
    lighting_mode: str
    trk_path: Path
    eodcol_path: Path
    minimap_path: Path
    metadata_path: Path
    eodmod_path: Path | None
    graf_models: int
    graf_parts: int
    scol_tris: int
    sobl_records: int
    ilin_nodes: int
    tex_count: int
    eodcol_aabb_min: tuple[float, float, float]
    eodcol_aabb_max: tuple[float, float, float]
    timings_ms: dict[str, float] = field(default_factory=dict)
    total_ms: float = 0.0


_SHORT_DIRT_KEYS = frozenset({
    "ANOKA", "BAXTER", "BEMIDJI", "CEDAR", "DENNISON", "DUNDAS", "EAGAN",
    "HASTINGS", "KENYON", "MANKATO", "SHAKOPEE", "WELCH",
})
_SHORT_ASPHALT_KEYS = frozenset({"BRISTOL", "MARTIN_1", "RICHMOND"})
_ROAD_COURSE_KEYS = frozenset({"SEARS__1", "WATKIN_1"})
_SUPERSPEEDWAY_KEYS = frozenset({"DAYTONA", "TALLAD_1"})


def _classify_surface(key: str) -> str:
    ku = key.upper()
    if ku in _SHORT_DIRT_KEYS:
        return "ShortDirt"
    if ku in _SHORT_ASPHALT_KEYS:
        return "ShortAsphalt"
    if ku in _ROAD_COURSE_KEYS:
        return "RoadCourse"
    if ku in _SUPERSPEEDWAY_KEYS:
        return "Superspeedway"
    return "Speedway"


def _disc_stem_from_val_key(val_key: str) -> str:
    ku = val_key.upper()
    return ku if len(ku) <= 8 else ku[:6] + "_1"


def _read_0tsr_v2_entries(data: bytes) -> list[dict[str, Any]]:
    """Fast in-memory 0TSR v2 archive unpacker."""
    if len(data) < 24 or data[:4] != b"0TSR" or struct.unpack_from("<I", data, 4)[0] != 2:
        raise ValueError("Not a valid 0TSR v2 archive")
    _total, build_time, count = struct.unpack_from("<III", data, 12)
    dir_off = 0x18
    dir_end = dir_off + count * 0x38
    if dir_end > len(data):
        raise ValueError("Truncated 0TSR v2 directory")
    cur = data.find(b"!IGM", dir_end)
    if cur == -1:
        cur = ((dir_end - 0x14 + 0x7F) & ~0x7F) + 0x14
    entries: list[dict[str, Any]] = []
    for i in range(count):
        eb = data[dir_off + i * 0x38 : dir_off + (i + 1) * 0x38]
        name = eb[:36].split(b"\0")[0].decode("latin1")
        fourcc = eb[0x24:0x28][::-1].decode("latin1", errors="replace")
        ver, size = struct.unpack_from("<II", eb, 0x28)
        has_igm = data[cur : cur + 4] == b"!IGM"
        ps = cur + (4 if has_igm else 0)
        pe = ps + size
        if pe > len(data):
            raise ValueError(f"Truncated 0TSR v2 member {name!r}")
        entries.append({"name": name, "fourcc": fourcc, "version": ver, "data": data[ps:pe]})
        nxt = data.find(b"!IGM", pe)
        cur = nxt if nxt != -1 else ps + ((size + 0x7F) & ~0x7F)
    return entries


def _read_0tsr_v6_entries(data: bytes) -> list[dict[str, Any]]:
    """Fast in-memory 0TSR v6 archive reader."""
    if len(data) < 0x28 or data[:4] != b"0TSR" or struct.unpack_from("<I", data, 8)[0] != 6:
        raise ValueError("Not a valid 0TSR v6 archive")
    dir_off, count = struct.unpack_from("<II", data, 20)
    ext_blk = struct.unpack_from("<I", data, 28)[0]
    entries: list[dict[str, Any]] = []
    for i in range(count):
        eb = data[dir_off + i * 0x44 : dir_off + (i + 1) * 0x44]
        name = eb[:32].split(b"\0")[0].decode("latin1")
        fourcc = eb[0x20:0x24][::-1].decode("latin1", errors="replace")
        ver, size, off, crc = struct.unpack_from("<IIII", eb, 0x24)
        start = dir_off + off
        entries.append({
            "name": name, "fourcc": fourcc, "version": ver, "data": data[start : start + size],
            "is_extractable": off >= ext_blk, "crc": crc,
        })
    return entries


def _has_night_sky_from_dir_header(header_bytes: bytes) -> bool:
    """Check if a 0TSR v2 directory header declares nsky*.tex without reading the whole .TRK."""
    if len(header_bytes) < 24 or header_bytes[:4] != b"0TSR":
        return False
    count = struct.unpack_from("<I", header_bytes, 20)[0]
    for i in range(count):
        off = 0x18 + i * 0x38
        if off + 36 > len(header_bytes):
            break
        raw_name = header_bytes[off : off + 36].split(b"\0", 1)[0].lower()
        if raw_name.startswith(b"nsky") and raw_name.endswith(b".tex"):
            return True
    return False


def _open_d2d_source(iso_path: Path | str):
    """Return a lightweight handle supporting list_tracks() and read_rel(rel_path)."""
    p = Path(iso_path)
    if p.is_dir():
        return ("dir", p, None, None)
    fh = open(p, "rb")
    vol = iso_mod._Iso9660(fh)
    return ("iso", p, fh, vol)


def scan_nascar_tracks(
    iso_path: Path | str,
    *,
    timings: dict[str, float] | None = None,
) -> list[NascarTrackInfo]:
    """Scan all 32 NASCAR: Dirt to Daytona tracks from an ISO or extracted disc in < 250 ms."""
    with span("scan_iso", timings=timings):
        kind, root_p, fh, vol = _open_d2d_source(iso_path)
        try:
            # 1. Read COMMON.RES -> tracks.val metadata
            meta_by_stem: dict[str, tuple[str, str]] = {}
            common_raw: bytes | None = None
            trk_records: list[tuple[str, str, int, Any]] = []  # (stem, rel, size, handle)

            if kind == "iso":
                d_ch = vol.children(vol.children(vol.root)["DATA"])
                res_dir = vol.children(d_ch["RES"])
                crec = res_dir.get("COMMON.RES")
                if crec is not None:
                    common_raw = vol.read(crec.offset, crec.size)
                for folder in ("TRACKS", "TRACKS2"):
                    if folder not in d_ch:
                        continue
                    for fname, rec in vol.children(d_ch[folder]).items():
                        if not rec.is_dir and fname.upper().endswith(".TRK"):
                            trk_records.append((fname[:-4].upper(), f"DATA/{folder}/{fname}", rec.size, rec))
            else:
                cp = root_p / "DATA" / "RES" / "COMMON.RES"
                if cp.is_file():
                    common_raw = cp.read_bytes()
                for folder in ("TRACKS", "TRACKS2"):
                    fp = root_p / "DATA" / folder
                    if not fp.is_dir():
                        continue
                    for fitem in fp.iterdir():
                        if fitem.is_file() and fitem.suffix.upper() == ".TRK":
                            trk_records.append((fitem.stem.upper(), f"DATA/{folder}/{fitem.name}", fitem.stat().st_size, fitem))

            if common_raw:
                for entry in _read_0tsr_v2_entries(common_raw):
                    if entry["name"].lower() == "tracks.val":
                        tv = valt.parse_binary_valt(entry["data"])
                        for sec in tv.get("sections", []):
                            pmap = dict(sec.get("pairs", []))
                            stem = _disc_stem_from_val_key(sec["name"])
                            meta_by_stem[stem] = (
                                str(pmap.get("name") or sec["name"]).strip(),
                                str(pmap.get("longname") or "").strip(),
                            )
                        break

            out: list[NascarTrackInfo] = []
            for stem, rel, size, handle in sorted(trk_records, key=lambda x: x[0]):
                if kind == "iso":
                    hdr24 = vol.read(handle.offset, min(24, handle.size))
                    cnt = struct.unpack_from("<I", hdr24, 20)[0] if len(hdr24) >= 24 else 0
                    dir_bytes = vol.read(handle.offset, min(0x18 + cnt * 0x38, handle.size))
                else:
                    with open(handle, "rb") as tf:
                        hdr24 = tf.read(24)
                        cnt = struct.unpack_from("<I", hdr24, 20)[0] if len(hdr24) >= 24 else 0
                        tf.seek(0)
                        dir_bytes = tf.read(0x18 + cnt * 0x38)
                disp, lng = meta_by_stem.get(stem, (stem.title(), ""))
                out.append(NascarTrackInfo(
                    track_key=stem,
                    trk_filename=f"{stem}.TRK",
                    iso_path=rel,
                    display_name=disp,
                    longname=lng,
                    surface_class=_classify_surface(stem),
                    archive_size=size,
                    has_night_sky=_has_night_sky_from_dir_header(dir_bytes),
                ))
            return out
        finally:
            if fh is not None:
                fh.close()


# ---------------------------------------------------------------------------
# Transcoder 1: TEX v9 -> TEX v10 (with Day/Night skybox remapping)
# ---------------------------------------------------------------------------
def transcode_tex_v9_to_v10(data: bytes) -> bytes:
    """Upgrade a NASCAR D2D TEX v9 payload to EOD TEX v10."""
    return inc.convert_tex_v9_to_v10(data)


# ---------------------------------------------------------------------------
# Transcoder 2: SCOL v1 -> SCOL v4 + 32-bit .eodcol
# ---------------------------------------------------------------------------
def _surface_id_d2d_to_eod(raw_surf: int, surface_class: str) -> int:
    """Map D2D SCOL surface id to EOD terrain surface id (0=dirt, 2=asphalt, 1=grass)."""
    if surface_class == "ShortDirt":
        return 0 if raw_surf in (0, 6) else 1
    if raw_surf == 0:
        return 2  # asphalt racing surface
    if raw_surf in (5, 6):
        return 0  # dirt / apron
    return 1  # grass / infield


def transcode_scol_v1_to_v4_and_eodcol(
    scol_v1: bytes,
    *,
    sobl_raw: bytes | None = None,
    surface_class: str = "Speedway",
    timings: dict[str, float] | None = None,
) -> tuple[bytes, Any, bytes, int]:
    """Convert D2D SCOL v1 into EOD SCOL v4 AND EodCollisionMesh32 (.eodcol bytes)."""
    with span("transcode_scol", timings=timings):
        if len(scol_v1) < 24:
            raise ValueError("SCOL v1 payload too short")
        n_tris, n_edges, _n3, off1, off2, _off3 = struct.unpack_from("<6I", scol_v1, 0)
        if off1 + n_tris * 36 > len(scol_v1) or off2 + n_edges * 12 > len(scol_v1):
            raise ValueError("SCOL v1 tables exceed payload bounds")

        # Vectorized decode of D2D SCOL v1 edge lines (a*x + b*z + c = 0) and triangles
        edge_arr = np.frombuffer(scol_v1, dtype="<f4", count=n_edges * 3, offset=off2).reshape(n_edges, 3).astype(np.float64)
        tri_dt = np.dtype([
            ("nbr", "<i2", 3), ("pad0", "<i2"),
            ("plane", "<f4", 4),
            ("eidx", "<u2", 3), ("surf", "<u2"), ("n3_idx", "<u2"), ("zone", "<u2"),
        ])
        tris_raw = np.frombuffer(scol_v1, dtype=tri_dt, count=n_tris, offset=off1)

        e_raw = tris_raw["eidx"]                           # (T, 3) uint16
        e_id = e_raw & 0x7FFF
        e_sgn = np.where((e_raw & 0x8000) != 0, -1.0, 1.0) # (T, 3)
        el = edge_arr[e_id] * e_sgn[:, :, None]            # (T, 3, 3): (a, b, c)

        # Intersect consecutive edge pairs (0,1), (1,2), (2,0) to obtain 3D vertices
        a1, b1, c1 = el[:, 0, 0], el[:, 0, 1], el[:, 0, 2]
        a2, b2, c2 = el[:, 1, 0], el[:, 1, 1], el[:, 1, 2]
        a3, b3, c3 = el[:, 2, 0], el[:, 2, 1], el[:, 2, 2]

        def _intersect(xa, xb, xc, ya, yb, yc):
            det = xa * yb - ya * xb
            safe = np.where(np.abs(det) > 1e-12, det, 1.0)
            px = np.where(np.abs(det) > 1e-12, (xb * yc - yb * xc) / safe, 0.0)
            pz = np.where(np.abs(det) > 1e-12, (xc * ya - yc * xa) / safe, 0.0)
            return px, pz, np.abs(det) > 1e-12

        x0, z0, ok0 = _intersect(a1, b1, c1, a2, b2, c2)
        x1, z1, ok1 = _intersect(a2, b2, c2, a3, b3, c3)
        x2, z2, ok2 = _intersect(a3, b3, c3, a1, b1, c1)
        valid_tri = ok0 & ok1 & ok2

        planes = tris_raw["plane"].astype(np.float64)      # (T, 4): a, b, c, d
        pa, pb, pc, pd = planes[:, 0], planes[:, 1], planes[:, 2], planes[:, 3]
        safe_pb = np.where(np.abs(pb) > 1e-6, pb, 1.0)
        y0 = np.where(np.abs(pb) > 1e-6, -(pa * x0 + pc * z0 + pd) / safe_pb, 0.0)
        y1 = np.where(np.abs(pb) > 1e-6, -(pa * x1 + pc * z1 + pd) / safe_pb, 0.0)
        y2 = np.where(np.abs(pb) > 1e-6, -(pa * x2 + pc * z2 + pd) / safe_pb, 0.0)

        # Orient each triangle's 3 edge normals inward using triangle centroid
        cx = (x0 + x1 + x2) / 3.0
        cz = (z0 + z1 + z2) / 3.0
        enorm = np.hypot(el[:, :, 0], el[:, :, 1])
        enorm_safe = np.where(enorm > 1e-12, enorm, 1.0)
        nx_f = el[:, :, 0] / enorm_safe
        nz_f = el[:, :, 1] / enorm_safe
        ed_f = el[:, :, 2] / enorm_safe
        signed_c = nx_f * cx[:, None] + nz_f * cz[:, None] + ed_f
        flip = np.where(signed_c < 0.0, -1.0, 1.0)
        nx_i = np.clip(np.rint(nx_f * flip * 32767.0), -32767, 32767).astype("<i2")
        nz_i = np.clip(np.rint(nz_f * flip * 32767.0), -32767, 32767).astype("<i2")
        ed_out = (ed_f * flip).astype("<f4")

        # Pack EOD SCOL v4 binary (header 12B + T*32B Table1 + T*20B Table2)
        t1_dt = np.dtype([
            ("e0_nx", "<i2"), ("e0_nz", "<i2"), ("e0_d", "<f4"),
            ("e1_nx", "<i2"), ("e1_nz", "<i2"), ("e1_d", "<f4"),
            ("e2_nx", "<i2"), ("e2_nz", "<i2"), ("e2_d", "<f4"),
            ("nbr", "<i2", 3), ("opt", "<u2"),
        ])
        t1 = np.zeros(n_tris, dtype=t1_dt)
        t1["e0_nx"], t1["e0_nz"], t1["e0_d"] = nx_i[:, 0], nz_i[:, 0], ed_out[:, 0]
        t1["e1_nx"], t1["e1_nz"], t1["e1_d"] = nx_i[:, 1], nz_i[:, 1], ed_out[:, 1]
        t1["e2_nx"], t1["e2_nz"], t1["e2_d"] = nx_i[:, 2], nz_i[:, 2], ed_out[:, 2]
        t1["nbr"] = tris_raw["nbr"]
        t1["opt"] = 0

        t2_dt = np.dtype([("plane", "<f4", 4), ("surf", "<i2"), ("b0", "u1"), ("b1", "u1")])
        t2 = np.zeros(n_tris, dtype=t2_dt)
        t2["plane"] = tris_raw["plane"]
        surf_raw = tris_raw["surf"]
        eod_surfs = np.array([_surface_id_d2d_to_eod(int(s), surface_class) for s in surf_raw], dtype="<i2")
        t2["surf"] = eod_surfs

        t1_off = 12
        t2_off = t1_off + n_tris * 32
        scol_v4 = struct.pack("<3I", n_tris, t1_off, t2_off) + t1.tobytes() + t2.tobytes()

        # Build 32-bit EodCollisionMesh32 (.eodcol) with full extents (>1 km superspeedways safe!)
        keep = valid_tri & (np.abs(x0) < 10000) & (np.abs(z0) < 10000) & (np.abs(x1) < 10000) & (np.abs(z1) < 10000)
        idx_k = np.flatnonzero(keep)
        n_k = len(idx_k)
        verts32 = np.empty((n_k * 3, 3), dtype=np.float32)
        verts32[0::3, 0], verts32[0::3, 1], verts32[0::3, 2] = x0[idx_k], y0[idx_k], z0[idx_k]
        verts32[1::3, 0], verts32[1::3, 1], verts32[1::3, 2] = x1[idx_k], y1[idx_k], z1[idx_k]
        verts32[2::3, 0], verts32[2::3, 1], verts32[2::3, 2] = x2[idx_k], y2[idx_k], z2[idx_k]
        tris32 = np.arange(n_k * 3, dtype=np.uint32).reshape(n_k, 3)
        sids32 = np.where(eod_surfs[idx_k] == 2, 1, 0).astype(np.uint8)

        wall_boxes: list[dict[str, Any]] = []
        if sobl_raw:
            try:
                sobl_p = wc.parse_sobl(sobl_raw)
                for rec in sobl_p.get("records", []):
                    if rec.get("kind") == "box":
                        wall_boxes.append({
                            "centre": list(rec["centre"]),
                            "radii": list(rec["half_extents"]),
                            "axes": [list(row) for row in rec["rows"]],
                        })
            except Exception:
                pass

        aabb_min = verts32.min(axis=0) if len(verts32) else np.zeros(3, dtype=np.float32)
        aabb_max = verts32.max(axis=0) if len(verts32) else np.zeros(3, dtype=np.float32)
        if rc is not None:
            mesh32 = rc.EodCollisionMesh32(
                vertices=verts32,
                triangles=tris32,
                surface_ids=sids32,
                wall_boxes=wall_boxes,
                aabb_min=aabb_min.astype(np.float32),
                aabb_max=aabb_max.astype(np.float32),
            )
            eodcol_bytes = rc.pack_eodcol(mesh32)
        else:
            mesh32 = None
            eodcol_bytes = b""
        return scol_v4, mesh32, eodcol_bytes, n_tris


# ---------------------------------------------------------------------------
# Transcoder 3: ILIN v19 -> ILIN v27 (track.ild, oval.ild, bw.ild, bw_oval.ild)
# ---------------------------------------------------------------------------
def _pack_ilin_v27(pts_xz: list[tuple[float, float]], speeds: list[float], widths: list[float]) -> bytes:
    """Serialize a closed loop of (x, z) waypoints into a valid EOD ILIN v27 payload."""
    n = len(pts_xz)
    if n < 3:
        raise ValueError("ILIN loop requires at least 3 waypoints")
    fixup_count = 1 + n
    nodes_off = 48 + fixup_count * 4
    fixups = [32] + [nodes_off + i * 60 + 20 for i in range(n)]

    seg_lens: list[float] = []
    tangents: list[tuple[float, float]] = []
    cum_dists: list[float] = []
    total_len = 0.0
    for i in range(n):
        x0, z0 = pts_xz[i]
        x1, z1 = pts_xz[(i + 1) % n]
        dx, dz = x1 - x0, z1 - z0
        d = math.hypot(dx, dz)
        if d < 1e-5:
            tx, tz, d = 0.0, 1.0, 1e-3
        else:
            tx, tz = dx / d, dz / d
        cum_dists.append(total_len)
        seg_lens.append(d)
        tangents.append((tx, tz))
        total_len += d

    out = bytearray()
    out += struct.pack(
        "<3I4f2H4I",
        0, 48, 60,
        float(total_len), float(total_len), float(total_len), 0.05,
        0, n,
        nodes_off, 0, 0, fixup_count,
    )
    out += struct.pack(f"<{fixup_count}I", *fixups)
    for i in range(n):
        x, z = pts_xz[i]
        tx, tz = tangents[i]
        nxt_off = nodes_off + ((i + 1) % n) * 60
        spd = max(12.0, min(95.0, float(speeds[i])))
        w = max(8.0, min(40.0, float(widths[i])))
        out += struct.pack(
            "<5fI9f",
            float(x), float(z), float(tx), float(tz), 0.0,
            nxt_off,
            float(seg_lens[i]), float(cum_dists[i]), spd, 0.005, w,
            0.0, 0.0, 0.0, 0.0,
        )
    return bytes(out)


def transcode_ilin_v19_to_v27(
    ilin_v19: bytes,
    *,
    timings: dict[str, float] | None = None,
) -> tuple[dict[str, bytes], list[tuple[float, float]]]:
    """Decode D2D ILIN v19 main loop and emit EOD ILIN v27 track/oval/bw/bw_oval .ild payloads."""
    with span("transcode_ilin", timings=timings):
        if len(ilin_v19) < 1856:
            raise ValueError("ILIN v19 payload shorter than 1856-byte header")
        hdr_sz, stride = struct.unpack_from("<II", ilin_v19, 4)
        if stride != 76:
            raise ValueError(f"Unexpected ILIN v19 stride {stride} (expected 76)")
        start_node, _sec_node, n_main, _n_fix = struct.unpack_from("<4I", ilin_v19, 1840)

        pts: list[tuple[float, float]] = []
        speeds: list[float] = []
        widths: list[float] = []
        seen: set[int] = set()
        cur = start_node
        max_steps = max(int(n_main) * 2, (len(ilin_v19) - start_node) // stride + 2)
        while start_node <= cur <= len(ilin_v19) - stride and cur not in seen and len(pts) < max_steps:
            seen.add(cur)
            nxt = struct.unpack_from("<I", ilin_v19, cur)[0]
            x, z = struct.unpack_from("<2f", ilin_v19, cur + 16)
            spd = struct.unpack_from("<f", ilin_v19, cur + 40)[0]
            wid = struct.unpack_from("<f", ilin_v19, cur + 48)[0]
            if math.isfinite(x) and math.isfinite(z):
                if not pts or math.hypot(x - pts[-1][0], z - pts[-1][1]) > 0.25:
                    pts.append((float(x), float(z)))
                    speeds.append(float(spd) if math.isfinite(spd) and 5.0 <= spd <= 120.0 else 38.0)
                    widths.append(float(wid) if math.isfinite(wid) and 6.0 <= wid <= 50.0 else 18.0)
            if nxt == start_node:
                break
            cur = nxt

        if len(pts) < 3:
            raise ValueError("Failed to extract valid main racing loop from ILIN v19")

        fwd_ild = _pack_ilin_v27(pts, speeds, widths)
        rev_pts = [pts[0]] + list(reversed(pts[1:]))
        rev_spd = [speeds[0]] + list(reversed(speeds[1:]))
        rev_wid = [widths[0]] + list(reversed(widths[1:]))
        bw_ild = _pack_ilin_v27(rev_pts, rev_spd, rev_wid)

        return (
            {
                "track.ild": fwd_ild,
                "oval.ild": fwd_ild,
                "bw.ild": bw_ild,
                "bw_oval.ild": bw_ild,
            },
            pts,
        )


# ---------------------------------------------------------------------------
# Transcoder 4: GRAF v13 -> GRAF v18 (4SPS VU1 transcode + EOD display list)
# ---------------------------------------------------------------------------
_WORLD_MSCAL_MAP = {
    0x1F: 0x4E,  # D2D world static mesh -> EOD world static mesh
    0x2D: 0x5C,  # D2D skinned/dynamic mesh -> EOD dynamic mesh
    0x26: 0x55,  # D2D type-1 mesh -> EOD type-1 mesh
}


def _transcode_world_vif_stream(stream: bytes) -> bytes:
    """Rewrite a D2D world 4SPS VIF stream for EOD VU1 (MSCAL 0x1F->0x4E, 0x2D->0x5C)."""
    out = bytearray()
    p = 0
    n_bytes = len(stream)
    while p + 4 <= n_bytes:
        code = struct.unpack_from("<I", stream, p)[0]
        cmd = (code >> 24) & 0x7F
        num = (code >> 16) & 0xFF
        imm = code & 0xFFFF
        if cmd >= 0x60:  # UNPACK
            vn, vl = (cmd >> 2) & 3, cmd & 3
            cnt = num or 256
            words = (cnt * 16 + 31) // 32 if vl == 3 else (cnt * (vn + 1) * (32 >> vl) + 31) // 32
            end_p = p + 4 + words * 4
            if end_p > n_bytes:
                # Preserve remaining bytes cleanly if trailing padding exists
                out += stream[p:]
                break
            out += stream[p:end_p]
            p = end_p
            continue
        if cmd == 0x20:  # STMASK (1 extra word)
            out += stream[p : p + 8]
            p += 8
            continue
        if cmd in (0x30, 0x31):  # STROW / STCOL (4 extra words)
            out += stream[p : p + 20]
            p += 20
            continue
        if cmd in (0x14, 0x15):  # MSCAL / MSCALF
            mapped = _WORLD_MSCAL_MAP.get(imm, 0x4E)
            out += struct.pack("<I", (code & 0xFFFF0000) | mapped)
            p += 4
            continue
        out += stream[p : p + 4]
        p += 4
    while len(out) % 16:
        out += b"\x00\x00\x00\x00"
    return bytes(out)


def _extract_graf_v13_models(data: bytes) -> list[tuple[int, dict[str, Any]]]:
    """Walk D2D GRAF v13 scene stream and extract all unique valid embedded mrModels."""
    if len(data) < 16:
        raise ValueError("GRAF v13 payload too short")
    root = struct.unpack_from("<I", data, 0)[0]
    if root < 4 or root >= len(data):
        raise ValueError(f"Invalid GRAF v13 root offset {root:#x}")

    rec_offs: list[int] = []
    seen_recs: set[int] = set()

    def _walk_range(start: int, end: int) -> None:
        p = start
        while p + 4 <= end:
            w = struct.unpack_from("<I", data, p)[0]
            op = w & 0x7F
            if op == 4:  # leaf: (tag, rec_off)
                if p + 8 <= end:
                    ro = struct.unpack_from("<I", data, p + 4)[0]
                    if 4 <= ro < root and ro not in seen_recs:
                        seen_recs.add(ro)
                        rec_offs.append(ro)
                p += 8
            elif op == 1:  # cond: (tag, skip_off)
                p += 8
            elif op == 2:  # sphere: (tag, cx, cy, cz, r, skip_off)
                p += 24
            elif op == 0 and (w & 0x100):  # LOD group: (tag, cx, cy, cz, tbl_off, end_off)
                if p + 24 > end:
                    break
                tbl_off, grp_end = struct.unpack_from("<II", data, p + 16)
                if tbl_off >= p + 24 and tbl_off < grp_end <= end:
                    # LOD table consists of (f32 dist2, u32 child_off) pairs up to first child_off
                    first_child = struct.unpack_from("<I", data, tbl_off + 4)[0]
                    if tbl_off < first_child <= grp_end:
                        child_offs: list[int] = []
                        q = tbl_off
                        while q + 8 <= first_child:
                            co = struct.unpack_from("<I", data, q + 4)[0]
                            if first_child <= co < grp_end:
                                child_offs.append(co)
                            q += 8
                        # Walk highest-detail child (LOD 0) cleanly, or all if single
                        if child_offs:
                            c0 = child_offs[0]
                            c1 = child_offs[1] if len(child_offs) > 1 else grp_end
                            _walk_range(c0, min(c1, grp_end))
                        p = grp_end
                        continue
                p += 24
            else:
                p += 4

    _walk_range(root, len(data))

    models: list[tuple[int, dict[str, Any]]] = []
    seen_models: set[int] = set()
    for ro in rec_offs:
        if ro + 0x20 > len(data):
            continue
        parsed = None
        for rel_slot in (0x10, 0x08, 0x14, 0x18, 0x1C):
            if ro + rel_slot + 4 > len(data):
                continue
            rel = struct.unpack_from("<I", data, ro + rel_slot)[0]
            moff = ro + rel
            if 0 < rel < 0x200 and moff not in seen_models and moff + 0x40 <= len(data):
                try:
                    cand = wg.parse_model(data, moff)
                    if cand["n_parts"] > 0:
                        parsed = cand
                        seen_models.add(moff)
                        break
                except Exception:
                    continue
        if parsed is not None:
            models.append((ro, parsed))
    return models


def transcode_graf_v13_to_v18(
    graf_v13: bytes,
    *,
    timings: dict[str, float] | None = None,
) -> tuple[bytes, int, int]:
    """Convert D2D GRAF v13 scene graph + 4SPS meshes into a valid EOD GRAF v18 payload."""
    with span("transcode_graf", timings=timings):
        extracted = _extract_graf_v13_models(graf_v13)
        if not extracted:
            raise ValueError("No valid mrModel records found in D2D GRAF v13")

        # Build transcoded model payloads first (each with 16-byte aligned 4SPS parts)
        built_models: list[tuple[bool, bytes]] = []  # (is_sky, model_bytes)
        total_parts = 0
        for _ro, mhdr in extracted:
            part_blobs: list[bytes] = []
            is_sky = False
            for pt in mhdr["parts"]:
                po = pt["offset"]
                psz = pt["size"]
                if po + 0x38 > len(graf_v13) or po + psz > len(graf_v13):
                    continue
                # Verify decodeable before keeping
                try:
                    bts = vif_mesh.decode_part(graf_v13, pt)
                    if not bts:
                        continue
                except Exception:
                    continue
                hdr = bytearray(graf_v13[po : po + 0x38])
                tex_name = hdr[8:24].split(b"\0", 1)[0].decode("latin1", errors="ignore")
                if tex_name.lower().startswith("nsky"):
                    tex_name = tex_name[1:]
                    hdr[8:24] = tex_name.encode("latin1")[:15].ljust(16, b"\0")
                if tex_name.lower().startswith("sky"):
                    is_sky = True
                stream_raw = graf_v13[po + 0x38 : po + psz]
                stream_eod = _transcode_world_vif_stream(stream_raw)
                new_sz = 0x38 + len(stream_eod)
                struct.pack_into("<I", hdr, 4, new_sz)
                struct.pack_into("<I", hdr, 0x1C, 2)  # EOD static part flag
                struct.pack_into("<I", hdr, 0x34, len(stream_eod) // 16)
                part_blobs.append(bytes(hdr) + stream_eod)

            if not part_blobs:
                continue
            total_parts += len(part_blobs)
            n_p = len(part_blobs)
            part_tbl_off = 0x40
            # Pad to 16 bytes after part offset table
            parts_start = (part_tbl_off + n_p * 4 + 15) & ~15
            p_offs: list[int] = []
            body = bytearray()
            cur = parts_start
            for pb in part_blobs:
                p_offs.append(cur)
                body += pb
                pad = (-len(pb)) & 15
                if pad:
                    body += b"\x00" * pad
                cur += len(pb) + pad

            bb = mhdr["bbox"]
            c = mhdr["centre"]
            m_hdr = struct.pack(
                "<6I10f",
                n_p, part_tbl_off, 0, 0, 1, 0,
                float(mhdr["radius"]),
                float(c[0]), float(c[1]), float(c[2]),
                float(bb["min"][0]), float(bb["max"][0]),
                float(bb["min"][1]), float(bb["max"][1]),
                float(bb["min"][2]), float(bb["max"][2]),
            )
            tbl_bytes = struct.pack(f"<{n_p}I", *p_offs)
            tbl_pad = b"\x00" * (parts_start - (part_tbl_off + len(tbl_bytes)))
            built_models.append((is_sky, m_hdr + tbl_bytes + tbl_pad + bytes(body)))

        if not built_models:
            raise ValueError("GRAF v13 transcode produced zero valid parts")

        # Assemble EOD GRAF v18:
        # [0x00..0x0C] header: (root=0x10, mlist_a=0, mlist_b=0) + 4B pad
        # [0x10..]     N x 12-byte NODE_LEAF chain (type=4, next, record_off)
        # [rec_base..] N x (record + embedded mrModel), 16-byte aligned
        n_m = len(built_models)
        root_off = 0x10
        nodes_end = root_off + n_m * 12
        rec_cursor = (nodes_end + 15) & ~15

        rec_offs: list[int] = []
        rec_buf = bytearray()
        sky_idx = 0
        for is_sky, mbytes in built_models:
            pad_pre = (16 - (rec_cursor & 15)) & 15
            if pad_pre:
                rec_buf += b"\x00" * pad_pre
                rec_cursor += pad_pre
            rec_offs.append(rec_cursor)
            if is_sky:
                # kind=1, subtype=4 (SKY), model at +0x20 (16-byte aligned)
                rec_hdr = struct.pack("<8I", 1, 0, 0x20, 0, wg.SKY, sky_idx, 0, 0)
                sky_idx += 1
            else:
                # kind=0 (Simple Model), model at +0x10 (16-byte aligned)
                rec_hdr = struct.pack("<4I", 0, 0, 0x10, 0)
            block = rec_hdr + mbytes
            rec_buf += block
            rec_cursor += len(block)

        out = bytearray()
        out += struct.pack("<4I", root_off, 0, 0, 0)
        for i in range(n_m):
            nxt = root_off + (i + 1) * 12 if (i + 1) < n_m else 0
            out += struct.pack("<3I", wg.NODE_LEAF, nxt, rec_offs[i])
        if len(out) < rec_offs[0]:
            out += b"\x00" * (rec_offs[0] - len(out))
        out += rec_buf
        return bytes(out), n_m, total_parts


# ---------------------------------------------------------------------------
# Transcoder 5: EOD track tables + Fast 0TSR v6 packer + .eodmod v2 bundler
# ---------------------------------------------------------------------------
_AFTON_DONOR_CACHE: dict[str, dict[str, dict[str, Any]]] = {}


def _load_afton_donor(eod_disc_data: Path | str) -> dict[str, dict[str, Any]]:
    base = Path(eod_disc_data)
    cand = base / "TRACKS" / "AFTON.TRK"
    if not cand.is_file():
        cand = base / "DATA" / "TRACKS" / "AFTON.TRK"
    if not cand.is_file():
        raise FileNotFoundError(f"Retail AFTON.TRK donor not found under {base}")
    key = str(cand.resolve())
    cached = _AFTON_DONOR_CACHE.get(key)
    if cached is not None:
        return cached
    entries = _read_0tsr_v6_entries(cand.read_bytes())
    by_l = {e["name"].lower(): e for e in entries}
    _AFTON_DONOR_CACHE[key] = by_l
    return by_l


def _compile_v11(sections: list[dict[str, Any]]) -> bytes:
    """Compile a list of {name, parent, pairs} dicts into EOD binary VALT v11."""
    tree = {
        "tool_vtable": 0,
        "unk1": 0,
        "orig_sec_count": len(sections),
        "is_v6": False,
        "sections": [
            {
                "name": s["name"],
                "parent": s.get("parent", ""),
                "pairs": list(s.get("pairs", [])),
                "sec_vtable": 0,
                "tool_ptr": 0,
            }
            for s in sections
        ],
    }
    return valt.compile_binary_valt(tree)


def _upgrade_valt_v6_to_v11(raw_v6: bytes) -> bytes:
    parsed = valt.parse_binary_valt(raw_v6)
    parsed["is_v6"] = False
    return valt.compile_binary_valt(parsed)


def synthesize_eod_track_tables(
    d2d_entries: list[dict[str, Any]],
    afton_donor: dict[str, dict[str, Any]],
    ilin_pts: list[tuple[float, float]],
    track_info: NascarTrackInfo,
    *,
    timings: dict[str, float] | None = None,
) -> list[dict[str, Any]]:
    """Build EOD VALT v11 tables, .gar sheets, event .va0 files, BP/BS, and tail SFX0."""
    with span("synth_eod_tables", timings=timings):
        out: list[dict[str, Any]] = []
        d2d_by_l = {e["name"].lower(): e for e in d2d_entries}

        # 1. Preserve all .gar (SETU v2) setup sheets verbatim
        for e in d2d_entries:
            if e["name"].lower().endswith(".gar"):
                out.append({
                    "name": e["name"], "fourcc": "SETU", "version": 2,
                    "data": e["data"], "is_extractable": False,
                })

        # 2. Upgrade track.val & camera2.val (VALT v6 -> v11) with EOD-safe physics keys
        is_dirt = "yes" if track_info.surface_class == "ShortDirt" else "no"
        is_plate = "yes" if track_info.surface_class == "Superspeedway" else "no"
        if "track.val" in d2d_by_l:
            tv = valt.parse_binary_valt(d2d_by_l["track.val"]["data"])
            secs = list(tv.get("sections", []))
            by_name = {s["name"].lower(): s for s in secs}
            if "physics" not in by_name:
                secs.append({"name": "Physics", "parent": "", "pairs": [("dirt_track", is_dirt), ("restrictor_plate", is_plate)]})
            else:
                pmap = dict(by_name["physics"]["pairs"])
                pmap.setdefault("dirt_track", is_dirt)
                pmap.setdefault("restrictor_plate", is_plate)
                by_name["physics"]["pairs"] = list(pmap.items())
            if "ai" not in by_name:
                secs.append({"name": "AI", "parent": "", "pairs": [("max_lookahead", "1.1"), ("min_lookahead", ".6"), ("pit_speed", "30")]})
            if "skill" not in by_name:
                secs.append({"name": "Skill", "parent": "", "pairs": [
                    ("easy_skill_range", ".20"), ("easy_speed_factor", ".80"),
                    ("medium_skill_range", ".15"), ("medium_speed_factor", ".90"),
                    ("hard_skill_range", ".10"), ("hard_speed_factor", "1.00"),
                ]})
            track_val_bytes = _compile_v11(secs)
        else:
            track_val_bytes = afton_donor["track.val"]["data"]
        out.append({"name": "track.val", "fourcc": "VALT", "version": 11, "data": track_val_bytes, "is_extractable": False})

        if "camera2.val" in d2d_by_l:
            cam2_bytes = _upgrade_valt_v6_to_v11(d2d_by_l["camera2.val"]["data"])
        else:
            cam2_bytes = afton_donor["camera2.val"]["data"]
        out.append({"name": "camera2.val", "fourcc": "VALT", "version": 11, "data": cam2_bytes, "is_extractable": False})

        # 3. Synthesize lineinfo.val from ILIN waypoints
        step = max(1, len(ilin_pts) // 16)
        exit_pairs = [(f"bound{idx}", f"{p[0]:+.3f}, {p[1]:+.3f}") for idx, p in enumerate(ilin_pts[::step][:16])]
        lineinfo_bytes = _compile_v11([
            {"name": "exit_path", "pairs": exit_pairs},
            {"name": "oval", "pairs": [("@exit_path", "exit_path"), ("@lookahead", "$0000$lookahead")]},
            {"name": "oval-bw", "pairs": [("@exit_path", "exit_path"), ("@lookahead", "$0001$lookahead")]},
            {"name": "$0000$lookahead", "pairs": [("default", "1.2")]},
            {"name": "$0001$lookahead", "pairs": [("default", "1.2")]},
        ])
        out.append({"name": "lineinfo.val", "fourcc": "VALT", "version": 11, "data": lineinfo_bytes, "is_extractable": False})

        # 4. Borrow clean EOD template tables + spatial helpers from retail AFTON.TRK
        for tname in ("def_objects.val", "obstacle.val", "shadow.val", "trkefx.val", "intrlude.val", "camera3.val"):
            if tname in afton_donor:
                d = afton_donor[tname]
                out.append({"name": d["name"], "fourcc": d["fourcc"], "version": d["version"], "data": d["data"], "is_extractable": False})

        # Spatial partition tables (track.bpp/track.bsp + track.bp/track.bs aliases + track.obt)
        if "track.bpp" in afton_donor:
            bpp = afton_donor["track.bpp"]
            out.append({"name": "track.bpp", "fourcc": bpp["fourcc"], "version": bpp["version"], "data": bpp["data"], "is_extractable": False})
            out.append({"name": "track.bp", "fourcc": bpp["fourcc"], "version": bpp["version"], "data": bpp["data"], "is_extractable": False})
        if "track.bsp" in afton_donor:
            bsp = afton_donor["track.bsp"]
            out.append({"name": "track.bsp", "fourcc": bsp["fourcc"], "version": bsp["version"], "data": bsp["data"], "is_extractable": False})
            out.append({"name": "track.bs", "fourcc": bsp["fourcc"], "version": bsp["version"], "data": bsp["data"], "is_extractable": False})
        if "track.obt" in d2d_by_l:
            obt = d2d_by_l["track.obt"]
            out.append({"name": "track.obt", "fourcc": "STAB", "version": 0, "data": obt["data"], "is_extractable": False})
        elif "track.obt" in afton_donor:
            obt = afton_donor["track.obt"]
            out.append({"name": "track.obt", "fourcc": obt["fourcc"], "version": obt["version"], "data": obt["data"], "is_extractable": False})

        # 5. Synthesize c_battle.va0 with [respawn] grid along the ILIN spline + contest .va0s
        n_pts = len(ilin_pts)
        respawn_labels = ("a", "b", "c", "d", "e", "f", "g", "h")
        respawn_pairs: list[tuple[str, str]] = []
        respawn_secs: list[dict[str, Any]] = []
        for idx, lbl in enumerate(respawn_labels):
            p_i = (idx * n_pts) // len(respawn_labels)
            x0, z0 = ilin_pts[p_i]
            x1, z1 = ilin_pts[(p_i + 1) % n_pts]
            dx, dz = x1 - x0, z1 - z0
            dl = max(math.hypot(dx, dz), 1e-5)
            sec_name = f"${idx:04d}${lbl}"
            respawn_pairs.append((f"@{lbl}", sec_name))
            respawn_secs.append({
                "name": sec_name,
                "pairs": [("dir", f"{dx / dl:.3f}, {dz / dl:.3f}"), ("pt", f"{x0:.2f}, {z0:.2f}")],
            })

        cx, cz = ilin_pts[0]
        battle_secs = [
            {"name": "respawn", "pairs": respawn_pairs},
            {"name": "objects", "pairs": [("@ewob_cam", "$0099$ewob_cam")]},
            *respawn_secs,
            {"name": "$0099$ewob_cam", "pairs": [("pt", f"{cx:.2f}, 18.00, {cz:.2f}"), ("look", f"{cx:.2f}, 0.00, {cz + 20.0:.2f}")]},
        ]
        battle_bytes = _compile_v11(battle_secs)
        race_bytes = _compile_v11([
            {"name": "objects", "pairs": [("@ewob_cam", "$0000$ewob_cam")]},
            {"name": "$0000$ewob_cam", "pairs": [("pt", f"{cx:.2f}, 18.00, {cz:.2f}"), ("look", f"{cx:.2f}, 0.00, {cz + 20.0:.2f}")]},
        ])
        for ev_name, ev_data in (
            ("c_battle.va0", battle_bytes),
            ("c_normal_race.va0", race_bytes),
            ("c_last_man_race.va0", race_bytes),
            ("c_whip_around_race.va0", race_bytes),
            ("c_suicide_race.va0", race_bytes),
            ("c_kill_derby.va0", battle_bytes),
        ):
            out.append({"name": ev_name, "fourcc": "VALT", "version": 11, "data": ev_data, "is_extractable": True})

        # 6. Borrow ambient SFX0 banks from retail AFTON.TRK (placed at resident tail by packer!)
        for e in afton_donor.values():
            if e["fourcc"].strip().upper() == "SFX0":
                out.append({
                    "name": e["name"], "fourcc": "SFX0", "version": e["version"],
                    "data": e["data"], "is_extractable": False,
                })
        return out


def _pack_0tsr_v6_memory(entries: list[dict[str, Any]], out_path: Path, build_time: int = 1089733550) -> None:
    """Pack a 0TSR v6 archive directly in memory honoring rst0.py's SFX0-tail & 0x800 stream alignment."""
    # Deduplicate case-insensitively (last wins)
    dedup: dict[str, dict[str, Any]] = {}
    for e in entries:
        dedup[e["name"].lower()] = e
    items = list(dedup.values())

    resident, resident_sfx, extractable = [], [], []
    for e in items:
        data = e["data"]
        rec = {
            "name": e["name"],
            "fourcc": e["fourcc"],
            "version": int(e.get("version", 1)),
            "data": data,
            "size": len(data),
            "crc": rst0.eod_crc(data),
            "is_extractable": bool(e.get("is_extractable", False)),
        }
        if rec["is_extractable"]:
            extractable.append(rec)
        elif str(rec["fourcc"]).strip().upper() == "SFX0":
            resident_sfx.append(rec)
        else:
            resident.append(rec)

    all_entries = resident + resident_sfx + extractable
    dir_offset = 0x80
    first_data = (dir_offset + len(all_entries) * 0x44 + 0x7F) & ~0x7F
    cur = first_data
    first_ext_rel = None
    for idx, rec in enumerate(all_entries):
        cur = (cur + 0x7F) & ~0x7F
        if rec["is_extractable"]:
            cur = (cur + rst0.STREAM_ALIGN - 1) & ~(rst0.STREAM_ALIGN - 1)
        rel = cur - dir_offset
        rec["offset"] = rel
        if rec["is_extractable"] and first_ext_rel is None:
            first_ext_rel = rel
        cur += ((rec["size"] + 0x80) & ~0x7F) if idx < len(all_entries) - 1 else rec["size"]

    total_size = cur
    if first_ext_rel is None:
        first_ext_rel = total_size - dir_offset

    hdr_pre = struct.pack(
        "<4sIIIIIIII",
        b"0TSR", 0x28, 6, total_size, build_time, dir_offset, len(all_entries), first_ext_rel, 0,
    )
    out = bytearray(total_size)
    out[0:0x24] = hdr_pre
    struct.pack_into("<I", out, 0x24, rst0.eod_crc(hdr_pre))

    for i, rec in enumerate(all_entries):
        row = dir_offset + i * 0x44
        out[row : row + 32] = rec["name"].encode("latin1")[:31].ljust(32, b"\0")
        out[row + 32 : row + 36] = rec["fourcc"].ljust(4, " ")[:4][::-1].encode("latin1")
        struct.pack_into("<IIII4I", out, row + 36, rec["version"], rec["size"], rec["offset"], rec["crc"], 0, 0, 0, 0)
        abs_off = dir_offset + rec["offset"]
        out[abs_off : abs_off + rec["size"]] = rec["data"]

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(out)


def _render_minimap_png(pts_xz: list[tuple[float, float]], out_path: Path, size: int = 256) -> None:
    xs = [p[0] for p in pts_xz]
    zs = [p[1] for p in pts_xz]
    span_w = max(max(xs) - min(xs), max(zs) - min(zs), 20.0)
    cx, cz = 0.5 * (min(xs) + max(xs)), 0.5 * (min(zs) + max(zs))
    half = 0.5 * span_w * 1.24
    scale = (size - 1) / (2.0 * half)
    px = [((x - (cx - half)) * scale, ((cz + half) - z) * scale) for x, z in pts_xz]
    px.append(px[0])

    img = Image.new("RGBA", (size, size), (14, 18, 26, 255))
    draw = ImageDraw.Draw(img, "RGBA")
    draw.line(px, fill=(42, 48, 60, 255), width=11, joint="curve")
    draw.line(px, fill=(210, 218, 232, 255), width=7, joint="curve")
    sx, sy = px[0]
    draw.ellipse([sx - 5, sy - 5, sx + 5, sy + 5], fill=(46, 204, 113, 255), outline=(255, 255, 255, 255), width=2)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    img.save(out_path, format="PNG")


def import_nascar_track(
    iso_path: Path | str,
    eod_disc_data: Path | str,
    track_key: str,
    out_dir: Path | str,
    *,
    lighting_mode: str = "day",
    export_eodmod: bool = True,
) -> ImportTrackResult:
    """Import a NASCAR D2D .TRK track into EOD 0TSR v6 + .eodcol + minimap + .eodmod v2."""
    t_total0 = time.perf_counter_ns()
    timings: dict[str, float] = {}
    mode = (lighting_mode or "day").strip().lower()
    if mode not in ("day", "night"):
        raise ValueError(f"Invalid lighting_mode {lighting_mode!r}; expected 'day' or 'night'")

    tracks = scan_nascar_tracks(iso_path, timings=timings)
    want = track_key.upper().removesuffix(".TRK")
    tinfo = next(
        (t for t in tracks if t.track_key == want or t.display_name.upper() == want),
        None,
    )
    if tinfo is None:
        raise KeyError(f"NASCAR track {track_key!r} not found in {iso_path}")

    with span("unpack_trk", timings=timings):
        kind, root_p, fh, vol = _open_d2d_source(iso_path)
        try:
            if kind == "iso":
                parts = tinfo.iso_path.split("/")
                node = vol.root
                for seg in parts:
                    node = vol.children(node)[seg.upper()]
                raw_trk = vol.read(node.offset, node.size)
            else:
                raw_trk = (root_p / tinfo.iso_path).read_bytes()
        finally:
            if fh is not None:
                fh.close()
        d2d_entries = _read_0tsr_v2_entries(raw_trk)
        d2d_by_l = {e["name"].lower(): e for e in d2d_entries}

    # 1. GRAF v13 -> GRAF v18
    graf_v18, n_models, n_parts = transcode_graf_v13_to_v18(d2d_by_l["track.grf"]["data"], timings=timings)

    # 2. SCOL v1 -> SCOL v4 + .eodcol (32-bit)
    sobl_raw = d2d_by_l["track.sol"]["data"]
    sobl_parsed = wc.parse_sobl(sobl_raw)
    scol_v4, mesh32, eodcol_bytes, n_scol_tris = transcode_scol_v1_to_v4_and_eodcol(
        d2d_by_l["track.bsc"]["data"],
        sobl_raw=sobl_raw,
        surface_class=tinfo.surface_class,
        timings=timings,
    )

    # 3. ILIN v19 -> ILIN v27 (track/oval/bw/bw_oval)
    ilin_map, ilin_pts = transcode_ilin_v19_to_v27(d2d_by_l["track.ild"]["data"], timings=timings)

    # 4. TEX v9 -> TEX v10 (with Day/Night skybox remapping)
    tex_entries: list[dict[str, Any]] = []
    with span("transcode_tex", timings=timings):
        nsky_by_target: dict[str, bytes] = {}
        raw_tex = [e for e in d2d_entries if e["fourcc"].strip().upper() == "TEX"]
        if mode == "night":
            for e in raw_tex:
                nl = e["name"].lower()
                if nl.startswith("nsky") and nl.endswith(".tex"):
                    nsky_by_target[nl[1:]] = e["data"]
        for e in raw_tex:
            nl = e["name"].lower()
            if nl.startswith("nsky") and nl.endswith(".tex"):
                continue  # consumed into sky*.tex when night mode, omitted when day mode
            src_data = nsky_by_target.get(nl, e["data"]) if mode == "night" else e["data"]
            tex_v10 = transcode_tex_v9_to_v10(src_data)
            tex_entries.append({
                "name": e["name"], "fourcc": "TEX ", "version": 10,
                "data": tex_v10, "is_extractable": False,
            })

    # 5. Synthesize EOD VALT/SETU/event/SFX0 tables
    afton_donor = _load_afton_donor(eod_disc_data)
    table_entries = synthesize_eod_track_tables(d2d_entries, afton_donor, ilin_pts, tinfo, timings=timings)

    # 6. Assemble & pack 0TSR v6 .TRK
    out_root = Path(out_dir)
    out_root.mkdir(parents=True, exist_ok=True)
    trk_path = out_root / f"{tinfo.track_key}.TRK"
    eodcol_path = out_root / f"{tinfo.track_key}.eodcol"
    minimap_path = out_root / "minimap_preview.png"
    metadata_path = out_root / f"{tinfo.track_key}_metadata.json"

    all_entries: list[dict[str, Any]] = [
        {"name": "track.grf", "fourcc": "GRAF", "version": 18, "data": graf_v18, "is_extractable": False},
        {"name": "track.bsc", "fourcc": "SCOL", "version": 4, "data": scol_v4, "is_extractable": False},
        {"name": "track.sol", "fourcc": "SOBL", "version": 3, "data": sobl_raw, "is_extractable": False},
        *[
            {"name": k, "fourcc": "ILIN", "version": 27, "data": v, "is_extractable": False}
            for k, v in ilin_map.items()
        ],
        *tex_entries,
        *table_entries,
    ]

    with span("pack_trk", timings=timings):
        _pack_0tsr_v6_memory(all_entries, trk_path)
        eodcol_path.write_bytes(eodcol_bytes)
        _render_minimap_png(ilin_pts, minimap_path)

    aabb_min = tuple(float(v) for v in mesh32.aabb_min) if mesh32 is not None else (0.0, 0.0, 0.0)
    aabb_max = tuple(float(v) for v in mesh32.aabb_max) if mesh32 is not None else (0.0, 0.0, 0.0)

    eodmod_path: Path | None = None
    if export_eodmod:
        with span("emit_eodmod", timings=timings):
            eodmod_path = out_root / f"{tinfo.track_key.lower()}_{mode}.eodmod"
            trk_rel = f"DATA/TRACKS/{tinfo.track_key}.TRK"
            col_rel = f"DATA/TRACKS/{tinfo.track_key}.eodcol"
            manifest = {
                "schema": mp2.SCHEMA_V2,
                "id": f"nascar_track_{tinfo.track_key.lower()}_{mode}",
                "name": f"NASCAR Track: {tinfo.display_name} ({mode.title()})",
                "version": "1.0.0",
                "author": "EOD World Studio NASCAR Importer",
                "description": f"Converted from NASCAR: Dirt to Daytona ({tinfo.display_name} {tinfo.longname}, {tinfo.surface_class}).",
                "tags": ["track", "nascar", tinfo.surface_class.lower(), mode],
                "operations": [
                    {"op": "addArchive", "target": trk_rel, "blob": trk_rel},
                    {"op": "addArchive", "target": col_rel, "blob": col_rel},
                    {
                        "op": "valtPatch",
                        "table": "tracks.val",
                        "patches": {
                            tinfo.track_key: {
                                "name": tinfo.display_name,
                                "longname": tinfo.longname or tinfo.surface_class,
                                "surface_class": tinfo.surface_class,
                                "lighting_mode": mode,
                            }
                        },
                    },
                ],
            }
            mp2.pack_eodmod_v2(
                manifest,
                {trk_rel: trk_path, col_rel: eodcol_path},
                eodmod_path,
            )

    total_ms = (time.perf_counter_ns() - t_total0) / 1_000_000.0
    meta = {
        "track_key": tinfo.track_key,
        "display_name": tinfo.display_name,
        "longname": tinfo.longname,
        "surface_class": tinfo.surface_class,
        "lighting_mode": mode,
        "has_night_sky": tinfo.has_night_sky,
        "graf_models": n_models,
        "graf_parts": n_parts,
        "scol_tris": n_scol_tris,
        "sobl_records": sobl_parsed["count"],
        "ilin_nodes": len(ilin_pts),
        "tex_count": len(tex_entries),
        "eodcol_aabb_min": list(aabb_min),
        "eodcol_aabb_max": list(aabb_max),
        "timings_ms": {k: round(v, 2) for k, v in timings.items()},
        "total_ms": round(total_ms, 2),
    }
    metadata_path.write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")

    return ImportTrackResult(
        track_key=tinfo.track_key,
        display_name=tinfo.display_name,
        lighting_mode=mode,
        trk_path=trk_path,
        eodcol_path=eodcol_path,
        minimap_path=minimap_path,
        metadata_path=metadata_path,
        eodmod_path=eodmod_path,
        graf_models=n_models,
        graf_parts=n_parts,
        scol_tris=n_scol_tris,
        sobl_records=sobl_parsed["count"],
        ilin_nodes=len(ilin_pts),
        tex_count=len(tex_entries),
        eodcol_aabb_min=aabb_min,
        eodcol_aabb_max=aabb_max,
        timings_ms=timings,
        total_ms=total_ms,
    )
