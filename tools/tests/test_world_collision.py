#!/usr/bin/env python3
"""Contracts for tools/world_collision.py (SOBL/SCOL parsing and safe write-back).

Retail checks are opt-in: EOD_TEST_RETAIL_MODELS=1 (disc from EOD_DISC or
EOD_DISC_DATA, default ~/Library/Application Support/TDEODRecomp/disc).
"""
import math
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import world_collision as wc  # noqa: E402
from rst0 import eod_crc  # noqa: E402

RETAIL = os.environ.get("EOD_TEST_RETAIL_MODELS") == "1"

# Exact retail census: SOBL records, BOX, TUBE, index entries, quadtree nodes, SCOL triangles.
RETAIL_COUNTS = {
    "AFTON.TRK": (1245, 1203, 42, 2635, 3477, 9519),
    "ANOKA.TRK": (713, 709, 4, 1540, 1537, 4930),
    "BAXTER.TRK": (995, 492, 503, 1998, 1657, 5455),
    "BEMIDJI.TRK": (626, 597, 29, 1411, 1573, 4622),
    "DENNISON.TRK": (903, 750, 153, 1923, 2125, 6600),
    "DUNDAS.TRK": (764, 747, 17, 1676, 1705, 4834),
    "EAGAN.TRK": (783, 643, 140, 1582, 1905, 5446),
    "EMPTYT_1.TRK": (0, 0, 0, 0, 1, 2),
    "GILROY.TRK": (1268, 987, 281, 2512, 2885, 6148),
    "HASTINGS.TRK": (851, 616, 235, 1875, 2253, 4938),
    "HOMEBASE.TRK": (1520, 1446, 74, 2798, 2965, 11434),
    "KENYON.TRK": (1102, 842, 260, 2134, 2209, 7098),
    "MANKATO.TRK": (1703, 1423, 280, 3536, 3433, 9167),
    "SPRING_1.TRK": (926, 926, 0, 1770, 2525, 11416),
    "WELCH.TRK": (1457, 1457, 0, 2983, 3669, 7021),
}


def rows_from_yaw(deg, tilt=0.0):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    ct, st = math.cos(math.radians(tilt)), math.sin(math.radians(tilt))
    x = [c, 0.0, -s]
    y = [s * st, ct, c * st]
    z = [s * ct, -st, c * ct]
    return x + y + z


def sobl_box(rows, centre, half, flags=3, surface=110):
    rec = bytearray(wc.SOBL_RECORD)
    struct.pack_into("<9f", rec, 0, *rows)
    struct.pack_into("<3f", rec, 0x24, *centre)
    struct.pack_into("<iII", rec, 0x30, -1, surface, 0x424F5820)
    norm = math.sqrt(sum(v * v for v in half)) + wc.SPHERE_PAD
    struct.pack_into("<4fI", rec, 0x60, *half, norm, flags)
    return bytes(rec)


def sobl_tube(centre, radius, half_length):
    rec = bytearray(wc.SOBL_RECORD)
    struct.pack_into("<9f", rec, 0, 1, 0, 0, 0, 0, 1, 0, -1, 0)
    struct.pack_into("<3f", rec, 0x24, *centre)
    struct.pack_into("<iII", rec, 0x30, -1, 0, 0x54554245)
    struct.pack_into("<2f", rec, 0x60, radius, half_length)
    return bytes(rec)


def synthetic_sobl():
    records = [sobl_box(rows_from_yaw(30), (5.0, 1.0, 5.0), (0.05, 15.0, 4.0)),
               sobl_box(rows_from_yaw(0, tilt=45), (2.0, 1.0, 2.0), (1.0, 2.0, 1.0)),
               sobl_tube((8.0, 2.5, 8.0), 0.3, 2.5)]
    index = [0, 1, 2]
    out = struct.pack("<5I", len(records), len(index), 0, 0, 0) + b"".join(records)
    out += struct.pack("<%dH" % len(index), *index)
    out += struct.pack("<4H", 0, 3, 0, 0)
    return out


def synthetic_scol():
    k = 32767
    tris = [[(0, k, 0.0), (k, 0, 0.0), (-k, -k, 10.0)],        # (0,0) (10,0) (0,10)
            [(0, -k, 10.0), (-k, 0, 10.0), (k, k, -10.0)]]     # (10,10) (0,10) (10,0)
    t1 = b""
    for edges in tris:
        t1 += b"".join(struct.pack("<hhf", *e) for e in edges) + struct.pack("<3hH", -1, -1, -1, 0)
    t2 = b"".join(struct.pack("<4fhBB", 0.0, 1.0, 0.0, 0.0, 6, 255, 255) for _ in tris)
    return struct.pack("<3I", len(tris), 12, 12 + len(t1)) + t1 + t2


def build_archive(members):
    """members: list of (name, fourcc, version, payload)."""
    dir_offset, count = 0x80, len(members)
    pos = dir_offset + count * 68
    layout = []
    for name, fourcc, version, payload in members:
        pos = (pos + 0x7F) & ~0x7F
        layout.append((name, fourcc, version, payload, pos))
        pos += len(payload)
    total = pos
    head = struct.pack("<4sIIIIIIII", b"0TSR", 0x28, 6, total, 0, dir_offset, count, total - dir_offset, 0)
    data = bytearray(head + struct.pack("<I", eod_crc(head)))
    data += b"\0" * (dir_offset - len(data))
    for name, fourcc, version, payload, start in layout:
        data += name.encode().ljust(32, b"\0") + fourcc.ljust(4)[::-1].encode()
        data += struct.pack("<IIII4I", version, len(payload), start - dir_offset, eod_crc(payload), 0, 0, 0, 0)
    for name, fourcc, version, payload, start in layout:
        data += b"\0" * (start - len(data)) + payload
    return bytes(data)


def synthetic_archive():
    return build_archive([("track.sol", "SOBL", 3, synthetic_sobl()), ("track.bsc", "SCOL", 4, synthetic_scol())])


def changed_offsets(a, b):
    assert len(a) == len(b)
    return {i for i in range(len(a)) if a[i] != b[i]}


class SyntheticContracts(unittest.TestCase):
    def test_parse_layouts(self):
        sobl = wc.parse_sobl(synthetic_sobl())
        self.assertEqual([r["kind"] for r in sobl["records"]], ["box", "box", "tube"])
        self.assertEqual(sobl["records"][0]["surface_name"], "wall")
        self.assertAlmostEqual(sobl["records"][0]["half_extents"][1], 15.0)
        lo, hi = wc.vertical_span(sobl["records"][0])
        self.assertAlmostEqual(hi - lo, 30.0, places=4)
        scol = wc.parse_scol(synthetic_scol())
        verts = sorted((round(v[0], 3), round(v[2], 3)) for v in scol["tris"][0]["verts"])
        self.assertEqual(verts, [(0.0, 0.0), (0.0, 10.0), (10.0, 0.0)])
        terrain = wc.Terrain(scol)
        g = terrain.ground(7.0, 6.0)
        self.assertTrue(g["inside"])
        self.assertEqual(g["tri"], 1)
        self.assertAlmostEqual(g["y"], 0.0)

    def test_corrupt_sizes_are_rejected(self):
        with self.assertRaises(ValueError):
            wc.parse_sobl(synthetic_sobl()[:-3])
        with self.assertRaises(ValueError):
            wc.parse_scol(synthetic_scol() + b"\0")

    def test_noop_write_back_is_byte_identical(self):
        source = synthetic_archive()
        new, _ = wc.apply_box_edits(source, [(0, "ry", 15.0)])
        self.assertEqual(new, source)

    def test_single_edit_changes_only_its_fields_and_crc(self):
        source = synthetic_archive()
        entries = wc.read_directory(source)
        sol = next(e for e in entries if e["name"] == "track.sol")
        new, report = wc.apply_box_edits(source, [(0, "top-above-ground", 1.5)])
        rec = sol["offset"] + wc.SOBL_HEADER
        allowed = set(range(rec + 0x24, rec + 0x30)) | set(range(rec + 0x64, rec + 0x68)) \
            | set(range(rec + 0x6C, rec + 0x70)) | set(range(sol["crc_field"], sol["crc_field"] + 4))
        changed = changed_offsets(source, new)
        self.assertTrue(changed)
        self.assertLessEqual(changed, allowed)
        edited = wc.parse_sobl(wc.member(new, wc.read_directory(new), "track.sol")[0])["records"][0]
        before = wc.parse_sobl(synthetic_sobl())["records"][0]
        self.assertAlmostEqual(wc.vertical_span(edited)[1], 1.5, places=4)
        # bottom face kept: new box lies inside the original box
        self.assertAlmostEqual(wc.vertical_span(edited)[0], wc.vertical_span(before)[0], places=4)
        rx, ry, rz = edited["half_extents"]
        self.assertAlmostEqual(edited["sphere_radius"], math.sqrt(rx * rx + ry * ry + rz * rz) + 0.2, places=5)
        self.assertEqual(report[0]["record"], 0)

    def test_refusals(self):
        source = synthetic_archive()
        for edit, needle in (((0, "rx", 0.01), "unverified"), ((0, "ry", 16.0), "only shrinking"),
                             ((1, "ry", 1.0), "tilted"), ((2, "ry", 1.0), "TUBE"),
                             ((0, "ry", 0.0), "< 0.01"), ((0, "bound", 1.0), "unverified")):
            with self.subTest(edit=edit):
                with self.assertRaisesRegex(ValueError, needle):
                    wc.apply_box_edits(source, [edit])

    def test_cli_never_writes_into_disc(self):
        with tempfile.TemporaryDirectory() as tmp:
            disc = Path(tmp) / "disc"
            (disc / "DATA/TRACKS").mkdir(parents=True)
            (disc / "DATA/TRACKS/ANOKA.TRK").write_bytes(synthetic_archive())
            with self.assertRaises(SystemExit):
                wc.main(["--disc", str(disc), "set-limit", "Anoka", "--records", "0", "--field", "ry",
                         "--value", "2", "--out", str(disc / "DATA/TRACKS/OUT.TRK")])
            out = Path(tmp) / "overlay/ANOKA.TRK"
            wc.main(["--disc", str(disc), "set-limit", "Anoka", "--records", "0", "--field", "ry",
                     "--value", "2", "--out", str(out)])
            self.assertEqual(len(out.read_bytes()), len(synthetic_archive()))
            self.assertEqual((disc / "DATA/TRACKS/ANOKA.TRK").read_bytes(), synthetic_archive())


@unittest.skipUnless(RETAIL, "Opt in to owner-supplied retail tracks")
class RetailContracts(unittest.TestCase):
    disc = wc.default_disc()

    def test_all_tracks_parse_with_exact_counts(self):
        seen = {}
        for row in wc.census(self.disc):
            seen[row["archive"]] = (row["sobl_records"], row["shapes"].get("BOX", 0), row["shapes"].get("TUBE", 0),
                                    row["index_entries"], row["quadtree_nodes"], row["scol_triangles"])
        self.assertEqual(seen, RETAIL_COUNTS)

    def test_retail_invariants(self):
        for path in sorted((Path(self.disc) / "DATA/TRACKS").glob("*.TRK")):
            with self.subTest(track=path.name):
                track = wc.load_track(path, self.disc)
                for rec in track["sobl"]["records"]:
                    if rec["kind"] == "box":
                        rx, ry, rz = rec["half_extents"]
                        self.assertLess(abs(rec["sphere_radius"] - math.sqrt(rx * rx + ry * ry + rz * rz) - 0.2), 1e-4)
                for t in track["scol"]["tris"]:
                    cx = sum(v[0] for v in t["verts"]) / 3
                    cz = sum(v[2] for v in t["verts"]) / 3
                    self.assertGreaterEqual(wc.Terrain.inside(t, cx, cz), -1e-3)

    def test_anoka_noop_and_single_edit(self):
        path = wc.resolve_track("Anoka", self.disc)
        source = path.read_bytes()
        new, _ = wc.apply_box_edits(source, [(362, "ry", 15.0)])
        self.assertEqual(new, source)
        new, report = wc.apply_box_edits(source, [(362, "top-above-ground", 1.5)])
        entries = wc.read_directory(source)
        sol = next(e for e in entries if e["name"] == "track.sol")
        rec = sol["offset"] + wc.SOBL_HEADER + 362 * wc.SOBL_RECORD
        allowed = set(range(rec + 0x24, rec + 0x30)) | set(range(rec + 0x64, rec + 0x68)) \
            | set(range(rec + 0x6C, rec + 0x70)) | set(range(sol["crc_field"], sol["crc_field"] + 4))
        changed = changed_offsets(source, new)
        self.assertTrue(changed)
        self.assertLessEqual(changed, allowed)
        wc.member(new, wc.read_directory(new), "track.sol")  # CRC verifies
        self.assertLess(report[0]["after"]["top"], report[0]["before"]["top"] - 10)


if __name__ == "__main__":
    unittest.main()
