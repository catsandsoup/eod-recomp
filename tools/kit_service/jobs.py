"""Persistent job queue: queued -> running -> succeeded | failed | cancelled.

Jobs live in the Kit database, so they survive the service. Each running job is
executed by a worker subprocess (kit_service/worker.py, which reuses the
eod_mcp job_worker kinds) in its own session, so cancel can kill its whole
process group. The worker holds a lease it renews every HEARTBEAT seconds.

Crash rules: a worker whose service died keeps running and is adopted by the
next service; a job whose worker is gone without a result is requeued (resuming
from its checkpoint where the kind supports it) or, for game runs, failed.
Duplicate submits (same kind, params and input content hashes) return the
existing job. Transient errors retry with exponential backoff; others fail.
Resources (export folder, profile, index, "game") are exclusive locks.
"""
import json
import os
import secrets
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

from . import db as kdb
from .common import PACKAGE, KitError, canonical, sha256_text

TERMINAL = ("succeeded", "failed", "cancelled")
ACTIVE = ("queued", "running")
LEASE_SECONDS = float(os.environ.get("EOD_KIT_LEASE_SECONDS", "15"))
CANCEL_GRACE = 5.0
MAX_RECOVERIES = 5
# kind -> policy. resumable: may be requeued after a crash. dedupe_succeeded: a duplicate submit
# of a finished job returns it while its recorded outputs are intact.
# lane: which worker lane runs it. Lanes have separate slot limits, so a long export never
# keeps an index build, a package build or a game run waiting (and vice versa).
KINDS = dict(
    texture_export=dict(resumable=True, dedupe_succeeded=False, lane="export"),
    census=dict(resumable=True, dedupe_succeeded=True, lane="index"),
    index_build=dict(resumable=True, dedupe_succeeded=True, lane="index"),
    cli=dict(resumable=True, dedupe_succeeded=False, lane="build"),
    test_run=dict(resumable=False, dedupe_succeeded=False, lane="game"),
    stub=dict(resumable=True, dedupe_succeeded=False, lane="export"),
)
# Slots per lane (EOD_KIT_LANE_<NAME>=n overrides). The game lane is also guarded by the global
# "game" resource lock; one slot here just keeps a second run queued instead of spawning it.
# Sized from the core count (Apple Silicon: 8-24 cores). Index builds parallelise internally (jobs=N).
_CORES = os.cpu_count() or 8
LANE_SLOTS = dict(export=max(2, _CORES // 4), index=1, build=max(2, _CORES // 4), game=1)


def lane_of(kind, params=None):
    if kind == "stub" and isinstance(params, dict) and params.get("lane") in LANE_SLOTS:
        return params["lane"]
    return KINDS.get(kind, {}).get("lane", "export")


def lane_slots():
    return {lane: int(os.environ.get("EOD_KIT_LANE_" + lane.upper(), str(n))) for lane, n in LANE_SLOTS.items()}
ENV_KEEP = ("PATH", "HOME", "TMPDIR", "LANG", "LC_ALL", "EOD_MCP_JOB_DELAY")


def pid_alive(pid):
    if not pid:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def is_worker(pid, job_id):
    try:
        text = subprocess.run(["ps", "-o", "command=", "-p", str(pid)], capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return False
    return "worker.py" in text and job_id in text


def holder_alive(conn, holder):
    kind, _, rest = holder.partition(":")
    if kind == "job":
        row = conn.execute("SELECT state FROM jobs WHERE id=?", (rest,)).fetchone()
        return bool(row and row["state"] == "running")
    try:
        return pid_alive(int(rest.split(":")[0]))
    except ValueError:
        return False


class Store:
    """Thread-local SQLite connections to one database file."""

    def __init__(self, path):
        self.path = Path(path)
        self.local = threading.local()
        kdb.connect(self.path).close()  # create / migrate once

    def conn(self):
        c = getattr(self.local, "conn", None)
        if c is None:
            c = self.local.conn = kdb.connect(self.path)
        return c


def job_view(row, ws=None, log_lines=20):
    out = dict(job_id=row["id"], kind=row["kind"], lane=lane_of(row["kind"], json.loads(row["params"] or "{}")),
               label=row["label"] or "", state=row["state"], source=row["source"],
               created=row["created"], started=row["started"], finished=row["finished"], attempts=row["attempts"],
               maxAttempts=row["max_attempts"], recoveries=row["recoveries"], pid=row["pid"],
               progress_count=row["progress_seq"] or 0, done=row["progress_done"], total=row["progress_total"],
               message=row["progress_message"], current=row["progress_current"],
               resources=json.loads(row["resources"] or "[]"), idemKey=row["idem_key"],
               cancelRequested=bool(row["cancel_requested"]), nextRunAt=row["next_run_at"])
    if row["error"]:
        out["error"] = row["error"]
        out["errorClass"] = row["error_class"]
    if row["note"]:
        out["note"] = row["note"]
    if row["state"] in TERMINAL:
        out["result"] = json.loads(row["result"]) if row["result"] else None
        out["result_link"] = f"eod://jobs/{row['id']}"
    if ws is not None:
        log = Path(ws) / "jobs" / row["id"] / "job.log"
        try:
            text = log.read_text(errors="replace")
            try:
                from eod_mcp.safety import redact
                text = redact(text)
            except Exception:  # noqa: BLE001
                pass
            out["log_tail"] = text.splitlines()[-log_lines:]
        except OSError:
            out["log_tail"] = []
    return out


class Jobs:
    def __init__(self, ws, store, max_workers=None, on_change=None):
        self.ws = Path(ws)
        self.store = store
        self.slots = lane_slots()
        if max_workers or os.environ.get("EOD_KIT_MAX_WORKERS"):  # legacy knob: caps every lane
            cap = int(max_workers or os.environ["EOD_KIT_MAX_WORKERS"])
            self.slots = {lane: min(n, cap) for lane, n in self.slots.items()}
        self.backoff_base = float(os.environ.get("EOD_KIT_BACKOFF_BASE", "2.0"))
        self.procs = {}       # job_id -> Popen (spawned by this service)
        self.adopted = {}     # job_id -> pid (left running by a previous service)
        self.cancel_at = {}
        self.cv = threading.Condition()
        self.stopped = threading.Event()
        self.thread = None
        self.on_change = on_change or (lambda job: None)
        self.tick_lock = threading.Lock()

    # ------------------------------------------------------------------ lifecycle
    def start(self):
        self.tick()  # adopt or requeue jobs left by a previous service before serving
        self.thread = threading.Thread(target=self._loop, name="kit-jobs", daemon=True)
        self.thread.start()

    def stop(self):
        self.stopped.set()
        self.wake()
        if self.thread:
            self.thread.join(5)

    def wake(self):
        with self.cv:
            self.cv.notify_all()

    def _loop(self):
        while not self.stopped.is_set():
            with self.cv:
                self.cv.wait(0.5)
            try:
                self.tick()
            except Exception as error:  # noqa: BLE001 - the scheduler must keep running
                print(f"kit-service: scheduler error: {type(error).__name__}: {error}", file=sys.stderr, flush=True)

    def busy(self):
        row = self.store.conn().execute("SELECT count(*) FROM jobs WHERE state IN ('queued','running')").fetchone()
        return row[0] > 0 or bool(self.procs) or bool(self.adopted)

    def tick(self):
        with self.tick_lock:
            self._reap()
            self._recover()
            self._escalate_cancels()
            self._dispatch()

    # ------------------------------------------------------------------ submit
    def submit(self, kind, params, label="", source=None, audit=False, resources=None, max_attempts=3, inputs=None):
        if kind not in KINDS:
            raise KitError("INVALID_ARGUMENT", f"Unknown job kind {kind!r}.", "Use one of: " + ", ".join(sorted(KINDS)))
        params = json.loads(canonical(params or {}))
        resources = sorted(set(resources if resources is not None else default_resources(kind, params)))
        if inputs is None:
            inputs = input_hashes(self.store.conn(), kind, params)
        idem = sha256_text(canonical(dict(kind=kind, params=params, inputs=inputs)))
        conn = self.store.conn()
        with kdb.tx(conn):
            row = conn.execute("SELECT * FROM jobs WHERE idem_key=? AND state IN ('queued','running','succeeded') "
                               "ORDER BY created DESC LIMIT 1", (idem,)).fetchone()
            if row and (row["state"] in ACTIVE or (KINDS[kind]["dedupe_succeeded"] and outputs_intact(row))):
                return dict(job_view(row), duplicate=True)
            if "game" in resources:
                busy = self._game_holder(conn)
                if busy:
                    raise KitError("CONFLICT", "A game test run is already in progress, so a second one was not started.",
                                   "Wait for it to finish or cancel it, then start the new run.", dict(holder=busy))
            job_id = time.strftime("%Y%m%d-%H%M%S-") + secrets.token_hex(3)
            now = time.time()
            dest = params.get("dest") or params.get("run_dir") or params.get("out")
            conn.execute("INSERT INTO jobs(id, kind, label, state, params, idem_key, resources, source, audit, max_attempts, "
                         "created, updated, dest) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
                         (job_id, kind, label, "queued", json.dumps(params), idem, json.dumps(resources), source,
                          int(bool(audit)), int(max_attempts), now, now, str(Path(dest).resolve()) if dest else None))
            self._event(conn, job_id, "queued", dict(inputs=len(inputs)))
            row = conn.execute("SELECT * FROM jobs WHERE id=?", (job_id,)).fetchone()
        (self.ws / "jobs" / job_id).mkdir(parents=True, exist_ok=True)
        self.wake()
        return dict(job_view(row), duplicate=False)

    def _game_holder(self, conn):
        for row in conn.execute("SELECT id, resources FROM jobs WHERE state IN ('queued','running')"):
            if "game" in json.loads(row["resources"] or "[]"):
                return "job:" + row["id"]
        lock = conn.execute("SELECT holder FROM locks WHERE resource='game'").fetchone()
        if lock and holder_alive(conn, lock["holder"]):
            return lock["holder"]
        return None

    # ------------------------------------------------------------------ queries
    def get(self, job_id):
        row = self.store.conn().execute("SELECT * FROM jobs WHERE id=?", (job_id,)).fetchone()
        if row is None:
            raise KitError("JOB_NOT_FOUND", f"No job {job_id}.", "List jobs with job.list.")
        return row

    def status(self, job_id, wait_seconds=0, since=None):
        row = self.get(job_id)
        base = row["progress_seq"] if since is None else int(since)
        deadline = time.time() + min(max(float(wait_seconds or 0), 0), 25)
        while row["state"] not in TERMINAL and (row["progress_seq"] or 0) <= base and time.time() < deadline:
            time.sleep(0.1)
            row = self.get(job_id)
        return job_view(row, self.ws)

    def list(self, state=None, kind=None, limit=200, dest=None):
        sql, args = "SELECT * FROM jobs WHERE 1=1", []
        for column, value in (("state", state), ("kind", kind), ("dest", dest)):
            if value:
                sql += f" AND {column}=?"
                args.append(value)
        sql += " ORDER BY created DESC, id DESC LIMIT ?"
        args.append(int(limit))
        return [dict(job_id=r["id"], kind=r["kind"], lane=lane_of(r["kind"], json.loads(r["params"] or "{}")),
                     state=r["state"], label=r["label"] or "", created=r["created"],
                     source=r["source"], done=r["progress_done"], total=r["progress_total"])
                for r in self.store.conn().execute(sql, args)]

    def events(self, job_id):
        return [dict(ts=r["ts"], event=r["event"], data=json.loads(r["data"] or "null")) for r in
                self.store.conn().execute("SELECT * FROM job_events WHERE job_id=? ORDER BY ts", (job_id,))]

    # ------------------------------------------------------------------ cancel
    def cancel(self, job_id):
        conn = self.store.conn()
        with kdb.tx(conn):
            row = self.get(job_id)
            if row["state"] in TERMINAL:
                return dict(job_id=job_id, state=row["state"], note="already finished")
            if row["state"] == "queued":
                conn.execute("UPDATE jobs SET state='cancelled', cancel_requested=1, finished=?, updated=? WHERE id=?",
                             (time.time(), time.time(), job_id))
                self._event(conn, job_id, "cancelled", None)
                self._release(conn, job_id)
                cancelled = True
            else:
                conn.execute("UPDATE jobs SET cancel_requested=1, updated=? WHERE id=?", (time.time(), job_id))
                self._event(conn, job_id, "cancel-requested", None)
                cancelled = False
        if cancelled:
            self.on_change(self.get(job_id))
            return dict(job_id=job_id, state="cancelled")
        pid = row["pid"]
        if pid:
            try:
                os.killpg(pid, signal.SIGTERM)
            except OSError:
                pass
        self.cancel_at[job_id] = time.time()
        self.wake()
        return dict(job_id=job_id, state="cancelling")

    def _escalate_cancels(self):
        for job_id, at in list(self.cancel_at.items()):
            row = self.store.conn().execute("SELECT state, pid FROM jobs WHERE id=?", (job_id,)).fetchone()
            if not row or row["state"] != "running":
                self.cancel_at.pop(job_id, None)
                continue
            if time.time() - at > CANCEL_GRACE and row["pid"]:
                try:
                    os.killpg(row["pid"], signal.SIGKILL)
                except OSError:
                    pass

    # ------------------------------------------------------------------ dispatch
    def _dispatch(self):
        conn = self.store.conn()
        running = {lane: 0 for lane in self.slots}
        for row in conn.execute("SELECT kind, params FROM jobs WHERE state='running'"):
            lane = lane_of(row["kind"], json.loads(row["params"] or "{}"))
            running[lane] = running.get(lane, 0) + 1
        now = time.time()
        for row in conn.execute("SELECT * FROM jobs WHERE state='queued' AND (next_run_at IS NULL OR next_run_at<=?) "
                                "ORDER BY created", (now,)).fetchall():
            if self.stopped.is_set():
                return
            lane = lane_of(row["kind"], json.loads(row["params"] or "{}"))
            if running.get(lane, 0) >= self.slots.get(lane, 1):
                continue
            token = secrets.token_hex(8)
            resources = json.loads(row["resources"] or "[]")
            with kdb.tx(conn):
                if any(self._lock_taken(conn, r, "job:" + row["id"]) for r in resources):
                    continue
                claimed = conn.execute("UPDATE jobs SET state='running', lease_owner=?, lease_expires=?, heartbeat=?, "
                                       "started=?, updated=?, attempts=attempts+1, pid=NULL WHERE id=? AND state='queued'",
                                       (token, now + LEASE_SECONDS, now, now, now, row["id"])).rowcount
                if not claimed:
                    continue
                for r in resources:
                    conn.execute("INSERT OR REPLACE INTO locks(resource, holder, pid, job_id, acquired, note) VALUES(?,?,?,?,?,?)",
                                 (r, "job:" + row["id"], None, row["id"], now, row["kind"]))
                self._event(conn, row["id"], "running", dict(attempt=row["attempts"] + 1))
            try:
                proc = self._spawn(row["id"], token)
            except OSError as error:
                conn.execute("UPDATE jobs SET result=? WHERE id=?",
                             (json.dumps(dict(ok=False, status="failed", error=f"worker could not start: {error}",
                                              errorClass="permanent")), row["id"]))
                self._finalize(row["id"], None)
                continue
            conn.execute("UPDATE jobs SET pid=? WHERE id=?", (proc.pid, row["id"]))
            self.procs[row["id"]] = proc
            running[lane] = running.get(lane, 0) + 1
            self.on_change(self.get(row["id"]))

    def _lock_taken(self, conn, resource, holder):
        lock = conn.execute("SELECT holder FROM locks WHERE resource=?", (resource,)).fetchone()
        if lock is None or lock["holder"] == holder:
            return False
        if holder_alive(conn, lock["holder"]):
            return True
        conn.execute("DELETE FROM locks WHERE resource=? AND holder=?", (resource, lock["holder"]))
        return False

    def _spawn(self, job_id, token):
        folder = self.ws / "jobs" / job_id
        folder.mkdir(parents=True, exist_ok=True)
        env = {k: v for k, v in os.environ.items() if k in ENV_KEEP or k.startswith("EOD_KIT_")}
        env["EOD_KIT_WORKSPACE"] = str(self.ws)
        with open(folder / "job.log", "ab") as log:
            return subprocess.Popen([sys.executable, "-I", str(PACKAGE / "worker.py"), str(self.store.path), job_id, token,
                                     str(folder)], stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                    start_new_session=True, env=env, cwd=str(folder))

    # ------------------------------------------------------------------ completion and recovery
    def _reap(self):
        for job_id, proc in list(self.procs.items()):
            code = proc.poll()
            if code is None:
                continue
            self.procs.pop(job_id, None)
            self._finalize(job_id, code)
        for job_id, pid in list(self.adopted.items()):
            if not pid_alive(pid) or _zombie(pid):
                self.adopted.pop(job_id, None)
                self._finalize(job_id, None, adopted=True)

    def _recover(self):
        conn = self.store.conn()
        now = time.time()
        for row in conn.execute("SELECT * FROM jobs WHERE state='running'").fetchall():
            if row["id"] in self.procs or row["id"] in self.adopted:
                continue
            pid = row["pid"]
            fresh = (row["lease_expires"] or 0) > now
            if pid and pid_alive(pid) and not _zombie(pid):
                if fresh or is_worker(pid, row["id"]):
                    if not fresh:  # alive but not renewing its lease: hung worker
                        try:
                            os.killpg(pid, signal.SIGKILL)
                        except OSError:
                            pass
                        continue
                    self.adopted[row["id"]] = pid
                    self._event(conn, row["id"], "adopted", dict(pid=pid))
                    continue
            if not pid and fresh:
                continue  # another scheduler is spawning it
            if row["result"]:
                self._finalize(row["id"], None, adopted=True)
            else:
                self._requeue_after_crash(row)

    def _requeue_after_crash(self, row):
        conn = self.store.conn()
        policy = KINDS.get(row["kind"], {})
        with kdb.tx(conn):
            current = conn.execute("SELECT state FROM jobs WHERE id=?", (row["id"],)).fetchone()
            if not current or current["state"] != "running":
                return
            if row["cancel_requested"]:
                conn.execute("UPDATE jobs SET state='cancelled', finished=?, updated=?, pid=NULL WHERE id=?",
                             (time.time(), time.time(), row["id"]))
                self._event(conn, row["id"], "cancelled", dict(reason="worker gone"))
            elif not policy.get("resumable") or row["recoveries"] >= MAX_RECOVERIES:
                message = ("server_restart: the game run was interrupted when the Kit service stopped; start it again"
                           if row["kind"] == "test_run" else "server_restart: the job stopped too many times")
                conn.execute("UPDATE jobs SET state='failed', error=?, error_class='permanent', finished=?, updated=?, "
                             "pid=NULL WHERE id=?", (message, time.time(), time.time(), row["id"]))
                self._event(conn, row["id"], "failed", dict(reason="server_restart"))
            else:
                params = resume_params(row["kind"], json.loads(row["params"]))
                conn.execute("UPDATE jobs SET state='queued', params=?, pid=NULL, lease_owner=NULL, lease_expires=NULL, "
                             "recoveries=recoveries+1, next_run_at=NULL, updated=?, note=? WHERE id=?",
                             (json.dumps(params), time.time(),
                              "requeued after its worker stopped with the Kit service" +
                              (" (resuming from checkpoint)" if params.get("resume") else ""), row["id"]))
                self._event(conn, row["id"], "requeued", dict(reason="worker gone", resume=bool(params.get("resume"))))
            self._release(conn, row["id"])
        self.wake()

    def _finalize(self, job_id, exit_code, adopted=False):
        conn = self.store.conn()
        with kdb.tx(conn):
            row = conn.execute("SELECT * FROM jobs WHERE id=?", (job_id,)).fetchone()
            if not row or row["state"] != "running":
                return
            result = json.loads(row["result"]) if row["result"] else _read_result(self.ws, job_id)
            now = time.time()
            error, error_class, state, retry_at = None, None, None, None
            if row["cancel_requested"] and not (result and result.get("status") == "complete"):
                state = "cancelled"
            elif result and result.get("ok"):
                state = "succeeded"
            else:
                if result is None:
                    error = f"worker exited with code {exit_code} without a result"
                    crashed = exit_code is None or exit_code < 0
                    error_class = "transient" if crashed else "permanent"
                else:
                    error = result.get("error") or f"job {result.get('status', 'failed')}"
                    error_class = result.get("errorClass") or "permanent"
                if error_class == "transient" and row["attempts"] < row["max_attempts"]:
                    retry_at = now + min(60.0, self.backoff_base * (2 ** (row["attempts"] - 1)))
                else:
                    state = "failed"
            if retry_at is not None:
                params = resume_params(row["kind"], json.loads(row["params"]))
                conn.execute("UPDATE jobs SET state='queued', next_run_at=?, error=?, error_class=?, params=?, pid=NULL, "
                             "lease_owner=NULL, lease_expires=NULL, result=NULL, updated=? WHERE id=?",
                             (retry_at, error, error_class, json.dumps(params), now, job_id))
                self._event(conn, job_id, "retry", dict(error=error, at=retry_at, attempt=row["attempts"]))
            else:
                conn.execute("UPDATE jobs SET state=?, finished=?, updated=?, exit_code=?, error=COALESCE(?, error), "
                             "error_class=COALESCE(?, error_class), result=COALESCE(result, ?), pid=pid WHERE id=?",
                             (state, now, now, exit_code, error, error_class,
                              json.dumps(result) if result is not None else None, job_id))
                self._event(conn, job_id, state, dict(exit=exit_code, adopted=adopted))
            self._release(conn, job_id)
        row = self.get(job_id)
        if row["state"] in TERMINAL and row["audit"]:
            self._audit(row)
        self.on_change(row)
        self.wake()

    def _release(self, conn, job_id):
        conn.execute("DELETE FROM locks WHERE holder=?", ("job:" + job_id,))

    def _event(self, conn, job_id, event, data):
        conn.execute("INSERT INTO job_events(job_id, ts, event, data) VALUES(?,?,?,?)",
                     (job_id, time.time(), event, json.dumps(data) if data is not None else None))

    def _audit(self, row):
        """Same audit line the MCP JobManager wrote (job:<kind> with the output fingerprint)."""
        try:
            from types import SimpleNamespace
            from eod_mcp.safety import Audit, fingerprint
            params = json.loads(row["params"])
            dest = params.get("dest") or params.get("run_dir")
            files = []
            if dest:
                try:
                    rel = Path(dest).resolve().relative_to(self.ws).as_posix()
                except ValueError:
                    rel = str(dest)
                files = [dict(path=rel, before=None, after=fingerprint(dest))]
            Audit(SimpleNamespace(workspace=self.ws)).write("job:" + row["kind"], dict(job_id=row["id"]), None, files,
                                                            ok=row["state"] == "succeeded", note=row["state"])
        except Exception as error:  # noqa: BLE001
            print(f"kit-service: audit failed: {error}", file=sys.stderr, flush=True)


# ---------------------------------------------------------------------- resource locks for synchronous calls

class Locks:
    def __init__(self, store):
        self.store = store

    def acquire(self, resources, holder, pid=None, wait=30.0, note="", reject_if=None):
        """Take every resource or none. Waits (serialises) up to `wait` seconds for another holder."""
        deadline = time.time() + max(0.0, float(wait))
        conn = self.store.conn()
        while True:
            with kdb.tx(conn):
                taken = []
                for r in resources:
                    lock = conn.execute("SELECT holder, note FROM locks WHERE resource=?", (r,)).fetchone()
                    if lock and lock["holder"] != holder:
                        if holder_alive(conn, lock["holder"]):
                            taken.append((r, lock["holder"], lock["note"]))
                        else:
                            conn.execute("DELETE FROM locks WHERE resource=?", (r,))
                if not taken:
                    for r in resources:
                        conn.execute("INSERT OR REPLACE INTO locks(resource, holder, pid, job_id, acquired, note) "
                                     "VALUES(?,?,?,?,?,?)", (r, holder, pid, None, time.time(), note))
                    return
            if reject_if:
                reject_if(taken)
            if time.time() >= deadline:
                what = ", ".join(_describe(r) for r, _, _ in taken)
                raise KitError("BUSY", f"Another Kit operation is changing {what}, so this change was not made.",
                               "Try again when the other operation finishes.", dict(holders=[h for _, h, _ in taken]))
            time.sleep(0.05)

    def release(self, resources, holder):
        conn = self.store.conn()
        for r in resources:
            conn.execute("DELETE FROM locks WHERE resource=? AND holder=?", (r, holder))

    def list(self):
        conn = self.store.conn()
        return [dict(resource=r["resource"], holder=r["holder"], jobId=r["job_id"], acquired=r["acquired"],
                     note=r["note"], alive=holder_alive(conn, r["holder"])) for r in conn.execute("SELECT * FROM locks")]


def _describe(resource):
    kind, _, value = resource.partition(":")
    name = Path(value).name if value else ""
    return {"profile": f"profile {name}", "project": f"project {name}", "export": f"export folder {name}",
            "index": "the asset index", "game": "the game test session"}.get(kind, resource)


# ---------------------------------------------------------------------- kind specifics

def default_resources(kind, params):
    if kind == "texture_export" and params.get("dest"):
        return ["export:" + str(Path(params["dest"]).resolve())]
    if kind in ("census", "index_build") and params.get("disc"):
        return ["index:" + str(Path(params["disc"]).resolve())]
    if kind == "test_run":
        out = ["game"]
        mod_dir = (params.get("env") or {}).get("EOD_MOD_DIR")
        if mod_dir and Path(mod_dir).name == "active":
            out.append("profile:" + str(Path(mod_dir).resolve().parent))
        return out
    if kind == "cli" and (params.get("dest") or params.get("out")):
        return ["export:" + str(Path(params.get("dest") or params["out"]).resolve())]
    if kind == "stub":
        return list(params.get("resources") or [])
    return []


def input_hashes(conn, kind, params):
    """Content hashes of the job's inputs (cached by file identity in file_hashes)."""
    from . import index
    disc = params.get("disc")
    if not disc or kind not in ("texture_export", "census", "index_build"):
        return {}
    disc = Path(disc).resolve()
    if not (disc / "DATA").is_dir():
        return {}
    if kind == "texture_export" and params.get("archives"):
        rels = [a if str(a).upper().startswith("DATA/") else "DATA/" + str(a) for a in params["archives"]]
        rels = [r for r in rels if (disc / r).is_file()]
    elif kind == "texture_export":
        rels = [r for r in index.walk(disc) if Path(r).suffix.lower() in (".car", ".res", ".trk")]
    else:
        rels = index.walk(disc)
    out = index.content_hashes(conn, disc, rels)
    slus = disc / "SLUS_209.10"
    if slus.is_file():
        out["SLUS_209.10"] = index.file_sha(conn, slus)
    return out


def outputs_intact(row):
    result = json.loads(row["result"] or "null") or {}
    for path, digest in (result.get("outputs") or {}).items():
        try:
            from eod_mcp.safety import sha_file
            if sha_file(path) != digest:
                return False
        except OSError:
            return False
    return True


def resume_params(kind, params):
    if kind == "texture_export" and params.get("dest") and (Path(params["dest"]) / "export-manifest.json").is_file():
        params = dict(params, resume=True)
    return params


def _read_result(ws, job_id):
    try:
        return json.loads((Path(ws) / "jobs" / job_id / "result.json").read_text())
    except (OSError, ValueError):
        return None


def _zombie(pid):
    """A child that exited but was not reaped still answers kill(0)."""
    try:
        done, _ = os.waitpid(pid, os.WNOHANG)
        return bool(done)
    except ChildProcessError:
        return False
    except OSError:
        return False
