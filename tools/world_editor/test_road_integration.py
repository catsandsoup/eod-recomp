#!/usr/bin/env python3
"""End-to-end integration tests for Procedural 3D Road/Island Builder -> World Studio UI -> .TRK/.eodcol compiler."""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path

from PIL import Image

EDITOR_DIR = Path(__file__).resolve().parent
REPO_ROOT = EDITOR_DIR.parent.parent
TOOLS_DIR = REPO_ROOT / "tools"
for _p in (str(EDITOR_DIR), str(TOOLS_DIR)):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from PySide6.QtWidgets import QApplication

import project_state as ps
import road_builder as rb
import road_collision as rc
import road_spline as rs
import world_graf as wg
import world_loader as wl
from app import configure_opengl_format
from main_window import EodWorldEditorWindow

DISC_DIR = Path("/Users/monty/Library/Application Support/TDEODRecomp/disc")
AFTON_TRK = DISC_DIR / "DATA" / "TRACKS" / "AFTON.TRK"


def _qapp() -> QApplication:
    configure_opengl_format()
    return QApplication.instance() or QApplication(["test-road-integration"])


class TestProceduralRoadIntegration(unittest.TestCase):
    def test_modl_v9_builder_roundtrip_via_world_graf(self) -> None:
        """Procedural BuiltRoad MeshParts encode to valid native MODL v9 (4SPS + EOD MSCAL 0x5C)."""
        plan = rs.make_jump_ramp_plan(start=(0.0, 0.0, 0.0), length=24.0, height=4.0, width=12.0)
        built = rb.build_road_mesh(plan)
        mod_bytes, tex_names = ps._build_modl_v9_from_mesh_parts(built.model.parts, scale=(1.0, 1.0, 1.0))
        self.assertIsNotNone(mod_bytes)
        assert mod_bytes is not None
        self.assertGreater(len(mod_bytes), 0x80)
        self.assertGreaterEqual(len(tex_names), 1)

        mhdr = wg.parse_model(mod_bytes, 0)
        self.assertEqual(mhdr["n_parts"], len(built.model.parts))
        decoded = wg.decode_model(mod_bytes, mhdr)
        decoded_tris = sum(len(p["tris"]) for p in decoded)
        self.assertEqual(decoded_tris, built.model.tri_count)
        for p in decoded:
            self.assertIn(0x5C, p["mscal"])

    def test_roads_menu_exposes_procedural_actions(self) -> None:
        """World Studio Roads menu exposes all 6 procedural 3D road/island builder actions."""
        _qapp()
        win = EodWorldEditorWindow(auto_load=False, show_home_hub=False)
        try:
            roads_menu = next(
                (a.menu() for a in win.menuBar().actions() if a.menu() and "roads" in a.text().replace("&", "").lower()),
                None,
            )
            self.assertIsNotNone(roads_menu)
            assert roads_menu is not None
            texts = [a.text() for a in roads_menu.actions() if not a.isSeparator()]
            expected = [
                "Build 3D Banked Oval Here",
                "Build 3D Figure-8 Track Here",
                "Build 3D Jump Ramp Here",
                "Build 3D Highway Bridge Here",
                "Extrude 3D Road from Active ILIN Spline",
                "Build Organic Terrain Island Here",
            ]
            for label in expected:
                self.assertIn(label, texts)
        finally:
            win.close()

    @unittest.skipUnless(AFTON_TRK.is_file(), f"Retail AFTON.TRK not found at {AFTON_TRK}")
    def test_afton_jump_ramp_bridge_overlay_and_export(self) -> None:
        """Build Jump Ramp & Bridge into real AFTON.TRK, save/reload overlay, compile .TRK + .eodcol + minimap."""
        _qapp()
        world = wl.load_track_world(AFTON_TRK, DISC_DIR)
        win = EodWorldEditorWindow(game_dir=DISC_DIR, auto_load=False, show_home_hub=False)
        try:
            win._attach_loaded_world(world)
            ramp_obj = win.build_procedural_road_preset("ramp")
            bridge_obj = win.build_procedural_road_preset("bridge")
            island_obj = win.build_procedural_island()

            self.assertIsNotNone(ramp_obj)
            self.assertIsNotNone(bridge_obj)
            self.assertIsNotNone(island_obj)
            assert ramp_obj is not None and bridge_obj is not None and island_obj is not None

            self.assertEqual(len(world.procedural_roads), 3)
            self.assertIn(ramp_obj.model_key, world.models)
            self.assertIn(bridge_obj.model_key, world.models)
            self.assertIn(island_obj.model_key, world.models)
            self.assertGreater(len(world.collision.custom_boxes), 0)
            for box in world.collision.custom_boxes:
                self.assertIn("min", box)
                self.assertIn("max", box)
                self.assertIn("centre", box)

            with tempfile.TemporaryDirectory(prefix="eod_road_int_") as tmp:
                tmp_path = Path(tmp)
                overlay_file = ps.save_project_overlay(
                    tmp_path / "afton_roads.eodworld.json",
                    "Afton Roads Test",
                    world,
                )
                self.assertTrue(overlay_file.is_file())

                # Reload a clean TrackWorld and verify overlay restores procedural_roads, models & placed objects
                reloaded = wl.load_track_world(AFTON_TRK, DISC_DIR)
                ps.apply_project_overlay(reloaded, overlay_file)
                self.assertEqual(len(reloaded.procedural_roads), 3)
                self.assertIn(ramp_obj.model_key, reloaded.models)
                self.assertIn(bridge_obj.model_key, reloaded.models)
                self.assertIn(island_obj.model_key, reloaded.models)
                reloaded_keys = {o.model_key for o in reloaded.objects if not o.deleted}
                self.assertIn(ramp_obj.model_key, reloaded_keys)
                self.assertIn(bridge_obj.model_key, reloaded_keys)
                self.assertIn(island_obj.model_key, reloaded_keys)

                # Compile mod layer into TemporaryDirectory
                mod_dir = tmp_path / "mod_out"
                report = ps.export_mod_layer(
                    reloaded,
                    "Afton Roads Test",
                    mod_dir,
                    allow_delta=False,
                )
                self.assertGreaterEqual(int(report["custom_objects_compiled"]), 3)

                eodcol_file = mod_dir / "DATA" / "TRACKS" / "AFTON.eodcol"
                self.assertTrue(eodcol_file.is_file(), f"Missing {eodcol_file}")
                self.assertEqual(Path(report["eodcol_path"]), eodcol_file)
                self.assertGreater(int(report["eodcol_tris"]), 0)

                unpacked = rc.unpack_eodcol(eodcol_file.read_bytes())
                self.assertEqual(len(unpacked.triangles), int(report["eodcol_tris"]))
                self.assertGreater(len(unpacked.triangles), 0)
                self.assertGreater(len(unpacked.wall_boxes), 0)

                minimap_file = mod_dir / "minimap_preview.png"
                self.assertTrue(minimap_file.is_file(), f"Missing {minimap_file}")
                with Image.open(minimap_file) as im:
                    self.assertEqual(im.size, (256, 256))
        finally:
            win.close()


if __name__ == "__main__":
    unittest.main()
