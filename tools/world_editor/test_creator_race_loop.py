"""Exercise the creator loop on a retail canvas. Set EOD_TEST_CREATOR_ONLINE=1
to run two game processes against independently installed copies of the package.
Use EOD_GAME_DIR for the retail data and EOD_PIE_EXE for a specific build.
"""
from __future__ import annotations

import hashlib
import json
import time
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from PySide6.QtWidgets import QApplication, QMessageBox

sys.path.insert(0, str(Path(__file__).resolve().parent))
import world_loader as wl
import project_state as ps
import mod_install
from app import configure_opengl_format
from main_window import EodWorldEditorWindow
from pie_host import resolve_eod_exe
from workers import CancelToken, editor_thread_pool


class TestCreatorRaceLoop(unittest.TestCase):
    def test_directed_lap_and_disconnected_or_overlapping_waypoints(self):
        g = wl.RoadGraph("test.ild")
        for x, z in [(0, 0), (8, 0), (0, 8), (20, 0), (24, 6), (30, 0)]:
            g.add_node(x, 0, z)
        for a, b in [(1, 2), (2, 3), (3, 1), (4, 5), (5, 6), (6, 4)]:
            g.add_edge(a, b)
        self.assertEqual(g.lap_route_issue()[0], 4)
        for node_id in [4, 5, 6]:
            g.remove_node(node_id)
        g.add_node(-8, 0, 8, is_side=True)
        self.assertIsNone(g.lap_route_issue())
        g.nodes[2].x = 0
        self.assertEqual(g.lap_route_issue()[0], 1)
        g.nodes[2].x = 8
        g.edges.pop()
        self.assertEqual(g.lap_route_issue()[0], 3)

    def test_place_reopen_package_install_and_race(self):
        disc = wl.default_game_dir()
        trk = disc / "DATA/TRACKS/ANOKA.TRK"
        if not trk.is_file():
            self.skipTest("Anoka retail track required")
        configure_opengl_format()
        app = QApplication.instance() or QApplication(["creator-race-test"])
        with tempfile.TemporaryDirectory(prefix="eod-creator-") as tmp:
            root = Path(tmp)
            with patch.dict(os.environ, {"EOD_EDITOR_APPDATA": str(root / "editor")}):
                win = EodWorldEditorWindow(game_dir=disc, auto_load=False, show_home_hub=False)
                try:
                    self._build_and_run(win, trk, disc, root)
                finally:
                    win.undo_stack.setClean()
                    win.close()
                    self.assertTrue(editor_thread_pool().waitForDone(15000))
                    app.processEvents()

    def test_template_blank_canvas_and_save_recovery(self):
        disc = wl.default_game_dir()
        trk = disc / "DATA/TRACKS/AFTON.TRK"
        if not trk.is_file():
            self.skipTest("Afton retail track required")
        configure_opengl_format()
        app = QApplication.instance() or QApplication(["creator-template-test"])
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {"EOD_EDITOR_APPDATA": tmp, "EOD_EDITOR_NO_PROMPTS": "1"}):
            win = EodWorldEditorWindow(game_dir=disc, auto_load=False, show_home_hub=False)
            try:
                world = wl.load_track_world(trk, disc, include_cars=False)
                win._attach_loaded_world(world)
                a = win._place_model_at("mod:w_jump4b.mod", 10, 10, 10)
                win._mark_saved()
                win.undo_stack.undo()
                win.add_marker_at_camera("Practice start")
                self.assertTrue(win._is_dirty(), "A new undo branch at the same index must remain unsaved")
                win.trigger_background_autosave(force=True, synchronous=True)
                self.assertTrue(win._is_dirty())
                self.assertIsNone(world.object_by_id(a.obj_id))
                template = Path(tmp) / "starter.eodworld.json"
                world.sync_all_events = False
                world.active_event_template = "Jump Race"
                doc = ps.build_project_overlay_dict("Starter", world)
                missing = {"obj_id": "ed_missing", "model_key": "mod:future.mod", "name": "Future ramp",
                           "position": [3, 4, 5], "scale": [1, 1, 1], "rotation_hpr": [10, 0, 0]}
                doc["added_objects"].append(missing)
                ps.atomic_write_json(template, doc)
                unchanged = template.read_bytes()
                with patch("project_state.os.replace", side_effect=OSError("disk full")):
                    with self.assertRaises(OSError):
                        ps.atomic_write_json(template, {"partial": True})
                self.assertEqual(template.read_bytes(), unchanged)
                win.open_project_file(template, as_template=True, background=True)
                def await_loaded(condition):
                    end = time.monotonic() + 45
                    while not condition() and time.monotonic() < end:
                        app.processEvents()
                        time.sleep(.01)
                    self.assertTrue(condition())
                await_loaded(lambda: win.world is not world)
                self.assertIsNone(win.project_path)
                self.assertEqual(win.world.active_event_template, "Jump Race")
                self.assertFalse(win.world.sync_all_events)
                orphan = win.world.object_by_id("ed_missing")
                self.assertEqual(orphan.position, (3, 4, 5))
                saved = ps.save_project_overlay(Path(tmp)/"remix.eodworld.json", "Remix", win.world)
                self.assertEqual(json.loads(saved.read_text())["added_objects"][-1]["model_key"], "mod:future.mod")
                before = win.world
                damaged = Path(tmp) / "broken.eodworld.json"
                damaged.write_text("{")
                with patch.object(QMessageBox, "critical") as error:
                    win.open_project_file(damaged)
                    error.assert_called_once()
                self.assertIs(win.world, before)
                self.assertEqual(template.read_bytes(), unchanged)
                win.new_blank_canvas()
                await_loaded(lambda: win.world.track_name.upper() == "EMPTYT_1")
                self.assertIsNone(win.project_path)
                starts = [win.add_marker_at_camera("Practice start") for _ in range(20)]
                snap = ps.build_project_overlay_dict("Blank sprint", win.world)
                self.assertTrue(all(starts))
                self.assertEqual(len({s.marker_id for s in starts}), 20)
                self.assertEqual(sum(m["kind"] == "Practice start" for m in snap["markers"]), 20)
            finally:
                win.close()
                self.assertTrue(editor_thread_pool().waitForDone(15000))
                app.processEvents()

    def _build_and_run(self, win, trk, disc, root):
        retail_hash = hashlib.sha256(trk.read_bytes()).digest()
        world = wl.load_track_world(trk, disc, include_cars=False)
        win._attach_loaded_world(world)
        event = world.event_templates["No-Rules Race"]
        win._switch_road_line(event.ilin_member)
        graph = world.road_graph
        self.assertIsNone(graph.lap_route_issue())
        win.check_ai_lap_route()
        self.assertIn("closed lap", win.lbl_status.toolTip())
        node = next(n for n in graph.nodes.values() if n.retail_index == 12)
        target = (node.x + 1.25, node.y, node.z)
        win._on_prop_road_pos(node, target)

        spawns = [m for m in world.markers if m.battle_spawn]
        spawn = win.add_marker_at_camera("Spawn")
        self.assertIsNotNone(spawn)
        x, y, z = spawns[0].position
        spawn.position = (x + 8, y, z)
        win.viewport.select_marker(spawn)
        win.swap_spawn_role()
        self.assertEqual(spawn.kind, "Practice start")
        with patch.object(win, "start_pie") as start:
            win.practice_selected_point()
            start.assert_called_once_with("free_roam", spawn_pos=(x + 8, y + 1.2, z, spawn.heading_deg))
        win.undo_stack.undo()
        self.assertTrue(spawn.battle_spawn)
        win.viewport.select_road_node(node.node_id)
        with patch.object(win, "start_pie") as start:
            win.practice_selected_point()
            self.assertEqual(start.call_args.kwargs["spawn_pos"][:3], (target[0], target[1] + 1.2, target[2]))
        key = "mod:w_jump4b.mod"
        ramp = win._place_model_at(key, x + 14, y, z)
        self.assertIsNotNone(ramp)
        win.viewport.select_object(None)
        win.undo_stack.undo()
        win.undo_stack.redo()
        # An optional slalom beside the normal AI line keeps a clear through lane.
        main_nodes = [n for n in graph.nodes.values() if not n.is_side]
        cx = sum(n.x for n in main_nodes) / len(main_nodes)
        cz = sum(n.z for n in main_nodes) / len(main_nodes)
        for ordinal, index in enumerate((18, 24, 30, 36, 42)):
            gate = next(n for n in graph.nodes.values() if n.retail_index == index)
            dx, _, dz = graph.node_direction(gate.node_id)
            length = math.hypot(dx, dz)
            right_x, right_z = dz/length, -dx/length
            inward = 1 if (cx-gate.x)*right_x + (cz-gate.z)*right_z >= 0 else -1
            side = 1 if ordinal % 2 else -1
            for model, offset in (("mod:w_haybl.mod", 5.5), ("mod:w_cone.mod", 3.8)):
                px, pz = gate.x + inward * (23 + side * offset) * right_x, gate.z + inward * (23 + side * offset) * right_z
                obj = win._place_model_at(model, px, world.collision.ground_height(px, pz), pz,
                                          rot=(math.degrees(math.atan2(dx, dz)), 0, 0))
                self.assertIsNotNone(obj)
        approach = next(n for n in graph.nodes.values() if n.retail_index == 14)
        dx, _, dz = graph.node_direction(approach.node_id)
        practice = win.add_marker_at_camera("Practice start")
        practice.name = "Hay bale slalom approach"
        inward = 1 if (cx-approach.x)*dz - (cz-approach.z)*dx >= 0 else -1
        px, pz = approach.x + 23*inward*dz, approach.z - 23*inward*dx
        practice.position = (px, world.collision.ground_height(px, pz), pz)
        practice.heading_deg = math.degrees(math.atan2(dx, dz))
        world.active_event_template = "No-Rules Race"
        world.custom_race.event_name = world.custom_race.event_type = "No-Rules Race"
        world.custom_race.player_car = "Bus"
        world.sync_all_events = False
        project = ps.save_project_overlay(root / "course.eodworld.json", "Anoka Hay Bale Slalom", world)

        reopened = wl.load_track_world(trk, disc, include_cars=False)
        ps.apply_project_overlay(reopened, project)
        self.assertEqual(reopened.object_by_id(ramp.obj_id).position, ramp.position)
        self.assertEqual(reopened.road_graph.nodes[node.node_id].x, target[0])
        package = root / "course.eodmod"
        package_id = "club.anoka-hay-bale-slalom"
        result = ps.package_world_mod(ps.snapshot_world_for_bake(reopened), "Anoka Hay Bale Slalom",
                                      package_id, "1.0.0", "Anoka Hay Bale Slalom", package,
                                      token=CancelToken(), progress=lambda *args: None)
        self.assertFalse(result["warnings"])
        profiles = []
        for role in ("host", "guest"):
            mod_install.install(package, disc, root=root / role)
            profiles.append(mod_install.profile_dir(root / role, "default") / "active")
        compiled = profiles[0] / world.trk_rel
        fingerprint = hashlib.sha256(compiled.read_bytes()).hexdigest()
        self.assertEqual(fingerprint, hashlib.sha256((profiles[1] / world.trk_rel).read_bytes()).hexdigest())
        self.assertEqual(hashlib.sha256(trk.read_bytes()).digest(), retail_hash)
        raw = wl.payload(compiled, graph.line_name)[0]
        layout = wl.ilin_fmt._layout(raw)
        position = struct.unpack_from("<2f", raw, layout["nodes_offset"] + node.retail_index * wl.ilin_fmt.NODE_SIZE)
        self.assertLess(math.dist(position, (target[0], target[2])), .01)
        self.assertTrue(wl.ilin_fmt.main_loop(raw)[1])
        for member in ("c_battle.va0", "c_normal_race.va0"):
            sections = wl.parse_binary_valt(wl.payload(compiled, member)[0])["sections"]
            placements = [v for section in sections for k, v in section["pairs"]
                          if k == "dyno" and str(v).startswith("edp_")]
            self.assertEqual(len(placements), 11 if member == "c_normal_race.va0" else 0)
            if member == "c_battle.va0":
                slots = next(s for s in sections if s["name"] == "respawn")
                self.assertEqual(len(slots["pairs"]), len(spawns) + 1)

        if os.environ.get("EOD_TEST_CREATOR_ONLINE") == "1":
            exe = resolve_eod_exe(disc)
            self.assertTrue(exe.is_file(), f"Missing game executable {exe}")
            for scenario in ("native-lap", "battle-respawn"):
                logs = root / scenario
                args = [sys.executable, str(wl.TOOLS_DIR / "test_online_completion.py"),
                        str(exe), str(disc), str(logs), scenario, "--guest-mod-dir", str(profiles[1])]
                if scenario == "battle-respawn":
                    args += ["--human-battle"]
                result = subprocess.run(args, env={**os.environ, "EOD_MOD_DIR": str(profiles[0])},
                                        capture_output=True, text=True, timeout=420)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                for role in ("host", "guest"):
                    log = (logs / role / "game.log").read_text(errors="replace")
                    self.assertIn(f"profile: package {package_id} 1.0.0", log)
                    self.assertIn(fingerprint, log)
                print(result.stdout.strip(), flush=True)


if __name__ == "__main__":
    unittest.main()
