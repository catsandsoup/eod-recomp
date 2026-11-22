"""Main Window for EOD World Editor (Native Windows 11 Qt 6 + OpenGL 3.3 Core + Embedded PIE)."""

from __future__ import annotations

import json
import os
import re
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

import numpy as np
from PySide6.QtCore import QPoint, QSize, Qt, QTimer, QUrl
from PySide6.QtGui import QAction, QActionGroup, QDesktopServices, QFontDatabase, QKeySequence, QUndoStack
from PySide6.QtWidgets import (
    QApplication,
    QFileDialog,
    QFrame,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QMainWindow,
    QMenu,
    QMessageBox,
    QProgressBar,
    QPushButton,
    QSplitter,
    QStackedWidget,
    QStatusBar,
    QToolBar,
    QVBoxLayout,
    QWidget,
)

from dialogs import CustomRaceDialog, PackageModDialog, ReplaceModelDialog, VehicleTunerDialog
from explorer_panel import ExplorerPanel
from home_hub import (
    HomeHubDialog,
    record_recent_project,
    thumbnail_path_for_project,
)
from perf_profiler import get_profiler
from pie_host import (
    GAME_EXE_LABEL,
    PieHostWidget,
    build_pie_launch_spec,
    pie_staging_root,
    tdeod_support_dir,
)
from vehicle_overlay import RegisteredVehicle, build_vehicle_pie_layer
from project_state import (
    AddMarkerCommand,
    AddRoadNodeCommand,
    BatchReplaceTexturesCommand,
    DeleteMarkerCommand,
    DeleteObjectCommand,
    DeleteRoadNodeCommand,
    EditEnvironmentCommand,
    EditMarkerMetaCommand,
    JoinRoadNodesCommand,
    MakeCollisionCommand,
    ModifyBarrierCommand,
    ModifyRoadLanesCommand,
    MoveRoadNodeCommand,
    MultiDeleteObjectsCommand,
    MultiTransformObjectsCommand,
    PlaceObjectCommand,
    ReplaceObjectModelCommand,
    ReplaceTextureCommand,
    ScatterPlaceCommand,
    SetAllWallsNoclipCommand,
    SplitRoadEdgeCommand,
    TransformMarkerCommand,
    TransformObjectCommand,
    apply_project_overlay,
    autosave_path_for,
    build_project_overlay_dict,
    clear_autosave,
    export_all_track_textures,
    export_mod_layer,
    export_object_obj,
    find_newer_autosave,
    load_png_rgba,
    package_world_mod,
    plan_clamp_sky_walls_to_assets,
    rebuild_procedural_entry,
    save_autosave_snapshot,
    save_project_overlay,
    scan_import_texture_folder,
    snapshot_world_for_bake,
)
from properties_panel import PropertiesPanel
from road_builder import (
    BuiltRoad,
    RoadControlPoint,
    RoadPlan,
    build_road_mesh,
    make_banked_oval_plan,
    make_bridge_plan,
    make_figure8_plan,
    make_jump_ramp_plan,
)
from road_collision import open_eod_junction
from theme import (
    BORDER,
    ElidedLabel,
    TEXT_MUTED,
    TEXT_PRIMARY,
    TEXT_SECONDARY,
    enable_windows_dark_titlebar,
    svg_icon,
)
from viewport_gl import ViewportGL
from workers import CancelToken, FunctionWorker, start_worker
from world_loader import (
    EnvironmentState,
    RoadNode,
    TrackWorld,
    WorldMarker,
    WorldObject,
    default_game_dir,
    discover_cars,
    discover_tracks,
    load_car_catalog,
    load_track_world,
    merge_car_catalog_into_world,
)


class EodWorldEditorWindow(QMainWindow):
    """Commercial-grade Qt 6 Windows 11 World Editor for Test Drive: Eve of Destruction."""

    def __init__(
        self,
        game_dir: Path | None = None,
        initial_track: str | None = None,
        project_file: Path | None = None,
        auto_load: bool = True,
        show_home_hub: bool = False,
    ):
        super().__init__()
        self.profiler = get_profiler()
        t_init_ns = time.perf_counter_ns()
        self.game_dir = Path(game_dir) if game_dir else default_game_dir()
        self.project_name = "Untitled World"
        self.project_path: Path | None = None
        self._mod_output_dir: Path | None = None
        self._saved_undo_index: int = 0
        self._pie_render_scale: int = 2
        self._pending_pie_spawn_pos: tuple[float, float, float, float] | None = None
        self._custom_car_archives: list[Path] = []
        self.world: TrackWorld | None = None
        self.undo_stack = QUndoStack(self)
        self._placed_counter = 1
        self._marker_counter = 1
        self._last_pie_mode = "free_roam"
        self._last_pie_profile: tuple[str, Path] | None = None
        self._last_pie_vehicle: RegisteredVehicle | None = None
        self._profile_pie_overlay: tempfile.TemporaryDirectory | None = None
        self._vehicle_tuner_dlg: VehicleTunerDialog | None = None
        self._pending_event_template: str | None = None
        self._pending_vehicle_drive: tuple[Path, RegisteredVehicle] | None = None
        self._pending_quick_free_roam: bool = False

        # Silent crash-safe autosave state (60s timer, zero UI hitch)
        self._autosave_dirty = False
        self._autosave_gen = 0
        self._autosave_token: CancelToken | None = None
        self._pending_recovery_path: Path | None = None

        # Non-blocking background job state
        self._job_gen = 0
        self._active_token: CancelToken | None = None
        self._car_job_gen = 0
        self._car_token: CancelToken | None = None

        self.resize(1540, 930)
        self.setMinimumSize(800, 540)

        # Core panels: Left Explorer | Center (Autosave Banner + OpenGL Viewport / Embedded PIE) | Right Properties
        self.explorer = ExplorerPanel()
        self.viewport = ViewportGL()
        self.pie_host = PieHostWidget()
        self.center_stack = QStackedWidget()
        self.center_stack.addWidget(self.viewport)  # Page 0: OpenGL 3.3 Core Editor
        self.center_stack.addWidget(self.pie_host)  # Page 1: Embedded Vulkan eod.exe PIE

        # Non-intrusive inline autosave recovery banner (hidden until a newer autosave is detected)
        self.autosave_banner = QFrame()
        self.autosave_banner.setObjectName("AutosaveRecoveryBanner")
        self.autosave_banner.setStyleSheet(
            "QFrame#AutosaveRecoveryBanner { background: #1e293b; border-bottom: 1px solid #38bdf8; }"
            "QFrame#AutosaveRecoveryBanner QLabel { color: #e2e8f0; border: none; }"
        )
        ab_lay = QHBoxLayout(self.autosave_banner)
        ab_lay.setContentsMargins(10, 5, 10, 5)
        ab_lay.setSpacing(8)
        self.lbl_autosave_msg = QLabel("A newer crash-recovery autosave was found.")
        self.btn_recover_autosave = QPushButton("Recover Autosave")
        self.btn_recover_autosave.setFixedHeight(22)
        self.btn_recover_autosave.setStyleSheet(
            "QPushButton { background: #2563eb; color: white; border: none; border-radius: 4px; padding: 2px 10px; font-weight: 600; }"
            "QPushButton:hover { background: #3b82f6; }"
        )
        self.btn_recover_autosave.clicked.connect(self.recover_pending_autosave)
        self.btn_discard_autosave = QPushButton("Discard")
        self.btn_discard_autosave.setFixedHeight(22)
        self.btn_discard_autosave.clicked.connect(self.discard_pending_autosave)
        ab_lay.addWidget(self.lbl_autosave_msg, 1)
        ab_lay.addWidget(self.btn_recover_autosave)
        ab_lay.addWidget(self.btn_discard_autosave)
        self.autosave_banner.hide()

        self._center_container = QWidget()
        self._center_container.setMinimumWidth(460)
        c_lay = QVBoxLayout(self._center_container)
        c_lay.setContentsMargins(0, 0, 0, 0)
        c_lay.setSpacing(0)
        c_lay.addWidget(self.autosave_banner)
        c_lay.addWidget(self.center_stack, 1)

        self.properties = PropertiesPanel()

        self.splitter = QSplitter(Qt.Orientation.Horizontal)
        self.splitter.addWidget(self.explorer)
        self.splitter.addWidget(self._center_container)
        self.splitter.addWidget(self.properties)
        self.splitter.setStretchFactor(0, 0)
        self.splitter.setStretchFactor(1, 1)
        self.splitter.setStretchFactor(2, 0)
        self.splitter.setSizes([308, 902, 330])
        self.splitter.setChildrenCollapsible(False)
        self._side_panel_requests = {"explorer": True, "inspector": True}
        self._compact_panel = "explorer"
        self.setCentralWidget(self.splitter)

        self._build_menu_bar()
        self._build_toolbar()
        self._build_status_bar()
        self._connect_signals()
        self._update_window_chrome()
        self._sync_pie_pickers_from_world()

        # Silent 60-second crash-safe autosave timer (runs only when dirty)
        self._autosave_timer = QTimer(self)
        self._autosave_timer.setInterval(60_000)
        self._autosave_timer.timeout.connect(self.trigger_background_autosave)
        self._autosave_timer.start()
        self.undo_stack.indexChanged.connect(self._on_undo_index_changed)

        enable_windows_dark_titlebar(self)

        # Discover retail .TRK files
        self.available_tracks = discover_tracks(self.game_dir)
        self.explorer.set_available_tracks(self.available_tracks)

        # Finish high-res app_startup phase timer
        if "app_startup" in self.profiler._active_starts:
            startup_ms = self.profiler.end_phase("app_startup")
        else:
            startup_ms = (time.perf_counter_ns() - t_init_ns) / 1_000_000.0
            self.profiler.record_phase("app_startup", startup_ms)
        self.append_log(f"[Perf] Editor UI ready in {startup_ms:.1f} ms", verbose=True)

        if auto_load:
            if project_file and Path(project_file).is_file():
                self.open_project_file(Path(project_file))
            elif show_home_hub:
                QTimer.singleShot(0, self.open_home_hub)
            elif self.available_tracks:
                target = self.available_tracks[0]
                if initial_track:
                    want = initial_track.upper().removesuffix(".TRK")
                    for tp in self.available_tracks:
                        if tp.stem.upper() == want:
                            target = tp
                            break
                self.load_track_async(target)
            else:
                self.set_action_status(
                    f"No .TRK files found in {self.game_dir / 'DATA' / 'TRACKS'} — use File -> Set Game Data Directory"
                )

    def showEvent(self, event) -> None:
        super().showEvent(event)
        enable_windows_dark_titlebar(self)

    def _is_dirty(self) -> bool:
        return bool(self.world is not None and (self.undo_stack.index() != self._saved_undo_index or self._autosave_dirty))

    def _mark_saved(self) -> None:
        self._saved_undo_index = self.undo_stack.index()
        self._autosave_dirty = False
        if hasattr(self.properties, "set_project_dirty"):
            self.properties.set_project_dirty(False)
        self._update_window_chrome()

    def _confirm_discard_changes(self, action_label: str = "continue") -> bool:
        """Guard destructive actions against silent data loss (Save / Discard / Cancel)."""
        if not self._is_dirty():
            return True
        if os.environ.get("EOD_EDITOR_NO_PROMPTS") == "1" or not self.isVisible():
            return True
        resp = QMessageBox.question(
            self,
            "Unsaved Changes",
            f"Project '{self.project_name}' has unsaved changes.\nSave before you {action_label}?",
            QMessageBox.StandardButton.Save
            | QMessageBox.StandardButton.Discard
            | QMessageBox.StandardButton.Cancel,
            QMessageBox.StandardButton.Save,
        )
        if resp == QMessageBox.StandardButton.Cancel:
            return False
        if resp == QMessageBox.StandardButton.Save:
            self.save_project()
            return not self._is_dirty()
        return True

    def closeEvent(self, event) -> None:
        if not self._confirm_discard_changes("close the editor"):
            event.ignore()
            return
        if hasattr(self, "_autosave_timer"):
            self._autosave_timer.stop()
        if self._autosave_token is not None:
            self._autosave_token.cancel()
        self._cancel_active_job()
        if self._car_token is not None:
            self._car_token.cancel()
        self.explorer._cancel_thumb_worker()
        self.pie_host.stop_pie(emit_signal=False)
        self._cleanup_profile_pie_overlay()
        super().closeEvent(event)

    # -----------------------------------------------------------------------
    # Window Title & Right-Aligned World Badge
    # -----------------------------------------------------------------------
    def _update_window_chrome(self) -> None:
        trk = self.world.track_name if self.world else "NO TRACK"
        dirty_mark = " *" if self._is_dirty() else ""
        self._sync_walls_noclip_action()
        self.act_package_mod.setEnabled(self.world is not None)
        self.setWindowTitle(
            f"EOD World Editor \u2013 Test Drive: Eve of Destruction \u2013 {trk} [{self.project_name}{dirty_mark}]"
        )
        self.world_badge.setText(f"World: {self.project_name}{dirty_mark}")
        if self.world:
            self.lbl_zone.setText(f"Track: {self.world.display_name} ({self.world.track_name})")

    # -----------------------------------------------------------------------
    # Full 14-Item Menu Bar
    # -----------------------------------------------------------------------
    def _build_menu_bar(self) -> None:
        mb = self.menuBar()

        self.world_badge = QLabel(f"World: {self.project_name}")
        self.world_badge.setProperty("badge", "world")
        mb.setCornerWidget(self.world_badge, Qt.Corner.TopRightCorner)

        # 1. File
        m_file = mb.addMenu("&File")
        self.act_home_hub = m_file.addAction(svg_icon("new_world", "#60a5fa"), "Home Hub...", self.open_home_hub)
        m_file.addSeparator()
        self.act_new = m_file.addAction(svg_icon("new_world"), "New World Project...", self.new_project, QKeySequence.StandardKey.New)
        self.act_open = m_file.addAction(svg_icon("open_world"), "Open Project...", self.open_project_dialog, QKeySequence.StandardKey.Open)
        self.act_save = m_file.addAction(svg_icon("save"), "Save Project", self.save_project, QKeySequence.StandardKey.Save)
        m_file.addAction("Save Project As...", self.save_project_as)
        m_file.addSeparator()
        self.act_export_mod = m_file.addAction(
            svg_icon("save", "#2ecc71"), "Export Mod Layer...", self.write_mod_layer_dialog, QKeySequence("Ctrl+Shift+S")
        )
        m_file.addAction("Export Mod Layer As...", lambda: self.write_mod_layer_dialog(prompt_dir=True))
        self.act_package_mod = m_file.addAction(
            "Package Mod as .eodmod...", self.package_mod_dialog, QKeySequence("Ctrl+Shift+E")
        )
        m_file.addAction("Mod Manager && Profiles...", self.open_mod_manager)
        m_file.addSeparator()
        m_file.addAction("Set Game Data Directory...", self.choose_game_dir)
        m_file.addSeparator()
        m_file.addAction("Quit" if sys.platform == "darwin" else "Exit", self.close, QKeySequence.StandardKey.Quit)

        # 2. Edit (industry-standard Undo / Redo / Duplicate / Delete with macOS Backspace support)
        m_edit = mb.addMenu("&Edit")
        act_undo = self.undo_stack.createUndoAction(self, "Undo")
        act_undo.setShortcut(QKeySequence.StandardKey.Undo)
        act_undo.setIcon(svg_icon("undo"))
        act_redo = self.undo_stack.createRedoAction(self, "Redo")
        act_redo.setShortcut(QKeySequence.StandardKey.Redo)
        act_redo.setIcon(svg_icon("redo"))
        m_edit.addAction(act_undo)
        m_edit.addAction(act_redo)
        m_edit.addSeparator()
        m_edit.addAction("Duplicate Selected", self.duplicate_selected_object, QKeySequence("Ctrl+D"))
        self.act_delete_selected = m_edit.addAction(
            svg_icon("trash", "#ff99a2"), "Delete Selected", self.delete_selected_item
        )
        self.act_delete_selected.setShortcuts(
            [QKeySequence.StandardKey.Delete, QKeySequence(Qt.Key.Key_Backspace)]
        )
        m_edit.addSeparator()
        m_edit.addAction("Select All Similar", self.select_similar_objects)

        # 3. World
        m_world = mb.addMenu("&World")
        m_world.addAction(svg_icon("sky", "#fbbf24"), "Environment / Skybox...", self.open_environment_panel)
        m_world.addAction("Reload Track from Retail Disc", self.reload_current_track)
        m_world.addAction("Open DATA/TRACKS Folder", self.open_tracks_folder)
        m_world.addSeparator()
        m_world.addAction(svg_icon("launch", "#2ecc71"), "Play Free Roam (PIE)", lambda: self.start_pie("free_roam"), QKeySequence("F5"))
        m_world.addAction(
            svg_icon("launch", "#38bdf8"),
            "Play From Camera (PIE)",
            self.play_from_camera,
            QKeySequence("Ctrl+Shift+F5"),
        )
        m_world.addAction(svg_icon("race", "#60a5fa"), "Play Event / Race (PIE)", lambda: self.start_pie("event"), QKeySequence("Shift+F5"))
        m_world.addAction(svg_icon("stop", "#ff99a2"), "Stop PIE", self.stop_pie)

        # 3. View
        m_view = mb.addMenu("&View")
        self._view_menu = m_view
        self.act_panel_explorer = self._add_toggle_action(
            m_view, "Explorer", True, lambda show: self._request_side_panel("explorer", show))
        self.act_panel_inspector = self._add_toggle_action(
            m_view, "Inspector", True, lambda show: self._request_side_panel("inspector", show))
        self.act_panel_explorer.setIcon(svg_icon("tree"))
        self.act_panel_inspector.setIcon(svg_icon("layers"))
        self.act_panel_explorer.setToolTip("Show or hide Explorer")
        self.act_panel_inspector.setToolTip("Show or hide Inspector")
        m_view.addSeparator()
        self.act_v_wire = self._add_toggle_action(m_view, "Wireframe", False, self._toggle_wireframe)
        self.act_v_col = self._add_toggle_action(m_view, "Collision Overlay (SCOL + SOBL)", False, self._toggle_collision)
        self.act_v_markers = self._add_toggle_action(m_view, "Markers & Start Grid", True, self._toggle_markers)
        self.act_v_light = self._add_toggle_action(m_view, "Lighting", True, self._toggle_lighting)
        self.act_v_labels = self._add_toggle_action(m_view, "Model Labels", False, self._toggle_labels)
        self.act_v_roads = self._add_toggle_action(m_view, "Racing Lines (ILIN)", True, self._toggle_roads)
        self.act_v_grid = self._add_toggle_action(m_view, "Grid", True, self._toggle_grid)
        m_view.addSeparator()
        self.act_v_perf_hud = self._add_toggle_action(
            m_view, "Performance Profiler / HUD", False, self._toggle_perf_hud
        )
        self.act_v_perf_hud.setShortcut(QKeySequence("F3"))

        # 4. Camera
        m_cam = mb.addMenu("&Camera")
        m_cam.addAction("Frame Selection", self.viewport.frame_selection, QKeySequence("F"))
        m_cam.addAction("Reset Track Overview", self._reset_camera_overview)
        m_cam.addAction("Top-Down Map View", self._camera_top_down)
        m_cam.addSeparator()
        m_cam.addAction("Fly Speed: Slow (15)", lambda: self._set_fly_speed(15.0))
        m_cam.addAction("Fly Speed: Normal (45)", lambda: self._set_fly_speed(45.0))
        m_cam.addAction("Fly Speed: Fast (120)", lambda: self._set_fly_speed(120.0))

        # 5. Objects
        m_obj = mb.addMenu("&Objects")
        m_obj.addAction(svg_icon("move"), "Move Tool", lambda: self.select_tool("move"), QKeySequence("G"))
        m_obj.addAction(svg_icon("rotate"), "Rotate Tool (or Rotate Ghost)", self._shortcut_rotate_or_ghost, QKeySequence("R"))
        m_obj.addAction(svg_icon("scale"), "Scale Tool", lambda: self.select_tool("scale"), QKeySequence("T"))
        m_obj.addAction("Cycle Move / Rotate / Scale (Tab)", self.viewport.cycle_transform_tool)
        m_obj.addAction(svg_icon("place"), "Place Mode (Cursor Preview)", lambda: self.select_tool("place"), QKeySequence("P"))
        m_obj.addAction(svg_icon("scatter"), "Scatter / Paint Brush", lambda: self.select_tool("scatter"), QKeySequence("B"))
        m_obj.addSeparator()
        m_obj.addAction(svg_icon("replace", "#93c5fd"), "Replace Selection With...", self.replace_selected_with_dialog)
        m_obj.addAction("Select All Similar", self.select_similar_objects)
        m_obj.addAction(svg_icon("trash", "#ff99a2"), "Delete All Similar", self.delete_similar_objects)
        m_obj.addAction(svg_icon("ground"), "Snap Selected to Ground", self.snap_selected_to_ground, QKeySequence("Ctrl+G"))
        m_obj.addAction("Duplicate Selected", self.duplicate_selected_object)
        m_obj.addAction(svg_icon("blender"), "Export Selected to Blender (OBJ)...", self.edit_selected_in_blender, QKeySequence("F2"))
        m_obj.addAction(svg_icon("shield"), "Make Collision for Selected", self.make_collision_for_selected)
        m_obj.addSeparator()
        m_obj.addAction(self.act_delete_selected)

        # 6. Markers
        m_mk = mb.addMenu("&Markers")
        m_mk.addAction(svg_icon("marker"), "Place Marker at View Target", lambda: self.add_marker_at_camera("Marker"))
        m_mk.addAction("Place Start Grid Slot", lambda: self.add_marker_at_camera("Start Grid", prefix="StartGrid"))
        m_mk.addAction("Place Spawn Point", lambda: self.add_marker_at_camera("Spawn"))
        m_mk.addAction("Place Chicken Marker", lambda: self.add_marker_at_camera("Chicken"))
        m_mk.addAction("Place Event Trigger", lambda: self.add_marker_at_camera("Trigger"))
        m_mk.addAction("Toggle Markers Visibility", lambda: self.act_v_markers.trigger())

        # 7. Cars
        m_cars = mb.addMenu("Ca&rs")
        m_cars.addAction(svg_icon("car", "#ff8a8a"), "Browse Cars && Wrecks Library (DATA/CAR)", lambda: self._filter_explorer_category("car-wreck"))
        m_cars.addAction(svg_icon("car", "#fbbf24"), "Car Studio && NASCAR Importer...", self.open_vehicle_tuner_dialog)
        m_cars.addSeparator()
        m_cars.addAction(svg_icon("car"), "Place Car Spawn at View Target", lambda: self.add_marker_at_camera("Car"))
        m_cars.addAction("Place AI Opponent Car Slot", lambda: self.add_marker_at_camera("Car", prefix="AI_Car"))

        # 8. Breakers
        m_brk = mb.addMenu("&Breakers")
        m_brk.addAction("Place Destructible Breaker Marker", lambda: self.add_marker_at_camera("Breaker"))
        m_brk.addAction("Filter Models: Dyno / Props", lambda: self._filter_explorer_category("dyno"))

        # 9. Roads
        m_roads = mb.addMenu("R&oads")
        m_roads.addAction(svg_icon("road"), "Toggle Road Tool (ILIN)", lambda: self.select_tool("road"))
        m_roads.addAction("End Current Road Chain", self.viewport.end_current_road_chain)
        m_roads.addAction("Show / Hide Racing Lines (ILIN)", lambda: self.act_v_roads.trigger())
        m_roads.addSeparator()
        m_roads.addAction(svg_icon("road", "#4ade80"), "Build 3D Banked Oval Here", lambda: self.build_procedural_road_preset("oval"))
        m_roads.addAction(svg_icon("road", "#60a5fa"), "Build 3D Figure-8 Track Here", lambda: self.build_procedural_road_preset("figure8"))
        m_roads.addAction(svg_icon("road", "#fbbf24"), "Build 3D Jump Ramp Here", lambda: self.build_procedural_road_preset("ramp"))
        m_roads.addAction(svg_icon("road", "#a78bfa"), "Build 3D Highway Bridge Here", lambda: self.build_procedural_road_preset("bridge"))
        m_roads.addAction(svg_icon("road", "#38bdf8"), "Extrude 3D Road from Active ILIN Spline", self.extrude_road_from_active_ilin)
        m_roads.addAction(svg_icon("road", "#34d399"), "Build Organic Terrain Island Here", self.build_procedural_island)

        # 10. Terrain
        m_terr = mb.addMenu("&Terrain")
        m_terr.addAction("Toggle Static Terrain Layer", lambda: self._toggle_layer_by_key("static"))
        m_terr.addAction("Toggle Optional Terrain Layer", lambda: self._toggle_layer_by_key("optional-terrain"))
        m_terr.addAction("Sample Ground Height Under Camera", self._sample_ground_under_camera)

        # 11. Mesh
        m_mesh = mb.addMenu("&Mesh")
        m_mesh.addAction("Toggle Wireframe Overlay", lambda: self.act_v_wire.trigger())
        m_mesh.addAction("Export Selected Object to OBJ...", self.edit_selected_in_blender)
        m_mesh.addAction("Export All Track Textures to Folder...", self.export_texture_folder_dialog)
        m_mesh.addAction("Import Texture Folder (HOSTTEX)...", self.import_texture_folder_dialog)
        m_mesh.addAction("Show Track Mesh Statistics...", self._show_mesh_stats)

        # 12. Races
        m_races = mb.addMenu("Ra&ces")
        m_races.addAction(svg_icon("race", "#60a5fa"), "Custom Race && AI Pathfinding (ILIN) Editor...", self.open_custom_race_dialog)
        self.act_sync_events = QAction(svg_icon("race", "#4ade80"), "Sync All Events", self)
        self.act_sync_events.setCheckable(True)
        self.act_sync_events.setChecked(True)
        self.act_sync_events.setToolTip(
            "When ON, modded optional-terrain ramps & props sync across all events and Free Roam (opt_terrain kind=0 + EOD_OPT_TERRAIN_MASK=0xFFFF)"
        )
        self.act_sync_events.toggled.connect(self._toggle_sync_all_events)
        m_races.addAction(self.act_sync_events)
        m_races.addSeparator()
        m_races.addAction("Jump Camera to Start / Finish Grid", self._jump_to_start_grid)
        m_races.addAction("Show Track ILIN Racing Lines Summary...", self._show_races_summary)

        # 13. Collision
        m_col = mb.addMenu("C&ollision")
        m_col.addAction(svg_icon("shield"), "Toggle Collision Wireframe (SCOL + SOBL)", lambda: self.act_v_col.trigger())
        m_col.addAction("Clamp Sky-Walls to Asset Height", self.clamp_sky_walls_to_assets)
        self.act_walls_noclip = QAction("Walls Noclip (Keep Ground Solid)", self)
        self.act_walls_noclip.setCheckable(True)
        self.act_walls_noclip.setToolTip("Disable vertical SOBL barriers in PIE and exported mods")
        self.act_walls_noclip.triggered.connect(self._set_walls_noclip)
        m_col.addAction(self.act_walls_noclip)
        m_col.addAction("Inspect Tallest Barriers...", self._show_tallest_barriers)
        m_col.addSeparator()
        m_col.addAction("Make Collision Volume for Selected", self.make_collision_for_selected)
        m_col.addAction("Show Collision Statistics...", self._show_collision_stats)

        # 14. Help
        m_help = mb.addMenu("&Help")
        m_help.addAction("Controls && Keybindings...", self._show_controls_dialog)
        m_help.addAction("About EOD World Editor...", self._show_about_dialog)

    def _add_toggle_action(self, menu, label: str, initial: bool, slot) -> QAction:
        act = QAction(label, self)
        act.setCheckable(True)
        act.setChecked(initial)
        act.toggled.connect(slot)
        menu.addAction(act)
        return act

    def _request_side_panel(self, panel: str, show: bool) -> None:
        self._side_panel_requests[panel] = show
        if show:
            self._compact_panel = panel
        self._fit_side_panels()

    def _fit_side_panels(self) -> None:
        panels = (("explorer", self.explorer, self.act_panel_explorer),
                  ("inspector", self.properties, self.act_panel_inspector))
        minimum = (self.explorer.minimumWidth() + self.properties.minimumWidth()
                   + self._center_container.minimumWidth() + 2 * self.splitter.handleWidth())
        visible = self._side_panel_requests.copy()
        compact = self.width() < minimum
        if compact and all(visible.values()):
            visible = {panel: panel == self._compact_panel for panel in visible}
        if hasattr(self, "main_toolbar"):
            style = (Qt.ToolButtonStyle.ToolButtonIconOnly if self.width() < 1400
                     else Qt.ToolButtonStyle.ToolButtonTextBesideIcon)
            self.main_toolbar.setToolButtonStyle(style)
        if hasattr(self, "_lbl_pie_event"):
            compact_tb = self.width() < 1100
            self._lbl_pie_event.setText("Ev" if compact_tb else "Event")
            self._lbl_pie_car.setText("Car" if compact_tb else "Vehicle")
            self._lbl_pie_renderer.setText("GPU" if compact_tb else "Renderer")
            self._lbl_pie_scale.setText("Res" if compact_tb else "PIE Scale")
            self.cmb_pie_event.setMaximumWidth(120 if compact_tb else 240)
            self.cmb_pie_car.setMaximumWidth(86 if compact_tb else 160)
            self.cmb_renderer_mode.setMaximumWidth(82 if compact_tb else 130)
            self.cmb_pie_scale.setMaximumWidth(56 if compact_tb else 110)
            self.spn_pie_ai.setMaximumWidth(58 if compact_tb else 80)
        for panel, widget, action in panels:
            widget.setVisible(visible[panel])
            blocked = action.blockSignals(True)
            action.setChecked(visible[panel])
            action.blockSignals(blocked)

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        if hasattr(self, "act_panel_inspector"):
            self._fit_side_panels()

    # -----------------------------------------------------------------------
    # Commercial Toolbar (UE PIE Group + Lumion/Twinmotion Tools + Snap)
    # -----------------------------------------------------------------------
    def _build_toolbar(self) -> None:
        tb = QToolBar("Main Toolbar")
        self.main_toolbar = tb
        tb.setMovable(False)
        tb.setIconSize(QSize(15, 15))
        tb.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonTextBesideIcon)
        self.addToolBar(tb)
        self.addToolBarBreak()
        settings = QToolBar("Play In Editor Settings")
        self.settings_toolbar = settings
        settings.setMovable(False)
        settings.setIconSize(QSize(15, 15))
        self.addToolBar(settings)

        tb.addAction(svg_icon("new_world"), "New", self.new_project)
        tb.addAction(svg_icon("open_world"), "Open", self.open_project_dialog)
        tb.addAction(svg_icon("save"), "Save", self.save_project)
        tb.addAction(svg_icon("save", "#2ecc71"), "Write Mod", self.write_mod_layer_dialog)
        tb.addSeparator()

        # Unreal Engine 4/5 Style Play-In-Editor (PIE) Group + Instant On-Demand Mode/Car/AI Pickers
        self.act_pie_roam = tb.addAction(svg_icon("launch", "#2ecc71"), "Free Roam", lambda: self.start_pie("free_roam"))
        self.act_pie_roam.setToolTip("Compile mod layer and launch Open-Track Free Roam (Battle, Solo/AI) directly inside the viewport (F5)")

        self.act_pie_from_cam = tb.addAction(svg_icon("launch", "#38bdf8"), "Play Here", self.play_from_camera)
        self.act_pie_from_cam.setToolTip(
            "Play From Camera: spawn directly at your current 3D editor camera position (Ctrl/Cmd+Shift+F5)"
        )

        self.act_pie_event = tb.addAction(svg_icon("race", "#60a5fa"), "Play Event", lambda: self.start_pie("event"))
        self.act_pie_event.setToolTip("Compile mod layer and launch the selected Event / Race & Car directly inside the viewport (Shift+F5)")

        self.act_pie_stop = tb.addAction(svg_icon("stop", "#ff7b86"), "Stop", self.stop_pie)
        self.act_pie_stop.setEnabled(False)
        self.act_pie_stop.setToolTip("Stop Play-In-Editor and return to 3D Editor (Esc)")

        from PySide6.QtWidgets import QComboBox, QSpinBox
        self.cmb_pie_event = QComboBox()
        self.cmb_pie_event.setToolTip(
            "Event Template / Race Mode — live-filters viewport ramps & props, switches active ILIN line, and targets PIE"
        )
        self.cmb_pie_event.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
        self.cmb_pie_event.setMinimumContentsLength(10)
        self.cmb_pie_event.setMinimumWidth(110)
        self.cmb_pie_event.addItems(
            ["All Templates (Combined)", "Battle (Free Roam)", "No-Rules Race", "Demolition Derby", "Career Overworld (Free Roam)"]
        )
        self.cmb_pie_event.currentTextChanged.connect(self._on_pie_picker_changed)
        self._lbl_pie_event = QLabel("Event")
        settings.addWidget(self._lbl_pie_event)
        settings.addWidget(self.cmb_pie_event)

        self.cmb_pie_car = QComboBox()
        self.cmb_pie_car.setEditable(True)
        self.cmb_pie_car.setInsertPolicy(QComboBox.InsertPolicy.NoInsert)
        self.cmb_pie_car.setToolTip("Choose any retail vehicle to spawn in (type to filter)")
        self.cmb_pie_car.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
        self.cmb_pie_car.setMinimumContentsLength(7)
        self.cmb_pie_car.setMinimumWidth(80)
        car_names = [p.stem.replace("_1", "").title() for p in discover_cars(self.game_dir)]
        self.cmb_pie_car.addItems(car_names or ["Algar", "Bus", "Copcar", "Taxi", "Detonator"])
        idx_algar = self.cmb_pie_car.findText("Algar", Qt.MatchFlag.MatchFixedString)
        if idx_algar >= 0:
            self.cmb_pie_car.setCurrentIndex(idx_algar)
        self.cmb_pie_car.currentTextChanged.connect(self._on_pie_picker_changed)
        self._lbl_pie_car = QLabel("Vehicle")
        settings.addWidget(self._lbl_pie_car)
        settings.addWidget(self.cmb_pie_car)

        self.spn_pie_ai = QSpinBox()
        self.spn_pie_ai.setRange(0, 12)
        self.spn_pie_ai.setValue(0)
        self.spn_pie_ai.setPrefix("AI: ")
        self.spn_pie_ai.setToolTip("AI opponent count (0 = solo open-track Free Roam)")
        self.spn_pie_ai.valueChanged.connect(self._on_pie_picker_changed)
        settings.addWidget(self.spn_pie_ai)

        tb.addAction(self.act_sync_events)
        tb.addAction(svg_icon("race", "#fbbf24"), "Race Setup", self.open_custom_race_dialog)
        self.act_vehicle_tuner = tb.addAction(svg_icon("car", "#fbbf24"), "Car Tuner", self.open_vehicle_tuner_dialog)
        self.act_vehicle_tuner.setToolTip("Open Live PIE Vehicle Boost / Handling Tuner & NASCAR / Custom Car Tools")

        # Renderer Quality Selector (PS2 Fast Performance vs Plume Vulkan High-Quality)
        self.cmb_renderer_mode = QComboBox()
        self.cmb_renderer_mode.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
        self.cmb_renderer_mode.setMinimumContentsLength(8)
        self.cmb_renderer_mode.addItem("PS2 Fast", "ps2")
        self.cmb_renderer_mode.addItem("Plume HQ", "plume")
        self.cmb_renderer_mode.setToolTip(
            "Renderer Quality for Viewport & Play-In-Editor (PIE):\n"
            "• PS2 Fast: Low-power PS2-style preview + EOD_NATIVE=0, EOD_MODERN=0\n"
            "• Plume HQ: Enhanced shading preview and native Plume Modern in PIE (Vulkan required)"
        )
        self.cmb_renderer_mode.currentIndexChanged.connect(self._on_renderer_mode_changed)
        settings.addSeparator()
        self._lbl_pie_renderer = QLabel("Renderer")
        settings.addWidget(self._lbl_pie_renderer)
        settings.addWidget(self.cmb_renderer_mode)

        self.cmb_pie_scale = QComboBox()
        self.cmb_pie_scale.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
        self.cmb_pie_scale.setMinimumContentsLength(3)
        self.cmb_pie_scale.addItem("1x", 1)
        self.cmb_pie_scale.addItem("2x", 2)
        self.cmb_pie_scale.addItem("3x", 3)
        self.cmb_pie_scale.addItem("4x", 4)
        self.cmb_pie_scale.setCurrentIndex(1)  # Default 2x HD instead of inheriting RenderScale=6 (>4K)!
        self.cmb_pie_scale.setToolTip(
            "Isolated PIE internal resolution scale: 1x (PS2), 2x (HD), 3x (1440p), 4x (4K) — never overwrites your global eod.ini"
        )
        self.cmb_pie_scale.currentIndexChanged.connect(
            lambda _i: setattr(self, "_pie_render_scale", int(self.cmb_pie_scale.currentData() or 2))
        )
        self._lbl_pie_scale = QLabel("PIE Scale")
        settings.addWidget(self._lbl_pie_scale)
        settings.addWidget(self.cmb_pie_scale)
        tb.addSeparator()

        # Transform & Lumion/Twinmotion Placement Tools
        self.tool_group = QActionGroup(self)
        self.tool_group.setExclusive(True)

        self.act_tool_move = QAction(svg_icon("move"), "Move", self)
        self.act_tool_move.setCheckable(True)
        self.act_tool_move.setChecked(True)
        self.act_tool_move.setToolTip("Move tool (G)")
        self.act_tool_move.triggered.connect(lambda: self.select_tool("move"))

        self.act_tool_rotate = QAction(svg_icon("rotate"), "Rotate", self)
        self.act_tool_rotate.setCheckable(True)
        self.act_tool_rotate.setToolTip("Rotate tool (R)")
        self.act_tool_rotate.triggered.connect(lambda: self.select_tool("rotate"))

        self.act_tool_scale = QAction(svg_icon("scale"), "Scale", self)
        self.act_tool_scale.setCheckable(True)
        self.act_tool_scale.setToolTip("Scale tool (T, or Tab to cycle Move/Rotate/Scale)")
        self.act_tool_scale.triggered.connect(lambda: self.select_tool("scale"))

        self.act_tool_place = QAction(svg_icon("place", "#67e8f9"), "Place", self)
        self.act_tool_place.setCheckable(True)
        self.act_tool_place.setToolTip("Ghost Placement Preview snapped to SCOL terrain + normal (P)")
        self.act_tool_place.triggered.connect(lambda: self.select_tool("place"))

        self.act_tool_scatter = QAction(svg_icon("scatter", "#4ade80"), "Scatter", self)
        self.act_tool_scatter.setCheckable(True)
        self.act_tool_scatter.setToolTip("Lumion/Twinmotion Scatter & Paint Brush (B)")
        self.act_tool_scatter.triggered.connect(lambda: self.select_tool("scatter"))

        self.act_tool_road = QAction(svg_icon("road"), "Road", self)
        self.act_tool_road.setCheckable(True)
        self.act_tool_road.setToolTip("ILIN Road / Racing Line tool")
        self.act_tool_road.triggered.connect(lambda: self.select_tool("road"))

        for act in (
            self.act_tool_move,
            self.act_tool_rotate,
            self.act_tool_scale,
            self.act_tool_place,
            self.act_tool_scatter,
            self.act_tool_road,
        ):
            self.tool_group.addAction(act)
            tb.addAction(act)

        # Grid & Angle Snap toggles + Environment / Skybox
        settings.addSeparator()
        self.act_grid_snap = QAction(svg_icon("snap_grid"), "Snap 1m", self)
        self.act_grid_snap.setCheckable(True)
        self.act_grid_snap.setToolTip("Toggle 1.0 m Grid Snap for Move & Ghost Placement")
        self.act_grid_snap.toggled.connect(self._toggle_grid_snap)
        settings.addAction(self.act_grid_snap)

        self.act_angle_snap = QAction(svg_icon("snap_angle"), "Snap 15°", self)
        self.act_angle_snap.setCheckable(True)
        self.act_angle_snap.setToolTip("Toggle 15° Angle Snap for Rotate & Ghost Placement")
        self.act_angle_snap.toggled.connect(self._toggle_angle_snap)
        settings.addAction(self.act_angle_snap)

        settings.addAction(svg_icon("sky", "#fbbf24"), "Env/Sky", self.open_environment_panel)
        settings.addSeparator()
        settings.addAction(self.act_panel_explorer)
        settings.addAction(self.act_panel_inspector)
        self._view_menu.addSeparator()
        self._view_menu.addAction(self.act_grid_snap)
        self._view_menu.addAction(self.act_angle_snap)

    # -----------------------------------------------------------------------
    # Bottom Status Bar (with Non-Blocking Background Progress + Cancel)
    # -----------------------------------------------------------------------
    def _build_status_bar(self) -> None:
        sb = QStatusBar()
        self.setStatusBar(sb)

        self.lbl_status = ElidedLabel("Ready")
        self.lbl_status.setContentsMargins(6, 0, 6, 0)
        self.lbl_status.setStyleSheet(f"color: {TEXT_PRIMARY};")
        sb.addWidget(self.lbl_status, 2)

        self.progress_bar = QProgressBar()
        self.progress_bar.setRange(0, 100)
        self.progress_bar.setFixedWidth(130)
        self.progress_bar.setFixedHeight(14)
        self.progress_bar.setTextVisible(False)
        self.progress_bar.hide()
        sb.addPermanentWidget(self.progress_bar)

        self.btn_cancel_job = QPushButton("Cancel")
        self.btn_cancel_job.setFixedHeight(18)
        self.btn_cancel_job.setStyleSheet("font-size: 7.5pt; padding: 0px 6px;")
        self.btn_cancel_job.clicked.connect(self._cancel_active_job)
        self.btn_cancel_job.hide()
        sb.addPermanentWidget(self.btn_cancel_job)

        def perm_lbl(text: str, color: str = TEXT_SECONDARY) -> QLabel:
            lbl = ElidedLabel(text)
            lbl.setContentsMargins(8, 0, 8, 0)
            lbl.setStyleSheet(f"color: {color}; border-left: 1px solid {BORDER};")
            sb.addPermanentWidget(lbl, 1)
            return lbl

        self.lbl_zone = perm_lbl("Track: —", TEXT_PRIMARY)
        self.lbl_cam = perm_lbl("Cam 0.0, 0.0, 0.0  speed 45")
        self.lbl_prof = perm_lbl("mlists 0  objs 0  inst 0  tris 0  draws 0  0.0 ms")
        self.lbl_hints = perm_lbl("Two fingers pan · Pinch zoom · RMB look · WASD fly", TEXT_MUTED)

        # UE5-style collapsible Output Log toggle button in status bar
        self.btn_output_log = QPushButton("Output Log")
        self.btn_output_log.setCheckable(True)
        self.btn_output_log.setFixedHeight(18)
        self.btn_output_log.setStyleSheet("font-size: 7.5pt; padding: 0px 8px;")
        self.btn_output_log.toggled.connect(self._toggle_output_log)
        sb.addPermanentWidget(self.btn_output_log)

        self._build_output_log_dock()

    def _build_output_log_dock(self) -> None:
        from PySide6.QtWidgets import QCheckBox, QDockWidget, QHBoxLayout, QPlainTextEdit, QVBoxLayout

        self.log_dock = QDockWidget("Output Log", self)
        self.log_dock.setObjectName("EodOutputLogDock")
        self.log_dock.setAllowedAreas(Qt.DockWidgetArea.BottomDockWidgetArea | Qt.DockWidgetArea.TopDockWidgetArea)
        container = QWidget(self.log_dock)
        lay = QVBoxLayout(container)
        lay.setContentsMargins(6, 4, 6, 4)
        lay.setSpacing(4)

        hdr = QHBoxLayout()
        hdr.setSpacing(8)
        self.chk_log_verbose = QCheckBox("Verbose Output")
        self.chk_log_verbose.setChecked(True)
        self.btn_dump_cprofile = QPushButton("Dump cProfile Trace")
        self.btn_dump_cprofile.setFixedHeight(22)
        self.btn_dump_cprofile.setToolTip("Capture & save a Python cProfile trace + GPU/CPU phase report to Output Log")
        self.btn_dump_cprofile.clicked.connect(self.dump_cprofile_trace)
        self.btn_copy_log = QPushButton("Copy Log")
        self.btn_copy_log.setFixedHeight(22)
        self.btn_copy_log.clicked.connect(self._copy_output_log)
        self.btn_clear_log = QPushButton("Clear")
        self.btn_clear_log.setFixedHeight(22)
        self.btn_clear_log.clicked.connect(lambda: self.log_text.clear())
        hdr.addWidget(self.chk_log_verbose)
        hdr.addStretch(1)
        hdr.addWidget(self.btn_dump_cprofile)
        hdr.addWidget(self.btn_copy_log)
        hdr.addWidget(self.btn_clear_log)
        lay.addLayout(hdr)

        self.log_text = QPlainTextEdit()
        self.log_text.setFont(QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont))
        self.log_text.setReadOnly(True)
        self.log_text.setMaximumBlockCount(4000)
        self.log_text.setStyleSheet(
            "QPlainTextEdit { background: #0f1116; color: #d6dce8; "
            "font-size: 8.5pt; border: 1px solid #2a2f3a; }"
        )
        lay.addWidget(self.log_text, 1)
        self.log_dock.setWidget(container)
        self.addDockWidget(Qt.DockWidgetArea.BottomDockWidgetArea, self.log_dock)
        self.log_dock.hide()
        self.log_dock.visibilityChanged.connect(lambda vis: self.btn_output_log.setChecked(vis))

    def _toggle_output_log(self, checked: bool) -> None:
        if hasattr(self, "log_dock"):
            self.log_dock.setVisible(checked)

    def _copy_output_log(self) -> None:
        from PySide6.QtWidgets import QApplication
        cb = QApplication.clipboard()
        if cb is not None:
            cb.setText(self.log_text.toPlainText())
            self.set_action_status("Copied Output Log to clipboard")

    def dump_cprofile_trace(self, out_dir: Path | None = None) -> tuple[Path, str]:
        """Capture and dump a cProfile trace + phase breakdown to disk and Output Log."""
        def _sample_editor_tick() -> None:
            self.profiler.snapshot()
            if self.world is not None:
                _ = len(self.world.active_objects)

        txt_path, summary = self.profiler.dump_cprofile_trace(out_dir=out_dir, sample_fn=_sample_editor_tick)
        self.append_log(f"[Profiler] Saved cProfile trace -> {txt_path}\n{summary}", force_open=True)
        self.set_action_status(f"Dumped cProfile trace -> {txt_path.name}")
        return txt_path, summary

    def append_log(self, msg: str, *, verbose: bool = False, force_open: bool = False) -> None:
        if not hasattr(self, "log_text"):
            return
        if verbose and not self.chk_log_verbose.isChecked():
            return
        import datetime
        ts = datetime.datetime.now().strftime("%H:%M:%S")
        self.log_text.appendPlainText(f"[{ts}] {msg}")
        if force_open and not self.log_dock.isVisible():
            self.log_dock.show()

    def set_action_status(self, msg: str) -> None:
        self.lbl_status.setText(msg)

    def _show_progress(self, pct: int, msg: str) -> None:
        self.progress_bar.setValue(pct)
        self.progress_bar.show()
        self.btn_cancel_job.show()
        if msg:
            self.set_action_status(msg)
            self.append_log(f"[{pct:3d}%] {msg}", verbose=True)

    def _hide_progress(self) -> None:
        self.progress_bar.hide()
        self.btn_cancel_job.hide()

    def _cancel_active_job(self) -> None:
        if self._active_token is not None:
            self._active_token.cancel()
            self._active_token = None
        self._hide_progress()
        self.set_action_status("Background task cancelled")

    # -----------------------------------------------------------------------
    # Signal Wiring
    # -----------------------------------------------------------------------
    def _connect_signals(self) -> None:
        # Explorer -> MainWindow
        self.explorer.trackSelected.connect(self._on_explorer_track_selected)
        self.explorer.objectPicked.connect(self._select_object_by_id)
        self.explorer.markerPicked.connect(self._select_marker_by_id)
        self.explorer.roadLineSelected.connect(self._switch_road_line)
        self.explorer.modelPlaceRequested.connect(self._place_model_in_front_of_camera)
        self.explorer.modelSelected.connect(self._on_explorer_model_selected)
        self.explorer.ghostPlaceRequested.connect(self._activate_ghost_place_for)
        self.explorer.scatterModeRequested.connect(self._activate_scatter_for)
        self.explorer.replaceSelectedRequested.connect(self._replace_selected_with_key)
        self.explorer.layerToggled.connect(self._on_layer_toggled)
        self.explorer.carsRequested.connect(self._start_background_car_scan)

        # Viewport -> MainWindow & Properties
        self.viewport.objectSelected.connect(self._on_viewport_obj_selected)
        self.viewport.selectionChanged.connect(self._on_viewport_selection_changed)
        self.viewport.roadNodeSelected.connect(self._on_viewport_road_selected)
        self.viewport.markerSelected.connect(self._on_viewport_marker_selected)
        self.viewport.barrierSelected.connect(self._on_viewport_barrier_selected)
        self.viewport.objectTransformed.connect(self._on_viewport_obj_transformed)
        self.viewport.multiObjectTransformed.connect(self._on_viewport_multi_obj_transformed)
        self.viewport.altDuplicateCompleted.connect(self._on_alt_duplicate_completed)
        self.viewport.toolCycled.connect(self.select_tool)
        self.viewport.markerTransformed.connect(self._on_viewport_marker_transformed)
        self.viewport.roadNodeMoved.connect(self._on_viewport_road_moved)
        self.viewport.roadActionRequested.connect(self._on_road_action)
        self.viewport.modelDropped.connect(self._place_model_at)
        self.viewport.modelStamped.connect(self._on_ghost_stamped)
        self.viewport.scatterStrokeCompleted.connect(self._on_scatter_stroke)
        self.viewport.dropToGroundRequested.connect(self.snap_selected_to_ground)
        self.viewport.contextMenuRequested.connect(self._show_viewport_context_menu)
        self.viewport.statsUpdated.connect(self._on_viewport_stats)
        self.viewport.statusMessage.connect(self.set_action_status)
        self.viewport.mode_pill.shieldToggled.connect(self._sync_collision_toggles)

        # PIE Host -> MainWindow
        self.pie_host.pieStopped.connect(self._on_pie_stopped)
        self.pie_host.pieStatusChanged.connect(self.set_action_status)
        self.pie_host.pieLogLine.connect(lambda line: self.append_log(line))
        self.pie_host.pieRestartRequested.connect(
            lambda: self.start_pie(self._last_pie_mode, profile=self._last_pie_profile,
                                   vehicle=self._last_pie_vehicle)
        )

        # Properties -> MainWindow
        self.properties.objectTransformEdited.connect(self._on_prop_obj_transform)
        self.properties.snapObjectToGround.connect(self.snap_object_to_ground)
        self.properties.deleteObjectRequested.connect(self._on_prop_delete_object)
        self.properties.selectSimilarRequested.connect(self.select_similar_objects)
        self.properties.deleteSimilarRequested.connect(self.delete_similar_objects)
        self.properties.includeLodTwinsChanged.connect(self._on_include_lod_twins_changed)
        self.properties.editInBlenderRequested.connect(self.edit_object_in_blender)
        self.properties.makeCollisionRequested.connect(self.make_collision_for_object)
        self.properties.replaceObjectRequested.connect(self.replace_object_with_dialog)
        self.properties.replaceTextureRequested.connect(self.replace_texture_dialog)
        self.properties.resetTextureRequested.connect(self.reset_texture_to_retail)

        self.properties.roadNodePosEdited.connect(self._on_prop_road_pos)
        self.properties.roadPullOutRequested.connect(self._road_pull_out)
        self.properties.roadDeleteNodeRequested.connect(self._road_delete_node)
        self.properties.roadDrawFromHereRequested.connect(self._road_draw_from_here)
        self.properties.roadDrawRoadsRequested.connect(lambda: self.select_tool("road"))
        self.properties.roadLaneDeltaRequested.connect(self._road_lane_delta)
        self.properties.roadSplitExitRequested.connect(self._road_split_exit)
        self.properties.roadSplitTwoWayRequested.connect(self._road_split_two_way)

        self.properties.markerTransformEdited.connect(self._on_prop_marker_edited)
        self.properties.markerMetaEdited.connect(self._on_prop_marker_meta_edited)
        self.properties.snapMarkerToGround.connect(self._snap_marker_to_ground)
        self.properties.deleteMarkerRequested.connect(self._delete_marker)

        self.properties.environmentEdited.connect(self._edit_environment)
        self.properties.openEnvironmentRequested.connect(self.open_environment_panel)

        self.properties.barrierHeightApplyRequested.connect(self.apply_barrier_height)
        self.properties.clampSkyWallsRequested.connect(self.clamp_sky_walls_to_assets)

    # -----------------------------------------------------------------------
    # Track Loading (Synchronous + Non-Blocking QThreadPool Background Worker)
    # -----------------------------------------------------------------------
    def _attach_loaded_world(self, world: TrackWorld) -> None:
        self.world = world
        self.undo_stack.clear()
        self._autosave_dirty = False
        self.autosave_banner.hide()
        self._pending_recovery_path = None
        if self._pending_event_template:
            world.active_event_template = self._pending_event_template
            world.custom_race.event_type = self._pending_event_template
            world.custom_race.event_name = self._pending_event_template
            self._pending_event_template = None
        self.viewport.set_world(world)
        self.explorer.set_world(world)
        self.explorer.set_available_tracks(self.available_tracks, world.trk_path)
        self.properties.show_nothing()
        self._update_window_chrome()
        self._sync_pie_pickers_from_world()

        cache_tag = "CACHE HIT" if world.loaded_from_cache else "COLD"
        if world.load_ms > 0.0:
            self.profiler.record_phase("track_load", world.load_ms, detail=cache_tag)
        self.append_log(
            f"[Track] Loaded {world.track_name} ({cache_tag}) in {world.load_ms:.1f} ms | "
            f"{len(world.models)} models, {len(world.objects)} instances, {world.total_triangles:,} tris",
            verbose=True,
        )
        self.set_action_status(
            f"Loaded {world.track_name} [{cache_tag} {world.load_ms:.1f} ms]: "
            f"{len(world.models)} models ({world.car_count} cars/wrecks), "
            f"{len(world.objects)} instances, {world.total_triangles:,} tris, {len(world.road_graph.nodes)} ILIN nodes"
        )
        if self.viewport.show_perf_hud:
            self.viewport.perf_hud.refresh(self.profiler.snapshot())

        if not world.car_catalog_loaded:
            self._start_background_car_scan()

        if self._pending_vehicle_drive:
            track, vehicle = self._pending_vehicle_drive
            self._pending_vehicle_drive = None
            if world.trk_path == track:
                QTimer.singleShot(0, self, lambda: self.start_pie("free_roam", vehicle=vehicle))
        if self._pending_quick_free_roam:
            self._pending_quick_free_roam = False
            QTimer.singleShot(50, lambda: self.start_pie("free_roam"))
        else:
            self.check_autosave_recovery()

    def load_track(self, trk_path: Path, include_cars: bool = True) -> None:
        """Synchronous track load (used by CLI/project-open/tests)."""
        QApplication.setOverrideCursor(Qt.CursorShape.WaitCursor)
        try:
            self.set_action_status(f"Loading {trk_path.name}...")
            QApplication.processEvents()
            world = load_track_world(trk_path, self.game_dir, include_cars=include_cars)
            self._attach_loaded_world(world)
        except Exception as exc:
            QMessageBox.critical(self, "Load Track Failed", f"Could not load {trk_path}:\n{exc}")
        finally:
            QApplication.restoreOverrideCursor()

    def load_track_async(self, trk_path: Path) -> None:
        """Non-blocking background track load on QThreadPool so UI/OpenGL never freeze."""
        self._pending_vehicle_drive = None
        if self.pie_host.is_running:
            self.stop_pie()
        self._cancel_active_job()
        self._job_gen += 1
        token = CancelToken(self._job_gen)
        self._active_token = token
        self._show_progress(5, f"Loading {trk_path.name} in background...")

        worker = FunctionWorker(
            load_track_world,
            trk_path,
            self.game_dir,
            include_cars=False,
            token=token,
            label=f"Loading {trk_path.name}",
        )
        worker.signals.progress.connect(self._on_bg_progress)
        worker.signals.finished.connect(self._on_track_loaded_bg)
        worker.signals.failed.connect(self._on_bg_failed)
        worker.signals.cancelled.connect(self._on_bg_cancelled)
        start_worker(worker, priority=10)

    def _on_bg_cancelled(self, gen: int) -> None:
        if gen == self._job_gen:
            self._pending_vehicle_drive = None
            self._active_token = None
            self._hide_progress()

    def _on_bg_progress(self, gen: int, pct: int, msg: str) -> None:
        if gen == self._job_gen:
            self._show_progress(pct, msg)

    def _on_track_loaded_bg(self, gen: int, world: TrackWorld) -> None:
        if gen != self._job_gen:
            return
        self._active_token = None
        self._hide_progress()
        self._attach_loaded_world(world)

    def _on_bg_failed(self, gen: int, err: str) -> None:
        if gen != self._job_gen:
            return
        self._pending_vehicle_drive = None
        self._active_token = None
        self._hide_progress()
        self.append_log(f"[ERROR] Background task failed:\n{err}", force_open=True)
        QMessageBox.critical(self, "Background Task Failed", err)

    def _start_background_car_scan(self) -> None:
        """Scan all 37 retail DATA/CAR/*.CAR files asynchronously on QThreadPool."""
        if self._car_token is not None:
            self._car_token.cancel()
        self._car_job_gen += 1
        token = CancelToken(self._car_job_gen)
        self._car_token = token
        worker = FunctionWorker(
            load_car_catalog,
            self.game_dir,
            token=token,
            label="Scanning DATA/CAR",
        )
        worker.signals.finished.connect(self._on_cars_loaded_bg)
        start_worker(worker, priority=0)

    def _on_cars_loaded_bg(self, gen: int, res: tuple[dict[str, Any], dict[str, Any]]) -> None:
        if gen != self._car_job_gen or self.world is None:
            return
        c_models, c_tex = res
        merge_car_catalog_into_world(self.world, c_models, c_tex)
        self.viewport.ensure_gl_textures()
        self.explorer.refresh_model_categories()
        self.explorer.refresh_counts()
        self.explorer._rebuild_model_grid()
        self.set_action_status(f"Loaded {self.world.car_count} retail Cars & Wrecks from DATA/CAR/")

    def _on_explorer_track_selected(self, path_str: str) -> None:
        target = Path(path_str)
        if self.world is not None and self.world.trk_path.resolve() == target.resolve():
            return
        if not self._confirm_discard_changes(f"switch track to {target.stem.upper()}"):
            if self.world is not None:
                self.explorer.set_available_tracks(self.available_tracks, self.world.trk_path)
            return
        self.load_track_async(target)

    def reload_current_track(self) -> None:
        if self.world is not None:
            if not self._confirm_discard_changes("reload the retail track"):
                return
            self.load_track_async(self.world.trk_path)

    # -----------------------------------------------------------------------
    # Adobe / Lumion-Style Home Hub, Viewport Thumbnails & Crash-Safe Autosave
    # -----------------------------------------------------------------------
    def open_home_hub(self) -> None:
        """Open the dark Home Hub dialog for New Project / Recent Projects / Quick Launch."""
        if not self._confirm_discard_changes("open the Home Hub"):
            return
        hub = HomeHubDialog(self.game_dir, parent=self)
        if hub.exec() == HomeHubDialog.DialogCode.Accepted:
            self.apply_home_hub_result(hub)
        elif self.world is None and self.available_tracks:
            self.load_track_async(self.available_tracks[0])

    def apply_home_hub_result(self, hub: HomeHubDialog, *, sync_load: bool = False) -> None:
        """Execute the chosen Home Hub action (also directly callable from headless tests)."""
        action = hub.result_action
        if action == "open_project" and hub.selected_project_file and hub.selected_project_file.is_file():
            self.open_project_file(hub.selected_project_file)
            return

        if action == "vehicle_importer":
            self.open_vehicle_tuner_dialog().show_nascar_import()
            return

        if action in ("new_project", "quick_free_roam"):
            self.project_name = hub.selected_project_name or "Untitled World"
            self.project_path = None
            self._pending_event_template = hub.selected_event_template
            self._pending_quick_free_roam = action == "quick_free_roam"
            self._update_window_chrome()
            target = hub.selected_track_path or (self.available_tracks[0] if self.available_tracks else None)
            if target is not None and target.is_file():
                if sync_load:
                    self.load_track(target, include_cars=False)
                else:
                    self.load_track_async(target)

    def capture_project_thumbnail(self, project_path: Path) -> Path | None:
        """Save a crisp 384x216 viewport thumbnail PNG right beside `.eodworld.json`."""
        try:
            thumb_path = thumbnail_path_for_project(project_path)
            img = None
            if getattr(self.viewport, "_gl_ready", False) and self.viewport.isVisible():
                try:
                    img = self.viewport.grabFramebuffer()
                except Exception:
                    img = None
            if img is None or img.isNull():
                pm = self.viewport.grab()
                img = pm.toImage() if not pm.isNull() else None
            if img is None or img.isNull():
                return None
            scaled = img.scaled(
                384,
                216,
                Qt.AspectRatioMode.KeepAspectRatioByExpanding,
                Qt.TransformationMode.SmoothTransformation,
            )
            thumb_path.parent.mkdir(parents=True, exist_ok=True)
            if scaled.save(str(thumb_path), "PNG"):
                return thumb_path
        except Exception:
            pass
        return None

    def _on_undo_index_changed(self, _idx: int) -> None:
        if self.world is not None:
            self._autosave_dirty = self.undo_stack.index() != self._saved_undo_index
            if hasattr(self.properties, "set_project_dirty"):
                self.properties.set_project_dirty(self._is_dirty())
            self._update_window_chrome()

    def trigger_background_autosave(self, *, force: bool = False, synchronous: bool = False) -> Path | None:
        """Silently write atomic crash-safe autosave (`<project>.autosave.tmp` -> `os.replace`) off the UI thread."""
        if self.world is None:
            return None
        if not force and not self._autosave_dirty:
            return None
        if self.pie_host.is_running and not force:
            return None
        target = autosave_path_for(self.project_path, self.world.track_name, self.project_name)
        doc = build_project_overlay_dict(self.project_name, self.world, self.viewport.camera_state())
        self._autosave_dirty = False
        if synchronous:
            saved = save_autosave_snapshot(target, doc)
            self.append_log(f"[Autosave] Saved crash-safe snapshot -> {saved.name}", verbose=True)
            return saved
        if self._autosave_token is not None:
            self._autosave_token.cancel()
        self._autosave_gen += 1
        token = CancelToken(self._autosave_gen)
        self._autosave_token = token
        worker = FunctionWorker(
            save_autosave_snapshot,
            target,
            doc,
            token=token,
            label="Autosaving project",
        )
        worker.signals.finished.connect(self._on_autosave_done)
        start_worker(worker, priority=-1)
        return target

    def _on_autosave_done(self, gen: int, saved_path: Path) -> None:
        if gen == self._autosave_gen:
            self._autosave_token = None
            self.append_log(f"[Autosave] Crash-safe snapshot updated -> {Path(saved_path).name}", verbose=True)

    def check_autosave_recovery(self, project_path: Path | None = None) -> Path | None:
        """Check if a newer crash-recovery autosave exists and surface non-intrusive banner."""
        if self.world is None:
            return None
        p_ref = project_path if project_path is not None else self.project_path
        newer = find_newer_autosave(p_ref, self.world.track_name, self.project_name)
        if newer is not None:
            self._pending_recovery_path = newer
            self.lbl_autosave_msg.setText(
                f"Crash-recovery autosave detected ({newer.name}) — newer than last manual save."
            )
            self.autosave_banner.show()
            self.append_log(f"[Autosave] Newer autosave available: {newer}", verbose=True)
        else:
            self._pending_recovery_path = None
            self.autosave_banner.hide()
        return newer

    def recover_pending_autosave(self) -> bool:
        """Apply pending crash-recovery autosave onto the loaded world."""
        if self.world is None or self._pending_recovery_path is None or not self._pending_recovery_path.is_file():
            self.autosave_banner.hide()
            return False
        path = self._pending_recovery_path
        doc = apply_project_overlay(self.world, path)
        if doc.get("project_name"):
            self.project_name = str(doc["project_name"])
        if doc.get("camera"):
            self.viewport.apply_camera_state(doc["camera"])
        for tname, ta in self.world.textures.items():
            if ta.is_replaced:
                self.viewport.update_gl_texture(tname)
        self.viewport.mark_collision_dirty()
        self.viewport.mark_scene_dirty()
        self.explorer.refresh_counts()
        self._update_window_chrome()
        self._sync_pie_pickers_from_world()
        self._autosave_dirty = True
        self._pending_recovery_path = None
        self.autosave_banner.hide()
        self.set_action_status(f"Recovered autosave -> {path.name}")
        return True

    def discard_pending_autosave(self) -> None:
        if self.world is not None:
            clear_autosave(self.project_path, self.world.track_name, self.project_name)
        self._pending_recovery_path = None
        self.autosave_banner.hide()
        self.set_action_status("Discarded crash-recovery autosave")

    # -----------------------------------------------------------------------
    # Project & Non-Destructive Mod Layer Management
    # -----------------------------------------------------------------------
    def _project_slug(self) -> str:
        return re.sub(r"[^A-Za-z0-9_.-]+", "_", self.project_name).strip("_") or "world_mod"

    def _default_mod_dir(self) -> Path:
        """Return isolated PIE staging folder (outside retail disc/ to preserve read-only disc invariant)."""
        return pie_staging_root() / self._project_slug()

    def _default_project_dir(self) -> Path:
        return Path.home() / "Documents" / "EOD Projects" / self._project_slug()

    def new_project(self) -> None:
        if not self._confirm_discard_changes("start a new project"):
            return
        name, ok = QInputDialog.getText(
            self, "New World Project", "Project name:", text=f"{self.world.display_name if self.world else 'EOD'} Mod"
        )
        if not ok or not name.strip():
            return
        self.project_name = name.strip()
        self.project_path = None
        self._mod_output_dir = None
        self._saved_undo_index = 0
        self._autosave_dirty = False
        if self.world is not None:
            self.load_track_async(self.world.trk_path)
        self._update_window_chrome()
        self._sync_pie_pickers_from_world()
        self.set_action_status(f"Started new world project '{self.project_name}'")

    def open_project_dialog(self) -> None:
        if not self._confirm_discard_changes("open another project"):
            return
        start_dir = self._default_project_dir().parent
        if not start_dir.is_dir():
            start_dir = Path.home() / "Documents"
        fn, _ = QFileDialog.getOpenFileName(
            self, "Open World Project", str(start_dir), "EOD World Project (*.eodworld.json);;JSON (*.json)"
        )
        if fn:
            self.open_project_file(Path(fn))

    def open_project_file(self, path: Path) -> None:
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
            trk_rel = raw.get("trk_rel", "")
            trk_path = (self.game_dir / trk_rel) if trk_rel else None
            if not trk_path or not trk_path.is_file():
                want = str(raw.get("track", "")).upper()
                trk_path = next((p for p in self.available_tracks if p.stem.upper() == want), None)
            if trk_path is None:
                raise FileNotFoundError(f"Track {raw.get('track')} not found in {self.game_dir}")
            self.load_track(trk_path, include_cars=True)
            assert self.world is not None
            doc = apply_project_overlay(self.world, path)
            self.project_name = doc.get("project_name", path.stem)
            self.project_path = path
            if doc.get("camera"):
                self.viewport.apply_camera_state(doc["camera"])
            for tname, ta in self.world.textures.items():
                if ta.is_replaced:
                    self.viewport.update_gl_texture(tname)
            self.viewport.mark_collision_dirty()
            self.viewport.mark_scene_dirty()
            self.explorer.refresh_counts()
            self._mark_saved()
            self._sync_pie_pickers_from_world()
            record_recent_project(
                path,
                self.project_name,
                self.world.track_name,
                display_name=self.world.display_name,
                event_template=self.world.active_event_template,
            )
            self.check_autosave_recovery(path)
            self.set_action_status(f"Opened project '{self.project_name}' from {path.name}")
        except Exception as exc:
            QMessageBox.critical(self, "Open Project Failed", str(exc))

    def save_project(self) -> None:
        if self.world is None:
            return
        if hasattr(self.properties, "flush_pending_edits"):
            self.properties.flush_pending_edits()
        if self.project_path is None:
            self.save_project_as()
            return
        saved = save_project_overlay(self.project_path, self.project_name, self.world, self.viewport.camera_state())
        self.project_path = saved
        thumb = self.capture_project_thumbnail(saved)
        record_recent_project(
            saved,
            self.project_name,
            self.world.track_name,
            display_name=self.world.display_name,
            event_template=self.world.active_event_template,
            thumbnail_path=thumb,
        )
        clear_autosave(saved, self.world.track_name, self.project_name)
        self._mark_saved()
        self.autosave_banner.hide()
        self.set_action_status(f"Saved project overlay -> {saved.name}")

    def save_project_as(self) -> None:
        if self.world is None:
            return
        if hasattr(self.properties, "flush_pending_edits"):
            self.properties.flush_pending_edits()
        default_dir = self.project_path.parent if self.project_path else self._default_project_dir()
        default_dir.mkdir(parents=True, exist_ok=True)
        default_file = default_dir / f"{self._project_slug()}.eodworld.json"
        fn, _ = QFileDialog.getSaveFileName(
            self, "Save World Project Overlay", str(default_file), "EOD World Project (*.eodworld.json)"
        )
        if not fn:
            return
        target = Path(fn)
        stem = target.name
        if stem.lower().endswith(".eodworld.json"):
            stem = stem[: -len(".eodworld.json")]
        elif stem.lower().endswith(".json"):
            stem = stem[: -len(".json")]
        if stem.strip():
            self.project_name = stem.strip()
        saved = save_project_overlay(target, self.project_name, self.world, self.viewport.camera_state())
        self.project_path = saved
        thumb = self.capture_project_thumbnail(saved)
        record_recent_project(
            saved,
            self.project_name,
            self.world.track_name,
            display_name=self.world.display_name,
            event_template=self.world.active_event_template,
            thumbnail_path=thumb,
        )
        clear_autosave(saved, self.world.track_name, self.project_name)
        self._mark_saved()
        self.autosave_banner.hide()
        self.set_action_status(f"Saved project overlay -> {saved.name}")

    def write_mod_layer_dialog(self, *, prompt_dir: bool = False) -> None:
        if self.world is None:
            return
        if hasattr(self.properties, "flush_pending_edits"):
            self.properties.flush_pending_edits()
        slug = self._project_slug()
        if self._mod_output_dir is not None and not prompt_dir:
            out_dir = self._mod_output_dir
        else:
            default_root = tdeod_support_dir() / "mods"
            default_root.mkdir(parents=True, exist_ok=True)
            dest = QFileDialog.getExistingDirectory(
                self, "Select Output Directory for Mod Layer", str(self._mod_output_dir.parent if self._mod_output_dir else default_root)
            )
            if not dest:
                return
            out_dir = Path(dest) / slug if Path(dest).name != slug else Path(dest)
            self._mod_output_dir = out_dir
        try:
            report = export_mod_layer(self.world, self.project_name, out_dir)
            self._sync_custom_cars_into_mod_dir(out_dir)
            cms = float(report.get("compile_ms", 0.0))
            bmode = str(report.get("bake_mode", "full")).upper()
            placed_total = int(report.get("editor_placed_objects", 0))
            compiled_total = int(report.get("custom_objects_compiled", 0))
            warnings_list = list(report.get("warnings") or [])
            self.profiler.record_phase("mod_compile", cms, detail=bmode)
            self._refresh_after_edit()
            self.append_log(
                f"[Mod Baker] {bmode} compile finished in {cms:.2f} ms -> {out_dir} "
                f"(placed compiled: {compiled_total}/{placed_total})",
            )
            for w_msg in warnings_list:
                self.append_log(f"[Mod Baker][WARN] {w_msg}", force_open=True)
            self.set_action_status(
                f"Exported mod layer ({bmode} {cms:.2f} ms, compiled {compiled_total}/{placed_total}) -> {out_dir}"
            )
            warn_block = ""
            if warnings_list:
                shown = "\n".join(f"  • {w}" for w in warnings_list[:6])
                more = f"\n  … and {len(warnings_list) - 6} more (see Output Log)" if len(warnings_list) > 6 else ""
                warn_block = f"\n\nWarnings ({len(warnings_list)}):\n{shown}{more}"
            summary = (
                f"Non-destructive mod layer exported ({bmode} bake in {cms:.2f} ms)!\n\n"
                f"Output folder: {out_dir}\n"
                f"Mod TRK copy: {report['mod_trk']}\n"
                f"Retail placement edits: {report['retail_placement_edits']} "
                f"({report['changed_payload_bytes']:,} bytes patched, {report['deleted_retail_objects']} deleted)\n"
                f"SOBL barriers patched: {report['sobl_barriers_patched']}\n"
                f"ILIN lines patched: {len(report['ilin_patched_lines'])}\n"
                f"HOSTTEX textures exported: {report['host_textures_exported']}\n"
                f"Placed objects compiled: {compiled_total} / {placed_total}\n\n"
                f"Retail .TRK SHA-256 verified UNTOUCHED.{warn_block}"
            )
            if placed_total > 0 and compiled_total < placed_total:
                QMessageBox.warning(self, "Mod Layer Written With Warnings", summary)
            else:
                QMessageBox.information(self, "Mod Layer Written", summary)
        except Exception as exc:
            QMessageBox.critical(self, "Export Mod Layer Failed", str(exc))

    def package_mod_dialog(self) -> None:
        if self.world is None:
            return
        if hasattr(self.properties, "flush_pending_edits"):
            self.properties.flush_pending_edits()
        if self._active_token is not None:
            self.set_action_status("A background job is running. Package the mod when it finishes.")
            return
        dialog = PackageModDialog(self.project_name, self)
        if not dialog.exec():
            return
        package_id, name, version = dialog.values()
        folder = self.project_path.parent if self.project_path else Path.home()
        filename, _ = QFileDialog.getSaveFileName(
            self, "Save .eodmod Package", str(folder / f"{package_id}-{version}.eodmod"), "EOD Mod (*.eodmod)"
        )
        if not filename:
            return
        output = Path(filename)
        if output.suffix.lower() != ".eodmod":
            output = output.with_name(f"{output.name}.eodmod")
        snapshot = snapshot_world_for_bake(self.world)
        self._job_gen += 1
        token = CancelToken(self._job_gen)
        self._active_token = token
        self._show_progress(0, "Preparing .eodmod package")
        worker = FunctionWorker(
            package_world_mod, snapshot, self.project_name, package_id, version, name, output,
            token=token, label="Packaging .eodmod",
        )
        worker.signals.progress.connect(self._on_bg_progress)
        worker.signals.finished.connect(self._on_package_mod_done)
        worker.signals.failed.connect(self._on_bg_failed)
        worker.signals.cancelled.connect(self._on_bg_cancelled)
        start_worker(worker)

    def _on_package_mod_done(self, gen: int, report: dict[str, Any]) -> None:
        if gen != self._job_gen or self._active_token is None:
            return
        self._active_token = None
        self._hide_progress()
        self.set_action_status(
            f"Packaged {Path(report['path']).name} ({report['native']} archive members, {report['host']} textures)"
        )
        self.append_log(f"[Package] Saved {report['path']} (SHA-256 {report['sha256']})")
        for warning in report["warnings"]:
            self.append_log(f"[Package] {warning}", force_open=True)

    def open_mod_manager(self) -> None:
        try:
            from mod_manager import ModManagerDialog

            dialog = ModManagerDialog(self.game_dir, parent=self, allow_pie=self.world is not None)
            if dialog.exec() and dialog.pie_request is not None:
                profile, mode = dialog.pie_request
                self.start_pie(mode, profile=(profile, dialog.root))
        except (ImportError, OSError, ValueError) as exc:
            QMessageBox.critical(self, "Open Mod Manager Failed", str(exc))

    def choose_game_dir(self) -> None:
        d = QFileDialog.getExistingDirectory(self, "Select EOD Game Data Directory", str(self.game_dir))
        if not d:
            return
        self.game_dir = Path(d)
        self.available_tracks = discover_tracks(self.game_dir)
        self.explorer.set_available_tracks(self.available_tracks)
        if self.available_tracks:
            self.load_track_async(self.available_tracks[0])
        else:
            self.set_action_status(f"No .TRK files found under {self.game_dir / 'DATA' / 'TRACKS'}")

    def open_tracks_folder(self) -> None:
        tracks_dir = self.game_dir / "DATA" / "TRACKS"
        if not tracks_dir.is_dir():
            tracks_dir = self.game_dir
        if tracks_dir.is_dir():
            if sys.platform == "win32":
                os.startfile(str(tracks_dir))
            else:
                QDesktopServices.openUrl(QUrl.fromLocalFile(str(tracks_dir)))

    def _sync_custom_cars_into_mod_dir(self, mod_dir: Path) -> None:
        """Copy any car archives saved in Car Studio into the active PIE/exported mod layer."""
        if not self._custom_car_archives:
            return
        import shutil

        dst_car_dir = mod_dir / "DATA" / "CAR"
        dst_car_dir.mkdir(parents=True, exist_ok=True)
        for src in list(self._custom_car_archives):
            if src.is_file():
                try:
                    shutil.copy2(src, dst_car_dir / src.name)
                except OSError:
                    pass

    # -----------------------------------------------------------------------
    # Unreal Engine 4/5 Play-In-Editor (PIE) Embedded Viewport
    # -----------------------------------------------------------------------
    def build_current_pie_spec(
        self,
        mod_dir: Path,
        mode: str,
        vehicle: RegisteredVehicle | None = None,
        spawn_pos: tuple[float, float, float, float] | None = None,
    ) -> dict[str, Any]:
        """Build a live PIE launch spec directly from the toolbar pickers, Sync All Events, and Arcade Mode."""
        assert self.world is not None
        cfg = self.world.custom_race
        ev = self.cmb_pie_event.currentText().strip() or cfg.event_type
        car = self.cmb_pie_car.currentText().strip() or cfg.player_car or "Algar"
        ai = int(self.spn_pie_ai.value())
        sync_all = bool(self.act_sync_events.isChecked())
        self.world.sync_all_events = sync_all
        cfg.sync_all_events = sync_all
        cfg.event_type = ev
        cfg.event_name = ev
        cfg.player_car = car
        cfg.ai_count = ai
        rmode = str(self.cmb_renderer_mode.currentData() or "ps2")
        arcade_mode = str(cfg.arcade_mode or "free_roam")
        pie_scale = int(self.cmb_pie_scale.currentData() or self._pie_render_scale) if hasattr(self, "cmb_pie_scale") else self._pie_render_scale
        spec = build_pie_launch_spec(
            self.game_dir,
            mod_dir,
            self.world.display_name,
            mode=mode,
            event_name=ev,
            car_name=str(vehicle.number) if vehicle is not None else car,
            ai_count=ai,
            laps=cfg.laps,
            free_roam_place=cfg.free_roam_place,
            renderer_mode=rmode,
            sync_all_events=sync_all,
            arcade_mode=arcade_mode,
            ai_autodrive=self.pie_host.btn_ai_drive.isChecked(),
            free_cam=self.pie_host.btn_free_cam.isChecked(),
            pie_render_scale=pie_scale,
            spawn_pos=spawn_pos,
        )
        if vehicle is not None:
            spec["label"] += f" · {vehicle.chassis} #{vehicle.number}"
        if spawn_pos is not None:
            spec["label"] += " · Camera Spawn"
        return spec

    def play_from_camera(self) -> None:
        """Launch PIE Free Roam spawning directly at the current 3D editor camera position."""
        if self.world is None:
            return
        cx, cy, cz = (float(self.viewport.cam_pos[0]), float(self.viewport.cam_pos[1]), float(self.viewport.cam_pos[2]))
        gy = self.world.collision.ground_height(cx, cz)
        spawn_y = max(gy + 1.2, min(cy, gy + 12.0))
        yaw = float(self.viewport.cam_yaw)
        self.start_pie("free_roam", spawn_pos=(cx, spawn_y, cz, yaw))

    def start_pie(
        self,
        mode: str = "free_roam",
        *,
        profile: tuple[str, Path] | None = None,
        vehicle: RegisteredVehicle | None = None,
        spawn_pos: tuple[float, float, float, float] | None = None,
    ) -> None:
        """Compile mod layer on QThreadPool worker, then launch live PIE."""
        if self.world is None:
            return
        if hasattr(self.properties, "flush_pending_edits"):
            self.properties.flush_pending_edits()
        mod_dir = self._default_mod_dir()

        # Sync live toolbar pickers + Sync All Events into TrackWorld before compiling mod layer
        self._on_pie_picker_changed()
        self.world.sync_all_events = bool(self.act_sync_events.isChecked())
        self.world.custom_race.sync_all_events = self.world.sync_all_events

        # Pause background thumbnail & car-scan workers so PIE compile & game get full CPU/GPU
        if getattr(self.explorer, "_thumb_token", None) is not None:
            self.explorer._thumb_token.cancel()
        if self._car_token is not None:
            self._car_token.cancel()
            self._car_token = None

        self._cancel_active_job()
        self._job_gen += 1
        token = CancelToken(self._job_gen)
        self._active_token = token
        self._pending_pie_mod_dir = mod_dir
        self._pending_pie_mode = mode
        self._pending_pie_profile = profile
        self._pending_pie_spawn_pos = spawn_pos
        self._show_progress(10, f"Compiling mod layer for PIE ({mode})...")

        if vehicle is not None:
            worker = FunctionWorker(
                build_vehicle_pie_layer, snapshot_world_for_bake(self.world), self.project_name, vehicle,
                token=token, label="Composing registered vehicle for PIE",
            )
        elif profile is None:
            worker = FunctionWorker(
                export_mod_layer, self.world, self.project_name, mod_dir,
                token=token, label="Compiling Mod Layer for PIE",
            )
        else:
            from profile_pie import build_profile_pie_layer

            worker = FunctionWorker(
                build_profile_pie_layer, snapshot_world_for_bake(self.world), self.project_name, *profile,
                token=token, label="Compiling project with installed profile",
            )
        worker.signals.progress.connect(self._on_bg_progress)
        worker.signals.finished.connect(self._on_pie_compile_done_slot)
        worker.signals.failed.connect(self._on_bg_failed)
        worker.signals.cancelled.connect(self._on_bg_cancelled)
        start_worker(worker, priority=10)

    def _on_pie_compile_done_slot(self, gen: int, report: dict[str, Any]) -> None:
        self._on_pie_compile_done(gen, report, report.get("pie_layer", self._pending_pie_mod_dir), self._pending_pie_mode)

    def _on_pie_compile_done(self, gen: int, report: dict[str, Any], mod_dir: Path, mode: str) -> None:
        if gen != self._job_gen or self.world is None or self._active_token is None:
            return
        self._active_token = None
        self._hide_progress()

        self._sync_custom_cars_into_mod_dir(Path(mod_dir))

        cms = float(report.get("compile_ms", 0.0)) if isinstance(report, dict) else 0.0
        bmode = str(report.get("bake_mode", "full")).upper() if isinstance(report, dict) else "FULL"
        placed_total = int(report.get("editor_placed_objects", 0)) if isinstance(report, dict) else 0
        compiled_total = int(report.get("custom_objects_compiled", 0)) if isinstance(report, dict) else 0
        warnings_list = list(report.get("warnings") or []) if isinstance(report, dict) else []
        for w_msg in warnings_list:
            self.append_log(f"[PIE][WARN] {w_msg}", force_open=True)
        self.profiler.record_phase("mod_compile", cms, detail=bmode)
        self.set_action_status(
            f"Mod compiled ({bmode} {cms:.2f} ms, placed {compiled_total}/{placed_total}) — launching PIE..."
        )

        vehicle = report.get("pie_vehicle")
        spawn_pos = self._pending_pie_spawn_pos
        if spawn_pos is None and self.world.markers:
            sp_mk = next((m for m in self.world.markers if m.kind in ("Spawn", "Start Grid")), None)
            if sp_mk is not None:
                spawn_pos = (float(sp_mk.position[0]), float(sp_mk.position[1]) + 1.0, float(sp_mk.position[2]), float(sp_mk.heading_deg))
        spec = self.build_current_pie_spec(mod_dir, mode, vehicle=vehicle, spawn_pos=spawn_pos)
        if report.get("pie_profile"):
            spec["label"] += f" · Profile {report['pie_profile']}"
        self.append_log(
            f"[PIE] {bmode} compile finished in {cms:.2f} ms (placed {compiled_total}/{placed_total}). "
            f"Launching {spec['exe']} | {spec['direct_start']} "
            f"| sync_all={self.world.sync_all_events} arcade={spec.get('arcade_mode')} "
            f"| scale={spec.get('pie_render_scale')}x | EOD_NATIVE={spec['env'].get('EOD_NATIVE')} "
            f"EOD_LOOK={spec['env'].get('EOD_LOOK', 'original')}",
            verbose=True,
        )
        self.center_stack.setCurrentWidget(self.pie_host)
        self.act_pie_stop.setEnabled(True)
        self.profiler.begin_phase("pie_launch")
        ok = self.pie_host.start_pie(spec)
        self._cleanup_profile_pie_overlay()
        if ok:
            self._profile_pie_overlay = report.get("pie_overlay")
            self._last_pie_mode = mode
            self._last_pie_profile = self._pending_pie_profile
            self._last_pie_vehicle = vehicle
        elif report.get("pie_overlay"):
            report["pie_overlay"].cleanup()
        pie_ms = self.profiler.end_phase("pie_launch", detail=mode)
        self.append_log(f"[PIE] Process spawn handed off in {pie_ms:.1f} ms", verbose=True)
        if not ok:
            exe_label = str(spec.get("exe_label") or GAME_EXE_LABEL)
            self.act_pie_stop.setEnabled(False)
            self.center_stack.setCurrentWidget(self.viewport)
            self.explorer.resume_thumbnails()
            hint = (
                "Build build-mac-plume/eod, install Eve of Destruction.app, or set EOD_PIE_EXE."
                if sys.platform == "darwin"
                else "Place eod.exe beside the editor, in build/eod.exe, or set EOD_PIE_EXE."
            )
            self.append_log(f"[PIE][ERROR] Could not launch {exe_label} at: {spec['exe']}", force_open=True)
            QMessageBox.warning(
                self,
                "Play In Editor (PIE)",
                f"Could not launch {exe_label} at:\n{spec['exe']}\n\n{hint}",
            )

    def stop_pie(self) -> None:
        self.pie_host.stop_pie(emit_signal=True)

    def _cleanup_profile_pie_overlay(self) -> None:
        if self._profile_pie_overlay is not None:
            self._profile_pie_overlay.cleanup()
            self._profile_pie_overlay = None

    def _on_pie_stopped(self) -> None:
        self._cleanup_profile_pie_overlay()
        self.act_pie_stop.setEnabled(False)
        self.center_stack.setCurrentWidget(self.viewport)
        self.viewport.setFocus()
        self.viewport.update()
        # Resume any paused background thumbnail & car-library scans so Explorer never stays blank!
        self.explorer.resume_thumbnails()
        if self.world is not None and self.world.car_count == 0 and self._car_token is None:
            self._start_background_car_scan()

    def launch_game(self) -> None:
        """Alias for backward compatibility: runs PIE Free Roam."""
        self.start_pie("free_roam")

    # -----------------------------------------------------------------------
    # Tools, Snapping & View Toggles
    # -----------------------------------------------------------------------
    def _shortcut_rotate_or_ghost(self) -> None:
        """Route `R` to rotate Ghost Preview when in Place Mode, otherwise switch to Rotate Tool."""
        if self.viewport.active_tool == "place":
            self.viewport.ghost_yaw = (self.viewport.ghost_yaw + self.viewport.angle_snap_step) % 360.0
            self.set_action_status(f"Ghost rotation: {self.viewport.ghost_yaw:.0f}°")
            self.viewport.update()
            return
        self.select_tool("rotate")

    def select_tool(self, tool: str) -> None:
        if tool in ("place", "scatter") and not self.viewport.ghost_model_key:
            cur = self.explorer.current_model_key()
            if cur:
                self.viewport.set_ghost_model(cur)
            elif self.world and self.world.models:
                # Prefer a car/wreck if available, else first model
                car_k = next((k for k, m in self.world.models.items() if m.category == "car-wreck"), None)
                self.viewport.set_ghost_model(car_k or next(iter(self.world.models.keys())))
        self.viewport.set_tool(tool)
        self.act_tool_move.setChecked(tool == "move")
        self.act_tool_rotate.setChecked(tool == "rotate")
        self.act_tool_scale.setChecked(tool == "scale")
        self.act_tool_place.setChecked(tool == "place")
        self.act_tool_scatter.setChecked(tool == "scatter")
        self.act_tool_road.setChecked(tool == "road")
        if tool == "road":
            self.act_v_roads.setChecked(True)

    def _on_pie_picker_changed(self, *_args: Any) -> None:
        if self.world is None:
            return
        ev = self.cmb_pie_event.currentText().strip() or "All Templates (Combined)"
        car = self.cmb_pie_car.currentText().strip() or "Algar"
        ai = int(self.spn_pie_ai.value())
        self.world.custom_race.event_type = ev
        self.world.custom_race.event_name = ev
        self.world.custom_race.player_car = car
        self.world.custom_race.ai_count = ai

        # Live Event Template viewport filter + synchronized active .ild racing line
        self.viewport.set_event_template(ev)
        tpl = self.world.event_templates.get(ev)
        if tpl is not None and tpl.ilin_member and tpl.ilin_member in self.world.all_road_graphs:
            if self.world.road_graph.line_name != tpl.ilin_member:
                self._switch_road_line(tpl.ilin_member)

    def _toggle_sync_all_events(self, checked: bool) -> None:
        if self.world is not None:
            self.world.sync_all_events = bool(checked)
            self.world.custom_race.sync_all_events = bool(checked)
            self.viewport.mark_scene_dirty()
        self.set_action_status(
            f"Sync All Events: {'ON (modded ramps & props sync across all events)' if checked else 'OFF (event-specific)'}"
        )

    def _sync_pie_pickers_from_world(self) -> None:
        if self.world is None or not hasattr(self, "cmb_pie_event"):
            return
        self.cmb_pie_event.blockSignals(True)
        self.cmb_pie_event.clear()
        events = ["All Templates (Combined)"]
        for ev in self.world.event_templates.keys():
            if ev not in events:
                events.append(ev)
        for ev in self.world.available_events:
            if ev not in events:
                events.append(ev)
        if "Battle (Free Roam)" not in events:
            events.insert(1, "Battle (Free Roam)")
        if "Career Overworld (Free Roam)" not in events:
            events.append("Career Overworld (Free Roam)")
        self.cmb_pie_event.addItems(events)

        want_ev = self.world.active_event_template or self.world.custom_race.event_type
        idx = self.cmb_pie_event.findText(want_ev, Qt.MatchFlag.MatchFixedString)
        if idx < 0 and self.world.custom_race.event_type:
            idx = self.cmb_pie_event.findText(self.world.custom_race.event_type, Qt.MatchFlag.MatchFixedString)
        self.cmb_pie_event.setCurrentIndex(idx if idx >= 0 else 0)
        self.cmb_pie_event.blockSignals(False)

        self.cmb_pie_car.blockSignals(True)
        cidx = self.cmb_pie_car.findText(self.world.custom_race.player_car, Qt.MatchFlag.MatchFixedString)
        if cidx >= 0:
            self.cmb_pie_car.setCurrentIndex(cidx)
        self.cmb_pie_car.blockSignals(False)

        self.spn_pie_ai.blockSignals(True)
        self.spn_pie_ai.setValue(max(0, min(12, int(self.world.custom_race.ai_count))))
        self.spn_pie_ai.blockSignals(False)

        if hasattr(self, "act_sync_events"):
            self.act_sync_events.blockSignals(True)
            self.act_sync_events.setChecked(bool(self.world.sync_all_events))
            self.act_sync_events.blockSignals(False)

        if hasattr(self.properties, "chk_lod_twins"):
            self.properties.chk_lod_twins.blockSignals(True)
            self.properties.chk_lod_twins.setChecked(bool(self.world.include_lod_twins))
            self.properties.chk_lod_twins.blockSignals(False)

    def _on_renderer_mode_changed(self, _index: int) -> None:
        rmode = str(self.cmb_renderer_mode.currentData() or "ps2")
        self.viewport.set_render_mode(rmode)
        label = "Plume HQ (native Vulkan)" if rmode == "plume" else "PS2 Fast (Original)"
        self.append_log(f"[Renderer] Switched to {label}", verbose=True)
        self.set_action_status(f"Renderer mode: {label}")

    def _toggle_grid_snap(self, on: bool) -> None:
        self.viewport.grid_snap_enabled = on
        self.set_action_status(f"Grid Snap ({self.viewport.grid_snap_step:.1f} m): {'ON' if on else 'OFF'}")

    def _toggle_angle_snap(self, on: bool) -> None:
        self.viewport.angle_snap_enabled = on
        self.set_action_status(f"Angle Snap ({self.viewport.angle_snap_step:.0f}°): {'ON' if on else 'OFF'}")

    def _toggle_wireframe(self, on: bool) -> None:
        self.viewport.show_wireframe = on
        self.viewport.update()

    def _toggle_collision(self, on: bool) -> None:
        self.viewport.show_collision = on
        self.viewport.mode_pill.btn_shield.blockSignals(True)
        self.viewport.mode_pill.btn_shield.setChecked(on)
        self.viewport.mode_pill.btn_shield.blockSignals(False)
        if "collision" in self.explorer.layer_checks:
            self.explorer.layer_checks["collision"].blockSignals(True)
            self.explorer.layer_checks["collision"].setChecked(on)
            self.explorer.layer_checks["collision"].blockSignals(False)
        self.viewport.update()

    def _sync_collision_toggles(self, on: bool) -> None:
        self.act_v_col.blockSignals(True)
        self.act_v_col.setChecked(on)
        self.act_v_col.blockSignals(False)
        if "collision" in self.explorer.layer_checks:
            self.explorer.layer_checks["collision"].blockSignals(True)
            self.explorer.layer_checks["collision"].setChecked(on)
            self.explorer.layer_checks["collision"].blockSignals(False)

    def _toggle_markers(self, on: bool) -> None:
        self.viewport.show_markers = on
        self.viewport.update()

    def _toggle_lighting(self, on: bool) -> None:
        self.viewport.show_lighting = on
        self.viewport.update()

    def _toggle_labels(self, on: bool) -> None:
        self.viewport.show_labels = on
        self.viewport.update()

    def _toggle_roads(self, on: bool) -> None:
        self.viewport.show_roads = on
        self.viewport.update()

    def _toggle_grid(self, on: bool) -> None:
        self.viewport.show_grid = on
        self.viewport.update()

    def _toggle_perf_hud(self, on: bool) -> None:
        self.viewport.set_perf_hud_visible(on)
        self.set_action_status(f"Performance Profiler HUD (F3): {'ON' if on else 'OFF'}")

    def _on_layer_toggled(self, key: str, visible: bool) -> None:
        if key == "roads":
            self.act_v_roads.setChecked(visible)
        elif key == "collision":
            self.act_v_col.setChecked(visible)
        elif key == "markers":
            self.act_v_markers.setChecked(visible)
        else:
            self.viewport.layer_visibility[key] = visible
            self.viewport.mark_scene_dirty()

    def _toggle_layer_by_key(self, key: str) -> None:
        cb = self.explorer.layer_checks.get(key)
        if cb is not None:
            cb.setChecked(not cb.isChecked())

    # -----------------------------------------------------------------------
    # Selection, Context Menu & Refresh
    # -----------------------------------------------------------------------
    def _refresh_after_edit(self) -> None:
        if self.world is not None:
            self._autosave_dirty = True
        self.viewport.mark_scene_dirty()
        self.viewport.mark_collision_dirty()
        self.explorer.refresh_counts()
        if self.viewport.selected_obj is not None and self.world is not None:
            sel_cnt = max(1, len([o for o in self.viewport.selected_objects if not o.deleted]))
            col_st = self.world.object_collision_status(self.viewport.selected_obj)
            self.properties.show_object(
                self.viewport.selected_obj, self.world.textures, selected_count=sel_cnt, collision_status=col_st
            )
        elif self.viewport.selected_road_node_id is not None and self.world is not None:
            node = self.world.road_graph.nodes.get(self.viewport.selected_road_node_id)
            self.properties.show_road_node(node, self.world.road_graph)
        elif self.viewport.selected_marker is not None:
            self.properties.show_marker(self.viewport.selected_marker)
        elif self.viewport.selected_barrier_idx >= 0 and self.world is not None:
            rec = self.world.collision.sobl_records[self.viewport.selected_barrier_idx]
            self.properties.show_barrier(rec)
        elif self.properties.stack.currentIndex() != 4:
            self.properties.show_nothing()

    def _on_viewport_obj_selected(self, obj: WorldObject | None) -> None:
        sel_cnt = max(1, len([o for o in self.viewport.selected_objects if not o.deleted]))
        col_st = self.world.object_collision_status(obj) if (self.world and obj is not None) else None
        self.properties.show_object(
            obj, self.world.textures if self.world else None, selected_count=sel_cnt, collision_status=col_st
        )
        if obj is not None and sel_cnt <= 1:
            self.set_action_status(f"Selected {obj.name} ({obj.source}, {obj.tri_count} tris)")

    def _on_viewport_selection_changed(self, objs: list[WorldObject]) -> None:
        active = [o for o in objs if o is not None and not o.deleted]
        if active:
            col_st = self.world.object_collision_status(active[-1]) if self.world else None
            self.properties.show_object(
                active[-1], self.world.textures if self.world else None, selected_count=len(active), collision_status=col_st
            )
            if len(active) > 1:
                self.set_action_status(f"Selected {len(active)} objects (primary: {active[-1].name})")
        elif (
            self.viewport.selected_road_node_id is None
            and self.viewport.selected_marker is None
            and self.viewport.selected_barrier_idx < 0
            and self.properties.stack.currentIndex() == 1
        ):
            self.properties.show_nothing()

    def _on_viewport_road_selected(self, node: RoadNode | None) -> None:
        if self.world is None:
            return
        self.properties.show_road_node(node, self.world.road_graph)
        if node is not None:
            kind = self.world.road_graph.node_kind(node.node_id)
            self.set_action_status(f"Selected Road node {node.node_id} ({kind})")

    def _on_viewport_marker_selected(self, marker: WorldMarker | None) -> None:
        self.properties.show_marker(marker)
        if marker is not None:
            self.set_action_status(f"Selected {marker.kind}: {marker.name}")

    def _on_viewport_barrier_selected(self, rec_idx: int) -> None:
        if self.world is None or rec_idx < 0 or rec_idx >= len(self.world.collision.sobl_records):
            return
        rec = self.world.collision.sobl_records[rec_idx]
        self.properties.show_barrier(rec)
        self.set_action_status(
            f"Selected SOBL Barrier #{rec_idx} ({rec['surface_name']}, +{rec['above_ground']:.1f} m above ground)"
        )

    def _show_viewport_context_menu(self, global_pos: QPoint) -> None:
        obj = self.viewport.selected_obj
        if obj is None or obj.deleted:
            return
        sel_cnt = len([o for o in self.viewport.selected_objects if not o.deleted])
        menu = QMenu(self)
        menu.addAction(svg_icon("replace", "#93c5fd"), "Replace Selection With...", lambda: self.replace_object_with_dialog(obj))
        menu.addAction("Select All Similar", lambda: self.select_similar_objects(obj))
        menu.addAction(svg_icon("trash", "#ff99a2"), "Delete All Similar", lambda: self.delete_similar_objects(obj))
        menu.addAction(svg_icon("ground"), "Snap to Ground (End / Ctrl+G)", lambda: self.snap_object_to_ground(obj))
        menu.addAction("Duplicate (Ctrl+D)", self.duplicate_selected_object)
        menu.addSeparator()
        menu.addAction(svg_icon("blender"), "Edit in Blender (OBJ)...", lambda: self.edit_object_in_blender(obj))
        menu.addAction(svg_icon("shield"), "Make Collision Box", lambda: self.make_collision_for_object(obj))
        menu.addSeparator()
        del_lbl = f"Delete Selected ({sel_cnt}) (Del)" if sel_cnt > 1 else "Delete (Del)"
        menu.addAction(svg_icon("trash", "#ff99a2"), del_lbl, self.delete_selected_item)
        menu.exec(global_pos)

    def _select_object_by_id(self, obj_id: str) -> None:
        if self.world is None:
            return
        obj = self.world.object_by_id(obj_id)
        if obj is not None:
            self.viewport.select_object(obj)
            self.viewport.frame_selection()

    def _select_marker_by_id(self, marker_id: str) -> None:
        if self.world is None:
            return
        mk = next((m for m in self.world.markers if m.marker_id == marker_id), None)
        if mk is not None:
            self.viewport.select_marker(mk)
            c = np.array(mk.position, dtype=np.float32)
            self.viewport.cam_pos = c + np.array([-22.0, 16.0, -22.0], dtype=np.float32)
            self.viewport.look_at(c)
            self.viewport.update()

    def _switch_road_line(self, line_name: str) -> None:
        if self.world is None or line_name not in self.world.all_road_graphs:
            return
        self.world.road_graph = self.world.all_road_graphs[line_name]
        self.world.custom_race.ilin_line = line_name
        self.viewport.selected_road_node_id = None
        self.viewport.road_chain_last_node = None
        self.viewport._roads_dirty = True
        self.viewport.update()
        if hasattr(self.explorer, "road_line_combo"):
            idx = self.explorer.road_line_combo.findData(line_name)
            if idx >= 0:
                self.explorer.road_line_combo.blockSignals(True)
                self.explorer.road_line_combo.setCurrentIndex(idx)
                self.explorer.road_line_combo.blockSignals(False)
        self.explorer.refresh_counts()
        self.set_action_status(f"Switched active ILIN line to {line_name} ({len(self.world.road_graph.nodes)} nodes)")

    # -----------------------------------------------------------------------
    # Explorer -> Ghost Place / Scatter / Replace Wiring
    # -----------------------------------------------------------------------
    def _on_explorer_model_selected(self, model_key: str) -> None:
        self.viewport.set_ghost_model(model_key)

    def _activate_ghost_place_for(self, model_key: str) -> None:
        self.viewport.set_ghost_model(model_key)
        self.select_tool("place")

    def _activate_scatter_for(self, model_key: str) -> None:
        self.viewport.set_ghost_model(model_key)
        self.select_tool("scatter")

    def _replace_selected_with_key(self, model_key: str) -> None:
        if self.viewport.selected_obj is None:
            self.set_action_status("Select an object in the 3D viewport first to swap it")
            return
        self.replace_object_model(self.viewport.selected_obj, model_key)

    # -----------------------------------------------------------------------
    # Object Operations (Move, Rotate, Scale, Replace With, Ghost, Scatter)
    # -----------------------------------------------------------------------
    def _on_viewport_obj_transformed(self, obj, old_p, old_r, old_s, new_p, new_r, new_s, label: str) -> None:
        self.undo_stack.push(
            TransformObjectCommand(obj, old_p, old_r, old_s, new_p, new_r, new_s, self._refresh_after_edit, label)
        )
        self.set_action_status(f"{label}: {obj.name}")

    def _on_viewport_multi_obj_transformed(self, entries: list[Any], label: str) -> None:
        if not entries:
            return
        self.undo_stack.push(MultiTransformObjectsCommand(entries, self._refresh_after_edit, label))
        self.set_action_status(f"{label}")

    def _on_alt_duplicate_completed(self, clones: list[WorldObject], label: str) -> None:
        """Persist Alt+Drag clones into TrackWorld via QUndoStack so they never vanish on deselect."""
        if self.world is None or not clones:
            return
        self.viewport.ensure_gl_textures()
        self.undo_stack.push(ScatterPlaceCommand(self.world, clones, self._refresh_after_edit, label=label))
        self.viewport.select_objects(clones)
        self.set_action_status(f"{label} ({clones[-1].name})")

    def _on_prop_obj_transform(self, obj: WorldObject, new_p, new_r, new_s) -> None:
        if obj.position == new_p and obj.rotation_hpr == new_r and obj.scale == new_s:
            return
        selected = [o for o in self.viewport.selected_objects if not o.deleted]
        if len(selected) > 1 and obj in selected:
            # Apply delta/ratio across entire selected compound assembly (e.g. ramp + top wear strip)
            dp = (new_p[0] - obj.position[0], new_p[1] - obj.position[1], new_p[2] - obj.position[2])
            dr = (new_r[0] - obj.rotation_hpr[0], new_r[1] - obj.rotation_hpr[1], new_r[2] - obj.rotation_hpr[2])
            sr = (
                (new_s[0] / obj.scale[0]) if abs(obj.scale[0]) > 1e-6 else 1.0,
                (new_s[1] / obj.scale[1]) if abs(obj.scale[1]) > 1e-6 else 1.0,
                (new_s[2] / obj.scale[2]) if abs(obj.scale[2]) > 1e-6 else 1.0,
            )
            pivot = np.array(obj.position, dtype=np.float32)
            entries = []
            for so in selected:
                op, orot, oscl = so.position, so.rotation_hpr, so.scale
                if so is obj:
                    entries.append((so, op, orot, oscl, new_p, new_r, new_s))
                else:
                    rel = np.array(op, dtype=np.float32) - pivot
                    scaled_p = pivot + rel * np.array(sr, dtype=np.float32) + np.array(dp, dtype=np.float32)
                    np_ = (float(scaled_p[0]), float(scaled_p[1]), float(scaled_p[2]))
                    nrot = ((orot[0] + dr[0]) % 360.0, (orot[1] + dr[1]) % 360.0, (orot[2] + dr[2]) % 360.0)
                    nscl = (oscl[0] * sr[0], oscl[1] * sr[1], oscl[2] * sr[2])
                    entries.append((so, op, orot, oscl, np_, nrot, nscl))
            self.undo_stack.push(
                MultiTransformObjectsCommand(entries, self._refresh_after_edit, f"Edit transform ({len(entries)} parts)")
            )
            self.set_action_status(f"Updated transform for {len(entries)} grouped parts ({obj.name})")
            return
        self.undo_stack.push(
            TransformObjectCommand(
                obj, obj.position, obj.rotation_hpr, obj.scale, new_p, new_r, new_s, self._refresh_after_edit, "Edit transform"
            )
        )
        self.set_action_status(f"Updated transform for {obj.name}")

    def replace_object_model(self, obj: WorldObject, new_model_key: str) -> bool:
        """Undoably swap `obj`'s model with any other model or Car/Wreck."""
        if self.world is None or new_model_key not in self.world.models or obj.deleted:
            return False
        self.viewport.ensure_gl_textures()
        self.undo_stack.push(ReplaceObjectModelCommand(self.world, obj, new_model_key, self._refresh_after_edit))
        self.set_action_status(f"Replaced object with {self.world.models[new_model_key].name}")
        return True

    def replace_object_with_dialog(self, obj: WorldObject) -> None:
        if self.world is None:
            return
        dlg = ReplaceModelDialog(self.world, self.explorer.thumb_cache, obj.name, parent=self)
        if dlg.exec() and dlg.selected_model_key:
            self.replace_object_model(obj, dlg.selected_model_key)

    def replace_selected_with_dialog(self) -> None:
        if self.viewport.selected_obj is not None:
            self.replace_object_with_dialog(self.viewport.selected_obj)

    def snap_object_to_ground(self, obj: WorldObject) -> None:
        if self.world is None or obj.deleted:
            return
        model = self.world.models.get(obj.model_key)
        if not model:
            return
        gx, gz = obj.position[0], obj.position[2]
        gy = self.world.collision.ground_height(gx, gz, below=obj.position[1] + 5.0)
        lo, _hi = obj.world_aabb(model)
        bottom_offset = obj.position[1] - float(lo[1])
        new_pos = (gx, gy + bottom_offset, gz)
        self.undo_stack.push(
            TransformObjectCommand(
                obj, obj.position, obj.rotation_hpr, obj.scale, new_pos, obj.rotation_hpr, obj.scale,
                self._refresh_after_edit, "Snap to ground",
            )
        )
        self.set_action_status(f"Snapped {obj.name} to ground (Y={new_pos[1]:.2f} m)")

    def snap_selected_to_ground(self) -> None:
        if self.viewport.selected_obj is not None:
            self.snap_object_to_ground(self.viewport.selected_obj)
        elif self.viewport.selected_marker is not None:
            self._snap_marker_to_ground(self.viewport.selected_marker)

    def _on_include_lod_twins_changed(self, checked: bool) -> None:
        if self.world is not None:
            self.world.include_lod_twins = bool(checked)
            self.world.custom_race.include_lod_twins = bool(checked)
        self.set_action_status(
            f"Include Overlapping LOD Twins & SOBL Barriers on delete: {'ON' if checked else 'OFF'}"
        )

    def select_similar_objects(self, seed: WorldObject | None = None) -> list[WorldObject]:
        if self.world is None:
            return []
        target = seed if isinstance(seed, WorldObject) else self.viewport.selected_obj
        if target is None or target.deleted:
            self.set_action_status("Select an object first to Select All Similar")
            return []
        matches = self.world.find_similar_objects(target)
        if matches:
            self.viewport.select_objects(matches)
            self.set_action_status(f"Selected {len(matches)} similar object(s) matching {target.name}")
        return matches

    def delete_objects_with_cleanup(
        self,
        targets: list[WorldObject],
        label: str | None = None,
    ) -> tuple[int, int]:
        """Undoably delete `targets` via MultiDeleteObjectsCommand + optional LOD twins & SOBL barriers."""
        if self.world is None:
            return (0, 0)
        clean = [o for o in targets if o is not None and not o.deleted]
        if not clean:
            return (0, 0)
        all_objs = list(clean)
        barrier_indices: list[int] = []
        if self.world.include_lod_twins:
            twins, barrier_indices = self.world.find_overlapping_lod_twins_and_barriers(clean)
            all_objs.extend(twins)
        cmd = MultiDeleteObjectsCommand(
            self.world,
            all_objs,
            barrier_indices=barrier_indices,
            on_change=self._refresh_after_edit,
            label=label,
        )
        self.undo_stack.push(cmd)
        self.viewport.select_object(None)
        del_cnt = len(cmd.objects)
        bar_cnt = len(cmd.barrier_indices)
        extra = f" + flattened {bar_cnt} overlapping SOBL barrier(s)" if bar_cnt else ""
        msg = f"Deleted {del_cnt} object(s){extra}"
        self.set_action_status(msg)
        self.append_log(f"[Edit] {msg}", verbose=True)
        return (del_cnt, bar_cnt)

    def delete_similar_objects(self, seed: WorldObject | None = None) -> tuple[int, int]:
        if self.world is None:
            return (0, 0)
        target = seed if isinstance(seed, WorldObject) else self.viewport.selected_obj
        if target is None or target.deleted:
            self.set_action_status("Select an object first to Delete All Similar")
            return (0, 0)
        matches = self.world.find_similar_objects(target) or [target]
        return self.delete_objects_with_cleanup(
            matches, label=f"Delete {len(matches)} similar ({target.name})"
        )

    def _on_prop_delete_object(self, obj: WorldObject) -> None:
        sel = [o for o in self.viewport.selected_objects if not o.deleted]
        if len(sel) > 1 and any(o.obj_id == obj.obj_id for o in sel):
            self.delete_objects_with_cleanup(sel)
        else:
            self.delete_objects_with_cleanup([obj])

    def delete_object(self, obj: WorldObject) -> None:
        self.undo_stack.push(DeleteObjectCommand(obj, self._refresh_after_edit))
        self.viewport.select_object(None)
        self.set_action_status(f"Deleted {obj.name}")

    def delete_selected_item(self) -> None:
        sel = [o for o in self.viewport.selected_objects if not o.deleted]
        if sel:
            self.delete_objects_with_cleanup(sel)
        elif self.viewport.selected_obj is not None and not self.viewport.selected_obj.deleted:
            self.delete_objects_with_cleanup([self.viewport.selected_obj])
        elif self.viewport.selected_road_node_id is not None:
            self._road_delete_node(self.viewport.selected_road_node_id)
        elif self.viewport.selected_marker is not None:
            self._delete_marker(self.viewport.selected_marker)

    def duplicate_selected_object(self) -> None:
        if self.world is None or self.viewport.selected_obj is None:
            return
        src = self.viewport.selected_obj
        pos = (src.position[0] + 4.0, src.position[1], src.position[2] + 4.0)
        self._place_model_at(src.model_key, pos[0], pos[1], pos[2], rot=src.rotation_hpr, scale=src.scale)

    def _build_placed_object(
        self,
        model_key: str,
        x: float,
        y: float,
        z: float,
        rot: tuple[float, float, float] = (0.0, 0.0, 0.0),
        scale: tuple[float, float, float] = (1.0, 1.0, 1.0),
    ) -> WorldObject | None:
        if self.world is None or model_key not in self.world.models:
            return None
        wm = self.world.models[model_key]
        obj_id = f"ed_{self._placed_counter:04d}"
        self._placed_counter += 1
        y_adj = y - float(wm.bbox_min[1]) * scale[1]
        cat = wm.category if wm.category != "prop-catalog" else "dyno"
        return WorldObject(
            obj_id=obj_id,
            name=wm.name,
            model_key=model_key,
            source="Placed by the editor",
            category=cat,
            subtype=wm.subtype,
            section_mlist="editor_layer",
            model_store=wm.model_store,
            hash_hex=wm.hash_hex,
            tri_count=wm.tri_count,
            textures=list(wm.textures),
            position=(x, y_adj, z),
            rotation_hpr=rot,
            scale=scale,
            written=False,
        )

    def _place_model_in_front_of_camera(self, model_key: str) -> None:
        if self.world is None:
            return
        fwd, _, _ = self.viewport.camera_vectors()
        p = self.viewport.cam_pos + fwd * 28.0
        gy = self.world.collision.ground_height(float(p[0]), float(p[2]))
        self._place_model_at(model_key, float(p[0]), gy, float(p[2]))

    def _place_model_at(
        self,
        model_key: str,
        x: float,
        y: float,
        z: float,
        rot: tuple[float, float, float] = (0.0, 0.0, 0.0),
        scale: tuple[float, float, float] = (1.0, 1.0, 1.0),
    ) -> WorldObject | None:
        obj = self._build_placed_object(model_key, x, y, z, rot=rot, scale=scale)
        if obj is None or self.world is None:
            return None
        self.viewport.ensure_gl_textures()
        self.undo_stack.push(PlaceObjectCommand(self.world, obj, self._refresh_after_edit))
        self.viewport.select_object(obj)
        self.set_action_status(f"Placed {obj.name} at ({obj.position[0]:.1f}, {obj.position[1]:.1f}, {obj.position[2]:.1f})")
        return obj

    def _on_ghost_stamped(self, model_key: str, pos: tuple[float, float, float], rot: tuple[float, float, float], scale: tuple[float, float, float]) -> None:
        obj = self._build_placed_object(model_key, pos[0], pos[1], pos[2], rot=rot, scale=scale)
        if obj is None or self.world is None:
            return
        self.viewport.ensure_gl_textures()
        self.undo_stack.push(PlaceObjectCommand(self.world, obj, self._refresh_after_edit))
        self.set_action_status(f"Stamped {obj.name} at ({pos[0]:.1f}, {pos[1]:.1f}, {pos[2]:.1f})")

    def _on_scatter_stroke(self, items: list[dict[str, Any]]) -> list[WorldObject]:
        if self.world is None or not items:
            return []
        objs: list[WorldObject] = []
        for it in items:
            px, py, pz = it["pos"]
            o = self._build_placed_object(it["model_key"], px, py, pz, rot=tuple(it["rot_hpr"]), scale=tuple(it["scale"]))
            if o is not None:
                objs.append(o)
        if objs:
            self.viewport.ensure_gl_textures()
            self.undo_stack.push(ScatterPlaceCommand(self.world, objs, self._refresh_after_edit))
            self.set_action_status(f"Scattered {len(objs)} instances of {objs[0].name}")
        return objs

    def make_collision_for_object(self, obj: WorldObject) -> None:
        if self.world is None:
            return
        self.undo_stack.push(MakeCollisionCommand(self.world, obj, self._refresh_after_edit))
        self.act_v_col.setChecked(True)
        self.set_action_status(f"Generated collision box for {obj.name}")

    def make_collision_for_selected(self) -> None:
        if self.viewport.selected_obj is not None:
            self.make_collision_for_object(self.viewport.selected_obj)

    def _camera_ground_target(self, dist: float = 30.0) -> tuple[float, float, float]:
        assert self.world is not None
        fwd, _, _ = self.viewport.camera_vectors()
        p = self.viewport.cam_pos + fwd * float(dist)
        tx, tz = float(p[0]), float(p[2])
        ty = float(self.world.collision.ground_height(tx, tz))
        return (tx, ty, tz)

    def _pick_procedural_textures(self) -> tuple[str, str, str, str, str]:
        """Resolve authentic track textures for procedural roads/islands with safe retail defaults."""
        defaults = ("rd-1.tex", "af_rdwr.tex", "wall01.tex", "conc01.tex", "W_dirt.tex")
        if self.world is None or not self.world.textures:
            return defaults
        by_low = {name.lower(): name for name in self.world.textures}

        def _pick(preferred: str, tokens: tuple[str, ...]) -> str:
            if preferred.lower() in by_low:
                return by_low[preferred.lower()]
            for tok in tokens:
                for low, orig in by_low.items():
                    if tok in low:
                        return orig
            return preferred

        return (
            _pick("rd-1.tex", ("rd-", "road", "asph", "tar", "pave")),
            _pick("af_rdwr.tex", ("rdwr", "wear", "skid", "track")),
            _pick("wall01.tex", ("wall", "barr", "guard", "conc")),
            _pick("conc01.tex", ("conc", "brdg", "bridge", "wall")),
            _pick("W_dirt.tex", ("dirt", "grass", "terr", "mud", "ground")),
        )

    @staticmethod
    def _wall_box_to_custom_box(box: dict[str, Any]) -> dict[str, Any]:
        """Attach world AABB (min/max) to an OBB wall box so viewport wireframe and SOBL baker both consume it."""
        c = np.asarray(box["centre"], dtype=np.float64)
        r = np.asarray(box["radii"], dtype=np.float64)
        ax = np.asarray(box["axes"], dtype=np.float64)
        half_ext = np.abs(ax[0]) * r[0] + np.abs(ax[1]) * r[1] + np.abs(ax[2]) * r[2]
        out = dict(box)
        out["min"] = [float(v) for v in (c - half_ext)]
        out["max"] = [float(v) for v in (c + half_ext)]
        return out

    def _commit_built_road(self, built: BuiltRoad) -> WorldObject | None:
        if self.world is None:
            return None
        wm = built.model
        self.world.models[wm.key] = wm
        self.world.procedural_roads.append(built.plan.to_dict())

        opened = open_eod_junction(
            self.world.collision.sobl_records,
            self.world.collision.sobl_edits,
            built.junction_mouths,
        )
        if opened:
            self.world.collision.rebuild_sobl_lines()

        for box in built.wall_boxes:
            self.world.collision.custom_boxes.append(self._wall_box_to_custom_box(box))

        obj_id = f"ed_{self._placed_counter:04d}"
        self._placed_counter += 1
        pivot = tuple(float(v) for v in wm.pivot)
        obj = WorldObject(
            obj_id=obj_id,
            name=wm.name,
            model_key=wm.key,
            source="Placed by the editor",
            category=wm.category,
            subtype=wm.subtype,
            section_mlist="editor_layer",
            model_store=wm.model_store,
            hash_hex=wm.hash_hex,
            tri_count=wm.tri_count,
            textures=list(wm.textures),
            position=pivot,
            rotation_hpr=(0.0, 0.0, 0.0),
            scale=(1.0, 1.0, 1.0),
            written=False,
        )
        self.viewport.ensure_gl_textures()
        self.undo_stack.push(PlaceObjectCommand(self.world, obj, self._refresh_after_edit))
        self.viewport.select_object(obj)
        self.set_action_status(
            f"Built 3D road '{wm.name}' ({wm.tri_count} tris, {len(built.wall_boxes)} barrier boxes, {len(opened)} junctions opened)"
        )
        return obj

    def build_procedural_road_preset(self, kind: str) -> WorldObject | None:
        """Build a procedural 3D road preset (oval, figure8, ramp, bridge) grounded at the camera target."""
        if self.world is None:
            return None
        tx, ty, tz = self._camera_ground_target(30.0)
        seq = len(self.world.procedural_roads) + 1
        k = kind.lower().strip()
        if k == "oval":
            plan = make_banked_oval_plan(center=(tx, ty, tz))
            plan.name = f"banked_oval_{seq:02d}"
        elif k == "figure8":
            plan = make_figure8_plan(center=(tx, ty, tz))
            plan.name = f"figure8_{seq:02d}"
        elif k == "ramp":
            plan = make_jump_ramp_plan(start=(tx, ty, tz))
            plan.name = f"jump_ramp_{seq:02d}"
        elif k == "bridge":
            end_x, end_z = tx, tz + 160.0
            end_y = float(self.world.collision.ground_height(end_x, end_z))
            plan = make_bridge_plan(start=(tx, ty, tz), end=(end_x, end_y, end_z))
            plan.name = f"highway_bridge_{seq:02d}"
        else:
            raise ValueError(f"Unknown procedural road preset: {kind!r}")

        surf_t, wear_t, barr_t, deck_t, _dirt_t = self._pick_procedural_textures()
        plan.surface_tex = surf_t
        plan.wear_tex = wear_t
        plan.barrier_tex = barr_t
        plan.deck_tex = deck_t
        built = build_road_mesh(plan, ground_fn=self.world.collision.ground_height)
        return self._commit_built_road(built)

    def extrude_road_from_active_ilin(self) -> WorldObject | None:
        """Extrude a full 3D road mesh from the active ILIN racing-line spline."""
        if self.world is None or not self.world.road_graph.nodes:
            return None
        g = self.world.road_graph
        out_adj: dict[int, list[int]] = {}
        for e in g.edges:
            if e.a in g.nodes and e.b in g.nodes and e.a != e.b:
                out_adj.setdefault(e.a, []).append(e.b)

        start_id = min(g.nodes.keys())
        order: list[int] = []
        visited: set[int] = set()
        cur: int | None = start_id
        closed = False
        while cur is not None and cur not in visited:
            order.append(cur)
            visited.add(cur)
            nxts = out_adj.get(cur, [])
            nxt = next((nid for nid in nxts if nid not in visited), None)
            if nxt is None and len(order) >= 3 and start_id in nxts:
                closed = True
            cur = nxt

        if len(order) < 2:
            order = sorted(g.nodes.keys())
            closed = False

        pts: list[RoadControlPoint] = []
        for nid in order:
            nd = g.nodes[nid]
            y = float(nd.y)
            if abs(y) < 1e-4:
                y = float(self.world.collision.ground_height(float(nd.x), float(nd.z)))
            pts.append(
                RoadControlPoint(
                    x=float(nd.x),
                    y=y,
                    z=float(nd.z),
                    width=max(8.0, float(nd.width or 14.0)),
                )
            )
        if len(pts) < 2:
            return None

        seq = len(self.world.procedural_roads) + 1
        surf_t, wear_t, barr_t, deck_t, _dirt_t = self._pick_procedural_textures()
        line_slug = re.sub(r"[^A-Za-z0-9_]+", "_", Path(g.line_name).stem).strip("_") or "ilin"
        plan = RoadPlan(
            name=f"{line_slug}_road_{seq:02d}",
            points=pts,
            closed=closed,
            surface_tex=surf_t,
            wear_tex=wear_t,
            barrier_tex=barr_t,
            deck_tex=deck_t,
            style="ilin_extrude",
        )
        built = build_road_mesh(plan, ground_fn=self.world.collision.ground_height)
        return self._commit_built_road(built)

    def build_procedural_island(self) -> WorldObject | None:
        """Build an organic 3D terrain dome island grounded at the camera target."""
        if self.world is None:
            return None
        tx, ty, tz = self._camera_ground_target(40.0)
        seq = len(self.world.procedural_roads) + 1
        name = f"terrain_island_{seq:02d}"
        _surf_t, _wear_t, _barr_t, _deck_t, dirt_t = self._pick_procedural_textures()
        entry = {
            "kind": "island",
            "name": name,
            "model_key": f"island:{name}",
            "center_xz": [tx, tz],
            "ground_y": ty,
            "radius_x": 36.0,
            "radius_z": 28.0,
            "height": 6.0,
            "rings": 8,
            "sectors": 24,
            "seed": seq,
            "tex": dirt_t,
        }
        wm, _ = rebuild_procedural_entry(self.world, entry)
        if wm is None:
            return None
        self.world.procedural_roads.append(entry)

        obj_id = f"ed_{self._placed_counter:04d}"
        self._placed_counter += 1
        pivot = tuple(float(v) for v in wm.pivot)
        obj = WorldObject(
            obj_id=obj_id,
            name=wm.name,
            model_key=wm.key,
            source="Placed by the editor",
            category=wm.category,
            subtype=wm.subtype,
            section_mlist="editor_layer",
            model_store=wm.model_store,
            hash_hex=wm.hash_hex,
            tri_count=wm.tri_count,
            textures=list(wm.textures),
            position=pivot,
            rotation_hpr=(0.0, 0.0, 0.0),
            scale=(1.0, 1.0, 1.0),
            written=False,
        )
        self.viewport.ensure_gl_textures()
        self.undo_stack.push(PlaceObjectCommand(self.world, obj, self._refresh_after_edit))
        self.viewport.select_object(obj)
        self.set_action_status(f"Built organic terrain island '{wm.name}' ({wm.tri_count} tris)")
        return obj

    def edit_object_in_blender(self, obj: WorldObject) -> None:
        if self.world is None:
            return
        export_dir = tdeod_support_dir() / "editor" / "blender_exports" / self.world.track_name.lower()
        try:
            obj_path = export_object_obj(self.world, obj, export_dir)
            self.set_action_status(f"Exported Blender OBJ -> {obj_path}")
            QMessageBox.information(
                self,
                "Edit in Blender",
                f"Exported mesh + MTL + textures (Blender RH Z-up):\n\n{obj_path}\n\n"
                f"Open this OBJ directly in Blender to inspect or edit geometry.",
            )
        except Exception as exc:
            QMessageBox.critical(self, "Blender Export Failed", str(exc))

    def edit_selected_in_blender(self) -> None:
        if self.viewport.selected_obj is not None:
            self.edit_object_in_blender(self.viewport.selected_obj)

    # -----------------------------------------------------------------------
    # Live Texture Replacement (OpenGL 3.3 + Vulkan HOSTTEX Export)
    # -----------------------------------------------------------------------
    def _on_texture_swapped(self, tex_name: str) -> None:
        self._on_textures_swapped([tex_name])

    def _on_textures_swapped(self, names: list[str]) -> None:
        self._autosave_dirty = True
        for tex_name in names:
            self.viewport.update_gl_texture(tex_name)
        if self.viewport.selected_obj is not None and self.world is not None:
            col_st = self.world.object_collision_status(self.viewport.selected_obj)
            self.properties.show_object(
                self.viewport.selected_obj, self.world.textures,
                selected_count=max(1, len(self.viewport.selected_objects)), collision_status=col_st,
            )

    def export_texture_folder_dialog(self) -> None:
        if self.world is None:
            return
        directory = QFileDialog.getExistingDirectory(self, "Export Track Textures", str(Path.home()))
        if not directory:
            return
        try:
            manifest = export_all_track_textures(self.world, directory)
        except (OSError, ValueError) as exc:
            QMessageBox.critical(self, "Export Track Textures Failed", str(exc))
            return
        self.set_action_status(f"Exported {manifest['count']} track textures and manifest.json to {directory}")

    def import_texture_folder_dialog(self) -> None:
        if self.world is None:
            return
        directory = QFileDialog.getExistingDirectory(self, "Import Track Texture Folder", str(Path.home()))
        if not directory:
            return
        try:
            replacements, skipped = scan_import_texture_folder(self.world, directory)
        except OSError as exc:
            QMessageBox.critical(self, "Import Track Textures Failed", str(exc))
            return
        for reason in skipped:
            self.log_text.appendPlainText(f"[Textures] Skipped {reason}")
        if not replacements:
            self.set_action_status(f"No matching track textures imported ({len(skipped)} skipped)")
            return
        self.undo_stack.push(BatchReplaceTexturesCommand(self.world, replacements, self._on_textures_swapped))
        self.set_action_status(f"Imported {len(replacements)} track textures ({len(skipped)} skipped). Write Mod exports HOSTTEX.")

    def replace_texture_from_png(self, tex_name: str, png_path: Path | str) -> bool:
        """Load `png_path` and undoably replace `tex_name` live in OpenGL + HOSTTEX queue."""
        if self.world is None or tex_name not in self.world.textures:
            return False
        w, h, rgba = load_png_rgba(png_path)
        self.undo_stack.push(
            ReplaceTextureCommand(
                self.world, tex_name, rgba, w, h, str(Path(png_path).resolve()), self._on_texture_swapped
            )
        )
        self.set_action_status(f"Replaced texture {tex_name} with {Path(png_path).name} ({w}x{h}) — Ctrl+Shift+S exports HOSTTEX")
        return True

    def replace_texture_dialog(self, tex_name: str) -> None:
        if self.world is None or tex_name not in self.world.textures:
            return
        fn, _ = QFileDialog.getOpenFileName(
            self, f"Replace Texture '{tex_name}' with PNG", str(self.game_dir), "Images (*.png *.jpg *.bmp *.tga)"
        )
        if not fn:
            return
        try:
            self.replace_texture_from_png(tex_name, fn)
        except Exception as exc:
            QMessageBox.critical(self, "Replace Texture Failed", str(exc))

    def reset_texture_to_retail(self, tex_name: str) -> None:
        if self.world is None or tex_name not in self.world.textures:
            return
        ta = self.world.textures[tex_name]
        if not ta.is_replaced:
            return
        self.undo_stack.push(
            ReplaceTextureCommand(
                self.world, tex_name, ta.orig_rgba, ta.orig_width, ta.orig_height, None, self._on_texture_swapped
            )
        )
        self.set_action_status(f"Reset texture {tex_name} back to retail")

    # -----------------------------------------------------------------------
    # Environment / Skybox & SOBL Barrier Removal
    # -----------------------------------------------------------------------
    def open_environment_panel(self) -> None:
        if self.world is None:
            return
        self._request_side_panel("inspector", True)
        stems = [p.stem.upper() for p in self.available_tracks]
        self.properties.show_environment(self.world.environment, stems)
        self.set_action_status(f"Environment / Skybox inspector active ({self.world.environment.preset_name})")

    def _on_environment_changed(self) -> None:
        if self.world is None:
            return
        self._autosave_dirty = True
        if self.properties.stack.currentIndex() == 4:
            self.properties.show_environment(self.world.environment)
        self.viewport.update()
        b = self.world.environment.borrowed_sky_track
        extra = f" · Borrowed sky: {b}" if b else ""
        self.set_action_status(f"Atmosphere updated: {self.world.environment.preset_name}{extra}")

    def _edit_environment(self, proposed: EnvironmentState) -> None:
        if self.world is None or proposed == self.world.environment:
            return
        self.undo_stack.push(EditEnvironmentCommand(
            self.world, self.world.environment.to_dict(), proposed.to_dict(), self._on_environment_changed
        ))

    def apply_barrier_height(self, rec_idx: int, target_top: float) -> None:
        if self.world is None or not (0 <= rec_idx < len(self.world.collision.sobl_records)):
            return
        self.undo_stack.push(
            ModifyBarrierCommand(
                self.world, {rec_idx: float(target_top)}, self._refresh_after_edit, f"Set barrier #{rec_idx} height"
            )
        )
        self.act_v_col.setChecked(True)
        self.set_action_status(f"Updated SOBL barrier #{rec_idx} top to {target_top:+.2f} m above ground")

    def clamp_sky_walls_to_assets(self) -> None:
        if self.world is None:
            return
        targets, counts = plan_clamp_sky_walls_to_assets(self.world)
        if not targets:
            self.set_action_status("No sky-walls to clamp")
            return
        self.undo_stack.push(ModifyBarrierCommand(self.world, targets, self._refresh_after_edit, "Clamp sky-walls"))
        self.act_v_col.setChecked(True)
        self.set_action_status(
            f"Clamped {counts['clamped_to_asset']} sky-walls to visual height and lowered "
            f"{counts['flattened_air']} barriers with no visible asset"
        )

    def _sync_walls_noclip_action(self) -> None:
        self.act_walls_noclip.setEnabled(self.world is not None)
        self.act_walls_noclip.setChecked(bool(self.world and self.world.all_walls_noclip))

    def _set_walls_noclip(self, enabled: bool) -> None:
        if self.world is None or self.world.all_walls_noclip == enabled:
            return
        self.undo_stack.push(SetAllWallsNoclipCommand(self.world, enabled, self._on_walls_noclip_changed))

    def _on_walls_noclip_changed(self) -> None:
        self._autosave_dirty = True
        self._sync_walls_noclip_action()
        state = "enabled" if self.act_walls_noclip.isChecked() else "disabled"
        self.set_action_status(f"Walls Noclip {state} for PIE and mod export")

    def flatten_all_track_barriers(self, target_top: float = -0.5) -> int:
        """One-click undoable removal/flattening of all vertical SOBL invisible walls on the track."""
        if self.world is None:
            return 0
        targets: dict[int, float] = {}
        for rec in self.world.collision.sobl_records:
            if rec.get("kind") == "box" and rec.get("vertical") and float(rec.get("above_ground", 0.0)) > target_top:
                targets[int(rec["index"])] = float(target_top)
        if not targets:
            self.set_action_status("No vertical SOBL barriers above ground to flatten")
            return 0
        self.undo_stack.push(
            ModifyBarrierCommand(
                self.world, targets, self._refresh_after_edit, f"Flatten {len(targets)} track barriers"
            )
        )
        self.act_v_col.setChecked(True)
        self.set_action_status(f"Flattened {len(targets)} SOBL track barriers (invisible walls removed — Ctrl+Shift+S writes track.sol)")
        return len(targets)

    def _show_tallest_barriers(self) -> None:
        if self.world is None:
            return
        boxes = [r for r in self.world.collision.sobl_records if r.get("kind") == "box"]
        tall = sorted(boxes, key=lambda r: -float(r.get("above_ground", 0.0)))[:12]
        lines = [
            f"  • Barrier #{r['index']:4d} ({r['surface_name']}): +{r['above_ground']:.2f} m above ground (ry={r['half_extents'][1]:.2f} m)"
            for r in tall
        ]
        tall10 = sum(1 for r in boxes if float(r.get("above_ground", 0.0)) >= 10.0)
        QMessageBox.information(
            self,
            "Tallest SOBL Track Barriers",
            f"Track: {self.world.display_name}\n"
            f"Total SOBL boxes: {len(boxes)}  |  Tall invisible walls (>=10 m): {tall10}\n\n"
            + ("\n".join(lines) if lines else "No SOBL boxes found.")
            + "\n\nToggle Collision Overlay and click a barrier to edit it, or use Clamp Sky-Walls to Asset Height.",
        )

    # -----------------------------------------------------------------------
    # Custom Race & AI Pathfinding (ILIN) + Vehicle / NASCAR Tuner Dialogs
    # -----------------------------------------------------------------------
    def open_custom_race_dialog(self) -> None:
        if self.world is None:
            return
        dlg = CustomRaceDialog(self.world, parent=self)
        if dlg.exec():
            cfg = self.world.custom_race
            if cfg.ilin_line in self.world.all_road_graphs and cfg.ilin_line != self.world.road_graph.line_name:
                self._switch_road_line(cfg.ilin_line)
            if cfg.event_type in self.world.event_templates:
                self.viewport.set_event_template(cfg.event_type)
            self._sync_pie_pickers_from_world()
            self.set_action_status(
                f"Custom Race saved: {cfg.event_name} ({cfg.event_type}, {cfg.laps} laps, {cfg.ai_count} AI, "
                f"car={cfg.player_car}, arcade={cfg.arcade_mode}, sync_all={self.world.sync_all_events})"
            )
            if dlg.launch_pie_after:
                self.start_pie("event")

    def open_vehicle_tuner_dialog(self) -> VehicleTunerDialog:
        """Open Live PIE Vehicle Boost / Handling Tuner & NASCAR / Custom Car Tools dialog."""
        if self._vehicle_tuner_dlg is not None and self._vehicle_tuner_dlg.isVisible():
            self._vehicle_tuner_dlg.raise_()
            self._vehicle_tuner_dlg.activateWindow()
            return self._vehicle_tuner_dlg

        def _on_boost(mult: float) -> None:
            self.pie_host.write_live_control(speed_boost=round(float(mult), 3))
            self.set_action_status(f"Live PIE Speed Boost: {mult:.2f}x")
            self.append_log(f"[VehicleTuner] Live PIE speed_boost={mult:.2f}x", verbose=True)

        dlg = VehicleTunerDialog(self.game_dir, on_live_boost_changed=_on_boost, parent=self)
        dlg.nascar.testDriveRequested.connect(self._test_drive_registered_vehicle)
        if hasattr(dlg, "handling") and hasattr(dlg.handling, "archiveSaved"):
            dlg.handling.archiveSaved.connect(self._on_car_handling_saved)
        self._vehicle_tuner_dlg = dlg
        dlg.show()
        dlg.raise_()
        dlg.activateWindow()
        return dlg

    def _on_car_handling_saved(self, archive_path: str) -> None:
        p = Path(archive_path)
        if p.is_file():
            self._custom_car_archives = [x for x in self._custom_car_archives if x.name.lower() != p.name.lower()]
            self._custom_car_archives.append(p)
            self.append_log(f"[CarStudio] Registered tuned car archive for PIE/Export: {p.name}")
            self.set_action_status(f"Tuned car ready for PIE & Export: {p.name}")

    def _test_drive_registered_vehicle(self, vehicle: RegisteredVehicle) -> None:
        if self.world is not None:
            self.start_pie("free_roam", vehicle=vehicle)
            return
        tracks = self.available_tracks
        if not tracks:
            QMessageBox.information(self, "Test Drive", "Set the EOD game data directory to choose a test track.")
            return
        names = [track.stem for track in tracks]
        name, accepted = QInputDialog.getItem(self, "Test Drive", "Track", names, 0, False)
        if accepted:
            track = tracks[names.index(name)]
            self.load_track_async(track)
            self._pending_vehicle_drive = (track, vehicle)

    # -----------------------------------------------------------------------
    # Road Tool / ILIN Node Operations (Undoable)
    # -----------------------------------------------------------------------
    def _on_viewport_road_moved(self, node: RoadNode, old_pos, new_pos) -> None:
        if self.world is None:
            return
        self.undo_stack.push(MoveRoadNodeCommand(self.world.road_graph, node, old_pos, new_pos, self._refresh_after_edit))
        self.set_action_status(f"Moved Road node {node.node_id} to ({new_pos[0]:.1f}, {new_pos[1]:.1f}, {new_pos[2]:.1f})")

    def _on_prop_road_pos(self, node: RoadNode, new_pos) -> None:
        if self.world is None:
            return
        old_pos = (node.x, node.y, node.z)
        if old_pos == new_pos:
            return
        self.undo_stack.push(MoveRoadNodeCommand(self.world.road_graph, node, old_pos, new_pos, self._refresh_after_edit))
        self.set_action_status(f"Moved Road node {node.node_id}")

    def _on_road_action(self, action: str, data: dict[str, Any]) -> None:
        if self.world is None:
            return
        g = self.world.road_graph
        if action == "add":
            x, y, z = data["pos"]
            cmd = AddRoadNodeCommand(
                g, x, y, z, data.get("connect_from"),
                lanes=int(data.get("lanes", 2)),
                width=float(data.get("width", 14.0)),
                two_way=False,
                on_change=self._refresh_after_edit,
            )
            self.undo_stack.push(cmd)
            self.viewport.road_chain_last_node = cmd.node.node_id
            self.viewport.select_road_node(cmd.node.node_id)
            self.set_action_status(f"Added Road node {cmd.node.node_id}")
        elif action == "split":
            edge_idx = int(data["edge_index"])
            if 0 <= edge_idx < len(g.edges):
                x, y, z = data["pos"]
                cmd = SplitRoadEdgeCommand(g, edge_idx, x, y, z, self._refresh_after_edit)
                self.undo_stack.push(cmd)
                self.viewport.road_chain_last_node = cmd.node.node_id
                self.viewport.select_road_node(cmd.node.node_id)
                self.set_action_status(f"Split road segment -> Road node {cmd.node.node_id}")
        elif action == "join":
            a, b = int(data["a"]), int(data["b"])
            cmd = JoinRoadNodesCommand(
                g, a, b, int(data.get("lanes", 2)), float(data.get("width", 14.0)), False, self._refresh_after_edit
            )
            self.undo_stack.push(cmd)
            self.set_action_status(f"Joined Road node {a} -> {b}")

    def _road_pull_out(self, node_id: int) -> None:
        if self.world is None or node_id not in self.world.road_graph.nodes:
            return
        g = self.world.road_graph
        n = g.nodes[node_id]
        dx, _dy, dz = g.node_direction(node_id)
        nx = n.x + (dx * 0.7 - dz * 0.7) * 18.0
        nz = n.z + (dz * 0.7 + dx * 0.7) * 18.0
        ny = self.world.collision.ground_height(nx, nz) + 0.35
        cmd = AddRoadNodeCommand(g, nx, ny, nz, node_id, n.lanes, n.width, n.two_way, self._refresh_after_edit)
        self.undo_stack.push(cmd)
        self.viewport.road_chain_last_node = cmd.node.node_id
        self.viewport.select_road_node(cmd.node.node_id)
        self.set_action_status(f"Pulled new road branch -> Road node {cmd.node.node_id}")

    def _road_delete_node(self, node_id: int) -> None:
        if self.world is None or node_id not in self.world.road_graph.nodes:
            return
        self.undo_stack.push(DeleteRoadNodeCommand(self.world.road_graph, node_id, self._refresh_after_edit))
        if self.viewport.road_chain_last_node == node_id:
            self.viewport.road_chain_last_node = None
        self.viewport.select_road_node(None)
        self.set_action_status(f"Deleted Road node {node_id}")

    def _road_draw_from_here(self, node_id: int) -> None:
        self.select_tool("road")
        self.viewport.road_chain_last_node = node_id
        self.set_action_status(f"Drawing road from node {node_id} — click terrain to extend")

    def _road_lane_delta(self, node_id: int, delta: int) -> None:
        if self.world is None or node_id not in self.world.road_graph.nodes:
            return
        self.undo_stack.push(
            ModifyRoadLanesCommand(self.world.road_graph, node_id, delta_lanes=delta, on_change=self._refresh_after_edit)
        )
        self.set_action_status(f"Adjusted lanes at Road node {node_id}")

    def _road_split_exit(self, node_id: int) -> None:
        if self.world is None or node_id not in self.world.road_graph.nodes:
            return
        g = self.world.road_graph
        n = g.nodes[node_id]
        dx, _dy, dz = g.node_direction(node_id)
        nx = n.x + (dx * 0.8 + dz * 0.6) * 20.0
        nz = n.z + (dz * 0.8 - dx * 0.6) * 20.0
        ny = self.world.collision.ground_height(nx, nz) + 0.35
        cmd = AddRoadNodeCommand(g, nx, ny, nz, node_id, max(1, n.lanes - 1), 8.5, False, self._refresh_after_edit)
        self.undo_stack.push(cmd)
        self.viewport.select_road_node(cmd.node.node_id)
        self.set_action_status(f"Split exit ramp from Road node {node_id} -> {cmd.node.node_id}")

    def _road_split_two_way(self, node_id: int) -> None:
        if self.world is None or node_id not in self.world.road_graph.nodes:
            return
        n = self.world.road_graph.nodes[node_id]
        self.undo_stack.push(
            ModifyRoadLanesCommand(
                self.world.road_graph, node_id, delta_lanes=0, two_way=not n.two_way, on_change=self._refresh_after_edit
            )
        )
        self.set_action_status(f"Toggled two-way traffic at Road node {node_id}")

    # -----------------------------------------------------------------------
    # Marker / Spawn / Start Grid / Trigger Operations (Undoable + 3D Gizmos)
    # -----------------------------------------------------------------------
    def add_marker_at_camera(self, kind: str, prefix: str | None = None) -> WorldMarker | None:
        if self.world is None:
            return None
        fwd, _, _ = self.viewport.camera_vectors()
        p = self.viewport.cam_pos + fwd * 22.0
        gy = self.world.collision.ground_height(float(p[0]), float(p[2]))
        mid = f"mk_{self._marker_counter:04d}"
        self._marker_counter += 1
        mk = WorldMarker(
            marker_id=mid,
            name=f"{prefix or kind}_{self._marker_counter - 1}",
            kind=kind,
            position=(float(p[0]), gy, float(p[2])),
            heading_deg=float(self.viewport.cam_yaw),
            source="Placed by the editor",
        )
        self.undo_stack.push(AddMarkerCommand(self.world, mk, self._refresh_after_edit))
        self.act_v_markers.setChecked(True)
        self.viewport.select_marker(mk)
        self.set_action_status(f"Placed {kind} '{mk.name}' at ({p[0]:.1f}, {gy:.1f}, {p[2]:.1f})")
        return mk

    def _on_viewport_marker_transformed(self, mk: WorldMarker, old_p, old_h: float, new_p, new_h: float, label: str) -> None:
        self.undo_stack.push(TransformMarkerCommand(mk, old_p, old_h, new_p, new_h, self._refresh_after_edit, label))
        self.set_action_status(f"{label}: {mk.name}")

    def _on_prop_marker_edited(self, mk: WorldMarker, pos, heading: float) -> None:
        if mk.position == pos and abs(mk.heading_deg - heading) < 1e-4:
            return
        self.undo_stack.push(
            TransformMarkerCommand(mk, mk.position, mk.heading_deg, pos, heading, self._refresh_after_edit, "Edit marker")
        )

    def _on_prop_marker_meta_edited(self, mk: WorldMarker, name: str, kind: str) -> None:
        if (name, kind) == (mk.name, mk.kind):
            return
        self.undo_stack.push(EditMarkerMetaCommand(mk, mk.name, mk.kind, name, kind, self._refresh_after_edit))
        self.set_action_status(f"Updated marker '{mk.name}' ({mk.kind})")

    def _snap_marker_to_ground(self, mk: WorldMarker) -> None:
        if self.world is None:
            return
        gy = self.world.collision.ground_height(mk.position[0], mk.position[2])
        new_p = (mk.position[0], gy, mk.position[2])
        self.undo_stack.push(
            TransformMarkerCommand(mk, mk.position, mk.heading_deg, new_p, mk.heading_deg, self._refresh_after_edit, "Snap marker")
        )
        self.set_action_status(f"Snapped {mk.name} to ground (Y={gy:.2f} m)")

    def _delete_marker(self, mk: WorldMarker) -> None:
        if self.world is None:
            return
        self.undo_stack.push(DeleteMarkerCommand(self.world, mk, self._refresh_after_edit))
        self.viewport.select_marker(None)
        self.set_action_status(f"Deleted {mk.name}")

    # -----------------------------------------------------------------------
    # Camera / Info Helpers
    # -----------------------------------------------------------------------
    def _on_viewport_stats(self, st: dict[str, Any]) -> None:
        cx, cy, cz = st["cam"]
        self.lbl_cam.setText(f"Cam {cx:.1f}, {cy:.1f}, {cz:.1f}   speed {st['speed']:.0f}")
        self.lbl_prof.setText(
            f"mlists {st['mlists']}  objs {st['objs']}  inst {st['inst']}  "
            f"tris {st['tris']:,}  draws {st['draws']}  {st['ms']:.1f} ms"
        )

    def _reset_camera_overview(self) -> None:
        self.viewport.select_object(None)
        self.viewport.select_road_node(None)
        self.viewport.frame_selection()

    def _camera_top_down(self) -> None:
        if self.world is None:
            return
        cx, cy, cz = self.world.world_center
        self.viewport.cam_pos = np.array([cx, cy + max(180.0, self.world.world_radius * 1.4), cz - 1.0], dtype=np.float32)
        self.viewport.cam_yaw = 0.0
        self.viewport.cam_pitch = -87.0
        self.viewport.update()

    def _set_fly_speed(self, spd: float) -> None:
        self.viewport.fly_speed = spd
        self.set_action_status(f"Camera fly speed set to {spd:.0f} m/s")

    def _filter_explorer_category(self, cat: str) -> None:
        self._request_side_panel("explorer", True)
        self.explorer.tabs.setCurrentIndex(1)
        idx = self.explorer.model_filter.findData(cat)
        if idx >= 0:
            self.explorer.model_filter.setCurrentIndex(idx)

    def _sample_ground_under_camera(self) -> None:
        if self.world is None:
            return
        cx, cz = float(self.viewport.cam_pos[0]), float(self.viewport.cam_pos[2])
        gy, nrm = self.world.collision.ground_sample(cx, cz)
        self.set_action_status(f"SCOL terrain at ({cx:.1f}, {cz:.1f}): Y={gy:.2f} m, normal=({nrm[0]:+.2f}, {nrm[1]:+.2f}, {nrm[2]:+.2f})")

    def _jump_to_start_grid(self) -> None:
        if self.world is None or not self.world.road_graph.nodes:
            return
        n0 = next(iter(self.world.road_graph.nodes.values()))
        c = np.array([n0.x, n0.y, n0.z], dtype=np.float32)
        self.viewport.cam_pos = c + np.array([-20.0, 14.0, -25.0], dtype=np.float32)
        self.viewport.look_at(c)
        self.viewport.update()
        self.set_action_status(f"Jumped to Start/Finish ({n0.x:.1f}, {n0.y:.1f}, {n0.z:.1f})")

    def _show_mesh_stats(self) -> None:
        if self.world is None:
            return
        QMessageBox.information(
            self,
            "Track Mesh Statistics",
            f"Track: {self.world.display_name} ({self.world.trk_path.name})\n"
            f"Unique Models: {len(self.world.models):,} ({self.world.car_count} retail Cars & Wrecks)\n"
            f"Placed Instances: {len(self.world.active_objects):,}\n"
            f"Total Triangles: {self.world.total_triangles:,}\n"
            f"Decoded Textures: {len(self.world.textures)}\n"
            f"GRAF Leaves: {self.world.graf_summary.get('leaves', 0):,}",
        )

    def _show_races_summary(self) -> None:
        if self.world is None:
            return
        lines = [f"  • {k}: {len(v.nodes)} nodes, {len(v.edges)} segments" for k, v in self.world.all_road_graphs.items()]
        QMessageBox.information(
            self,
            "ILIN Racing Lines",
            f"Track: {self.world.display_name}\nActive line: {self.world.road_graph.line_name}\n"
            f"Custom Race: {self.world.custom_race.event_name} ({self.world.custom_race.laps} laps, {self.world.custom_race.ai_count} AI)\n\n"
            + ("\n".join(lines) if lines else "No ILIN lines found."),
        )

    def _show_collision_stats(self) -> None:
        if self.world is None:
            return
        c = self.world.collision
        QMessageBox.information(
            self,
            "Collision Statistics",
            f"Track: {self.world.display_name}\n"
            f"SCOL Terrain Triangles: {c.scol_count:,}\n"
            f"SOBL Static Solids: {c.sobl_count:,} ({c.sobl_boxes:,} boxes, {c.sobl_tubes:,} tubes)\n"
            f"Modified / Flattened Barriers: {len(c.sobl_edits)}\n"
            f"Custom Editor Collision Boxes: {len(c.custom_boxes)}",
        )

    def _show_controls_dialog(self) -> None:
        mod = "Cmd" if sys.platform == "darwin" else "Ctrl"
        del_key = "Delete / Backspace" if sys.platform == "darwin" else "Del"
        QMessageBox.information(
            self,
            "Controls & Keybindings",
            "Camera Navigation:\n"
            "  • Two-finger scroll or middle-mouse drag: Pan\n"
            "  • Shift + two-finger scroll or Alt + right-mouse drag: Orbit around the pointer\n"
            f"  • Pinch or {mod} + scroll: Zoom around the pointer\n"
            "  • Right-mouse drag: Look around. Hold right mouse and scroll to change fly speed.\n"
            "  • Right-Click Selected Object: Quick Context Menu (Replace With, Ground, Delete)\n"
            "  • W / A / S / D: Fly horizontally  |  Q / E: Down / Up\n"
            "  • Shift: 2.8x fly boost  |  F: Frame selection\n\n"
            "Editing & Play-In-Editor:\n"
            "  • Tab: Cycle Move / Rotate / Scale  |  G: Move  |  R: Rotate (or +15° in Place Mode)  |  T: Scale\n"
            "  • P: Place Cursor Mode  |  B: Scatter Brush  |  Alt+Drag: Duplicate & Drag\n"
            f"  • End / {mod}+G: Drop selected object or marker to terrain\n"
            f"  • {mod}+D: Duplicate  |  F2: Export to Blender OBJ  |  {del_key}: Delete\n"
            f"  • {mod}+Z / {mod}+Shift+Z: Undo / Redo\n"
            f"  • {mod}+S: Save project  |  {mod}+Shift+S: Export Mod Layer\n"
            f"  • F5: Free Roam (PIE)  |  {mod}+Shift+F5: Play From Camera  |  Shift+F5: Play Event  |  F4 (in-game): 6-DOF Free Cam",
        )

    def _show_about_dialog(self) -> None:
        from PySide6 import __version__ as pyside_ver
        plat = {"darwin": "macOS", "win32": "Windows"}.get(sys.platform, sys.platform)
        QMessageBox.about(
            self,
            "About EOD World Editor",
            f"<h3>EOD World Editor</h3>"
            f"<p>Native {plat} Qt 6 (PySide6 {pyside_ver}) + OpenGL 3.3 Core / Plume Vulkan "
            f"world editor with Unreal-style Play-In-Editor (PIE) for <b>Test Drive: Eve of Destruction</b>.</p>"
            f"<p>Non-destructively edits retail 0TSR .TRK / .CAR archives (GRAF v18, 4SPS VIF meshes, "
            f"37 retail Cars &amp; Wrecks, Vulkan HOSTTEX PNG swaps, SCOL/SOBL barriers, and ILIN racing lines).</p>",
        )
