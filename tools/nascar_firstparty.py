"""Additive first-party NASCAR vehicle lifecycle: 3D UI icons, unpainted Sweeney's skins,
full-panel PAINTKIT UV mapping, UIBIG canvas synthesis, and Tracy/Perfetto telemetry.
"""

from __future__ import annotations

from contextlib import contextmanager
import copy
import math
from pathlib import Path
import re
import struct
import sys
import time
from typing import Any, Iterator

import numpy as np
from PIL import Image, ImageFilter

_TOOLS_DIR = Path(__file__).resolve().parent
_EDITOR_DIR = _TOOLS_DIR / "world_editor"
for _p in (str(_TOOLS_DIR), str(_EDITOR_DIR)):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import rst0
import tex as tex_mod
import valt
import vif_mesh

CANV_HEADER_SIZE = 36
CANV_SIZE = CANV_HEADER_SIZE + 512 * 256 * 4  # 524,324 bytes
PAINTKIT_PANELS: tuple[str, ...] = (
    "hood",
    "roof",
    "ldoor",
    "rdoor",
    "lfender",
    "rfender",
    "decklid",
)
SWEENEYS_PRIMERS: dict[str, tuple[int, int, int]] = {
    "primer_gray": (154, 158, 162),
    "factory_white": (226, 228, 224),
    "barn_red": (152, 48, 42),
    "steel_blue": (68, 98, 128),
    "matte_black": (46, 49, 54),
}

_SWIZZLE_CACHE: dict[tuple[int, int], np.ndarray] = {}


def _get_profiler() -> Any:
    try:
        import perf_profiler

        return perf_profiler.get_profiler()
    except Exception:
        return None


@contextmanager
def span(name: str, category: str = "nascar_import", timings: dict[str, float] | None = None) -> Iterator[None]:
    """Record a Tracy/Perfetto Chrome-Trace span via tools/world_editor/perf_profiler.py."""
    prof = _get_profiler()
    t0 = time.perf_counter_ns()
    if prof is not None and hasattr(prof, "span"):
        cm = prof.span(name, category)
    elif prof is not None and hasattr(prof, "zone"):
        cm = prof.zone(name, category)
    else:
        cm = None
    if cm is not None:
        with cm:
            try:
                yield
            finally:
                if timings is not None:
                    ms = (time.perf_counter_ns() - t0) / 1_000_000.0
                    timings[name] = timings.get(name, 0.0) + ms
    else:
        try:
            yield
        finally:
            if timings is not None:
                ms = (time.perf_counter_ns() - t0) / 1_000_000.0
                timings[name] = timings.get(name, 0.0) + ms


def _upload_addr8_lut(w: int, h: int) -> np.ndarray:
    key = (w, h)
    lut = _SWIZZLE_CACHE.get(key)
    if lut is not None:
        return lut
    ys, xs = np.mgrid[0:h, 0:w].astype(np.int32)
    if h >= 32:
        block = (ys & ~15) * w + (xs & ~15) * 2
        swap = (((ys + 2) >> 2) & 1) * 4
        row = (((ys & ~3) >> 1) + (ys & 1)) & 7
        col = row * w * 2 + ((xs + swap) & 7) * 4
        lane = ((ys >> 1) & 1) + ((xs >> 2) & 2)
        lut = (block + col + lane).reshape(-1)
    else:
        lut = (ys * w + xs).reshape(-1)
    _SWIZZLE_CACHE[key] = lut
    return lut


def _as_rgba_image(img: Image.Image | np.ndarray | bytes, size: tuple[int, int] | None = None) -> Image.Image:
    if isinstance(img, Image.Image):
        out = img.convert("RGBA")
    elif isinstance(img, np.ndarray):
        arr = np.asarray(img, dtype=np.uint8)
        out = Image.fromarray(arr, mode="RGBA")
    elif isinstance(img, (bytes, bytearray, memoryview)):
        raw = bytes(img)
        if len(raw) == 512 * 256 * 4:
            out = Image.frombytes("RGBA", (512, 256), raw)
        elif len(raw) == 256 * 256 * 4:
            out = Image.frombytes("RGBA", (256, 256), raw)
        else:
            w, h, rgba, _ = tex_mod.decompile_tex(raw)
            out = Image.frombytes("RGBA", (w, h), rgba)
    else:
        raise TypeError(f"Unsupported RGBA image input: {type(img)!r}")
    if size is not None and out.size != size:
        out = out.resize(size, Image.Resampling.LANCZOS)
    return out


def encode_tex_v10(
    image: Image.Image | np.ndarray | bytes,
    *,
    fmt: int = 0x201,
    flags: int = 0x320600,
    header_word5: int = 0,
) -> bytes:
    """Fast vectorized PS2 TEX v10 8-bit palettized encoder (100% compatible with tex.decompile_tex)."""
    im = _as_rgba_image(image)
    w, h = im.size
    if w < 16 or h < 16 or w > 1024 or h > 1024 or (w & (w - 1)) or (h & (h - 1)):
        raise ValueError(f"Invalid TEX power-of-two dimensions: {w}x{h}")

    q = im.quantize(colors=256, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
    pal_raw = q.getpalette(rawmode="RGBA") or []
    pal = np.zeros((256, 4), dtype=np.uint8)
    n_pal = min(256, len(pal_raw) // 4)
    if n_pal:
        pal[:n_pal] = np.frombuffer(bytes(pal_raw[: n_pal * 4]), dtype=np.uint8).reshape(n_pal, 4)
    # TEX stores palette entries as [A, R, G, B]
    pal_argb = np.stack([pal[:, 3], pal[:, 0], pal[:, 1], pal[:, 2]], axis=1).tobytes()

    idx_mip = np.asarray(q, dtype=np.uint8)
    levels = 1
    cw, ch = w, h
    while min(cw, ch) > 2:
        levels += 1
        cw, ch = max(1, cw // 2), max(1, ch // 2)

    all_mips: list[bytes] = []
    mip_offsets: list[int] = []
    cur_off = 0x480
    cur_idx = idx_mip
    cw, ch = w, h
    for lev in range(levels):
        if lev > 0:
            cw, ch = max(1, cw // 2), max(1, ch // 2)
            cur_idx = cur_idx[::2, ::2][:ch, :cw]
        flat = cur_idx.reshape(-1)
        if ch >= 32:
            swiz = np.empty(cw * ch, dtype=np.uint8)
            swiz[_upload_addr8_lut(cw, ch)] = flat
            mip_bytes = swiz.tobytes()
        else:
            alloc = cw * ch
            if lev < levels - 1 and alloc < 128:
                alloc = 128
            buf = bytearray(alloc)
            buf[: cw * ch] = flat.tobytes()
            mip_bytes = bytes(buf)
        mip_offsets.append(cur_off)
        cur_off += len(mip_bytes)
        all_mips.append(mip_bytes)

    hdr = bytearray(0x80)
    struct.pack_into("<IIIII", hdr, 0, w, h, levels, fmt, flags)
    struct.pack_into("<I", hdr, 0x14, header_word5)
    struct.pack_into("<I", hdr, 0x18, 0x80)
    for i, off in enumerate(mip_offsets):
        struct.pack_into("<I", hdr, 0x1C + i * 4, off)
    return bytes(hdr) + pal_argb + b"".join(all_mips)


def encode_ui_icon_pair(img: Image.Image) -> tuple[bytes, bytes]:
    """Encode a 256x256 RGBA UI icon to (256x256 .tm0, 16x16 .tex) TEX v10 blobs."""
    full = _as_rgba_image(img, (256, 256))
    small = full.resize((16, 16), Image.Resampling.LANCZOS)
    return (
        encode_tex_v10(full, fmt=0x201, flags=0x320600, header_word5=0),
        encode_tex_v10(small, fmt=0x201, flags=0x320600, header_word5=0),
    )


def encode_livery_tex_pair(img: Image.Image) -> tuple[bytes, bytes]:
    """Encode a 512x256 body skin to CT.RES (512x256 .tm0, 256x128 .tex) TEX v10 blobs."""
    full = _as_rgba_image(img, (512, 256))
    half = full.resize((256, 128), Image.Resampling.LANCZOS)
    return (
        encode_tex_v10(full, fmt=0x30201, flags=0x320600, header_word5=0x100),
        encode_tex_v10(half, fmt=0x30201, flags=0x320600, header_word5=0x100),
    )


class FastRst0Archive:
    """In-memory 0TSR v6 reader/writer that preserves untouched retail payloads & CRCs."""

    def __init__(self, path: Path | str) -> None:
        raw = Path(path).read_bytes()
        if raw[:4] != b"0TSR":
            raise ValueError(f"Not a 0TSR archive: {path}")
        hdr_size, version, _total, build_time, dir_off, count, extract_blk, unk = struct.unpack_from("<8I", raw, 4)
        if version != 6:
            raise ValueError(f"Expected 0TSR v6 archive, got v{version}: {path}")
        self.version = version
        self.build_time = build_time
        self.unk = unk
        self.entries: list[dict[str, Any]] = []
        self._index: dict[str, int] = {}
        self.original_meta: dict[str, tuple[int, int, bytes]] = {}
        for i in range(count):
            eb = raw[dir_off + i * 0x44 : dir_off + (i + 1) * 0x44]
            name = eb[:32].split(b"\0")[0].decode("latin1")
            fourcc = eb[0x20:0x24][::-1].decode("latin1", errors="replace").strip()
            ver, sz, off, crc = struct.unpack_from("<4I", eb, 0x24)
            payload = raw[dir_off + off : dir_off + off + sz]
            rec = {
                "name": name,
                "fourcc": fourcc,
                "version": ver,
                "size": sz,
                "crc": crc,
                "is_extractable": off >= extract_blk,
                "data": payload,
            }
            self._index[name.lower()] = len(self.entries)
            self.entries.append(rec)
            self.original_meta[name.lower()] = (sz, crc, payload)

    def has(self, name: str) -> bool:
        return name.lower() in self._index

    def read(self, name: str) -> bytes:
        idx = self._index.get(name.lower())
        if idx is None:
            raise KeyError(f"Missing entry {name!r}")
        return self.entries[idx]["data"]

    def put(
        self,
        name: str,
        data: bytes,
        *,
        version: int = 11,
        fourcc: str = "VALT",
        extractable: bool = False,
        replace: bool = False,
    ) -> None:
        key = name.lower()
        crc = rst0.eod_crc(data)
        rec = {
            "name": name,
            "fourcc": fourcc,
            "version": version,
            "size": len(data),
            "crc": crc,
            "is_extractable": extractable,
            "data": data,
        }
        if key in self._index:
            if not replace:
                raise ValueError(f"Archive entry collision: {name}")
            self.entries[self._index[key]] = rec
        else:
            self._index[key] = len(self.entries)
            self.entries.append(rec)

    def assert_preserved(self, allowed: set[str] | tuple[str, ...] = ()) -> None:
        allow = {a.lower() for a in allowed}
        for key, (sz, crc, orig_bytes) in self.original_meta.items():
            if key in allow:
                continue
            idx = self._index.get(key)
            if idx is None:
                raise AssertionError(f"Retail entry deleted: {key}")
            cur = self.entries[idx]
            if cur["size"] != sz or cur["crc"] != crc or cur["data"] != orig_bytes:
                raise AssertionError(f"Retail entry mutated: {key}")

    def pack(self, out_path: Path | str) -> Path:
        dir_off = 0x80
        res = [e for e in self.entries if not e["is_extractable"] and e["fourcc"].upper() != "SFX0"]
        sfx = [e for e in self.entries if not e["is_extractable"] and e["fourcc"].upper() == "SFX0"]
        ext = [e for e in self.entries if e["is_extractable"]]
        ordered = res + sfx + ext

        cur = (dir_off + len(ordered) * 0x44 + 0x7F) & ~0x7F
        first_ext: int | None = None
        offsets: list[int] = []
        for i, e in enumerate(ordered):
            cur = (cur + 0x7F) & ~0x7F
            if e["is_extractable"]:
                cur = (cur + rst0.STREAM_ALIGN - 1) & ~(rst0.STREAM_ALIGN - 1)
            rel = cur - dir_off
            offsets.append(rel)
            if e["is_extractable"] and first_ext is None:
                first_ext = rel
            cur += ((e["size"] + 0x80) & ~0x7F) if i < len(ordered) - 1 else e["size"]
        total_sz = cur
        if first_ext is None:
            first_ext = total_sz - dir_off

        hdr_pre = struct.pack(
            "<4sIIIIIIII", b"0TSR", 0x28, self.version, total_sz, self.build_time, dir_off, len(ordered), first_ext, self.unk
        )
        out = bytearray(total_sz)
        out[0:0x28] = hdr_pre + struct.pack("<I", rst0.eod_crc(hdr_pre))
        pos = dir_off
        for e, rel in zip(ordered, offsets):
            nm = e["name"].encode("latin1")[:32].ljust(32, b"\0")
            fcc = e["fourcc"].ljust(4, " ")[:4][::-1].encode("latin1")
            out[pos : pos + 0x44] = nm + fcc + struct.pack("<IIII4I", e["version"], e["size"], rel, e["crc"], 0, 0, 0, 0)
            pos += 0x44
            dstart = dir_off + rel
            out[dstart : dstart + e["size"]] = e["data"]
        dest = Path(out_path)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(out)
        return dest


# ---------------------------------------------------------------------------
# In-memory MODL triangle decoding & fast NumPy Z-buffered 3/4 isometric icons
# ---------------------------------------------------------------------------

_NON_BODY_HINTS = ("glass", "wind", "nsuit", "roll", "pad", "pipe", "wheel", "tire", "steer", "shadow", "gauge", "dash", "flag", "air", "filt", "light")


def _extract_modl_triangles(modl_bytes: bytes) -> dict[str, Any]:
    hdr = vif_mesh.parse_modl(modl_bytes)
    part_records: list[tuple[int, str, np.ndarray, np.ndarray, np.ndarray]] = []
    tex_areas: dict[str, float] = {}
    for p in hdr["parts"]:
        tex_name = p["texture"].lower()
        group = int(p["group"]) & 0xFFFF
        verts_p: list[tuple[float, float, float]] = []
        verts_uv: list[tuple[float, float]] = []
        tris_i: list[tuple[int, int, int]] = []
        base = 0
        for b in vif_mesh.decode_part(modl_bytes, p):
            if b["mscal"] == 4:
                continue
            bverts = b["verts"]
            for v in bverts:
                verts_p.append(v["p"])
                verts_uv.append(v["uv"])
            for i0, i1, i2 in vif_mesh.strip_triangles(bverts):
                tris_i.append((base + i0, base + i1, base + i2))
            base += len(bverts)
        if not tris_i:
            continue
        vp = np.asarray(verts_p, dtype=np.float32)
        vuv = np.asarray(verts_uv, dtype=np.float32)
        ti = np.asarray(tris_i, dtype=np.int32)
        e1 = vp[ti[:, 1]] - vp[ti[:, 0]]
        e2 = vp[ti[:, 2]] - vp[ti[:, 0]]
        areas = 0.5 * np.linalg.norm(np.cross(e1, e2), axis=1)
        tex_areas[tex_name] = tex_areas.get(tex_name, 0.0) + float(areas.sum())
        part_records.append((group, tex_name, vp, vuv, ti))

    if not part_records:
        raise ValueError("MODL contains no renderable geometry")

    body_candidates = {k: v for k, v in tex_areas.items() if k and not any(h in k for h in _NON_BODY_HINTS)}
    body_tex = max(body_candidates or tex_areas, key=(body_candidates or tex_areas).get)

    all_p: list[np.ndarray] = []
    all_uv: list[np.ndarray] = []
    all_t: list[np.ndarray] = []
    all_g: list[np.ndarray] = []
    all_body: list[np.ndarray] = []
    offset = 0
    for group, tex_name, vp, vuv, ti in part_records:
        all_p.append(vp)
        all_uv.append(vuv)
        all_t.append(ti + offset)
        all_g.append(np.full(len(ti), group, dtype=np.int32))
        all_body.append(np.full(len(ti), tex_name == body_tex, dtype=bool))
        offset += len(vp)

    pos = np.vstack(all_p)
    uv = np.vstack(all_uv)
    tris = np.vstack(all_t)
    groups = np.concatenate(all_g)
    is_body = np.concatenate(all_body)
    return {"pos": pos, "uv": uv, "tris": tris, "groups": groups, "is_body": is_body, "body_tex": body_tex}


def _add_wheel_silhouettes(pos: np.ndarray, uv: np.ndarray, tris: np.ndarray, is_body: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Synthesize lightweight 4-wheel cylinders from body bounds for crisp showroom silhouettes."""
    bmin, bmax = pos.min(axis=0), pos.max(axis=0)
    half_x = max(0.5, 0.5 * (bmax[0] - bmin[0]))
    length = max(2.0, bmax[2] - bmin[2])
    r = min(0.36, 0.072 * length)
    w_t = 0.24
    z_f = bmin[2] + 0.74 * length
    z_r = bmin[2] + 0.24 * length
    y_c = bmin[1] + r * 0.65
    segs = 12
    ang = np.linspace(0.0, 2.0 * math.pi, segs, endpoint=False)
    cy, cz = np.sin(ang) * r, np.cos(ang) * r

    w_pos, w_uv, w_tri = [], [], []
    base = len(pos)
    for sx in (-1.0, 1.0):
        x_out = sx * (half_x * 0.94)
        x_in = x_out - sx * w_t
        for zc in (z_f, z_r):
            verts = []
            for k in range(segs):
                verts.append((x_out, y_c + cy[k], zc + cz[k]))
                verts.append((x_in, y_c + cy[k], zc + cz[k]))
            c_out = len(verts)
            verts.append((x_out, y_c, zc))
            c_in = len(verts)
            verts.append((x_in, y_c, zc))
            for k in range(segs):
                kn = (k + 1) % segs
                o0, i0, o1, i1 = 2 * k, 2 * k + 1, 2 * kn, 2 * kn + 1
                w_tri.extend([(base + o0, base + o1, base + i0), (base + o1, base + i1, base + i0)])
                w_tri.append((base + c_out, base + o0, base + o1))
                w_tri.append((base + c_in, base + i1, base + i0))
            w_pos.append(np.asarray(verts, dtype=np.float32))
            w_uv.append(np.zeros((len(verts), 2), dtype=np.float32))
            base += len(verts)

    wp = np.vstack([pos, *w_pos])
    wuv = np.vstack([uv, *w_uv])
    wt = np.vstack([tris, np.asarray(w_tri, dtype=np.int32)])
    wb = np.concatenate([is_body, np.zeros(len(w_tri), dtype=bool)])
    return wp, wuv, wt, wb


def _rasterize_buffers(
    mesh: dict[str, Any],
    *,
    size: int,
    yaw_deg: float,
    pitch_deg: float,
    fill_frac: float,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Rasterize MODL once into (u_buf, v_buf, shade_buf, body_buf, hit_mask)."""
    pos, uv, tris, is_body = _add_wheel_silhouettes(mesh["pos"], mesh["uv"], mesh["tris"], mesh["is_body"])
    center = 0.5 * (pos.min(axis=0) + pos.max(axis=0))
    pts = pos - center

    yaw, pitch = math.radians(yaw_deg), math.radians(pitch_deg)
    cy, sy = math.cos(yaw), math.sin(yaw)
    cp, sp = math.cos(pitch), math.sin(pitch)
    # Mirror X for native showroom front-left facing handedness
    rx = -(pts[:, 0] * cy + pts[:, 2] * sy)
    rz0 = -pts[:, 0] * sy + pts[:, 2] * cy
    ry = pts[:, 1] * cp - rz0 * sp
    rz = pts[:, 1] * sp + rz0 * cp

    span_x = max(1e-4, float(rx.max() - rx.min()))
    span_y = max(1e-4, float(ry.max() - ry.min()))
    scale = (size * fill_frac) / max(span_x, span_y)
    cx_p, cy_p = 0.5 * (rx.min() + rx.max()), 0.5 * (ry.min() + ry.max())
    sx = (rx - cx_p) * scale + 0.5 * size
    sy_scr = 0.5 * size - (ry - cy_p) * scale

    v0 = tris[:, 0]
    v1 = tris[:, 1]
    v2 = tris[:, 2]
    p0 = np.stack([rx[v0], ry[v0], rz[v0]], axis=1)
    p1 = np.stack([rx[v1], ry[v1], rz[v1]], axis=1)
    p2 = np.stack([rx[v2], ry[v2], rz[v2]], axis=1)
    nrm = np.cross(p1 - p0, p2 - p0)
    nlen = np.linalg.norm(nrm, axis=1, keepdims=True)
    nrm = nrm / np.maximum(nlen, 1e-6)
    light = np.array([-0.38, 0.78, 0.50], dtype=np.float32)
    light /= np.linalg.norm(light)
    ndotl = np.abs(nrm @ light)
    tri_shade = np.clip(0.46 + 0.58 * ndotl, 0.35, 1.12).astype(np.float32)

    zbuf = np.full((size, size), -1e9, dtype=np.float32)
    ubuf = np.zeros((size, size), dtype=np.float32)
    vbuf = np.zeros((size, size), dtype=np.float32)
    sbuf = np.zeros((size, size), dtype=np.float32)
    bbuf = np.zeros((size, size), dtype=bool)
    hbuf = np.zeros((size, size), dtype=bool)

    x0s, x1s, x2s = sx[v0], sx[v1], sx[v2]
    y0s, y1s, y2s = sy_scr[v0], sy_scr[v1], sy_scr[v2]
    z0s, z1s, z2s = rz[v0], rz[v1], rz[v2]
    u0s, u1s, u2s = uv[v0, 0], uv[v1, 0], uv[v2, 0]
    vv0s, vv1s, vv2s = uv[v0, 1], uv[v1, 1], uv[v2, 1]

    for i in range(len(tris)):
        x0, x1, x2 = float(x0s[i]), float(x1s[i]), float(x2s[i])
        y0, y1, y2 = float(y0s[i]), float(y1s[i]), float(y2s[i])
        denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
        if abs(denom) < 0.25:
            continue
        min_x = max(0, int(math.floor(min(x0, x1, x2))))
        max_x = min(size - 1, int(math.ceil(max(x0, x1, x2))))
        min_y = max(0, int(math.floor(min(y0, y1, y2))))
        max_y = min(size - 1, int(math.ceil(max(y0, y1, y2))))
        if min_x > max_x or min_y > max_y:
            continue
        inv_d = 1.0 / denom
        xs = np.arange(min_x, max_x + 1, dtype=np.float32)[None, :] + 0.5
        ys = np.arange(min_y, max_y + 1, dtype=np.float32)[:, None] + 0.5
        w0 = ((y1 - y2) * (xs - x2) + (x2 - x1) * (ys - y2)) * inv_d
        w1 = ((y2 - y0) * (xs - x2) + (x0 - x2) * (ys - y2)) * inv_d
        w2 = 1.0 - w0 - w1
        inside = (w0 >= -1e-4) & (w1 >= -1e-4) & (w2 >= -1e-4)
        if not inside.any():
            continue
        z = w0 * z0s[i] + w1 * z1s[i] + w2 * z2s[i]
        sub_z = zbuf[min_y : max_y + 1, min_x : max_x + 1]
        win = inside & (z > sub_z)
        if not win.any():
            continue
        sub_z[win] = z[win]
        ubuf[min_y : max_y + 1, min_x : max_x + 1][win] = (w0 * u0s[i] + w1 * u1s[i] + w2 * u2s[i])[win]
        vbuf[min_y : max_y + 1, min_x : max_x + 1][win] = (w0 * vv0s[i] + w1 * vv1s[i] + w2 * vv2s[i])[win]
        sbuf[min_y : max_y + 1, min_x : max_x + 1][win] = tri_shade[i]
        bbuf[min_y : max_y + 1, min_x : max_x + 1][win] = bool(is_body[i])
        hbuf[min_y : max_y + 1, min_x : max_x + 1][win] = True

    return ubuf, vbuf, sbuf, bbuf, hbuf


def _shade_buffers(
    bufs: tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray, np.ndarray],
    livery_arr: np.ndarray,
) -> Image.Image:
    ubuf, vbuf, sbuf, bbuf, hbuf = bufs
    th, tw = livery_arr.shape[:2]
    out = np.zeros((*hbuf.shape, 4), dtype=np.uint8)
    ui = np.clip((ubuf % 1.0) * (tw - 1), 0, tw - 1).astype(np.int32)
    vi = np.clip((vbuf % 1.0) * (th - 1), 0, th - 1).astype(np.int32)
    sampled = livery_arr[vi, ui, :3].astype(np.float32)
    non_body = np.array([34.0, 38.0, 44.0], dtype=np.float32)
    rgb = np.where(bbuf[:, :, None], sampled, non_body[None, None, :]) * sbuf[:, :, None]
    out[hbuf, :3] = np.clip(rgb[hbuf], 0, 255).astype(np.uint8)
    out[hbuf, 3] = 255
    return Image.fromarray(out, mode="RGBA")


def render_vehicle_ui_icons(
    modl_bytes: bytes,
    livery_rgba: Image.Image | np.ndarray | bytes,
    size: int = 256,
    *,
    _cached_buffers: tuple[Any, Any] | None = None,
) -> tuple[Image.Image, Image.Image]:
    """Render alpha-masked 256x256 3/4 isometric uicar (showroom) and uicars (HUD) icons."""
    livery_arr = np.asarray(_as_rgba_image(livery_rgba), dtype=np.uint8)
    if _cached_buffers is None:
        mesh = _extract_modl_triangles(modl_bytes)
        buf_showroom = _rasterize_buffers(mesh, size=size, yaw_deg=35.0, pitch_deg=18.0, fill_frac=0.80)
        buf_hud = _rasterize_buffers(mesh, size=size, yaw_deg=42.0, pitch_deg=15.0, fill_frac=0.92)
    else:
        buf_showroom, buf_hud = _cached_buffers
    return _shade_buffers(buf_showroom, livery_arr), _shade_buffers(buf_hud, livery_arr)


# ---------------------------------------------------------------------------
# Full-panel PAINTKIT UV derivation, Sweeney's primer skins & UIBIG CANV
# ---------------------------------------------------------------------------

def _dominant_uv_rect(
    tri_uvs: list[np.ndarray],
    tri_areas: list[float],
    tex_w: int,
    tex_h: int,
) -> tuple[int, int, int, int] | None:
    if not tri_uvs:
        return None
    gw, gh = 32, 16
    grid = np.zeros((gh, gw), dtype=np.float64)
    ctrs = np.array([uv.mean(axis=0) for uv in tri_uvs], dtype=np.float64)
    ctrs[:, 0] = np.clip(ctrs[:, 0], 0.0, 1.0)
    ctrs[:, 1] = np.clip(ctrs[:, 1], 0.0, 1.0)
    gx = np.clip((ctrs[:, 0] * gw).astype(np.int32), 0, gw - 1)
    gy = np.clip((ctrs[:, 1] * gh).astype(np.int32), 0, gh - 1)
    ws = np.asarray(tri_areas, dtype=np.float64)
    np.add.at(grid, (gy, gx), ws)
    peak = np.unravel_index(int(np.argmax(grid)), grid.shape)
    thresh = grid[peak] * 0.04
    visited = {peak}
    stack = [peak]
    while stack:
        cy, cx = stack.pop()
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                ny, nx = cy + dy, cx + dx
                if 0 <= ny < gh and 0 <= nx < gw and (ny, nx) not in visited and grid[ny, nx] > thresh:
                    visited.add((ny, nx))
                    stack.append((ny, nx))
    pts = [tri_uvs[i] for i in range(len(tri_uvs)) if (int(gy[i]), int(gx[i])) in visited]
    if not pts:
        pts = tri_uvs
    stacked = np.vstack(pts)
    u_px = np.clip(stacked[:, 0] * (tex_w - 1), 0, tex_w - 1)
    v_px = np.clip(stacked[:, 1] * (tex_h - 1), 0, tex_h - 1)
    x0 = int(math.floor(float(np.percentile(u_px, 2))))
    x1 = int(math.ceil(float(np.percentile(u_px, 98))))
    y0 = int(math.floor(float(np.percentile(v_px, 2))))
    y1 = int(math.ceil(float(np.percentile(v_px, 98))))
    if x1 - x0 < 12:
        mid = (x0 + x1) // 2
        x0, x1 = max(0, mid - 8), min(tex_w - 1, mid + 8)
    if y1 - y0 < 12:
        mid = (y0 + y1) // 2
        y0, y1 = max(0, mid - 8), min(tex_h - 1, mid + 8)
    return (max(0, x0), max(0, y0), min(tex_w - 1, x1), min(tex_h - 1, y1))


def _resolve_rect_overlaps(
    rects: dict[str, tuple[int, int, int, int]], tex_w: int, tex_h: int
) -> dict[str, tuple[int, int, int, int]]:
    out = dict(rects)
    keys = list(PAINTKIT_PANELS)
    for _ in range(8):
        changed = False
        for i in range(len(keys)):
            for j in range(i + 1, len(keys)):
                ka, kb = keys[i], keys[j]
                ax0, ay0, ax1, ay1 = out[ka]
                bx0, by0, bx1, by1 = out[kb]
                ox = min(ax1, bx1) - max(ax0, bx0)
                oy = min(ay1, by1) - max(ay0, by0)
                if ox <= 0 or oy <= 0:
                    continue
                changed = True
                if ox <= oy:
                    mid = (max(ax0, bx0) + min(ax1, bx1)) // 2
                    if (ax0 + ax1) <= (bx0 + bx1):
                        ax1, bx0 = max(ax0 + 8, mid - 1), min(bx1 - 8, mid + 1)
                    else:
                        bx1, ax0 = max(bx0 + 8, mid - 1), min(ax1 - 8, mid + 1)
                else:
                    mid = (max(ay0, by0) + min(ay1, by1)) // 2
                    if (ay0 + ay1) <= (by0 + by1):
                        ay1, by0 = max(ay0 + 8, mid - 1), min(by1 - 8, mid + 1)
                    else:
                        by1, ay0 = max(by0 + 8, mid - 1), min(ay1 - 8, mid + 1)
                out[ka] = (max(0, ax0), max(0, ay0), min(tex_w - 1, max(ax0 + 4, ax1)), min(tex_h - 1, max(ay0 + 4, ay1)))
                out[kb] = (max(0, bx0), max(0, by0), min(tex_w - 1, max(bx0 + 4, bx1)), min(tex_h - 1, max(by0 + 4, by1)))
        if not changed:
            break
    return out


def derive_paintkit_regions(modl_bytes: bytes, tex_w: int = 512, tex_h: int = 256) -> dict[str, str]:
    """Derive non-overlapping 512x256 PAINTKIT.RES/regions.val boxes for all 7 paintable panels."""
    mesh = _extract_modl_triangles(modl_bytes)
    pos, uv, tris, groups, is_body = mesh["pos"], mesh["uv"], mesh["tris"], mesh["groups"], mesh["is_body"]
    bmask = is_body if is_body.any() else np.ones(len(tris), dtype=bool)
    btris = tris[bmask]
    bgroups = groups[bmask]

    p0, p1, p2 = pos[btris[:, 0]], pos[btris[:, 1]], pos[btris[:, 2]]
    nrm = np.cross(p1 - p0, p2 - p0)
    areas = 0.5 * np.linalg.norm(nrm, axis=1)
    valid = areas > 1e-6
    btris, bgroups, p0, p1, p2, nrm, areas = (
        btris[valid], bgroups[valid], p0[valid], p1[valid], p2[valid], nrm[valid], areas[valid]
    )
    nrm = nrm / (2.0 * areas[:, None])
    ctrs = (p0 + p1 + p2) / 3.0
    tri_uvs = np.stack([uv[btris[:, 0]], uv[btris[:, 1]], uv[btris[:, 2]]], axis=1)

    xmax = max(0.5, float(np.max(np.abs(ctrs[:, 0]))))
    ymin, ymax = float(ctrs[:, 1].min()), float(ctrs[:, 1].max())
    zmin, zmax = float(ctrs[:, 2].min()), float(ctrs[:, 2].max())
    ylen = max(0.2, ymax - ymin)
    zlen = max(0.5, zmax - zmin)

    buckets: dict[str, list[tuple[np.ndarray, float, float, float]]] = {p: [] for p in PAINTKIT_PANELS}
    group_map = {2: "hood", 11: "decklid", 6: "ldoor", 5: "rdoor", 4: "lfender", 3: "rfender"}

    for i in range(len(btris)):
        g = int(bgroups[i])
        x, y, z = float(ctrs[i, 0]), float(ctrs[i, 1]), float(ctrs[i, 2])
        nx, ny = abs(float(nrm[i, 0])), abs(float(nrm[i, 1]))
        zf = (z - zmin) / zlen
        yf = (y - ymin) / ylen
        panel: str | None = None
        if g in group_map:
            cand = group_map[g]
            if cand in ("hood", "decklid") and ny > 0.25:
                panel = cand
            elif cand in ("ldoor", "lfender") and x < -0.45 * xmax and nx > 0.25:
                panel = cand
            elif cand in ("rdoor", "rfender") and x > 0.45 * xmax and nx > 0.25:
                panel = cand
        if panel is None:
            if ny > 0.42 and abs(x) < 0.78 * xmax:
                if zf > 0.62 and yf > 0.30:
                    panel = "hood"
                elif 0.26 <= zf <= 0.62 and yf > 0.66:
                    panel = "roof"
                elif zf < 0.30 and yf > 0.38:
                    panel = "decklid"
            elif abs(x) > 0.55 * xmax and nx > 0.30:
                if 0.26 <= zf <= 0.64 and yf < 0.86:
                    panel = "rdoor" if x > 0 else "ldoor"
                elif zf > 0.64 and yf < 0.78:
                    panel = "rfender" if x > 0 else "lfender"
        if panel is not None:
            buckets[panel].append((tri_uvs[i], float(areas[i]), z, float(tri_uvs[i, :, 0].mean())))

    # Deterministic fallback slots inside 512x256 if any exotic mesh lacks a panel
    fallback_rects: dict[str, tuple[int, int, int, int]] = {
        "hood": (372, 152, 502, 248),
        "roof": (314, 10, 412, 108),
        "decklid": (420, 10, 506, 114),
        "rdoor": (166, 124, 266, 178),
        "ldoor": (166, 186, 266, 246),
        "rfender": (272, 124, 362, 178),
        "lfender": (272, 186, 362, 246),
    }
    raw_rects: dict[str, tuple[int, int, int, int]] = {}
    for pname in PAINTKIT_PANELS:
        items = buckets[pname]
        rect = _dominant_uv_rect([it[0] for it in items], [it[1] for it in items], tex_w, tex_h) if items else None
        raw_rects[pname] = rect if rect is not None else fallback_rects[pname]

    clean_rects = _resolve_rect_overlaps(raw_rects, tex_w, tex_h)

    def _is_mirrored(pname: str) -> str:
        items = buckets[pname]
        if len(items) < 4:
            return "yes"
        zs = np.array([it[2] for it in items])
        us = np.array([it[3] for it in items])
        corr = float(np.corrcoef(zs, us)[0, 1]) if zs.std() > 1e-4 and us.std() > 1e-4 else -1.0
        return "yes" if corr < 0 else "no"

    result: dict[str, str] = {
        "alt_decklid_cam": "yes",
        "decklid_rot": "0",
        "ldoor_mirror": _is_mirrored("ldoor"),
        "lfender_mirror": _is_mirrored("lfender"),
    }
    for pname in PAINTKIT_PANELS:
        x0, y0, x1, y1 = clean_rects[pname]
        result[pname] = f"{x0}, {y0}, {x1}, {y1}"
    return result


def parse_panel_rect(val: str | tuple[int, int, int, int]) -> tuple[int, int, int, int]:
    if isinstance(val, tuple):
        return (int(val[0]), int(val[1]), int(val[2]), int(val[3]))
    parts = [int(p.strip()) for p in str(val).split(",")]
    if len(parts) != 4:
        raise ValueError(f"Invalid panel rect: {val!r}")
    return (parts[0], parts[1], parts[2], parts[3])


def generate_sweeneys_primer_skins(
    livery_rgba: Image.Image | np.ndarray | bytes,
    panel_uv_rects: dict[str, Any],
) -> dict[str, Image.Image]:
    """Generate 5 clean unpainted Sweeney's Dealership primer skins at 512x256.

    Repaints all 7 body panel UV regions with uniform base-coat primer while preserving
    subtle dark panel seams/crevices and leaving headlights, grille, taillights & trim intact.
    """
    base_img = _as_rgba_image(livery_rgba, (512, 256))
    base_arr = np.asarray(base_img, dtype=np.float32)
    gray_img = base_img.convert("L")
    luma = np.asarray(gray_img, dtype=np.float32) / 255.0
    blur = np.asarray(gray_img.filter(ImageFilter.BoxBlur(3)), dtype=np.float32) / 255.0
    # Keep ONLY true dark fine panel shut-lines/crevices; erase 100% of sponsors & door numbers
    dark_gate = np.clip((0.26 - luma) / 0.26, 0.0, 1.0)
    crevice = np.clip((blur - luma) * 1.6, 0.0, 0.10) * dark_gate
    seam_factor = (1.0 - crevice)[:, :, None]

    # Build smooth mask covering the 7 painted body panels
    mask_u8 = np.zeros((256, 512), dtype=np.uint8)
    for pname in PAINTKIT_PANELS:
        if pname not in panel_uv_rects:
            continue
        x0, y0, x1, y1 = parse_panel_rect(panel_uv_rects[pname])
        x0, x1 = max(0, min(511, x0)), max(0, min(511, x1))
        y0, y1 = max(0, min(255, y0)), max(0, min(255, y1))
        mask_u8[y0 : y1 + 1, x0 : x1 + 1] = 255
    mask_soft = np.asarray(Image.fromarray(mask_u8, mode="L").filter(ImageFilter.BoxBlur(1)), dtype=np.float32) / 255.0
    # Ensure interior of each panel stays 100% clean base-coat with zero bleed-through
    mask = np.where(mask_u8 > 0, 1.0, mask_soft)[:, :, None]

    skins: dict[str, Image.Image] = {}
    for skin_name, rgb in SWEENEYS_PRIMERS.items():
        coat = np.array(rgb, dtype=np.float32)[None, None, :] * seam_factor
        out = base_arr.copy()
        out[:, :, :3] = base_arr[:, :, :3] * (1.0 - mask) + coat * mask
        out[:, :, 3] = 255.0
        skins[skin_name] = Image.fromarray(np.clip(out, 0, 255).astype(np.uint8), mode="RGBA")
    return skins


def build_uibig_canvas(base_primer_rgba: Image.Image | np.ndarray | bytes, template_cvs_bytes: bytes) -> bytes:
    """Encode a 512x256 UIBIG.RES/<Chassis>-o.cvs (CANV, 524,324 bytes) matching retail header."""
    if len(template_cvs_bytes) != CANV_SIZE:
        raise ValueError(f"Expected {CANV_SIZE}-byte CANV template, got {len(template_cvs_bytes)}")
    im = _as_rgba_image(base_primer_rgba, (512, 256))
    payload = im.tobytes()
    if len(payload) != 512 * 256 * 4:
        raise ValueError("Invalid 512x256 RGBA payload size")
    return template_cvs_bytes[:CANV_HEADER_SIZE] + payload


def paint_uibig_canvas_panel(
    cvs_bytes: bytes,
    panel_rect: str | tuple[int, int, int, int],
    rgba: tuple[int, int, int, int],
) -> bytes:
    """Simulate Paint Shop spraying a color onto one panel UV rectangle of a CANV buffer."""
    if len(cvs_bytes) != CANV_SIZE:
        raise ValueError(f"Invalid CANV size: {len(cvs_bytes)}")
    x0, y0, x1, y1 = parse_panel_rect(panel_rect)
    arr = np.frombuffer(cvs_bytes[CANV_HEADER_SIZE:], dtype=np.uint8).reshape(256, 512, 4).copy()
    arr[max(0, y0) : min(256, y1 + 1), max(0, x0) : min(512, x1 + 1)] = np.asarray(rgba, dtype=np.uint8)
    return cvs_bytes[:CANV_HEADER_SIZE] + arr.tobytes()


# ---------------------------------------------------------------------------
# Additive First-Party Archive Registration & Retail Preservation Guards
# ---------------------------------------------------------------------------

def _section_like(table: dict[str, Any], name: str, pairs: list[tuple[str, str]]) -> dict[str, Any]:
    tmpl = table["sections"][0]
    return {
        "name": name,
        "parent": "",
        "sec_vtable": tmpl["sec_vtable"],
        "tool_ptr": tmpl.get("tool_ptr", 0),
        "pairs": list(pairs),
    }


def format_profile_summary(timings_ms: dict[str, float]) -> str:
    total = float(timings_ms.get("total_ms", sum(v for k, v in timings_ms.items() if k != "total_ms")))
    ordered_stages = ("scan", "paintkit_uv", "sweeneys_primer", "render_icons", "uibig_canvas", "pack_res")
    parts = [f"{s} {timings_ms[s]:.0f}ms" for s in ordered_stages if s in timings_ms]
    if not parts:
        return f"Tracy: {total:.0f} ms"
    return f"Tracy: {total:.0f} ms ({' · '.join(parts)})"


def _allocate_dealer_ids(used: set[str], count: int = 5, start: int = 930) -> list[int]:
    ids: list[int] = []
    cur = start
    while len(ids) < count:
        if str(cur) not in used:
            ids.append(cur)
            used.add(str(cur))
        cur += 1
    return ids


def register_additive_firstparty_vehicle(
    *,
    eod_data: Path | str,
    output_data: Path | str,
    chassis: str,
    livery_id: int,
    modl_bytes: bytes,
    livery_rgba: Image.Image | np.ndarray | bytes,
    driver_name: str = "",
    display_number: str = "",
    stats: tuple[int, int, int] = (9, 8, 7),
    price: int = 16000,
    desc: str = "",
    enable_icons: bool = True,
    enable_sweeneys: bool = True,
    enable_paintkit: bool = True,
    livery_tex_blob: bytes | None = None,
    existing_res_dir: Path | str | None = None,
    timings: dict[str, float] | None = None,
) -> dict[str, Any]:
    """Build additive COMMON/CT/UICAR/PAINTKIT/UIBIG archives with zero retail overwrite."""
    t_all = time.perf_counter_ns()
    stage_ms: dict[str, float] = timings if timings is not None else {}

    retail_res = Path(eod_data) / "RES"
    src_res = Path(existing_res_dir) if existing_res_dir is not None else retail_res
    out_res = Path(output_data) / "RES"
    out_res.mkdir(parents=True, exist_ok=True)

    livery_img = _as_rgba_image(livery_rgba, (512, 256))

    # 1. Derive PAINTKIT 7-panel UV rectangles from MODL geometry
    regions: dict[str, str] = {}
    with span("paintkit_uv", timings=stage_ms):
        regions = derive_paintkit_regions(modl_bytes, tex_w=512, tex_h=256)

    # 2. Generate 5 clean unpainted Sweeney's Dealership primer skins
    primers: dict[str, Image.Image] = {}
    if enable_sweeneys or enable_paintkit:
        with span("sweeneys_primer", timings=stage_ms):
            primers = generate_sweeneys_primer_skins(livery_img, regions)

    # 3. Render 256x256 3D alpha-masked uicar_ & uicars_ icons (cached projection)
    driver_uicar_pair: tuple[bytes, bytes] | None = None
    driver_uicars_pair: tuple[bytes, bytes] | None = None
    primer_icon_pairs: dict[str, tuple[tuple[bytes, bytes], tuple[bytes, bytes]]] = {}
    if enable_icons:
        with span("render_icons", timings=stage_ms):
            mesh = _extract_modl_triangles(modl_bytes)
            buf_showroom = _rasterize_buffers(mesh, size=256, yaw_deg=35.0, pitch_deg=18.0, fill_frac=0.80)
            buf_hud = _rasterize_buffers(mesh, size=256, yaw_deg=42.0, pitch_deg=15.0, fill_frac=0.92)
            cached = (buf_showroom, buf_hud)
            uicar_im, uicars_im = render_vehicle_ui_icons(modl_bytes, livery_img, size=256, _cached_buffers=cached)
            driver_uicar_pair = encode_ui_icon_pair(uicar_im)
            driver_uicars_pair = encode_ui_icon_pair(uicars_im)
            if enable_sweeneys:
                for skin_name, skin_img in primers.items():
                    s_big, s_small = render_vehicle_ui_icons(modl_bytes, skin_img, size=256, _cached_buffers=cached)
                    primer_icon_pairs[skin_name] = (encode_ui_icon_pair(s_big), encode_ui_icon_pair(s_small))

    # 4. Synthesize UIBIG.RES/<Chassis>-o.cvs (524,324-byte CANV)
    uibig_arc = FastRst0Archive(src_res / "UIBIG.RES")
    uibig_entry_name = f"{chassis}-o.cvs"
    cvs_blob: bytes | None = None
    if enable_paintkit:
        with span("uibig_canvas", timings=stage_ms):
            template_cvs = uibig_arc.read("Algar-o.cvs")
            base_primer = primers.get("primer_gray", livery_img)
            cvs_blob = build_uibig_canvas(base_primer, template_cvs)

    # 5. Patch & pack COMMON, CT, UICAR, PAINTKIT, UIBIG additively with retail guards
    dealer_ids: list[int] = []
    retail_preserved: dict[str, int] = {}
    with span("pack_res", timings=stage_ms):
        common_arc = FastRst0Archive(src_res / "COMMON.RES")
        ct_arc = FastRst0Archive(src_res / "CT.RES")
        uicar_arc = FastRst0Archive(src_res / "UICAR.RES")
        paintkit_arc = FastRst0Archive(src_res / "PAINTKIT.RES")

        # Snapshot retail baseline tables for strict zero-overwrite assertions
        retail_common = FastRst0Archive(retail_res / "COMMON.RES") if src_res != retail_res else common_arc
        retail_tables = {
            n: valt.parse_binary_valt(retail_common.read(n))
            for n in ("carclass.val", "car_stats.val", "buycar.val", "dirt.val")
        }
        retail_buycar_cars = [s for s in retail_tables["buycar.val"]["sections"] if s["name"] == "car"]
        retail_buycar_vehs = [s for s in retail_tables["buycar.val"]["sections"] if s["name"] == "vehicle"]
        retail_regions = valt.parse_binary_valt(
            (FastRst0Archive(retail_res / "PAINTKIT.RES") if src_res != retail_res else paintkit_arc).read("regions.val")
        )
        retail_uicar_count = sum(
            1 for e in (FastRst0Archive(retail_res / "UICAR.RES") if src_res != retail_res else uicar_arc).entries
            if e["name"].lower().startswith("uicar_") and e["name"].lower().endswith(".tm0")
        )

        tables = {
            n: valt.parse_binary_valt(common_arc.read(n))
            for n in ("carclass.val", "car_stats.val", "buycar.val", "dirt.val")
        }

        # 5a. Additive NASCAR Class 4 ([Nascar] + [$0004$chassis]) without touching retail classes
        cc = tables["carclass.val"]
        if not any(s["name"] == "Nascar" for s in cc["sections"]):
            cc["sections"].append(
                _section_like(cc, "Nascar", [("@chassis", "$0004$chassis"), ("reference_chassis", chassis)])
            )
            cc["root_count"] = int(cc.get("root_count", len(cc["sections"]))) + 1
        cls4 = next((s for s in cc["sections"] if s["name"] == "$0004$chassis"), None)
        if cls4 is None:
            cls4 = _section_like(cc, "$0004$chassis", [(chassis, "")])
            cc["sections"].append(cls4)
        elif not any(k.casefold() == chassis.casefold() for k, _ in cls4["pairs"]):
            cls4["pairs"].append((chassis, ""))

        # 5b. car_stats.val
        cs = tables["car_stats.val"]
        known_stats = {dict(s["pairs"]).get("chassis", "").casefold() for s in cs["sections"]}
        if chassis.casefold() not in known_stats:
            cs["sections"].append(
                _section_like(
                    cs,
                    "carstats",
                    [
                        ("chassis", chassis),
                        ("speed", str(stats[0])),
                        ("handling", str(stats[1])),
                        ("durability", str(stats[2])),
                    ],
                )
            )

        # Collect all used numeric IDs across retail & additive tables to guarantee zero collisions
        used_ids: set[str] = {
            str(dict(s["pairs"])["number"])
            for tbl_name in ("dirt.val", "buycar.val")
            for s in tables[tbl_name]["sections"]
            if "number" in dict(s["pairs"])
        }
        used_ids.update(
            m[1]
            for e in ct_arc.entries
            if (m := re.fullmatch(r"(\d+)\.(?:tex|tm0)", e["name"], re.I))
        )
        used_ids.add(str(livery_id))

        # 5c. dirt.val driver entry (additive)
        dt = tables["dirt.val"]
        existing_dirt_nums = {dict(s["pairs"]).get("number") for s in dt["sections"]}
        if str(livery_id) not in existing_dirt_nums:
            dt["sections"].append(
                _section_like(
                    dt,
                    "Driver",
                    [
                        ("body_color", "0xFFFFFFFF"),
                        ("chassis", chassis),
                        ("display_number", str(display_number or livery_id)),
                        ("name", driver_name or f"{chassis} #{display_number or livery_id}"),
                        ("number", str(livery_id)),
                        ("rank", "101"),
                    ],
                )
            )

        # 5d. Sweeney's Dealership 5 unpainted primer skins in buycar.val + CT.RES
        bc = tables["buycar.val"]
        if enable_sweeneys:
            existing_buy_chassis = {
                dict(s["pairs"]).get("chassis", "").casefold()
                for s in bc["sections"]
                if s["name"] == "car"
            }
            if chassis.casefold() not in existing_buy_chassis:
                dealer_ids = _allocate_dealer_ids(used_ids, count=len(SWEENEYS_PRIMERS), start=930)
                bc["sections"].append(
                    _section_like(
                        bc,
                        "car",
                        [
                            ("chassis", chassis),
                            ("desc", desc or f"Unpainted {chassis} NASCAR chassis ready for Sweeney's Paint Shop."),
                            ("price", str(price)),
                        ],
                    )
                )
                for did, (skin_name, rgb) in zip(dealer_ids, SWEENEYS_PRIMERS.items()):
                    bc["sections"].append(
                        _section_like(
                            bc,
                            "vehicle",
                            [
                                ("body_color", f"{rgb[0]}, {rgb[1]}, {rgb[2]}"),
                                ("chassis", chassis),
                                ("group_1", "20"),
                                ("number", str(did)),
                            ],
                        )
                    )
                    tm0_b, tex_b = encode_livery_tex_pair(primers[skin_name])
                    ct_arc.put(f"{did}.tm0", tm0_b, version=10, fourcc="TEX", extractable=True, replace=True)
                    ct_arc.put(f"{did}.tex", tex_b, version=10, fourcc="TEX", extractable=False, replace=True)
                    if enable_icons and skin_name in primer_icon_pairs:
                        (u_tm0, u_tex), (us_tm0, us_tex) = primer_icon_pairs[skin_name]
                        uicar_arc.put(f"uicar_{did}.tm0", u_tm0, version=10, fourcc="TEX", extractable=True, replace=True)
                        uicar_arc.put(f"uicar_{did}.tex", u_tex, version=10, fourcc="TEX", extractable=False, replace=True)
                        uicar_arc.put(f"uicars_{did}.tm0", us_tm0, version=10, fourcc="TEX", extractable=True, replace=True)
                        uicar_arc.put(f"uicars_{did}.tex", us_tex, version=10, fourcc="TEX", extractable=False, replace=True)

        # 5e. Driver livery in CT.RES + uicar_/uicars_ in UICAR.RES
        if livery_tex_blob is not None:
            from import_nascar_cars import drop_top_mip

            drv_tm0 = livery_tex_blob
            drv_tex = drop_top_mip(livery_tex_blob)
        else:
            drv_tm0, drv_tex = encode_livery_tex_pair(livery_img)
        ct_arc.put(f"{livery_id}.tm0", drv_tm0, version=10, fourcc="TEX", extractable=True, replace=True)
        ct_arc.put(f"{livery_id}.tex", drv_tex, version=10, fourcc="TEX", extractable=False, replace=True)

        if enable_icons and driver_uicar_pair is not None and driver_uicars_pair is not None:
            uicar_arc.put(f"uicar_{livery_id}.tm0", driver_uicar_pair[0], version=10, fourcc="TEX", extractable=True, replace=True)
            uicar_arc.put(f"uicar_{livery_id}.tex", driver_uicar_pair[1], version=10, fourcc="TEX", extractable=False, replace=True)
            uicar_arc.put(f"uicars_{livery_id}.tm0", driver_uicars_pair[0], version=10, fourcc="TEX", extractable=True, replace=True)
            uicar_arc.put(f"uicars_{livery_id}.tex", driver_uicars_pair[1], version=10, fourcc="TEX", extractable=False, replace=True)

        # 5f. PAINTKIT.RES/regions.val + UIBIG.RES/<Chassis>-o.cvs
        if enable_paintkit:
            reg_tbl = valt.parse_binary_valt(paintkit_arc.read("regions.val"))
            if not any(s["name"].casefold() == chassis.casefold() for s in reg_tbl["sections"]):
                reg_tbl["sections"].append(_section_like(reg_tbl, chassis, sorted(regions.items())))
            # Assert all 26 retail regions sections remain untouched at prefix
            assert reg_tbl["sections"][: len(retail_regions["sections"])] == retail_regions["sections"], (
                "Retail PAINTKIT regions.val entries were modified!"
            )
            paintkit_arc.put("regions.val", valt.compile_binary_valt(reg_tbl), version=11, fourcc="VALT", replace=True)
            assert cvs_blob is not None and len(cvs_blob) == CANV_SIZE
            uibig_arc.put(uibig_entry_name, cvs_blob, version=1, fourcc="CANV", extractable=False, replace=True)

        # Strict retail zero-overwrite verification across all 4 COMMON tables & 5 RES archives
        for tbl_name, retail_tbl in retail_tables.items():
            cur_secs = tables[tbl_name]["sections"]
            ret_secs = retail_tbl["sections"]
            assert cur_secs[: len(ret_secs)] == ret_secs, f"Retail {tbl_name} sections modified!"
        assert len(retail_buycar_cars) >= 26, f"Expected >=26 retail buycar chassis, got {len(retail_buycar_cars)}"
        assert len(retail_buycar_vehs) >= 130, f"Expected >=130 retail buycar vehicles, got {len(retail_buycar_vehs)}"
        assert retail_uicar_count >= 143, f"Expected >=143 retail uicar entries, got {retail_uicar_count}"

        for tbl_name, tbl in tables.items():
            common_arc.put(tbl_name, valt.compile_binary_valt(tbl), version=11, fourcc="VALT", replace=True)

        common_arc.assert_preserved(allowed=set(tables.keys()))
        ct_arc.assert_preserved(allowed={f"{livery_id}.tm0", f"{livery_id}.tex"})
        uicar_arc.assert_preserved()
        paintkit_arc.assert_preserved(allowed={"regions.val"} if enable_paintkit else ())
        uibig_arc.assert_preserved()

        common_arc.pack(out_res / "COMMON.RES")
        ct_arc.pack(out_res / "CT.RES")
        uicar_arc.pack(out_res / "UICAR.RES")
        paintkit_arc.pack(out_res / "PAINTKIT.RES")
        uibig_arc.pack(out_res / "UIBIG.RES")

        retail_preserved = {
            "buycar_chassis": len(retail_buycar_cars),
            "buycar_vehicles": len(retail_buycar_vehs),
            "uicar_entries": retail_uicar_count,
            "paintkit_chassis": len(retail_regions["sections"]),
            "uibig_entries": len(uibig_arc.original_meta),
        }

    stage_ms["total_ms"] = (time.perf_counter_ns() - t_all) / 1_000_000.0
    return {
        "chassis": chassis,
        "livery_id": livery_id,
        "dealer_ids": dealer_ids,
        "regions": regions,
        "uibig_entry": uibig_entry_name if enable_paintkit else "",
        "retail_preserved": retail_preserved,
        "timings_ms": dict(stage_ms),
        "profile_summary": format_profile_summary(stage_ms),
    }


def apply_firstparty_to_overlay(
    *,
    eod_data: Path | str,
    overlay_dir: Path | str,
    chassis: str,
    livery_id: int,
    driver_name: str = "",
    display_number: str = "",
    enable_icons: bool = True,
    enable_sweeneys: bool = True,
    enable_paintkit: bool = True,
) -> dict[str, Any]:
    """Upgrade an imported vehicle overlay directory with additive first-party assets."""
    from create_custom_car import car_archive_filename

    ov_data = Path(overlay_dir) / "DATA"
    car_arc = FastRst0Archive(ov_data / "CAR" / car_archive_filename(chassis))
    modl_bytes = car_arc.read("Low1.mod")
    ct_arc = FastRst0Archive(ov_data / "RES" / "CT.RES")
    livery_blob = ct_arc.read(f"{livery_id}.tm0")
    w, h, rgba, _ = tex_mod.decompile_tex(livery_blob)
    livery_img = Image.frombytes("RGBA", (w, h), rgba)
    return register_additive_firstparty_vehicle(
        eod_data=eod_data,
        output_data=ov_data,
        chassis=chassis,
        livery_id=livery_id,
        modl_bytes=modl_bytes,
        livery_rgba=livery_img,
        driver_name=driver_name,
        display_number=display_number,
        enable_icons=enable_icons,
        enable_sweeneys=enable_sweeneys,
        enable_paintkit=enable_paintkit,
        livery_tex_blob=livery_blob,
    )
