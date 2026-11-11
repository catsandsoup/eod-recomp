"""SQLite store (WAL) for the asset index, job queue, locks and entity states.

The index tables are derived data: `index.rebuild` drops and refills them from
the disc. Jobs, locks, tags added by users and state history are not derived.
Schema changes are numbered migrations; never edit a released migration.
"""
import sqlite3
import time
from pathlib import Path

MIGRATIONS = [
    # 1: asset index ---------------------------------------------------------
    """
    CREATE TABLE IF NOT EXISTS files(
        disc TEXT NOT NULL, path TEXT NOT NULL, ord INTEGER, size INTEGER, mtime_ns INTEGER, sha256 TEXT,
        kind TEXT, members INTEGER, parse_error TEXT, indexed_at REAL,
        PRIMARY KEY(disc, path));
    CREATE TABLE IF NOT EXISTS assets(
        id INTEGER PRIMARY KEY,
        disc TEXT NOT NULL, asset_id TEXT NOT NULL, archive TEXT NOT NULL, member TEXT NOT NULL, member_index INTEGER,
        archive_sha256 TEXT, member_sha256 TEXT, type TEXT, size INTEGER, crc INTEGER, crc_ok INTEGER,
        role TEXT, role_confidence TEXT, width INTEGER, height INTEGER, levels INTEGER, format INTEGER, flags INTEGER,
        bits INTEGER, index_key TEXT, palette_key TEXT, decode_status TEXT, record TEXT NOT NULL,
        UNIQUE(disc, asset_id COLLATE NOCASE));
    CREATE INDEX IF NOT EXISTS assets_archive ON assets(disc, archive COLLATE NOCASE);
    CREATE INDEX IF NOT EXISTS assets_member_sha ON assets(member_sha256);
    CREATE INDEX IF NOT EXISTS assets_palette ON assets(palette_key);
    CREATE TABLE IF NOT EXISTS fields(
        asset INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE, seq INTEGER,
        section TEXT, key TEXT, value TEXT);
    CREATE INDEX IF NOT EXISTS fields_asset ON fields(asset);
    CREATE INDEX IF NOT EXISTS fields_key ON fields(section, key);
    CREATE TABLE IF NOT EXISTS tags(
        disc TEXT NOT NULL, asset_id TEXT NOT NULL COLLATE NOCASE, tag TEXT NOT NULL, source TEXT NOT NULL,
        PRIMARY KEY(disc, asset_id, tag));
    CREATE TABLE IF NOT EXISTS relations(
        disc TEXT NOT NULL, src TEXT NOT NULL COLLATE NOCASE, rel TEXT NOT NULL, dst TEXT NOT NULL COLLATE NOCASE,
        PRIMARY KEY(disc, src, rel, dst));
    CREATE INDEX IF NOT EXISTS relations_dst ON relations(disc, dst, rel);
    CREATE TABLE IF NOT EXISTS thumbnails(
        member_sha256 TEXT NOT NULL, size INTEGER NOT NULL, path TEXT NOT NULL, sha256 TEXT NOT NULL,
        width INTEGER, height INTEGER, created REAL, PRIMARY KEY(member_sha256, size));
    CREATE TABLE IF NOT EXISTS file_hashes(
        path TEXT PRIMARY KEY, size INTEGER, mtime_ns INTEGER, ino INTEGER, sha256 TEXT, hashed_at REAL);
    CREATE TABLE IF NOT EXISTS index_runs(
        disc TEXT NOT NULL, started REAL, finished REAL, mode TEXT, status TEXT,
        files INTEGER, changed INTEGER, skipped INTEGER, removed INTEGER, members INTEGER, seconds REAL);
    CREATE VIRTUAL TABLE IF NOT EXISTS assets_fts USING fts5(name, tags, fields, tokenize='trigram');
    """,
    # 2: jobs, locks, state machines ------------------------------------------
    """
    CREATE TABLE IF NOT EXISTS jobs(
        id TEXT PRIMARY KEY, kind TEXT NOT NULL, label TEXT, state TEXT NOT NULL, params TEXT NOT NULL,
        idem_key TEXT, resources TEXT NOT NULL DEFAULT '[]', source TEXT, audit INTEGER DEFAULT 0,
        attempts INTEGER DEFAULT 0, max_attempts INTEGER DEFAULT 3, recoveries INTEGER DEFAULT 0,
        next_run_at REAL, created REAL, started REAL, finished REAL, updated REAL,
        pid INTEGER, lease_owner TEXT, lease_expires REAL, heartbeat REAL, cancel_requested INTEGER DEFAULT 0,
        progress_done INTEGER, progress_total INTEGER, progress_message TEXT, progress_current TEXT,
        progress_seq INTEGER DEFAULT 0, checkpoint TEXT, result TEXT, error TEXT, error_class TEXT,
        exit_code INTEGER, dest TEXT, note TEXT);
    CREATE INDEX IF NOT EXISTS jobs_idem ON jobs(idem_key);
    CREATE INDEX IF NOT EXISTS jobs_state ON jobs(state);
    CREATE INDEX IF NOT EXISTS jobs_dest ON jobs(dest);
    CREATE TABLE IF NOT EXISTS job_events(
        job_id TEXT NOT NULL, ts REAL NOT NULL, event TEXT NOT NULL, data TEXT);
    CREATE INDEX IF NOT EXISTS job_events_job ON job_events(job_id, ts);
    CREATE TABLE IF NOT EXISTS locks(
        resource TEXT PRIMARY KEY, holder TEXT NOT NULL, pid INTEGER, job_id TEXT, acquired REAL, note TEXT);
    CREATE TABLE IF NOT EXISTS entity_state(
        kind TEXT NOT NULL, key TEXT NOT NULL, state TEXT NOT NULL, updated REAL, detail TEXT,
        PRIMARY KEY(kind, key));
    CREATE TABLE IF NOT EXISTS state_log(
        ts REAL, kind TEXT, key TEXT, action TEXT, from_state TEXT, to_state TEXT, ok INTEGER, message TEXT);
    """,
]
SCHEMA_VERSION = len(MIGRATIONS)
INDEX_TABLES = ("assets_fts", "fields", "relations", "assets", "files", "index_runs")


def connect(path, timeout=30.0):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(str(path), timeout=timeout, isolation_level=None, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA synchronous=NORMAL")
    conn.execute("PRAGMA foreign_keys=ON")
    conn.execute(f"PRAGMA busy_timeout={int(timeout * 1000)}")
    migrate(conn)
    return conn


def version(conn):
    return conn.execute("PRAGMA user_version").fetchone()[0]


def migrate(conn, target=SCHEMA_VERSION):
    current = version(conn)
    if current > SCHEMA_VERSION:
        raise RuntimeError(f"Kit database schema {current} is newer than this service ({SCHEMA_VERSION}); "
                           "update the Creation Kit or rebuild the index in a new workspace")
    while current < target:
        conn.execute("BEGIN IMMEDIATE")
        try:
            if version(conn) == current:  # another process may have migrated meanwhile
                for statement in _statements(MIGRATIONS[current]):
                    conn.execute(statement)
                conn.execute(f"PRAGMA user_version={current + 1}")
            conn.execute("COMMIT")
        except BaseException:
            conn.execute("ROLLBACK")
            raise
        current = version(conn)


def _statements(script):
    """Split a migration into statements (no semicolons inside our DDL literals)."""
    return [s.strip() for s in script.split(";") if s.strip()]


class tx:
    """BEGIN IMMEDIATE ... COMMIT, rolled back on error."""

    def __init__(self, conn):
        self.conn = conn

    def __enter__(self):
        self.conn.execute("BEGIN IMMEDIATE")
        return self.conn

    def __exit__(self, kind, value, trace):
        self.conn.execute("ROLLBACK" if kind else "COMMIT")
        return False


def now():
    return time.time()
