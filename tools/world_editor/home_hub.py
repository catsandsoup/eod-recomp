"""Adobe / Lumion-Style Home Hub, Project Naming & Recent Projects Store for EOD World Editor.

Persists:
  - `%LOCALAPPDATA%\\EODWorldEditor\\recents.json` (atomic writes via tmp -> os.replace)
  - `%LOCALAPPDATA%\\EODWorldEditor\\prefs.json` (`skip_home_hub`, last selected track)
  - `<project_stem>.thumb.png` beside `.eodworld.json` for rich Recent Projects cards
"""

from __future__ import annotations

import json
import os
import time
from datetime import datetime
from pathlib import Path
from typing import Any

from PySide6.QtCore import QEvent, Qt, Signal
from PySide6.QtGui import QColor, QFont, QPainter, QPixmap
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QFileDialog,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QScrollArea,
    QVBoxLayout,
    QWidget,
)

from theme import (
    ACCENT_BLUE,
    ACCENT_BLUE_HOVER,
    BG_ELEVATED,
    BG_HOVER,
    BG_INPUT,
    BG_PANEL,
    BG_ROOT,
    BORDER,
    BORDER_LIGHT,
    ElidedLabel,
    TEXT_MUTED,
    TEXT_PRIMARY,
    TEXT_SECONDARY,
    enable_windows_dark_titlebar,
    svg_icon,
)
from world_loader import discover_tracks, track_cache_exists
import world_collision as wc


DEFAULT_EVENT_TEMPLATES: tuple[str, ...] = (
    "All Templates (Combined)",
    "Battle (Free Roam)",
    "No-Rules Race",
    "Figure-8 Jump Race",
    "Demolition Derby",
    "Jump Race",
    "Suicide Race",
    "Gauntlet",
    "School Bus",
    "Trailer Race",
    "Career Overworld (Free Roam)",
)


# ---------------------------------------------------------------------------
# Persistent AppData Store (unified via world_loader.editor_appdata_dir)
# ---------------------------------------------------------------------------
def editor_appdata_dir() -> Path:
    """Return canonical per-user EOD World Editor data dir (respects EOD_EDITOR_APPDATA override)."""
    from world_loader import editor_appdata_dir as _canonical_editor_appdata_dir

    return _canonical_editor_appdata_dir()


def recents_path() -> Path:
    return editor_appdata_dir() / "recents.json"


def prefs_path() -> Path:
    return editor_appdata_dir() / "prefs.json"


def _atomic_write_json(path: Path, payload: Any) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    os.replace(tmp, path)
    return path


def thumbnail_path_for_project(project_path: Path | str) -> Path:
    """Return clean PNG thumbnail path beside an `.eodworld.json` project file."""
    p = Path(project_path)
    name = p.name
    if name.lower().endswith(".eodworld.json"):
        stem = name[: -len(".eodworld.json")]
    else:
        stem = p.stem
    return p.with_name(f"{stem}.thumb.png")


def load_editor_prefs(path: Path | None = None) -> dict[str, Any]:
    fp = Path(path) if path is not None else prefs_path()
    defaults: dict[str, Any] = {
        "skip_home_hub": False,
        "last_track": "EMPTYT_1",
    }
    if not fp.is_file():
        return defaults
    try:
        data = json.loads(fp.read_text(encoding="utf-8"))
        if isinstance(data, dict):
            defaults.update(data)
    except Exception:
        pass
    return defaults


def save_editor_prefs(prefs: dict[str, Any], path: Path | None = None) -> Path:
    fp = Path(path) if path is not None else prefs_path()
    merged = load_editor_prefs(fp)
    merged.update(prefs)
    return _atomic_write_json(fp, merged)


def load_recent_projects(path: Path | None = None) -> list[dict[str, Any]]:
    fp = Path(path) if path is not None else recents_path()
    if not fp.is_file():
        return []
    try:
        data = json.loads(fp.read_text(encoding="utf-8"))
        if isinstance(data, dict) and isinstance(data.get("projects"), list):
            return [e for e in data["projects"] if isinstance(e, dict) and e.get("project_path")]
        if isinstance(data, list):
            return [e for e in data if isinstance(e, dict) and e.get("project_path")]
    except Exception:
        pass
    return []


def record_recent_project(
    project_path: Path | str,
    project_name: str,
    track_name: str,
    *,
    display_name: str = "",
    event_template: str = "",
    thumbnail_path: Path | str | None = None,
    path: Path | None = None,
    max_entries: int = 16,
) -> list[dict[str, Any]]:
    """Insert or bump a project in recents.json atomically and return updated list."""
    fp = Path(path) if path is not None else recents_path()
    norm = str(Path(project_path).resolve())
    thumb_str = ""
    if thumbnail_path:
        tp = Path(thumbnail_path)
        if tp.is_file():
            thumb_str = str(tp.resolve())
    else:
        cand = thumbnail_path_for_project(norm)
        if cand.is_file():
            thumb_str = str(cand.resolve())

    existing = load_recent_projects(fp)
    filtered = [e for e in existing if str(Path(e["project_path"]).resolve()).lower() != norm.lower()]
    entry = {
        "project_path": norm,
        "project_name": project_name or Path(norm).stem,
        "track_name": track_name.upper(),
        "display_name": display_name or track_name.title(),
        "event_template": event_template or "All Templates (Combined)",
        "thumbnail_path": thumb_str,
        "last_edited": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "last_edited_epoch": time.time(),
    }
    updated = [entry] + filtered[: max(0, max_entries - 1)]
    _atomic_write_json(fp, {"schema": "eod.recent_projects/1", "projects": updated})
    return updated


def _format_timestamp(entry: dict[str, Any]) -> str:
    ep = entry.get("last_edited_epoch")
    if isinstance(ep, (int, float)) and ep > 0:
        dt = datetime.fromtimestamp(ep)
        return dt.strftime("%b %d, %H:%M")
    iso = str(entry.get("last_edited", ""))
    return iso[:16].replace("T", " ") if iso else "Recently"


def _make_placeholder_pixmap(title: str, subtitle: str, w: int = 240, h: int = 135) -> QPixmap:
    pm = QPixmap(w, h)
    pm.fill(QColor("#141923"))
    p = QPainter(pm)
    p.setRenderHint(QPainter.RenderHint.Antialiasing, True)
    # Subtle stylized track ring + horizon accent
    p.setPen(QColor("#283548"))
    p.drawRoundedRect(10, 10, w - 20, h - 20, 8, 8)
    p.setPen(QColor("#38bdf8"))
    p.setFont(QFont("Segoe UI", 11, QFont.Weight.Bold))
    p.drawText(pm.rect().adjusted(14, 0, -14, -18), Qt.AlignmentFlag.AlignCenter, title)
    p.setPen(QColor("#94a3b8"))
    p.setFont(QFont("Segoe UI", 8))
    p.drawText(pm.rect().adjusted(14, 24, -14, 0), Qt.AlignmentFlag.AlignCenter, subtitle)
    p.end()
    return pm


# ---------------------------------------------------------------------------
# Recent Project Card Widget
# ---------------------------------------------------------------------------
class RecentProjectCard(QFrame):
    clicked = Signal(str)  # project_path

    def __init__(self, entry: dict[str, Any], parent: QWidget | None = None):
        super().__init__(parent)
        self.entry = entry
        self.project_path = str(entry.get("project_path", ""))
        self.setAccessibleName(str(entry.get("project_name", "Untitled")))
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setObjectName("RecentProjectCard")
        self.setFixedWidth(252)
        self.setStyleSheet(
            f"""
            QFrame#RecentProjectCard {{
                background: {BG_ELEVATED};
                border: 1px solid {BORDER};
                border-radius: 7px;
            }}
            QFrame#RecentProjectCard:hover, QFrame#RecentProjectCard:focus {{
                background: {BG_HOVER};
                border: 1px solid {ACCENT_BLUE};
            }}
            """
        )

        lay = QVBoxLayout(self)
        lay.setContentsMargins(8, 8, 8, 8)
        lay.setSpacing(5)

        thumb_lbl = QLabel()
        thumb_lbl.setFixedSize(234, 128)
        thumb_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        thumb_lbl.setStyleSheet("background: #10141c; border-radius: 4px; border: 1px solid #262c38;")

        tpath = entry.get("thumbnail_path") or ""
        pm = QPixmap(tpath) if (tpath and Path(tpath).is_file()) else QPixmap()
        if pm.isNull():
            pm = _make_placeholder_pixmap(
                str(entry.get("display_name") or entry.get("track_name") or "EOD"),
                str(entry.get("track_name") or ""),
                234,
                128,
            )
        else:
            pm = pm.scaled(
                234,
                128,
                Qt.AspectRatioMode.KeepAspectRatioByExpanding,
                Qt.TransformationMode.SmoothTransformation,
            )
        thumb_lbl.setPixmap(pm)
        lay.addWidget(thumb_lbl)

        title = ElidedLabel(str(entry.get("project_name", "Untitled")))
        title.setStyleSheet(f"color: {TEXT_PRIMARY}; font-weight: 600; background: transparent; border: none;")
        lay.addWidget(title)

        meta = QHBoxLayout()
        meta.setContentsMargins(0, 0, 0, 0)
        trk_lbl = ElidedLabel(str(entry.get("display_name") or entry.get("track_name", "")))
        trk_lbl.setStyleSheet("color: #60a5fa; font-size: 8pt; background: transparent; border: none;")
        ts_lbl = QLabel(_format_timestamp(entry))
        ts_lbl.setStyleSheet(f"color: {TEXT_MUTED}; font-size: 8pt; background: transparent; border: none;")
        meta.addWidget(trk_lbl, 1)
        meta.addWidget(ts_lbl)
        lay.addLayout(meta)

        exists = Path(self.project_path).is_file()
        sub = ElidedLabel(Path(self.project_path).name if exists else "Missing file")
        sub.setStyleSheet(
            f"color: {TEXT_SECONDARY if exists else '#f87171'}; font-size: 7.5pt; background: transparent; border: none;"
        )
        lay.addWidget(sub)

    def mousePressEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self.clicked.emit(self.project_path)
        super().mousePressEvent(event)

    def keyPressEvent(self, event) -> None:
        if event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter, Qt.Key.Key_Space):
            self.clicked.emit(self.project_path)
            event.accept()
            return
        super().keyPressEvent(event)


# ---------------------------------------------------------------------------
# Home Hub Dialog (Adobe / Lumion Dark Windows-11 Launcher)
# ---------------------------------------------------------------------------
class HomeHubDialog(QDialog):
    """Zero-friction Home Hub shown on launch (skipable via --track / prefs / headless tests)."""

    def __init__(
        self,
        game_dir: Path,
        parent: QWidget | None = None,
        *,
        recents_file: Path | None = None,
        prefs_file: Path | None = None,
    ):
        super().__init__(parent)
        self.game_dir = Path(game_dir)
        self._recents_file = recents_file
        self._prefs_file = prefs_file
        self.tracks: list[Path] = discover_tracks(self.game_dir)
        self.prefs = load_editor_prefs(self._prefs_file)
        self.recents = load_recent_projects(self._recents_file)

        # Structured output populated when user picks an action
        self.result_action: str | None = None  # "new_project" | "open_project" | "quick_free_roam" | "vehicle_importer"
        self.selected_project_name: str = "Afton Studio Mod"
        self.selected_track_path: Path | None = self.tracks[0] if self.tracks else None
        self.selected_event_template: str = "Battle (Free Roam)"
        self.selected_project_file: Path | None = None

        self.setWindowTitle("EOD World Editor \u2013 Home Hub")
        self.setMinimumSize(800, 540)
        self.resize(1100, 700)
        self.setStyleSheet(f"QDialog {{ background: {BG_ROOT}; color: {TEXT_PRIMARY}; }}")
        enable_windows_dark_titlebar(self)

        self._build_ui()
        self._populate_tracks()
        self._populate_recents()

    def showEvent(self, event) -> None:
        super().showEvent(event)
        enable_windows_dark_titlebar(self)

    def _build_ui(self) -> None:
        root = QVBoxLayout(self)
        root.setContentsMargins(18, 16, 18, 14)
        root.setSpacing(14)

        # Header banner
        hdr = QHBoxLayout()
        title_box = QVBoxLayout()
        title_box.setSpacing(2)
        lbl_brand = QLabel("EOD World Editor")
        lbl_brand.setFont(QFont("Segoe UI", 16, QFont.Weight.Bold))
        lbl_brand.setStyleSheet(f"color: {TEXT_PRIMARY};")
        lbl_sub = QLabel("Test Drive: Eve of Destruction \u2014 Non-Destructive World & Race Studio")
        lbl_sub.setWordWrap(True)
        lbl_sub.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 9pt;")
        title_box.addWidget(lbl_brand)
        title_box.addWidget(lbl_sub)
        hdr.addLayout(title_box, 1)

        self.chk_skip_hub = QCheckBox("Skip Home Hub on Startup")
        self.chk_skip_hub.setChecked(bool(self.prefs.get("skip_home_hub", False)))
        self.chk_skip_hub.toggled.connect(self._on_skip_pref_toggled)
        hdr.addWidget(self.chk_skip_hub)
        root.addLayout(hdr)

        # Quick Actions bar
        qa_frame = QFrame()
        qa_frame.setStyleSheet(
            f"QFrame {{ background: {BG_PANEL}; border: 1px solid {BORDER}; border-radius: 8px; }}"
        )
        qa_lay = QHBoxLayout(qa_frame)
        qa_lay.setContentsMargins(12, 10, 12, 10)
        qa_lay.setSpacing(10)

        lbl_qa = QLabel("Quick Launch:")
        lbl_qa.setFont(QFont("Segoe UI", 9, QFont.Weight.DemiBold))
        lbl_qa.setStyleSheet(f"color: {TEXT_SECONDARY}; border: none;")
        qa_lay.addWidget(lbl_qa)

        self.btn_open_project = QPushButton(svg_icon("open_world"), " Open .eodworld.json...")
        self.btn_open_project.setMinimumHeight(32)
        self.btn_open_project.clicked.connect(self._on_open_project_browse)
        qa_lay.addWidget(self.btn_open_project)

        self.btn_quick_free_roam = QPushButton(svg_icon("launch", "#2ecc71"), " Quick Free Roam")
        self.btn_quick_free_roam.setMinimumHeight(32)
        self.btn_quick_free_roam.setStyleSheet(
            "QPushButton { background: #1d4d32; border: 1px solid #2ecc71; color: #eafff2; "
            "border-radius: 5px; padding: 4px 12px; font-weight: 600; }"
            "QPushButton:hover { background: #256440; }"
        )
        self.btn_quick_free_roam.clicked.connect(self._on_quick_free_roam)
        qa_lay.addWidget(self.btn_quick_free_roam)

        self.btn_vehicle_importer = QPushButton(svg_icon("car", "#fbbf24"), " Vehicle / NASCAR Importer")
        self.btn_vehicle_importer.setMinimumHeight(32)
        self.btn_vehicle_importer.setStyleSheet(
            "QPushButton { background: #3b2d14; border: 1px solid #d97706; color: #fef3c7; "
            "border-radius: 5px; padding: 4px 12px; font-weight: 600; }"
            "QPushButton:hover { background: #4d3a1a; }"
        )
        self.btn_vehicle_importer.clicked.connect(self._on_vehicle_importer)
        qa_lay.addWidget(self.btn_vehicle_importer)

        qa_lay.addStretch(1)
        root.addWidget(qa_frame)

        # Main 2-column body: Left (New World Project) | Right (Recent Projects Grid)
        body = QHBoxLayout()
        body.setSpacing(14)

        # --- Left Column: New World Project ---
        left_panel = QFrame()
        left_panel.setMinimumWidth(330)
        left_panel.setMaximumWidth(385)
        left_panel.setStyleSheet(
            f"QFrame {{ background: {BG_PANEL}; border: 1px solid {BORDER}; border-radius: 8px; }}"
        )
        l_lay = QVBoxLayout(left_panel)
        l_lay.setContentsMargins(14, 14, 14, 14)
        l_lay.setSpacing(10)

        lbl_new = QLabel("New World Project")
        lbl_new.setFont(QFont("Segoe UI", 11.5, QFont.Weight.Bold))
        lbl_new.setStyleSheet(f"color: {TEXT_PRIMARY}; border: none;")
        l_lay.addWidget(lbl_new)

        lbl_pn = QLabel("Project Name")
        lbl_pn.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 8.5pt; border: none;")
        l_lay.addWidget(lbl_pn)

        self.txt_project_name = QLineEdit("Afton Studio Mod")
        self.txt_project_name.setPlaceholderText("Enter a name for your world mod...")
        self.txt_project_name.setMinimumHeight(30)
        self.txt_project_name.setStyleSheet(
            f"QLineEdit {{ background: {BG_INPUT}; border: 1px solid {BORDER_LIGHT}; border-radius: 5px; "
            f"padding: 4px 8px; color: {TEXT_PRIMARY}; font-size: 9.5pt; }}"
        )
        l_lay.addWidget(self.txt_project_name)

        lbl_trk = QLabel("Retail Track (.TRK)")
        lbl_trk.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 8.5pt; border: none;")
        l_lay.addWidget(lbl_trk)

        self.lst_tracks = QListWidget()
        self.lst_tracks.setMinimumHeight(72)
        self.lst_tracks.setStyleSheet(
            f"QListWidget {{ background: {BG_INPUT}; border: 1px solid {BORDER}; border-radius: 5px; padding: 2px; }}"
            f"QListWidget::item {{ padding: 5px 8px; border-radius: 4px; }}"
            f"QListWidget::item:selected {{ background: {ACCENT_BLUE}; color: white; }}"
        )
        self.lst_tracks.currentRowChanged.connect(self._on_track_row_changed)
        l_lay.addWidget(self.lst_tracks, 1)

        # Compact track preview card
        self.track_preview_card = QFrame()
        self.track_preview_card.setStyleSheet(
            f"QFrame {{ background: {BG_ELEVATED}; border: 1px solid {BORDER}; border-radius: 6px; }}"
        )
        tp_lay = QHBoxLayout(self.track_preview_card)
        tp_lay.setContentsMargins(10, 8, 10, 8)
        tp_lay.setSpacing(10)
        self.lbl_track_preview_img = QLabel()
        self.lbl_track_preview_img.setFixedSize(112, 64)
        self.lbl_track_preview_img.setStyleSheet("background: #0f131b; border-radius: 4px; border: 1px solid #262c38;")
        tp_lay.addWidget(self.lbl_track_preview_img)
        tp_meta = QVBoxLayout()
        tp_meta.setSpacing(2)
        self.lbl_track_preview_title = QLabel("Select a track")
        self.lbl_track_preview_title.setFont(QFont("Segoe UI", 9.5, QFont.Weight.DemiBold))
        self.lbl_track_preview_title.setStyleSheet(f"color: {TEXT_PRIMARY}; border: none;")
        self.lbl_track_preview_meta = QLabel("")
        self.lbl_track_preview_meta.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 8pt; border: none;")
        self.lbl_track_cache_badge = QLabel("")
        self.lbl_track_cache_badge.setStyleSheet("color: #4ade80; font-size: 8pt; font-weight: 600; border: none;")
        tp_meta.addWidget(self.lbl_track_preview_title)
        tp_meta.addWidget(self.lbl_track_preview_meta)
        tp_meta.addWidget(self.lbl_track_cache_badge)
        tp_lay.addLayout(tp_meta, 1)
        l_lay.addWidget(self.track_preview_card)

        lbl_ev = QLabel("Event Template")
        lbl_ev.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 8.5pt; border: none;")
        l_lay.addWidget(lbl_ev)

        self.cmb_event_template = QComboBox()
        self.cmb_event_template.setMinimumHeight(28)
        self.cmb_event_template.addItems(list(DEFAULT_EVENT_TEMPLATES))
        idx_b = self.cmb_event_template.findText("Battle (Free Roam)")
        if idx_b >= 0:
            self.cmb_event_template.setCurrentIndex(idx_b)
        l_lay.addWidget(self.cmb_event_template)

        self.btn_create = QPushButton(svg_icon("new_world", "#ffffff"), " Create World Project")
        self.btn_create.setMinimumHeight(36)
        self.btn_create.setStyleSheet(
            f"QPushButton {{ background: {ACCENT_BLUE}; border: 1px solid {ACCENT_BLUE_HOVER}; "
            f"color: white; border-radius: 6px; font-size: 10pt; font-weight: 700; }}"
            f"QPushButton:hover {{ background: {ACCENT_BLUE_HOVER}; }}"
        )
        self.btn_create.clicked.connect(self._on_create_project)
        l_lay.addWidget(self.btn_create)

        body.addWidget(left_panel)

        # --- Right Column: Recent Projects Grid ---
        right_panel = QFrame()
        right_panel.setStyleSheet(
            f"QFrame {{ background: {BG_PANEL}; border: 1px solid {BORDER}; border-radius: 8px; }}"
        )
        r_lay = QVBoxLayout(right_panel)
        r_lay.setContentsMargins(14, 14, 14, 14)
        r_lay.setSpacing(10)

        r_hdr = QHBoxLayout()
        lbl_rec = QLabel("Recent Projects")
        lbl_rec.setFont(QFont("Segoe UI", 11.5, QFont.Weight.Bold))
        lbl_rec.setStyleSheet(f"color: {TEXT_PRIMARY}; border: none;")
        self.lbl_rec_count = QLabel("0 saved")
        self.lbl_rec_count.setStyleSheet(f"color: {TEXT_MUTED}; font-size: 8.5pt; border: none;")
        r_hdr.addWidget(lbl_rec)
        r_hdr.addStretch(1)
        r_hdr.addWidget(self.lbl_rec_count)
        r_lay.addLayout(r_hdr)

        self.recents_scroll = QScrollArea()
        self.recents_scroll.setWidgetResizable(True)
        self.recents_scroll.setFrameShape(QFrame.Shape.NoFrame)
        self.recents_scroll.setStyleSheet("QScrollArea { background: transparent; border: none; }")
        self.recents_container = QWidget()
        self.recents_container.setStyleSheet("background: transparent;")
        self.recents_grid = QGridLayout(self.recents_container)
        self.recents_grid.setContentsMargins(2, 2, 2, 2)
        self.recents_grid.setSpacing(10)
        self.recents_scroll.setWidget(self.recents_container)
        self.recents_scroll.viewport().installEventFilter(self)
        self.recent_cards: list[RecentProjectCard] = []
        self._recent_columns = 0
        r_lay.addWidget(self.recents_scroll, 1)

        body.addWidget(right_panel, 1)
        root.addLayout(body, 1)

        # Footer
        foot = QHBoxLayout()
        self.lbl_game_dir = ElidedLabel(f"Retail Disc: {self.game_dir}")
        self.lbl_game_dir.setStyleSheet(f"color: {TEXT_MUTED}; font-size: 8pt;")
        foot.addWidget(self.lbl_game_dir, 1)
        btn_cancel = QPushButton("Close")
        btn_cancel.clicked.connect(self.reject)
        foot.addWidget(btn_cancel)
        root.addLayout(foot)

    def _populate_tracks(self) -> None:
        self.lst_tracks.clear()
        want_last = str(self.prefs.get("last_track", "EMPTYT_1")).upper()
        select_row = 0
        for idx, tp in enumerate(self.tracks):
            disp = wc.track_display_name(tp)
            item = QListWidgetItem(f"{disp}  ({tp.stem.upper()})")
            item.setData(Qt.ItemDataRole.UserRole, str(tp))
            self.lst_tracks.addItem(item)
            if tp.stem.upper() == want_last:
                select_row = idx
        if self.tracks:
            self.lst_tracks.setCurrentRow(select_row)
            self._on_track_row_changed(select_row)

    def _on_track_row_changed(self, row: int) -> None:
        if row < 0 or row >= len(self.tracks):
            return
        tp = self.tracks[row]
        self.selected_track_path = tp
        disp = wc.track_display_name(tp)
        mb_sz = tp.stat().st_size / (1024.0 * 1024.0) if tp.is_file() else 0.0
        cached = track_cache_exists(tp)
        self.lbl_track_preview_title.setText(disp)
        self.lbl_track_preview_meta.setText(f"{tp.name}  \u2022  {mb_sz:.1f} MB")
        if cached:
            self.lbl_track_cache_badge.setText("\u26a1 Turbo Binary Cache Ready")
            self.lbl_track_cache_badge.setStyleSheet("color: #4ade80; font-size: 8pt; font-weight: 600; border: none;")
        else:
            self.lbl_track_cache_badge.setText("First load builds Turbo Cache")
            self.lbl_track_cache_badge.setStyleSheet("color: #94a3b8; font-size: 8pt; border: none;")

        # Show thumbnail from most recent project on this track if available
        thumb_pm = QPixmap()
        for r in self.recents:
            if str(r.get("track_name", "")).upper() == tp.stem.upper():
                tpath = r.get("thumbnail_path") or ""
                if tpath and Path(tpath).is_file():
                    thumb_pm = QPixmap(tpath)
                    if not thumb_pm.isNull():
                        thumb_pm = thumb_pm.scaled(
                            112, 64, Qt.AspectRatioMode.KeepAspectRatioByExpanding, Qt.TransformationMode.SmoothTransformation
                        )
                        break
        if thumb_pm.isNull():
            thumb_pm = _make_placeholder_pixmap(tp.stem.upper(), disp, 112, 64)
        self.lbl_track_preview_img.setPixmap(thumb_pm)

        # Auto-suggest default project name if user hasn't customized it away from "<Track> Studio Mod"
        cur_name = self.txt_project_name.text().strip()
        if not cur_name or cur_name.endswith(" Studio Mod"):
            self.txt_project_name.setText(f"{disp} Studio Mod")

    def _populate_recents(self) -> None:
        while self.recents_grid.count():
            it = self.recents_grid.takeAt(0)
            if it and it.widget():
                it.widget().deleteLater()

        self.recent_cards.clear()
        self._recent_columns = 0
        self.lbl_rec_count.setText(f"{len(self.recents)} saved")
        if not self.recents:
            empty = QLabel(
                "No recent projects yet.\n\nCreate a New World Project on the left or click Open .eodworld.json.\n"
                "Viewport thumbnails are saved automatically beside each project!"
            )
            empty.setAlignment(Qt.AlignmentFlag.AlignCenter)
            empty.setWordWrap(True)
            empty.setStyleSheet(f"color: {TEXT_MUTED}; font-size: 9.5pt; padding: 40px;")
            self.recents_grid.addWidget(empty, 0, 0, 1, 2)
            return

        for entry in self.recents:
            card = RecentProjectCard(entry, self.recents_container)
            card.clicked.connect(self._on_recent_card_clicked)
            self.recent_cards.append(card)
        self._fit_recent_cards()

    def eventFilter(self, watched, event) -> bool:
        if watched is self.recents_scroll.viewport() and event.type() == QEvent.Type.Resize:
            self._fit_recent_cards()
        return super().eventFilter(watched, event)

    def _fit_recent_cards(self) -> None:
        if not self.recent_cards:
            return
        grid = self.recents_grid
        margins = grid.contentsMargins()
        available = self.recents_scroll.viewport().width() - margins.left() - margins.right()
        cols = 2 if available >= 2 * self.recent_cards[0].width() + grid.horizontalSpacing() else 1
        if cols == self._recent_columns:
            return
        for row in range(grid.rowCount()):
            grid.setRowStretch(row, 0)
        for idx, card in enumerate(self.recent_cards):
            grid.removeWidget(card)
            grid.addWidget(card, idx // cols, idx % cols, Qt.AlignmentFlag.AlignTop)
        grid.setRowStretch((len(self.recent_cards) - 1) // cols + 1, 1)
        self._recent_columns = cols

    def _on_skip_pref_toggled(self, checked: bool) -> None:
        self.prefs["skip_home_hub"] = bool(checked)
        save_editor_prefs({"skip_home_hub": bool(checked)}, self._prefs_file)

    def _commit_selected_track_pref(self) -> None:
        if self.selected_track_path is not None:
            save_editor_prefs({"last_track": self.selected_track_path.stem.upper()}, self._prefs_file)

    def _on_create_project(self) -> None:
        self.selected_project_name = self.txt_project_name.text().strip() or "Untitled World"
        self.selected_event_template = self.cmb_event_template.currentText().strip() or "Battle (Free Roam)"
        self._commit_selected_track_pref()
        self.result_action = "new_project"
        self.accept()

    def _on_recent_card_clicked(self, project_path: str) -> None:
        self.selected_project_file = Path(project_path)
        self.result_action = "open_project"
        self.accept()

    def _on_open_project_browse(self) -> None:
        fn, _ = QFileDialog.getOpenFileName(
            self, "Open World Project", str(self.game_dir), "EOD World Project (*.eodworld.json);;JSON (*.json)"
        )
        if not fn:
            return
        self.selected_project_file = Path(fn)
        self.result_action = "open_project"
        self.accept()

    def _on_quick_free_roam(self) -> None:
        self.selected_project_name = self.txt_project_name.text().strip() or "Quick Free Roam"
        self.selected_event_template = "Battle (Free Roam)"
        self._commit_selected_track_pref()
        self.result_action = "quick_free_roam"
        self.accept()

    def _on_vehicle_importer(self) -> None:
        self.selected_project_name = self.txt_project_name.text().strip() or "Vehicle Workshop"
        self.selected_event_template = self.cmb_event_template.currentText().strip() or "Battle (Free Roam)"
        self._commit_selected_track_pref()
        self.result_action = "vehicle_importer"
        self.accept()
