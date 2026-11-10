#!/usr/bin/env python3
"""EOD mod packages (docs/CREATION_KIT_TEXTURE_CONTRACTS.md section 5).

A package is a directory (or `.eodmod` zip of it) holding `eodmod.json`, native
member payloads under `native/<archive>/<member>` and host replacement PNGs
under `host/`. Library functions return dicts and raise ValueError with
user-facing messages; `main()` is the CLI.
"""
import argparse
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import sys
import tempfile
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rst0 import eod_crc, pack_archive, unpack_archive
from modding_backend import atomic_write, directory, payload

SCHEMA = 1
MANIFEST = "eodmod.json"
ID_RE = re.compile(r"[a-z0-9][a-z0-9._-]{0,63}")
SEMVER_RE = re.compile(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
                       r"(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z.-]+)?")
HEX64 = re.compile(r"[0-9a-f]{64}")
HEX16 = re.compile(r"[0-9a-f]{16}")
MAX_SIDE = 4096
MAX_UNZIPPED = 4 << 30


def sha(data):
    return hashlib.sha256(data).hexdigest()


def file_sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


_SHA_CACHE = {}


def cached_file_sha(path):
    st = os.stat(path)
    key = (str(path), st.st_mtime_ns, st.st_size)
    if key not in _SHA_CACHE:
        _SHA_CACHE[key] = file_sha(path)
    return _SHA_CACHE[key]


def semver_key(version):
    match = SEMVER_RE.fullmatch(version or "")
    if not match:
        raise ValueError(f"Version must be semantic (for example 1.0.0): {version!r}")
    pre = match.group(4)
    pre_key = (1,) if pre is None else (0,) + tuple(
        (0, int(p), "") if p.isdigit() else (1, 0, p) for p in pre.split("."))
    return tuple(int(match.group(i)) for i in (1, 2, 3)) + (pre_key,)


def check_rel(path, what="Package path"):
    """Relative POSIX path with no traversal, drive, control or empty segments."""
    if not isinstance(path, str) or not path or path.startswith("/") or "\\" in path or ":" in path:
        raise ValueError(f"{what} must be a relative POSIX path: {path!r}")
    if any(ord(c) < 32 or ord(c) == 127 for c in path):
        raise ValueError(f"{what} contains a control character: {path!r}")
    if any(part in ("", ".", "..") for part in path.split("/")):
        raise ValueError(f"{what} contains an empty, '.' or '..' segment: {path!r}")
    return path


def split_asset(asset_id):
    archive, sep, member = (asset_id or "").rpartition("#")
    if not sep or not archive or not member or "/" in member:
        raise ValueError(f"Asset must be ARCHIVE#MEMBER: {asset_id!r}")
    check_rel(archive, "Archive")
    return archive, member


# ---------------------------------------------------------------- retail disc

def disc_root(disc):
    root = Path(disc).expanduser().resolve()
    if not (root / "DATA").is_dir() or not (root / "SLUS_209.10").is_file():
        raise ValueError("Select the extracted game folder containing DATA and SLUS_209.10")
    return root


def slus_sha(disc):
    return cached_file_sha(disc_root(disc) / "SLUS_209.10")


def retail_archive(disc, archive):
    """(path, disc spelling) for a disc-relative archive, matched case-insensitively."""
    root = disc_root(disc)
    check_rel(archive, "Archive")
    current, spelled = root, []
    for part in archive.split("/"):
        if not current.is_dir():
            raise ValueError("Retail archive not found: " + archive)
        names = [p for p in current.iterdir() if p.name.lower() == part.lower()]
        exact = [p for p in names if p.name == part]
        if len(names) != 1 and not exact:
            raise ValueError("Retail archive not found: " + archive if not names else "Ambiguous retail path: " + archive)
        current = (exact or names)[0]
        spelled.append(current.name)
    resolved = current.resolve()
    if spelled[0].upper() != "DATA" or not resolved.is_file() or (root / spelled[0]).resolve() not in resolved.parents:
        raise ValueError("Archive must be a file inside the disc's DATA directory: " + archive)
    return resolved, "/".join(spelled)


def retail_member(disc, archive, member):
    """(bytes, entry, archive path, disc spelling) of a retail member, case-insensitively."""
    path, spelled = retail_archive(disc, archive)
    entries = directory(path)
    matches = [e for e in entries if e["name"].lower() == member.lower()]
    if len(matches) != 1:
        raise ValueError(f"Member not found in retail archive: {spelled}#{member}")
    data, entry = payload(path, matches[0]["name"], entries, verify_crc=False)
    return data, entry, path, spelled


def build_archive(source, changes):
    """Archive bytes = retail `source` with members replaced by `changes` {exact name: bytes}.

    Same-size replacements patch payload and directory CRC in place (every other
    byte kept, as modding_backend.save_single_archive); otherwise the retail
    archive is unpacked and repacked by rst0. Every non-replaced member is
    verified byte-identical and every replaced member exact with a valid CRC.
    """
    source = Path(source)
    original = source.read_bytes()
    entries = directory(source)
    originals = {e["name"]: payload(source, e["name"], entries, verify_crc=False)[0] for e in entries}
    unknown = set(changes) - set(originals)
    if unknown:
        raise ValueError("Replacement member is not in the retail archive: " + ", ".join(sorted(unknown)))
    with tempfile.TemporaryDirectory(prefix="eod-compose-") as temp:
        packed = Path(temp) / "packed"
        if all(len(data) == len(originals[name]) for name, data in changes.items()):
            built = bytearray(original)
            dir_offset = struct.unpack_from("<I", original, 20)[0]
            for index, entry in enumerate(entries):
                if entry["name"] in changes:
                    data = changes[entry["name"]]
                    built[entry["offset"]:entry["offset"] + len(data)] = data
                    struct.pack_into("<I", built, dir_offset + index * 68 + 48, eod_crc(data))
            packed.write_bytes(built)
            method = "in-place"
        else:
            stage = Path(temp) / "stage"
            with contextlib.redirect_stdout(io.StringIO()):
                unpack_archive(str(source), str(stage))
                for name, data in changes.items():
                    (stage / name).write_bytes(data)
                pack_archive(str(stage), str(packed))
            method = "repack"
        built_entries = directory(packed)
        if sorted(e["name"] for e in built_entries) != sorted(originals):
            raise ValueError("Built archive member inventory changed: " + source.name)
        for entry in built_entries:
            name = entry["name"]
            data, _ = payload(packed, name, built_entries, verify_crc=name in changes)
            if data != changes.get(name, originals[name]):
                raise ValueError("Built archive member differs from its source: " + name)
        return packed.read_bytes(), method


# ---------------------------------------------------------------- PNG / identity

def png_size(path):
    with open(path, "rb") as handle:
        head = handle.read(33)
    if len(head) < 24 or head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        raise ValueError(f"Not a PNG image: {Path(path).name}")
    return struct.unpack(">II", head[16:24])


def replacement_scale(width, height, orig_width, orig_height):
    k = width // orig_width if orig_width else 0
    if (k < 1 or k & (k - 1) or width != orig_width * k or height != orig_height * k
            or width > MAX_SIDE or height > MAX_SIDE):
        raise ValueError(f"Replacement must be {orig_width}x{orig_height} times a power of two "
                         f"(at most {MAX_SIDE} per side); got {width}x{height}")
    return k


def host_identity(data, asset):
    from texture_identity import texture_identity
    if len(data) < 32 or struct.unpack_from("<I", data, 12)[0] & 255 in (2, 3):
        raise ValueError(f"{asset} is a direct-colour texture; host replacement v1 supports indexed 4/8-bit TEX only")
    try:
        return texture_identity(data)
    except (ValueError, IndexError, struct.error) as error:
        raise ValueError(f"{asset} has no runtime texture identity: {error}") from None


# ---------------------------------------------------------------- manifest

def _require(record, key, kind, where):
    value = record.get(key) if isinstance(record, dict) else None
    if not isinstance(value, kind) or isinstance(value, bool) and kind is not bool:
        raise ValueError(f"{where}: '{key}' is missing or has the wrong type")
    return value


def _hex(record, key, pattern, where):
    value = _require(record, key, str, where)
    if not pattern.fullmatch(value):
        raise ValueError(f"{where}: '{key}' must be lowercase hex")
    return value


def validate_manifest(m):
    """Schema check of a parsed eodmod.json. Returns the list of payload files."""
    if not isinstance(m, dict) or m.get("schemaVersion") != SCHEMA:
        raise ValueError("Not an EOD mod package (eodmod.json schemaVersion 1)")
    if not ID_RE.fullmatch(_require(m, "id", str, MANIFEST)):
        raise ValueError("Package id must match [a-z0-9][a-z0-9._-]{0,63}")
    semver_key(_require(m, "version", str, MANIFEST))
    if not _require(m, "name", str, MANIFEST).strip():
        raise ValueError("Package name is empty")
    _hex(_require(m, "game", dict, MANIFEST), "slusSha256", HEX64, "game")
    seen_deps = set()
    for dep in _require(m, "dependencies", list, MANIFEST):
        dep_id = _require(dep, "id", str, "dependency")
        semver_key(_require(dep, "minVersion", str, "dependency"))
        if not ID_RE.fullmatch(dep_id) or dep_id == m["id"] or dep_id in seen_deps:
            raise ValueError(f"Invalid or duplicate dependency: {dep_id}")
        seen_deps.add(dep_id)
    files, assets = [], set()
    for entry in _require(m, "native", list, MANIFEST):
        where = "native entry"
        archive = check_rel(_require(entry, "archive", str, where), "Archive")
        member = _require(entry, "member", str, where)
        check_rel(member, "Member")
        if "/" in member or archive.split("/")[0].upper() != "DATA":
            raise ValueError(f"Native entry must name a DATA archive member: {archive}#{member}")
        for key in ("sourceArchiveSha256", "sourceMemberSha256", "sha256"):
            _hex(entry, key, HEX64, where)
        key = (archive.lower(), member.lower())
        if key in assets:
            raise ValueError(f"Duplicate native entry: {archive}#{member}")
        assets.add(key)
        files.append((check_rel(_require(entry, "file", str, where)), entry["sha256"], "native/"))
    host_ids = set()
    for entry in _require(m, "host", list, MANIFEST):
        where = "host entry"
        asset = _require(entry, "assetId", str, where)
        archive, member = split_asset(asset)
        if asset.lower() in host_ids:
            raise ValueError("Duplicate host entry: " + asset)
        host_ids.add(asset.lower())
        _hex(entry, "sourceMemberSha256", HEX64, where)
        _hex(entry, "sha256", HEX64, where)
        _hex(entry, "indexKey", HEX16, where)
        if entry.get("paletteKey") != "*":
            _hex(entry, "paletteKey", HEX16, where)
        dims = [_require(entry, k, int, where) for k in ("origWidth", "origHeight", "width", "height")]
        if min(dims) < 1:
            raise ValueError(f"{asset}: dimensions must be positive")
        replacement_scale(dims[2], dims[3], dims[0], dims[1])
        if any(c in asset for c in "\t\n\r"):
            raise ValueError("Asset id contains a tab or newline: " + asset)
        files.append((check_rel(_require(entry, "file", str, where)), entry["sha256"], "host/"))
    folded = set()
    for path, _, prefix in files:
        if not path.startswith(prefix) or (prefix == "host/" and not path.lower().endswith(".png")):
            raise ValueError(f"Package file must be under {prefix}: {path}")
        if path.lower() in folded:
            raise ValueError("Duplicate or case-colliding package file: " + path)
        folded.add(path.lower())
    return files


def scan_tree(root):
    """Relative POSIX files of a package directory; rejects links, specials and case collisions."""
    root = Path(root)
    if root.is_symlink() or not root.is_dir():
        raise ValueError(f"Package folder not found: {root}")
    files, folded = [], {}
    for current, dirs, names in os.walk(root, followlinks=False):
        for name in dirs + names:
            path = Path(current) / name
            rel = path.relative_to(root).as_posix()
            check_rel(rel)
            mode = os.lstat(path).st_mode
            if stat.S_ISLNK(mode):
                raise ValueError("Package contains a symbolic link: " + rel)
            if not (stat.S_ISDIR(mode) or stat.S_ISREG(mode)):
                raise ValueError("Package contains a special file: " + rel)
            if rel.lower() in folded:
                raise ValueError(f"Package paths collide by case: {folded[rel.lower()]} / {rel}")
            folded[rel.lower()] = rel
            if stat.S_ISREG(mode):
                files.append(rel)
    return sorted(files)


def load_manifest(package):
    path = Path(package) / MANIFEST
    if path.is_symlink() or not path.is_file():
        raise ValueError("Package has no eodmod.json")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(f"eodmod.json is not valid JSON: {error}") from None


def verify(package, disc=None):
    """Verify schema, path safety and payload hashes; with `disc`, also game and source hashes."""
    package = Path(package)
    if package.is_file():
        with tempfile.TemporaryDirectory(prefix="eod-verify-") as temp:
            return verify(unzip(package, Path(temp) / "pkg")["path"], disc)
    present = scan_tree(package)
    m = load_manifest(package)
    listed = validate_manifest(m)
    extra = set(present) - {p for p, _, _ in listed} - {MANIFEST}
    if extra:
        raise ValueError("Package contains unlisted files: " + ", ".join(sorted(extra)))
    for path, digest, _ in listed:
        if path not in present:
            raise ValueError("Package file missing: " + path)
        if file_sha(package / path) != digest:
            raise ValueError("Package file hash mismatch (modified or corrupt): " + path)
    for entry in m["host"]:
        if png_size(package / entry["file"]) != (entry["width"], entry["height"]):
            raise ValueError(f"{entry['assetId']}: PNG size differs from eodmod.json")
    result = dict(id=m["id"], version=m["version"], name=m["name"], native=len(m["native"]),
                  host=len(m["host"]), dependencies=m["dependencies"], manifestSha256=file_sha(package / MANIFEST),
                  files=sorted(p for p, _, _ in listed) + [MANIFEST], path=str(package))
    if disc is not None:
        result.update(verify_sources(m, disc))
    return result


def verify_sources(m, disc):
    """Refuse a package built for another game release or other retail archive versions."""
    if slus_sha(disc) != m["game"]["slusSha256"]:
        raise ValueError(f"Package {m['id']} was built for a different game executable (SLUS_209.10 hash differs)")
    archives = {}
    for entry in m["native"]:
        path, spelled = retail_archive(disc, entry["archive"])
        if cached_file_sha(path) != entry["sourceArchiveSha256"]:
            raise ValueError(f"Package {m['id']} targets a different version of {spelled}")
        data = retail_member(disc, spelled, entry["member"])[0]
        if sha(data) != entry["sourceMemberSha256"]:
            raise ValueError(f"Package {m['id']} targets a different version of {spelled}#{entry['member']}")
        archives[spelled] = archives.get(spelled, 0) + 1
    for entry in m["host"]:
        archive, member = split_asset(entry["assetId"])
        data, _, _, spelled = retail_member(disc, archive, member)
        if sha(data) != entry["sourceMemberSha256"]:
            raise ValueError(f"Package {m['id']} targets a different version of {entry['assetId']}")
        ident = host_identity(data, entry["assetId"])
        if (ident["indexKey"], ident["width"], ident["height"]) != (entry["indexKey"], entry["origWidth"], entry["origHeight"]) \
                or entry["paletteKey"] not in ("*", ident["paletteKey"]):
            raise ValueError(f"{entry['assetId']}: runtime identity differs from the retail texture")
    return dict(gameCompatible=True, archives=archives)


def write_manifest(package, m):
    validate_manifest(m)
    atomic_write(Path(package) / MANIFEST, (json.dumps(m, indent=2, sort_keys=True) + "\n").encode())


# ---------------------------------------------------------------- builders

def new_manifest(disc, package_id, version, name=None):
    m = dict(schemaVersion=SCHEMA, id=package_id, version=version, name=name or package_id,
             game=dict(slusSha256=slus_sha(disc)), dependencies=[], native=[], host=[])
    validate_manifest(m)
    return m


def _fresh_output(out, disc):
    out = Path(out).expanduser().resolve()
    root = disc_root(disc)
    if out == root or root in out.parents or out in root.parents:
        raise ValueError("Choose a package folder separate from the game disc")
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        raise ValueError(f"Package folder already exists and is not empty: {out}")
    return out


def _publish_dir(stage, out):
    if out.exists():
        out.rmdir()
    os.replace(stage, out)


def init(out, disc, package_id, version, name=None):
    out = _fresh_output(out, disc)
    m = new_manifest(disc, package_id, version, name)
    out.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{out.name}.tmp-", dir=out.parent))
    try:
        write_manifest(stage, m)
        _publish_dir(stage, out)
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    return verify(out, disc)


def from_project(project, disc, package_id, version, out, name=None):
    """Package only the members a Kit project's built overlay changed versus retail."""
    project = Path(project).expanduser().resolve()
    out = _fresh_output(out, disc)
    try:
        data = json.loads(project.read_text())
    except (OSError, ValueError) as error:
        raise ValueError(f"Cannot read project: {error}") from None
    if data.get("version") != 1 or not isinstance(data.get("archives"), dict) or not data["archives"]:
        raise ValueError("Not a built EOD Kit project (version 1)")
    overlay = project.parent / (project.name + ".assets") / "overlay"
    m = new_manifest(disc, package_id, version, name)
    out.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{out.name}.tmp-", dir=out.parent))
    report = []
    try:
        for rel in sorted(data["archives"]):
            record = data["archives"][rel]
            source, spelled = retail_archive(disc, rel)
            source_sha = cached_file_sha(source)
            if source_sha != record.get("sourceSha256"):
                raise ValueError(f"Project was built from a different version of {spelled}")
            built = (overlay / rel).resolve()
            if overlay.resolve() not in built.parents or not built.is_file() or file_sha(built) != record.get("outputSha256"):
                raise ValueError(f"Built archive missing or modified; rebuild it in the Kit: {rel}")
            retail_entries, built_entries = directory(source), directory(built)
            if sorted(e["name"] for e in retail_entries) != sorted(e["name"] for e in built_entries):
                raise ValueError(f"Built archive adds or removes members (unsupported): {rel}")
            changes = {}
            for entry in retail_entries:
                old = payload(source, entry["name"], retail_entries, verify_crc=False)[0]
                new = payload(built, entry["name"], built_entries, verify_crc=False)[0]
                if old == new:
                    continue
                if eod_crc(new) != next(e["crc"] for e in built_entries if e["name"] == entry["name"]):
                    raise ValueError(f"Built member has a bad CRC: {rel}#{entry['name']}")
                changes[entry["name"]] = new
                file = f"native/{spelled}/{entry['name']}"
                (stage / file).parent.mkdir(parents=True, exist_ok=True)
                (stage / file).write_bytes(new)
                m["native"].append(dict(archive=spelled, member=entry["name"], sourceArchiveSha256=source_sha,
                                        sourceMemberSha256=sha(old), file=file, sha256=sha(new)))
            rebuilt = sha(build_archive(source, changes)[0]) if changes else source_sha
            report.append(dict(archive=spelled, members=sorted(changes), reproducesProjectBuild=rebuilt == record["outputSha256"]))
        if not m["native"]:
            raise ValueError("Project's built archives do not differ from retail")
        write_manifest(stage, m)
        verify(stage, disc)
        _publish_dir(stage, out)
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    result = verify(out, disc)
    result["archiveReport"] = report
    return result


def add_host(package, disc, asset, image, any_palette=False):
    """Add (or replace) a host replacement for an indexed retail texture."""
    package = Path(package).expanduser().resolve()
    verify(package)
    m = load_manifest(package)
    archive, member = split_asset(asset)
    data, entry, _, spelled = retail_member(disc, archive, member)
    if slus_sha(disc) != m["game"]["slusSha256"]:
        raise ValueError("Package was created for a different game executable")
    if entry["type"] != "TEX":
        raise ValueError(f"{spelled}#{entry['name']} is not a TEX texture")
    asset_id = f"{spelled}#{entry['name']}"
    ident = host_identity(data, asset_id)
    image = Path(image).expanduser().resolve()
    width, height = png_size(image)
    replacement_scale(width, height, ident["width"], ident["height"])
    from tex import read_png
    try:
        decoded = read_png(str(image))
    except (ValueError, OSError, struct.error) as error:
        raise ValueError(f"Cannot read replacement PNG: {error}") from None
    if tuple(decoded[:2]) != (width, height):
        raise ValueError("Replacement PNG header and pixel data disagree")
    file = f"host/{spelled}/{entry['name']}.png"
    old = [h for h in m["host"] if h["assetId"].lower() == asset_id.lower()]
    stale = {h["file"] for h in old} - {file}
    m["host"] = [h for h in m["host"] if h not in old]
    png = image.read_bytes()
    m["host"].append(dict(assetId=asset_id, sourceMemberSha256=sha(data), indexKey=ident["indexKey"],
                          paletteKey="*" if any_palette else ident["paletteKey"], origWidth=ident["width"],
                          origHeight=ident["height"], file=file, sha256=sha(png), width=width, height=height))
    m["host"].sort(key=lambda h: h["assetId"].lower())
    validate_manifest(m)
    atomic_write(package / file, png)
    write_manifest(package, m)
    for path in stale:
        (package / path).unlink(missing_ok=True)
    return verify(package, disc)


def inspect(package):
    package = Path(package)
    if package.is_file():
        with tempfile.TemporaryDirectory(prefix="eod-inspect-") as temp:
            result = inspect(unzip(package, Path(temp) / "pkg")["path"])
            result["path"] = str(package)
            return result
    m = load_manifest(package)
    validate_manifest(m)
    return dict(id=m["id"], version=m["version"], name=m["name"], game=m["game"], dependencies=m["dependencies"],
                native=[f"{e['archive']}#{e['member']}" for e in m["native"]],
                host=[dict(assetId=e["assetId"], indexKey=e["indexKey"], paletteKey=e["paletteKey"],
                           size=f"{e['width']}x{e['height']}") for e in m["host"]], path=str(package))


# ---------------------------------------------------------------- .eodmod zip

def zip_package(package, out):
    info = verify(package)
    out = Path(out).expanduser().resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    fd, temp = tempfile.mkstemp(prefix=".eodmod-", dir=out.parent)
    os.close(fd)
    try:
        with zipfile.ZipFile(temp, "w", zipfile.ZIP_DEFLATED) as archive:
            for rel in sorted(info["files"]):
                item = zipfile.ZipInfo(rel, (1980, 1, 1, 0, 0, 0))
                item.external_attr = (stat.S_IFREG | 0o644) << 16
                item.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(item, (Path(package) / rel).read_bytes())
        os.replace(temp, out)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)
    return dict(path=str(out), sha256=file_sha(out), files=len(info["files"]), id=info["id"], version=info["version"])


def unzip(source, out):
    """Extract a .eodmod safely (no traversal, links, case collisions or bombs), then verify."""
    out = Path(out).expanduser().resolve()
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        raise ValueError(f"Destination already exists and is not empty: {out}")
    try:
        archive = zipfile.ZipFile(source)
    except (OSError, zipfile.BadZipFile) as error:
        raise ValueError(f"Not a readable .eodmod archive: {error}") from None
    with archive:
        infos, folded, total = [], {}, 0
        for item in archive.infolist():
            name = item.filename
            if stat.S_ISLNK(item.external_attr >> 16):
                raise ValueError("Archive contains a symbolic link: " + name)
            if item.flag_bits & 1:
                raise ValueError("Encrypted archives are not supported")
            is_dir = name.endswith("/")
            parts = check_rel(name[:-1] if is_dir else name, "Archive entry").split("/")
            for depth in range(1, len(parts) + 1):
                prefix = "/".join(parts[:depth])
                kind = "file" if depth == len(parts) and not is_dir else "dir"
                previous = folded.setdefault(prefix.lower(), (prefix, kind, item))
                if previous[0] != prefix:
                    raise ValueError(f"Archive paths collide by case: {previous[0]} / {prefix}")
                if previous[1] != kind or (kind == "file" and previous[2] is not item):
                    raise ValueError(f"Archive path is duplicated or both a file and a folder: {prefix}")
            total += item.file_size
            if total > MAX_UNZIPPED:
                raise ValueError("Archive expands beyond the 4 GiB package limit")
            if not is_dir:
                infos.append(item)
        if MANIFEST not in {i.filename for i in infos}:
            raise ValueError("Archive has no eodmod.json at its top level")
        out.parent.mkdir(parents=True, exist_ok=True)
        stage = Path(tempfile.mkdtemp(prefix=f".{out.name}.tmp-", dir=out.parent))
        try:
            for item in infos:
                target = stage / item.filename
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(item) as src, open(target, "xb") as dst:
                    shutil.copyfileobj(src, dst, 1 << 20)
            verify(stage)
            _publish_dir(stage, out)
        finally:
            shutil.rmtree(stage, ignore_errors=True)
    return verify(out)


# ---------------------------------------------------------------- CLI

def main(argv=None):
    parser = argparse.ArgumentParser(description="Build, verify and pack EOD mod packages")
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("from-project", help="Package a built Kit project's changed members")
    p.add_argument("project"); p.add_argument("--disc", required=True)
    p.add_argument("--id", required=True); p.add_argument("--version", required=True)
    p.add_argument("--name"); p.add_argument("--out", required=True)
    p = sub.add_parser("init", help="Create an empty package")
    p.add_argument("package"); p.add_argument("--disc", required=True)
    p.add_argument("--id", required=True); p.add_argument("--version", required=True); p.add_argument("--name")
    p = sub.add_parser("add-host", help="Add a host texture replacement")
    p.add_argument("package"); p.add_argument("--disc", required=True)
    p.add_argument("--asset", required=True); p.add_argument("--image", required=True)
    p.add_argument("--any-palette", action="store_true", help="Bind regardless of the active palette")
    p = sub.add_parser("inspect"); p.add_argument("package")
    p = sub.add_parser("verify"); p.add_argument("package"); p.add_argument("--disc")
    p = sub.add_parser("zip"); p.add_argument("package"); p.add_argument("out")
    p = sub.add_parser("unzip"); p.add_argument("archive"); p.add_argument("out")
    args = parser.parse_args(argv)
    try:
        if args.cmd == "from-project":
            result = from_project(args.project, args.disc, args.id, args.version, args.out, args.name)
        elif args.cmd == "init":
            result = init(args.package, args.disc, args.id, args.version, args.name)
        elif args.cmd == "add-host":
            result = add_host(args.package, args.disc, args.asset, args.image, args.any_palette)
        elif args.cmd == "inspect":
            result = inspect(args.package)
        elif args.cmd == "verify":
            result = verify(args.package, args.disc)
        elif args.cmd == "zip":
            result = zip_package(args.package, args.out)
        else:
            result = unzip(args.archive, args.out)
    except (ValueError, OSError) as error:
        print(json.dumps(dict(ok=False, error=str(error))))
        return 1
    print(json.dumps(dict(ok=True, result=result), indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
