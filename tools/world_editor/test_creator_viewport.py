"""Native Qt input and pixel regression. EOD_TEST_CREATOR_VIEWPORT=1 needs a visible desktop.
EOD_TEST_CREATOR_BENCHMARK=1 also measures native Plume render/present calls on that desktop.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

import numpy as np
from PySide6.QtCore import QEvent, QPoint, QPointF, Qt
from PySide6.QtGui import QFocusEvent, QImage, QMouseEvent, QWheelEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication

sys.path.insert(0, str(Path(__file__).resolve().parent))
from app import configure_bundled_runtime, configure_opengl_format
from main_window import EodWorldEditorWindow
from workers import editor_thread_pool
from world_loader import MeshPart, WorldModel, WorldObject, default_game_dir, load_track_world


@unittest.skipUnless(os.getenv("EOD_TEST_CREATOR_VIEWPORT") == "1", "requires an interactive desktop")
class TestCreatorViewport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        configure_bundled_runtime()
        configure_opengl_format()
        cls.app = QApplication.instance() or QApplication(["creator-viewport"])
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        env = patch.dict(os.environ, {"EOD_EDITOR_APPDATA": cls.tmp.name, "EOD_EDITOR_NO_PROMPTS": "1"})
        env.start()
        cls.addClassCleanup(env.stop)

    def setUp(self):
        disc = default_game_dir()
        path = disc / "DATA/TRACKS/AFTON.TRK"
        if not path.is_file():
            self.skipTest("Afton retail data required")
        self.win = EodWorldEditorWindow(game_dir=disc, auto_load=False, show_home_hub=False)
        self.vp = self.win.viewport
        self.world = load_track_world(path, disc, include_cars=True)
        self.win._attach_loaded_world(self.world)
        self.win.resize(1150, 740)
        self.win.show()
        self.app.processEvents()

    def tearDown(self):
        self.win.close()
        self.assertTrue(editor_thread_pool().waitForDone(15000))
        self.app.processEvents()

    def frame(self, renderer, held=False):
        if held:
            # Deliver deterministic camera frames while a synthetic button is held.
            # A physical focus change is a separate explicit test below.
            if renderer == "plume":
                for _ in range(3):
                    self.vp._paint_plume_impl()
                    time.sleep(.025)
        else:
            for _ in range(6):
                self.vp.update()
                self.app.processEvents()
                QTest.qWait(28)
        if renderer == "plume":
            self.assertGreater(self.vp._plume_viewport.presented, 0)
            pos = self.vp.mapTo(self.win, QPoint(0, 0))
            image = self.win.screen().grabWindow(self.win.winId(), pos.x(), pos.y(),
                                                  self.vp.width(), self.vp.height()).toImage()
        else:
            image = self.vp.grabFramebuffer()
        self.assertFalse(image.isNull())
        image = image.convertToFormat(QImage.Format.Format_RGB888)
        return np.frombuffer(image.bits(), np.uint8).reshape(image.height(), image.bytesPerLine())[:, :image.width()*3].copy()

    def test_move_cancel_deselect_undo_pixels(self):
        for renderer in ("ps2", "plume"):
            with self.subTest(renderer=renderer):
                self._move_cancel_deselect(renderer)

    def test_toolbar_placement_previews_before_stamping(self):
        vp, win = self.vp, self.win
        p = next(m for m in self.world.markers if m.kind == "Start Grid").position
        vp.cam_pos = np.asarray(p, np.float32) + [-12, 20, -22]
        vp.look_at(np.asarray(p))
        action = next(a for a in vp.actions() if a.text().endswith("Start Gate"))
        action.trigger()
        start = QPoint(vp.width()//2, vp.height()//2)
        count = len(self.world.markers)
        QTest.mouseClick(vp, Qt.MouseButton.LeftButton, pos=start)
        self.assertEqual(len(self.world.markers), count)  # base dirt below a gap
        win.activate_build_piece("deck")
        center = np.asarray(p, np.float32) + [0., 30., 0.]
        win._on_ghost_stamped(vp.ghost_model_key, tuple(center), (0., 0., 0.), (1., 1., 1.))
        deck = vp.selected_obj
        lo, hi = deck.world_aabb(self.world.models[deck.model_key])
        center = (lo + hi) * .5
        vp.cam_pos = (center + [-12., 20., -22.]).astype(np.float32)
        vp.look_at(center)
        action.trigger()
        QTest.mouseMove(vp, start + QPoint(8, 5))
        QTest.mouseMove(vp, start)
        self.assertTrue(vp._marker_preview())
        self.assertEqual(len(self.world.markers), count)
        expected = vp.ghost_pos.copy()
        QTest.mouseClick(vp, Qt.MouseButton.LeftButton, pos=start)
        self.assertEqual(len(self.world.markers), count + 1)
        point = vp.selected_marker
        self.assertEqual(point.kind, "Practice start")
        np.testing.assert_allclose(point.position, expected, atol=.01)
        QTest.keyClick(vp, Qt.Key.Key_Escape)
        self.assertEqual(vp.active_tool, "move")
        self.assertIs(self.world.markers[-1], point)
        win.undo_stack.undo()
        self.assertNotIn(point, self.world.markers)

    def test_banked_upper_deck_ghost_normal_and_project_roundtrip(self):
        import project_state as ps
        vp, win = self.vp, self.win
        verts = np.zeros((4, 8), np.float32)
        verts[:, :3] = [(-8, 0, -8), (-8, 0, 8), (8, 0, 8), (8, 0, -8)]
        verts[:, 4] = 1
        mesh = MeshPart("", verts, np.asarray([0, 1, 2, 0, 2, 3], np.uint32), 2)
        key = "road:test_deck"
        self.world.models[key] = WorldModel(key, "Deck", "Roads", "procedural:road", "", [mesh], 2, [],
                                            verts[:, :3].min(0), verts[:, :3].max(0), (0., 0., 0.))
        decks = []
        for index, height in enumerate((110., 130.)):
            deck = WorldObject(f"deck_{index}", "Deck", key, "Placed by the editor", "Roads",
                               "Procedural Road", "editor_layer", "", "", 2, [], (2000., height, 2000.),
                               rotation_hpr=(35., 10., 21.), scale=(1.4, 1.0, 1.7))
            self.world.objects.append(deck)
            decks.append(deck)
        vp.mark_scene_dirty()
        target = np.asarray(decks[1].position)
        vp.cam_pos = (target + [0., 24., -4.]).astype(np.float32)
        vp.look_at(target)
        origin, direction = vp.screen_ray(vp.width()/2, vp.height()/2)
        point, normal = vp.placement_surface(origin, direction)
        np.testing.assert_allclose(point, target, atol=.002)
        np.testing.assert_allclose(normal, decks[1].world_matrix()[:3, 1], atol=1e-5)
        # The same click with the upper deck hidden lands on the lower mesh.
        decks[1].visible = False
        vp.mark_objects_dirty([decks[1]])
        self.assertLess(vp.placement_surface(origin, direction)[0][1], 115.)
        # A stock triangle visible through a gap cannot become a creator start.
        decks[0].source = "Retail"
        decks[0].retail_initial_pos = decks[0].position
        decks[0].retail_initial_rot = decks[0].rotation_hpr
        decks[0].retail_initial_scale = decks[0].scale
        self.assertIsNone(vp.placement_surface(origin, direction, custom_only=True))
        decks[1].visible = True
        vp.mark_objects_dirty([decks[1]])
        self.assertIsNone(vp.marker_surface(origin, -direction))
        sky = vp.screen_ray(0, 0)
        self.assertIsNone(vp.placement_surface(*sky))
        self.assertIsNotNone(vp.placement_surface(*sky, free_space=True))
        win.place_start_with_mouse("Practice start")
        center = QPoint(vp.width()//2, vp.height()//2)
        QTest.mouseMove(vp, center + QPoint(4, 3))
        QTest.mouseMove(vp, center)
        QTest.mouseClick(vp, Qt.MouseButton.LeftButton, pos=center)
        marker = vp.selected_marker
        np.testing.assert_allclose(marker.normal, normal, atol=1e-5)
        geom = vp._marker_geometry(marker.position, marker.heading_deg, marker.normal, marker.kind)
        # The forward arrow's three vertices stay on a plane parallel to the bank.
        heights = (geom[-3:, :3] - marker.position) @ normal
        np.testing.assert_allclose(heights, .33, atol=.001)
        with tempfile.TemporaryDirectory() as tmp:
            path = ps.save_project_overlay(Path(tmp) / "bank.eodworld.json", "Bank", self.world)
            doc = json.loads(path.read_text())
            preset = next(m for m in doc["markers"] if m["marker_id"] != marker.marker_id)
            preset.pop("normal")
            path.write_text(json.dumps(doc))
            ps.apply_project_overlay(self.world, path)
            markers = {m.marker_id: m for m in self.world.markers}
            self.assertEqual(markers[preset["marker_id"]].normal, (0., 1., 0.))
            np.testing.assert_allclose(markers[marker.marker_id].normal, normal, atol=1e-5)

    def test_alt_body_drag_and_wheel_on_empty_sky(self):
        vp, win = self.vp, self.win
        vp.show_roads = vp.show_markers = False
        vp.grid_snap_enabled = False
        ramp = win._place_model_at("mod:w_jump4b.mod", 1500., 130., 1500.)
        vp.frame_selection()
        win.select_tool("move")
        self.frame("ps2")
        click = None
        for y in range(vp.height()//4, 3*vp.height()//4, 8):
            for x in range(vp.width()//4, 3*vp.width()//4, 8):
                ray = vp.screen_ray(x, y)
                if vp._pick_gizmo_axis(*ray) < 0 and vp._pick_object(*ray) is ramp:
                    click = QPoint(x, y)
                    break
            if click is not None:
                break
        self.assertIsNotNone(click)
        baseline = len(self.world.objects)
        index = win.undo_stack.index()
        original = ramp.position
        left, alt = Qt.MouseButton.LeftButton, Qt.KeyboardModifier.AltModifier
        QTest.mousePress(vp, left, alt, click)
        end = click + QPoint(28, -14)
        QTest.mouseMove(vp, end)
        self.assertIsNotNone(vp._body_drag)
        clone = vp.selected_obj
        self.assertIsNot(clone, ramp)
        self.assertEqual(len(self.world.objects), baseline + 1)
        before = clone.position
        self.app.sendEvent(vp, QWheelEvent(QPointF(end), QPointF(vp.mapToGlobal(end)), QPoint(), QPoint(0, 120),
                                            left, alt, Qt.ScrollPhase.NoScrollPhase, False))
        np.testing.assert_allclose(clone.position, np.asarray(before) + (0., 1., 0.), atol=.002)
        QTest.mouseRelease(vp, left, alt, end)
        self.assertIs(vp.selected_obj, clone)
        self.assertEqual(ramp.position, original)
        self.assertEqual(win.undo_stack.index(), index + 1)
        win.undo_stack.undo()
        self.assertTrue(clone.deleted)
        win.undo_stack.redo()
        self.assertFalse(clone.deleted)

    def test_diagonal_marker_drag_from_diamond_and_axis_junction(self):
        vp, win = self.vp, self.win
        vp.show_markers = vp.show_roads = True
        vp.grid_snap_enabled = False
        point = next(m for m in self.world.markers if m.kind == "Start Grid")
        heading = point.heading_deg
        for renderer in ("ps2", "plume"):
            vp.set_render_mode(renderer)
            for selected in (False, True):
                with self.subTest(renderer=renderer, selected=selected):
                    win.select_tool("move")
                    vp.select_marker(point if selected else None)
                    # Camera centred on the visible diamond for a first press, and
                    # the exact junction of the three arrows for a selected marker.
                    target = np.asarray(point.position) + [0, 0 if selected else 1.6, 0]
                    vp.cam_pos = (target + [-12, 14, -24]).astype(np.float32)
                    vp.look_at(target)
                    self.frame(renderer)
                    start = QPoint(vp.width()//2, vp.height()//2)
                    end = start + QPoint(36, -19)
                    old = point.position
                    index = win.undo_stack.index()
                    QTest.mousePress(vp, Qt.MouseButton.LeftButton, pos=start)
                    self.assertIs(vp.selected_marker, point)
                    self.assertEqual(vp._gizmo_drag_axis, -1)
                    QTest.mouseMove(vp, end, 10)
                    QTest.mouseRelease(vp, Qt.MouseButton.LeftButton, pos=end)
                    moved = point.position
                    self.assertGreater(abs(moved[0]-old[0]), .03)
                    self.assertGreater(abs(moved[2]-old[2]), .03)
                    self.assertEqual(point.heading_deg, heading)
                    self.assertEqual(win.undo_stack.index(), index+1)
                    vp.select_marker(None)
                    win.undo_stack.undo()
                    self.assertEqual(point.position, old)
                    win.undo_stack.redo()
                    self.assertEqual(point.position, moved)
                    import project_state as ps
                    saved = ps.build_project_overlay_dict("Drag check", self.world)
                    restored = next(m for m in saved["markers"] if m["marker_id"] == point.marker_id)
                    self.assertEqual(tuple(restored["position"]), moved)
                    win.undo_stack.undo()

    def _move_cancel_deselect(self, renderer):
        vp, win = self.vp, self.win
        vp.set_render_mode(renderer)
        vp.show_markers = vp.show_roads = False
        target = np.array([-250., 35., 150.], np.float32)
        vp.cam_pos = (target + [-10, 8, -23]).astype(np.float32)
        vp.look_at(target)
        # Select and render an actual oriented retail barrier, then begin a different tool.
        vp.show_collision = True
        wall = next(r for r in self.world.collision.sobl_records if r.get("kind") == "box")
        vp.select_barrier(wall["index"])
        self.frame(renderer)
        ray = np.asarray(wall["rows"][0], np.float32)
        self.assertGreaterEqual(vp._pick_sobl_barrier(np.asarray(wall["centre"]) + ray * 10, -ray), 0)
        vp.show_collision = False
        layer = win.explorer.layer_checks["event-prop"]
        layer.setChecked(False)
        ramp = win._place_model_at("mod:w_jump4b.mod", *target)
        self.assertTrue(layer.isChecked())
        self.assertEqual(vp.selected_barrier_idx, -1)
        vp.frame_selection()
        win.select_tool("move")
        self.frame(renderer)
        start = None
        for y in range(vp.height()//4, 3*vp.height()//4, 8):
            for x in range(vp.width()//4, 3*vp.width()//4, 8):
                origin, direction = vp.screen_ray(x, y)
                if vp._pick_gizmo_axis(origin, direction) < 0 and vp._pick_object_with_t(origin, direction)[0] is ramp:
                    start = QPoint(x, y)
                    break
            if start is not None:
                break
        self.assertIsNotNone(start)
        end = start + QPoint(35, 8)
        initial = ramp.position
        count = win.undo_stack.count()
        left = Qt.MouseButton.LeftButton
        QTest.mousePress(vp, left, pos=start)
        QTest.mouseMove(vp, end, 10)
        self.assertNotEqual(ramp.position, initial)
        self.app.sendEvent(vp, QFocusEvent(QEvent.Type.FocusOut))
        QTest.mouseRelease(vp, left, pos=end)
        self.assertEqual(ramp.position, initial)
        self.assertEqual(win.undo_stack.count(), count)
        vp.select_object(ramp)
        QTest.mousePress(vp, left, pos=start)
        at_press = self.frame(renderer, held=True)
        def move_held(point):
            self.app.sendEvent(vp, QMouseEvent(QEvent.Type.MouseMove, QPointF(point), QPointF(vp.mapToGlobal(point)),
                                                Qt.MouseButton.NoButton, left, Qt.KeyboardModifier.NoModifier))
        move_held(end)
        first_pos = ramp.position
        during = self.frame(renderer, held=True)
        self.assertTrue(vp._lmb_down)
        self.assertEqual(win.undo_stack.count(), count)
        self.assertGreater(np.count_nonzero(during != at_press), 2000)
        move_held(end + QPoint(18, -5))
        self.assertNotEqual(ramp.position, first_pos, "Second pointer move should advance the transform before release")
        during_second_move = self.frame(renderer, held=True)
        self.assertGreater(np.count_nonzero(during_second_move != during), 2000)
        QTest.mouseRelease(vp, left, pos=end + QPoint(18, -5))
        self.assertNotEqual(ramp.position, initial)
        self.assertEqual(win.undo_stack.count(), count + 1)
        vp.select_object(None)
        present = self.frame(renderer)
        win.undo_stack.undo()
        win.undo_stack.undo()
        absent = self.frame(renderer)
        win.undo_stack.redo()
        win.undo_stack.redo()
        vp.select_object(None)
        restored = self.frame(renderer)
        changed = np.count_nonzero(present != absent)
        self.assertGreater(changed, 500)
        self.assertLess(np.count_nonzero(restored != present), changed / 10)
        QTest.mousePress(vp, Qt.MouseButton.RightButton, pos=end)
        self.app.sendEvent(vp, QFocusEvent(QEvent.Type.FocusOut))
        self.assertNotEqual(vp.cursor().shape(), Qt.CursorShape.BlankCursor)
        self.assertFalse(vp._rmb_down or vp._lmb_down)
        win.undo_stack.undo()
        win.undo_stack.undo()

    @unittest.skipUnless(os.getenv("EOD_TEST_CREATOR_BENCHMARK") == "1", "opt-in frame timing")
    def test_frame_times(self):
        vp = self.vp
        vp.set_render_mode("plume")
        for width, height in ((1280, 720), (1920, 1080), (3840, 2160)):
            dpr = vp.devicePixelRatioF()
            vp.setFixedSize(round(width/dpr), round(height/dpr))
            self.frame("plume")
            for collision in (False, True):
                vp.show_collision = collision
                samples = []
                for i in range(75):
                    vp.cam_yaw += .18
                    before = vp._plume_viewport.presented
                    begin = time.perf_counter()
                    vp._paint_plume_impl()
                    elapsed = (time.perf_counter() - begin) * 1000
                    if i >= 15 and vp._plume_viewport.presented > before:
                        samples.append(elapsed)
                    self.app.processEvents()
                self.assertGreater(len(samples), 45)
                self.assertEqual((round(vp._plume_viewport.width()*dpr), round(vp._plume_viewport.height()*dpr)), (width, height))
                print(f"{width}x{height} collision={collision} n={len(samples)} p50/p95/max ms "
                      f"{np.percentile(samples, [50, 95, 100]).round(2)}", flush=True)


if __name__ == "__main__":
    unittest.main()
