"""Unreal Engine 4/5 Style Play-In-Editor (PIE) Embedded Viewport for EOD World Editor.

Launches `eod.exe` directly into the compiled mod layer (`EOD_MOD_DIR`, `EOD_DIRECT_START`,
`EOD_SKIP_MOVIES=1`) and embeds its live Vulkan window directly inside the Qt 6 central
viewport widget on Windows via Win32 `SetParent` + borderless `WS_CHILD` + `MoveWindow`
synced on resize, with a top PIE control bar (`Stop PIE`, `Pop Out Window`, `Restart`).
"""

from __future__ import annotations

import ctypes
import os
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any

from PySide6.QtCore import QEvent, QObject, QRect, QSize, Qt, QTimer, Signal
from PySide6.QtGui import QKeyEvent
from PySide6.QtWidgets import (
    QApplication,
    QComboBox,
    QFrame,
    QHBoxLayout,
    QLabel,
    QLayout,
    QLineEdit,
    QPlainTextEdit,
    QPushButton,
    QSpinBox,
    QTextEdit,
    QVBoxLayout,
    QWidget,
)

from theme import ACCENT_GREEN, BG_ROOT, BORDER, TEXT_PRIMARY, TEXT_SECONDARY, svg_icon


class _PlaybackControls(QLayout):
    """Wrap the live controls to their natural size in a narrow viewport."""

    def __init__(self):
        super().__init__()
        self._items = []
        self.setContentsMargins(0, 0, 0, 0)
        self.setSpacing(6)

    def addItem(self, item):
        self._items.append(item)

    def count(self):
        return len(self._items)

    def itemAt(self, index):
        return self._items[index] if 0 <= index < self.count() else None

    def takeAt(self, index):
        return self._items.pop(index) if 0 <= index < self.count() else None

    def hasHeightForWidth(self):
        return True

    def minimumSize(self):
        size = QSize(0, 0)
        for item in self._items:
            if not item.isEmpty():
                size = size.expandedTo(item.minimumSize())
        return size

    def sizeHint(self):
        return self.minimumSize()

    def heightForWidth(self, width):
        return self._arrange(QRect(0, 0, max(1, width), 0), apply=False)

    def setGeometry(self, rect):
        super().setGeometry(rect)
        self._arrange(rect, apply=True)

    def _arrange(self, rect, *, apply):
        x = y = row_height = 0
        for item in self._items:
            if item.isEmpty():
                continue
            hint = item.sizeHint().expandedTo(item.minimumSize())
            width = min(hint.width(), rect.width())
            if x and x + width > rect.width():
                x = 0
                y += row_height + self.spacing()
                row_height = 0
            if apply:
                item.setGeometry(QRect(rect.x() + x, rect.y() + y, width, hint.height()))
            x += width + self.spacing()
            row_height = max(row_height, hint.height())
        return y + row_height


# Win32 window style & message constants
GWL_STYLE = -16
GWL_EXSTYLE = -20
WS_CHILD = 0x40000000
WS_VISIBLE = 0x10000000
WS_CLIPSIBLINGS = 0x04000000
WS_CLIPCHILDREN = 0x02000000
WS_POPUP = 0x80000000
WS_BORDER = 0x00800000
WS_DLGFRAME = 0x00400000
WS_CAPTION = 0x00C00000
WS_THICKFRAME = 0x00040000
WS_MINIMIZEBOX = 0x00020000
WS_MAXIMIZEBOX = 0x00010000
WS_SYSMENU = 0x00080000
WS_OVERLAPPEDWINDOW = 0x00CF0000
WS_EX_DLGMODALFRAME = 0x00000001
WS_EX_WINDOWEDGE = 0x00000100
WS_EX_CLIENTEDGE = 0x00000200
WS_EX_STATICEDGE = 0x00020000
WS_EX_APPWINDOW = 0x00040000
WM_SIZE = 0x0005
WM_ACTIVATE = 0x0006
WM_SETFOCUS = 0x0007
WM_CLOSE = 0x0010
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WA_ACTIVE = 1
SIZE_RESTORED = 0
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_NOZORDER = 0x0004
SWP_NOACTIVATE = 0x0010
SWP_FRAMECHANGED = 0x0020
SWP_SHOWWINDOW = 0x0040

_STRIP_STYLE_MASK = (
    WS_OVERLAPPEDWINDOW
    | WS_CAPTION
    | WS_BORDER
    | WS_DLGFRAME
    | WS_THICKFRAME
    | WS_MINIMIZEBOX
    | WS_MAXIMIZEBOX
    | WS_SYSMENU
    | WS_POPUP
)
_STRIP_EXSTYLE_MASK = (
    WS_EX_DLGMODALFRAME
    | WS_EX_WINDOWEDGE
    | WS_EX_CLIENTEDGE
    | WS_EX_STATICEDGE
    | WS_EX_APPWINDOW
)


def strip_embedded_window_styles(style: int, exstyle: int) -> tuple[int, int]:
    """Strip all top-level window frame/border/ex-edge chrome for borderless WS_CHILD embedding."""
    new_style = (int(style) & ~_STRIP_STYLE_MASK) | WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS
    new_exstyle = int(exstyle) & ~_STRIP_EXSTYLE_MASK
    return new_style, new_exstyle


def compute_embedded_client_rect(
    logical_w: int,
    logical_h: int,
    dpr: float = 1.0,
    *,
    win32_client_size: tuple[int, int] | None = None,
    nc_insets: tuple[int, int, int, int] = (0, 0, 0, 0),
    parent_dpi: int = 0,
    child_dpi: int = 0,
) -> dict[str, int]:
    """Compute exact Win32 physical rect for the embedded child HWND at any DPI scale.

    Returns dict with:
      - `x`, `y`, `w`, `h`: exact outer rect passed to `SetWindowPos` / `MoveWindow` so the
        child's *client* area fills `(0, 0, client_w, client_h)` with zero left/right clipping.
      - `client_w`, `client_h`: exact physical client pixels of the embed container.
      - `wm_size_w`, `wm_size_h`: DPI-consistent client dimensions for `WM_SIZE` `lparam`.
    """
    scale = float(dpr) if dpr and dpr > 0.0 else 1.0
    qt_w = max(1, int(round(max(1, int(logical_w)) * scale)))
    qt_h = max(1, int(round(max(1, int(logical_h)) * scale)))

    if win32_client_size is not None and win32_client_size[0] > 0 and win32_client_size[1] > 0:
        rw, rh = int(win32_client_size[0]), int(win32_client_size[1])
        # Never exceed the smaller of Win32 GetClientRect and Qt's DPI-scaled size during layout
        # transitions so left/right HUD edges are never pushed outside the visible container.
        client_w = min(rw, qt_w) if abs(rw - qt_w) > 2 else rw
        client_h = min(rh, qt_h) if abs(rh - qt_h) > 2 else rh
    else:
        client_w, client_h = qt_w, qt_h

    nc_l, nc_t, nc_r, nc_b = (max(0, int(v)) for v in nc_insets)
    win_x = -nc_l
    win_y = -nc_t
    win_w = max(1, client_w + nc_l + nc_r)
    win_h = max(1, client_h + nc_t + nc_b)

    if parent_dpi > 0 and child_dpi > 0 and child_dpi != parent_dpi:
        ratio = float(child_dpi) / float(parent_dpi)
        wm_w = max(1, int(round(client_w * ratio)))
        wm_h = max(1, int(round(client_h * ratio)))
    else:
        wm_w, wm_h = client_w, client_h

    return {
        "x": win_x,
        "y": win_y,
        "w": win_w,
        "h": win_h,
        "client_w": client_w,
        "client_h": client_h,
        "wm_size_w": wm_w,
        "wm_size_h": wm_h,
    }

# Canonical event display labels aligned with src/session_start.cpp canonical_contest_alias()
CANONICAL_EVENT_MAP: dict[str, str] = {
    "normal": "No-Rules Race",
    "normal_race": "No-Rules Race",
    "c_normal_race": "No-Rules Race",
    "no rules race": "No-Rules Race",
    "no-rules race": "No-Rules Race",
    "fig8jump": "Figure-8 Jump Race",
    "figure8_jump_race": "Figure-8 Jump Race",
    "c_figure8_jump_race": "Figure-8 Jump Race",
    "figure 8 jump": "Figure-8 Jump Race",
    "figure-8 jump": "Figure-8 Jump Race",
    "figure 8 jump race": "Figure-8 Jump Race",
    "figure-8 jump race": "Figure-8 Jump Race",
    "fig8": "Figure-8",
    "figure8_race": "Figure-8",
    "c_figure8_race": "Figure-8",
    "figure 8": "Figure-8",
    "figure-8": "Figure-8",
    "road_race": "Jump Race",
    "jump_race": "Jump Race",
    "c_road_race": "Jump Race",
    "c_jump_race": "Jump Race",
    "jump race": "Jump Race",
    "demo_derby_contest": "Demolition Derby",
    "demo_derby": "Demolition Derby",
    "kill_derby": "Demolition Derby",
    "c_kill_derby": "Demolition Derby",
    "demolition derby": "Demolition Derby",
    "knockout_contest": "Knockout",
    "kill_knock": "Knockout",
    "c_kill_knock": "Knockout",
    "knockout": "Knockout",
    "pushoff_contest": "Push-Off",
    "kill_push": "Push-Off",
    "pushoff": "Push-Off",
    "c_pushoff": "Push-Off",
    "push-off": "Push-Off",
    "save_ring": "Suicide Race",
    "c_save_ring": "Suicide Race",
    "suicide": "Suicide Race",
    "suicide_race": "Suicide Race",
    "c_suicide_race": "Suicide Race",
    "suicide race": "Suicide Race",
    "stop_and_go": "Stop-and-Go",
    "stop_and_go_race": "Stop-and-Go",
    "c_stop_and_go_race": "Stop-and-Go",
    "stop-and-go": "Stop-and-Go",
    "shortcut": "Shortcut Race",
    "shortcut_race": "Shortcut Race",
    "c_shortcut_race": "Shortcut Race",
    "shortcut race": "Shortcut Race",
    "flag_pole": "Flagpole Race",
    "flagpole_race": "Flagpole Race",
    "c_flagpole_race": "Flagpole Race",
    "flagpole race": "Flagpole Race",
    "whip": "Whip-Around",
    "whip_around_race": "Whip-Around",
    "c_whip_around_race": "Whip-Around",
    "whip-around": "Whip-Around",
    "whiparound": "Whip-Around",
    "chain_race": "Chain Race",
    "chain race": "Chain Race",
    "fwd_bwd_race": "Forward-Backward",
    "c_fwd_bwd_race": "Forward-Backward",
    "forward-backward": "Forward-Backward",
    "last_man_race": "Last Man",
    "c_last_man_race": "Last Man",
    "last man": "Last Man",
    "school_bus": "School Bus",
    "c_school_bus": "School Bus",
    "school bus": "School Bus",
    "trailer_race": "Trailer Race",
    "c_trailer_race": "Trailer Race",
    "trailer race": "Trailer Race",
    "pointtopoint": "Point-to-Point",
    "c_pointtopoint": "Point-to-Point",
    "point-to-point": "Point-to-Point",
    "detention": "Detention",
    "c_detention": "Detention",
    "gauntlet": "Gauntlet",
    "c_gauntlet": "Gauntlet",
    "red_rover": "Red Rover",
    "c_red_rover": "Red Rover",
    "red rover": "Red Rover",
    "wrangle": "Wrangler",
    "c_wrangle": "Wrangler",
    "wrangler": "Wrangler",
    "capture": "Downtown",
    "c_capture": "Downtown",
    "downtown": "Downtown",
    "stuntman": "Daredevil",
    "c_stuntman": "Daredevil",
    "daredevil": "Daredevil",
    "battle": "Battle",
    "c_battle": "Battle",
    "battle (free roam)": "Battle",
    "free roam (battle solo)": "Battle",
    "free roam": "Battle",
}

ARCADE_MODES: list[tuple[str, str]] = [
    ("free_roam", "Standard / Free Roam"),
    ("tag", "Tag / Infected"),
    ("cat_mouse", "Cat & Mouse"),
    ("juggernaut", "Fat Kid / Juggernaut"),
]


def canonicalize_event_name(event_name: str) -> str:
    """Map raw VALT/va0/UI event labels to exact SLUS_209.10 contest names."""
    raw = (event_name or "").strip()
    if not raw or raw == "#0":
        return "Battle"
    low = raw.lower()
    if low.startswith("all templates") or low.startswith("all ramps"):
        return "Battle"
    if low.startswith("battle"):
        return "Battle"
    return CANONICAL_EVENT_MAP.get(low, raw)


def canonicalize_car_name(car_name: str) -> str:
    """Clean UI display car names into exact DATA/CAR / vehicle-table stems."""
    clean = (car_name or "Algar").split("(")[0].strip()
    if clean.lower().endswith(".car"):
        clean = clean[:-4]
    aliases = {
        "ambula": "Ambulance",
        "ambula_1": "Ambulance",
        "bulldo": "Bulldog",
        "bulldo_1": "Bulldog",
        "carrer": "Carrere",
        "carrer_1": "Carrere",
        "fillmo": "Fillmore",
        "fillmo_1": "Fillmore",
        "gladia": "Gladiator",
        "gladia_1": "Gladiator",
        "hurric": "Hurricane",
        "hurric_1": "Hurricane",
        "imperi": "Imperial",
        "imperi_1": "Imperial",
        "reaper": "Reaper",
        "detona": "Detonator",
        "detona_1": "Detonator",
    }
    return aliases.get(clean.lower(), clean or "Algar")


def resolve_eod_exe(game_dir: Path | str, *, prefer_plume: bool = False) -> Path:
    """Locate the PIE executable for the current platform and renderer."""
    override = os.environ.get("EOD_PIE_EXE")
    if override:
        return Path(override).expanduser().resolve()
    gdir = Path(game_dir).resolve()
    repository = Path(__file__).resolve().parents[2]
    if sys.platform == "darwin":
        bundle_executable = Path("Eve of Destruction.app/Contents/MacOS/eod")
        builds = ["build-mac-plume", "build-mac"] if prefer_plume else ["build-mac", "build-mac-plume"]
        candidates = [
            gdir / "eod",
            gdir / bundle_executable,
            *(repository / build / "eod" for build in builds),
            Path.home() / "Applications" / bundle_executable,
            Path("/Applications") / bundle_executable,
            gdir / "eod.exe",
        ]
    else:
        candidates = [gdir / "eod.exe", repository / "build" / "eod.exe"]
        local_app = os.environ.get("LOCALAPPDATA")
        if local_app:
            candidates.append(Path(local_app) / "TDEODRecomp" / "eod.exe")
    for c in candidates:
        if c.is_file():
            return c
    return candidates[0]


def build_pie_launch_spec(
    game_dir: Path | str,
    mod_dir: Path | str,
    track_display_name: str,
    mode: str = "free_roam",
    event_name: str = "#0",
    car_name: str = "Algar",
    ai_count: int = 8,
    free_roam_place: str = "home",
    renderer_mode: str = "ps2",
    sync_all_events: bool = True,
    arcade_mode: str = "free_roam",
    ai_autodrive: bool = False,
    free_cam: bool = False,
    laps: int | None = None,
) -> dict[str, Any]:
    """Build exact process args and environment variables for `eod.exe` PIE launch.

    Matches `src/session_start.h`, `src/battle_bots.cpp`, `src/battle_driver.cpp`, and
    `runtime/render/native_renderer.cpp`:
      - Career Free Roam (HomeBase): `EOD_DIRECT_START="mode=career;place=<place>"`
      - Track Free Roam:
          * When a specific non-Battle template is chosen (e.g. `Figure-8 Jump Race`,
            `Jump Race`, `Gauntlet`), launches THAT exact template with `ai=0` (Forced
            Free Roam solo on the selected map variation!).
          * When default/`Battle (Free Roam)`/`All Templates` is chosen, launches
            `event=Battle` with `ai=<ai_count>` (0 = solo).
      - Event / Race: launches the canonicalized contest name with `ai=<ai_count>`
        and the optional `laps=<count>` for lap races.
      - Launches with `--windowed` so `eod.ini` `Fullscreen=1` never clips the embedded HUD.
      - Configures `EOD_PIE_CONTROL_FILE`, `EOD_OPT_TERRAIN_MASK`, `EOD_ARCADE_MODE`,
        `EOD_AI_AUTODRIVE`, `EOD_FREE_CAM`, and `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1`.
    """
    gdir = Path(game_dir).resolve()
    mdir = Path(mod_dir).resolve()
    clean_car = canonicalize_car_name(car_name)
    trk_up = track_display_name.strip()
    rmode = "plume" if renderer_mode == "plume" else "ps2"
    exe = resolve_eod_exe(gdir, prefer_plume=rmode == "plume")
    rtag = "Plume HQ" if rmode == "plume" else "PS2 Fast"

    raw_ev = (event_name or "").strip()
    canon_ev = canonicalize_event_name(raw_ev)
    is_specific_template = bool(
        raw_ev
        and raw_ev != "#0"
        and not raw_ev.lower().startswith("all templates")
        and not raw_ev.lower().startswith("all ramps")
        and not raw_ev.lower().startswith("battle")
        and not raw_ev.lower().startswith("free roam")
        and raw_ev.lower() != "career overworld (free roam)"
    )

    if mode == "career_roam" or (mode == "free_roam" and trk_up.lower() in ("homebase", "home base", "career")):
        direct = f"mode=career;place={free_roam_place or 'home'}"
        label = f"Career Free Roam ({free_roam_place or 'home'}) [{rtag}]"
    elif mode == "free_roam":
        if is_specific_template:
            # Forced Free Roam on the selected map variation (e.g. Figure-8 Jump Race, ai=0!)
            roam_ai = 0
            direct = f"mode=action;track={trk_up};event={canon_ev};car={clean_car};paint=0;humans=1;ai={roam_ai}"
            label = f"Free Roam ({canon_ev}, Solo) — {trk_up} ({clean_car}) [{rtag}]"
        else:
            roam_ai = max(0, min(12, int(ai_count))) if ai_count is not None else 0
            direct = f"mode=action;track={trk_up};event=Battle;car={clean_car};paint=0;humans=1;ai={roam_ai}"
            label = f"Free Roam (Battle, {roam_ai} AI) — {trk_up} ({clean_car}) [{rtag}]"
    else:
        if raw_ev.lower() == "career overworld (free roam)":
            direct = f"mode=career;place={free_roam_place or 'home'}"
            label = f"Career Free Roam ({free_roam_place or 'home'}) [{rtag}]"
        else:
            ev_ai = max(0, min(12, int(ai_count))) if ai_count is not None else 8
            parts = [
                "mode=action",
                f"track={trk_up}",
                f"event={canon_ev}",
                f"car={clean_car}",
                "paint=0",
                "humans=1",
                f"ai={ev_ai}",
            ]
            if laps is not None:
                parts.append(f"laps={max(1, min(50, int(laps)))}")
            direct = ";".join(parts)
            label = f"Event — {trk_up} · {canon_ev} ({ev_ai} AI · {clean_car}) [{rtag}]"

    control_file = mdir / "pie_live_control.ini"
    env = dict(os.environ)
    env["EOD_MOD_DIR"] = str(mdir)
    env["EOD_SKIP_MOVIES"] = "1"
    env["EOD_DIRECT_START"] = direct
    env["EOD_SESSION"] = direct  # alias for diagnostics/scripts
    env["SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS"] = "1"
    env["EOD_PIE_CONTROL_FILE"] = str(control_file)
    env["EOD_ARCADE_MODE"] = (arcade_mode or "free_roam").strip().lower()
    env["EOD_AI_AUTODRIVE"] = "1" if ai_autodrive else "0"
    env["EOD_FREE_CAM"] = "1" if free_cam else "0"
    env.pop("EOD_NATIVE_ONLY", None)
    env["EOD_MODERN"] = "0"
    args = [str(exe), "--windowed", "--data", str(gdir)]
    if sync_all_events:
        env["EOD_OPT_TERRAIN_MASK"] = "0xFFFF"
    if rmode == "plume":
        env["EOD_PLUME"] = "1"
        env["EOD_NATIVE"] = "1"
        env["EOD_LOOK"] = "modern"
        args.extend(["--renderer", "vulkan", "--require-plume"])
    else:
        env["EOD_NATIVE"] = "0"
        env.pop("EOD_LOOK", None)

    return {
        "exe": str(exe),
        "args": args,
        "cwd": str(gdir),
        "mod_dir": str(mdir),
        "control_file": str(control_file),
        "direct_start": direct,
        "renderer_mode": rmode,
        "arcade_mode": env["EOD_ARCADE_MODE"],
        "ai_autodrive": bool(ai_autodrive),
        "free_cam": bool(free_cam),
        "label": label,
        "env": env,
    }


_USER32_TYPED = None
_KERNEL32_TYPED = None


def _win32_user32():
    """Return ctypes.windll.user32 with explicit 64-bit Win32 argtypes/restype."""
    global _USER32_TYPED
    if _USER32_TYPED is not None:
        return _USER32_TYPED
    import ctypes.wintypes as wt

    u32 = ctypes.windll.user32
    LONG_PTR = ctypes.c_ssize_t
    u32.IsWindow.argtypes = [wt.HWND]
    u32.IsWindow.restype = wt.BOOL
    u32.IsWindowVisible.argtypes = [wt.HWND]
    u32.IsWindowVisible.restype = wt.BOOL
    u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
    u32.GetWindowThreadProcessId.restype = wt.DWORD
    u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
    u32.GetWindowRect.restype = wt.BOOL
    u32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
    u32.GetClientRect.restype = wt.BOOL
    u32.GetWindowLongPtrW.argtypes = [wt.HWND, ctypes.c_int]
    u32.GetWindowLongPtrW.restype = LONG_PTR
    u32.SetWindowLongPtrW.argtypes = [wt.HWND, ctypes.c_int, LONG_PTR]
    u32.SetWindowLongPtrW.restype = LONG_PTR
    u32.SetParent.argtypes = [wt.HWND, wt.HWND]
    u32.SetParent.restype = wt.HWND
    u32.MoveWindow.argtypes = [wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.BOOL]
    u32.MoveWindow.restype = wt.BOOL
    u32.SetWindowPos.argtypes = [
        wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.UINT
    ]
    u32.SetWindowPos.restype = wt.BOOL
    u32.PostMessageW.argtypes = [wt.HWND, wt.UINT, ctypes.c_size_t, LONG_PTR]
    u32.PostMessageW.restype = wt.BOOL
    u32.SendMessageW.argtypes = [wt.HWND, wt.UINT, ctypes.c_size_t, LONG_PTR]
    u32.SendMessageW.restype = LONG_PTR
    u32.AttachThreadInput.argtypes = [wt.DWORD, wt.DWORD, wt.BOOL]
    u32.AttachThreadInput.restype = wt.BOOL
    u32.SetFocus.argtypes = [wt.HWND]
    u32.SetFocus.restype = wt.HWND
    if hasattr(u32, "GetDpiForWindow"):
        u32.GetDpiForWindow.argtypes = [wt.HWND]
        u32.GetDpiForWindow.restype = wt.UINT
    if hasattr(u32, "ClientToScreen"):
        u32.ClientToScreen.argtypes = [wt.HWND, ctypes.POINTER(wt.POINT)]
        u32.ClientToScreen.restype = wt.BOOL
    _USER32_TYPED = u32
    return u32


def _win32_kernel32():
    """Return ctypes.windll.kernel32 with explicit 64-bit Win32 argtypes/restype."""
    global _KERNEL32_TYPED
    if _KERNEL32_TYPED is not None:
        return _KERNEL32_TYPED
    import ctypes.wintypes as wt

    k32 = ctypes.windll.kernel32
    k32.GetCurrentThreadId.argtypes = []
    k32.GetCurrentThreadId.restype = wt.DWORD
    _KERNEL32_TYPED = k32
    return k32


def _qt_key_to_vk(event: QKeyEvent) -> int:
    """Resolve Win32 virtual-key code from a Qt key event (with fallback map for synthetic events)."""
    vk = int(event.nativeVirtualKey())
    if vk:
        return vk
    k = event.key()
    if Qt.Key.Key_A <= k <= Qt.Key.Key_Z:
        return int(k)
    if Qt.Key.Key_0 <= k <= Qt.Key.Key_9:
        return int(k)
    fallback = {
        Qt.Key.Key_Space: 0x20,
        Qt.Key.Key_Return: 0x0D,
        Qt.Key.Key_Enter: 0x0D,
        Qt.Key.Key_Escape: 0x1B,
        Qt.Key.Key_Tab: 0x09,
        Qt.Key.Key_Backspace: 0x08,
        Qt.Key.Key_Left: 0x25,
        Qt.Key.Key_Up: 0x26,
        Qt.Key.Key_Right: 0x27,
        Qt.Key.Key_Down: 0x28,
        Qt.Key.Key_Shift: 0x10,
        Qt.Key.Key_Control: 0x11,
        Qt.Key.Key_Alt: 0x12,
    }
    return fallback.get(k, 0)


def _find_process_hwnd_win32(pid: int) -> int:
    """Locate the primary visible top-level Win32 HWND belonging to `pid`."""
    if sys.platform != "win32":
        return 0
    import ctypes.wintypes as wt

    user32 = _win32_user32()
    found: list[int] = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, ctypes.c_ssize_t)
    def enum_cb(hwnd, _lparam):
        if not user32.IsWindowVisible(hwnd):
            return True
        proc_id = wt.DWORD(0)
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(proc_id))
        if proc_id.value == pid:
            rect = wt.RECT()
            if user32.GetWindowRect(hwnd, ctypes.byref(rect)):
                if (rect.right - rect.left) >= 160 and (rect.bottom - rect.top) >= 120:
                    found.append(int(hwnd))
                    return False
        return True

    user32.EnumWindows(enum_cb, 0)
    return found[0] if found else 0


class PieHostWidget(QWidget):
    """Central Qt 6 viewport container that embeds a live `eod.exe` Vulkan HWND."""

    pieStopped = Signal()
    pieStatusChanged = Signal(str)
    pieRestartRequested = Signal()
    pieLogLine = Signal(str)

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self._proc: subprocess.Popen | None = None
        self._child_hwnd: int = 0
        self._attached_tid: int = 0
        self._orig_style: int = 0
        self._orig_exstyle: int = 0
        self._last_embedded_size: tuple[int, int] = (0, 0)
        self._popped_out: bool = False
        self._last_spec: dict[str, Any] | None = None
        self._plume_ready: threading.Event | None = None
        self._plume_deadline = 0.0
        self._poll_attempts: int = 0
        self._control_file: Path | None = None
        self._live_state: dict[str, Any] = {
            "ai_autodrive": 0,
            "free_cam": 0,
            "camera_mode": -1,
            "speed_boost": 1.0,
            "arcade_mode": "free_roam",
        }
        self._app_filter_installed = False

        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        # Top Unreal-style PIE control bar
        self.bar = QFrame()
        self.bar.setStyleSheet(
            f"QFrame {{ background: #142219; border-bottom: 1px solid #2b6e44; }}"
        )
        bar_lay = QVBoxLayout(self.bar)
        bar_lay.setContentsMargins(10, 5, 10, 5)
        bar_lay.setSpacing(6)
        state_row = QHBoxLayout()
        bar_lay.addLayout(state_row)
        controls = _PlaybackControls()
        bar_lay.addLayout(controls)

        self.lbl_state = QLabel("● PIE READY")
        self.lbl_state.setStyleSheet(
            f"color: {ACCENT_GREEN}; font-weight: 700; font-size: 9pt; background: transparent;"
        )
        self.lbl_detail = QLabel("")
        self.lbl_detail.setWordWrap(True)
        self.lbl_detail.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 8.2pt; background: transparent;")

        self.btn_free_cam = QPushButton(" Free Cam")
        self.btn_free_cam.setCheckable(True)
        self.btn_free_cam.setToolTip("Toggle Free / Elevated Camera View inside PIE live (hotkey C also cycles camera)")
        self.btn_free_cam.toggled.connect(self._on_free_cam_toggled)

        self.btn_ai_drive = QPushButton(" AI Auto-Drive")
        self.btn_ai_drive.setCheckable(True)
        self.btn_ai_drive.setToolTip("Let AI drive your player car along the edited ILIN racing line live!")
        self.btn_ai_drive.toggled.connect(self._on_ai_drive_toggled)

        self.cmb_arcade = QComboBox()
        self.cmb_arcade.setMinimumWidth(128)
        self.cmb_arcade.setToolTip("Live Arcade / Custom Game Mode overlay")
        for key, title in ARCADE_MODES:
            self.cmb_arcade.addItem(title, key)
        self.cmb_arcade.currentIndexChanged.connect(self._on_arcade_changed)

        self.btn_focus_game = QPushButton(" Focus Controls")
        self.btn_focus_game.setToolTip("Give Win32 keyboard & controller focus to embedded eod.exe")
        self.btn_focus_game.clicked.connect(self.focus_embedded_game)

        self.btn_restart = QPushButton(" Restart")
        self.btn_restart.setIcon(svg_icon("rotate"))
        self.btn_restart.clicked.connect(self.pieRestartRequested.emit)

        self.btn_popout = QPushButton(" Pop Out")
        self.btn_popout.setIcon(svg_icon("popout"))
        self.btn_popout.clicked.connect(self.pop_out_window)

        self.btn_stop = QPushButton("Stop PIE")
        self.btn_stop.setToolTip("Stop Play In Editor (Shift+Esc)")
        self.btn_stop.setIcon(svg_icon("stop", "#ff99a2"))
        self.btn_stop.setProperty("danger", True)
        self.btn_stop.clicked.connect(self.stop_pie)

        state_row.addWidget(self.lbl_state)
        state_row.addWidget(self.lbl_detail, 1)
        for button in (self.btn_stop, self.btn_restart, self.btn_free_cam, self.btn_ai_drive, self.cmb_arcade,
                       self.btn_focus_game, self.btn_popout):
            controls.addWidget(button)
        if sys.platform != "win32":
            self.btn_focus_game.setEnabled(False)
            self.btn_popout.setEnabled(False)
            self.btn_focus_game.setToolTip("Switch to the game window to give it keyboard focus.")
            self.btn_popout.setToolTip("On this platform the game opens in a separate window.")
        root.addWidget(self.bar)

        # Native Win32 embed surface
        self.embed_container = QWidget()
        self.embed_container.setAttribute(Qt.WidgetAttribute.WA_NativeWindow, True)
        self.embed_container.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.embed_container.setContentsMargins(0, 0, 0, 0)
        self.embed_container.setStyleSheet(f"background-color: {BG_ROOT}; border: none;")
        self.embed_container.installEventFilter(self)
        c_lay = QVBoxLayout(self.embed_container)
        c_lay.setContentsMargins(0, 0, 0, 0)
        c_lay.setSpacing(0)
        self.lbl_placeholder = QLabel("Launching eod.exe Vulkan viewport...")
        self.lbl_placeholder.setWordWrap(True)
        self.lbl_placeholder.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.lbl_placeholder.setStyleSheet(f"color: {TEXT_PRIMARY}; font-size: 11pt; background: transparent;")
        c_lay.addWidget(self.lbl_placeholder)
        root.addWidget(self.embed_container, 1)

        self._embed_timer = QTimer(self)
        self._embed_timer.setInterval(60)
        self._embed_timer.timeout.connect(self._poll_embed_and_alive)

    @property
    def is_running(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    def write_live_control(self, **updates: Any) -> None:
        """Atomically persist live PIE toggle / car-tuner settings to `EOD_PIE_CONTROL_FILE`."""
        self._live_state.update(updates)
        if self._control_file is None:
            return
        try:
            self._control_file.parent.mkdir(parents=True, exist_ok=True)
            lines = [f"{k}={v}" for k, v in self._live_state.items()]
            tmp = self._control_file.with_suffix(".ini.tmp")
            tmp.write_text("\n".join(lines) + "\n", encoding="utf-8")
            os.replace(tmp, self._control_file)
        except Exception:
            pass

    def _on_free_cam_toggled(self, checked: bool) -> None:
        self.write_live_control(free_cam=1 if checked else 0)
        self.pieStatusChanged.emit(f"PIE Free Cam: {'ON' if checked else 'OFF'}")
        self.focus_embedded_game()

    def _on_ai_drive_toggled(self, checked: bool) -> None:
        self.write_live_control(ai_autodrive=1 if checked else 0)
        self.pieStatusChanged.emit(f"PIE AI Auto-Drive (ILIN): {'ON' if checked else 'OFF'}")
        self.focus_embedded_game()

    def _on_arcade_changed(self, _index: int) -> None:
        mode = str(self.cmb_arcade.currentData() or "free_roam")
        self.write_live_control(arcade_mode=mode)
        self.pieStatusChanged.emit(f"PIE Arcade Mode: {self.cmb_arcade.currentText()}")
        self.focus_embedded_game()

    def _install_app_filter(self) -> None:
        app = QApplication.instance()
        if app is not None and not self._app_filter_installed:
            app.installEventFilter(self)
            self._app_filter_installed = True

    def _remove_app_filter(self) -> None:
        app = QApplication.instance()
        if app is not None and self._app_filter_installed:
            app.removeEventFilter(self)
            self._app_filter_installed = False

    def start_pie(self, spec: dict[str, Any]) -> bool:
        """Start `eod.exe` using `spec` from `build_pie_launch_spec` and begin Win32 embedding."""
        self.stop_pie(emit_signal=False)
        exe = Path(spec["exe"])
        if not exe.is_file():
            self.lbl_placeholder.setText(f"eod.exe not found at:\n{exe}")
            self.pieStatusChanged.emit(f"PIE failed: {exe} not found")
            return False

        self._last_spec = spec
        plume_ready = threading.Event() if spec.get("renderer_mode") == "plume" else None
        self._plume_ready = plume_ready
        self._plume_deadline = time.monotonic() + 30.0 if plume_ready is not None else 0.0
        self._popped_out = False
        self._child_hwnd = 0
        self._attached_tid = 0
        self._poll_attempts = 0
        self._control_file = Path(spec["control_file"]) if spec.get("control_file") else (Path(spec["mod_dir"]) / "pie_live_control.ini")

        # Sync bar widgets with initial spec state without spamming signals
        self.btn_free_cam.blockSignals(True)
        self.btn_free_cam.setChecked(bool(spec.get("free_cam", False)))
        self.btn_free_cam.blockSignals(False)
        self.btn_ai_drive.blockSignals(True)
        self.btn_ai_drive.setChecked(bool(spec.get("ai_autodrive", False)))
        self.btn_ai_drive.blockSignals(False)
        amode = str(spec.get("arcade_mode", "free_roam"))
        aidx = self.cmb_arcade.findData(amode)
        if aidx >= 0:
            self.cmb_arcade.blockSignals(True)
            self.cmb_arcade.setCurrentIndex(aidx)
            self.cmb_arcade.blockSignals(False)
        self.write_live_control(
            free_cam=1 if self.btn_free_cam.isChecked() else 0,
            ai_autodrive=1 if self.btn_ai_drive.isChecked() else 0,
            arcade_mode=amode,
        )

        self.btn_popout.setEnabled(sys.platform == "win32")
        self.lbl_state.setText("● PIE STARTING")
        self.lbl_detail.setText(f"{spec['label']}  |  EOD_MOD_DIR={Path(spec['mod_dir']).name}")
        self.lbl_placeholder.setText(
            f"Starting eod.exe in viewport...\n\n{spec['label']}\nEOD_DIRECT_START = {spec['direct_start']}"
        )
        self.lbl_placeholder.show()

        launch_args = list(spec.get("args") or [str(exe), "--windowed"])
        self.pieLogLine.emit(
            f"[PIE] Launching {' '.join(launch_args)} | cwd={spec['cwd']} | renderer={spec.get('renderer_mode')} "
            f"| EOD_DIRECT_START={spec['direct_start']} | EOD_MOD_DIR={spec['mod_dir']}"
        )
        try:
            self._proc = subprocess.Popen(
                launch_args,
                cwd=spec["cwd"],
                env=spec["env"],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
            )
        except Exception as exc:
            self.lbl_placeholder.setText(f"Failed to launch eod.exe:\n{exc}")
            self.pieLogLine.emit(f"[PIE][ERROR] Failed to launch eod.exe: {exc}")
            self.pieStatusChanged.emit(f"PIE launch error: {exc}")
            self._proc = None
            return False

        # Stream eod.exe stdout/stderr on a lightweight daemon thread into Output Log
        proc_ref = self._proc

        def _pump_stdout() -> None:
            try:
                if proc_ref.stdout is not None:
                    for raw_line in proc_ref.stdout:
                        line = raw_line.rstrip("\r\n")
                        if line:
                            if plume_ready is not None and line.startswith("[native] plume renderer ready ("):
                                plume_ready.set()
                            self.pieLogLine.emit(f"[eod.exe] {line}")
            except Exception:
                pass

        threading.Thread(target=_pump_stdout, daemon=True, name="eod-pie-stdout").start()

        self._install_app_filter()
        self._embed_timer.start()
        self.pieLogLine.emit(f"[PIE] Started PID {self._proc.pid}; polling for game window...")
        self.pieStatusChanged.emit(f"PIE launched PID {self._proc.pid}: {spec['label']}")
        return True

    def _poll_embed_and_alive(self) -> None:
        if self._proc is None:
            self._embed_timer.stop()
            return
        if self._proc.poll() is not None:
            self._embed_timer.stop()
            self._detach_thread_input()
            self._remove_app_filter()
            code = self._proc.returncode
            self._proc = None
            self._child_hwnd = 0
            if self._plume_ready is not None and not self._plume_ready.is_set():
                self.pieStatusChanged.emit(f"Plume HQ failed to initialize (exit {code}). See the Output Log.")
            else:
                self.pieStatusChanged.emit(f"PIE exited (code {code})")
            self.pieStopped.emit()
            return

        if self._plume_ready is not None and not self._plume_ready.is_set():
            if time.monotonic() >= self._plume_deadline:
                self.stop_pie(emit_signal=False)
                message = "Plume HQ did not initialize. Use a Plume-enabled game build with Vulkan."
                self.pieLogLine.emit(f"[PIE][ERROR] {message}")
                self.pieStatusChanged.emit(message)
                self.pieStopped.emit()
            return

        if sys.platform != "win32":
            if not self._popped_out:
                self._popped_out = True
                self.lbl_state.setText("● PIE RUNNING")
                self.lbl_placeholder.setText("Play In Editor is running in the game window.\nClick Stop PIE to return.")
        elif not self._popped_out:
            if not self._child_hwnd:
                self._poll_attempts += 1
                hwnd = _find_process_hwnd_win32(self._proc.pid)
                if hwnd:
                    self._attach_hwnd(hwnd)
                elif self._poll_attempts > 250:
                    # 15 seconds fallback: keep monitoring process without blocking
                    self.lbl_placeholder.setText(
                        "eod.exe is running in an external window.\nClick Stop PIE (Shift+Esc) to return."
                    )
            else:
                # Re-sync automatically if Qt layout/splitter/DPI changed the embed container size
                cur_sz = self._physical_client_size()
                if cur_sz != self._last_embedded_size:
                    self._sync_embedded_geometry()

    def _detach_thread_input(self) -> None:
        if sys.platform != "win32" or not self._attached_tid:
            return
        try:
            user32 = _win32_user32()
            kernel32 = _win32_kernel32()
            cur_tid = int(kernel32.GetCurrentThreadId())
            if cur_tid and self._attached_tid and cur_tid != self._attached_tid:
                user32.AttachThreadInput(cur_tid, self._attached_tid, False)
        except Exception:
            pass
        self._attached_tid = 0

    def focus_embedded_game(self) -> bool:
        """Attach Win32 thread input and transfer keyboard focus + active state to embedded eod.exe."""
        if sys.platform != "win32" or not self._child_hwnd or self._popped_out:
            return False
        try:
            import ctypes.wintypes as wt

            user32 = _win32_user32()
            kernel32 = _win32_kernel32()
            if not user32.IsWindow(self._child_hwnd):
                return False
            cur_tid = int(kernel32.GetCurrentThreadId())
            pid = wt.DWORD(0)
            eod_tid = int(user32.GetWindowThreadProcessId(self._child_hwnd, ctypes.byref(pid)))
            if cur_tid and eod_tid and cur_tid != eod_tid and self._attached_tid != eod_tid:
                self._detach_thread_input()
                if user32.AttachThreadInput(cur_tid, eod_tid, True):
                    self._attached_tid = eod_tid
            user32.SetFocus(self._child_hwnd)
            user32.SendMessageW(self._child_hwnd, WM_ACTIVATE, WA_ACTIVE, 0)
            user32.SendMessageW(self._child_hwnd, WM_SETFOCUS, 0, 0)
            return True
        except Exception:
            return False

    def _attach_hwnd(self, hwnd: int) -> None:
        if sys.platform != "win32":
            return
        try:
            user32 = _win32_user32()
            parent_hwnd = int(self.embed_container.winId())
            if not parent_hwnd:
                return
            self._orig_style = int(user32.GetWindowLongPtrW(hwnd, GWL_STYLE))
            self._orig_exstyle = int(user32.GetWindowLongPtrW(hwnd, GWL_EXSTYLE))

            # Strip ALL non-client frame/border/ex-edge chrome before SetParent so Win32 drops
            # DWM/WS_THICKFRAME/WS_BORDER/WS_EX_WINDOWEDGE insets (which otherwise clip left/right HUD!)
            new_style, new_exstyle = strip_embedded_window_styles(self._orig_style, self._orig_exstyle)
            user32.SetWindowLongPtrW(hwnd, GWL_STYLE, new_style)
            user32.SetWindowLongPtrW(hwnd, GWL_EXSTYLE, new_exstyle)
            user32.SetWindowPos(
                hwnd, 0, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED,
            )
            user32.SetParent(hwnd, parent_hwnd)
            # Ensure parent clips children cleanly
            p_style = int(user32.GetWindowLongPtrW(parent_hwnd, GWL_STYLE))
            if not (p_style & WS_CLIPCHILDREN):
                user32.SetWindowLongPtrW(parent_hwnd, GWL_STYLE, p_style | WS_CLIPCHILDREN)
            self._child_hwnd = hwnd
            self._last_embedded_size = (0, 0)
            self.lbl_placeholder.hide()
            self.lbl_state.setText("● PIE RUNNING")
            self._sync_embedded_geometry()
            self.focus_embedded_game()
            # Re-sync after Vulkan swapchain creation so client rect & HUD never clip on left/right edges
            QTimer.singleShot(150, self._sync_embedded_geometry)
            QTimer.singleShot(450, self._sync_embedded_geometry)
            QTimer.singleShot(900, self._sync_embedded_geometry)
            QTimer.singleShot(800, self.focus_embedded_game)
            self.pieStatusChanged.emit(
                f"PIE embedded live Vulkan viewport (HWND 0x{hwnd:X}) — WASD/Arrows or Controller to drive"
            )
        except Exception as exc:
            self.pieStatusChanged.emit(f"PIE embed warning: {exc}")

    def _measure_child_nc_insets(self, child_hwnd: int) -> tuple[int, int, int, int]:
        """Measure any residual non-client (left, top, right, bottom) frame insets on `child_hwnd`."""
        if sys.platform != "win32" or not child_hwnd:
            return (0, 0, 0, 0)
        try:
            import ctypes.wintypes as wt

            user32 = _win32_user32()
            wrc = wt.RECT()
            crc = wt.RECT()
            if not user32.GetWindowRect(child_hwnd, ctypes.byref(wrc)):
                return (0, 0, 0, 0)
            if not user32.GetClientRect(child_hwnd, ctypes.byref(crc)):
                return (0, 0, 0, 0)
            pt = wt.POINT(0, 0)
            if hasattr(user32, "ClientToScreen") and user32.ClientToScreen(child_hwnd, ctypes.byref(pt)):
                nc_l = max(0, int(pt.x - wrc.left))
                nc_t = max(0, int(pt.y - wrc.top))
                nc_r = max(0, int(wrc.right - (pt.x + (crc.right - crc.left))))
                nc_b = max(0, int(wrc.bottom - (pt.y + (crc.bottom - crc.top))))
                return (nc_l, nc_t, nc_r, nc_b)
        except Exception:
            pass
        return (0, 0, 0, 0)

    def _physical_client_size(self) -> tuple[int, int]:
        """Exact physical pixel client size of embed_container (DPI-exact across 100%-200%+ scaling)."""
        win32_sz: tuple[int, int] | None = None
        if sys.platform == "win32":
            try:
                import ctypes.wintypes as wt

                user32 = _win32_user32()
                parent_hwnd = int(self.embed_container.winId())
                if parent_hwnd:
                    rc = wt.RECT()
                    if user32.GetClientRect(parent_hwnd, ctypes.byref(rc)):
                        rw = int(rc.right - rc.left)
                        rh = int(rc.bottom - rc.top)
                        if rw > 0 and rh > 0:
                            win32_sz = (rw, rh)
            except Exception:
                pass
        dpr = float(self.embed_container.devicePixelRatioF() or self.devicePixelRatioF() or 1.0)
        rect = compute_embedded_client_rect(
            self.embed_container.width(),
            self.embed_container.height(),
            dpr,
            win32_client_size=win32_sz,
        )
        return rect["client_w"], rect["client_h"]

    def _sync_embedded_geometry(self) -> None:
        if sys.platform != "win32" or not self._child_hwnd or self._popped_out:
            return
        try:
            user32 = _win32_user32()
            if not user32.IsWindow(self._child_hwnd):
                return
            parent_hwnd = int(self.embed_container.winId())
            parent_dpi = 0
            child_dpi = 0
            if hasattr(user32, "GetDpiForWindow"):
                try:
                    if parent_hwnd:
                        parent_dpi = int(user32.GetDpiForWindow(parent_hwnd))
                    child_dpi = int(user32.GetDpiForWindow(self._child_hwnd))
                except Exception:
                    parent_dpi = child_dpi = 0

            win32_sz: tuple[int, int] | None = None
            if parent_hwnd:
                import ctypes.wintypes as wt

                rc = wt.RECT()
                if user32.GetClientRect(parent_hwnd, ctypes.byref(rc)):
                    rw = int(rc.right - rc.left)
                    rh = int(rc.bottom - rc.top)
                    if rw > 0 and rh > 0:
                        win32_sz = (rw, rh)

            dpr = float(self.embed_container.devicePixelRatioF() or self.devicePixelRatioF() or 1.0)
            nc_insets = self._measure_child_nc_insets(self._child_hwnd)
            rect = compute_embedded_client_rect(
                self.embed_container.width(),
                self.embed_container.height(),
                dpr,
                win32_client_size=win32_sz,
                nc_insets=nc_insets,
                parent_dpi=parent_dpi,
                child_dpi=child_dpi,
            )
            x, y, w, h = rect["x"], rect["y"], rect["w"], rect["h"]
            user32.MoveWindow(self._child_hwnd, x, y, w, h, True)
            user32.SetWindowPos(
                self._child_hwnd, 0, x, y, w, h,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW,
            )
            wm_w, wm_h = rect["wm_size_w"], rect["wm_size_h"]
            lparam = ((wm_h & 0xFFFF) << 16) | (wm_w & 0xFFFF)
            user32.SendMessageW(self._child_hwnd, WM_SIZE, SIZE_RESTORED, lparam)
            self._last_embedded_size = (rect["client_w"], rect["client_h"])
        except Exception:
            pass

    def forward_key_event(self, event: QKeyEvent, pressed: bool) -> bool:
        """Belt-and-suspenders Qt -> Win32 WM_KEYDOWN/WM_KEYUP forwarding to embedded eod.exe."""
        if sys.platform != "win32" or not self._child_hwnd or self._popped_out:
            return False
        vk = _qt_key_to_vk(event)
        if not vk:
            return False
        scan = int(event.nativeScanCode()) & 0xFF
        lparam = (1 | (scan << 16))
        if not pressed:
            lparam |= (1 << 30) | (1 << 31)
        elif event.isAutoRepeat():
            lparam |= (1 << 30)
        try:
            user32 = _win32_user32()
            if not user32.IsWindow(self._child_hwnd):
                return False
            msg = WM_KEYDOWN if pressed else WM_KEYUP
            return bool(user32.PostMessageW(self._child_hwnd, msg, vk, lparam))
        except Exception:
            return False

    def pop_out_window(self) -> None:
        """Detach embedded `eod.exe` back into a standalone top-level Win32 window."""
        self._detach_thread_input()
        if sys.platform == "win32" and self._child_hwnd:
            try:
                user32 = _win32_user32()
                user32.SetParent(self._child_hwnd, 0)
                if self._orig_style:
                    user32.SetWindowLongPtrW(self._child_hwnd, GWL_STYLE, self._orig_style)
                if self._orig_exstyle:
                    user32.SetWindowLongPtrW(self._child_hwnd, GWL_EXSTYLE, self._orig_exstyle | WS_EX_APPWINDOW)
                user32.MoveWindow(self._child_hwnd, 120, 80, 1280, 720, True)
                user32.SetWindowPos(
                    self._child_hwnd, 0, 120, 80, 1280, 720, SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW
                )
            except Exception:
                pass
        self._popped_out = True
        self._child_hwnd = 0
        self.btn_popout.setEnabled(False)
        self.lbl_state.setText("● PIE POPPED OUT")
        self.lbl_placeholder.setText(
            "eod.exe is running in its popped-out window.\nClick Stop PIE (Shift+Esc) to return to the 3D editor."
        )
        self.lbl_placeholder.show()
        self.pieStatusChanged.emit("PIE popped out to standalone window")

    def stop_pie(self, emit_signal: bool = True) -> None:
        """Cleanly close `eod.exe` and return to the OpenGL 3D editor viewport."""
        self._embed_timer.stop()
        self._detach_thread_input()
        self._remove_app_filter()
        if sys.platform == "win32" and self._child_hwnd:
            try:
                user32 = _win32_user32()
                user32.PostMessageW(self._child_hwnd, WM_CLOSE, 0, 0)
            except Exception:
                pass
        if self._proc is not None:
            if self._proc.poll() is None:
                try:
                    self._proc.terminate()
                    self._proc.wait(timeout=2.5)
                except Exception:
                    try:
                        self._proc.kill()
                    except Exception:
                        pass
            self._proc = None
        self._child_hwnd = 0
        self._popped_out = False
        self.lbl_state.setText("● PIE STOPPED")
        if emit_signal:
            self.pieStatusChanged.emit("PIE stopped — returned to OpenGL 3D Editor")
            self.pieStopped.emit()

    def eventFilter(self, obj: QObject, event: QEvent) -> bool:
        if self.is_running and not self._popped_out and self._child_hwnd:
            etype = event.type()
            if obj is self.embed_container and etype in (
                QEvent.Type.Resize,
                QEvent.Type.Move,
                QEvent.Type.Show,
            ):
                QTimer.singleShot(0, self._sync_embedded_geometry)
            elif etype == QEvent.Type.MouseButtonPress and obj is self.embed_container:
                self.focus_embedded_game()
            elif etype in (QEvent.Type.KeyPress, QEvent.Type.KeyRelease) and isinstance(event, QKeyEvent):
                # Do not steal typing from text/number inputs when user intentionally clicks one
                fw = QApplication.focusWidget()
                if isinstance(fw, (QLineEdit, QTextEdit, QPlainTextEdit, QSpinBox)):
                    return super().eventFilter(obj, event)
                if etype == QEvent.Type.KeyPress and event.key() == Qt.Key.Key_Escape:
                    if event.modifiers() & Qt.KeyboardModifier.ShiftModifier:
                        self.stop_pie()
                        return True
                # Only forward keys while PIE widget is visible inside active window
                if self.isVisible():
                    self.forward_key_event(event, etype == QEvent.Type.KeyPress)
                    return True
        return super().eventFilter(obj, event)

    def mousePressEvent(self, event) -> None:
        self.focus_embedded_game()
        super().mousePressEvent(event)

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        self._sync_embedded_geometry()
        QTimer.singleShot(0, self._sync_embedded_geometry)

    def keyPressEvent(self, event: QKeyEvent) -> None:
        if event.key() == Qt.Key.Key_Escape and (event.modifiers() & Qt.KeyboardModifier.ShiftModifier):
            self.stop_pie()
            return
        if self.is_running and self._child_hwnd and not self._popped_out:
            self.forward_key_event(event, True)
            event.accept()
            return
        super().keyPressEvent(event)

    def keyReleaseEvent(self, event: QKeyEvent) -> None:
        if self.is_running and self._child_hwnd and not self._popped_out:
            self.forward_key_event(event, False)
            event.accept()
            return
        super().keyReleaseEvent(event)
