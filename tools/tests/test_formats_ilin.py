"""ILIN labelled decoder: synthetic layout tests always run; retail tests need the disc.

Retail data is read only. The disc is ~/Library/Application Support/TDEODRecomp/disc
or EOD_DISC. Members are read with verify_crc=False because the empty track
`.ili` placeholders carry a directory checksum of the original, larger file.
"""
import os
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import formats  # noqa: E402
from formats import ilin  # noqa: E402

DISC = Path(os.environ.get("EOD_DISC", "~/Library/Application Support/TDEODRecomp/disc")).expanduser()
RETAIL = (DISC / "DATA/TRACKS").is_dir() and (DISC / "DATA/RES/GHOSTCAR.RES").is_file()
EXPECTED_MEMBERS = 211
EMPTY_MEMBERS = 21


def synthetic(count=4, tail=True):
    """Closed line of `count` nodes, one side-line pointer and the 16-byte list record."""
    base = 48 + 4 * (count + 3)
    node_end = base + count * 60
    out = bytearray(struct.pack("<3I3ffIIIII", 0, 48, 60, 40.0, 40.0, 40.0, 0.05, count << 16, base, 0, 0, count + 3))
    out += struct.pack(f"<{count + 3}I", 32, *[base + i * 60 + 20 for i in range(count)],
                       base + 56, node_end + 12)
    for i in range(count):
        nxt = base + ((i + 1) % count) * 60
        out += struct.pack("<4fIIf", 10.0 * i, 5.0 * i + 1, 1.0, 0.0, i << 16, nxt, 10.0)
        out += struct.pack("<fffffII", 10.0 * i, 35.556, 0.01, 24.4, 0.0, 0, 256) + struct.pack("<I", 0)
    out += struct.pack("<4I", 0, 0, 2, base)
    return bytes(out)


class SyntheticTests(unittest.TestCase):
    def test_registered_and_round_trips(self):
        self.assertIs(formats.decoder_for("ILIN"), ilin)
        data = synthetic()
        view = formats.describe("ILIN", data, {"member": "oval.ild"})
        self.assertEqual(view["title"], "Oval racing line")
        self.assertEqual(formats.apply_edits("ILIN", data, {}), data)
        self.assertEqual(view["preview"]["kind"], "path")
        self.assertTrue(view["preview"]["closed"])
        self.assertEqual(len(view["preview"]["points"]), 4)

    def test_every_byte_covered_once(self):
        data = synthetic(6)
        view = ilin.decode(data, {})
        owner = bytearray(len(data))
        for item in view["fields"] + view["blocks"]:
            for i in range(item["offset"], item["offset"] + item["size"]):
                owner[i] += 1
        self.assertEqual(set(owner), {1})

    def test_edit_changes_only_that_float(self):
        data = synthetic()
        new = formats.apply_edits("ILIN", data, {"node.2.x": 123.5})
        diff = [i for i in range(len(data)) if data[i] != new[i]]
        self.assertTrue(0 < len(diff) <= 4)
        self.assertAlmostEqual(struct.unpack_from("<f", new, 48 + 4 * 7 + 2 * 60)[0], 123.5)

    def test_bad_edits_explain_themselves(self):
        data = synthetic()
        for edits in ({"node.99.x": 1.0}, {"node.1.x": float("nan")}, {"node.1.x": 1e9}, {"header.tag": 1}):
            with self.assertRaises(ValueError):
                formats.apply_edits("ILIN", data, edits)

    def test_empty_member_decodes(self):
        view = formats.describe("ILIN", b"", {"member": "roadrace.ili"})
        self.assertEqual(view["fields"], [])
        self.assertFalse(view["editable"])


@unittest.skipUnless(RETAIL, "retail disc not found (set EOD_DISC)")
class RetailTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import modding_backend as mb
        cls.mb = mb
        cls.members = []
        for archive in sorted(list((DISC / "DATA/TRACKS").glob("*.TRK")) + [DISC / "DATA/RES/GHOSTCAR.RES"]):
            entries = mb.directory(archive)
            for entry in entries:
                if entry["type"] == "ILIN":
                    data, _ = mb.payload(archive, entry["name"], entries, verify_crc=False)
                    cls.members.append((archive, entry["name"], data))

    def test_member_count(self):
        self.assertEqual(len(self.members), EXPECTED_MEMBERS)
        self.assertEqual(sum(1 for m in self.members if not m[2]), EMPTY_MEMBERS)

    def test_all_decode_cover_every_byte_and_round_trip(self):
        for archive, name, data in self.members:
            with self.subTest(member=f"{archive.name}#{name}"):
                view = formats.describe("ILIN", data, {"member": name})
                if data:
                    self.assertEqual(view["warnings"], [])
                self.assertEqual(view["schema"], "eod.ilin/1")
                owner = bytearray(len(data))
                for item in view["fields"] + view["blocks"]:
                    self.assertLessEqual(item["offset"] + item["size"], len(data))
                    for i in range(item["offset"], item["offset"] + item["size"]):
                        owner[i] += 1
                self.assertEqual(set(owner) - {1}, set())
                self.assertEqual(formats.apply_edits("ILIN", data, {}), data)
                if data:
                    self.assertTrue(view["preview"]["points"])
                    ids = [f["id"] for f in view["fields"]]
                    self.assertEqual(len(ids), len(set(ids)))

    def test_one_node_edit_changes_only_its_floats(self):
        archive, name, data = next(m for m in self.members if m[0].name == "ANOKA.TRK" and m[1] == "oval.ild")
        view = ilin.decode(data, {})
        x = next(f for f in view["fields"] if f["id"] == "node.12.x")
        z = next(f for f in view["fields"] if f["id"] == "node.12.z")
        new = formats.apply_edits("ILIN", data, {"node.12.x": x["value"] + 2.0, "node.12.z": z["value"] - 3.0})
        diff = [i for i in range(len(data)) if data[i] != new[i]]
        self.assertEqual(len(new), len(data))
        self.assertTrue(set(diff) <= set(range(x["offset"], x["offset"] + 8)))
        moved = ilin.decode(new, {})["preview"]["points"][12]
        self.assertAlmostEqual(moved[0], x["value"] + 2.0, places=3)
        self.assertAlmostEqual(moved[2], z["value"] - 3.0, places=3)
        only_x = formats.apply_edits("ILIN", data, {"node.12.x": x["value"] + 1.0})
        self.assertLessEqual(sum(a != b for a, b in zip(data, only_x)), 4)

    def test_anoka_points_match_existing_parsers(self):
        import trk
        import world_export
        path = DISC / "DATA/TRACKS/ANOKA.TRK"
        entries = self.mb.directory(path)
        track = dict(path=path, entries=entries)
        line = world_export.racing_line(track, "oval.ild")
        self.assertEqual(world_export.racing_line(track)[:5], world_export.racing_line(track, "bw_oval.ild")[:5])
        data, _ = self.mb.payload(path, "oval.ild", entries, verify_crc=False)
        view = ilin.decode(data, {})
        pts = view["preview"]["points"]
        self.assertTrue(view["preview"]["closed"])
        self.assertEqual(len(pts), 100)
        for point, (x, z) in zip(pts, line):
            self.assertEqual((point[0], point[2]), (x, z))
        # trk.decompile_ild on the same bytes: every node X/Z agrees.
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / "oval.ild"
            copy.write_bytes(data)
            model = trk.decompile_ild(str(copy))
        fields = {f["id"]: f["value"] for f in view["fields"]}
        for i, node in enumerate(model["nodes"]):
            self.assertEqual(fields[f"node.{i}.x"], node[0])
            self.assertEqual(fields[f"node.{i}.z"], node[1])
            self.assertEqual(fields[f"node.{i}.segment_length"], node[6])

    def test_main_line_length_matches_header(self):
        for archive, name, data in self.members:
            if not data or name.endswith(".ili") and archive.suffix == ".RES":
                continue
            with self.subTest(member=f"{archive.name}#{name}"):
                view = ilin.decode(data, {})
                f = {x["id"]: x["value"] for x in view["fields"]}
                total = sum(f[f"node.{i}.segment_length"] for i in range(f["header.main_node_count"]))
                self.assertAlmostEqual(total, f["header.lap_length_a"], delta=0.05)


if __name__ == "__main__":
    unittest.main()
