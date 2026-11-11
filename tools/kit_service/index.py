"""SQLite asset index built from asset_registry's census logic.

Identity and relation semantics are those of docs/CREATION_KIT_TEXTURE_CONTRACTS.md
sections 1-2 (assetId, archiveSha256, memberSha256, type, crcOk, TEX header
fields, duplicateOf, variantOf, runtimeKeyShared). Each asset row keeps the
exact census record (JSON) plus indexed columns, decoded VALT fields, tags,
lazily generated thumbnails and an FTS5 (trigram) document.

Incremental update: a file whose (size, mtime_ns) is unchanged is skipped
without reading it; a changed file is hashed and skipped if its sha256 is
unchanged; only archives with new content are re-analysed. Every archive is
committed separately, so a cancelled or killed build resumes where it stopped.
"""
import hashlib
import json
import mmap
import os
import time
from pathlib import Path

from .common import KitError, ensure_tools_path

ensure_tools_path()
import asset_registry  # noqa: E402
from modding_backend import archive_path  # noqa: E402

from . import db as kdb  # noqa: E402

COMPACT_KEYS = ("assetId", "type", "role", "roleConfidence", "width", "height", "format", "duplicateOf",
                "runtimeKeyShared", "size")
MAX_FIELDS = 4000
FTS_FIELDS_CHARS = 64 * 1024
THUMB_SIZES = (64, 128, 256)


# --------------------------------------------------------------------------- hashing

def file_sha(conn, path):
    """sha256 of a file, cached in file_hashes by (size, mtime_ns, inode)."""
    path = Path(path)
    st = path.stat()
    row = conn.execute("SELECT size, mtime_ns, ino, sha256 FROM file_hashes WHERE path=?", (str(path),)).fetchone()
    if row and (row["size"], row["mtime_ns"], row["ino"]) == (st.st_size, st.st_mtime_ns, st.st_ino):
        return row["sha256"]
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 22), b""):
            digest.update(block)
    value = digest.hexdigest()
    remember_sha(conn, path, st, value)
    return value


def remember_sha(conn, path, st, value):
    conn.execute("INSERT OR REPLACE INTO file_hashes(path, size, mtime_ns, ino, sha256, hashed_at) VALUES(?,?,?,?,?,?)",
                 (str(path), st.st_size, st.st_mtime_ns, st.st_ino, value, time.time()))


def content_hashes(conn, disc, relatives):
    disc = Path(disc).resolve()
    return {rel: file_sha(conn, archive_path(disc, rel)) for rel in sorted(set(relatives))}


def walk(disc):
    """Disc-relative DATA files in census order (asset_registry.inventory's walk)."""
    names = []
    for root, dirs, files in os.walk(disc / "DATA"):
        dirs.sort(key=str.casefold)
        for name in sorted(files, key=str.casefold):
            names.append((Path(root) / name).relative_to(disc).as_posix())
    return names


# --------------------------------------------------------------------------- analysis (pool worker)

def analyze(task):
    """asset_registry.analyze_member plus decoded VALT fields. Never raises."""
    out = asset_registry.analyze_member(task)
    path, entry, _, _ = task
    if entry["type"] == "VALT":
        try:
            from valt import parse_binary_valt
            with open(path, "rb") as handle:
                handle.seek(entry["offset"])
                table = parse_binary_valt(handle.read(entry["size"]))
            fields = []
            for section in table["sections"]:
                for key, value in section["pairs"]:
                    fields.append((section["name"], key, str(value)))
                    if len(fields) >= MAX_FIELDS:
                        break
            out["fields"] = fields
        except Exception as error:  # noqa: BLE001 - undecodable tables are recorded, not fatal
            out["fieldsError"] = str(error) or type(error).__name__
    return out


def _archive_members(disc, relative, record, entries):
    path = archive_path(disc, relative)
    out = []
    with path.open("rb") as handle, mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) as view:
        for i, entry in enumerate(entries):
            blob = view[entry["offset"]:entry["offset"] + entry["size"]]
            out.append(dict(entry, archive=relative, archiveSha256=record["sha256"],
                            memberSha256=hashlib.sha256(blob).hexdigest(), path=str(path),
                            assetId=asset_registry.asset_id(relative, entry["name"]), index=i))
    return out


# --------------------------------------------------------------------------- writing

def _auto_tags(record):
    tags = {record["type"].lower()}
    if record.get("role"):
        tags.add(record["role"])
    if record.get("crcOk") is False:
        tags.add("crc-mismatch")
    for mode in record.get("reimport", []):
        tags.add(mode)
    status = (record.get("decode") or {}).get("status")
    if status and status != "ok":
        tags.add("decode-" + status)
    if record.get("duplicateOf"):
        tags.add("duplicate")
    if record.get("variantOf"):
        tags.add("variant")
    if record.get("runtimeKeyShared"):
        tags.add("runtime-key-shared")
    return sorted(tags)


def _fts_fields(record, fields):
    parts = []
    if record["type"] == "TEX" and record.get("width"):
        parts.append(f"{record['width']}x{record['height']} format{record.get('format')} bits{record.get('bitsPerPixel')}")
    for key in ("indexKey", "paletteKey"):
        if record.get(key):
            parts.append(record[key])
    text = " ".join(parts) + " " + " ".join(f"{s}.{k}={v}" for s, k, v in fields or ())
    return text[:FTS_FIELDS_CHARS]


def _user_tags(conn, disc, asset_id):
    return [r[0] for r in conn.execute("SELECT tag FROM tags WHERE disc=? AND asset_id=? AND source='user' ORDER BY tag",
                                       (disc, asset_id))]


def _write_fts(conn, rowid, record, fields, user_tags):
    conn.execute("DELETE FROM assets_fts WHERE rowid=?", (rowid,))
    conn.execute("INSERT INTO assets_fts(rowid, name, tags, fields) VALUES(?,?,?,?)",
                 (rowid, f"{record['assetId']} {record['type']} {record.get('role') or ''}",
                  " ".join(_auto_tags(record) + list(user_tags)), _fts_fields(record, fields)))


def _delete_archive(conn, disc, relative):
    ids = [r[0] for r in conn.execute("SELECT id FROM assets WHERE disc=? AND archive=? COLLATE NOCASE", (disc, relative))]
    for i in range(0, len(ids), 500):
        chunk = ids[i:i + 500]
        marks = ",".join("?" * len(chunk))
        conn.execute(f"DELETE FROM assets_fts WHERE rowid IN ({marks})", chunk)
        conn.execute(f"DELETE FROM fields WHERE asset IN ({marks})", chunk)
        conn.execute(f"DELETE FROM assets WHERE id IN ({marks})", chunk)
    return len(ids)


def _write_file_row(conn, disc, ordinal, record, st):
    conn.execute("INSERT OR REPLACE INTO files(disc, path, ord, size, mtime_ns, sha256, kind, members, parse_error, indexed_at) "
                 "VALUES(?,?,?,?,?,?,?,?,?,?)",
                 (disc, record["path"], ordinal, record["size"], st.st_mtime_ns if st else None, record["sha256"],
                  record["kind"], record["members"], record["parseError"], time.time()))


def _insert_asset(conn, disc, record, index, fields):
    cur = conn.execute(
        "INSERT INTO assets(disc, asset_id, archive, member, member_index, archive_sha256, member_sha256, type, size, crc, "
        "crc_ok, role, role_confidence, width, height, levels, format, flags, bits, index_key, palette_key, decode_status, "
        "record) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        (disc, record["assetId"], record["archive"], record["member"], index, record["archiveSha256"],
         record["memberSha256"], record["type"], record["size"], record["crc"],
         None if record.get("crcOk") is None else int(record["crcOk"]), record.get("role"),
         record.get("roleConfidence"), record.get("width"), record.get("height"), record.get("levels"),
         record.get("format"), record.get("flags"), record.get("bitsPerPixel"), record.get("indexKey"),
         record.get("paletteKey"), (record.get("decode") or {}).get("status"), json.dumps(record)))
    rowid = cur.lastrowid
    if fields:
        conn.executemany("INSERT INTO fields(asset, seq, section, key, value) VALUES(?,?,?,?,?)",
                         [(rowid, i, s, k, v) for i, (s, k, v) in enumerate(fields)])
    _write_fts(conn, rowid, record, fields, _user_tags(conn, disc, record["assetId"]))
    return rowid


# --------------------------------------------------------------------------- build

def build(conn, disc, jobs=1, full=False, progress=None, cancel=None):
    """Index (or incrementally update) one disc. Returns a run summary dict."""
    started = time.time()
    disc = Path(disc).expanduser().resolve()
    if not (disc / "DATA").is_dir():
        raise KitError("DISC_NOT_FOUND", "This folder has no DATA folder, so it can't be indexed.",
                       "Choose the extracted game folder containing DATA and SLUS_209.10.")
    key = str(disc)
    emit = progress or (lambda event: None)
    if full:
        with kdb.tx(conn):
            ids = [r[0] for r in conn.execute("SELECT id FROM assets WHERE disc=?", (key,))]
            for i in range(0, len(ids), 500):
                chunk = ids[i:i + 500]
                conn.execute(f"DELETE FROM assets_fts WHERE rowid IN ({','.join('?' * len(chunk))})", chunk)
            conn.execute("DELETE FROM fields WHERE asset IN (SELECT id FROM assets WHERE disc=?)", (key,))
            conn.execute("DELETE FROM assets WHERE disc=?", (key,))
            conn.execute("DELETE FROM relations WHERE disc=?", (key,))
            conn.execute("DELETE FROM tags WHERE disc=? AND source='auto'", (key,))
            conn.execute("DELETE FROM files WHERE disc=?", (key,))
    prior = {r["path"]: r for r in conn.execute("SELECT * FROM files WHERE disc=?", (key,))}
    names = walk(disc)
    emit(dict(event="start", done=0, total=len(names), message="scan"))
    changed, skipped_stat, skipped_hash = [], 0, 0
    with kdb.tx(conn):
        for ordinal, rel in enumerate(names):
            st = (disc / rel).stat()
            row = prior.pop(rel, None)
            if row and row["size"] == st.st_size and row["mtime_ns"] == st.st_mtime_ns:
                if row["ord"] != ordinal:
                    conn.execute("UPDATE files SET ord=? WHERE disc=? AND path=?", (ordinal, key, rel))
                skipped_stat += 1
                continue
            record, entries = asset_registry._classify_file(disc, rel)
            if record["sha256"]:
                remember_sha(conn, disc / rel, st, record["sha256"])
            if row and row["sha256"] == record["sha256"] and row["parse_error"] == record["parseError"]:
                conn.execute("UPDATE files SET ord=?, size=?, mtime_ns=?, indexed_at=? WHERE disc=? AND path=?",
                             (ordinal, st.st_size, st.st_mtime_ns, time.time(), key, rel))
                skipped_hash += 1
                continue
            changed.append((ordinal, rel, record, entries, st))
        removed = list(prior)
        for rel in removed:
            _delete_archive(conn, key, rel)
            conn.execute("DELETE FROM files WHERE disc=? AND path=?", (key, rel))
    # Non-archive or unreadable files are written immediately; archives after analysis.
    pending, records, tasks = {}, {}, []
    with kdb.tx(conn):
        for ordinal, rel, record, entries, st in changed:
            if not entries:
                _delete_archive(conn, key, rel)
                _write_file_row(conn, key, ordinal, record, st)
                continue
            members = _archive_members(disc, rel, record, entries)
            pending[rel] = dict(ordinal=ordinal, record=record, st=st, left=len(members), members=[])
            for m in members:
                rec = asset_registry.member_record(m)
                records[m["assetId"]] = (rel, m["index"], rec)
                pending[rel]["members"].append(m["assetId"])
                tasks.append((m["path"], dict(name=m["name"], type=m["type"], size=m["size"], offset=m["offset"],
                                              crc=m["crc"]), True, m["assetId"]))
    fields_by_id, done, written = {}, [0], [0]
    last = [0.0]

    def flush(rel):
        info = pending.pop(rel)
        with kdb.tx(conn):
            _delete_archive(conn, key, rel)
            for aid in info["members"]:
                _, index, rec = records[aid]
                _insert_asset(conn, key, rec, index, fields_by_id.get(aid))
            _write_file_row(conn, key, info["ordinal"], info["record"], info["st"])
        written[0] += 1

    def on_result(task, result):
        rel, _, rec = records[result["assetId"]]
        asset_registry.merge_analysis(rec, result)
        if result.get("fields"):
            fields_by_id[result["assetId"]] = result["fields"]
        if result.get("fieldsError"):
            rec.setdefault("warnings", []).append("fields-unavailable: " + result["fieldsError"])
        done[0] += 1
        pending[rel]["left"] -= 1
        if pending[rel]["left"] == 0:
            flush(rel)
        if time.time() - last[0] > 0.25 or done[0] == len(tasks):
            last[0] = time.time()
            emit(dict(event="progress", done=done[0], total=len(tasks), message="analyze", current=result["assetId"]))

    if tasks:
        asset_registry.run_tasks(analyze, tasks, max(1, int(jobs or 1)), cancel, on_result)
    cancelled = bool(cancel is not None and cancel.is_set() and pending)
    relations_changed = 0
    if (changed or removed or full) and not cancelled:
        emit(dict(event="progress", done=done[0], total=len(tasks), message="relations"))
        relations_changed = link(conn, key)
    status = "cancelled" if cancelled else "complete"
    summary = dict(status=status, disc=key, files=len(names), changed=len(changed), skippedUnchangedStat=skipped_stat,
                   skippedUnchangedHash=skipped_hash, removed=len(removed), archivesWritten=written[0],
                   membersAnalysed=done[0], relationsUpdated=relations_changed, seconds=round(time.time() - started, 2),
                   mode="rebuild" if full else "incremental")
    conn.execute("INSERT INTO index_runs(disc, started, finished, mode, status, files, changed, skipped, removed, members, seconds) "
                 "VALUES(?,?,?,?,?,?,?,?,?,?,?)", (key, started, time.time(), summary["mode"], status, len(names),
                                                   len(changed), skipped_stat + skipped_hash, len(removed), done[0],
                                                   summary["seconds"]))
    emit(dict(event="finish", done=done[0], total=len(tasks), message=status))
    return summary


def link(conn, disc):
    """Recompute duplicateOf/variantOf/runtimeKeyShared/indexKeyShared for a disc (asset_registry.link_records)."""
    rows = conn.execute("SELECT id, record FROM assets WHERE disc=?", (disc,)).fetchall()
    records = []
    for row in rows:
        rec = json.loads(row["record"])
        rec["_id"] = row["id"]
        rec["_before"] = {k: rec.get(k) for k in ("duplicateOf", "variantOf", "runtimeKeyShared", "indexKeyShared")}
        records.append(rec)
    asset_registry.link_records(records)
    updated = 0
    with kdb.tx(conn):
        conn.execute("DELETE FROM relations WHERE disc=?", (disc,))
        relation_rows = []
        for rec in records:
            rowid, before = rec.pop("_id"), rec.pop("_before")
            for rel in ("duplicateOf", "variantOf", "runtimeKeyShared", "indexKeyShared"):
                for other in rec.get(rel) or ():
                    relation_rows.append((disc, rec["assetId"], rel, other))
            if {k: rec.get(k) for k in before} != before:
                conn.execute("UPDATE assets SET record=? WHERE id=?", (json.dumps(rec), rowid))
                fields = [(r["section"], r["key"], r["value"]) for r in
                          conn.execute("SELECT section, key, value FROM fields WHERE asset=? ORDER BY seq", (rowid,))]
                _write_fts(conn, rowid, rec, fields, _user_tags(conn, disc, rec["assetId"]))
                updated += 1
        conn.executemany("INSERT OR IGNORE INTO relations(disc, src, rel, dst) VALUES(?,?,?,?)", relation_rows)
    return updated


# --------------------------------------------------------------------------- queries

def _disc_key(disc):
    if not disc:
        raise KitError("NOT_CONFIGURED", "No game folder was given for the index query.", "Pass disc: the extracted game folder.")
    return str(Path(disc).expanduser().resolve())


def has_index(conn, disc):
    return conn.execute("SELECT 1 FROM assets WHERE disc=? LIMIT 1", (disc,)).fetchone() is not None


def _require(conn, disc):
    if not has_index(conn, disc):
        raise KitError("NOT_CONFIGURED", "This game folder has not been indexed yet.",
                       "Run index.build (eod_census_start in MCP), then search again.")


def compact(record):
    return {k: record[k] for k in COMPACT_KEYS if k in record}


def search(conn, disc, params):
    disc = _disc_key(disc)
    _require(conn, disc)
    where, args = ["a.disc=?"], [disc]
    if params.get("query"):
        where.append("instr(lower(a.asset_id), lower(?)) > 0")
        args.append(params["query"])
    if params.get("text"):
        text = str(params["text"])
        if len(text) >= 3:
            where.append("a.id IN (SELECT rowid FROM assets_fts WHERE assets_fts MATCH ?)")
            args.append('"' + text.replace('"', '""') + '"')
        else:
            where.append("a.id IN (SELECT rowid FROM assets_fts WHERE name LIKE ? OR tags LIKE ? OR fields LIKE ?)")
            args += ["%" + text + "%"] * 3
    for column, key in (("role", "role"), ("type", "type")):
        if params.get(key):
            where.append(f"a.{column}=? COLLATE NOCASE")
            args.append(params[key])
    if params.get("archive"):
        where.append("a.archive=? COLLATE NOCASE")
        args.append(params["archive"])
    if params.get("min_w"):
        where.append("COALESCE(a.width, 0) >= ?")
        args.append(int(params["min_w"]))
    if params.get("min_h"):
        where.append("COALESCE(a.height, 0) >= ?")
        args.append(int(params["min_h"]))
    if params.get("shares_palette_with"):
        row = conn.execute("SELECT palette_key FROM assets WHERE disc=? AND asset_id=? COLLATE NOCASE",
                           (disc, params["shares_palette_with"])).fetchone()
        if not row or not row[0]:
            raise KitError("NOT_CONFIGURED", "Unknown asset or no paletteKey: " + params["shares_palette_with"])
        where.append("a.palette_key=?")
        args.append(row[0])
    if params.get("duplicate_of"):
        where.append("a.asset_id IN (SELECT src FROM relations WHERE disc=? AND rel='duplicateOf' AND dst=?)")
        args += [disc, params["duplicate_of"]]
    if params.get("tag"):
        where.append("a.asset_id IN (SELECT asset_id FROM tags WHERE disc=? AND tag=?)")
        args += [disc, params["tag"]]
    sql = (" FROM assets a JOIN files f ON f.disc=a.disc AND f.path=a.archive WHERE " + " AND ".join(where))
    total = conn.execute("SELECT count(*)" + sql, args).fetchone()[0]
    start = int(params.get("cursor") or 0)
    limit = max(1, min(int(params.get("limit") or 50), 1000))
    rows = conn.execute("SELECT a.record" + sql + " ORDER BY f.ord, a.member_index LIMIT ? OFFSET ?",
                        args + [limit, start]).fetchall()
    out = dict(assets=[compact(json.loads(r[0])) for r in rows], total=total)
    if start + limit < total:
        out["next"] = str(start + limit)
    return out


def get(conn, disc, asset_id, ws=None):
    disc = _disc_key(disc)
    _require(conn, disc)
    row = conn.execute("SELECT id, record, member_sha256 FROM assets WHERE disc=? AND asset_id=? COLLATE NOCASE",
                       (disc, asset_id)).fetchone()
    if row is None:
        raise KitError("NOT_CONFIGURED", "Asset not in the index: " + asset_id, "Search the index with index.search")
    record = json.loads(row["record"])
    record["tags"] = sorted(set(_auto_tags(record)) | set(_user_tags(conn, disc, record["assetId"])))
    record["relations"] = {}
    for r in conn.execute("SELECT rel, dst FROM relations WHERE disc=? AND src=? ORDER BY rel, dst", (disc, record["assetId"])):
        record["relations"].setdefault(r["rel"], []).append(r["dst"])
    fields = conn.execute("SELECT section, key, value FROM fields WHERE asset=? ORDER BY seq LIMIT 500", (row["id"],)).fetchall()
    if fields:
        record["fields"] = [dict(section=f["section"], key=f["key"], value=f["value"]) for f in fields]
    thumbs = conn.execute("SELECT size, path, sha256 FROM thumbnails WHERE member_sha256=? ORDER BY size",
                          (row["member_sha256"],)).fetchall()
    record["thumbnails"] = [dict(size=t["size"], path=t["path"], sha256=t["sha256"]) for t in thumbs if Path(t["path"]).is_file()]
    return record


def census(conn, disc):
    """The census document (asset_registry.census shape) reconstructed from the index."""
    disc = _disc_key(disc)
    _require(conn, disc)
    files = [dict(path=r["path"], size=r["size"], sha256=r["sha256"], kind=r["kind"], members=r["members"],
                  parseError=r["parse_error"]) for r in conn.execute("SELECT * FROM files WHERE disc=? ORDER BY ord", (disc,))]
    assets = [json.loads(r[0]) for r in conn.execute(
        "SELECT a.record FROM assets a JOIN files f ON f.disc=a.disc AND f.path=a.archive WHERE a.disc=? "
        "ORDER BY f.ord, a.member_index", (disc,))]
    slus = Path(disc) / "SLUS_209.10"
    return dict(schemaVersion=asset_registry.SCHEMA_VERSION, tool="asset_registry", source="kit-service-index",
                created=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                disc=dict(slusSha256=file_sha(conn, slus) if slus.is_file() else None), cancelled=False, seconds=0,
                files=files, assets=assets, totals=asset_registry.totals(files, assets),
                notCovered=asset_registry.not_covered(assets))


def status(conn, disc, db_file=None):
    disc = _disc_key(disc)
    counts = dict(files=conn.execute("SELECT count(*) FROM files WHERE disc=?", (disc,)).fetchone()[0],
                  assets=conn.execute("SELECT count(*) FROM assets WHERE disc=?", (disc,)).fetchone()[0],
                  relations=conn.execute("SELECT count(*) FROM relations WHERE disc=?", (disc,)).fetchone()[0],
                  fields=conn.execute("SELECT count(*) FROM fields f JOIN assets a ON a.id=f.asset WHERE a.disc=?",
                                      (disc,)).fetchone()[0],
                  thumbnails=conn.execute("SELECT count(*) FROM thumbnails").fetchone()[0])
    run = conn.execute("SELECT * FROM index_runs WHERE disc=? ORDER BY started DESC LIMIT 1", (disc,)).fetchone()
    out = dict(disc=disc, schemaVersion=kdb.version(conn), indexed=counts["assets"] > 0, counts=counts,
               lastRun=dict(run) if run else None)
    if db_file:
        files = [Path(str(db_file) + s) for s in ("", "-wal", "-shm")]
        out["dbBytes"] = sum(f.stat().st_size for f in files if f.is_file())
    return out


def tag(conn, disc, asset_id, add=(), remove=()):
    disc = _disc_key(disc)
    row = conn.execute("SELECT id, asset_id, record FROM assets WHERE disc=? AND asset_id=? COLLATE NOCASE",
                       (disc, asset_id)).fetchone()
    if row is None:
        raise KitError("NOT_CONFIGURED", "Asset not in the index: " + asset_id)
    with kdb.tx(conn):
        for t in add:
            conn.execute("INSERT OR IGNORE INTO tags(disc, asset_id, tag, source) VALUES(?,?,?,'user')",
                         (disc, row["asset_id"], str(t).strip().lower()))
        for t in remove:
            conn.execute("DELETE FROM tags WHERE disc=? AND asset_id=? AND tag=? AND source='user'",
                         (disc, row["asset_id"], str(t).strip().lower()))
        record = json.loads(row["record"])
        fields = [(r["section"], r["key"], r["value"]) for r in
                  conn.execute("SELECT section, key, value FROM fields WHERE asset=? ORDER BY seq", (row["id"],))]
        _write_fts(conn, row["id"], record, fields, _user_tags(conn, disc, row["asset_id"]))
    return dict(assetId=row["asset_id"], tags=_user_tags(conn, disc, row["asset_id"]))


def thumbnail(conn, ws, disc, asset_id, size=128):
    """Lazily generated mip-zero thumbnail PNG (nearest-neighbour, aspect kept)."""
    import modding_backend as mb
    from eod_mcp import imaging
    if size not in THUMB_SIZES:
        raise KitError("INVALID_ARGUMENT", f"Thumbnail size must be one of {', '.join(map(str, THUMB_SIZES))}.")
    disc_key = _disc_key(disc)
    _require(conn, disc_key)
    row = conn.execute("SELECT archive, member, member_sha256, type FROM assets WHERE disc=? AND asset_id=? COLLATE NOCASE",
                       (disc_key, asset_id)).fetchone()
    if row is None:
        raise KitError("NOT_CONFIGURED", "Asset not in the index: " + asset_id)
    if row["type"] != "TEX":
        raise KitError("UNSUPPORTED", f"{asset_id} is {row['type']}; thumbnails are made for textures only.")
    have = conn.execute("SELECT path, sha256, width, height FROM thumbnails WHERE member_sha256=? AND size=?",
                        (row["member_sha256"], size)).fetchone()
    if have and Path(have["path"]).is_file():
        return dict(assetId=asset_id, size=size, path=have["path"], sha256=have["sha256"], width=have["width"],
                    height=have["height"], cached=True)
    data, _ = mb.payload(mb.archive_path(Path(disc_key), row["archive"]), row["member"], verify_crc=False)
    if hashlib.sha256(data).hexdigest() != row["member_sha256"]:
        raise KitError("STALE_INDEX", "The texture on disc changed after it was indexed.", "Update the index, then try again.")
    w, h, rgba, _ = mb.decode_preview(data)
    scale = min(1.0, size / max(w, h))
    nw, nh = max(1, int(w * scale)), max(1, int(h * scale))
    png = imaging.png_bytes(nw, nh, imaging.scale(w, h, rgba, nw, nh))
    sha = hashlib.sha256(png).hexdigest()
    target = Path(ws) / "service" / "thumbs" / row["member_sha256"][:2] / f"{row['member_sha256']}-{size}.png"
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_suffix(f".{os.getpid()}.tmp")
    temporary.write_bytes(png)
    os.replace(temporary, target)
    conn.execute("INSERT OR REPLACE INTO thumbnails(member_sha256, size, path, sha256, width, height, created) VALUES(?,?,?,?,?,?,?)",
                 (row["member_sha256"], size, str(target), sha, nw, nh, time.time()))
    return dict(assetId=asset_id, size=size, path=str(target), sha256=sha, width=nw, height=nh, cached=False)
