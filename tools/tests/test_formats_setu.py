"""SETU car setup sheet decoder: synthetic tests always run; retail tests need the disc (read only).

The disc is ~/Library/Application Support/TDEODRecomp/disc or EOD_DISC.
"""
import os
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import formats  # noqa: E402
import modding_backend  # noqa: E402
from formats import setu  # noqa: E402

DISC = Path(os.environ.get("EOD_DISC", "~/Library/Application Support/TDEODRecomp/disc")).expanduser()
RETAIL = (DISC / "DATA").is_dir()

# Anoka default.gar values (retail), used as a realistic synthetic sheet.
SAMPLE = struct.pack("<49f", 0.5074, 0.5, 0, 0, 0, 0.5, 0, 1,
                     0.125, 0.2051, 0.2821, 0.4, 28, 0.125, 0.2051, 0.2821, 0.4, 28,
                     0.625, 0.2051, 0.2821, 0.5, 28, 0.625, 0.2051, 0.2821, 0.3333, 28,
                     0.2571, 0.55, 3.5, 2.8, 2.3, 1.6, 0.6, 0.2, 0.5, 5, 0.5, 0.5, 0.502, 22, 0, 0, 0, 0, 0, 0, 0)


def covered_exactly(view, size):
    spans = sorted((x["offset"], x["offset"] + x["size"]) for x in view["fields"] + view["blocks"])
    cursor = 0
    for start, stop in spans:
        if start != cursor:
            return False
        cursor = stop
    return cursor == size


def retail_sheets():
    sheets = []
    for path in sorted((DISC / "DATA").rglob("*")):
        if not path.is_file() or path.suffix.upper() not in (".TRK", ".RES", ".CAR"):
            continue
        try:
            entries = modding_backend.directory(path)
        except ValueError:
            continue
        for entry in entries:
            if entry["type"] == "SETU":
                data = modding_backend.payload(path, entry["name"], entries, verify_crc=False)[0]
                sheets.append((f"{path.relative_to(DISC)}#{entry['name']}", entry["name"], data))
    return sheets


class SyntheticTests(unittest.TestCase):
    def test_registered_and_every_byte_covered(self):
        self.assertIs(formats.decoder_for("SETU"), setu)
        view = formats.describe("SETU", SAMPLE, {"member": "default.gar"})
        self.assertEqual(view["schema"], "eod.setu/1")
        self.assertEqual(view["title"], "Car setup sheet")
        self.assertTrue(covered_exactly(view, setu.SIZE))
        self.assertEqual(len({f["id"] for f in view["fields"]}), len(view["fields"]))
        self.assertTrue(view["summary"].startswith("Default sheet"))
        self.assertEqual(view["preview"]["kind"], "table")
        groups = {f["group"] for f in view["fields"]}
        for group in ("Front left wheel", "Rear right wheel", "Gearbox", "Final drive", "Brakes", "Steering", "Fuel"):
            self.assertIn(group, groups)
        for f in view["fields"]:
            self.assertIn(f["confidence"], ("verified", "high", "medium", "low"))
            self.assertTrue(f["explanation"] and f["evidence"])
            self.assertLessEqual(f["min"], f["value"])
            self.assertLessEqual(f["value"], f["max"])

    def test_no_edit_is_identical(self):
        self.assertEqual(formats.apply_edits("SETU", SAMPLE, {}), SAMPLE)
        self.assertEqual(setu.encode(SAMPLE, {}), SAMPLE)

    def test_resubmitting_decoded_values_is_identical(self):
        view = setu.decode(SAMPLE, {})
        edits = {f["id"]: f["value"] for f in view["fields"]}
        self.assertEqual(setu.encode(SAMPLE, edits), SAMPLE)

    def test_edit_changes_only_that_field(self):
        view = setu.decode(SAMPLE, {})
        for field in view["fields"]:
            new = field["max"] if field["value"] != field["max"] else field["min"]
            out = setu.encode(SAMPLE, {field["id"]: new})
            changed = [i for i in range(setu.SIZE) if out[i] != SAMPLE[i]]
            self.assertTrue(changed, field["id"])
            self.assertTrue(all(field["offset"] <= i < field["offset"] + 4 for i in changed), field["id"])
            after = {f["id"]: f["value"] for f in setu.decode(out, {})["fields"]}
            self.assertAlmostEqual(after[field["id"]], new, places=5)

    def test_gear_and_pressure_edits(self):
        out = formats.apply_edits("SETU", SAMPLE, {"gear.1": 9.5, "front_left.pressure": "35"})
        self.assertEqual(struct.unpack_from("<f", out, 0x78)[0], 9.5)
        self.assertEqual(struct.unpack_from("<f", out, 0x30)[0], 35.0)
        changed = {i for i in range(setu.SIZE) if out[i] != SAMPLE[i]}
        self.assertTrue(changed <= set(range(0x30, 0x34)) | set(range(0x78, 0x7C)))

    def test_rejections(self):
        for edits in ({"gear.1": 0.0}, {"gear.1": 11}, {"front_left.pressure": 5}, {"fuel.load": 23},
                      {"chassis.front_sway_bar": 1.5}, {"final_drive": float("nan")}, {"final_drive": True},
                      {"final_drive": "fast"}, {"no.such": 1}):
            with self.assertRaises(ValueError):
                setu.encode(SAMPLE, edits)
        with self.assertRaises(ValueError):
            setu.decode(SAMPLE[:-4], {})

    def test_wrong_size_falls_back_to_generic(self):
        view = formats.describe("SETU", SAMPLE + b"\0\0\0\0", {})
        self.assertEqual(view["schema"], "eod.generic/1")
        self.assertTrue(view["warnings"])


@unittest.skipUnless(RETAIL, "retail disc not found")
class RetailTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sheets = retail_sheets()

    def test_all_64_sheets_round_trip(self):
        self.assertEqual(len(self.sheets), 64)
        for where, member, data in self.sheets:
            self.assertEqual(len(data), setu.SIZE, where)
            view = formats.describe("SETU", data, {"member": member})
            self.assertEqual(view["schema"], "eod.setu/1", where)
            self.assertTrue(covered_exactly(view, setu.SIZE), where)
            self.assertEqual(formats.apply_edits("SETU", data, {}), data, where)
            edits = {f["id"]: f["value"] for f in view["fields"]}
            self.assertEqual(setu.encode(data, edits), data, where)
            self.assertIsNotNone(setu.role(member), where)
            for f in view["fields"]:
                self.assertTrue(f["min"] <= f["value"] <= f["max"], f"{where} {f['id']} {f['value']}")

    def test_payload_has_no_fourcc_header(self):
        for where, _, data in self.sheets:
            self.assertNotIn(b"SETU", data, where)
            self.assertNotIn(b"UTES", data, where)

    def test_retail_field_edit_is_local(self):
        where, member, data = next(s for s in self.sheets if s[0].endswith("ANOKA.TRK#default.gar"))
        out = setu.encode(data, {"gear.1": 6.0})
        self.assertEqual([i for i in range(setu.SIZE) if out[i] != data[i]],
                         [i for i in range(0x78, 0x7C) if out[i] != data[i]])
        with self.assertRaises(ValueError):
            setu.encode(data, {"front_right.pressure": 99})


if __name__ == "__main__":
    unittest.main()
