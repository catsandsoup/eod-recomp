"""Native Windows 11 Dark Chrome, Qt 6 Fusion + QSS theme, crisp SVG icon factory,
and R/G/B axis-badged numeric editors for EOD World Editor.
"""

from __future__ import annotations

import ctypes
import sys
from functools import lru_cache

from PySide6.QtCore import QByteArray, QSize, Qt, Signal
from PySide6.QtGui import QColor, QFont, QFontDatabase, QIcon, QPainter, QPalette, QPixmap
from PySide6.QtSvg import QSvgRenderer
from PySide6.QtWidgets import (
    QApplication,
    QDoubleSpinBox,
    QHBoxLayout,
    QLabel,
    QSizePolicy,
    QWidget,
)

# ---------------------------------------------------------------------------
# Color palette constants (commercial dark desktop UI)
# ---------------------------------------------------------------------------
BG_ROOT = "#18191d"
BG_PANEL = "#1b1c20"
BG_ELEVATED = "#22242a"
BG_INPUT = "#15161a"
BG_HOVER = "#2a2d35"
BG_SELECTED = "#263652"
BORDER = "#32353e"
BORDER_LIGHT = "#3f434e"
TEXT_PRIMARY = "#e6e9f0"
TEXT_SECONDARY = "#9aa1b2"
TEXT_MUTED = "#6d7485"
ACCENT_BLUE = "#2d7ff9"
ACCENT_BLUE_HOVER = "#3e8bff"
ACCENT_GREEN = "#2ecc71"
ACCENT_RED = "#e05252"
ACCENT_AMBER = "#f0a938"

AXIS_X_COLOR = "#d94a4a"
AXIS_Y_COLOR = "#43b564"
AXIS_Z_COLOR = "#3b82f6"


def enable_windows_dark_titlebar(widget: QWidget) -> bool:
    """Enable native Windows 11 immersive dark title bar via DwmSetWindowAttribute."""
    if sys.platform != "win32":
        return False
    try:
        hwnd = int(widget.winId())
        if not hwnd:
            return False
        dwmapi = ctypes.windll.dwmapi
        value = ctypes.c_int(1)
        # DWMWA_USE_IMMERSIVE_DARK_MODE = 20 (Win10 2004+ / Win11), fallback 19
        for attr in (20, 19):
            hr = dwmapi.DwmSetWindowAttribute(
                ctypes.c_void_p(hwnd),
                ctypes.c_uint(attr),
                ctypes.byref(value),
                ctypes.sizeof(value),
            )
            if hr == 0:
                # Optionally set caption color (#1b1c20 -> COLORREF 0x00201c1b) on Win11
                try:
                    caption_color = ctypes.c_int(0x00201C1B)
                    dwmapi.DwmSetWindowAttribute(
                        ctypes.c_void_p(hwnd),
                        ctypes.c_uint(35),  # DWMWA_CAPTION_COLOR
                        ctypes.byref(caption_color),
                        ctypes.sizeof(caption_color),
                    )
                except Exception:
                    pass
                return True
    except Exception:
        return False
    return False


# ---------------------------------------------------------------------------
# SVG Icon Factory
# ---------------------------------------------------------------------------
_SVG_PATHS: dict[str, str] = {
    "new_world": (
        '<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z" '
        'fill="none" stroke="{c}" stroke-width="1.8" stroke-linejoin="round"/>'
        '<polyline points="14 2 14 8 20 8" fill="none" stroke="{c}" stroke-width="1.8"/>'
        '<line x1="12" y1="12" x2="12" y2="18" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<line x1="9" y1="15" x2="15" y2="15" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
    ),
    "open_world": (
        '<path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z" '
        'fill="none" stroke="{c}" stroke-width="1.8" stroke-linejoin="round"/>'
    ),
    "save": (
        '<path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z" '
        'fill="none" stroke="{c}" stroke-width="1.8" stroke-linejoin="round"/>'
        '<polyline points="17 21 17 13 7 13 7 21" fill="none" stroke="{c}" stroke-width="1.8"/>'
        '<polyline points="7 3 7 8 15 8" fill="none" stroke="{c}" stroke-width="1.8"/>'
    ),
    "marker": (
        '<path d="M21 10c0 7-9 13-9 13s-9-6-9-13a9 9 0 0 1 18 0z" '
        'fill="none" stroke="{c}" stroke-width="1.8"/>'
        '<circle cx="12" cy="10" r="3" fill="none" stroke="{c}" stroke-width="1.8"/>'
    ),
    "car": (
        '<path d="M5 17h14v-5l-2-4H7l-2 4v5z" fill="none" stroke="{c}" stroke-width="1.8" stroke-linejoin="round"/>'
        '<circle cx="7.5" cy="17.5" r="2" fill="none" stroke="{c}" stroke-width="1.8"/>'
        '<circle cx="16.5" cy="17.5" r="2" fill="none" stroke="{c}" stroke-width="1.8"/>'
        '<line x1="6" y1="12" x2="18" y2="12" stroke="{c}" stroke-width="1.5"/>'
    ),
    "launch": (
        '<polygon points="6 4 20 12 6 20 6 4" fill="{c}" stroke="{c}" stroke-width="1.5" stroke-linejoin="round"/>'
    ),
    "move": (
        '<polyline points="5 9 2 12 5 15" fill="none" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<polyline points="9 5 12 2 15 5" fill="none" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<polyline points="15 19 12 22 9 19" fill="none" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<polyline points="19 9 22 12 19 15" fill="none" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<line x1="2" y1="12" x2="22" y2="12" stroke="{c}" stroke-width="1.8"/>'
        '<line x1="12" y1="2" x2="12" y2="22" stroke="{c}" stroke-width="1.8"/>'
    ),
    "rotate": (
        '<path d="M21 12a9 9 0 1 1-2.64-6.36" fill="none" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<polyline points="21 3 21 9 15 9" fill="none" stroke="{c}" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>'
    ),
    "scale": (
        '<rect x="3" y="13" width="8" height="8" rx="1" fill="none" stroke="{c}" stroke-width="1.8"/>'
        '<polyline points="15 3 21 3 21 9" fill="none" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<line x1="14" y1="10" x2="21" y2="3" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
    ),
    "road": (
        '<path d="M4 20L9 4M20 20L15 4" fill="none" stroke="{c}" stroke-width="1.9" stroke-linecap="round"/>'
        '<line x1="12" y1="6" x2="12" y2="9" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<line x1="12" y1="12" x2="12" y2="15" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<line x1="12" y1="18" x2="12" y2="20" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
    ),
    "shield": (
        '<path d="M12 2L4 5v6c0 5.25 3.4 10.1 8 11.5 4.6-1.4 8-6.25 8-11.5V5l-8-3z" '
        'fill="none" stroke="{c}" stroke-width="1.8" stroke-linejoin="round"/>'
    ),
    "tree": (
        '<line x1="8" y1="6" x2="21" y2="6" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<line x1="8" y1="12" x2="21" y2="12" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<line x1="8" y1="18" x2="21" y2="18" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<circle cx="4" cy="6" r="1.3" fill="{c}"/>'
        '<circle cx="4" cy="12" r="1.3" fill="{c}"/>'
        '<circle cx="4" cy="18" r="1.3" fill="{c}"/>'
    ),
    "cube": (
        '<path d="M12 2L3 7v10l9 5 9-5V7l-9-5z" fill="none" stroke="{c}" stroke-width="1.8" stroke-linejoin="round"/>'
        '<polyline points="3 7 12 12 21 7" fill="none" stroke="{c}" stroke-width="1.6"/>'
        '<line x1="12" y1="12" x2="12" y2="22" stroke="{c}" stroke-width="1.6"/>'
    ),
    "sections": (
        '<rect x="3" y="3" width="7" height="7" rx="1" fill="none" stroke="{c}" stroke-width="1.8"/>'
        '<rect x="14" y="3" width="7" height="7" rx="1" fill="none" stroke="{c}" stroke-width="1.8"/>'
        '<rect x="3" y="14" width="7" height="7" rx="1" fill="none" stroke="{c}" stroke-width="1.8"/>'
        '<rect x="14" y="14" width="7" height="7" rx="1" fill="none" stroke="{c}" stroke-width="1.8"/>'
    ),
    "layers": (
        '<polygon points="12 2 2 7 12 12 22 7 12 2" fill="none" stroke="{c}" stroke-width="1.8" stroke-linejoin="round"/>'
        '<polyline points="2 12 12 17 22 12" fill="none" stroke="{c}" stroke-width="1.8" stroke-linejoin="round"/>'
        '<polyline points="2 17 12 22 22 17" fill="none" stroke="{c}" stroke-width="1.8" stroke-linejoin="round"/>'
    ),
    "ground": (
        '<line x1="12" y1="3" x2="12" y2="15" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<polyline points="8 11 12 15 16 11" fill="none" stroke="{c}" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>'
        '<line x1="4" y1="19" x2="20" y2="19" stroke="{c}" stroke-width="2" stroke-linecap="round"/>'
    ),
    "trash": (
        '<polyline points="3 6 5 6 21 6" fill="none" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<path d="M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6m3 0V4a1 1 0 0 1 1-1h6a1 1 0 0 1 1 1v2" '
        'fill="none" stroke="{c}" stroke-width="1.8" stroke-linejoin="round"/>'
    ),
    "blender": (
        '<circle cx="13" cy="13" r="5" fill="none" stroke="{c}" stroke-width="1.8"/>'
        '<circle cx="13" cy="13" r="2" fill="{c}"/>'
        '<line x1="4" y1="13" x2="8" y2="13" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<line x1="7" y1="7" x2="10" y2="10" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<line x1="7" y1="19" x2="10" y2="16" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
    ),
    "search": (
        '<circle cx="11" cy="11" r="7" fill="none" stroke="{c}" stroke-width="1.8"/>'
        '<line x1="16.65" y1="16.65" x2="21" y2="21" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
    ),
    "undo": (
        '<polyline points="9 14 4 9 9 4" fill="none" stroke="{c}" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>'
        '<path d="M20 20v-7a4 4 0 0 0-4-4H4" fill="none" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
    ),
    "redo": (
        '<polyline points="15 14 20 9 15 4" fill="none" stroke="{c}" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>'
        '<path d="M4 20v-7a4 4 0 0 1 4-4h12" fill="none" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
    ),
    "object_mode": (
        '<polygon points="12 2 21 7 21 17 12 22 3 17 3 7 12 2" fill="none" stroke="{c}" stroke-width="1.8"/>'
    ),
    "edit_mode": (
        '<path d="M12 20h9" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<path d="M16.5 3.5a2.12 2.12 0 0 1 3 3L7 19l-4 1 1-4L16.5 3.5z" fill="none" stroke="{c}" stroke-width="1.8"/>'
    ),
    "place": (
        '<path d="M12 2L4 6.5v11L12 22l8-4.5v-11L12 2z" fill="none" stroke="{c}" stroke-width="1.7" stroke-dasharray="3 2"/>'
        '<line x1="12" y1="8" x2="12" y2="16" stroke="{c}" stroke-width="1.9" stroke-linecap="round"/>'
        '<line x1="8" y1="12" x2="16" y2="12" stroke="{c}" stroke-width="1.9" stroke-linecap="round"/>'
    ),
    "scatter": (
        '<circle cx="7" cy="7" r="2.2" fill="{c}"/>'
        '<circle cx="16.5" cy="6.5" r="1.8" fill="{c}"/>'
        '<circle cx="12" cy="13" r="2.4" fill="{c}"/>'
        '<circle cx="6.5" cy="17" r="1.8" fill="{c}"/>'
        '<circle cx="17.5" cy="16.5" r="2.1" fill="{c}"/>'
    ),
    "replace": (
        '<polyline points="17 2 21 6 17 10" fill="none" stroke="{c}" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>'
        '<path d="M3 11V9a3 3 0 0 1 3-3h15" fill="none" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<polyline points="7 22 3 18 7 14" fill="none" stroke="{c}" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>'
        '<path d="M21 13v2a3 3 0 0 1-3 3H3" fill="none" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
    ),
    "sky": (
        '<circle cx="12" cy="12" r="4.5" fill="none" stroke="{c}" stroke-width="1.8"/>'
        '<line x1="12" y1="2" x2="12" y2="5" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<line x1="12" y1="19" x2="12" y2="22" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<line x1="2" y1="12" x2="5" y2="12" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<line x1="19" y1="12" x2="22" y2="12" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<line x1="4.9" y1="4.9" x2="7.1" y2="7.1" stroke="{c}" stroke-width="1.6" stroke-linecap="round"/>'
        '<line x1="16.9" y1="16.9" x2="19.1" y2="19.1" stroke="{c}" stroke-width="1.6" stroke-linecap="round"/>'
        '<line x1="19.1" y1="4.9" x2="16.9" y2="7.1" stroke="{c}" stroke-width="1.6" stroke-linecap="round"/>'
        '<line x1="7.1" y1="16.9" x2="4.9" y2="19.1" stroke="{c}" stroke-width="1.6" stroke-linecap="round"/>'
    ),
    "snap_grid": (
        '<rect x="3" y="3" width="18" height="18" rx="1.5" fill="none" stroke="{c}" stroke-width="1.7"/>'
        '<line x1="9" y1="3" x2="9" y2="21" stroke="{c}" stroke-width="1.4"/>'
        '<line x1="15" y1="3" x2="15" y2="21" stroke="{c}" stroke-width="1.4"/>'
        '<line x1="3" y1="9" x2="21" y2="9" stroke="{c}" stroke-width="1.4"/>'
        '<line x1="3" y1="15" x2="21" y2="15" stroke="{c}" stroke-width="1.4"/>'
    ),
    "snap_angle": (
        '<path d="M4 19h16" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<path d="M4 19L16 6" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<path d="M11 19a7 7 0 0 0-2.2-5.1" fill="none" stroke="{c}" stroke-width="1.7"/>'
    ),
    "stop": (
        '<rect x="5" y="5" width="14" height="14" rx="2" fill="{c}" stroke="{c}" stroke-width="1.5"/>'
    ),
    "popout": (
        '<path d="M14 3h7v7" fill="none" stroke="{c}" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>'
        '<line x1="10" y1="14" x2="21" y2="3" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
        '<path d="M19 14v5a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V7a2 2 0 0 1 2-2h5" fill="none" stroke="{c}" stroke-width="1.8" stroke-linecap="round"/>'
    ),
    "race": (
        '<path d="M4 21V4" stroke="{c}" stroke-width="1.9" stroke-linecap="round"/>'
        '<path d="M4 5h12l-2 4 2 4H4" fill="none" stroke="{c}" stroke-width="1.8" stroke-linejoin="round"/>'
    ),
    "texture": (
        '<rect x="3" y="3" width="18" height="18" rx="2" fill="none" stroke="{c}" stroke-width="1.8"/>'
        '<circle cx="8.5" cy="8.5" r="1.8" fill="{c}"/>'
        '<polyline points="21 15 16 10 5 21" fill="none" stroke="{c}" stroke-width="1.7" stroke-linejoin="round"/>'
    ),
}


@lru_cache(maxsize=256)
def svg_pixmap(name: str, color: str = TEXT_PRIMARY, size: int = 18) -> QPixmap:
    body = _SVG_PATHS.get(name, _SVG_PATHS["cube"]).format(c=color)
    svg = (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" '
        f'width="{size}" height="{size}">{body}</svg>'
    )
    renderer = QSvgRenderer(QByteArray(svg.encode("utf-8")))
    pm = QPixmap(size, size)
    pm.fill(Qt.GlobalColor.transparent)
    painter = QPainter(pm)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
    renderer.render(painter)
    painter.end()
    return pm


@lru_cache(maxsize=256)
def svg_icon(name: str, color: str = TEXT_PRIMARY, active_color: str = "#ffffff", size: int = 18) -> QIcon:
    icon = QIcon()
    icon.addPixmap(svg_pixmap(name, color, size), QIcon.Mode.Normal, QIcon.State.Off)
    icon.addPixmap(svg_pixmap(name, active_color, size), QIcon.Mode.Normal, QIcon.State.On)
    icon.addPixmap(svg_pixmap(name, active_color, size), QIcon.Mode.Active, QIcon.State.On)
    icon.addPixmap(svg_pixmap(name, TEXT_MUTED, size), QIcon.Mode.Disabled, QIcon.State.Off)
    return icon


# ---------------------------------------------------------------------------
# Application theme (Qt 6 Fusion + Commercial Dark QSS)
# ---------------------------------------------------------------------------
QSS_STYLESHEET = f"""
QMainWindow, QWidget {{
    background-color: {BG_PANEL};
    color: {TEXT_PRIMARY};
    font-size: 9pt;
}}

QMenuBar {{
    background-color: {BG_ROOT};
    color: {TEXT_PRIMARY};
    border-bottom: 1px solid {BORDER};
    padding: 1px 4px;
    spacing: 2px;
}}
QMenuBar::item {{
    background: transparent;
    padding: 4px 8px;
    border-radius: 4px;
}}
QMenuBar::item:selected {{
    background-color: {BG_HOVER};
}}
QMenuBar::item:pressed {{
    background-color: {ACCENT_BLUE};
    color: #ffffff;
}}

QMenu {{
    background-color: {BG_ELEVATED};
    color: {TEXT_PRIMARY};
    border: 1px solid {BORDER_LIGHT};
    border-radius: 6px;
    padding: 4px;
}}
QMenu::item {{
    padding: 5px 24px 5px 12px;
    border-radius: 4px;
}}
QMenu::item:selected {{
    background-color: {ACCENT_BLUE};
    color: #ffffff;
}}
QMenu::separator {{
    height: 1px;
    background: {BORDER};
    margin: 4px 6px;
}}

QToolBar {{
    background-color: {BG_PANEL};
    border-bottom: 1px solid {BORDER};
    spacing: 3px;
    padding: 3px 6px;
}}
QToolBar::separator {{
    width: 1px;
    background-color: {BORDER};
    margin: 4px 6px;
}}
QToolButton {{
    background-color: transparent;
    color: {TEXT_PRIMARY};
    border: 1px solid transparent;
    border-radius: 5px;
    padding: 4px 8px;
    font-weight: 500;
}}
QToolButton:hover {{
    background-color: {BG_HOVER};
    border-color: {BORDER};
}}
QToolButton:pressed {{
    background-color: #1f222a;
}}
QToolButton:checked {{
    background-color: {ACCENT_BLUE};
    color: #ffffff;
    border-color: {ACCENT_BLUE_HOVER};
}}

QSplitter::handle {{
    background-color: {BORDER};
}}
QSplitter::handle:horizontal {{
    width: 1px;
}}
QSplitter::handle:vertical {{
    height: 1px;
}}

QLineEdit {{
    background-color: {BG_INPUT};
    color: {TEXT_PRIMARY};
    border: 1px solid {BORDER};
    border-radius: 4px;
    padding: 4px 7px;
    selection-background-color: {ACCENT_BLUE};
}}
QDoubleSpinBox QLineEdit, QSpinBox QLineEdit {{
    border: none;
    padding: 0px 2px;
    margin: 0px;
    background: transparent;
    color: {TEXT_PRIMARY};
}}
QLineEdit:focus {{
    border-color: {ACCENT_BLUE};
}}
QComboBox QAbstractItemView {{
    background-color: {BG_ELEVATED};
    color: {TEXT_PRIMARY};
    border: 1px solid {BORDER_LIGHT};
    selection-background-color: {ACCENT_BLUE};
    outline: none;
}}

QPushButton {{
    background-color: {BG_ELEVATED};
    color: {TEXT_PRIMARY};
    border: 1px solid {BORDER};
    border-radius: 5px;
    padding: 5px 10px;
    font-weight: 500;
}}
QPushButton:hover {{
    background-color: {BG_HOVER};
    border-color: {BORDER_LIGHT};
}}
QPushButton:pressed {{
    background-color: #1e2027;
}}
QPushButton:disabled {{
    color: {TEXT_MUTED};
    background-color: #1a1b1f;
}}
QPushButton[primary="true"] {{
    background-color: {ACCENT_BLUE};
    border-color: {ACCENT_BLUE_HOVER};
    color: #ffffff;
}}
QPushButton[primary="true"]:hover {{
    background-color: {ACCENT_BLUE_HOVER};
}}
QPushButton[danger="true"] {{
    background-color: #3a1f24;
    border-color: #6b2b34;
    color: #ff99a2;
}}
QPushButton[danger="true"]:hover {{
    background-color: #54252d;
    border-color: {ACCENT_RED};
    color: #ffffff;
}}

QCheckBox {{
    color: {TEXT_SECONDARY};
    spacing: 6px;
}}
QCheckBox::indicator {{
    width: 14px;
    height: 14px;
    border-radius: 3px;
    border: 1px solid {BORDER_LIGHT};
    background-color: {BG_INPUT};
}}
QCheckBox::indicator:checked {{
    background-color: {ACCENT_BLUE};
    border-color: {ACCENT_BLUE_HOVER};
}}

QTabWidget::pane {{
    border: none;
    background-color: {BG_PANEL};
}}
QTabBar::tab {{
    background-color: {BG_ROOT};
    color: {TEXT_SECONDARY};
    border: 1px solid {BORDER};
    border-bottom: none;
    border-top-left-radius: 4px;
    border-top-right-radius: 4px;
    padding: 5px 6px;
    margin-right: 1px;
    font-size: 8.5pt;
}}
QTabBar::tab:selected {{
    background-color: {BG_ELEVATED};
    color: {TEXT_PRIMARY};
    border-color: {BORDER_LIGHT};
}}
QTabBar::tab:hover:!selected {{
    background-color: {BG_HOVER};
    color: {TEXT_PRIMARY};
}}

QListWidget, QTreeWidget {{
    background-color: {BG_INPUT};
    color: {TEXT_PRIMARY};
    border: 1px solid {BORDER};
    border-radius: 4px;
    outline: none;
}}
QListWidget::item {{
    background-color: {BG_ELEVATED};
    border: 1px solid {BORDER};
    border-radius: 5px;
    padding: 4px;
    margin: 2px;
}}
QListWidget::item:hover {{
    border-color: {BORDER_LIGHT};
    background-color: {BG_HOVER};
}}
QListWidget::item:selected {{
    border: 1px solid {ACCENT_BLUE};
    background-color: {BG_SELECTED};
}}
QTreeWidget::item {{
    padding: 3px 4px;
}}
QTreeWidget::item:hover {{
    background-color: {BG_HOVER};
}}
QTreeWidget::item:selected {{
    background-color: {BG_SELECTED};
    color: #ffffff;
}}
QHeaderView::section {{
    background-color: {BG_ELEVATED};
    color: {TEXT_SECONDARY};
    border: none;
    border-bottom: 1px solid {BORDER};
    padding: 4px 6px;
    font-weight: 600;
}}

QScrollArea {{
    border: none;
    background-color: transparent;
}}
QScrollBar:vertical {{
    background: {BG_ROOT};
    width: 10px;
    margin: 0;
}}
QScrollBar::handle:vertical {{
    background: #363a45;
    min-height: 24px;
    border-radius: 4px;
    margin: 2px;
}}
QScrollBar::handle:vertical:hover {{
    background: #4b5060;
}}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{
    height: 0px;
}}
QScrollBar:horizontal {{
    background: {BG_ROOT};
    height: 10px;
    margin: 0;
}}
QScrollBar::handle:horizontal {{
    background: #363a45;
    min-width: 24px;
    border-radius: 4px;
    margin: 2px;
}}
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {{
    width: 0px;
}}

QStatusBar {{
    background-color: {BG_ROOT};
    color: {TEXT_SECONDARY};
    border-top: 1px solid {BORDER};
    font-size: 8.5pt;
}}
QStatusBar::item {{
    border: none;
}}

QFrame[card="true"] {{
    background-color: {BG_ELEVATED};
    border: 1px solid {BORDER};
    border-radius: 6px;
}}
QFrame[hud="true"] {{
    background-color: rgba(24, 25, 29, 218);
    border: 1px solid rgba(72, 78, 92, 210);
    border-radius: 8px;
}}
QLabel[sectionTitle="true"] {{
    color: {TEXT_SECONDARY};
    font-size: 8pt;
    font-weight: 700;
    letter-spacing: 0.5px;
    text-transform: uppercase;
}}
QLabel[muted="true"] {{
    color: {TEXT_SECONDARY};
}}
QLabel[badge="world"] {{
    background-color: {BG_ELEVATED};
    color: {TEXT_PRIMARY};
    border: 1px solid {BORDER_LIGHT};
    border-radius: 4px;
    padding: 2px 9px;
    font-size: 8.5pt;
    font-weight: 600;
}}
"""


class ElidedLabel(QLabel):
    """Keep status text readable without imposing a width on its window."""

    def __init__(self, text: str = ""):
        super().__init__()
        self.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        self.setText(text)

    def setText(self, text: str) -> None:
        self._full_text = text
        self.setAccessibleName(text)
        self.setToolTip(text)
        self._fit_text()

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        self._fit_text()

    def _fit_text(self) -> None:
        available = max(0, self.contentsRect().width())
        super().setText(self.fontMetrics().elidedText(self._full_text, Qt.TextElideMode.ElideRight, available))


def apply_dark_theme(app: QApplication) -> None:
    """Apply the shared palette with each platform's UI font."""
    app.setStyle("Fusion")
    font = (QFontDatabase.systemFont(QFontDatabase.SystemFont.GeneralFont)
            if sys.platform == "darwin" else QFont("Segoe UI", 9))
    font.setHintingPreference(QFont.HintingPreference.PreferDefaultHinting)
    app.setFont(font)

    pal = QPalette()
    pal.setColor(QPalette.ColorRole.Window, QColor(BG_PANEL))
    pal.setColor(QPalette.ColorRole.WindowText, QColor(TEXT_PRIMARY))
    pal.setColor(QPalette.ColorRole.Base, QColor(BG_INPUT))
    pal.setColor(QPalette.ColorRole.AlternateBase, QColor(BG_ELEVATED))
    pal.setColor(QPalette.ColorRole.ToolTipBase, QColor(BG_ELEVATED))
    pal.setColor(QPalette.ColorRole.ToolTipText, QColor(TEXT_PRIMARY))
    pal.setColor(QPalette.ColorRole.Text, QColor(TEXT_PRIMARY))
    pal.setColor(QPalette.ColorRole.Button, QColor(BG_ELEVATED))
    pal.setColor(QPalette.ColorRole.ButtonText, QColor(TEXT_PRIMARY))
    pal.setColor(QPalette.ColorRole.BrightText, QColor("#ffffff"))
    pal.setColor(QPalette.ColorRole.Highlight, QColor(ACCENT_BLUE))
    pal.setColor(QPalette.ColorRole.HighlightedText, QColor("#ffffff"))
    pal.setColor(QPalette.ColorRole.PlaceholderText, QColor(TEXT_MUTED))
    app.setPalette(pal)
    app.setStyleSheet(QSS_STYLESHEET)


# ---------------------------------------------------------------------------
# R/G/B Axis-Badged SpinBox & Vec3 Editor
# ---------------------------------------------------------------------------
class AxisSpinBox(QWidget):
    """A compact colored axis badge (R/G/B) paired with a precision QDoubleSpinBox."""

    valueChanged = Signal(float)
    editingFinished = Signal()

    def __init__(
        self,
        axis_label: str,
        badge_color: str,
        decimals: int = 2,
        step: float = 0.25,
        minimum: float = -100000.0,
        maximum: float = 100000.0,
        parent: QWidget | None = None,
    ):
        super().__init__(parent)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        self.badge = QLabel(axis_label)
        self.badge.setFixedSize(16, 26)
        self.badge.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.badge.setStyleSheet(
            f"background-color: {badge_color}; color: #ffffff; font-weight: 700; "
            f"font-size: 8pt; border-top-left-radius: 4px; border-bottom-left-radius: 4px; "
            f"border: 1px solid {badge_color};"
        )

        self.spin = QDoubleSpinBox()
        self.spin.setDecimals(decimals)
        self.spin.setSingleStep(step)
        self.spin.setRange(minimum, maximum)
        self.spin.setMinimumWidth(0)
        self.spin.setFixedHeight(26)
        self.spin.setButtonSymbols(QDoubleSpinBox.ButtonSymbols.NoButtons)
        self.spin.setStyleSheet(
            f"QDoubleSpinBox {{ background-color: {BG_INPUT}; color: {TEXT_PRIMARY}; "
            f"border: 1px solid {BORDER}; "
            f"border-top-left-radius: 0px; border-bottom-left-radius: 0px; "
            f"border-top-right-radius: 4px; border-bottom-right-radius: 4px; "
            f"padding: 1px 4px; font-size: 8.5pt; }}"
            f"QDoubleSpinBox:focus {{ border-color: {ACCENT_BLUE}; }}"
            f"QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {{ subcontrol-origin: border; width: 0px; border: none; }}"
        )
        self.spin.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Fixed)
        self.setMinimumWidth(0)

        layout.addWidget(self.badge)
        layout.addWidget(self.spin, 1)

        self.spin.valueChanged.connect(self.valueChanged.emit)
        self.spin.editingFinished.connect(self.editingFinished.emit)

    def value(self) -> float:
        return float(self.spin.value())

    def setValue(self, val: float) -> None:
        self.spin.blockSignals(True)
        self.spin.setValue(float(val))
        self.spin.blockSignals(False)


class Vec3Editor(QWidget):
    """Horizontal row of three R/G/B AxisSpinBoxes (X/Y/Z or H/P/R)."""

    valuesChanged = Signal(float, float, float)
    editingFinished = Signal()

    def __init__(
        self,
        labels: tuple[str, str, str] = ("X", "Y", "Z"),
        decimals: int = 2,
        step: float = 0.25,
        minimum: float = -100000.0,
        maximum: float = 100000.0,
        parent: QWidget | None = None,
    ):
        super().__init__(parent)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(4)

        colors = (AXIS_X_COLOR, AXIS_Y_COLOR, AXIS_Z_COLOR)
        self.boxes: list[AxisSpinBox] = []
        for lbl, col in zip(labels, colors):
            box = AxisSpinBox(lbl, col, decimals=decimals, step=step, minimum=minimum, maximum=maximum)
            box.valueChanged.connect(self._on_any_changed)
            box.editingFinished.connect(self.editingFinished.emit)
            layout.addWidget(box, 1)
            self.boxes.append(box)

    def _on_any_changed(self, _val: float) -> None:
        self.valuesChanged.emit(self.boxes[0].value(), self.boxes[1].value(), self.boxes[2].value())

    def values(self) -> tuple[float, float, float]:
        return (self.boxes[0].value(), self.boxes[1].value(), self.boxes[2].value())

    def setValues(self, x: float, y: float, z: float) -> None:
        self.boxes[0].setValue(x)
        self.boxes[1].setValue(y)
        self.boxes[2].setValue(z)
