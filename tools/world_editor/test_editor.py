#!/usr/bin/env python3
"""Automated verification suite for EOD World Editor (Qt 6 PySide6 + OpenGL 3.3 Core + PIE).

Verifies:
  1. Retail .TRK discovery (15 tracks) & DATA/CAR/*.CAR vehicle/wreck library scan (37 cars)
  2. Real loading of AFTON.TRK and DUNDAS.TRK (models, instances, tris, textures,
     ILIN racing lines, SCOL terrain + surface normals, SOBL barriers, Start Grid markers)
  3. 3D studio thumbnail rendering (track models + retail Car/Wreck models + off-thread QImage)
  4. Twinmotion/UE 'Replace Selection With...', Ghost Placement Preview, Scatter / Paint Brush,
     Grid & Angle Snap, Marker 3D gizmo undo/redo
  5. Environment / Skybox presets & cross-track sky borrow + Texture/Material PNG swap
  6. SOBL Barrier height editing & one-click 'Remove / Flatten Track Barriers'
  7. Custom Race & AI Pathfinding (ILIN) config + Unreal-style PIE launch spec & widget toggle
  8. Real in-game Mod Write-Back (Ctrl+Shift+S): GRAF moves + deletes, SOBL barrier patches,
     ILIN edits, Vulkan HOSTTEX (.eodhtx + replacements.tsv), PROFILE.TXT, retail SHA-256 UNTOUCHED
  9. Live OpenGL 3.3 Core rendering (glError == 0) across normal, Ghost, Scatter, Sunset/Night,
     live PNG texture swap, Collision overlay, and Road Tool
 10. Non-blocking QThreadPool FunctionWorker + CancelToken execution
"""

from __future__ import annotations

import hashlib
import json
import random
import struct
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch

EDITOR_DIR = Path(__file__).resolve().parent
if str(EDITOR_DIR) not in sys.path:
    sys.path.insert(0, str(EDITOR_DIR))

from PIL import Image
from PySide6 import __version__ as PYSIDE6_VERSION
from PySide6.QtCore import QPoint, QPointF, Qt
from PySide6.QtGui import QNativeGestureEvent, QPointingDevice, QWheelEvent
from PySide6.QtWidgets import QApplication, QDialog, QFileDialog, QMessageBox

import pie_host
import project_state as ps
import world_loader as wl
from app import configure_opengl_format
from main_window import EodWorldEditorWindow
from theme import apply_dark_theme
from thumbnails import render_model_thumbnail, render_model_thumbnail_qimage
from viewport_gl import ViewportGL
from workers import CancelToken, FunctionWorker, editor_thread_pool


def _sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class TestPieLaunchSpec(unittest.TestCase):
    def test_trackpad_events_pan_orbit_and_zoom_without_editing(self) -> None:
        configure_opengl_format()
        app = QApplication.instance() or QApplication(["test-viewport-navigation"])
        viewport = ViewportGL()
        viewport.resize(800, 540)
        position = QPointF(460, 310)
        no_modifier = Qt.KeyboardModifier.NoModifier

        def wheel(pixel, angle, modifiers=no_modifier, phase=Qt.ScrollPhase.NoScrollPhase):
            event = QWheelEvent(position, position, QPoint(*pixel), QPoint(*angle),
                                Qt.MouseButton.NoButton, modifiers, phase, False)
            app.sendEvent(viewport, event)
            self.assertTrue(event.isAccepted())

        def gesture(kind, value=0.0):
            event = QNativeGestureEvent(kind, QPointingDevice.primaryPointingDevice(), 2,
                                        position, position, position, value, QPointF())
            app.sendEvent(viewport, event)
            self.assertTrue(event.isAccepted())

        try:
            original = viewport.camera_state()
            wheel((24, -14), (0, 120), phase=Qt.ScrollPhase.ScrollUpdate)
            panned = viewport.camera_state()
            self.assertNotEqual(panned["pos"], original["pos"])
            self.assertEqual(panned["yaw"], original["yaw"])
            self.assertEqual(panned["fly_speed"], original["fly_speed"])
            wheel((100, 50), (0, 0), phase=Qt.ScrollPhase.ScrollMomentum)
            self.assertEqual(viewport.camera_state(), panned)
            wheel((0, 0), (0, 0), phase=Qt.ScrollPhase.ScrollEnd)

            wheel((0, 0), (30, -30), Qt.KeyboardModifier.ShiftModifier)
            orbited = viewport.camera_state()
            self.assertNotEqual(orbited["yaw"], panned["yaw"])
            self.assertNotEqual(orbited["pitch"], panned["pitch"])
            self.assertNotEqual(orbited["pos"], panned["pos"])

            wheel((0, 0), (0, 120), Qt.KeyboardModifier.ControlModifier)
            zoomed = viewport.camera_state()
            self.assertNotEqual(zoomed["pos"], orbited["pos"])
            self.assertEqual(zoomed["yaw"], orbited["yaw"])
            self.assertEqual(zoomed["fly_speed"], original["fly_speed"])

            gesture(Qt.NativeGestureType.BeginNativeGesture)
            gesture(Qt.NativeGestureType.ZoomNativeGesture, 0.15)
            pinched = viewport.camera_state()
            self.assertNotEqual(pinched["pos"], zoomed["pos"])
            self.assertEqual(pinched["yaw"], zoomed["yaw"])
            gesture(Qt.NativeGestureType.RotateNativeGesture, 10.0)
            self.assertAlmostEqual(viewport.cam_yaw, (pinched["yaw"] + 10.0) % 360.0)
            gesture(Qt.NativeGestureType.EndNativeGesture)
            self.assertIsNone(viewport._native_camera_anchor)

            viewport._lmb_down = True
            editing = viewport.camera_state()
            wheel((0, 20), (0, 0))
            self.assertEqual(viewport.camera_state(), editing)
            viewport._lmb_down = False
            viewport._rmb_down = True
            wheel((0, 0), (0, 120))
            self.assertGreater(viewport.fly_speed, original["fly_speed"])
            self.assertEqual(viewport.camera_state()["pos"], editing["pos"])
        finally:
            viewport.close()

    def test_compact_editor_exposes_panels_and_playback_controls(self) -> None:
        configure_opengl_format()
        app = QApplication.instance() or QApplication(["test-editor-layout"])
        apply_dark_theme(app)
        with tempfile.TemporaryDirectory() as directory:
            window = EodWorldEditorWindow(game_dir=Path(directory), auto_load=False)
            try:
                window.resize(800, 540)
                window.show()
                app.processEvents()
                self.assertEqual(window.size().toTuple(), (800, 540))
                self.assertTrue(window.explorer.isVisible())
                self.assertFalse(window.properties.isVisible())
                self.assertTrue(window.cmb_pie_event.isVisible())
                self.assertTrue(window.cmb_pie_car.isVisible())
                self.assertTrue(window.cmb_renderer_mode.isVisible())
                for toolbar in (window.main_toolbar, window.settings_toolbar):
                    for action in toolbar.actions():
                        if not action.isSeparator():
                            self.assertTrue(toolbar.widgetForAction(action).isVisible(), action.text())
                window.act_panel_inspector.trigger()
                window.center_stack.setCurrentWidget(window.pie_host)
                app.processEvents()
                self.assertTrue(window.properties.isVisible())
                self.assertFalse(window.explorer.isVisible())
                self.assertGreaterEqual(window.center_stack.width(), window.viewport.minimumWidth())
                for widget in (window.pie_host.btn_stop, window.pie_host.btn_restart,
                               window.pie_host.btn_ai_drive, window.pie_host.cmb_arcade):
                    self.assertTrue(widget.isVisible())
                    corner = widget.mapTo(window.pie_host.bar, widget.rect().bottomRight())
                    self.assertTrue(window.pie_host.bar.rect().contains(corner))
                window._filter_explorer_category("car-wreck")
                self.assertTrue(window.explorer.isVisible())
                self.assertFalse(window.properties.isVisible())
                window.resize(1440, 840)
                app.processEvents()
                self.assertTrue(window.explorer.isVisible())
                self.assertTrue(window.properties.isVisible())
            finally:
                window.close()
                app.processEvents()

    def test_macos_editor_disc_discovery_matches_game_layout(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            expected = home / "Library" / "Application Support" / "TDEODRecomp" / "disc"
            (expected / "DATA" / "TRACKS").mkdir(parents=True)
            with patch.dict("os.environ", {}, clear=True), patch.object(wl.sys, "platform", "darwin"), \
                    patch.object(Path, "home", return_value=home):
                self.assertEqual(wl.default_game_dir(), expected)

    def test_macos_launch_resolves_native_binary_and_disc_root(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            disc = Path(directory).resolve()
            binary = disc / "eod"
            binary.touch()
            with patch.dict("os.environ", {}, clear=True), patch.object(pie_host.sys, "platform", "darwin"):
                spec = pie_host.build_pie_launch_spec(disc, disc / "mod", "Afton", renderer_mode="plume")
            self.assertEqual(spec["exe"], str(binary))
            self.assertEqual(spec["args"][1:4], ["--windowed", "--data", str(disc)])

    def test_explicit_pie_executable_overrides_discovery(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "my-eod"
            with patch.dict("os.environ", {"EOD_PIE_EXE": str(binary)}, clear=True):
                self.assertEqual(pie_host.resolve_eod_exe("."), binary.resolve())

    def test_renderer_options_select_distinct_backends(self) -> None:
        with patch.dict("os.environ", {"EOD_NATIVE_ONLY": "1", "EOD_PLUME": "0", "EOD_LOOK": "modern"}, clear=True):
            classic = pie_host.build_pie_launch_spec(".", "test", "Afton", renderer_mode="ps2")
            plume = pie_host.build_pie_launch_spec(".", "test", "Afton", renderer_mode="plume")

        self.assertEqual(classic["env"]["EOD_NATIVE"], "0")
        self.assertEqual(classic["env"]["EOD_MODERN"], "0")
        self.assertNotIn("EOD_NATIVE_ONLY", classic["env"])
        self.assertNotIn("EOD_LOOK", classic["env"])
        self.assertNotIn("--require-plume", classic["args"])
        self.assertEqual(plume["env"]["EOD_NATIVE"], "1")
        self.assertEqual(plume["env"]["EOD_LOOK"], "modern")
        self.assertEqual(plume["env"]["EOD_MODERN"], "0")
        self.assertEqual(plume["env"]["EOD_PLUME"], "1")
        self.assertNotIn("EOD_NATIVE_ONLY", plume["env"])
        self.assertEqual(plume["args"][-3:], ["--renderer", "vulkan", "--require-plume"])

    def test_laps_are_forwarded_only_to_action_events(self) -> None:
        for mode, track, event, expected in (
            ("event", "Afton", "No-Rules Race", "laps=7"),
            ("free_roam", "Afton", "No-Rules Race", None),
            ("career_roam", "HomeBase", "No-Rules Race", None),
            ("event", "HomeBase", "Career Overworld (Free Roam)", None),
        ):
            with self.subTest(mode=mode, event=event):
                spec = pie_host.build_pie_launch_spec(".", "test", track, mode=mode, event_name=event, laps=7)
                parts = spec["env"]["EOD_DIRECT_START"].split(";")
                self.assertEqual(next((part for part in parts if part.startswith("laps=")), None), expected)


class TestEodWorldEditor(unittest.TestCase):
    app: QApplication
    game_dir: Path
    afton_world: wl.TrackWorld
    dundas_world: wl.TrackWorld
    afton_hash_before: str

    @classmethod
    def setUpClass(cls) -> None:
        configure_opengl_format()
        cls.app = QApplication.instance() or QApplication(["test_editor"])
        apply_dark_theme(cls.app)
        cls.game_dir = wl.default_game_dir()
        afton_path = cls.game_dir / "DATA" / "TRACKS" / "AFTON.TRK"
        dundas_path = cls.game_dir / "DATA" / "TRACKS" / "DUNDAS.TRK"
        assert afton_path.is_file(), f"Missing retail track: {afton_path}"
        assert dundas_path.is_file(), f"Missing retail track: {dundas_path}"
        cls.afton_hash_before = _sha256_file(afton_path)
        cls.afton_world = wl.load_track_world(afton_path, cls.game_dir, include_cars=True)
        cls.dundas_world = wl.load_track_world(dundas_path, cls.game_dir, include_cars=True)

    @classmethod
    def tearDownClass(cls) -> None:
        from workers import shutdown_editor_workers

        shutdown_editor_workers()

    def test_01_discover_retail_tracks_and_cars(self) -> None:
        tracks = wl.discover_tracks(self.game_dir)
        names = [p.name.upper() for p in tracks]
        self.assertGreaterEqual(len(tracks), 15)
        self.assertIn("AFTON.TRK", names)
        self.assertIn("DUNDAS.TRK", names)

        cars = wl.discover_cars(self.game_dir)
        self.assertGreaterEqual(len(cars), 37)
        self.assertEqual(self.afton_world.car_count, len(cars))
        car_models = [m for m in self.afton_world.models.values() if m.category == "car-wreck"]
        self.assertEqual(len(car_models), 37)
        self.assertIn("car:BUS.CAR", self.afton_world.models)
        self.assertIn("car:COPCAR.CAR", self.afton_world.models)
        self.assertIn("car:ALGAR.CAR", self.afton_world.models)
        print(f"[VERIFIED] DATA/CAR library: {len(car_models)}/37 retail Cars & Wrecks loaded with meshes & textures")

    def test_02_afton_and_dundas_load_numbers(self) -> None:
        for name, w in (("AFTON.TRK", self.afton_world), ("DUNDAS.TRK", self.dundas_world)):
            self.assertGreater(len(w.models), 1000, f"{name} models")
            self.assertGreater(len(w.objects), 1000, f"{name} instances")
            self.assertGreater(w.total_triangles, 40000, f"{name} triangles")
            self.assertGreater(len(w.textures), 100, f"{name} textures")
            self.assertGreater(len(w.road_graph.nodes), 200, f"{name} ILIN nodes")
            self.assertGreater(w.collision.scol_count, 3000, f"{name} SCOL tris")
            self.assertGreater(w.collision.sobl_boxes, 500, f"{name} SOBL boxes")
            # Verify SCOL ground normal sampling & Start Grid markers
            gy, nrm = w.collision.ground_sample(w.world_center[0], w.world_center[2])
            self.assertGreater(nrm[1], 0.0)
            grid_markers = [m for m in w.markers if m.kind in ("Spawn", "Start Grid")]
            self.assertGreaterEqual(len(grid_markers), 4)
            print(
                f"[VERIFIED] {name}: models={len(w.models)} (cars={w.car_count}), instances={len(w.objects)}, "
                f"tris={w.total_triangles}, textures={len(w.textures)}, "
                f"ilin_nodes={len(w.road_graph.nodes)} ({w.road_graph.line_name}, {len(w.all_road_graphs)} lines), "
                f"scol_tris={w.collision.scol_count}, sobl_boxes={w.collision.sobl_boxes}, start_grid={len(grid_markers)}"
            )

    def test_03_thumbnail_rendering_scenery_and_cars(self) -> None:
        first_model = next(iter(self.afton_world.models.values()))
        pm = render_model_thumbnail(first_model, self.afton_world.textures)
        self.assertFalse(pm.isNull())
        self.assertEqual(pm.width(), 112)
        self.assertEqual(pm.height(), 80)

        bus = self.afton_world.models["car:BUS.CAR"]
        qimg = render_model_thumbnail_qimage(bus, self.afton_world.textures)
        self.assertFalse(qimg.isNull())
        self.assertEqual(qimg.width(), 112)
        self.assertEqual(qimg.height(), 80)

    def test_04_replace_with_scatter_ghost_snapping_and_markers(self) -> None:
        win = EodWorldEditorWindow(game_dir=self.game_dir, auto_load=False)
        win.world = wl.load_track_world(self.afton_world.trk_path, self.game_dir, include_cars=True)
        win.viewport.set_world(win.world)
        win.explorer.set_world(win.world)
        win._update_window_chrome()

        # Verify all 14 top-level menus intact
        menu_titles = [a.text().replace("&", "") for a in win.menuBar().actions()]
        expected_menus = [
            "File", "Edit", "World", "View", "Camera", "Objects", "Markers", "Cars",
            "Breakers", "Roads", "Terrain", "Mesh", "Races", "Collision", "Help",
        ]
        self.assertEqual(menu_titles, expected_menus)

        # 1. Twinmotion/UE 'Replace With...' -> swap scenery prop into BUS.CAR wreck & Undo/Redo!
        obj = win.world.objects[15]
        orig_key = obj.model_key
        win.viewport.select_object(obj)
        self.assertEqual(win.properties.stack.currentIndex(), 1)
        ok = win.replace_object_model(obj, "car:BUS.CAR")
        self.assertTrue(ok)
        self.assertEqual(obj.model_key, "car:BUS.CAR")
        self.assertIn("Bus", obj.name)
        win.undo_stack.undo()
        self.assertEqual(obj.model_key, orig_key)
        win.undo_stack.redo()
        self.assertEqual(obj.model_key, "car:BUS.CAR")

        # 2. Ghost Placement Preview mode + stamp snapped to SCOL normal
        win._activate_ghost_place_for("car:COPCAR.CAR")
        self.assertEqual(win.viewport.active_tool, "place")
        self.assertEqual(win.viewport.ghost_model_key, "car:COPCAR.CAR")
        self.assertFalse(win.viewport.place_hud.isHidden())
        cx, _cy, cz = win.world.world_center
        gy, nrm = win.world.collision.ground_sample(cx, cz)
        rot = wl.normal_to_pitch_roll(nrm, 45.0)
        before_cnt = len(win.world.active_objects)
        win._on_ghost_stamped("car:COPCAR.CAR", (cx, gy, cz), rot, (1.0, 1.0, 1.0))
        self.assertEqual(len(win.world.active_objects), before_cnt + 1)
        win.undo_stack.undo()
        self.assertEqual(len(win.world.active_objects), before_cnt)

        # 3. Scatter / Paint Brush tool + HUD + single undoable batch stroke
        win._activate_scatter_for("car:TAXI.CAR")
        self.assertEqual(win.viewport.active_tool, "scatter")
        self.assertFalse(win.viewport.scatter_hud.isHidden())
        win.viewport.scatter_hud.spn_radius.setValue(24.0)
        win.viewport.scatter_hud.spn_density.setValue(5)
        items = win.viewport.scatter_at_point(cx, cz, "car:TAXI.CAR", rng=random.Random(42))
        self.assertGreaterEqual(len(items), 3)
        placed_batch = win._on_scatter_stroke(items)
        self.assertEqual(len(placed_batch), len(items))
        self.assertEqual(len(win.world.active_objects), before_cnt + len(items))
        # Single undo removes the entire scatter stroke!
        win.undo_stack.undo()
        self.assertEqual(len(win.world.active_objects), before_cnt)
        win.undo_stack.redo()
        self.assertEqual(len(win.world.active_objects), before_cnt + len(items))

        # 4. Grid & Angle Snap toggles
        win.act_grid_snap.setChecked(True)
        win.act_angle_snap.setChecked(True)
        self.assertTrue(win.viewport.grid_snap_enabled)
        self.assertTrue(win.viewport.angle_snap_enabled)
        self.assertAlmostEqual(win.viewport._snap_val(12.6, 1.0, True), 13.0)
        self.assertAlmostEqual(win.viewport._snap_val(23.0, 15.0, True), 30.0)

        # 5. Spawns / Start Grid markers + undoable move/rotate
        mk = win.world.markers[0]
        orig_mp = mk.position
        orig_mh = mk.heading_deg
        win.viewport.select_marker(mk)
        self.assertEqual(win.properties.stack.currentIndex(), 3)
        win._on_viewport_marker_transformed(
            mk, orig_mp, orig_mh, (orig_mp[0] + 6.0, orig_mp[1], orig_mp[2] - 4.0), (orig_mh + 30.0) % 360.0, "Move spawn"
        )
        self.assertAlmostEqual(mk.position[0], orig_mp[0] + 6.0, places=3)
        win.undo_stack.undo()
        self.assertEqual(mk.position, orig_mp)

        win.close()
        self.app.processEvents()

    def test_05_environment_texture_swap_sobl_barriers_and_real_mod_writeback(self) -> None:
        win = EodWorldEditorWindow(game_dir=self.game_dir, auto_load=False)
        win.world = wl.load_track_world(self.afton_world.trk_path, self.game_dir, include_cars=True)
        win.viewport.set_world(win.world)
        win.explorer.set_world(win.world)
        win._update_window_chrome()

        # 1. Move one retail static object & delete another retail static object
        static_objs = [o for o in win.world.objects if o.source == "Retail" and o.category == "static" and o.retail_instance_id]
        self.assertGreater(len(static_objs), 20)
        moved_obj = static_objs[5]
        deleted_obj = static_objs[6]
        orig_pos = moved_obj.position
        win._on_prop_obj_transform(
            moved_obj, (orig_pos[0] + 4.5, orig_pos[1] + 1.5, orig_pos[2] - 2.0), moved_obj.rotation_hpr, (1.0, 1.0, 1.0)
        )
        win.delete_object(deleted_obj)
        self.assertTrue(deleted_obj.deleted)

        # 2. Edit ILIN racing line node
        g = win.world.road_graph
        n0 = next(iter(g.nodes.values()))
        win._on_prop_road_pos(n0, (n0.x + 2.5, n0.y, n0.z - 1.5))
        self.assertTrue(g.dirty)

        # 3. SOBL barrier individual height edit + one-click Flatten All Track Barriers!
        tall_barriers = [r for r in win.world.collision.sobl_records if r.get("kind") == "box" and r.get("vertical") and r["above_ground"] >= 10.0]
        self.assertGreater(len(tall_barriers), 100)
        b0_idx = tall_barriers[0]["index"]
        win.apply_barrier_height(b0_idx, 1.5)
        self.assertLessEqual(win.world.collision.sobl_records[b0_idx]["above_ground"], 2.0)
        flat_count = win.flatten_all_track_barriers(-0.5)
        self.assertGreater(flat_count, 500)
        # Verify undo/redo restores barriers cleanly
        win.undo_stack.undo()
        self.assertEqual(len(win.world.collision.sobl_edits), 1)
        win.undo_stack.redo()
        self.assertGreater(len(win.world.collision.sobl_edits), 500)

        # 4. Environment / Skybox preset + borrow DUNDAS sky for HOSTTEX export
        win.open_environment_panel()
        self.assertEqual(win.properties.stack.currentIndex(), 4)
        win.properties.env_preset_combo.setCurrentText("Golden Sunset")
        self.assertEqual(win.world.environment.preset_name, "Golden Sunset")
        win.world.environment.borrowed_sky_track = "DUNDAS"

        # 5. Live PNG texture replacement + real mod write-back (GRAF + SOBL + ILIN + HOSTTEX!)
        target_tex = next(
            (name for name, ta in win.world.textures.items() if ta.raw_tex_bytes and not name.startswith("car/")),
            None,
        )
        self.assertIsNotNone(target_tex)
        assert target_tex is not None

        with tempfile.TemporaryDirectory() as tmp:
            tmp_p = Path(tmp)
            png_path = tmp_p / "custom_road_hd.png"
            img = Image.new("RGBA", (256, 256), (220, 80, 40, 255))
            img.save(png_path)

            ok = win.replace_texture_from_png(target_tex, png_path)
            self.assertTrue(ok)
            self.assertTrue(win.world.textures[target_tex].is_replaced)

            # Save overlay round-trip
            ov = ps.save_project_overlay(tmp_p / "afton_v2.eodworld.json", "Afton Studio Mod", win.world)
            self.assertTrue(ov.is_file())
            fresh = wl.load_track_world(self.afton_world.trk_path, self.game_dir, include_cars=True)
            ps.apply_project_overlay(fresh, ov)
            self.assertEqual(fresh.environment.preset_name, "Golden Sunset")
            self.assertTrue(fresh.textures[target_tex].is_replaced)
            self.assertGreater(len(fresh.collision.sobl_edits), 500)

            # Real Mod Write-Back (Ctrl+Shift+S)
            mod_out = tmp_p / "mods" / "Afton_Studio_Mod"
            report = ps.export_mod_layer(win.world, "Afton Studio Mod", mod_out)

            self.assertTrue(Path(report["mod_trk"]).is_file())
            self.assertTrue(report["mod_trk_differs_from_retail"])
            self.assertGreater(report["changed_payload_bytes"], 0)
            self.assertEqual(report["deleted_retail_objects"], 1)
            self.assertGreater(report["sobl_barriers_patched"], 500)
            self.assertIn("track.ild", report["ilin_patched_lines"])
            self.assertGreaterEqual(report["host_textures_exported"], 1)
            self.assertTrue(Path(report["profile_txt"]).is_file())
            self.assertTrue(Path(report["custom_race_json"]).is_file())

            # Validate Vulkan HOSTTEX/replacements.tsv and .eodhtx headers!
            tsv = mod_out / "HOSTTEX" / "replacements.tsv"
            self.assertTrue(tsv.is_file())
            tsv_lines = tsv.read_text(encoding="utf-8").strip().splitlines()
            self.assertEqual(tsv_lines[0], "eod-host-textures 1")
            self.assertGreaterEqual(len(tsv_lines), 2)
            first_rel = tsv_lines[1].split("\t")[4]
            htx_bytes = (mod_out / "HOSTTEX" / first_rel).read_bytes()
            self.assertEqual(htx_bytes[:8], b"EODHTX1\0")
            hw, hh, hlvls, hflags = struct.unpack_from("<4H", htx_bytes, 8)
            self.assertEqual(hflags, 0)
            self.assertEqual(hlvls, max(hw, hh).bit_length())

            print(
                f"[VERIFIED] Mod Write-Back: placements={report['retail_placement_edits']} "
                f"(deleted={report['deleted_retail_objects']}, changed_bytes={report['changed_payload_bytes']}), "
                f"sobl_patched={report['sobl_barriers_patched']}, ilin={report['ilin_patched_lines']}, "
                f"hosttex={report['host_textures_exported']} (.eodhtx validated)"
            )

        # Verify retail AFTON.TRK SHA-256 is 100% UNTOUCHED!
        self.assertEqual(_sha256_file(self.afton_world.trk_path), self.afton_hash_before)

        # 6. Verify PIE launch spec & CustomRaceConfig integration
        win.world.custom_race.event_type = "Battle (8 AI)"
        win.world.custom_race.player_car = "Bus"
        win.world.custom_race.ai_count = 8
        spec = pie_host.build_pie_launch_spec(
            self.game_dir,
            self.game_dir / "mods" / "Afton_Studio_Mod",
            win.world.display_name,
            mode="event",
            event_name=win.world.custom_race.event_type,
            car_name=win.world.custom_race.player_car,
            ai_count=win.world.custom_race.ai_count,
        )
        self.assertEqual(spec["env"]["EOD_SKIP_MOVIES"], "1")
        self.assertIn("track=Afton", spec["direct_start"])
        self.assertIn("car=Bus", spec["direct_start"])
        self.assertIn("EOD_MOD_DIR", spec["env"])
        print(f"[VERIFIED] PIE Spec: EOD_DIRECT_START='{spec['direct_start']}'")

        win.close()
        self.app.processEvents()

    def test_06_live_opengl33_core_render_all_new_modes(self) -> None:
        from OpenGL import GL

        win = EodWorldEditorWindow(game_dir=self.game_dir, auto_load=False)
        win.world = wl.load_track_world(self.afton_world.trk_path, self.game_dir, include_cars=True)
        win.viewport.set_world(win.world)
        win.explorer.set_world(win.world)
        win._update_window_chrome()
        win.show()
        for _ in range(20):
            self.app.processEvents()
            time.sleep(0.015)

        vp = win.viewport
        self.assertTrue(vp._gl_ready, "OpenGL 3.3 Core context failed to initialize")
        self.assertGreater(vp._last_tris_drawn, 50000)
        self.assertGreater(vp._last_draw_calls, 50)

        # Exercise: placed Car/Wreck + Ghost Place hologram + Scatter brush ring + Sunset/Night + Collision + Live PNG swap
        cx, _cy, cz = win.world.world_center
        gy = win.world.collision.ground_height(cx, cz)
        win._place_model_at("car:BUS.CAR", cx, gy, cz)
        win._activate_ghost_place_for("car:COPCAR.CAR")
        vp.ghost_pos = vp.cam_pos.copy()
        vp.ghost_pos[1] = gy
        self.app.processEvents()

        win._activate_scatter_for("car:TAXI.CAR")
        vp.scatter_hover_pos = vp.cam_pos.copy()
        win.world.environment.apply_preset("Golden Sunset")
        win._toggle_collision(True)
        self.app.processEvents()

        win.world.environment.apply_preset("Anoka Night")
        target_tex = next(iter(win.world.textures.keys()))
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "live_swap.png"
            Image.new("RGBA", (64, 64), (30, 220, 120, 255)).save(p)
            win.replace_texture_from_png(target_tex, p)
            self.app.processEvents()

        vp.makeCurrent()
        err = GL.glGetError()
        gl_ver = GL.glGetString(GL.GL_VERSION)
        vp.doneCurrent()
        self.assertEqual(err, 0, f"Unexpected OpenGL 3.3 error: {err}")
        print(f"[VERIFIED] Live OpenGL 3.3 Core (all Phase 1 modes): {gl_ver} | glError=0 | draws={vp._last_draw_calls} | tris={vp._last_tris_drawn}")

        win.close()
        self.app.processEvents()

    def test_07_background_qthreadpool_worker_nonblocking(self) -> None:
        done: list[Any] = []
        token = CancelToken(1)

        def job(val: int, *, token=None, progress=None):
            if progress:
                progress(50, "halfway")
            return val * 2

        from workers import start_worker
        # 50/50 stress test: prove main-thread janitor prevents PySide6 worker-thread GC signal drops
        for i in range(50):
            tok = CancelToken(i + 1)
            w = FunctionWorker(job, i, token=tok, label=f"stress_{i}")
            w.signals.finished.connect(lambda g, res: done.append((g, res)))
            start_worker(w, priority=10)

        deadline = time.perf_counter() + 6.0
        while len(done) < 50 and time.perf_counter() < deadline:
            self.app.processEvents()
            time.sleep(0.005)

        self.assertEqual(len(done), 50, f"Dropped worker signals: received {len(done)}/50")
        print("[VERIFIED] QThreadPool non-blocking FunctionWorker (start_worker) 50/50 = 100% delivery")

    def test_08_lighting_perf_dual_renderer_and_output_log(self) -> None:
        import numpy as np
        from pie_host import build_pie_launch_spec

        # 1. Verify mutually exclusive PS2 Fast vs Plume HQ PIE env vars + Free Roam uses Battle (NOT #0 Jump Race!)
        spec_ps2 = build_pie_launch_spec(self.game_dir, self.game_dir / "mods" / "Test", "Afton", mode="free_roam", ai_count=0, renderer_mode="ps2")
        spec_plume = build_pie_launch_spec(self.game_dir, self.game_dir / "mods" / "Test", "Afton", mode="event", event_name="No-Rules Race", car_name="Bus", renderer_mode="plume")
        self.assertEqual(spec_ps2["env"]["EOD_NATIVE"], "0")
        self.assertEqual(spec_ps2["env"]["EOD_MODERN"], "0")
        self.assertIn("event=Battle", spec_ps2["direct_start"])
        self.assertIn("ai=0", spec_ps2["direct_start"])
        self.assertEqual(spec_plume["env"]["EOD_MODERN"], "0")
        self.assertEqual(spec_plume["env"]["EOD_NATIVE"], "1")
        self.assertEqual(spec_plume["env"]["EOD_LOOK"], "modern")
        self.assertIn("event=No-Rules Race", spec_plume["direct_start"])
        self.assertIn("car=Bus", spec_plume["direct_start"])

        # 2. Verify live lit framebuffer is bright (never pitch-black), fast (<25ms),
        #    idle timer is stopped (0% idle CPU), and QPainter labels don't clobber GL state.
        win = EodWorldEditorWindow(game_dir=self.game_dir, auto_load=False)
        win.world = self.afton_world
        win.viewport.set_world(win.world)
        win.show()
        self.app.processEvents()

        vp = win.viewport
        self.assertFalse(vp._timer.isActive(), "Demand-driven flight timer must be stopped when idle")

        for mode in ("ps2", "plume"):
            vp.set_render_mode(mode)
            vp.show_lighting = True
            vp.repaint()
            self.app.processEvents()
            if mode == "plume" and vp._plume_viewport is not None and vp._plume_viewport.presented > 0:
                # Native Vulkan/Metal Plume child surface presents directly to its own swapchain (not QOpenGLWidget FBO)
                self.assertGreater(vp._plume_viewport.triangles, 1000, "Native Plume viewport rendered too few triangles")
            else:
                img = vp.grabFramebuffer()
                arr = np.frombuffer(img.constBits(), dtype=np.uint8).reshape(img.height(), img.width(), 4)
                mean_luma = float(arr[:, :, :3].mean())
                self.assertGreater(mean_luma, 60.0, f"{mode} lit AFTON frame too dark (mean_luma={mean_luma:.1f})")
            self.assertLess(vp._last_frame_ms, 25.0, f"{mode} frame too slow ({vp._last_frame_ms:.2f} ms)")

        # Labels + Lighting ON across 2 OpenGL frames must not clobber GL state
        vp.set_render_mode("ps2")
        vp.show_labels = True
        vp.repaint()
        self.app.processEvents()
        vp.repaint()
        self.app.processEvents()
        img_lbl = vp.grabFramebuffer()
        arr_lbl = np.frombuffer(img_lbl.constBits(), dtype=np.uint8).reshape(img_lbl.height(), img_lbl.width(), 4)
        self.assertGreater(float(arr_lbl[:, :, :3].mean()), 60.0, "QPainter labels clobbered OpenGL lighting state")

        # 3. Verify Output Log drawer + Verbose + Copy Log + append_log
        win.append_log("[TEST] PIE diagnostic line", verbose=True)
        self.assertIn("[TEST] PIE diagnostic line", win.log_text.toPlainText())
        win.chk_log_verbose.setChecked(False)
        win.append_log("[TEST] hidden verbose line", verbose=True)
        self.assertNotIn("[TEST] hidden verbose line", win.log_text.toPlainText())
        win._copy_output_log()
        cb = self.app.clipboard()
        if cb is not None:
            self.assertIn("[TEST] PIE diagnostic line", cb.text())
        print(
            f"[VERIFIED] Lighting + Perf + Dual-Renderer + Output Log: "
            f"frame={vp._last_frame_ms:.2f}ms | idle_timer={vp._timer.isActive()} | lit_luma={arr_lbl[:,:,:3].mean():.1f}"
        )

        win.close()
        self.app.processEvents()

    def test_09_pie_canonical_event_templates_and_ilin_sync(self) -> None:
        # 1. Canonical PIE event & car name mapping
        self.assertEqual(pie_host.canonicalize_event_name("c_figure8_jump_race"), "Figure-8 Jump Race")
        self.assertEqual(pie_host.canonicalize_event_name("Battle (Free Roam)"), "Battle")
        self.assertEqual(pie_host.canonicalize_event_name("All Templates (Combined)"), "Battle")
        self.assertEqual(pie_host.canonicalize_event_name("gauntlet"), "Gauntlet")
        self.assertEqual(pie_host.canonicalize_car_name("bulldo_1"), "Bulldog")
        self.assertEqual(pie_host.canonicalize_car_name("COPCAR.CAR"), "COPCAR")

        win = EodWorldEditorWindow(game_dir=self.game_dir, auto_load=False)
        win._attach_loaded_world(wl.load_track_world(self.afton_world.trk_path, self.game_dir, include_cars=True))

        # 2. Event Template dropdown populated with All Templates + canonical track templates
        items = [win.cmb_pie_event.itemText(i) for i in range(win.cmb_pie_event.count())]
        self.assertEqual(items[0], "All Templates (Combined)")
        self.assertIn("Battle (Free Roam)", items)
        self.assertIn("Figure-8 Jump Race", items)
        self.assertIn("Gauntlet", items)

        # 3. Switching Event Template updates viewport filter AND switches active .ild racing line
        fig8_tpl = win.world.event_templates["Figure-8 Jump Race"]
        win.cmb_pie_event.setCurrentText("Figure-8 Jump Race")
        self.assertEqual(win.world.active_event_template, "Figure-8 Jump Race")
        if fig8_tpl.ilin_member and fig8_tpl.ilin_member in win.world.all_road_graphs:
            self.assertEqual(win.world.road_graph.line_name, fig8_tpl.ilin_member)

        # 4. Sync All Events toggle (default ON) + live toolbar -> build_current_pie_spec wiring
        self.assertTrue(win.act_sync_events.isChecked())
        self.assertTrue(win.world.sync_all_events)
        win.cmb_pie_car.setCurrentText("Bus")
        win.spn_pie_ai.setValue(6)
        win.world.custom_race.arcade_mode = "tag"

        # Free Roam on a specific template forces solo ai=0 on that template
        spec_roam = win.build_current_pie_spec(self.game_dir / "mods" / "TestSpec", "free_roam")
        self.assertIn("event=Figure-8 Jump Race", spec_roam["direct_start"])
        self.assertIn("car=Bus", spec_roam["direct_start"])
        self.assertIn("ai=0", spec_roam["direct_start"])
        self.assertIn("--windowed", spec_roam["args"])
        self.assertEqual(spec_roam["env"]["EOD_OPT_TERRAIN_MASK"], "0xFFFF")
        self.assertEqual(spec_roam["env"]["EOD_ARCADE_MODE"], "tag")

        # Play Event honors live AI count & Sync All Events OFF removes mask
        win.act_sync_events.setChecked(False)
        self.assertFalse(win.world.sync_all_events)
        spec_ev = win.build_current_pie_spec(self.game_dir / "mods" / "TestSpec", "event")
        self.assertIn("event=Figure-8 Jump Race", spec_ev["direct_start"])
        self.assertIn("ai=6", spec_ev["direct_start"])
        self.assertNotIn("EOD_OPT_TERRAIN_MASK", spec_ev["env"])

        print(
            f"[VERIFIED] PIE Canonical + Event Template + ILIN Sync: "
            f"templates={len(items)}, fig8_ilin={win.world.road_graph.line_name}, direct='{spec_ev['direct_start']}'"
        )
        win.close()
        self.app.processEvents()

    def test_10_ramp_scaling_bake_ilin_recalc_and_car_injection(self) -> None:
        from formats import ilin as ilin_fmt
        import rst0

        world = wl.load_track_world(self.afton_world.trk_path, self.game_dir, include_cars=True)
        world.sync_all_events = True

        # 1. Scale an optional-terrain ramp (Item 2: VIF + opt_terrain promotion + BPPT + SCOL baking)
        ramp = next(
            (o for o in world.objects if o.category == "optional-terrain" and o.opt_terrain_mode is not None and o.retail_instance_id),
            None,
        )
        self.assertIsNotNone(ramp, "Expected an optional-terrain ramp in AFTON.TRK")
        assert ramp is not None
        ramp.scale = (1.0, 2.25, 1.0)
        ramp.written = False

        # 2. Edit an ILIN racing line node (Item 4: tangent/segLen recalculation)
        g = world.road_graph
        node_ids = sorted(g.nodes.keys())
        n_edit = g.nodes[node_ids[5]]
        n_edit.x += 4.5
        n_edit.z -= 3.0
        g.dirty = True

        # 3. Place a retail .CAR wreck onto the track (Item 5: live .TRK injection + CRC)
        cx, _cy, cz = world.world_center
        gy = world.collision.ground_height(cx, cz)
        cop_model = world.models["car:COPCAR.CAR"]
        placed_car = wl.WorldObject(
            obj_id="ed_test_copcar",
            name=cop_model.name,
            model_key="car:COPCAR.CAR",
            source="Placed by the editor",
            category="car-wreck",
            subtype="car",
            section_mlist="editor_layer",
            model_store=cop_model.model_store,
            hash_hex=cop_model.hash_hex,
            tri_count=cop_model.tri_count,
            textures=list(cop_model.textures),
            position=(cx, gy - float(cop_model.bbox_min[1]), cz),
            rotation_hpr=(30.0, 0.0, 0.0),
            scale=(1.0, 1.0, 1.0),
            written=False,
        )
        world.objects.append(placed_car)

        with tempfile.TemporaryDirectory() as tmp:
            mod_out = Path(tmp) / "mods" / "RampCarTest"
            report = ps.export_mod_layer(world, "RampCarTest", mod_out)

            # Verify ramp VIF + opt_terrain + BPPT/SCOL bake
            self.assertGreaterEqual(report["vif_models_baked"], 1)
            self.assertGreater(report["vif_vertices_baked"], 0)
            self.assertGreaterEqual(report["opt_terrain_promoted"], 1)
            self.assertGreater(report["bppt_planes_patched"] + report["scol_planes_patched"], 0)

            # Verify placed .CAR compiled into modded .TRK
            self.assertGreaterEqual(report["custom_objects_compiled"], 1)
            mod_trk = Path(report["mod_trk"])
            self.assertTrue(mod_trk.is_file())
            import world_collision as wc

            orig_entries = wc.read_directory(self.afton_world.trk_path.read_bytes())
            mod_trk_bytes = mod_trk.read_bytes()
            entries = wc.read_directory(mod_trk_bytes)
            self.assertGreater(len(entries), len(orig_entries), "Injected .CAR members missing from repacked .TRK")

            # Verify ILIN recalculation produced exact moved X/Z, unit tangent, and recalculated seg_len
            ild_entry = next(e for e in entries if e["name"] == g.line_name)
            ild_bytes = mod_trk_bytes[ild_entry["offset"] : ild_entry["offset"] + ild_entry["size"]]
            layout = ilin_fmt._layout(ild_bytes)
            o = layout["nodes_offset"] + n_edit.retail_index * ilin_fmt.NODE_SIZE
            rx, rz, dir_x, dir_z = struct.unpack_from("<4f", ild_bytes, o)
            seg_len = struct.unpack_from("<f", ild_bytes, o + 24)[0]
            self.assertAlmostEqual(rx, n_edit.x, places=2)
            self.assertAlmostEqual(rz, n_edit.z, places=2)
            norm = (dir_x ** 2 + dir_z ** 2) ** 0.5
            self.assertAlmostEqual(norm, 1.0, places=3)
            self.assertGreater(seg_len, 0.0)
            # Verify CRC-32 of repacked ILIN matches directory entry
            self.assertEqual(rst0.eod_crc(ild_bytes), ild_entry["crc"])

            # Verify injected custom car model/texture exists in repacked .TRK
            orig_names = {e["name"].lower() for e in orig_entries}
            added_names = [e["name"] for e in entries if e["name"].lower() not in orig_names]
            self.assertGreaterEqual(len(added_names), 1, f"Expected injected car members, got {added_names}")

            print(
                f"[VERIFIED] Ramp Bake + ILIN Recalc + Car Injection: "
                f"vif_verts={report['vif_vertices_baked']}, opt_promoted={report['opt_terrain_promoted']}, "
                f"bppt={report['bppt_planes_patched']}, scol={report['scol_planes_patched']}, "
                f"custom_compiled={report['custom_objects_compiled']}"
            )

        # Retail disc still 100% untouched
        self.assertEqual(_sha256_file(self.afton_world.trk_path), self.afton_hash_before)

    def test_11_multiselect_similar_lod_twins_vehicle_tuner_and_live_ini(self) -> None:
        win = EodWorldEditorWindow(game_dir=self.game_dir, auto_load=False)
        win._attach_loaded_world(wl.load_track_world(self.afton_world.trk_path, self.game_dir, include_cars=True))

        # 1. Select All Similar + multi-select badge + atomic MultiTransformObjectsCommand undo/redo
        seed = next(
            (o for o in win.world.objects if o.source == "Retail" and len(win.world.find_similar_objects(o)) >= 3),
            None,
        )
        self.assertIsNotNone(seed)
        assert seed is not None
        matches = win.select_similar_objects(seed)
        self.assertGreaterEqual(len(matches), 3)
        self.assertEqual(len(win.viewport.selected_objects), len(matches))
        self.assertFalse(win.properties.lbl_multi_sel.isHidden())

        orig_positions = [o.position for o in matches[:3]]
        entries = [
            (
                o,
                o.position,
                o.rotation_hpr,
                o.scale,
                (o.position[0] + 3.0, o.position[1], o.position[2] - 2.0),
                o.rotation_hpr,
                o.scale,
            )
            for o in matches[:3]
        ]
        win._on_viewport_multi_obj_transformed(entries, "Move selection")
        for o, op in zip(matches[:3], orig_positions):
            self.assertAlmostEqual(o.position[0], op[0] + 3.0, places=3)
        win.undo_stack.undo()
        for o, op in zip(matches[:3], orig_positions):
            self.assertEqual(o.position, op)

        # 2. Delete Selected with Include Overlapping LOD Twins & SOBL Barrier (single undo restores both!)
        static_objs = [o for o in win.world.objects if o.source == "Retail" and o.category == "static" and o.retail_instance_id]
        target_with_bar = next(
            (o for o in static_objs if win.world.find_overlapping_lod_twins_and_barriers([o])[1]),
            None,
        )
        self.assertIsNotNone(target_with_bar, "Expected a static object with overlapping SOBL barrier")
        assert target_with_bar is not None
        win.properties.chk_lod_twins.setChecked(True)
        self.assertTrue(win.world.include_lod_twins)
        win.viewport.select_object(target_with_bar)
        self.assertEqual(len(win.world.collision.sobl_edits), 0)
        win.delete_selected_item()
        self.assertTrue(target_with_bar.deleted)
        self.assertGreater(len(win.world.collision.sobl_edits), 0)
        win.undo_stack.undo()
        self.assertFalse(target_with_bar.deleted)
        self.assertEqual(len(win.world.collision.sobl_edits), 0)

        # 3. Delete All Similar + single undo
        del_cnt, _bar_cnt = win.delete_similar_objects(seed)
        self.assertGreaterEqual(del_cnt, len(matches))
        self.assertTrue(all(m.deleted for m in matches))
        win.undo_stack.undo()
        self.assertTrue(all(not m.deleted for m in matches))

        # 4. VehicleTunerDialog + PieHostWidget live control INI
        with tempfile.TemporaryDirectory() as tmp:
            ctrl_ini = Path(tmp) / "pie_live_control.ini"
            win.pie_host._control_file = ctrl_ini
            dlg = win.open_vehicle_tuner_dialog()
            dlg.sld_boost.setValue(165)
            self.assertTrue(ctrl_ini.is_file())
            txt = ctrl_ini.read_text(encoding="utf-8")
            self.assertIn("speed_boost=1.65", txt)

            win.pie_host.write_live_control(free_cam=1, ai_autodrive=1, arcade_mode="juggernaut")
            txt2 = ctrl_ini.read_text(encoding="utf-8")
            self.assertIn("free_cam=1", txt2)
            self.assertIn("ai_autodrive=1", txt2)
            self.assertIn("arcade_mode=juggernaut", txt2)
            dlg.close()

        print(
            f"[VERIFIED] Multi-Select + Similar ({len(matches)}) + LOD/SOBL Cleanup + VehicleTuner Live INI"
        )
        win.close()
        self.app.processEvents()

    def test_12_perf_profiler_gpu_queries_hud_and_cprofile_dump(self) -> None:
        from OpenGL import GL
        import perf_profiler as pp

        win = EodWorldEditorWindow(game_dir=self.game_dir, auto_load=False)
        win._attach_loaded_world(wl.load_track_world(self.afton_world.trk_path, self.game_dir, include_cars=False))
        win.resize(960, 600)
        win.show()
        self.app.processEvents()

        # Verify View -> Performance Profiler / HUD (F3) toggle + live GL_TIME_ELAPSED pass queries
        self.assertEqual(win.act_v_perf_hud.shortcut().toString(), "F3")
        self.assertTrue(win.viewport.perf_hud.isHidden())
        win.act_v_perf_hud.setChecked(True)
        self.assertFalse(win.viewport.perf_hud.isHidden())

        # Render 3 frames so ping-pong GL_TIME_ELAPSED queries harvest results cleanly with glError == 0
        for _ in range(3):
            win.viewport.makeCurrent()
            win.viewport.paintGL()
            gl_err = int(GL.glGetError())
            win.viewport.doneCurrent()
            self.assertEqual(gl_err, 0, f"Unexpected GL error with GL_TIME_ELAPSED queries: {gl_err}")
            self.app.processEvents()

        # Verify high-res perf_counter_ns phase timers
        prof = pp.get_profiler()
        with prof.phase("pie_launch", detail="free_roam"):
            time.sleep(0.002)
        snap = prof.snapshot()
        self.assertGreater(snap["phases"]["app_startup"]["count"], 0)
        self.assertGreater(snap["phases"]["track_load"]["count"], 0)
        self.assertGreater(snap["phases"]["pie_launch"]["last_ms"], 0.5)
        self.assertGreaterEqual(snap["total_gpu_ms"], 0.0)
        self.assertGreaterEqual(snap["terrain_ms"], 0.0)
        self.assertGreaterEqual(snap["props_ms"], 0.0)
        self.assertGreaterEqual(snap["water_ms"], 0.0)
        self.assertGreaterEqual(snap["overlays_ms"], 0.0)

        # Verify "Dump cProfile Trace" button & trace output
        with tempfile.TemporaryDirectory() as tmp:
            txt_path, summary = win.dump_cprofile_trace(out_dir=Path(tmp))
            self.assertTrue(txt_path.is_file())
            self.assertTrue(txt_path.with_suffix(".prof").is_file())
            self.assertIn("Top Functions", summary)
            self.assertIn("Saved cProfile trace", win.log_text.toPlainText())

        print(
            f"[VERIFIED] Perf Profiler & F3 HUD: glErr=0 | gpu_queries={snap['gpu_queries_active']} "
            f"| total_gpu={snap['total_gpu_ms']:.2f}ms (terr={snap['terrain_ms']:.2f}, props={snap['props_ms']:.2f}, "
            f"water={snap['water_ms']:.2f}, ov={snap['overlays_ms']:.2f}) | startup={snap['phases']['app_startup']['last_ms']:.1f}ms"
        )
        win.close()
        self.app.processEvents()

    def test_13_home_hub_recent_projects_thumbnail_and_autosave_recovery(self) -> None:
        import os
        import home_hub as hh

        old_appdata = os.environ.get("EOD_EDITOR_APPDATA")
        try:
            with tempfile.TemporaryDirectory() as tmp:
                tmp_p = Path(tmp)
                os.environ["EOD_EDITOR_APPDATA"] = str(tmp_p / "appdata")

                win = EodWorldEditorWindow(game_dir=self.game_dir, auto_load=False)
                win._attach_loaded_world(wl.load_track_world(self.afton_world.trk_path, self.game_dir, include_cars=False))
                win.project_name = "Afton Turbo Derby"
                proj_file = tmp_p / "projects" / "afton_turbo.eodworld.json"
                win.project_path = proj_file
                win.resize(900, 560)
                win.show()
                self.app.processEvents()

                # 1. Save project -> writes overlay + viewport PNG thumbnail beside .eodworld.json + recents.json
                win.save_project()
                self.assertTrue(proj_file.is_file())
                thumb_file = hh.thumbnail_path_for_project(proj_file)
                self.assertTrue(thumb_file.is_file(), f"Missing viewport thumbnail: {thumb_file}")
                recents = hh.load_recent_projects()
                self.assertEqual(len(recents), 1)
                self.assertEqual(recents[0]["project_name"], "Afton Turbo Derby")
                self.assertEqual(recents[0]["track_name"], "AFTON")
                self.assertEqual(Path(recents[0]["thumbnail_path"]).resolve(), thumb_file.resolve())

                # 2. Home Hub dialog: tracks, event template, recent cards, Skip preference, New World action
                hub = hh.HomeHubDialog(self.game_dir)
                self.assertGreaterEqual(hub.lst_tracks.count(), 15)
                self.assertEqual(len(hub.recent_cards), 1)
                hub.recents.append(dict(recents[0], project_name="Another saved project"))
                hub._populate_recents()
                hub.resize(800, 540)
                hub.show()
                self.app.processEvents()
                self.assertEqual(hub.size().toTuple(), (800, 540))
                self.assertEqual(hub.recent_cards[0].x(), hub.recent_cards[1].x())
                self.assertEqual(hub.recents_scroll.horizontalScrollBar().maximum(), 0)
                self.assertTrue(hub.rect().contains(hub.btn_create.mapTo(hub, hub.btn_create.rect().bottomRight())))
                hub.resize(1100, 700)
                self.app.processEvents()
                self.assertEqual(hub.recent_cards[0].y(), hub.recent_cards[1].y())
                self.assertLess(hub.recent_cards[0].x(), hub.recent_cards[1].x())
                hub.chk_skip_hub.setChecked(True)
                self.assertTrue(hh.load_editor_prefs()["skip_home_hub"])
                hub.chk_skip_hub.setChecked(False)
                self.assertFalse(hh.load_editor_prefs()["skip_home_hub"])

                hub.txt_project_name.setText("Dundas Night Jump")
                idx_ev = hub.cmb_event_template.findText("Demolition Derby")
                self.assertGreaterEqual(idx_ev, 0)
                hub.cmb_event_template.setCurrentIndex(idx_ev)
                hub._on_create_project()
                self.assertEqual(hub.result_action, "new_project")
                self.assertEqual(hub.selected_project_name, "Dundas Night Jump")
                self.assertEqual(hub.selected_event_template, "Demolition Derby")

                win.apply_home_hub_result(hub, sync_load=True)
                self.assertEqual(win.project_name, "Dundas Night Jump")
                self.assertEqual(win.world.active_event_template, "Demolition Derby")
                hub.close()

                # 3. Silent crash-safe atomic autosave + recovery banner
                win.project_path = proj_file
                win.project_name = "Afton Turbo Derby"
                win.world = wl.load_track_world(self.afton_world.trk_path, self.game_dir, include_cars=False)
                target_obj = next(o for o in win.world.objects if o.source == "Retail" and o.category == "static")
                orig_x = target_obj.position[0]
                target_obj.position = (orig_x + 12.5, target_obj.position[1], target_obj.position[2])
                win._autosave_dirty = True
                saved_auto = win.trigger_background_autosave(synchronous=True)
                self.assertIsNotNone(saved_auto)
                assert saved_auto is not None
                self.assertTrue(saved_auto.is_file())
                self.assertFalse(saved_auto.with_name(saved_auto.name + ".autosave.tmp").exists())
                # Ensure autosave mtime is strictly newer than manual save
                t_future = proj_file.stat().st_mtime + 5.0
                os.utime(saved_auto, (t_future, t_future))

                # Reload clean project -> recovery banner appears -> Recover restores +12.5m edit!
                win.world = wl.load_track_world(self.afton_world.trk_path, self.game_dir, include_cars=False)
                ps.apply_project_overlay(win.world, proj_file)
                found = win.check_autosave_recovery(proj_file)
                self.assertEqual(found, saved_auto)
                self.assertFalse(win.autosave_banner.isHidden())
                self.assertTrue(win.recover_pending_autosave())
                recovered_obj = win.world.object_by_id(target_obj.obj_id)
                self.assertIsNotNone(recovered_obj)
                assert recovered_obj is not None
                self.assertAlmostEqual(recovered_obj.position[0], orig_x + 12.5, places=3)
                self.assertTrue(win.autosave_banner.isHidden())

                # Manual save clears autosave cleanly
                win.save_project()
                self.assertFalse(saved_auto.exists())

                print(
                    f"[VERIFIED] Home Hub + Recent Projects + Viewport Thumb ({thumb_file.stat().st_size:,} B) + Crash-Safe Autosave Recovery"
                )
                win.close()
                self.app.processEvents()
        finally:
            if old_appdata is None:
                os.environ.pop("EOD_EDITOR_APPDATA", None)
            else:
                os.environ["EOD_EDITOR_APPDATA"] = old_appdata

    def test_14_binary_track_cache_and_sub10ms_delta_mod_baker(self) -> None:
        import os
        import rst0
        import world_collision as wc

        old_appdata = os.environ.get("EOD_EDITOR_APPDATA")
        try:
            with tempfile.TemporaryDirectory() as tmp:
                tmp_p = Path(tmp)
                os.environ["EOD_EDITOR_APPDATA"] = str(tmp_p / "cache_root")

                # 1. Binary Track Cache: cold parse vs warm cache hit
                afton_path = self.afton_world.trk_path
                w_cold = wl.load_track_world(afton_path, self.game_dir, include_cars=False, use_cache=True)
                self.assertFalse(w_cold.loaded_from_cache)
                self.assertTrue(wl.track_cache_exists(afton_path))

                w_warm = wl.load_track_world(afton_path, self.game_dir, include_cars=False, use_cache=True)
                self.assertTrue(w_warm.loaded_from_cache)
                self.assertTrue(str(w_warm.cache_path).endswith(".eodcache"))
                speedup = w_cold.load_ms / max(0.1, w_warm.load_ms)
                self.assertGreater(speedup, 10.0, f"Expected >10x cache speedup, got {speedup:.1f}x")
                self.assertEqual(len(w_cold.models), len(w_warm.models))
                self.assertEqual(len(w_cold.objects), len(w_warm.objects))
                self.assertEqual(w_cold.total_triangles, w_warm.total_triangles)
                self.assertEqual(len(w_cold.textures), len(w_warm.textures))
                self.assertEqual(w_cold.collision.scol_count, w_warm.collision.scol_count)
                self.assertEqual(w_cold.collision.sobl_boxes, w_warm.collision.sobl_boxes)

                # 2. Fast Delta-Only Mod Baker (<10 ms transform delta, bit-exact 0TSR CRCs)
                static_obj = next(
                    o for o in w_warm.objects if o.source == "Retail" and o.category == "static" and o.retail_instance_id
                )
                static_obj.position = (static_obj.position[0] + 2.0, static_obj.position[1], static_obj.position[2] - 1.0)
                n0 = next(iter(w_warm.road_graph.nodes.values()))
                n0.x += 1.5
                w_warm.road_graph.dirty = True
                tall = next(r for r in w_warm.collision.sobl_records if r.get("kind") == "box" and r.get("vertical"))
                w_warm.collision.apply_barrier_height_in_memory(tall["index"], 1.5)

                mod_out = tmp_p / "mods" / "DeltaSpeedTest"
                rep_full = ps.export_mod_layer(w_warm, "DeltaSpeedTest", mod_out)
                self.assertEqual(rep_full["bake_mode"], "full")

                # Delta #1: GRAF transform + SOBL + ILIN
                static_obj.position = (static_obj.position[0] + 1.0, static_obj.position[1], static_obj.position[2])
                n0.x += 0.5
                w_warm.road_graph.dirty = True
                tall2 = [r for r in w_warm.collision.sobl_records if r.get("kind") == "box" and r.get("vertical")][1]
                w_warm.collision.apply_barrier_height_in_memory(tall2["index"], 2.0)
                rep_delta_all = ps.export_mod_layer(w_warm, "DeltaSpeedTest", mod_out)
                self.assertEqual(rep_delta_all["bake_mode"], "delta")
                self.assertIn("track.grf", rep_delta_all["delta_patched_members"])
                self.assertIn("track.sol", rep_delta_all["delta_patched_members"])
                self.assertIn("track.ild", rep_delta_all["delta_patched_members"])

                # Delta #2..#4: transform-only delta bakes (take best of 3 to avoid transient AV scan spikes)
                best_delta_ms = 1e9
                rep_delta = rep_delta_all
                for _ in range(3):
                    static_obj.position = (static_obj.position[0] + 0.5, static_obj.position[1], static_obj.position[2])
                    rep_delta = ps.export_mod_layer(w_warm, "DeltaSpeedTest", mod_out)
                    self.assertEqual(rep_delta["bake_mode"], "delta")
                    best_delta_ms = min(best_delta_ms, float(rep_delta["compile_ms"]))

                self.assertLess(best_delta_ms, 10.0, f"Expected <10 ms delta bake, got {best_delta_ms:.2f} ms")
                self.assertTrue(rep_delta["mod_trk_differs_from_retail"])

                # Verify bit-exact 0TSR CRCs for all 3 delta-patched members!
                mod_raw = Path(rep_delta["mod_trk"]).read_bytes()
                mod_entries = wc.read_directory(mod_raw)
                for mname in ("track.grf", "track.sol", "track.ild"):
                    ent = next(x for x in mod_entries if x["name"].lower() == mname)
                    sub = mod_raw[ent["offset"] : ent["offset"] + ent["size"]]
                    self.assertEqual(rst0.eod_crc(sub), ent["crc"], f"Bit-exact CRC mismatch on {mname}")

                # Retail disc 100% untouched
                self.assertEqual(_sha256_file(afton_path), self.afton_hash_before)

                print(
                    f"[VERIFIED] Binary Track Cache: cold={w_cold.load_ms:.1f}ms -> warm={w_warm.load_ms:.1f}ms ({speedup:.1f}x) | "
                    f"Delta Mod Baker: full={rep_full['compile_ms']:.1f}ms -> delta_all3={rep_delta_all['compile_ms']:.2f}ms, "
                    f"delta_xform={best_delta_ms:.2f}ms (all CRCs bit-exact!)"
                )
        finally:
            if old_appdata is None:
                os.environ.pop("EOD_EDITOR_APPDATA", None)
            else:
                os.environ["EOD_EDITOR_APPDATA"] = old_appdata

    def test_15_scaled_event_prop_wall_baking_and_collision(self) -> None:
        import struct
        import modding_backend as mb
        import valt
        import world_export as we
        import world_graf as wg

        w = wl.load_track_world(self.afton_world.trk_path, self.game_dir, include_cars=False)
        wall = next(
            (
                o
                for o in w.objects
                if o.source == "Retail"
                and o.category == "event-prop"
                and o.retail_event_member
                and "obswall" in o.model_key.lower()
            ),
            None,
        )
        self.assertIsNotNone(wall, "Expected retail obswall event-prop in AFTON")
        assert wall is not None
        wm = w.models[wall.model_key]
        orig_dx = float(wm.bbox_max[0] - wm.bbox_min[0])
        orig_dy = float(wm.bbox_max[1] - wm.bbox_min[1])
        orig_dz = float(wm.bbox_max[2] - wm.bbox_min[2])

        sx, sy, sz = (2.0, 12.0, 1.5)
        wall.scale = (sx, sy, sz)
        wall.rotation_hpr = (35.0, 10.0, 5.0)
        wall.written = False

        with tempfile.TemporaryDirectory() as tmp:
            mod_out = Path(tmp) / "mods" / "GiantWallTest"
            rep = ps.export_mod_layer(w, "GiantWallTest", mod_out)
            self.assertEqual(rep["custom_objects_compiled"], 1)
            self.assertTrue(rep["mod_trk_differs_from_retail"])

            mod_trk = Path(rep["mod_trk"])
            ents = mb.directory(mod_trk)
            by_l = {e["name"].lower(): e for e in ents}
            self.assertIn("edp_001.mod", by_l, "Scaled event-prop .mod must be compiled into modded .TRK")

            # Verify baked .mod VIF bounding box is scaled ~12x in Y!
            mod_raw, _ = mb.payload(mod_trk, by_l["edp_001.mod"]["name"], ents, verify_crc=True)
            mhdr = wg.parse_model(mod_raw, 0)
            baked_dy = float(mhdr["bbox"]["max"][1] - mhdr["bbox"]["min"][1])
            self.assertAlmostEqual(baked_dy, orig_dy * sy, delta=0.5)

            # Verify obstacle.val contains solid type=wall with scaled dx, dy, dz!
            obst_raw, _ = mb.payload(mod_trk, by_l["obstacle.val"]["name"], ents, verify_crc=True)
            obst = valt.parse_binary_valt(obst_raw)
            edp_sec = next(
                (s for s in obst["sections"] if dict(s["pairs"]).get("name") == "edp_001"),
                None,
            )
            self.assertIsNotNone(edp_sec, "obstacle.val missing edp_001 wall entry")
            assert edp_sec is not None
            dp = dict(edp_sec["pairs"])
            self.assertEqual(dp.get("type"), "wall")
            self.assertEqual(dp.get("surf_type"), "wall")
            self.assertAlmostEqual(float(dp["dx"]), max(0.4, orig_dx * sx), delta=0.05)
            self.assertAlmostEqual(float(dp["dy"]), max(0.4, orig_dy * sy), delta=0.05)
            self.assertAlmostEqual(float(dp["dz"]), max(0.4, orig_dz * sz), delta=0.05)

            # Verify original retail wall in its va0 was hidden (y < -4000) & new edp_001 was injected
            va0_key = wall.retail_event_member.lower()
            va0_raw, _ = mb.payload(mod_trk, by_l[va0_key]["name"], ents, verify_crc=True)
            vmodel = valt.parse_binary_valt(va0_raw)
            has_edp_ref = any(dict(s["pairs"]).get("dyno") == "edp_001" for s in vmodel["sections"])
            self.assertTrue(has_edp_ref, f"{wall.retail_event_member} must spawn scaled edp_001 wall")
            hidden_count = 0
            for s in vmodel["sections"]:
                p = dict(s["pairs"])
                f64 = p.get("unfloat_frame64")
                if f64 and len(f64) == 64:
                    rows = we.decode_frame64(f64)
                    if rows[10] < -4000.0:
                        hidden_count += 1
            self.assertGreaterEqual(hidden_count, 1, "Original retail event-prop wall should be hidden below -4000 m")

            # Verify retail AFTON.TRK untouched
            self.assertEqual(_sha256_file(self.afton_world.trk_path), self.afton_hash_before)
            print(
                f"[VERIFIED] Scaled Event-Prop Wall Bake: baked_dy={baked_dy:.2f}m (orig={orig_dy:.2f}m x{sy:.0f}), "
                f"obstacle.val wall=({dp['dx']}, {dp['dy']}, {dp['dz']}), hidden_orig={hidden_count}"
            )

    def test_16_absurd_height_terrain_solid_collision_strow_and_status(self) -> None:
        import math
        import struct
        import modding_backend as mb
        import vif_patch
        import world_collision as wc
        import world_graf as wg

        w = wl.load_track_world(self.afton_world.trk_path, self.game_dir, include_cars=False)

        # 1. Verify Collision status classification in world_loader + PropertiesPanel
        hill_scol = next(o for o in w.objects if o.name == "hills_d54c8")
        hill_backdrop = next(o for o in w.objects if o.name == "s-hill_1a1068")
        bldg_sobl = next(o for o in w.objects if o.name == "billys_6f308")
        ev_wall = next(o for o in w.objects if o.category == "event-prop" and "obswall" in o.model_key.lower())

        self.assertEqual(w.object_collision_status(hill_scol), "Solid (SCOL)")
        self.assertEqual(w.object_collision_status(hill_backdrop), "Visual Backdrop -> Auto-Solidified on Edit")
        self.assertEqual(w.object_collision_status(bldg_sobl), "Solid (SOBL)")
        self.assertEqual(w.object_collision_status(ev_wall), "Solid (Event Wall)")

        win = EodWorldEditorWindow(game_dir=self.game_dir, auto_load=False)
        win._attach_loaded_world(w)
        win.viewport.select_object(hill_backdrop)
        self.assertEqual(win.properties.val_collision.text(), "Visual Backdrop -> Auto-Solidified on Edit")
        win.viewport.select_object(hill_scol)
        self.assertEqual(win.properties.val_collision.text(), "Solid (SCOL)")
        win.close()
        self.app.processEvents()

        # 2. STROW safety test: forbidden_row_offs must NEVER be mutated even on 50x scale!
        ents_ret = mb.directory(self.afton_world.trk_path)
        graf_e = next(e for e in ents_ret if e["type"] == "GRAF")
        graf_orig, _ = mb.payload(self.afton_world.trk_path, graf_e["name"], ents_ret, verify_crc=False)
        graf_parsed = wg.parse_graf(graf_orig)
        by_id = {f"i{i:04d}": lf for i, lf in enumerate(graf_parsed["leaves"])}
        hill_leaf = by_id[hill_scol.retail_instance_id]
        mhdr = wg.parse_model(graf_orig, hill_leaf["model_offset"])
        row_offs = {
            int(b["pos"]["row_off"])
            for part in mhdr["parts"]
            for b in vif_patch.scan_part(graf_orig, part)
            if b.get("count", 0) > 0 and "pos" in b and b["pos"]["row_off"] >= 0
        }
        self.assertGreater(len(row_offs), 0)
        buf_guard = bytearray(graf_orig)
        ps._encode_vif_parts_vertices(
            buf_guard,
            graf_orig,
            [(part, lambda p: (p[0], p[1] * 50.0, p[2])) for part in mhdr["parts"]],
            forbidden_row_offs=row_offs,
        )
        for ro in row_offs:
            self.assertEqual(buf_guard[ro : ro + 12], graf_orig[ro : ro + 12], "Forbidden STROW was mutated!")

        # 3. Absurd-height terrain bake: SCOL lifting + NODE_SPHERE cull growth + SOBL scaling + 0-SCOL SOBL synthesis
        hill_scol.scale = (1.0, 15.0, 1.0)
        hill_backdrop.scale = (1.0, 12.0, 1.0)
        bldg_sobl.scale = (1.0, 5.0, 1.0)

        retail_sobl_parsed = wc.parse_sobl(mb.payload(self.afton_world.trk_path, "track.sol", ents_ret, verify_crc=False)[0])
        retail_sobl_count = int(retail_sobl_parsed["count"])

        with tempfile.TemporaryDirectory() as tmp:
            mod_out = Path(tmp) / "mods" / "AbsurdHeightTest"
            rep = ps.export_mod_layer(w, "AbsurdHeightTest", mod_out)
            self.assertGreaterEqual(rep["vif_models_baked"], 3)
            self.assertGreater(rep["scol_planes_patched"], 50, "hills_d54c8 should lift >50 SCOL triangles")
            self.assertGreater(rep["sobl_scaled_barriers"], 0, "billys_6f308 should scale overlapping SOBL barrier heights")
            self.assertGreater(rep["sobl_boxes_synthesized"], 0, "Visual-only s-hill_1a1068 should auto-synthesize SOBL boxes")

            mod_trk = Path(rep["mod_trk"])
            mod_ents = mb.directory(mod_trk)
            mod_sol, _ = mb.payload(mod_trk, "track.sol", mod_ents, verify_crc=True)
            mod_sobl = wc.parse_sobl(mod_sol)
            self.assertEqual(mod_sobl["count"], retail_sobl_count + rep["sobl_boxes_synthesized"])
            # Every synthesized box index must be reachable from the SOBL quadtree index list
            idx_set = set(mod_sobl["index"])
            synth_recs = []
            for new_idx in range(retail_sobl_count, mod_sobl["count"]):
                self.assertIn(new_idx, idx_set)
                rec = mod_sobl["records"][new_idx]
                self.assertEqual(rec["kind"], "box")
                self.assertGreaterEqual(rec["half_extents"][1], 0.35)
                synth_recs.append(rec)
            self.assertGreater(max(r["half_extents"][1] for r in synth_recs), 5.0)

            # Verify ancestor NODE_SPHERE cull radii cover the giant scaled hills_d54c8!
            mod_graf, _ = mb.payload(mod_trk, graf_e["name"], mod_ents, verify_crc=True)
            mod_mhdr = wg.parse_model(mod_graf, hill_leaf["model_offset"])
            nc = tuple(mod_mhdr["centre"])
            nrad = float(mod_mhdr["radius"])
            for step in hill_leaf["path"]:
                if step["kind"] != "sphere":
                    continue
                cx, cy, cz, sr = struct.unpack_from("<4f", mod_graf, step["node"] + 0x0C)
                need = math.dist(nc, (cx, cy, cz)) + nrad
                self.assertGreaterEqual(sr + 1e-3, need, "Ancestor NODE_SPHERE too small for giant hill!")

            self.assertEqual(_sha256_file(self.afton_world.trk_path), self.afton_hash_before)
            print(
                f"[VERIFIED] Absurd-Height Terrain + STROW Safety: scol={rep['scol_planes_patched']}, "
                f"sobl_scaled={rep['sobl_scaled_barriers']}, sobl_synth={rep['sobl_boxes_synthesized']} "
                f"(track.sol boxes {retail_sobl_count}->{mod_sobl['count']})"
            )

    def test_18_bulk_texture_pack_actions_round_trip(self) -> None:
        win = EodWorldEditorWindow(game_dir=self.game_dir, auto_load=False)
        world = wl.load_track_world(self.afton_world.trk_path, self.game_dir, include_cars=False)
        win._attach_loaded_world(world)
        mesh_menu = next(action.menu() for action in win.menuBar().actions() if action.text() == "&Mesh")
        actions = {action.text(): action for action in mesh_menu.actions()}
        try:
            self.assertIn("Export All Track Textures to Folder...", actions)
            self.assertIn("Import Texture Folder (HOSTTEX)...", actions)
            texture = next(asset for asset in world.textures.values() if asset.ensure_identity()[0])
            before = texture.rgba
            with tempfile.TemporaryDirectory() as folder:
                export_dir = Path(folder) / "export"
                import_dir = Path(folder) / "import"
                export_dir.mkdir()
                import_dir.mkdir()
                with patch.object(QFileDialog, "getExistingDirectory", return_value=str(export_dir)):
                    actions["Export All Track Textures to Folder..."].trigger()
                manifest = json.loads((export_dir / "manifest.json").read_text())
                record = next(item for item in manifest["textures"] if item["name"] == texture.name)
                self.assertTrue((export_dir / record["file"]).is_file())
                color = (12, 34, 56, 255)
                Image.new("RGBA", (16, 16), color).save(import_dir / record["file"])
                with patch.object(QFileDialog, "getExistingDirectory", return_value=str(import_dir)):
                    actions["Import Texture Folder (HOSTTEX)..."].trigger()
                self.assertEqual(texture.rgba, bytes(color) * 256)
                self.assertTrue(texture.is_replaced)
                mod_dir = Path(folder) / "mod"
                report = ps.export_mod_layer(world, "Texture Pack Test", mod_dir)
                self.assertEqual(report["host_textures_exported"], 1)
                self.assertIn(texture.index_key, (mod_dir / "HOSTTEX" / "replacements.tsv").read_text())
                win.undo_stack.undo()
                self.assertEqual(texture.rgba, before)
                self.assertFalse(texture.is_replaced)
        finally:
            win.close()

    def test_19_smart_barrier_actions_preserve_manual_edits_and_bake_noclip(self) -> None:
        win = EodWorldEditorWindow(game_dir=self.game_dir, auto_load=False)
        world = wl.load_track_world(self.afton_world.trk_path, self.game_dir, include_cars=False)
        win._attach_loaded_world(world)
        col_menu = next(action.menu() for action in win.menuBar().actions() if action.text() == "C&ollision")
        actions = {action.text(): action for action in col_menu.actions()}
        try:
            self.assertIn("Clamp Sky-Walls to Asset Height", actions)
            self.assertIn("Walls Noclip (Keep Ground Solid)", actions)
            before = dict(world.collision.sobl_edits)
            actions["Clamp Sky-Walls to Asset Height"].trigger()
            self.assertGreater(len(world.collision.sobl_edits), len(before))
            self.assertFalse(world.all_walls_noclip)
            clamped = dict(world.collision.sobl_edits)
            noclip = actions["Walls Noclip (Keep Ground Solid)"]
            with tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                control = ps.export_mod_layer(world, "Barrier Test", root / "clamped")
                control_ground, _ = wl.payload(Path(control["mod_trk"]), "track.bsc")
                noclip.trigger()
                self.assertTrue(world.all_walls_noclip)
                self.assertEqual(world.collision.sobl_edits, clamped)
                report = ps.export_mod_layer(world, "Barrier Test", root / "noclip")
                self.assertTrue(report["all_walls_noclip"])
                out_track = Path(report["mod_trk"])
                sol, _ = wl.payload(out_track, "track.sol")
                baked = ps.wc.parse_sobl(sol)["records"]
                index = next(iter(clamped))
                self.assertLess(baked[index]["centre"][1], -1000.0)
                mod_ground, _ = wl.payload(out_track, "track.bsc")
                self.assertEqual(mod_ground, control_ground)
            noclip.trigger()
            self.assertFalse(world.all_walls_noclip)
            self.assertEqual(world.collision.sobl_edits, clamped)
            win.undo_stack.undo()
            self.assertTrue(noclip.isChecked())
            win.undo_stack.undo()
            self.assertFalse(noclip.isChecked())
            win.undo_stack.undo()
            self.assertEqual(world.collision.sobl_edits, before)
        finally:
            win.close()

    def test_20_shutdown_cancels_running_thumb_workers(self) -> None:
        import workers

        self.assertTrue(hasattr(workers, "shutdown_editor_workers"))
        started = threading.Event()
        finished = threading.Event()
        token = CancelToken()

        def render_thumb(_item, _textures):
            started.set()
            deadline = time.monotonic() + 2.0
            while not token.is_cancelled and time.monotonic() < deadline:
                time.sleep(0.001)
            finished.set()

        worker = workers.ThumbnailBatchWorker([("thumb", None)], {}, render_thumb, token)
        workers.start_worker(worker)
        self.assertTrue(started.wait(2.0))
        workers.shutdown_editor_workers()
        self.assertTrue(token.is_cancelled)
        self.assertTrue(finished.is_set())
        self.assertNotIn(worker, workers._LIVE_WORKERS)

    def test_21_inspector_environment_and_marker_edits_can_be_undone(self) -> None:
        win = EodWorldEditorWindow(game_dir=self.game_dir, auto_load=False)
        world = wl.load_track_world(self.afton_world.trk_path, self.game_dir, include_cars=False)
        win._attach_loaded_world(world)
        try:
            original_environment = world.environment.to_dict()
            win.open_environment_panel()
            win.properties.env_preset_combo.setCurrentText("Golden Sunset")
            self.assertEqual(world.environment.preset_name, "Golden Sunset")
            win.properties.env_ambient.setValue(0.45)
            self.assertAlmostEqual(world.environment.ambient_intensity, 0.45)
            win.undo_stack.undo()
            self.assertEqual(world.environment.preset_name, "Golden Sunset")
            self.assertNotAlmostEqual(win.properties.env_ambient.value(), 0.45)
            win.undo_stack.undo()
            self.assertEqual(world.environment.to_dict(), original_environment)

            marker = world.markers[0]
            win.viewport.select_marker(marker)
            original_name = marker.name
            win.properties.mk_name_ed.setText("Renamed marker")
            win.properties.mk_name_ed.editingFinished.emit()
            self.assertEqual(marker.name, "Renamed marker")
            win.undo_stack.undo()
            self.assertEqual(marker.name, original_name)
            self.assertEqual(win.properties.mk_name_ed.text(), original_name)
        finally:
            win.close()

    def test_22_file_action_packages_unsaved_mod_to_verified_eodmod(self) -> None:
        import mod_package

        win = EodWorldEditorWindow(game_dir=self.game_dir, auto_load=False)
        world = wl.load_track_world(self.afton_world.trk_path, self.game_dir, include_cars=False)
        win._attach_loaded_world(world)
        win.project_name = "Track Package Test"
        file_menu = next(action.menu() for action in win.menuBar().actions() if action.text() == "&File")
        actions = {action.text(): action for action in file_menu.actions()}
        try:
            self.assertIn("Package Mod as .eodmod...", actions)
            action = actions["Package Mod as .eodmod..."]
            self.assertEqual(action.shortcut().toString(), "Ctrl+Shift+E")
            barrier = next(rec for rec in world.collision.sobl_records if rec.get("vertical"))
            win.apply_barrier_height(barrier["index"], -0.5)
            graph = world.road_graph
            next(iter(graph.nodes.values())).x += 2.0
            graph.dirty = False  # Earlier mod bakes clear this flag.
            project_written_state = [obj.written for obj in world.objects]
            texture_name = next(name for name, texture in world.textures.items() if texture.ensure_identity()[0])
            with tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                png = root / "replacement.png"
                Image.new("RGBA", (16, 16), (12, 34, 56, 255)).save(png)
                win.replace_texture_from_png(texture_name, png)
                output = root / "test.eodmod"
                with patch.object(QDialog, "exec", return_value=QDialog.DialogCode.Accepted), \
                        patch.object(QFileDialog, "getSaveFileName", return_value=(str(output), "")), \
                        patch.object(QMessageBox, "critical") as error:
                    action.trigger()
                    self.assertIsNotNone(win._active_token)
                    deadline = time.monotonic() + 30.0
                    while win._active_token is not None and time.monotonic() < deadline:
                        self.app.processEvents()
                        time.sleep(0.005)
                    self.assertIsNone(win._active_token)
                    error.assert_not_called()
                verified = mod_package.verify(output, self.game_dir)
                self.assertEqual(verified["id"], "track-package-test")
                self.assertEqual(verified["version"], "1.0.0")
                self.assertEqual(verified["host"], 1)
                self.assertIn("native/DATA/TRACKS/AFTON.TRK/track.sol", verified["files"])
                self.assertIn(f"native/DATA/TRACKS/AFTON.TRK/{graph.line_name}", verified["files"])
                self.assertNotIn("env_override.ini", verified["files"])
                self.assertIsNone(win.project_path)
                self.assertEqual([obj.written for obj in world.objects], project_written_state)
                self.assertEqual(_sha256_file(world.trk_path), self.afton_hash_before)
        finally:
            win.close()

    def test_23_mod_manager_launches_private_profile_pie_overlay(self) -> None:
        import os

        import mod_install
        import mod_manager
        import mod_package

        win = EodWorldEditorWindow(game_dir=self.game_dir, auto_load=False)
        world = wl.load_track_world(self.afton_world.trk_path, self.game_dir, include_cars=False)
        win._attach_loaded_world(world)
        real_dialog = mod_manager.ModManagerDialog
        barrier = next(rec for rec in world.collision.sobl_records if rec.get("vertical"))
        win.apply_barrier_height(barrier["index"], -0.5)
        file_menu = next(action.menu() for action in win.menuBar().actions() if action.text() == "&File")
        action = next(action for action in file_menu.actions() if action.text() == "Mod Manager && Profiles...")
        try:
            with tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                profiles = root / "profiles"
                base_sol, _ = wl.payload(world.trk_path, "track.sol")
                # Profile intentionally edits the same archive in a conflicting and a nonconflicting member.
                edited_sol = bytearray(base_sol)
                edited_sol[100] ^= 1
                line_name = "oval.ild"
                base_line, _ = wl.payload(world.trk_path, line_name)
                line = ps.ilin_fmt.encode(base_line, {"node.0.x": 123.0})
                baked = root / "baked" / world.trk_rel
                baked.parent.mkdir(parents=True)
                baked.write_bytes(mod_package.build_archive(
                    world.trk_path, {"track.sol": bytes(edited_sol), line_name: line}
                )[0])
                package = root / "package"
                mod_package.from_overlay(root / "baked", self.game_dir, "pie-profile", "1.0.0", package)
                indexed = [
                    (name, asset) for name, asset in world.textures.items()
                    if asset.asset_id.startswith(world.trk_rel + "#") and all(asset.ensure_identity())
                ]
                edited_name, edited_texture = indexed[0]
                _, retained_texture = next(
                    (name, asset) for name, asset in indexed[1:] if asset.index_key != edited_texture.index_key
                )
                for idx, asset in enumerate((edited_texture, retained_texture)):
                    image = root / f"profile-{idx}.png"
                    Image.new("RGBA", (asset.orig_width, asset.orig_height), (200, 30, 40, 255)).save(image)
                    mod_package.add_host(package, self.game_dir, asset.asset_id, image)
                project_image = root / "authored.png"
                Image.new("RGBA", (edited_texture.orig_width, edited_texture.orig_height), (30, 40, 200, 255)).save(project_image)
                win.replace_texture_from_png(edited_name, project_image)
                mod_install.install(package, self.game_dir, profile="selected", root=profiles)
                original_active = Path(mod_install.launch_env(profile="selected", root=profiles)["EOD_MOD_DIR"])
                original_bytes = (original_active / world.trk_rel).read_bytes()
                profile_rows = {
                    row[5]: row for row in (
                        line.split("\t") for line in (original_active / "HOSTTEX/replacements.tsv").read_text().splitlines()[1:]
                    )
                }

                def choose_dialog(game_dir, parent=None, **kwargs):
                    dialog = real_dialog(game_dir, parent, root=profiles, **kwargs)

                    def choose():
                        deadline = time.monotonic() + 5
                        while dialog._token is not None and time.monotonic() < deadline:
                            self.app.processEvents()
                            time.sleep(0.005)
                        dialog.profile_picker.setCurrentText("selected")
                        while dialog._token is not None and time.monotonic() < deadline:
                            self.app.processEvents()
                            time.sleep(0.005)
                        self.assertTrue(dialog.play_free_roam_button.isEnabled())
                        dialog.play_free_roam_button.click()
                        return dialog.result()

                    dialog.exec = choose
                    return dialog

                with patch.object(mod_manager, "ModManagerDialog", side_effect=choose_dialog), \
                        patch.object(win.pie_host, "start_pie", return_value=True) as spawn, \
                        patch.object(QMessageBox, "critical") as error, \
                        patch.dict(os.environ, {"EOD_MOD_DIR": str(root / "unrelated-overlay")}):
                    action.trigger()
                    deadline = time.monotonic() + 30
                    while win._active_token is not None and time.monotonic() < deadline:
                        self.app.processEvents()
                        time.sleep(0.005)
                    error.assert_not_called()
                    spawn.assert_called_once()
                    self.assertEqual(os.environ["EOD_MOD_DIR"], str(root / "unrelated-overlay"))
                    spec = spawn.call_args.args[0]
                    overlay = Path(spec["env"]["EOD_MOD_DIR"])
                    self.assertNotEqual(overlay, original_active)
                    merged = overlay / world.trk_rel
                    self.assertEqual(wl.payload(merged, line_name)[0], line)
                    merged_sol = wl.payload(merged, "track.sol")[0]
                    self.assertNotEqual(merged_sol, base_sol)
                    self.assertNotEqual(merged_sol, bytes(edited_sol))
                    self.assertEqual((original_active / world.trk_rel).read_bytes(), original_bytes)
                    self.assertTrue((overlay / "env_override.ini").is_file())
                    merged_rows = {
                        row[5]: row for row in (
                            line.split("\t") for line in (overlay / "HOSTTEX/replacements.tsv").read_text().splitlines()[1:]
                        )
                    }
                    self.assertEqual(len(merged_rows), 2)
                    retained = merged_rows[retained_texture.asset_id]
                    self.assertEqual(retained[1], retained_texture.palette_key)
                    self.assertEqual(
                        (overlay / "HOSTTEX" / retained[4]).read_bytes(),
                        (original_active / "HOSTTEX" / profile_rows[retained_texture.asset_id][4]).read_bytes(),
                    )
                    self.assertNotEqual(
                        (overlay / "HOSTTEX" / merged_rows[edited_texture.asset_id][4]).read_bytes(),
                        (original_active / "HOSTTEX" / profile_rows[edited_texture.asset_id][4]).read_bytes(),
                    )
                    win.stop_pie()
                    self.assertFalse(overlay.exists())
                self.assertEqual(_sha256_file(world.trk_path), self.afton_hash_before)
        finally:
            win.close()

    def test_17_stupid_angle_3d_ilin_ramps_and_pie_dpi_rect(self) -> None:
        import math
        import struct
        import modding_backend as mb
        from formats import ilin as ilin_fmt
        import pie_host as ph

        # 1. ILIN 3D elevation preservation in recalculate_geometry (steep + vertical climbs!)
        ents = mb.directory(self.afton_world.trk_path)
        ild_e = next(e for e in ents if e["name"].lower() == "track.ild")
        clean_ild, _ = mb.payload(self.afton_world.trk_path, ild_e["name"], ents, verify_crc=False)
        layout = ilin_fmt._layout(clean_ild)
        base = layout["nodes_offset"]
        x0, z0 = struct.unpack_from("<2f", clean_ild, base)
        # Put node 1 directly above node 0 (vertical 90 deg climb!) and node 2 at a steep 60 deg climb
        edits = {"node.1.x": x0, "node.1.z": z0, "node.2.x": x0 + 10.0, "node.2.z": z0}
        heights = {0: 0.0, 1: 40.0, 2: 100.0}
        recalc_3d = ilin_fmt.encode(clean_ild, edits, recalculate=True, node_heights=heights)
        seg0 = struct.unpack_from("<f", recalc_3d, base + 24)[0]
        seg1 = struct.unpack_from("<f", recalc_3d, base + ilin_fmt.NODE_SIZE + 24)[0]
        dx0, dz0 = struct.unpack_from("<2f", recalc_3d, base + 8)
        self.assertAlmostEqual(seg0, 40.0, places=2, msg="Vertical 40m climb should preserve 3D segment length!")
        self.assertAlmostEqual(seg1, math.hypot(10.0, 60.0), places=2, msg="Steep climb should preserve 3D segment length!")
        self.assertAlmostEqual(math.hypot(dx0, dz0), 1.0, places=3, msg="Vertical climb must keep valid unit horizontal tangent!")

        # 2. PropertiesPanel + ViewportGL 3D gizmo for selected ILIN Road Node
        w = wl.load_track_world(self.afton_world.trk_path, self.game_dir, include_cars=False)
        win = EodWorldEditorWindow(game_dir=self.game_dir, auto_load=False)
        win._attach_loaded_world(w)
        rn = next(iter(w.road_graph.nodes.values()))
        win.viewport.select_road_node(rn.node_id)
        origin, gkind = win.viewport._active_gizmo_origin()
        self.assertEqual(gkind, "road_node")
        self.assertIsNotNone(origin)
        # Arbitrary 3D elevation edit (+85.5 m) must NOT be flattened
        win._on_prop_road_pos(rn, (rn.x, 85.5, rn.z))
        self.assertAlmostEqual(rn.y, 85.5, places=3)
        self.assertIn("P ", win.properties.val_road_dir.text())
        win.close()
        self.app.processEvents()

        # 3. 3D tilted optional-terrain ramp (full H/P/R + scale -> tilted BPPT 3D collision planes!)
        ramp = next(
            (o for o in w.objects if o.category == "optional-terrain" and o.opt_terrain_mode is not None),
            None,
        )
        self.assertIsNotNone(ramp)
        assert ramp is not None
        ramp.rotation_hpr = (25.0, 42.0, 18.0)
        ramp.scale = (1.5, 2.5, 1.5)
        with tempfile.TemporaryDirectory() as tmp:
            mod_out = Path(tmp) / "mods" / "TiltedRampTest"
            rep = ps.export_mod_layer(w, "TiltedRampTest", mod_out)
            self.assertGreater(rep["bppt_planes_patched"], 0)
            mod_ents = mb.directory(Path(rep["mod_trk"]))
            bpp_mod, _ = mb.payload(Path(rep["mod_trk"]), "track.bpp", mod_ents, verify_crc=True)
            slot = wl.parse_bppt(bpp_mod)[ramp.opt_terrain_mode]
            tilted = 0
            for pi in range(slot["n_planes"]):
                a, b, c, _d = struct.unpack_from("<4f", bpp_mod, slot["planes_off"] + pi * 20)
                if b >= 0.01 and (abs(a) > 0.25 or abs(c) > 0.25):
                    tilted += 1
            self.assertGreater(tilted, 0, "Expected steeply tilted 3D planes in track.bpp!")

        # 4. Embedded PIE Win32 style stripping + DPI-scaled client rect math (1.0x..2.0x)
        raw_style = ph.WS_OVERLAPPEDWINDOW | ph.WS_POPUP | ph.WS_VISIBLE
        raw_exstyle = ph.WS_EX_WINDOWEDGE | ph.WS_EX_CLIENTEDGE | ph.WS_EX_DLGMODALFRAME | ph.WS_EX_APPWINDOW
        ns, nxs = ph.strip_embedded_window_styles(raw_style, raw_exstyle)
        self.assertTrue(bool(ns & ph.WS_CHILD))
        self.assertFalse(bool(ns & (ph.WS_CAPTION | ph.WS_THICKFRAME | ph.WS_BORDER | ph.WS_POPUP)))
        self.assertEqual(nxs, 0)

        for dpr in (1.0, 1.25, 1.5, 2.0):
            r = ph.compute_embedded_client_rect(960, 540, dpr)
            self.assertEqual(r["client_w"], int(round(960 * dpr)))
            self.assertEqual(r["client_h"], int(round(540 * dpr)))
            self.assertEqual((r["x"], r["y"], r["w"], r["h"]), (0, 0, r["client_w"], r["client_h"]))

        # Stale oversized Win32 GetClientRect during shrink clamps to exact Qt physical width (no HUD edge clip!)
        r_shrink = ph.compute_embedded_client_rect(900, 540, 2.0, win32_client_size=(1840, 1080))
        self.assertEqual(r_shrink["client_w"], 1800)

        # Residual non-client frame insets offset x/y negatively so client area starts at exact (0, 0)
        r_nc = ph.compute_embedded_client_rect(960, 540, 1.5, nc_insets=(8, 31, 8, 8))
        self.assertEqual((r_nc["x"], r_nc["y"]), (-8, -31))
        self.assertEqual((r_nc["w"], r_nc["h"]), (1440 + 16, 810 + 39))

        # Cross-DPI WM_SIZE scaling (Qt PMv2 192 DPI embedding DPI-unaware 96 DPI child)
        r_dpi = ph.compute_embedded_client_rect(960, 540, 2.0, parent_dpi=192, child_dpi=96)
        self.assertEqual((r_dpi["client_w"], r_dpi["client_h"]), (1920, 1080))
        self.assertEqual((r_dpi["wm_size_w"], r_dpi["wm_size_h"]), (960, 540))

        print(
            f"[VERIFIED] Stupid-Angle 3D ILIN (seg0={seg0:.1f}m, seg1={seg1:.1f}m) + "
            f"Tilted BPPT ({tilted} 3D planes) + PIE DPI Rect Math (1.0x..2.0x)"
        )


if __name__ == "__main__":
    print(f"Running EOD World Editor tests (PySide6 {PYSIDE6_VERSION}, Python {sys.version.split()[0]})...")
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(TestEodWorldEditor)
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)
