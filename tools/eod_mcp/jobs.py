"""Job manager: child-process jobs with progress.jsonl, status long-poll, cancel."""
import json
import os
import secrets
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

from .safety import ToolError, redact

WORKER = str(Path(__file__).resolve().parent / "job_worker.py")
TERMINAL = ("succeeded", "failed", "cancelled")


def _pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


class JobManager:
    def __init__(self, config, on_change=None):
        self.config = config
        self.root = config.workspace / "jobs"
        self.root.mkdir(parents=True, exist_ok=True)
        self.procs = {}
        self.cancelled = set()
        self.lock = threading.Lock()
        self.on_change = on_change or (lambda: None)
        self.game_lock = threading.Lock()
        self.finish_hook = None
        self._recover()

    def _recover(self):
        for d in self.root.iterdir():
            j = self._load(d.name)
            if j and j["state"] in ("queued", "running") and not (j.get("pid") and _pid_alive(j["pid"])):
                j.update(state="failed", error="server_restart", finished=time.time())
                self._save(j)

    def _dir(self, job_id):
        if not job_id or "/" in job_id or ".." in job_id:
            raise ToolError("JOB_NOT_FOUND", "Bad job id")
        return self.root / job_id

    def _load(self, job_id):
        try:
            return json.loads((self._dir(job_id) / "job.json").read_text())
        except (OSError, ValueError, ToolError):
            return None

    def _save(self, j):
        d = self._dir(j["job_id"])
        tmp = d / "job.json.tmp"
        tmp.write_text(json.dumps(j, indent=1))
        os.replace(tmp, d / "job.json")

    def start(self, kind, params, label=""):
        if kind == "test_run":
            if not self.game_lock.acquire(blocking=False):
                raise ToolError("CONFLICT", "A game run is already in progress", "Wait for it or call eod_job_cancel")
        job_id = time.strftime("%Y%m%d-%H%M%S-") + secrets.token_hex(3)
        d = self.root / job_id
        d.mkdir(parents=True)
        j = dict(job_id=job_id, kind=kind, label=label, state="queued", params=params, created=time.time())
        (d / "job.json").write_text(json.dumps(j))
        log = open(d / "job.log", "wb")
        env = {k: v for k, v in os.environ.items() if k in ("PATH", "HOME", "TMPDIR", "LANG", "LC_ALL", "EOD_MCP_JOB_DELAY")}
        proc = subprocess.Popen([sys.executable, "-I", WORKER, str(d)], stdout=log, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL, start_new_session=True, env=env)
        log.close()
        j.update(state="running", pid=proc.pid, started=time.time())
        self._save(j)
        with self.lock:
            self.procs[job_id] = proc
        threading.Thread(target=self._watch, args=(job_id, proc, kind), daemon=True).start()
        return dict(job_id=job_id, kind=kind, state="running")

    def _watch(self, job_id, proc, kind):
        rc = proc.wait()
        d = self._dir(job_id)
        j = self._load(job_id) or {}
        result = None
        try:
            result = json.loads((d / "result.json").read_text())
        except (OSError, ValueError):
            pass
        if job_id in self.cancelled and not (result and result.get("status") == "complete"):
            state = "cancelled"
        elif rc == 0 and result and result.get("ok"):
            state = "succeeded"
        else:
            state = "failed"
            if result is None:
                j["error"] = f"worker exited with code {rc} without a result"
        j.update(state=state, finished=time.time(), exit_code=rc)
        self._save(j)
        if kind == "test_run":
            self.game_lock.release()
        if self.finish_hook:
            try:
                self.finish_hook(j, result)
            except Exception:  # noqa: BLE001
                pass
        self.on_change()

    def cancel(self, job_id):
        j = self._load(job_id)
        if not j:
            raise ToolError("JOB_NOT_FOUND", f"No job {job_id}", "Call eod_job_list")
        if j["state"] in TERMINAL:
            return dict(job_id=job_id, state=j["state"], note="already finished")
        proc = self.procs.get(job_id)
        pid = j.get("pid")
        self.cancelled.add(job_id)
        target = proc.pid if proc else pid
        try:
            os.killpg(target, signal.SIGTERM)
        except OSError:
            pass

        def reaper():
            for _ in range(50):
                if not _pid_alive(target) or (proc and proc.poll() is not None):
                    return
                time.sleep(0.1)
            try:
                os.killpg(target, signal.SIGKILL)
            except OSError:
                pass
        threading.Thread(target=reaper, daemon=True).start()
        return dict(job_id=job_id, state="cancelling")

    def _progress(self, job_id):
        p = self._dir(job_id) / "progress.jsonl"
        if not p.is_file():
            return None, 0
        lines = p.read_text().splitlines()
        try:
            return json.loads(lines[-1]), len(lines)
        except (IndexError, ValueError):
            return None, len(lines)

    def status(self, job_id, wait_seconds=0, since=None, notify=None):
        j = self._load(job_id)
        if not j:
            raise ToolError("JOB_NOT_FOUND", f"No job {job_id}", "Call eod_job_list")
        deadline = time.time() + min(max(wait_seconds or 0, 0), 25)
        _, base = self._progress(job_id)
        base = since if since is not None else base
        while True:
            j = self._load(job_id)
            last, n = self._progress(job_id)
            if notify and last and n != getattr(self, "_n_" + job_id, -1):
                setattr(self, "_n_" + job_id, n)
                notify(last.get("done"), last.get("total"), last.get("message"))
            if j["state"] in TERMINAL or n > base or time.time() >= deadline:
                break
            time.sleep(0.2)
        out = dict(job_id=job_id, kind=j["kind"], state=j["state"], progress_count=n,
                   done=(last or {}).get("done"), total=(last or {}).get("total"),
                   message=(last or {}).get("message"), current=(last or {}).get("current"))
        if j.get("error"):
            out["error"] = j["error"]
        log = self._dir(job_id) / "job.log"
        out["log_tail"] = redact(log.read_text(errors="replace")).splitlines()[-20:] if log.is_file() else []
        if j["state"] in TERMINAL:
            try:
                out["result"] = json.loads((self._dir(job_id) / "result.json").read_text())
            except (OSError, ValueError):
                out["result"] = None
            out["result_link"] = f"eod://jobs/{job_id}"
        return out

    def list(self, state=None):
        rows = []
        for d in sorted(self.root.iterdir(), reverse=True):
            j = self._load(d.name)
            if j and (state is None or j["state"] == state):
                rows.append(dict(job_id=j["job_id"], kind=j["kind"], state=j["state"], label=j.get("label", ""), created=j["created"]))
        return rows
