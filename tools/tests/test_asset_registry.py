"""Asset census on synthetic 0TSR archives; retail census when EOD_TEST_RETAIL_TEXTURES=1."""
import contextlib
import io
import json
import os
import random
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import asset_registry
import rst0
from tex import compile_tex

RETAIL = Path(os.environ.get("EOD_TEST_DISC", str(Path.home() / "Library/Application Support/TDEODRecomp/disc")))
RETAIL_ON = os.environ.get("EOD_TEST_RETAIL_TEXTURES") == "1"


def make_tex(seed, size=16, colors=8):
    rng = random.Random(seed)
    palette = [bytes(rng.randrange(256) for _ in range(3)) + b"\xff" for _ in range(colors)]
    return compile_tex(size, size, b"".join(rng.choice(palette) for _ in range(size * size)))


def pack(path, members):
    """members: [(name, fourcc, bytes)] -> 0TSR v6 archive at path."""
    with tempfile.TemporaryDirectory() as work:
        for name, _, data in members:
            Path(work, name).write_bytes(data)
        Path(work, "manifest.json").write_text(json.dumps(dict(
            version=6, entries=[dict(name=n, fourcc=f, version=1, is_extractable=False) for n, f, _ in members])))
        path.parent.mkdir(parents=True, exist_ok=True)
        with contextlib.redirect_stdout(io.StringIO()):
            rst0.pack_archive(work, str(path))


def corrupt_member(path, name, payload):
    """Flip one payload byte so the directory CRC no longer matches."""
    import modding_backend
    entry = next(e for e in modding_backend.directory(path) if e["name"] == name)
    raw = bytearray(path.read_bytes())
    raw[entry["offset"] + len(payload) // 2] ^= 0xFF
    path.write_bytes(raw)


def make_disc(root):
    """Small disc: sky/dup/variant/CRC/undecodable/raster-like/non-archive cases."""
    disc = Path(root) / "disc"
    (disc / "DATA").mkdir(parents=True)
    (disc / "SLUS_209.10").write_bytes(b"synthetic executable")
    a, b = make_tex(1), make_tex(2)
    pack(disc / "DATA/TRACKS/ANOKA.TRK", [
        ("sky01A.tex", "TEX", a), ("sky01A.tm0", "TEX", make_tex(3)), ("ground.tex", "TEX", b),
        ("ground2.tex", "TEX", b), ("bad.tex", "TEX", b"not a texture header at all, just bytes!!"),
        ("roadrace.ili", "ILIN", b"index data " * 8), ("pic.img", "IMAG", b"\1" * 40)])
    pack(disc / "DATA/RES/CT.RES", [("790.tex", "TEX", make_tex(4)), ("tra.tex", "TEX", a)])
    pack(disc / "DATA/CAR/PALO.CAR", [("body.tex", "TEX", make_tex(5, 32)), ("Low0.mod", "MODL", b"mesh" * 9)])
    corrupt_member(disc / "DATA/CAR/PALO.CAR", "Low0.mod", b"mesh" * 9)
    (disc / "DATA/VIDEO").mkdir()
    (disc / "DATA/VIDEO/INTRO.PSS").write_bytes(b"\x00\x00\x01\xba" + b"video" * 10)
    (disc / "DATA/BROKEN.CAR").write_bytes(b"NOPE" + b"\0" * 60)
    return disc


class SyntheticCensus(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.disc = make_disc(cls.tmp.name)
        cls.result = asset_registry.census(cls.disc)
        cls.by_id = {a["assetId"]: a for a in cls.result["assets"]}

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_inventory_and_totals(self):
        totals = self.result["totals"]
        self.assertEqual(totals["archives"], 3)
        self.assertEqual(totals["members"], 11)
        self.assertEqual(totals["tex"], 8)
        self.assertEqual(totals["texDecode"], {"ok": 7, "failed": 1})
        self.assertEqual([e["path"] for e in totals["archiveParseErrors"]], ["DATA/BROKEN.CAR"])
        kinds = {f["path"]: f["kind"] for f in self.result["files"]}
        self.assertEqual(kinds["DATA/VIDEO/INTRO.PSS"], "other")
        self.assertEqual(kinds["DATA/TRACKS/ANOKA.TRK"], "0tsr-v6")

    def test_crc_mismatch_is_recorded_not_fatal(self):
        self.assertEqual(self.result["totals"]["crcMismatches"], ["DATA/CAR/PALO.CAR#Low0.mod"])
        self.assertFalse(self.by_id["DATA/CAR/PALO.CAR#Low0.mod"]["crcOk"])
        self.assertTrue(self.by_id["DATA/CAR/PALO.CAR#body.tex"]["crcOk"])

    def test_undecodable_tex(self):
        bad = self.by_id["DATA/TRACKS/ANOKA.TRK#bad.tex"]
        self.assertEqual(bad["decode"]["status"], "failed")
        self.assertTrue(bad["decode"]["error"])
        self.assertNotIn("indexKey", bad)

    def test_duplicates_are_linked_not_merged(self):
        one, two = self.by_id["DATA/TRACKS/ANOKA.TRK#ground.tex"], self.by_id["DATA/TRACKS/ANOKA.TRK#ground2.tex"]
        self.assertEqual(one["duplicateOf"], [two["assetId"]])
        self.assertEqual(two["duplicateOf"], [one["assetId"]])
        shared = self.by_id["DATA/TRACKS/ANOKA.TRK#sky01A.tex"]
        self.assertEqual(shared["duplicateOf"], ["DATA/RES/CT.RES#tra.tex"])
        self.assertEqual(one["runtimeKeyShared"], [two["assetId"]])

    def test_tex_and_tm0_are_variants(self):
        self.assertEqual(self.by_id["DATA/TRACKS/ANOKA.TRK#sky01A.tex"]["variantOf"], ["DATA/TRACKS/ANOKA.TRK#sky01A.tm0"])
        self.assertEqual(self.by_id["DATA/TRACKS/ANOKA.TRK#ground.tex"]["variantOf"], [])

    def test_roles(self):
        roles = {k: (v["role"], v["roleConfidence"]) for k, v in self.by_id.items() if v["type"] == "TEX"}
        self.assertEqual(roles["DATA/TRACKS/ANOKA.TRK#sky01A.tex"], ("sky", "high"))
        self.assertEqual(roles["DATA/TRACKS/ANOKA.TRK#ground.tex"][0], "track")
        self.assertEqual(roles["DATA/RES/CT.RES#790.tex"][0], "vehicle-paint")
        self.assertEqual(roles["DATA/RES/CT.RES#tra.tex"][0], "other")
        self.assertEqual(roles["DATA/CAR/PALO.CAR#body.tex"][0], "vehicle")
        self.assertEqual(asset_registry.classify_role("DATA/RES/UICAR.RES", "uicar_3.tex")[0], "ui")
        self.assertEqual(asset_registry.classify_role("DATA/RES/COMMON.RES", "bigfont.tex")[0], "font")

    def test_header_and_identity_fields(self):
        body = self.by_id["DATA/CAR/PALO.CAR#body.tex"]
        self.assertEqual((body["width"], body["height"], body["bitsPerPixel"]), (32, 32, 8))
        self.assertEqual(len(body["indexKey"]), 16)
        self.assertEqual(body["reimport"], ["native-same-size", "host-replacement"])
        self.assertEqual(body["alpha"], "straight-0-255-from-palette")

    def test_not_covered_lists_raster_like_types(self):
        self.assertEqual(self.result["notCovered"]["IMAG"]["count"], 1)
        self.assertNotIn("MODL", self.result["notCovered"])

    def test_census_is_deterministic_and_pool_matches_inline(self):
        again = asset_registry.census(self.disc, jobs=2)
        strip = lambda r: [{k: v for k, v in a.items()} for a in r["assets"]]
        self.assertEqual(strip(again), strip(self.result))


@unittest.skipUnless(RETAIL_ON and RETAIL.is_dir(), "set EOD_TEST_RETAIL_TEXTURES=1 with a retail disc")
class RetailCensus(unittest.TestCase):
    def test_full_census(self):
        result = asset_registry.census(RETAIL, jobs=max(1, (os.cpu_count() or 2) - 2))
        totals = result["totals"]
        self.assertEqual(totals["archives"], 72)
        self.assertEqual(totals["members"], 7329)
        self.assertEqual(totals["tex"], 4923)
        self.assertEqual(totals["archiveParseErrors"], [])
        self.assertEqual(totals["texDecode"], {"ok": 4923})
        self.assertIn("DATA/TRACKS/ANOKA.TRK#roadrace.ili", totals["crcMismatches"])
        self.assertEqual({"IMAG", "CANV", "GTEX"} - set(result["notCovered"]), set())


if __name__ == "__main__":
    unittest.main()
