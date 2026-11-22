"""Unit tests for road_spline.py."""
from __future__ import annotations

import json
import unittest

import numpy as np

from road_spline import (
    RoadControlPoint,
    RoadPlan,
    catmull_rom_sample,
    compute_station_frames,
    grade_limited_profile,
    make_banked_oval_plan,
    make_bridge_plan,
    make_figure8_plan,
    make_jump_ramp_plan,
    resample_polyline,
)


class TestRoadSpline(unittest.TestCase):
    def test_plan_json_roundtrip(self) -> None:
        plan = make_bridge_plan(start=(10.0, 2.0, -5.0), end=(10.0, 2.0, 155.0), deck_height=12.0, width=16.0)
        plan.median_barrier = True
        plan.points[0].is_junction = True
        plan.points[0].barrier_left = "guardrail"
        raw = json.dumps(plan.to_dict())
        restored = RoadPlan.from_dict(json.loads(raw))
        self.assertEqual(restored, plan)

    def test_catmull_rom_open_and_closed(self) -> None:
        pts = np.array([[0.0, 0.0, 0.0], [0.0, 0.0, 20.0], [20.0, 0.0, 40.0], [40.0, 0.0, 40.0]])
        open_s = catmull_rom_sample(pts, closed=False, samples_per_seg=10)
        self.assertEqual(open_s.shape, (31, 3))
        np.testing.assert_allclose(open_s[0], pts[0], atol=1e-9)
        np.testing.assert_allclose(open_s[10], pts[1], atol=1e-9)
        np.testing.assert_allclose(open_s[-1], pts[-1], atol=1e-9)

        sq = np.array([[10.0, 0.0, 0.0], [0.0, 0.0, 10.0], [-10.0, 0.0, 0.0], [0.0, 0.0, -10.0]])
        closed_s = catmull_rom_sample(sq, closed=True, samples_per_seg=8)
        self.assertEqual(closed_s.shape, (32, 3))
        np.testing.assert_allclose(closed_s[0], sq[0], atol=1e-9)
        np.testing.assert_allclose(closed_s[8], sq[1], atol=1e-9)
        self.assertEqual(catmull_rom_sample(pts[:1]).shape, (1, 3))

    def test_resample_polyline_uniform(self) -> None:
        line = np.array([[0.0, 0.0, 0.0], [0.0, 0.0, 60.0]])
        st, arc_s, t_params = resample_polyline(line, step_m=6.0, closed=False)
        self.assertEqual(len(st), 11)
        np.testing.assert_allclose(np.diff(arc_s), 6.0, atol=1e-9)
        self.assertAlmostEqual(t_params[0], 0.0)
        self.assertAlmostEqual(t_params[-1], 1.0)
        np.testing.assert_allclose(st[-1], [0.0, 0.0, 60.0])

        angles = np.linspace(0.0, 2.0 * np.pi, 64, endpoint=False)
        ring = np.column_stack([30.0 * np.cos(angles), np.zeros_like(angles), 30.0 * np.sin(angles)])
        st_c, arc_c, t_c = resample_polyline(ring, step_m=6.0, closed=True)
        seg_d = np.linalg.norm(np.roll(st_c, -1, axis=0) - st_c, axis=1)
        self.assertTrue(np.allclose(seg_d, seg_d[0], rtol=0.05))
        self.assertAlmostEqual(arc_c[0], 0.0)
        self.assertLess(t_c[-1], 1.0)

    def test_grade_limited_profile_clamps_and_holds_pins(self) -> None:
        arc_s = np.arange(0.0, 306.0, 6.0)  # 51 stations, 300 m total
        y_raw = np.where(arc_s >= 150.0, 10.0, 0.0)  # sharp 10 m cliff at midpoint
        pinned = np.zeros(len(arc_s), dtype=bool)
        pinned[[0, 25, 50]] = True
        y_raw[0] = 0.0
        y_raw[25] = 10.0
        y_raw[50] = 0.0

        y_out = grade_limited_profile(arc_s, y_raw, pinned, max_grade=0.08, smooth_passes=30)
        np.testing.assert_allclose(y_out[pinned], y_raw[pinned], atol=1e-12)
        grades = np.abs(np.diff(y_out) / np.diff(arc_s))
        self.assertLessEqual(float(np.max(grades)), 0.08 + 1e-9)
        self.assertGreater(y_out[12], 0.5)
        self.assertGreater(y_out[37], 0.5)

    def test_compute_station_frames_lh_and_banked(self) -> None:
        pts = np.column_stack([np.zeros(6), np.zeros(6), np.arange(6) * 10.0])
        t, r, u = compute_station_frames(pts, np.zeros(6), closed=False)
        np.testing.assert_allclose(t, np.tile([0.0, 0.0, 1.0], (6, 1)), atol=1e-9)
        np.testing.assert_allclose(r, np.tile([1.0, 0.0, 0.0], (6, 1)), atol=1e-9)
        np.testing.assert_allclose(u, np.tile([0.0, 1.0, 0.0], (6, 1)), atol=1e-9)

        banks = np.linspace(-20.0, 20.0, 6)
        t2, r2, u2 = compute_station_frames(pts, banks, closed=False)
        for i in range(6):
            self.assertAlmostEqual(float(np.linalg.norm(t2[i])), 1.0, places=9)
            self.assertAlmostEqual(float(np.linalg.norm(r2[i])), 1.0, places=9)
            self.assertAlmostEqual(float(np.linalg.norm(u2[i])), 1.0, places=9)
            self.assertAlmostEqual(float(np.dot(t2[i], r2[i])), 0.0, places=9)
            self.assertAlmostEqual(float(np.dot(t2[i], u2[i])), 0.0, places=9)
            self.assertAlmostEqual(float(np.dot(r2[i], u2[i])), 0.0, places=9)
            np.testing.assert_allclose(np.cross(r2[i], u2[i]), t2[i], atol=1e-9)
            self.assertAlmostEqual(float(r2[i, 1]), np.sin(np.deg2rad(banks[i])), places=9)

    def test_presets_end_to_end(self) -> None:
        oval = make_banked_oval_plan(center=(10, 5, -20), rx=90.0, rz=55.0, width=18.0, bank_deg=14.0)
        fig8 = make_figure8_plan(center=(0, 0, 0), rx=85.0, rz=50.0, width=15.0)
        ramp = make_jump_ramp_plan(start=(0, 0, 0), length=40.0, height=5.5, width=14.0)
        bridge = make_bridge_plan(start=(0, 0, 0), end=(0, 0, 160), deck_height=10.0, width=14.0)

        self.assertTrue(oval.closed)
        self.assertAlmostEqual(max(p.bank_deg for p in oval.points), 14.0, places=6)
        self.assertTrue(fig8.closed)
        self.assertTrue(any(p.is_bridge for p in fig8.points))
        self.assertFalse(ramp.closed)
        self.assertAlmostEqual(ramp.points[-1].y, 5.5, places=6)
        self.assertFalse(bridge.closed)
        self.assertTrue(any(p.is_bridge for p in bridge.points))
        self.assertAlmostEqual(max(p.y for p in bridge.points), 10.0, places=6)

        for plan in (oval, fig8, ramp, bridge):
            ctrl = np.array([[p.x, p.y, p.z] for p in plan.points], dtype=np.float64)
            dense = catmull_rom_sample(ctrl, closed=plan.closed, samples_per_seg=12)
            st, arc_s, _ = resample_polyline(dense, step_m=plan.step_m, closed=plan.closed)
            t, r, u = compute_station_frames(st, np.zeros(len(st)), closed=plan.closed)
            self.assertEqual(t.shape, st.shape)
            self.assertGreater(arc_s[-1], 30.0)
            np.testing.assert_allclose(np.cross(r, u), t, atol=1e-9)


if __name__ == "__main__":
    unittest.main()
