#!/usr/bin/env python3
"""Build a relocatable Apple Silicon World Editor app and ZIP."""

import argparse
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
APP_NAME = "EOD World Editor"
ARCHIVE_NAME = "EOD-World-Editor-macOS-arm64.zip"


def run(*args, **options):
    subprocess.run(args, check=True, **options)


def package(game: Path, plume: Path, output: Path) -> Path:
    if output.exists():
        raise ValueError(f"Choose a new output directory: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    tools = ROOT / "tools"
    editor = tools / "world_editor"
    env = os.environ.copy()
    env.pop("EOD_BUNDLE_MODS", None)
    # Cloud-managed working folders can attach Finder metadata while a bundle is being signed.
    with tempfile.TemporaryDirectory(prefix="eod-world-editor-") as folder:
        stage = Path(folder)
        runtime = stage / "GameRuntime.bundle"
        run(sys.executable, tools / "macos/bundle.py", runtime, game, env=env)
        runtime_contents = runtime / "Contents"
        if not (runtime_contents / "Resources/vulkan/icd.d/MoltenVK_icd.json").is_file():
            raise ValueError("Build the game with Plume and Vulkan before packaging the editor")
        icon = runtime_contents / "Resources/AppIcon.icns"
        run(
            sys.executable, "-m", "PyInstaller", "--noconfirm", "--onedir", "--windowed",
            "--name", APP_NAME, "--osx-bundle-identifier", "org.eod-recomp.worldeditor",
            "--target-arch", "arm64", "--icon", icon,
            "--paths", tools, "--paths", editor, "--distpath", stage / "dist",
            "--workpath", stage / "work", "--specpath", stage,
            "--add-data", f"{tools / 'nascar_livery_ids.json'}:.",
            "--add-data", f"{tools / 'formats/valt_schema_data.json'}:formats",
            editor / "app.py",
        )
        app = stage / "dist" / f"{APP_NAME}.app"
        contents = app / "Contents"
        runtime.rename(contents / "Resources" / runtime.name)
        library = contents / "Frameworks" / "eod_editor_plume.dylib"
        shutil.copyfile(plume, library)
        run("xattr", "-cr", app)
        run("codesign", "--force", "--sign", "-", library)
        info_path = contents / "Info.plist"
        info = plistlib.loads(info_path.read_bytes())
        game_info = plistlib.loads((contents / "Resources" / runtime.name / "Contents/Info.plist").read_bytes())
        info["CFBundleShortVersionString"] = game_info["CFBundleShortVersionString"]
        info["CFBundleVersion"] = game_info["CFBundleVersion"]
        info["LSMinimumSystemVersion"] = game_info["LSMinimumSystemVersion"]
        info["NSHighResolutionCapable"] = True
        info_path.write_bytes(plistlib.dumps(info))
        for path in app.rglob("*"):
            if path.name in ("DATA", "Mods", "SLUS_209.10") or path.suffix.lower() in (
                ".iso", ".trk", ".car", ".res", ".eodproject", ".eodworld", ".eodmod",
            ):
                raise ValueError(f"Unexpected game or project data in the editor bundle: {path}")
        run("codesign", "--force", "--sign", "-", app)
        run("codesign", "--verify", "--deep", "--strict", app)
        archive = stage / ARCHIVE_NAME
        run("ditto", "-c", "-k", "--keepParent", "--norsrc", app, archive)
        output.mkdir()
        shutil.move(app, output / app.name)
        shutil.move(archive, output / archive.name)
    return output / ARCHIVE_NAME


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-executable", required=True, type=Path)
    parser.add_argument("--plume-library", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("Build the macOS World Editor on macOS")
    print(package(args.game_executable.resolve(strict=True), args.plume_library.resolve(strict=True),
                  args.output.resolve()))


if __name__ == "__main__":
    main()
