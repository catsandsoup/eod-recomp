#!/usr/bin/env python3
"""Kit service job queue: idempotent submit, one game run at a time (stub executable, the
game is never launched), profile write locks, transient retries, cancel, kill -9 recovery
(requeue from checkpoint and adoption of a surviving worker) and batch texture export resume.

    EOD_HEADLESS=1 EOD_MUTE=1 python3 tools/tests/test_kit_service_jobs.py
"""
import json
import os
import shutil
import signal
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.append(str(TOOLS))
os.environ.update(EOD_KIT_SERVICE_IDLE="120", EOD_KIT_BACKOFF_BASE="0.1", EOD_KIT_LEASE_SECONDS="4",
                  EOD_KIT_HEARTBEAT_SECONDS="0.5")
import test_asset_registry as tar  # noqa: E402
import test_mod_package as tmp_pkg  # noqa: E402
from test_kit_service import service_pid, stop_service, wait_job  # noqa: E402
from kit_service.client import Client  # noqa: E402
from kit_service.common import KitError  # noqa: E402

STUB_GAME = """#!/bin/sh
# Stand-in for the game executable: never renders, just produces the capture the job expects.
sleep "${STUB_SECONDS:-2}"
printf 'png' > "$EOD_EDITOR_PREVIEW"
"""


class JobCase(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="kitjobs-")).resolve()
        self.ws = self.tmp / "ws"
        self.client = Client(self.ws)
        self.client.call("service.ping")
        self.addCleanup(lambda: shutil.rmtree(self.tmp, ignore_errors=True))
        self.addCleanup(lambda: stop_service(self.ws))

    def stub(self, **params):
        return self.client.call("job.submit", dict(kind="stub", params=params))

    def until(self, predicate, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            value = predicate()
            if value:
                return value
            time.sleep(0.05)
        self.fail("condition not reached")

    def worker_pid(self, job_id):
        return self.until(lambda: self.client.call("job.status", dict(job_id=job_id))["pid"])

    def kill_service(self):
        pid = service_pid(self.ws)
        os.kill(pid, signal.SIGKILL)
        self.until(lambda: not _alive(pid))


def _alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


class SubmitTest(JobCase):
    def test_duplicate_submit_returns_same_job(self):
        a = self.stub(steps=30, sleep=0.05, tag="x")
        b = self.stub(steps=30, sleep=0.05, tag="x")
        c = self.stub(steps=30, sleep=0.05, tag="y")
        self.assertEqual(a["job_id"], b["job_id"])
        self.assertTrue(b["duplicate"])
        self.assertNotEqual(a["job_id"], c["job_id"])
        ids = {j["job_id"] for j in self.client.call("job.list")["jobs"]}
        self.assertEqual(ids, {a["job_id"], c["job_id"]})

    def test_transient_errors_retry_with_backoff_permanent_fail(self):
        job = self.stub(steps=2, sleep=0.01, fail_transient=2)
        done = wait_job(self.client, job["job_id"])
        self.assertEqual(done["state"], "succeeded", done)
        self.assertEqual(done["attempts"], 3)
        retries = [e for e in self.client.call("job.events", dict(job_id=job["job_id"]))["events"] if e["event"] == "retry"]
        self.assertEqual(len(retries), 2)
        gaps = [r["data"]["at"] - r["ts"] for r in retries]
        self.assertLess(gaps[0], gaps[1], "backoff must grow")
        bad = wait_job(self.client, self.stub(fail_permanent=True)["job_id"])
        self.assertEqual((bad["state"], bad["attempts"], bad["errorClass"]), ("failed", 1, "permanent"))
        self.assertIn("simulated permanent failure", bad["error"])

    def test_cancel_running_and_queued(self):
        running = self.stub(steps=200, sleep=0.05, resources=["export:/x"])
        queued = self.stub(steps=5, sleep=0.05, resources=["export:/x"])  # same resource: waits
        self.until(lambda: self.client.call("job.status", dict(job_id=running["job_id"]))["done"])
        self.assertEqual(self.client.call("job.status", dict(job_id=queued["job_id"]))["state"], "queued")
        self.assertEqual(self.client.call("job.cancel", dict(job_id=queued["job_id"]))["state"], "cancelled")
        self.assertEqual(self.client.call("job.cancel", dict(job_id=running["job_id"]))["state"], "cancelling")
        self.assertEqual(wait_job(self.client, running["job_id"])["state"], "cancelled")


class GameRunTest(JobCase):
    def test_one_game_run_at_a_time(self):
        exe = self.tmp / "stub-game.sh"
        exe.write_text(STUB_GAME)
        exe.chmod(0o755)
        run = self.tmp / "runs" / "one"

        def params(name):
            folder = self.tmp / "runs" / name
            return dict(executable=str(exe), arguments=["--headless"], timeout_s=30, run_dir=str(folder),
                        capture=str(folder / "capture.png"), run_id=name,
                        env=dict(PATH=os.environ["PATH"], STUB_SECONDS="2", EOD_EDITOR_PREVIEW=str(folder / "capture.png")))
        first = self.client.call("job.submit", dict(kind="test_run", params=params("one")))
        with self.assertRaises(KitError) as ctx:
            self.client.call("job.submit", dict(kind="test_run", params=params("two")))
        self.assertEqual(ctx.exception.code, "CONFLICT")
        self.assertIn("already in progress", ctx.exception.message)
        with self.assertRaises(KitError):  # the app's game lock is refused too
            self.client.call("lock.acquire", dict(resource="game", pid=os.getpid(), owner="app"))
        done = wait_job(self.client, first["job_id"])
        self.assertEqual(done["state"], "succeeded", done)
        self.assertTrue((run / "capture.png").is_file())
        # once finished, the app may take the slot; then an MCP run is refused
        self.client.call("lock.acquire", dict(resource="game", pid=os.getpid(), owner="app"))
        with self.assertRaises(KitError):
            self.client.call("job.submit", dict(kind="test_run", params=params("three")))
        self.client.call("lock.release", dict(resource="game", pid=os.getpid(), owner="app"))
        third = self.client.call("job.submit", dict(kind="test_run", params=params("three")))
        self.assertEqual(wait_job(self.client, third["job_id"])["state"], "succeeded")


class ProfileLockTest(JobCase):
    def setUp(self):
        super().setUp()
        import mod_install
        self.disc = tmp_pkg.make_disc(self.tmp)
        self.root = self.tmp / "profiles"
        pkg = tmp_pkg.make_package(self.tmp / "pkg", self.disc, "hd", {(tmp_pkg.ANOKA, "sky.tex"): tmp_pkg.make_tex(9)})
        mod_install.install(str(pkg), str(self.disc), "dev", str(self.root))

    def call(self, action, **kw):
        request = dict(action=action, disc=str(self.disc), project=str(self.tmp / "p/project.eodproject"),
                       profile="dev", profileRoot=str(self.root), **kw)
        return json.loads(self.client.raw("backend", request))["result"]

    def test_concurrent_profile_writes_are_serialised_or_rejected(self):
        results = []
        threads = [threading.Thread(target=lambda a=a: results.append(self.call(a, id="hd")))
                   for a in ("profile_disable", "profile_enable", "profile_disable", "profile_enable")]
        [t.start() for t in threads]
        [t.join(60) for t in threads]
        self.assertEqual([r["ok"] for r in results], [True] * 4, results)  # serialised, none collided on flock
        holder = dict(resource="profile:" + str((self.root / "dev").resolve()), pid=os.getpid(), owner="other-tool")
        self.client.call("lock.acquire", holder)
        busy = self.call("profile_disable", id="hd", lockWait=0)
        self.assertFalse(busy["ok"])
        self.assertIn("Another Kit operation is changing profile dev", busy["error"])
        self.client.call("lock.release", holder)
        self.assertTrue(self.call("profile_disable", id="hd")["ok"])
        missing = self.call("profile_enable", id="nope")
        self.assertFalse(missing["ok"])
        self.assertIn("nope is not installed in profile dev", missing["error"])
        st = self.client.call("state.get", dict(kind="package", profile="dev", profileRoot=str(self.root), id="hd"))
        self.assertEqual(st["state"], "disabled")
        self.assertIn("enable", st["allowed"])

    def test_profile_in_use_by_game_run_is_not_changed(self):
        exe = self.tmp / "stub-game.sh"
        exe.write_text(STUB_GAME)
        exe.chmod(0o755)
        folder = self.tmp / "runs/r"
        active = (self.root / "dev" / "active").resolve()
        job = self.client.call("job.submit", dict(kind="test_run", params=dict(
            executable=str(exe), arguments=[], timeout_s=30, run_dir=str(folder), capture=str(folder / "capture.png"),
            run_id="r", env=dict(PATH=os.environ["PATH"], STUB_SECONDS="3", EOD_MOD_DIR=str(active),
                                 EOD_EDITOR_PREVIEW=str(folder / "capture.png")))))
        self.until(lambda: self.client.call("job.status", dict(job_id=job["job_id"]))["state"] == "running")
        refused = self.call("profile_disable", id="hd")
        self.assertFalse(refused["ok"])
        self.assertIn("in use by a running game test", refused["error"])
        wait_job(self.client, job["job_id"])
        self.assertTrue(self.call("profile_disable", id="hd")["ok"])


class CrashRecoveryTest(JobCase):
    def test_kill_service_and_worker_requeues_from_checkpoint(self):
        out = self.tmp / "out"
        job = self.stub(steps=40, sleep=0.05, out=str(out))
        self.until(lambda: (self.client.call("job.status", dict(job_id=job["job_id"]))["done"] or 0) >= 10)
        worker = self.worker_pid(job["job_id"])
        self.kill_service()
        os.kill(worker, signal.SIGKILL)
        done = wait_job(Client(self.ws), job["job_id"])
        self.assertEqual(done["state"], "succeeded", done)
        self.assertEqual(done["recoveries"], 1)
        self.assertGreater(done["result"]["resumedFrom"], 0)
        writes = (out / "writes.log").read_text().split()
        self.assertEqual(sorted(set(writes), key=int), [str(i) for i in range(40)])
        self.assertLessEqual(len(writes) - len(set(writes)), 1, "at most the step in flight at the kill repeats")

    def test_kill_service_only_adopts_running_worker(self):
        job = self.stub(steps=30, sleep=0.05, out=str(self.tmp / "out"))
        worker = self.worker_pid(job["job_id"])
        self.kill_service()
        self.assertTrue(_alive(worker))
        done = wait_job(Client(self.ws), job["job_id"])
        self.assertEqual((done["state"], done["recoveries"], done["attempts"]), ("succeeded", 0, 1))
        events = [e["event"] for e in Client(self.ws).call("job.events", dict(job_id=job["job_id"]))["events"]]
        self.assertIn("adopted", events)

    def test_texture_export_killed_mid_batch_resumes_without_duplicates(self):
        os.environ["EOD_MCP_JOB_DELAY"] = "0.05"
        stop_service(self.ws)  # restart with the slower progress so the kill lands mid batch
        self.client = Client(self.ws)
        try:
            disc = self.tmp / "disc"
            (disc / "DATA").mkdir(parents=True)
            (disc / "SLUS_209.10").write_bytes(b"synthetic")
            tar.pack(disc / "DATA/TRACKS/ANOKA.TRK", [(f"t{i:03d}.tex", "TEX", tar.make_tex(i)) for i in range(40)])
            dest = self.tmp / "export"
            job = self.client.call("job.submit", dict(kind="texture_export", params=dict(
                disc=str(disc), dest=str(dest), archives=["DATA/TRACKS/ANOKA.TRK"], jobs=1)))
            self.until(lambda: (self.client.call("job.status", dict(job_id=job["job_id"]))["done"] or 0) >= 8)
            worker = self.worker_pid(job["job_id"])
            self.kill_service()
            os.killpg(worker, signal.SIGKILL)
        finally:
            os.environ.pop("EOD_MCP_JOB_DELAY", None)
        done = wait_job(Client(self.ws), job["job_id"])
        self.assertEqual(done["state"], "succeeded", done)
        self.assertEqual(done["recoveries"], 1)
        manifest = json.loads((dest / "export-manifest.json").read_text())
        ids = [a["assetId"] for a in manifest["assets"]]
        self.assertEqual(len(ids), 40)
        self.assertEqual(len(set(ids)), 40)
        self.assertEqual(manifest["summary"]["exported"], 40)
        # Textures finished before the kill are skipped (checkpointed) or found byte-identical and left alone.
        kept = [a for a in manifest["assets"] if a.get("written") in ("skipped-resumed", "unchanged")]
        self.assertGreaterEqual(len(kept), 8, [a.get("written") for a in manifest["assets"]])
        pngs = list((dest / "textures").rglob("*.png"))
        self.assertEqual(len(pngs), 40)
        self.assertEqual(list((dest / "textures").rglob("*.tmp")), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
