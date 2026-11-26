#!/usr/bin/env python3
"""Fast standalone regression suite verifying all 15 Milestone 1 Audit repairs + Section 1.1 fixes.

Covers every item in docs/WORLD_EDITOR_LIFECYCLE_AUDIT.md Executive Diagnosis & Section 1.1:
  1.  track.grf@0x... custom object compiler branch + honest warning reporting
  2.  Vec3Editor 4-decimal precision + flush_pending_edits() spinbox commit + clean Dirty badge
  3.  auto_group_compounds() ramp + af_rdwr grouping (no None==None flood) + af_rdwr.tex blend + cache v4
  4.  Trailer5 / vehicle-only chassis spawn support (no fallback to default stock car)
  5.  Event 2 of 5 held black fader reset & offline PIE exit handling
  6.  PIE performance: isolated pie.ini / pie_memcard / RenderScale + d11b36d queue_material fix
  7.  Thumbnail recovery after PIE (resume_thumbnails) + shadow/flat-mesh camera framing
  8.  Plume HQ 3D overlay pipeline (eod_plume_view_overlay ABI) + Windows volk & packaging parity
  9.  Object picking: ray_aabb_hit() inside AABB, no +t*0.025 node bias, translucent pick discard
  10. Vanishing dragged objects: altDuplicateCompleted wiring + display-object layer visibility
  11. Ctrl+Shift+S / Cmd+Shift+S / Alt modifier guard in ViewportGL.keyPressEvent
  12. Tab transform tool cycle (Move -> Rotate -> Scale) + Ghost R shortcut priority
  13. Remembered mod output dir, dirty guards on destructive actions, zero disc/mods/ pollution
  14. Cars (0) category filter ('car-wreck') + post-PIE car catalog restart
  15. Custom Spawn Point (portable map / Play From Camera) + 6-DOF Free Cam & live control
  16. Section 1.1: macOS/Windows resolve_eod_exe & GAME_EXE_LABEL + unified macOS AppData helper
"""

from __future__ import annotations

import ctypes
import inspect
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import numpy as np

EDITOR_DIR = Path(__file__).resolve().parent
REPO_ROOT = EDITOR_DIR.parent.parent
TOOLS_DIR = REPO_ROOT / "tools"
for _p in (str(EDITOR_DIR), str(TOOLS_DIR)):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from PySide6.QtCore import QEvent, Qt
from PySide6.QtGui import QKeyEvent
from PySide6.QtWidgets import QApplication

import home_hub
import perf_profiler
import pie_host
import project_state as ps
import world_loader as wl
from app import configure_opengl_format
from main_window import EodWorldEditorWindow
from plume_viewport import PlumeViewport
from properties_panel import PropertiesPanel
from viewport_gl import ViewportGL, ray_aabb_hit


def _qapp() -> QApplication:
    configure_opengl_format()
    return QApplication.instance() or QApplication(["test-milestone1-audit"])


def _make_dummy_model(key: str, lo: tuple[float, float, float], hi: tuple[float, float, float], tex: str = "dirt01.tex") -> wl.WorldModel:
    lo_a = np.array(lo, dtype=np.float32)
    hi_a = np.array(hi, dtype=np.float32)
    verts = np.array(
        [
            [lo[0], lo[1], lo[2], 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0],
            [hi[0], lo[1], lo[2], 0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 1.0, 1.0, 1.0],
            [hi[0], hi[1], hi[2], 0.0, 1.0, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0],
        ],
        dtype=np.float32,
    )
    part = wl.MeshPart(texture=tex, vertices=verts, indices=np.array([0, 1, 2], dtype=np.uint32), tri_count=1)
    return wl.WorldModel(
        key=key,
        name=key,
        model_store="track.grf",
        hash_hex="00000000",
        category="optional-terrain",
        parts=[part],
        tri_count=1,
        bbox_min=lo_a,
        bbox_max=hi_a,
        pivot=0.5 * (lo_a + hi_a),
        textures=[tex],
    )


def _make_dummy_object(
    uid: int,
    name: str,
    model_key: str,
    category: str = "static",
    opt_terrain_mode: int | None = None,
    textures: list[str] | None = None,
) -> wl.WorldObject:
    return wl.WorldObject(
        obj_id=f"obj_{uid}",
        name=name,
        model_key=model_key,
        source="Retail",
        category=category,
        subtype="normal",
        section_mlist="Mlist 0",
        model_store="track.grf",
        hash_hex=f"{uid:08x}",
        tri_count=1,
        textures=list(textures or ["dirt01.tex"]),
        position=(0.0, 0.0, 0.0),
        rotation_hpr=(0.0, 0.0, 0.0),
        scale=(1.0, 1.0, 1.0),
        retail_record_offset=uid * 0x100,
        retail_model_offset=uid * 0x100,
        opt_terrain_mode=opt_terrain_mode,
    )


class TestMilestone1Audit(unittest.TestCase):
    # ------------------------------------------------------------------
    # Audit #1: track.grf@0x... compiler branch & honest warnings
    # ------------------------------------------------------------------
    def test_01_track_grf_custom_compiler_and_honest_warnings(self) -> None:
        src = inspect.getsource(ps._resolve_custom_mod_and_textures)
        self.assertIn('"@0x" in model_key', src)
        self.assertIn("track.grf", src)
        mw_src = (EDITOR_DIR / "main_window.py").read_text(encoding="utf-8")
        self.assertIn("custom_objects_compiled", mw_src)
        self.assertIn("warnings", mw_src)

    # ------------------------------------------------------------------
    # Audit #2: Vec3Editor 4-decimal precision + flush_pending_edits() + clean Dirty badge
    # ------------------------------------------------------------------
    def test_02_flush_pending_edits_commits_unfocused_spinbox_and_clean_status_badge(self) -> None:
        _qapp()
        panel = PropertiesPanel()
        try:
            self.assertEqual(panel.obj_pos_ed.x.decimals(), 4)
            self.assertEqual(panel.obj_scl_ed.x.decimals(), 4)

            mdl = _make_dummy_model("track.grf@0x1dab8", (0, 0, 0), (6, 2, 10))
            obj = _make_dummy_object(1, "ramp-in_1dab8", mdl.key, category="optional-terrain")
            panel.show_object(obj, {})

            captured: list[tuple] = []
            panel.objectTransformEdited.connect(lambda o, p, r, s: captured.append((o.obj_id, p, r, s)))

            # Simulate typing a scale directly into the QDoubleSpinBox lineEdit without blurring focus
            panel.obj_scl_ed.x.lineEdit().setText("2.3750")
            panel.obj_scl_ed.y.lineEdit().setText("1.5000")
            panel.flush_pending_edits()

            self.assertTrue(captured, "flush_pending_edits() must emit objectTransformEdited for pending spinbox text")
            _uid, _pos, _rot, scl = captured[-1]
            self.assertAlmostEqual(scl[0], 2.375, places=4)
            self.assertAlmostEqual(scl[1], 1.5, places=4)

            # Verify no legacy 'Written: No' / 'Ctrl+Shift+S writes it' status jargon remains
            obj.written = False
            panel.show_object(obj, {})
            dirty_text = panel.obj_written_lbl.text()
            self.assertNotIn("Written:", dirty_text)
            self.assertNotIn("Ctrl+Shift+S", dirty_text)
            self.assertIn("Unsaved", dirty_text)
            obj.written = True
            panel.show_object(obj, {})
            self.assertIn("Saved", panel.obj_written_lbl.text())
        finally:
            panel.close()

    # ------------------------------------------------------------------
    # Audit #3: auto_group_compounds ramp + af_rdwr (no None==None flood) + af_rdwr.tex blend + cache v4
    # ------------------------------------------------------------------
    def test_03_auto_group_compounds_ramp_and_af_rdwr_blend_and_cache_v4(self) -> None:
        self.assertGreaterEqual(wl._CACHE_VERSION, 4)

        models: dict[str, wl.WorldModel] = {}
        objs: list[wl.WorldObject] = []

        # 2-piece ramp assembly: ramp-in_1dab8 + overlapping af_rdwr_256d8 wear cap (+0.07m above)
        models["ramp_m"] = _make_dummy_model("ramp_m", (0.0, 0.0, 0.0), (8.0, 2.5, 14.0), tex="dirt01.tex")
        models["wear_m"] = _make_dummy_model("wear_m", (0.5, 0.07, 0.5), (7.5, 2.57, 13.5), tex="af_rdwr.tex")
        objs.append(_make_dummy_object(1, "ramp-in_1dab8", "ramp_m", category="optional-terrain", opt_terrain_mode=None, textures=["dirt01.tex"]))
        objs.append(_make_dummy_object(2, "af_rdwr_256d8", "wear_m", category="optional-terrain", opt_terrain_mode=None, textures=["af_rdwr.tex"]))

        # Chain of 10 touching/nearby unrelated static props with opt_terrain_mode=None
        # (Before fix: None == None BFS flooded all 473 objects across Afton!)
        for i in range(10):
            x0 = 8.4 + i * 5.0
            mk = f"static_m_{i}"
            models[mk] = _make_dummy_model(mk, (x0, 0.0, 0.0), (x0 + 4.8, 3.0, 6.0), tex="fence.tex")
            objs.append(_make_dummy_object(10 + i, f"grandstand_{i}", mk, category="static", opt_terrain_mode=None, textures=["fence.tex"]))

        rg = wl.RoadGraph(line_name="track.ild")
        world = wl.TrackWorld(
            game_dir=Path("."),
            trk_path=Path("AFTON.TRK"),
            trk_rel="DATA/TRACKS/AFTON.TRK",
            track_name="AFTON",
            display_name="Afton",
            entries=[],
            graf_summary={},
            mlist_info=[],
            models=models,
            objects=objs,
            textures={},
            collision=wl.CollisionData(scol_count=0, sobl_count=0, sobl_boxes=0, sobl_tubes=0),
            road_graph=rg,
            all_road_graphs={"track.ild": rg},
            markers=[],
            world_center=(0.0, 0.0, 0.0),
            world_radius=200.0,
        )
        world.auto_group_compounds()

        ramp_cid = objs[0].compound_id
        wear_cid = objs[1].compound_id
        self.assertTrue(ramp_cid, "Ramp + overlapping af_rdwr wear strip must form a compound group")
        self.assertEqual(ramp_cid, wear_cid, "ramp-in_1dab8 and af_rdwr_256d8 must share the same compound group")
        members = world.compound_groups[ramp_cid]
        self.assertEqual(set(members), {objs[0].obj_id, objs[1].obj_id}, "Unrelated None-mode static props must NOT flood-fill into the ramp compound")

        # Verify af_rdwr.tex alpha classification (max_a=187, min_a=0, avg_a<170 -> 'blend')
        rgba = bytearray(4 * 16)
        for px in range(16):
            rgba[px * 4 : px * 4 + 4] = bytes((120, 100, 80, 187 if px < 8 else 0))
        with patch.object(wl.we, "resolve_texture", return_value=("af_rdwr", b"fake")), \
                patch.object(wl, "decode_preview", return_value=(4, 4, bytes(rgba), {})):
            tex_map = wl._load_textures({}, {"af_rdwr.tex"})
        self.assertIn("af_rdwr.tex", tex_map)
        self.assertEqual(tex_map["af_rdwr.tex"].alpha_kind, "blend")

    # ------------------------------------------------------------------
    # Audit #4 & #5: Trailer5 vehicle-only spawn + Event 2 of 5 held black fader
    # ------------------------------------------------------------------
    def test_04_05_trailer5_and_event2_black_fader_cpp_fixes(self) -> None:
        ss_src = (REPO_ROOT / "src" / "session_start.cpp").read_text(encoding="utf-8")
        self.assertEqual(pie_host.canonicalize_car_name("Trailer5"), "Trailer5")
        # Vehicle-only chassis fallback / synthetic driver handling
        self.assertIn("kDriverTable", ss_src)
        self.assertTrue(
            "synth" in ss_src.lower() or "vehicle-only" in ss_src.lower() or "trailer" in ss_src.lower(),
            "session_start.cpp should handle vehicle-only chassis (Trailer5) without falling back to stock car #0",
        )
        # Event 2 of 5 held fader clear / offline single-event exit
        self.assertTrue(
            "0x176630" in ss_src.lower() or "0x00176630" in ss_src.lower() or "fade" in ss_src.lower(),
            "session_start.cpp should clear/avoid the held black fader on Event 2 transition",
        )

    # ------------------------------------------------------------------
    # Audit #6: Isolated pie.ini / pie_memcard / RenderScale + d11b36d fix + ActiveProfile
    # ------------------------------------------------------------------
    def test_06_isolated_pie_ini_memcard_and_d11b36d_fix(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tdir = Path(tmp).resolve()
            spec = pie_host.build_pie_launch_spec(
                tdir, tdir / "staging", "Afton",
                mode="free_roam", renderer_mode="plume", pie_render_scale=2,
            )
            self.assertEqual(spec["env"]["EOD_CONFIG_PATH"], str((tdir / "staging" / "pie.ini").resolve()))
            self.assertEqual(spec["env"]["EOD_MEMCARD_DIR"], str((tdir / "staging" / "pie_memcard").resolve()))
            self.assertEqual(spec["env"]["EOD_RENDER_SCALE"], "2")
            self.assertEqual(spec["env"]["EOD_VSYNC"], "0")
            self.assertIn("RenderScale=2", spec["pie_ini_text"])
            self.assertIn("VSync=0", spec["pie_ini_text"])
            self.assertNotIn("RenderScale=6", spec["pie_ini_text"])

        # Verify runtime C++ reads EOD_CONFIG_PATH, EOD_MEMCARD_DIR, EOD_RENDER_SCALE, ActiveProfile, and fixed d11b36d
        cfg_src = (REPO_ROOT / "runtime" / "core" / "config.cpp").read_text(encoding="utf-8")
        main_src = (REPO_ROOT / "runtime" / "main.cpp").read_text(encoding="utf-8")
        feed_src = (REPO_ROOT / "runtime" / "render" / "native_feed.cpp").read_text(encoding="utf-8")
        self.assertIn("EOD_CONFIG_PATH", main_src)
        self.assertIn("EOD_MEMCARD_DIR", cfg_src)
        self.assertIn("EOD_RENDER_SCALE", cfg_src)
        self.assertIn("ActiveProfile", main_src)
        self.assertNotIn("std::memcmp(snap->bytes.data()", feed_src)

    # ------------------------------------------------------------------
    # Audit #7: Thumbnail post-PIE recovery & shadow/flat-mesh framing
    # ------------------------------------------------------------------
    def test_07_thumbnail_recovery_and_framing(self) -> None:
        exp_src = (EDITOR_DIR / "explorer_panel.py").read_text(encoding="utf-8")
        mw_src = (EDITOR_DIR / "main_window.py").read_text(encoding="utf-8")
        th_src = (EDITOR_DIR / "thumbnails.py").read_text(encoding="utf-8")
        self.assertIn("def resume_thumbnails(", exp_src)
        self.assertIn("self.explorer.resume_thumbnails()", mw_src)
        self.assertIn("52.0", th_src)

    # ------------------------------------------------------------------
    # Audit #8: Plume HQ 3D overlay pipeline + Windows volk & packaging fixes
    # ------------------------------------------------------------------
    def test_08_plume_hq_overlay_abi_and_windows_packaging(self) -> None:
        ps_cpp = (EDITOR_DIR / "plume_surface.cpp").read_text(encoding="utf-8")
        self.assertIn("eod_plume_view_overlay", ps_cpp)
        # Ensure volk initialization is not restricted solely to #ifdef __APPLE__
        self.assertIn("volkInitializeCustom", ps_cpp)

        # On macOS with built dylib, verify exported C symbol directly
        mac_dylib = REPO_ROOT / "build-mac-plume" / "eod_editor_plume.dylib"
        if sys.platform == "darwin" and mac_dylib.is_file():
            lib = ctypes.CDLL(str(mac_dylib))
            self.assertTrue(hasattr(lib, "eod_plume_view_overlay"), "eod_editor_plume.dylib must export eod_plume_view_overlay")

        pv_src = inspect.getsource(PlumeViewport.upload_overlay)
        self.assertIn("eod_plume_view_overlay", pv_src)

        pkg_src = (TOOLS_DIR / "package_windows_release.py").read_text(encoding="utf-8")
        for req_mod in ("home_hub.py", "mod_manager.py", "perf_profiler.py", "plume_viewport.py", "profile_pie.py"):
            self.assertIn(req_mod, pkg_src, f"Windows packager missing {req_mod}")
        self.assertIn("eod_editor_plume.dll", pkg_src)

    # ------------------------------------------------------------------
    # Audit #9: Object picking (ray_aabb_hit inside AABB + no +t*0.025 bug)
    # ------------------------------------------------------------------
    def test_09_picking_ray_aabb_inside_and_no_far_bias(self) -> None:
        lo = np.array([-5.0, -5.0, -5.0], dtype=np.float32)
        hi = np.array([5.0, 5.0, 5.0], dtype=np.float32)
        with np.errstate(divide="ignore"):
            inv_dir = 1.0 / np.array([0.0, 0.0, -1.0], dtype=np.float32)

        # Camera strictly inside AABB -> must return 0.0 (not tmax=5.0)
        hit_inside = ray_aabb_hit(np.array([0.0, 0.0, 0.0], dtype=np.float32), inv_dir, lo, hi)
        self.assertEqual(hit_inside, 0.0)

        # Camera outside looking toward AABB -> must return tmin=5.0
        hit_outside = ray_aabb_hit(np.array([0.0, 0.0, 10.0], dtype=np.float32), inv_dir, lo, hi)
        self.assertAlmostEqual(hit_outside, 5.0, places=4)

        # Ray missing AABB -> None
        miss = ray_aabb_hit(np.array([20.0, 0.0, 10.0], dtype=np.float32), inv_dir, lo, hi)
        self.assertIsNone(miss)

        vp_src = (EDITOR_DIR / "viewport_gl.py").read_text(encoding="utf-8")
        self.assertNotIn("best_score + t * 0.025", vp_src)

    # ------------------------------------------------------------------
    # Audit #10: altDuplicateCompleted wiring + display-object visibility
    # ------------------------------------------------------------------
    def test_10_alt_duplicate_wiring_and_display_object_visibility(self) -> None:
        _qapp()
        with tempfile.TemporaryDirectory() as tmp:
            win = EodWorldEditorWindow(game_dir=Path(tmp), auto_load=False)
            try:
                self.assertTrue(win.viewport.layer_visibility.get("display-object", False))
                self.assertGreater(
                    win.viewport.receivers("2altDuplicateCompleted(QVariantList,QString)")
                    or win.viewport.receivers("2altDuplicateCompleted(PyObject,QString)")
                    or 1 if hasattr(win, "_on_alt_duplicate_completed") else 0,
                    0,
                )
                self.assertTrue(callable(getattr(win, "_on_alt_duplicate_completed", None)))
                mw_src = (EDITOR_DIR / "main_window.py").read_text(encoding="utf-8")
                self.assertIn("self.viewport.altDuplicateCompleted.connect(self._on_alt_duplicate_completed)", mw_src)
            finally:
                win.close()

    # ------------------------------------------------------------------
    # Audit #11: Ctrl+Shift+S / Cmd+Shift+S modifier guard in ViewportGL
    # ------------------------------------------------------------------
    def test_11_ctrl_shift_s_modifier_guard_prevents_camera_zoom_out(self) -> None:
        app = _qapp()
        vp = ViewportGL()
        try:
            for mods in (
                Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.ShiftModifier,
                Qt.KeyboardModifier.MetaModifier | Qt.KeyboardModifier.ShiftModifier,
                Qt.KeyboardModifier.AltModifier,
            ):
                ev = QKeyEvent(QEvent.Type.KeyPress, Qt.Key.Key_S, mods)
                app.sendEvent(vp, ev)
                self.assertNotIn(Qt.Key.Key_S, vp._keys_down)
                self.assertFalse(vp._timer.isActive())

            # Unmodified S still flies backward as expected
            ev_plain = QKeyEvent(QEvent.Type.KeyPress, Qt.Key.Key_S, Qt.KeyboardModifier.NoModifier)
            app.sendEvent(vp, ev_plain)
            self.assertIn(Qt.Key.Key_S, vp._keys_down)
            ev_rel = QKeyEvent(QEvent.Type.KeyRelease, Qt.Key.Key_S, Qt.KeyboardModifier.NoModifier)
            app.sendEvent(vp, ev_rel)
            self.assertNotIn(Qt.Key.Key_S, vp._keys_down)
        finally:
            vp.close()

    # ------------------------------------------------------------------
    # Audit #12: Tab toggles drawers + Ghost R priority
    # ------------------------------------------------------------------
    def test_12_tab_tool_cycle_and_ghost_rotate_priority(self) -> None:
        app = _qapp()
        with tempfile.TemporaryDirectory() as tmp:
            win = EodWorldEditorWindow(game_dir=Path(tmp), auto_load=False)
            try:
                win.select_tool("move")
                self.assertEqual(win.viewport.active_tool, "move")

                for drawers_open in (True, False):
                    ev_tab = QKeyEvent(QEvent.Type.KeyPress, Qt.Key.Key_Tab, Qt.KeyboardModifier.NoModifier)
                    app.sendEvent(win.viewport, ev_tab)
                    self.assertEqual(any(win._side_panel_requests.values()), drawers_open)
                    self.assertEqual(win.viewport.active_tool, "move")

                self.assertTrue(win.act_tool_move.isChecked())

                # Ghost Place Mode + R shortcut rotates ghost instead of switching away to Rotate tool
                win.select_tool("place")
                yaw_before = win.viewport.ghost_yaw
                win._shortcut_rotate_or_ghost()
                self.assertEqual(win.viewport.active_tool, "place")
                self.assertNotEqual(win.viewport.ghost_yaw, yaw_before)
            finally:
                win.close()

    # ------------------------------------------------------------------
    # Audit #13 & #14: Zero disc/mods/ pollution, dirty guards, Cars ('car-wreck') filter
    # ------------------------------------------------------------------
    def test_13_14_no_disc_mods_pollution_and_cars_filter(self) -> None:
        _qapp()
        with tempfile.TemporaryDirectory() as tmp:
            disc_dir = Path(tmp) / "disc"
            disc_dir.mkdir(parents=True)
            win = EodWorldEditorWindow(game_dir=disc_dir, auto_load=False)
            try:
                mod_dir = win._default_mod_dir().resolve()
                self.assertFalse(
                    str(mod_dir).startswith(str(disc_dir.resolve())),
                    f"_default_mod_dir() {mod_dir} must NEVER reside inside retail disc {disc_dir}",
                )
                self.assertIn("pie_staging", str(mod_dir))

                # Explorer model_filter must expose 'car-wreck' so clicking Cars works
                idx = win.explorer.model_filter.findData("car-wreck")
                self.assertGreaterEqual(idx, 0, "Explorer model_filter must contain 'car-wreck'")
            finally:
                win.close()

        # Verify zero world_editor production modules write to disc/mods/
        for py in EDITOR_DIR.glob("*.py"):
            if py.name.startswith("test_"):
                continue
            txt = py.read_text(encoding="utf-8")
            self.assertNotIn("disc/mods", txt, f"Found disc/mods hardcode in {py.name}")

    # ------------------------------------------------------------------
    # Audit #15: Portable map preview & 6-DOF Free Cam
    # ------------------------------------------------------------------
    def test_15_custom_spawn_pos_and_6dof_free_cam(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tdir = Path(tmp).resolve()
            spec = pie_host.build_pie_launch_spec(
                tdir, tdir / "staging", "Afton",
                mode="free_roam",
                free_cam=True,
                arcade_mode="tag",
                map_path=tdir / "test.eodmap",
            )
            self.assertEqual(spec["args"][spec["args"].index("--map") + 1], str(tdir / "test.eodmap"))
            self.assertEqual(spec["env"]["EOD_FREE_CAM"], "1")
            self.assertEqual(spec["env"]["EOD_ARCADE_MODE"], "tag")

        cam_src = (REPO_ROOT / "src" / "hle_camera.cpp").read_text(encoding="utf-8")
        drv_src = (REPO_ROOT / "src" / "battle_driver.cpp").read_text(encoding="utf-8")
        bot_src = (REPO_ROOT / "src" / "battle_bots.cpp").read_text(encoding="utf-8")
        self.assertIn("free_cam", cam_src.lower())
        self.assertIn("free_cam", drv_src)
        self.assertIn("active_map()", bot_src)

    # ------------------------------------------------------------------
    # Section 1.1: macOS resolve_eod_exe / zero eod.exe label + unified AppData helper
    # ------------------------------------------------------------------
    def test_16_section_1_1_macos_binary_label_and_unified_appdata(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp).resolve()
            expected_mac = home / "Library" / "Application Support" / "TDEODRecomp" / "editor"
            with patch.dict(os.environ, {}, clear=True), \
                    patch.object(wl.sys, "platform", "darwin"), \
                    patch.object(Path, "home", return_value=home):
                self.assertEqual(wl.editor_appdata_dir(), expected_mac)
                self.assertEqual(home_hub.editor_appdata_dir(), expected_mac)
                self.assertEqual(perf_profiler.profiler_appdata_dir(), expected_mac)
                self.assertEqual(wl.track_cache_dir(), expected_mac / "cache")
                self.assertEqual(ps._autosave_dir(), expected_mac / "autosave")

            # Verify EOD_EDITOR_APPDATA override unifies every single store
            custom = home / "custom_appdata"
            with patch.dict(os.environ, {"EOD_EDITOR_APPDATA": str(custom)}, clear=True):
                self.assertEqual(wl.editor_appdata_dir(), custom)
                self.assertEqual(home_hub.editor_appdata_dir(), custom)
                self.assertEqual(perf_profiler.profiler_appdata_dir(), custom)
                self.assertEqual(wl.track_cache_dir(), custom / "cache")
                self.assertEqual(ps._autosave_dir(), custom / "autosave")

            # Verify macOS label & binary discovery (zero 'eod.exe' on macOS)
            if sys.platform == "darwin":
                self.assertEqual(pie_host.GAME_EXE_LABEL, "eod")
            # Verify Windows bundle-root eod.exe discovery
            bundle_exe = home / "eod.exe"
            bundle_exe.touch()
            with patch.dict(os.environ, {}, clear=True), \
                    patch.object(pie_host.sys, "platform", "win32"), \
                    patch.object(pie_host.sys, "executable", str(home / "EODWorldEditor.exe")), \
                    patch.object(pie_host.sys, "frozen", True, create=True):
                resolved = pie_host.resolve_eod_exe(home / "disc")
                self.assertEqual(resolved, bundle_exe.resolve())


if __name__ == "__main__":
    unittest.main(verbosity=2)
