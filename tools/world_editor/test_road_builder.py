"""Unit tests for procedural 3D road mesh, barriers, bridge piers & organic island."""
from __future__ import annotations

import json
import unittest

import numpy as np

from road_builder import (
    RoadControlPoint,
    RoadPlan,
    build_organic_island,
    build_road_mesh,
    make_banked_oval_plan,
    make_bridge_plan,
    make_figure8_plan,
    make_jump_ramp_plan,
)


class TestRoadBuilder(unittest.TestCase):
    def _assert_valid_part(self, part) -> None:
        self.assertEqual(part.vertices.dtype, np.float32)
        self.assertEqual(part.indices.dtype, np.uint32)
        self.assertEqual(part.vertices.ndim, 2)
        self.assertEqual(part.vertices.shape[1], 8)
        self.assertGreater(len(part.vertices), 0)
        self.assertGreater(len(part.indices), 0)
        self.assertEqual(len(part.indices) % 3, 0)
        self.assertEqual(part.tri_count, len(part.indices) // 3)
        self.assertTrue(np.all(np.isfinite(part.vertices)))
        self.assertLess(int(part.indices.max()), len(part.vertices))
        norms = np.linalg.norm(part.vertices[:, 3:6], axis=1)
        self.assertTrue(np.allclose(norms, 1.0, atol=1e-3))

    def test_all_four_presets_valid_meshes_and_collision(self) -> None:
        presets = [
            make_banked_oval_plan(),
            make_figure8_plan(),
            make_jump_ramp_plan(),
            make_bridge_plan(),
        ]
        for plan in presets:
            with self.subTest(preset=plan.name):
                built = build_road_mesh(plan, ground_fn=lambda x, z: -2.0)
                model = built.model
                self.assertEqual(model.model_store, "procedural:road")
                self.assertEqual(len(model.hash_hex), 16)
                self.assertEqual(model.tri_count, sum(p.tri_count for p in model.parts))
                self.assertGreater(len(model.parts), 0)
                self.assertGreater(len(built.ground_quads), 0)
                self.assertGreater(len(built.wall_boxes), 0)
                for p in model.parts:
                    self._assert_valid_part(p)
                local_xyz = np.vstack([p.vertices[:, :3] for p in model.parts])
                self.assertTrue(np.allclose(model.bbox_min, local_xyz.min(axis=0), atol=1e-4))
                self.assertTrue(np.allclose(model.bbox_max, local_xyz.max(axis=0), atol=1e-4))
                self.assertEqual(built.ground_quads[0].shape, (4, 3))
                self.assertEqual(built.ground_quads[0].dtype, np.float32)
                for wb in built.wall_boxes:
                    self.assertEqual(len(wb["centre"]), 3)
                    self.assertTrue(all(r > 0 for r in wb["radii"]))
                    axes = np.asarray(wb["axes"], dtype=np.float64)
                    self.assertTrue(np.allclose(axes @ axes.T, np.eye(3), atol=1e-4))

    def test_bridge_piers_reach_ground(self) -> None:
        plan = make_bridge_plan()
        built_g = build_road_mesh(plan, ground_fn=lambda x, z: -15.0)
        deck_parts = [p for p in built_g.model.parts if p.texture == plan.deck_tex]
        self.assertEqual(len(deck_parts), 1)
        world_y = deck_parts[0].vertices[:, 1] + built_g.model.pivot[1]
        self.assertLessEqual(float(world_y.min()), -16.9)

        built_default = build_road_mesh(plan, ground_fn=None)
        deck_def = [p for p in built_default.model.parts if p.texture == plan.deck_tex][0]
        world_y_def = deck_def.vertices[:, 1] + built_default.model.pivot[1]
        bridge_max_y = float(built_default.stations_xyz[:, 1].max())
        self.assertLessEqual(float(world_y_def.min()), bridge_max_y - 11.9)

    def test_junction_mouths_open_barriers(self) -> None:
        pts = [
            RoadControlPoint(0.0, 0.0, 0.0, width=16.0),
            RoadControlPoint(0.0, 0.0, 40.0, width=16.0),
            RoadControlPoint(0.0, 0.0, 80.0, width=16.0, is_junction=True),
            RoadControlPoint(0.0, 0.0, 120.0, width=16.0),
            RoadControlPoint(0.0, 0.0, 160.0, width=16.0),
        ]
        plan = RoadPlan(name="junction_test", points=pts, step_m=5.0, median_barrier=True)
        built = build_road_mesh(plan)
        self.assertEqual(len(built.junction_mouths), 1)
        j_pos, j_rad = built.junction_mouths[0]
        self.assertAlmostEqual(j_rad, 12.0, places=3)
        self.assertGreater(len(built.wall_boxes), 0)
        for wb in built.wall_boxes:
            c = np.asarray(wb["centre"], dtype=np.float64)
            # Axial distance along Z from junction must exceed half_w + 2.0 (10.0m)
            self.assertGreater(abs(float(c[2] - j_pos[2])), 8.5)

    def test_organic_island(self) -> None:
        part = build_organic_island((50.0, -30.0), 40.0, 30.0, height=8.0, rings=8, sectors=24, seed=7)
        self._assert_valid_part(part)
        self.assertEqual(part.texture, "W_dirt.tex")
        self.assertTrue(np.all(part.vertices[:, 4] > 0.0))
        self.assertAlmostEqual(float(part.vertices[:, 1].max()), 8.0, places=4)
        self.assertAlmostEqual(float(part.vertices[:, 1].min()), 0.0, places=4)
        other = build_organic_island((50.0, -30.0), 40.0, 30.0, height=8.0, rings=8, sectors=24, seed=99)
        self.assertFalse(np.allclose(part.vertices[:, :3], other.vertices[:, :3]))

    def test_json_round_trip(self) -> None:
        plan = make_figure8_plan()
        restored = RoadPlan.from_dict(json.loads(json.dumps(plan.to_dict())))
        a = build_road_mesh(plan)
        b = build_road_mesh(restored)
        self.assertEqual(a.model.hash_hex, b.model.hash_hex)
        self.assertEqual(a.model.tri_count, b.model.tri_count)
        self.assertEqual(len(a.ground_quads), len(b.ground_quads))
        self.assertEqual(len(a.wall_boxes), len(b.wall_boxes))


if __name__ == "__main__":
    unittest.main()
