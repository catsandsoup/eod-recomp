"""Unit & headless Qt tests for Car Studio 3D Showroom, Bone.mvl Damage,
Texture/HOSTTEX Studio, and VehicleTunerDialog 4-tab integration.
"""

from __future__ import annotations

import struct
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

EDITOR_DIR = Path(__file__).resolve().parent
TOOLS_DIR = EDITOR_DIR.parent
for _p in (str(EDITOR_DIR), str(TOOLS_DIR)):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from PySide6.QtGui import QImage
from PySide6.QtWidgets import QApplication

from car_studio_showroom import (
    CarShowroomAssembly,
    CarShowroomPanel,
    apply_damage_preview,
    export_car_texture_png,
    load_car_showroom_assembly,
    render_showroom_frame,
    replace_car_texture_png,
    upscale_car_texture_hosttex,
)
from dialogs import VehicleTunerDialog

DISC_DIR = Path("/Users/monty/Library/Application Support/TDEODRecomp/disc")


class TestCarStudioShowroom(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = QApplication.instance() or QApplication(["test_car_studio_showroom"])

    def _require_disc_car(self, car_name: str) -> Path:
        car_path = DISC_DIR / "DATA" / "CAR" / car_name
        if not car_path.is_file():
            self.skipTest(f"Retail disc fixture missing: {car_path}")
        return car_path

    def test_load_real_cars_chassis_wheels_and_bones(self) -> None:
        for car_file in ("ALGAR.CAR", "BUS.CAR"):
            self._require_disc_car(car_file)
            asm = load_car_showroom_assembly(DISC_DIR, car_file, lod_name="Low0")
            self.assertIsInstance(asm, CarShowroomAssembly)
            self.assertGreater(len(asm.chassis_parts), 0, f"{car_file} chassis parts")
            self.assertGreaterEqual(len(asm.wheel_parts), 4, f"{car_file} positioned wheels")
            self.assertGreater(len(asm.bones), 0, f"{car_file} Bone.mvl/Bone.val bones")
            self.assertIn("Chassis", asm.physics_fields)
            self.assertGreater(len(asm.textures), 0, f"{car_file} decoded textures")

            # Verify 4 wheels span both left/right X and front/rear Z
            w_xyz = np.concatenate([p.vertices[:, :3] for p in asm.wheel_parts], axis=0)
            self.assertLess(float(w_xyz[:, 0].min()), -0.3)
            self.assertGreater(float(w_xyz[:, 0].max()), 0.3)
            self.assertLess(float(w_xyz[:, 2].min()), -0.5)
            self.assertGreater(float(w_xyz[:, 2].max()), 0.5)

    def test_damage_preview_zero_deformed_no_nans_and_detach(self) -> None:
        self._require_disc_car("ALGAR.CAR")
        asm = load_car_showroom_assembly(DISC_DIR, "ALGAR.CAR", lod_name="Low0")
        orig_v0 = asm.chassis_parts[0].vertices.copy()

        # 0% damage must match originals identically without mutating
        pristine = apply_damage_preview(asm.chassis_parts, asm.bones, 0.0)
        self.assertEqual(len(pristine), len(asm.chassis_parts))
        for a, b in zip(asm.chassis_parts, pristine):
            np.testing.assert_array_equal(a.vertices, b.vertices)

        # 75% damage must deform measurably, stay deterministic, produce zero NaNs, and not mutate input
        damaged_a = apply_damage_preview(asm.chassis_parts, asm.bones, 75.0)
        damaged_b = apply_damage_preview(asm.chassis_parts, asm.bones, 75.0)
        np.testing.assert_array_equal(asm.chassis_parts[0].vertices, orig_v0)

        max_disp = 0.0
        for orig, d1, d2 in zip(asm.chassis_parts, damaged_a, damaged_b):
            self.assertTrue(np.all(np.isfinite(d1.vertices)))
            np.testing.assert_array_equal(d1.vertices, d2.vertices)
            disp = np.linalg.norm(d1.vertices[:, :3] - orig.vertices[:, :3], axis=1)
            max_disp = max(max_disp, float(disp.max()))
        self.assertGreater(max_disp, 0.05)

        # Panel detachment hides selected index cleanly
        detached = apply_damage_preview(asm.chassis_parts, asm.bones, 50.0, detached_panels={0})
        self.assertEqual(len(detached), len(asm.chassis_parts) - 1)

    def test_render_showroom_frame_non_blank_and_changes_with_damage(self) -> None:
        self._require_disc_car("ALGAR.CAR")
        asm = load_car_showroom_assembly(DISC_DIR, "ALGAR.CAR", lod_name="Low0")

        img0 = render_showroom_frame(asm, width=320, height=200, yaw_deg=-35.0, pitch_deg=18.0, damage_pct=0.0)
        img75 = render_showroom_frame(asm, width=320, height=200, yaw_deg=-35.0, pitch_deg=18.0, damage_pct=75.0)
        self.assertIsInstance(img0, QImage)
        self.assertEqual((img0.width(), img0.height()), (320, 200))

        buf0 = np.frombuffer(img0.constBits(), dtype=np.uint8).reshape((200, 320, 4)).copy()
        buf75 = np.frombuffer(img75.constBits(), dtype=np.uint8).reshape((200, 320, 4)).copy()
        # Non-blank: center region contains car colors distinct from corners
        self.assertGreater(int(buf0[:, :, :3].std()), 12)
        # Damage visibly alters rendered pixels
        diff_pixels = int(np.any(buf0[:, :, :3] != buf75[:, :, :3], axis=2).sum())
        self.assertGreater(diff_pixels, 150)

    def test_texture_export_replace_and_hosttex_eodhtx1(self) -> None:
        self._require_disc_car("ALGAR.CAR")
        asm = load_car_showroom_assembly(DISC_DIR, "ALGAR.CAR", lod_name="Low0")
        tex_key = asm.body_texture_key or next(iter(asm.textures))

        with tempfile.TemporaryDirectory() as tmp:
            tmp_dir = Path(tmp)
            exported = export_car_texture_png(asm, tex_key, tmp_dir / "body_export.png")
            self.assertTrue(exported.is_file())
            with Image.open(exported) as im:
                self.assertEqual(im.mode, "RGBA")
                self.assertGreater(im.width, 0)

            # Create a custom vivid crimson replacement PNG and apply it
            custom_png = tmp_dir / "custom_paint.png"
            Image.new("RGBA", (64, 64), (210, 28, 36, 255)).save(custom_png, format="PNG")
            updated = replace_car_texture_png(asm, tex_key, custom_png)
            self.assertTrue(updated.is_replaced)
            self.assertEqual((updated.width, updated.height), (64, 64))
            self.assertEqual(updated.average_rgb, (210, 28, 36))

            # Upscale to EODHTX1 HOSTTEX file and verify header & mipmap payload size
            eodhtx_path = upscale_car_texture_hosttex(asm, tex_key, tmp_dir / "hosttex", scale=2)
            self.assertTrue(eodhtx_path.is_file())
            raw = eodhtx_path.read_bytes()
            self.assertEqual(raw[:8], b"EODHTX1\0")
            tw, th, mips, flags = struct.unpack_from("<4H", raw, 8)
            self.assertEqual(flags, 0)
            self.assertGreaterEqual(tw, 128)
            self.assertGreaterEqual(th, 128)
            self.assertEqual(mips, max(tw, th).bit_length())
            expected_size = 16 + sum(max(1, tw >> i) * max(1, th >> i) * 4 for i in range(mips))
            self.assertEqual(len(raw), expected_size)

    def test_vehicle_tuner_dialog_four_tabs_and_sync(self) -> None:
        self._require_disc_car("ALGAR.CAR")
        dlg = VehicleTunerDialog(DISC_DIR)
        self.addCleanup(dlg.close)
        dlg.show()
        self.app.processEvents()

        tabs = dlg._vehicle_tabs
        self.assertEqual(tabs.count(), 4)
        labels = [tabs.tabText(i).replace("&&", "&") for i in range(4)]
        self.assertEqual(
            labels,
            ["3D Showroom & Paint", "Handling", "Dyno & Telemetry", "NASCAR Import"],
        )
        self.assertIsInstance(dlg.showroom, CarShowroomPanel)
        self.assertIsNotNone(dlg.showroom.assembly)
        self.assertEqual(dlg.width(), 860)
        self.assertEqual(dlg.height(), 740)

        # Switching damage slider updates viewport cleanly
        dlg.showroom.sld_damage.setValue(65)
        self.app.processEvents()
        self.assertIn("65%", dlg.showroom.lbl_damage.text())
        self.assertIsNotNone(dlg.showroom.viewport.pixmap())
        self.assertFalse(dlg.showroom.viewport.pixmap().isNull())


if __name__ == "__main__":
    unittest.main()
