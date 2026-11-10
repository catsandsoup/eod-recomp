import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mod_package
import modding_backend as mb
import vehicle_export as ve
import vehicle_import as vi
import vif_patch
from tex import write_png

DISC = Path(os.environ.get("EOD_DISC", Path.home() / "Library/Application Support/TDEODRecomp/disc"))
RETAIL = os.environ.get("EOD_TEST_RETAIL_MODELS") == "1" and (DISC / "DATA/CAR").is_dir()


def scaled_obj(src, dst, models, factor):
    out, cur = [], ""
    for line in Path(src).read_text().splitlines():
        if line.startswith("o "):
            cur = line[2:].split("__")[0].lower()
        if line.startswith("v ") and cur in models:
            x, y, z = line.split()[1:4]
            line = "v %.7f %s %s" % (float(x) * factor, y, z)
        out.append(line)
    Path(dst).write_text("\n".join(out) + "\n")


class Synthetic(unittest.TestCase):
    def test_foreign_object_name_refused(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / "x.obj"
            p.write_text("o cube\nv 0 0 0\n")
            with self.assertRaises(vi.ImportError_):
                vi.split_obj(p)

    def test_split_rebases_indices(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / "x.obj"
            p.write_text("o A__slot00_g0_t\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvn 0 1 0\nf 1/1/1 2/1/1 3/1/1\n"
                         "o B__slot00_g0_t\nv 5 5 5\nv 6 5 5\nv 5 6 5\nf 4/1/1 5/1/1 6/1/1\n")
            texts = vi.split_obj(p)
            self.assertEqual(set(texts), {"A", "B"})
            self.assertIn("f 1/1/1 2/1/1 3/1/1", texts["B"])
            self.assertIn("v 5 5 5", texts["B"])


@unittest.skipUnless(RETAIL, "set EOD_TEST_RETAIL_MODELS=1 with a retail disc")
class Retail(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)
        cls.disc = DISC.resolve()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_no_edit_roundtrip_every_car_byte_identical(self):
        models = 0
        for car in ve.car_files(self.disc):
            info = ve.export_car(self.disc, car.name, self.dir / "rt", lod="all", wheels=False)
            changes, report = vi.patch_models(self.disc, car, self.dir / "rt" / car.stem / f"{car.stem}.obj")
            self.assertEqual(changes, {}, car.name)
            self.assertEqual(len(report["models"]), info["totals"]["models"], car.name)
            models += len(report["models"])
            self.assertTrue(all(not m["changed"] for m in report["models"].values()))
        self.assertEqual(models, 278)  # 395 exported models minus 117 wheel models

    def test_edit_makes_verified_package(self):
        ve.export_car(self.disc, "AMBULA_1.CAR", self.dir / "e", lod="all", wheels=False)
        obj = self.dir / "e/AMBULA_1/wide.obj"
        scaled_obj(self.dir / "e/AMBULA_1/AMBULA_1.obj", obj, {"low1", "low2", "low3", "low4"}, 1.12)
        r = vi.import_patch(self.disc, "AMBULA_1.CAR", self.dir / "pkg", "ambulance-wide-test", obj=obj)
        self.assertEqual(sorted(m.split("#")[1] for m in r["changedMembers"]), ["Low1.mod", "Low2.mod", "Low3.mod", "Low4.mod"])
        self.assertTrue(r["gameCompatible"])
        # Semantic: patched Low1 decodes to ~1.12x width; archive rebuild keeps every other member identical.
        car = ve.find_car(self.disc, "AMBULA_1.CAR")
        old = mb.payload(car, "Low1.mod")[0]
        new = (self.dir / "pkg/native/DATA/CAR/AMBULA_1.CAR/Low1.mod").read_bytes()
        self.assertEqual(len(old), len(new))
        a, b = ve.decode_model(old), ve.decode_model(new)
        xa = max(abs(v["p"][0]) for p in a["parts"] for v in p["verts"])
        xb = max(abs(v["p"][0]) for p in b["parts"] for v in p["verts"])
        self.assertAlmostEqual(xb / xa, 1.12, delta=0.01)
        self.assertEqual([len(p["tris"]) for p in a["parts"]], [len(p["tris"]) for p in b["parts"]])
        built, method = mod_package.build_archive(car, {"Low1.mod": new})
        self.assertEqual(method, "in-place")
        self.assertEqual(len(built), car.stat().st_size)

    def test_refusals(self):
        ve.export_car(self.disc, "BUS.CAR", self.dir / "r", lod="1", wheels=False)
        obj = self.dir / "r/BUS/BUS.obj"
        with self.assertRaises(vi.ImportError_):  # nothing edited
            vi.import_patch(self.disc, "BUS.CAR", self.dir / "none", "bus-none", obj=obj)
        lines = obj.read_text().splitlines()
        i = max(n for n, l in enumerate(lines) if l.startswith("f "))
        broken = self.dir / "r/topo.obj"
        broken.write_text("\n".join(lines[:i] + lines[i + 1:]) + "\n")
        with self.assertRaises(vi.ImportError_):  # removed face = topology change
            vi.import_patch(self.disc, "BUS.CAR", self.dir / "topo", "bus-topo", obj=broken)
        wrong = self.dir / "r/wrong.obj"
        wrong.write_text(obj.read_text().replace("Low1__", "Low9__"))
        with self.assertRaises(vi.ImportError_):
            vi.import_patch(self.disc, "BUS.CAR", self.dir / "wrong", "bus-wrong", obj=wrong)
        write_png(str(self.dir / "tiny.png"), 16, 16, bytes([1, 2, 3, 255]) * 256)
        with self.assertRaises(vi.ImportError_):  # native size differs
            vi.import_patch(self.disc, "BUS.CAR", self.dir / "tex", "bus-tex", textures=[f"Bus1.tex={self.dir / 'tiny.png'}"])

    def test_texture_replacement_same_layout(self):
        car = ve.find_car(self.disc, "BUS.CAR")
        import struct
        w, h = struct.unpack_from("<2I", mb.payload(car, "Bus1.tex")[0])
        write_png(str(self.dir / "red.png"), w, h, bytes([200, 20, 20, 255]) * (w * h))
        r = vi.import_patch(self.disc, "BUS.CAR", self.dir / "texpkg", "bus-red", textures=[f"Bus1.tex={self.dir / 'red.png'}"])
        self.assertEqual(r["changedMembers"], ["DATA/CAR/BUS.CAR#Bus1.tex"])

    def test_new_vehicle_limits(self):
        with self.assertRaises(ValueError):
            vi.import_new(self.disc, self.dir / "n1", "Trailer", "Zed")
        with self.assertRaises(ValueError):
            vi.import_new(self.disc, self.dir / "n2", "Algar", "Algar")
        with self.assertRaises(vi.ImportError_):
            vi.import_new(self.disc, self.dir / "n3", "Algar", "Zed", car_class="special")

    def test_new_vehicle_overlay(self):
        write_png(str(self.dir / "b.png"), 512, 256, bytes([30, 60, 200, 255]) * (512 * 256))
        r = vi.import_new(self.disc, self.dir / "new", "Algar", "Zed", "777", "midsize", self.dir / "b.png")
        self.assertEqual(r["patchedMembers"], ["Zed.tm0"])
        self.assertTrue((self.dir / "new/DATA/CAR/ZED.CAR").is_file())
        entries = {e["name"] for e in mb.directory(self.dir / "new/DATA/CAR/ZED.CAR")}
        self.assertIn("Zed.tm0", entries)


if __name__ == "__main__":
    unittest.main()
