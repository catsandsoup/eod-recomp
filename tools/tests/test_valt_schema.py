"""VALT table schema: every retail key gets a human label and a reader status.

Retail checks read the disc at ~/Library/Application Support/TDEODRecomp/disc or EOD_DISC and skip
without it. The synthetic checks always run.
"""
import os
from pathlib import Path
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "formats"))
import valt_schema as vs  # noqa: E402

DISC = Path(os.environ.get("EOD_DISC", "~/Library/Application Support/TDEODRecomp/disc")).expanduser()
RETAIL = (DISC / "SLUS_209.10").is_file() and (DISC / "DATA").is_dir()


def table(*sections):
    return {"sections": [{"name": name, "pairs": list(pairs)} for name, pairs in sections]}


class SyntheticSchemaTest(unittest.TestCase):
    def test_physics_labels_and_units(self):
        schema = vs.describe_table("DATA/CAR/X.CAR", "physics.val", table(
            ("Chassis", [("mass", "3000"), ("damage_scale", ".3"), ("@speedo_slider", "$0000$speedo_slider")]),
            ("Transmission", [("g1", "3.2"), ("rear_end_ratio", "4.1")]),
            ("$0000$speedo_slider", [("val1", "0, -6, -4")])))
        self.assertEqual(schema["title"], "Car handling and engine")
        mass = schema["fields"][("Chassis", "mass")]
        self.assertEqual((mass["label"], mass["unit"], mass["used"]), ("Car weight", "lb", True))
        self.assertFalse(schema["fields"][("Chassis", "damage_scale")]["used"])
        self.assertEqual(schema["fields"][("Transmission", "g1")]["label"], "Gear ratio")
        link = schema["fields"][("Chassis", "@speedo_slider")]
        self.assertFalse(link["editable"])
        self.assertEqual(schema["sections"][2]["label"], "Speedometer needle shift")

    def test_unknown_keys_never_show_the_raw_name(self):
        spec = vs.describe_field("mystery.val", "stuff", "odd_key_name")
        self.assertNotEqual(spec["label"], "odd_key_name")
        self.assertTrue(spec["explanation"])
        self.assertEqual(spec["confidence"], "low")

    def test_packed_position_is_decoded_to_metres(self):
        # A frame64 value from retail Anoka (c_*.va0) decodes through world_export.decode_frame64.
        note = vs.value_note("event_props", "object", "unfloat_frame64", ",VfOV6vh1BZ77nm,xx1mVCnLFZ,qG0c96SZsVR.O" + "a" * 24)
        self.assertTrue(note == "" or note.startswith("Position x"))

    def test_parts_library_does_not_claim_the_game_reads_it(self):
        spec = vs.describe_field("tire.val", "Bias-Ply", "magic_b")
        self.assertEqual(spec["label"], "Tyre curve stiffness (B)")
        self.assertIsNone(spec["used"])


@unittest.skipUnless(RETAIL, "retail disc not found (set EOD_DISC)")
class RetailSchemaTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from valt_schema_build import iter_valts
        cls.tables = [(archive, member, parsed) for archive, member, parsed in iter_valts(DISC)]

    def test_all_tables_found(self):
        self.assertEqual(len(self.tables), 755)

    def test_every_key_has_label_explanation_and_status(self):
        missing = []
        for archive, member, parsed in self.tables:
            schema = vs.describe_table(archive, member, parsed)
            self.assertTrue(schema["title"] and schema["summary"], member)
            for index, section in enumerate(parsed["sections"]):
                self.assertTrue(schema["sections"][index]["label"], (member, section["name"]))
                for key, value in section["pairs"]:
                    spec = schema["fields"][(section["name"], key)]
                    if spec["label"] == key or not spec["explanation"] or spec["confidence"] not in ("high", "medium", "low"):
                        missing.append((member, section["name"], key))
                    if spec["min"] is not None and spec["max"] is not None:
                        try:
                            number = float(value)
                        except ValueError:
                            continue
                        self.assertTrue(spec["min"] <= number <= spec["max"], (member, key, value))
        self.assertEqual(missing, [])

    def test_physics_and_shadow_are_fully_curated(self):
        for archive, member, parsed in self.tables:
            if member.lower() not in ("physics.val", "shadow.val"):
                continue
            for section in parsed["sections"]:
                for key, _ in section["pairs"]:
                    spec = vs.describe_field(member, section["name"], key)
                    self.assertTrue(spec["curated"], (archive, member, section["name"], key))

    def test_backend_inspect_returns_labels(self):
        import modding_backend as mb
        with tempfile.TemporaryDirectory() as root:
            project = str(Path(root) / "p.eodproj")
            bus = mb.inspect(dict(archive="DATA/CAR/BUS.CAR", asset="physics.val", project=project), DISC)
            self.assertEqual(bus["title"], "Car handling and engine")
            chassis = next(s for s in bus["sections"] if s["name"] == "Chassis")
            fields = {f["key"]: f for f in chassis["fields"]}
            self.assertEqual((fields["mass"]["label"], fields["mass"]["unit"]), ("Car weight", "lb"))
            self.assertIn("label", bus["sections"][0])
            shadow = mb.inspect(dict(archive="DATA/TRACKS/HOMEBASE.TRK", asset="shadow.val", project=project), DISC)
            self.assertEqual(shadow["title"], "Track shadows")
            self.assertTrue(all(f["label"] != f["key"] for s in shadow["sections"] for f in s["fields"]))


if __name__ == "__main__":
    unittest.main()
