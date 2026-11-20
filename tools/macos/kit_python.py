"""Embed the python.org CPython runtime for the native Creation Kit backend."""
import os
from pathlib import Path
import shutil
import subprocess


DEFAULT_FRAMEWORK = Path('/Library/Frameworks/Python.framework/Versions/Current')


def run(*args):
    return subprocess.check_output(args, text=True)


def install_python(source, contents):
    source = source.expanduser().resolve()
    version = source.name
    library = source / 'Python'
    headers = source / 'include' / f'python{version}'
    standard_library = source / 'lib' / f'python{version}'
    info = source / 'Resources/Info.plist'
    version_parts = version.split('.')
    if (len(version_parts) != 2 or version_parts[0] != '3'
            or not version_parts[1].isdigit() or int(version_parts[1]) < 10
            or not all(path.is_file() for path in (
                library, headers / 'Python.h', standard_library / 'LICENSE.txt', info))):
        raise ValueError('Use an arm64-capable python.org Python 3.10 or newer '
                         'framework version directory with --python-framework')
    if 'arm64' not in run('lipo', '-archs', library).split():
        raise ValueError(f'Python framework lacks an arm64 build: {library}')

    framework = contents / 'Frameworks/Python.framework'
    target = framework / 'Versions' / version
    resources = target / 'Resources'
    resources.mkdir(parents=True)
    shutil.copyfile(info, resources / 'Info.plist')
    shutil.copyfile(standard_library / 'LICENSE.txt', resources / 'LICENSE.txt')
    (framework / 'Versions/Current').symlink_to(version)
    (framework / 'Resources').symlink_to('Versions/Current/Resources')
    (framework / 'Python').symlink_to('Versions/Current/Python')

    # These optional modules use the upstream install's Tcl, curses or OpenSSL.
    # The kit uses the standard library's built-in hash implementations.
    target_library = target / 'lib' / standard_library.name
    shutil.copytree(standard_library, target_library, ignore=shutil.ignore_patterns(
        '__pycache__', '*.pyc', 'site-packages', 'config-*', 'test', 'tests',
        'idlelib', 'tkinter', 'turtledemo', 'ensurepip', 'lib-dynload'))

    extension_dir = target_library / 'lib-dynload'
    extension_dir.mkdir()
    optional = {'_tkinter', '_curses', '_curses_panel', '_ssl', '_hashlib'}
    extensions = []
    for extension in sorted((standard_library / 'lib-dynload').glob('*.so')):
        if extension.name.split('.')[0] in optional or extension.name.startswith(('_test', 'xx')):
            continue
        dependencies = run('otool', '-arch', 'arm64', '-L', extension).splitlines()[1:]
        if any(not dep.lstrip().startswith(('/usr/lib/', '/System/')) for dep in dependencies):
            raise ValueError(f'Python extension needs a non-system library: {extension}')
        destination = extension_dir / extension.name
        shutil.copyfile(extension, destination)
        extensions.append(destination)

    embedded_library = target / 'Python'
    shutil.copyfile(library, embedded_library)
    relative_library = f'Python.framework/Versions/{version}/Python'
    install_name = run('otool', '-arch', 'arm64', '-D', library).splitlines()[1].strip()
    subprocess.run(['install_name_tool', '-id', '@rpath/' + relative_library,
                    embedded_library], check=True)

    executable = contents / 'MacOS/eod-kit-python'
    subprocess.run(['xcrun', 'clang', '-arch', 'arm64', '-mmacosx-version-min=14.0',
                    '-I', headers, Path(__file__).with_suffix('.c'), library,
                    '-Wl,-headerpad_max_install_names', '-o', executable], check=True)
    subprocess.run(['install_name_tool', '-change', install_name,
                    '@executable_path/../Frameworks/' + relative_library, executable], check=True)
    subprocess.run(['xattr', '-cr', contents], check=True)
    if extensions:
        subprocess.run(['codesign', '--force', '--sign', '-', *extensions],
                       stdout=subprocess.DEVNULL, check=True)
    for code in (framework, executable):
        subprocess.run(['codesign', '--force', '--sign', '-', code], check=True)

    # Match a Finder launch and ensure CPython loads its standard library in place.
    subprocess.run([executable, '-c',
                    'import bz2, ctypes, hashlib, lzma, pathlib, sqlite3, sys, zlib; '
                    'assert pathlib.Path(sys.prefix) == pathlib.Path(sys.argv[1]); '
                    'assert sys.flags.isolated and sys.dont_write_bytecode',
                    target], env={'PATH': os.defpath}, check=True)
