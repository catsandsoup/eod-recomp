import json
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import vehicle_export as ve

DISC = Path(os.environ.get("EOD_DISC", Path.home() / "Library/Application Support/TDEODRecomp/disc"))
RETAIL = os.environ.get("EOD_TEST_RETAIL_MODELS") == "1" and (DISC / "DATA/CAR").is_dir()
# Pinned from a full --lod all export of the retail disc.
TOTALS = dict(models=395, parts=3244, vertices=533863, triangles=288917, materials=431, unresolved=12)
UNRESOLVED = {"GLADIA_1": ["RollBar.tex"], "HURRIC_1": ["RollBar.tex"],
              **{f"TRAILER{s}": ["HWindows.tex", "Hobby.tex"] for s in ("", "2", "3", "4", "5")}}


class AlphaUnit(unittest.TestCase):
    def test_treatment(self):
        self.assertEqual(ve.alpha_treatment(bytes([60] * 50), "cutout"), ("opaque", 0.0))
        self.assertEqual(ve.alpha_treatment(bytes([0] * 10 + [255] * 90), "cutout")[0], "cutout")
        self.assertEqual(ve.alpha_treatment(bytes([60] * 50), "blend")[0], "blend")
        self.assertEqual(ve.alpha_treatment(bytes([0] * 10), "opaque")[0], "opaque")
        self.assertEqual(ve.apply_alpha(bytes([9, 9, 9, 0, 1, 1, 1, 60]), "cutout"), bytes([9, 9, 9, 0, 1, 1, 1, 255]))


class Parsing(unittest.TestCase):
    def test_lod_spec(self):
        self.assertEqual(ve.parse_lods("1"), ({1}, False))
        self.assertEqual(ve.parse_lods("1,2,shadow"), ({1, 2}, True))
        self.assertEqual(ve.parse_lods("ALL"), (None, True))
        for bad in ("", "x", "1,low"):
            with self.assertRaises(ValueError):
                ve.parse_lods(bad)

    def test_not_a_disc(self):
        with tempfile.TemporaryDirectory() as t, self.assertRaises(ValueError):
            ve.disc_root(t)


@unittest.skipUnless(RETAIL, "set EOD_TEST_RETAIL_MODELS=1 with a retail disc")
class Retail(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dest = Path(cls.tmp.name)
        cls.summary = ve.export_all(DISC, cls.dest, lod="all", paint=None, blender=False, wheels=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_accounting_exact(self):
        s = self.summary
        self.assertEqual((s["discCars"], s["exported"], s["failed"]), (37, 37, 0), s["failures"])
        self.assertEqual(s["totals"], TOTALS)
        got = {c["car"]: [u["texture"] for u in c["unresolvedMaterials"]] for c in s["cars"] if c["unresolvedMaterials"]}
        self.assertEqual({k: sorted(v) for k, v in got.items()}, UNRESOLVED)
        self.assertTrue(all(not c["missingLods"] for c in s["cars"]))

    def test_obj_mtl_wellformed(self):
        for car in self.summary["cars"]:
            d = self.dest / car["car"]
            info = json.loads((d / "vehicle.json").read_text())
            v = vt = vn = f = 0
            lines = (d / f"{car['car']}.obj").read_text().splitlines()
            for line in lines:
                t = line.split()
                if t[:1] == ["v"]:
                    v += 1
                    self.assertEqual(len(t), 4)
                elif t[:1] == ["vt"]:
                    vt += 1
                elif t[:1] == ["vn"]:
                    vn += 1
                elif t[:1] == ["f"]:
                    f += 1
                    for c in t[1:]:
                        a = [int(x) for x in c.split("/")]
                        self.assertTrue(1 <= a[0] <= v and 1 <= a[1] <= vt and 1 <= a[2] <= vn, (car["car"], line))
            self.assertEqual((v, vt, vn), (info["totals"]["vertices"],) * 3)
            self.assertEqual(f, info["totals"]["triangles"])
            self.assertEqual(f, sum(p["triangles"] for p in info["parts"]))
            mtl = (d / f"{car['car']}.mtl").read_text()
            maps = re.findall(r"^map_Kd (.+)$", mtl, re.M)
            self.assertEqual(sorted(maps), sorted(info["files"]["textures"]))
            for m in maps:
                head = (d / m).read_bytes()[:8]
                self.assertEqual(head, b"\x89PNG\r\n\x1a\n")
            names = {m["name"] for m in info["materials"]}
            self.assertEqual(set(re.findall(r"^newmtl (.+)$", mtl, re.M)), names)
            self.assertEqual({p["material"] for p in info["parts"]}, names)
            for m in info["materials"]:
                if m["status"] == "unresolved":
                    self.assertNotIn(m["name"] + ".png", " ".join(maps))
                else:
                    self.assertIn(m["status"], ("resolved", "no-texture-field"))
            self.assertTrue(info["members"] and all(x["crcOk"] for x in info["members"]))
            self.assertTrue(info["physics"] and info["wheels"]["placement"] == "origin")

    def test_bus_alpha_and_double_sided(self):
        with tempfile.TemporaryDirectory() as t:
            ve.export_car(DISC, "DATA/CAR/BUS.CAR", t)
            ve.export_car(DISC, "DATA/CAR/BUS.CAR", Path(t) / "blend", alpha="blend")
            ve.export_car(DISC, "DATA/CAR/BUS.CAR", Path(t) / "two", double_sided=True)
            mtl = (Path(t) / "BUS/BUS.mtl").read_text()
            self.assertEqual(re.findall(r"^map_d (.+)$", mtl, re.M), ["textures/wheel_bustire.png"])
            self.assertEqual(re.findall(r"^map_d (.+)$", (Path(t) / "blend/BUS/BUS.mtl").read_text(), re.M),
                             ["textures/wheel_bustire.png"])
            body = (Path(t) / "BUS/textures/Bus1.png").read_bytes()
            self.assertEqual(body[:8], b"\x89PNG\r\n\x1a\n")
            one = (Path(t) / "BUS/BUS.obj").read_text().splitlines()
            two = (Path(t) / "two/BUS/BUS.obj").read_text().splitlines()
            cnt = lambda ls, k: sum(1 for l in ls if l.startswith(k + " "))
            self.assertEqual(cnt(two, "f"), 2 * cnt(one, "f"))
            self.assertEqual(cnt(two, "vn"), 2 * cnt(one, "vn"))
            self.assertEqual(cnt(two, "v"), cnt(one, "v"))
            nv, nvn = cnt(two, "v"), cnt(two, "vn")
            for l in two:
                if l.startswith("f "):
                    for c in l.split()[1:]:
                        a = [int(x) for x in c.split("/")]
                        self.assertTrue(a[0] <= nv and a[2] <= nvn)

    def test_bus_livery_as_kit(self):
        info = json.loads((self.dest / "BUS/vehicle.json").read_text())
        mats = {m["texture"]: m for m in info["materials"]}
        self.assertEqual(info["paint"]["number"], "790")
        self.assertEqual(mats["Bus1.tex"]["source"], "DATA/RES/CT.RES#790.tm0")
        self.assertEqual(mats["Bus2.tex"]["source"], "DATA/RES/CT.RES#79a.tm0")
        self.assertTrue(all(m["scope"] == "wheel" for m in info["materials"] if m["name"].startswith("wheel_")))

    def test_explicit_paint_never_substituted(self):
        with tempfile.TemporaryDirectory() as t:
            with self.assertRaises(ValueError):
                ve.export_car(DISC, "BUS.CAR", t, paint="99999")
            with self.assertRaises(ValueError):
                ve.export_car(DISC, "TRAILER.CAR", t, paint="790")


if __name__ == "__main__":
    unittest.main()
