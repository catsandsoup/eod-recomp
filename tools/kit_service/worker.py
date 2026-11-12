"""Job worker subprocess: python3 -I worker.py <db> <job_id> <lease_token> <job_dir>.

Runs one job kind and reports progress, heartbeats and the result straight into
the Kit database, so it keeps working (and its lease stays fresh) even if the
service that started it dies. The census / texture_export / cli / test_run
kinds are the eod_mcp job_worker functions; index_build and stub are added here.
If the lease is taken away (the job was requeued elsewhere) the worker stops.
"""
import errno
import json
import os
import signal
import sqlite3
import sys
import threading
import time
import traceback
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.append(str(TOOLS))

from kit_service import db as kdb  # noqa: E402
from kit_service.jobs import LEASE_SECONDS  # noqa: E402
from eod_mcp import job_worker as jw  # noqa: E402

HEARTBEAT = float(os.environ.get("EOD_KIT_HEARTBEAT_SECONDS", "2"))
TRANSIENT_ERRNO = {errno.EAGAIN, errno.EBUSY, errno.EINTR, errno.ETIMEDOUT, errno.EMFILE, errno.ENFILE, errno.ENOLCK}


def classify(error):
    """transient errors are retried with backoff; everything else fails the job."""
    if isinstance(error, (TimeoutError, ConnectionError, BlockingIOError, InterruptedError)):
        return "transient"
    if isinstance(error, sqlite3.OperationalError) and any(w in str(error) for w in ("locked", "busy")):
        return "transient"
    if isinstance(error, OSError) and error.errno in TRANSIENT_ERRNO:
        return "transient"
    return "permanent"


class Reporter:
    def __init__(self, db_file, job_id, token):
        self.conn = kdb.connect(db_file)
        self.job_id, self.token = job_id, token
        self.lock = threading.Lock()
        self.lost = threading.Event()
        self.last = 0.0

    def heartbeat(self):
        with self.lock:
            now = time.time()
            n = self.conn.execute("UPDATE jobs SET heartbeat=?, lease_expires=? WHERE id=? AND lease_owner=? AND state='running'",
                                  (now, now + LEASE_SECONDS, self.job_id, self.token)).rowcount
        if n == 0:
            self.lost.set()
        return n

    def progress(self, done=None, total=None, message="", current=None, force=False):
        now = time.time()
        if not force and now - self.last < 0.1 and not (done is not None and total and done >= total):
            return
        self.last = now
        with self.lock:
            self.conn.execute("UPDATE jobs SET progress_done=?, progress_total=?, progress_message=?, progress_current=?, "
                              "progress_seq=progress_seq+1, updated=? WHERE id=? AND lease_owner=?",
                              (done, total, message, current, now, self.job_id, self.token))

    def checkpoint(self, data):
        with self.lock:
            self.conn.execute("UPDATE jobs SET checkpoint=? WHERE id=? AND lease_owner=?",
                              (json.dumps(data), self.job_id, self.token))

    def result(self, data):
        with self.lock:
            self.conn.execute("UPDATE jobs SET result=? WHERE id=? AND lease_owner=?",
                              (json.dumps(data, default=str), self.job_id, self.token))


def index_build(p, reporter):
    from kit_service import index
    conn = kdb.connect(p["db"])

    def cb(event):
        reporter.progress(event.get("done"), event.get("total"), event.get("message", ""), event.get("current"))
        if jw.DELAY:
            time.sleep(jw.DELAY)
    summary = index.build(conn, p["disc"], p.get("jobs", 1), bool(p.get("full")), cb, jw.cancel)
    if summary["status"] == "cancelled":
        return dict(ok=True, status="cancelled", summary=summary)
    outputs = {}
    if p.get("census_out"):
        census = index.census(conn, p["disc"])
        out = Path(p["census_out"])
        out.parent.mkdir(parents=True, exist_ok=True)
        tmp = out.with_suffix(".tmp")
        tmp.write_text(json.dumps(census))
        os.replace(tmp, out)
        if p.get("meta_out"):
            Path(p["meta_out"]).write_text(json.dumps(dict(slusSha256=census["disc"]["slusSha256"], tool="asset_registry",
                                                           created=census["created"], source="kit-service-index")))
        from eod_mcp.safety import sha_file
        outputs[str(out)] = sha_file(out)
        summary["totals"] = census["totals"]
    return dict(ok=True, status="complete", summary=summary, totals=summary.get("totals"), seconds=summary["seconds"],
                path=p.get("census_out"), outputs=outputs)


def stub(p, reporter):
    """Test stand-in for long jobs: sleeps in steps, optional checkpoint files and transient failures."""
    row = reporter.conn.execute("SELECT attempts, checkpoint FROM jobs WHERE id=?", (reporter.job_id,)).fetchone()
    if row["attempts"] <= int(p.get("fail_transient", 0)):
        raise BlockingIOError(errno.EAGAIN, "simulated transient failure")
    if p.get("fail_permanent"):
        raise ValueError("simulated permanent failure")
    steps = int(p.get("steps", 10))
    out = Path(p["out"]) if p.get("out") else None
    start = (json.loads(row["checkpoint"]) if row["checkpoint"] else {}).get("next", 0)
    for i in range(start, steps):
        if jw.cancel.is_set():
            return dict(ok=True, status="cancelled", done=i, total=steps)
        if out:
            out.mkdir(parents=True, exist_ok=True)
            target = out / f"step-{i:04d}.txt"
            with open(out / "writes.log", "a") as log:
                log.write(f"{i}\n")
            target.write_text(f"step {i}\n")
        reporter.checkpoint(dict(next=i + 1))
        reporter.progress(i + 1, steps, "stub", f"step-{i}", force=True)
        time.sleep(float(p.get("sleep", 0.1)))
    return dict(ok=True, status="complete", done=steps, total=steps, resumedFrom=start)


def main():
    db_file, job_id, token, job_dir = sys.argv[1:5]
    reporter = Reporter(db_file, job_id, token)
    jw.job_dir = Path(job_dir)
    jw.progress = lambda done=None, total=None, message="", current=None: reporter.progress(done, total, message, current)
    jw.install_signals()
    if reporter.heartbeat() == 0:
        print("kit-worker: lease not held; exiting", file=sys.stderr)
        return 3

    def beat():
        while True:
            time.sleep(HEARTBEAT)
            if reporter.heartbeat() == 0:
                print("kit-worker: lease lost; stopping", file=sys.stderr, flush=True)
                jw.on_term(signal.SIGTERM, None)
                time.sleep(CANCEL_WAIT)
                os._exit(4)
    threading.Thread(target=beat, daemon=True).start()
    row = reporter.conn.execute("SELECT kind, params FROM jobs WHERE id=?", (job_id,)).fetchone()
    kind, params = row["kind"], json.loads(row["params"])
    params.setdefault("db", db_file)
    try:
        if kind == "index_build":
            res = index_build(params, reporter)
        elif kind == "stub":
            res = stub(params, reporter)
        else:
            res = jw.KINDS[kind](params)
    except Exception as error:  # noqa: BLE001
        traceback.print_exc()
        res = dict(ok=False, status="failed", error=f"{type(error).__name__}: {error}", errorClass=classify(error))
    if "done" in res:
        reporter.progress(res.get("done"), res.get("total"), "finished", None, force=True)
    reporter.result(res)
    jw.job_dir.mkdir(parents=True, exist_ok=True)
    jw.write_result(res)
    return 0 if res.get("ok") else 1


CANCEL_WAIT = 5

if __name__ == "__main__":
    sys.exit(main())
