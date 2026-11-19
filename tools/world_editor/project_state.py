"""Project state, QUndoStack commands, .eodworld.json overlay serialization,
Blender OBJ export, and non-destructive Mod Layer write-back (Ctrl+Shift+S).

Retail .TRK archives are NEVER modified in place.
"""

from __future__ import annotations

import hashlib
import json
import math
import os
import re
import shutil
import struct
import tempfile
import time
from pathlib import Path
from typing import Any, Callable

import numpy as np
from PIL import Image
from PySide6.QtGui import QUndoCommand, QUndoStack

from world_loader import (
    CustomRaceConfig,
    EnvironmentState,
    RoadEdge,
    RoadGraph,
    RoadNode,
    TrackWorld,
    WorldMarker,
    WorldObject,
    compose_transform,
    parse_bppt,
    query_bppt_plane,
)
import modding_backend as mb
import rst0
import texture_identity as tex_id
import valt
import vehicle_export as veh_exp
import vif_patch
import world_collision as wc
import world_export as we
import world_graf as wg
from formats import ilin as ilin_fmt
from rst0 import eod_crc
from tex import write_png
from texture_preview import decode_preview


# ---------------------------------------------------------------------------
# Undo / Redo Commands
# ---------------------------------------------------------------------------
class TransformObjectCommand(QUndoCommand):
    def __init__(
        self,
        obj: WorldObject,
        old_pos: tuple[float, float, float],
        old_rot: tuple[float, float, float],
        old_scale: tuple[float, float, float],
        new_pos: tuple[float, float, float],
        new_rot: tuple[float, float, float],
        new_scale: tuple[float, float, float],
        on_change: Callable[[], None],
        label: str = "Transform object",
    ):
        super().__init__(label)
        self.obj = obj
        self.old_state = (tuple(old_pos), tuple(old_rot), tuple(old_scale), obj.written)
        self.new_state = (tuple(new_pos), tuple(new_rot), tuple(new_scale), False)
        self.on_change = on_change

    def redo(self) -> None:
        self.obj.position, self.obj.rotation_hpr, self.obj.scale, self.obj.written = self.new_state
        self.on_change()

    def undo(self) -> None:
        self.obj.position, self.obj.rotation_hpr, self.obj.scale, self.obj.written = self.old_state
        self.on_change()


class MultiTransformObjectsCommand(QUndoCommand):
    """Single undoable command moving/rotating/scaling multiple selected objects together."""

    def __init__(
        self,
        entries: list[
            tuple[
                WorldObject,
                tuple[float, float, float],
                tuple[float, float, float],
                tuple[float, float, float],
                tuple[float, float, float],
                tuple[float, float, float],
                tuple[float, float, float],
            ]
        ],
        on_change: Callable[[], None],
        label: str = "Transform selection",
    ):
        super().__init__(label)
        self.entries = [
            (obj, (tuple(op), tuple(or_), tuple(os_), obj.written), (tuple(np_), tuple(nr), tuple(ns), False))
            for obj, op, or_, os_, np_, nr, ns in entries
        ]
        self.on_change = on_change

    def redo(self) -> None:
        for obj, _old, new in self.entries:
            obj.position, obj.rotation_hpr, obj.scale, obj.written = new
        self.on_change()

    def undo(self) -> None:
        for obj, old, _new in self.entries:
            obj.position, obj.rotation_hpr, obj.scale, obj.written = old
        self.on_change()


class PlaceObjectCommand(QUndoCommand):
    def __init__(self, world: TrackWorld, obj: WorldObject, on_change: Callable[[], None]):
        super().__init__(f"Place {obj.name}")
        self.world = world
        self.obj = obj
        self.on_change = on_change

    def redo(self) -> None:
        if self.obj not in self.world.objects:
            self.world.objects.append(self.obj)
        self.obj.deleted = False
        self.on_change()

    def undo(self) -> None:
        self.obj.deleted = True
        self.on_change()


class DeleteObjectCommand(QUndoCommand):
    def __init__(self, obj: WorldObject, on_change: Callable[[], None]):
        super().__init__(f"Delete {obj.name}")
        self.obj = obj
        self.old_written = obj.written
        self.on_change = on_change

    def redo(self) -> None:
        self.obj.deleted = True
        self.obj.written = False
        self.on_change()

    def undo(self) -> None:
        self.obj.deleted = False
        self.obj.written = self.old_written
        self.on_change()


class MultiDeleteObjectsCommand(QUndoCommand):
    """Single undoable command deleting multiple objects + LOD twins + flattening overlapping SOBL barriers."""

    def __init__(
        self,
        world: TrackWorld,
        objects: list[WorldObject],
        barrier_indices: list[int] | None = None,
        on_change: Callable[[], None] = lambda: None,
        label: str | None = None,
    ):
        uniq_objs: list[WorldObject] = []
        seen_ids: set[str] = set()
        for o in objects:
            if o.obj_id not in seen_ids and not o.deleted:
                seen_ids.add(o.obj_id)
                uniq_objs.append(o)
        b_list = sorted(set(int(i) for i in (barrier_indices or [])))
        desc = label or f"Delete {len(uniq_objs)} object(s)" + (f" + {len(b_list)} barrier(s)" if b_list else "")
        super().__init__(desc)
        self.world = world
        self.objects = [(o, o.written) for o in uniq_objs]
        self.barrier_indices = b_list
        self._old_barriers: dict[int, dict[str, Any] | None] = {
            idx: dict(world.collision.sobl_edits[idx]) if idx in world.collision.sobl_edits else None
            for idx in b_list
        }
        self._new_barriers: dict[int, dict[str, Any]] = {}
        self.on_change = on_change

    def redo(self) -> None:
        for o, _w in self.objects:
            o.deleted = True
            o.written = False
        if self.barrier_indices:
            for idx in self.barrier_indices:
                if idx in self._new_barriers:
                    self.world.collision.restore_barrier_state(idx, self._new_barriers[idx])
                else:
                    self._new_barriers[idx] = self.world.collision.apply_barrier_height_in_memory(idx, -0.5)
            self.world.collision.rebuild_sobl_lines()
        self.on_change()

    def undo(self) -> None:
        for o, old_w in self.objects:
            o.deleted = False
            o.written = old_w
        if self.barrier_indices:
            for idx in self.barrier_indices:
                self.world.collision.restore_barrier_state(idx, self._old_barriers.get(idx))
            self.world.collision.rebuild_sobl_lines()
        self.on_change()


class MakeCollisionCommand(QUndoCommand):
    def __init__(self, world: TrackWorld, obj: WorldObject, on_change: Callable[[], None]):
        super().__init__(f"Make collision for {obj.name}")
        self.world = world
        self.obj = obj
        self.on_change = on_change
        model = world.models[obj.model_key]
        lo, hi = obj.world_aabb(model)
        self.box = {
            "obj_id": obj.obj_id,
            "name": obj.name,
            "min": [float(v) for v in lo],
            "max": [float(v) for v in hi],
            "surface": "wall",
        }
        self._prev_flag = obj.has_custom_collision

    def redo(self) -> None:
        self.obj.has_custom_collision = True
        self.obj.written = False
        if self.box not in self.world.collision.custom_boxes:
            self.world.collision.custom_boxes.append(self.box)
        self.on_change()

    def undo(self) -> None:
        self.obj.has_custom_collision = self._prev_flag
        if self.box in self.world.collision.custom_boxes:
            self.world.collision.custom_boxes.remove(self.box)
        self.on_change()


class MoveRoadNodeCommand(QUndoCommand):
    def __init__(
        self,
        graph: RoadGraph,
        node: RoadNode,
        old_pos: tuple[float, float, float],
        new_pos: tuple[float, float, float],
        on_change: Callable[[], None],
    ):
        super().__init__(f"Move road node {node.node_id}")
        self.graph = graph
        self.node = node
        self.old_pos = old_pos
        self.new_pos = new_pos
        self.on_change = on_change

    def redo(self) -> None:
        self.node.x, self.node.y, self.node.z = self.new_pos
        self.graph.dirty = True
        self.on_change()

    def undo(self) -> None:
        self.node.x, self.node.y, self.node.z = self.old_pos
        self.on_change()


class AddRoadNodeCommand(QUndoCommand):
    def __init__(
        self,
        graph: RoadGraph,
        x: float,
        y: float,
        z: float,
        connect_from: int | None,
        lanes: int,
        width: float,
        two_way: bool,
        on_change: Callable[[], None],
    ):
        super().__init__("Add road node")
        self.graph = graph
        self.node = RoadNode(
            node_id=graph.next_id,
            x=x,
            y=y,
            z=z,
            line_name=graph.line_name,
            lanes=lanes,
            width=width,
            two_way=two_way,
        )
        graph.next_id += 1
        self.edge = (
            RoadEdge(a=connect_from, b=self.node.node_id, lanes=lanes, width=width, two_way=two_way)
            if connect_from is not None and connect_from in graph.nodes
            else None
        )
        self.on_change = on_change

    def redo(self) -> None:
        self.graph.nodes[self.node.node_id] = self.node
        if self.edge and self.edge not in self.graph.edges:
            self.graph.edges.append(self.edge)
        self.graph.dirty = True
        self.on_change()

    def undo(self) -> None:
        self.graph.nodes.pop(self.node.node_id, None)
        if self.edge and self.edge in self.graph.edges:
            self.graph.edges.remove(self.edge)
        self.on_change()


class SplitRoadEdgeCommand(QUndoCommand):
    def __init__(
        self,
        graph: RoadGraph,
        edge_index: int,
        x: float,
        y: float,
        z: float,
        on_change: Callable[[], None],
    ):
        super().__init__("Split road segment")
        self.graph = graph
        self.old_edge = graph.edges[edge_index]
        self.node = RoadNode(
            node_id=graph.next_id,
            x=x,
            y=y,
            z=z,
            line_name=graph.line_name,
            lanes=self.old_edge.lanes,
            width=self.old_edge.width,
            two_way=self.old_edge.two_way,
        )
        graph.next_id += 1
        self.e1 = RoadEdge(
            a=self.old_edge.a, b=self.node.node_id,
            lanes=self.old_edge.lanes, width=self.old_edge.width, two_way=self.old_edge.two_way,
        )
        self.e2 = RoadEdge(
            a=self.node.node_id, b=self.old_edge.b,
            lanes=self.old_edge.lanes, width=self.old_edge.width, two_way=self.old_edge.two_way,
        )
        self.on_change = on_change

    def redo(self) -> None:
        if self.old_edge in self.graph.edges:
            self.graph.edges.remove(self.old_edge)
        self.graph.nodes[self.node.node_id] = self.node
        self.graph.edges.extend([self.e1, self.e2])
        self.graph.dirty = True
        self.on_change()

    def undo(self) -> None:
        for e in (self.e1, self.e2):
            if e in self.graph.edges:
                self.graph.edges.remove(e)
        self.graph.nodes.pop(self.node.node_id, None)
        if self.old_edge not in self.graph.edges:
            self.graph.edges.append(self.old_edge)
        self.on_change()


class JoinRoadNodesCommand(QUndoCommand):
    def __init__(
        self,
        graph: RoadGraph,
        a: int,
        b: int,
        lanes: int,
        width: float,
        two_way: bool,
        on_change: Callable[[], None],
    ):
        super().__init__(f"Join road nodes {a} & {b}")
        self.graph = graph
        self.edge = RoadEdge(a=a, b=b, lanes=lanes, width=width, two_way=two_way)
        self.on_change = on_change

    def redo(self) -> None:
        exists = any(
            (e.a == self.edge.a and e.b == self.edge.b) or (e.a == self.edge.b and e.b == self.edge.a)
            for e in self.graph.edges
        )
        if not exists:
            self.graph.edges.append(self.edge)
            self.graph.dirty = True
        self.on_change()

    def undo(self) -> None:
        if self.edge in self.graph.edges:
            self.graph.edges.remove(self.edge)
        self.on_change()


class DeleteRoadNodeCommand(QUndoCommand):
    def __init__(self, graph: RoadGraph, node_id: int, on_change: Callable[[], None]):
        super().__init__(f"Delete road node {node_id}")
        self.graph = graph
        self.node_id = node_id
        self.saved_node: RoadNode | None = None
        self.saved_edges: list[RoadEdge] = []
        self.on_change = on_change

    def redo(self) -> None:
        self.saved_node, self.saved_edges = self.graph.remove_node(self.node_id)
        self.graph.dirty = True
        self.on_change()

    def undo(self) -> None:
        if self.saved_node is not None:
            self.graph.nodes[self.node_id] = self.saved_node
        for e in self.saved_edges:
            if e not in self.graph.edges:
                self.graph.edges.append(e)
        self.on_change()


class ModifyRoadLanesCommand(QUndoCommand):
    def __init__(
        self,
        graph: RoadGraph,
        node_id: int,
        delta_lanes: int = 0,
        two_way: bool | None = None,
        on_change: Callable[[], None] = lambda: None,
    ):
        super().__init__(f"Edit lanes at node {node_id}")
        self.graph = graph
        self.node_id = node_id
        self.delta_lanes = delta_lanes
        self.new_two_way = two_way
        self.on_change = on_change
        n = graph.nodes[node_id]
        self.old_lanes = n.lanes
        self.old_width = n.width
        self.old_two_way = n.two_way
        self.adj_old: list[tuple[RoadEdge, int, float, bool]] = [
            (e, e.lanes, e.width, e.two_way)
            for e in graph.edges
            if e.a == node_id or e.b == node_id
        ]

    def redo(self) -> None:
        n = self.graph.nodes[self.node_id]
        n.lanes = max(1, min(8, self.old_lanes + self.delta_lanes))
        n.width = max(5.0, n.lanes * 6.5)
        if self.new_two_way is not None:
            n.two_way = self.new_two_way
        for e, _ol, _ow, _ot in self.adj_old:
            e.lanes = n.lanes
            e.width = n.width
            e.two_way = n.two_way
        self.graph.dirty = True
        self.on_change()

    def undo(self) -> None:
        n = self.graph.nodes[self.node_id]
        n.lanes = self.old_lanes
        n.width = self.old_width
        n.two_way = self.old_two_way
        for e, ol, ow, ot in self.adj_old:
            e.lanes = ol
            e.width = ow
            e.two_way = ot
        self.on_change()


class AddMarkerCommand(QUndoCommand):
    def __init__(self, world: TrackWorld, marker: WorldMarker, on_change: Callable[[], None]):
        super().__init__(f"Add {marker.kind}: {marker.name}")
        self.world = world
        self.marker = marker
        self.on_change = on_change

    def redo(self) -> None:
        if self.marker not in self.world.markers:
            self.world.markers.append(self.marker)
        self.on_change()

    def undo(self) -> None:
        if self.marker in self.world.markers:
            self.world.markers.remove(self.marker)
        self.on_change()


class DeleteMarkerCommand(QUndoCommand):
    def __init__(self, world: TrackWorld, marker: WorldMarker, on_change: Callable[[], None]):
        super().__init__(f"Delete {marker.name}")
        self.world = world
        self.marker = marker
        self.on_change = on_change

    def redo(self) -> None:
        if self.marker in self.world.markers:
            self.world.markers.remove(self.marker)
        self.on_change()

    def undo(self) -> None:
        if self.marker not in self.world.markers:
            self.world.markers.append(self.marker)
        self.on_change()


class TransformMarkerCommand(QUndoCommand):
    def __init__(
        self,
        marker: WorldMarker,
        old_pos: tuple[float, float, float],
        old_hdg: float,
        new_pos: tuple[float, float, float],
        new_hdg: float,
        on_change: Callable[[], None],
        label: str = "Transform marker",
    ):
        super().__init__(label)
        self.marker = marker
        self.old_pos = tuple(old_pos)
        self.old_hdg = float(old_hdg)
        self.new_pos = tuple(new_pos)
        self.new_hdg = float(new_hdg)
        self.on_change = on_change

    def redo(self) -> None:
        self.marker.position = self.new_pos
        self.marker.heading_deg = self.new_hdg
        self.on_change()

    def undo(self) -> None:
        self.marker.position = self.old_pos
        self.marker.heading_deg = self.old_hdg
        self.on_change()


class ReplaceObjectModelCommand(QUndoCommand):
    """Twinmotion / UE-style 'Replace Selection With...' (undoable model/car swap)."""

    def __init__(
        self,
        world: TrackWorld,
        obj: WorldObject,
        new_model_key: str,
        on_change: Callable[[], None],
    ):
        wm = world.models[new_model_key]
        super().__init__(f"Replace {obj.name} with {wm.name}")
        self.world = world
        self.obj = obj
        self.on_change = on_change
        self.old_state = (
            obj.model_key,
            obj.name,
            obj.model_store,
            obj.hash_hex,
            obj.tri_count,
            list(obj.textures),
            obj.written,
            obj.position,
        )
        # Keep the object seated cleanly at the same bottom Y
        old_m = world.models.get(obj.model_key)
        new_pos = obj.position
        if old_m is not None:
            old_bottom = obj.position[1] + float(old_m.bbox_min[1]) * obj.scale[1]
            new_y = old_bottom - float(wm.bbox_min[1]) * obj.scale[1]
            new_pos = (obj.position[0], float(new_y), obj.position[2])
        self.new_state = (
            new_model_key,
            wm.name,
            wm.model_store,
            wm.hash_hex,
            wm.tri_count,
            list(wm.textures),
            False,
            new_pos,
        )

    def _apply(self, st: tuple[Any, ...]) -> None:
        (
            self.obj.model_key,
            self.obj.name,
            self.obj.model_store,
            self.obj.hash_hex,
            self.obj.tri_count,
            self.obj.textures,
            self.obj.written,
            self.obj.position,
        ) = st
        self.on_change()

    def redo(self) -> None:
        self._apply(self.new_state)

    def undo(self) -> None:
        self._apply(self.old_state)


class ScatterPlaceCommand(QUndoCommand):
    """Single undoable command for a Scatter / Paint Brush stroke placing N objects."""

    def __init__(self, world: TrackWorld, objs: list[WorldObject], on_change: Callable[[], None]):
        super().__init__(f"Scatter {len(objs)} objects")
        self.world = world
        self.objs = list(objs)
        self.on_change = on_change

    def redo(self) -> None:
        for obj in self.objs:
            if obj not in self.world.objects:
                self.world.objects.append(obj)
            obj.deleted = False
        self.on_change()

    def undo(self) -> None:
        for obj in self.objs:
            obj.deleted = True
        self.on_change()


class ReplaceTextureCommand(QUndoCommand):
    """Undoable live PNG texture swap in OpenGL viewport + HOSTTEX export queue."""

    def __init__(
        self,
        world: TrackWorld,
        tex_name: str,
        new_rgba: bytes,
        new_w: int,
        new_h: int,
        png_path: str | None,
        on_tex_change: Callable[[str], None],
    ):
        super().__init__(f"Replace texture {tex_name}")
        self.world = world
        self.tex_name = tex_name
        self.on_tex_change = on_tex_change
        ta = world.textures[tex_name]
        self.old_state = (ta.width, ta.height, ta.rgba, ta.average_rgb, ta.is_replaced, ta.replaced_png_path)
        arr = np.frombuffer(new_rgba, dtype=np.uint8).reshape(-1, 4)
        op = arr[:, 3] > 0
        avg = tuple(int(v) for v in arr[op, :3].mean(axis=0)) if np.any(op) else (140, 140, 140)
        is_rep = png_path is not None
        self.new_state = (int(new_w), int(new_h), bytes(new_rgba), avg, is_rep, png_path)

    def _apply(self, st: tuple[Any, ...]) -> None:
        ta = self.world.textures.get(self.tex_name)
        if ta is None:
            return
        ta.width, ta.height, ta.rgba, ta.average_rgb, ta.is_replaced, ta.replaced_png_path = st
        self.on_tex_change(self.tex_name)

    def redo(self) -> None:
        self._apply(self.new_state)

    def undo(self) -> None:
        self._apply(self.old_state)


class ModifyBarrierCommand(QUndoCommand):
    """Undoable SOBL barrier height adjustment or full-track flatten."""

    def __init__(
        self,
        world: TrackWorld,
        target_heights: dict[int, float],
        on_change: Callable[[], None],
        label: str = "Edit collision barrier",
    ):
        super().__init__(label)
        self.world = world
        self.target_heights = dict(target_heights)
        self.on_change = on_change
        self.old_states: dict[int, dict[str, Any] | None] = {
            idx: (dict(world.collision.sobl_edits[idx]) if idx in world.collision.sobl_edits else None)
            for idx in target_heights
        }
        self.new_states: dict[int, dict[str, Any]] = {}

    def redo(self) -> None:
        for idx, h in self.target_heights.items():
            if idx in self.new_states:
                self.world.collision.restore_barrier_state(idx, self.new_states[idx])
            else:
                try:
                    self.new_states[idx] = self.world.collision.apply_barrier_height_in_memory(idx, h)
                except Exception:
                    pass
        self.world.collision.rebuild_sobl_lines()
        self.on_change()

    def undo(self) -> None:
        for idx, st in self.old_states.items():
            self.world.collision.restore_barrier_state(idx, st)
        self.world.collision.rebuild_sobl_lines()
        self.on_change()


# ---------------------------------------------------------------------------
# Vulkan Host Texture Replacement (.eodhtx) Builder (Contract Section 4)
# ---------------------------------------------------------------------------
def _nearest_host_scale(orig_w: int, orig_h: int, src_w: int, src_h: int) -> int:
    """Pick power-of-two scale factor k >= 1 such that orig*k <= 4096."""
    max_k = 1
    while orig_w * (max_k * 2) <= 4096 and orig_h * (max_k * 2) <= 4096:
        max_k *= 2
    want = max(src_w / max(1, orig_w), src_h / max(1, orig_h))
    k = 1
    while k < max_k and k < want:
        k *= 2
    return k


def build_eodhtx_from_rgba(
    rgba_bytes: bytes,
    src_w: int,
    src_h: int,
    orig_w: int,
    orig_h: int,
) -> tuple[bytes, int, int]:
    """Encode an RGBA8 image into a validated EODHTX1 full mipmap chain for Vulkan HOSTTEX."""
    k = _nearest_host_scale(orig_w, orig_h, src_w, src_h)
    tw, th = orig_w * k, orig_h * k
    img = Image.frombytes("RGBA", (src_w, src_h), rgba_bytes)
    if (src_w, src_h) != (tw, th):
        img = img.resize((tw, th), Image.Resampling.LANCZOS)

    levels: list[bytes] = [img.tobytes("raw", "RGBA")]
    cur = img
    w, h = tw, th
    while w > 1 or h > 1:
        nw, nh = max(1, w >> 1), max(1, h >> 1)
        cur = cur.resize((nw, nh), Image.Resampling.BOX)
        levels.append(cur.tobytes("raw", "RGBA"))
        w, h = nw, nh

    data = b"EODHTX1\0" + struct.pack("<4H", tw, th, len(levels), 0) + b"".join(levels)
    # Validate contract header & chain size
    expected_levels = max(tw, th).bit_length()
    expected_size = 16 + sum(max(1, tw >> i) * max(1, th >> i) * 4 for i in range(expected_levels))
    if len(levels) != expected_levels or len(data) != expected_size:
        raise AssertionError(f"Invalid .eodhtx mip chain for {tw}x{th}")
    return data, tw, th


def load_png_rgba(png_path: Path | str) -> tuple[int, int, bytes]:
    """Load any PNG/image via Pillow as straight RGBA8 bytes."""
    img = Image.open(str(png_path)).convert("RGBA")
    return img.width, img.height, img.tobytes("raw", "RGBA")


# ---------------------------------------------------------------------------
# Project Overlay (.eodworld.json) Save / Load
# ---------------------------------------------------------------------------
def save_project_overlay(
    path: Path | str,
    project_name: str,
    world: TrackWorld,
    camera_state: dict[str, Any] | None = None,
) -> Path:
    """Save non-destructive .eodworld.json overlay. Never touches retail .TRK."""
    out_path = Path(path)
    if not out_path.name.lower().endswith(".eodworld.json"):
        out_path = out_path.with_suffix(".eodworld.json")
    out_path.parent.mkdir(parents=True, exist_ok=True)

    modified_retail: list[dict[str, Any]] = []
    added_objects: list[dict[str, Any]] = []
    deleted_ids: list[str] = []

    for obj in world.objects:
        if obj.source == "Retail":
            if obj.deleted:
                deleted_ids.append(obj.obj_id)
                continue
            pos_changed = any(abs(a - b) > 1e-4 for a, b in zip(obj.position, obj.retail_initial_pos))
            rot_changed = any(abs(a - b) > 1e-4 for a, b in zip(obj.rotation_hpr, obj.retail_initial_rot))
            scl_changed = any(abs(a - 1.0) > 1e-4 for a in obj.scale)
            model_changed = (
                obj.retail_original_model_key is not None and obj.model_key != obj.retail_original_model_key
            )
            if pos_changed or rot_changed or scl_changed or model_changed or obj.has_custom_collision:
                entry = {
                    "obj_id": obj.obj_id,
                    "position": list(obj.position),
                    "rotation_hpr": list(obj.rotation_hpr),
                    "scale": list(obj.scale),
                    "has_custom_collision": obj.has_custom_collision,
                    "written": obj.written,
                }
                if model_changed:
                    entry["model_key"] = obj.model_key
                    entry["name"] = obj.name
                modified_retail.append(entry)
        else:
            if obj.deleted:
                continue
            added_objects.append(
                {
                    "obj_id": obj.obj_id,
                    "name": obj.name,
                    "model_key": obj.model_key,
                    "category": obj.category,
                    "subtype": obj.subtype,
                    "section_mlist": obj.section_mlist,
                    "position": list(obj.position),
                    "rotation_hpr": list(obj.rotation_hpr),
                    "scale": list(obj.scale),
                    "has_custom_collision": obj.has_custom_collision,
                    "written": obj.written,
                }
            )

    roads_doc: dict[str, Any] = {}
    for lname, g in world.all_road_graphs.items():
        roads_doc[lname] = {
            "line_name": g.line_name,
            "next_id": g.next_id,
            "dirty": g.dirty,
            "nodes": [
                {
                    "node_id": n.node_id,
                    "x": n.x,
                    "y": n.y,
                    "z": n.z,
                    "is_side": n.is_side,
                    "retail_index": n.retail_index,
                    "lanes": n.lanes,
                    "width": n.width,
                    "two_way": n.two_way,
                }
                for n in g.nodes.values()
            ],
            "edges": [
                {
                    "a": e.a,
                    "b": e.b,
                    "lanes": e.lanes,
                    "width": e.width,
                    "two_way": e.two_way,
                }
                for e in g.edges
            ],
        }

    markers_doc = [
        {
            "marker_id": m.marker_id,
            "name": m.name,
            "kind": m.kind,
            "position": list(m.position),
            "heading_deg": m.heading_deg,
            "source": m.source,
            "note": m.note,
        }
        for m in world.markers
    ]

    replaced_tex_doc = {
        name: ta.replaced_png_path
        for name, ta in world.textures.items()
        if ta.is_replaced and ta.replaced_png_path
    }
    sobl_edits_doc = {str(k): v for k, v in world.collision.sobl_edits.items()}

    doc = {
        "schema": "eod.world_project/2",
        "project_name": project_name,
        "saved_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "track": world.track_name,
        "trk_rel": world.trk_rel,
        "active_road_line": world.road_graph.line_name,
        "camera": camera_state or {},
        "environment": world.environment.to_dict(),
        "custom_race": world.custom_race.to_dict(),
        "replaced_textures": replaced_tex_doc,
        "sobl_barrier_edits": sobl_edits_doc,
        "modified_retail_objects": modified_retail,
        "added_objects": added_objects,
        "deleted_object_ids": deleted_ids,
        "custom_collision_boxes": world.collision.custom_boxes,
        "roads": roads_doc,
        "markers": markers_doc,
    }
    out_path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    return out_path


def apply_project_overlay(world: TrackWorld, path: Path | str) -> dict[str, Any]:
    """Load and apply an .eodworld.json overlay onto an already loaded TrackWorld."""
    doc = json.loads(Path(path).read_text(encoding="utf-8"))
    by_id = {o.obj_id: o for o in world.objects}

    for del_id in doc.get("deleted_object_ids", []):
        if del_id in by_id:
            by_id[del_id].deleted = True

    for mod in doc.get("modified_retail_objects", []):
        obj = by_id.get(mod["obj_id"])
        if obj is None:
            continue
        obj.position = tuple(float(v) for v in mod["position"])
        obj.rotation_hpr = tuple(float(v) for v in mod["rotation_hpr"])
        obj.scale = tuple(float(v) for v in mod["scale"])
        obj.has_custom_collision = bool(mod.get("has_custom_collision", False))
        obj.written = bool(mod.get("written", False))
        new_mk = mod.get("model_key")
        if new_mk and new_mk in world.models:
            wm = world.models[new_mk]
            obj.model_key = new_mk
            obj.name = mod.get("name", wm.name)
            obj.model_store = wm.model_store
            obj.hash_hex = wm.hash_hex
            obj.tri_count = wm.tri_count
            obj.textures = list(wm.textures)

    for add in doc.get("added_objects", []):
        mkey = add["model_key"]
        if mkey not in world.models:
            continue
        wm = world.models[mkey]
        obj = WorldObject(
            obj_id=add["obj_id"],
            name=add.get("name", wm.name),
            model_key=mkey,
            source="Placed by the editor",
            category=add.get("category", wm.category),
            subtype=add.get("subtype", wm.subtype),
            section_mlist=add.get("section_mlist", "editor_layer"),
            model_store=wm.model_store,
            hash_hex=wm.hash_hex,
            tri_count=wm.tri_count,
            textures=list(wm.textures),
            position=tuple(float(v) for v in add["position"]),
            rotation_hpr=tuple(float(v) for v in add["rotation_hpr"]),
            scale=tuple(float(v) for v in add["scale"]),
            written=bool(add.get("written", False)),
            has_custom_collision=bool(add.get("has_custom_collision", False)),
        )
        world.objects.append(obj)

    world.collision.custom_boxes = list(doc.get("custom_collision_boxes", []))

    if "environment" in doc:
        world.environment = EnvironmentState.from_dict(doc["environment"])
    if "custom_race" in doc:
        world.custom_race = CustomRaceConfig.from_dict(doc["custom_race"])

    # Restore replaced textures if PNG files still exist
    for tname, png_p in doc.get("replaced_textures", {}).items():
        ta = world.textures.get(tname)
        if ta is not None and png_p and Path(png_p).is_file():
            try:
                w, h, rgba = load_png_rgba(png_p)
                ta.width, ta.height, ta.rgba = w, h, rgba
                arr = np.frombuffer(rgba, dtype=np.uint8).reshape(-1, 4)
                op = arr[:, 3] > 0
                ta.average_rgb = tuple(int(v) for v in arr[op, :3].mean(axis=0)) if np.any(op) else (140, 140, 140)
                ta.is_replaced = True
                ta.replaced_png_path = str(png_p)
            except Exception:
                pass

    # Restore SOBL barrier edits
    sobl_edits_doc = doc.get("sobl_barrier_edits", {})
    if sobl_edits_doc:
        for k_str, ed in sobl_edits_doc.items():
            try:
                idx = int(k_str)
                if 0 <= idx < len(world.collision.sobl_records):
                    world.collision.restore_barrier_state(idx, ed)
            except Exception:
                pass
        world.collision.rebuild_sobl_lines()

    roads_doc = doc.get("roads", {})
    avail = list(roads_doc.keys()) or list(world.all_road_graphs.keys())
    for lname, rdata in roads_doc.items():
        g = RoadGraph(
            line_name=lname,
            available_lines=avail,
            next_id=int(rdata.get("next_id", 1)),
            dirty=bool(rdata.get("dirty", False)),
        )
        for nd in rdata.get("nodes", []):
            node = RoadNode(
                node_id=int(nd["node_id"]),
                x=float(nd["x"]),
                y=float(nd["y"]),
                z=float(nd["z"]),
                line_name=lname,
                is_side=bool(nd.get("is_side", False)),
                retail_index=nd.get("retail_index"),
                lanes=int(nd.get("lanes", 2)),
                width=float(nd.get("width", 14.0)),
                two_way=bool(nd.get("two_way", False)),
            )
            g.nodes[node.node_id] = node
        for ed in rdata.get("edges", []):
            g.edges.append(
                RoadEdge(
                    a=int(ed["a"]),
                    b=int(ed["b"]),
                    lanes=int(ed.get("lanes", 2)),
                    width=float(ed.get("width", 14.0)),
                    two_way=bool(ed.get("two_way", False)),
                )
            )
        world.all_road_graphs[lname] = g

    active_line = doc.get("active_road_line")
    if active_line and active_line in world.all_road_graphs:
        world.road_graph = world.all_road_graphs[active_line]

    if "markers" in doc:
        world.markers = [
            WorldMarker(
                marker_id=m["marker_id"],
                name=m["name"],
                kind=m["kind"],
                position=tuple(float(v) for v in m["position"]),
                heading_deg=float(m.get("heading_deg", 0.0)),
                source=m.get("source", "Placed by the editor"),
                note=str(m.get("note", "")),
            )
            for m in doc["markers"]
        ]

    return doc


# ---------------------------------------------------------------------------
# Non-destructive Mod Layer Write-Back (Ctrl+Shift+S & Play-In-Editor)
# ---------------------------------------------------------------------------
def _sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _export_host_textures(
    world: TrackWorld,
    slug: str,
    out_dir: Path,
) -> tuple[list[dict[str, Any]], list[str]]:
    """Write Vulkan HOSTTEX/replacements.tsv + .eodhtx mip chains for replaced textures & borrowed sky."""
    host_dir = out_dir / "HOSTTEX"
    pkg_sub = re.sub(r"[^a-z0-9._-]+", "-", slug.lower()).strip("-") or "world-mod"
    tex_copy_dir = out_dir / "textures_replaced"
    rows = ["eod-host-textures 1"]
    exported: list[dict[str, Any]] = []
    warnings: list[str] = []
    seen_keys: set[str] = set()
    counter = 0

    def emit_entry(
        rgba: bytes,
        sw: int,
        sh: int,
        orig_w: int,
        orig_h: int,
        index_key: str,
        asset_id: str,
        label: str,
        png_src: str | None = None,
    ) -> None:
        nonlocal counter
        if index_key in seen_keys:
            return
        seen_keys.add(index_key)
        eodhtx_bytes, tw, th = build_eodhtx_from_rgba(rgba, sw, sh, orig_w, orig_h)
        rel = f"{pkg_sub}/{counter:04d}-{index_key}.eodhtx"
        target = host_dir / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(eodhtx_bytes)
        # Use '*' palette wildcard so CLUT/lighting variants still bind reliably in Vulkan
        rows.append("\t".join((index_key, "*", str(orig_w), str(orig_h), rel, asset_id, pkg_sub)))
        exported.append(
            {
                "name": label,
                "asset_id": asset_id,
                "index_key": index_key,
                "orig_size": [orig_w, orig_h],
                "host_size": [tw, th],
                "file": f"HOSTTEX/{rel}",
                "bytes": len(eodhtx_bytes),
            }
        )
        # Also copy/write a reference PNG in textures_replaced/
        tex_copy_dir.mkdir(parents=True, exist_ok=True)
        safe_png = re.sub(r"[^A-Za-z0-9_.-]+", "_", label) + ".png"
        if png_src and Path(png_src).is_file():
            shutil.copy2(png_src, tex_copy_dir / safe_png)
        else:
            write_png(str(tex_copy_dir / safe_png), sw, sh, rgba)
        counter += 1

    # 1. User-replaced textures from Texture / Material Inspector
    for name, ta in sorted(world.textures.items()):
        if not ta.is_replaced:
            continue
        ikey, _pkey = ta.ensure_identity()
        if not ikey:
            warnings.append(f"Texture {name} is direct-color or missing raw TEX; skipped HOSTTEX key")
            continue
        emit_entry(
            ta.rgba,
            ta.width,
            ta.height,
            ta.orig_width,
            ta.orig_height,
            ikey,
            ta.asset_id,
            name,
            png_src=ta.replaced_png_path,
        )

    # 2. Borrowed Skybox from another retail .TRK archive (swaps sky01A..16B via HOSTTEX!)
    borrowed = world.environment.borrowed_sky_track
    if borrowed and borrowed.upper() != world.track_name.upper():
        src_trk = world.game_dir / "DATA" / "TRACKS" / f"{borrowed.upper()}.TRK"
        if src_trk.is_file():
            try:
                src_entries = mb.directory(src_trk)
                src_sky = {
                    e["name"].lower(): e
                    for e in src_entries
                    if e["type"] == "TEX" and re.fullmatch(r"sky\d\d[ab]\.tex", e["name"], re.I)
                }
                dst_sky = {
                    e["name"].lower(): e
                    for e in world.entries
                    if e["type"] == "TEX" and re.fullmatch(r"sky\d\d[ab]\.tex", e["name"], re.I)
                }
                for sname, dst_e in sorted(dst_sky.items()):
                    src_e = src_sky.get(sname)
                    if not src_e:
                        continue
                    dst_raw, _ = mb.payload(world.trk_path, dst_e["name"], world.entries, verify_crc=False)
                    src_raw, _ = mb.payload(src_trk, src_e["name"], src_entries, verify_crc=False)
                    ident = tex_id.texture_identity(dst_raw)
                    sw, sh, srgba, _meta = decode_preview(src_raw)
                    emit_entry(
                        srgba,
                        sw,
                        sh,
                        ident["width"],
                        ident["height"],
                        ident["indexKey"],
                        f"{world.trk_rel}#{dst_e['name']}",
                        f"sky_{borrowed.lower()}_{dst_e['name']}",
                    )
            except Exception as exc:
                warnings.append(f"Borrowed sky export from {borrowed} failed: {exc}")

    if exported:
        host_dir.mkdir(parents=True, exist_ok=True)
        (host_dir / "replacements.tsv").write_text("\n".join(rows) + "\n", encoding="utf-8")

    return exported, warnings


# ---------------------------------------------------------------------------
# VIF Vertex Baking (track.grf & standalone .mod) + BPPT / SCOL Collision
# + Editor-Placed Car/Prop .TRK Injection & All-Events Sync
# ---------------------------------------------------------------------------
def _object_baked_matrix(world: TrackWorld, obj: WorldObject) -> tuple[np.ndarray, np.ndarray]:
    """Return (m4, pivot) for baking `obj`, anchoring bottom base when scaling Y."""
    pivot = np.array(obj.retail_initial_pos, dtype=np.float64)
    m4 = obj.world_matrix().astype(np.float64)
    model = world.models.get(obj.model_key)
    if model is not None and abs(obj.scale[1] - 1.0) > 1e-4 and abs(obj.position[1] - obj.retail_initial_pos[1]) < 1e-3:
        orig_min_y = float(pivot[1]) + float(model.bbox_min[1])
        lo, _hi = obj.world_aabb(model)
        dy_anchor = orig_min_y - float(lo[1])
        if abs(dy_anchor) > 1e-5:
            m4[1, 3] += dy_anchor
    return m4, pivot


def _encode_vif_part_vertices(
    buf: bytearray,
    orig_data: bytes,
    part: dict[str, Any],
    transform_pt: Callable[[tuple[float, float, float]], tuple[float, float, float]],
) -> tuple[list[tuple[float, float, float]], int]:
    """Transform and re-encode every VIF vertex in `part` in place, safely handling shared STROWs."""
    batches = vif_patch.scan_part(orig_data, part)
    valid = [b for b in batches if b.get("count", 0) > 0 and "pos" in b and "q199" in b]
    if not valid:
        return [], 0

    all_new_pts: list[tuple[float, float, float]] = []
    batch_new_pts: list[list[tuple[float, float, float]]] = []
    for b in valid:
        old_pts = vif_patch.read_batch(orig_data, b)["p"]
        new_pts = [transform_pt(p) for p in old_pts]
        batch_new_pts.append(new_pts)
        all_new_pts.extend(new_pts)

    # Group batches by STROW offset so raising an exponent for a shared STROW refits all users cleanly
    by_row: dict[int, list[int]] = {}
    for bi, b in enumerate(valid):
        by_row.setdefault(int(b["pos"]["row_off"]), []).append(bi)

    changed_verts = 0
    for row_off, grp in by_row.items():
        first_sl = valid[grp[0]]["pos"]
        bits = int(first_sl["bits"])
        top = (1 << bits) - 1
        orig_row = list(first_sl["row"])
        new_row = list(orig_row)

        if bits < 32 and row_off >= 0:
            for c in range(3):
                max_span = max(
                    (max(p[c] for p in batch_new_pts[bi]) - min(p[c] for p in batch_new_pts[bi]))
                    for bi in grp
                )
                cur_exp = (orig_row[c] >> 23) & 0xFF
                # Pick smallest exponent >= cur_exp whose u16/u8 range covers the largest batch span
                chosen_row = orig_row[c]
                if max_span > (top - 2) * vif_patch.ulp_of(chosen_row):
                    for e in range(cur_exp + 1, 152):
                        cand = e << 23
                        if (top - 2) * vif_patch.ulp_of(cand) >= max_span * 1.02:
                            chosen_row = cand
                            break
                new_row[c] = chosen_row
            if new_row != orig_row:
                struct.pack_into("<3I", buf, row_off, *new_row)

        for bi in grp:
            b = valid[bi]
            sl = b["pos"]
            want = batch_new_pts[bi]
            n = b["count"]
            offs = list(struct.unpack_from("<3f", orig_data, b["q199"]))
            new_off = list(offs)
            cols: list[list[int]] = []
            for c in range(3):
                vals = [p[c] for p in want]
                if bits == 32:
                    us = [struct.unpack("<I", struct.pack("<f", x - offs[c]))[0] for x in vals]
                    cols.append(us)
                else:
                    r = new_row[c]
                    base_f = vif_patch.fbits(r)
                    ulp = vif_patch.ulp_of(r)
                    lo_v = min(vals)
                    # Keep existing offset if everything already fits, else shift q199 cleanly
                    cur_us = [(x - offs[c] - base_f) / ulp for x in vals]
                    if r == orig_row[c] and all(-0.5 <= u < top + 0.5 for u in cur_us):
                        us = [min(top, max(0, round(u))) for u in cur_us]
                    else:
                        no = vif_patch.f32round(lo_v - ulp - base_f)
                        new_off[c] = no
                        us = [min(top, max(0, round((x - no - base_f) / ulp))) for x in vals]
                    cols.append(us)
            if new_off != offs:
                struct.pack_into("<3f", buf, b["q199"], *new_off)
            fmt = vif_patch.UF[bits] % 3
            for j in range(n):
                struct.pack_into(fmt, buf, sl["off"] + j * sl["stride"], cols[0][j], cols[1][j], cols[2][j])
            changed_verts += n

    return all_new_pts, changed_verts


def _bake_modl_scale_bytes(mod_bytes: bytes, scale: tuple[float, float, float]) -> bytes:
    """Bake non-unit `(sx, sy, sz)` directly into a standalone `.mod` file's VIF vertices + bounds."""
    sx, sy, sz = (float(scale[0]), float(scale[1]), float(scale[2]))
    if abs(sx - 1.0) <= 1e-4 and abs(sy - 1.0) <= 1e-4 and abs(sz - 1.0) <= 1e-4:
        return mod_bytes
    mhdr = wg.parse_model(mod_bytes, 0)
    buf = bytearray(mod_bytes)
    all_pts: list[tuple[float, float, float]] = []
    for part in mhdr["parts"]:
        pts, _cnt = _encode_vif_part_vertices(
            buf,
            mod_bytes,
            part,
            lambda p: (p[0] * sx, p[1] * sy, p[2] * sz),
        )
        all_pts.extend(pts)
    if all_pts:
        xs = [p[0] for p in all_pts]
        ys = [p[1] for p in all_pts]
        zs = [p[2] for p in all_pts]
        cx = 0.5 * (min(xs) + max(xs))
        cy = 0.5 * (min(ys) + max(ys))
        cz = 0.5 * (min(zs) + max(zs))
        rad = max(math.dist(p, (cx, cy, cz)) for p in all_pts) + 0.05
        struct.pack_into("<f", buf, 0x18, float(rad))
        struct.pack_into("<3f", buf, 0x1C, float(cx), float(cy), float(cz))
        struct.pack_into(
            "<6f", buf, 0x28,
            float(min(xs)), float(max(xs)),
            float(min(ys)), float(max(ys)),
            float(min(zs)), float(max(zs)),
        )
    return bytes(buf)


def _plane_from_tri(
    v0: np.ndarray, v1: np.ndarray, v2: np.ndarray
) -> tuple[float, float, float, float] | None:
    """Compute upward-pointing unit plane `(a, b, c, d)` for `y = -(a*x + c*z + d) / b`."""
    n = np.cross(v1 - v0, v2 - v0)
    nl = float(np.linalg.norm(n))
    if nl < 1e-7:
        return None
    n = n / nl
    if n[1] < 0.0:
        n = -n
    if n[1] < 0.05:
        return None
    d = -float(np.dot(n, v0))
    return (float(n[0]), float(n[1]), float(n[2]), d)


def _point_in_tri_xz(px: float, pz: float, v0: np.ndarray, v1: np.ndarray, v2: np.ndarray) -> tuple[bool, float]:
    """2D barycentric test in XZ returning `(inside, interpolated_y)`."""
    x0, y0, z0 = float(v0[0]), float(v0[1]), float(v0[2])
    x1, y1, z1 = float(v1[0]), float(v1[1]), float(v1[2])
    x2, y2, z2 = float(v2[0]), float(v2[1]), float(v2[2])
    det = (z1 - z2) * (x0 - x2) + (x2 - x1) * (z0 - z2)
    if abs(det) < 1e-8:
        return False, 0.0
    inv = 1.0 / det
    l0 = ((z1 - z2) * (px - x2) + (x2 - x1) * (pz - z2)) * inv
    l1 = ((z2 - z0) * (px - x2) + (x0 - x2) * (pz - z2)) * inv
    l2 = 1.0 - l0 - l1
    if l0 >= -0.02 and l1 >= -0.02 and l2 >= -0.02:
        return True, l0 * y0 + l1 * y1 + l2 * y2
    return False, 0.0


def _bake_graf_vif_and_collision(
    trk_bytes: bytearray,
    entries: list[dict[str, Any]],
    world: TrackWorld,
    vif_objs: list[WorldObject],
    all_edited_terrain_objs: list[WorldObject],
    warnings: list[str],
) -> dict[str, int]:
    """Bake VIF vertex transforms into `track.grf`, promote opt_terrain when syncing all events,
    and bake matching collision planes into `track.bpp` (BPPT) and `track.bsc` (SCOL).
    """
    stats = {
        "vif_models_baked": 0,
        "vif_vertices_baked": 0,
        "opt_terrain_promoted": 0,
        "bppt_planes_patched": 0,
        "scol_planes_patched": 0,
    }

    # --- 1. Patch track.grf (VIF scale/rotate/translate + opt_terrain all-events promotion) ---
    graf_e = next((e for e in entries if e["type"] == "GRAF"), None)
    if graf_e is not None:
        graf_orig = bytes(trk_bytes[graf_e["offset"] : graf_e["offset"] + graf_e["size"]])
        graf_buf = bytearray(graf_orig)
        graf_parsed = wg.parse_graf(graf_orig)
        by_id = {f"i{i:04d}": leaf for i, leaf in enumerate(graf_parsed["leaves"])}
        mlists = [info for info in graf_parsed["mlist_info"] if info]
        graf_changed = False

        # Track already-baked model offsets so shared models aren't double-transformed
        baked_offsets: set[int] = set()
        for obj in vif_objs:
            if not obj.retail_instance_id or obj.retail_instance_id not in by_id:
                continue
            leaf = by_id[obj.retail_instance_id]
            mod_off = leaf.get("model_offset")
            if not mod_off or mod_off in baked_offsets:
                continue
            baked_offsets.add(mod_off)
            try:
                mhdr = wg.parse_model(graf_orig, mod_off)
                m4, pivot = _object_baked_matrix(world, obj)

                def _xform(p: tuple[float, float, float]) -> tuple[float, float, float]:
                    lx = p[0] - pivot[0]
                    ly = p[1] - pivot[1]
                    lz = p[2] - pivot[2]
                    nx = m4[0, 0] * lx + m4[0, 1] * ly + m4[0, 2] * lz + m4[0, 3]
                    ny = m4[1, 0] * lx + m4[1, 1] * ly + m4[1, 2] * lz + m4[1, 3]
                    nz = m4[2, 0] * lx + m4[2, 1] * ly + m4[2, 2] * lz + m4[2, 3]
                    return (float(nx), float(ny), float(nz))

                all_pts: list[tuple[float, float, float]] = []
                obj_vcount = 0
                for part in mhdr["parts"]:
                    pts, vc = _encode_vif_part_vertices(graf_buf, graf_orig, part, _xform)
                    all_pts.extend(pts)
                    obj_vcount += vc

                if all_pts:
                    xs = [p[0] for p in all_pts]
                    ys = [p[1] for p in all_pts]
                    zs = [p[2] for p in all_pts]
                    nc = (0.5 * (min(xs) + max(xs)), 0.5 * (min(ys) + max(ys)), 0.5 * (min(zs) + max(zs)))
                    nrad = max(math.dist(p, nc) for p in all_pts) + 0.05
                    struct.pack_into("<f", graf_buf, mod_off + 0x18, float(nrad))
                    struct.pack_into("<3f", graf_buf, mod_off + 0x1C, *nc)
                    struct.pack_into(
                        "<6f",
                        graf_buf,
                        mod_off + 0x28,
                        float(min(xs)), float(max(xs)),
                        float(min(ys)), float(max(ys)),
                        float(min(zs)), float(max(zs)),
                    )
                    # Update mlist sphere copies matching old (centre, radius)
                    old_key = struct.pack("<4f", *mhdr["centre"], mhdr["radius"])
                    for info in mlists:
                        lo_, hi_ = info["offset"], info["offset"] + info["batches"]["offset"]
                        pos_s = lo_
                        while True:
                            k = graf_orig.find(old_key, pos_s, hi_)
                            if k < 0:
                                break
                            if (k - lo_) % 4 == 0:
                                struct.pack_into("<4f", graf_buf, k, nc[0], nc[1], nc[2], float(nrad))
                            pos_s = k + 4
                    # Grow ancestor NODE_SPHERE cull radii so raised/scaled ramp never culls out
                    for step in leaf["path"]:
                        if step["kind"] != "sphere":
                            continue
                        n_off = step["node"]
                        cx, cy, cz, r = struct.unpack_from("<4f", graf_buf, n_off + 0x0C)
                        need = math.dist(nc, (cx, cy, cz)) + float(nrad)
                        if need > r:
                            struct.pack_into("<f", graf_buf, n_off + 0x18, float(need * 1.01))

                    stats["vif_models_baked"] += 1
                    stats["vif_vertices_baked"] += obj_vcount
                    graf_changed = True
            except Exception as exc:
                warnings.append(f"VIF bake warning for {obj.name} ({obj.obj_id}): {exc}")

        # Sync Modded Ramps & Props to All Events / Free Roam:
        # Promote active Optional Terrain leaves (record->kind 1, subtype 20) to Simple Model (kind=0)
        # so f_00223008 never hides them in Battle / Free Roam / Figure-8 / Jump Race / Gauntlet!
        if world.sync_all_events:
            active_opt_ids = {
                o.retail_instance_id
                for o in world.objects
                if not o.deleted and o.category == "optional-terrain" and o.retail_instance_id
            }
            for inst_id in sorted(active_opt_ids):
                leaf = by_id.get(inst_id)
                if not leaf or not leaf.get("optional_terrain"):
                    continue
                rec_off = leaf.get("record")
                if rec_off is not None and rec_off + 4 <= len(graf_buf):
                    if struct.unpack_from("<I", graf_buf, rec_off)[0] == 1:
                        struct.pack_into("<I", graf_buf, rec_off, 0)
                        stats["opt_terrain_promoted"] += 1
                        graf_changed = True

        if graf_changed:
            new_graf = bytes(graf_buf)
            trk_bytes[graf_e["offset"] : graf_e["offset"] + graf_e["size"]] = new_graf
            struct.pack_into("<I", trk_bytes, graf_e["crc_field"], eod_crc(new_graf))

    # --- 2. Patch track.bpp (BPPT v7) optional-terrain ramp collision planes ---
    bpp_e = next((e for e in entries if e["name"].lower() == "track.bpp" and e["type"] == "BPPT"), None)
    if bpp_e is not None and all_edited_terrain_objs:
        bpp_orig = bytes(trk_bytes[bpp_e["offset"] : bpp_e["offset"] + bpp_e["size"]])
        bpp_buf = bytearray(bpp_orig)
        bppt_slots = parse_bppt(bpp_orig)
        bpp_changed = False
        slots_xz_transform: dict[int, tuple[np.ndarray, np.ndarray]] = {}

        for obj in all_edited_terrain_objs:
            if obj.category != "optional-terrain" or obj.opt_terrain_mode is None:
                continue
            slot = bppt_slots.get(obj.opt_terrain_mode)
            model = world.models.get(obj.model_key)
            if slot is None or model is None:
                continue
            m4, pivot = _object_baked_matrix(world, obj)
            planes_off = slot["planes_off"]
            patched_pidx: set[int] = set()

            for part in model.parts:
                if len(part.indices) < 3:
                    continue
                local_pts = part.vertices[:, 0:3].astype(np.float64)
                orig_pts = local_pts + pivot[None, :]
                ones = np.ones((len(local_pts), 1), dtype=np.float64)
                new_pts = (np.hstack([local_pts, ones]) @ m4.T)[:, :3]
                tris = part.indices.reshape(-1, 3)

                for a_i, b_i, c_i in tris:
                    ov0, ov1, ov2 = orig_pts[a_i], orig_pts[b_i], orig_pts[c_i]
                    nv0, nv1, nv2 = new_pts[a_i], new_pts[b_i], new_pts[c_i]
                    plane = _plane_from_tri(nv0, nv1, nv2)
                    if plane is None:
                        continue
                    # Query exact plane index in track.bpp at original triangle centroid (with inset fallback)
                    cx = (ov0[0] + ov1[0] + ov2[0]) / 3.0
                    cz = (ov0[2] + ov1[2] + ov2[2]) / 3.0
                    pidx = query_bppt_plane(bpp_orig, slot, float(cx), float(cz))
                    if pidx is None:
                        for corner in (ov0, ov1, ov2):
                            qx = cx * 0.8 + corner[0] * 0.2
                            qz = cz * 0.8 + corner[2] * 0.2
                            pidx = query_bppt_plane(bpp_orig, slot, float(qx), float(qz))
                            if pidx is not None:
                                break
                    if pidx is None or pidx in patched_pidx:
                        continue
                    patched_pidx.add(pidx)
                    struct.pack_into("<4f", bpp_buf, planes_off + pidx * 20, *plane)
                    stats["bppt_planes_patched"] += 1
                    bpp_changed = True

            # Record XZ transform if ramp was moved/scaled/rotated in XZ so BSP split lines stay aligned
            dx = abs(float(m4[0, 3]) - float(pivot[0]))
            dz = abs(float(m4[2, 3]) - float(pivot[2]))
            xz_changed = (
                dx > 0.05
                or dz > 0.05
                or abs(obj.scale[0] - 1.0) > 1e-3
                or abs(obj.scale[2] - 1.0) > 1e-3
                or any(abs(a - b) > 1e-3 for a, b in zip(obj.rotation_hpr, obj.retail_initial_rot))
            )
            if xz_changed and obj.opt_terrain_mode not in slots_xz_transform:
                slots_xz_transform[obj.opt_terrain_mode] = (m4, pivot)

        # If a slot's ramps were uniformly shifted/scaled in XZ, transform its 2D BSP split lines too
        for mode_id, (m4, pivot) in slots_xz_transform.items():
            # Only transform entire slot BSP when every active ramp in that mode shares the transform,
            # OR when there is 1 ramp in that mode
            mode_objs = [
                o for o in world.objects
                if not o.deleted and o.category == "optional-terrain" and o.opt_terrain_mode == mode_id
            ]
            if len(mode_objs) == 1:
                slot = bppt_slots[mode_id]
                A2 = np.array([[m4[0, 0], m4[0, 2]], [m4[2, 0], m4[2, 2]]], dtype=np.float64)
                det = float(np.linalg.det(A2))
                if abs(det) > 1e-6:
                    A2_inv = np.linalg.inv(A2)
                    tx = float(m4[0, 3]) - (A2[0, 0] * float(pivot[0]) + A2[0, 1] * float(pivot[2]))
                    tz = float(m4[2, 3]) - (A2[1, 0] * float(pivot[0]) + A2[1, 1] * float(pivot[2]))
                    tvec = np.array([tx, tz], dtype=np.float64)
                    nodes_off = slot["nodes_off"]
                    for ni in range(slot["n_nodes"]):
                        nx, nz, d_val, l_ch, r_ch = struct.unpack_from("<3f2i", bpp_orig, nodes_off + ni * 20)
                        n_old = np.array([nx, nz], dtype=np.float64)
                        n_new = A2_inv.T @ n_old
                        nl = float(np.linalg.norm(n_new))
                        if nl > 1e-7:
                            d_new = (d_val - float(np.dot(n_new, tvec))) / nl
                            n_new = n_new / nl
                            struct.pack_into(
                                "<3f", bpp_buf, nodes_off + ni * 20, float(n_new[0]), float(n_new[1]), float(d_new)
                            )
                            bpp_changed = True

        if bpp_changed:
            new_bpp = bytes(bpp_buf)
            trk_bytes[bpp_e["offset"] : bpp_e["offset"] + bpp_e["size"]] = new_bpp
            struct.pack_into("<I", trk_bytes, bpp_e["crc_field"], eod_crc(new_bpp))

    # --- 3. Patch track.bsc (SCOL v4) terrain collision planes + 0xFE all-events sentinel ---
    bsc_e = next((e for e in entries if e["name"].lower() == "track.bsc" and e["type"] == "SCOL"), None)
    if bsc_e is not None:
        bsc_orig = bytes(trk_bytes[bsc_e["offset"] : bsc_e["offset"] + bsc_e["size"]])
        bsc_buf = bytearray(bsc_orig)
        bsc_changed = False
        count, t1, t2 = struct.unpack_from("<3I", bsc_orig, 0)

        static_edited = [o for o in all_edited_terrain_objs if o.category == "static"]
        if static_edited and count > 0:
            try:
                scol = wc.parse_scol(bsc_orig)
                for obj in static_edited:
                    model = world.models.get(obj.model_key)
                    if model is None:
                        continue
                    m4, pivot = _object_baked_matrix(world, obj)
                    # Build original & transformed triangles for this static mesh
                    orig_lo = pivot + model.bbox_min.astype(np.float64)
                    orig_hi = pivot + model.bbox_max.astype(np.float64)
                    new_lo_f, new_hi_f = obj.world_aabb(model)
                    # Keep union XZ footprint so uniform-scaled ramps raise all SCOL tris under them
                    min_x = min(float(orig_lo[0]), float(new_lo_f[0])) - 0.5
                    max_x = max(float(orig_hi[0]), float(new_hi_f[0])) + 0.5
                    min_z = min(float(orig_lo[2]), float(new_lo_f[2])) - 0.5
                    max_z = max(float(orig_hi[2]), float(new_hi_f[2])) + 0.5
                    min_y_orig = float(orig_lo[1]) - 3.0
                    max_y_orig = float(orig_hi[1]) + 3.0

                    # Collect transformed triangles for exact height sampling
                    new_tris: list[tuple[np.ndarray, np.ndarray, np.ndarray]] = []
                    for part in model.parts:
                        if len(part.indices) < 3:
                            continue
                        lpts = part.vertices[:, 0:3].astype(np.float64)
                        ones = np.ones((len(lpts), 1), dtype=np.float64)
                        wpts = (np.hstack([lpts, ones]) @ m4.T)[:, :3]
                        for a_i, b_i, c_i in part.indices.reshape(-1, 3):
                            new_tris.append((wpts[a_i], wpts[b_i], wpts[c_i]))

                    for t in scol["tris"]:
                        u0, u1, u2 = (np.array(v, dtype=np.float64) for v in t["verts"])
                        cx = (u0[0] + u1[0] + u2[0]) / 3.0
                        cy = (u0[1] + u1[1] + u2[1]) / 3.0
                        cz = (u0[2] + u1[2] + u2[2]) / 3.0
                        if not (min_x <= cx <= max_x and min_z <= cz <= max_z):
                            continue
                        if not (min_y_orig <= cy <= max_y_orig):
                            continue

                        # Compute lifted Y at each of the 3 SCOL corners (keep XZ exact to match Table 1 edges!)
                        lifted_corners: list[np.ndarray] = []
                        any_hit = False
                        for u in (u0, u1, u2):
                            best_y = None
                            for tv0, tv1, tv2 in new_tris:
                                hit, hy = _point_in_tri_xz(float(u[0]), float(u[2]), tv0, tv1, tv2)
                                if hit and (best_y is None or hy > best_y):
                                    best_y = hy
                            if best_y is not None:
                                any_hit = True
                                lifted_corners.append(np.array([u[0], best_y, u[2]], dtype=np.float64))
                            else:
                                # Fallback: transform original corner height via m4 relative to pivot
                                rel = u - pivot
                                ny = float(m4[1, 0] * rel[0] + m4[1, 1] * rel[1] + m4[1, 2] * rel[2] + m4[1, 3])
                                lifted_corners.append(np.array([u[0], ny, u[2]], dtype=np.float64))

                        # Only patch SCOL tris that actually overlap the ramp mesh (or original bbox)
                        in_orig = (
                            float(orig_lo[0]) - 0.3 <= cx <= float(orig_hi[0]) + 0.3
                            and float(orig_lo[2]) - 0.3 <= cz <= float(orig_hi[2]) + 0.3
                        )
                        if not any_hit and not in_orig:
                            continue
                        plane = _plane_from_tri(lifted_corners[0], lifted_corners[1], lifted_corners[2])
                        if plane is not None:
                            struct.pack_into("<4f", bsc_buf, t2 + t["index"] * 20, *plane)
                            stats["scol_planes_patched"] += 1
                            bsc_changed = True
            except Exception as exc:
                warnings.append(f"SCOL terrain collision bake warning: {exc}")

        # Mark table2[0].aux[1] = 0xFE when Sync All Events is ON so eod.exe sets 0x003D1C70 = 0xFFFF
        # activating track.bpp ramp collision across Battle / Free Roam, Figure 8, Jump Race, Gauntlet!
        if world.sync_all_events and count > 0 and t2 + 20 <= len(bsc_buf):
            if bsc_buf[t2 + 19] != 0xFE:
                bsc_buf[t2 + 19] = 0xFE
                bsc_changed = True

        if bsc_changed:
            new_bsc = bytes(bsc_buf)
            trk_bytes[bsc_e["offset"] : bsc_e["offset"] + bsc_e["size"]] = new_bsc
            struct.pack_into("<I", trk_bytes, bsc_e["crc_field"], eod_crc(new_bsc))

    return stats


def _resolve_custom_mod_and_textures(
    world: TrackWorld,
    model_key: str,
    scale: tuple[float, float, float],
) -> tuple[bytes | None, dict[str, bytes], tuple[float, float, float]]:
    """Extract raw `.mod` bytes (with scale baked into VIF) and required `.tex` bytes for a placed/swapped model."""
    wm = world.models.get(model_key)
    if wm is None:
        return None, {}, (2.0, 1.5, 4.0)

    mod_bytes: bytes | None = None
    extra_tex: dict[str, bytes] = {}

    if model_key.startswith("car:"):
        car_filename = model_key.split(":", 1)[1]
        car_path = world.game_dir / "DATA" / "CAR" / car_filename
        if not car_path.is_file():
            return None, {}, (2.0, 1.5, 4.0)
        cents = mb.directory(car_path)
        by_l = {e["name"].lower(): e for e in cents if e["type"] == "MODL"}
        pick = by_l.get("low3.mod") or by_l.get("low2.mod") or by_l.get("low1.mod")
        if pick is None:
            return None, {}, (2.0, 1.5, 4.0)
        mod_bytes, _ = mb.payload(car_path, pick["name"], cents, verify_crc=False)
        # Collect every .tex referenced by the car's parts (including default paint livery!)
        paint = None
        try:
            paint = mb.vehicle_paint(world.game_dir, car_path, include_embedded=True)
        except Exception:
            paint = None
        mhdr = wg.parse_model(mod_bytes, 0)
        for p in mhdr["parts"]:
            tname = (p.get("texture") or "").strip()
            if not tname or tname.lower() in ("-", ".tex") or tname.lower() in extra_tex:
                continue
            tdata = None
            try:
                tdata = mb.resolve_material(world.game_dir, car_path, tname, paint=paint, request=veh_exp.KIT_REQUEST)
            except Exception:
                tdata = None
            if tdata:
                extra_tex[tname.lower()] = (tname, tdata)
    elif model_key.startswith("mod:"):
        mod_name = model_key.split(":", 1)[1]
        # Check current .TRK first, then RACE.RES / HOMEBASE.TRK
        hit = next((e for e in world.entries if e["name"].lower() == mod_name.lower() and e["type"] == "MODL"), None)
        if hit is not None:
            mod_bytes, _ = mb.payload(world.trk_path, hit["name"], world.entries, verify_crc=False)
        else:
            for arc_rel in ("DATA/RES/RACE.RES", "DATA/TRACKS/HOMEBASE.TRK"):
                ap = world.game_dir / arc_rel
                if not ap.is_file():
                    continue
                aents = mb.directory(ap)
                ah = next((e for e in aents if e["name"].lower() == mod_name.lower() and e["type"] == "MODL"), None)
                if ah is not None:
                    mod_bytes, _ = mb.payload(ap, ah["name"], aents, verify_crc=False)
                    break

    if mod_bytes is None:
        return None, {}, (2.0, 1.5, 4.0)

    mod_bytes = _bake_modl_scale_bytes(mod_bytes, scale)
    dims = (
        max(0.4, float(wm.bbox_max[0] - wm.bbox_min[0]) * abs(float(scale[0]))),
        max(0.4, float(wm.bbox_max[1] - wm.bbox_min[1]) * abs(float(scale[1]))),
        max(0.4, float(wm.bbox_max[2] - wm.bbox_min[2]) * abs(float(scale[2]))),
    )
    tex_map = {tname: tdata for (tname, tdata) in extra_tex.values()}
    return mod_bytes, tex_map, dims


def _compile_custom_objects_into_trk(
    mod_trk_path: Path,
    world: TrackWorld,
    custom_objs: list[WorldObject],
    warnings: list[str],
) -> int:
    """Inject editor-placed & replaced cars/props into the modded 0TSR v6 `.TRK` archive:
      - Adds resident `.mod` (MODL v9) + missing `.tex` (TEX v4) members
      - Appends `[dyno]` (`type = wall`, `dx/dy/dz`) definitions into `obstacle.val`
      - Appends `$NNNN$object` (`dyno`, `unfloat_frame64`) entries into `c_*.va0` tables
      - Repacks deterministically via `rst0.pack_archive()` preserving 2048-byte stream alignment.
    """
    if not custom_objs:
        return 0

    with tempfile.TemporaryDirectory(prefix="eod_trk_repack_") as tmp_dir:
        rst0.unpack_archive(str(mod_trk_path), tmp_dir)
        tmp_p = Path(tmp_dir)
        manifest_p = tmp_p / "manifest.json"
        manifest = json.loads(manifest_p.read_text(encoding="utf-8"))
        manifest.pop("original_layout", None)
        entries_meta: list[dict[str, Any]] = manifest["entries"]
        existing_lower = {e["name"].lower() for e in entries_meta}

        # Load or initialize track obstacle.val
        obst_meta = next((e for e in entries_meta if e["name"].lower() == "obstacle.val"), None)
        if obst_meta is not None:
            obst_model = valt.parse_binary_valt((tmp_p / obst_meta["name"]).read_bytes())
        else:
            obst_model = {"tool_vtable": 0x40C218, "unk1": 0, "orig_sec_count": 0, "sections": [], "is_v6": False}
            entries_meta.append(
                {
                    "name": "obstacle.val",
                    "fourcc": "VALT",
                    "version": 11,
                    "is_extractable": False,
                }
            )
            existing_lower.add("obstacle.val")

        # Determine which c_*.va0 tables receive placed objects
        all_va0_names = [
            e["name"] for e in entries_meta if e["fourcc"].strip() == "VALT" and e["name"].lower().startswith("c_")
        ]
        target_va0s = list(all_va0_names)
        if not world.sync_all_events:
            tpl = world.event_templates.get(world.active_event_template)
            if tpl and tpl.va0_member and tpl.va0_member in all_va0_names:
                target_va0s = [tpl.va0_member]

        # Resolve unique (model_key, quantized_scale) -> dyno_name
        dyno_cache: dict[tuple[str, tuple[float, float, float]], str] = {}
        placed_records: list[tuple[str, WorldObject]] = []
        dyno_counter = 1

        for obj in custom_objs:
            q_scale = (round(obj.scale[0], 3), round(obj.scale[1], 3), round(obj.scale[2], 3))
            ckey = (obj.model_key, q_scale)
            if ckey not in dyno_cache:
                mod_bytes, tex_map, (dx, dy, dz) = _resolve_custom_mod_and_textures(world, obj.model_key, q_scale)
                if mod_bytes is None:
                    warnings.append(f"Skipped in-game compile for {obj.name}: no standalone MODL source")
                    continue
                dyno_name = f"edp_{dyno_counter:03d}"
                mod_member_name = f"{dyno_name}.mod"
                dyno_counter += 1

                # Write resident .mod member
                (tmp_p / mod_member_name).write_bytes(mod_bytes)
                if mod_member_name.lower() not in existing_lower:
                    entries_meta.append(
                        {
                            "name": mod_member_name,
                            "fourcc": "MODL",
                            "version": 9,
                            "is_extractable": False,
                        }
                    )
                    existing_lower.add(mod_member_name.lower())

                # Write any missing resident .tex members referenced by this .mod
                for tname, tbytes in tex_map.items():
                    if tname.lower() in existing_lower or len(tname.encode("latin1")) > 31:
                        continue
                    (tmp_p / tname).write_bytes(tbytes)
                    entries_meta.append(
                        {
                            "name": tname,
                            "fourcc": "TEX",
                            "version": 10,
                            "is_extractable": False,
                        }
                    )
                    existing_lower.add(tname.lower())

                # Append [dyno] wall definition to obstacle.val so SolBuilder creates solid collision
                obst_model["sections"].append(
                    {
                        "name": "dyno",
                        "parent": "",
                        "pairs": [
                            ("name", dyno_name),
                            ("type", "wall"),
                            ("model", mod_member_name),
                            ("dx", f"{dx:.3f}"),
                            ("dy", f"{dy:.3f}"),
                            ("dz", f"{dz:.3f}"),
                            ("drop_height", "0"),
                            ("mass", "30000"),
                            ("surf_type", "metal" if obj.model_key.startswith("car:") else "wood"),
                        ],
                        "sec_vtable": 0x40C0D8,
                        "tool_ptr": 0,
                    }
                )
                dyno_cache[ckey] = dyno_name

            placed_records.append((dyno_cache[ckey], obj))

        if not placed_records:
            return 0

        # Compile updated obstacle.val
        obst_file = next(e["name"] for e in entries_meta if e["name"].lower() == "obstacle.val")
        (tmp_p / obst_file).write_bytes(valt.compile_binary_valt(obst_model))

        # Inject placed object frames into target c_*.va0 tables
        for va0_name in target_va0s:
            va0_path = tmp_p / va0_name
            try:
                vmodel = valt.parse_binary_valt(va0_path.read_bytes())
                existing_nums = [
                    int(m.group(1))
                    for s in vmodel["sections"]
                    for m in [re.match(r"^\$(\d+)\$", s.get("name", ""))]
                    if m
                ]
                next_num = (max(existing_nums) + 1) if existing_nums else 1
                for dyno_name, obj in placed_records:
                    # Pure rigid rotation + translation matrix (scale is already baked into .mod!)
                    m4 = compose_transform(obj.position, obj.rotation_hpr, (1.0, 1.0, 1.0))
                    rows = we.matrix_to_native_rows([float(v) for v in m4.reshape(-1)])
                    frame64 = we.encode_frame64(rows)
                    sec_name = f"${next_num:04d}$object"
                    next_num += 1
                    vmodel["sections"].append(
                        {
                            "name": sec_name,
                            "parent": "",
                            "pairs": [
                                ("dyno", dyno_name),
                                ("unfloat_frame64", frame64),
                            ],
                            "sec_vtable": 0x40C0D8,
                            "tool_ptr": 0,
                        }
                    )
                va0_path.write_bytes(valt.compile_binary_valt(vmodel))
            except Exception as exc:
                warnings.append(f"Inject custom objects into {va0_name} warning: {exc}")

        manifest_p.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        out_tmp = mod_trk_path.with_suffix(".TRK.repack")
        rst0.pack_archive(tmp_dir, str(out_tmp))
        os.replace(out_tmp, mod_trk_path)
        return len(placed_records)


def export_mod_layer(
    world: TrackWorld,
    project_name: str,
    dest_dir: Path | str,
    *,
    token: Any = None,
    progress: Any = None,
) -> dict[str, Any]:
    """Write a real, playable non-destructive mod layer into `dest_dir` (never edits retail DATA/).

    1. Patches retail .TRK into `<dest_dir>/DATA/TRACKS/<TRACK>.TRK`:
       - Static / Optional-Terrain translations + deletions (Y=-5000) via `world_export.place`
       - Real VIF vertex baking (`track.grf`) for scaled/rotated/raised ramps & terrain meshes
       - Real collision baking (`track.bpp` BPPT optional-terrain ramps + `track.bsc` SCOL terrain)
       - Sync Modded Ramps & Props to All Events / Free Roam (`opt_terrain` promotion + `0xFE` marker + `c_*.va0` sync)
       - Editor-placed & replaced Cars/Wrecks/Props injected into `.TRK` (`0TSR` `.mod`/`.tex` + `obstacle.val` + `c_*.va0`)
       - SOBL barrier height / flatten edits (`track.sol`) via `world_collision.apply_box_edits`
       - ILIN racing line node X/Z edits (`*.ild`) with full tangent, length, distance & lap-length recalculation
    2. Writes Vulkan `HOSTTEX/replacements.tsv` + `.eodhtx` mipmap chains for PNG texture swaps & borrowed skies.
    3. Writes `PROFILE.TXT`, `custom_race.json`, `<slug>.eodworld.json`, and `mod_layer_manifest.json`.
    """
    out = Path(dest_dir).resolve()
    retail_data_dir = (world.game_dir / "DATA").resolve()
    try:
        out.relative_to(retail_data_dir)
        raise ValueError("Refusing to write mod layer inside retail DATA/ directory.")
    except ValueError as exc:
        if "Refusing" in str(exc):
            raise

    tracks_out = out / "DATA" / "TRACKS"
    tracks_out.mkdir(parents=True, exist_ok=True)
    mod_trk_path = tracks_out / world.trk_path.name
    if mod_trk_path.resolve() == world.trk_path.resolve():
        raise ValueError("Refusing to overwrite retail .TRK file.")

    retail_sha_before = _sha256_file(world.trk_path)
    if progress:
        progress(10, "Collecting retail placement, VIF & custom prop edits...")

    # Collect edits categorized by pipeline path
    retail_edits: list[dict[str, Any]] = []
    vif_bake_objs: list[WorldObject] = []
    all_edited_terrain_objs: list[WorldObject] = []
    custom_inject_objs: list[WorldObject] = []
    deleted_retail_count = 0
    seen_event_edits: set[tuple[str, str]] = set()

    def _add_event_edit(ev_member: str, prop_id: str, edit_dict: dict[str, Any]) -> None:
        key = (ev_member.lower(), prop_id)
        if key not in seen_event_edits:
            seen_event_edits.add(key)
            retail_edits.append(edit_dict)

    # Index event props by (model_key, rounded_initial_pos) for All-Events prop sync
    ev_props_by_sig: dict[tuple[str, int, int, int], list[WorldObject]] = {}
    for obj in world.objects:
        if obj.source == "Retail" and obj.category == "event-prop" and obj.retail_event_member:
            sig = (
                obj.model_key,
                int(round(obj.retail_initial_pos[0] * 2.0)),
                int(round(obj.retail_initial_pos[1] * 2.0)),
                int(round(obj.retail_initial_pos[2] * 2.0)),
            )
            ev_props_by_sig.setdefault(sig, []).append(obj)

    for obj in world.objects:
        if obj.deleted:
            if obj.source != "Retail":
                continue
            if obj.retail_instance_id and obj.category in ("static", "optional-terrain"):
                retail_edits.append({"instance": obj.retail_instance_id, "translate": [0.0, -5000.0, 0.0]})
                deleted_retail_count += 1
            elif obj.retail_event_member and obj.category == "event-prop":
                pid = f"e{obj.retail_event_prop_index:04d}" if obj.retail_event_prop_index is not None else obj.obj_id
                _add_event_edit(
                    obj.retail_event_member,
                    pid,
                    {"event": obj.retail_event_member, "prop": pid, "translate": [0.0, -5000.0, 0.0]},
                )
                deleted_retail_count += 1
                # Sync deletion to matching event props in other c_*.va0 tables when Sync All Events is ON
                if world.sync_all_events:
                    sig = (
                        obj.model_key,
                        int(round(obj.retail_initial_pos[0] * 2.0)),
                        int(round(obj.retail_initial_pos[1] * 2.0)),
                        int(round(obj.retail_initial_pos[2] * 2.0)),
                    )
                    for peer in ev_props_by_sig.get(sig, ()):
                        if peer is obj or not peer.retail_event_member:
                            continue
                        ppid = f"e{peer.retail_event_prop_index:04d}" if peer.retail_event_prop_index is not None else peer.obj_id
                        _add_event_edit(
                            peer.retail_event_member,
                            ppid,
                            {"event": peer.retail_event_member, "prop": ppid, "translate": [0.0, -5000.0, 0.0]},
                        )
            continue

        # Editor-placed cars/wrecks/props -> compile into .TRK via obstacle.val + c_*.va0!
        if obj.source != "Retail":
            custom_inject_objs.append(obj)
            continue

        # Replaced retail object ("Replace With...") -> hide original & inject replacement!
        if obj.retail_original_model_key and obj.model_key != obj.retail_original_model_key:
            if obj.retail_instance_id and obj.category in ("static", "optional-terrain"):
                retail_edits.append({"instance": obj.retail_instance_id, "translate": [0.0, -5000.0, 0.0]})
            elif obj.retail_event_member and obj.category == "event-prop":
                pid = f"e{obj.retail_event_prop_index:04d}" if obj.retail_event_prop_index is not None else obj.obj_id
                _add_event_edit(
                    obj.retail_event_member,
                    pid,
                    {"event": obj.retail_event_member, "prop": pid, "translate": [0.0, -5000.0, 0.0]},
                )
            custom_inject_objs.append(obj)
            continue

        dx = obj.position[0] - obj.retail_initial_pos[0]
        dy = obj.position[1] - obj.retail_initial_pos[1]
        dz = obj.position[2] - obj.retail_initial_pos[2]
        moved = abs(dx) > 1e-4 or abs(dy) > 1e-4 or abs(dz) > 1e-4
        rotated = any(abs(a - b) > 1e-4 for a, b in zip(obj.rotation_hpr, obj.retail_initial_rot))
        scaled = any(abs(s - 1.0) > 1e-4 for s in obj.scale)
        if not moved and not rotated and not scaled:
            continue

        if obj.retail_instance_id and obj.category in ("static", "optional-terrain"):
            all_edited_terrain_objs.append(obj)
            if scaled or rotated:
                # Full VIF vertex bake handles translate + rotate + scale around pivot with ground anchoring!
                vif_bake_objs.append(obj)
            elif moved:
                retail_edits.append({"instance": obj.retail_instance_id, "translate": [dx, dy, dz]})
        elif obj.retail_instance_id and obj.category == "dyno":
            m4 = compose_transform(obj.position, obj.rotation_hpr, (1.0, 1.0, 1.0))
            retail_edits.append({"instance": obj.retail_instance_id, "matrix": [float(v) for v in m4.reshape(-1)]})
        elif obj.retail_event_member and obj.category == "event-prop":
            pid = f"e{obj.retail_event_prop_index:04d}" if obj.retail_event_prop_index is not None else obj.obj_id
            if rotated:
                m4 = compose_transform(obj.position, obj.rotation_hpr, (1.0, 1.0, 1.0))
                mat_list = [float(v) for v in m4.reshape(-1)]
                _add_event_edit(
                    obj.retail_event_member,
                    pid,
                    {"event": obj.retail_event_member, "prop": pid, "matrix": mat_list},
                )
            elif moved:
                _add_event_edit(
                    obj.retail_event_member,
                    pid,
                    {"event": obj.retail_event_member, "prop": pid, "translate": [dx, dy, dz]},
                )

    if progress:
        progress(25, f"Patching {len(retail_edits)} placement records...")

    place_report: dict[str, Any] | list[Any] = {}
    place_changed_bytes = 0
    place_warnings: list[str] = []
    if retail_edits:
        with tempfile.TemporaryDirectory(prefix="eod_mod_stage_") as stage_tmp:
            staged_trk = Path(stage_tmp) / world.trk_path.name
            try:
                place_report = we.place(world.game_dir, world.trk_rel, retail_edits, staged_trk)
                place_changed_bytes = int(place_report.get("changedPayloadBytes", 0))
                shutil.copy2(staged_trk, mod_trk_path)
            except Exception as exc:
                place_warnings.append(f"world_export.place fallback: {exc}")
                shutil.copy2(world.trk_path, mod_trk_path)
    else:
        shutil.copy2(world.trk_path, mod_trk_path)

    if token is not None and token.is_cancelled:
        raise InterruptedError("Mod export cancelled")

    # Bake VIF vertex transforms (scale/rotate/raise) + BPPT & SCOL collision + All-Events ramp sync
    if progress:
        progress(40, "Baking VIF mesh vertices & BPPT/SCOL collision...")
    trk_bytes = bytearray(mod_trk_path.read_bytes())
    entries = wc.read_directory(bytes(trk_bytes))
    bake_stats = _bake_graf_vif_and_collision(
        trk_bytes, entries, world, vif_bake_objs, all_edited_terrain_objs, place_warnings
    )
    if any(bake_stats.values()) or world.sync_all_events:
        mod_trk_path.write_bytes(bytes(trk_bytes))

    # Patch modified SOBL collision barriers (track.sol)
    if progress:
        progress(55, "Patching SOBL collision barriers...")
    sobl_patched_count = 0
    if world.collision.sobl_edits:
        sobl_list = [
            (int(idx), str(ed["field"]), float(ed["value"]))
            for idx, ed in sorted(world.collision.sobl_edits.items())
        ]
        try:
            new_trk_bytes, sobl_rep = wc.apply_box_edits(mod_trk_path.read_bytes(), sobl_list, anchor="bottom")
            mod_trk_path.write_bytes(new_trk_bytes)
            sobl_patched_count = len(sobl_rep)
        except Exception as exc:
            place_warnings.append(f"SOBL barrier patch warning: {exc}")

    # Patch modified retail ILIN node X/Z positions AND recalculate tangents, segment lengths & lap length!
    if progress:
        progress(65, "Patching & recalculating ILIN racing lines...")
    ilin_patched_lines: list[str] = []
    trk_bytes = bytearray(mod_trk_path.read_bytes())
    entries = wc.read_directory(bytes(trk_bytes))
    for lname, graph in world.all_road_graphs.items():
        if not graph.dirty:
            continue
        entry = next((e for e in entries if e["name"].lower() == lname.lower()), None)
        if not entry:
            continue
        orig_ild = bytes(trk_bytes[entry["offset"] : entry["offset"] + entry["size"]])
        edits_map: dict[str, float] = {}
        for node in graph.nodes.values():
            if node.retail_index is not None:
                edits_map[f"node.{node.retail_index}.x"] = node.x
                edits_map[f"node.{node.retail_index}.z"] = node.z
        if edits_map:
            try:
                patched_ild = ilin_fmt.encode(orig_ild, edits_map, recalculate=True)
                if len(patched_ild) == entry["size"]:
                    trk_bytes[entry["offset"] : entry["offset"] + entry["size"]] = patched_ild
                    struct.pack_into("<I", trk_bytes, entry["crc_field"], eod_crc(patched_ild))
                    ilin_patched_lines.append(lname)
            except Exception as exc:
                place_warnings.append(f"ILIN {lname} patch warning: {exc}")

    if ilin_patched_lines:
        mod_trk_path.write_bytes(bytes(trk_bytes))

    # Inject editor-placed & replaced Cars/Wrecks/Props into modded .TRK (0TSR repack + obstacle.val + c_*.va0)
    custom_objects_compiled = 0
    if custom_inject_objs:
        if progress:
            progress(74, f"Compiling {len(custom_inject_objs)} placed/swapped cars & props into .TRK...")
        try:
            custom_objects_compiled = _compile_custom_objects_into_trk(
                mod_trk_path, world, custom_inject_objs, place_warnings
            )
        except Exception as exc:
            place_warnings.append(f"Custom object .TRK compile warning: {exc}")

    # Export Vulkan HOSTTEX replacements (.eodhtx + replacements.tsv)
    if progress:
        progress(84, "Building Vulkan HOSTTEX replacements...")
    slug = re.sub(r"[^A-Za-z0-9_.-]+", "_", project_name).strip("_") or "world_mod"
    host_exported, host_warnings = _export_host_textures(world, slug, out)
    place_warnings.extend(host_warnings)

    # Write PROFILE.TXT (logged by eod.exe at startup when EOD_MOD_DIR is set)
    mod_trk_sha = _sha256_file(mod_trk_path)
    profile_lines = [
        f"profile {project_name}",
        f"track {world.track_name} ({world.display_name})",
        f"native {world.trk_rel} {mod_trk_sha}",
        f"placements {len(retail_edits)} changed_bytes={place_changed_bytes} deleted={deleted_retail_count}",
        f"vif_baked models={bake_stats['vif_models_baked']} verts={bake_stats['vif_vertices_baked']}",
        f"collision_baked bppt={bake_stats['bppt_planes_patched']} scol={bake_stats['scol_planes_patched']} sobl={sobl_patched_count}",
        f"sync_all_events {world.sync_all_events} opt_terrain_promoted={bake_stats['opt_terrain_promoted']}",
        f"custom_objects_compiled {custom_objects_compiled}",
        f"ilin_lines {','.join(ilin_patched_lines) if ilin_patched_lines else 'none'}",
        f"host_textures {len(host_exported)}",
        f"environment {world.environment.preset_name} borrowed_sky={world.environment.borrowed_sky_track or 'none'}",
    ]
    (out / "PROFILE.TXT").write_text("\n".join(profile_lines) + "\n", encoding="utf-8")

    # Write custom_race.json for PIE & tools
    race_doc = dict(
        world.custom_race.to_dict(),
        track=world.track_name,
        display_name=world.display_name,
        start_grid=[
            {"name": m.name, "kind": m.kind, "position": list(m.position), "heading_deg": m.heading_deg}
            for m in world.markers
            if m.kind in ("Spawn", "Start Grid", "Car")
        ],
    )
    (out / "custom_race.json").write_text(json.dumps(race_doc, indent=2) + "\n", encoding="utf-8")

    # Write overlay project json alongside manifest
    overlay_path = save_project_overlay(out / f"{slug}.eodworld.json", project_name, world)

    for obj in world.objects:
        if not obj.deleted:
            obj.written = True
    for g in world.all_road_graphs.values():
        g.dirty = False

    retail_sha_after = _sha256_file(world.trk_path)
    if retail_sha_after != retail_sha_before:
        raise AssertionError("CRITICAL: retail .TRK SHA-256 changed during mod export!")

    manifest = {
        "schema": "eod.mod_layer/3",
        "project_name": project_name,
        "track": world.track_name,
        "source_retail_trk": str(world.trk_path),
        "retail_trk_sha256": retail_sha_after,
        "mod_trk": str(mod_trk_path),
        "mod_trk_sha256": mod_trk_sha,
        "mod_trk_differs_from_retail": mod_trk_sha != retail_sha_after,
        "overlay_json": str(overlay_path),
        "profile_txt": str(out / "PROFILE.TXT"),
        "custom_race_json": str(out / "custom_race.json"),
        "retail_placement_edits": len(retail_edits),
        "deleted_retail_objects": deleted_retail_count,
        "changed_payload_bytes": place_changed_bytes,
        "place_report_count": len(place_report.get("edits", [])) if isinstance(place_report, dict) else len(place_report),
        "vif_models_baked": bake_stats["vif_models_baked"],
        "vif_vertices_baked": bake_stats["vif_vertices_baked"],
        "opt_terrain_promoted": bake_stats["opt_terrain_promoted"],
        "bppt_planes_patched": bake_stats["bppt_planes_patched"],
        "scol_planes_patched": bake_stats["scol_planes_patched"],
        "sobl_barriers_patched": sobl_patched_count,
        "ilin_patched_lines": ilin_patched_lines,
        "custom_objects_compiled": custom_objects_compiled,
        "sync_all_events": bool(world.sync_all_events),
        "host_textures_exported": len(host_exported),
        "host_textures": host_exported,
        "editor_placed_objects": sum(
            1 for o in world.objects if o.source != "Retail" and not o.deleted
        ),
        "custom_collision_boxes": len(world.collision.custom_boxes),
        "warnings": place_warnings,
        "exported_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    manifest_path = out / "mod_layer_manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    manifest["manifest_path"] = str(manifest_path)
    if progress:
        progress(100, "Mod layer exported")
    return manifest


# ---------------------------------------------------------------------------
# Edit in Blender (OBJ + MTL + PNG export)
# ---------------------------------------------------------------------------
def export_object_obj(world: TrackWorld, obj: WorldObject, out_dir: Path | str) -> Path:
    """Export a single WorldObject to Wavefront OBJ + MTL + PNG textures (Blender Z-up)."""
    dest = Path(out_dir)
    dest.mkdir(parents=True, exist_ok=True)
    stem = re.sub(r"[^A-Za-z0-9_.-]+", "_", f"{obj.name}_{obj.obj_id}")
    obj_path = dest / f"{stem}.obj"
    mtl_path = dest / f"{stem}.mtl"

    model = world.models[obj.model_key]
    m4 = obj.world_matrix()
    lines = [
        f"# EOD World Editor OBJ export for {obj.name} ({obj.obj_id})",
        "# Axes converted to Blender RH Z-up: (x, z, y), reversed winding",
        f"mtllib {mtl_path.name}",
        f"o {stem}",
    ]
    mats: dict[str, str] = {}
    vbase = 1
    for part in model.parts:
        mat = re.sub(r"[^A-Za-z0-9_.-]+", "_", Path(part.texture).stem or "untextured")
        mats[mat] = part.texture
        lines.append(f"usemtl {mat}")
        pts = part.vertices[:, 0:3]
        ones = np.ones((len(pts), 1), dtype=np.float32)
        wpts = (np.hstack([pts, ones]) @ m4.T)[:, :3]
        uvs = part.vertices[:, 6:8]
        for p in wpts:
            lines.append(f"v {p[0]:.5f} {p[2]:.5f} {p[1]:.5f}")
        for uv in uvs:
            lines.append(f"vt {uv[0]:.5f} {1.0 - uv[1]:.5f}")
        tris = part.indices.reshape(-1, 3)
        for a, b, c in tris:
            ia, ib, ic = int(a) + vbase, int(c) + vbase, int(b) + vbase
            lines.append(f"f {ia}/{ia} {ib}/{ib} {ic}/{ic}")
        vbase += len(pts)

    obj_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    mtl_lines = [f"# Material library for {stem}"]
    for mat, tex_name in sorted(mats.items()):
        mtl_lines.extend(["", f"newmtl {mat}", "Kd 1 1 1", "Ka 0 0 0", "Ks 0 0 0", "illum 1"])
        tasset = world.textures.get(tex_name)
        if tasset is not None:
            png_name = f"{mat}.png"
            write_png(str(dest / png_name), tasset.width, tasset.height, tasset.rgba)
            mtl_lines.append(f"map_Kd {png_name}")
            if tasset.alpha_kind in ("cutout", "blend"):
                mtl_lines.append(f"map_d {png_name}")
    mtl_path.write_text("\n".join(mtl_lines) + "\n", encoding="utf-8")
    return obj_path
