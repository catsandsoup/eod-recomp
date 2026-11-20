"""Qt native child and geometry bridge for the in-process Plume viewport."""
from __future__ import annotations

import ctypes
from pathlib import Path

import numpy as np
from PySide6.QtCore import Qt
from PySide6.QtWidgets import QWidget

from project_state import build_eodhtx_from_rgba

VERTEX = np.dtype([("xyz", "<f4", 3), ("uv", "<f4", 2), ("normal", "i1", 4), ("color", "u1", 4)])
BATCH = np.dtype([("first", "<u4"), ("count", "<u4"), ("slot", "<u4"), ("center", "<f4", 3)])
ALPHA_KIND = {"opaque": 0, "cutout": 1, "blend": 2}


def pack_world(world, layers, slots):
    """Bake visible placements, batching solid surfaces and keeping translucent parts separate."""
    vertices, solid, blended = [], {}, []
    base = 0
    for obj in world.objects:
        if not layers.get(obj.category, True) or not world.object_visible_in_event_template(obj):
            continue
        model = world.models.get(obj.model_key)
        if model is None:
            continue
        transform = obj.world_matrix()
        for part in model.parts:
            if not len(part.indices):
                continue
            src = part.vertices
            vertex = np.zeros(len(src), dtype=VERTEX)
            vertex["xyz"] = src[:, :3] @ transform[:3, :3].T + transform[:3, 3]
            vertex["uv"] = src[:, 6:8]
            vertex["color"] = (168, 176, 189, 255)  # the shared untextured fallback
            texture = world.textures.get(part.texture)
            slot = slots.setdefault(part.texture, len(slots) + 1) if texture is not None else 0
            vertices.append(vertex)
            indices = np.asarray(part.indices, dtype="<u4").reshape(-1) + base
            if texture is not None and texture.alpha_kind == "blend":
                blended.append((slot, indices, tuple(vertex["xyz"].mean(axis=0))))
            else:
                solid.setdefault(slot, []).append(indices)
            base += len(src)
    if not vertices:
        return np.empty(0, dtype=VERTEX), np.empty(0, dtype="<u4"), np.empty(0, dtype=BATCH)
    index_parts, records, offset = [], [], 0
    for slot, parts in solid.items():
        count = sum(len(indices) for indices in parts)
        records.append((offset, count, slot, (0, 0, 0)))
        index_parts.extend(parts)
        offset += count
    for slot, indices, center in blended:
        records.append((offset, len(indices), slot, center))
        index_parts.append(indices)
        offset += len(indices)
    return np.concatenate(vertices), np.concatenate(index_parts), np.array(records, dtype=BATCH)


def clip_matrix(view_proj):
    """OpenGL LH camera to Vulkan's top-left viewport and reversed [0, 1] depth."""
    clip = np.array(view_proj, dtype=np.float32, order="C", copy=True)
    clip[1] = -clip[1]
    clip[2] = (clip[3] - clip[2]) * 0.5
    return clip


class PlumeViewport(QWidget):
    def __init__(self, parent: QWidget, module_path: str):
        super().__init__(parent)
        self.lib = ctypes.CDLL(str(Path(module_path).expanduser().resolve(strict=True)))
        ptr, u32 = ctypes.c_void_p, ctypes.c_uint32
        self.lib.eod_plume_view_create.argtypes = [ptr]
        self.lib.eod_plume_view_create.restype = ptr
        self.lib.eod_plume_view_destroy.argtypes = [ptr]
        self.lib.eod_plume_view_destroy.restype = None
        self.lib.eod_plume_view_mesh.argtypes = [ptr, ptr, u32, ptr, u32, ptr, u32]
        self.lib.eod_plume_view_mesh.restype = ctypes.c_bool
        self.lib.eod_plume_view_texture.argtypes = [ptr, u32, u32, u32, u32, ctypes.c_char_p, ctypes.c_uint64]
        self.lib.eod_plume_view_texture.restype = ctypes.c_bool
        self.lib.eod_plume_view_clear_textures.argtypes = [ptr]
        self.lib.eod_plume_view_clear_textures.restype = None
        self.lib.eod_plume_view_draw.argtypes = [ptr, u32, u32, ptr, ptr, ptr]
        self.lib.eod_plume_view_draw.restype = ctypes.c_int
        self.handle = None
        self.triangles = 0
        self.presented = 0
        self._world = None
        self._slots = {}
        self._resident = {}
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setAttribute(Qt.WidgetAttribute.WA_DontCreateNativeAncestors)
        self.setAttribute(Qt.WidgetAttribute.WA_NativeWindow)
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.setAttribute(Qt.WidgetAttribute.WA_NoSystemBackground)

    def paintEvent(self, event):
        # Plume presents to the platform surface directly.
        pass

    def open_surface(self):
        if not self.handle:
            self.show()
            self.handle = self.lib.eod_plume_view_create(int(self.winId()))
            if not self.handle:
                raise RuntimeError("could not create a Vulkan child surface")

    def close_surface(self):
        if self.handle:
            self.lib.eod_plume_view_destroy(self.handle)
            self.handle = None
        self._world = None
        self._slots.clear()
        self._resident.clear()
        self.hide()

    def _upload_texture(self, name, texture):
        cached = self._resident.get(name)
        source_size = (texture.width, texture.height)
        retail_size = (texture.orig_width, texture.orig_height) if texture.is_replaced else source_size
        params = (source_size, retail_size, texture.alpha_kind)
        if cached and cached[0] is texture.rgba and cached[1:] == params:
            return
        encoded, width, height = build_eodhtx_from_rgba(texture.rgba, *source_size, *retail_size)
        # The native uploader takes the tightly packed RGBA levels after the EODHTX1 header.
        pixels = encoded[16:]
        if not self.lib.eod_plume_view_texture(self.handle, self._slots[name], width, height,
                                               ALPHA_KIND[texture.alpha_kind], pixels, len(pixels)):
            raise RuntimeError(f"could not upload track texture {name!r}")
        self._resident[name] = (texture.rgba, *params)

    def update_textures(self, world, names):
        """Update the changed descriptors, requesting geometry regrouping for alpha class changes."""
        if world is not self._world:
            return True
        regroup = False
        for name in names:
            texture = world.textures.get(name)
            cached = self._resident.get(name)
            if texture is None or cached is None:
                regroup = True
                continue
            regroup |= cached[-1] != texture.alpha_kind
            self._upload_texture(name, texture)
        return regroup

    def upload(self, world, layers):
        if world is not self._world:
            self.lib.eod_plume_view_clear_textures(self.handle)
            self._world = world
            self._slots.clear()
            self._resident.clear()
        vertices, indices, batches = pack_world(world, layers, self._slots)
        used_slots = set(batches["slot"])
        for name, slot in self._slots.items():
            if slot in used_slots:
                self._upload_texture(name, world.textures[name])
        if not self.lib.eod_plume_view_mesh(self.handle, vertices.ctypes.data, len(vertices),
                                            indices.ctypes.data, len(indices), batches.ctypes.data, len(batches)):
            raise RuntimeError("could not upload the current track geometry")
        self.triangles = len(indices) // 3

    def draw(self, view_proj, env):
        matrix = clip_matrix(view_proj)
        fog = np.array((*env.fog_color, 0, env.fog_start, env.fog_end, env.fog_density, env.ambient_intensity),
                       dtype=np.float32)
        clear = np.array(env.sky_horizon, dtype=np.float32)
        dpr = self.devicePixelRatioF()
        width, height = round(self.width() * dpr), round(self.height() * dpr)
        result = self.lib.eod_plume_view_draw(self.handle, width, height, matrix.ctypes.data,
                                              fog.ctypes.data, clear.ctypes.data)
        if result < 0:
            raise RuntimeError(f"native presentation failed at {width}×{height}")
        self.presented += result
        return result > 0
