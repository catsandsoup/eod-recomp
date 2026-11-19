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
from pathlib import Path
from typing import Any

from PySide6.QtCore import Qt, QTimer, Signal
from PySide6.QtWidgets import (
    QFrame,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from theme import ACCENT_GREEN, BG_ROOT, BORDER, TEXT_PRIMARY, TEXT_SECONDARY, svg_icon

# Win32 window style constants
GWL_STYLE = -16
GWL_EXSTYLE = -20
WS_CHILD = 0x40000000
WS_VISIBLE = 0x10000000
WS_POPUP = 0x80000000
WS_CAPTION = 0x00C00000
WS_THICKFRAME = 0x00040000
WS_MINIMIZEBOX = 0x00020000
WS_MAXIMIZEBOX = 0x00010000
WS_SYSMENU = 0x00080000
WS_EX_APPWINDOW = 0x00040000
WM_CLOSE = 0x0010
SWP_NOZORDER = 0x0004
SWP_NOACTIVATE = 0x0010
SWP_FRAMECHANGED = 0x0020
SWP_SHOWWINDOW = 0x0040


def resolve_eod_exe(game_dir: Path | str) -> Path:
    """Locate `eod.exe` across game_dir, repo build/, and %LOCALAPPDATA%\\TDEODRecomp."""
    gdir = Path(game_dir).resolve()
    candidates = [
        gdir / "eod.exe",
        Path(__file__).resolve().parents[2] / "build" / "eod.exe",
    ]
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
) -> dict[str, Any]:
    """Build exact process args and environment variables for `eod.exe` PIE launch.

    Matches `src/session_start.h` and `runtime/render/native_renderer.cpp`:
      - Career Free Roam (HomeBase): `EOD_DIRECT_START="mode=career;place=<place>"`
      - Track Free Roam / Event: `EOD_DIRECT_START="mode=action;track=<DisplayName>;event=<event>;car=<car>;paint=0;humans=1"`
      - Renderer Mode (mutually exclusive per `native_renderer.cpp:1157,1893`):
          * `renderer_mode="ps2"`   -> `EOD_NATIVE=0`, `EOD_MODERN=0` (PS2 GS fast performance, sips power)
          * `renderer_mode="plume"` -> `EOD_MODERN=1`, `EOD_NATIVE=0` (Modern Plume Vulkan HQ)
      - Also sets `EOD_MOD_DIR=<mod_dir>` and `EOD_SKIP_MOVIES=1`.
    """
    gdir = Path(game_dir).resolve()
    mdir = Path(mod_dir).resolve()
    exe = resolve_eod_exe(gdir)
    clean_car = car_name.split("(")[0].strip() or "Algar"
    trk_up = track_display_name.strip()
    rmode = "plume" if renderer_mode == "plume" else "ps2"
    rtag = "Plume HQ" if rmode == "plume" else "PS2 Fast"

    if mode == "career_roam" or (mode == "free_roam" and trk_up.lower() in ("homebase", "home base", "career")):
        direct = f"mode=career;place={free_roam_place or 'home'}"
        label = f"Career Free Roam ({free_roam_place or 'home'}) [{rtag}]"
    elif mode == "free_roam":
        # Open-track Free Roam on any race/arena track: Battle mode with ai=0 (solo) or chosen AI
        roam_ai = max(0, int(ai_count)) if ai_count is not None else 0
        direct = f"mode=action;track={trk_up};event=Battle;car={clean_car};paint=0;humans=1;ai={roam_ai}"
        label = f"Free Roam (Battle, {roam_ai} AI) — {trk_up} ({clean_car}) [{rtag}]"
    else:
        ev = (event_name or "Battle").strip()
        if ev.lower() == "career overworld (free roam)":
            direct = f"mode=career;place={free_roam_place or 'home'}"
            label = f"Career Free Roam ({free_roam_place or 'home'}) [{rtag}]"
        else:
            clean_ev = "Battle" if ev.lower().startswith("battle") else ev
            parts = ["mode=action", f"track={trk_up}", f"event={clean_ev}", f"car={clean_car}", "paint=0", "humans=1"]
            if clean_ev.lower() == "battle":
                parts.append(f"ai={max(0, int(ai_count))}")
            direct = ";".join(parts)
            label = f"Event — {trk_up} · {ev} ({clean_car}) [{rtag}]"

    env = dict(os.environ)
    env["EOD_MOD_DIR"] = str(mdir)
    env["EOD_SKIP_MOVIES"] = "1"
    env["EOD_DIRECT_START"] = direct
    env["EOD_SESSION"] = direct  # alias for diagnostics/scripts
    env["SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS"] = "1"
    if rmode == "plume":
        env["EOD_MODERN"] = "1"
        env["EOD_NATIVE"] = "0"
    else:
        env["EOD_NATIVE"] = "0"
        env["EOD_MODERN"] = "0"

    return {
        "exe": str(exe),
        "cwd": str(gdir),
        "mod_dir": str(mdir),
        "direct_start": direct,
        "renderer_mode": rmode,
        "label": label,
        "env": env,
    }


_USER32_TYPED = None


def _win32_user32():
    """Return ctypes.windll.user32 with explicit 64-bit Win32 argtypes/restype."""
    global _USER32_TYPED
    if _USER32_TYPED is not None:
        return _USER32_TYPED
    import ctypes.wintypes as wt

    u32 = ctypes.windll.user32
    LONG_PTR = ctypes.c_ssize_t
    u32.IsWindowVisible.argtypes = [wt.HWND]
    u32.IsWindowVisible.restype = wt.BOOL
    u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
    u32.GetWindowThreadProcessId.restype = wt.DWORD
    u32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
    u32.GetWindowRect.restype = wt.BOOL
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
    _USER32_TYPED = u32
    return u32


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
        self._orig_style: int = 0
        self._orig_exstyle: int = 0
        self._popped_out: bool = False
        self._last_spec: dict[str, Any] | None = None
        self._poll_attempts: int = 0

        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        # Top Unreal-style PIE control bar
        self.bar = QFrame()
        self.bar.setStyleSheet(
            f"QFrame {{ background: #142219; border-bottom: 1px solid #2b6e44; }}"
        )
        bar_lay = QHBoxLayout(self.bar)
        bar_lay.setContentsMargins(10, 6, 10, 6)
        bar_lay.setSpacing(8)

        self.lbl_state = QLabel("● PIE READY")
        self.lbl_state.setStyleSheet(
            f"color: {ACCENT_GREEN}; font-weight: 700; font-size: 9pt; background: transparent;"
        )
        self.lbl_detail = QLabel("")
        self.lbl_detail.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 8.5pt; background: transparent;")

        self.btn_restart = QPushButton(" Restart")
        self.btn_restart.setIcon(svg_icon("rotate"))
        self.btn_restart.setFixedHeight(26)
        self.btn_restart.clicked.connect(self.pieRestartRequested.emit)

        self.btn_popout = QPushButton(" Pop Out Window")
        self.btn_popout.setIcon(svg_icon("popout"))
        self.btn_popout.setFixedHeight(26)
        self.btn_popout.clicked.connect(self.pop_out_window)

        self.btn_stop = QPushButton(" Stop PIE (Esc)")
        self.btn_stop.setIcon(svg_icon("stop", "#ff99a2"))
        self.btn_stop.setProperty("danger", True)
        self.btn_stop.setFixedHeight(26)
        self.btn_stop.clicked.connect(self.stop_pie)

        bar_lay.addWidget(self.lbl_state)
        bar_lay.addWidget(self.lbl_detail, 1)
        bar_lay.addWidget(self.btn_restart)
        bar_lay.addWidget(self.btn_popout)
        bar_lay.addWidget(self.btn_stop)
        root.addWidget(self.bar)

        # Native Win32 embed surface
        self.embed_container = QWidget()
        self.embed_container.setAttribute(Qt.WidgetAttribute.WA_NativeWindow, True)
        self.embed_container.setStyleSheet(f"background-color: {BG_ROOT}; border: none;")
        c_lay = QVBoxLayout(self.embed_container)
        self.lbl_placeholder = QLabel("Launching eod.exe Vulkan viewport...")
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

    def start_pie(self, spec: dict[str, Any]) -> bool:
        """Start `eod.exe` using `spec` from `build_pie_launch_spec` and begin Win32 embedding."""
        self.stop_pie(emit_signal=False)
        exe = Path(spec["exe"])
        if not exe.is_file():
            self.lbl_placeholder.setText(f"eod.exe not found at:\n{exe}")
            self.pieStatusChanged.emit(f"PIE failed: {exe} not found")
            return False

        self._last_spec = spec
        self._popped_out = False
        self._child_hwnd = 0
        self._poll_attempts = 0
        self.btn_popout.setEnabled(True)
        self.lbl_state.setText("● PIE STARTING")
        self.lbl_detail.setText(f"{spec['label']}  |  EOD_MOD_DIR={Path(spec['mod_dir']).name}")
        self.lbl_placeholder.setText(
            f"Starting eod.exe in viewport...\n\n{spec['label']}\nEOD_DIRECT_START = {spec['direct_start']}"
        )
        self.lbl_placeholder.show()

        self.pieLogLine.emit(
            f"[PIE] Launching {exe} | cwd={spec['cwd']} | renderer={spec.get('renderer_mode')} "
            f"| EOD_DIRECT_START={spec['direct_start']} | EOD_MOD_DIR={spec['mod_dir']}"
        )
        try:
            self._proc = subprocess.Popen(
                [str(exe)],
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
                            self.pieLogLine.emit(f"[eod.exe] {line}")
            except Exception:
                pass

        threading.Thread(target=_pump_stdout, daemon=True, name="eod-pie-stdout").start()

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
            code = self._proc.returncode
            self._proc = None
            self._child_hwnd = 0
            self.pieStatusChanged.emit(f"PIE exited (code {code})")
            self.pieStopped.emit()
            return

        if sys.platform == "win32" and not self._child_hwnd and not self._popped_out:
            self._poll_attempts += 1
            hwnd = _find_process_hwnd_win32(self._proc.pid)
            if hwnd:
                self._attach_hwnd(hwnd)
            elif self._poll_attempts > 250:
                # 15 seconds fallback: keep monitoring process without blocking
                self.lbl_placeholder.setText("eod.exe is running in an external window.\nClick Stop PIE (Esc) to return.")

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

            new_style = (
                self._orig_style
                & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_POPUP)
            ) | WS_CHILD | WS_VISIBLE
            user32.SetWindowLongPtrW(hwnd, GWL_STYLE, new_style)
            user32.SetParent(hwnd, parent_hwnd)
            self._child_hwnd = hwnd
            self.lbl_placeholder.hide()
            self.lbl_state.setText("● PIE RUNNING")
            self._sync_embedded_geometry()
            self.pieStatusChanged.emit(f"PIE embedded live Vulkan viewport (HWND 0x{hwnd:X})")
        except Exception as exc:
            self.pieStatusChanged.emit(f"PIE embed warning: {exc}")

    def _sync_embedded_geometry(self) -> None:
        if sys.platform != "win32" or not self._child_hwnd or self._popped_out:
            return
        try:
            user32 = _win32_user32()
            dpr = float(self.devicePixelRatioF())
            w = max(1, int(round(self.embed_container.width() * dpr)))
            h = max(1, int(round(self.embed_container.height() * dpr)))
            user32.MoveWindow(self._child_hwnd, 0, 0, w, h, True)
            user32.SetWindowPos(
                self._child_hwnd, 0, 0, 0, w, h,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW,
            )
        except Exception:
            pass

    def pop_out_window(self) -> None:
        """Detach embedded `eod.exe` back into a standalone top-level Win32 window."""
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
        self.lbl_placeholder.setText("eod.exe is running in its popped-out window.\nClick Stop PIE (Esc) to return to the 3D editor.")
        self.lbl_placeholder.show()
        self.pieStatusChanged.emit("PIE popped out to standalone window")

    def stop_pie(self, emit_signal: bool = True) -> None:
        """Cleanly close `eod.exe` and return to the OpenGL 3D editor viewport."""
        self._embed_timer.stop()
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

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        self._sync_embedded_geometry()

    def keyPressEvent(self, event) -> None:
        if event.key() == Qt.Key.Key_Escape:
            self.stop_pie()
            return
        super().keyPressEvent(event)
