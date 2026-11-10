#!/usr/bin/env python3
"""eod-creation-kit MCP server tests: spawns the server and speaks JSON-RPC over stdio.

Run from the build directory:  EOD_HEADLESS=1 EOD_MUTE=1 python3 ../tools/tests/test_eod_mcp.py
Retail-gated (EOD_TEST_RETAIL_TEXTURES=1): census + ANOKA sky export against the real disc, read-only.
"""
import base64
import hashlib
import json
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_asset_registry as tar  # noqa: E402
from test_mod_package import ANOKA, make_disc as mp_disc  # noqa: E402
from texture_preview import decode_preview  # noqa: E402
from tex import write_png  # noqa: E402

SERVER = TOOLS / "eod_mcp" / "server.py"
RETAIL = Path(os.environ.get("EOD_TEST_DISC", str(Path.home() / "Library/Application Support/TDEODRecomp/disc")))
RETAIL_ON = os.environ.get("EOD_TEST_RETAIL_TEXTURES") == "1"


class Client:
    def __init__(self, env):
        full = {k: v for k, v in os.environ.items() if k in ("PATH", "HOME", "TMPDIR")}
        full.update(env)
        self.p = subprocess.Popen([sys.executable, "-I", str(SERVER)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, env=full, text=True, bufsize=1)
        self.q, self.notes, self.stray, self.n = queue.Queue(), [], [], 0
        self.err = []
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=lambda: [self.err.append(l) for l in self.p.stderr], daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            try:
                m = json.loads(line)
                assert m.get("jsonrpc") == "2.0"
            except (ValueError, AssertionError):
                self.stray.append(line)
                continue
            (self.q if "id" in m else self.notes).put(m) if "id" in m else self.notes.append(m)

    def send(self, method, params=None, notify=False):
        msg = dict(jsonrpc="2.0", method=method)
        if params is not None:
            msg["params"] = params
        if not notify:
            self.n += 1
            msg["id"] = self.n
        self.p.stdin.write(json.dumps(msg) + "\n")
        self.p.stdin.flush()
        return msg.get("id")

    def rpc(self, method, params=None, timeout=60):
        rid = self.send(method, params)
        while True:
            m = self.q.get(timeout=timeout)
            if m["id"] == rid:
                return m

    def call(self, name, args=None, timeout=120, meta=None):
        params = dict(name=name, arguments=args or {})
        if meta:
            params["_meta"] = meta
        m = self.rpc("tools/call", params, timeout)
        return m

    def tool(self, name, args=None, ok=True, **kw):
        m = self.call(name, args, **kw)
        assert "result" in m, m
        r = m["result"]
        if ok:
            assert not r["isError"], r
        return r

    def handshake(self):
        r = self.rpc("initialize", dict(protocolVersion="2025-06-18", capabilities={}, clientInfo=dict(name="test", version="0")))
        self.send("notifications/initialized", notify=True)
        return r["result"]

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(10)
        except Exception:  # noqa: BLE001
            self.p.kill()


def tree_hash(root):
    h = hashlib.sha256()
    for p in sorted(Path(root).rglob("*")):
        if p.is_file():
            h.update(str(p.relative_to(root)).encode() + hashlib.sha256(p.read_bytes()).digest())
    return h.hexdigest()


def sc(result):
    return result["structuredContent"]


class Base(unittest.TestCase):
    ALLOW = {}

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="eodmcp-")).resolve()
        self.disc = self.make_disc(self.tmp)
        self.ws = self.tmp / "ws"
        self.ws.mkdir()
        self.client = Client(dict(EOD_DISC=str(self.disc), EOD_KIT_WORKSPACE=str(self.ws), EOD_HEADLESS="1", EOD_MUTE="1", **self.ALLOW))
        self.addCleanup(lambda: shutil.rmtree(self.tmp, ignore_errors=True))
        self.addCleanup(self.client.close)
        self.client.handshake()

    make_disc = staticmethod(lambda root: tar.make_disc(root))

    def wait_job(self, job_id, timeout=120):
        end = time.time() + timeout
        while time.time() < end:
            s = sc(self.client.tool("eod_job_status", dict(job_id=job_id, wait_seconds=5)))
            if s["state"] in ("succeeded", "failed", "cancelled"):
                return s
        self.fail("job timed out")

    def plan_apply(self, name, args):
        d = sc(self.client.tool(name, dict(args, dry_run=True)))
        self.assertTrue(d["dry_run"])
        return self.client.tool(name, dict(args, dry_run=False, plan_id=d["plan_id"]))


class ProtocolTest(Base):
    def test_handshake_tools_and_hygiene(self):
        info = self.client.rpc("initialize", dict(protocolVersion="2025-06-18", capabilities={}))["result"]
        self.assertEqual(info["protocolVersion"], "2025-06-18")
        for cap in ("tools", "resources", "prompts"):
            self.assertIn(cap, info["capabilities"])
        tools = self.client.rpc("tools/list")["result"]["tools"]
        names = {t["name"] for t in tools}
        self.assertIn("eod_kit_status", names)
        self.assertNotIn("eod_test_run_start", names)  # run disabled
        for t in tools:
            self.assertIs(t["inputSchema"]["additionalProperties"], False, t["name"])
            self.assertRegex(t["description"], r"^\[(R|W:project|W:profile|RUN)\] ", t["name"])
            self.assertIn("readOnlyHint", t["annotations"])
        self.assertEqual(self.client.rpc("ping")["result"], {})
        self.assertEqual(self.client.stray, [])

    def test_errors(self):
        self.assertEqual(self.client.call("eod_nope")["error"]["code"], -32602)
        bad = self.client.call("eod_entries", {})
        self.assertEqual(bad["error"]["code"], -32602)
        self.assertIn("archive", bad["error"]["message"])
        self.assertEqual(self.client.call("eod_entries", dict(archive="x", extra=1))["error"]["code"], -32602)
        r = self.client.tool("eod_entries", dict(archive="DATA/NOPE.TRK"), ok=False)
        self.assertTrue(r["isError"])
        self.assertEqual(sc(r)["error"]["code"], "BACKEND_ERROR")
        self.assertEqual(self.client.rpc("nope/method")["error"]["code"], -32601)
        self.assertIn("result", self.client.rpc("ping"))  # still alive

    def test_run_refused_without_allow_run(self):
        r = self.client.tool("eod_test_run_start", dict(profile="dev-x"), ok=False)
        self.assertEqual(sc(r)["error"]["code"], "CAPABILITY_UNAVAILABLE")
        st = sc(self.client.tool("eod_kit_status"))
        self.assertFalse(st["runEnabled"])
        self.assertTrue(any(m["tool"] == "eod_test_run_start" for m in st["missing"]))

    def test_prompts_and_docs(self):
        names = [p["name"] for p in self.client.rpc("prompts/list")["result"]["prompts"]]
        self.assertIn("texture_pack_workflow", names)
        got = self.client.rpc("prompts/get", dict(name="texture_pack_workflow", arguments=dict(scope="sky", scale="4", pack_id="hd"))) ["result"]
        self.assertIn("4x", got["messages"][0]["content"]["text"])
        doc = self.client.rpc("resources/read", dict(uri="eod://docs/CREATION_KIT_TEXTURE_CONTRACTS"))["result"]["contents"][0]
        self.assertIn("Asset identity", doc["text"])
        self.assertIn("error", self.client.rpc("resources/read", dict(uri="eod://docs/../../AGENTS")))
        self.assertTrue(self.client.rpc("resources/templates/list")["result"]["resourceTemplates"])


class GatingTest(unittest.TestCase):
    def test_hidden_modules(self):
        tmp = Path(tempfile.mkdtemp()).resolve()
        self.addCleanup(lambda: shutil.rmtree(tmp, ignore_errors=True))
        disc = tar.make_disc(tmp)
        (tmp / "ws").mkdir()
        c = Client(dict(EOD_DISC=str(disc), EOD_KIT_WORKSPACE=str(tmp / "ws"), EOD_MCP_HIDE_MODULES="mod_install,sky_export"))
        self.addCleanup(c.close)
        c.handshake()
        names = {t["name"] for t in c.rpc("tools/list")["result"]["tools"]}
        self.assertNotIn("eod_mod_install", names)
        self.assertNotIn("eod_sky_export_start", names)
        self.assertIn("eod_texture_export_start", names)
        st = sc(c.tool("eod_kit_status"))
        missing = {m["tool"]: m["reason"] for m in st["missing"]}
        self.assertIn("mod_install", missing["eod_mod_install"])
        self.assertIn("sky_export", missing["eod_sky_export_start"])
        r = c.tool("eod_sky_export_start", dict(archive=ANOKA), ok=False)
        self.assertEqual(sc(r)["error"]["code"], "CAPABILITY_UNAVAILABLE")
        self.assertTrue(c.rpc("prompts/list")["result"]["prompts"])


class ReadOnlyTest(Base):
    def test_catalog_entries_inspect_match_backend(self):
        import modding_backend as mb
        cat = sc(self.client.tool("eod_catalog"))
        ref = mb.dispatch(dict(action="catalog", disc=str(self.disc)))
        self.assertEqual(cat["archives"], ref["archives"])
        self.assertEqual([a["path"] for a in sc(self.client.tool("eod_catalog", dict(kind="trk")))["archives"]], ["DATA/TRACKS/ANOKA.TRK"])
        ent = sc(self.client.tool("eod_entries", dict(archive="DATA/TRACKS/ANOKA.TRK")))["entries"]
        ref = mb.dispatch(dict(action="entries", disc=str(self.disc), archive="DATA/TRACKS/ANOKA.TRK"))["entries"]
        self.assertEqual([(e["name"], e["type"], e["size"], e["crc"]) for e in ent], [(e["name"], e["type"], e["size"], e["crc"]) for e in ref])
        self.assertTrue(next(e for e in ent if e["name"] == "sky01A.tex")["crcOk"])
        r = self.client.tool("eod_inspect", dict(archive="DATA/TRACKS/ANOKA.TRK", member="sky01A.tex"))
        self.assertEqual(sc(r)["type"], "TEX")
        self.assertTrue(any(b["type"] == "image" for b in r["content"]))
        env = sc(self.client.tool("eod_track_environment", dict(archive="DATA/TRACKS/ANOKA.TRK")))
        self.assertEqual(env["skyTextures"], ["sky01A.tex"])

    def test_census_search_preview_resource(self):
        self.assertEqual(sc(self.client.tool("eod_asset_search", ok=False))["error"]["code"], "NOT_CONFIGURED")
        job = sc(self.client.tool("eod_census_start"))
        self.assertEqual(self.wait_job(job["job_id"])["state"], "succeeded")
        self.assertEqual(sc(self.client.tool("eod_census_start"))["state"], "cached")
        sky = sc(self.client.tool("eod_asset_search", dict(role="sky")))
        self.assertEqual([a["assetId"] for a in sky["assets"]], ["DATA/TRACKS/ANOKA.TRK#sky01A.tex"])
        rec = sc(self.client.tool("eod_asset_get", dict(asset_id="DATA/TRACKS/ANOKA.TRK#sky01A.tex")))
        self.assertEqual(rec["width"], 16)
        dup = sc(self.client.tool("eod_asset_search", dict(duplicate_of="DATA/TRACKS/ANOKA.TRK#ground.tex")))
        self.assertEqual([a["assetId"] for a in dup["assets"]], ["DATA/TRACKS/ANOKA.TRK#ground2.tex"])
        pv = self.client.tool("eod_asset_preview", dict(asset_ids=["DATA/TRACKS/ANOKA.TRK#sky01A.tex"]))
        blocks = [b for b in pv["content"] if b["type"] == "image"]
        png = base64.b64decode(blocks[0]["data"])
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        self.assertEqual(int.from_bytes(png[16:20], "big"), 16)
        self.assertLess(len(png), 1_500_000)
        uri = "eod://asset/DATA/TRACKS/ANOKA.TRK/sky01A.tex/preview.png"
        res = self.client.rpc("resources/read", dict(uri=uri))["result"]["contents"][0]
        self.assertEqual(res["mimeType"], "image/png")
        self.assertEqual(base64.b64decode(res["blob"]), png)
        sheet = self.client.tool("eod_asset_preview", dict(asset_ids=["DATA/TRACKS/ANOKA.TRK#sky01A.tex", "DATA/TRACKS/ANOKA.TRK#ground.tex"], sheet=True))
        self.assertEqual(len([b for b in sheet["content"] if b["type"] == "image"]), 1)
        summary = self.client.rpc("resources/read", dict(uri="eod://census"))["result"]["contents"][0]
        self.assertIn("totals", json.loads(summary["text"]))


class ExportTest(Base):
    def test_dry_run_apply_progress_and_resume(self):
        args = dict(archive="DATA/TRACKS/ANOKA.TRK", role="sky")
        dry = sc(self.client.tool("eod_texture_export_start", args))
        self.assertTrue(dry["dry_run"])
        self.assertEqual(dry["plan"]["toExport"], 1)
        self.assertFalse((self.ws / dry["plan"]["out"]).exists())
        need = self.client.tool("eod_texture_export_start", dict(args, dry_run=False), ok=False)
        self.assertEqual(sc(need)["error"]["code"], "NEEDS_CONFIRM")
        stale = self.client.tool("eod_texture_export_start", dict(args, dry_run=False, plan_id="0" * 16), ok=False)
        self.assertEqual(sc(stale)["error"]["code"], "STALE_PLAN")
        applied = sc(self.client.tool("eod_texture_export_start", dict(args, dry_run=False, plan_id=dry["plan_id"])))
        final = self.wait_job(applied["job_id"])
        self.assertEqual(final["state"], "succeeded", final)
        self.assertEqual(final["result"]["summary"]["exported"], 1)
        out = self.ws / applied["out"]
        self.assertTrue((out / "export-manifest.json").is_file())
        self.assertTrue(list(out.rglob("sky01A.tex.png")))
        audit = [json.loads(l) for l in (self.ws / "audit.jsonl").read_text().splitlines()]
        self.assertTrue(any(e["tool"] == "eod_texture_export_start" and e["planId"] == dry["plan_id"] for e in audit))
        self.assertTrue(any(e["tool"] == "job:texture_export" and e["ok"] for e in audit))
        # the same plan is stale once the inputs changed (an export manifest now exists)
        again = self.client.tool("eod_texture_export_start", dict(args, dry_run=False, plan_id=dry["plan_id"]), ok=False)
        self.assertEqual(sc(again)["error"]["code"], "STALE_PLAN")

    def test_progress_notifications_monotonic(self):
        dry = sc(self.client.tool("eod_texture_export_start", dict(archive="DATA/TRACKS/ANOKA.TRK")))
        job = sc(self.client.tool("eod_texture_export_start", dict(archive="DATA/TRACKS/ANOKA.TRK", dry_run=False, plan_id=dry["plan_id"])))
        self.client.notes.clear()
        end = time.time() + 60
        while time.time() < end:
            s = sc(self.client.tool("eod_job_status", dict(job_id=job["job_id"], wait_seconds=2), meta=dict(progressToken="tok1")))
            if s["state"] != "running":
                break
        prog = [n["params"]["progress"] for n in self.client.notes if n["method"] == "notifications/progress"]
        self.assertEqual(prog, sorted(prog))
        self.assertEqual(len(prog), len(set(prog)))


class BigDiscCase(Base):
    make_disc = staticmethod(lambda root: BigDiscCase.build(root))

    @staticmethod
    def build(root):
        disc = Path(root) / "disc"
        (disc / "DATA").mkdir(parents=True)
        (disc / "SLUS_209.10").write_bytes(b"synthetic executable")
        tar.pack(disc / "DATA/TRACKS/ANOKA.TRK", [(f"t{i:03d}.tex", "TEX", tar.make_tex(i)) for i in range(40)])
        return disc


class CancelTest(BigDiscCase):
    ALLOW = dict(EOD_MCP_JOB_DELAY="0.15")

    def test_cancel_then_resume(self):
        args = dict(archive="DATA/TRACKS/ANOKA.TRK")
        res = self.plan_apply("eod_texture_export_start", args)
        job = sc(res)
        for _ in range(100):
            s = sc(self.client.tool("eod_job_status", dict(job_id=job["job_id"], wait_seconds=2)))
            if s["done"]:
                break
        self.assertEqual(sc(self.client.tool("eod_job_cancel", dict(job_id=job["job_id"])))["state"], "cancelling")
        final = self.wait_job(job["job_id"])
        self.assertEqual(final["state"], "cancelled")
        manifest = json.loads((self.ws / job["out"] / "export-manifest.json").read_text())
        self.assertEqual(manifest["run"]["status"], "cancelled")
        self.assertLess(manifest["run"]["done"], 40)
        self.assertEqual(sc(self.client.tool("eod_job_list", dict(state="cancelled")))["jobs"][0]["job_id"], job["job_id"])
        run_id = Path(job["out"]).name
        r = self.plan_apply("eod_texture_export_start", dict(resume=run_id))
        done = self.wait_job(sc(r)["job_id"])
        self.assertEqual(done["state"], "succeeded")
        self.assertEqual(json.loads((self.ws / job["out"] / "export-manifest.json").read_text())["summary"]["exported"], 40)


class SafetyTest(Base):
    def test_disc_never_written(self):
        before = tree_hash(self.disc)
        for out in (str(self.disc / "DATA" / "x"), str(self.disc), "../disc/DATA/x", "/Applications/Foo.app/x", "~/x"):
            r = self.client.tool("eod_texture_export_start", dict(archive="DATA/TRACKS/ANOKA.TRK", out=out, dry_run=False, plan_id="x"), ok=False)
            self.assertEqual(sc(r)["error"]["code"], "UNSAFE_PATH", out)
        (self.ws / "exports").mkdir()
        os.symlink(self.disc / "DATA", self.ws / "exports" / "evil")
        r = self.client.tool("eod_texture_export_start", dict(archive="DATA/TRACKS/ANOKA.TRK", out="exports/evil/x"), ok=False)
        self.assertEqual(sc(r)["error"]["code"], "UNSAFE_PATH")
        self.assertEqual(tree_hash(self.disc), before)

    def test_profiles_dev_only_with_marker(self):
        r = self.client.tool("eod_mod_enable", dict(profile="default", id="x"), ok=False)
        self.assertEqual(sc(r)["error"]["code"], "UNSAFE_PATH")
        (self.ws / "profiles" / "dev-nomark").mkdir(parents=True)
        r = self.client.tool("eod_mod_enable", dict(profile="dev-nomark", id="x"), ok=False)
        self.assertEqual(sc(r)["error"]["code"], "UNSAFE_PATH")
        self.assertIn("marker", sc(r)["error"]["message"])
        self.assertEqual(self.client.call("eod_profile_create", dict(name="release"))["error"]["code"], -32602)

    def test_import_path_must_be_in_workspace(self):
        outside = self.tmp / "evil.png"
        write_png(str(outside), 16, 16, bytes(16 * 16 * 4))
        r = self.client.tool("eod_texture_compare", dict(asset_id="DATA/TRACKS/ANOKA.TRK#sky01A.tex", png=str(outside)), ok=False)
        self.assertEqual(sc(r)["error"]["code"], "UNSAFE_PATH")


class PackageFlowTest(Base):
    make_disc = staticmethod(lambda root: mp_disc(root))

    def test_replace_package_install_and_destructive_confirm(self):
        asset = ANOKA + "#sky.tex"
        data = self.client.tool("eod_inspect", dict(archive=ANOKA, member="sky.tex"))
        self.assertEqual(sc(data)["type"], "TEX")
        import modding_backend as mb
        orig = mb.payload(mb.archive_path(self.disc, ANOKA), "sky.tex")[0]
        w, h, px, _ = decode_preview(orig)
        px = bytearray(px)
        for i in range(0, len(px), 4):
            px[i] = 255 - px[i]
        (self.ws / "imports").mkdir()
        write_png(str(self.ws / "imports/new.png"), w, h, bytes(px))
        write_png(str(self.ws / "imports/big.png"), w * 2, h * 2, bytes(w * 2 * h * 2 * 4))
        bad = self.client.tool("eod_texture_replace", dict(asset_id=asset, png="imports/big.png", mode="native"), ok=False)
        self.assertEqual(sc(bad)["error"]["code"], "UNSUPPORTED")
        self.assertEqual(sc(bad)["error"]["details"]["expected"], [w, h])
        cmp = self.client.tool("eod_texture_compare", dict(asset_id=asset, png="imports/new.png"))
        self.assertTrue(sc(cmp)["nativeEligible"])
        args = dict(asset_id=asset, png="imports/new.png", mode="native")
        dry = sc(self.client.tool("eod_texture_replace", args))
        self.assertFalse((self.ws / "projects/default/project.json").exists())
        self.assertFalse(dry["plan"]["verified"])
        self.client.tool("eod_texture_replace", dict(args, dry_run=False, plan_id=dry["plan_id"]))
        v = self.client.tool("eod_project_validate")
        self.assertTrue(sc(v)["ok"])
        pdry = sc(self.client.tool("eod_package_build", dict(id="mypack", version="1.0.0")))
        self.assertEqual(pdry["plan"]["manifest"]["native"], [ANOKA + "#sky.tex"])
        self.assertFalse((self.ws / "packages").exists())
        built = sc(self.client.tool("eod_package_build", dict(id="mypack", version="1.0.0", dry_run=False, plan_id=pdry["plan_id"])))
        self.assertTrue((self.ws / built["zip"]).is_file())
        val = sc(self.client.tool("eod_package_validate", dict(path=built["zip"])))
        self.assertTrue(val["ok"], val)
        self.assertEqual(sc(self.client.tool("eod_package_inspect", dict(path=built["out"])))["id"], "mypack")
        self.plan_apply("eod_profile_create", dict(name="dev-a"))
        self.assertTrue((self.ws / "profiles/dev-a/.eod-dev-profile").is_file())
        self.plan_apply("eod_mod_install", dict(profile="dev-a", package=built["zip"]))
        desc = sc(self.client.tool("eod_profile_describe", dict(name="dev-a")))
        self.assertEqual([p["id"] for p in desc["packages"]], ["mypack"])
        self.assertIn("mypack", desc["profileTxt"])
        active = self.ws / "profiles/dev-a/active" / ANOKA
        got = mb.payload(active, "sky.tex")[0]
        self.assertNotEqual(got, orig)
        self.assertEqual(mb.payload(active, "ground.tex")[0], mb.payload(mb.archive_path(self.disc, ANOKA), "ground.tex")[0])
        plan = sc(self.client.tool("eod_launch_plan", dict(profile="dev-a")))
        self.assertTrue(plan["environment"]["EOD_MOD_DIR"].endswith("dev-a/active"))
        self.assertEqual(plan["environment"]["EOD_MUTE"], "1")
        # destructive: needs plan and exact confirm
        d = sc(self.client.tool("eod_mod_uninstall", dict(profile="dev-a", id="mypack")))
        r = self.client.tool("eod_mod_uninstall", dict(profile="dev-a", id="mypack", dry_run=False, plan_id=d["plan_id"]), ok=False)
        self.assertEqual(sc(r)["error"]["code"], "NEEDS_CONFIRM")
        r = self.client.tool("eod_mod_uninstall", dict(profile="dev-a", id="mypack", dry_run=False, plan_id=d["plan_id"], confirm="mypack"))
        self.assertTrue(sc(r)["applied"])
        self.assertFalse(active.exists())
        audit = [json.loads(l) for l in (self.ws / "audit.jsonl").read_text().splitlines()]
        tools = [e["tool"] for e in audit]
        for t in ("eod_texture_replace", "eod_package_build", "eod_profile_create", "eod_mod_install", "eod_mod_uninstall"):
            self.assertIn(t, tools)
        inst = next(e for e in audit if e["tool"] == "eod_mod_install")
        self.assertNotEqual(inst["files"][0]["before"], inst["files"][0]["after"])
        # log redaction
        self.assertNotIn(str(Path.home()), json.dumps(self.client.tool("eod_job_list")))


@unittest.skipUnless(RETAIL_ON and RETAIL.is_dir(), "set EOD_TEST_RETAIL_TEXTURES=1 with a retail disc")
class RetailTest(unittest.TestCase):
    def test_census_and_anoka_sky_export_read_only(self):
        tmp = Path(tempfile.mkdtemp()).resolve()
        self.addCleanup(lambda: shutil.rmtree(tmp, ignore_errors=True))
        slus = hashlib.sha256((RETAIL / "SLUS_209.10").read_bytes()).hexdigest()
        c = Client(dict(EOD_DISC=str(RETAIL), EOD_KIT_WORKSPACE=str(tmp), EOD_HEADLESS="1", EOD_MUTE="1"))
        self.addCleanup(c.close)
        c.handshake()
        job = sc(c.tool("eod_census_start", dict(jobs=4)))
        end = time.time() + 1500
        while time.time() < end:
            s = sc(c.tool("eod_job_status", dict(job_id=job["job_id"], wait_seconds=20)))
            if s["state"] != "running":
                break
        self.assertEqual(s["state"], "succeeded", s)
        sky = sc(c.tool("eod_asset_search", dict(role="sky", archive="DATA/TRACKS/ANOKA.TRK", limit=200)))
        self.assertGreater(sky["total"], 0)
        res = TestCase_plan(c, dict(archive="DATA/TRACKS/ANOKA.TRK", role="sky"))
        job = sc(res)
        end = time.time() + 600
        while time.time() < end:
            s = sc(c.tool("eod_job_status", dict(job_id=job["job_id"], wait_seconds=20)))
            if s["state"] != "running":
                break
        self.assertEqual(s["state"], "succeeded", s)
        self.assertEqual(s["result"]["summary"]["exported"], sky["total"])
        self.assertEqual(hashlib.sha256((RETAIL / "SLUS_209.10").read_bytes()).hexdigest(), slus)
        self.assertTrue(list((tmp / job["out"]).rglob("sky*.tex.png")))


def TestCase_plan(c, args):
    d = sc(c.tool("eod_texture_export_start", args))
    return c.tool("eod_texture_export_start", dict(args, dry_run=False, plan_id=d["plan_id"]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
