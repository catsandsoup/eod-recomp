#!/usr/bin/env python3
"""Batch export of native TEX mip zero to PNG plus export-manifest.json.

Contract: docs/CREATION_KIT_TEXTURE_CONTRACTS.md section 3. Nothing is ever
written inside the disc; existing files with different content are never
overwritten except by --resume for the same asset.

    python3 tools/texture_export.py --disc DISC --dest DIR [--select GLOB ...]
        [--archive PATH ...] [--role ROLE] [--dry-run] [--resume] [--jobs N]

Progress is JSON lines on stderr, the final summary JSON goes to stdout.
Exit status: 0 all exported, 1 some failed/unsupported, 2 refused, 130/143 cancelled.
"""
import argparse
import fnmatch
import hashlib
import json
import os
import signal
import struct
import sys
import threading
import time
import unicodedata
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rst0 import eod_crc
from asset_registry import (analyze_tex, classify_role, inventory, link_records, link_runtime_keys,
                            member_record, run_tasks, sha256)
from tex import write_png

SCHEMA_VERSION = 1
TOOL = "texture_export"
TOOL_VERSION = "1"
MANIFEST = "export-manifest.json"
ROLES = ("sky", "vehicle-paint", "vehicle", "track", "ui", "font", "other")
MAX_HOST_SIDE = 4096


class ExportError(Exception):
    """A refusal: bad destination, existing manifest without --resume, ..."""


def _fold(text):
    return unicodedata.normalize("NFD", text).casefold()


def _now():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def atomic_write(path, data):
    """Temp file + fsync + rename in the target directory (mode 0644)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.parent / f".eod-{os.getpid()}-{threading.get_ident()}.tmp"
    try:
        with open(temporary, "wb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def check_dest(disc, dest):
    disc = Path(disc).resolve()
    resolved = Path(dest).expanduser().resolve()
    if resolved == disc or disc in resolved.parents:
        raise ExportError("Destination must not be inside the disc")
    if resolved.exists() and not resolved.is_dir():
        raise ExportError("Destination is not a directory")
    return resolved


def output_relative(archive, member):
    """Relative POSIX output path; raises ValueError for unsafe names."""
    if "/" in member:
        raise ValueError(f"Unsafe path component {member!r}")
    for part in [*archive.split("/"), member]:
        if part in ("", ".", "..") or "\\" in part or any(ord(c) < 32 or ord(c) == 127 for c in part):
            raise ValueError(f"Unsafe path component {part!r}")
    return f"textures/{archive}/{member}.png"


def safe_target(dest, relative):
    """Absolute path under dest; refuses '..', absolute paths and symlinked parents."""
    parts = relative.split("/")
    if relative.startswith("/") or any(p in ("", ".", "..") for p in parts):
        raise ValueError("Output path escapes the destination")
    current = dest
    for part in parts:
        current = current / part
        if current.is_symlink():
            raise ValueError("Output path crosses a symbolic link")
    if dest not in current.resolve().parents:
        raise ValueError("Output path escapes the destination")
    return current


def _file_sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


# ---------------------------------------------------------------- worker (process pool)

def export_worker(task):
    """Decode one TEX and write its PNG atomically. Never raises."""
    assetid = task["assetId"]
    try:
        with open(task["path"], "rb") as handle:
            handle.seek(task["offset"])
            data = handle.read(task["size"])
        crc_ok = eod_crc(data) == task["crc"]
        info, pixels = analyze_tex(data)
        result = dict(assetId=assetid, crcOk=crc_ok, info=info)
        if info["status"] != "ok":
            return result
        target = Path(task["target"])
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = target.parent / f".eod-{os.getpid()}-{sum(map(ord, assetid)) % 100000}.tmp"
        try:
            write_png(str(temporary), info["width"], info["height"], pixels)
            new_hash = _file_sha(temporary)
            action = "written"
            if target.is_symlink():
                raise ExportError("Output is a symbolic link")
            if target.exists():
                if _file_sha(target) == new_hash:
                    action = "unchanged"
                elif task["allowOverwrite"]:
                    action = "overwritten"
                else:
                    raise ExportError("Existing file with different content was not overwritten")
            if action != "unchanged":
                os.replace(temporary, target)
        finally:
            if temporary.exists():
                temporary.unlink()
        result.update(outputSha256=new_hash, action=action)
    except Exception as error:
        result = dict(assetId=assetid, crcOk=None, info=dict(status="failed"), error=str(error) or type(error).__name__)
    return result


# ------------------------------------------------------------------- planning

def _resolve_archives(files, requested, disc):
    known = {_fold(f["path"]): f["path"] for f in files if f["kind"] == "0tsr-v6" and not f["parseError"]}
    chosen = set()
    for item in requested:
        text = str(item).replace("\\", "/")
        if os.path.isabs(text):
            try:
                text = Path(text).resolve().relative_to(disc).as_posix()
            except ValueError:
                raise ExportError(f"Archive is outside the disc: {item}")
        match = known.get(_fold(text)) or known.get(_fold("DATA/" + text))
        if not match:
            raise ExportError(f"Unknown archive: {item}")
        chosen.add(match)
    return chosen


def _load_prior(manifest_path):
    prior = json.loads(manifest_path.read_text())
    if prior.get("schemaVersion") != SCHEMA_VERSION or not isinstance(prior.get("assets"), list):
        raise ExportError("Existing export-manifest.json is not schemaVersion 1")
    return prior


def build_plan(disc, dest, select, archives, role, resume):
    """Returns (records, tasks-by-id, prior_manifest, slus). Records carry `_plan`."""
    if role is not None and role not in ROLES:
        raise ExportError(f"Unknown role {role!r}; use one of {', '.join(ROLES)}")
    manifest_path = dest / MANIFEST
    prior = None
    if manifest_path.exists():
        if not resume:
            raise ExportError("Destination already holds an export manifest; use --resume to continue it")
        prior = _load_prior(manifest_path)
    files, members = inventory(disc)
    chosen = _resolve_archives(files, archives, disc) if archives else None
    everything = [member_record(m) for m in members]
    link_records(everything)
    by_id = {m["assetId"]: m for m in members}
    patterns = [_fold(p) for p in select]
    records = []
    for record in everything:
        if record["type"] != "TEX":
            continue
        if chosen is not None and record["archive"] not in chosen:
            continue
        if role is not None and record["role"] != role:
            continue
        if patterns and not any(fnmatch.fnmatchcase(_fold(record["assetId"]), p) for p in patterns):
            continue
        record["_member"] = by_id[record["assetId"]]
        records.append(record)
    # Output paths, case-folded: colliding assets are all refused (order must not pick a winner).
    seen = {}
    for record in records:
        try:
            record["output"] = output_relative(record["archive"], record["member"])
            safe_target(dest, record["output"])
        except ValueError as error:
            record["_fail"] = str(error)
            continue
        seen.setdefault(_fold(record["output"]), []).append(record)
    for group in seen.values():
        if len(group) > 1:
            for record in group:
                others = ", ".join(r["assetId"] for r in group if r is not record)
                record["_fail"] = f"Output path collides (case-folded) with {others}"
    slus = Path(disc) / "SLUS_209.10"
    return records, prior, (sha256(slus.read_bytes()) if slus.is_file() else None)


def _finish(record, status, error=None):
    record["status"] = status
    record["error"] = error
    record.setdefault("warnings", [])
    record["_done"] = True


def _public(record):
    return {k: v for k, v in record.items() if not k.startswith("_")}


def _manifest(records, slus, created, run, carried=()):
    finished = [_public(r) for r in records if r.get("_done")]
    counts = {s: sum(1 for a in finished + list(carried) if a["status"] == s) for s in ("exported", "failed", "unsupported")}
    return dict(schemaVersion=SCHEMA_VERSION, tool=TOOL, toolVersion=TOOL_VERSION, disc=dict(slusSha256=slus),
                created=created, updated=_now(), summary=dict(total=len(finished) + len(carried), **counts),
                run=run, assets=finished + list(carried))


# --------------------------------------------------------------------- export

def export_batch(disc, dest, select=(), archives=(), role=None, dry_run=False, resume=False, jobs=1,
                 progress=None, cancel=None, checkpoint_every=50, checkpoint_seconds=2.0):
    """Export mip-zero PNGs. Returns the manifest dict (also written unless dry_run).

    progress(event) receives {"event": "start"|"progress"|"finish", ...}; progress
    events carry done/total/assetId. cancel (threading.Event) stops cleanly: the
    manifest is written with run.status "cancelled" and lists finished assets."""
    started = time.time()
    disc = Path(disc).resolve()
    dest = check_dest(disc, dest)
    cancel = cancel or threading.Event()
    emit = progress or (lambda event: None)
    records, prior, slus = build_plan(disc, dest, select, archives, role, resume)
    if prior and prior.get("disc", {}).get("slusSha256") != slus:
        raise ExportError("Existing manifest was exported from a different executable (slusSha256 differs)")
    prior_by_id = {_fold(a["assetId"]): a for a in (prior or {}).get("assets", [])}
    created = (prior or {}).get("created") or _now()
    planned = {_fold(r["assetId"]) for r in records}
    carried = [a for k, a in prior_by_id.items() if k not in planned]  # earlier outputs outside this selection
    todo, resumed, plan_rows = [], 0, []
    for record in records:
        if "_fail" in record:
            _finish(record, "failed", record["_fail"])
            plan_rows.append(dict(assetId=record["assetId"], action="refuse", error=record["_fail"]))
            continue
        old = prior_by_id.get(_fold(record["assetId"]))
        target = safe_target(dest, record["output"])
        if (old and old.get("status") == "exported" and old.get("memberSha256") == record["memberSha256"]
                and old.get("output") == record["output"] and target.is_file()
                and _file_sha(target) == old.get("outputSha256")):
            record.update({k: v for k, v in old.items() if k not in ("duplicateOf", "variantOf", "runtimeKeyShared", "indexKeyShared")})
            record["_done"], record["written"] = True, "skipped-resumed"
            resumed += 1
            plan_rows.append(dict(assetId=record["assetId"], action="skip-resumed"))
            continue
        record["_target"] = target
        record["_overwrite"] = bool(old and old.get("output") == record["output"])
        todo.append(record)
        plan_rows.append(dict(assetId=record["assetId"], action="export", output=record["output"]))
    total = len(records)
    if dry_run:
        return dict(dryRun=True, total=total, toExport=len(todo), skipResumed=resumed,
                    refused=sum(1 for r in records if "_fail" in r), assets=plan_rows)

    dest.mkdir(parents=True, exist_ok=True)
    if resume:
        for stale in (dest / "textures").rglob(".eod-*.tmp") if (dest / "textures").is_dir() else ():
            stale.unlink()
    done = [0]
    emit(dict(event="start", total=total, toExport=len(todo), skipResumed=resumed))
    for record in records:
        if record.get("_done"):
            done[0] += 1
            emit(dict(event="progress", done=done[0], total=total, assetId=record["assetId"]))

    state = dict(last=time.time(), since=0)

    def run_info(status):
        return dict(status=status, total=total, done=done[0], resumed=resumed, seconds=round(time.time() - started, 1),
                    select=list(select), archives=list(archives), role=role, jobs=jobs)

    def checkpoint(status="running"):
        atomic_write(dest / MANIFEST, json.dumps(_manifest(records, slus, created, run_info(status), carried), indent=1).encode())
        state.update(last=time.time(), since=0)

    checkpoint()
    by_id = {r["assetId"]: r for r in todo}
    tasks = [dict(assetId=r["assetId"], path=r["_member"]["path"], offset=r["_member"]["offset"], size=r["size"],
                  crc=r["crc"], target=str(r["_target"]), allowOverwrite=r["_overwrite"]) for r in todo]

    def on_result(task, result):
        record = by_id[result["assetId"]]
        info = result["info"]
        warnings = list(info.pop("warnings", [])) + ([] if result["crcOk"] in (True, None) else ["crc-mismatch"])
        status = info.pop("status")
        error = result.get("error") or info.pop("error", None)
        record["crcOk"] = result["crcOk"]
        record["warnings"] = warnings
        for key in ("width", "height", "levels", "format", "flags", "bitsPerPixel", "decodedRgbaSha256", "alpha",
                    "indexKey", "paletteKey", "reimport"):
            if key in info:
                record[key] = info[key]
        if status == "ok" and "outputSha256" in result:
            record["outputSha256"] = result["outputSha256"]
            record["written"] = result["action"]
            _finish(record, "exported")
        else:
            _finish(record, status if status in ("unsupported", "failed") else "failed", error or "no output")
            if status == "ok":
                record["status"] = "failed"
        done[0] += 1
        state["since"] += 1
        emit(dict(event="progress", done=done[0], total=total, assetId=record["assetId"]))
        if state["since"] >= checkpoint_every or time.time() - state["last"] >= checkpoint_seconds:
            checkpoint()

    run_tasks(export_worker, tasks, jobs, cancel, on_result)
    cancelled = cancel.is_set() and done[0] < total
    link_runtime_keys([r for r in records if r.get("_done")])
    status = "cancelled" if cancelled else "complete"
    manifest = _manifest(records, slus, created, run_info(status), carried)
    atomic_write(dest / MANIFEST, json.dumps(manifest, indent=1).encode())
    emit(dict(event="finish", status=status, done=done[0], total=total, summary=manifest["summary"]))
    return manifest


# --------------------------------------------------------------------- reimport

def png_size(path):
    """(width, height) from the IHDR of a PNG without decoding it."""
    with open(path, "rb") as handle:
        head = handle.read(24)
    if len(head) < 24 or head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        raise ValueError("Not a PNG file")
    return struct.unpack(">II", head[16:24])


def classify_reimport(manifest, asset, png):
    """Which reimport modes an edited PNG qualifies for. Classification only.

    native-same-size: dimensions equal the original and the texture is natively
    editable. host-replacement: indexed texture (has runtime keys) and
    dimensions are origW*k by origH*k, k a power of two, each side <= 4096."""
    if not isinstance(manifest, dict):
        manifest = json.loads(Path(manifest).read_text())
    found = next((a for a in manifest["assets"] if _fold(a["assetId"]) == _fold(asset)), None)
    if found is None or found.get("status") != "exported":
        raise ValueError(f"Asset is not an exported asset in this manifest: {asset}")
    width, height = png_size(png)
    ow, oh = found["width"], found["height"]
    modes = found.get("reimport", [])
    reasons = {}
    native = (width, height) == (ow, oh) and "native-same-size" in modes
    if (width, height) != (ow, oh):
        reasons["native-same-size"] = "dimensions differ from the original"
    elif "native-same-size" not in modes:
        reasons["native-same-size"] = "texture is not natively editable"
    scale = None
    if "host-replacement" not in modes:
        reasons["host-replacement"] = "no runtime identity key (direct-colour or undecodable)"
    elif width % ow or height % oh or width // ow != height // oh:
        reasons["host-replacement"] = "not a uniform integer multiple of the original"
    else:
        k = width // ow
        if k & (k - 1):
            reasons["host-replacement"] = f"scale {k} is not a power of two"
        elif max(width, height) > MAX_HOST_SIDE:
            reasons["host-replacement"] = f"{width}x{height} exceeds {MAX_HOST_SIDE} per side"
        else:
            scale = k
    return dict(assetId=found["assetId"], png=dict(width=width, height=height), original=dict(width=ow, height=oh),
                nativeSameSize=native, hostReplacement=scale is not None, scale=scale, reasons=reasons)


# -------------------------------------------------------------------------- CLI

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--disc")
    parser.add_argument("--dest")
    parser.add_argument("--select", action="append", default=[], metavar="GLOB", help="glob on assetId (case-insensitive)")
    parser.add_argument("--archive", action="append", default=[], metavar="PATH")
    parser.add_argument("--role", choices=ROLES)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    parser.add_argument("--reimport-check", nargs=3, metavar=("MANIFEST", "ASSETID", "PNG"))
    args = parser.parse_args(argv)
    if args.reimport_check:
        try:
            print(json.dumps(classify_reimport(*args.reimport_check), indent=1))
            return 0
        except (OSError, ValueError, KeyError) as error:
            print(json.dumps(dict(error=str(error))), file=sys.stderr)
            return 2
    if not args.disc or not args.dest:
        parser.error("--disc and --dest are required")
    cancel = threading.Event()
    signalled = []

    def stop(number, _frame):
        if signalled:
            os._exit(128 + number)
        signalled.append(number)
        cancel.set()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    def emit(event):
        print(json.dumps(event), file=sys.stderr, flush=True)

    try:
        result = export_batch(args.disc, args.dest, args.select, args.archive, args.role, args.dry_run, args.resume,
                              args.jobs, emit, cancel)
    except ExportError as error:
        print(json.dumps(dict(error=str(error))), file=sys.stderr)
        return 2
    if result.get("dryRun"):
        print(json.dumps({k: v for k, v in result.items() if k != "assets"}, indent=1))
        return 0
    summary = dict(status=result["run"]["status"], manifest=str(Path(args.dest).expanduser().resolve() / MANIFEST),
                   seconds=result["run"]["seconds"], resumed=result["run"]["resumed"], **result["summary"],
                   written=sum(1 for a in result["assets"] if a.get("written") == "written"),
                   unchanged=sum(1 for a in result["assets"] if a.get("written") == "unchanged"),
                   failures=[dict(assetId=a["assetId"], error=a["error"]) for a in result["assets"]
                             if a["status"] != "exported"][:50])
    print(json.dumps(summary, indent=1))
    if signalled:
        return 128 + signalled[0]
    return 0 if summary["failed"] == 0 and summary["unsupported"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
