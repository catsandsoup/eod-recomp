#!/usr/bin/env python3
"""Entry point for EOD World Editor (Qt 6 PySide6 + OpenGL 3.3 Core)."""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

# Ensure tools/world_editor is on sys.path even when launched from elsewhere
EDITOR_DIR = Path(__file__).resolve().parent
if str(EDITOR_DIR) not in sys.path:
    sys.path.insert(0, str(EDITOR_DIR))

from PySide6.QtGui import QSurfaceFormat
from PySide6.QtWidgets import QApplication

from home_hub import load_editor_prefs
from main_window import EodWorldEditorWindow
from perf_profiler import get_profiler
from theme import apply_dark_theme


def configure_opengl_format() -> None:
    fmt = QSurfaceFormat()
    fmt.setVersion(3, 3)
    fmt.setProfile(QSurfaceFormat.OpenGLContextProfile.CoreProfile)
    fmt.setDepthBufferSize(24)
    fmt.setStencilBufferSize(8)
    fmt.setSamples(4)
    fmt.setSwapBehavior(QSurfaceFormat.SwapBehavior.DoubleBuffer)
    QSurfaceFormat.setDefaultFormat(fmt)


def main(argv: list[str] | None = None) -> int:
    get_profiler().begin_phase("app_startup")
    parser = argparse.ArgumentParser(description="EOD World Editor (Qt 6 + OpenGL 3.3 Core)")
    parser.add_argument("--game-dir", type=Path, default=None, help="Game directory containing DATA/TRACKS")
    parser.add_argument("--track", type=str, default=None, help="Initial track stem (e.g. AFTON, DUNDAS) — skips Home Hub")
    parser.add_argument("--project", type=Path, default=None, help="Optional .eodworld.json overlay to open — skips Home Hub")
    parser.add_argument("--no-home-hub", action="store_true", help="Skip the Home Hub launcher dialog on startup")
    args = parser.parse_args(argv)

    configure_opengl_format()

    app = QApplication(sys.argv)
    app.setApplicationName("EOD World Editor")
    app.setOrganizationName("eod-recomp")
    apply_dark_theme(app)

    prefs = load_editor_prefs()
    skip_hub = (
        args.track is not None
        or args.project is not None
        or bool(args.no_home_hub)
        or bool(prefs.get("skip_home_hub", False))
        or os.environ.get("EOD_SKIP_HOME_HUB") == "1"
    )
    initial_track = args.track or str(prefs.get("last_track", "AFTON"))

    window = EodWorldEditorWindow(
        game_dir=args.game_dir,
        initial_track=initial_track,
        project_file=args.project,
        show_home_hub=not skip_hub,
    )
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
