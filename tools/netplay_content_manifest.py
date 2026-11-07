"""Build a location-independent compatibility manifest for effective TDEOD data.

Offline packaging helper, not a networking handshake or trust signature. Match
the streamer's DATA overlay precedence. Runtime/protocol/config compatibility
must be checked separately. Reject ambiguous trees instead of blessing content
whose resolution can differ between case-sensitive and insensitive machines.
"""
import argparse
import hashlib
import json
from pathlib import Path


def _inventory(root):
    root = Path(root)
    if not root.is_dir():
        raise ValueError(f"Missing content directory: {root}")
    entries = {}

    def visit(directory, prefix):
        for child in sorted(directory.iterdir()):
            if child.is_symlink():
                raise ValueError(f"Content symlink is not supported: {child}")
            if not child.name.isascii():
                raise ValueError(f"Non-ASCII game resource name: {child.name}")
            key = prefix + child.name.upper()
            if key in entries:
                raise ValueError(f"Case-insensitive content collision: {key}")
            if child.is_dir():
                entries[key] = None
                visit(child, key + '/')
            elif child.is_file():
                entries[key] = child
            else:
                raise ValueError(f"Unsupported content entry: {child}")
    visit(root, '')
    return entries


def _data_tree(root):
    root = Path(root)
    if not root.is_dir():
        raise ValueError(f"Missing game root: {root}")
    matches = [p for p in root.iterdir() if p.name.upper() == 'DATA']
    if len(matches) != 1 or matches[0].is_symlink():
        raise ValueError('Expected one unambiguous DATA directory')
    return _inventory(matches[0])


def build_manifest(disc, overlay=None):
    effective = _data_tree(disc)
    if overlay is not None:
        additions = _data_tree(overlay)
        for key, value in additions.items():
            if key in effective and (value is None) != (effective[key] is None):
                raise ValueError(f"Overlay file/directory collision: {key}")
        effective.update(additions)
    files = []
    for key, path in sorted(effective.items()):
        if path is None:
            continue
        before = path.stat()
        digest = hashlib.sha256()
        size = 0
        with path.open('rb') as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b''):
                digest.update(chunk)
                size += len(chunk)
        after = path.stat()
        if (before.st_size, before.st_mtime_ns, before.st_ino) != (
                after.st_size, after.st_mtime_ns, after.st_ino) or size != after.st_size:
            raise ValueError(f"Content changed while hashing: {key}")
        files.append({'path': 'DATA/' + key, 'size': size, 'sha256': digest.hexdigest()})
    if not files:
        raise ValueError('Empty game content cannot identify a playable installation')
    body = {'schema': 1, 'scope': 'effective-data', 'files': files}
    encoded = json.dumps(body, sort_keys=True, separators=(',', ':')).encode('ascii')
    return {**body, 'content_sha256': hashlib.sha256(encoded).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disc', required=True)
    parser.add_argument('--overlay')
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    result = build_manifest(args.disc, args.overlay)
    # Never replace existing data (including an accidental asset output path).
    with Path(args.output).open('x') as output:
        json.dump(result, output, indent=2)
        output.write('\n')


if __name__ == '__main__':
    main()
