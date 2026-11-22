#!/usr/bin/env python3
"""Assemble and verify a standalone Windows x64 release bundle for EOD Remaster + World Editor.

Creates `dist/EOD-Recomp-Windows-x64/` (and optional `.zip` + `SHA256SUMS-Windows-x64.txt`)
containing:
  - `eod.exe` (and `eodsetup.exe` if present in `build/`)
  - `Launch EOD World Editor.bat` (1-click root launcher with auto-venv bootstrap or PyInstaller runtime)
  - `tools/world_editor/` + all required `tools/*.py` and `tools/formats/*` modules
  - `README.md`, `docs/TUTORIALS.md`, `docs/WORLD_EDITOR.md`, companion specs, and `docs/img/` screenshots
  - Strict safety check guaranteeing zero retail disc assets (`DATA/`, `.TRK`, `.CAR`, `.RES`, `.ISO`, `SLUS_209.10`).

Usage:
  python tools/package_windows_release.py --verify-only
  python tools/package_windows_release.py --zip
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "dist" / "EOD-Recomp-Windows-x64"

# Core World Editor & Modding Python modules in tools/
REQUIRED_TOOL_FILES: tuple[str, ...] = (
    "asset_registry.py",
    "audio.py",
    "create_custom_car.py",
    "eod_mod_gui.py",
    "import_nascar.py",
    "import_nascar_cars.py",
    "iso9660_reader.py",
    "mod_install.py",
    "mod_package.py",
    "modding_backend.py",
    "modl.py",
    "nascar_engine_audio.py",
    "nascar_gen.py",
    "nascar_livery_ids.json",
    "nascar_parts.py",
    "nascar_preview.py",
    "nascar_roster.py",
    "nascar_wheels.py",
    "netplay_content_manifest.py",
    "rst0.py",
    "sky_export.py",
    "tex.py",
    "texture_edit.py",
    "texture_export.py",
    "texture_identity.py",
    "texture_preview.py",
    "trk.py",
    "valt.py",
    "valt_schema_build.py",
    "vehicle_export.py",
    "vehicle_import.py",
    "vehicle_materials.py",
    "vif_mesh.py",
    "vif_patch.py",
    "world_collision.py",
    "world_export.py",
    "world_graf.py",
)

REQUIRED_WORLD_EDITOR_FILES: tuple[str, ...] = (
    "__init__.py",
    "app.py",
    "car_studio_handling.py",
    "dialogs.py",
    "explorer_panel.py",
    "home_hub.py",
    "launch_editor.bat",
    "launch_editor_console.bat",
    "main_window.py",
    "mod_manager.py",
    "nascar_import_panel.py",
    "perf_profiler.py",
    "pie_host.py",
    "plume_viewport.py",
    "profile_pie.py",
    "project_state.py",
    "properties_panel.py",
    "test_editor.py",
    "theme.py",
    "thumbnails.py",
    "vehicle_overlay.py",
    "viewport_gl.py",
    "workers.py",
    "world_loader.py",
)

REQUIRED_DOCS: tuple[str, ...] = (
    "docs/WORLD_EDITOR.md",
    "docs/WORLD_EDITOR_ROADMAP.md",
    "docs/WORLD_EDITOR_ARCHITECTURE.md",
    "docs/WORLD_EDITOR_FRICTION_AND_UX.md",
)

OPTIONAL_DOCS: tuple[str, ...] = (
    "docs/TUTORIALS.md",
    "docs/NATIVE_RENDERER.md",
    "docs/FORMAT_COVERAGE.md",
    "docs/WORLD_FORMAT.md",
    "docs/COLLISION_AND_BOUNDARIES.md",
    "docs/ILIN_RACING_LINES.md",
    "docs/HOST_TEXTURE_REPLACEMENT.md",
    "docs/CUSTOM_VEHICLE_PIPELINE.md",
)

REQUIRED_CURATED_SCREENSHOTS: tuple[str, ...] = (
    "docs/img/world_editor/world_editor_overview.jpg",
    "docs/img/world_editor/world_editor_pie.jpg",
    "docs/img/world_editor/world_editor_ilin_and_cars.jpg",
    "docs/img/world_editor/world_editor_inspector.jpg",
    "docs/img/world_editor/remaster_6x_gameplay.jpg",
)

FORBIDDEN_DIR_NAMES: frozenset[str] = frozenset({
    "DATA",
    "Mods",
    "SLUS_209.10",
    ".venv",
    "__pycache__",
})

FORBIDDEN_SUFFIXES: frozenset[str] = frozenset({
    ".car",
    ".trk",
    ".res",
    ".bnk",
    ".iso",
    ".bin",
    ".pss",
    ".m2v",
    ".eodproject",
    ".pyc",
})

ROOT_LAUNCHER_BAT = r"""@echo off
setlocal EnableExtensions
set "BUNDLE_ROOT=%~dp0"
set "EDITOR_DIR=%BUNDLE_ROOT%tools\world_editor"
set "ONEDIR_EXE=%EDITOR_DIR%\dist\EODWorldEditor\EODWorldEditor.exe"
set "VENV_DIR=%EDITOR_DIR%\.venv"
set "VENV_PYTHONW=%VENV_DIR%\Scripts\pythonw.exe"
set "VENV_PYTHON=%VENV_DIR%\Scripts\python.exe"

REM 1. Prefer bundled PyInstaller one-dir executable if built
if exist "%ONEDIR_EXE%" (
    start "" /D "%EDITOR_DIR%" "%ONEDIR_EXE%" %*
    exit /b 0
)

REM 2. Use existing virtual environment if already bootstrapped
if exist "%VENV_PYTHONW%" (
    start "" /D "%EDITOR_DIR%" "%VENV_PYTHONW%" "%EDITOR_DIR%\app.py" %*
    exit /b 0
)

REM 3. First-run auto-bootstrap of Python virtual environment for World Editor
echo ============================================================================
echo  EOD 3D World Editor ^& Creation Kit - First-Time Environment Bootstrap
echo ============================================================================
echo.

set "SYS_PYTHON="
where py >nul 2>nul && set "SYS_PYTHON=py -3"
if "%SYS_PYTHON%"=="" (
    where python >nul 2>nul && set "SYS_PYTHON=python"
)
if "%SYS_PYTHON%"=="" (
    echo [ERROR] Python 3.10+ was not found on your PATH.
    echo Please install Python 3.11 or 3.12 from https://www.python.org/downloads/
    echo and re-run "Launch EOD World Editor.bat".
    pause
    exit /b 1
)

echo [1/2] Creating isolated virtual environment in "%VENV_DIR%"...
%SYS_PYTHON% -m venv "%VENV_DIR%"
if errorlevel 1 (
    echo [ERROR] Failed to create virtual environment.
    pause
    exit /b 1
)

echo [2/2] Installing Qt 6 ^(PySide6^), OpenGL, NumPy, and Pillow...
"%VENV_PYTHON%" -m pip install --upgrade pip
"%VENV_PYTHON%" -m pip install PySide6 numpy Pillow PyOpenGL
if errorlevel 1 (
    echo [ERROR] Failed to install World Editor dependencies.
    pause
    exit /b 1
)

echo.
echo [OK] Launching EOD 3D World Editor...
start "" /D "%EDITOR_DIR%" "%VENV_PYTHONW%" "%EDITOR_DIR%\app.py" %*
endlocal
"""

TUTORIALS_FALLBACK_MD = """# EOD Remaster & 3D World Editor Tutorials

See [README.md](../README.md) and [WORLD_EDITOR.md](WORLD_EDITOR.md) for quick-start instructions,
keyboard shortcuts, and Play-In-Editor (`F5`) workflows.
"""


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_ast_import_closure(root: Path) -> list[str]:
    """Verify that every local `tools/` and `tools/world_editor/` import is listed in REQUIRED_*_FILES."""
    tools_dir = root / "tools"
    we_dir = tools_dir / "world_editor"
    all_tool_py = {p.stem: p.name for p in tools_dir.glob("*.py")}
    all_we_py = {p.stem: p.name for p in we_dir.glob("*.py")}
    bundled_tool_set = set(REQUIRED_TOOL_FILES)
    bundled_we_set = set(REQUIRED_WORLD_EDITOR_FILES)
    missing: list[str] = []

    queue = list(we_dir.glob("*.py"))
    visited: set[Path] = set()
    while queue:
        p = queue.pop(0)
        if p in visited or not p.is_file():
            continue
        visited.add(p)
        tree = ast.parse(p.read_text(encoding="utf-8"), filename=str(p))
        for node in ast.walk(tree):
            mod_names: list[str] = []
            if isinstance(node, ast.Import):
                mod_names.extend(n.name.split(".")[0] for n in node.names)
            elif isinstance(node, ast.ImportFrom):
                if node.level > 0 and node.module:
                    mod_names.append(node.module.split(".")[0])
                elif node.module:
                    parts = node.module.split(".")
                    mod_names.append(parts[0])
                    if parts[0] == "world_editor" and len(parts) > 1:
                        mod_names.append(parts[1])
            for m in mod_names:
                if p.parent == we_dir and m in all_we_py:
                    fname = all_we_py[m]
                    if fname not in bundled_we_set and f"world_editor/{fname}" not in missing:
                        missing.append(f"world_editor/{fname}")
                    queue.append(we_dir / fname)
                elif m in all_tool_py:
                    fname = all_tool_py[m]
                    if fname not in bundled_tool_set and fname not in missing:
                        missing.append(fname)
                    queue.append(tools_dir / fname)
    return missing


def verify_source_inputs(root: Path, game_exe: Path) -> dict[str, object]:
    """Validate all source inputs required to build the Windows release package."""
    errors: list[str] = []

    if not game_exe.is_file():
        errors.append(f"Missing game executable: {game_exe}")
    elif game_exe.stat().st_size < 1_000_000:
        errors.append(f"Game executable looks unexpectedly small ({game_exe.stat().st_size} bytes): {game_exe}")

    readme = root / "README.md"
    if not readme.is_file():
        errors.append("Missing README.md")

    for rel in REQUIRED_WORLD_EDITOR_FILES:
        p = root / "tools" / "world_editor" / rel
        if not p.is_file():
            errors.append(f"Missing World Editor module: tools/world_editor/{rel}")

    for rel in REQUIRED_TOOL_FILES:
        p = root / "tools" / rel
        if not p.is_file():
            errors.append(f"Missing required tool module: tools/{rel}")

    formats_dir = root / "tools" / "formats"
    if not (formats_dir / "__init__.py").is_file() or not (formats_dir / "ilin.py").is_file():
        errors.append("Missing tools/formats package files")

    for rel in REQUIRED_DOCS:
        if not (root / rel).is_file():
            errors.append(f"Missing required documentation file: {rel}")

    for rel in REQUIRED_CURATED_SCREENSHOTS:
        p = root / rel
        if not p.is_file() or p.stat().st_size == 0:
            errors.append(f"Missing or empty curated screenshot: {rel}")

    missing_closure = verify_ast_import_closure(root)
    if missing_closure:
        errors.append(f"Unbundled transitive tools/*.py imports detected: {missing_closure}")

    if errors:
        raise ValueError("Source verification failed:\n  - " + "\n  - ".join(errors))

    setup_exe = game_exe.parent / "eodsetup.exe"
    return {
        "game_executable": str(game_exe),
        "game_executable_size": game_exe.stat().st_size,
        "game_executable_sha256": sha256_file(game_exe),
        "has_eodsetup_exe": setup_exe.is_file(),
        "world_editor_files": len(REQUIRED_WORLD_EDITOR_FILES),
        "tool_files": len(REQUIRED_TOOL_FILES),
        "curated_screenshots": len(REQUIRED_CURATED_SCREENSHOTS),
        "tutorials_present": (root / "docs" / "TUTORIALS.md").is_file(),
    }


def verify_staged_bundle(bundle_dir: Path) -> dict[str, object]:
    """Audit a staged bundle directory to guarantee zero retail assets and full completeness."""
    if not bundle_dir.is_dir():
        raise ValueError(f"Bundle directory does not exist: {bundle_dir}")

    required_paths = [
        bundle_dir / "eod.exe",
        bundle_dir / "Launch EOD World Editor.bat",
        bundle_dir / "README.md",
        bundle_dir / "docs" / "WORLD_EDITOR.md",
        bundle_dir / "docs" / "TUTORIALS.md",
        bundle_dir / "tools" / "world_editor" / "app.py",
        bundle_dir / "tools" / "formats" / "ilin.py",
    ]
    for req in required_paths:
        if not req.is_file():
            raise ValueError(f"Staged bundle is missing required file: {req.relative_to(bundle_dir)}")

    for rel in REQUIRED_CURATED_SCREENSHOTS:
        if not (bundle_dir / rel).is_file():
            raise ValueError(f"Staged bundle is missing curated screenshot: {rel}")

    file_count = 0
    total_bytes = 0
    for path in bundle_dir.rglob("*"):
        if path.name in FORBIDDEN_DIR_NAMES:
            raise ValueError(f"Forbidden retail/private directory in release bundle: {path.relative_to(bundle_dir)}")
        if path.is_file():
            file_count += 1
            total_bytes += path.stat().st_size
            if path.suffix.lower() in FORBIDDEN_SUFFIXES:
                raise ValueError(f"Forbidden retail/binary file in release bundle: {path.relative_to(bundle_dir)}")
            if path.name.lower() in ("eod.ini", "slus_209.10"):
                raise ValueError(f"Forbidden runtime/retail file in release bundle: {path.relative_to(bundle_dir)}")

    return {
        "bundle_dir": str(bundle_dir),
        "file_count": file_count,
        "total_bytes": total_bytes,
        "zero_retail_assets_verified": True,
    }


def build_bundle(
    root: Path,
    game_exe: Path,
    output_dir: Path,
    create_zip: bool = False,
    run_pyinstaller: bool = False,
) -> dict[str, object]:
    """Assemble `dist/EOD-Recomp-Windows-x64/` and optional `.zip` archive."""
    src_report = verify_source_inputs(root, game_exe)

    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    # 1. Copy eod.exe (and eodsetup.exe / eod_editor_plume.dll if built)
    shutil.copy2(game_exe, output_dir / "eod.exe")
    setup_exe = game_exe.parent / "eodsetup.exe"
    if setup_exe.is_file():
        shutil.copy2(setup_exe, output_dir / "eodsetup.exe")
    plume_dll = game_exe.parent / "eod_editor_plume.dll"
    if plume_dll.is_file():
        shutil.copy2(plume_dll, output_dir / "eod_editor_plume.dll")

    # 2. Write 1-click root World Editor launcher
    (output_dir / "Launch EOD World Editor.bat").write_text(
        ROOT_LAUNCHER_BAT.replace("\r\n", "\n").replace("\n", "\r\n"),
        encoding="utf-8",
    )

    # 3. Copy tools/world_editor/, required tools/*.py, and tools/formats/
    dest_tools = output_dir / "tools"
    dest_we = dest_tools / "world_editor"
    dest_we.mkdir(parents=True, exist_ok=True)
    if plume_dll.is_file():
        shutil.copy2(plume_dll, dest_we / "eod_editor_plume.dll")
    for rel in REQUIRED_WORLD_EDITOR_FILES:
        shutil.copy2(root / "tools" / "world_editor" / rel, dest_we / rel)

    for rel in REQUIRED_TOOL_FILES:
        shutil.copy2(root / "tools" / rel, dest_tools / rel)

    shutil.copytree(
        root / "tools" / "formats",
        dest_tools / "formats",
        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
    )

    # 4. Copy README.md, docs, and screenshots
    shutil.copy2(root / "README.md", output_dir / "README.md")
    dest_docs = output_dir / "docs"
    dest_docs.mkdir(parents=True, exist_ok=True)

    for rel in REQUIRED_DOCS:
        shutil.copy2(root / rel, output_dir / rel)
    for rel in OPTIONAL_DOCS:
        src_doc = root / rel
        if src_doc.is_file():
            shutil.copy2(src_doc, output_dir / rel)

    if not (dest_docs / "TUTORIALS.md").is_file():
        (dest_docs / "TUTORIALS.md").write_text(TUTORIALS_FALLBACK_MD, encoding="utf-8")

    if (root / "docs" / "wiki").is_dir():
        shutil.copytree(
            root / "docs" / "wiki",
            dest_docs / "wiki",
            ignore=shutil.ignore_patterns(".git", "__pycache__"),
        )

    if (root / "docs" / "img").is_dir():
        shutil.copytree(
            root / "docs" / "img",
            dest_docs / "img",
            ignore=shutil.ignore_patterns("__pycache__"),
        )

    # 5. Create empty mods/ folder with a short README explaining delta-only mods
    mods_dir = output_dir / "mods"
    mods_dir.mkdir(exist_ok=True)
    (mods_dir / "README.txt").write_text(
        "Place installed .eodmod packages or World Editor mod layers in this folder.\n"
        "No retail DATA/ files are bundled with EOD Remaster.\n",
        encoding="utf-8",
    )

    # 6. Optional PyInstaller --onedir build if requested
    if run_pyinstaller:
        subprocess.run(
            [
                sys.executable,
                "-m",
                "PyInstaller",
                "--noconfirm",
                "--onedir",
                "--windowed",
                "--name",
                "EODWorldEditor",
                "--paths",
                str(dest_tools),
                "--paths",
                str(dest_we),
                "--distpath",
                str(dest_we / "dist"),
                "--workpath",
                str(root / "build" / "pyinstaller_work"),
                "--specpath",
                str(root / "build"),
                "--add-data",
                f"{dest_tools / 'nascar_livery_ids.json'}:.",
                "--add-data",
                f"{dest_tools / 'formats/valt_schema_data.json'}:formats",
                str(dest_we / "app.py"),
            ],
            check=True,
        )

    # 7. Audit staged bundle for completeness and zero retail assets
    bundle_report = verify_staged_bundle(output_dir)

    result: dict[str, object] = {
        "source": src_report,
        "bundle": bundle_report,
    }

    # 8. Optional .zip archive + SHA256SUMS
    if create_zip:
        zip_path = output_dir.parent / f"{output_dir.name}.zip"
        if zip_path.exists():
            zip_path.unlink()
        with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
            for item in sorted(output_dir.rglob("*")):
                if item.is_file():
                    arcname = f"{output_dir.name}/{item.relative_to(output_dir).as_posix()}"
                    zf.write(item, arcname)
        zip_sha = sha256_file(zip_path)
        sums_path = output_dir.parent / "SHA256SUMS-Windows-x64.txt"
        sums_path.write_text(f"{zip_sha}  {zip_path.name}\n", encoding="utf-8")
        result["zip_archive"] = str(zip_path)
        result["zip_size_bytes"] = zip_path.stat().st_size
        result["zip_sha256"] = zip_sha
        result["sha256sums_file"] = str(sums_path)

    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--game-executable",
        type=Path,
        default=ROOT / "build" / "eod.exe",
        help="Path to compiled Windows x64 eod.exe (default: build/eod.exe)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=DEFAULT_OUTPUT,
        help="Target release staging folder (default: dist/EOD-Recomp-Windows-x64)",
    )
    parser.add_argument(
        "--verify-only",
        action="store_true",
        help="Validate source inputs, import closure, and existing bundle (if present) without writing files",
    )
    parser.add_argument(
        "--zip",
        action="store_true",
        help="Also build dist/EOD-Recomp-Windows-x64.zip and SHA256SUMS-Windows-x64.txt",
    )
    parser.add_argument(
        "--pyinstaller",
        action="store_true",
        help="Optionally bundle a standalone PyInstaller --onedir World Editor executable",
    )
    args = parser.parse_args()

    game_exe = args.game_executable.resolve()
    output_dir = args.output.resolve()

    if args.verify_only:
        report: dict[str, object] = {
            "mode": "verify-only",
            "source": verify_source_inputs(ROOT, game_exe),
        }
        if output_dir.is_dir():
            report["staged_bundle"] = verify_staged_bundle(output_dir)
        print(json.dumps(report, indent=2))
        return 0

    report = build_bundle(
        root=ROOT,
        game_exe=game_exe,
        output_dir=output_dir,
        create_zip=args.zip,
        run_pyinstaller=args.pyinstaller,
    )
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
