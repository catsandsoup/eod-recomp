"""Format registry contract, generic view coverage and retail inspect() guarantees.

Retail checks are opt-in: EOD_TEST_RETAIL_FORMATS=1 (disc: EOD_TEST_DISC).
"""
import contextlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import formats
import modding_backend as backend
from formats import generic
from rst0 import pack_archive

RETAIL = Path(os.environ.get("EOD_TEST_DISC", str(Path.home() / "Library/Application Support/TDEODRecomp/disc")))
RETAIL_ON = os.environ.get("EOD_TEST_RETAIL_FORMATS") == "1" and (RETAIL / "DATA").is_dir()
NATIVE_KINDS = {"TEX", "VALT", "MODL"}


def covered(view, size):
    spans = sorted([(f["offset"], f["offset"] + f["size"]) for f in view["fields"] if "offset" in f] +
                   [(b["offset"], b["offset"] + b["size"]) for b in view["blocks"]])
    cursor = 0
    for start, end in spans:
        if start > cursor:
            return False
        cursor = max(cursor, end)
    return cursor >= size


class RegistryTest(unittest.TestCase):
    def test_registry_loads_and_claims_are_unique(self):
        for fourcc in formats.supported():
            self.assertIsNotNone(formats.decoder_for(fourcc))

    def test_generic_view_covers_every_byte_and_is_not_hex(self):
        data = bytes(range(256)) * 3 + b"Hello world, plain text here" + bytes(40)
        view = formats.describe("ZZZZ", data, {})
        self.assertTrue(covered(view, len(data)))
        self.assertEqual(view["schema"], "eod.generic/1")
        self.assertTrue(any(b["name"].startswith("Unknown block") for b in view["blocks"]))
        self.assertEqual(formats.describe("ZZZZ", b"", {})["blocks"], [])
        self.assertFalse(generic.decode(b"!!!! %%%% ....", {})["blocks"][0].get("known"))

    def test_untouched_types_are_read_only(self):
        with self.assertRaises(ValueError):
            formats.apply_edits("ZZZZ", b"abc", {"x": 1})

    def test_unknown_member_inspects_as_format_and_saves_edits(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            stage = root / "stage"
            stage.mkdir()
            (stage / "a.zzz").write_bytes(b"Some readable text" + bytes(30))
            (stage / "manifest.json").write_text(
                '{"version":6,"entries":[{"name":"a.zzz","fourcc":"ZZZZ","version":1,"is_extractable":true}]}')
            (root / "disc/DATA/CAR").mkdir(parents=True)
            with contextlib.redirect_stdout(io.StringIO()):
                pack_archive(str(stage), str(root / "disc/DATA/CAR/X.CAR"))
            request = dict(action="inspect", disc=str(root / "disc"), project=str(root / "p/p.eodproject"),
                           archive="DATA/CAR/X.CAR", asset="a.zzz")
            result = backend.dispatch(request)
            self.assertEqual(result["kind"], "format")
            self.assertIn("hex", result)
            self.assertEqual(result["warnings"], [])
            with self.assertRaises(ValueError) as caught:
                backend.dispatch(dict(request, action="save_format", formatEdits={"x": 1}))
            self.assertIn("read-only", str(caught.exception))
            self.assertNotIn("Traceback", str(caught.exception))

    def test_format_edits_persist_and_rebuild_with_fixed_crc(self):
        import types
        fake = types.ModuleType("fake_zzzz")
        fake.FOURCCS = ("ZZZZ",)
        fake.decode = lambda data, ctx: dict(
            schema="eod.fake/1", title="Fake", summary="s", confidence="high", editable=True, preview=None,
            fields=[dict(id="n", label="Number", type="u8", value=data[0], offset=0, size=1, editable=True,
                         min=0, max=200, confidence="high")],
            blocks=[dict(name="Unknown block 1", offset=1, size=len(data) - 1, known=False)])

        def encode(data, edits):
            if not 0 <= edits["n"] <= 200:
                raise ValueError("Number must be between 0 and 200.")
            return bytes([edits["n"]]) + data[1:]
        fake.encode = encode
        formats._load()["ZZZZ"] = fake
        self.addCleanup(formats._load().pop, "ZZZZ")
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            stage = root / "stage"
            stage.mkdir()
            (stage / "a.zzz").write_bytes(bytes([5]) + bytes(30))
            (stage / "b.zzz").write_bytes(bytes(range(50)))
            (stage / "manifest.json").write_text(
                '{"version":6,"entries":[{"name":"a.zzz","fourcc":"ZZZZ","version":1,"is_extractable":true},'
                '{"name":"b.zzz","fourcc":"ZZZZ","version":1,"is_extractable":true}]}')
            (root / "disc/DATA/CAR").mkdir(parents=True)
            with contextlib.redirect_stdout(io.StringIO()):
                pack_archive(str(stage), str(root / "disc/DATA/CAR/X.CAR"))
            request = dict(disc=str(root / "disc"), project=str(root / "p/p.eodproject"),
                           archive="DATA/CAR/X.CAR", asset="a.zzz")
            with self.assertRaises(ValueError):
                backend.dispatch(dict(request, action="save_format", formatEdits={"n": 201}))
            saved = backend.dispatch(dict(request, action="save_format", formatEdits={"n": 9}))
            built = Path(saved["archivePath"])
            entries = backend.directory(built)
            data, entry = backend.payload(built, "a.zzz", entries)  # verifies the new CRC
            self.assertEqual(data[0], 9)
            self.assertEqual(backend.payload(built, "b.zzz", entries)[0], bytes(range(50)))
            reopened = backend.dispatch(dict(request, action="inspect"))
            self.assertEqual(reopened["format"]["fields"][0]["value"], 9)
            self.assertTrue(reopened["edited"])
            self.assertEqual(backend.dispatch(dict(request, action="validate"))["archives"], 1)


@unittest.skipUnless(RETAIL_ON, "Set EOD_TEST_RETAIL_FORMATS=1 with a retail disc to run")
class RetailTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.project = str(Path(cls.temp.name) / "p/p.eodproject")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def inspect(self, archive, asset):
        return backend.dispatch(dict(action="inspect", disc=str(RETAIL), project=self.project,
                                     archive=archive, asset=asset))

    def test_crc_mismatch_opens_with_notice(self):
        result = self.inspect("DATA/TRACKS/HOMEBASE.TRK", "opt6.ili")
        self.assertEqual(result["kind"], "format")
        self.assertIn(backend.CRC_WARNING, result["warnings"])
        self.assertIn(backend.CRC_WARNING, result["format"]["warnings"])

    def test_no_member_is_raw_binary_and_generic_views_cover_every_byte(self):
        total = mismatches = 0
        for path in sorted((RETAIL / "DATA").rglob("*")):
            if path.suffix.lower() not in (".car", ".res", ".trk"):
                continue
            relative = path.relative_to(RETAIL).as_posix()
            for entry in backend.directory(path):
                result = self.inspect(relative, entry["name"])
                total += 1
                self.assertNotEqual(result["kind"], "binary", relative + "#" + entry["name"])
                mismatches += backend.CRC_WARNING in result["warnings"]
                if result["kind"] == "format" and result["format"]["schema"] == "eod.generic/1":
                    self.assertTrue(covered(result["format"], result["size"]), relative + "#" + entry["name"])
        self.assertEqual(total, 7329)
        self.assertEqual(mismatches, 21)


if __name__ == "__main__":
    unittest.main()
