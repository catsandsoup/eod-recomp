"""Fast 3D studio thumbnail renderer for EOD World Editor Explorer model cards.

Renders isometric 3/4 shaded 3D previews of decoded WorldModel geometry using
numpy + Pillow painter's algorithm with Lambert lighting and decoded EOD texture
colors. Caches QPixmaps in memory so scrolling hundreds of models stays 60 FPS.
"""

from __future__ import annotations

import math
from typing import TYPE_CHECKING

import numpy as np
from PIL import Image, ImageDraw
from PySide6.QtGui import QImage, QPixmap

if TYPE_CHECKING:
    from world_loader import TextureAsset, WorldModel


THUMB_W = 112
THUMB_H = 80
MAX_THUMB_TRIS = 500


class ThumbnailCache:
    """In-memory cache of rendered 3D model preview QPixmaps."""

    def __init__(self) -> None:
        self._cache: dict[str, QPixmap] = {}

    def clear(self) -> None:
        self._cache.clear()

    def get(
        self,
        model: WorldModel,
        textures: dict[str, TextureAsset],
    ) -> QPixmap:
        cached = self._cache.get(model.key)
        if cached is not None:
            return cached
        pm = render_model_thumbnail(model, textures)
        self._cache[model.key] = pm
        return pm


def _pil_to_qimage(img: Image.Image) -> QImage:
    rgba = img.convert("RGBA")
    data = rgba.tobytes("raw", "RGBA")
    qimg = QImage(data, rgba.width, rgba.height, rgba.width * 4, QImage.Format.Format_RGBA8888)
    return qimg.copy()


def _pil_to_qpixmap(img: Image.Image) -> QPixmap:
    return QPixmap.fromImage(_pil_to_qimage(img))


def render_model_thumbnail_qimage(
    model: WorldModel,
    textures: dict[str, TextureAsset],
    width: int = THUMB_W,
    height: int = THUMB_H,
) -> QImage:
    """Thread-safe off-thread 3/4 shaded 3D preview renderer returning a QImage."""
    # Supersample 2x for crisp edges then downsample
    sw, sh = width * 2, height * 2
    bg_tint = (26, 23, 30, 255) if model.category == "car-wreck" else (23, 25, 31, 255)
    img = Image.new("RGBA", (sw, sh), bg_tint)
    draw = ImageDraw.Draw(img)

    # Subtle radial-like studio gradient backdrop
    for row in range(0, sh, 4):
        t = row / max(1, sh)
        c = int(34 - t * 14)
        if model.category == "car-wreck":
            draw.rectangle([0, row, sw, row + 4], fill=(c + 5, c + 1, c + 5, 255))
        else:
            draw.rectangle([0, row, sw, row + 4], fill=(c, c + 2, c + 7, 255))

    # Subtle ground horizon line
    draw.line([(0, int(sh * 0.78)), (sw, int(sh * 0.78))], fill=(46, 50, 60, 255), width=2)

    if not model.parts or model.tri_count == 0:
        return _pil_to_qimage(img.resize((width, height), Image.Resampling.LANCZOS))

    center = (model.bbox_min + model.bbox_max) * 0.5
    ext = model.bbox_max - model.bbox_min
    radius = float(np.linalg.norm(ext)) * 0.5
    if radius < 1e-4:
        radius = 1.0

    # Camera rotation: yaw 38 deg, pitch 24 deg
    yaw = math.radians(38.0)
    pitch = math.radians(24.0)
    cy, sy = math.cos(yaw), math.sin(yaw)
    cp, sp = math.cos(pitch), math.sin(pitch)

    # Light direction
    ldir = np.array([0.45, 0.80, 0.38], dtype=np.float32)
    ldir /= np.linalg.norm(ldir)

    # Collect triangles across parts (subsampled if huge)
    step = max(1, model.tri_count // MAX_THUMB_TRIS)
    tri_records: list[tuple[float, tuple[float, float], tuple[float, float], tuple[float, float], tuple[int, int, int]]] = []

    dist = radius * 2.55
    focal = min(sw, sh) * 1.12
    cx_img, cy_img = sw * 0.5, sh * 0.52

    for part in model.parts:
        tex_asset = textures.get(part.texture)
        if tex_asset is not None:
            base_rgb = tex_asset.average_rgb
        elif part.texture:
            h = hash(part.texture)
            base_rgb = (100 + (h & 0x5F), 110 + ((h >> 6) & 0x5F), 120 + ((h >> 12) & 0x5F))
        else:
            base_rgb = (165, 172, 184)

        verts = part.vertices[:, 0:3] - center
        tris = part.indices.reshape(-1, 3)[::step]
        if len(tris) == 0:
            continue

        # Transform vertices to camera space
        x = verts[:, 0]
        y = verts[:, 1]
        z = verts[:, 2]
        x1 = cy * x - sy * z
        z1 = sy * x + cy * z
        y2 = cp * y - sp * z1
        z2 = sp * y + cp * z1
        zc = dist - z2
        zc = np.maximum(zc, 0.05)

        sx_arr = cx_img + focal * (x1 / zc)
        sy_arr = cy_img - focal * (y2 / zc)

        # Triangle normals & depth
        v0 = verts[tris[:, 0]]
        v1 = verts[tris[:, 1]]
        v2 = verts[tris[:, 2]]
        fn = np.cross(v1 - v0, v2 - v0)
        fn_len = np.linalg.norm(fn, axis=1)
        fn_len = np.maximum(fn_len, 1e-6)
        ndotl = np.abs((fn @ ldir) / fn_len)
        shades = 0.34 + 0.66 * ndotl

        depths = (zc[tris[:, 0]] + zc[tris[:, 1]] + zc[tris[:, 2]]) * (1.0 / 3.0)

        r0, g0, b0 = base_rgb
        for idx_t in range(len(tris)):
            i0, i1, i2 = int(tris[idx_t, 0]), int(tris[idx_t, 1]), int(tris[idx_t, 2])
            sh_val = float(shades[idx_t])
            col = (
                min(255, int(r0 * sh_val)),
                min(255, int(g0 * sh_val)),
                min(255, int(b0 * sh_val)),
            )
            tri_records.append(
                (
                    float(depths[idx_t]),
                    (float(sx_arr[i0]), float(sy_arr[i0])),
                    (float(sx_arr[i1]), float(sy_arr[i1])),
                    (float(sx_arr[i2]), float(sy_arr[i2])),
                    col,
                )
            )

    # Painter's sort: farthest first
    tri_records.sort(key=lambda t: t[0], reverse=True)
    for _d, p0, p1, p2, col in tri_records:
        draw.polygon([p0, p1, p2], fill=(col[0], col[1], col[2], 255))

    down = img.resize((width, height), Image.Resampling.LANCZOS)
    return _pil_to_qimage(down)


def render_model_thumbnail(
    model: WorldModel,
    textures: dict[str, TextureAsset],
    width: int = THUMB_W,
    height: int = THUMB_H,
) -> QPixmap:
    """Render a crisp 3/4 shaded 3D preview of `model` onto a dark studio card (main thread)."""
    return QPixmap.fromImage(render_model_thumbnail_qimage(model, textures, width, height))
