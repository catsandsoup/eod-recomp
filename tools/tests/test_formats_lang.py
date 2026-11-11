"""LANG game-text decoder: synthetic tests always run; retail tests need the disc (read only).

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
from formats import lang  # noqa: E402

DISC = Path(os.environ.get("EOD_DISC", "~/Library/Application Support/TDEODRecomp/disc")).expanduser()
MEMBERS = (("DATA/RES/GENESIS.RES", "english.lng", 325), ("DATA/RES/LOCALE.RES", "usenglish_lang.lng", 877))
RETAIL = all((DISC / archive).is_file() for archive, _, _ in MEMBERS)


def build(pairs, order=None):
    """Synthetic LANG: header, index, pool. `order` permutes pool order of the 2N strings."""
    n = len(pairs)
    strings = [s for pair in pairs for s in pair]
    order = order or list(range(len(strings)))
    pos, at = {}, 0x44 + 8 * n
    for idx in order:
        pos[idx] = at
        at += len(strings[idx].encode("latin-1")) + 1
    out = bytearray(b"English\0" + struct.pack("<I", 0x7E4) + bytes(0x34) + struct.pack("<I", n))
    for i in range(n):
        out += struct.pack("<II", pos[2 * i], pos[2 * i + 1])
    for idx in order:
        out += strings[idx].encode("latin-1") + b"\0"
    return bytes(out)


PAIRS = [("Main_Menu:Career", "CAREER"), ("Main_Menu:SingleRace", "QUICK\nRACE"),
         ("Career:Credits:RacePrize", "Race Prize #%d"), ("Units:Mph", "MPH")]


def text_of(view, key):
    return next(f["value"] for f in view["fields"] if f["id"].startswith("string.") and f["label"] == key)


def coverage(view, size):
    spans = sorted((x["offset"], x["offset"] + x["size"]) for x in view["fields"] + view["blocks"])
    cursor = 0
    for start, stop in spans:
        if start != cursor:
            return False
        cursor = stop
    return cursor == size


def retail_payload(archive, member):
    return modding_backend.payload(DISC / archive, member, verify_crc=False)[0]


class SyntheticTests(unittest.TestCase):
    def test_registered_covered_and_round_trips(self):
        self.assertIs(formats.decoder_for("LANG"), lang)
        data = build(PAIRS, order=[1, 0, 3, 2, 5, 4, 7, 6])  # pool order differs from index order
        view = formats.describe("LANG", data, {"member": "usenglish_lang.lng"})
        self.assertEqual(view["title"], "Game text")
        self.assertTrue(coverage(view, len(data)))
        self.assertEqual(formats.apply_edits("LANG", data, {}), data)
        self.assertEqual(lang.encode(data, {}), data)
        self.assertEqual(view["preview"]["kind"], "table")
        self.assertEqual(view["preview"]["rows"][1][2], "QUICK\\nRACE")

    def test_shorter_and_longer_edit(self):
        data = build(PAIRS)
        view = formats.describe("LANG", data, {})
        ids = {f["label"]: f["id"] for f in view["fields"] if f["id"].startswith("string.")}
        for new in ("GO", "CAREER MODE WITH A MUCH LONGER LABEL"):
            out = formats.apply_edits("LANG", data, {ids["Main_Menu:Career"]: new})
            after = formats.describe("LANG", out, {})
            self.assertTrue(coverage(after, len(out)))
            for key, old in PAIRS:
                self.assertEqual(text_of(after, key), new if key == "Main_Menu:Career" else dict(PAIRS)[key])

    def test_placeholder_guard(self):
        data = build(PAIRS)
        field = "string.2"
        self.assertEqual(lang.specifiers("Race Prize #%d"), ["%d"])
        out = formats.apply_edits("LANG", data, {field: "Prize number %d!"})
        self.assertIn(b"Prize number %d!", out)
        for bad in ("Race Prize", "Race Prize #%s", "%d %d", "#%x"):
            with self.assertRaises(ValueError, msg=bad) as ctx:
                formats.apply_edits("LANG", data, {field: bad})
            self.assertIn("placeholders", str(ctx.exception))

    def test_encoding_and_controls(self):
        data = build(PAIRS)
        formats.apply_edits("LANG", data, {"string.3": "Km/h ö"})
        for bad in ("日本", "tab\there", "a\0b"):
            with self.assertRaises(ValueError):
                formats.apply_edits("LANG", data, {"string.3": bad})
        with self.assertRaises(ValueError):
            formats.apply_edits("LANG", data, {"string.99": "x"})

    def test_bad_layout_raises(self):
        with self.assertRaises(ValueError):
            lang.decode(b"English\0" + bytes(100), {})


@unittest.skipUnless(RETAIL, "retail disc not found (set EOD_DISC)")
class RetailTests(unittest.TestCase):
    def test_both_members_covered_and_byte_exact(self):
        for archive, member, count in MEMBERS:
            data = retail_payload(archive, member)
            view = formats.describe("LANG", data, {"member": member})
            self.assertEqual(view["warnings"], [], member)
            self.assertEqual(view["schema"], "eod.lang/1", member)
            self.assertEqual(len([f for f in view["fields"] if f["id"].startswith("string.")]), count)
            self.assertTrue(coverage(view, len(data)), member)
            self.assertEqual(lang.encode(data, {}), data, member)
            self.assertEqual(formats.apply_edits("LANG", data, {}), data)

    def test_edit_shorter_and_longer_keeps_everything_else(self):
        data = retail_payload("DATA/RES/LOCALE.RES", "usenglish_lang.lng")
        before = formats.describe("LANG", data, {"member": "usenglish_lang.lng"})
        strings = {f["id"]: f["value"] for f in before["fields"] if f["id"].startswith("string.")}
        target = next(f["id"] for f in before["fields"] if f["label"] == "Main_Menu:Career")
        self.assertEqual(strings[target], "CAREER")
        for new in ("GO", "CAREER MODE " * 8):
            out = formats.apply_edits("LANG", data, {target: new})
            after = formats.describe("LANG", out, {"member": "usenglish_lang.lng"})
            self.assertTrue(coverage(after, len(out)))
            now = {f["id"]: f["value"] for f in after["fields"] if f["id"].startswith("string.")}
            self.assertEqual(now.pop(target), new)
            self.assertEqual(now, {k: v for k, v in strings.items() if k != target})
            self.assertEqual([f["label"] for f in after["fields"]], [f["label"] for f in before["fields"]])
            self.assertEqual(out[:0x40], data[:0x40])

    def test_retail_format_specifiers_and_guard(self):
        data = retail_payload("DATA/RES/LOCALE.RES", "usenglish_lang.lng")
        view = formats.describe("LANG", data, {})
        with_spec = {f["label"]: f["id"] for f in view["fields"]
                     if f["id"].startswith("string.") and lang.specifiers(f["value"])}
        self.assertEqual(sorted(with_spec), ["Career:Credits:RacePrize", "Career:Credits:SeasonPrize"])
        with self.assertRaises(ValueError):
            formats.apply_edits("LANG", data, {with_spec["Career:Credits:RacePrize"]: "Race Prize"})


if __name__ == "__main__":
    unittest.main()
