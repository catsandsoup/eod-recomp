"""Sky tile-set export. Synthetic mesh always; retail Anoka when EOD_TEST_RETAIL_TEXTURES=1.

Run: EOD_HEADLESS=1 EOD_MUTE=1 python3 -m unittest tools/tests/test_sky_export.py
"""
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import modding_backend as mb  # noqa: E402
import sky_export as se  # noqa: E402
from tex import read_png, write_png  # noqa: E402
from texture_preview import decode_preview  # noqa: E402

RETAIL = Path(os.environ.get("EOD_TEST_DISC", str(Path.home() / "Library/Application Support/TDEODRecomp/disc")))
RETAIL_ON = os.environ.get("EOD_TEST_RETAIL_TEXTURES") == "1"
ANOKA = "DATA/TRACKS/ANOKA.TRK"


def sha(b):
    return hashlib.sha256(b).hexdigest()


def errors(a, b):
    e = [abs(a[i] - b[i]) for i in range(len(a)) if i % 4 != 3]
    return sum(e) / len(e), max(e)


class SyntheticSkyTest(unittest.TestCase):
    """One 32x32 tile on a 4x4 vertex grid: az 30..60 deg, el 10..40 deg, radius 100."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        w = h = 32
        self.rgba = bytes(c for y in range(h) for x in range(w)
                          for c in (8 * x, 8 * y, 128 + 3 * (x - y), 255))
        (self.root / "tiles").mkdir()
        write_png(str(self.root / "tiles/sky01A.tex.png"), w, h, self.rgba)
        verts, tris = [], []
        for j in range(4):
            for i in range(4):
                az, el = math.radians(60 - 10 * i), math.radians(40 - 10 * j)  # u grows with decreasing azimuth
                verts.append([100 * math.cos(el) * math.sin(az), 100 * math.sin(el), 100 * math.cos(el) * math.cos(az),
                              i / 3, j / 3])
        for j in range(3):
            for i in range(3):
                a = j * 4 + i
                tris += [[a, a + 1, a + 4], [a + 1, a + 5, a + 4]]
        self.manifest = dict(archive="SYN", tiles=[dict(
            member="sky01A.tex", assetId="SYN#sky01A.tex", indexKey="0", paletteKey="0", width=w, height=h,
            output="tiles/sky01A.tex.png", decodedRgbaSha256=sha(self.rgba), mesh=dict(vertices=verts, triangles=tris))],
            panorama=dict(projection=dict(width=384, azimuthAtX0=0.0, xDirection=1)))
        se.write_json(self.root / "sky-manifest.json", self.manifest)

    def test_panorama_places_tile_by_direction(self):
        W, H, pano = se.render_panorama(self.manifest, {"sky01A.tex": (self.rgba, 32, 32)})
        self.assertEqual((W, H), (384, 96))
        alpha = lambda az, el: pano[(int((90 - el) / 90 * H) * W + int(az / 360 * W)) * 4 + 3]  # noqa: E731
        self.assertEqual(alpha(45, 25), 255)
        self.assertEqual(alpha(90, 25), 0)
        self.assertEqual(alpha(45, 60), 0)
        # u = 0 sits at azimuth 60 (red channel 0), u = 1 at azimuth 30.
        o = lambda az: (int((90 - 25) / 90 * H) * W + int(az / 360 * W)) * 4  # noqa: E731
        self.assertLess(pano[o(59)], pano[o(31)])

    def test_reimport_tiles_identity_and_k(self):
        res = se.reimport(self.root / "sky-manifest.json", self.root / "out", tiles_dir=self.root / "tiles", k=4)
        t = res["tiles"][0]
        self.assertEqual(t["changedTexels"], 0)
        self.assertEqual(t["files"]["native"]["rgbaSha256"], sha(self.rgba))
        w, h, big = read_png(str(self.root / "out" / t["files"]["k4"]["file"]))
        self.assertEqual((w, h), (128, 128))
        self.assertEqual(se.downscale(big, w, h, 4), self.rgba)

    def test_panorama_roundtrip_bounded(self):
        W, H, pano = se.render_panorama(self.manifest, {"sky01A.tex": (self.rgba, 32, 32)}, 2048)
        write_png(str(self.root / "pano.png"), W, H, pano)
        res = se.reimport(self.root / "sky-manifest.json", self.root / "back", panorama=self.root / "pano.png")
        _, _, back = read_png(str(self.root / "back" / res["tiles"][0]["files"]["native"]["file"]))
        mae, worst = errors(self.rgba, back)
        self.assertEqual(res["tiles"][0]["uncoveredTexels"], 0)
        self.assertLess(mae, 2.0)
        self.assertLess(worst, 16)

    def test_rejects_bad_k_and_edited_source(self):
        with self.assertRaises(ValueError):
            se.reimport(self.root / "sky-manifest.json", self.root / "o", tiles_dir=self.root / "tiles", k=3)
        write_png(str(self.root / "tiles/sky01A.tex.png"), 32, 32, bytes(32 * 32 * 4))
        with self.assertRaises(ValueError):
            se.reimport(self.root / "sky-manifest.json", self.root / "o", tiles_dir=self.root / "tiles")

    def test_test_artwork_is_indexable(self):
        t = dict(self.manifest["tiles"][0], geometry=dict(upEdge="v0 (PNG top)"),
                 neighbours=dict(u0=dict(tile="sky16A.tex"), u1=dict(tile="sky02A.tex")))
        for scale in (1, 8):
            w, h, rgba = se.test_tile(t, scale, scale)
            self.assertEqual((w, h), (32 * scale, 32 * scale))
            self.assertLessEqual(len({rgba[i:i + 4] for i in range(0, len(rgba), 4)}), 256)


@unittest.skipUnless(RETAIL_ON and RETAIL.is_dir(), "set EOD_TEST_RETAIL_TEXTURES=1 with a retail disc")
class RetailAnokaSkyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dest = Path(cls.tmp.name) / "sky"
        cls.manifest = se.export(RETAIL, ANOKA, cls.dest)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_topology_and_identity(self):
        m = self.manifest
        self.assertEqual(m["topology"], "ring16-horizon-band-A-plus-cap-B")
        self.assertEqual(len(m["tiles"]), 32)
        by = {t["member"]: t for t in m["tiles"]}
        self.assertEqual(by["sky01A.tex"]["neighbours"]["u1"]["tile"], "sky02A.tex")
        self.assertEqual(by["sky01A.tex"]["neighbours"]["v0"]["tile"], "sky01B.tex")
        self.assertEqual(by["sky12B.tex"]["runtimeKeyShared"], ["DATA/TRACKS/ANOKA.TRK#sky13B.tex"])
        self.assertTrue(all(t["crcOk"] for t in m["tiles"]))

    def test_tile_export_lossless(self):
        path = RETAIL / ANOKA
        entries = mb.directory(path)
        for t in self.manifest["tiles"]:
            w, h, rgba, _ = decode_preview(mb.payload(path, t["member"], entries)[0])
            pw, ph, png = read_png(str(self.dest / t["output"]))
            self.assertEqual((pw, ph, sha(png)), (w, h, sha(rgba)), t["member"])
            self.assertEqual(t["decodedRgbaSha256"], sha(rgba))

    def test_unchanged_tiles_reimport_identical(self):
        out = Path(self.tmp.name) / "re"
        res = se.reimport(self.dest / "sky-manifest.json", out, tiles_dir=self.dest / "tiles")
        for t, rec in zip(self.manifest["tiles"], res["tiles"]):
            self.assertEqual(rec["changedTexels"], 0)
            self.assertEqual(rec["files"]["native"]["rgbaSha256"], t["decodedRgbaSha256"])

    def test_panorama_roundtrip_bounded(self):
        out = Path(self.tmp.name) / "pano"
        res = se.reimport(self.dest / "sky-manifest.json", out, panorama=self.dest / "panorama.png")
        total, worst = 0.0, 0
        for t, rec in zip(self.manifest["tiles"], res["tiles"]):
            _, _, a = read_png(str(self.dest / t["output"]))
            _, _, b = read_png(str(out / rec["files"]["native"]["file"]))
            mae, mx = errors(a, b)
            total, worst = total + mae, max(worst, mx)
        # Measured 2026-11-11 at the default 2048x512: mean of tile MAEs 0.18/255, max 42 (isolated stars).
        self.assertLess(total / 32, 1.0)
        self.assertLess(worst, 64)
        print(json.dumps(dict(panoramaRoundtripMeanAbs=round(total / 32, 3), max=worst)))


if __name__ == "__main__":
    unittest.main()
