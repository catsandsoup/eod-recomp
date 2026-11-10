#!/usr/bin/env python3
"""Package a separate development Creation Kit; never build or deploy the game."""
import argparse
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'tools/editor_macos'


def build_layout(root):
    root = Path(root).resolve()
    output = root / 'build/creation-kit'
    layout = dict(output=output, scratch=output / 'swift',
                  app=output / 'EOD Creation Kit Development.app',
                  stage=output / 'EOD Creation Kit Development.staging.app',
                  previous=output / 'EOD Creation Kit Development.previous.app')
    for path in layout.values():
        for component in (path, *path.parents):
            if component == root:
                break
            if component.is_symlink():
                raise ValueError('Development output cannot follow symbolic links: ' + str(component))
    return layout


def runtime_config(root, game_executable):
    game = Path(game_executable).expanduser().resolve()
    if not game.is_file() or not os.access(game, os.X_OK):
        raise ValueError('Choose an existing executable game build; packaging never builds the game')
    output = build_layout(root)['output']
    if game == output or output in game.parents:
        raise ValueError('Game executable must be outside the development packaging directory')
    return dict(repository=str(Path(root).resolve()), gameExecutable=str(game))


def bundle_info():
    return dict(CFBundleName='EOD Creation Kit Development',
                CFBundleDisplayName='EOD Creation Kit Development',
                CFBundleIdentifier='org.eod-recomp.creationkit.development',
                CFBundleExecutable='EODModdingSuite', CFBundlePackageType='APPL',
                CFBundleShortVersionString='0.3.0', CFBundleVersion='3',
                LSMinimumSystemVersion='14.0', NSHighResolutionCapable=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-executable', required=True, type=Path)
    args = parser.parse_args()
    layout = build_layout(ROOT)
    config = runtime_config(ROOT, args.game_executable)
    command = ['swift', 'build', '--package-path', str(PACKAGE),
               '--scratch-path', str(layout['scratch']), '-c', 'release']
    subprocess.run(command, check=True)
    binary_dir = subprocess.check_output(command + ['--show-bin-path'], text=True).strip()
    stage = layout['stage']
    if stage.exists():
        shutil.rmtree(stage)
    executable = stage / 'Contents/MacOS'
    resources = stage / 'Contents/Resources'
    executable.mkdir(parents=True)
    backend = resources / 'Backend'
    backend.mkdir(parents=True)
    shutil.copy2(Path(binary_dir) / 'EODModdingSuite', executable / 'EODModdingSuite')
    for name in ('modding_backend.py', 'rst0.py', 'valt.py', 'tex.py', 'texture_preview.py', 'texture_edit.py', 'vif_mesh.py'):
        shutil.copy2(ROOT / 'tools' / name, backend / name)
    (resources / 'studio-runtime.json').write_text(json.dumps(config))
    with (stage / 'Contents/Info.plist').open('wb') as handle:
        plistlib.dump(bundle_info(), handle)
    subprocess.run(['codesign', '--force', '--sign', '-', str(stage)], check=True)
    subprocess.run(['codesign', '--verify', '--strict', str(stage)], check=True)
    if layout['previous'].exists():
        shutil.rmtree(layout['previous'])
    if layout['app'].exists():
        layout['app'].rename(layout['previous'])
    stage.rename(layout['app'])
    print(layout['app'])


if __name__ == '__main__':
    main()
