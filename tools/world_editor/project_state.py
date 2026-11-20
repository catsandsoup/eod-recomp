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
from copy import deepcopy
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
    load_track_world,
    parse_bppt,
    query_bppt_plane,
)
import modding_backend as mb
import mod_package
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


class SetAllWallsNoclipCommand(QUndoCommand):
    """Undoable export setting independent of the project's individual barrier edits."""

    def __init__(
        self,
        world: TrackWorld,
        enabled: bool,
        on_change: Callable[[], None],
    ):
        super().__init__(
            "Enable All Walls Noclip (Keep Ground Solid)" if enabled else "Disable All Walls Noclip (Restore Walls)"
        )
        self.world = world
        self.enabled = bool(enabled)
        self.prev_enabled = bool(world.all_walls_noclip)
        self.on_change = on_change
    def redo(self) -> None:
        self.world.all_walls_noclip = self.enabled
        self.on_change()

    def undo(self) -> None:
        self.world.all_walls_noclip = self.prev_enabled
        self.on_change()


class GroupObjectsCommand(QUndoCommand):
    """Wrap multiple selected objects into a user group (Ctrl+G)."""

    def __init__(self, world: TrackWorld, objs: list[WorldObject], on_change: Callable[[], None]):
        super().__init__(f"Group {len(objs)} objects")
        self.world = world
        self.objs = [o for o in objs if o is not None and not o.deleted]
        self.on_change = on_change
        self.prev_gids = {o.obj_id: o.user_group_id for o in self.objs}
        self.group_id = f"grp_{world._next_user_group_seq:03d}"
        world._next_user_group_seq += 1

    def redo(self) -> None:
        self.world.create_user_group(self.objs, group_id=self.group_id)
        self.on_change()

    def undo(self) -> None:
        self.world.user_groups.pop(self.group_id, None)
        for o in self.objs:
            prev = self.prev_gids.get(o.obj_id, "")
            o.user_group_id = prev
            if prev:
                self.world.user_groups.setdefault(prev, []).append(o.obj_id)
        self.on_change()


class UngroupObjectsCommand(QUndoCommand):
    """Dissolve user or auto-compound group(s) for selected objects (Ctrl+Shift+G)."""

    def __init__(self, world: TrackWorld, objs: list[WorldObject], on_change: Callable[[], None]):
        super().__init__("Ungroup selection")
        self.world = world
        self.objs = [o for o in objs if o is not None and not o.deleted]
        self.on_change = on_change
        self._snap: list[tuple[WorldObject, str, str, str]] = []

    def redo(self) -> None:
        self._snap = self.world.ungroup_objects(self.objs)
        self.on_change()

    def undo(self) -> None:
        for o, u_gid, c_gid, c_lbl in self._snap:
            o.user_group_id = u_gid
            o.compound_id = c_gid
            o.compound_label = c_lbl
            if u_gid:
                lst = self.world.user_groups.setdefault(u_gid, [])
                if o.obj_id not in lst:
                    lst.append(o.obj_id)
            elif c_gid:
                lst = self.world.compound_groups.setdefault(c_gid, [])
                if o.obj_id not in lst:
                    lst.append(o.obj_id)
        self.on_change()


class MultiDuplicateObjectsCommand(QUndoCommand):
    """Duplicate 1..N objects (or an entire compound/user group) in a single undoable step (Ctrl+D / Alt+Drag)."""

    def __init__(
        self,
        world: TrackWorld,
        clones: list[WorldObject],
        on_change: Callable[[], None],
        label: str = "Duplicate selection",
    ):
        super().__init__(label)
        self.world = world
        self.clones = list(clones)
        self.on_change = on_change

    def redo(self) -> None:
        for obj in self.clones:
            obj.deleted = False
            if obj not in self.world.objects:
                self.world.objects.append(obj)
            if obj.user_group_id:
                lst = self.world.user_groups.setdefault(obj.user_group_id, [])
                if obj.obj_id not in lst:
                    lst.append(obj.obj_id)
        self.on_change()

    def undo(self) -> None:
        for obj in self.clones:
            obj.deleted = True
            if obj in self.world.objects:
                self.world.objects.remove(obj)
            if obj.user_group_id and obj.user_group_id in self.world.user_groups:
                self.world.user_groups.pop(obj.user_group_id, None)
        self.on_change()


class EditMarkerMetaCommand(QUndoCommand):
    """Undoable marker name / kind edit (eliminates QUndoStack bypass)."""

    def __init__(
        self,
        marker: WorldMarker,
        old_name: str,
        old_kind: str,
        new_name: str,
        new_kind: str,
        on_change: Callable[[], None],
    ):
        super().__init__(f"Edit marker {new_name}")
        self.marker = marker
        self.old_name = old_name
        self.old_kind = old_kind
        self.new_name = new_name
        self.new_kind = new_kind
        self.on_change = on_change

    def redo(self) -> None:
        self.marker.name = self.new_name
        self.marker.kind = self.new_kind
        self.on_change()

    def undo(self) -> None:
        self.marker.name = self.old_name
        self.marker.kind = self.old_kind
        self.on_change()


class EditEnvironmentCommand(QUndoCommand):
    """Undoable Environment / Skybox settings edit (eliminates QUndoStack bypass)."""

    def __init__(
        self,
        world: TrackWorld,
        old_dict: dict[str, Any],
        new_dict: dict[str, Any],
        on_change: Callable[[], None],
        label: str = "Edit environment",
    ):
        super().__init__(label)
        self.world = world
        self.old_dict = dict(old_dict)
        self.new_dict = dict(new_dict)
        self.on_change = on_change

    def redo(self) -> None:
        self.world.environment = EnvironmentState.from_dict(self.new_dict)
        self.on_change()

    def undo(self) -> None:
        self.world.environment = EnvironmentState.from_dict(self.old_dict)
        self.on_change()


class BatchReplaceTexturesCommand(QUndoCommand):
    """Single undoable command replacing multiple track textures (Bulk Texture Pack / 4K-8K Skybox)."""

    def __init__(
        self,
        world: TrackWorld,
        replacements: list[tuple[str, int, int, bytes, str | None]],
        on_change: Callable[[list[str]], None],
        label: str = "Batch replace textures",
    ):
        super().__init__(label)
        self.world = world
        self.replacements = list(replacements)
        self.on_change = on_change
        self._old: dict[str, tuple[int, int, bytes, tuple[int, int, int], bool, str | None]] = {}
        for name, _w, _h, _rgba, _path in self.replacements:
            ta = world.textures.get(name)
            if ta is not None:
                self._old[name] = (
                    ta.width,
                    ta.height,
                    ta.rgba,
                    ta.average_rgb,
                    ta.is_replaced,
                    ta.replaced_png_path,
                )

    def redo(self) -> None:
        touched: list[str] = []
        for name, w, h, rgba, path in self.replacements:
            ta = self.world.textures.get(name)
            if ta is None:
                continue
            ta.width, ta.height, ta.rgba = w, h, rgba
            arr = np.frombuffer(rgba, dtype=np.uint8).reshape(-1, 4)
            op = arr[:, 3] > 0
            ta.average_rgb = tuple(int(v) for v in arr[op, :3].mean(axis=0)) if np.any(op) else (140, 140, 140)
            ta.is_replaced = True
            ta.replaced_png_path = path
            touched.append(name)
        self.on_change(touched)

    def undo(self) -> None:
        touched: list[str] = []
        for name, state in self._old.items():
            ta = self.world.textures.get(name)
            if ta is None:
                continue
            ta.width, ta.height, ta.rgba, ta.average_rgb, ta.is_replaced, ta.replaced_png_path = state
            touched.append(name)
        self.on_change(touched)


# ---------------------------------------------------------------------------
# Smart Barriers: Clamp Invisible Sky-Walls to Asset Height vs. All Walls Noclip
# ---------------------------------------------------------------------------
def plan_clamp_sky_walls_to_assets(
    world: TrackWorld,
    *,
    min_wall_above_ground: float = 1.8,
    xz_margin: float = 2.0,
    asset_clearance: float = 0.25,
    air_wall_target: float = -0.5,
) -> tuple[dict[int, float], dict[str, int]]:
    """Measure overlapping visual assets (`track.grf` / `c_*.va0`) for every vertical SOBL box.

    - If visual geometry overlaps the SOBL footprint (fences, concrete walls, buildings, parked cars):
      clamps the SOBL box top down to that asset's actual visual height (+`asset_clearance`), keeping
      physical walls 100% solid at ground level while removing invisible sky-walls above them!
    - If 0 visual assets overlap (pure invisible air-wall): flattens to `air_wall_target` (-0.5 m).
    """
    # Collect visual asset AABBs once (skip sky, shadows, deleted, giant ground sheets > 120m)
    visual_boxes: list[tuple[float, float, float, float, float]] = []
    for obj in world.objects:
        if obj.deleted or not obj.visible:
            continue
        nl = obj.name.lower()
        if nl.startswith("sky") or nl.endswith("-sh") or nl.endswith("_sh") or "shadow" in nl:
            continue
        model = world.models.get(obj.model_key)
        if model is None:
            continue
        lo, hi = obj.world_aabb(model)
        span_x = float(hi[0] - lo[0])
        span_z = float(hi[2] - lo[2])
        span_y = float(hi[1] - lo[1])
        # Ignore flat ground sheets (<0.35m tall & huge XZ) so they don't count as vertical walls
        if max(span_x, span_z) > 120.0 and span_y < 1.2:
            continue
        if span_y < 0.35:
            continue
        visual_boxes.append((float(lo[0]), float(hi[0]), float(lo[2]), float(hi[2]), float(hi[1])))

    # Fast NumPy vectorized AABB overlap check across all visual assets
    if visual_boxes:
        vb = np.array(visual_boxes, dtype=np.float32)
        v_minx, v_maxx, v_minz, v_maxz, v_maxy = vb[:, 0], vb[:, 1], vb[:, 2], vb[:, 3], vb[:, 4]
    else:
        v_minx = v_maxx = v_minz = v_maxz = v_maxy = np.zeros((0,), dtype=np.float32)

    plan: dict[int, float] = {}
    clamped_to_asset = 0
    flattened_air = 0
    untouched = 0

    for rec in world.collision.sobl_records:
        if rec.get("kind") != "box" or not rec.get("vertical"):
            continue
        cur_above = float(rec.get("above_ground", 0.0))
        if cur_above < min_wall_above_ground:
            untouched += 1
            continue
        ridx = int(rec["index"])
        cx, _cy, cz = rec["centre"]
        fx, fz = wc.footprint(rec)
        gnd_y = float(rec.get("ground_y", 0.0))
        rlo_x, rhi_x = cx - fx - xz_margin, cx + fx + xz_margin
        rlo_z, rhi_z = cz - fz - xz_margin, cz + fz + xz_margin

        if len(v_minx) > 0:
            mask = (v_maxx >= rlo_x) & (v_minx <= rhi_x) & (v_maxz >= rlo_z) & (v_minz <= rhi_z)
            if np.any(mask):
                max_vis_top = float(v_maxy[mask].max())
                vis_h = max(0.8, (max_vis_top - gnd_y) + asset_clearance)
                if cur_above > vis_h + 0.35:
                    plan[ridx] = vis_h
                    clamped_to_asset += 1
                else:
                    untouched += 1
                continue

        # 0 overlapping visual assets -> pure invisible air-wall -> flatten!
        plan[ridx] = float(air_wall_target)
        flattened_air += 1

    return plan, {
        "clamped_to_asset": clamped_to_asset,
        "flattened_air": flattened_air,
        "untouched": untouched,
        "total_modified": len(plan),
    }


# ---------------------------------------------------------------------------
# Real In-Game Skybox (Presets + 4K/8K HDRI Equirectangular) + Bulk Texture Pack
# ---------------------------------------------------------------------------
def write_env_override_ini(world: TrackWorld, out_dir: Path) -> Path:
    """Write track-scoped lighting and fog settings for native Plume Modern."""
    env = world.environment
    sd = env.sun_direction()
    defaults = EnvironmentState()
    authored = any(getattr(env, field) != getattr(defaults, field) for field in (
        "sun_azimuth", "sun_elevation", "sun_color", "ambient_intensity", "fog_color",
        "fog_start", "fog_end", "fog_density",
    ))
    lines = [
        "[environment]",
        f"track={world.track_name}",
        f"enabled={int(authored)}",
        f"preset={env.preset_name}",
        f"sun_azimuth={env.sun_azimuth:.3f}",
        f"sun_elevation={env.sun_elevation:.3f}",
        f"sun_dir={sd[0]:.5f},{sd[1]:.5f},{sd[2]:.5f}",
        f"sun_color={env.sun_color[0]:.4f},{env.sun_color[1]:.4f},{env.sun_color[2]:.4f}",
        f"ambient_intensity={env.ambient_intensity:.4f}",
        f"fog_color={env.fog_color[0]:.4f},{env.fog_color[1]:.4f},{env.fog_color[2]:.4f}",
        f"fog_start={env.fog_start:.2f}",
        f"fog_end={env.fog_end:.2f}",
        f"fog_density={env.fog_density:.4f}",
        f"sky_zenith={env.sky_zenith[0]:.4f},{env.sky_zenith[1]:.4f},{env.sky_zenith[2]:.4f}",
        f"sky_mid={env.sky_mid[0]:.4f},{env.sky_mid[1]:.4f},{env.sky_mid[2]:.4f}",
        f"sky_horizon={env.sky_horizon[0]:.4f},{env.sky_horizon[1]:.4f},{env.sky_horizon[2]:.4f}",
        f"sky_ground={env.sky_ground[0]:.4f},{env.sky_ground[1]:.4f},{env.sky_ground[2]:.4f}",
        f"borrowed_sky_track={env.borrowed_sky_track or ''}",
        f"custom_skybox_path={env.custom_skybox_path or ''}",
    ]
    p = out_dir / "env_override.ini"
    p.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return p


def decode_hdr_rgbe(hdr_path: Path | str) -> tuple[int, int, bytes]:
    """Pure-Python Radiance `.hdr` (RGBE / XYZE) decoder + ACES filmic tone-map to sRGB RGBA8."""
    raw = Path(hdr_path).read_bytes()
    # Parse ASCII header until blank line
    pos = 0
    end_hdr = raw.find(b"\n\n")
    if end_hdr < 0:
        end_hdr = raw.find(b"\r\n\r\n")
        if end_hdr < 0:
            raise ValueError("Invalid Radiance .hdr header (missing blank line)")
        pos = end_hdr + 4
    else:
        pos = end_hdr + 2
    nl = raw.find(b"\n", pos)
    if nl < 0:
        raise ValueError("Invalid Radiance .hdr resolution string")
    res_line = raw[pos:nl].decode("ascii", errors="ignore").strip()
    pos = nl + 1
    m = re.search(r"([+-])Y\s+(\d+)\s+([+-])X\s+(\d+)", res_line)
    if not m:
        raise ValueError(f"Unsupported Radiance .hdr resolution line: {res_line!r}")
    h = int(m.group(2))
    w = int(m.group(4))
    rgbe = np.zeros((h, w, 4), dtype=np.uint8)
    n_raw = len(raw)

    for y in range(h):
        if pos + 4 > n_raw:
            break
        b0, b1, b2, b3 = raw[pos], raw[pos + 1], raw[pos + 2], raw[pos + 3]
        if b0 == 2 and b1 == 2 and (b2 & 0x80) == 0 and ((b2 << 8) | b3) == w:
            # New-style RLE scanline
            pos += 4
            for ch in range(4):
                x = 0
                while x < w and pos < n_raw:
                    code = raw[pos]
                    pos += 1
                    if code > 128:
                        run = code - 128
                        val = raw[pos] if pos < n_raw else 0
                        pos += 1
                        end_x = min(w, x + run)
                        rgbe[y, x:end_x, ch] = val
                        x = end_x
                    else:
                        end_x = min(w, x + code)
                        n_cp = end_x - x
                        rgbe[y, x:end_x, ch] = np.frombuffer(raw[pos : pos + n_cp], dtype=np.uint8)
                        pos += n_cp
                        x = end_x
        else:
            # Uncompressed RGBE scanline
            n_bytes = w * 4
            chunk = raw[pos : pos + n_bytes]
            if len(chunk) < n_bytes:
                break
            rgbe[y] = np.frombuffer(chunk, dtype=np.uint8).reshape(w, 4)
            pos += n_bytes

    if m.group(1) == "+":
        rgbe = rgbe[::-1]
    if m.group(3) == "-":
        rgbe = rgbe[:, ::-1]

    # Convert RGBE -> linear float32 RGB -> ACES filmic tone map -> sRGB u8 RGBA
    exp = rgbe[:, :, 3].astype(np.float32)
    scale = np.where(exp > 0, np.ldexp(1.0, (exp - 136.0).astype(np.int32)), 0.0)
    rgb = rgbe[:, :, :3].astype(np.float32) * scale[:, :, None]
    # ACES Narkowicz filmic tone-mapping
    a, b, c, d, e = 2.51, 0.03, 2.43, 0.59, 0.14
    mapped = np.clip((rgb * (a * rgb + b)) / (rgb * (c * rgb + d) + e), 0.0, 1.0)
    srgb = np.clip(np.power(mapped, 1.0 / 2.2) * 255.0 + 0.5, 0.0, 255.0).astype(np.uint8)
    out = np.empty((h, w, 4), dtype=np.uint8)
    out[:, :, :3] = srgb
    out[:, :, 3] = 255
    return w, h, out.tobytes()


def load_sky_image_rgba(image_path: Path | str) -> tuple[int, int, bytes]:
    """Load `.hdr`, `.png`, `.jpg`, or `.jpeg` skybox image as sRGB RGBA8 bytes."""
    p = Path(image_path)
    if p.suffix.lower() == ".hdr":
        return decode_hdr_rgbe(p)
    return load_png_rgba(p)


def _render_preset_sky_tile_rgba(
    env: EnvironmentState,
    tile_index_1based: int,
    is_upper_b: bool,
    width: int = 128,
    height: int = 128,
    base_rgba: bytes | None = None,
    base_size: tuple[int, int] | None = None,
) -> bytes:
    """Synthesize a graded atmospheric sky dome tile (`sky01A..16B`) matching `EnvironmentState`."""
    # Azimuth sector for tile 1..16 (22.5° per tile around 360°)
    az0 = ((tile_index_1based - 1) % 16) * (2.0 * math.pi / 16.0)
    az1 = az0 + (2.0 * math.pi / 16.0)
    el_bot = math.radians(42.0 if is_upper_b else -4.0)
    el_top = math.radians(88.0 if is_upper_b else 44.0)

    u = np.linspace(0.0, 1.0, width, dtype=np.float32)[None, :]
    v = np.linspace(1.0, 0.0, height, dtype=np.float32)[:, None]
    az = az0 + u * (az1 - az0)
    el = el_bot + v * (el_top - el_bot)
    ce = np.cos(el)
    dx = np.sin(az) * ce
    dy = np.sin(el)
    dz = np.cos(az) * ce

    y = np.clip(dy, -1.0, 1.0)
    t1 = np.clip(y / 0.25, 0.0, 1.0)
    t1 = t1 * t1 * (3.0 - 2.0 * t1)
    t2 = np.clip((y - 0.25) / 0.60, 0.0, 1.0)
    t2 = t2 * t2 * (3.0 - 2.0 * t2)

    hz = np.array(env.sky_horizon, dtype=np.float32)
    md = np.array(env.sky_mid, dtype=np.float32)
    zn = np.array(env.sky_zenith, dtype=np.float32)
    gr = np.array(env.sky_ground, dtype=np.float32)
    col = hz * (1.0 - t1[:, :, None]) + md * t1[:, :, None]
    col = col * (1.0 - t2[:, :, None]) + zn * t2[:, :, None]
    below = np.clip(-y / 0.18, 0.0, 1.0)
    col = np.where(y[:, :, None] >= 0.0, col, hz * (1.0 - below[:, :, None]) + gr * below[:, :, None])

    sd = np.array(env.sun_direction(), dtype=np.float32)
    sdot = np.clip(dx * sd[0] + dy * sd[1] + dz * sd[2], 0.0, 1.0)
    sc = np.array(env.sun_color, dtype=np.float32)
    glow = (np.power(sdot, 22.0) * 0.42)[:, :, None] * sc
    disc = (np.clip((sdot - 0.9965) / 0.0025, 0.0, 1.0) * 1.10)[:, :, None] * sc
    col = np.clip(col + glow + disc, 0.0, 1.0)

    # Blend subtle cloud/horizon silhouette detail from retail sky tile if provided
    if base_rgba and base_size:
        bw, bh = base_size
        bimg = Image.frombytes("RGBA", (bw, bh), base_rgba)
        if (bw, bh) != (width, height):
            bimg = bimg.resize((width, height), Image.Resampling.BILINEAR)
        barr = np.asarray(bimg, dtype=np.float32)[:, :, :3] / 255.0
        luma = barr[:, :, 0] * 0.299 + barr[:, :, 1] * 0.587 + barr[:, :, 2] * 0.114
        # Preserve cloud contrast subtly without pulling daytime blue back into sunset/night!
        cloud_mod = np.clip(0.78 + luma * 0.42, 0.65, 1.25)[:, :, None]
        col = np.clip(col * cloud_mod, 0.0, 1.0)

    rgba = np.empty((height, width, 4), dtype=np.uint8)
    rgba[:, :, :3] = np.clip(col * 255.0 + 0.5, 0.0, 255.0).astype(np.uint8)
    rgba[:, :, 3] = 255
    return rgba.tobytes()


def project_equirectangular_to_sky_tiles(
    world: TrackWorld,
    image_path: Path | str,
    *,
    tile_size: int = 512,
) -> list[tuple[str, int, int, bytes, str | None]]:
    """Project a 4K/8K equirectangular `.hdr`/`.png`/`.jpg` panorama onto EOD's `sky01A..16B` dome tiles.

    Returns a list of `(tex_name, width, height, rgba_bytes, None)` ready for `BatchReplaceTexturesCommand`
    and Vulkan `HOSTTEX` baking!
    """
    src_w, src_h, src_rgba = load_sky_image_rgba(image_path)
    pano = np.frombuffer(src_rgba, dtype=np.uint8).reshape(src_h, src_w, 4)

    # Find all sky01A..16B textures (both in world.textures and .TRK entries)
    sky_names: list[tuple[str, int, bool]] = []
    seen_low: set[str] = set()
    for name in sorted(world.textures.keys()):
        m = re.fullmatch(r"sky(\d\d)([ab])\.tex", name, re.I)
        if m:
            sky_names.append((name, int(m.group(1)), m.group(2).lower() == "b"))
            seen_low.add(name.lower())
    # Also ensure sky textures from .TRK entries are loaded into world.textures if not already present
    for e in world.entries:
        if e["type"] == "TEX":
            m = re.fullmatch(r"sky(\d\d)([ab])\.tex", e["name"], re.I)
            if m and e["name"].lower() not in seen_low:
                try:
                    raw_b, _ = mb.payload(world.trk_path, e["name"], world.entries, verify_crc=False)
                    tw, th, trgba, meta = decode_preview(raw_b)
                    from world_loader import TextureAsset
                    world.textures[e["name"]] = TextureAsset(
                        name=e["name"],
                        width=tw,
                        height=th,
                        rgba=trgba,
                        alpha_kind="opaque",
                        average_rgb=(128, 128, 128),
                        asset_id=f"{world.trk_rel}#{e['name']}",
                        orig_width=tw,
                        orig_height=th,
                        orig_rgba=trgba,
                        raw_tex_bytes=raw_b,
                    )
                    sky_names.append((e["name"], int(m.group(1)), m.group(2).lower() == "b"))
                    seen_low.add(e["name"].lower())
                except Exception:
                    continue

    # Scale target tile size cleanly from source panorama (supports up to 1024x1024 per tile for 4K/8K!)
    tw_out = min(1024, max(128, tile_size))
    if src_w >= 4096:
        tw_out = min(1024, max(tw_out, 512))
    elif src_w <= 1024:
        tw_out = min(tw_out, 256)
    # Force power-of-two tile dimension compatible with EODHTX1
    tw_pow = 1
    while tw_pow * 2 <= tw_out:
        tw_pow *= 2
    tw_out = max(128, tw_pow)

    u_grid = np.linspace(0.0, 1.0, tw_out, endpoint=False, dtype=np.float32)[None, :]
    v_grid = np.linspace(0.0, 1.0, tw_out, endpoint=False, dtype=np.float32)[:, None]

    out: list[tuple[str, int, int, bytes, str | None]] = []
    for name, idx_1, is_upper in sky_names:
        az_u0 = ((idx_1 - 1) % 16) / 16.0
        az_u = (az_u0 + u_grid / 16.0) % 1.0
        # Upper band B covers zenith -> 45 deg (equirectangular V in [0.0, 0.25])
        # Lower band A covers 45 deg -> horizon (equirectangular V in [0.25, 0.52])
        if is_upper:
            eq_v = v_grid * 0.25
        else:
            eq_v = 0.25 + v_grid * 0.27
        px = np.clip((az_u * (src_w - 1)).astype(np.int32), 0, src_w - 1)
        py = np.clip((eq_v * (src_h - 1)).astype(np.int32), 0, src_h - 1)
        tile = np.ascontiguousarray(pano[py, px], dtype=np.uint8)
        tile[:, :, 3] = 255
        out.append((name, tw_out, tw_out, tile.tobytes(), None))
    return out


def export_all_track_textures(world: TrackWorld, out_dir: Path | str) -> dict[str, Any]:
    """Export every decoded track texture to `<out_dir>/<tex_name>.png` + `manifest.json`."""
    dest = Path(out_dir).resolve()
    if dest.is_relative_to((world.game_dir / "DATA").resolve()):
        raise ValueError("Choose a texture export folder outside the retail DATA directory.")
    dest.mkdir(parents=True, exist_ok=True)
    items: list[dict[str, Any]] = []
    for name, ta in sorted(world.textures.items()):
        if name.startswith("car/"):
            continue
        out_png = dest / f"{Path(name).name}.png"
        write_png(str(out_png), ta.width, ta.height, ta.rgba)
        ikey, _pkey = ta.ensure_identity()
        items.append(
            {
                "name": name,
                "file": out_png.name,
                "width": ta.width,
                "height": ta.height,
                "orig_width": ta.orig_width,
                "orig_height": ta.orig_height,
                "alpha_kind": ta.alpha_kind,
                "hosttex_eligible": bool(ikey),
                "asset_id": ta.asset_id,
            }
        )
    manifest = {
        "schema": "eod.texture_pack/1",
        "track": world.track_name,
        "count": len(items),
        "textures": items,
    }
    (dest / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def scan_import_texture_folder(
    world: TrackWorld,
    folder_path: Path | str,
) -> tuple[list[tuple[str, int, int, bytes, str | None]], list[str]]:
    """Scan a folder for `<tex_name>.png` matching `world.textures` for 1-click Bulk HOSTTEX Import."""
    src = Path(folder_path)
    if not src.is_dir():
        return [], [f"Folder not found: {src}"]
    by_stem: dict[str, str] = {}
    by_full: dict[str, str] = {}
    for name in world.textures:
        if name.startswith("car/"):
            continue
        by_full[name.lower()] = name
        by_stem[Path(name).stem.lower()] = name

    replacements: list[tuple[str, int, int, bytes, str | None]] = []
    imported_targets: set[str] = set()
    skipped: list[str] = []
    for p in sorted(src.iterdir()):
        if not p.is_file() or p.suffix.lower() not in (".png", ".jpg", ".jpeg", ".tga", ".bmp"):
            continue
        stem_l = p.stem.lower()
        target = by_full.get(stem_l) or by_full.get(f"{stem_l}.tex") or by_stem.get(stem_l)
        if not target:
            skipped.append(p.name)
            continue
        if target in imported_targets:
            skipped.append(f"{p.name} (duplicate match for {target})")
            continue
        try:
            w, h, rgba = load_png_rgba(p)
            replacements.append((target, w, h, rgba, str(p.resolve())))
            imported_targets.add(target)
        except Exception as exc:
            skipped.append(f"{p.name} ({exc})")
    return replacements, skipped


# ---------------------------------------------------------------------------
# Vulkan Host Texture Replacement (.eodhtx) Builder (Contract Section 4)
# ---------------------------------------------------------------------------
def _nearest_host_scale(orig_w: int, orig_h: int, src_w: int, src_h: int) -> int:
    """Pick a power-of-two scale within the shared HOSTTEX dimension limit."""
    if not 1 <= orig_w <= mod_package.MAX_SIDE or not 1 <= orig_h <= mod_package.MAX_SIDE:
        raise ValueError("Original texture dimensions are outside the HOSTTEX limit")
    max_k = 1
    while max(orig_w, orig_h) * (max_k * 2) <= mod_package.MAX_SIDE:
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
# ---------------------------------------------------------------------------
# Project Overlay (.eodworld.json) Save / Load & Crash-Safe Atomic Autosave
# ---------------------------------------------------------------------------
def build_project_overlay_dict(
    project_name: str,
    world: TrackWorld,
    camera_state: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Snapshot the non-destructive project overlay dictionary on the UI thread (<1 ms)."""
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

    return {
        "schema": "eod.world_project/2",
        "project_name": project_name,
        "saved_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "track": world.track_name,
        "trk_rel": world.trk_rel,
        "active_road_line": world.road_graph.line_name,
        "camera": dict(camera_state) if camera_state else {},
        "environment": world.environment.to_dict(),
        "custom_race": world.custom_race.to_dict(),
        "replaced_textures": replaced_tex_doc,
        "sobl_barrier_edits": sobl_edits_doc,
        "all_walls_noclip": bool(world.all_walls_noclip),
        "user_groups": dict(world.user_groups),
        "modified_retail_objects": modified_retail,
        "added_objects": added_objects,
        "deleted_object_ids": deleted_ids,
        "custom_collision_boxes": list(world.collision.custom_boxes),
        "roads": roads_doc,
        "markers": markers_doc,
    }


def atomic_write_json(
    target_path: Path | str,
    doc: dict[str, Any],
    *,
    tmp_suffix: str = ".autosave.tmp",
    compact: bool = False,
) -> Path:
    """Crash-safe atomic JSON write (`<target><tmp_suffix>` -> `os.replace`)."""
    out = Path(target_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + tmp_suffix)
    text = (json.dumps(doc, separators=(",", ":")) if compact else json.dumps(doc, indent=2)) + "\n"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
        f.flush()
    os.replace(tmp, out)
    return out


def _autosave_dir() -> Path:
    override = os.environ.get("EOD_EDITOR_APPDATA")
    if override:
        base = Path(override)
    else:
        local = os.environ.get("LOCALAPPDATA")
        base = (Path(local) / "EODWorldEditor") if local else (Path.home() / ".eod_world_editor")
    d = base / "autosave"
    d.mkdir(parents=True, exist_ok=True)
    return d


def autosave_path_for(
    project_path: Path | str | None,
    track_name: str = "TRACK",
    project_name: str = "Untitled",
) -> Path:
    """Return the canonical autosave file path for either a saved or unsaved project."""
    if project_path is not None:
        p = Path(project_path)
        return p.with_name(p.name + ".autosave.json")
    slug = re.sub(r"[^A-Za-z0-9_.-]+", "_", project_name).strip("_") or "untitled"
    trk = re.sub(r"[^A-Za-z0-9_.-]+", "_", track_name.upper()).strip("_") or "TRACK"
    return _autosave_dir() / f"{trk}_{slug}.eodworld.json.autosave.json"


def save_autosave_snapshot(
    target_path: Path | str,
    doc: dict[str, Any],
    *,
    token: Any = None,
    progress: Any = None,
) -> Path:
    """Background-worker friendly atomic autosave (`<project>.autosave.tmp` -> `os.replace`)."""
    if token is not None and token.is_cancelled:
        raise InterruptedError("Autosave cancelled")
    return atomic_write_json(Path(target_path), doc, tmp_suffix=".autosave.tmp", compact=True)


def find_newer_autosave(
    project_path: Path | str | None,
    track_name: str = "",
    project_name: str = "",
) -> Path | None:
    """Return autosave path if it exists, is valid JSON, and is newer than the last manual save."""
    cand = autosave_path_for(project_path, track_name or "TRACK", project_name or "Untitled")
    if not cand.is_file():
        return None
    try:
        auto_mtime = cand.stat().st_mtime
        if project_path is not None and Path(project_path).is_file():
            manual_mtime = Path(project_path).stat().st_mtime
            if auto_mtime <= manual_mtime + 0.05:
                return None
        # Validate JSON integrity before prompting user
        parsed = json.loads(cand.read_text(encoding="utf-8"))
        if not isinstance(parsed, dict) or not parsed.get("track"):
            return None
        return cand
    except Exception:
        return None


def clear_autosave(
    project_path: Path | str | None,
    track_name: str = "",
    project_name: str = "",
) -> None:
    cand = autosave_path_for(project_path, track_name or "TRACK", project_name or "Untitled")
    for p in (cand, cand.with_name(cand.name + ".autosave.tmp")):
        try:
            if p.is_file():
                p.unlink()
        except Exception:
            pass


def save_project_overlay(
    path: Path | str,
    project_name: str,
    world: TrackWorld,
    camera_state: dict[str, Any] | None = None,
) -> Path:
    """Save non-destructive .eodworld.json overlay atomically. Never touches retail .TRK."""
    out_path = Path(path)
    if not out_path.name.lower().endswith(".eodworld.json"):
        out_path = out_path.with_suffix(".eodworld.json")
    doc = build_project_overlay_dict(project_name, world, camera_state)
    return atomic_write_json(out_path, doc, tmp_suffix=".tmp", compact=False)


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
    world.all_walls_noclip = bool(doc.get("all_walls_noclip", False))
    ug = doc.get("user_groups", {})
    if isinstance(ug, dict):
        world.user_groups = {str(k): list(v) for k, v in ug.items() if isinstance(v, list)}
        by_oid = {o.obj_id: o for o in world.objects}
        for gid, oids in world.user_groups.items():
            for oid in oids:
                if oid in by_oid:
                    by_oid[oid].user_group_id = gid

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

    # 3. Real In-Game Skybox Presets (Sunset, Night, Overcast, Desert, Clear Day):
    #    Bake graded sky01A..16B HOSTTEX .eodhtx + reference PNGs so Sunset renders in eod.exe!
    preset_name = world.environment.preset_name
    if (
        preset_name
        and preset_name != "Track Default"
        and not (borrowed and borrowed.upper() != world.track_name.upper())
    ):
        dst_sky = [
            e
            for e in world.entries
            if e["type"] == "TEX" and re.fullmatch(r"sky(\d\d)([ab])\.tex", e["name"], re.I)
        ]
        for dst_e in sorted(dst_sky, key=lambda x: x["name"].lower()):
            # Skip if user already explicitly replaced this sky tile (e.g., via 4K/8K HDRI importer)
            ta_existing = world.textures.get(dst_e["name"])
            if ta_existing is not None and ta_existing.is_replaced:
                continue
            m_sky = re.fullmatch(r"sky(\d\d)([ab])\.tex", dst_e["name"], re.I)
            if not m_sky:
                continue
            try:
                dst_raw, _ = mb.payload(world.trk_path, dst_e["name"], world.entries, verify_crc=False)
                ident = tex_id.texture_identity(dst_raw)
                if not ident.get("indexKey"):
                    continue
                bw, bh, brgba, _meta = decode_preview(dst_raw)
                tile_idx = int(m_sky.group(1))
                is_upper = m_sky.group(2).lower() == "b"
                tile_w = max(128, int(ident["width"]))
                tile_h = max(128, int(ident["height"]))
                srgba = _render_preset_sky_tile_rgba(
                    world.environment,
                    tile_idx,
                    is_upper,
                    width=tile_w,
                    height=tile_h,
                    base_rgba=brgba,
                    base_size=(bw, bh),
                )
                emit_entry(
                    srgba,
                    tile_w,
                    tile_h,
                    int(ident["width"]),
                    int(ident["height"]),
                    ident["indexKey"],
                    f"{world.trk_rel}#{dst_e['name']}",
                    f"sky_preset_{dst_e['name']}",
                )
            except Exception as exc:
                warnings.append(f"Preset sky export for {dst_e['name']} failed: {exc}")

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


def _collect_part_non_pos_row_offs(orig_data: bytes, part: dict[str, Any]) -> set[int]:
    """Return STROW offsets used by any non-position UNPACK (e.g. UV) in `part`."""
    non_pos: set[int] = set()
    for b in vif_patch.scan_part(orig_data, part):
        if b.get("count", 0) <= 0:
            continue
        if "uv" in b and b["uv"].get("row_off", -1) >= 0:
            non_pos.add(int(b["uv"]["row_off"]))
    return non_pos


def _encode_vif_parts_vertices(
    buf: bytearray,
    orig_data: bytes,
    parts_with_xforms: list[tuple[dict[str, Any], Callable[[tuple[float, float, float]], tuple[float, float, float]]]],
    *,
    forbidden_row_offs: set[int] | None = None,
) -> tuple[list[tuple[float, float, float]], int]:
    """Transform and re-encode VIF position vertices across one or more parts in place.

    Groups batches by `row_off` across ALL provided parts so raising a shared STROW
    exponent refits every user consistently, and NEVER mutates any STROW offset listed in
    `forbidden_row_offs` (e.g. STROWs referenced by unbaked models or non-position UV lanes).
    """
    all_valid: list[tuple[dict[str, Any], list[tuple[float, float, float]]]] = []
    all_new_pts: list[tuple[float, float, float]] = []
    blocked_rows: set[int] = set(forbidden_row_offs) if forbidden_row_offs else set()

    for part, transform_pt in parts_with_xforms:
        batches = vif_patch.scan_part(orig_data, part)
        for b in batches:
            if b.get("count", 0) <= 0:
                continue
            if "uv" in b and b["uv"].get("row_off", -1) >= 0:
                blocked_rows.add(int(b["uv"]["row_off"]))
            if "pos" in b and "q199" in b:
                old_pts = vif_patch.read_batch(orig_data, b)["p"]
                new_pts = [transform_pt(p) for p in old_pts]
                all_valid.append((b, new_pts))
                all_new_pts.extend(new_pts)

    if not all_valid:
        return [], 0

    # Group ALL affected batches across ALL parts by their STROW offset
    by_row: dict[int, list[int]] = {}
    for bi, (b, _pts) in enumerate(all_valid):
        by_row.setdefault(int(b["pos"]["row_off"]), []).append(bi)

    changed_verts = 0
    for row_off, grp in by_row.items():
        first_sl = all_valid[grp[0]][0]["pos"]
        orig_row = list(first_sl["row"])
        new_row = list(orig_row)
        min_bits = min(int(all_valid[bi][0]["pos"]["bits"]) for bi in grp)
        can_mutate_strow = min_bits < 32 and row_off >= 0 and row_off not in blocked_rows

        if can_mutate_strow:
            for c in range(3):
                # Find tightest exponent >= cur_exp that fits EVERY batch in grp (even if mixed 8/16-bit)
                need_raise = False
                for bi in grp:
                    b_bits = int(all_valid[bi][0]["pos"]["bits"])
                    if b_bits >= 32:
                        continue
                    b_top = (1 << b_bits) - 1
                    vals_c = [p[c] for p in all_valid[bi][1]]
                    span_c = max(vals_c) - min(vals_c)
                    if span_c > (b_top - 2) * vif_patch.ulp_of(orig_row[c]):
                        need_raise = True
                        break
                chosen_row = orig_row[c]
                if need_raise:
                    cur_exp = (orig_row[c] >> 23) & 0xFF
                    for e in range(cur_exp + 1, 160):
                        cand = e << 23
                        ulp_c = vif_patch.ulp_of(cand)
                        fits_all = True
                        for bi in grp:
                            b_bits = int(all_valid[bi][0]["pos"]["bits"])
                            if b_bits >= 32:
                                continue
                            b_top = (1 << b_bits) - 1
                            vals_c = [p[c] for p in all_valid[bi][1]]
                            if (b_top - 2) * ulp_c < (max(vals_c) - min(vals_c)) * 1.02:
                                fits_all = False
                                break
                        if fits_all:
                            chosen_row = cand
                            break
                new_row[c] = chosen_row
            if new_row != orig_row:
                struct.pack_into("<3I", buf, row_off, *new_row)

        for bi in grp:
            b, want = all_valid[bi]
            sl = b["pos"]
            bits = int(sl["bits"])
            top = (1 << bits) - 1
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


def _encode_vif_part_vertices(
    buf: bytearray,
    orig_data: bytes,
    part: dict[str, Any],
    transform_pt: Callable[[tuple[float, float, float]], tuple[float, float, float]],
    *,
    forbidden_row_offs: set[int] | None = None,
) -> tuple[list[tuple[float, float, float]], int]:
    """Transform and re-encode every VIF vertex in `part` in place, safely handling shared STROWs."""
    return _encode_vif_parts_vertices(
        buf,
        orig_data,
        [(part, transform_pt)],
        forbidden_row_offs=forbidden_row_offs,
    )


def _bake_modl_scale_bytes(mod_bytes: bytes, scale: tuple[float, float, float]) -> bytes:
    """Bake non-unit `(sx, sy, sz)` directly into a standalone `.mod` file's VIF vertices + bounds."""
    sx, sy, sz = (float(scale[0]), float(scale[1]), float(scale[2]))
    if abs(sx - 1.0) <= 1e-4 and abs(sy - 1.0) <= 1e-4 and abs(sz - 1.0) <= 1e-4:
        return mod_bytes
    mhdr = wg.parse_model(mod_bytes, 0)
    buf = bytearray(mod_bytes)
    scale_fn = lambda p: (p[0] * sx, p[1] * sy, p[2] * sz)
    all_pts, _cnt = _encode_vif_parts_vertices(
        buf,
        mod_bytes,
        [(part, scale_fn) for part in mhdr["parts"]],
    )
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
    """Compute upward-pointing unit plane `(a, b, c, d)` for `y = -(a*x + c*z + d) / b`.

    Supports steep/tilted 3D ramps and berms up to ~89.4 deg (`n.y >= 0.01`).
    """
    n = np.cross(v1 - v0, v2 - v0)
    nl = float(np.linalg.norm(n))
    if nl < 1e-7:
        return None
    n = n / nl
    if n[1] < 0.0:
        n = -n
    if n[1] < 0.01:
        return None
    d = -float(np.dot(n, v0))
    return (float(n[0]), float(n[1]), float(n[2]), d)


def _synthesize_static_sobl_boxes(world: TrackWorld, obj: WorldObject) -> list[dict[str, Any]]:
    """Build 1..4 tight solid `SOBL` BOX definitions covering a transformed static mesh."""
    model = world.models.get(obj.model_key)
    if model is None:
        return []
    m4, _pivot = _object_baked_matrix(world, obj)
    pts_list: list[np.ndarray] = []
    for part in model.parts:
        if len(part.vertices) == 0:
            continue
        lpts = part.vertices[:, 0:3].astype(np.float64)
        ones = np.ones((len(lpts), 1), dtype=np.float64)
        wpts = (np.hstack([lpts, ones]) @ m4.T)[:, :3]
        pts_list.append(wpts)
    if not pts_list:
        lo, hi = obj.world_aabb(model)
        wpts_all = np.array([lo, hi], dtype=np.float64)
    else:
        wpts_all = np.vstack(pts_list)

    lo = wpts_all.min(axis=0)
    hi = wpts_all.max(axis=0)
    span_x = float(hi[0] - lo[0])
    span_z = float(hi[2] - lo[2])
    max_horiz = max(span_x, span_z)

    # Split wide mountains/berms (> 80 m) into up to 4 tightly fitted segments along their longest axis
    n_segs = 1 if max_horiz <= 80.0 else min(4, max(2, int(math.ceil(max_horiz / 70.0))))
    axis = 0 if span_x >= span_z else 2
    boxes: list[dict[str, Any]] = []
    ident_rows = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]

    if n_segs == 1:
        rx = max(0.35, 0.5 * float(hi[0] - lo[0]))
        ry = max(0.35, 0.5 * float(hi[1] - lo[1]))
        rz = max(0.35, 0.5 * float(hi[2] - lo[2]))
        cx = 0.5 * float(lo[0] + hi[0])
        cy = 0.5 * float(lo[1] + hi[1])
        cz = 0.5 * float(lo[2] + hi[2])
        boxes.append({"centre": [cx, cy, cz], "half_extents": [rx, ry, rz], "rows": ident_rows, "surface_type": 110})
        return boxes

    a_lo = float(lo[axis])
    a_hi = float(hi[axis])
    step = (a_hi - a_lo) / n_segs
    for s in range(n_segs):
        s0 = a_lo + s * step - 0.5
        s1 = a_lo + (s + 1) * step + 0.5
        mask = (wpts_all[:, axis] >= s0) & (wpts_all[:, axis] <= s1)
        sub = wpts_all[mask]
        if len(sub) == 0:
            continue
        slo = sub.min(axis=0)
        shi = sub.max(axis=0)
        rx = max(0.35, 0.5 * float(shi[0] - slo[0]))
        # Anchor every hill segment down to the mesh base `lo[1]` so cars cannot sneak under edge slices
        base_y = float(lo[1])
        ry = max(0.35, 0.5 * float(shi[1] - base_y))
        rz = max(0.35, 0.5 * float(shi[2] - slo[2]))
        cx = 0.5 * float(slo[0] + shi[0])
        cy = 0.5 * float(base_y + shi[1])
        cz = 0.5 * float(slo[2] + shi[2])
        boxes.append({"centre": [cx, cy, cz], "half_extents": [rx, ry, rz], "rows": ident_rows, "surface_type": 110})

    if not boxes:
        rx = max(0.35, 0.5 * float(hi[0] - lo[0]))
        ry = max(0.35, 0.5 * float(hi[1] - lo[1]))
        rz = max(0.35, 0.5 * float(hi[2] - lo[2]))
        boxes.append(
            {
                "centre": [0.5 * float(lo[0] + hi[0]), 0.5 * float(lo[1] + hi[1]), 0.5 * float(lo[2] + hi[2])],
                "half_extents": [rx, ry, rz],
                "rows": ident_rows,
                "surface_type": 110,
            }
        )
    return boxes


def _append_sobl_boxes(sol_bytes: bytes, new_boxes: list[dict[str, Any]]) -> bytes:
    """Append solid `BOX` records into a retail `track.sol` (`SOBL` v3) member and update its quadtree.

    Clones a retail `BOX` template record so all native vtable/shape-ctor/box_flags bytes remain
    100% valid for SLUS_209.10 `f_001A00B8` / `f_00177CA0`, inserts each new record index into
    the deepest enclosing quadtree node, and preserves every existing record and quadtree cell.
    """
    if not new_boxes or len(sol_bytes) < wc.SOBL_HEADER:
        return sol_bytes
    parsed = wc.parse_sobl(sol_bytes)
    orig_count = int(parsed["count"])
    if orig_count + len(new_boxes) >= 0xFFFF:
        return sol_bytes

    # Find an existing retail BOX record to use as byte-accurate binary template
    tmpl_rec = next((r for r in parsed["records"] if r.get("kind") == "box"), None)
    if tmpl_rec is None:
        return sol_bytes
    tmpl_off = int(tmpl_rec["offset"])
    tmpl_bytes = sol_bytes[tmpl_off : tmpl_off + wc.SOBL_RECORD]

    # Build new 0x108-byte BOX records
    appended_recs = bytearray()
    new_box_meta: list[tuple[int, float, float, float, float]] = []  # (rec_idx, x0, x1, z0, z1)
    for bi, spec in enumerate(new_boxes):
        rec_idx = orig_count + bi
        rbuf = bytearray(tmpl_bytes)
        rows = spec.get("rows") or [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]
        cx, cy, cz = (float(v) for v in spec["centre"])
        rx, ry, rz = (max(0.1, float(v)) for v in spec["half_extents"])
        sr = math.sqrt(rx * rx + ry * ry + rz * rz) + wc.SPHERE_PAD
        surf = int(spec.get("surface_type", 110))
        struct.pack_into(
            "<9f", rbuf, 0x00,
            float(rows[0][0]), float(rows[0][1]), float(rows[0][2]),
            float(rows[1][0]), float(rows[1][1]), float(rows[1][2]),
            float(rows[2][0]), float(rows[2][1]), float(rows[2][2]),
        )
        struct.pack_into("<3f", rbuf, 0x24, cx, cy, cz)
        struct.pack_into("<i", rbuf, 0x30, -1)
        struct.pack_into("<I", rbuf, 0x34, surf)
        struct.pack_into("<4f", rbuf, 0x60, rx, ry, rz, sr)
        appended_recs.extend(rbuf)
        hx = sum(abs(float(rows[k][0])) * (rx, ry, rz)[k] for k in range(3))
        hz = sum(abs(float(rows[k][2])) * (rx, ry, rz)[k] for k in range(3))
        new_box_meta.append((rec_idx, cx - hx, cx + hx, cz - hz, cz + hz))

    # Pick the deepest quadtree node whose XZ bounds fully enclose each new box (fallback: root node 0)
    cells = wc.quadtree_cells(parsed)
    additions_by_node: dict[int, list[int]] = {}
    for rec_idx, bx0, bx1, bz0, bz1 in new_box_meta:
        best_node = 0
        best_depth = -1
        for nidx, x0, x1, z0, z1, depth in cells:
            if x0 <= bx0 and bx1 <= x1 and z0 <= bz0 and bz1 <= z1:
                if depth > best_depth:
                    best_depth = depth
                    best_node = nidx
        additions_by_node.setdefault(best_node, []).append(rec_idx)

    # Rebuild u16 index list + quadtree nodes preserving every existing node's exact index slice
    orig_index = parsed["index"]
    orig_nodes = parsed["nodes"]
    new_index: list[int] = []
    new_nodes: list[tuple[int, int, int, int]] = []
    for nidx, (first, end, child, pad) in enumerate(orig_nodes):
        extra = additions_by_node.get(nidx)
        slice_items = orig_index[first:end]
        if slice_items or extra:
            nf = len(new_index)
            new_index.extend(slice_items)
            if extra:
                new_index.extend(extra)
            ne = len(new_index)
        else:
            nf = ne = len(new_index)
        new_nodes.append((nf, ne, child, pad))

    # Assemble updated SOBL v3 binary
    new_count = orig_count + len(new_boxes)
    out = bytearray()
    out.extend(struct.pack("<5I", new_count, len(new_index), 0, 0, 0))
    out.extend(sol_bytes[wc.SOBL_HEADER : parsed["list_offset"]])
    out.extend(appended_recs)
    if new_index:
        out.extend(struct.pack(f"<{len(new_index)}H", *new_index))
    for nf, ne, child, pad in new_nodes:
        out.extend(struct.pack("<4H", nf, ne, child, pad))
    return bytes(out)


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


def _grow_ancestor_spheres(
    graf_buf: bytearray,
    leaves: list[dict[str, Any]],
    nc: tuple[float, float, float],
    nrad: float,
) -> bool:
    """Grow all ancestor `NODE_SPHERE` cull radii across `leaves` so scaled/moved meshes never cull out."""
    changed = False
    for leaf in leaves:
        for step in leaf.get("path", ()):
            if step.get("kind") != "sphere":
                continue
            n_off = int(step["node"])
            cx, cy, cz, r = struct.unpack_from("<4f", graf_buf, n_off + 0x0C)
            need = math.dist(nc, (cx, cy, cz)) + float(nrad)
            if need > r:
                struct.pack_into("<f", graf_buf, n_off + 0x18, float(need * 1.02 + 0.5))
                changed = True
    return changed


def _bake_graf_vif_and_collision(
    trk_bytes: bytearray,
    entries: list[dict[str, Any]],
    world: TrackWorld,
    vif_objs: list[WorldObject],
    all_edited_terrain_objs: list[WorldObject],
    warnings: list[str],
    auto_sobl_boxes_out: list[dict[str, Any]] | None = None,
) -> dict[str, int]:
    """Bake VIF vertex transforms into `track.grf`, promote opt_terrain when syncing all events,
    bake matching collision planes into `track.bpp` (BPPT) and `track.bsc` (SCOL), scale
    overlapping `track.sol` (SOBL) barrier heights, and synthesize SOBL boxes for visual-only
    edited static terrain meshes with 0 overlapping retail SCOL triangles.
    """
    stats = {
        "vif_models_baked": 0,
        "vif_vertices_baked": 0,
        "opt_terrain_promoted": 0,
        "bppt_planes_patched": 0,
        "scol_planes_patched": 0,
        "sobl_scaled_barriers": 0,
        "sobl_boxes_synthesized": 0,
    }

    # --- 1. Patch track.grf (VIF scale/rotate/translate + NODE_SPHERE bounds + opt_terrain promotion) ---
    graf_e = next((e for e in entries if e["type"] == "GRAF"), None)
    if graf_e is not None:
        graf_orig = bytes(trk_bytes[graf_e["offset"] : graf_e["offset"] + graf_e["size"]])
        graf_buf = bytearray(graf_orig)
        graf_parsed = wg.parse_graf(graf_orig)
        by_id = {f"i{i:04d}": leaf for i, leaf in enumerate(graf_parsed["leaves"])}
        leaves_by_mod: dict[int, list[dict[str, Any]]] = {}
        for leaf in graf_parsed["leaves"]:
            mo = leaf.get("model_offset")
            if mo:
                leaves_by_mod.setdefault(int(mo), []).append(leaf)
        mlists = [info for info in graf_parsed["mlist_info"] if info]
        graf_changed = False

        # Determine which model offsets will be VIF-baked and guard any STROW used by unbaked models!
        bake_targets: list[tuple[int, WorldObject, dict[str, Any]]] = []
        baked_offsets: set[int] = set()
        for obj in vif_objs:
            if not obj.retail_instance_id or obj.retail_instance_id not in by_id:
                continue
            leaf = by_id[obj.retail_instance_id]
            mod_off = leaf.get("model_offset")
            if not mod_off or int(mod_off) in baked_offsets:
                continue
            baked_offsets.add(int(mod_off))
            bake_targets.append((int(mod_off), obj, leaf))

        forbidden_row_offs: set[int] = set()
        if bake_targets:
            # Collect every STROW offset referenced by ANY unbaked model in track.grf so we NEVER corrupt them
            for mo in leaves_by_mod:
                if mo in baked_offsets:
                    continue
                try:
                    uhdr = wg.parse_model(graf_orig, mo)
                    for upart in uhdr["parts"]:
                        for ub in vif_patch.scan_part(graf_orig, upart):
                            if ub.get("count", 0) <= 0:
                                continue
                            for role in ("pos", "uv"):
                                if role in ub and ub[role].get("row_off", -1) >= 0:
                                    forbidden_row_offs.add(int(ub[role]["row_off"]))
                except Exception:
                    continue

        for mod_off, obj, leaf in bake_targets:
            try:
                mhdr = wg.parse_model(graf_orig, mod_off)
                if obj.category == "dyno":
                    sx, sy, sz = (float(obj.scale[0]), float(obj.scale[1]), float(obj.scale[2]))

                    def _xform(p: tuple[float, float, float], _sx=sx, _sy=sy, _sz=sz) -> tuple[float, float, float]:
                        return (p[0] * _sx, p[1] * _sy, p[2] * _sz)
                else:
                    m4, pivot = _object_baked_matrix(world, obj)

                    def _xform(p: tuple[float, float, float], _m4=m4, _piv=pivot) -> tuple[float, float, float]:
                        lx = p[0] - _piv[0]
                        ly = p[1] - _piv[1]
                        lz = p[2] - _piv[2]
                        nx = _m4[0, 0] * lx + _m4[0, 1] * ly + _m4[0, 2] * lz + _m4[0, 3]
                        ny = _m4[1, 0] * lx + _m4[1, 1] * ly + _m4[1, 2] * lz + _m4[1, 3]
                        nz = _m4[2, 0] * lx + _m4[2, 1] * ly + _m4[2, 2] * lz + _m4[2, 3]
                        return (float(nx), float(ny), float(nz))

                all_pts, obj_vcount = _encode_vif_parts_vertices(
                    graf_buf,
                    graf_orig,
                    [(part, _xform) for part in mhdr["parts"]],
                    forbidden_row_offs=forbidden_row_offs,
                )

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
                    # Grow ALL ancestor NODE_SPHERE cull radii for every leaf referencing this model
                    _grow_ancestor_spheres(graf_buf, leaves_by_mod.get(mod_off, [leaf]), nc, nrad)

                    stats["vif_models_baked"] += 1
                    stats["vif_vertices_baked"] += obj_vcount
                    graf_changed = True
            except Exception as exc:
                warnings.append(f"VIF bake warning for {obj.name} ({obj.obj_id}): {exc}")

        # Also grow ancestor NODE_SPHERE cull radii for translated-only static/optional-terrain edits
        for obj in all_edited_terrain_objs:
            if not obj.retail_instance_id or obj.retail_instance_id not in by_id:
                continue
            leaf = by_id[obj.retail_instance_id]
            mod_off = leaf.get("model_offset")
            if not mod_off or int(mod_off) in baked_offsets:
                continue
            nrad = struct.unpack_from("<f", graf_buf, int(mod_off) + 0x18)[0]
            nc = struct.unpack_from("<3f", graf_buf, int(mod_off) + 0x1C)
            if _grow_ancestor_spheres(graf_buf, leaves_by_mod.get(int(mod_off), [leaf]), nc, float(nrad)):
                graf_changed = True

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
    static_edited = [o for o in all_edited_terrain_objs if o.category == "static"]
    scol_hits_by_obj: dict[str, int] = {o.obj_id: 0 for o in static_edited}
    bsc_e = next((e for e in entries if e["name"].lower() == "track.bsc" and e["type"] == "SCOL"), None)
    if bsc_e is not None:
        bsc_orig = bytes(trk_bytes[bsc_e["offset"] : bsc_e["offset"] + bsc_e["size"]])
        bsc_buf = bytearray(bsc_orig)
        bsc_changed = False
        count, t1, t2 = struct.unpack_from("<3I", bsc_orig, 0)

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
                    min_y_orig = float(orig_lo[1]) - 6.0
                    max_y_orig = float(orig_hi[1]) + 6.0

                    # Collect transformed triangles with precomputed XZ AABBs + 3D planes for fast sampling
                    new_tris: list[tuple[np.ndarray, np.ndarray, np.ndarray, float, float, float, float, float, float]] = []
                    for part in model.parts:
                        if len(part.indices) < 3:
                            continue
                        lpts = part.vertices[:, 0:3].astype(np.float64)
                        ones = np.ones((len(lpts), 1), dtype=np.float64)
                        wpts = (np.hstack([lpts, ones]) @ m4.T)[:, :3]
                        for a_i, b_i, c_i in part.indices.reshape(-1, 3):
                            v0, v1, v2 = wpts[a_i], wpts[b_i], wpts[c_i]
                            tx0 = min(float(v0[0]), float(v1[0]), float(v2[0])) - 0.1
                            tx1 = max(float(v0[0]), float(v1[0]), float(v2[0])) + 0.1
                            tz0 = min(float(v0[2]), float(v1[2]), float(v2[2])) - 0.1
                            tz1 = max(float(v0[2]), float(v1[2]), float(v2[2])) + 0.1
                            tcx = (float(v0[0]) + float(v1[0]) + float(v2[0])) / 3.0
                            tcz = (float(v0[2]) + float(v1[2]) + float(v2[2])) / 3.0
                            new_tris.append((v0, v1, v2, tx0, tx1, tz0, tz1, tcx, tcz))

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
                            ux, uz = float(u[0]), float(u[2])
                            best_y = None
                            for tv0, tv1, tv2, tx0, tx1, tz0, tz1, _tcx, _tcz in new_tris:
                                if not (tx0 <= ux <= tx1 and tz0 <= uz <= tz1):
                                    continue
                                hit, hy = _point_in_tri_xz(ux, uz, tv0, tv1, tv2)
                                if hit and (best_y is None or hy > best_y):
                                    best_y = hy
                            if best_y is not None:
                                any_hit = True
                                lifted_corners.append(np.array([ux, best_y, uz], dtype=np.float64))
                            else:
                                # Nearest transformed triangle plane fallback (handles pitched/rolled edges cleanly)
                                near_y = None
                                near_d2 = 4.0  # within 2.0 m in XZ
                                for tv0, tv1, tv2, _tx0, _tx1, _tz0, _tz1, tcx, tcz in new_tris:
                                    d2 = (ux - tcx) * (ux - tcx) + (uz - tcz) * (uz - tcz)
                                    if d2 < near_d2:
                                        tp = _plane_from_tri(tv0, tv1, tv2)
                                        if tp is not None and tp[1] >= 0.05:
                                            near_d2 = d2
                                            near_y = -(tp[0] * ux + tp[2] * uz + tp[3]) / tp[1]
                                if near_y is not None:
                                    lifted_corners.append(np.array([ux, near_y, uz], dtype=np.float64))
                                else:
                                    rel = u - pivot
                                    ny = float(m4[1, 0] * rel[0] + m4[1, 1] * rel[1] + m4[1, 2] * rel[2] + m4[1, 3])
                                    lifted_corners.append(np.array([ux, ny, uz], dtype=np.float64))

                        # Only patch SCOL tris that actually overlap the ramp/hill mesh (or original bbox)
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
                            scol_hits_by_obj[obj.obj_id] = scol_hits_by_obj.get(obj.obj_id, 0) + 1
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

    # --- 4. Scale matching SOBL barrier heights overlapping edited static meshes + auto-solidify 0-SCOL hills ---
    sol_e = next((e for e in entries if e["name"].lower() == "track.sol" and e["type"] == "SOBL"), None)
    if static_edited and sol_e is not None:
        sol_orig = bytes(trk_bytes[sol_e["offset"] : sol_e["offset"] + sol_e["size"]])
        sol_buf = bytearray(sol_orig)
        sol_changed = False
        try:
            sobl_parsed = wc.parse_sobl(sol_orig)
            sobl_records = sobl_parsed["records"]
            scaled_rec_idxs: set[int] = set()

            for obj in static_edited:
                model = world.models.get(obj.model_key)
                if model is None:
                    continue
                m4, pivot = _object_baked_matrix(world, obj)
                orig_lo = pivot + model.bbox_min.astype(np.float64)
                orig_hi = pivot + model.bbox_max.astype(np.float64)
                new_lo_f, new_hi_f = obj.world_aabb(model)
                new_top_y = float(new_hi_f[1])
                orig_top_y = float(orig_hi[1])
                sy = abs(float(obj.scale[1]))

                min_x = min(float(orig_lo[0]), float(new_lo_f[0])) - 1.0
                max_x = max(float(orig_hi[0]), float(new_hi_f[0])) + 1.0
                min_z = min(float(orig_lo[2]), float(new_lo_f[2])) - 1.0
                max_z = max(float(orig_hi[2]), float(new_hi_f[2])) + 1.0

                overlapping_boxes: list[dict[str, Any]] = []
                for rec in sobl_records:
                    if rec.get("kind") != "box":
                        continue
                    cx, _cy, cz = rec["centre"]
                    fx, fz = wc.footprint(rec)
                    if (cx + fx) >= min_x and (cx - fx) <= max_x and (cz + fz) >= min_z and (cz - fz) <= max_z:
                        overlapping_boxes.append(rec)

                # Scale matching vertical SOBL barrier heights (dy) when static wall/berm grows in Y or rises
                if overlapping_boxes and (sy > 1.01 or new_top_y > orig_top_y + 0.25):
                    for rec in overlapping_boxes:
                        ridx = int(rec["index"])
                        if ridx in world.collision.sobl_edits or ridx in scaled_rec_idxs:
                            continue
                        if abs(float(rec["rows"][1][1])) < wc.VERTICAL_LIMIT:
                            continue
                        orig_rx, orig_ry, orig_rz = (float(v) for v in rec["half_extents"])
                        orig_lo_y, orig_hi_y = wc.vertical_span(rec)
                        target_top = max(orig_hi_y, new_top_y)
                        target_ry = max(orig_ry * max(1.0, sy), 0.5 * (target_top - orig_lo_y))
                        if target_ry > orig_ry + 0.05:
                            new_cy = orig_lo_y + target_ry
                            new_sr = math.sqrt(orig_rx * orig_rx + target_ry * target_ry + orig_rz * orig_rz) + wc.SPHERE_PAD
                            off = int(rec["offset"])
                            struct.pack_into("<3f", sol_buf, off + 0x24, float(rec["centre"][0]), float(new_cy), float(rec["centre"][2]))
                            struct.pack_into("<f", sol_buf, off + 0x64, float(target_ry))
                            struct.pack_into("<f", sol_buf, off + 0x6C, float(new_sr))
                            scaled_rec_idxs.add(ridx)
                            stats["sobl_scaled_barriers"] += 1
                            sol_changed = True

                # Auto-synthesize solid SOBL collision boxes if this static mesh has 0 overlapping retail SCOL tris
                # AND no overlapping retail SOBL boxes (e.g. visual-only outskirts hills/mountains)
                if scol_hits_by_obj.get(obj.obj_id, 0) == 0 and auto_sobl_boxes_out is not None:
                    new_min_x, new_max_x = float(new_lo_f[0]) - 0.5, float(new_hi_f[0]) + 0.5
                    new_min_z, new_max_z = float(new_lo_f[2]) - 0.5, float(new_hi_f[2]) + 0.5
                    has_sobl_at_new = any(
                        (r["centre"][0] + wc.footprint(r)[0]) >= new_min_x
                        and (r["centre"][0] - wc.footprint(r)[0]) <= new_max_x
                        and (r["centre"][2] + wc.footprint(r)[1]) >= new_min_z
                        and (r["centre"][2] - wc.footprint(r)[1]) <= new_max_z
                        for r in overlapping_boxes
                    )
                    if not has_sobl_at_new:
                        synth = _synthesize_static_sobl_boxes(world, obj)
                        if synth:
                            auto_sobl_boxes_out.extend(synth)
                            stats["sobl_boxes_synthesized"] += len(synth)

            if sol_changed:
                new_sol = bytes(sol_buf)
                trk_bytes[sol_e["offset"] : sol_e["offset"] + sol_e["size"]] = new_sol
                struct.pack_into("<I", trk_bytes, sol_e["crc_field"], eod_crc(new_sol))
        except Exception as exc:
            warnings.append(f"SOBL height/synthesis bake warning: {exc}")

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
        # Check current .TRK first, then RACE.RES / HOMEBASE.TRK / COMMON.RES
        hit = next((e for e in world.entries if e["name"].lower() == mod_name.lower() and e["type"] == "MODL"), None)
        if hit is not None:
            mod_bytes, _ = mb.payload(world.trk_path, hit["name"], world.entries, verify_crc=False)
        else:
            for arc_rel in ("DATA/RES/RACE.RES", "DATA/TRACKS/HOMEBASE.TRK", "DATA/RES/COMMON.RES"):
                ap = world.game_dir / arc_rel
                if not ap.is_file():
                    continue
                aents = mb.directory(ap)
                ah = next((e for e in aents if e["name"].lower() == mod_name.lower() and e["type"] == "MODL"), None)
                if ah is not None:
                    mod_bytes, _ = mb.payload(ap, ah["name"], aents, verify_crc=False)
                    # If the model came from a non-resident archive (e.g. HOMEBASE.TRK), pull referenced textures too
                    if arc_rel.upper().endswith(".TRK"):
                        mhdr_tmp = wg.parse_model(mod_bytes, 0)
                        a_tex = {e["name"].lower(): e for e in aents if e["type"] == "TEX"}
                        for p in mhdr_tmp["parts"]:
                            tname = (p.get("texture") or "").strip()
                            if not tname or tname.lower() in ("-", ".tex") or tname.lower() in extra_tex:
                                continue
                            te = a_tex.get(tname.lower())
                            if te is not None:
                                tdata, _ = mb.payload(ap, te["name"], aents, verify_crc=False)
                                extra_tex[tname.lower()] = (te["name"], tdata)
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


def _encode_road_graph_to_ild(orig_ild: bytes, graph: RoadGraph) -> tuple[bytes, bool]:
    """Encode `graph` into valid binary ILIN (`.ild`) bytes with 3D `(x, y, z)` heights.

    Returns `(patched_bytes, resized)`:
      - When node topology is unchanged (all retail nodes present, no added/split/deleted nodes),
        runs the fast in-place `ilin.encode()` preserving exact file size (`resized=False`).
      - When nodes were added, pulled out, split, or deleted (`retail_index is None` or count changed),
        traverses `graph.edges` into ordered main-loop + side-branch lists and calls
        `ilin.encode(..., rebuild_main=..., rebuild_side=...)` rebuilding the binary pointer
        fix-up table and recalculating 3D segment lengths & lap length (`resized=True`).
    """
    orig_lay = ilin_fmt._layout(orig_ild)
    retail_count = int(orig_lay["count"])
    has_new_nodes = any(n.retail_index is None for n in graph.nodes.values())
    present_retail = {int(n.retail_index) for n in graph.nodes.values() if n.retail_index is not None}
    nodes_deleted = len(present_retail) < retail_count

    if not has_new_nodes and not nodes_deleted:
        edits_map: dict[str, float] = {}
        node_heights: dict[int, float] = {}
        for node in graph.nodes.values():
            if node.retail_index is not None:
                edits_map[f"node.{node.retail_index}.x"] = node.x
                edits_map[f"node.{node.retail_index}.z"] = node.z
                node_heights[int(node.retail_index)] = float(node.y)
        if not edits_map:
            return orig_ild, False
        return ilin_fmt.encode(orig_ild, edits_map, recalculate=True, node_heights=node_heights), False

    # Build directed adjacency from graph.edges (preserving main-loop vs side-branch ordering)
    out_adj: dict[int, list[int]] = {}
    in_adj: dict[int, list[int]] = {}
    for e in graph.edges:
        if e.a in graph.nodes and e.b in graph.nodes and e.a != e.b:
            out_adj.setdefault(e.a, []).append(e.b)
            in_adj.setdefault(e.b, []).append(e.a)

    # Pick start node: retail node 0 if still present, else smallest retail_index non-side node, else first node
    main_candidates = [n for n in graph.nodes.values() if not n.is_side]
    if not main_candidates:
        main_candidates = list(graph.nodes.values())
    start_node = next((n for n in main_candidates if n.retail_index == 0), None)
    if start_node is None:
        start_node = min(
            main_candidates,
            key=lambda n: (n.retail_index if n.retail_index is not None else 10**9, n.node_id),
        )

    # Walk primary loop preferring non-side successors (and avoiding backtracking)
    main_order: list[int] = []
    main_set: set[int] = set()
    cur = start_node.node_id
    closed = False
    while cur is not None and cur not in main_set:
        main_order.append(cur)
        main_set.add(cur)
        succs = out_adj.get(cur, [])
        nxt = None
        # First prefer an unvisited non-side successor
        for s in succs:
            if s not in main_set and not graph.nodes[s].is_side:
                nxt = s
                break
        if nxt is None:
            # Check if we closed back onto start_node
            if len(main_order) >= 3 and start_node.node_id in succs:
                closed = True
                break
            # Otherwise take any unvisited successor
            for s in succs:
                if s not in main_set:
                    nxt = s
                    break
        cur = nxt

    # Include any remaining non-side nodes that were disconnected (append along nearest point in main_order)
    for n in sorted(main_candidates, key=lambda x: (x.retail_index if x.retail_index is not None else 10**9, x.node_id)):
        if n.node_id not in main_set:
            main_order.append(n.node_id)
            main_set.add(n.node_id)

    # Ensure at least 2 main nodes
    if len(main_order) < 2:
        return orig_ild, False

    main_idx_by_nid = {nid: idx for idx, nid in enumerate(main_order)}
    main_specs = [
        {
            "x": float(graph.nodes[nid].x),
            "y": float(graph.nodes[nid].y),
            "z": float(graph.nodes[nid].z),
            "retail_index": graph.nodes[nid].retail_index,
            "width": float(graph.nodes[nid].width),
        }
        for nid in main_order
    ]

    # Collect side-branch / pulled-out nodes (any remaining nodes not in main_set)
    side_nids = [nid for nid in graph.nodes if nid not in main_set]
    side_idx_by_nid = {nid: len(main_order) + j for j, nid in enumerate(side_nids)}
    side_specs: list[dict[str, Any]] = []
    for nid in side_nids:
        nd = graph.nodes[nid]
        # Find nearest main node as join anchor
        best_m = 0
        best_d = 1e18
        for m_i, mnid in enumerate(main_order):
            mn = graph.nodes[mnid]
            d = (mn.x - nd.x) ** 2 + (mn.z - nd.z) ** 2
            if d < best_d:
                best_d = d
                best_m = m_i
        # Resolve outgoing next pointer (prefer explicit edge target)
        nxt_combined: int | None = None
        for s in out_adj.get(nid, []):
            if s in side_idx_by_nid:
                nxt_combined = side_idx_by_nid[s]
                break
            if s in main_idx_by_nid:
                nxt_combined = main_idx_by_nid[s]
                break
        if nxt_combined is None:
            nxt_combined = best_m
        side_specs.append(
            {
                "x": float(nd.x),
                "y": float(nd.y),
                "z": float(nd.z),
                "retail_index": nd.retail_index,
                "join_main": best_m,
                "next_idx": nxt_combined,
            }
        )

    rebuilt = ilin_fmt.encode(
        orig_ild,
        {},
        recalculate=True,
        rebuild_main=main_specs,
        rebuild_side=side_specs if side_specs else None,
        closed=closed or True,
    )
    return rebuilt, (len(rebuilt) != len(orig_ild))


def _compile_custom_objects_into_trk(
    mod_trk_path: Path,
    world: TrackWorld,
    custom_objs: list[WorldObject],
    warnings: list[str],
    extra_sobl_boxes: list[dict[str, Any]] | None = None,
    extra_members: dict[str, bytes] | None = None,
    all_walls_noclip: bool = False,
) -> int:
    """Inject editor-placed, replaced & scaled cars/props (plus synthesized SOBL boxes,
    rebuilt `.ild` road lines, and optional `all_walls_noclip` obstacle.val wall flatten)
    into the modded 0TSR v6 `.TRK` archive cleanly.
    """
    if not custom_objs and not extra_sobl_boxes and not extra_members and not all_walls_noclip:
        return 0

    with tempfile.TemporaryDirectory(prefix="eod_trk_repack_") as tmp_dir:
        rst0.unpack_archive(str(mod_trk_path), tmp_dir)
        tmp_p = Path(tmp_dir)
        manifest_p = tmp_p / "manifest.json"
        manifest = json.loads(manifest_p.read_text(encoding="utf-8"))
        manifest.pop("original_layout", None)
        entries_meta: list[dict[str, Any]] = manifest["entries"]
        existing_lower = {e["name"].lower() for e in entries_meta}

        sobl_appended = False
        if extra_sobl_boxes:
            sol_meta = next((e for e in entries_meta if e["name"].lower() == "track.sol"), None)
            if sol_meta is not None:
                sol_path = tmp_p / sol_meta["name"]
                orig_sol = sol_path.read_bytes()
                new_sol = _append_sobl_boxes(orig_sol, extra_sobl_boxes)
                if new_sol != orig_sol:
                    sol_path.write_bytes(new_sol)
                    sobl_appended = True

        members_replaced = False
        if extra_members:
            by_low = {e["name"].lower(): e["name"] for e in entries_meta}
            for mname, mbytes in extra_members.items():
                actual = by_low.get(mname.lower())
                if actual:
                    (tmp_p / actual).write_bytes(mbytes)
                    members_replaced = True

        noclip_patched = False
        if all_walls_noclip:
            obst_meta_nc = next((e for e in entries_meta if e["name"].lower() == "obstacle.val"), None)
            if obst_meta_nc is not None:
                try:
                    obst_p_nc = tmp_p / obst_meta_nc["name"]
                    om_nc = valt.parse_binary_valt(obst_p_nc.read_bytes())
                    changed_nc = False
                    for sec in om_nc.get("sections", []):
                        pairs = sec.get("pairs", [])
                        is_wall = any(str(k).lower() == "type" and str(v).lower() == "wall" for k, v in pairs)
                        if is_wall:
                            new_pairs = []
                            for k, v in pairs:
                                kl = str(k).lower()
                                if kl in ("dx", "dy", "dz"):
                                    new_pairs.append((k, "0.010"))
                                    changed_nc = True
                                elif kl == "drop_height":
                                    new_pairs.append((k, "-500"))
                                    changed_nc = True
                                else:
                                    new_pairs.append((k, v))
                            sec["pairs"] = new_pairs
                    if changed_nc:
                        obst_p_nc.write_bytes(valt.compile_binary_valt(om_nc))
                        noclip_patched = True
                except Exception as exc:
                    warnings.append(f"All Walls Noclip obstacle.val patch warning: {exc}")

        placed_records: list[tuple[str, WorldObject]] = []
        if custom_objs:
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
            va0_by_l = {n.lower(): n for n in all_va0_names}
            base_target_va0s = list(all_va0_names)
            if not world.sync_all_events:
                tpl = world.event_templates.get(world.active_event_template)
                if tpl and tpl.va0_member and tpl.va0_member in all_va0_names:
                    base_target_va0s = [tpl.va0_member]

            # Resolve unique (model_key, quantized_scale) -> dyno_name
            dyno_cache: dict[tuple[str, tuple[float, float, float]], str] = {}
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

                    if obj.model_key.startswith("car:"):
                        surf_type = "metal"
                    elif obj.category == "event-prop" or "wall" in obj.name.lower() or "obsw" in obj.model_key.lower():
                        surf_type = "wall"
                    else:
                        surf_type = "wood"

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
                                ("surf_type", surf_type),
                            ],
                            "sec_vtable": 0x40C0D8,
                            "tool_ptr": 0,
                        }
                    )
                    dyno_cache[ckey] = dyno_name

                placed_records.append((dyno_cache[ckey], obj))

            if placed_records:
                # Compile updated obstacle.val
                obst_file = next(e["name"] for e in entries_meta if e["name"].lower() == "obstacle.val")
                (tmp_p / obst_file).write_bytes(valt.compile_binary_valt(obst_model))

                # Build per-va0 injection list so scaled retail event-props ALWAYS land in their own va0 too
                by_va0: dict[str, list[tuple[str, WorldObject]]] = {}
                for dyno_name, obj in placed_records:
                    if world.sync_all_events:
                        dests = all_va0_names
                    else:
                        dests = list(base_target_va0s)
                        if obj.retail_event_member:
                            exact = va0_by_l.get(obj.retail_event_member.lower())
                            if exact and exact not in dests:
                                dests.append(exact)
                    for vn in dests:
                        by_va0.setdefault(vn, []).append((dyno_name, obj))

                # Inject placed object frames into target c_*.va0 tables
                for va0_name, recs_for_va0 in by_va0.items():
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
                        for dyno_name, obj in recs_for_va0:
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

        if not placed_records and not sobl_appended and not members_replaced and not noclip_patched:
            return 0

        manifest_p.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        out_tmp = mod_trk_path.with_suffix(".TRK.repack")
        rst0.pack_archive(tmp_dir, str(out_tmp))
        os.replace(out_tmp, mod_trk_path)
        return len(placed_records)


# ---------------------------------------------------------------------------
# Fast GF(2) Bit-Exact Incremental EOD CRC-32 + Delta-Only Mod Baker (<10 ms!)
# ---------------------------------------------------------------------------
_CRC_TABLE = rst0.CRC_TABLE


def _crc_step_zero(v: int) -> int:
    return (((v << 8) & 0xFFFFFFFF) ^ _CRC_TABLE[(v >> 24) & 0xFF]) & 0xFFFFFFFF


def _crc_mat_mul_vec(mat: list[int], vec: int) -> int:
    out = 0
    bit = 0
    while vec:
        if vec & 1:
            out ^= mat[bit]
        vec >>= 1
        bit += 1
    return out


def _crc_build_shift_mats(max_pow: int = 25) -> list[list[int]]:
    m0 = [_crc_step_zero(1 << b) for b in range(32)]
    mats = [m0]
    for _ in range(max_pow):
        prev = mats[-1]
        mats.append([_crc_mat_mul_vec(prev, col) for col in prev])
    return mats


_CRC_SHIFT_MATS: list[list[int]] = _crc_build_shift_mats(25)


def _crc_shift_zeros(v: int, n_bytes: int) -> int:
    k = 0
    while n_bytes:
        if n_bytes & 1:
            v = _crc_mat_mul_vec(_CRC_SHIFT_MATS[k], v)
        n_bytes >>= 1
        k += 1
    return v


def delta_eod_crc(
    orig_crc: int,
    orig_bytes: bytes | bytearray,
    new_bytes: bytes | bytearray,
    spans: list[tuple[int, int]],
) -> int:
    """Compute bit-exact `rst0.eod_crc(new_bytes)` in <0.1 ms via GF(2) span folding."""
    n = len(orig_bytes)
    if n < 4 or len(new_bytes) != n:
        return eod_crc(bytes(new_bytes))
    if not spans:
        return orig_crc
    # Merge & sort overlapping/adjacent spans so each byte is XORed once
    ordered = sorted((max(0, s), min(n, e)) for s, e in spans if e > s)
    if not ordered:
        return orig_crc
    merged: list[tuple[int, int]] = []
    cs, ce = ordered[0]
    for s, e in ordered[1:]:
        if s <= ce:
            if e > ce:
                ce = e
        else:
            merged.append((cs, ce))
            cs, ce = s, e
    merged.append((cs, ce))

    tbl = _CRC_TABLE
    dc_total = 0
    for s, e in merged:
        pos = s
        if pos < 4:
            hdr_end = min(4, e)
            dc_hdr = 0
            for p in range(pos, hdr_end):
                diff = orig_bytes[p] ^ new_bytes[p]
                if diff:
                    dc_hdr |= diff << ((3 - p) * 8)
            if dc_hdr:
                dc_total ^= _crc_shift_zeros(dc_hdr, n - 4)
            pos = hdr_end
        if pos < e:
            dc = 0
            for p in range(pos, e):
                diff = orig_bytes[p] ^ new_bytes[p]
                dc = (((dc << 8) & 0xFFFFFFFF) ^ diff ^ tbl[(dc >> 24) & 0xFF]) & 0xFFFFFFFF
            if dc:
                dc_total ^= _crc_shift_zeros(dc, n - e)
    return (orig_crc ^ dc_total) & 0xFFFFFFFF


# In-memory caches for retail patch indexes and per-mod delta bake state
_RETAIL_PATCH_CACHE: dict[str, dict[str, Any]] = {}
_MOD_DELTA_STATE: dict[str, dict[str, Any]] = {}


def _get_retail_patch_index(world: TrackWorld) -> dict[str, Any]:
    """Return cached retail .TRK metadata & clean member slices (populated once per track)."""
    key = os.path.abspath(str(world.trk_path))
    st = os.stat(key)
    sig = (st.st_size, st.st_mtime_ns)
    cached = _RETAIL_PATCH_CACHE.get(key)
    if cached is not None and cached["stat"] == sig:
        return cached
    raw = world.trk_path.read_bytes()
    retail_sha = hashlib.sha256(raw).hexdigest()
    entries = wc.read_directory(raw)
    by_name = {e["name"].lower(): e for e in entries}
    graf_entry = next((e for e in entries if e["type"] == "GRAF"), None)
    clean_graf = raw[graf_entry["offset"] : graf_entry["offset"] + graf_entry["size"]] if graf_entry else b""
    sol_entry = by_name.get("track.sol")
    clean_sol = raw[sol_entry["offset"] : sol_entry["offset"] + sol_entry["size"]] if sol_entry else b""

    clean_ilds: dict[str, bytes] = {}
    clean_va0s: dict[str, bytes] = {}
    for e in entries:
        low = e["name"].lower()
        if low.endswith(".ild"):
            clean_ilds[low] = raw[e["offset"] : e["offset"] + e["size"]]
        elif low.startswith("c_") and e["type"] == "VALT":
            clean_va0s[low] = raw[e["offset"] : e["offset"] + e["size"]]

    idx = {
        "stat": sig,
        "retail_sha256": retail_sha,
        "entries": entries,
        "by_name": by_name,
        "graf_entry": graf_entry,
        "clean_graf": clean_graf,
        "sol_entry": sol_entry,
        "clean_sol": clean_sol,
        "clean_ilds": clean_ilds,
        "clean_va0s": clean_va0s,
        "graf_parsed": None,
        "by_id": None,
        "mlists": None,
        "model_patch_meta": {},  # mod_off -> (q199_offsets, mlist_hits, model_hdr)
        "event_props": {},       # ev_name.lower() -> list[prop]
    }
    _RETAIL_PATCH_CACHE[key] = idx
    return idx


def _ensure_graf_patch_tables(idx: dict[str, Any]) -> None:
    if idx["graf_parsed"] is not None:
        return
    graf = wg.parse_graf(idx["clean_graf"])
    idx["graf_parsed"] = graf
    idx["by_id"] = {f"i{i:04d}": leaf for i, leaf in enumerate(graf["leaves"])}
    idx["mlists"] = [info for info in graf["mlist_info"] if info]


def _get_model_patch_meta(idx: dict[str, Any], mod_off: int) -> tuple[list[int], list[int], dict[str, Any]]:
    meta = idx["model_patch_meta"].get(mod_off)
    if meta is not None:
        return meta
    orig = idx["clean_graf"]
    model = wg.parse_model(orig, mod_off)
    q199: list[int] = []
    for part in model["parts"]:
        q199.extend(we.vif_q199_offsets(orig, part))
    key = struct.pack("<4f", *model["centre"], model["radius"])
    mhits: list[int] = []
    for info in idx["mlists"]:
        lo_, hi_ = info["offset"], info["offset"] + info["batches"]["offset"]
        start = lo_
        while True:
            k = orig.find(key, start, hi_)
            if k < 0:
                break
            if (k - lo_) % 4 == 0:
                mhits.append(k)
            start = k + 4
    meta = (q199, mhits, model)
    idx["model_patch_meta"][mod_off] = meta
    return meta


def delta_eod_crc_patches(
    orig_crc: int,
    orig_bytes: bytes | bytearray,
    patches: list[tuple[int, bytes]],
) -> int:
    """Compute bit-exact `rst0.eod_crc` from sparse `(offset, new_bytes)` patches without copying the full buffer."""
    n = len(orig_bytes)
    if n < 4 or not patches:
        return orig_crc
    # Resolve overlapping patches (later patch wins) onto minimal merged spans
    by_span: dict[int, int] = {}
    for off, data in patches:
        for i, b in enumerate(data):
            p = off + i
            if 0 <= p < n:
                by_span[p] = b
    if not by_span:
        return orig_crc
    positions = sorted(by_span.keys())
    tbl = _CRC_TABLE
    dc_total = 0
    idx_p = 0
    total_p = len(positions)
    while idx_p < total_p:
        s = positions[idx_p]
        e = s + 1
        while idx_p + 1 < total_p and positions[idx_p + 1] == e:
            idx_p += 1
            e += 1
        idx_p += 1
        pos = s
        if pos < 4:
            hdr_end = min(4, e)
            dc_hdr = 0
            for p in range(pos, hdr_end):
                diff = orig_bytes[p] ^ by_span[p]
                if diff:
                    dc_hdr |= diff << ((3 - p) * 8)
            if dc_hdr:
                dc_total ^= _crc_shift_zeros(dc_hdr, n - 4)
            pos = hdr_end
        if pos < e:
            dc = 0
            for p in range(pos, e):
                diff = orig_bytes[p] ^ by_span[p]
                dc = (((dc << 8) & 0xFFFFFFFF) ^ diff ^ tbl[(dc >> 24) & 0xFF]) & 0xFFFFFFFF
            if dc:
                dc_total ^= _crc_shift_zeros(dc, n - e)
    return (orig_crc ^ dc_total) & 0xFFFFFFFF


def _patch_graf_sparse_fast(
    idx: dict[str, Any],
    base_graf: bytes,
    base_crc: int,
    graf_edits: list[dict[str, Any]],
) -> tuple[list[tuple[int, bytes]], int, list[tuple[int, int]], int]:
    """Patch GRAF static/optional-terrain/dyno transforms into sparse byte slices (<0.5 ms, zero 3.4MB copy!)."""
    _ensure_graf_patch_tables(idx)
    patches: list[tuple[int, bytes]] = []
    spans: list[tuple[int, int]] = []
    by_id = idx["by_id"]
    changed_bytes = 0

    for edit in graf_edits:
        leaf = by_id.get(edit.get("instance"))
        if leaf is None:
            continue
        if "matrix" in edit:
            rows = we.matrix_to_native_rows([float(x) for x in edit["matrix"]])
            off = leaf["record"] + wg.DYNO_MATRIX
            patches.append((off, struct.pack("<12f", *rows)))
            spans.append((off, off + 48))
            changed_bytes += 48
            continue
        delta = [float(x) for x in edit.get("translate", (0.0, 0.0, 0.0))]
        if not any(abs(v) > 1e-7 for v in delta):
            continue
        mo = leaf.get("model_offset")
        if not mo:
            continue
        q199, mhits, _model = _get_model_patch_meta(idx, mo)
        for q in q199:
            ox, oy, oz = struct.unpack_from("<3f", base_graf, q)
            patches.append((q, struct.pack("<3f", ox + delta[0], oy + delta[1], oz + delta[2])))
            spans.append((q, q + 12))
            changed_bytes += 12
        cx, cy, cz = struct.unpack_from("<3f", base_graf, mo + 0x1C)
        nrad = struct.unpack_from("<f", base_graf, mo + 0x18)[0]
        nc = (cx + delta[0], cy + delta[1], cz + delta[2])
        bvals = list(struct.unpack_from("<6f", base_graf, mo + 0x28))
        for a in range(3):
            bvals[2 * a] += delta[a]
            bvals[2 * a + 1] += delta[a]
        patches.append((mo + 0x1C, struct.pack("<3f", *nc)))
        patches.append((mo + 0x28, struct.pack("<6f", *bvals)))
        spans.append((mo + 0x1C, mo + 0x28))
        spans.append((mo + 0x28, mo + 0x40))
        changed_bytes += 36
        for k in mhits:
            mx, my, mz = struct.unpack_from("<3f", base_graf, k)
            patches.append((k, struct.pack("<3f", mx + delta[0], my + delta[1], mz + delta[2])))
            spans.append((k, k + 12))
            changed_bytes += 12
        # Grow ancestor NODE_SPHERE cull radii so moved hills/walls never cull out
        for step in leaf.get("path", ()):
            if step.get("kind") != "sphere":
                continue
            n_off = int(step["node"])
            scx, scy, scz, sr = struct.unpack_from("<4f", base_graf, n_off + 0x0C)
            need = math.dist(nc, (scx, scy, scz)) + float(nrad)
            if need > sr:
                r_off = n_off + 0x18
                patches.append((r_off, struct.pack("<f", float(need * 1.02 + 0.5))))
                spans.append((r_off, r_off + 4))
                changed_bytes += 4

    new_crc = delta_eod_crc_patches(base_crc, base_graf, patches)
    return patches, new_crc, spans, changed_bytes


def _patch_sobl_fast(
    clean_sol: bytes,
    clean_crc: int,
    world: TrackWorld,
) -> tuple[bytearray, int, list[tuple[int, int]], int]:
    """Patch SOBL barrier edits directly into `track.sol` using pre-planned fields (<1 ms)."""
    buf = bytearray(clean_sol)
    spans: list[tuple[int, int]] = []
    patched = 0
    recs = world.collision.sobl_records
    edits: dict[int, dict[str, Any] | None] = dict(world.collision.sobl_edits)
    if world.all_walls_noclip:
        edits.update(
            (int(rec["index"]), None)
            for rec in recs if rec.get("kind") == "box" and rec.get("vertical")
        )
    for rec_idx, ed in sorted(edits.items()):
        idx = int(rec_idx)
        if not (0 <= idx < len(recs)):
            continue
        rec = recs[idx]
        if rec.get("kind") != "box":
            continue
        off = int(rec["offset"])
        if ed is None:
            # The SOBL quadtree indexes only XZ. Move the wall below the playable world
            # without changing its registered footprint or the original project edits.
            struct.pack_into("<f", buf, off + 0x28, -5000.0)
            spans.append((off + 0x28, off + 0x2C))
            patched += 1
            continue
        if "ry" in ed and "centre" in ed and "sphere_radius" in ed:
            new_ry = float(ed["ry"])
            new_centre = [float(v) for v in ed["centre"]]
            new_sphere = float(ed["sphere_radius"])
        else:
            orig_rec = dict(rec)
            orig_rec["half_extents"] = list(rec["orig_half_extents"])
            orig_rec["centre"] = list(rec["orig_centre"])
            orig_rec["sphere_radius"] = float(rec["orig_sphere_radius"])
            plan = wc.plan_box_edit(orig_rec, str(ed["field"]), float(ed["value"]), "bottom", world.collision.terrain)
            new_ry = float(plan["ry"])
            new_centre = [float(v) for v in plan["centre"]]
            new_sphere = float(plan["sphere_radius"])
        struct.pack_into("<3f", buf, off + 0x24, *new_centre)
        struct.pack_into("<f", buf, off + 0x64, new_ry)
        struct.pack_into("<f", buf, off + 0x6C, new_sphere)
        spans.append((off + 0x24, off + 0x30))
        spans.append((off + 0x64, off + 0x70))
        patched += 1
    new_crc = delta_eod_crc(clean_crc, clean_sol, buf, spans)
    return buf, new_crc, spans, patched


def _patch_va0_events_fast(
    idx: dict[str, Any],
    world: TrackWorld,
    event_edits: list[dict[str, Any]],
) -> tuple[dict[str, tuple[bytes, int]], int]:
    """Patch c_*.va0 event prop frame64 records directly from cached clean members (<1 ms)."""
    if not event_edits:
        return {}, 0
    track_proxy = None
    dirty_bufs: dict[str, bytearray] = {}
    dirty_spans: dict[str, list[tuple[int, int]]] = {}
    changed_bytes = 0

    for edit in event_edits:
        ev_name = str(edit["event"])
        low = ev_name.lower()
        props = idx["event_props"].get(low)
        if props is None:
            if track_proxy is None:
                track_proxy = we.load_track(world.game_dir, world.trk_rel)
            props = we.event_props(track_proxy, ev_name)
            idx["event_props"][low] = props
        pid = edit.get("prop")
        k = int(pid[1:]) if isinstance(pid, str) and pid.startswith("e") else int(edit.get("index", -1))
        if not (0 <= k < len(props)):
            continue
        prop = props[k]
        rows = list(prop["frame"])
        if "matrix" in edit:
            M = [float(x) for x in edit["matrix"]]
            new_rows = we.matrix_to_native_rows(M)
            if prop["localFrame"]:
                L = we.native_rows_to_matrix(prop["localFrame"])
                R = [[L[0], L[1], L[2]], [L[4], L[5], L[6]], [L[8], L[9], L[10]]]
                t = [L[3], L[7], L[11]]
                inv = [
                    R[0][0], R[1][0], R[2][0], -(R[0][0] * t[0] + R[1][0] * t[1] + R[2][0] * t[2]),
                    R[0][1], R[1][1], R[2][1], -(R[0][1] * t[0] + R[1][1] * t[1] + R[2][1] * t[2]),
                    R[0][2], R[1][2], R[2][2], -(R[0][2] * t[0] + R[1][2] * t[1] + R[2][2] * t[2]),
                    0.0, 0.0, 0.0, 1.0,
                ]
                new_rows = we.compose_rows(new_rows, we.matrix_to_native_rows(inv))
        elif "translate" in edit:
            d = [float(x) for x in edit["translate"]]
            new_rows = rows[:9] + [rows[9 + a] + d[a] if d[a] != 0.0 else rows[9 + a] for a in range(3)]
        else:
            continue
        if new_rows == rows:
            continue
        text = we.encode_frame64(new_rows)
        ml = prop["member"].lower()
        clean = idx["clean_va0s"].get(ml)
        if clean is None:
            continue
        buf = dirty_bufs.get(ml)
        if buf is None:
            buf = bytearray(clean)
            dirty_bufs[ml] = buf
            dirty_spans[ml] = []
        moff = int(prop["memberOffset"])
        buf[moff : moff + 64] = text.encode("latin1")
        dirty_spans[ml].append((moff, moff + 64))
        changed_bytes += 64

    out: dict[str, tuple[bytes, int]] = {}
    for ml, buf in dirty_bufs.items():
        orig = idx["clean_va0s"][ml]
        orig_crc = int(idx["by_name"][ml]["crc"])
        new_crc = delta_eod_crc(orig_crc, orig, buf, dirty_spans[ml])
        out[ml] = (bytes(buf), new_crc)
    return out, changed_bytes


def export_mod_layer(
    world: TrackWorld,
    project_name: str,
    dest_dir: Path | str,
    *,
    token: Any = None,
    progress: Any = None,
    allow_delta: bool = True,
) -> dict[str, Any]:
    """Write a real, playable non-destructive mod layer into `dest_dir` (never edits retail DATA/).

    Supports a `<10 ms` **Fast Delta-Only Mod Baker** when `<dest_dir>/DATA/TRACKS/<TRACK>.TRK`
    already exists and only transforms / ILIN / SOBL changed (no new `.CAR`/`.mod` archive members),
    patching dirty member payloads + exact 0TSR CRCs in-place without re-copying or re-parsing
    untouched retail members!
    """
    t_compile_ns = time.perf_counter_ns()
    out = Path(os.path.abspath(str(dest_dir)))
    retail_trk_abs = os.path.abspath(str(world.trk_path))
    retail_data_abs = os.path.abspath(str(world.game_dir / "DATA"))
    out_str = str(out)
    if out_str.lower() == retail_data_abs.lower() or out_str.lower().startswith(retail_data_abs.lower() + os.sep):
        raise ValueError("Refusing to write mod layer inside retail DATA/ directory.")

    tracks_out = out / "DATA" / "TRACKS"
    mod_trk_path = tracks_out / world.trk_path.name
    mod_key = os.path.abspath(str(mod_trk_path))
    if mod_key.lower() == retail_trk_abs.lower():
        raise ValueError("Refusing to overwrite retail .TRK file.")
    if not tracks_out.is_dir():
        tracks_out.mkdir(parents=True, exist_ok=True)

    retail_idx = _get_retail_patch_index(world)
    retail_sha_before = retail_idx["retail_sha256"]
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

    # Lazily index event props only if an event-prop is actually deleted (saves ~3 ms on delta bakes!)
    ev_props_by_sig: dict[tuple[str, int, int, int], list[WorldObject]] | None = None

    def _get_ev_props_by_sig() -> dict[tuple[str, int, int, int], list[WorldObject]]:
        nonlocal ev_props_by_sig
        if ev_props_by_sig is None:
            ev_props_by_sig = {}
            for o2 in world.objects:
                if o2.source == "Retail" and o2.category == "event-prop" and o2.retail_event_member:
                    sig2 = (
                        o2.model_key,
                        int(round(o2.retail_initial_pos[0] * 2.0)),
                        int(round(o2.retail_initial_pos[1] * 2.0)),
                        int(round(o2.retail_initial_pos[2] * 2.0)),
                    )
                    ev_props_by_sig.setdefault(sig2, []).append(o2)
        return ev_props_by_sig

    for obj in world.objects:
        # Ultra-fast single-branch skip for untouched retail objects (~99.9% of 6,066 instances)
        iscl = obj.retail_initial_scale
        if (
            not obj.deleted
            and obj.position == obj.retail_initial_pos
            and obj.source == "Retail"
            and obj.rotation_hpr == obj.retail_initial_rot
            and (obj.retail_original_model_key is None or obj.model_key == obj.retail_original_model_key)
            and (
                obj.scale == iscl
                or (
                    abs(obj.scale[0] - iscl[0]) <= 1e-4
                    and abs(obj.scale[1] - iscl[1]) <= 1e-4
                    and abs(obj.scale[2] - iscl[2]) <= 1e-4
                )
            )
        ):
            continue
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
                if world.sync_all_events:
                    sig = (
                        obj.model_key,
                        int(round(obj.retail_initial_pos[0] * 2.0)),
                        int(round(obj.retail_initial_pos[1] * 2.0)),
                        int(round(obj.retail_initial_pos[2] * 2.0)),
                    )
                    for peer in _get_ev_props_by_sig().get(sig, ()):
                        if peer is obj or not peer.retail_event_member:
                            continue
                        ppid = f"e{peer.retail_event_prop_index:04d}" if peer.retail_event_prop_index is not None else peer.obj_id
                        _add_event_edit(
                            peer.retail_event_member,
                            ppid,
                            {"event": peer.retail_event_member, "prop": ppid, "translate": [0.0, -5000.0, 0.0]},
                        )
            continue

        if obj.source != "Retail":
            custom_inject_objs.append(obj)
            continue

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

        # Fast-path skip for unmodified retail objects (avoids ~50,000 genexpr/abs calls!)
        pos = obj.position
        ipos = obj.retail_initial_pos
        rot = obj.rotation_hpr
        irot = obj.retail_initial_rot
        scl = obj.scale
        if pos == ipos and rot == irot and scl == iscl:
            continue

        dx = pos[0] - ipos[0]
        dy = pos[1] - ipos[1]
        dz = pos[2] - ipos[2]
        moved = abs(dx) > 1e-4 or abs(dy) > 1e-4 or abs(dz) > 1e-4
        rotated = abs(rot[0] - irot[0]) > 1e-4 or abs(rot[1] - irot[1]) > 1e-4 or abs(rot[2] - irot[2]) > 1e-4
        scaled = abs(scl[0] - iscl[0]) > 1e-4 or abs(scl[1] - iscl[1]) > 1e-4 or abs(scl[2] - iscl[2]) > 1e-4
        if not moved and not rotated and not scaled:
            continue

        if obj.retail_instance_id and obj.category in ("static", "optional-terrain"):
            all_edited_terrain_objs.append(obj)
            if scaled or rotated:
                vif_bake_objs.append(obj)
            elif moved:
                retail_edits.append({"instance": obj.retail_instance_id, "translate": [dx, dy, dz]})
        elif obj.retail_instance_id and obj.category == "dyno":
            if scaled and obj.model_key.startswith(("mod:", "car:")):
                m_hide = compose_transform((obj.position[0], -5000.0, obj.position[2]), obj.rotation_hpr, (1.0, 1.0, 1.0))
                retail_edits.append({"instance": obj.retail_instance_id, "matrix": [float(v) for v in m_hide.reshape(-1)]})
                custom_inject_objs.append(obj)
            else:
                if scaled:
                    vif_bake_objs.append(obj)
                m4 = compose_transform(obj.position, obj.rotation_hpr, (1.0, 1.0, 1.0))
                retail_edits.append({"instance": obj.retail_instance_id, "matrix": [float(v) for v in m4.reshape(-1)]})
        elif obj.retail_event_member and obj.category == "event-prop":
            pid = f"e{obj.retail_event_prop_index:04d}" if obj.retail_event_prop_index is not None else obj.obj_id
            if scaled:
                # PS2 unfloat_frame64 has no scale: hide original retail prop and inject scaled .mod + solid obstacle wall!
                _add_event_edit(
                    obj.retail_event_member,
                    pid,
                    {"event": obj.retail_event_member, "prop": pid, "translate": [0.0, -5000.0, 0.0]},
                )
                if world.sync_all_events:
                    sig = (
                        obj.model_key,
                        int(round(obj.retail_initial_pos[0] * 2.0)),
                        int(round(obj.retail_initial_pos[1] * 2.0)),
                        int(round(obj.retail_initial_pos[2] * 2.0)),
                    )
                    for peer in _get_ev_props_by_sig().get(sig, ()):
                        if peer is obj or not peer.retail_event_member:
                            continue
                        ppid = f"e{peer.retail_event_prop_index:04d}" if peer.retail_event_prop_index is not None else peer.obj_id
                        _add_event_edit(
                            peer.retail_event_member,
                            ppid,
                            {"event": peer.retail_event_member, "prop": ppid, "translate": [0.0, -5000.0, 0.0]},
                        )
                custom_inject_objs.append(obj)
            elif rotated:
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

    slug = re.sub(r"[^A-Za-z0-9_.-]+", "_", project_name).strip("_") or "world_mod"
    hosttex_sig = (
        world.environment.preset_name,
        world.environment.borrowed_sky_track,
        world.environment.custom_skybox_path,
        tuple(sorted((n, ta.replaced_png_path, len(ta.rgba)) for n, ta in world.textures.items() if ta.is_replaced)),
    )
    env_sig = repr(world.environment.to_dict())
    custom_boxes_count = len(world.collision.custom_boxes)
    has_structural_ilin = any(
        g.dirty and (
            any(n.retail_index is None for n in g.nodes.values())
            or (
                lname.lower() in retail_idx["clean_ilds"]
                and len(g.nodes) != ilin_fmt._layout(retail_idx["clean_ilds"][lname.lower()])["count"]
            )
        )
        for lname, g in world.all_road_graphs.items()
    )
    graf_edits = [e for e in retail_edits if "event" not in e]
    event_edits = [e for e in retail_edits if "event" in e]
    graf_sig = repr(graf_edits)
    event_sig = repr(event_edits)
    sobl_sig = repr(sorted((int(k), v) for k, v in world.collision.sobl_edits.items())) if world.collision.sobl_edits else ""
    prev_state = _MOD_DELTA_STATE.get(mod_key)

    # -----------------------------------------------------------------------
    # FAST DELTA-ONLY MOD BAKER (<10 ms in-place member patching!)
    # Activates whenever mod_trk_path already exists & no new .CAR/.mod/VIF
    # structural changes occurred — patches ONLY dirty members + 0TSR CRCs.
    # -----------------------------------------------------------------------
    needs_static_sobl_synth = False
    if allow_delta and prev_state is not None and not custom_inject_objs and not vif_bake_objs:
        known_solid_ids = prev_state.get("known_solid_static_ids")
        for o in all_edited_terrain_objs:
            if o.category != "static":
                continue
            if known_solid_ids is not None and o.obj_id in known_solid_ids:
                continue
            om = world.models.get(o.model_key)
            if om is None:
                continue
            lo_f, hi_f = o.world_aabb(om)
            mn_x, mx_x = float(lo_f[0]) - 0.5, float(hi_f[0]) + 0.5
            mn_z, mx_z = float(lo_f[2]) - 0.5, float(hi_f[2]) + 0.5
            has_sobl = False
            for rec in world.collision.sobl_records:
                if rec.get("kind") != "box":
                    continue
                rcx, _rcy, rcz = rec["centre"]
                rfx, rfz = wc.footprint(rec)
                if (rcx + rfx) >= mn_x and (rcx - rfx) <= mx_x and (rcz + rfz) >= mn_z and (rcz - rfz) <= mx_z:
                    has_sobl = True
                    break
            if not has_sobl:
                scol_c, _sobl_l = world.static_overlap_counts(o, use_initial=False)
                if scol_c == 0:
                    needs_static_sobl_synth = True
                    break
            if known_solid_ids is not None:
                known_solid_ids.add(o.obj_id)

    can_delta = (
        allow_delta
        and prev_state is not None
        and os.path.isfile(mod_key)
        and os.stat(mod_key).st_size == prev_state["mod_size"]
        and prev_state["retail_trk_path"] == retail_trk_abs
        and prev_state["retail_sha256"] == retail_sha_before
        and not custom_inject_objs
        and custom_boxes_count == 0
        and not world.all_walls_noclip
        and not has_structural_ilin
        and not prev_state["has_custom_members"]
        and not vif_bake_objs
        and not prev_state["had_vif_bake"]
        and not needs_static_sobl_synth
        and not any(o.category == "optional-terrain" for o in all_edited_terrain_objs)
        and bool(world.sync_all_events) == prev_state["sync_all_events"]
    )

    if can_delta:
        assert prev_state is not None
        place_warnings: list[str] = []
        delta_patched_members: list[str] = []
        place_changed_bytes = prev_state["last_place_changed_bytes"]
        sobl_patched_count = prev_state["last_sobl_patched"]
        ilin_patched_lines: list[str] = []
        mod_by_name = prev_state["mod_by_name"]
        member_crcs = dict(prev_state["member_crcs"])

        with open(mod_trk_path, "r+b") as fh:
            # 1. In-place GRAF transforms (sparse patches + 4B CRC — zero 3.4MB copy!)
            if graf_sig != prev_state["last_graf_sig"]:
                ge = prev_state["graf_entry"]
                if ge is not None:
                    base_graf = prev_state["base_graf_bytes"]
                    base_crc = prev_state["base_graf_crc"]
                    patches, new_crc, new_spans, chg_b = _patch_graf_sparse_fast(retail_idx, base_graf, base_crc, graf_edits)
                    g_off = int(ge["offset"])
                    # Restore any previously patched spans from base_graf first, then write new patches
                    for s, e in prev_state["prev_graf_spans"]:
                        fh.seek(g_off + s)
                        fh.write(base_graf[s:e])
                    for off_p, pbytes in patches:
                        fh.seek(g_off + off_p)
                        fh.write(pbytes)
                    fh.seek(int(ge["crc_field"]))
                    fh.write(struct.pack("<I", new_crc))
                    prev_state["prev_graf_spans"] = new_spans
                    prev_state["last_graf_sig"] = graf_sig
                    place_changed_bytes = chg_b
                    prev_state["last_place_changed_bytes"] = chg_b
                    member_crcs[ge["name"].lower()] = new_crc
                    delta_patched_members.append(ge["name"])

            # 2. In-place Event Prop frames (c_*.va0)
            if event_sig != prev_state["last_event_sig"]:
                va0_patches, ev_chg = _patch_va0_events_fast(retail_idx, world, event_edits)
                # Restore any previously modified va0 no longer edited
                for old_ml in prev_state["prev_va0_members"]:
                    if old_ml not in va0_patches and old_ml in retail_idx["clean_va0s"]:
                        ent = mod_by_name.get(old_ml)
                        if ent:
                            clean_b = retail_idx["clean_va0s"][old_ml]
                            fh.seek(int(ent["offset"]))
                            fh.write(clean_b)
                            fh.seek(int(ent["crc_field"]))
                            fh.write(struct.pack("<I", int(ent["crc"])))
                            member_crcs[old_ml] = int(ent["crc"])
                            delta_patched_members.append(ent["name"])
                for ml, (new_b, new_crc) in va0_patches.items():
                    ent = mod_by_name.get(ml)
                    if ent and len(new_b) == int(ent["size"]):
                        fh.seek(int(ent["offset"]))
                        fh.write(new_b)
                        fh.seek(int(ent["crc_field"]))
                        fh.write(struct.pack("<I", new_crc))
                        member_crcs[ml] = new_crc
                        delta_patched_members.append(ent["name"])
                prev_state["prev_va0_members"] = set(va0_patches.keys())
                prev_state["last_event_sig"] = event_sig
                place_changed_bytes += ev_chg

            # 3. In-place SOBL barrier edits (track.sol)
            if sobl_sig != prev_state["last_sobl_sig"]:
                se = retail_idx["sol_entry"]
                if se is not None:
                    clean_sol = retail_idx["clean_sol"]
                    buf_s, new_scrc, sobl_spans, s_cnt = _patch_sobl_fast(clean_sol, int(se["crc"]), world)
                    s_off = int(se["offset"])
                    if len(sobl_spans) > 256:
                        fh.seek(s_off)
                        fh.write(buf_s)
                    else:
                        for s, e in sorted(set(prev_state["prev_sobl_spans"] + sobl_spans)):
                            fh.seek(s_off + s)
                            fh.write(buf_s[s:e])
                    fh.seek(int(se["crc_field"]))
                    fh.write(struct.pack("<I", new_scrc))
                    prev_state["prev_sobl_spans"] = sobl_spans
                    prev_state["last_sobl_sig"] = sobl_sig
                    sobl_patched_count = s_cnt
                    prev_state["last_sobl_patched"] = s_cnt
                    member_crcs["track.sol"] = new_scrc
                    delta_patched_members.append("track.sol")

            # 4. In-place ILIN racing line edits (*.ild)
            for lname, graph in world.all_road_graphs.items():
                if not graph.dirty:
                    continue
                low = lname.lower()
                ent = mod_by_name.get(low)
                clean_ild = retail_idx["clean_ilds"].get(low)
                if ent is None or clean_ild is None:
                    continue
                edits_map: dict[str, float] = {}
                node_heights: dict[int, float] = {}
                for node in graph.nodes.values():
                    if node.retail_index is not None:
                        edits_map[f"node.{node.retail_index}.x"] = node.x
                        edits_map[f"node.{node.retail_index}.z"] = node.z
                        node_heights[int(node.retail_index)] = float(node.y)
                if edits_map:
                    try:
                        patched_ild = ilin_fmt.encode(clean_ild, edits_map, recalculate=True, node_heights=node_heights)
                        if len(patched_ild) == int(ent["size"]):
                            retail_ild_crc = int(retail_idx["by_name"][low]["crc"])
                            new_icrc = delta_eod_crc(retail_ild_crc, clean_ild, patched_ild, [(0, len(patched_ild))])
                            fh.seek(int(ent["offset"]))
                            fh.write(patched_ild)
                            fh.seek(int(ent["crc_field"]))
                            fh.write(struct.pack("<I", new_icrc))
                            ilin_patched_lines.append(lname)
                            member_crcs[low] = new_icrc
                            delta_patched_members.append(lname)
                    except Exception as exc:
                        place_warnings.append(f"ILIN {lname} patch warning: {exc}")

        # 5. HOSTTEX + env_override.ini (skip re-encoding if unchanged!)
        if hosttex_sig != prev_state["last_hosttex_sig"]:
            host_exported, host_warnings = _export_host_textures(world, slug, out)
            place_warnings.extend(host_warnings)
            prev_state["last_hosttex_sig"] = hosttex_sig
            prev_state["last_host_exported"] = host_exported
        else:
            host_exported = list(prev_state["last_host_exported"])
        if env_sig != prev_state.get("last_env_sig"):
            write_env_override_ini(world, out)
            prev_state["last_env_sig"] = env_sig

        prev_state["member_crcs"] = member_crcs
        any_diff = bool(retail_edits or world.collision.sobl_edits or ilin_patched_lines or prev_state["base_differs"])
        if any_diff:
            fp_src = f"{retail_sha_before}:{sorted(member_crcs.items())}:{place_changed_bytes}".encode("utf-8")
            mod_trk_sha = hashlib.sha256(fp_src).hexdigest()
        else:
            mod_trk_sha = retail_sha_before

        for obj in all_edited_terrain_objs:
            obj.written = True
        for g in world.all_road_graphs.values():
            g.dirty = False

        # Verify retail file stat untouched (zero-cost safety check)
        st_now = os.stat(retail_trk_abs)
        if (st_now.st_size, st_now.st_mtime_ns) != retail_idx["stat"]:
            if _sha256_file(world.trk_path) != retail_sha_before:
                raise AssertionError("CRITICAL: retail .TRK SHA-256 changed during delta mod export!")

        compile_ms = (time.perf_counter_ns() - t_compile_ns) / 1_000_000.0
        manifest = {
            "schema": "eod.mod_layer/3",
            "bake_mode": "delta",
            "compile_ms": round(compile_ms, 2),
            "delta_patched_members": delta_patched_members,
            "project_name": project_name,
            "track": world.track_name,
            "source_retail_trk": str(world.trk_path),
            "retail_trk_sha256": retail_sha_before,
            "mod_trk": str(mod_trk_path),
            "mod_trk_sha256": mod_trk_sha,
            "mod_trk_differs_from_retail": mod_trk_sha != retail_sha_before,
            "overlay_json": str(out / f"{slug}.eodworld.json"),
            "profile_txt": str(out / "PROFILE.TXT"),
            "custom_race_json": str(out / "custom_race.json"),
            "retail_placement_edits": len(retail_edits),
            "deleted_retail_objects": deleted_retail_count,
            "changed_payload_bytes": place_changed_bytes,
            "place_report_count": len(retail_edits),
            "vif_models_baked": 0,
            "vif_vertices_baked": 0,
            "opt_terrain_promoted": prev_state["opt_terrain_promoted"],
            "bppt_planes_patched": 0,
            "scol_planes_patched": 0,
            "sobl_barriers_patched": sobl_patched_count,
            "sobl_scaled_barriers": 0,
            "sobl_boxes_synthesized": 0,
            "ilin_patched_lines": ilin_patched_lines,
            "custom_objects_compiled": 0,
            "sync_all_events": bool(world.sync_all_events),
            "host_textures_exported": len(host_exported),
            "host_textures": host_exported,
            "editor_placed_objects": 0,
            "custom_collision_boxes": len(world.collision.custom_boxes),
            "warnings": place_warnings,
            "exported_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        }
        manifest_path = out / "mod_layer_manifest.json"
        manifest["manifest_path"] = str(manifest_path)
        manifest_path.write_text(json.dumps(manifest, separators=(",", ":")) + "\n", encoding="utf-8")
        if progress:
            progress(100, f"Delta mod bake complete ({compile_ms:.2f} ms)")
        return manifest

    # -----------------------------------------------------------------------
    # FULL MOD BAKER (initial bake or structural VIF / .CAR / .mod repack)
    # -----------------------------------------------------------------------
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

    # Bake VIF vertex transforms (scale/rotate/raise) + BPPT & SCOL collision + SOBL scaling/synthesis + All-Events ramp sync
    if progress:
        progress(40, "Baking VIF mesh vertices & BPPT/SCOL/SOBL collision...")
    trk_bytes = bytearray(mod_trk_path.read_bytes())
    entries = wc.read_directory(bytes(trk_bytes))
    auto_sobl_boxes: list[dict[str, Any]] = []
    bake_stats = _bake_graf_vif_and_collision(
        trk_bytes,
        entries,
        world,
        vif_bake_objs,
        all_edited_terrain_objs,
        place_warnings,
        auto_sobl_boxes_out=auto_sobl_boxes,
    )

    # Patch modified SOBL collision barriers (track.sol) using fast direct patcher!
    if progress:
        progress(55, "Patching SOBL collision barriers...")
    sobl_patched_count = 0
    sobl_spans_full: list[tuple[int, int]] = []
    if world.collision.sobl_edits or world.all_walls_noclip:
        se = next((e for e in entries if e["name"].lower() == "track.sol"), None)
        if se is not None and retail_idx["clean_sol"]:
            try:
                base_sol = (
                    bytes(trk_bytes[se["offset"] : se["offset"] + se["size"]])
                    if bake_stats.get("sobl_scaled_barriers", 0) > 0
                    else retail_idx["clean_sol"]
                )
                base_sol_crc = (
                    struct.unpack_from("<I", trk_bytes, se["crc_field"])[0]
                    if bake_stats.get("sobl_scaled_barriers", 0) > 0
                    else int(se["crc"])
                )
                buf_s, new_scrc, sobl_spans_full, sobl_patched_count = _patch_sobl_fast(
                    base_sol, base_sol_crc, world
                )
                trk_bytes[se["offset"] : se["offset"] + se["size"]] = buf_s
                struct.pack_into("<I", trk_bytes, se["crc_field"], new_scrc)
            except Exception as exc:
                place_warnings.append(f"SOBL barrier patch warning: {exc}")

    # Patch modified ILIN racing lines (supports both in-place edits AND added/pulled/split/deleted nodes!)
    if progress:
        progress(65, "Patching & recalculating ILIN racing lines...")
    ilin_patched_lines = []
    rebuilt_ild_members: dict[str, bytes] = {}
    for lname, graph in world.all_road_graphs.items():
        if not graph.dirty:
            continue
        entry = next((e for e in entries if e["name"].lower() == lname.lower()), None)
        if not entry:
            continue
        orig_ild = bytes(trk_bytes[entry["offset"] : entry["offset"] + entry["size"]])
        try:
            patched_ild, resized = _encode_road_graph_to_ild(orig_ild, graph)
            if patched_ild == orig_ild:
                continue
            if not resized and len(patched_ild) == entry["size"]:
                trk_bytes[entry["offset"] : entry["offset"] + entry["size"]] = patched_ild
                new_icrc = delta_eod_crc(int(entry["crc"]), orig_ild, patched_ild, [(0, len(patched_ild))])
                struct.pack_into("<I", trk_bytes, entry["crc_field"], new_icrc)
                ilin_patched_lines.append(lname)
            else:
                rebuilt_ild_members[entry["name"]] = patched_ild
                ilin_patched_lines.append(lname)
        except Exception as exc:
            place_warnings.append(f"ILIN {lname} patch warning: {exc}")

    # Convert Make Collision (`world.collision.custom_boxes`) into SOBL boxes so they actually bake into track.sol!
    custom_sobl_baked = 0
    for cbox in world.collision.custom_boxes:
        cmin = cbox.get("min")
        cmax = cbox.get("max")
        if not cmin or not cmax or len(cmin) < 3 or len(cmax) < 3:
            continue
        cx = 0.5 * (float(cmin[0]) + float(cmax[0]))
        cy = 0.5 * (float(cmin[1]) + float(cmax[1]))
        cz = 0.5 * (float(cmin[2]) + float(cmax[2]))
        rx = max(0.25, 0.5 * abs(float(cmax[0]) - float(cmin[0])))
        ry = max(0.25, 0.5 * abs(float(cmax[1]) - float(cmin[1])))
        rz = max(0.25, 0.5 * abs(float(cmax[2]) - float(cmin[2])))
        auto_sobl_boxes.append(
            {
                "centre": (cx, cy, cz),
                "radii": (rx, ry, rz),
                "axes": ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)),
                "surface": str(cbox.get("surface", "wall")),
            }
        )
        custom_sobl_baked += 1

    # Single write of patched trk_bytes instead of 3 redundant 10 MB writes!
    if any(bake_stats.values()) or world.sync_all_events or sobl_patched_count or ilin_patched_lines:
        mod_trk_path.write_bytes(bytes(trk_bytes))

    # Inject editor-placed, replaced & scaled Cars/Wrecks/Props + auto/custom SOBL boxes + resized ILIN + walls noclip
    custom_objects_compiled = 0
    if custom_inject_objs or auto_sobl_boxes or rebuilt_ild_members or world.all_walls_noclip:
        if progress:
            progress(74, f"Compiling {len(custom_inject_objs)} custom props & {len(auto_sobl_boxes)} solid boxes into .TRK...")
        try:
            custom_objects_compiled = _compile_custom_objects_into_trk(
                mod_trk_path,
                world,
                custom_inject_objs,
                place_warnings,
                extra_sobl_boxes=auto_sobl_boxes,
                extra_members=rebuilt_ild_members,
                all_walls_noclip=bool(world.all_walls_noclip),
            )
        except Exception as exc:
            place_warnings.append(f"Custom object .TRK compile warning: {exc}")

    # Write env_override.ini (read by eod.exe modern renderer) + Vulkan HOSTTEX replacements
    env_ini_path = write_env_override_ini(world, out)
    if progress:
        progress(84, "Building Vulkan HOSTTEX replacements...")
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
        f"collision_baked bppt={bake_stats['bppt_planes_patched']} scol={bake_stats['scol_planes_patched']} sobl={sobl_patched_count} sobl_scaled={bake_stats['sobl_scaled_barriers']} sobl_synth={bake_stats['sobl_boxes_synthesized']}",
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

    # Record delta bake state for future <10 ms in-place delta bakes!
    if (
        allow_delta
        and not custom_inject_objs
        and not auto_sobl_boxes
        and not rebuilt_ild_members
        and not world.all_walls_noclip
        and not vif_bake_objs
        and bake_stats["sobl_scaled_barriers"] == 0
        and not any(o.category == "optional-terrain" for o in all_edited_terrain_objs)
    ):
        ge = retail_idx["graf_entry"]
        base_graf = retail_idx["clean_graf"]
        base_graf_crc = int(ge["crc"]) if ge is not None else 0
        # If sync_all_events promoted opt_terrain leaves (record->kind 1 -> 0), mirror those exact 4-byte promotions on base_graf
        if ge is not None and bake_stats["opt_terrain_promoted"] > 0:
            _ensure_graf_patch_tables(retail_idx)
            bg_mut = bytearray(base_graf)
            promo_spans: list[tuple[int, int]] = []
            by_id = retail_idx["by_id"]
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
                if rec_off is not None and rec_off + 4 <= len(bg_mut):
                    if struct.unpack_from("<I", bg_mut, rec_off)[0] == 1:
                        struct.pack_into("<I", bg_mut, rec_off, 0)
                        promo_spans.append((rec_off, rec_off + 4))
            base_graf_crc = delta_eod_crc(base_graf_crc, base_graf, bg_mut, promo_spans)
            base_graf = bytes(bg_mut)
        # Capture initial graf spans so next delta bake cleanly reverts/updates them
        init_graf_spans: list[tuple[int, int]] = []
        if graf_edits and ge is not None:
            _p, _c, init_graf_spans, _cb = _patch_graf_sparse_fast(retail_idx, base_graf, base_graf_crc, graf_edits)
        final_entries = wc.read_directory(mod_trk_path.read_bytes())
        _MOD_DELTA_STATE[mod_key] = {
            "retail_trk_path": retail_trk_abs,
            "retail_sha256": retail_sha_after,
            "mod_size": os.stat(mod_key).st_size,
            "mod_by_name": {e["name"].lower(): e for e in final_entries},
            "member_crcs": {e["name"].lower(): int(e["crc"]) for e in final_entries},
            "graf_entry": next((e for e in final_entries if e["type"] == "GRAF"), None),
            "base_graf_bytes": base_graf,
            "base_graf_crc": base_graf_crc,
            "base_differs": bake_stats["opt_terrain_promoted"] > 0,
            "opt_terrain_promoted": bake_stats["opt_terrain_promoted"],
            "has_custom_members": False,
            "had_vif_bake": False,
            "sync_all_events": bool(world.sync_all_events),
            "last_graf_sig": graf_sig,
            "last_event_sig": event_sig,
            "last_sobl_sig": sobl_sig,
            "last_hosttex_sig": hosttex_sig,
            "last_env_sig": env_sig,
            "last_host_exported": list(host_exported),
            "last_place_changed_bytes": place_changed_bytes,
            "last_sobl_patched": sobl_patched_count,
            "prev_graf_spans": init_graf_spans,
            "prev_sobl_spans": sobl_spans_full,
            "prev_va0_members": {str(e["event"]).lower() for e in event_edits},
            "known_solid_static_ids": {o.obj_id for o in all_edited_terrain_objs if o.category == "static"},
        }
    else:
        _MOD_DELTA_STATE.pop(mod_key, None)

    compile_ms = (time.perf_counter_ns() - t_compile_ns) / 1_000_000.0
    manifest = {
        "schema": "eod.mod_layer/3",
        "bake_mode": "full",
        "compile_ms": round(compile_ms, 2),
        "delta_patched_members": [],
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
        "sobl_scaled_barriers": bake_stats["sobl_scaled_barriers"],
        "sobl_boxes_synthesized": bake_stats["sobl_boxes_synthesized"],
        "ilin_patched_lines": ilin_patched_lines,
        "custom_objects_compiled": custom_objects_compiled,
        "sync_all_events": bool(world.sync_all_events),
        "host_textures_exported": len(host_exported),
        "host_textures": host_exported,
        "editor_placed_objects": sum(
            1 for o in world.objects if o.source != "Retail" and not o.deleted
        ),
        "custom_collision_boxes": len(world.collision.custom_boxes),
        "custom_sobl_baked": custom_sobl_baked,
        "all_walls_noclip": bool(world.all_walls_noclip),
        "env_override_ini": str(env_ini_path),
        "warnings": place_warnings,
        "exported_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    manifest_path = out / "mod_layer_manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    manifest["manifest_path"] = str(manifest_path)
    if progress:
        progress(100, f"Mod layer exported ({compile_ms:.1f} ms)")
    return manifest


def snapshot_world_for_bake(world: TrackWorld) -> TrackWorld:
    """Freeze editor state while sharing the immutable mesh and terrain backing data."""
    collision = world.collision
    read_only = (
        collision._terrain,
        collision.scol_line_verts,
        collision.scol_base_line_verts,
        collision.sobl_line_verts,
        collision.sobl_line_colors,
        collision.bppt_line_verts,
    )
    memo = {id(value): value for value in read_only if value is not None}
    memo[id(world.models)] = dict(world.models)
    return deepcopy(world, memo)


def mark_road_edits_for_fresh_bake(world: TrackWorld, token: Any) -> None:
    """Recover road edits whose dirty flag was cleared by an earlier export."""
    retail = load_track_world(world.trk_path, world.game_dir, include_cars=False, token=token)
    for line_name, graph in world.all_road_graphs.items():
        original = retail.all_road_graphs.get(line_name)
        if original is not None and (graph.nodes != original.nodes or graph.edges != original.edges):
            graph.dirty = True


def package_world_mod(
    world: TrackWorld,
    project_name: str,
    package_id: str,
    version: str,
    name: str,
    output: Path,
    *,
    token: Any,
    progress: Callable[[int, str], None],
) -> dict[str, Any]:
    """Bake an editor snapshot, verify its package, and publish the zip atomically."""
    output = output.expanduser().resolve()
    disc = mod_package.disc_root(world.game_dir)
    if output.is_relative_to(disc):
        raise ValueError("Choose a package destination outside the retail disc folder.")
    if token.is_cancelled:
        raise InterruptedError("Packaging cancelled")

    # A previous export can clear a graph's dirty flag. A fresh bake still needs
    # to include road edits whose nodes or edges differ from the retail graph.
    progress(5, "Checking racing lines against retail")
    mark_road_edits_for_fresh_bake(world, token)
    if token.is_cancelled:
        raise InterruptedError("Packaging cancelled")

    with tempfile.TemporaryDirectory(prefix="eod-world-package-") as directory:
        root = Path(directory)
        layer = root / "overlay"
        package = root / "package"
        report = export_mod_layer(
            world, project_name, layer, token=token, allow_delta=False,
            progress=lambda pct, message: progress(10 + pct * 6 // 10, message),
        )
        if token.is_cancelled:
            raise InterruptedError("Packaging cancelled")
        progress(75, "Building and verifying package contents")
        contents = mod_package.from_overlay(layer, disc, package_id, version, package, name)
        if token.is_cancelled:
            raise InterruptedError("Packaging cancelled")
        progress(90, "Compressing .eodmod archive")
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".eodmod-", dir=output.parent) as publish_dir:
            staged = Path(publish_dir) / output.name
            result = mod_package.zip_package(package, staged)
            if token.is_cancelled:
                raise InterruptedError("Packaging cancelled")
            os.replace(staged, output)
    return {
        **result, "path": str(output), "native": contents["native"], "host": contents["host"],
        "warnings": report["warnings"],
    }


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
