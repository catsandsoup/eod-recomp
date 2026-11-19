"""Modal Dialogs for EOD World Editor:
  1. ReplaceModelDialog — Twinmotion/UE-style 'Replace Selection With...' picker (including Cars & Wrecks)
  2. CustomRaceDialog   — Custom Race & AI Pathfinding (ILIN) Editor feeding PIE & Mod Export
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Callable

from PySide6.QtCore import QSize, Qt
from PySide6.QtGui import QIcon
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListView,
    QListWidget,
    QListWidgetItem,
    QMessageBox,
    QPushButton,
    QSlider,
    QSpinBox,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from pie_host import ARCADE_MODES
from theme import TEXT_MUTED, TEXT_PRIMARY, TEXT_SECONDARY, enable_windows_dark_titlebar, svg_icon
from thumbnails import THUMB_H, THUMB_W, ThumbnailCache
from world_loader import CustomRaceConfig, REPO_ROOT, TrackWorld, discover_cars


class ReplaceModelDialog(QDialog):
    """Searchable visual 3D thumbnail picker to replace selected prop with any model or car/wreck."""

    def __init__(
        self,
        world: TrackWorld,
        thumb_cache: ThumbnailCache,
        current_name: str = "",
        default_category: str = "car-wreck",
        parent: QWidget | None = None,
    ):
        super().__init__(parent)
        self.world = world
        self.thumb_cache = thumb_cache
        self.selected_model_key: str | None = None

        self.setWindowTitle(f"Replace Selection With... ({current_name})")
        self.resize(720, 560)
        enable_windows_dark_titlebar(self)

        root = QVBoxLayout(self)
        root.setContentsMargins(12, 12, 12, 12)
        root.setSpacing(8)

        hdr = QLabel(f"Swap <b>{current_name or 'selected object'}</b> with any track model or retail Car / Wreck:")
        hdr.setStyleSheet(f"color: {TEXT_PRIMARY}; font-size: 9.5pt;")
        root.addWidget(hdr)

        bar = QHBoxLayout()
        bar.setSpacing(8)
        self.search_ed = QLineEdit()
        self.search_ed.setPlaceholderText("Search cars, wrecks, barrels, buildings, textures...")
        self.search_ed.textChanged.connect(self._populate)

        self.cat_combo = QComboBox()
        self.cat_combo.setMinimumWidth(190)
        cat_counts: dict[str, int] = {}
        for m in world.models.values():
            cat_counts[m.category] = cat_counts.get(m.category, 0) + 1
        self.cat_combo.addItem(f"All models ({len(world.models)})", "all")
        if "car-wreck" in cat_counts:
            self.cat_combo.addItem(f"Cars & Wrecks ({cat_counts['car-wreck']})", "car-wreck")
        for cat, cnt in sorted(cat_counts.items()):
            if cat == "car-wreck":
                continue
            self.cat_combo.addItem(f"{cat} ({cnt})", cat)
        idx = self.cat_combo.findData(default_category)
        if idx >= 0:
            self.cat_combo.setCurrentIndex(idx)
        self.cat_combo.currentIndexChanged.connect(self._populate)

        bar.addWidget(self.search_ed, 1)
        bar.addWidget(self.cat_combo)
        root.addLayout(bar)

        self.grid = QListWidget()
        self.grid.setViewMode(QListView.ViewMode.IconMode)
        self.grid.setResizeMode(QListView.ResizeMode.Adjust)
        self.grid.setMovement(QListView.Movement.Static)
        self.grid.setWrapping(True)
        self.grid.setSpacing(6)
        self.grid.setIconSize(QSize(THUMB_W, THUMB_H))
        self.grid.setGridSize(QSize(THUMB_W + 20, THUMB_H + 34))
        self.grid.setUniformItemSizes(True)
        self.grid.setWordWrap(True)
        self.grid.itemDoubleClicked.connect(self._accept_selected)
        root.addWidget(self.grid, 1)

        btns = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        btns.button(QDialogButtonBox.StandardButton.Ok).setText("Replace Selection")
        btns.button(QDialogButtonBox.StandardButton.Ok).setProperty("primary", True)
        btns.accepted.connect(self._accept_selected)
        btns.rejected.connect(self.reject)
        root.addWidget(btns)

        self._populate()

    def _populate(self) -> None:
        self.grid.clear()
        q = self.search_ed.text().strip().lower()
        cat = self.cat_combo.currentData() or "all"
        matched = []
        for m in self.world.models.values():
            if cat != "all" and m.category != cat:
                continue
            if q:
                hay = f"{m.name} {m.category} {m.subtype} {' '.join(m.textures)}".lower()
                if q not in hay:
                    continue
            matched.append(m)
        matched.sort(key=lambda m: (0 if m.category == "car-wreck" else 1, m.name.lower()))
        for m in matched[:220]:
            pm = self.thumb_cache.get(m, self.world.textures)
            item = QListWidgetItem(QIcon(pm), f"{m.name}\n{m.tri_count} tris")
            item.setData(Qt.ItemDataRole.UserRole, m.key)
            item.setToolTip(f"{m.name}\n{m.model_store}\n{m.tri_count} triangles")
            self.grid.addItem(item)
        if self.grid.count() > 0:
            self.grid.setCurrentRow(0)

    def _accept_selected(self) -> None:
        item = self.grid.currentItem()
        if item is not None:
            self.selected_model_key = str(item.data(Qt.ItemDataRole.UserRole))
            self.accept()


class CustomRaceDialog(QDialog):
    """Custom Race & AI Pathfinding (ILIN) Editor feeding directly into PIE & Mod Export."""

    EVENT_PRESETS = [
        "No-Rules Race",
        "Demolition Derby",
        "Figure-8 Jump",
        "Battle (8 AI)",
        "Flag Rush",
        "Trailer Race",
        "Suicide Race",
        "Gauntlet",
        "Whiparound",
    ]

    FREE_ROAM_PLACES = ["home", "dealer", "garage", "malt shop", "pete's", "tj's", "junkyard", "arena"]

    def __init__(self, world: TrackWorld, parent: QWidget | None = None):
        super().__init__(parent)
        self.world = world
        self.launch_pie_after = False
        self.setWindowTitle(f"Custom Race & AI Pathfinding (ILIN) — {world.display_name}")
        self.resize(480, 440)
        enable_windows_dark_titlebar(self)

        root = QVBoxLayout(self)
        root.setContentsMargins(14, 14, 14, 14)
        root.setSpacing(10)

        hdr = QLabel(
            f"<b>{world.display_name} ({world.track_name})</b> — Configure race rules, vehicle, "
            f"linked AI racing line (ILIN), and Play-In-Editor (PIE) launch parameters."
        )
        hdr.setWordWrap(True)
        hdr.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 8.5pt;")
        root.addWidget(hdr)

        form = QFormLayout()
        form.setSpacing(8)
        cfg = world.custom_race

        self.ed_name = QLineEdit(cfg.event_name)
        form.addRow("Event Name:", self.ed_name)

        self.cmb_event = QComboBox()
        self.cmb_event.setEditable(True)
        seen: set[str] = set()
        for ev in list(world.available_events) + self.EVENT_PRESETS:
            if ev not in seen:
                self.cmb_event.addItem(ev)
                seen.add(ev)
        self.cmb_event.setCurrentText(cfg.event_type)
        form.addRow("Event Type / Contest:", self.cmb_event)

        self.cmb_car = QComboBox()
        car_names = sorted(
            {
                m.name.split("(")[0].strip()
                for m in world.models.values()
                if m.category == "car-wreck" and not m.name.lower().startswith("trailer")
            }
        )
        if not car_names:
            car_names = ["Algar", "Bulldog", "Bus", "Copcar", "Coyote", "Gladia", "Hunter", "Taxi", "Venom"]
        for cn in car_names:
            self.cmb_car.addItem(cn)
        idx_c = self.cmb_car.findText(cfg.player_car, Qt.MatchFlag.MatchFixedString)
        if idx_c >= 0:
            self.cmb_car.setCurrentIndex(idx_c)
        form.addRow("Player Vehicle:", self.cmb_car)

        self.spn_laps = QSpinBox()
        self.spn_laps.setRange(1, 50)
        self.spn_laps.setValue(cfg.laps)
        form.addRow("Laps:", self.spn_laps)

        self.spn_ai = QSpinBox()
        self.spn_ai.setRange(0, 15)
        self.spn_ai.setValue(cfg.ai_count)
        form.addRow("AI Opponents:", self.spn_ai)

        self.spn_speed = QDoubleSpinBox()
        self.spn_speed.setRange(0.4, 2.5)
        self.spn_speed.setSingleStep(0.05)
        self.spn_speed.setDecimals(2)
        self.spn_speed.setSuffix(" x")
        self.spn_speed.setValue(cfg.ai_speed_scale)
        form.addRow("AI Speed Scale:", self.spn_speed)

        self.cmb_ilin = QComboBox()
        for lname, g in world.all_road_graphs.items():
            self.cmb_ilin.addItem(f"{lname} ({len(g.nodes)} nodes)", lname)
        idx_l = self.cmb_ilin.findData(cfg.ilin_line)
        if idx_l >= 0:
            self.cmb_ilin.setCurrentIndex(idx_l)
        form.addRow("Linked ILIN AI Line:", self.cmb_ilin)

        self.cmb_place = QComboBox()
        for pl in self.FREE_ROAM_PLACES:
            self.cmb_place.addItem(pl)
        idx_p = self.cmb_place.findText(cfg.free_roam_place)
        if idx_p >= 0:
            self.cmb_place.setCurrentIndex(idx_p)
        form.addRow("Career Free-Roam Place:", self.cmb_place)

        self.cmb_arcade = QComboBox()
        for key, title in ARCADE_MODES:
            self.cmb_arcade.addItem(title, key)
        idx_a = self.cmb_arcade.findData(cfg.arcade_mode)
        if idx_a >= 0:
            self.cmb_arcade.setCurrentIndex(idx_a)
        form.addRow("Arcade Game Mode:", self.cmb_arcade)

        self.chk_sync_events = QCheckBox("Sync Modded Ramps && Props to All Events / Free Roam")
        self.chk_sync_events.setChecked(bool(world.sync_all_events))
        form.addRow("Event Layout Sync:", self.chk_sync_events)

        grid_cnt = sum(1 for m in world.markers if m.kind in ("Spawn", "Start Grid", "Car"))
        lbl_grid = QLabel(f"{grid_cnt} Start Grid / Spawn / Car markers placed on track")
        lbl_grid.setStyleSheet(f"color: {TEXT_MUTED}; font-size: 8.5pt;")
        form.addRow("Start Grid Markers:", lbl_grid)

        root.addLayout(form)
        root.addStretch(1)

        btn_row = QHBoxLayout()
        btn_play = QPushButton(" Save && Play Event (PIE)")
        btn_play.setIcon(svg_icon("launch", "#2ecc71"))
        btn_play.setProperty("primary", True)
        btn_play.clicked.connect(self._save_and_play)

        btn_save = QPushButton("Save Settings")
        btn_save.clicked.connect(self._save_only)

        btn_cancel = QPushButton("Cancel")
        btn_cancel.clicked.connect(self.reject)

        btn_row.addWidget(btn_play)
        btn_row.addStretch(1)
        btn_row.addWidget(btn_save)
        btn_row.addWidget(btn_cancel)
        root.addLayout(btn_row)

    def build_config(self) -> CustomRaceConfig:
        sync_on = self.chk_sync_events.isChecked()
        self.world.sync_all_events = sync_on
        return CustomRaceConfig(
            event_name=self.ed_name.text().strip() or self.cmb_event.currentText().strip() or "No-Rules Race",
            event_type=self.cmb_event.currentText().strip() or "No-Rules Race",
            laps=int(self.spn_laps.value()),
            ai_count=int(self.spn_ai.value()),
            player_car=self.cmb_car.currentText().strip() or "Algar",
            ai_speed_scale=float(self.spn_speed.value()),
            ilin_line=str(self.cmb_ilin.currentData() or self.world.road_graph.line_name),
            free_roam_place=self.cmb_place.currentText().strip() or "home",
            arcade_mode=str(self.cmb_arcade.currentData() or "free_roam"),
            sync_all_events=sync_on,
            include_lod_twins=bool(self.world.include_lod_twins),
        )

    def _save_only(self) -> None:
        self.world.custom_race = self.build_config()
        self.launch_pie_after = False
        self.accept()

    def _save_and_play(self) -> None:
        self.world.custom_race = self.build_config()
        self.launch_pie_after = True
        self.accept()


class VehicleTunerDialog(QDialog):
    """Live PIE Vehicle Boost / Handling Tuner & NASCAR / Custom Car Tooling Hub."""

    def __init__(
        self,
        game_dir: Path,
        on_live_boost_changed: Callable[[float], None] | None = None,
        parent: QWidget | None = None,
    ):
        super().__init__(parent)
        self.game_dir = Path(game_dir)
        self._on_live_boost = on_live_boost_changed
        self.setWindowTitle("Vehicles — Live PIE Car Tuner & NASCAR / Custom Car Tools")
        self.resize(520, 380)
        enable_windows_dark_titlebar(self)

        root = QVBoxLayout(self)
        root.setContentsMargins(14, 14, 14, 14)
        root.setSpacing(10)

        tabs = QTabWidget()

        # Tab 1: Live PIE Car Tuner
        t_live = QWidget()
        l_lay = QFormLayout(t_live)
        l_lay.setSpacing(10)

        info = QLabel(
            "Adjust live PIE vehicle power/boost immediately while driving inside Play-In-Editor. "
            "Permanent car handling edits live in <code>DATA/CAR/&lt;CAR&gt;.CAR#physics.val</code> "
            "(legacy <code>SETU</code> <code>.gar</code> sheets are verified unused by SLUS_209.10)."
        )
        info.setWordWrap(True)
        info.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 8.5pt;")
        l_lay.addRow(info)

        self.sld_boost = QSlider(Qt.Orientation.Horizontal)
        self.sld_boost.setRange(50, 250)
        self.sld_boost.setValue(100)
        self.lbl_boost = QLabel("1.00 x")
        self.sld_boost.valueChanged.connect(self._on_boost_slider)
        b_row = QHBoxLayout()
        b_row.addWidget(self.sld_boost, 1)
        b_row.addWidget(self.lbl_boost)
        l_lay.addRow("Live PIE Speed Boost:", b_row)

        self.cmb_car = QComboBox()
        for cp in discover_cars(self.game_dir):
            self.cmb_car.addItem(cp.stem.replace("_1", "").title(), str(cp))
        l_lay.addRow("Inspect Retail Car:", self.cmb_car)

        tabs.addTab(t_live, "Live PIE Car Tuner")

        # Tab 2: NASCAR & Custom Car CLI Bridge
        t_nascar = QWidget()
        n_lay = QVBoxLayout(t_nascar)
        n_lay.setSpacing(8)
        nascar_script = REPO_ROOT / "tools" / "import_nascar_cars.py"
        custom_script = REPO_ROOT / "tools" / "create_custom_car.py"
        lbl_n = QLabel(
            f"<b>NASCAR: Dirt to Daytona &amp; Custom Car Pipelines</b><br>"
            f"• NASCAR importer: <code>{nascar_script.name}</code> ({'available' if nascar_script.is_file() else 'not found'})<br>"
            f"• Custom car cloner: <code>{custom_script.name}</code> ({'available' if custom_script.is_file() else 'not found'})<br><br>"
            f"All imported/cloned <code>.CAR</code> archives placed under <code>mods/&lt;Project&gt;/DATA/CAR/</code> "
            f"appear automatically in Explorer's Cars &amp; Wrecks catalog and PIE vehicle picker."
        )
        lbl_n.setWordWrap(True)
        lbl_n.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 8.5pt;")
        n_lay.addWidget(lbl_n)
        n_lay.addStretch(1)
        tabs.addTab(t_nascar, "NASCAR & Custom Cars")

        root.addWidget(tabs, 1)

        btns = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        btns.rejected.connect(self.accept)
        btns.accepted.connect(self.accept)
        root.addWidget(btns)

    def _on_boost_slider(self, val: int) -> None:
        mult = val / 100.0
        self.lbl_boost.setText(f"{mult:.2f} x")
        if self._on_live_boost is not None:
            self._on_live_boost(mult)
