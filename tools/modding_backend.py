#!/usr/bin/env python3
"""Native retail asset/project backend shared by editor clients. JSON in, JSON out.

Uses the existing EOD format tools. It does not implement the proposed modern
engine manifests, arbitrary geometry compilation, or simulated game results.
"""
import contextlib
import hashlib
import io
import json
import math
import os
import re
from pathlib import Path
import struct
import sys
import tempfile
from functools import lru_cache

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rst0 import eod_crc, pack_archive, unpack_archive
from valt import compile_binary_valt, parse_binary_valt
from tex import write_png
from texture_preview import decode_preview
import vif_mesh

# Units for the curated vehicle inspector. Native tables retain their own data.
FIELDS = {
    ("Chassis", "mass"): "lb",
    ("Chassis", "grip"): "scale",
    ("Chassis", "damage_scale"): "scale",
    ("EngineNormal", "power_max"): "hp",
    ("EngineNormal", "torque_max"): "lb-ft",
    ("EngineNormal", "redline"): "rpm",
}
CAR_NAMES = ("Algar Blitz Bulldog Carrere Desio Digit Hurricane Moth Palo Top Arrow "
             "Cannon Empire Guerilla Hunter Limo Ocelot Prince Coyote Durand Fillmore "
             "Gladiator Mongoose Orbital Outlaw Rocket Venom Ambulance Bus Clown "
             "Copcar Hearse Postal Taxi Tow").split()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def inside(path, root):
    return path == root or root in path.parents


def archive_path(disc, relative):
    rel = Path(relative)
    path = (disc / rel).resolve()
    if rel.is_absolute() or ".." in rel.parts or not inside(path, disc / "DATA"):
        raise ValueError("Archive must be inside the selected disc's DATA directory")
    return path


def directory(path):
    size = path.stat().st_size
    with path.open("rb") as handle:
        hdr = handle.read(40)
        if len(hdr) != 40 or hdr[:4] != b"0TSR" or struct.unpack_from("<I", hdr, 8)[0] != 6:
            raise ValueError("Only native EOD 0TSR v6 archives are supported")
        if eod_crc(hdr[:36]) != struct.unpack_from("<I", hdr, 36)[0]:
            raise ValueError("Archive header CRC mismatch")
        offset, count = struct.unpack_from("<II", hdr, 20)
        if offset < 40 or count > 100000 or offset + count * 68 > size:
            raise ValueError("Truncated archive directory")
        handle.seek(offset)
        raw = handle.read(count * 68)
    entries, names = [], set()
    for i in range(count):
        entry = raw[i * 68:(i + 1) * 68]
        name = entry[:32].split(b"\0")[0].decode("latin1")
        length, position, crc = struct.unpack_from("<III", entry, 40)
        if not name or name in (".", "..", "manifest.json") or "/" in name or "\\" in name or name.lower() in names:
            raise ValueError("Unsafe or duplicate archive member")
        names.add(name.lower())
        if position + offset + length > size:
            raise ValueError("Truncated archive member: " + name)
        entries.append(dict(name=name, type=entry[32:36][::-1].decode("latin1").strip(),
                            size=length, offset=offset + position, crc=crc))
    return entries


@lru_cache(maxsize=256)
def cached_directory(path, modified, size):
    return directory(Path(path))


def resolve_material(disc, archive, name):
    if name.lower() in ("", "-", ".tex"):
        return None
    # Resource namespaces are mounted with the model's own archive first, then
    # the game's resident resources. Do not substitute another venue's same name.
    shared = ("HOBBYSET.RES", "RACE.RES", "COMMON.RES", "GENESIS.RES")
    candidates = [archive] + [disc / "DATA/RES" / filename for filename in shared]
    for candidate in candidates:
        if not candidate.is_file():
            continue
        stat = candidate.stat()
        entries = cached_directory(str(candidate), stat.st_mtime_ns, stat.st_size)
        match = next((e for candidate_name in (name, Path(name).stem + ".tm0")
                      for e in entries if e["name"].lower() == candidate_name.lower()), None)
        if match:
            return payload(candidate, match["name"], entries)[0]
    return None


def payload(path, name, entries=None):
    matches = [e for e in (entries or directory(path)) if e["name"] == name]
    if len(matches) != 1:
        raise ValueError("Asset not found: " + name)
    entry = matches[0]
    with path.open("rb") as handle:
        handle.seek(entry["offset"])
        data = handle.read(entry["size"])
    if eod_crc(data) != entry["crc"]:
        raise ValueError("Asset CRC mismatch: " + name)
    return data, entry


def project_path(request, disc):
    path = Path(request["project"]).expanduser().resolve()
    if inside(path, disc) or inside(disc, path.parent):
        raise ValueError("Choose a project folder separate from the original disc")
    return path


def load_project(path):
    data = json.loads(path.read_text())
    if data.get("version") != 1 or not isinstance(data.get("archives"), dict):
        raise ValueError("Not an EOD retail mod project (version 1)")
    return data


def atomic_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".eod-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def storage_path(project_file, relative):
    root = project_file.parent / (project_file.name + ".assets")
    output = root / relative
    if root.resolve() != root or output.resolve() != output or not inside(output, root):
        raise ValueError("Project output path escapes its storage or uses a symbolic link")
    return output


def overlay_file(project_file, relative):
    return storage_path(project_file, "overlay/" + relative)


def apply_edits(table, edits):
    seen = set()
    for edit in edits:
        section, key, value = edit["section"], edit["key"], str(edit["value"])
        identity = (section, key)
        if "sectionIndex" in edit and "pairIndex" in edit:
            si, pi = edit["sectionIndex"], edit["pairIndex"]
            if type(si) is not int or type(pi) is not int or not 0 <= si < len(table["sections"]) or not 0 <= pi < len(table["sections"][si]["pairs"]):
                raise ValueError("Native table row index is out of range")
            s, i = table["sections"][si], pi
            if s["name"] != section or s["pairs"][i][0] != key:
                raise ValueError("Native table row changed since it was opened")
        else:
            matches = [(s, i) for s in table["sections"] if s["name"] == section
                       for i, pair in enumerate(s["pairs"]) if pair[0] == key]
            if len(matches) != 1:
                raise ValueError("Missing or ambiguous native field: " + section + "/" + key)
            s, i = matches[0]
            si, pi = next(index for index, candidate in enumerate(table["sections"]) if candidate is s), i
        row = (si, pi)
        if row in seen:
            raise ValueError("Duplicate native field: " + section + "/" + key)
        seen.add(row)
        if "\0" in value:
            raise ValueError("Native table strings cannot contain a NUL byte")
        value.encode("latin1")
        if identity in FIELDS:
            if not re.fullmatch(r"[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?", value):
                raise ValueError("Enter a decimal number for " + section + "/" + key)
            number = float(value)
            try:
                stored = struct.unpack("<f", struct.pack("<f", number))[0]
            except (OverflowError, struct.error):
                raise ValueError("Value cannot be represented by the game's 32-bit float") from None
            if not math.isfinite(stored) or (number != 0 and stored == 0):
                raise ValueError("Value cannot be represented by the game's 32-bit float")
            if identity == ("Chassis", "mass") and stored <= 0:
                raise ValueError("Mass must be positive; the game divides by this value")
            value = format(stored, ".9g")
        s["pairs"][i] = (key, value)
    return table


def table_fields(table):
    """Compare native numeric values at their actual float32 precision."""
    result = []
    for section in table["sections"]:
        fields = []
        for key, value in section["pairs"]:
            if (section["name"], key) in FIELDS:
                try:
                    value = format(struct.unpack("<f", struct.pack("<f", float(value)))[0], ".9g")
                except (ValueError, OverflowError, struct.error):
                    pass
            fields.append((key, value))
        result.append((section["name"], fields))
    return result


def saved_edits(request, disc, source_sha=None):
    path = project_path(request, disc)
    if not path.exists():
        return []
    project = load_project(path)
    if Path(project["disc"]).resolve() != disc:
        raise ValueError("Project belongs to a different game installation")
    record = project["archives"].get(request["archive"], {})
    if source_sha and record and record["sourceSha256"] != source_sha:
        raise ValueError("Retail archive changed since this project was saved")
    return record.get("assets", {}).get(request["asset"], [])


def inspect(request, disc):
    path = archive_path(disc, request["archive"])
    data, entry = payload(path, request["asset"])
    result = dict(name=entry["name"], type=entry["type"], size=len(data), sha256=sha(data))
    cache = storage_path(project_path(request, disc), ".cache")
    if entry["type"] == "VALT":
        table = parse_binary_valt(data)
        original = [[v for _, v in s["pairs"]] for s in table["sections"]]
        edits = saved_edits(request, disc, sha(path.read_bytes()))
        if edits:
            apply_edits(table, edits)
        editable = not table.get("is_v6", False)
        result.update(kind="table", sections=[dict(name=s["name"], fields=[
            dict(id=f"{si}:{pi}", sectionIndex=si, pairIndex=pi, key=k, value=v, original=original[si][pi],
                 editable=editable,
                 featured=(s["name"], k) in FIELDS and entry["name"].lower() == "physics.val",
                 unit=FIELDS.get((s["name"], k), "")) for pi, (k, v) in enumerate(s["pairs"])])
            for si, s in enumerate(table["sections"])])
    elif entry["type"] == "TEX":
        image, meta = texture(data, cache)
        result.update(kind="texture", image=image, metadata=meta)
    elif entry["type"] == "MODL":
        mesh = vif_mesh.parse_modl(data)
        parts = []
        for part in mesh["parts"]:
            vertices, triangles = [], []
            for batch in vif_mesh.decode_part(data, part):
                base = len(vertices)
                faces = vif_mesh.triangulate_batch(batch)
                triangles.extend([a + base, b + base, c + base] for a, b, c in faces)
                vertices.extend(batch["verts"])
            image = None
            texture_error = None
            try:
                tex = resolve_material(disc, path, part["texture"])
                if tex is not None:
                    image, _ = texture(tex, cache)
                elif part["texture"].lower() not in ("", "-", ".tex"):
                    texture_error = "Material texture not found in model or resident archives: " + part["texture"]
            except (ValueError, IndexError, struct.error) as error:
                texture_error = str(error)
            if vertices and triangles:
                parts.append(dict(name=f"slot {part['slot']} · group {part['group'] & 0xffff}",
                    positions=[v["p"] for v in vertices], normals=[v["n"] for v in vertices],
                    uvs=[v["uv"] for v in vertices], triangles=triangles,
                    texture=image, textureError=texture_error))
        if not parts:
            raise ValueError("No supported VIF triangle batches in this model")
        result.update(kind="model", parts=parts, markers=mesh["markers"])
    else:
        lines = []
        for offset in range(0, min(len(data), 8192), 16):
            chunk = data[offset:offset + 16]
            text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
            lines.append(f"{offset:08X}  {chunk.hex(' ').upper():47s}  {text}")
        result.update(kind="binary", message="Raw native asset · " + entry["type"], hex="\n".join(lines))
    return result


def texture(data, cache):
    w, h, rgba, meta = decode_preview(data)
    cache.mkdir(parents=True, exist_ok=True)
    path = cache / (sha(data + b"studio-preview-v2" + rgba) + ".png")
    if path.resolve() != path:
        raise ValueError("Texture cache file uses a symbolic link")
    if not path.exists():
        write_png(str(path), w, h, rgba)
    return str(path), meta


def save(request, disc):
    project_file = project_path(request, disc)
    source = archive_path(disc, request["archive"])
    original = source.read_bytes()
    source_sha = sha(original)
    saved_edits(request, disc, source_sha)  # Detect a stale source/project installation.
    project = load_project(project_file) if project_file.exists() else dict(version=1, disc=str(disc), archives={})
    record = project["archives"].setdefault(request["archive"], dict(sourceSha256=source_sha, assets={}))
    record["assets"][request["asset"]] = request["edits"]
    # All assets in the source archive must pass CRC checks before any output.
    entries = directory(source)
    originals = {e["name"]: payload(source, e["name"], entries)[0] for e in entries}
    output = overlay_file(project_file, request["archive"])
    with tempfile.TemporaryDirectory(prefix="eod-build-") as temp:
        stage, packed = Path(temp) / "stage", Path(temp) / "packed.car"
        with contextlib.redirect_stdout(io.StringIO()):
            unpack_archive(str(source), str(stage))
            for asset, edits in record["assets"].items():
                if asset not in originals or next(e for e in entries if e["name"] == asset)["type"] != "VALT":
                    raise ValueError("Unsupported saved project edit")
                edited = apply_edits(parse_binary_valt(originals[asset]), edits)
                encoded = compile_binary_valt(edited)
                # Verify semantic roundtrip before native packaging.
                if parse_binary_valt(encoded)["sections"] != edited["sections"]:
                    # The compiler may normalize opaque metadata; fields must survive.
                    actual = parse_binary_valt(encoded)
                    if [(s["name"], list(map(tuple, s["pairs"]))) for s in actual["sections"]] != [
                            (s["name"], list(map(tuple, s["pairs"]))) for s in edited["sections"]]:
                        raise ValueError("Native table roundtrip failed")
                (stage / asset).write_bytes(encoded)
            pack_archive(str(stage), str(packed))
        built_entries = directory(packed)
        if {e["name"] for e in built_entries} != set(originals):
            raise ValueError("Built archive member inventory changed")
        for entry in built_entries:
            built, _ = payload(packed, entry["name"], built_entries)
            if entry["name"] not in record["assets"] and built != originals[entry["name"]]:
                raise ValueError("Unedited archive payload changed: " + entry["name"])
        built = packed.read_bytes()
    record["outputSha256"] = sha(built)
    project.update(selectedArchive=request["archive"], selectedAsset=request["asset"])
    if source.suffix.lower() == ".car":
        project["testCarArchive"] = request["archive"]
    # Migrate older builds' shared output folders by verified content, retaining
    # every other saved archive before publishing this project's isolated output.
    for relative, other in project["archives"].items():
        if relative == request["archive"]:
            continue
        destination = overlay_file(project_file, relative)
        if destination.is_file() and sha(destination.read_bytes()) == other["outputSha256"]:
            continue
        legacy = (project_file.parent / "overlay" / relative).resolve()
        if not inside(legacy, project_file.parent / "overlay") or not legacy.is_file() or sha(legacy.read_bytes()) != other["outputSha256"]:
            raise ValueError("Saved archive missing; rebuild this asset first: " + relative)
        atomic_write(destination, legacy.read_bytes())
    previous = output.read_bytes() if output.exists() else None
    atomic_write(output, built)
    try:
        atomic_write(project_file, (json.dumps(project, indent=2) + "\n").encode())
    except BaseException:
        if previous is None:
            output.unlink(missing_ok=True)
        else:
            atomic_write(output, previous)
        raise
    return dict(project=str(project_file), overlay=str(storage_path(project_file, "overlay")),
                archivePath=str(output), sha256=sha(built), bytes=len(built))


def validate(request, disc):
    path = project_path(request, disc)
    project = load_project(path)
    if Path(project["disc"]).resolve() != disc or not project["archives"]:
        raise ValueError("Project has no built assets for this game installation")
    for relative, record in project["archives"].items():
        original = archive_path(disc, relative)
        output = overlay_file(path, relative)
        if sha(original.read_bytes()) != record["sourceSha256"]:
            raise ValueError("Retail archive changed: " + relative)
        if not output.exists() or sha(output.read_bytes()) != record["outputSha256"]:
            raise ValueError("Built archive missing or modified; save the project again: " + relative)
        directory(output)
        for asset, edits in record["assets"].items():
            source_data, _ = payload(original, asset)
            built_data, _ = payload(output, asset)
            expected = apply_edits(parse_binary_valt(source_data), edits)
            actual = parse_binary_valt(built_data)
            if table_fields(expected) != table_fields(actual):
                raise ValueError("Project edits do not match the built archive; save again")
    overlay = storage_path(path, "overlay")
    extras = {f.relative_to(overlay).as_posix() for f in overlay.rglob("*") if f.is_file()} - set(project["archives"])
    if extras:
        raise ValueError("Overlay contains untracked files: " + ", ".join(sorted(extras)))
    return dict(overlay=str(overlay), archives=len(project["archives"]))


def launch_plan(request, disc):
    checked = validate(request, disc)
    project_file = project_path(request, disc)
    project = load_project(project_file)
    executable = Path(request["executable"]).expanduser().resolve()
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise ValueError("Choose an executable game build")
    stem = Path(project.get("testCarArchive", project["selectedArchive"])).stem.upper()
    def native_name(name):
        return (name if len(name) <= 8 else name[:6] + "_1").upper()
    car = next((name for name in CAR_NAMES if native_name(name) == stem), None)
    if not car:
        car = "Algar"
    run_dir = storage_path(project_file, "runs")
    for existing in storage_path(project_file, "").rglob("*"):
        if existing.is_symlink():
            raise ValueError("Project storage contains a symbolic link: " + existing.name)
    run_dir.mkdir(parents=True, exist_ok=True)
    config = storage_path(project_file, "runs/test.ini")
    atomic_write(config, b"[Video]\nRenderer=vulkan\nRenderScale=1\nAspectRatio=4:3\nFrameSkip=0\nVSync=1\n")
    args = ["--data", str(disc), "--config", str(config)]
    environment = dict(EOD_MOD_DIR=checked["overlay"], EOD_MEMCARD=str(storage_path(project_file, "runs/memcard")),
        EOD_DIRECT_START=f"mode=action;track=Anoka;event=No-Rules Race;car={car};paint=0;humans=1",
        EOD_SKIP_MOVIES="1")
    if request.get("headless"):
        args += ["--headless"]
        environment.update(EOD_HEADLESS="1", EOD_MUTE="1")
    if request.get("frames"):
        frames = int(request["frames"])
        if not 1 <= frames <= 36000:
            raise ValueError("Test duration must be 1–36000 frames")
        args += ["--frames", str(frames)]
    return dict(executable=str(executable), arguments=args, environment=environment,
                logPath=str(storage_path(project_file, "runs/game.log")), car=car)


def dispatch(request):
    action = request["action"]
    if action == "load":
        path = Path(request["project"]).expanduser().resolve()
        project = load_project(path)
        project_path(request, Path(project["disc"]).resolve())
        return project
    disc = Path(request["disc"]).expanduser().resolve()
    if not (disc / "DATA").is_dir():
        raise ValueError("Select the extracted game folder containing DATA and SLUS_209.10")
    if action == "catalog":
        archives, errors = [], []
        for path in sorted((disc / "DATA").rglob("*")):
            if path.is_file() and path.suffix.lower() in (".car", ".res", ".trk"):
                relative = path.relative_to(disc).as_posix()
                try:
                    entries = directory(archive_path(disc, relative))
                    archives.append(dict(path=relative, name=path.name, count=len(entries), size=path.stat().st_size))
                except (OSError, ValueError, struct.error) as error:
                    errors.append(relative + ": " + str(error))
        return dict(archives=archives, assetCount=sum(a["count"] for a in archives), errors=errors)
    if action == "entries":
        path = archive_path(disc, request["archive"])
        return dict(entries=directory(path))
    if action == "inspect":
        return inspect(request, disc)
    if action == "export_native":
        destination = Path(request["destination"]).expanduser().resolve()
        if inside(destination, disc):
            raise ValueError("Export must not overwrite the installed disc")
        data, _ = payload(archive_path(disc, request["archive"]), request["asset"])
        atomic_write(destination, data)
        return dict(path=str(destination), bytes=len(data), sha256=sha(data))
    if action == "save":
        return save(request, disc)
    if action == "validate":
        return validate(request, disc)
    if action == "launch_plan":
        return launch_plan(request, disc)
    raise ValueError("Unknown action: " + action)


def main():
    try:
        request = json.load(sys.stdin)
        result = dispatch(request)
        print(json.dumps(dict(ok=True, result=result), allow_nan=False))
    except Exception as error:
        print(json.dumps(dict(ok=False, error=str(error))))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
