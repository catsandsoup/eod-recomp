"""Unit tests for Sprint 2B road_collision module."""
from __future__ import annotations

import unittest
import numpy as np

from road_builder import (
    RoadControlPoint,
    RoadPlan,
    build_road_mesh,
    make_banked_oval_plan,
    make_bridge_plan,
    make_jump_ramp_plan,
)
from road_collision import (
    SURFACE_OBJECT,
    SURFACE_ROAD,
    SURFACE_TERRAIN,
    build_unified_collision_32bit,
    open_eod_junction,
    pack_eodcol,
    query_ground_y_32bit,
    rasterize_minimap_rgba,
    sculpt_terrain_along_corridor,
    synthesize_road_graph,
    unpack_eodcol,
)
from world_loader import MeshPart, WorldModel, WorldObject


def _make_ramp_model(key: str = "ramp_test") -> WorldModel:
    """Create a 10m x 10m sloped ramp (local Y = 0 at z=-5 to Y = 4 at z=+5) plus a vertical back wall."""
    # Sloped top face (ny = 10/sqrt(116) ~= 0.928 >= 0.5) + vertical back wall (z = +5, ny = 0 < 0.5)
    verts = np.array([
        [-5.0, 0.0, -5.0, 0, 1, 0, 0, 0],
        [ 5.0, 0.0, -5.0, 0, 1, 0, 1, 0],
        [ 5.0, 4.0,  5.0, 0, 1, 0, 1, 1],
        [-5.0, 4.0,  5.0, 0, 1, 0, 0, 1],
        # Vertical back wall (z = 5, y = 0..4)
        [-5.0, 0.0,  5.0, 0, 0, 1, 0, 0],
        [ 5.0, 0.0,  5.0, 0, 0, 1, 1, 0],
        [ 5.0, 4.0,  5.0, 0, 0, 1, 1, 1],
        [-5.0, 4.0,  5.0, 0, 0, 1, 0, 1],
    ], dtype=np.float32)
    idx = np.array([0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7], dtype=np.uint32)
    part = MeshPart(texture="concrete", vertices=verts, indices=idx, tri_count=4)
    return WorldModel(
        key=key,
        name="Ramp",
        category="stunt",
        model_store="custom",
        hash_hex="deadbeef",
        parts=[part],
        tri_count=4,
        textures=["concrete"],
        bbox_min=np.array([-5.0, 0.0, -5.0], dtype=np.float32),
        bbox_max=np.array([5.0, 4.0, 5.0], dtype=np.float32),
        pivot=(0.0, 0.0, 0.0),
    )


def _make_world_object(obj_id: str, model_key: str, pos, scale=(1.0, 1.0, 1.0), deleted=False) -> WorldObject:
    return WorldObject(
        obj_id=obj_id,
        name=obj_id,
        model_key=model_key,
        source="custom",
        category="stunt",
        subtype="ramp",
        section_mlist="",
        model_store="custom",
        hash_hex="deadbeef",
        tri_count=4,
        textures=["concrete"],
        position=pos,
        rotation_hpr=(0.0, 0.0, 0.0),
        scale=scale,
        deleted=deleted,
    )


class TestRoadCollision(unittest.TestCase):
    def test_open_eod_junction_selective_flattening(self) -> None:
        ident = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]
        records = [
            {"index": 10, "kind": "box", "centre": [100.0, 2.0, 50.0], "radii": [2.0, 1.8, 6.0], "axes": ident, "above_ground": 0.0, "surface_name": "wall"},
            {"index": 11, "kind": "box", "centre": [104.0, 25.0, 50.0], "radii": [2.0, 1.5, 2.0], "axes": ident, "above_ground": 0.0, "surface_name": "overpass"},
            {"index": 12, "kind": "tube", "centre": [100.0, 2.0, 50.0], "radii": [2.0, 2.0, 2.0], "axes": ident, "above_ground": 0.0, "surface_name": "pole"},
            {"index": 13, "kind": "box", "centre": [250.0, 2.0, 250.0], "radii": [3.0, 1.6, 3.0], "axes": ident, "above_ground": 0.0, "surface_name": "distant"},
        ]
        edits: dict[int, dict] = {}
        opened = open_eod_junction(records, edits, [(np.array([105.0, 1.0, 50.0], dtype=np.float32), 8.0)])
        self.assertEqual(opened, [10])
        self.assertEqual(set(edits.keys()), {10})
        self.assertAlmostEqual(edits[10]["ry"], 0.05)
        self.assertTrue(edits[10]["flattened_by_junction"])
        self.assertAlmostEqual(edits[10]["orig_ry"], 1.8)

    def test_sculpt_terrain_flattens_hill_and_preserves_bridges(self) -> None:
        bridge = build_road_mesh(make_bridge_plan())
        # Dense grid across x in [-40, 40], z in [0, 160] with hills on approach (z=10), under bridge (z=80), and far away (x=35)
        xs = np.linspace(-40.0, 40.0, 41, dtype=np.float32)
        zs = np.linspace(0.0, 160.0, 81, dtype=np.float32)
        gx, gz = np.meshgrid(xs, zs)
        gy = (
            6.0 * np.exp(-((gx) ** 2 + (gz - 10.0) ** 2) / 18.0)   # hill poking through non-bridge approach
            + 5.0 * np.exp(-((gx) ** 2 + (gz - 80.0) ** 2) / 18.0) # hill right under bridge center
            + 9.0 * np.exp(-((gx - 35.0) ** 2 + (gz - 10.0) ** 2) / 12.0)  # distant hill
        ).astype(np.float32)
        verts = np.column_stack([gx.ravel(), gy.ravel(), gz.ravel()])

        sculpted, modified = sculpt_terrain_along_corridor(verts, bridge, shoulder_skirt_m=8.0, clearance_m=0.08)
        self.assertGreater(modified, 0)
        # Non-bridge approach (x=0, z=10) hill must be sculpted down near road_y - 0.08
        idx_approach = int(np.argmin(np.hypot(verts[:, 0] - 0.0, verts[:, 2] - 10.0)))
        self.assertLess(sculpted[idx_approach, 1], verts[idx_approach, 1] - 2.0)
        # Under bridge center (x=0, z=80) terrain must NOT be sculpted
        idx_under_bridge = int(np.argmin(np.hypot(verts[:, 0] - 0.0, verts[:, 2] - 80.0)))
        self.assertAlmostEqual(float(sculpted[idx_under_bridge, 1]), float(verts[idx_under_bridge, 1]), places=5)
        # Distant hill (x=35, z=10) must be untouched
        idx_far = int(np.argmin(np.hypot(verts[:, 0] - 35.0, verts[:, 2] - 10.0)))
        self.assertAlmostEqual(float(sculpted[idx_far, 1]), float(verts[idx_far, 1]), places=5)

    def test_unified_collision_32bit_query_and_pack_roundtrip(self) -> None:
        oval = build_road_mesh(make_banked_oval_plan())
        ramp_model = _make_ramp_model("ramp_test")
        # Moved to (200, 10, 300) and scaled (2x in X, 1.5x in Y, 2x in Z):
        # world z spans [290, 310], world y spans [10, 16]; at (200, 300) mid-ramp -> y = 13.0!
        ramp_obj = _make_world_object("ramp_1", "ramp_test", pos=(200.0, 10.0, 300.0), scale=(2.0, 1.5, 2.0))
        deleted_obj = _make_world_object("ramp_del", "ramp_test", pos=(200.0, 50.0, 300.0), deleted=True)
        base_tri = np.array([[[-20.0, -2.0, -20.0], [20.0, -2.0, -20.0], [0.0, -2.0, 20.0]]], dtype=np.float32)

        mesh = build_unified_collision_32bit(
            built_roads=[oval],
            edited_objects=[ramp_obj, deleted_obj],
            models={"ramp_test": ramp_model},
            base_scol_tris=base_tri,
        )
        self.assertEqual(mesh.triangles.dtype, np.uint32)
        self.assertIn(SURFACE_TERRAIN, mesh.surface_ids)
        self.assertIn(SURFACE_ROAD, mesh.surface_ids)
        self.assertIn(SURFACE_OBJECT, mesh.surface_ids)
        # Road barriers + 1 OBB for the ramp's vertical back wall (deleted_obj excluded)
        self.assertEqual(len(mesh.wall_boxes), len(oval.wall_boxes) + 1)

        # Raycast hits scaled ramp surface accurately at center & upper edge
        self.assertAlmostEqual(query_ground_y_32bit(mesh, 200.0, 300.0), 13.0, places=3)
        self.assertAlmostEqual(query_ground_y_32bit(mesh, 200.0, 305.0), 14.5, places=3)
        # Raycast hits procedural oval road station
        st0 = oval.stations_xyz[0]
        road_y = query_ground_y_32bit(mesh, float(st0[0]), float(st0[2]))
        self.assertIsNotNone(road_y)
        self.assertAlmostEqual(road_y, float(st0[1]), delta=0.35)
        # Raycast misses empty space
        self.assertIsNone(query_ground_y_32bit(mesh, -9999.0, -9999.0))

        # Pack & unpack round-trip bit-exact
        blob = pack_eodcol(mesh)
        unpacked = unpack_eodcol(blob)
        self.assertTrue(np.array_equal(unpacked.vertices, mesh.vertices))
        self.assertTrue(np.array_equal(unpacked.triangles, mesh.triangles))
        self.assertTrue(np.array_equal(unpacked.surface_ids, mesh.surface_ids))
        self.assertTrue(np.array_equal(unpacked.aabb_min, mesh.aabb_min))
        self.assertTrue(np.array_equal(unpacked.aabb_max, mesh.aabb_max))
        self.assertEqual(unpacked.wall_boxes, mesh.wall_boxes)
        self.assertEqual(pack_eodcol(unpacked), blob)

    def test_takeoff_open_gap_and_landing_are_distinct_contact_surfaces(self) -> None:
        """A sidecar geometry check, independent of vehicle suspension and native gameplay."""
        takeoff = build_road_mesh(make_jump_ramp_plan(start=(0, 0, 0), length=40, height=5.5, width=14))
        landing = build_road_mesh(make_bridge_plan(start=(0, 5.5, 50), end=(0, 5.5, 90), deck_height=0, width=14))
        surface = build_unified_collision_32bit([takeoff, landing], [], {})
        restored = unpack_eodcol(pack_eodcol(surface))
        heights = [query_ground_y_32bit(restored, 0, z) for z in range(1, 40)]
        self.assertTrue(all(y is not None for y in heights))
        self.assertTrue(all(-.01 <= b-a < .5 for a,b in zip(heights, heights[1:])))
        for x in (-1.0, 1.0):
            self.assertIsNone(query_ground_y_32bit(restored, x, 45))
            self.assertAlmostEqual(query_ground_y_32bit(restored, x, 55), 5.5, delta=.1)

    def test_synthesize_road_graph_closed_and_open_with_banking(self) -> None:
        oval_plan = make_banked_oval_plan()
        oval = build_road_mesh(oval_plan)
        g_oval, sp_oval = synthesize_road_graph(oval, line_name="oval.ild")
        self.assertEqual(len(g_oval.nodes), len(oval.stations_xyz))
        self.assertEqual(len(g_oval.edges), len(oval.stations_xyz))  # closed loop wraps last -> 0
        self.assertEqual((g_oval.edges[-1].a, g_oval.edges[-1].b), (len(oval.stations_xyz) - 1, 0))
        self.assertTrue(all(12.0 <= v <= 62.0 for v in sp_oval))

        # Verify banking boosts cornering speed on identical curved geometry
        flat_pts = [
            RoadControlPoint(p.x, p.y, p.z, width=p.width, bank_deg=0.0, barrier_left=p.barrier_left, barrier_right=p.barrier_right)
            for p in oval_plan.points
        ]
        flat_oval = build_road_mesh(RoadPlan(name="flat_oval", points=flat_pts, closed=True, step_m=oval_plan.step_m))
        _, sp_flat = synthesize_road_graph(flat_oval, line_name="flat.ild")
        banked_mask = np.abs(oval.bank_deg) > 8.0
        self.assertTrue(np.any(banked_mask))
        self.assertGreater(float(np.mean(np.array(sp_oval)[banked_mask])), float(np.mean(np.array(sp_flat)[banked_mask])))

        # Open straight highway graph
        hwy = build_road_mesh(make_bridge_plan())
        g_hwy, sp_hwy = synthesize_road_graph(hwy, line_name="hwy.ild")
        self.assertEqual(len(g_hwy.edges), len(hwy.stations_xyz) - 1)
        self.assertTrue(all(e.two_way for e in g_hwy.edges))
        self.assertGreater(float(np.mean(sp_hwy)), 55.0)

    def test_rasterize_minimap_rgba(self) -> None:
        oval = build_road_mesh(make_banked_oval_plan())
        g_oval, _ = synthesize_road_graph(oval)
        rgba, bounds = rasterize_minimap_rgba([oval], [g_oval], size=128)
        self.assertEqual(len(rgba), 128 * 128 * 4)
        arr = np.frombuffer(rgba, dtype=np.uint8).reshape(128, 128, 4)
        # Non-blank: road pixels (bright asphalt) and green start marker present
        self.assertGreater(int(np.count_nonzero(arr[:, :, 0] > 170)), 50)
        self.assertGreater(int(np.count_nonzero((arr[:, :, 1] > 180) & (arr[:, :, 0] < 100))), 5)
        min_x, max_x, min_z, max_z = bounds
        self.assertLess(min_x, max_x)
        self.assertLess(min_z, max_z)


if __name__ == "__main__":
    unittest.main()
