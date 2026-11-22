"""Right Properties Panel (context-sensitive QStackedWidget) for EOD World Editor.

Pages:
  0. Nothing selected: Quick links to Environment / Skybox & Barrier tools.
  1. Object selected: Model metadata, R/G/B Position/Rotation/Scale, Texture/Material
     Inspector with clickable decoded thumbnails & live PNG replacement, bottom action
     dock (Snap to ground, Replace With..., Edit in Blender, Make collision, Delete).
  2. Road Node selected: ILIN racing line waypoint details, Position, Road action dock.
  3. Marker / Spawn / Start Grid selected: Editable Name, Kind, Position, Heading, Snap, Delete.
  4. Environment / Skybox: Live presets, Borrow Sky From Track, Sun Azimuth/Elevation,
     Ambient intensity, Fog color/start/end/density with live OpenGL 3.3 Core updates.
  5. SOBL Collision Barrier selected: Height above ground editor, Flatten Barrier, Flatten All.
"""

from __future__ import annotations

import math
from dataclasses import replace
from typing import Any

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QColor, QImage, QPixmap
from PySide6.QtWidgets import (
    QCheckBox,
    QColorDialog,
    QComboBox,
    QDoubleSpinBox,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QScrollArea,
    QStackedWidget,
    QVBoxLayout,
    QWidget,
)

from theme import (
    ACCENT_AMBER,
    ACCENT_BLUE,
    ACCENT_GREEN,
    BG_ELEVATED,
    BG_INPUT,
    BORDER,
    TEXT_MUTED,
    TEXT_PRIMARY,
    TEXT_SECONDARY,
    AxisSpinBox,
    Vec3Editor,
    svg_icon,
)
from world_loader import (
    ENVIRONMENT_PRESETS,
    EnvironmentState,
    RoadGraph,
    RoadNode,
    TextureAsset,
    WorldMarker,
    WorldObject,
)


def _make_card() -> tuple[QFrame, QVBoxLayout]:
    frame = QFrame()
    frame.setProperty("card", True)
    lay = QVBoxLayout(frame)
    lay.setContentsMargins(10, 9, 10, 9)
    lay.setSpacing(6)
    return frame, lay


def _make_kv_row(grid: QGridLayout, row: int, key: str) -> QLabel:
    k_lbl = QLabel(key)
    k_lbl.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 8.5pt; background: transparent;")
    v_lbl = QLabel("—")
    v_lbl.setWordWrap(True)
    v_lbl.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
    v_lbl.setStyleSheet(f"color: {TEXT_PRIMARY}; font-size: 8.5pt; font-weight: 500; background: transparent;")
    grid.addWidget(k_lbl, row, 0, Qt.AlignmentFlag.AlignTop)
    grid.addWidget(v_lbl, row, 1, Qt.AlignmentFlag.AlignTop)
    return v_lbl


def _texture_to_pixmap(ta: TextureAsset, size: int = 44) -> QPixmap:
    qimg = QImage(ta.rgba, ta.width, ta.height, ta.width * 4, QImage.Format.Format_RGBA8888)
    pm = QPixmap.fromImage(qimg.copy())
    return pm.scaled(size, size, Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation)


class PropertiesPanel(QWidget):
    """Context-sensitive Inspector Panel on the right side of the editor."""

    # Object signals
    objectTransformEdited = Signal(object, tuple, tuple, tuple)  # obj, new_pos, new_rot, new_scale
    snapObjectToGround = Signal(object)
    deleteObjectRequested = Signal(object)
    editInBlenderRequested = Signal(object)
    makeCollisionRequested = Signal(object)
    replaceObjectRequested = Signal(object)
    replaceTextureRequested = Signal(str)    # tex_name
    resetTextureRequested = Signal(str)      # tex_name
    selectSimilarRequested = Signal(object)  # WorldObject
    deleteSimilarRequested = Signal(object)  # WorldObject
    includeLodTwinsChanged = Signal(bool)

    # Road node signals
    roadNodePosEdited = Signal(object, tuple)      # node, (x, y, z)
    roadPullOutRequested = Signal(int)             # node_id
    roadDeleteNodeRequested = Signal(int)          # node_id
    roadDrawFromHereRequested = Signal(int)        # node_id
    roadDrawRoadsRequested = Signal()
    roadLaneDeltaRequested = Signal(int, int)      # node_id, +1/-1
    roadSplitExitRequested = Signal(int)           # node_id
    roadSplitTwoWayRequested = Signal(int)         # node_id

    # Marker signals
    markerTransformEdited = Signal(object, tuple, float)  # marker, pos, heading
    markerMetaEdited = Signal(object, str, str)           # marker, new_name, new_kind
    snapMarkerToGround = Signal(object)
    deleteMarkerRequested = Signal(object)

    # Environment / Skybox signals
    environmentEdited = Signal(object)  # proposed EnvironmentState
    openEnvironmentRequested = Signal()

    # SOBL Barrier signals
    barrierHeightApplyRequested = Signal(int, float)  # rec_idx, target_top_above_ground
    clampSkyWallsRequested = Signal()

    MARKER_KINDS = ["Spawn", "Start Grid", "Car", "Chicken", "Trigger", "Breaker", "Marker"]

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setMinimumWidth(305)
        self.setMaximumWidth(420)

        self._current_obj: WorldObject | None = None
        self._current_textures: dict[str, TextureAsset] = {}
        self._current_node: RoadNode | None = None
        self._current_graph: RoadGraph | None = None
        self._current_marker: WorldMarker | None = None
        self._current_env: EnvironmentState | None = None
        self._current_barrier_idx: int = -1
        self._updating = False

        outer = QVBoxLayout(self)
        outer.setContentsMargins(8, 8, 8, 8)
        outer.setSpacing(6)

        hdr_row = QHBoxLayout()
        hdr_row.setSpacing(6)
        hdr = QLabel("PROPERTIES")
        hdr.setProperty("sectionTitle", True)
        self.btn_env_shortcut = QPushButton(" Env / Sky")
        self.btn_env_shortcut.setIcon(svg_icon("sky", "#fbbf24"))
        self.btn_env_shortcut.setToolTip("Open Environment, Skybox, Sun & Fog Inspector")
        self.btn_env_shortcut.clicked.connect(self.openEnvironmentRequested.emit)
        hdr_row.addWidget(hdr)
        hdr_row.addStretch(1)
        hdr_row.addWidget(self.btn_env_shortcut)
        outer.addLayout(hdr_row)

        self.stack = QStackedWidget()
        outer.addWidget(self.stack, 1)

        self._build_empty_page()        # 0
        self._build_object_page()       # 1
        self._build_road_node_page()    # 2
        self._build_marker_page()       # 3
        self._build_environment_page()  # 4
        self._build_barrier_page()      # 5

        self.show_nothing()

    def _add_scrolling_page(self, page: QWidget) -> None:
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setWidget(page)
        self.stack.addWidget(scroll)

    # -----------------------------------------------------------------------
    # Page 0: Nothing selected
    # -----------------------------------------------------------------------
    def _build_empty_page(self) -> None:
        page = QWidget()
        lay = QVBoxLayout(page)
        lay.addStretch(1)

        title = QLabel("Nothing selected")
        title.setAlignment(Qt.AlignmentFlag.AlignCenter)
        title.setStyleSheet(f"font-size: 11pt; font-weight: 600; color: {TEXT_PRIMARY};")

        sub = QLabel("Select an object, road node, spawn marker,\nor SOBL barrier in the 3D view.")
        sub.setAlignment(Qt.AlignmentFlag.AlignCenter)
        sub.setStyleSheet(f"font-size: 8.5pt; color: {TEXT_MUTED};")

        btn_env = QPushButton(" Environment && Skybox...")
        btn_env.setIcon(svg_icon("sky", "#fbbf24"))
        btn_env.clicked.connect(self.openEnvironmentRequested.emit)

        btn_bar = QPushButton(" Clamp Sky-Walls to Asset Height")
        btn_bar.setIcon(svg_icon("shield", "#f97316"))
        btn_bar.clicked.connect(self.clampSkyWallsRequested.emit)

        lay.addWidget(title)
        lay.addWidget(sub)
        lay.addSpacing(14)
        lay.addWidget(btn_env)
        lay.addWidget(btn_bar)
        lay.addStretch(2)
        self._add_scrolling_page(page)

    # -----------------------------------------------------------------------
    # Page 1: Object selected (+ Texture/Material Inspector + Replace With...)
    # -----------------------------------------------------------------------
    def _build_object_page(self) -> None:
        page = QWidget()
        page_lay = QVBoxLayout(page)
        page_lay.setContentsMargins(0, 0, 0, 0)
        page_lay.setSpacing(8)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        body = QWidget()
        lay = QVBoxLayout(body)
        lay.setContentsMargins(0, 0, 2, 0)
        lay.setSpacing(8)

        # Header card
        h_card, h_lay = _make_card()
        self.obj_title = QLabel("Model")
        self.obj_title.setStyleSheet("font-size: 11pt; font-weight: 700; color: #ffffff; background: transparent;")
        self.obj_title.setWordWrap(True)
        self.lbl_multi_sel = QLabel("")
        self.lbl_multi_sel.setStyleSheet(
            "color: #4ade80; font-size: 8.2pt; font-weight: 600; background: transparent;"
        )
        self.lbl_multi_sel.hide()
        self.obj_written_lbl = QLabel("Status: Unsaved Changes")
        self.obj_written_lbl.setStyleSheet("color: #fbbf24; font-size: 8.5pt; font-weight: 600; background: transparent;")
        h_lay.addWidget(self.obj_title)
        h_lay.addWidget(self.lbl_multi_sel)
        h_lay.addWidget(self.obj_written_lbl)
        lay.addWidget(h_card)

        # Metadata card
        m_card, m_lay = _make_card()
        m_hdr = QLabel("ASSET DETAILS")
        m_hdr.setProperty("sectionTitle", True)
        m_hdr.setStyleSheet("background: transparent;")
        m_lay.addWidget(m_hdr)

        grid = QGridLayout()
        grid.setHorizontalSpacing(10)
        grid.setVerticalSpacing(4)
        grid.setColumnStretch(1, 1)
        self.val_source = _make_kv_row(grid, 0, "Source")
        self.val_section = _make_kv_row(grid, 1, "Section / Mlist")
        self.val_store = _make_kv_row(grid, 2, "Model store")
        self.val_hash = _make_kv_row(grid, 3, "Hash")
        self.val_tris = _make_kv_row(grid, 4, "Triangles")
        self.val_tex = _make_kv_row(grid, 5, "Textures")
        self.val_collision = _make_kv_row(grid, 6, "Collision")
        m_lay.addLayout(grid)
        lay.addWidget(m_card)

        # Transform card
        t_card, t_lay = _make_card()
        t_hdr = QLabel("TRANSFORM")
        t_hdr.setProperty("sectionTitle", True)
        t_hdr.setStyleSheet("background: transparent;")
        t_lay.addWidget(t_hdr)

        lbl_pos = QLabel("Position (m)")
        lbl_pos.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 8.5pt; background: transparent;")
        self.obj_pos_ed = Vec3Editor(("X", "Y", "Z"), decimals=4, step=0.5)
        self.obj_pos_ed.editingFinished.connect(self._on_obj_transform_edited)
        self.pos_edit = self.obj_pos_ed

        lbl_rot = QLabel("Rotation (H / P / R deg)")
        lbl_rot.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 8.5pt; background: transparent;")
        self.obj_rot_ed = Vec3Editor(("H", "P", "R"), decimals=2, step=5.0, minimum=-360.0, maximum=360.0)
        self.obj_rot_ed.editingFinished.connect(self._on_obj_transform_edited)
        self.rot_edit = self.obj_rot_ed

        lbl_scl = QLabel("Scale")
        lbl_scl.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 8.5pt; background: transparent;")
        self.obj_scl_ed = Vec3Editor(("X", "Y", "Z"), decimals=4, step=0.1, minimum=0.05, maximum=200.0)
        self.obj_scl_ed.editingFinished.connect(self._on_obj_transform_edited)
        self.scale_edit = self.obj_scl_ed

        t_lay.addWidget(lbl_pos)
        t_lay.addWidget(self.obj_pos_ed)
        t_lay.addWidget(lbl_rot)
        t_lay.addWidget(self.obj_rot_ed)
        t_lay.addWidget(lbl_scl)
        t_lay.addWidget(self.obj_scl_ed)
        lay.addWidget(t_card)

        # Texture / Material Inspector card (live PNG swap -> OpenGL + HOSTTEX!)
        tx_card, tx_lay = _make_card()
        tx_hdr = QLabel("TEXTURES & MATERIALS (HOSTTEX)")
        tx_hdr.setProperty("sectionTitle", True)
        tx_hdr.setStyleSheet("background: transparent;")
        tx_lay.addWidget(tx_hdr)
        self.tex_rows_container = QVBoxLayout()
        self.tex_rows_container.setContentsMargins(0, 0, 0, 0)
        self.tex_rows_container.setSpacing(5)
        tx_lay.addLayout(self.tex_rows_container)
        lay.addWidget(tx_card)

        lay.addStretch(1)
        scroll.setWidget(body)
        page_lay.addWidget(scroll, 1)

        # Bottom action dock
        dock, d_lay = _make_card()
        act_grid = QGridLayout()
        act_grid.setSpacing(6)

        self.btn_replace = QPushButton(" Replace With...")
        self.btn_replace.setIcon(svg_icon("replace", "#93c5fd"))
        self.btn_replace.setProperty("primary", True)
        self.btn_replace.clicked.connect(lambda: self._current_obj and self.replaceObjectRequested.emit(self._current_obj))

        self.btn_select_similar = QPushButton(" Select All Similar")
        self.btn_select_similar.setToolTip("Select every visible object sharing this model or primary texture (Shift+Click to multi-select)")
        self.btn_select_similar.clicked.connect(lambda: self._current_obj and self.selectSimilarRequested.emit(self._current_obj))

        self.btn_delete_similar = QPushButton(" Delete All Similar")
        self.btn_delete_similar.setIcon(svg_icon("trash", "#ff99a2"))
        self.btn_delete_similar.setProperty("danger", True)
        self.btn_delete_similar.setToolTip("Delete all similar objects (e.g. entire fence line) in one undoable step")
        self.btn_delete_similar.clicked.connect(lambda: self._current_obj and self.deleteSimilarRequested.emit(self._current_obj))

        self.btn_snap = QPushButton(" Snap to ground")
        self.btn_snap.setIcon(svg_icon("ground"))
        self.btn_snap.clicked.connect(lambda: self._current_obj and self.snapObjectToGround.emit(self._current_obj))

        self.btn_delete = QPushButton(" Delete")
        self.btn_delete.setIcon(svg_icon("trash", "#ff99a2"))
        self.btn_delete.setProperty("danger", True)
        self.btn_delete.clicked.connect(lambda: self._current_obj and self.deleteObjectRequested.emit(self._current_obj))

        self.btn_blender = QPushButton(" Edit in Blender")
        self.btn_blender.setIcon(svg_icon("blender"))
        self.btn_blender.clicked.connect(lambda: self._current_obj and self.editInBlenderRequested.emit(self._current_obj))

        self.btn_collision = QPushButton(" Make collision")
        self.btn_collision.setIcon(svg_icon("shield"))
        self.btn_collision.clicked.connect(lambda: self._current_obj and self.makeCollisionRequested.emit(self._current_obj))

        self.chk_lod_twins = QCheckBox("Include Overlapping LOD Twins && SOBL Barrier")
        self.chk_lod_twins.setChecked(True)
        self.chk_lod_twins.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 7.8pt; background: transparent;")
        self.chk_lod_twins.setToolTip(
            "When deleting a fence/wall/prop, automatically delete its co-located LOD/billboard twin "
            "and flatten overlapping SOBL invisible barriers in one undoable step."
        )
        self.chk_lod_twins.toggled.connect(self.includeLodTwinsChanged.emit)

        act_grid.addWidget(self.btn_replace, 0, 0, 1, 2)
        act_grid.addWidget(self.btn_select_similar, 1, 0)
        act_grid.addWidget(self.btn_delete_similar, 1, 1)
        act_grid.addWidget(self.btn_snap, 2, 0)
        act_grid.addWidget(self.btn_delete, 2, 1)
        act_grid.addWidget(self.btn_blender, 3, 0)
        act_grid.addWidget(self.btn_collision, 3, 1)
        d_lay.addLayout(act_grid)
        d_lay.addWidget(self.chk_lod_twins)
        page_lay.addWidget(dock)

        self.stack.addWidget(page)

    def _rebuild_texture_inspector(self, obj: WorldObject) -> None:
        while self.tex_rows_container.count():
            item = self.tex_rows_container.takeAt(0)
            w = item.widget()
            if w is not None:
                w.deleteLater()

        if not obj.textures:
            lbl = QLabel("Untextured mesh part")
            lbl.setStyleSheet(f"color: {TEXT_MUTED}; font-size: 8pt; background: transparent;")
            self.tex_rows_container.addWidget(lbl)
            return

        for tname in obj.textures[:12]:
            ta = self._current_textures.get(tname)
            row_frame = QFrame()
            row_frame.setStyleSheet(
                f"QFrame {{ background: {BG_INPUT}; border: 1px solid {BORDER}; border-radius: 5px; }}"
            )
            rlay = QHBoxLayout(row_frame)
            rlay.setContentsMargins(6, 5, 6, 5)
            rlay.setSpacing(7)

            thumb_btn = QPushButton()
            thumb_btn.setFixedSize(46, 46)
            thumb_btn.setToolTip(f"Click to replace {tname} with a PNG texture")
            thumb_btn.setCursor(Qt.CursorShape.PointingHandCursor)
            if ta is not None:
                thumb_btn.setIcon(_texture_to_pixmap(ta, 42))
                thumb_btn.setIconSize(thumb_btn.size())
            else:
                thumb_btn.setText("?")
            thumb_btn.clicked.connect(lambda _=False, tn=tname: self.replaceTextureRequested.emit(tn))

            info_col = QVBoxLayout()
            info_col.setContentsMargins(0, 0, 0, 0)
            info_col.setSpacing(2)
            short = tname.split("/")[-1]
            lbl_n = QLabel(short)
            lbl_n.setStyleSheet("color: #f1f5f9; font-size: 8.5pt; font-weight: 600; background: transparent;")
            if ta is not None:
                badge = " · [REPLACED]" if ta.is_replaced else ""
                sub_txt = f"{ta.width}×{ta.height} ({ta.alpha_kind}){badge}"
                sub_col = ACCENT_GREEN if ta.is_replaced else TEXT_SECONDARY
            else:
                sub_txt = "Unresolved"
                sub_col = TEXT_MUTED
            lbl_s = QLabel(sub_txt)
            lbl_s.setStyleSheet(f"color: {sub_col}; font-size: 7.5pt; background: transparent;")
            info_col.addWidget(lbl_n)
            info_col.addWidget(lbl_s)

            btn_col = QVBoxLayout()
            btn_col.setContentsMargins(0, 0, 0, 0)
            btn_col.setSpacing(3)
            btn_swap = QPushButton("Replace PNG...")
            btn_swap.setFixedHeight(22)
            btn_swap.setStyleSheet("font-size: 7.5pt; padding: 2px 6px;")
            btn_swap.clicked.connect(lambda _=False, tn=tname: self.replaceTextureRequested.emit(tn))
            btn_col.addWidget(btn_swap)

            if ta is not None and ta.is_replaced:
                btn_rst = QPushButton("Reset")
                btn_rst.setFixedHeight(20)
                btn_rst.setStyleSheet("font-size: 7.5pt; padding: 1px 6px;")
                btn_rst.clicked.connect(lambda _=False, tn=tname: self.resetTextureRequested.emit(tn))
                btn_col.addWidget(btn_rst)

            rlay.addWidget(thumb_btn)
            rlay.addLayout(info_col, 1)
            rlay.addLayout(btn_col)
            self.tex_rows_container.addWidget(row_frame)

    # -----------------------------------------------------------------------
    # Page 2: Road Node selected
    # -----------------------------------------------------------------------
    def _build_road_node_page(self) -> None:
        page = QWidget()
        page_lay = QVBoxLayout(page)
        page_lay.setContentsMargins(0, 0, 0, 0)
        page_lay.setSpacing(8)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        body = QWidget()
        lay = QVBoxLayout(body)
        lay.setContentsMargins(0, 0, 2, 0)
        lay.setSpacing(8)

        h_card, h_lay = _make_card()
        self.road_title = QLabel("Road node 1")
        self.road_title.setStyleSheet("font-size: 11pt; font-weight: 700; color: #ffffff; background: transparent;")
        self.road_subtitle = QLabel("ILIN racing line waypoint")
        self.road_subtitle.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 8.5pt; background: transparent;")
        h_lay.addWidget(self.road_title)
        h_lay.addWidget(self.road_subtitle)
        lay.addWidget(h_card)

        d_card, d_lay = _make_card()
        grid = QGridLayout()
        grid.setHorizontalSpacing(10)
        grid.setVerticalSpacing(5)
        grid.setColumnStretch(1, 1)
        self.val_road_kind = _make_kv_row(grid, 0, "Kind")
        self.val_road_line = _make_kv_row(grid, 1, "ILIN Line")
        self.val_road_lanes = _make_kv_row(grid, 2, "Lanes / Width")
        self.val_road_dir = _make_kv_row(grid, 3, "Direction (auto)")
        d_lay.addLayout(grid)
        lay.addWidget(d_card)

        p_card, p_lay = _make_card()
        lbl_p = QLabel("POSITION (X / Y / Z)")
        lbl_p.setProperty("sectionTitle", True)
        lbl_p.setStyleSheet("background: transparent;")
        self.road_pos_ed = Vec3Editor(("X", "Y", "Z"), decimals=4, step=0.5)
        self.road_pos_ed.editingFinished.connect(self._on_road_pos_edited)
        self.road_pos_edit = self.road_pos_ed
        p_lay.addWidget(lbl_p)
        p_lay.addWidget(self.road_pos_ed)
        lay.addWidget(p_card)

        lay.addStretch(1)
        scroll.setWidget(body)
        page_lay.addWidget(scroll, 1)

        dock, dk_lay = _make_card()
        g = QGridLayout()
        g.setSpacing(6)

        btn_pull = QPushButton("Pull a road out")
        btn_pull.setProperty("primary", True)
        btn_pull.clicked.connect(lambda: self._current_node and self.roadPullOutRequested.emit(self._current_node.node_id))

        btn_del = QPushButton("Delete node")
        btn_del.setProperty("danger", True)
        btn_del.clicked.connect(lambda: self._current_node and self.roadDeleteNodeRequested.emit(self._current_node.node_id))

        btn_draw_from = QPushButton("Draw from here")
        btn_draw_from.clicked.connect(lambda: self._current_node and self.roadDrawFromHereRequested.emit(self._current_node.node_id))

        btn_draw_all = QPushButton("Draw roads")
        btn_draw_all.clicked.connect(self.roadDrawRoadsRequested.emit)

        btn_plus_lane = QPushButton("+ Lane")
        btn_plus_lane.clicked.connect(lambda: self._current_node and self.roadLaneDeltaRequested.emit(self._current_node.node_id, 1))

        btn_minus_lane = QPushButton("- Lane")
        btn_minus_lane.clicked.connect(lambda: self._current_node and self.roadLaneDeltaRequested.emit(self._current_node.node_id, -1))

        btn_split_exit = QPushButton("Split here: Exit")
        btn_split_exit.clicked.connect(lambda: self._current_node and self.roadSplitExitRequested.emit(self._current_node.node_id))

        btn_split_twoway = QPushButton("Split here: Two directions")
        btn_split_twoway.clicked.connect(lambda: self._current_node and self.roadSplitTwoWayRequested.emit(self._current_node.node_id))

        g.addWidget(btn_pull, 0, 0)
        g.addWidget(btn_del, 0, 1)
        g.addWidget(btn_draw_from, 1, 0)
        g.addWidget(btn_draw_all, 1, 1)
        g.addWidget(btn_plus_lane, 2, 0)
        g.addWidget(btn_minus_lane, 2, 1)
        g.addWidget(btn_split_exit, 3, 0)
        g.addWidget(btn_split_twoway, 3, 1)
        dk_lay.addLayout(g)
        page_lay.addWidget(dock)

        self.stack.addWidget(page)

    # -----------------------------------------------------------------------
    # Page 3: Marker / Spawn / Start Grid selected
    # -----------------------------------------------------------------------
    def _build_marker_page(self) -> None:
        page = QWidget()
        lay = QVBoxLayout(page)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(8)

        h_card, h_lay = _make_card()
        self.mk_title = QLabel("Marker")
        self.mk_title.setStyleSheet("font-size: 11pt; font-weight: 700; color: #ffffff; background: transparent;")
        self.mk_kind_lbl = QLabel("Kind: Marker")
        self.mk_kind_lbl.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 8.5pt; background: transparent;")
        h_lay.addWidget(self.mk_title)
        h_lay.addWidget(self.mk_kind_lbl)
        lay.addWidget(h_card)

        m_card, m_lay = _make_card()
        m_hdr = QLabel("MARKER / SPAWN DETAILS")
        m_hdr.setProperty("sectionTitle", True)
        m_hdr.setStyleSheet("background: transparent;")
        m_lay.addWidget(m_hdr)

        self.mk_name_ed = QLineEdit()
        self.mk_name_ed.editingFinished.connect(self._on_marker_meta_edited)
        self.mk_kind_combo = QComboBox()
        for k in self.MARKER_KINDS:
            self.mk_kind_combo.addItem(k)
        self.mk_kind_combo.currentIndexChanged.connect(self._on_marker_meta_edited)

        m_lay.addWidget(QLabel("Name"))
        m_lay.addWidget(self.mk_name_ed)
        m_lay.addWidget(QLabel("Marker / Event Type"))
        m_lay.addWidget(self.mk_kind_combo)
        lay.addWidget(m_card)

        t_card, t_lay = _make_card()
        lbl_p = QLabel("TRANSFORM (DRAG 3D GIZMO OR EDIT)")
        lbl_p.setProperty("sectionTitle", True)
        lbl_p.setStyleSheet("background: transparent;")
        self.mk_pos_ed = Vec3Editor(("X", "Y", "Z"), decimals=4, step=0.5)
        self.mk_pos_ed.editingFinished.connect(self._on_marker_edited)
        self.marker_pos_edit = self.mk_pos_ed
        self.sobl_pos_edit: Vec3Editor | None = None
        self.sobl_ext_edit: Vec3Editor | None = None
        self.mk_hdg_ed = AxisSpinBox("H", "#43b564", decimals=2, step=5.0, minimum=-360.0, maximum=360.0)
        self.mk_hdg_ed.editingFinished.connect(self._on_marker_edited)
        t_lay.addWidget(lbl_p)
        t_lay.addWidget(self.mk_pos_ed)
        t_lay.addWidget(QLabel("Heading (deg)"))
        t_lay.addWidget(self.mk_hdg_ed)
        lay.addWidget(t_card)

        lay.addStretch(1)

        dock, dk_lay = _make_card()
        row = QHBoxLayout()
        btn_snap = QPushButton(" Snap to ground")
        btn_snap.setIcon(svg_icon("ground"))
        btn_snap.clicked.connect(lambda: self._current_marker and self.snapMarkerToGround.emit(self._current_marker))
        btn_del = QPushButton(" Delete")
        btn_del.setIcon(svg_icon("trash", "#ff99a2"))
        btn_del.setProperty("danger", True)
        btn_del.clicked.connect(lambda: self._current_marker and self.deleteMarkerRequested.emit(self._current_marker))
        row.addWidget(btn_snap)
        row.addWidget(btn_del)
        dk_lay.addLayout(row)
        lay.addWidget(dock)

        self._add_scrolling_page(page)

    # -----------------------------------------------------------------------
    # Page 4: Environment / Skybox / Fog / Sun Panel
    # -----------------------------------------------------------------------
    def _build_environment_page(self) -> None:
        page = QWidget()
        page_lay = QVBoxLayout(page)
        page_lay.setContentsMargins(0, 0, 0, 0)
        page_lay.setSpacing(8)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        body = QWidget()
        lay = QVBoxLayout(body)
        lay.setContentsMargins(0, 0, 2, 0)
        lay.setSpacing(8)

        h_card, h_lay = _make_card()
        t = QLabel("Environment & Skybox")
        t.setStyleSheet("font-size: 11pt; font-weight: 700; color: #ffffff; background: transparent;")
        s = QLabel("Live OpenGL 3.3 atmosphere + cross-track Vulkan HOSTTEX sky swap")
        s.setWordWrap(True)
        s.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 8pt; background: transparent;")
        h_lay.addWidget(t)
        h_lay.addWidget(s)
        lay.addWidget(h_card)

        sky_card, sky_lay = _make_card()
        sh = QLabel("SKYBOX PRESET & TRACK SKY SWAP")
        sh.setProperty("sectionTitle", True)
        sh.setStyleSheet("background: transparent;")
        sky_lay.addWidget(sh)

        self.env_preset_combo = QComboBox()
        for pname in ENVIRONMENT_PRESETS:
            self.env_preset_combo.addItem(pname)
        self.env_preset_combo.currentTextChanged.connect(self._on_env_preset_changed)

        self.env_borrow_combo = QComboBox()
        self.env_borrow_combo.addItem("Keep Track's Retail Sky", "")
        self.env_borrow_combo.currentIndexChanged.connect(
            lambda _index: self._edit_environment(borrowed_sky_track=self.env_borrow_combo.currentData() or None)
        )

        sky_lay.addWidget(QLabel("Atmosphere Preset"))
        sky_lay.addWidget(self.env_preset_combo)
        sky_lay.addWidget(QLabel("Borrow Sky Tiles From Track (in-game HOSTTEX)"))
        sky_lay.addWidget(self.env_borrow_combo)
        lay.addWidget(sky_card)

        sun_card, sun_lay = _make_card()
        suh = QLabel("SUN & LIGHTING")
        suh.setProperty("sectionTitle", True)
        suh.setStyleSheet("background: transparent;")
        sun_lay.addWidget(suh)

        self.env_sun_az = QDoubleSpinBox()
        self.env_sun_az.setRange(0.0, 360.0)
        self.env_sun_az.setSingleStep(5.0)
        self.env_sun_az.setSuffix(" °")
        self.env_sun_az.valueChanged.connect(lambda value: self._edit_environment(sun_azimuth=value))

        self.env_sun_el = QDoubleSpinBox()
        self.env_sun_el.setRange(2.0, 89.0)
        self.env_sun_el.setSingleStep(2.0)
        self.env_sun_el.setSuffix(" °")
        self.env_sun_el.valueChanged.connect(lambda value: self._edit_environment(sun_elevation=value))

        self.env_ambient = QDoubleSpinBox()
        self.env_ambient.setRange(0.15, 2.0)
        self.env_ambient.setSingleStep(0.05)
        self.env_ambient.setDecimals(2)
        self.env_ambient.valueChanged.connect(lambda value: self._edit_environment(ambient_intensity=value))

        sgrid = QGridLayout()
        sgrid.addWidget(QLabel("Sun Azimuth"), 0, 0)
        sgrid.addWidget(self.env_sun_az, 0, 1)
        sgrid.addWidget(QLabel("Sun Elevation"), 1, 0)
        sgrid.addWidget(self.env_sun_el, 1, 1)
        sgrid.addWidget(QLabel("Ambient Light"), 2, 0)
        sgrid.addWidget(self.env_ambient, 2, 1)
        sun_lay.addLayout(sgrid)
        lay.addWidget(sun_card)

        fog_card, fog_lay = _make_card()
        fh = QLabel("DISTANCE FOG")
        fh.setProperty("sectionTitle", True)
        fh.setStyleSheet("background: transparent;")
        fog_lay.addWidget(fh)

        self.env_fog_start = QDoubleSpinBox()
        self.env_fog_start.setRange(10.0, 3000.0)
        self.env_fog_start.setSingleStep(25.0)
        self.env_fog_start.setSuffix(" m")
        self.env_fog_start.valueChanged.connect(lambda value: self._edit_environment(fog_start=value))

        self.env_fog_end = QDoubleSpinBox()
        self.env_fog_end.setRange(50.0, 6000.0)
        self.env_fog_end.setSingleStep(50.0)
        self.env_fog_end.setSuffix(" m")
        self.env_fog_end.valueChanged.connect(lambda value: self._edit_environment(fog_end=value))

        self.env_fog_density = QDoubleSpinBox()
        self.env_fog_density.setRange(0.0, 1.0)
        self.env_fog_density.setSingleStep(0.05)
        self.env_fog_density.setDecimals(2)
        self.env_fog_density.valueChanged.connect(lambda value: self._edit_environment(fog_density=value))

        self.btn_fog_color = QPushButton("Pick Fog Color...")
        self.btn_fog_color.clicked.connect(self._pick_fog_color)

        fgrid = QGridLayout()
        fgrid.addWidget(QLabel("Fog Start"), 0, 0)
        fgrid.addWidget(self.env_fog_start, 0, 1)
        fgrid.addWidget(QLabel("Fog End"), 1, 0)
        fgrid.addWidget(self.env_fog_end, 1, 1)
        fgrid.addWidget(QLabel("Fog Density"), 2, 0)
        fgrid.addWidget(self.env_fog_density, 2, 1)
        fog_lay.addLayout(fgrid)
        fog_lay.addWidget(self.btn_fog_color)
        lay.addWidget(fog_card)

        lay.addStretch(1)
        scroll.setWidget(body)
        page_lay.addWidget(scroll, 1)
        self.stack.addWidget(page)

    # -----------------------------------------------------------------------
    # Page 5: SOBL Collision Barrier Inspector
    # -----------------------------------------------------------------------
    def _build_barrier_page(self) -> None:
        page = QWidget()
        lay = QVBoxLayout(page)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(8)

        h_card, h_lay = _make_card()
        self.bar_title = QLabel("SOBL Barrier #0")
        self.bar_title.setStyleSheet("font-size: 11pt; font-weight: 700; color: #ffffff; background: transparent;")
        self.bar_sub = QLabel("Static track.sol collision wall")
        self.bar_sub.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 8.5pt; background: transparent;")
        h_lay.addWidget(self.bar_title)
        h_lay.addWidget(self.bar_sub)
        lay.addWidget(h_card)

        d_card, d_lay = _make_card()
        grid = QGridLayout()
        grid.setHorizontalSpacing(10)
        grid.setVerticalSpacing(5)
        grid.setColumnStretch(1, 1)
        self.val_bar_surf = _make_kv_row(grid, 0, "Surface")
        self.val_bar_gnd = _make_kv_row(grid, 1, "Ground Y")
        self.val_bar_above = _make_kv_row(grid, 2, "Height Above Ground")
        self.val_bar_ry = _make_kv_row(grid, 3, "Half-Height (ry)")
        self.val_bar_edit = _make_kv_row(grid, 4, "Editable")
        d_lay.addLayout(grid)
        lay.addWidget(d_card)

        e_card, e_lay = _make_card()
        eh = QLabel("LOWER / REMOVE INVISIBLE WALL")
        eh.setProperty("sectionTitle", True)
        eh.setStyleSheet("background: transparent;")
        e_lay.addWidget(eh)

        self.bar_target_spin = QDoubleSpinBox()
        self.bar_target_spin.setRange(-20.0, 30.0)
        self.bar_target_spin.setSingleStep(0.5)
        self.bar_target_spin.setSuffix(" m above ground")
        self.bar_target_spin.setValue(1.5)

        btn_apply = QPushButton("Apply Height to Barrier")
        btn_apply.setProperty("primary", True)
        btn_apply.clicked.connect(
            lambda: self._current_barrier_idx >= 0
            and self.barrierHeightApplyRequested.emit(self._current_barrier_idx, float(self.bar_target_spin.value()))
        )

        btn_flat_one = QPushButton("Flatten This Barrier (-0.5 m)")
        btn_flat_one.clicked.connect(
            lambda: self._current_barrier_idx >= 0
            and self.barrierHeightApplyRequested.emit(self._current_barrier_idx, -0.5)
        )

        btn_flat_all = QPushButton("Clamp Sky-Walls to Asset Height")
        btn_flat_all.clicked.connect(self.clampSkyWallsRequested.emit)

        e_lay.addWidget(QLabel("Target Top Above Ground (shrink-only):"))
        e_lay.addWidget(self.bar_target_spin)
        e_lay.addWidget(btn_apply)
        e_lay.addWidget(btn_flat_one)
        e_lay.addSpacing(6)
        e_lay.addWidget(btn_flat_all)
        lay.addWidget(e_card)

        lay.addStretch(1)
        self._add_scrolling_page(page)

    # -----------------------------------------------------------------------
    # Context Switching API
    # -----------------------------------------------------------------------
    def show_nothing(self) -> None:
        self._current_obj = None
        self._current_node = None
        self._current_marker = None
        self._current_barrier_idx = -1
        self.stack.setCurrentIndex(0)

    def show_object(
        self,
        obj: WorldObject | None,
        textures: dict[str, TextureAsset] | None = None,
        selected_count: int = 1,
        collision_status: str | None = None,
    ) -> None:
        if obj is None or obj.deleted:
            self.show_nothing()
            return
        self._updating = True
        self._current_obj = obj
        if textures is not None:
            self._current_textures = textures
        self._current_node = None
        self._current_marker = None
        self._current_barrier_idx = -1

        self.obj_title.setText(obj.name)
        if selected_count > 1:
            self.lbl_multi_sel.setText(f"{selected_count} objects selected (Shift+Click to add/remove)")
            self.lbl_multi_sel.show()
            self.btn_delete.setText(f" Delete ({selected_count})")
        else:
            self.lbl_multi_sel.hide()
            self.btn_delete.setText(" Delete")
        if obj.written:
            self.obj_written_lbl.setText("Status: Saved in Mod Layer")
            self.obj_written_lbl.setStyleSheet("color: #34d399; font-size: 8.5pt; font-weight: 600; background: transparent;")
        else:
            self.obj_written_lbl.setText("Status: Unsaved Changes")
            self.obj_written_lbl.setStyleSheet("color: #fbbf24; font-size: 8.5pt; font-weight: 600; background: transparent;")

        self.val_source.setText(obj.source)
        self.val_section.setText(obj.section_mlist)
        self.val_store.setText(obj.model_store)
        self.val_hash.setText(obj.hash_hex)
        self.val_tris.setText(f"{obj.tri_count:,}")
        tex_list = ", ".join(t.split("/")[-1] for t in obj.textures[:3])
        if len(obj.textures) > 3:
            tex_list += f" (+{len(obj.textures) - 3})"
        self.val_tex.setText(f"{len(obj.textures)}" + (f" ({tex_list})" if tex_list else ""))
        if collision_status is None:
            if obj.category == "event-prop" or obj.source != "Retail":
                collision_status = "Solid (Event Wall)"
            elif obj.category == "optional-terrain":
                collision_status = "Solid (BPPT Ramp)"
            elif obj.category == "static":
                collision_status = "Solid (SCOL)"
            else:
                collision_status = "Visual Backdrop -> Auto-Solidified on Edit"
        self.val_collision.setText(collision_status)
        col_color = ACCENT_GREEN if collision_status.startswith("Solid") else ACCENT_AMBER
        self.val_collision.setStyleSheet(
            f"color: {col_color}; font-size: 8.5pt; font-weight: 600; background: transparent;"
        )

        self.obj_pos_ed.setValues(*obj.position)
        self.obj_rot_ed.setValues(*obj.rotation_hpr)
        self.obj_scl_ed.setValues(*obj.scale)
        self.btn_collision.setText(" Collision Ready" if obj.has_custom_collision else " Make collision")
        self._rebuild_texture_inspector(obj)

        self.stack.setCurrentIndex(1)
        self._updating = False

    def show_road_node(self, node: RoadNode | None, graph: RoadGraph | None) -> None:
        if node is None or graph is None or node.node_id not in graph.nodes:
            self.show_nothing()
            return
        self._updating = True
        self._current_obj = None
        self._current_node = node
        self._current_graph = graph
        self._current_marker = None
        self._current_barrier_idx = -1

        kind = graph.node_kind(node.node_id)
        dx, dy, dz = graph.node_direction(node.node_id)
        hdg = math.degrees(math.atan2(dx, dz))
        slope_deg = math.degrees(math.atan2(dy, max(1e-6, math.hypot(dx, dz))))

        self.road_title.setText(f"Road node {node.node_id}")
        self.road_subtitle.setText(
            f"{'Side line' if node.is_side else 'Main racing line'}"
            + (f" · retail #{node.retail_index}" if node.retail_index is not None else " · editor node")
        )
        self.val_road_kind.setText(kind)
        self.val_road_line.setText(graph.line_name)
        dir_txt = "two-way" if node.two_way else "one-way"
        self.val_road_lanes.setText(f"{node.lanes} lane{'s' if node.lanes != 1 else ''} · {node.width:.1f} m ({dir_txt})")
        self.val_road_dir.setText(f"({dx:+.2f}, {dy:+.2f}, {dz:+.2f}) · H {hdg:+.1f}° / P {slope_deg:+.1f}°")
        self.road_pos_ed.setValues(node.x, node.y, node.z)

        self.stack.setCurrentIndex(2)
        self._updating = False

    def show_marker(self, marker: WorldMarker | None) -> None:
        if marker is None:
            self.show_nothing()
            return
        self._updating = True
        self._current_obj = None
        self._current_node = None
        self._current_marker = marker
        self._current_barrier_idx = -1

        self.mk_title.setText(marker.name)
        self.mk_kind_lbl.setText(f"Kind: {marker.kind} · Source: {marker.source}")
        self.mk_name_ed.setText(marker.name)
        idx = self.mk_kind_combo.findText(marker.kind)
        self.mk_kind_combo.setCurrentIndex(idx if idx >= 0 else 0)
        self.mk_pos_ed.setValues(*marker.position)
        self.mk_hdg_ed.setValue(marker.heading_deg)

        self.stack.setCurrentIndex(3)
        self._updating = False

    def show_environment(self, env: EnvironmentState, available_track_stems: list[str] | None = None) -> None:
        self._updating = True
        self._current_env = env
        self._current_obj = None
        self._current_node = None
        self._current_marker = None
        self._current_barrier_idx = -1

        if available_track_stems is not None:
            self.env_borrow_combo.clear()
            self.env_borrow_combo.addItem("Keep Track's Retail Sky", "")
            for stem in available_track_stems:
                if stem.upper() != "EMPTYT_1":
                    self.env_borrow_combo.addItem(f"Borrow {stem.upper()} Sky (HOSTTEX)", stem.upper())

        pidx = self.env_preset_combo.findText(env.preset_name)
        if pidx >= 0:
            self.env_preset_combo.setCurrentIndex(pidx)
        bidx = self.env_borrow_combo.findData(env.borrowed_sky_track or "")
        self.env_borrow_combo.setCurrentIndex(bidx if bidx >= 0 else 0)

        self.env_sun_az.setValue(env.sun_azimuth)
        self.env_sun_el.setValue(env.sun_elevation)
        self.env_ambient.setValue(env.ambient_intensity)
        self.env_fog_start.setValue(env.fog_start)
        self.env_fog_end.setValue(env.fog_end)
        self.env_fog_density.setValue(env.fog_density)
        self._update_fog_swatch(env.fog_color)

        self.stack.setCurrentIndex(4)
        self._updating = False

    def show_barrier(self, rec: dict[str, Any] | None) -> None:
        if rec is None:
            self.show_nothing()
            return
        self._updating = True
        self._current_obj = None
        self._current_node = None
        self._current_marker = None
        self._current_barrier_idx = int(rec["index"])

        self.bar_title.setText(f"SOBL Barrier #{rec['index']}")
        self.bar_sub.setText("Modified in editor" if rec.get("modified") else "Retail track.sol wall box")
        self.val_bar_surf.setText(str(rec.get("surface_name", "wall")))
        self.val_bar_gnd.setText(f"{rec.get('ground_y', 0.0):.2f} m")
        above = float(rec.get("above_ground", 0.0))
        self.val_bar_above.setText(f"{above:+.2f} m")
        ry = float(rec["half_extents"][1]) if rec.get("half_extents") else 0.0
        self.val_bar_ry.setText(f"{ry:.3f} m")
        self.val_bar_edit.setText("Yes (vertical box)" if rec.get("vertical") else "No (tilted)")
        self.bar_target_spin.setValue(max(-2.0, min(above, 1.5)))

        self.stack.setCurrentIndex(5)
        self._updating = False

    def _update_fog_swatch(self, rgb: tuple[float, float, float]) -> None:
        r, g, b = (int(max(0, min(255, c * 255))) for c in rgb)
        self.btn_fog_color.setStyleSheet(
            f"background-color: rgb({r},{g},{b}); color: {'#111' if (r*0.3+g*0.6+b*0.1) > 135 else '#fff'}; "
            f"font-weight: 600; border: 1px solid {BORDER}; border-radius: 5px; padding: 5px;"
        )
        self.btn_fog_color.setText(f"Fog Color  (#{r:02X}{g:02X}{b:02X})")

    # -----------------------------------------------------------------------
    # Commit Handlers
    # -----------------------------------------------------------------------
    def set_object(self, obj: WorldObject | None) -> None:
        self.show_object(obj, self._current_textures)

    def flush_pending_edits(self) -> None:
        """Flush any in-progress spinbox/text edits so Save/Write Mod/PIE never misses uncommitted values."""
        if self._updating:
            return
        for ed in (
            getattr(self, "pos_edit", None),
            getattr(self, "rot_edit", None),
            getattr(self, "scale_edit", None),
            getattr(self, "road_pos_edit", None),
            getattr(self, "marker_pos_edit", None),
            getattr(self, "mk_hdg_ed", None),
            getattr(self, "sobl_pos_edit", None),
            getattr(self, "sobl_ext_edit", None),
        ):
            if ed is not None and hasattr(ed, "flush"):
                ed.flush()
        if self._current_marker is not None and hasattr(self, "mk_name_ed"):
            self._on_marker_meta_edited()

    def _on_obj_transform_edited(self) -> None:
        if self._updating or self._current_obj is None:
            return
        self.objectTransformEdited.emit(
            self._current_obj,
            self.obj_pos_ed.values(),
            self.obj_rot_ed.values(),
            self.obj_scl_ed.values(),
        )

    def _on_road_pos_edited(self) -> None:
        if self._updating or self._current_node is None:
            return
        self.roadNodePosEdited.emit(self._current_node, self.road_pos_ed.values())

    def _on_marker_edited(self) -> None:
        if self._updating or self._current_marker is None:
            return
        self.markerTransformEdited.emit(
            self._current_marker,
            self.mk_pos_ed.values(),
            self.mk_hdg_ed.value(),
        )

    def _on_marker_meta_edited(self) -> None:
        if self._updating or self._current_marker is None:
            return
        name = self.mk_name_ed.text().strip() or self._current_marker.name
        kind = self.mk_kind_combo.currentText()
        self.markerMetaEdited.emit(self._current_marker, name, kind)

    def _on_env_preset_changed(self, preset_name: str) -> None:
        if self._updating or self._current_env is None:
            return
        proposed = replace(self._current_env)
        proposed.apply_preset(preset_name)
        self.environmentEdited.emit(proposed)

    def _edit_environment(self, **changes: Any) -> None:
        if self._updating or self._current_env is None:
            return
        self.environmentEdited.emit(replace(self._current_env, **changes))

    def _pick_fog_color(self) -> None:
        if self._current_env is None:
            return
        r, g, b = (int(c * 255) for c in self._current_env.fog_color)
        col = QColorDialog.getColor(QColor(r, g, b), self, "Select Atmosphere Fog Color")
        if col.isValid():
            self._edit_environment(fog_color=(col.redF(), col.greenF(), col.blueF()))
