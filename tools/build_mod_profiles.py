#!/usr/bin/env python3
"""Compose Phantom and Battle overlays without losing either COMMON.RES change.

COMMON.RES entries use a strict three-way merge against the installed retail
archive. This does not merge arbitrary VALT records inside a conflicting entry.
Publication is an atomic, exclusive directory rename; existing profiles survive.
"""
import argparse
import ctypes
import errno
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile

from rst0 import unpack_archive, pack_archive


DERIVED_ENTRY_FIELDS = {"offset", "orig_offset", "size", "orig_size", "crc"}
HEADER_FIELDS = ("version", "build_time", "unk")


def _data_root(root):
    matches = [p for p in root.iterdir() if p.name.casefold() == "data"]
    if len(matches) != 1 or not matches[0].is_dir() or matches[0].is_symlink():
        raise ValueError(f"Expected one ordinary DATA directory in {root}")
    return matches[0]


def _inventory(root):
    data = _data_root(root)
    result = {}
    for path in data.rglob("*"):
        if path.is_symlink():
            raise ValueError(f"Profile symlink is unsupported: {path}")
        if not path.is_file() or path.suffix.lower() == ".json":
            continue  # reports are profile metadata, never runtime DATA files
        relative = path.relative_to(data)
        key = relative.as_posix().casefold()
        if key in result:
            raise ValueError(f"Case-colliding profile files: {relative}")
        result[key] = (relative, path)
    return result


def _find_case(root, relative):
    path = root
    for part in relative.parts:
        matches = [p for p in path.iterdir() if p.name.casefold() == part.casefold()]
        if len(matches) != 1 or matches[0].is_symlink():
            raise ValueError(f"Missing or ambiguous retail baseline: {relative}")
        path = matches[0]
    if not path.is_file():
        raise ValueError(f"Retail baseline is not a file: {relative}")
    return path


def _archive(path, directory):
    unpack_archive(str(path), str(directory))
    manifest = json.loads((directory / "manifest.json").read_text())
    if manifest.get("version") != 6:
        raise ValueError(f"Unsupported COMMON.RES version in {path}")
    entries = {}
    for entry in manifest["entries"]:
        key = entry["name"].casefold()
        if key in entries:
            raise ValueError(f"Duplicate COMMON.RES entry: {entry['name']}")
        metadata = {k: v for k, v in entry.items() if k not in DERIVED_ENTRY_FIELDS}
        entries[key] = (metadata, (directory / entry["name"]).read_bytes())
    return manifest, entries


def _merge_common(baseline, phantom, battle, target, scratch):
    loaded = [_archive(path, scratch / label) for label, path in
              (("retail", baseline), ("phantom", phantom), ("battle", battle))]
    header = {key: loaded[0][0].get(key) for key in HEADER_FIELDS}
    for manifest, _ in loaded[1:]:
        if {key: manifest.get(key) for key in HEADER_FIELDS} != header:
            raise ValueError("Incompatible COMMON.RES header metadata")
    base, left, right = [entries for _, entries in loaded]
    # Preserve retail ordering, then deterministic new resources from either side.
    keys = list(base) + sorted((left.keys() | right.keys()) - base.keys())
    merged = scratch / "merged"
    merged.mkdir()
    manifest = dict(header, entries=[])
    actions = []
    for key in keys:
        original, a, b = base.get(key), left.get(key), right.get(key)
        if a == b:
            chosen, source = a, "identical" if a != original else "retail"
        elif a == original:
            chosen, source = b, "battle"
        elif b == original:
            chosen, source = a, "phantom"
        else:
            raise ValueError(f"Conflicting COMMON.RES entry (payload or metadata): {key}")
        actions.append(dict(entry=key, source=source, deleted=chosen is None))
        if chosen is not None:
            metadata, payload = chosen
            manifest["entries"].append(metadata)
            (merged / metadata["name"]).write_bytes(payload)
    (merged / "manifest.json").write_text(json.dumps(manifest, indent=2))
    pack_archive(str(merged), str(target))
    return actions


def _publish_exclusively(staging, output):
    """Atomic publication that cannot replace a concurrently created directory."""
    if os.name == "nt":
        os.rename(staging, output)  # Windows rename refuses an existing target.
        return
    libc = ctypes.CDLL(None, use_errno=True)
    if sys.platform == "darwin":
        rename = libc.renamex_np
        rename.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
        result = rename(os.fsencode(staging), os.fsencode(output), 0x00000004)  # RENAME_EXCL (macOS SDK sys/stdio.h)
    elif sys.platform.startswith("linux") and hasattr(libc, "renameat2"):
        rename = libc.renameat2
        rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
        result = rename(-100, os.fsencode(staging), -100, os.fsencode(output), 1)  # AT_FDCWD, RENAME_NOREPLACE
    else:
        raise OSError(errno.ENOTSUP, "Exclusive atomic directory publication unsupported on this platform")
    if result:
        error = ctypes.get_errno()
        if error in (errno.EEXIST, errno.ENOTEMPTY):
            raise FileExistsError(error, "Output profile already exists", str(output))
        raise OSError(error, os.strerror(error), str(output))


def build_combined_profile(disc, phantom, battle, output):
    """Return a composition report after publishing a new complete profile."""
    disc, phantom, battle, output = [Path(p).expanduser().resolve() for p in (disc, phantom, battle, output)]
    sources = (disc, phantom, battle)
    for source in sources:
        if not source.is_dir():
            raise ValueError(f"Source directory not found: {source}")
    for i, path in enumerate((*sources, output)):
        for other in (*sources, output)[i + 1:]:
            if path == other or path in other.parents or other in path.parents:
                raise ValueError("Source and output directories must be distinct and must not nest")
    if output.exists():
        raise FileExistsError(f"Output profile already exists: {output}")
    profiles = [_inventory(root) for root in (phantom, battle)]
    if not all(profiles):
        raise ValueError("Both source profiles must contain runtime DATA files")
    output.parent.mkdir(parents=True, exist_ok=True)
    report = dict(profile="combined", sources=dict(disc=str(disc), phantom=str(phantom), battle=str(battle)),
                  files=[], common_entries=[])
    with tempfile.TemporaryDirectory(prefix=".combined-", dir=output.parent) as temporary:
        scratch = Path(temporary)
        staged = scratch / "profile"
        staged.mkdir()
        left, right = profiles
        for key in sorted(left.keys() | right.keys()):
            a, b = left.get(key), right.get(key)
            relative = (a or b)[0]
            target = staged / "DATA" / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            if key == "res/common.res":
                baseline = _find_case(_data_root(disc), Path("RES/COMMON.RES"))
                report["common_entries"] = _merge_common(baseline, a[1] if a else baseline,
                                                           b[1] if b else baseline, target, scratch)
                source = "entry-merge"
            elif a and b:
                if a[1].read_bytes() != b[1].read_bytes():
                    raise ValueError(f"Conflicting profile file: DATA/{relative}")
                shutil.copy2(a[1], target)
                source = "identical"
            else:
                shutil.copy2((a or b)[1], target)
                source = "phantom" if a else "battle"
            report["files"].append(dict(path="DATA/" + relative.as_posix(), source=source,
                                        sha256=hashlib.sha256(target.read_bytes()).hexdigest()))
        (staged / "composition-report.json").write_text(json.dumps(report, indent=2) + "\n")
        _publish_exclusively(staged, output)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disc", required=True, help="Installed retail disc root containing DATA")
    parser.add_argument("--phantom", required=True, help="Existing Phantom profile containing DATA")
    parser.add_argument("--battle", required=True, help="Existing Battle profile containing DATA")
    parser.add_argument("--output", required=True, help="Fresh combined profile directory; must not exist")
    args = parser.parse_args()
    try:
        report = build_combined_profile(args.disc, args.phantom, args.battle, args.output)
    except (ValueError, OSError) as error:
        parser.exit(1, f"Profile composition failed: {error}\n")
    print(f"Created combined profile with {len(report['files'])} runtime files: {Path(args.output).resolve()}")


if __name__ == "__main__":
    main()
