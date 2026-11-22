"""Unit tests for Car Studio Dyno, Gearbox Sawtooth & Pacejka Tyre graphs."""

from __future__ import annotations

import os
import unittest

import numpy as np

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtGui import QImage
from PySide6.QtWidgets import QApplication

from car_studio_graphs import (
    CarTelemetryGraphsPanel,
    compute_dyno_curve,
    compute_gearbox_sawtooth,
    compute_pacejka_curve,
    estimate_performance_summary,
    parse_physics_dict,
)


class TestCarStudioGraphs(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls._app = QApplication.instance() or QApplication([])

    def _sample_phys(self) -> dict:
        return {
            "idle_rpm": 850,
            "redline": 6800,
            "torque_max": 340,
            "torque_rpm": 4200,
            "power_max": 365,
            "power_rpm": 6000,
            "rear_end_ratio": 3.73,
            "g1": 3.35,
            "g2": 2.05,
            "g3": 1.40,
            "g4": 1.00,
            "g5": 0.78,
            "g6": 0.0,
            "tire_diameter": 0.68,
            "mass": 1520.0,
            "Front": {"magic_b": 10.5, "magic_c": 1.9, "magic_d": 1.08, "magic_e": 0.97, "grip": 1.0},
            "Rear": {"magic_b": 11.0, "magic_c": 1.9, "magic_d": 1.12, "magic_e": 0.97, "grip": 1.0},
            "FrontDirt.magic_d": 0.78,
            "FrontDirt.grip": 0.75,
        }

    def test_parse_physics_dict_normalizes_nested_and_dotted(self) -> None:
        p = parse_physics_dict(self._sample_phys())
        self.assertAlmostEqual(p["redline"], 6800.0)
        self.assertAlmostEqual(p["Front"]["magic_d"], 1.08)
        self.assertAlmostEqual(p["FrontDirt"]["magic_d"], 0.78)
        self.assertAlmostEqual(p["FrontDirt"]["grip"], 0.75)

    def test_dyno_curve_identity_and_monotonic_rpm(self) -> None:
        phys = self._sample_phys()
        rpm, torque, hp = compute_dyno_curve(phys, samples=96)
        self.assertEqual(len(rpm), 96)
        self.assertTrue(np.all(np.diff(rpm) > 0.0))
        self.assertAlmostEqual(float(rpm[0]), 850.0, places=3)
        self.assertAlmostEqual(float(rpm[-1]), 6800.0, places=3)
        expected_hp = torque * rpm / 5252.0
        self.assertLess(float(np.max(np.abs(hp - expected_hp))), 1e-2)
        self.assertAlmostEqual(float(np.max(torque)), 340.0, delta=15.0)
        self.assertAlmostEqual(float(np.max(hp)), 365.0, delta=20.0)

    def test_gearbox_sawtooth_monotonic_and_inverse_final_drive(self) -> None:
        phys = self._sample_phys()
        gears_a = compute_gearbox_sawtooth(phys)
        self.assertEqual([g["gear_num"] for g in gears_a], [1, 2, 3, 4, 5])
        max_speeds = [g["max_mph"] for g in gears_a]
        self.assertTrue(all(b > a for a, b in zip(max_speeds, max_speeds[1:])))
        self.assertAlmostEqual(gears_a[-1]["top_speed_mph"], max_speeds[-1], places=4)

        phys_tall = dict(phys, rear_end_ratio=phys["rear_end_ratio"] * 0.5)
        gears_b = compute_gearbox_sawtooth(phys_tall)
        for ga, gb in zip(gears_a, gears_b):
            self.assertAlmostEqual(gb["max_mph"], ga["max_mph"] * 2.0, places=4)

    def test_pacejka_curve_shape_and_bounds(self) -> None:
        x, y = compute_pacejka_curve(b=10.0, c=1.9, d=1.15, e=0.97, grip=0.9, samples=128)
        self.assertEqual(len(x), 128)
        self.assertTrue(np.all(np.isfinite(y)))
        self.assertAlmostEqual(float(x[0]), 0.0)
        self.assertAlmostEqual(float(y[0]), 0.0, places=6)
        self.assertGreater(float(y[2]), 0.0)
        self.assertAlmostEqual(float(np.max(y)), 0.9 * 1.15, delta=0.03)

    def test_performance_summary_scales_with_hp_and_mass(self) -> None:
        base = self._sample_phys()
        s_base = estimate_performance_summary(base)
        self.assertGreater(s_base["est_0_60_s"], 2.0)
        self.assertLess(s_base["est_0_60_s"], 10.0)

        double_hp = dict(base, power_max=base["power_max"] * 2.0, torque_max=base["torque_max"] * 2.0)
        s_hp = estimate_performance_summary(double_hp)
        self.assertLess(s_hp["est_0_60_s"], s_base["est_0_60_s"])
        self.assertLess(s_hp["est_quarter_mile_s"], s_base["est_quarter_mile_s"])
        self.assertGreater(s_hp["est_quarter_trap_mph"], s_base["est_quarter_trap_mph"])

        half_mass = dict(base, mass=base["mass"] * 0.5)
        s_light = estimate_performance_summary(half_mass)
        self.assertLess(s_light["est_0_60_s"], s_base["est_0_60_s"])
        self.assertLess(s_light["est_quarter_mile_s"], s_base["est_quarter_mile_s"])
        self.assertAlmostEqual(s_light["power_to_weight_hp_ton"], s_base["power_to_weight_hp_ton"] * 2.0, places=3)

    def test_telemetry_panel_renders_offscreen(self) -> None:
        panel = CarTelemetryGraphsPanel(self._sample_phys())
        panel.resize(780, 460)
        panel.show()
        self._app.processEvents()

        # Update with high-output tune & render offscreen into a QImage
        panel.set_physics({"power_max": 520, "torque_max": 480, "redline": 7500, "g6": 0.65})
        self._app.processEvents()
        self.assertIn("HP", panel._kpi_labels["peak_hp"].text())

        img = QImage(panel.size(), QImage.Format.Format_ARGB32)
        img.fill(0)
        panel.render(img)
        self.assertFalse(img.isNull())
        self.assertGreater(img.width(), 0)
        panel.close()


if __name__ == "__main__":
    unittest.main()
