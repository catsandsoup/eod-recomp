#!/usr/bin/env python3
"""Package the development Kit and an isolated runtime without disc/project data.

Reuses the existing macOS runtime bundler for its dylib closure and licenses.
Does not publish, change the accepted app, or include an extracted game.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile

from macos.kit_python import DEFAULT_FRAMEWORK

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-executable', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--python-framework', type=Path, default=DEFAULT_FRAMEWORK,
                        help='arm64 python.org CPython framework version directory')
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        raise ValueError('Choose a new release staging directory')
    env = {k: v for k, v in os.environ.items() if not k.startswith('EOD_')}
    env.update(EOD_HEADLESS='1', EOD_MUTE='1')
    subprocess.run([sys.executable, str(ROOT/'tools/build_modding_app.py'),
                    '--game-executable', str(args.game_executable.resolve()),
                    '--python-framework', str(args.python_framework)], check=True, env=env)
    output.mkdir(parents=True)
    app = output/'EOD Creation Kit Development.app'
    shutil.copytree(ROOT/'build/creation-kit'/app.name, app, symlinks=True)
    resources = app/'Contents/Resources'
    runtime = resources/'GameRuntime.bundle'
    subprocess.run([sys.executable, str(ROOT/'tools/macos/bundle.py'), str(runtime),
                    str(args.game_executable.resolve())], check=True, env=env)
    (resources/'studio-runtime.json').write_text(json.dumps(dict(
        portable='1', bundledGameExecutable='GameRuntime.bundle/Contents/MacOS/eod'))+'\n')
    info_path = app/'Contents/Info.plist'
    info = plistlib.loads(info_path.read_bytes())
    game_info = plistlib.loads((runtime/'Contents/Info.plist').read_bytes())
    info['LSMinimumSystemVersion'] = max(
        (info['LSMinimumSystemVersion'], game_info['LSMinimumSystemVersion']),
        key=lambda v: tuple(map(int, v.split('.'))))
    info_path.write_bytes(plistlib.dumps(info))
    # Reject accidentally included retail or personal data before publication.
    for path in app.rglob('*'):
        if path.name in ('DATA', 'Mods', 'SLUS_209.10') or path.suffix in ('.eodproject', '.CAR', '.TRK', '.RES'):
            raise ValueError('Unexpected game/project data: '+str(path))
    # Fail before publishing if any bundled file embeds this builder's home path.
    private_prefix = str(Path.home()).encode()
    for path in app.rglob('*'):
        if path.is_file() and private_prefix in path.read_bytes():
            raise ValueError('Bundle contains a private build path: '+str(path.relative_to(app)))
    subprocess.run(['xattr', '-cr', str(app)], check=True)
    subprocess.run(['xattr', '-d', 'com.apple.FinderInfo', str(app)],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(['codesign', '--force', '--sign', '-', str(app)], check=True)
    subprocess.run(['codesign', '--verify', '--deep', '--strict', str(app)], check=True)
    readme = (ROOT/'docs/CREATION_KIT_RELEASE.md').read_text()
    readme = readme.replace('<!-- macOS minimum -->',
                            f'This build requires macOS {info["LSMinimumSystemVersion"]} or later.\n')
    (output/'README.md').write_text(readme)
    archive = output/'EOD-Creation-Kit-Development-macOS-arm64.zip'
    with tempfile.TemporaryDirectory(prefix='eod-kit-archive-', dir=output.parent) as scratch:
        staging_archive = Path(scratch) / archive.name
        subprocess.run(['ditto', '-c', '-k', '--norsrc', str(output), str(staging_archive)], check=True)
        staging_archive.rename(archive)
    (output/'SHA256SUMS-Creation-Kit.txt').write_text(hashlib.sha256(archive.read_bytes()).hexdigest()+'  '+archive.name+'\n')
    print(archive)


if __name__ == '__main__':
    main()
