#!/usr/bin/env python3
"""Census of every archive and member on a retail disc (assetId contract v1).

Identity, duplicate and runtime-key semantics follow
docs/CREATION_KIT_TEXTURE_CONTRACTS.md sections 1-2. The census never fails on
a CRC mismatch (it records `crcOk`) and never writes inside the disc.

    python3 tools/asset_registry.py --disc DISC --out census.json [--jobs N]
"""
import argparse
import collections
import concurrent.futures as futures
import contextlib
import hashlib
import json
import mmap
import multiprocessing
import os
import re
import signal
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from modding_backend import archive_path, directory
from rst0 import eod_crc
from texture_identity import texture_identity
from texture_preview import _source_lanes, decode_preview

SCHEMA_VERSION = 1
ARCHIVE_EXTENSIONS = {".car", ".res", ".trk"}
# Raster-looking FourCCs that this tooling does not decode or export. STMP is a
# guess from the name; none of these formats has been analysed here.
RASTER_LIKE = {"IMAG", "CANV", "GTEX", "ICON", "STMP"}
SKY_RE = re.compile(r"sky\d\d[AB]\.tex", re.IGNORECASE)
UI_ARCHIVES = {"UIMEM", "UIBIG", "UIACTION", "UIEDIT", "UI3D", "UICAR", "OVERLAYS", "LOCALE",
               "VICTORY", "RACEDAY", "HOBBYSET", "PAINTKIT", "TRKEDIT", "COMMON", "PART", "GENESIS",
               "RACE", "GHOSTCAR"}


def asset_id(archive, member):
    return f"{archive}#{member}"


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def classify_role(archive, member, kind="TEX"):
    """(role, confidence, reason) from names only; heuristics, not game truth."""
    stem = Path(archive).stem.upper()
    ext = Path(archive).suffix.lower()
    name = member.lower()
    if ext == ".trk" and SKY_RE.fullmatch(member):
        return "sky", "high", "track member named sky<NN><A|B>.tex"
    if re.search(r"font|fnt", name):
        return "font", "low", "member name mentions font"
    if stem == "CT" and ext == ".res" and re.fullmatch(r"\d+\.(tex|tm0)", name):
        return "vehicle-paint", "medium", "numbered member of CT.RES"
    if stem == "CT" and ext == ".res" and re.fullmatch(r"\d+[a-z]\.(tex|tm0)", name):
        return "vehicle-paint", "low", "number+letter member of CT.RES (variant naming not verified)"
    if ext == ".car":
        return "vehicle", "medium", "member of a .CAR archive"
    if ext == ".trk":
        return "track", "medium", "member of a .TRK archive"
    if stem.startswith("UI") or stem in UI_ARCHIVES or "menu" in name:
        return "ui", "low", "UI/menu archive or member name"
    return "other", "low", "no heuristic matched"


# ----------------------------------------------------------------------------- TEX

def analyze_tex(data):
    """Decode mip zero and derive identity. Returns (info dict, RGBA bytes or None).

    Never raises: decode problems become status failed/unsupported + error."""
    info = {}
    if len(data) >= 32:
        w, h, levels, fmt, flags = struct.unpack_from("<5I", data)
        info.update(width=w, height=h, levels=levels, format=fmt, flags=flags)
    try:
        width, height, pixels, meta = decode_preview(data)
    except Exception as error:  # malformed assets must not stop a census
        message = str(error) or type(error).__name__
        info.update(status="unsupported" if message.startswith("Unsupported TEX pixel format") else "failed",
                    error=message)
        return info, None
    bits = meta["bits_per_pixel"]
    info.update(status="ok", width=width, height=height, levels=meta["levels"], format=meta["format"],
                flags=meta["flags"], bitsPerPixel=bits, decodedRgbaSha256=sha256(pixels),
                alpha="straight-0-255-from-palette" if bits <= 8 else
                ("opaque-rgb555" if bits == 16 else "straight-0-255-source"))
    reimport, warnings = [], []
    if bits in (4, 8):
        try:
            ident = texture_identity(data)
            info.update(indexKey=ident["indexKey"], paletteKey=ident["paletteKey"])
            reimport.append("host-replacement")
        except Exception as error:
            warnings.append("identity-unavailable: " + str(error))
        try:
            from texture_edit import _layout
            _layout(data)
            for level in range(meta["levels"]):
                mw, mh = max(1, width >> level), max(1, height >> level)
                lane = range(mw * mh) if mh < 32 else _source_lanes(mw, mh, bits)
                if len(set(lane)) != mw * mh or max(lane) >= mw * mh:
                    raise ValueError("upload layout aliases pixels")
            reimport.insert(0, "native-same-size")
        except Exception as error:
            warnings.append("native-reimport-unavailable: " + str(error))
    info.update(reimport=reimport, warnings=warnings)
    return info, pixels


def analyze_member(task):
    """Process-pool worker: CRC check and (for TEX) decode of one member."""
    path, entry, decode, assetid = task
    with open(path, "rb") as handle:
        handle.seek(entry["offset"])
        data = handle.read(entry["size"])
    crc = eod_crc(data)
    out = dict(assetId=assetid, crc=crc, crcOk=crc == entry["crc"])
    if decode and entry["type"] == "TEX":
        info, _ = analyze_tex(data)
        out["tex"] = info
    return out


@contextlib.contextmanager
def _stdlib_first():
    """Spawned workers inherit sys.path; keep tools/ modules from shadowing the stdlib, so
    keep this directory last while a pool lives (it is still importable)."""
    here = str(Path(__file__).resolve().parent)
    saved = list(sys.path)
    sys.path[:] = [p for p in saved if os.path.abspath(p or ".") != here] + [here]
    try:
        yield
    finally:
        sys.path[:] = saved


def _worker_init():
    signal.signal(signal.SIGINT, signal.SIG_IGN)  # the parent owns cancellation
    signal.signal(signal.SIGTERM, signal.SIG_IGN)


def run_tasks(function, tasks, jobs, cancel=None, on_result=None):
    """Run function over tasks (inline for jobs<=1, spawn pool otherwise).

    Yields nothing; calls on_result(task, result) in the parent. Stops
    submitting once cancel is set; running chunks finish and are delivered."""
    if jobs <= 1:
        for task in tasks:
            if cancel is not None and cancel.is_set():
                return
            on_result(task, function(task))
        return
    ctx = multiprocessing.get_context("spawn")
    with _stdlib_first(), futures.ProcessPoolExecutor(max_workers=jobs, mp_context=ctx, initializer=_worker_init) as pool:
        pending = {}
        iterator = iter(tasks)
        done_submitting = False
        while pending or not done_submitting:
            while not done_submitting and len(pending) < jobs * 4:
                if cancel is not None and cancel.is_set():
                    done_submitting = True
                    break
                try:
                    task = next(iterator)
                except StopIteration:
                    done_submitting = True
                    break
                pending[pool.submit(function, task)] = task
            if not pending:
                break
            finished, _ = futures.wait(pending, timeout=0.2, return_when=futures.FIRST_COMPLETED)
            for future in finished:
                on_result(pending.pop(future), future.result())


# ------------------------------------------------------------------- inventory

def _classify_file(disc, relative):
    """Hash a DATA file; parse its directory when it is a 0TSR v6 archive."""
    record = dict(path=relative, size=0, sha256=None, kind="other", members=None, parseError=None)
    entries = []
    try:
        path = archive_path(disc, relative)
        record["size"] = path.stat().st_size
        with path.open("rb") as handle:
            head = handle.read(12)
            handle.seek(0)
            digest = hashlib.sha256()
            for block in iter(lambda: handle.read(1 << 22), b""):
                digest.update(block)
            record["sha256"] = digest.hexdigest()
        expected = path.suffix.lower() in ARCHIVE_EXTENSIONS
        if head[:4] == b"0TSR":
            second = struct.unpack_from("<I", head, 4)[0]
            third = struct.unpack_from("<I", head, 8)[0] if len(head) >= 12 else 0
            record["kind"] = "0tsr-v2" if second == 2 else "0tsr-v6" if third == 6 else "0tsr-other"
            if record["kind"] != "0tsr-v6":
                record["parseError"] = "0TSR variant not supported by the v6 directory reader"
            else:
                entries = directory(path)
                record["members"] = len(entries)
        elif expected:
            record["parseError"] = f"expected 0TSR archive, magic {head[:4]!r}"
    except Exception as error:
        record["parseError"] = str(error) or type(error).__name__
        entries = []
    return record, entries


def inventory(disc):
    """All files under DATA with archive directories. Returns (files, members).

    members: list of dicts (archive, member, type, size, offset, crc, path)."""
    disc = Path(disc).resolve()
    names = []
    for root, dirs, files in os.walk(disc / "DATA"):
        dirs.sort(key=str.casefold)
        for name in sorted(files, key=str.casefold):
            names.append((Path(root) / name).relative_to(disc).as_posix())
    files, members = [], []
    for relative in names:
        record, entries = _classify_file(disc, relative)
        files.append(record)
        if entries:
            path = archive_path(disc, relative)
            with path.open("rb") as handle, mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as view:
                for entry in entries:
                    blob = view[entry["offset"]:entry["offset"] + entry["size"]]
                    members.append(dict(entry, archive=relative, archiveSha256=record["sha256"],
                                        memberSha256=sha256(blob), path=str(path),
                                        assetId=asset_id(relative, entry["name"])))
    return files, members


def link_records(members):
    """duplicateOf / variantOf / runtimeKeyShared over the given records, in place."""
    by_sha = collections.defaultdict(list)
    by_stem = collections.defaultdict(list)
    for m in members:
        by_sha[m["memberSha256"]].append(m["assetId"])
        stem, ext = os.path.splitext(m["member"])
        if m["type"] == "TEX" and ext.lower() in (".tex", ".tm0"):
            by_stem[(m["archive"].casefold(), stem.casefold())].append(m)
    for m in members:
        m["duplicateOf"] = sorted(i for i in by_sha[m["memberSha256"]] if i != m["assetId"])
        stem, ext = os.path.splitext(m["member"])
        key = (m["archive"].casefold(), stem.casefold())
        m["variantOf"] = sorted(o["assetId"] for o in by_stem.get(key, [])
                                if o is not m and os.path.splitext(o["member"])[1].lower() != ext.lower()
                                ) if m["type"] == "TEX" else []
    link_runtime_keys(members)


def link_runtime_keys(members):
    """runtimeKeyShared: other assets with the same indexKey and paletteKey (a
    binding would be ambiguous); indexKeyShared: same indexKey, other palette."""
    pairs, indexes = collections.defaultdict(list), collections.defaultdict(list)
    for m in members:
        if m.get("indexKey"):
            pairs[(m["indexKey"], m["paletteKey"])].append(m["assetId"])
            indexes[m["indexKey"]].append(m)
    for m in members:
        if m.get("indexKey"):
            m["runtimeKeyShared"] = sorted(i for i in pairs[(m["indexKey"], m["paletteKey"])] if i != m["assetId"])
            m["indexKeyShared"] = sorted(o["assetId"] for o in indexes[m["indexKey"]]
                                         if o["paletteKey"] != m["paletteKey"])


def member_record(m):
    """Public (JSON) record of a directory entry; analysis fills in the rest."""
    record = dict(assetId=m["assetId"], archive=m["archive"], member=m["name"], archiveSha256=m["archiveSha256"],
                  memberSha256=m["memberSha256"], type=m["type"], size=m["size"], crc=m["crc"], crcOk=None,
                  duplicateOf=[], variantOf=[])
    if m["type"] == "TEX":
        record["role"], record["roleConfidence"], record["roleReason"] = classify_role(m["archive"], m["name"])
    return record


def merge_analysis(record, result):
    record["crcOk"] = result["crcOk"]
    if "tex" in result:
        tex = dict(result["tex"])
        record["warnings"] = tex.pop("warnings", []) + ([] if result["crcOk"] else ["crc-mismatch"])
        record["decode"] = dict(status=tex.pop("status"), error=tex.pop("error", None))
        record.update(tex)
    elif record["type"] == "TEX":
        record["warnings"] = [] if result["crcOk"] else ["crc-mismatch"]


# ---------------------------------------------------------------------- census

def census(disc, jobs=1, progress=None, cancel=None):
    """Full registry dict. progress(event dict) is called per analysed member."""
    started = time.time()
    disc = Path(disc).resolve()
    files, members = inventory(disc)
    records = {m["assetId"]: member_record(m) for m in members}
    tasks = [(m["path"], dict(name=m["name"], type=m["type"], size=m["size"], offset=m["offset"], crc=m["crc"]),
              True, m["assetId"]) for m in members]
    ids = [m["assetId"] for m in members]
    done = [0]

    def on_result(task, result):
        assetid = result["assetId"]
        merge_analysis(records[assetid], result)
        done[0] += 1
        if progress:
            progress(dict(event="progress", done=done[0], total=len(tasks), assetId=assetid))

    run_tasks(analyze_member, tasks, jobs, cancel, on_result)
    ordered = [records[i] for i in ids]
    link_records(ordered)
    for record in ordered:
        record.pop("path", None)
    slus = disc / "SLUS_209.10"
    return dict(schemaVersion=SCHEMA_VERSION, tool="asset_registry", created=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                disc=dict(slusSha256=sha256(slus.read_bytes()) if slus.is_file() else None),
                cancelled=bool(cancel is not None and cancel.is_set()),
                seconds=round(time.time() - started, 1),
                files=files, assets=ordered, totals=totals(files, ordered), notCovered=not_covered(ordered))


def totals(files, assets):
    tex = [a for a in assets if a["type"] == "TEX"]
    return dict(
        files=len(files), filesByKind=dict(collections.Counter(f["kind"] for f in files)),
        archives=sum(1 for f in files if f["kind"] == "0tsr-v6" and not f["parseError"]),
        archiveParseErrors=[dict(path=f["path"], error=f["parseError"]) for f in files if f["parseError"]],
        members=len(assets), membersByType=dict(sorted(collections.Counter(a["type"] for a in assets).items())),
        crcMismatches=[a["assetId"] for a in assets if a["crcOk"] is False],
        tex=len(tex), texDecode=dict(collections.Counter(a["decode"]["status"] for a in tex if "decode" in a)),
        texDecodeErrors=dict(collections.Counter(a["decode"]["error"] for a in tex if a.get("decode", {}).get("error"))),
        texByFormatBits=dict(collections.Counter(str(a.get("bitsPerPixel")) for a in tex)),
        texByRole=dict(collections.Counter(a["role"] for a in tex)),
        texIndexed=sum(1 for a in tex if a.get("indexKey")),
        texWithDuplicatePayload=sum(1 for a in tex if a["duplicateOf"]),
        texWithSharedRuntimeKey=sum(1 for a in tex if a.get("runtimeKeyShared")),
        texWithSharedIndexKeyOtherPalette=sum(1 for a in tex if a.get("indexKeyShared")),
        texWithVariant=sum(1 for a in tex if a["variantOf"]),
        texNativeReimport=sum(1 for a in tex if "native-same-size" in a.get("reimport", [])),
        texHostReplacement=sum(1 for a in tex if "host-replacement" in a.get("reimport", [])),
        texExtensions=dict(collections.Counter(os.path.splitext(a["member"])[1].lower() for a in tex)))


def not_covered(assets):
    """Raster-looking FourCCs outside TEX; reported, never decoded."""
    groups = collections.defaultdict(list)
    for a in assets:
        if a["type"] in RASTER_LIKE:
            groups[a["type"]].append(a)
    return {t: dict(count=len(v), bytes=sum(a["size"] for a in v), archives=sorted({a["archive"] for a in v}),
                    extensions=sorted({os.path.splitext(a["member"])[1].lower() for a in v}),
                    examples=[a["assetId"] for a in v[:5]]) for t, v in sorted(groups.items())}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--disc", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    args = parser.parse_args(argv)
    out = Path(args.out).resolve()
    disc = Path(args.disc).resolve()
    if out == disc or disc in out.parents:
        parser.error("--out must not be inside the disc")

    def progress(event):
        if event["done"] % 250 == 0 or event["done"] == event["total"]:
            print(json.dumps(event), file=sys.stderr, flush=True)

    result = census(disc, args.jobs, progress)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=1, sort_keys=False))
    print(json.dumps(dict(seconds=result["seconds"], totals=result["totals"], notCovered=result["notCovered"]), indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
