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
)
import modding_backend as mb
import texture_identity as tex_id
import world_export as we
import world_collision as wc
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
       - Static / Optional-Terrain moves + deletions (Y=-5000) via `world_export.place`
       - Spawned Dyno 4x3 matrices via `world_export.place`
       - Event props (`c_<event>.va0` `unfloat_frame64`) moves, rotations & deletions
       - SOBL barrier height / flatten edits (`track.sol`) via `world_collision.apply_box_edits`
       - ILIN racing line node X/Z edits (`*.ild`) via `formats/ilin.encode`
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
        progress(10, "Collecting retail placement edits...")

    # Collect retail placement edits compatible with world_export.place
    retail_edits: list[dict[str, Any]] = []
    deleted_retail_count = 0
    for obj in world.objects:
        if obj.source != "Retail":
            continue
        if obj.deleted:
            if obj.retail_instance_id and obj.category in ("static", "optional-terrain"):
                retail_edits.append({"instance": obj.retail_instance_id, "translate": [0.0, -5000.0, 0.0]})
                deleted_retail_count += 1
            elif obj.retail_event_member and obj.category == "event-prop":
                retail_edits.append(
                    {"event": obj.retail_event_member, "prop": obj.obj_id, "translate": [0.0, -5000.0, 0.0]}
                )
                deleted_retail_count += 1
            continue

        dx = obj.position[0] - obj.retail_initial_pos[0]
        dy = obj.position[1] - obj.retail_initial_pos[1]
        dz = obj.position[2] - obj.retail_initial_pos[2]
        moved = abs(dx) > 1e-4 or abs(dy) > 1e-4 or abs(dz) > 1e-4
        rotated = any(abs(a - b) > 1e-4 for a, b in zip(obj.rotation_hpr, obj.retail_initial_rot))
        if not moved and not rotated:
            continue

        if obj.retail_instance_id and obj.category in ("static", "optional-terrain"):
            if moved:
                retail_edits.append({"instance": obj.retail_instance_id, "translate": [dx, dy, dz]})
        elif obj.retail_instance_id and obj.category == "dyno":
            m4 = compose_transform(obj.position, obj.rotation_hpr, (1.0, 1.0, 1.0))
            retail_edits.append({"instance": obj.retail_instance_id, "matrix": [float(v) for v in m4.reshape(-1)]})
        elif obj.retail_event_member and obj.category == "event-prop":
            if rotated:
                m4 = compose_transform(obj.position, obj.rotation_hpr, (1.0, 1.0, 1.0))
                retail_edits.append(
                    {
                        "event": obj.retail_event_member,
                        "prop": obj.obj_id,
                        "matrix": [float(v) for v in m4.reshape(-1)],
                    }
                )
            elif moved:
                retail_edits.append(
                    {"event": obj.retail_event_member, "prop": obj.obj_id, "translate": [dx, dy, dz]}
                )

    if progress:
        progress(30, f"Patching {len(retail_edits)} placement records...")

    place_report: dict[str, Any] | list[Any] = {}
    place_changed_bytes = 0
    place_warnings: list[str] = []
    if retail_edits:
        # Stage through an external temp directory so we.place()'s disc-prefix guard
        # never rejects D:\Test Drive - Eve of Destruction (USA)\mods\<Project>\!
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

    # Patch modified SOBL collision barriers (track.sol)
    if progress:
        progress(50, "Patching SOBL collision barriers...")
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

    # Patch modified retail ILIN node X/Z positions into the mod .TRK copy
    if progress:
        progress(65, "Patching ILIN racing lines...")
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
                patched_ild = ilin_fmt.encode(orig_ild, edits_map)
                if len(patched_ild) == entry["size"]:
                    trk_bytes[entry["offset"] : entry["offset"] + entry["size"]] = patched_ild
                    struct.pack_into("<I", trk_bytes, entry["crc_field"], eod_crc(patched_ild))
                    ilin_patched_lines.append(lname)
            except Exception as exc:
                place_warnings.append(f"ILIN {lname} patch warning: {exc}")

    if ilin_patched_lines:
        mod_trk_path.write_bytes(bytes(trk_bytes))

    # Export Vulkan HOSTTEX replacements (.eodhtx + replacements.tsv)
    if progress:
        progress(80, "Building Vulkan HOSTTEX replacements...")
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
        f"sobl_barriers {sobl_patched_count}",
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
        "schema": "eod.mod_layer/2",
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
        "sobl_barriers_patched": sobl_patched_count,
        "ilin_patched_lines": ilin_patched_lines,
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
