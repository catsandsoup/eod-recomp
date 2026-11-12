#!/usr/bin/env python3
"""Kit service concurrency lanes: interactive reads stay fast while long work runs elsewhere.

    EOD_HEADLESS=1 EOD_MUTE=1 python3 tools/tests/test_kit_service_lanes.py

Covers: 3D model inspects run in "model" lane processes and writes in "write" lane
processes (never in the service interpreter); a dead lane process is replaced; job
lanes have separate slots (a full export lane does not delay an index build or a
game run); and inspect latency of 20 assets while a long export job runs stays
within 1.5x of idle.
"""
import json
import os
import shutil
import signal
import statistics
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.append(str(Path(__file__).resolve().parents[1]))
os.environ.update(EOD_KIT_SERVICE_IDLE="120", EOD_KIT_BACKOFF_BASE="0.1", EOD_KIT_LEASE_SECONDS="4", EOD_KIT_LANE_EXPORT="2",
                  EOD_KIT_HEARTBEAT_SECONDS="0.5")
import test_asset_registry as tar  # noqa: E402
from test_kit_service import make_disc, service_pid, spawn_backend, stop_service, wait_job  # noqa: E402
from kit_service.client import Client  # noqa: E402

ASSETS = 20


class LaneCase(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="kitlanes-")).resolve()
        self.disc = make_disc(self.tmp)
        tar.pack(self.disc / "DATA/TRACKS/BIG.TRK",
                 [(f"t{i:03d}.tex", "TEX", tar.make_tex(i % 200, 64)) for i in range(120)])
        self.ws = self.tmp / "ws"
        self.addCleanup(lambda: shutil.rmtree(self.tmp, ignore_errors=True))
        self.addCleanup(lambda: stop_service(self.ws))
        self.client = Client(self.ws)

    def backend(self, **request):
        return json.loads(self.client.raw("backend", request))["result"]

    def req(self, action, **kw):
        return dict(action=action, disc=str(self.disc), project=str(self.tmp / "proj/project.eodproject"), **kw)

    def lanes(self):
        return self.client.call("service.status")["lanes"]

    def until(self, predicate, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            value = predicate()
            if value:
                return value
            time.sleep(0.05)
        self.fail("condition not reached")


class BackendLaneTest(LaneCase):
    def test_model_inspect_runs_in_model_lane_and_matches_helper(self):
        request = self.req("inspect", archive="DATA/CAR/PALO.CAR", asset="Low0.mod")
        got = self.backend(**request)
        self.assertEqual(got, spawn_backend(request))
        lanes = self.lanes()
        self.assertEqual(lanes["model"]["calls"], 1)
        self.assertNotIn(service_pid(self.ws), lanes["model"]["pids"])
        # a texture inspect stays on the service's read threads
        self.backend(**self.req("inspect", archive="DATA/TRACKS/ANOKA.TRK", asset="sky01A.tex"))
        self.assertEqual(self.lanes()["model"]["calls"], 1)

    def test_dead_model_lane_process_is_replaced(self):
        request = self.req("inspect", archive="DATA/CAR/PALO.CAR", asset="Low0.mod")
        self.backend(**request)
        for pid in self.lanes()["model"]["pids"]:
            os.kill(pid, signal.SIGKILL)
        self.client.call("cache.clear")
        self.assertEqual(self.backend(**request), spawn_backend(request))

    def test_writes_run_in_write_lane(self):
        base = self.req("inspect", archive="DATA/CAR/ALGAR.CAR", asset="physics.val")
        saved = self.backend(**dict(base, action="save", edits=[dict(section="Chassis", key="mass", value="4100")]))
        self.assertTrue(saved["ok"], saved)
        self.assertEqual(self.lanes()["write"]["calls"], 1)
        after = self.backend(**base)["result"]["sections"][0]["fields"][0]["value"]
        self.assertEqual(after, "4100")
        state = self.client.call("state.get", dict(kind="project", project=base["project"]))
        self.assertEqual(state["state"], "saved")


class JobLaneTest(LaneCase):
    def stub(self, **params):
        return self.client.call("job.submit", dict(kind="stub", params=params))

    def test_full_export_lane_does_not_delay_index_or_game_lane(self):
        exports = [self.stub(steps=200, sleep=0.05, tag=i) for i in range(3)]
        self.until(lambda: sum(self.client.call("job.status", dict(job_id=j["job_id"]))["state"] == "running"
                               for j in exports) == 2)
        third = self.client.call("job.status", dict(job_id=exports[2]["job_id"]))
        self.assertEqual(third["state"], "queued", "export lane has two slots")
        index = self.stub(steps=2, sleep=0.05, lane="index")
        game = self.stub(steps=2, sleep=0.05, lane="game")
        for job in (index, game):
            done = wait_job(self.client, job["job_id"], timeout=15)
            self.assertEqual(done["state"], "succeeded", done)
        self.assertEqual(self.client.call("job.status", dict(job_id=exports[2]["job_id"]))["state"], "queued")
        lanes = {j["job_id"]: j["lane"] for j in self.client.call("job.list")["jobs"]}
        self.assertEqual(lanes[index["job_id"]], "index")
        self.assertEqual(lanes[game["job_id"]], "game")
        for job in exports:
            self.client.call("job.cancel", dict(job_id=job["job_id"]))


class ReleaseWhenIdleTest(LaneCase):
    """Nothing is held for its own sake: idle lane processes exit and idle caches are dropped."""
    def setUp(self):
        os.environ.update(EOD_KIT_LANE_IDLE="1", EOD_KIT_CACHE_IDLE="2")
        super().setUp()
        self.addCleanup(lambda: [os.environ.pop(k, None) for k in ("EOD_KIT_LANE_IDLE", "EOD_KIT_CACHE_IDLE")])

    def test_idle_lanes_exit_and_caches_empty(self):
        self.backend(**self.req("inspect", archive="DATA/CAR/PALO.CAR", asset="Low0.mod"))
        self.backend(**self.req("inspect", archive="DATA/TRACKS/ANOKA.TRK", asset="sky01A.tex"))
        status = self.client.call("service.status")
        self.assertEqual(status["lanes"]["model"]["processes"], 1)
        self.assertEqual(status["lanes"]["write"]["processes"], 0, "lanes start on demand only")
        self.assertGreater(status["caches"]["payloads"]["bytes"], 0)
        time.sleep(4.5)  # no requests: lane idle 1 s, cache idle 2 s, watchdog ticks every second
        status = self.client.call("service.status")
        self.assertEqual(status["lanes"]["model"]["processes"], 0)
        self.assertEqual(sum(c["bytes"] for c in status["caches"].values()), 0)
        # and work still runs afterwards
        self.assertTrue(self.backend(**self.req("inspect", archive="DATA/CAR/PALO.CAR", asset="Low0.mod"))["ok"])


def _inspect_pass(case):
    """Inspect ASSETS distinct textures with cold caches; returns per-request wall times (ms)."""
    case.client.call("cache.clear")
    times = []
    for i in range(ASSETS):
        request = case.req("inspect", archive="DATA/TRACKS/BIG.TRK", asset=f"t{i:03d}.tex")
        start = time.perf_counter()
        envelope = case.backend(**request)
        times.append((time.perf_counter() - start) * 1000)
        case.assertTrue(envelope["ok"], envelope)
    return times


class InspectWhileExportTest(LaneCase):
    def setUp(self):
        os.environ["EOD_MCP_JOB_DELAY"] = "0.02"
        super().setUp()
        self.addCleanup(lambda: os.environ.pop("EOD_MCP_JOB_DELAY", None))

    def test_inspect_latency_while_export_runs(self):
        _inspect_pass(self)  # warm imports
        idle = [statistics.median(_inspect_pass(self)) for _ in range(3)]
        job = self.client.call("job.submit", dict(kind="texture_export", params=dict(
            disc=str(self.disc), dest=str(self.tmp / "export"), archives=["DATA/TRACKS/BIG.TRK"], jobs=1)))
        self.until(lambda: (self.client.call("job.status", dict(job_id=job["job_id"]))["done"] or 0) >= 2)
        loaded = [statistics.median(_inspect_pass(self)) for _ in range(3)]
        status = self.client.call("job.status", dict(job_id=job["job_id"]))
        self.assertEqual(status["state"], "running", "the export must still be running while we measure")
        idle_ms, loaded_ms = statistics.median(idle), statistics.median(loaded)
        report = dict(idle_median_ms=round(idle_ms, 2), loaded_median_ms=round(loaded_ms, 2),
                      ratio=round(loaded_ms / idle_ms, 2), assets=ASSETS, export_done=status["done"],
                      export_total=status["total"])
        print("inspect-while-export:", json.dumps(report), file=sys.stderr)
        out = os.environ.get("EOD_KIT_LANES_REPORT")
        if out:
            Path(out).write_text(json.dumps(report, indent=2))
        # 1.5x of idle, with a 3 ms floor so sub-millisecond noise on a fast machine can't fail it
        self.assertLessEqual(loaded_ms, max(1.5 * idle_ms, idle_ms + 3.0), report)
        self.client.call("job.cancel", dict(job_id=job["job_id"]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
