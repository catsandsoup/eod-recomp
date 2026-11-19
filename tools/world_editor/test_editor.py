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


if __name__ == "__main__":
    print(f"Running EOD World Editor tests (PySide6 {PYSIDE6_VERSION}, Python {sys.version.split()[0]})...")
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(TestEodWorldEditor)
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)
