#!/usr/bin/env python3
"""Kit service: protocol, backend envelope parity with the spawn-per-call helper, caches,
SQLite index (census parity, incremental update, FTS, thumbnails, tags), migrations,
auto-start / stale-socket recovery, stdio mode and state machines.

    EOD_HEADLESS=1 EOD_MUTE=1 python3 tools/tests/test_kit_service.py
"""
import contextlib
import io
import json
import os
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.append(str(TOOLS))
import test_asset_registry as tar  # noqa: E402
import asset_registry  # noqa: E402
from valt import compile_binary_valt  # noqa: E402
from kit_service import db as kdb, states  # noqa: E402
from kit_service import common as C  # noqa: E402
from kit_service.client import Client  # noqa: E402
from kit_service.common import KitError  # noqa: E402

BACKEND = TOOLS / "modding_backend.py"
SERVER = TOOLS / "kit_service" / "server.py"
os.environ.setdefault("EOD_KIT_SERVICE_IDLE", "120")


def make_disc(root):
    disc = tar.make_disc(root)
    table = dict(tool_vtable=0, unk1=0, unk2=0, sections=[dict(name="Chassis", parent="", sec_vtable=0, tool_ptr=0,
                                                              pairs=[("mass", "2800"), ("grip", "0.90")])])
    tar.pack(disc / "DATA/CAR/ALGAR.CAR", [("physics.val", "VALT", compile_binary_valt(table)),
                                           ("blob.bin", "TEST", bytes(range(200)))])
    return disc


def spawn_backend(request):
    """Exactly how the Swift app called the backend before the service."""
    out = subprocess.run(["/usr/bin/python3", "-I", str(BACKEND)], input=json.dumps(request).encode(),
                         capture_output=True, check=False).stdout
    return json.loads(out)


def service_pid(ws):
    return (C.read_service_info(ws) or {}).get("pid")


def stop_service(ws):
    pid = service_pid(ws)
    try:
        Client(ws, autostart=False).call("service.shutdown")
    except Exception:  # noqa: BLE001
        pass
    for _ in range(100):
        if not pid or not _alive(pid):
            return
        time.sleep(0.05)
    with contextlib.suppress(OSError):
        os.kill(pid, signal.SIGKILL)


def _alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def wait_job(client, job_id, timeout=120):
    end = time.time() + timeout
    while time.time() < end:
        s = client.call("job.status", dict(job_id=job_id, wait_seconds=5))
        if s["state"] in ("succeeded", "failed", "cancelled"):
            return s
    raise AssertionError("job timed out: " + job_id)


class ServiceCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="kitsvc-")).resolve()
        cls.disc = make_disc(cls.tmp)
        cls.ws = cls.tmp / "ws"
        cls.client = Client(cls.ws)
        cls.client.call("service.ping")

    @classmethod
    def tearDownClass(cls):
        stop_service(cls.ws)
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def backend(self, **request):
        return json.loads(self.client.raw("backend", request))["result"]

    def req(self, action, **kw):
        return dict(action=action, disc=str(self.disc), project=str(self.tmp / "proj/project.eodproject"), **kw)


class BackendParityTest(ServiceCase):
    def test_envelopes_match_spawn_per_call(self):
        cases = [self.req("catalog"), self.req("entries", archive="DATA/TRACKS/ANOKA.TRK"),
                 self.req("inspect", archive="DATA/TRACKS/ANOKA.TRK", asset="sky01A.tex"),
                 self.req("inspect", archive="DATA/CAR/ALGAR.CAR", asset="physics.val"),
                 self.req("inspect", archive="DATA/CAR/ALGAR.CAR", asset="blob.bin"),
                 self.req("environment", archive="DATA/TRACKS/ANOKA.TRK"),
                 self.req("entries", archive="DATA/NOPE.TRK"), self.req("entries", archive="../etc/passwd"),
                 dict(action="catalog", disc=str(self.tmp / "missing")),
                 self.req("profile_enable", id="missing", profileRoot=str(self.tmp / "profiles"))]
        for request in cases:
            with self.subTest(action=request["action"], archive=request.get("archive"), asset=request.get("asset")):
                ref = spawn_backend(request)
                got = self.backend(**request)
                if request["action"] == "profile_enable":  # state machine words the rejection; both refuse
                    self.assertFalse(ref["ok"])
                    self.assertFalse(got["ok"])
                    self.assertRegex(got["error"], "not installed|doesn't exist")
                    continue
                self.assertEqual(got, ref)
                # second call is served from the response cache and must be identical
                self.assertEqual(self.backend(**request), ref)

    def test_save_validate_and_cache_invalidation(self):
        project = str(self.tmp / "save/project.eodproject")
        base = dict(action="inspect", disc=str(self.disc), project=project, archive="DATA/CAR/ALGAR.CAR", asset="physics.val")
        before = self.backend(**base)["result"]["sections"][0]["fields"][0]["value"]
        self.assertEqual(before, "2800")
        saved = self.backend(**dict(base, action="save", edits=[dict(section="Chassis", key="mass", value="4200")]))
        self.assertTrue(saved["ok"], saved)
        after = self.backend(**base)["result"]["sections"][0]["fields"][0]["value"]
        self.assertEqual(after, "4200", "a saved project edit must not be hidden by the response cache")
        self.assertEqual(self.backend(**base), spawn_backend(base))
        self.assertTrue(self.backend(**dict(base, action="validate"))["ok"])
        state = self.client.call("state.get", dict(kind="project", project=project))
        self.assertEqual(state["state"], "saved")
        self.assertIn("package", state["allowed"])

    def test_caches_report_hits(self):
        request = self.req("inspect", archive="DATA/TRACKS/ANOKA.TRK", asset="ground.tex")
        self.backend(**request)
        first = self.client.call("service.status")["caches"]["responses"]["hits"]
        self.backend(**request)
        self.assertEqual(self.client.call("service.status")["caches"]["responses"]["hits"], first + 1)


class IndexTest(ServiceCase):
    def build(self, **kw):
        job = self.client.call("index.build", dict(disc=str(self.disc), jobs=1, **kw))
        done = wait_job(self.client, job["job_id"])
        self.assertEqual(done["state"], "succeeded", done)
        return done["result"]["summary"]

    def test_index_census_parity_search_and_relations(self):
        self.build()
        census = asset_registry.census(self.disc)
        mine = self.client.call("index.census", dict(disc=str(self.disc)))
        strip = lambda rs: [{k: v for k, v in r.items() if k != "warnings"} for r in rs]  # noqa: E731
        self.assertEqual(strip(mine["assets"]), strip(census["assets"]))
        self.assertEqual(mine["files"], census["files"])
        self.assertEqual(mine["totals"], census["totals"])
        sky = self.client.call("index.search", dict(disc=str(self.disc), role="sky"))
        self.assertEqual([a["assetId"] for a in sky["assets"]], ["DATA/TRACKS/ANOKA.TRK#sky01A.tex"])
        dup = self.client.call("index.search", dict(disc=str(self.disc), duplicate_of="DATA/TRACKS/ANOKA.TRK#ground.tex"))
        self.assertEqual([a["assetId"] for a in dup["assets"]], ["DATA/TRACKS/ANOKA.TRK#ground2.tex"])
        fts = self.client.call("index.search", dict(disc=str(self.disc), text="mass=2800"))
        self.assertEqual([a["assetId"] for a in fts["assets"]], ["DATA/CAR/ALGAR.CAR#physics.val"])
        record = self.client.call("index.get", dict(disc=str(self.disc), assetId="data/tracks/anoka.trk#SKY01A.TEX"))
        self.assertEqual(record["width"], 16)
        self.assertIn("variantOf", record["relations"])
        self.assertEqual(record["memberSha256"], next(a for a in census["assets"] if a["assetId"].endswith("#sky01A.tex"))["memberSha256"])
        valt = self.client.call("index.get", dict(disc=str(self.disc), assetId="DATA/CAR/ALGAR.CAR#physics.val"))
        self.assertEqual(valt["fields"][0], dict(section="Chassis", key="mass", value="2800"))

    def test_thumbnails_and_tags(self):
        self.build()
        aid = "DATA/TRACKS/ANOKA.TRK#sky01A.tex"
        first = self.client.call("index.thumbnail", dict(disc=str(self.disc), assetId=aid, size=64))
        self.assertFalse(first["cached"])
        self.assertTrue(Path(first["path"]).is_file())
        again = self.client.call("index.thumbnail", dict(disc=str(self.disc), assetId=aid, size=64))
        self.assertTrue(again["cached"])
        self.assertEqual(again["sha256"], first["sha256"])
        self.client.call("index.tag", dict(disc=str(self.disc), assetId=aid, add=["Favourite-Sky"]))
        tagged = self.client.call("index.search", dict(disc=str(self.disc), tag="favourite-sky"))
        self.assertEqual([a["assetId"] for a in tagged["assets"]], [aid])
        self.assertEqual(self.client.call("index.search", dict(disc=str(self.disc), text="favourite"))["total"], 1)

    def test_incremental_and_full_rebuild(self):
        self.build(full=True)
        summary = self.build()
        self.assertEqual(summary["changed"], 0)
        self.assertEqual(summary["skippedUnchangedStat"], summary["files"])
        archive = self.disc / "DATA/RES/CT.RES"
        os.utime(archive, ns=(time.time_ns(), time.time_ns()))
        # same content hashes: the duplicate submit returns the finished job instead of re-indexing
        last = self.client.call("job.list", dict(kind="index_build", limit=1))["jobs"][0]["job_id"]
        again = self.client.call("index.build", dict(disc=str(self.disc), jobs=1))
        self.assertEqual((again["job_id"], again["duplicate"]), (last, True))
        tar.pack(self.disc / "DATA/CAR/PALO.CAR", [("body.tex", "TEX", tar.make_tex(42, 32))])
        summary = self.build()
        self.assertEqual((summary["changed"], summary["skippedUnchangedHash"]), (1, 1))
        tar.pack(archive, [("790.tex", "TEX", tar.make_tex(40)), ("new.tex", "TEX", tar.make_tex(41))])
        summary = self.build()
        self.assertEqual(summary["changed"], 1)
        self.assertEqual(self.client.call("index.search", dict(disc=str(self.disc), query="CT.RES#new"))["total"], 1)
        full = self.build(full=True)
        self.assertEqual(full["changed"], full["files"])
        status = self.client.call("index.status", dict(disc=str(self.disc)))
        self.assertTrue(status["indexed"])
        self.assertEqual(status["schemaVersion"], kdb.SCHEMA_VERSION)

    def test_search_before_index_is_not_configured(self):
        other = make_disc(self.tmp / "other")
        with self.assertRaises(KitError) as ctx:
            self.client.call("index.search", dict(disc=str(other)))
        self.assertEqual(ctx.exception.code, "NOT_CONFIGURED")


class LifecycleTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="kitlife-")).resolve()
        self.ws = self.tmp / "ws"
        self.addCleanup(lambda: shutil.rmtree(self.tmp, ignore_errors=True))
        self.addCleanup(lambda: stop_service(self.ws))

    def test_concurrent_autostart_starts_one_server_and_recovers_stale_socket(self):
        C.service_dir(self.ws).mkdir(parents=True)
        stale = C.socket_path(self.ws)
        stale.parent.mkdir(parents=True, exist_ok=True)
        stale.write_text("stale")  # not a socket: a crashed server's leftover
        (C.service_dir(self.ws) / "service.json").write_text(json.dumps(dict(pid=999999, socket=str(stale))))
        pids, errors = [], []

        def ping():
            try:
                pids.append(Client(self.ws).call("service.ping")["pid"])
            except Exception as e:  # noqa: BLE001
                errors.append(e)
        threads = [threading.Thread(target=ping) for _ in range(6)]
        [t.start() for t in threads]
        [t.join(60) for t in threads]
        self.assertEqual(errors, [])
        self.assertEqual(len(set(pids)), 1, pids)
        servers = subprocess.run(["pgrep", "-f", f"server.py --workspace {self.ws}"], capture_output=True, text=True).stdout.split()
        self.assertEqual(len(servers), 1, servers)

    def test_kill_9_then_next_client_restarts(self):
        first = Client(self.ws).call("service.ping")["pid"]
        os.kill(first, signal.SIGKILL)
        time.sleep(0.2)
        second = Client(self.ws).call("service.ping")["pid"]
        self.assertNotEqual(first, second)

    def test_idle_exit(self):
        env = dict(os.environ, EOD_KIT_SERVICE_IDLE="1")
        subprocess.run([sys.executable, "-I", str(SERVER), "--workspace", str(self.ws), "--detach"], env=env, check=True)
        pid = service_pid(self.ws)
        for _ in range(100):
            if not _alive(pid):
                break
            time.sleep(0.1)
        self.assertFalse(_alive(pid), "service did not exit when idle")

    def test_stdio_mode(self):
        disc = make_disc(self.tmp)
        p = subprocess.Popen([sys.executable, "-I", str(SERVER), "--stdio", "--workspace", str(self.ws)],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        lines = [dict(jsonrpc="2.0", id=1, method="service.ping"),
                 dict(jsonrpc="2.0", id=2, method="backend", params=dict(action="catalog", disc=str(disc))),
                 dict(jsonrpc="2.0", id=3, method="nope")]
        out, _ = p.communicate(("\n".join(json.dumps(x) for x in lines) + "\nnot json\n").encode(), timeout=60)
        replies = [json.loads(x) for x in out.decode().splitlines()]
        self.assertEqual(replies[0]["result"]["mode"], "stdio")
        self.assertTrue(replies[1]["result"]["ok"])
        self.assertEqual(replies[1]["result"], spawn_backend(dict(action="catalog", disc=str(disc))))
        self.assertEqual(replies[2]["error"]["code"], C.METHOD_NOT_FOUND)
        self.assertEqual(replies[3]["error"]["code"], C.PARSE_ERROR)


class MigrationTest(unittest.TestCase):
    def test_upgrade_from_schema_1_and_refuse_newer(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "kit.db"
            raw = sqlite3.connect(str(path), isolation_level=None)
            for statement in kdb._statements(kdb.MIGRATIONS[0]):
                raw.execute(statement)
            raw.execute("PRAGMA user_version=1")
            raw.close()
            conn = kdb.connect(path)
            self.assertEqual(kdb.version(conn), kdb.SCHEMA_VERSION)
            conn.execute("SELECT * FROM jobs").fetchall()
            conn.execute(f"PRAGMA user_version={kdb.SCHEMA_VERSION + 1}")
            conn.close()
            with self.assertRaises(RuntimeError):
                kdb.connect(path)


class StateMachineTest(unittest.TestCase):
    def test_package_transitions(self):
        P = states.PACKAGE
        self.assertEqual(P.check("built", "install", "hd", dict(enable=True)), "enabled")
        self.assertEqual(P.check("built", "install", "hd", dict(enable=False)), "installed")
        self.assertEqual(P.check("enabled", "disable", "hd"), "disabled")
        self.assertEqual(P.check("disabled", "enable", "hd"), "enabled")
        self.assertEqual(P.check("enabled", "uninstall", "hd"), "uninstalled")
        with self.assertRaises(KitError) as ctx:
            P.check("uninstalled", "enable", "hd", extra=dict(profile="dev"))
        self.assertEqual(ctx.exception.code, "INVALID_TRANSITION")
        self.assertIn("not installed in profile dev", ctx.exception.message)
        self.assertTrue(ctx.exception.hint)
        with self.assertRaises(KitError):
            P.check("draft", "install", "hd")

    def test_project_and_profile_rules(self):
        with self.assertRaises(KitError) as ctx:
            states.PROJECT.check("new", "package", "p")
        self.assertIn("nothing to package", ctx.exception.message)
        self.assertEqual(states.PROJECT.check("packaged", "save", "p"), "saved")
        with self.assertRaises(KitError) as ctx:
            states.PROFILE.check("in-use", "enable", "dev")
        self.assertIn("running game test", ctx.exception.message)
        with self.assertRaises(KitError) as ctx:
            states.PROFILE.check("blocked", "launch", "dev", extra=dict(blocked="a#b"))
        self.assertIn("a#b", ctx.exception.message)

    def test_error_envelope_matches_helper_main(self):
        """backend.error_envelope must stay in step with modding_backend.main()."""
        from kit_service import backend
        import modding_backend as mb
        for error in (ValueError("plain"), mb.PlainError("sentence", "cause") if hasattr(mb, "PlainError") else ValueError("x"),
                      OSError("disk"), RuntimeError("boom")):
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf), unittest.mock.patch.object(mb, "dispatch", side_effect=error), \
                    unittest.mock.patch.object(sys, "stdin", io.StringIO("{}")):
                mb.main()
            self.assertEqual(backend.error_envelope(error), json.loads(buf.getvalue()))


import unittest.mock  # noqa: E402

if __name__ == "__main__":
    unittest.main(verbosity=2)
