"""Exercise the shipped Windows World Editor with no game files."""

from __future__ import annotations

import ctypes
from ctypes import wintypes
import io
import json
import os
from pathlib import Path
import struct
import sys
import traceback
from unittest.mock import patch


def assert_under(child: Path, parent: Path) -> None:
    if not child.resolve().is_relative_to(parent.resolve()):
        raise RuntimeError(f"Expected a bundled file under {parent.name}: {child.name}")


def loaded_module(name: str) -> Path:
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
    kernel.GetModuleHandleW.restype = wintypes.HMODULE
    kernel.GetModuleFileNameW.argtypes = [wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]
    kernel.GetModuleFileNameW.restype = wintypes.DWORD
    handle = kernel.GetModuleHandleW(name)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    path = ctypes.create_unicode_buffer(32768)
    size = kernel.GetModuleFileNameW(handle, path, len(path))
    if not 0 < size < len(path):
        raise ctypes.WinError(ctypes.get_last_error())
    return Path(path.value).resolve()


def run(bundle: Path, empty_game: Path) -> int:
    if sys.platform != "win32" or struct.calcsize("P") != 8:
        raise SystemExit("Expected native 64-bit Windows CPython.")
    runtime = bundle / "runtime" / "python"
    site = runtime / "Lib" / "site-packages"
    source = bundle / "tools" / "world_editor"
    profile = Path(os.environ["EOD_EDITOR_APPDATA"])
    assert_under(Path(sys.executable), runtime)
    assert_under(loaded_module("python313.dll"), runtime)
    if not profile.is_dir() or Path(os.environ["LOCALAPPDATA"]).parent != profile.parent:
        raise RuntimeError("The World Editor requires an isolated smoke-test profile.")

    os.environ["QT_PLUGIN_PATH"] = str(site / "PySide6" / "plugins")
    os.environ["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(site / "PySide6" / "plugins" / "platforms")
    sys.path.insert(0, str(source))
    from OpenGL import GL
    from PIL import Image, _imaging
    import numpy as np
    from numpy._core import _multiarray_umath
    from PySide6 import QtCore, QtGui, QtOpenGL, QtOpenGLWidgets, QtSvg, QtWidgets
    import shiboken6
    import app as editor_app
    from project_state import apply_project_overlay, save_project_overlay
    from world_loader import CollisionData, RoadGraph, TrackWorld

    for module in (GL, Image, _imaging, np, _multiarray_umath, QtCore, QtGui, QtOpenGL, QtOpenGLWidgets,
                   QtSvg, QtWidgets, shiboken6):
        assert_under(Path(module.__file__), site)
    for name in ("Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6OpenGL.dll", "Qt6OpenGLWidgets.dll", "Qt6Svg.dll"):
        assert_under(loaded_module(name), site)
    assert_under(Path(editor_app.__file__), source)

    # Resolve the native editor extension's Windows load-time dependencies without creating a GPU device.
    plume_path = bundle / "eod_editor_plume.dll"
    ctypes.WinDLL(str(plume_path))
    if loaded_module(plume_path.name) != plume_path.resolve():
        raise RuntimeError("The native viewport library resolved outside the bundle.")

    matrix = np.array([[2.0, 0.0], [0.0, 4.0]])
    if not np.allclose(np.linalg.solve(matrix, np.array([6.0, 20.0])), [3.0, 5.0]):
        raise RuntimeError("The bundled NumPy native linear algebra check failed.")
    png = io.BytesIO()
    Image.new("RGBA", (1, 1), (21, 42, 84, 255)).save(png, format="PNG")
    png.seek(0)
    with Image.open(png) as image:
        if image.getpixel((0, 0)) != (21, 42, 84, 255):
            raise RuntimeError("The bundled Pillow native PNG check failed.")

    class SmokeApplication(QtWidgets.QApplication):
        def __init__(self, arguments: list[str]):
            super().__init__(arguments)
            QtCore.QTimer.singleShot(0, self.check_window)

        def check_window(self) -> None:
            try:
                self.verify_window()
            except Exception:
                traceback.print_exc()
                self.exit(1)
            else:
                self.exit(0)

        def verify_window(self) -> None:
            windows = [
                widget for widget in self.topLevelWidgets()
                if isinstance(widget, editor_app.EodWorldEditorWindow) and widget.isVisible()
            ]
            if self.platformName() != "offscreen" or len(windows) != 1:
                raise RuntimeError("The expected visible offscreen World Editor main window was not created.")
            assert_under(loaded_module("qoffscreen.dll"), site)
            window, = windows
            if (window.game_dir != empty_game or window.available_tracks or window.world is not None
                    or not window.centralWidget()):
                raise RuntimeError("The main window did not start with a clean, empty game directory.")

            # Provide just the name the user normally types. Run the real project action and serialization.
            with patch.object(QtWidgets.QInputDialog, "getText", return_value=("Windows smoke", True)):
                window.new_project()
            if window.project_name != "Windows smoke" or window.project_path is not None:
                raise RuntimeError("The World Editor New Project action did not initialize a project.")

            road_graph = RoadGraph(line_name="SMOKE")
            world = TrackWorld(
                game_dir=empty_game, trk_path=empty_game / "synthetic.trk", trk_rel="synthetic.trk",
                track_name="SMOKE", display_name="Synthetic Windows smoke", entries=[], graf_summary={},
                mlist_info=[], models={}, objects=[], textures={}, collision=CollisionData(scol_count=0, sobl_count=0, sobl_boxes=0, sobl_tubes=0),
                road_graph=road_graph, all_road_graphs={}, markers=[], world_center=(0.0, 0.0, 0.0),
                world_radius=1.0,
            )
            overlay = save_project_overlay(profile / "windows-smoke.eodworld.json", window.project_name, world)
            result = apply_project_overlay(world, overlay)
            if (json.loads(overlay.read_text(encoding="utf-8"))["schema"] != "eod.world_project/2"
                    or result["project_name"] != window.project_name):
                raise RuntimeError("The synthetic project overlay did not round-trip.")

            window.close()
            print(f"PASS bundled World Editor window and synthetic project overlay "
                  f"(Python {sys.version.split()[0]}, Qt {QtCore.qVersion()}, "
                  f"NumPy {np.__version__}, native viewport dependency loaded)", flush=True)

    editor_app.QApplication = SmokeApplication
    sys.argv = [str(source / "app.py"), "--game-dir", str(empty_game), "--no-home-hub"]
    return editor_app.main()


if __name__ == "__main__":
    raise SystemExit(run(Path(sys.argv[1]).resolve(strict=True), Path(sys.argv[2]).resolve(strict=True)))
