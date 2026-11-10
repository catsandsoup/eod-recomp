"""Batch TEX export: synthetic discs always, retail subset when EOD_TEST_RETAIL_TEXTURES=1."""
import copy
import json
import os
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
import zlib
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import asset_registry
import texture_export
import tex
from test_asset_registry import RETAIL, RETAIL_ON, make_disc, make_tex, pack

TOOLS = Path(__file__).resolve().parents[1]


def run(disc, dest, **kwargs):
    return texture_export.export_batch(disc, dest, **kwargs)


def by_id(manifest):
    return {a["assetId"]: a for a in manifest["assets"]}


def output_hashes(dest):
    return {str(p.relative_to(dest)): texture_export._file_sha(p) for p in sorted(Path(dest).rglob("*.png"))}


class SyntheticExport(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.disc = make_disc(self.root)
        self.dest = self.root / "out"

    def tearDown(self):
        self.tmp.cleanup()

    def test_full_export_manifest(self):
        manifest = run(self.disc, self.dest)
        self.assertEqual(manifest["summary"], dict(total=8, exported=7, failed=1, unsupported=0))
        self.assertEqual(manifest["run"]["status"], "complete")
        assets = by_id(manifest)
        sky = assets["DATA/TRACKS/ANOKA.TRK#sky01A.tex"]
        self.assertEqual(sky["output"], "textures/DATA/TRACKS/ANOKA.TRK/sky01A.tex.png")
        self.assertEqual(texture_export._file_sha(self.dest / sky["output"]), sky["outputSha256"])
        self.assertEqual(sky["role"], "sky")
        self.assertEqual(sky["alpha"], "straight-0-255-from-palette")
        self.assertEqual(sky["reimport"], ["native-same-size", "host-replacement"])
        self.assertEqual(json.loads((self.dest / "export-manifest.json").read_text())["summary"], manifest["summary"])
        bad = assets["DATA/TRACKS/ANOKA.TRK#bad.tex"]
        self.assertEqual(bad["status"], "failed")
        self.assertTrue(bad["error"])
        self.assertFalse((self.dest / "textures/DATA/TRACKS/ANOKA.TRK/bad.tex.png").exists())

    def test_duplicate_payloads_stay_separate_assets(self):
        assets = by_id(run(self.disc, self.dest))
        one, two = assets["DATA/TRACKS/ANOKA.TRK#ground.tex"], assets["DATA/TRACKS/ANOKA.TRK#ground2.tex"]
        self.assertNotEqual(one["output"], two["output"])
        self.assertEqual(one["outputSha256"], two["outputSha256"])
        self.assertEqual(one["duplicateOf"], [two["assetId"]])
        self.assertEqual(one["runtimeKeyShared"], [two["assetId"]])

    def test_crc_mismatch_still_exports_with_warning(self):
        import test_asset_registry as t
        t.corrupt_member(self.disc / "DATA/RES/CT.RES", "790.tex", make_tex(4))
        record = by_id(run(self.disc, self.dest))["DATA/RES/CT.RES#790.tex"]
        self.assertFalse(record["crcOk"])
        self.assertIn("crc-mismatch", record["warnings"])
        self.assertEqual(record["status"], "exported")

    def test_filters(self):
        manifest = run(self.disc, self.dest, select=["data/tracks/anoka.trk#sky*"], role="sky")
        self.assertEqual(sorted(by_id(manifest)), ["DATA/TRACKS/ANOKA.TRK#sky01A.tex"])
        manifest = run(self.disc, self.root / "o2", archives=["TRACKS/anoka.trk", "DATA/CAR/PALO.CAR"], role="vehicle")
        self.assertEqual(sorted(by_id(manifest)), ["DATA/CAR/PALO.CAR#body.tex"])
        with self.assertRaises(texture_export.ExportError):
            run(self.disc, self.root / "o3", archives=["DATA/NOPE.CAR"])

    def test_dry_run_writes_nothing(self):
        plan = run(self.disc, self.dest, dry_run=True)
        self.assertEqual((plan["total"], plan["toExport"]), (8, 8))
        self.assertFalse(self.dest.exists())

    def test_destination_must_be_outside_disc(self):
        with self.assertRaises(texture_export.ExportError):
            run(self.disc, self.disc / "DATA" / "export")
        link = self.root / "link"
        link.symlink_to(self.disc / "DATA", target_is_directory=True)
        with self.assertRaises(texture_export.ExportError):
            run(self.disc, link / "sub")
        self.assertEqual(sorted(p.name for p in (self.disc / "DATA").iterdir()), ["BROKEN.CAR", "CAR", "RES", "TRACKS", "VIDEO"])

    def test_traversal_and_symlink_components_refused(self):
        for member in ("../x", "..", "a\x07b", "a\\b"):
            with self.assertRaises(ValueError):
                texture_export.output_relative("DATA/CAR/X.CAR", member)
        self.dest.mkdir()
        for relative in ("textures/../../escape.png", "/abs.png", "textures//x.png"):
            with self.assertRaises(ValueError):
                texture_export.safe_target(self.dest.resolve(), relative)
        outside = self.root / "outside"
        outside.mkdir()
        (self.dest / "textures").symlink_to(outside, target_is_directory=True)
        manifest = run(self.disc, self.dest)
        self.assertEqual(manifest["summary"]["exported"], 0)
        self.assertEqual(list(outside.iterdir()), [])

    def test_case_folded_collisions_refuse_all_colliders(self):
        real = asset_registry.inventory

        def with_clash(disc):
            files, members = real(disc)
            clone = copy.deepcopy(next(m for m in members if m["assetId"] == "DATA/CAR/PALO.CAR#body.tex"))
            clone["archive"] = "DATA/car/palo.car"
            clone["assetId"] = "DATA/car/palo.car#body.tex"
            return files, members + [clone]

        with mock.patch.object(texture_export, "inventory", with_clash):
            assets = by_id(run(self.disc, self.dest, archives=["DATA/CAR/PALO.CAR"]))
        self.assertEqual(assets["DATA/CAR/PALO.CAR#body.tex"]["status"], "exported")  # clone is not selected
        with mock.patch.object(texture_export, "inventory", with_clash):
            assets = by_id(run(self.disc, self.root / "clash", select=["DATA/*/pal*.car#body.tex"]))
        self.assertEqual({a["status"] for a in assets.values()}, {"failed"})
        self.assertIn("collides", assets["DATA/car/palo.car#body.tex"]["error"])
        self.assertFalse(list((self.root / "clash").rglob("*.png")))

    def test_existing_different_file_is_never_overwritten(self):
        target = self.dest / "textures/DATA/CAR/PALO.CAR/body.tex.png"
        target.parent.mkdir(parents=True)
        target.write_bytes(b"precious")
        assets = by_id(run(self.disc, self.dest))
        self.assertEqual(assets["DATA/CAR/PALO.CAR#body.tex"]["status"], "failed")
        self.assertEqual(target.read_bytes(), b"precious")
        # --resume without a manifest record for this asset must not overwrite either
        manifest_path = self.dest / "export-manifest.json"
        manifest_path.unlink()
        assets = by_id(run(self.disc, self.dest, resume=True))
        self.assertEqual(assets["DATA/CAR/PALO.CAR#body.tex"]["status"], "failed")
        self.assertEqual(target.read_bytes(), b"precious")

    def test_existing_manifest_requires_resume(self):
        run(self.disc, self.dest)
        with self.assertRaises(texture_export.ExportError):
            run(self.disc, self.dest)

    def test_noop_reexport_has_identical_hashes(self):
        first = run(self.disc, self.dest)
        before = output_hashes(self.dest)
        second = run(self.disc, self.dest, resume=True)
        self.assertEqual(second["run"]["resumed"], 7)
        self.assertEqual(output_hashes(self.dest), before)
        fresh = run(self.disc, self.root / "fresh")
        self.assertEqual(output_hashes(self.root / "fresh"), before)
        self.assertEqual({k: v["outputSha256"] for k, v in by_id(fresh).items() if "outputSha256" in v},
                         {k: v["outputSha256"] for k, v in by_id(first).items() if "outputSha256" in v})

    def test_resume_after_interruption(self):
        cancel, seen = threading.Event(), []

        def progress(event):
            if event["event"] == "progress":
                seen.append(event["assetId"])
                if len(seen) == 3:
                    cancel.set()

        partial = run(self.disc, self.dest, progress=progress, cancel=cancel)
        self.assertEqual(partial["run"]["status"], "cancelled")
        self.assertEqual(partial["summary"]["total"], 3)
        self.assertEqual(json.loads((self.dest / "export-manifest.json").read_text())["run"]["status"], "cancelled")
        done = run(self.disc, self.dest, resume=True)
        self.assertEqual(done["run"]["status"], "complete")
        self.assertEqual(done["run"]["resumed"], sum(1 for a in partial["assets"] if a["status"] == "exported"))
        self.assertEqual(len(done["assets"]), len({a["assetId"] for a in done["assets"]}))
        reference = run(self.disc, self.root / "ref")
        self.assertEqual(output_hashes(self.dest), output_hashes(self.root / "ref"))
        self.assertEqual(done["summary"], reference["summary"])

    def test_resume_from_stale_checkpoint_after_kill(self):
        full = run(self.disc, self.dest)
        before = output_hashes(self.dest)
        stale = dict(full, assets=full["assets"][:2], run=dict(full["run"], status="running"))
        (self.dest / "export-manifest.json").write_text(json.dumps(stale))
        (self.dest / "textures/DATA/CAR/.eod-1-2.tmp").parent.mkdir(parents=True, exist_ok=True)
        (self.dest / "textures/DATA/CAR/.eod-1-2.tmp").write_bytes(b"half")
        again = run(self.disc, self.dest, resume=True)
        self.assertEqual(output_hashes(self.dest), before)
        self.assertEqual(again["summary"], full["summary"])
        self.assertFalse(list(self.dest.rglob(".eod-*.tmp")))
        self.assertTrue(any(a.get("written") == "unchanged" for a in again["assets"]))

    def test_resume_keeps_records_outside_current_selection(self):
        run(self.disc, self.dest)
        manifest = run(self.disc, self.dest, resume=True, role="sky")
        self.assertEqual(manifest["summary"]["total"], 8)
        self.assertEqual(manifest["run"]["total"], 1)

    def test_progress_events_and_pool(self):
        events = []
        manifest = run(self.disc, self.dest, jobs=2, progress=events.append)
        progress = [e for e in events if e["event"] == "progress"]
        self.assertEqual([e["done"] for e in progress], list(range(1, 9)))
        self.assertTrue(all(e["total"] == 8 and e["assetId"] for e in progress))
        self.assertEqual(events[0]["event"], "start")
        self.assertEqual(events[-1]["event"], "finish")
        self.assertEqual(manifest["summary"]["exported"], 7)

    def test_cli_progress_lines_and_exit_status(self):
        proc = subprocess.run([sys.executable, str(TOOLS / "texture_export.py"), "--disc", str(self.disc), "--dest",
                               str(self.dest), "--jobs", "1"], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 1)  # bad.tex failed
        lines = [json.loads(l) for l in proc.stderr.splitlines()]
        self.assertTrue(any(l.get("event") == "progress" and l["done"] == 8 for l in lines))
        summary = json.loads(proc.stdout)
        self.assertEqual((summary["exported"], summary["failed"], summary["status"]), (7, 1, "complete"))
        refused = subprocess.run([sys.executable, str(TOOLS / "texture_export.py"), "--disc", str(self.disc), "--dest",
                                  str(self.disc / "DATA" / "x")], capture_output=True, text=True)
        self.assertEqual(refused.returncode, 2)

    def test_cli_sigterm_leaves_resumable_partial_manifest(self):
        disc = self.root / "big"
        (disc / "DATA").mkdir(parents=True)
        (disc / "SLUS_209.10").write_bytes(b"x")
        pack(disc / "DATA/TRACKS/BIG.TRK", [(f"t{i}.tex", "TEX", make_tex(i, 16)) for i in range(300)])
        proc = subprocess.Popen([sys.executable, str(TOOLS / "texture_export.py"), "--disc", str(disc), "--dest",
                                 str(self.dest), "--jobs", "1"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        for line in proc.stderr:
            if json.loads(line).get("event") == "progress":
                proc.send_signal(signal.SIGTERM)
                break
        out, _ = proc.communicate(timeout=60)
        if proc.returncode == 0:
            self.skipTest("export finished before the signal arrived")
        self.assertEqual(proc.returncode, 143)
        partial = json.loads((self.dest / "export-manifest.json").read_text())
        self.assertEqual(partial["run"]["status"], "cancelled")
        self.assertLess(len(partial["assets"]), 300)
        done = run(disc, self.dest, resume=True)
        self.assertEqual((done["summary"]["exported"], done["run"]["status"]), (300, "complete"))


class ReimportClassification(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        asset = dict(assetId="A.CAR#x.tex", status="exported", width=16, height=16, reimport=["native-same-size", "host-replacement"])
        direct = dict(assetId="A.CAR#d.tex", status="exported", width=64, height=64, reimport=[])
        self.manifest = dict(assets=[asset, direct])

    def tearDown(self):
        self.tmp.cleanup()

    def png(self, w, h):
        path = self.root / f"{w}x{h}.png"
        tex.write_png(str(path), w, h, bytes(w * h * 4))
        return path

    def check(self, asset, w, h):
        return texture_export.classify_reimport(self.manifest, asset, self.png(w, h))

    def test_same_size(self):
        r = self.check("a.car#X.tex", 16, 16)
        self.assertEqual((r["nativeSameSize"], r["hostReplacement"], r["scale"]), (True, True, 1))

    def test_integer_power_of_two_scale(self):
        r = self.check("A.CAR#x.tex", 64, 64)
        self.assertEqual((r["nativeSameSize"], r["hostReplacement"], r["scale"]), (False, True, 4))

    def test_rejections(self):
        for w, h, reason in ((48, 48, "power of two"), (64, 32, "uniform"), (8192 // 16 * 16, 8192, "4096")):
            r = self.check("A.CAR#x.tex", w, h)
            self.assertFalse(r["hostReplacement"], (w, h))
        self.assertIn("power of two", self.check("A.CAR#x.tex", 48, 48)["reasons"]["host-replacement"])
        self.assertIn("uniform", self.check("A.CAR#x.tex", 64, 32)["reasons"]["host-replacement"])

    def test_cap_at_4096(self):
        self.manifest["assets"][0].update(width=256, height=256)
        self.assertTrue(self.check("A.CAR#x.tex", 4096, 4096)["hostReplacement"])
        self.assertIn("4096", self.check("A.CAR#x.tex", 8192, 8192)["reasons"]["host-replacement"])

    def test_direct_colour_qualifies_for_nothing(self):
        r = self.check("A.CAR#d.tex", 64, 64)
        self.assertEqual((r["nativeSameSize"], r["hostReplacement"]), (False, False))

    def test_unknown_asset(self):
        with self.assertRaises(ValueError):
            self.check("A.CAR#nope.tex", 16, 16)


def encode_png(width, height, ctype, depth, rows, palette=None, trns=None, filters=None, interlace=False):
    """Mini PNG encoder: rows are lists of raw sample ints; cycles filter types."""
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]

    def pack_row(samples):
        if depth == 16:
            return b"".join(struct.pack(">H", s) for s in samples)
        if depth == 8:
            return bytes(samples)
        bits = "".join(format(s, f"0{depth}b") for s in samples)
        bits += "0" * (-len(bits) % 8)
        return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))

    def filtered(packed_rows):
        out, prev = bytearray(), None
        bpp = max(1, channels * depth // 8)
        for n, row in enumerate(packed_rows):
            ft = (filters or [0, 1, 2, 3, 4])[n % len(filters or [0, 1, 2, 3, 4])]
            up = prev or bytes(len(row))
            res = bytearray()
            for i, v in enumerate(row):
                a = row[i - bpp] if i >= bpp else 0
                b = up[i]
                c = up[i - bpp] if i >= bpp else 0
                if ft == 0: p = 0
                elif ft == 1: p = a
                elif ft == 2: p = b
                elif ft == 3: p = (a + b) >> 1
                else:
                    pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                    p = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                res.append((v - p) & 255)
            out += bytes([ft]) + res
            prev = row
        return bytes(out)

    if interlace:
        data = b""
        for x0, y0, dx, dy in tex._ADAM7:
            sub = [[rows[y][x * channels + k] for x in range(x0, width, dx) for k in range(channels)]
                   for y in range(y0, height, dy)] if x0 < width and y0 < height else []
            data += filtered([pack_row(r) for r in sub])
    else:
        data = filtered([pack_row(r) for r in rows])

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, ctype, 0, 0, int(interlace)))
    if palette:
        png += chunk(b"PLTE", bytes(palette))
    if trns is not None:
        png += chunk(b"tRNS", bytes(trns))
    return png + chunk(b"IDAT", zlib.compress(data)) + chunk(b"IEND", b"")


class ReadPngTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def read(self, png):
        path = Path(self.tmp.name) / "t.png"
        path.write_bytes(png)
        return tex.read_png(str(path))

    def rows(self, w, h, channels, top, seed=7):
        import random
        r = random.Random(seed)
        return [[r.randrange(top + 1) for _ in range(w * channels)] for _ in range(h)]

    def test_rgba8_all_filters_and_interlace(self):
        rows = self.rows(11, 9, 4, 255)
        want = bytes(v for row in rows for v in row)
        for interlace in (False, True):
            self.assertEqual(self.read(encode_png(11, 9, 6, 8, rows, interlace=interlace)), (11, 9, want))

    def test_rgb8_and_16(self):
        rows = self.rows(7, 5, 3, 255)
        want = b"".join(bytes((r[3*x], r[3*x+1], r[3*x+2], 255)) for r in rows for x in range(7))
        self.assertEqual(self.read(encode_png(7, 5, 2, 8, rows))[2], want)
        rows16 = [[v * 257 for v in row] for row in rows]
        self.assertEqual(self.read(encode_png(7, 5, 2, 16, rows16, interlace=True))[2], want)

    def test_gray_and_gray_alpha(self):
        rows = self.rows(9, 4, 1, 255)
        want = b"".join(bytes((v, v, v, 255)) for r in rows for v in r)
        self.assertEqual(self.read(encode_png(9, 4, 0, 8, rows))[2], want)
        self.assertEqual(self.read(encode_png(9, 4, 0, 16, [[v * 257 for v in r] for r in rows], interlace=True))[2], want)
        ga = self.rows(6, 3, 2, 255)
        want = b"".join(bytes((r[2*x], r[2*x], r[2*x], r[2*x+1])) for r in ga for x in range(6))
        self.assertEqual(self.read(encode_png(6, 3, 4, 8, ga))[2], want)
        low = self.rows(10, 3, 1, 3)
        want = b"".join(bytes((v * 85,) * 3 + (255,)) for r in low for v in r)
        self.assertEqual(self.read(encode_png(10, 3, 0, 2, low))[2], want)

    def test_palette_with_transparency(self):
        palette = [255, 0, 0, 0, 255, 0, 0, 0, 255, 9, 9, 9]
        rows = self.rows(13, 6, 1, 3)
        want = b"".join(bytes(palette[3*v:3*v+3]) + bytes([[10, 200][v == 1] if v < 2 else 255]) for r in rows for v in r)
        for depth in (2, 4, 8):
            for interlace in (False, True):
                got = self.read(encode_png(13, 6, 3, depth, rows, palette, [10, 200], interlace=interlace))
                self.assertEqual(got[2], want, (depth, interlace))

    def test_trns_for_gray_and_rgb(self):
        rows = [[5, 6, 5]]
        got = self.read(encode_png(3, 1, 0, 8, rows, trns=[0, 5]))[2]
        self.assertEqual(got, bytes((5, 5, 5, 0, 6, 6, 6, 255, 5, 5, 5, 0)))
        got = self.read(encode_png(2, 1, 2, 8, [[1, 2, 3, 4, 5, 6]], trns=[0, 1, 0, 2, 0, 3]))[2]
        self.assertEqual(got, bytes((1, 2, 3, 0, 4, 5, 6, 255)))

    def test_errors_are_clear(self):
        with self.assertRaises(ValueError):
            self.read(b"not a png at all")
        bad = encode_png(4, 4, 3, 8, [[0] * 4] * 4, palette=[0, 0, 0])
        bad = bad.replace(b"PLTE", b"XXXX")
        with self.assertRaises(ValueError):
            self.read(bad)
        with self.assertRaises(ValueError):
            self.read(encode_png(4, 4, 6, 8, [[0] * 16] * 4)[:-30])


@unittest.skipUnless(RETAIL_ON and RETAIL.is_dir(), "set EOD_TEST_RETAIL_TEXTURES=1 with a retail disc")
class RetailExport(unittest.TestCase):
    def test_subset_roundtrips_exactly(self):
        import modding_backend
        import texture_edit
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "out"
            manifest = run(RETAIL, dest, select=["DATA/TRACKS/ANOKA.TRK#*", "DATA/RES/CT.RES#790*"],
                           jobs=max(1, (os.cpu_count() or 2) - 2))
            self.assertEqual(manifest["summary"]["failed"] + manifest["summary"]["unsupported"], 0)
            self.assertGreater(manifest["summary"]["exported"], 100)
            assets = by_id(manifest)
            self.assertTrue(any(a["role"] == "sky" for a in assets.values()))
            self.assertTrue(any(a["role"] == "vehicle-paint" for a in assets.values()))
            for assetid, asset in assets.items():
                archive, member = assetid.split("#", 1)
                original, _ = modding_backend.payload(RETAIL / archive, member, verify_crc=False)
                png = dest / asset["output"]
                self.assertEqual(texture_edit.import_png(original, png), original, assetid)
                self.assertEqual(asset_registry.sha256(original), asset["memberSha256"])
            again = run(RETAIL, dest, select=["DATA/TRACKS/ANOKA.TRK#*", "DATA/RES/CT.RES#790*"], resume=True)
            self.assertEqual(again["run"]["resumed"], len(assets))


if __name__ == "__main__":
    unittest.main()
