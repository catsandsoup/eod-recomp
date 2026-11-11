"""LAYT menu layout decoder: synthetic tests always run; retail tests need the disc (read only).

The disc is ~/Library/Application Support/TDEODRecomp/disc or EOD_DISC.
"""
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import formats  # noqa: E402
import modding_backend  # noqa: E402
from formats import layt  # noqa: E402

DISC = Path(os.environ.get("EOD_DISC", "~/Library/Application Support/TDEODRecomp/disc")).expanduser()
RETAIL = (DISC / "DATA/RES/COMMON.RES").is_file()
EXPECTED_LAYOUTS = 200


def record(kind, x=0, y=0, w=32, h=32, s1="", s2="", style=0, group=-1):
    out = bytearray(layt.RECORD)
    struct.pack_into("<I6i", out, 0, kind, style, x, y, 0, w, h)
    out[0x20:0x20 + len(s1)] = s1.encode("latin-1")
    out[0x40:0x40 + len(s2)] = s2.encode("latin-1")
    struct.pack_into("<i", out, 0x88, group)
    return bytes(out)


def build(records, background="dialog"):
    head = bytearray(layt.HEADER_SIZE)
    head[4:4 + len(background)] = background.encode()
    struct.pack_into("<I", head, 0x64, 0x68)
    return bytes(head) + b"".join(records) + bytes(layt.RECORD)


SYNTH = [record(2, 26, 160, 59, 31, "yes", "Generic:ACCEPT", style=3),
         record(28, 155, 85, s1="arial18.fnt", s2="#message", style=12),
         record(33, 10, 20, 100, 50, "Art", "menu_plate.art"),
         record(3, 5, 6, 20, 30, "old", "b_yes.img")]


def coverage(view, size):
    spans = sorted((x["offset"], x["offset"] + x["size"]) for x in view["fields"] + view["blocks"])
    cursor = 0
    for start, stop in spans:
        if start != cursor:
            return False
        cursor = stop
    return cursor == size


def rows_of(view):
    return (view.get("table") or view["preview"])["rows"]


def changed(a, b):
    return [i for i in range(len(a)) if a[i] != b[i]]


class SyntheticTests(unittest.TestCase):
    def test_registered_covered_and_round_trips(self):
        self.assertIs(formats.decoder_for("LAYT"), layt)
        data = build(SYNTH)
        view = formats.describe("LAYT", data, {"member": "COMMON.RES#yesnocan.lyt"})
        self.assertEqual(view["title"], "Menu layout")
        self.assertTrue(coverage(view, len(data)))
        self.assertEqual(formats.apply_edits("LAYT", data, {}), data)
        self.assertEqual(layt.encode(data, {}), data)
        self.assertEqual((view.get("table") or view["preview"])["columns"], ["#", "Type", "Name", "X", "Y", "W", "H", "Image/Font", "Text"])
        self.assertEqual(rows_of(view)[0], [1, "Image button", "yes", 26, 160, 59, 31, "", "Generic:ACCEPT"])
        self.assertEqual(rows_of(view)[1][7:], ["arial18.fnt", "#message"])
        self.assertIn("yes / no / cancel prompt", view["summary"].lower())

    def test_position_edit_changes_only_its_bytes(self):
        data = build(SYNTH)
        out = formats.apply_edits("LAYT", data, {"w0.x": 66, "w2.h": 70})
        self.assertEqual(changed(data, out), [layt.HEADER_SIZE + 8, layt.RECORD * 2 + layt.HEADER_SIZE + 0x18])
        self.assertEqual(struct.unpack_from("<i", out, layt.HEADER_SIZE + 8)[0], 66)
        self.assertTrue(coverage(formats.describe("LAYT", out, {}), len(out)))

    def test_string_edits(self):
        data = build(SYNTH)
        out = formats.apply_edits("LAYT", data, {"w1.s1": "arial16.fnt", "w2.s2": "tab_box.art", "w0.s2": "Generic:BACK"})
        view = formats.describe("LAYT", out, {})
        rows = rows_of(view)
        self.assertEqual((rows[1][7], rows[2][7], rows[0][8]), ("arial16.fnt", "tab_box.art", "Generic:BACK"))
        shorter = formats.apply_edits("LAYT", data, {"w2.s2": "a.art"})
        self.assertEqual(rows_of(formats.describe("LAYT", shorter, {}))[2][7], "a.art")
        self.assertTrue(coverage(formats.describe("LAYT", shorter, {}), len(shorter)))

    def test_rejections_are_plain_sentences(self):
        data = build(SYNTH)
        bad = {"w2.s2": ("x" * 70 + ".art", "at most"), "w1.s1": ("title.png", "file ending"),
               "w0.s1": ("sure", "break the link"), "w1.s2": ("hello", "break the link"),
               "w1.w": (50, "ignores width"), "w3.x": (4, "retired"), "w0.x": (5000, "between"),
               "w0.y": (1.5, "whole number"), "w9.x": (1, "not an editable"), "w0.depth": (1, "not an editable"),
               "w2.s2": ("a\0.art", "control characters")}
        for key, (value, word) in bad.items():
            with self.assertRaises(ValueError, msg=key) as ctx:
                formats.apply_edits("LAYT", data, {key: value})
            self.assertIn(word, str(ctx.exception), key)

    def test_background_edit_and_header_fields(self):
        data = build(SYNTH, "dialog2.img")
        view = formats.describe("LAYT", data, {})
        self.assertEqual(next(f["value"] for f in view["fields"] if f["id"] == "header.background"), "dialog2.img")
        out = formats.apply_edits("LAYT", data, {"header.background": "dialog"})
        self.assertEqual(out[4:20], b"dialog" + bytes(10))
        for bad in ("z" * 40 + ".img", "back.png"):
            with self.assertRaises(ValueError):
                formats.apply_edits("LAYT", data, {"header.background": bad})

    def test_bad_files_fall_back_with_a_warning(self):
        for data in (b"\0" * 10, build(SYNTH)[:-layt.RECORD], build(SYNTH)[:300]):
            view = formats.describe("LAYT", data, {})
            self.assertTrue(view["warnings"])
            self.assertTrue(view["blocks"])

    def test_schematic_is_written_without_a_disc(self):
        with tempfile.TemporaryDirectory() as cache:
            view = formats.describe("LAYT", build(SYNTH), {"member": "x.lyt", "cache": cache})
            self.assertEqual(view["preview"]["kind"], "image")
            self.assertEqual((view["preview"]["width"], view["preview"]["height"]), (640, 480))
            self.assertTrue(Path(view["preview"]["png"]).read_bytes().startswith(b"\x89PNG"))
            self.assertEqual(view["table"]["kind"], "table")


def retail_layouts():
    found = []
    for pattern in ("*.RES", "*.TRK"):
        for path in sorted((DISC / "DATA").rglob(pattern)):
            try:
                entries = modding_backend.directory(path)
            except Exception:
                continue
            found += [(path, e["name"]) for e in entries if e["type"] == "LAYT"]
    return found


@unittest.skipUnless(RETAIL, "retail disc not found (set EOD_DISC)")
class RetailTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.items = retail_layouts()
        cls.cache = tempfile.mkdtemp()

    def test_all_decode_with_full_coverage_and_round_trip(self):
        self.assertEqual(len(self.items), EXPECTED_LAYOUTS)
        for path, name in self.items:
            data = modding_backend.payload(path, name, verify_crc=False)[0]
            view = formats.describe("LAYT", data, {"member": name})
            self.assertEqual(view["schema"], "eod.layt/1", f"{path.name}#{name}: {view['warnings']}")
            self.assertTrue(coverage(view, len(data)), f"{path.name}#{name} leaves bytes uncovered")
            self.assertEqual(formats.apply_edits("LAYT", data, {}), data)
            self.assertEqual(layt.encode(data, {}), data)
            self.assertFalse(view["warnings"], f"{path.name}#{name}: {view['warnings']}")
            self.assertEqual(len(rows_of(view)), sum(f["id"].endswith(".kind") for f in view["fields"]))

    def test_every_editable_field_edit_changes_only_its_own_bytes(self):
        for path, name in self.items[::7]:
            data = modding_backend.payload(path, name, verify_crc=False)[0]
            view = formats.describe("LAYT", data, {"member": name})
            for f in view["fields"]:
                if not f["editable"] or f["type"] != "i32":
                    continue
                new = f["value"] + 1 if f["value"] < f["max"] else f["value"] - 1
                out = formats.apply_edits("LAYT", data, {f["id"]: new})
                self.assertTrue(set(changed(data, out)) <= set(range(f["offset"], f["offset"] + 4)), f"{name} {f['id']}")
                self.assertEqual(struct.unpack_from("<i", out, f["offset"])[0], new)

    def test_yes_no_cancel_prompt(self):
        data = modding_backend.payload(DISC / "DATA/RES/COMMON.RES", "yesnocan.lyt", verify_crc=False)[0]
        self.assertEqual(len(data), 1952)
        view = formats.describe("LAYT", data, {"member": "yesnocan.lyt", "archive": "DATA/RES/COMMON.RES",
                                               "disc": str(DISC), "cache": self.cache})
        rows = rows_of(view)
        self.assertEqual([r[2] for r in rows if r[2]], ["yes", "no", "cancel"])
        self.assertEqual([r[8] for r in rows if r[8]], ["#message", "#title"])
        self.assertEqual(view["preview"]["kind"], "image")
        out = formats.apply_edits("LAYT", data, {"w3.y": 30})
        self.assertEqual(len(changed(data, out)), 1)

    def test_main_menu_move_button(self):
        data = modding_backend.payload(DISC / "DATA/RES/UIACTION.RES", "main.lyt", verify_crc=False)[0]
        view = formats.describe("LAYT", data, {"member": "main.lyt"})
        career = next(r for r in rows_of(view) if r[2] == "career")
        out = formats.apply_edits("LAYT", data, {f"w{career[0] - 1}.x": career[3] - 40})
        after = formats.describe("LAYT", out, {"member": "main.lyt"})
        moved = next(r for r in rows_of(after) if r[2] == "career")
        self.assertEqual(moved[3], career[3] - 40)
        at = layt.HEADER_SIZE + layt.RECORD * (career[0] - 1) + 8
        self.assertTrue(changed(data, out) and set(changed(data, out)) <= set(range(at, at + 4)))


if __name__ == "__main__":
    unittest.main()
