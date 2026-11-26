#!/usr/bin/env python3
"""Sprint 3E-2 — Red-Team Gate 1: Adversarial Multi-Step E2E UI + Headless `eod` Chaos Suite.

Actively attempts to break EOD World Studio & Game Runtime across 5 red-team journeys:
  1. Automated UX / Potemkin / Layout / OS Auditor (duplicate menus, OS leak strings, resize safety).
  2. Journey 1: Exact Audit Repro -> Alt+Drag clone -> compound ramp scale -> export -> live headless `eod`.
  3. Journey 2: Procedural 3D Roads/Island + Undo/Redo Storm + Blender OBJ Round-Trip + Overlay + .eodcol.
  4. Journey 3: Car Studio 4-tab + Damage/Dyno + Paint->HOSTTEX EODHTX1 + 125-Car NASCAR ISO + .eodmod v2.
  5. Journey 4: Hotkey/Modifier Fuzzing + Vectorized Pick Equivalence + Tracy/Perfetto Latency Budget Gate.
"""

from __future__ import annotations

import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
os.environ.setdefault("EOD_EDITOR_NO_PROMPTS", "1")

EDITOR_DIR = Path(__file__).resolve().parent
REPO_ROOT = EDITOR_DIR.parent.parent
TOOLS_DIR = REPO_ROOT / "tools"
for _p in (str(EDITOR_DIR), str(TOOLS_DIR)):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from PySide6.QtCore import QEvent, Qt
from PySide6.QtGui import QKeyEvent
from PySide6.QtWidgets import (
    QAbstractButton,
    QApplication,
    QComboBox,
    QLabel,
    QMenu,
    QToolBar,
    QWidget,
)

import mod_package_v2 as mp2
import project_state as ps
import road_collision as rc
import world_loader as wl
from app import configure_opengl_format
from car_studio_showroom import (
    export_car_texture_png,
    render_showroom_frame,
    replace_car_texture_png,
    upscale_car_texture_hosttex,
)
from dialogs import VehicleTunerDialog
from eod_blender_addon import eod_bridge
from main_window import EodWorldEditorWindow
from nascar_scanner import EphemeralTestDriveSession, scan_nascar_source
from perf_profiler import get_process_rss_mb, get_profiler
from workers import editor_thread_pool

DISC_DIR = Path("/Users/monty/Library/Application Support/TDEODRecomp/disc")
AFTON_TRK = DISC_DIR / "DATA" / "TRACKS" / "AFTON.TRK"
BUILD_DIR = REPO_ROOT / "build-mac-plume"
EOD_BIN = BUILD_DIR / "eod"
REAL_NASCAR_ISO = Path("/Users/monty/Downloads/NASCAR - Dirt to Daytona (USA).iso")


def _qapp() -> QApplication:
    configure_opengl_format()
    return QApplication.instance() or QApplication(["test-redteam-e2e"])


def _norm_label(text: str) -> str:
    return text.replace("&&", "\x00").replace("&", "").replace("\x00", "&").strip()


class TestRedTeamE2E(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = _qapp()
        cls.afton_available = AFTON_TRK.is_file()

    def _fresh_afton_world(self) -> wl.TrackWorld:
        if not AFTON_TRK.is_file():
            self.skipTest(f"Retail AFTON.TRK not found: {AFTON_TRK}")
        return wl.load_track_world(AFTON_TRK, DISC_DIR)

    def _cleanup_window(self, win: EodWorldEditorWindow) -> None:
        try:
            win.explorer._cancel_thumb_worker()
        except Exception:
            pass
        editor_thread_pool().waitForDone(2000)
        win.close()
        self.app.processEvents()

    # ------------------------------------------------------------------
    # Test 1: Automated UX / Potemkin / Layout / OS Auditor
    # ------------------------------------------------------------------
    def test_01_ux_potemkin_layout_and_os_auditor(self) -> None:
        world = self._fresh_afton_world()
        win = EodWorldEditorWindow(game_dir=DISC_DIR, auto_load=False, show_home_hub=False)
        dlg = VehicleTunerDialog(DISC_DIR)
        try:
            win._attach_loaded_world(world)
            win.show()
            dlg.show()
            self.app.processEvents()

            # 1a. Walk every menu & submenu: assert zero duplicate sibling labels
            duplicate_hits: list[str] = []
            seen_top: set[str] = set()
            menu_texts: list[str] = []

            def walk_menu(menu: QMenu, path: str) -> None:
                siblings: set[str] = set()
                for act in menu.actions():
                    if act.isSeparator():
                        continue
                    lbl = _norm_label(act.text())
                    if not lbl:
                        continue
                    menu_texts.append(f"{path} > {lbl}")
                    key = lbl.lower()
                    if key in siblings:
                        duplicate_hits.append(f"{path} > {lbl}")
                    siblings.add(key)
                    if act.menu() is not None:
                        walk_menu(act.menu(), f"{path} > {lbl}")

            for top_act in win.menuBar().actions():
                if top_act.isSeparator():
                    continue
                tlbl = _norm_label(top_act.text())
                if tlbl.lower() in seen_top:
                    duplicate_hits.append(f"MenuBar > {tlbl}")
                seen_top.add(tlbl.lower())
                if top_act.menu() is not None:
                    walk_menu(top_act.menu(), tlbl)

            self.assertEqual(
                duplicate_hits,
                [],
                f"Duplicate menu/submenu action labels detected: {duplicate_hits}",
            )
            self.assertGreater(len(menu_texts), 30, "Menu bar appears unexpectedly empty")

            # 1b. macOS OS-leak auditor: zero 'eod.exe' or 'Windows 11' in visible UI text
            if sys.platform == "darwin":
                forbidden = ("eod.exe", "windows 11")
                leaks: list[str] = []

                def scan_widget_tree(root: QWidget, scope: str) -> None:
                    for w in [root, *root.findChildren(QWidget)]:
                        texts: list[str] = []
                        if isinstance(w, (QLabel, QAbstractButton)):
                            texts.append(w.text())
                        elif isinstance(w, QComboBox):
                            for i in range(w.count()):
                                texts.append(w.itemText(i))
                        if w.toolTip():
                            texts.append(w.toolTip())
                        if w.statusTip():
                            texts.append(w.statusTip())
                        for txt in texts:
                            low = txt.lower()
                            for bad in forbidden:
                                if bad in low:
                                    leaks.append(f"{scope}:{type(w).__name__}({txt!r})")

                    for act in root.findChildren(QMenu):
                        for a in act.actions():
                            for txt in (a.text(), a.toolTip(), a.statusTip()):
                                low = txt.lower()
                                for bad in forbidden:
                                    if bad in low:
                                        leaks.append(f"{scope}:MenuAction({txt!r})")

                scan_widget_tree(win, "MainWindow")
                scan_widget_tree(dlg, "VehicleTunerDialog")
                self.assertEqual(leaks, [], f"Windows-only strings leaked into macOS UI: {leaks}")

            # 1c. Responsive layout resize stress across 800x540, 1280x720, 1920x1080
            toolbars = win.findChildren(QToolBar)
            self.assertGreater(len(toolbars), 0, "Expected at least one toolbar")
            for w_px, h_px in ((800, 540), (1280, 720), (1920, 1080)):
                win.resize(w_px, h_px)
                self.app.processEvents()
                self.assertGreater(win.explorer.width(), 0, f"Explorer collapsed at {w_px}x{h_px}")
                self.assertGreater(win.properties.width(), 0, f"Inspector collapsed at {w_px}x{h_px}")
                self.assertGreater(win.viewport.width(), 0, f"Viewport collapsed at {w_px}x{h_px}")
                for tb in toolbars:
                    if tb.isVisible():
                        self.assertGreater(tb.width(), 0, f"Toolbar collapsed at {w_px}x{h_px}")
        finally:
            dlg.close()
            self._cleanup_window(win)

    # ------------------------------------------------------------------
    # Test 2: Journey 1 — Exact Audit Repro -> Live Headless `eod` Boot
    # ------------------------------------------------------------------
    def test_02_journey1_audit_repro_and_live_headless_eod_boot(self) -> None:
        self.assertTrue(EOD_BIN.is_file(), f"Missing eod binary: {EOD_BIN}")
        world = self._fresh_afton_world()
        win = EodWorldEditorWindow(game_dir=DISC_DIR, auto_load=False, show_home_hub=False)
        try:
            win._attach_loaded_world(world)

            # 2a. Place a real track.grf@0x... prop
            track_grf_key = next(
                (k for k in world.models.keys() if k.startswith("track.grf@0x") and world.models[k].tri_count > 0),
                None,
            )
            self.assertIsNotNone(track_grf_key, "Expected track.grf@0x... model in Afton")
            assert track_grf_key is not None
            before_count = sum(1 for o in world.objects if not o.deleted)
            win._place_model_in_front_of_camera(track_grf_key)
            placed = win.viewport.selected_obj
            self.assertIsNotNone(placed, "Placing track.grf@0x... model must select new object")
            assert placed is not None
            self.assertEqual(placed.model_key, track_grf_key)

            # 2b. Alt+Drag clone via altDuplicateCompleted -> deselect -> ZERO objects vanished
            clones = win.viewport._spawn_alt_drag_clones([placed])
            self.assertEqual(len(clones), 1)
            clones[0].position = (placed.position[0] + 5.0, placed.position[1], placed.position[2] + 5.0)
            win.viewport.altDuplicateCompleted.emit(clones, "Alt+Drag Duplicate")
            self.app.processEvents()
            self.assertEqual(win.undo_stack.undoText(), "Alt+Drag Duplicate")

            win.viewport.select_object(None)
            self.app.processEvents()
            live_ids = {o.obj_id for o in world.objects if not o.deleted}
            self.assertIn(placed.obj_id, live_ids, "Placed object vanished after deselect!")
            self.assertIn(clones[0].obj_id, live_ids, "Alt+Drag clone vanished after deselect!")
            self.assertEqual(len(live_ids), before_count + 2)

            # 2c. Select ramp-in_1dab8 -> assert 3-part compound group selected
            ramp_obj = next(
                (o for o in world.objects if not o.deleted and "ramp-in_1dab8" in o.name.lower()),
                None,
            )
            self.assertIsNotNone(ramp_obj, "Expected ramp-in_1dab8 in Afton")
            assert ramp_obj is not None
            members = world.get_group_members(ramp_obj, visible_only=True)
            self.assertEqual(
                len(members),
                3,
                f"Expected 3-part compound group for ramp-in_1dab8, got {len(members)}: {[m.name for m in members]}",
            )
            ordered = [m for m in members if m is not ramp_obj] + [ramp_obj]
            win.viewport.select_objects(ordered)
            self.app.processEvents()
            self.assertEqual(len(win.viewport.selected_objects), 3)

            # 2d. Type 2.0 into Scale spinbox WITHOUT pressing Enter -> flush_pending_edits -> export
            win.properties.obj_scl_ed.x.lineEdit().setText("2.0")
            win.properties.obj_scl_ed.y.lineEdit().setText("2.0")
            win.properties.obj_scl_ed.z.lineEdit().setText("2.0")
            win.properties.flush_pending_edits()
            self.app.processEvents()
            for m in members:
                self.assertAlmostEqual(m.scale[0], 2.0, places=3, msg=f"Compound member {m.name} X scale not applied")
                self.assertAlmostEqual(m.scale[1], 2.0, places=3, msg=f"Compound member {m.name} Y scale not applied")
                self.assertAlmostEqual(m.scale[2], 2.0, places=3, msg=f"Compound member {m.name} Z scale not applied")

            with tempfile.TemporaryDirectory(prefix="eod_rt_j1_") as tmp:
                tmp_path = Path(tmp)
                mod_dir = tmp_path / "mod_out"
                report = ps.export_mod_layer(world, "RedTeam Journey 1", mod_dir, allow_delta=False)

                editor_placed_n = sum(1 for o in world.objects if not o.deleted and o.source == "Placed by the editor")
                self.assertEqual(editor_placed_n, 2)
                self.assertEqual(
                    int(report["custom_objects_compiled"]),
                    int(report.get("editor_placed_objects", editor_placed_n)),
                    f"Custom compiled mismatch: report={report}",
                )
                self.assertEqual(int(report["custom_objects_compiled"]), 2)
                self.assertGreaterEqual(int(report["vif_models_baked"]), 3, f"Expected 3 scaled ramp VIF models baked: {report}")
                self.assertGreater(int(report["vif_vertices_baked"]), 0)
                self.assertTrue(report["mod_trk_differs_from_retail"])
                self.assertTrue((mod_dir / "DATA" / "TRACKS" / "AFTON.TRK").is_file())

                # 2e. Boot live headless `eod` with EOD_MOD_DIR & EOD_TRACE_OUT
                trace_out = tmp_path / "rt_game.json"
                env = os.environ.copy()
                env.update(
                    {
                        "EOD_MOD_DIR": str(mod_dir),
                        "EOD_TRACE_OUT": str(trace_out),
                        "EOD_SKIP_MOVIES": "1",
                        "EOD_FAST_BOOT": "1",
                        "EOD_SKIP_MEMCARD": "1",
                        "EOD_DIRECT_START": "mode=action;track=Afton;event=Battle;car=Algar;paint=0;humans=1;ai=2",
                    }
                )
                proc = subprocess.run(
                    [str(EOD_BIN), "--headless", "--frames", "60", "--data", str(DISC_DIR)],
                    cwd=str(REPO_ROOT),
                    env=env,
                    capture_output=True,
                    text=True,
                    timeout=90,
                )
                self.assertEqual(
                    proc.returncode,
                    0,
                    f"Headless eod crashed with mod layer (rc={proc.returncode}):\n"
                    f"STDOUT:\n{proc.stdout[-1200:]}\nSTDERR:\n{proc.stderr[-1200:]}",
                )
                self.assertTrue(trace_out.is_file(), "rt_game.json not emitted by headless eod")
                trace_data = json.loads(trace_out.read_text(encoding="utf-8"))
                self.assertGreater(len(trace_data.get("traceEvents", [])), 0, "Empty traceEvents from headless eod")
        finally:
            self._cleanup_window(win)

    # ------------------------------------------------------------------
    # Test 3: Journey 2 — Procedural Road + Blender OBJ + Undo/Redo Storm
    # ------------------------------------------------------------------
    def test_03_journey2_procedural_roads_blender_obj_and_undo_storm(self) -> None:
        world = self._fresh_afton_world()
        win = EodWorldEditorWindow(game_dir=DISC_DIR, auto_load=False, show_home_hub=False)
        try:
            win._attach_loaded_world(world)

            oval = win.build_procedural_road_preset("oval")
            bridge = win.build_procedural_road_preset("bridge")
            island = win.build_procedural_island()
            self.assertIsNotNone(oval)
            self.assertIsNotNone(bridge)
            self.assertIsNotNone(island)
            assert oval is not None and bridge is not None and island is not None
            built_objs = [oval, bridge, island]
            self.assertEqual(len(world.procedural_roads), 3)

            # Undo 3x -> all 3 deleted
            for _ in range(3):
                win.undo_stack.undo()
            self.app.processEvents()
            for o in built_objs:
                self.assertTrue(o.deleted, f"Undo storm failed to mark {o.name} deleted")

            # Redo 3x -> all 3 restored
            for _ in range(3):
                win.undo_stack.redo()
            self.app.processEvents()
            for o in built_objs:
                self.assertFalse(o.deleted, f"Redo storm failed to restore {o.name}")

            with tempfile.TemporaryDirectory(prefix="eod_rt_j2_") as tmp:
                tmp_path = Path(tmp)

                # Blender OBJ export -> re-import via eod_bridge.parse_obj_to_mesh_parts
                obj_path = ps.export_object_obj(world, bridge, tmp_path / "blender_bridge")
                self.assertTrue(obj_path.is_file())
                reimported_parts = eod_bridge.parse_obj_to_mesh_parts(obj_path)
                self.assertGreater(len(reimported_parts), 0)
                reimported_tris = sum(int(p.tri_count) for p in reimported_parts)
                self.assertEqual(reimported_tris, bridge.tri_count)

                # Save & reload .eodworld.json preserving procedural_roads
                overlay_path = ps.save_project_overlay(tmp_path / "rt_roads.eodworld.json", "RedTeam Roads", world)
                self.assertTrue(overlay_path.is_file())
                reloaded = self._fresh_afton_world()
                ps.apply_project_overlay(reloaded, overlay_path)
                self.assertEqual(len(reloaded.procedural_roads), 3)

                # Export mod layer verifying .eodcol + minimap_preview.png
                mod_dir = tmp_path / "mod_out"
                report = ps.export_mod_layer(reloaded, "RedTeam Roads", mod_dir, allow_delta=False)
                self.assertGreaterEqual(int(report["custom_objects_compiled"]), 3)
                eodcol_path = mod_dir / "DATA" / "TRACKS" / "AFTON.eodcol"
                self.assertTrue(eodcol_path.is_file())
                unpacked = rc.unpack_eodcol(eodcol_path.read_bytes())
                self.assertGreater(len(unpacked.triangles), 0)
                minimap_path = mod_dir / "minimap_preview.png"
                self.assertTrue(minimap_path.is_file())
                with Image.open(minimap_path) as im:
                    self.assertEqual(im.size, (256, 256))
        finally:
            self._cleanup_window(win)

    # ------------------------------------------------------------------
    # Test 4: Journey 3 — Car Studio + Paint->HOSTTEX + 125-Car NASCAR + .eodmod v2
    # ------------------------------------------------------------------
    def test_04_journey3_car_studio_hosttex_nascar_and_eodmod_v2(self) -> None:
        if not (DISC_DIR / "DATA" / "CAR" / "ALGAR.CAR").is_file():
            self.skipTest("Retail ALGAR.CAR not present")

        dlg = VehicleTunerDialog(DISC_DIR)
        try:
            dlg.show()
            self.app.processEvents()
            self.assertEqual(dlg._vehicle_tabs.count(), 4)
            asm = dlg.showroom.assembly
            self.assertIsNotNone(asm)
            assert asm is not None

            # 4a. Scrub damage slider 0 -> 80% & verify visual pixel delta
            dlg.showroom.sld_damage.setValue(0)
            self.app.processEvents()
            img0 = render_showroom_frame(asm, width=256, height=160, damage_pct=0.0)
            dlg.showroom.sld_damage.setValue(80)
            self.app.processEvents()
            self.assertIn("80%", dlg.showroom.lbl_damage.text())
            img80 = render_showroom_frame(asm, width=256, height=160, damage_pct=80.0)
            b0 = np.frombuffer(img0.constBits(), dtype=np.uint8).reshape((160, 256, 4)).copy()
            b80 = np.frombuffer(img80.constBits(), dtype=np.uint8).reshape((160, 256, 4)).copy()
            self.assertGreater(int(np.any(b0[:, :, :3] != b80[:, :, :3], axis=2).sum()), 120)

            # 4b. Verify Dyno/Telemetry graphs update when torque/physics changes
            base_hp_lbl = dlg.telemetry._kpi_labels["peak_hp"].text()
            base_tq_lbl = dlg.telemetry._kpi_labels["peak_torque_ftlb"].text()
            boosted_phys = {k: (dict(v) if isinstance(v, dict) else v) for k, v in asm.physics_fields.items()}
            eng = boosted_phys.setdefault("Engine", {})
            eng["torque_max"] = str(float(dlg.telemetry._phys["torque_max"]) * 1.65)
            eng["power_max"] = str(float(dlg.telemetry._phys["power_max"]) * 1.65)
            dlg.telemetry.set_physics(boosted_phys)
            self.app.processEvents()
            new_hp_lbl = dlg.telemetry._kpi_labels["peak_hp"].text()
            new_tq_lbl = dlg.telemetry._kpi_labels["peak_torque_ftlb"].text()
            self.assertNotEqual(
                (base_hp_lbl, base_tq_lbl),
                (new_hp_lbl, new_tq_lbl),
                f"Dyno/Telemetry KPIs must update when engine physics changes ({base_hp_lbl}/{base_tq_lbl} vs {new_hp_lbl}/{new_tq_lbl})",
            )

            # 4c. Real player-outcome Paint/Replace -> HOSTTEX EODHTX1 -> verify
            with tempfile.TemporaryDirectory(prefix="eod_rt_j3_") as tmp:
                tmp_path = Path(tmp)
                tex_key = asm.body_texture_key or next(iter(asm.textures))
                exported_png = export_car_texture_png(asm, tex_key, tmp_path / "stock_body.png")
                self.assertTrue(exported_png.is_file())
                stock_render = render_showroom_frame(asm, width=256, height=160, damage_pct=0.0)
                stock_buf = np.frombuffer(stock_render.constBits(), dtype=np.uint8).reshape((160, 256, 4)).copy()

                # Paint a high-contrast racing stripe livery onto the exported PNG
                with Image.open(exported_png) as im:
                    painted = im.convert("RGBA")
                    px = np.array(painted, dtype=np.uint8)
                    px[:, :, 0] = 235
                    px[:, :, 1] = 24
                    px[:, :, 2] = 36
                    px[:, ::8, :] = [255, 240, 40, 255]
                    painted_png = tmp_path / "painted_body.png"
                    Image.fromarray(px, mode="RGBA").save(painted_png, format="PNG")

                updated = replace_car_texture_png(asm, tex_key, painted_png)
                self.assertTrue(updated.is_replaced)
                painted_render = render_showroom_frame(asm, width=256, height=160, damage_pct=0.0)
                painted_buf = np.frombuffer(painted_render.constBits(), dtype=np.uint8).reshape((160, 256, 4)).copy()
                paint_diff = int(np.any(stock_buf[:, :, :3] != painted_buf[:, :, :3], axis=2).sum())
                self.assertGreater(paint_diff, 200, "Replacing car paint must visibly alter rendered 3D showroom pixels")

                eodhtx_path = upscale_car_texture_hosttex(asm, tex_key, tmp_path / "hosttex", scale=2)
                self.assertTrue(eodhtx_path.is_file())
                htx_bytes = eodhtx_path.read_bytes()
                self.assertEqual(htx_bytes[:8], b"EODHTX1\0")
                tw, th, mips, flags = struct.unpack_from("<4H", htx_bytes, 8)
                self.assertEqual(flags, 0)
                self.assertEqual(tw, updated.width * 2)
                self.assertEqual(th, updated.height * 2)
                self.assertEqual(mips, max(tw, th).bit_length())
                expected_len = 16 + sum(max(1, tw >> i) * max(1, th >> i) * 4 for i in range(mips))
                self.assertEqual(len(htx_bytes), expected_len)

                # 4d. Scan real NASCAR ISO (if present) + ephemeral test-drive discard
                if REAL_NASCAR_ISO.is_file():
                    scan = scan_nascar_source(REAL_NASCAR_ISO)
                    self.assertEqual(len(scan.entries), 125, f"Expected 125 NASCAR cars, got {len(scan.entries)}")
                    series_set = {e.series for e in scan.entries}
                    self.assertEqual(len(series_set), 5, f"Expected 5 NASCAR series, got {series_set}")
                    sess = EphemeralTestDriveSession(REAL_NASCAR_ISO, DISC_DIR, scan.entries[0])
                    built_veh = sess.build()
                    staged = sess.temp_dir
                    self.assertIsNotNone(built_veh)
                    self.assertTrue(sess.overlay_dir.is_dir())
                    sess.discard()
                    self.assertFalse(staged.exists(), "Ephemeral NASCAR session must clean up completely on discard")

                # 4e. Pack & compose two .eodmod v2 packages cleanly
                pkg_a = tmp_path / "mod_a.eodmod"
                pkg_b = tmp_path / "mod_b.eodmod"
                mp2.pack_eodmod_v2(
                    {
                        "schemaVersion": 2,
                        "id": "rt.paint.a",
                        "name": "RedTeam Paint Mod A",
                        "version": "1.0.0",
                        "operations": [
                            {
                                "op": "hostTexture",
                                "target": f"hosttex/{eodhtx_path.name}",
                                "blob": f"tex/{eodhtx_path.name}",
                            },
                            {
                                "op": "valtPatch",
                                "table": "DATA/CAR/ALGAR.CAR#Physics.val",
                                "patches": {"Engine": {"MaxTorque": "820.0"}},
                            },
                        ],
                    },
                    {f"tex/{eodhtx_path.name}": htx_bytes},
                    pkg_a,
                )
                mp2.pack_eodmod_v2(
                    {
                        "schemaVersion": 2,
                        "id": "rt.tune.b",
                        "name": "RedTeam Handling Mod B",
                        "version": "1.0.0",
                        "operations": [
                            {
                                "op": "valtPatch",
                                "table": "DATA/CAR/ALGAR.CAR#Physics.val",
                                "patches": {"Chassis": {"Mass": "1450.0"}},
                            },
                        ],
                    },
                    {},
                    pkg_b,
                )
                stage_dir = tmp_path / "composed_stage"
                composed = mp2.compose_eodmod_v2_stack([pkg_a, pkg_b], stage_dir)
                self.assertEqual(composed["counts"]["packages"], 2)
                self.assertEqual(composed["counts"]["hostTexture"], 1)
                self.assertEqual(composed["counts"]["valtPatch"], 2)
                self.assertEqual(len(composed["valt_conflicts"]), 0)
                self.assertTrue((stage_dir / "hosttex" / eodhtx_path.name).is_file())
                merged_valt = json.loads((stage_dir / "valt_patches.json").read_text(encoding="utf-8"))
                tbl = merged_valt["tables"]["data/car/algar.car#physics.val"]
                self.assertEqual(tbl["Engine"]["MaxTorque"], "820.0")
                self.assertEqual(tbl["Chassis"]["Mass"], "1450.0")
        finally:
            dlg.close()
            self.app.processEvents()

    # ------------------------------------------------------------------
    # Test 5: Journey 4 — Hotkey/Modifier Fuzzing + Tracy/Perfetto Latency Gate
    # ------------------------------------------------------------------
    def test_05_journey4_hotkey_fuzzing_and_tracy_latency_budget_gate(self) -> None:
        world = self._fresh_afton_world()
        win = EodWorldEditorWindow(game_dir=DISC_DIR, auto_load=False, show_home_hub=False)
        try:
            win._attach_loaded_world(world)
            vp = win.viewport
            vp.resize(1280, 720)
            self.app.processEvents()

            # 5a. Modifier chords must NEVER move camera or latch WASD flight timer
            cam_before = vp.cam_pos.copy()
            for key, mods in (
                (Qt.Key.Key_S, Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.ShiftModifier),
                (Qt.Key.Key_S, Qt.KeyboardModifier.MetaModifier | Qt.KeyboardModifier.ShiftModifier),
                (Qt.Key.Key_W, Qt.KeyboardModifier.ControlModifier),
                (Qt.Key.Key_A, Qt.KeyboardModifier.AltModifier),
                (Qt.Key.Key_D, Qt.KeyboardModifier.MetaModifier),
            ):
                ev = QKeyEvent(QEvent.Type.KeyPress, key, mods)
                self.app.sendEvent(vp, ev)
                vp._on_tick()
                self.assertFalse(vp._timer.isActive(), f"Modifier chord latched flight timer for key={key}")
                np.testing.assert_allclose(vp.cam_pos, cam_before, atol=1e-6)

            # Tab opens and closes drawers
            win.select_tool("move")
            for drawers_open in (True, False):
                ev_tab = QKeyEvent(QEvent.Type.KeyPress, Qt.Key.Key_Tab, Qt.KeyboardModifier.NoModifier)
                self.app.sendEvent(vp, ev_tab)
                self.assertEqual(any(win._side_panel_requests.values()), drawers_open)
                self.assertEqual(vp.active_tool, "move")

            # Fuzz single-key hotkeys G, R, T, P, B, Escape without crash
            for k in (Qt.Key.Key_G, Qt.Key.Key_R, Qt.Key.Key_T, Qt.Key.Key_P, Qt.Key.Key_B, Qt.Key.Key_Escape):
                ev = QKeyEvent(QEvent.Type.KeyPress, k, Qt.KeyboardModifier.NoModifier)
                self.app.sendEvent(vp, ev)
                ev_rel = QKeyEvent(QEvent.Type.KeyRelease, k, Qt.KeyboardModifier.NoModifier)
                self.app.sendEvent(vp, ev_rel)
            self.app.processEvents()

            # 5b. Verify vectorized AABB cache matches WorldObject.world_aabb() to <1e-3 across Afton!
            vp._ensure_pick_aabb_cache()
            self.assertIsNotNone(vp._pick_aabb_lo)
            self.assertIsNotNone(vp._pick_aabb_hi)
            assert vp._pick_aabb_lo is not None and vp._pick_aabb_hi is not None
            self.assertGreater(len(vp._pick_aabb_objs), 1000)
            for idx in (0, len(vp._pick_aabb_objs) // 3, len(vp._pick_aabb_objs) // 2, len(vp._pick_aabb_objs) - 1):
                obj = vp._pick_aabb_objs[idx]
                ref_lo, ref_hi = obj.world_aabb(world.models[obj.model_key])
                np.testing.assert_allclose(vp._pick_aabb_lo[idx], ref_lo, atol=1e-3)
                np.testing.assert_allclose(vp._pick_aabb_hi[idx], ref_hi, atol=1e-3)

            # Verify cache invalidation updates AABB when object moves & mark_scene_dirty is called
            probe_obj = vp._pick_aabb_objs[0]
            orig_pos = probe_obj.position
            probe_obj.position = (orig_pos[0] + 17.5, orig_pos[1], orig_pos[2] - 8.0)
            vp.mark_scene_dirty()
            self.assertTrue(vp._pick_aabb_dirty)
            vp._ensure_pick_aabb_cache()
            ref_lo, ref_hi = probe_obj.world_aabb(world.models[probe_obj.model_key])
            probe_idx = vp._pick_aabb_objs.index(probe_obj)
            np.testing.assert_allclose(vp._pick_aabb_lo[probe_idx], ref_lo, atol=1e-3)
            np.testing.assert_allclose(vp._pick_aabb_hi[probe_idx], ref_hi, atol=1e-3)
            # Object drags patch the existing row without scanning the rest of the scene.
            previous_lo = vp._pick_aabb_lo
            probe_obj.position = (orig_pos[0] - 4.0, orig_pos[1] + 3.0, orig_pos[2])
            vp.mark_objects_dirty([probe_obj])
            vp._ensure_pick_aabb_cache()
            self.assertIs(vp._pick_aabb_lo, previous_lo)
            lo, hi = probe_obj.world_aabb(world.models[probe_obj.model_key])
            np.testing.assert_allclose(vp._pick_aabb_lo[probe_idx], lo, atol=1e-3)
            np.testing.assert_allclose(vp._pick_aabb_hi[probe_idx], hi, atol=1e-3)
            probe_obj.position = orig_pos
            vp.mark_scene_dirty()

            # 5c. Benchmark 10x explorer._rebuild_model_grid() + 10x viewport._pick_object_with_t()
            prof = get_profiler()
            prof.clear_zones()
            vp._ensure_pick_aabb_cache()  # warm cache after restoring probe_obj

            queries = ("", "tree", "ramp", "car", "", "wall", "fence", "", "road", "")
            for q in queries:
                win.explorer.model_search.setText(q)
                win.explorer._rebuild_model_grid()

            for i in range(10):
                sx = 200.0 + i * 90.0
                sy = 180.0 + (i % 5) * 80.0
                ray_o, ray_d = vp.screen_ray(sx, sy)
                vp._pick_object_with_t(ray_o, ray_d)

            stats = prof.zone_stats()
            self.assertIn("viewport.pick_cpu", stats)
            self.assertIn("explorer.rebuild_grid", stats)
            pick_p95 = float(stats["viewport.pick_cpu"]["p95_ms"])
            grid_p95 = float(stats["explorer.rebuild_grid"]["p95_ms"])
            rss_mb = get_process_rss_mb()

            with tempfile.TemporaryDirectory(prefix="eod_rt_j4_") as tmp:
                trace_path = prof.dump_chrome_trace(Path(tmp) / "redteam_editor_trace.json")
                self.assertTrue(trace_path.is_file())
                parsed = json.loads(trace_path.read_text(encoding="utf-8"))
                self.assertGreater(len(parsed.get("traceEvents", [])), 15)

            print(
                f"\n[REDTEAM_PERF] RSS={rss_mb:.1f} MB | "
                f"viewport.pick_cpu p50={stats['viewport.pick_cpu']['p50_ms']:.2f} ms "
                f"p95={pick_p95:.2f} ms (budget < 15.0 ms) | "
                f"explorer.rebuild_grid p50={stats['explorer.rebuild_grid']['p50_ms']:.2f} ms "
                f"p95={grid_p95:.2f} ms (budget < 25.0 ms)"
            )
            self.assertLess(pick_p95, 15.0, f"viewport.pick_cpu p95 {pick_p95:.2f} ms exceeded 15.0 ms budget!")
            self.assertLess(grid_p95, 25.0, f"explorer.rebuild_grid p95 {grid_p95:.2f} ms exceeded 25.0 ms budget!")
            self.assertGreater(rss_mb, 0.0)
        finally:
            self._cleanup_window(win)


if __name__ == "__main__":
    unittest.main(verbosity=2)
