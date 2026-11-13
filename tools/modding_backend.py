#!/usr/bin/env python3
"""Native retail asset/project backend shared by editor clients. JSON in, JSON out.

Uses the existing EOD format tools. It does not implement the proposed modern
engine manifests, arbitrary geometry compilation, or simulated game results.
"""
import contextlib
import hashlib
import io
import base64
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
import formats
from formats import valt_schema
from vehicle_materials import BODY_TEXTURE_ALIASES

# Units for the curated vehicle inspector. Native tables retain their own data.
FIELDS = {
    ("Chassis", "mass"): "lb",
    ("Chassis", "grip"): "scale",
    ("Chassis", "damage_tweak"): "scale",
    ("EngineNormal", "power_max"): "hp",
    ("EngineNormal", "torque_max"): "lb-ft",
    ("EngineNormal", "redline"): "rpm",
}
DEV_PROFILE = "dev"
JOBS_ROOT = Path("~/Library/Application Support/EODCreationKit/jobs")
TRACK_NAMES = ("Afton Anoka Baxter Bemidji Dennison Dundas Eagan Gilroy Hastings HomeBase Kenyon Mankato Springfield Welch").split()

CAR_NAMES = ("Algar Blitz Bulldog Carrere Desio Digit Hurricane Moth Palo Top Arrow "
             "Cannon Empire Guerilla Hunter Limo Ocelot Prince Coyote Durand Fillmore "
             "Gladiator Mongoose Orbital Outlaw Rocket Venom Ambulance Bus Clown "
             "Copcar Hearse Postal Taxi Tow").split()


class PlainError(ValueError):
    """A sentence for the modder; `detail` keeps the technical cause for logs and MCP."""

    def __init__(self, message, detail=None):
        super().__init__(message)
        self.detail = detail


def sha(data):
    return hashlib.sha256(data).hexdigest()


def inside(path, root):
    return path == root or root in path.parents


def archive_path(disc, relative):
    rel = Path(relative)
    path = (disc / rel).resolve()
    if rel.is_absolute() or ".." in rel.parts or not inside(path, disc / "DATA"):
        raise ValueError("That archive is outside the game folder's DATA folder, so it can't be opened. Choose an archive from the game sources list.")
    return path


def directory(path):
    size = path.stat().st_size
    with path.open("rb") as handle:
        hdr = handle.read(40)
        if len(hdr) != 40 or hdr[:4] != b"0TSR" or struct.unpack_from("<I", hdr, 8)[0] != 6:
            raise PlainError("This file isn't a game archive the Creation Kit can read. Choose a .CAR, .RES or .TRK file from the game folder.", "0TSR version 6 header expected")
        if eod_crc(hdr[:36]) != struct.unpack_from("<I", hdr, 36)[0]:
            raise PlainError("This archive's header is damaged, so it was not opened. Re-extract the game folder from your disc and try again.", "archive header checksum mismatch")
        offset, count = struct.unpack_from("<II", hdr, 20)
        if offset < 40 or count > 100000 or offset + count * 68 > size:
            raise PlainError("This archive is incomplete, so it was not opened. Re-extract the game folder from your disc and try again.", "archive directory runs past end of file")
        handle.seek(offset)
        raw = handle.read(count * 68)
    entries, names = [], set()
    for i in range(count):
        entry = raw[i * 68:(i + 1) * 68]
        name = entry[:32].split(b"\0")[0].decode("latin1")
        length, position, crc = struct.unpack_from("<III", entry, 40)
        if not name or name in (".", "..", "manifest.json") or "/" in name or "\\" in name or name.lower() in names:
            raise PlainError("This archive lists the same file twice or uses an unsafe file name, so it was not opened. Re-extract the game folder from your disc.", "duplicate or unsafe member name: " + name)
        names.add(name.lower())
        if position + offset + length > size:
            raise PlainError(f"{name} extends past the end of its archive, so it can't be read. Re-extract the game folder from your disc.", "member runs past end of file")
        entries.append(dict(name=name, type=entry[32:36][::-1].decode("latin1").strip(),
                            size=length, offset=offset + position, crc=crc))
    return entries


@lru_cache(maxsize=256)
def cached_directory(path, modified, size):
    return directory(Path(path))


def vehicle_paint(disc, archive, include_embedded=False):
    if archive.suffix.lower() != ".car":
        return None
    stem = archive.stem.upper()
    chassis = next((name for name in CAR_NAMES if (name.upper() if len(name) <= 8 else name[:6].upper()+"_1") == stem), None)
    if not chassis or chassis.lower() not in BODY_TEXTURE_ALIASES:
        return None
    local_names = {e["name"].lower() for e in directory(archive)}
    aliases = BODY_TEXTURE_ALIASES[chassis.lower()]
    if not include_embedded and all(n.lower() in local_names or (Path(n).stem + ".tm0").lower() in local_names for n in aliases):
        return None
    common, paints = disc / "DATA/RES/COMMON.RES", disc / "DATA/RES/CT.RES"
    if not common.is_file() or not paints.is_file():
        return None
    # Native numbered livery contract: Driver.chassis + Driver.number, CT.RES.
    # Use a deterministic real driver, and expose this default in the preview.
    stat = common.stat()
    entries = cached_directory(str(common), stat.st_mtime_ns, stat.st_size)
    match = next((e for e in entries if e["name"].lower() == "dirt.val"), None)
    if match is None:
        return None
    raw, _ = payload(common, match["name"], entries, verify_crc=False)
    table = parse_binary_valt(raw)
    paint_stat = paints.stat()
    paint_entries = cached_directory(str(paints), paint_stat.st_mtime_ns, paint_stat.st_size)
    for section in table["sections"]:
        values = dict(section["pairs"])
        if section["name"].lower() != "driver" or values.get("chassis", "").lower() != chassis.lower():
            continue
        number = values.get("number", "")
        if not re.fullmatch(r"[0-9]+", number):
            continue
        resource = next((e for suffix in (".tm0", ".tex") for e in paint_entries
                         if e["name"].lower() == (number+suffix).lower()), None)
        if resource:
            return dict(chassis=chassis, number=number, driver=values.get("name", ""),
                        asset=resource["name"], archive=paints)
    return None


def edited_material(disc, archive, name, request):
    if not request:
        return None
    relative = archive.relative_to(disc).as_posix()
    entries = directory(archive)
    # An edited high-detail payload also supplies a model's .tex material alias.
    for candidate in (Path(name).stem + ".tm0", name):
        entry = next((e for e in entries if e["name"].lower() == candidate.lower()), None)
        if entry is None:
            continue
        selected = dict(request, archive=relative, asset=entry["name"])
        pending = request.get("textureEdits", {}) if relative == request["archive"] else {}
        edit = pending.get(entry["name"]) or saved_edits(selected, disc, sha(archive.read_bytes()))
        if edit:
            return texture_edit_bytes(edit, payload(archive, entry["name"], entries, verify_crc=False)[0])
    return None


def resolve_material(disc, archive, name, paint=None, request=None):
    if name.lower() in ("", "-", ".tex"):
        return None
    edited = edited_material(disc, archive, name, request)
    if edited is not None:
        return edited
    paint = paint if paint is not None else vehicle_paint(disc, archive)
    if request and paint and name.lower() in {n.lower() for n in BODY_TEXTURE_ALIASES.get(paint["chassis"].lower(), ())}:
        paint_name = paint["asset"]
        if paint["chassis"] == "Bus" and name.lower() == "bus2.tex" and paint["number"] == "790":
            paint_name = "79a.tm0"
        edited = edited_material(disc, paint["archive"], paint_name, request)
        return edited if edited is not None else payload(paint["archive"], paint_name, verify_crc=False)[0]
    # Resource namespaces are mounted with the model's own archive first, then
    # the game's resident resources. Do not substitute another venue's same name.
    shared = ("HOBBYSET.RES", "RACE.RES", "PART.RES", "COMMON.RES", "GENESIS.RES")
    candidates = [archive] + [disc / "DATA/RES" / filename for filename in shared]
    for candidate in candidates:
        if not candidate.is_file():
            continue
        stat = candidate.stat()
        entries = cached_directory(str(candidate), stat.st_mtime_ns, stat.st_size)
        match = next((e for candidate_name in (name, Path(name).stem + ".tm0")
                      for e in entries if e["name"].lower() == candidate_name.lower()), None)
        if match:
            edited = edited_material(disc, candidate, name, request)
            return edited if edited is not None else payload(candidate, match["name"], entries, verify_crc=False)[0]
    if paint and name.lower() in {n.lower() for n in BODY_TEXTURE_ALIASES.get(paint["chassis"].lower(), ())}:
        return payload(paint["archive"], paint["asset"], verify_crc=False)[0]
    return None


def payload(path, name, entries=None, verify_crc=True):
    matches = [e for e in (entries or directory(path)) if e["name"] == name]
    if len(matches) != 1:
        raise ValueError(f"{name} is not in this archive. Refresh the list and choose again.")
    entry = matches[0]
    with path.open("rb") as handle:
        handle.seek(entry["offset"])
        data = handle.read(entry["size"])
    entry = dict(entry, crcOk=eod_crc(data) == entry["crc"])
    if verify_crc and not entry["crcOk"]:
        raise PlainError(f"{name} does not match its stored checksum, so it was not used. "
                         "Re-extract the game folder from your disc and try again.", "member checksum mismatch")
    return data, entry


def project_path(request, disc):
    path = Path(request["project"]).expanduser().resolve()
    if inside(path, disc) or inside(disc, path.parent):
        raise ValueError("The project folder can't be inside the game folder. Choose a different folder so the original files stay untouched.")
    return path


def load_project(path):
    data = json.loads(path.read_text())
    if data.get("version") != 1 or not isinstance(data.get("archives"), dict):
        raise ValueError("This isn't a Creation Kit project file. Open a project saved here, or start a new one.")
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
        raise ValueError("The project's storage contains a shortcut (symbolic link), which isn't allowed. Remove it or choose another project folder.")
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
                raise ValueError("A field in this table no longer exists. Reopen the asset and repeat the edit.")
            s, i = table["sections"][si], pi
            if s["name"] != section or s["pairs"][i][0] != key:
                raise ValueError("This table changed after it was opened. Reopen the asset and repeat the edit.")
        else:
            matches = [(s, i) for s in table["sections"] if s["name"] == section
                       for i, pair in enumerate(s["pairs"]) if pair[0] == key]
            if len(matches) != 1:
                raise ValueError(f"The field {section} / {key} could not be found, or appears more than once. Reopen the asset and choose it again.")
            s, i = matches[0]
            si, pi = next(index for index, candidate in enumerate(table["sections"]) if candidate is s), i
        row = (si, pi)
        if row in seen:
            raise ValueError(f"{section} / {key} was edited twice in one save. Keep a single value and save again.")
        seen.add(row)
        if "\0" in value:
            raise ValueError("Text values can't contain a null character. Remove it and try again.")
        try:
            value.encode("latin1")
        except UnicodeEncodeError:
            raise ValueError("Text values can only use standard Western characters. Replace the others and try again.") from None
        if identity in FIELDS:
            if not re.fullmatch(r"[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?", value):
                raise ValueError(f"{key} needs a number, such as 2800 or 0.9.")
            number = float(value)
            try:
                stored = struct.unpack("<f", struct.pack("<f", number))[0]
            except (OverflowError, struct.error):
                raise ValueError("That number is too large or too small for the game to store. Enter a smaller value.") from None
            if not math.isfinite(stored) or (number != 0 and stored == 0):
                raise ValueError("That number is too large or too small for the game to store. Enter a smaller value.")
            if identity == ("Chassis", "mass") and stored <= 0:
                raise ValueError("Mass must be above zero; the game can't simulate a car with no weight. Enter a positive value.")
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


@lru_cache(maxsize=64)
def _archive_sha(path, modified, size):
    return sha(Path(path).read_bytes())


def archive_sha(path):
    stat = path.stat()
    return _archive_sha(str(path), stat.st_mtime_ns, stat.st_size)


def saved_edits(request, disc, source_sha=None):
    path = project_path(request, disc)
    if not path.exists():
        return []
    project = load_project(path)
    if Path(project["disc"]).resolve() != disc:
        raise ValueError("This project was made for a different game folder. Open it with its original game folder, or start a new project.")
    record = project["archives"].get(request["archive"], {})
    if source_sha and record and record["sourceSha256"] != source_sha:
        raise ValueError("The game file this project edits has changed since the project was saved. Re-extract the original game folder, or start a new project.")
    return record.get("assets", {}).get(request["asset"], [])


CRC_WARNING = "This file's checksum differs on the original disc. It opens normally, and saved edits are verified."
FORMAT_EDIT = "format"


def hex_dump(data, limit=8192):
    lines = []
    for offset in range(0, min(len(data), limit), 16):
        chunk = data[offset:offset + 16]
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{offset:08X}  {chunk.hex(' ').upper():47s}  {text}")
    return "\n".join(lines)


def format_result(request, disc, path, entry, data, result, warnings):
    """Labelled view for any member without a dedicated preview (SETU, ILIN, LAYT, ...)."""
    original = data
    saved = saved_edits(request, disc, archive_sha(path))
    pending = request.get("formatEdits")
    edits = pending if isinstance(pending, dict) else (saved["edits"] if isinstance(saved, dict) and saved.get("kind") == FORMAT_EDIT else None)
    if edits:
        data = formats.apply_edits(entry["type"], data, edits)
    context = dict(archive=request["archive"], member=entry["name"], disc=str(disc), compact=not request.get("expert"))
    view = formats.describe(entry["type"], data, context)
    view["warnings"] = list(warnings) + list(view.get("warnings", []))
    changed = bool(edits) and data != original
    result.update(kind="format", format=view, hex=hex_dump(data), edited=changed,
                  size=len(data), sha256=sha(data), warnings=view["warnings"])
    return result


def inspect(request, disc):
    path = archive_path(disc, request["archive"])
    data, entry = payload(path, request["asset"], verify_crc=False)
    result = dict(name=entry["name"], type=entry["type"], size=len(data), sha256=sha(data))
    warnings = [] if entry["crcOk"] else [CRC_WARNING]
    result["warnings"] = warnings
    cache = storage_path(project_path(request, disc), ".cache")
    DECODE_ERRORS = (ValueError, IndexError, KeyError, struct.error, ArithmeticError)

    def unreadable(kind):
        return format_result(request, disc, path, entry, data, result, warnings + [
            f"This {kind} couldn't be previewed, so its contents are listed below. It saves unchanged."])

    if entry["type"] == "VALT":
        try:
            table = parse_binary_valt(data)
        except DECODE_ERRORS:
            return unreadable("table")
        original = [[v for _, v in s["pairs"]] for s in table["sections"]]
        edits = saved_edits(request, disc, archive_sha(path))
        if edits:
            apply_edits(table, edits)
        editable = not table.get("is_v6", False)
        schema = valt_schema.describe_table(request["archive"], entry["name"], table)

        def field_json(si, pi, section, key, value):
            spec = schema["fields"][(section["name"], key)]
            explanation = spec["explanation"]
            note = valt_schema.value_note(schema["family"], section["name"], key, value)
            if note:
                explanation = (explanation + " " + note + ".").strip()
            return dict(id=f"{si}:{pi}", sectionIndex=si, pairIndex=pi, key=key, value=value, original=original[si][pi],
                        editable=editable and spec["editable"],
                        featured=(section["name"], key) in FIELDS and entry["name"].lower() == "physics.val",
                        unit=spec["unit"] or FIELDS.get((section["name"], key), ""),
                        label=spec["label"], group=spec["group"], explanation=explanation,
                        confidence=spec["confidence"], used=spec["used"], min=spec["min"], max=spec["max"])

        result.update(kind="table", title=schema["title"], summary=schema["summary"], sections=[
            dict(name=s["name"], label=schema["sections"][si]["label"], explanation=schema["sections"][si]["explanation"],
                 fields=[field_json(si, pi, s, k, v) for pi, (k, v) in enumerate(s["pairs"])])
            for si, s in enumerate(table["sections"])])
    elif entry["type"] == "TEX":
        edit = request.get("textureEdit") or saved_edits(request, disc, archive_sha(path))
        if edit:
            data = texture_edit_bytes(edit, data)
        try:
            image, meta = texture(data, cache)
        except DECODE_ERRORS:
            return unreadable("texture")
        result.update(kind="texture", image=image, metadata=meta)
    elif entry["type"] == "MODL":
        try:
            mesh = vif_mesh.parse_modl(data)
        except DECODE_ERRORS:
            return unreadable("model")
        paint = vehicle_paint(disc, path, include_embedded=True)
        parts = []
        for part in mesh["parts"]:
            vertices, triangles = [], []
            try:
                batches = list(vif_mesh.decode_part(data, part))
                for batch in batches:
                    base = len(vertices)
                    faces = vif_mesh.triangulate_batch(batch)
                    triangles.extend([a + base, b + base, c + base] for a, b, c in faces)
                    vertices.extend(batch["verts"])
            except DECODE_ERRORS:
                vertices, triangles = [], []
            image = None
            texture_error = None
            try:
                tex = resolve_material(disc, path, part["texture"], paint=paint, request=request)
                if tex is not None:
                    image, _ = texture(tex, cache)
                elif part["texture"].lower() not in ("", "-", ".tex"):
                    texture_error = f"Texture {part['texture']} was not found in this model's archive or the game's shared archives."
            except (ValueError, IndexError, struct.error) as error:
                texture_error = f"Texture {part['texture']} could not be read."
            if vertices and triangles:
                parts.append(dict(name=f"slot {part['slot']} · group {part['group'] & 0xffff}",
                    positions=[v["p"] for v in vertices], normals=[v["n"] for v in vertices],
                    uvs=[v["uv"] for v in vertices], triangles=triangles,
                    texture=image, textureError=texture_error))
        if not parts:
            return format_result(request, disc, path, entry, data, result, warnings + [
                "This model's geometry can't be previewed yet. Its contents are listed below and it saves unchanged."])
        result.update(kind="model", parts=parts, markers=mesh["markers"],
                      paintDescription=(f"Preview livery {paint['number']} · {paint['driver']} · CT.RES/{paint['asset']}" if paint else None))
    else:
        return format_result(request, disc, path, entry, data, result, warnings)
    return result


def texture(data, cache):
    w, h, rgba, meta = decode_preview(data)
    cache.mkdir(parents=True, exist_ok=True)
    path = cache / (sha(data + b"studio-preview-v2" + rgba) + ".png")
    if path.resolve() != path:
        raise ValueError("The preview cache contains a shortcut (symbolic link) and was not used. Delete the project's .cache folder and try again.")
    if not path.exists():
        write_png(str(path), w, h, rgba)
    return str(path), meta


def save_single_archive(request, disc):
    project_file = project_path(request, disc)
    source = archive_path(disc, request["archive"])
    original = source.read_bytes()
    source_sha = sha(original)
    saved_edits(request, disc, source_sha)  # Detect a stale source/project installation.
    project = load_project(project_file) if project_file.exists() else dict(version=1, disc=str(disc), archives={})
    record = project["archives"].setdefault(request["archive"], dict(sourceSha256=source_sha, assets={}))
    if "edits" in request:
        record["assets"][request["asset"]] = request["edits"]
    if request.get("formatEdits"):
        if not isinstance(request["formatEdits"], dict):
            raise ValueError("The changes could not be read. Reopen the asset and try again.")
        record["assets"][request["asset"]] = dict(kind=FORMAT_EDIT, edits=request["formatEdits"])
    record["assets"].update(request.get("textureEdits", {}))
    if not record["assets"]:
        raise ValueError("There are no changes to save. Edit a value or texture first.")
    # Keep opaque source payloads exact, including pre-existing retail CRC
    # discrepancies. Edited members must still pass their original CRC check.
    entries = directory(source)
    originals = {e["name"]: payload(source, e["name"], entries, verify_crc=False)[0] for e in entries}
    warnings = [e["name"] for e in entries if eod_crc(originals[e["name"]]) != e["crc"]]
    changes = {}
    for asset, edits in record["assets"].items():
        if asset not in originals:
            raise ValueError("A saved change refers to a file the archive no longer contains. Discard that change and try again.")
        data, entry = payload(source, asset, entries, verify_crc=False)
        if entry["type"] == "TEX":
            encoded = texture_edit_bytes(edits, data)
        elif isinstance(edits, dict) and edits.get("kind") == FORMAT_EDIT:
            encoded = formats.apply_edits(entry["type"], data, edits["edits"])
        elif entry["type"] == "VALT" and isinstance(edits, list):
            edited = apply_edits(parse_binary_valt(data), edits)
            encoded = compile_binary_valt(edited)
            if table_fields(parse_binary_valt(encoded)) != table_fields(edited):
                raise PlainError("The edited table could not be written back without altering other values, so nothing was saved. Try a smaller edit.", "table round-trip comparison failed")
        else:
            raise ValueError("This project holds a kind of change this version can't build. Update the Creation Kit, or discard that change.")
        changes[asset] = encoded
    output = overlay_file(project_file, request["archive"])
    with tempfile.TemporaryDirectory(prefix="eod-build-") as temp:
        stage, packed = Path(temp) / "stage", Path(temp) / "packed.car"
        if all(len(data) == len(originals[name]) for name, data in changes.items()):
            # Native texture edits retain size. Patch only their bytes and CRC,
            # preserving every original world/layout/sector/directory byte else.
            edited_archive = bytearray(original)
            directory_offset = struct.unpack_from("<I", original, 20)[0]
            for index, entry in enumerate(entries):
                if entry["name"] in changes:
                    data = changes[entry["name"]]
                    edited_archive[entry["offset"]:entry["offset"]+len(data)] = data
                    struct.pack_into("<I", edited_archive, directory_offset+index*68+48, eod_crc(data))
            packed.write_bytes(edited_archive)
        else:
            with contextlib.redirect_stdout(io.StringIO()):
                unpack_archive(str(source), str(stage))
                for asset, data in changes.items():
                    (stage / asset).write_bytes(data)
                pack_archive(str(stage), str(packed))
        built_entries = directory(packed)
        if {e["name"] for e in built_entries} != set(originals):
            raise ValueError("The rebuilt archive doesn't have the same files as the original, so it was discarded. Nothing was saved; try again.")
        for entry in built_entries:
            data, _ = payload(packed, entry["name"], built_entries, verify_crc=entry["name"] in changes)
            if entry["name"] not in changes and data != originals[entry["name"]]:
                raise ValueError(f"{entry['name']} would have changed without being edited, so the rebuilt archive was discarded. Nothing was saved.")
        built = packed.read_bytes()
    record["sourceCrcWarnings"] = warnings
    record["outputSha256"] = sha(built)
    selected = request["asset"]
    if request.get("textureEdits") and selected not in record["assets"]:
        selected = next(iter(request["textureEdits"]))
    project.update(selectedArchive=request["archive"], selectedAsset=selected)
    if source.suffix.lower() == ".trk":
        project["testTrackArchive"] = request["archive"]
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
            raise ValueError(f"A previously built archive ({relative}) is missing. Open that asset and choose Save & Build again.")
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
                archivePath=str(output), sha256=sha(built), bytes=len(built), sourceCrcWarnings=warnings)


def save_format(request, disc):
    """Persist labelled-field edits ({field id: value}) for one member and rebuild its archive.

    Edits are stored in the project beside VALT and texture edits; the rebuilt
    member gets a fresh directory CRC, every other member stays byte-identical.
    """
    edits = request.get("formatEdits")
    if not isinstance(edits, dict) or not edits:
        raise ValueError("There are no changes to save. Edit a value first.")
    source = archive_path(disc, request["archive"])
    data, entry = payload(source, request["asset"], verify_crc=False)
    formats.apply_edits(entry["type"], data, edits)  # reject out-of-range values before anything is written
    result = save_single_archive(request, disc)
    result["edited"] = request["asset"]
    return result


def save(request, disc):
    """Save a Bus body template and its verified paint-zero runtime resources.

    Native Bus logs bind bus1/2.tex to 790/79a.tex for paint=0. Both low and
    streamed mip-zero resources must be rebuilt; editing the CAR alone is inert.
    Other vehicle mappings are not inferred from filenames.
    """
    if request["archive"].upper() != "DATA/CAR/BUS.CAR":
        return save_single_archive(request, disc)
    project_file = project_path(request, disc)
    project = load_project(project_file) if project_file.exists() else {"archives": {}}
    assets = dict(project["archives"].get(request["archive"], {}).get("assets", {}))
    assets.update(request.get("textureEdits", {}))
    sources = {Path(name).stem.lower(): name for name in sorted(assets, key=lambda n: n.lower().endswith(".tm0"))
               if isinstance(assets[name], dict) and Path(name).stem.lower() in ("bus1", "bus2")}
    if not sources:
        return save_single_archive(request, disc)
    paint = vehicle_paint(disc, archive_path(disc, request["archive"]), include_embedded=True)
    if not paint or paint["number"] != "790":
        raise ValueError("Bus paint changes can only be built when the bus uses its standard paint set. This copy differs, so nothing was saved.")
    from texture_edit import import_png
    texture_edits = {}
    with tempfile.TemporaryDirectory(prefix="eod-bus-paint-") as folder:
        png = Path(folder) / "paint.png"
        for body, source_name in sources.items():
            original, _ = payload(archive_path(disc, request["archive"]), source_name, verify_crc=False)
            data = texture_edit_bytes(assets[source_name], original)
            width, height, pixels, _ = decode_preview(data)
            number = "790" if body == "bus1" else "79a"
            for suffix in (".tex", ".tm0"):
                target_name = number + suffix
                target, _ = payload(paint["archive"], target_name, verify_crc=False)
                tw, th, _, _ = decode_preview(target)
                resized = b"".join(pixels[((y*height//th)*width+x*width//tw)*4:((y*height//th)*width+x*width//tw)*4+4]
                                   for y in range(th) for x in range(tw))
                write_png(str(png), tw, th, resized)
                texture_edits[target_name] = texture_record(import_png(target, png))
    # Both archives and the project are a single recoverable publication.
    paths = [project_file, overlay_file(project_file, request["archive"]), overlay_file(project_file, "DATA/RES/CT.RES")]
    previous = {p: p.read_bytes() if p.exists() else None for p in paths}
    try:
        paint_request = dict(request, archive="DATA/RES/CT.RES", asset=next(iter(texture_edits)), textureEdits=texture_edits)
        paint_request.pop("edits", None)
        save_single_archive(paint_request, disc)
        return save_single_archive(request, disc)
    except BaseException:
        for path, data in previous.items():
            if data is None:
                path.unlink(missing_ok=True)
            else:
                atomic_write(path, data)
        raise


def validate(request, disc):
    path = project_path(request, disc)
    project = load_project(path)
    if Path(project["disc"]).resolve() != disc or not project["archives"]:
        raise ValueError("This project has no saved changes for this game folder. Choose Save & Build on an edit first.")
    for relative, record in project["archives"].items():
        original = archive_path(disc, relative)
        output = overlay_file(path, relative)
        if sha(original.read_bytes()) != record["sourceSha256"]:
            raise ValueError(f"{relative} has changed since this project was built. Choose Save & Build again.")
        if not output.exists() or sha(output.read_bytes()) != record["outputSha256"]:
            raise ValueError(f"The built copy of {relative} is missing or was modified. Choose Save & Build again.")
        directory(output)
        for asset, edits in record["assets"].items():
            source_data, _ = payload(original, asset, verify_crc=False)
            built_data, _ = payload(output, asset)
            if isinstance(edits, dict) and edits.get("kind") == FORMAT_EDIT:
                _, entry = payload(original, asset, verify_crc=False)
                if built_data != formats.apply_edits(entry["type"], source_data, edits["edits"]):
                    raise ValueError("Saved changes don't match the built archive. Choose Save & Build again.")
            elif isinstance(edits, dict):
                expected = texture_edit_bytes(edits, source_data)
                if built_data != expected:
                    raise ValueError("A saved texture doesn't match the built archive. Choose Save & Build again.")
            else:
                expected = apply_edits(parse_binary_valt(source_data), edits)
                actual = parse_binary_valt(built_data)
                if table_fields(expected) != table_fields(actual):
                    raise ValueError("Saved changes don't match the built archive. Choose Save & Build again.")
    overlay = storage_path(path, "overlay")
    extras = {f.relative_to(overlay).as_posix() for f in overlay.rglob("*") if f.is_file()} - set(project["archives"])
    if extras:
        raise ValueError("The mod output holds files this project didn't build (" + ", ".join(sorted(extras)) + "). Remove them from the project's overlay folder or rebuild.")
    return dict(overlay=str(overlay), archives=len(project["archives"]))


def launch_plan(request, disc):
    checked = validate(request, disc)
    return build_launch_plan(request, disc, checked["overlay"])


def launch_profile(request, disc):
    """Same isolated session as launch_plan, but EOD_MOD_DIR is the dev profile's composed overlay."""
    import mod_install
    name, root = request.get("profile") or DEV_PROFILE, request.get("profileRoot")
    if not any(p["enabled"] for p in mod_install.status(name, root)["packages"]):
        raise ValueError("No mod is enabled in the test profile. Install and enable a package in Mods first.")
    env = mod_install.launch_env(name, root)
    plan = build_launch_plan(request, disc, env["EOD_MOD_DIR"])
    plan["profile"] = request.get("profile") or DEV_PROFILE
    return plan


def build_launch_plan(request, disc, mod_dir):
    project_file = project_path(request, disc)
    project = load_project(project_file)
    executable = Path(request["executable"]).expanduser().resolve()
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise ValueError("The game application could not be found. Choose the game build to test with.")
    car_archive = project.get("testCarArchive") or project.get("selectedArchive") or ""
    car_note = None
    if car_archive.lower().endswith(".car"):
        stem = Path(car_archive).stem.upper()
        car = next((name for name in CAR_NAMES if (name if len(name) <= 8 else name[:6] + "_1").upper() == stem), None)
        if not car:
            raise ValueError(f"{Path(car_archive).name} can't be driven in a test race yet. "
                             "Choose another vehicle, or save the project from a track.")
    else:
        car = "Algar"
        car_note = "No vehicle in this project; the test drives Algar"
    track_archive = project.get("testTrackArchive", "DATA/TRACKS/ANOKA.TRK")
    track = track_name(track_archive)
    run_dir = storage_path(project_file, "runs")
    for existing in storage_path(project_file, "").rglob("*"):
        if existing.is_symlink():
            raise ValueError(f"The project's storage contains a shortcut (symbolic link) named {existing.name}. Remove it and try again.")
    run_dir.mkdir(parents=True, exist_ok=True)
    config = storage_path(project_file, "runs/test.ini")
    atomic_write(config, b"[Video]\nRenderer=vulkan\nRenderScale=1\nAspectRatio=4:3\nFrameSkip=0\nVSync=1\n")
    storage_path(project_file, "runs/first-race.png").unlink(missing_ok=True)
    args = ["--data", str(disc), "--config", str(config)]
    environment = dict(EOD_MOD_DIR=mod_dir, EOD_MEMCARD=str(storage_path(project_file, "runs/memcard")),
        EOD_DIRECT_START=f"mode=action;track={track};event=No-Rules Race;car={car};paint=0;humans=1",
        EOD_SKIP_MOVIES="1", EOD_EDITOR_TEST="1",
        EOD_EDITOR_PREVIEW=str(storage_path(project_file, "runs/first-race.png")))
    if request.get("headless"):
        args += ["--headless"]
        environment.update(EOD_HEADLESS="1", EOD_MUTE="1")
    if request.get("frames"):
        frames = int(request["frames"])
        if not 1 <= frames <= 36000:
            raise ValueError("Test length must be between 1 and 36,000 frames.")
        args += ["--frames", str(frames)]
    return dict(executable=str(executable), arguments=args, environment=environment,
                logPath=str(storage_path(project_file, "runs/game.log")), car=car, track=track, carNote=car_note)


def track_name(archive):
    stem = Path(archive).stem.upper()
    name = next((name for name in TRACK_NAMES if (name.upper() if len(name) <= 8 else name[:6].upper() + "_1") == stem), None)
    if name is None:
        raise ValueError(f"{stem} can't be raced in a test yet. Choose another track.")
    return name


def texture_edit_bytes(edit, original):
    if not isinstance(edit, dict) or edit.get("kind") != "texture":
        raise ValueError("A saved texture change isn't in a form this version can read. Discard it and edit the texture again.")
    try:
        data = base64.b64decode(edit["data"], validate=True)
    except (ValueError, KeyError) as error:
        raise ValueError("A saved texture change is damaged. Discard it and edit the texture again.") from error
    if sha(data) != edit.get("sha256") or len(data) != len(original) or data[:128] != original[:128]:
        raise ValueError("A saved texture change no longer matches the game's texture. Discard it and edit the texture again.")
    decode_preview(data)
    return data


def texture_record(data):
    return dict(kind="texture", data=base64.b64encode(data).decode("ascii"), sha256=sha(data))


def environment(request, disc):
    archive = archive_path(disc, request["archive"])
    if archive.suffix.lower() != ".trk":
        raise ValueError("Choose a track archive (.TRK).")
    entries = directory(archive)
    return dict(track=track_name(request["archive"]),
                textures=[e["name"] for e in entries if e["type"] == "TEX"],
                skyTextures=[e["name"] for e in entries if re.fullmatch(r"sky[0-9]{2}[AB]\.tex", e["name"], re.IGNORECASE)],
                worldGeometryEditable=False)


def prepare_texture(request, disc):
    from texture_edit import import_png, tint_texture
    source = archive_path(disc, request["archive"])
    entries = directory(source)
    names = environment(request, disc)["skyTextures"] if request.get("allSky") else [request["asset"]]
    if not names:
        raise ValueError("This track has no sky textures to tint.")
    edits = {}
    cache = storage_path(project_path(request, disc), ".cache")
    for name in names:
        original, entry = payload(source, name, entries, verify_crc=False)
        if entry["type"] != "TEX":
            raise ValueError("That file isn't a texture. Choose a texture from the list.")
        existing = request.get("textureEdits", {}).get(name)
        if not existing:
            selected = dict(request, asset=name)
            existing = saved_edits(selected, disc, sha(source.read_bytes()))
        data = texture_edit_bytes(existing, original) if existing else original
        if "png" in request:
            data = import_png(data, Path(request["png"]).expanduser().resolve())
        else:
            data = tint_texture(data, tuple(request["tint"]))
        edits[name] = texture_record(data)
    preview_asset = request["asset"] if request["asset"] in edits else names[0]
    image, meta = texture(texture_edit_bytes(edits[preview_asset], payload(source, preview_asset, entries, verify_crc=False)[0]), cache)
    return dict(edits=edits, image=image, previewAsset=preview_asset, count=len(edits))


def export_texture(request, disc):
    from texture_edit import export_png
    destination = Path(request["destination"]).expanduser().resolve()
    if inside(destination, disc):
        raise ValueError("Choose a destination outside the game folder so the original files stay untouched.")
    data, _ = payload(archive_path(disc, request["archive"]), request["asset"], verify_crc=False)
    edit = request.get("textureEdit") or saved_edits(request, disc)
    if edit:
        data = texture_edit_bytes(edit, data)
    destination.parent.mkdir(parents=True, exist_ok=True)
    export_png(data, destination)
    return dict(path=str(destination), bytes=destination.stat().st_size, sha256=sha(destination.read_bytes()))


# ---------------------------------------------------------------- texture pipeline

def _dest_checked(disc, destination):
    import texture_export
    try:
        return texture_export.check_dest(disc, destination)
    except texture_export.ExportError as error:
        raise ValueError(str(error)) from None


def _job_paths(request, dest):
    root = Path(request.get("jobsRoot") or JOBS_ROOT).expanduser().resolve()
    job = hashlib.sha256(str(dest).encode()).hexdigest()[:16]
    return job, root / (job + ".json"), root / (job + ".progress"), root / (job + ".summary")


def _alive(pid):
    try:
        done, _ = os.waitpid(pid, os.WNOHANG)
        if done:
            return False
    except ChildProcessError:
        pass
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def _last_events(path):
    events, error = {}, None
    try:
        with path.open("rb") as handle:
            handle.seek(0, os.SEEK_END)
            handle.seek(max(0, handle.tell() - 65536))
            lines = handle.read().decode("utf-8", "replace").splitlines()
        with path.open("rb") as handle:
            first = handle.readline().decode("utf-8", "replace")
        lines.insert(0, first)
    except OSError:
        return events, error
    for line in lines:
        try:
            item = json.loads(line)
        except ValueError:
            continue
        if not isinstance(item, dict):
            continue
        if "error" in item:
            error = str(item["error"])
        if "event" in item:
            events[item["event"]] = item
    return events, error


def export_batch_start(request, disc):
    dest = _dest_checked(disc, request["destination"])
    job, record_path, progress_path, summary_path = _job_paths(request, dest)
    current = export_batch_status(request, disc)
    if current["state"] == "running":
        raise ValueError("An export to this folder is still running. Wait for it to finish, or choose another folder.")
    resume = bool(request.get("resume"))
    if (dest / "export-manifest.json").exists() and not resume:
        raise ValueError("This folder already contains an export. Resume it, or choose another folder.")
    command = [sys.executable, str(Path(__file__).resolve().parent / "texture_export.py"),
               "--disc", str(disc), "--dest", str(dest)]
    for glob in request.get("select") or []:
        command += ["--select", glob]
    for archive in request.get("archives") or []:
        command += ["--archive", archive]
    if request.get("role"):
        command += ["--role", request["role"]]
    if request.get("jobs"):
        command += ["--jobs", str(int(request["jobs"]))]
    if resume:
        command.append("--resume")
    record_path.parent.mkdir(parents=True, exist_ok=True)
    summary_path.unlink(missing_ok=True)
    import subprocess
    with progress_path.open("wb") as err, summary_path.open("wb") as out:
        child = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=out, stderr=err, start_new_session=True,
                                 cwd=str(record_path.parent))
    child.returncode = 0  # detached on purpose; liveness is tracked by pid, not by this handle
    atomic_write(record_path, json.dumps(dict(jobId=job, pid=child.pid, destination=str(dest), disc=str(disc),
                                              resume=resume, cancelRequested=False)).encode())
    return dict(jobId=job, pid=child.pid, destination=str(dest), resumed=resume)


def export_batch_status(request, disc):
    dest = _dest_checked(disc, request["destination"])
    job, record_path, progress_path, summary_path = _job_paths(request, dest)
    manifest = dest / "export-manifest.json"
    result = dict(jobId=job, destination=str(dest), state="idle", done=0, total=0, assetId=None,
                  resumable=manifest.is_file(), manifest=str(manifest) if manifest.is_file() else None)
    if manifest.is_file():
        try:
            run = json.loads(manifest.read_text()).get("run", {})
            result.update(done=run.get("done", 0), total=run.get("total", 0), previousRun=run.get("status"))
        except (OSError, ValueError):
            pass
    try:
        record = json.loads(record_path.read_text())
    except (OSError, ValueError):
        return result
    events, error = _last_events(progress_path)
    if "start" in events:
        result["total"] = events["start"].get("total", 0)
    if "progress" in events:
        result.update(done=events["progress"].get("done", 0), assetId=events["progress"].get("assetId"))
    try:
        summary = json.loads(summary_path.read_text())
    except (OSError, ValueError):
        summary = None
    if summary:
        result.update(state=summary.get("status", "complete"), summary={k: v for k, v in summary.items()},
                      exported=summary.get("exported", 0), failed=summary.get("failed", 0),
                      unsupported=summary.get("unsupported", 0), failures=summary.get("failures", []),
                      done=result["total"] or result["done"] if summary.get("status") == "complete" else result["done"])
        result["resumable"] = True
    elif _alive(record["pid"]):
        result["state"] = "running"
        result["cancelRequested"] = record.get("cancelRequested", False)
    else:
        result.update(state="cancelled" if record.get("cancelRequested") else "failed",
                      error=error or "The export stopped before it finished. Resume it to continue.")
    return result


def export_batch_cancel(request, disc):
    dest = _dest_checked(disc, request["destination"])
    job, record_path, _, _ = _job_paths(request, dest)
    try:
        record = json.loads(record_path.read_text())
    except (OSError, ValueError):
        raise ValueError("No export is running for this folder, so there is nothing to cancel.") from None
    if not _alive(record["pid"]):
        return export_batch_status(request, disc)
    record["cancelRequested"] = True
    atomic_write(record_path, json.dumps(record).encode())
    os.kill(record["pid"], 15)
    return dict(export_batch_status(request, disc), cancelRequested=True)


def _retail_texture_record(disc, archive, asset):
    from mod_package import retail_member
    from asset_registry import analyze_tex
    data, entry, _, spelled = retail_member(disc, archive, asset)
    if entry["type"] != "TEX":
        raise ValueError(f"{entry['name']} isn't a texture, so it can't be exported or replaced. Choose a texture.")
    info, _ = analyze_tex(data)
    if info.get("status") != "ok":
        raise PlainError("This texture can't be read, so it can't be exported. The game file is unchanged.", str(info.get("error", "texture decode failed")))
    return dict(assetId=f"{spelled}#{entry['name']}", status="exported", width=info["width"], height=info["height"],
                reimport=info.get("reimport", []))


def reimport_check(request, disc):
    import texture_export
    png = Path(request["png"]).expanduser().resolve()
    if request.get("manifest"):
        manifest = json.loads(Path(request["manifest"]).expanduser().read_text())
        asset_id = request["archive"] + "#" + request["asset"]
    else:
        record = _retail_texture_record(disc, request["archive"], request["asset"])
        manifest, asset_id = dict(assets=[record]), record["assetId"]
    result = texture_export.classify_reimport(manifest, asset_id, png)
    k = result["scale"]
    result["mode"] = ("native" if result["nativeSameSize"] else "host" if result["hostReplacement"] else None)
    if result["mode"] == "host":
        result["result"] = dict(width=result["png"]["width"], height=result["png"]["height"], scale=k)
    return result


def _host_map(project):
    hosts = project.get("hostTextures")
    return hosts if isinstance(hosts, dict) else {}


def import_host(request, disc):
    import mod_package as mp
    from tex import read_png
    project_file = project_path(request, disc)
    data, entry, _, spelled = mp.retail_member(disc, request["archive"], request["asset"])
    asset_id = f"{spelled}#{entry['name']}"
    if entry["type"] != "TEX":
        raise ValueError(f"{entry['name']} isn't a texture, so it can't be exported or replaced. Choose a texture.")
    ident = mp.host_identity(data, asset_id)
    png = Path(request["png"]).expanduser().resolve()
    width, height = mp.png_size(png)
    k = mp.replacement_scale(width, height, ident["width"], ident["height"])
    try:
        if tuple(read_png(str(png))[:2]) != (width, height):
            raise ValueError("That PNG file is damaged. Export it again from your image editor and retry.")
    except (OSError, struct.error) as error:
        raise PlainError("That image couldn't be read. Choose a PNG file.", str(error)) from None
    image = png.read_bytes()
    project = load_project(project_file) if project_file.exists() else dict(version=1, disc=str(disc), archives={})
    if Path(project["disc"]).resolve() != disc:
        raise ValueError("This project was made for a different game folder. Open it with its original game folder, or start a new project.")
    relative = f"host/{spelled}/{entry['name']}.png"
    target = storage_path(project_file, relative)
    hosts = dict(_host_map(project))
    hosts[asset_id] = dict(image=relative, sha256=sha(image), k=k, width=width, height=height)
    project["hostTextures"] = hosts
    previous = target.read_bytes() if target.exists() else None
    atomic_write(target, image)
    try:
        atomic_write(project_file, (json.dumps(project, indent=2) + "\n").encode())
    except BaseException:
        target.unlink(missing_ok=True) if previous is None else atomic_write(target, previous)
        raise
    return dict(assetId=asset_id, **hosts[asset_id])


def remove_host(request, disc):
    project_file = project_path(request, disc)
    project = load_project(project_file)
    hosts = dict(_host_map(project))
    key = next((k for k in hosts if k.lower() == (request["archive"] + "#" + request["asset"]).lower()), None)
    if key is None:
        raise ValueError("This texture has no high-resolution replacement to remove.")
    record = hosts.pop(key)
    project["hostTextures"] = hosts
    atomic_write(project_file, (json.dumps(project, indent=2) + "\n").encode())
    storage_path(project_file, record["image"]).unlink(missing_ok=True)
    return dict(assetId=key, remaining=len(hosts))


def _next_version(folder):
    best = 0
    if folder.is_dir():
        for child in folder.iterdir():
            found = re.fullmatch(r"[a-z0-9._-]+-1\.0\.([0-9]+)", child.name)
            if found:
                best = max(best, int(found.group(1)))
    return f"1.0.{best + 1}"


def package_project(request, disc):
    import mod_package as mp
    import shutil
    project_file = project_path(request, disc)
    project = load_project(project_file)
    hosts = _host_map(project)
    if not project["archives"] and not hosts:
        raise ValueError("There is nothing to package yet. Choose Save & Build on an edit first.")
    slug = re.sub(r"[^a-z0-9._-]+", "-", project_file.parent.name.lower()).strip("-._") or "mod"
    package_id = request.get("id") or slug[:64]
    packages = storage_path(project_file, "packages")
    version = request.get("version") or _next_version(packages)
    out = Path(request["out"]).expanduser().resolve() if request.get("out") else packages / f"{package_id}-{version}"
    name = request.get("name") or project_file.parent.name
    for key, record in hosts.items():
        image = storage_path(project_file, record["image"])
        if not image.is_file() or sha(image.read_bytes()) != record["sha256"]:
            raise ValueError(f"The saved replacement image for {key} is missing or was changed. Replace the texture again.")
    created = not out.exists()
    try:
        if project["archives"]:
            info = mp.from_project(project_file, disc, package_id, version, out, name)
        else:
            info = mp.init(out, disc, package_id, version, name)
        for key, record in sorted(hosts.items()):
            info = mp.add_host(out, disc, key, storage_path(project_file, record["image"]))
    except BaseException:
        if created:
            shutil.rmtree(out, ignore_errors=True)
        raise
    return dict(path=str(out), id=package_id, version=version, native=info["native"],
                host=len(hosts), info=info)


def _profile_args(request):
    return request.get("profile") or DEV_PROFILE, request.get("profileRoot")


def profile_status(request, disc):
    import mod_install
    name, root = _profile_args(request)
    return mod_install.status(name, root)


def profile_install(request, disc):
    import mod_install
    name, root = _profile_args(request)
    return mod_install.install(request["package"], str(disc), name, root, enable=request.get("enable", True))


def profile_change(request, disc, action):
    import mod_install
    name, root = _profile_args(request)
    return getattr(mod_install, action)(request["id"], name, root, str(disc))


def profile_accept_conflict(request, disc):
    import mod_install
    name, root = _profile_args(request)
    return mod_install.accept_conflict(request["key"], request["winner"], name, root, str(disc))


def dispatch(request):
    action = request["action"]
    if action == "load":
        path = Path(request["project"]).expanduser().resolve()
        project = load_project(path)
        project_path(request, Path(project["disc"]).resolve())
        return project
    disc = Path(request["disc"]).expanduser().resolve()
    if not (disc / "DATA").is_dir():
        raise ValueError("That folder isn't the extracted game. Choose the folder that contains DATA and SLUS_209.10.")
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
    if action == "environment":
        return environment(request, disc)
    if action == "prepare_texture":
        return prepare_texture(request, disc)
    if action == "export_texture":
        return export_texture(request, disc)
    if action == "entries":
        path = archive_path(disc, request["archive"])
        return dict(entries=directory(path))
    if action == "inspect":
        return inspect(request, disc)
    if action == "export_native":
        destination = Path(request["destination"]).expanduser().resolve()
        if inside(destination, disc):
            raise ValueError("Choose a destination outside the game folder so the original files stay untouched.")
        data, _ = payload(archive_path(disc, request["archive"]), request["asset"], verify_crc=False)
        atomic_write(destination, data)
        return dict(path=str(destination), bytes=len(data), sha256=sha(data))
    if action == "save":
        return save(request, disc)
    if action == "save_format":
        return save_format(request, disc)
    if action == "validate":
        return validate(request, disc)
    if action == "launch_plan":
        return launch_plan(request, disc)
    if action == "launch_profile":
        return launch_profile(request, disc)
    if action in ("export_batch_start", "export_batch_status", "export_batch_cancel"):
        return globals()[action](request, disc)
    if action in ("reimport_check", "import_host", "remove_host", "package_project", "profile_status",
                  "profile_install", "profile_accept_conflict"):
        return globals()[action](request, disc)
    if action in ("profile_enable", "profile_disable", "profile_uninstall"):
        return profile_change(request, disc, action[len("profile_"):])
    raise PlainError("The Creation Kit asked its helper for something it doesn't support. Update the app.", "unknown action: " + str(action))


def main():
    try:
        request = json.load(sys.stdin)
        result = dispatch(request)
        print(json.dumps(dict(ok=True, result=result), allow_nan=False))
    except Exception as error:
        if isinstance(error, json.JSONDecodeError):
            message = "The Creation Kit sent a request its helper couldn't read. Restart the app and try again."
            detail = str(error)
        elif isinstance(error, ValueError):
            message, detail = str(error), getattr(error, "detail", None)
        elif isinstance(error, OSError):
            message = "A file couldn't be read or written. Check that the folder is available and has free space, then try again."
            detail = f"{type(error).__name__}: {error}"
        else:
            message = "The Creation Kit couldn't finish this action. Nothing was changed. Try again, or reopen the project."
            detail = f"{type(error).__name__}: {error}"
        print(json.dumps(dict(ok=False, error=message, detail=detail)))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
