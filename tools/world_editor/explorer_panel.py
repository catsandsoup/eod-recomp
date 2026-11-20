"""Left Explorer Panel for EOD World Editor.

Contains:
  - Track selector QComboBox (all retail .TRK files in DATA/TRACKS)
  - Icon tabs: Scene Tree, Models, Sections/Mlists, Layers (with live counts)
  - Models tab: Search box, 'All models (N)' category filter, 'Only models the map places'
    checkbox, and 2-column visual 3D thumbnail QListWidget with drag-and-drop & double-click placement.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

from PySide6.QtCore import QByteArray, QMimeData, QSize, Qt, QTimer, Signal
from PySide6.QtGui import QDrag, QIcon, QPixmap
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListView,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QTabWidget,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from theme import (
    BG_ELEVATED,
    BORDER,
    TEXT_MUTED,
    TEXT_PRIMARY,
    TEXT_SECONDARY,
    svg_icon,
)
from thumbnails import THUMB_H, THUMB_W, ThumbnailCache, render_model_thumbnail_qimage
from viewport_gl import MIME_MODEL_KEY
from workers import CancelToken, ThumbnailBatchWorker, start_worker
from world_loader import TrackWorld, WorldMarker, WorldModel, WorldObject


class ModelGridWidget(QListWidget):
    """2-column IconMode thumbnail grid supporting drag-and-drop onto the 3D viewport."""

    modelPlaceRequested = Signal(str)

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setViewMode(QListView.ViewMode.IconMode)
        self.setResizeMode(QListView.ResizeMode.Adjust)
        self.setMovement(QListView.Movement.Static)
        self.setWrapping(True)
        self.setSpacing(0)
        self.setIconSize(QSize(THUMB_W, THUMB_H))
        self.setGridSize(QSize(THUMB_W + 18, THUMB_H + 34))
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.setUniformItemSizes(True)
        self.setWordWrap(True)
        self.setDragEnabled(True)
        self.setSelectionMode(QListWidget.SelectionMode.SingleSelection)
        self.itemDoubleClicked.connect(self._on_double_click)

    def resizeEvent(self, event):
        super().resizeEvent(event)
        # The icon view keeps a small inset around its first and last cells.
        width = max(1, self.viewport().width() - 6)
        columns = max(1, width // THUMB_W)
        cell = QSize(width // columns, THUMB_H + 28)
        if self.gridSize() != cell:
            self.setGridSize(cell)
            self.setIconSize(QSize(min(THUMB_W, max(1, cell.width() - 10)), THUMB_H))

    def _on_double_click(self, item: QListWidgetItem) -> None:
        mkey = item.data(Qt.ItemDataRole.UserRole)
        if mkey:
            self.modelPlaceRequested.emit(str(mkey))

    def startDrag(self, supportedActions) -> None:
        item = self.currentItem()
        if item is None:
            return
        mkey = item.data(Qt.ItemDataRole.UserRole)
        if not mkey:
            return
        mime = QMimeData()
        mime.setData(MIME_MODEL_KEY, QByteArray(str(mkey).encode("utf-8")))
        drag = QDrag(self)
        drag.setMimeData(mime)
        pm = item.icon().pixmap(QSize(THUMB_W, THUMB_H))
        if not pm.isNull():
            drag.setPixmap(pm)
        drag.exec(Qt.DropAction.CopyAction)


class ExplorerPanel(QWidget):
    """Left sidebar with Track Selector, Scene Tree, 2-column Models Grid (incl. Cars & Wrecks), Mlists, and Layers."""

    trackSelected = Signal(str)          # .TRK file path
    objectPicked = Signal(str)           # obj_id
    markerPicked = Signal(str)           # marker_id
    roadLineSelected = Signal(str)       # .ild name
    modelPlaceRequested = Signal(str)    # model_key (double-click or Place button)
    modelSelected = Signal(str)          # model_key (single click -> updates Ghost/Scatter target)
    ghostPlaceRequested = Signal(str)    # model_key -> activate Ghost Place Mode
    scatterModeRequested = Signal(str)   # model_key -> activate Scatter Brush Mode
    replaceSelectedRequested = Signal(str)  # model_key -> Replace Selection With this model
    layerToggled = Signal(str, bool)     # layer_key, visible

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self.setMinimumWidth(275)
        self.setMaximumWidth(420)
        self.world: TrackWorld | None = None
        self.thumb_cache = ThumbnailCache()
        self._thumb_gen: int = 0
        self._thumb_token: CancelToken | None = None
        self._thumb_item_map: dict[str, QListWidgetItem] = {}
        self._placeholder_icon = QIcon(self._make_placeholder_pixmap())

        root = QVBoxLayout(self)
        root.setContentsMargins(8, 8, 8, 8)
        root.setSpacing(6)

        # Track selector header
        trk_row = QHBoxLayout()
        trk_row.setSpacing(6)
        lbl_trk = QLabel("TRACK")
        lbl_trk.setProperty("sectionTitle", True)
        self.track_combo = QComboBox()
        self.track_combo.setMinimumHeight(28)
        self.track_combo.currentIndexChanged.connect(self._on_track_combo)
        trk_row.addWidget(lbl_trk)
        trk_row.addWidget(self.track_combo, 1)
        root.addLayout(trk_row)

        # Icon tabs: Scene Tree, Models, Sections/Mlists, Layers
        self.tabs = QTabWidget()
        self.tabs.setDocumentMode(False)
        self.tabs.setUsesScrollButtons(True)
        self.tabs.tabBar().setExpanding(True)
        self.tabs.tabBar().setElideMode(Qt.TextElideMode.ElideNone)
        root.addWidget(self.tabs, 1)

        # Tab 0: Scene Tree
        self._build_scene_tab()
        # Tab 1: Models (2-column visual thumbnail grid)
        self._build_models_tab()
        # Tab 2: Sections / Mlists
        self._build_sections_tab()
        # Tab 3: Layers
        self._build_layers_tab()

        # Default to Models tab (matches clean-room workflow with 2-col preview grid!)
        self.tabs.setCurrentIndex(1)

    @staticmethod
    def _make_placeholder_pixmap() -> QPixmap:
        pm = QPixmap(THUMB_W, THUMB_H)
        pm.fill(Qt.GlobalColor.transparent)
        return pm

    # -----------------------------------------------------------------------
    # Tab Builders
    # -----------------------------------------------------------------------
    def _build_scene_tab(self) -> None:
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setContentsMargins(4, 6, 4, 4)
        lay.setSpacing(5)

        self.scene_search = QLineEdit()
        self.scene_search.setPlaceholderText("Filter scene objects...")
        self.scene_search.textChanged.connect(self._refresh_scene_tree)
        lay.addWidget(self.scene_search)

        self.scene_tree = QTreeWidget()
        self.scene_tree.setHeaderLabels(["Item", "Info"])
        self.scene_tree.setColumnWidth(0, 175)
        self.scene_tree.itemClicked.connect(self._on_scene_tree_click)
        lay.addWidget(self.scene_tree, 1)

        self.tabs.addTab(w, "Tree")

    def _build_models_tab(self) -> None:
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setContentsMargins(4, 6, 4, 4)
        lay.setSpacing(5)

        self.model_search = QLineEdit()
        self.model_search.setPlaceholderText("Search assets")
        self.model_search.setToolTip("Search models, cars, wrecks and textures")
        self.model_search.textChanged.connect(self._rebuild_model_grid)
        lay.addWidget(self.model_search)

        filter_row = QHBoxLayout()
        filter_row.setSpacing(4)
        self.model_filter = QComboBox()
        self.model_filter.addItem("All models (0)", "all")
        self.model_filter.currentIndexChanged.connect(self._rebuild_model_grid)

        self.btn_cars_quick = QPushButton(" Cars")
        self.btn_cars_quick.setIcon(svg_icon("car", "#ff8a8a"))
        self.btn_cars_quick.setCheckable(True)
        self.btn_cars_quick.setToolTip("Show retail Cars & Wrecks (DATA/CAR/*.CAR)")
        self.btn_cars_quick.toggled.connect(self._on_cars_quick_toggled)

        filter_row.addWidget(self.model_filter, 1)
        filter_row.addWidget(self.btn_cars_quick)
        lay.addLayout(filter_row)

        self.chk_only_placed = QCheckBox("Only models the map places")
        self.chk_only_placed.setChecked(False)
        self.chk_only_placed.toggled.connect(self._rebuild_model_grid)
        lay.addWidget(self.chk_only_placed)

        self.model_grid = ModelGridWidget()
        self.model_grid.modelPlaceRequested.connect(self.modelPlaceRequested.emit)
        self.model_grid.currentItemChanged.connect(self._on_model_item_changed)
        lay.addWidget(self.model_grid, 1)

        act_row = QGridLayout()
        act_row.setSpacing(4)
        btn_ghost = QPushButton("Ghost")
        btn_ghost.setIcon(svg_icon("place"))
        btn_ghost.setToolTip("Enter Ghost Placement Mode with selected model/car")
        btn_ghost.clicked.connect(self._ghost_place_selected_model)

        btn_scat = QPushButton("Scatter")
        btn_scat.setIcon(svg_icon("scatter"))
        btn_scat.setToolTip("Paint / Scatter selected model or wrecked car across terrain")
        btn_scat.clicked.connect(self._scatter_selected_model)

        btn_rep = QPushButton("Swap")
        btn_rep.setIcon(svg_icon("replace"))
        btn_rep.setToolTip("Replace currently selected scene object with this model/car")
        btn_rep.clicked.connect(self._replace_with_selected_model)

        btn_place = QPushButton("Place")
        btn_place.clicked.connect(self._place_selected_model)

        act_row.addWidget(btn_ghost, 0, 0)
        act_row.addWidget(btn_scat, 0, 1)
        act_row.addWidget(btn_rep, 1, 0)
        act_row.addWidget(btn_place, 1, 1)
        lay.addLayout(act_row)

        self.tabs.addTab(w, "Models")

    def _build_sections_tab(self) -> None:
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setContentsMargins(4, 6, 4, 4)
        lay.setSpacing(6)

        lbl_road = QLabel("ACTIVE ILIN RACING LINE")
        lbl_road.setProperty("sectionTitle", True)
        lay.addWidget(lbl_road)

        self.road_line_combo = QComboBox()
        self.road_line_combo.currentIndexChanged.connect(self._on_road_line_changed)
        lay.addWidget(self.road_line_combo)

        lbl_sec = QLabel("GRAF DISPLAY LIST & MLISTS")
        lbl_sec.setProperty("sectionTitle", True)
        lay.addWidget(lbl_sec)

        self.sections_tree = QTreeWidget()
        self.sections_tree.setHeaderLabels(["Section / Mlist", "Count"])
        self.sections_tree.setColumnWidth(0, 185)
        lay.addWidget(self.sections_tree, 1)

        self.tabs.addTab(w, "Mlists")

    def _build_layers_tab(self) -> None:
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setContentsMargins(6, 8, 6, 6)
        lay.setSpacing(6)

        lbl = QLabel("VISIBILITY LAYERS")
        lbl.setProperty("sectionTitle", True)
        lay.addWidget(lbl)

        self.layer_checks: dict[str, QCheckBox] = {}
        layer_defs = [
            ("static", "Static World Geometry", True),
            ("car-wreck", "Cars & Wrecks (Placed)", True),
            ("dyno", "Dyno / Track Props", True),
            ("event-prop", "Event Props (c_*.va0)", True),
            ("billboard", "Billboards / Sprites", True),
            ("optional-terrain", "Optional Terrain", True),
            ("display-object", "Display Objects", True),
            ("roads", "Racing Lines (ILIN)", True),
            ("collision", "Collision (SCOL + SOBL)", False),
            ("markers", "Markers / Spawns / Cars", True),
        ]
        for key, label, default_on in layer_defs:
            cb = QCheckBox(label)
            cb.setChecked(default_on)
            cb.toggled.connect(lambda checked, k=key: self.layerToggled.emit(k, checked))
            lay.addWidget(cb)
            self.layer_checks[key] = cb

        lay.addStretch(1)
        self.tabs.addTab(w, "Layers")

    # -----------------------------------------------------------------------
    # Population & Refresh
    # -----------------------------------------------------------------------
    def set_available_tracks(self, track_paths: list[Path], current_path: Path | None = None) -> None:
        self.track_combo.blockSignals(True)
        self.track_combo.clear()
        sel_idx = 0
        for i, p in enumerate(track_paths):
            self.track_combo.addItem(f"{p.stem.upper()}  ({p.name})", str(p))
            if current_path and p.resolve() == current_path.resolve():
                sel_idx = i
        if track_paths:
            self.track_combo.setCurrentIndex(sel_idx)
        self.track_combo.blockSignals(False)

    def set_world(self, world: TrackWorld) -> None:
        self.world = world
        self.thumb_cache.clear()
        self._cancel_thumb_worker()
        self.refresh_model_categories()

        # Populate road lines
        self.road_line_combo.blockSignals(True)
        self.road_line_combo.clear()
        for lname, g in world.all_road_graphs.items():
            self.road_line_combo.addItem(f"{lname}  ({len(g.nodes)} nodes)", lname)
        idx = self.road_line_combo.findData(world.road_graph.line_name)
        if idx >= 0:
            self.road_line_combo.setCurrentIndex(idx)
        self.road_line_combo.blockSignals(False)

        self.refresh_counts()
        self._rebuild_model_grid()
        self._refresh_sections_tree()

    def refresh_model_categories(self) -> None:
        """Rebuild model category dropdown (keeping current selection & pinning Cars & Wrecks)."""
        if self.world is None:
            return
        prev_cat = self.model_filter.currentData() or "all"
        self.model_filter.blockSignals(True)
        self.model_filter.clear()
        cat_counts: dict[str, int] = {}
        for m in self.world.models.values():
            cat_counts[m.category] = cat_counts.get(m.category, 0) + 1
        self.model_filter.addItem(f"All models ({len(self.world.models)})", "all")
        if "car-wreck" in cat_counts:
            self.model_filter.addItem(f"Cars & Wrecks ({cat_counts['car-wreck']})", "car-wreck")
            self.btn_cars_quick.setText(f" Cars ({cat_counts['car-wreck']})")
        else:
            self.btn_cars_quick.setText(" Cars (0)")
        for cat, cnt in sorted(cat_counts.items()):
            if cat == "car-wreck":
                continue
            self.model_filter.addItem(f"{cat} ({cnt})", cat)
        idx = self.model_filter.findData(prev_cat)
        self.model_filter.setCurrentIndex(idx if idx >= 0 else 0)
        self.model_filter.blockSignals(False)

    def refresh_counts(self) -> None:
        if self.world is None:
            return
        active_objs = [o for o in self.world.objects if not o.deleted]
        mlist_cnt = sum(1 for m in self.world.mlist_info if m is not None)
        self.tabs.setTabText(0, "Tree")
        self.tabs.setTabToolTip(0, f"Scene Tree ({len(active_objs)} instances)")
        self.tabs.setTabText(1, "Models")
        self.tabs.setTabToolTip(1, f"Models ({len(self.world.models)})")
        self.tabs.setTabText(2, "Mlists")
        self.tabs.setTabToolTip(2, f"Sections / Mlists ({mlist_cnt})")
        self.tabs.setTabText(3, "Layers")
        self.tabs.setTabToolTip(3, "Visibility Layers (9)")

        # Update layer checkbox counts
        by_cat: dict[str, int] = {}
        for o in active_objs:
            by_cat[o.category] = by_cat.get(o.category, 0) + 1
        labels = {
            "static": f"Static World Geometry ({by_cat.get('static', 0)})",
            "car-wreck": f"Cars & Wrecks ({by_cat.get('car-wreck', 0)})",
            "dyno": f"Dyno / Track Props ({by_cat.get('dyno', 0)})",
            "event-prop": f"Event Props ({by_cat.get('event-prop', 0)})",
            "billboard": f"Billboards / Sprites ({by_cat.get('billboard', 0)})",
            "optional-terrain": f"Optional Terrain ({by_cat.get('optional-terrain', 0)})",
            "display-object": f"Display Objects ({by_cat.get('display-object', 0)})",
            "roads": f"Racing Lines / ILIN ({len(self.world.road_graph.nodes)} nodes)",
            "collision": f"Collision ({self.world.collision.scol_count} tris, {self.world.collision.sobl_boxes} boxes)",
            "markers": f"Markers / Spawns / Cars ({len(self.world.markers)})",
        }
        for k, text in labels.items():
            if k in self.layer_checks:
                self.layer_checks[k].setText(text)
        self._refresh_scene_tree()

    def _refresh_scene_tree(self) -> None:
        self.scene_tree.clear()
        if self.world is None:
            return
        q = self.scene_search.text().strip().lower()

        # Group placed objects by category
        groups: dict[str, list[WorldObject]] = {}
        for o in self.world.objects:
            if o.deleted:
                continue
            if q and q not in o.name.lower() and q not in o.obj_id.lower() and q not in o.category.lower():
                continue
            grp_name = "Placed by Editor" if o.source != "Retail" else o.category
            groups.setdefault(grp_name, []).append(o)

        for grp_name, objs in sorted(groups.items()):
            parent = QTreeWidgetItem([f"{grp_name}", f"{len(objs)}"])
            self.scene_tree.addTopLevelItem(parent)
            for o in objs[:250]:
                child = QTreeWidgetItem([o.name, f"{o.tri_count} tris"])
                child.setData(0, Qt.ItemDataRole.UserRole, ("obj", o.obj_id))
                parent.addChild(child)
            if len(objs) > 250:
                more = QTreeWidgetItem([f"... +{len(objs) - 250} more (use search)", ""])
                parent.addChild(more)
            if grp_name in ("Placed by Editor", "dyno", "event-prop"):
                parent.setExpanded(True)

        # Markers & Cars
        if self.world.markers:
            m_parent = QTreeWidgetItem(["Markers & Cars", str(len(self.world.markers))])
            self.scene_tree.addTopLevelItem(m_parent)
            for m in self.world.markers:
                if q and q not in m.name.lower() and q not in m.kind.lower():
                    continue
                child = QTreeWidgetItem([m.name, m.kind])
                child.setData(0, Qt.ItemDataRole.UserRole, ("marker", m.marker_id))
                m_parent.addChild(child)
            m_parent.setExpanded(True)

    def _cancel_thumb_worker(self) -> None:
        if self._thumb_token is not None:
            self._thumb_token.cancel()
        self._thumb_gen += 1
        self._thumb_item_map.clear()

    def _rebuild_model_grid(self) -> None:
        self._cancel_thumb_worker()
        self.model_grid.clear()
        if self.world is None:
            return

        q = self.model_search.text().strip().lower()
        cat = self.model_filter.currentData() or "all"
        only_placed = self.chk_only_placed.isChecked()

        self.btn_cars_quick.blockSignals(True)
        self.btn_cars_quick.setChecked(cat == "car-wreck")
        self.btn_cars_quick.blockSignals(False)

        matched: list[WorldModel] = []
        for m in self.world.models.values():
            if only_placed and not m.placed_by_map:
                continue
            if cat != "all" and m.category != cat:
                continue
            if q:
                hay = f"{m.name} {m.category} {m.subtype} {' '.join(m.textures)}".lower()
                if q not in hay:
                    continue
            matched.append(m)

        # Put Cars & Wrecks first when viewing that category or searching
        if cat == "car-wreck":
            matched.sort(key=lambda m: m.name.lower())

        to_render: list[tuple[str, WorldModel]] = []
        for m in matched[:400]:
            label = f"{m.name}\n{m.tri_count} tris"
            item = QListWidgetItem(self._placeholder_icon, label)
            item.setData(Qt.ItemDataRole.UserRole, m.key)
            item.setToolTip(
                f"{m.name}\nStore: {m.model_store}\nCategory: {m.category} ({m.subtype})\n"
                f"Triangles: {m.tri_count} | Textures: {len(m.textures)}\n"
                f"Hash: {m.hash_hex}"
            )
            cached = self.thumb_cache._cache.get(m.key)
            if cached is not None:
                item.setIcon(QIcon(cached))
            else:
                self._thumb_item_map[m.key] = item
                to_render.append((m.key, m))
            self.model_grid.addItem(item)

        # Render thumbnails off the UI thread on QThreadPool (zero UI stutter!)
        if to_render:
            token = CancelToken(self._thumb_gen)
            self._thumb_token = token
            worker = ThumbnailBatchWorker(
                to_render,
                dict(self.world.textures),
                render_model_thumbnail_qimage,
                token,
            )
            worker.signals.thumbnailReady.connect(self._on_worker_thumb_ready)
            start_worker(worker, priority=-5)

    def _on_worker_thumb_ready(self, gen: int, mkey: str, qimg: Any) -> None:
        if gen != self._thumb_gen:
            return
        pm = QPixmap.fromImage(qimg)
        self.thumb_cache._cache[mkey] = pm
        item = self._thumb_item_map.pop(mkey, None)
        if item is not None:
            try:
                item.setIcon(QIcon(pm))
            except RuntimeError:
                pass

    def _refresh_sections_tree(self) -> None:
        self.sections_tree.clear()
        if self.world is None:
            return
        for idx, minfo in enumerate(self.world.mlist_info):
            if not minfo:
                continue
            top = QTreeWidgetItem([f"mlist_{idx} @ 0x{minfo['offset']:x}", f"{minfo['batches']['count']} batches"])
            top.addChild(QTreeWidgetItem(["Textures", str(minfo["textures"]["count"])]))
            top.addChild(QTreeWidgetItem(["Cull Spheres", str(minfo["spheres"]["count"])]))
            top.addChild(QTreeWidgetItem(["Table 1C", str(minfo["table1c"]["count"])]))
            top.addChild(QTreeWidgetItem(["Table 24", str(minfo["table24"]["count"])]))
            self.sections_tree.addTopLevelItem(top)
            top.setExpanded(True)

        # GRAF leaf breakdown
        leaf_top = QTreeWidgetItem(["GRAF Leaf Breakdown", str(self.world.graf_summary.get("leaves", 0))])
        for k, v in self.world.graf_summary.get("leaf_kinds", {}).items():
            leaf_top.addChild(QTreeWidgetItem([k, str(v)]))
        self.sections_tree.addTopLevelItem(leaf_top)
        leaf_top.setExpanded(True)

        # Collision summary
        col_top = QTreeWidgetItem(["Collision Summary", ""])
        col_top.addChild(QTreeWidgetItem(["SCOL Terrain Tris", str(self.world.collision.scol_count)]))
        col_top.addChild(QTreeWidgetItem(["SOBL Boxes", str(self.world.collision.sobl_boxes)]))
        col_top.addChild(QTreeWidgetItem(["SOBL Tubes", str(self.world.collision.sobl_tubes)]))
        self.sections_tree.addTopLevelItem(col_top)
        col_top.setExpanded(True)

    # -----------------------------------------------------------------------
    # Handlers
    # -----------------------------------------------------------------------
    def _on_track_combo(self, _index: int) -> None:
        path = self.track_combo.currentData()
        if path:
            self.trackSelected.emit(str(path))

    def _on_road_line_changed(self, _index: int) -> None:
        lname = self.road_line_combo.currentData()
        if lname:
            self.roadLineSelected.emit(str(lname))

    def _on_scene_tree_click(self, item: QTreeWidgetItem, _col: int) -> None:
        data = item.data(0, Qt.ItemDataRole.UserRole)
        if not data:
            return
        kind, ident = data
        if kind == "obj":
            self.objectPicked.emit(ident)
        elif kind == "marker":
            self.markerPicked.emit(ident)

    def _on_cars_quick_toggled(self, checked: bool) -> None:
        want = "car-wreck" if checked else "all"
        idx = self.model_filter.findData(want)
        if idx >= 0:
            self.model_filter.setCurrentIndex(idx)

    def _on_model_item_changed(self, current: QListWidgetItem | None, _prev: QListWidgetItem | None) -> None:
        if current is not None:
            mkey = current.data(Qt.ItemDataRole.UserRole)
            if mkey:
                self.modelSelected.emit(str(mkey))

    def current_model_key(self) -> str | None:
        item = self.model_grid.currentItem()
        if item is not None:
            mkey = item.data(Qt.ItemDataRole.UserRole)
            if mkey:
                return str(mkey)
        return None

    def _place_selected_model(self) -> None:
        mkey = self.current_model_key()
        if mkey:
            self.modelPlaceRequested.emit(mkey)

    def _ghost_place_selected_model(self) -> None:
        mkey = self.current_model_key()
        if mkey:
            self.ghostPlaceRequested.emit(mkey)

    def _scatter_selected_model(self) -> None:
        mkey = self.current_model_key()
        if mkey:
            self.scatterModeRequested.emit(mkey)

    def _replace_with_selected_model(self) -> None:
        mkey = self.current_model_key()
        if mkey:
            self.replaceSelectedRequested.emit(mkey)
