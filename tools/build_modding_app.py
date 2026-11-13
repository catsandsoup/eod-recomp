#!/usr/bin/env python3
"""Package a separate development Creation Kit; never build or deploy the game."""
import argparse
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'tools/editor_macos'


def build_layout(root):
    root = Path(root).resolve()
    output = root / 'build/creation-kit'
    layout = dict(output=output, scratch=output / 'swift',
                  app=output / 'EOD Creation Kit Development.app',
                  stage=output / 'EOD Creation Kit Development.staging.app',
                  previous=output / 'EOD Creation Kit Development.previous.app.backup')
    for path in layout.values():
        for component in (path, *path.parents):
            if component == root:
                break
            if component.is_symlink():
                raise ValueError('Development output cannot follow symbolic links: ' + str(component))
    return layout


def runtime_config(root, game_executable, initial_project=None):
    game = Path(game_executable).expanduser().resolve()
    if not game.is_file() or not os.access(game, os.X_OK):
        raise ValueError('Choose an existing executable game build; packaging never builds the game')
    output = build_layout(root)['output']
    if game == output or output in game.parents:
        raise ValueError('Game executable must be outside the development packaging directory')
    config = dict(repository=str(Path(root).resolve()), gameExecutable=str(game))
    if initial_project is not None:
        project = Path(initial_project).expanduser().resolve()
        if not project.is_file():
            raise ValueError('Initial project does not exist')
        config['initialProject'] = str(project)
    return config


def bundle_info():
    return dict(CFBundleName='EOD Creation Kit Development',
                CFBundleDisplayName='EOD Creation Kit Development',
                CFBundleIdentifier='org.eod-recomp.creationkit.development',
                CFBundleExecutable='EODModdingSuite', CFBundlePackageType='APPL',
                CFBundleIconFile='AppIcon',
                CFBundleShortVersionString='0.5.1', CFBundleVersion='9',
                LSMinimumSystemVersion='14.0', NSHighResolutionCapable=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-executable', required=True, type=Path)
    parser.add_argument('--project', type=Path, help='Project to open on first launch')
    args = parser.parse_args()
    layout = build_layout(ROOT)
    config = runtime_config(ROOT, args.game_executable, args.project)
    command = ['swift', 'build', '--build-system', 'native', '--package-path', str(PACKAGE),
               '--scratch-path', str(layout['scratch']), '-c', 'release',
               '-Xswiftc', '-file-prefix-map', '-Xswiftc', str(Path.home()) + '=/build',
               '-Xswiftc', '-debug-prefix-map', '-Xswiftc', str(Path.home()) + '=/build']
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
    subprocess.run(['xcrun', 'strip', '-S', str(executable / 'EODModdingSuite')], check=True)
    for name in ('modding_backend.py', 'rst0.py', 'valt.py', 'tex.py', 'texture_preview.py', 'texture_edit.py', 'vehicle_materials.py', 'vif_mesh.py',
                 'texture_export.py', 'asset_registry.py', 'texture_identity.py', 'mod_package.py', 'mod_install.py'):
        shutil.copy2(ROOT / 'tools' / name, backend / name)
    # Labelled format views (docs/FORMAT_VIEW_CONTRACT.md); decoders are self-contained.
    shutil.copytree(ROOT / 'tools' / 'formats', backend / 'formats',
                    ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    # Persistent backend service and MCP server (docs/KIT_SERVICE.md, docs/MCP_SERVER.md).
    for package in ('kit_service', 'eod_mcp'):
        shutil.copytree(ROOT / 'tools' / package, backend / package,
                        ignore=shutil.ignore_patterns('__pycache__', '*.pyc', 'tests'))
    (resources / 'studio-runtime.json').write_text(json.dumps(config))
    # Reuse the repository's existing artwork and standard macOS icon tooling.
    icon = ROOT / 'tools/macos/assets/AppIcon.png'
    with tempfile.TemporaryDirectory(prefix='eod-kit-icon-') as folder:
        iconset = Path(folder) / 'AppIcon.iconset'
        iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                suffix = '@2x' if scale == 2 else ''
                subprocess.run(['sips', '-z', str(size*scale), str(size*scale), str(icon),
                    '--out', str(iconset / f'icon_{size}x{size}{suffix}.png')], check=True, stdout=subprocess.DEVNULL)
        subprocess.run(['iconutil', '-c', 'icns', str(iconset), '-o', str(resources / 'AppIcon.icns')], check=True)
    with (stage / 'Contents/Info.plist').open('wb') as handle:
        plistlib.dump(bundle_info(), handle)
    subprocess.run(['xattr', '-cr', str(stage)], check=True)
    subprocess.run(['codesign', '--force', '--sign', '-', str(stage)], check=True)
    subprocess.run(['codesign', '--verify', '--strict', str(stage)], check=True)
    legacy_previous = layout['output'] / 'EOD Creation Kit Development.previous.app'
    if legacy_previous.exists():
        if layout['previous'].exists():
            shutil.rmtree(layout['previous'])
        legacy_previous.rename(layout['previous'])
    if layout['previous'].exists():
        shutil.rmtree(layout['previous'])
    if layout['app'].exists():
        layout['app'].rename(layout['previous'])
    stage.rename(layout['app'])
    print(layout['app'])


if __name__ == '__main__':
    main()
