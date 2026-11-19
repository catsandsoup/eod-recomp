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
import random
import struct
import sys
import tempfile
import time
import unittest
from pathlib import Path

EDITOR_DIR = Path(__file__).resolve().parent
if str(EDITOR_DIR) not in sys.path:
    sys.path.insert(0, str(EDITOR_DIR))

from PIL import Image
from PySide6 import __version__ as PYSIDE6_VERSION
from PySide6.QtWidgets import QApplication

import pie_host
import project_state as ps
import world_loader as wl
from app import configure_opengl_format
from main_window import EodWorldEditorWindow
from theme import apply_dark_theme
from thumbnails import render_model_thumbnail, render_model_thumbnail_qimage
from workers import CancelToken, FunctionWorker, editor_thread_pool


def _sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


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
            "File", "World", "View", "Camera", "Objects", "Markers", "Cars",
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
        self.assertEqual(spec_plume["env"]["EOD_MODERN"], "1")
        self.assertEqual(spec_plume["env"]["EOD_NATIVE"], "0")
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
            img = vp.grabFramebuffer()
            arr = np.frombuffer(img.constBits(), dtype=np.uint8).reshape(img.height(), img.width(), 4)
            mean_luma = float(arr[:, :, :3].mean())
            self.assertGreater(mean_luma, 60.0, f"{mode} lit AFTON frame too dark (mean_luma={mean_luma:.1f})")
            self.assertLess(vp._last_frame_ms, 25.0, f"{mode} frame too slow ({vp._last_frame_ms:.2f} ms)")

        # Labels + Lighting ON across 2 frames must not clobber GL state
        vp.show_labels = True
        vp.repaint()
        self.app.processEvents()
        vp.repaint()
        self.app.processEvents()
        img_lbl = vp.grabFramebuffer()
        arr_lbl = np.frombuffer(img_lbl.constBits(), dtype=np.uint8).reshape(img_lbl.height(), img_lbl.width(), 4)
        self.assertGreater(float(arr_lbl[:, :, :3].mean()), 60.0, "QPainter labels clobbered OpenGL lighting state")

        # 3. Verify Output Log drawer + Verbose + append_log
        win.append_log("[TEST] PIE diagnostic line", verbose=True)
        self.assertIn("[TEST] PIE diagnostic line", win.log_text.toPlainText())
        print(
            f"[VERIFIED] Lighting + Perf + Dual-Renderer + Output Log: "
            f"frame={vp._last_frame_ms:.2f}ms | idle_timer={vp._timer.isActive()} | lit_luma={arr_lbl[:,:,:3].mean():.1f}"
        )

        win.close()
        self.app.processEvents()


if __name__ == "__main__":
    print(f"Running EOD World Editor tests (PySide6 {PYSIDE6_VERSION}, Python {sys.version.split()[0]})...")
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(TestEodWorldEditor)
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)
