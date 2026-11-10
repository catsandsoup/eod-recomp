#!/usr/bin/env python3
"""Record or verify SHA-256 hashes of files that development work must not change.

    protected_state.py record OUT.json PATH...   # files or directories (recursive)
    protected_state.py verify OUT.json           # exit 1 and list any difference

Symbolic links are recorded by target text, never followed. Nothing is modified.
"""
import hashlib
import json
import os
import sys
from pathlib import Path


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def walk(root):
    root = Path(root).expanduser()
    if root.is_symlink():
        yield str(root), "symlink:" + os.readlink(root)
        return
    if root.is_file():
        yield str(root), digest(root)
        return
    for folder, dirs, files in os.walk(root, followlinks=False):
        dirs.sort()
        for name in sorted(files) + [d for d in dirs if os.path.islink(os.path.join(folder, d))]:
            path = Path(folder) / name
            yield str(path), ("symlink:" + os.readlink(path)) if path.is_symlink() else digest(path)


def main(argv):
    if len(argv) >= 3 and argv[0] == "record":
        roots = [str(Path(p).expanduser()) for p in argv[2:]]
        missing = [p for p in roots if not os.path.lexists(p)]
        files = {path: value for root in roots if os.path.lexists(root) for path, value in walk(root)}
        Path(argv[1]).parent.mkdir(parents=True, exist_ok=True)
        Path(argv[1]).write_text(json.dumps(dict(roots=roots, missingRoots=missing, files=files), indent=1) + "\n")
        print(f"recorded {len(files)} files from {len(roots) - len(missing)} roots; missing roots: {missing}")
        return 0
    if len(argv) == 2 and argv[0] == "verify":
        record = json.loads(Path(argv[1]).read_text())
        now = {path: value for root in record["roots"] if os.path.lexists(root) for path, value in walk(root)}
        changed = sorted(p for p in record["files"] if now.get(p) != record["files"][p])
        added = sorted(set(now) - set(record["files"]))
        for p in changed:
            print("CHANGED" if p in now else "MISSING", p)
        for p in added:
            print("ADDED", p)
        print(f"{len(record['files'])} recorded, {len(changed)} changed/missing, {len(added)} added")
        return 1 if changed or added else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
