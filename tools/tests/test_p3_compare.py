"""Regression tests for the Classic parity gate (requires numpy and Pillow)."""
import contextlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import p3_compare


class ClassicParityTests(unittest.TestCase):
    def run_comparison(self, color, *, exact):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dump = root / "frame.gsd"
            scanout = Image.new("RGB", (8, 8), color)
            scanout.save(root / "frame_00100.png")
            scanout.save(dump.with_suffix(".gsd.live.png"))
            dump.with_suffix(".gsd.live.vblank").write_text("106\n")
            if exact:
                # A neighboring EE screenshot matches perfectly. It must not
                # replace the image captured on the verified GS boundary.
                Image.new("RGB", (8, 8)).save(root / "frame_00100.png")

            def replay(_exe, _ini, path, env):
                output = Path(path + ".ppm")
                Image.new("RGB", (8, 8)).save(output)
                if "EOD_GS_REPLAY_ZSHOT" in env:
                    Path(env["EOD_GS_REPLAY_ZSHOT"]).write_bytes(b"Z32 8 8\n" + bytes(256))
                if exact:
                    self.assertEqual(env["EOD_GS_REPLAY_MEASURE_AT"], "0")
                    self.assertEqual(env["EOD_GS_REPLAY_STOP_AT"], "106")
                fake_replay.tris = 0
                return str(output)

            args = ["p3_compare", "--live", str(root), "--dump", str(dump),
                    "--vblanks", "100", "--ini", "software.ini", "--min-tri-px", "0",
                    "--require-pass", "--search", "0..0"]
            if exact:
                args.append("--exact-scanout")
            with patch.object(sys, "argv", args), patch.object(p3_compare, "replay", side_effect=replay) as fake_replay:
                with contextlib.redirect_stdout(io.StringIO()):
                    return p3_compare.main()

    def test_p3_threshold_is_per_channel(self):
        self.assertEqual(self.run_comparison((0, 0, 2), exact=False), 1)
        self.assertEqual(self.run_comparison((0, 0, 1), exact=False), 0)

    def test_exact_mode_uses_the_terminal_scanout(self):
        self.assertEqual(self.run_comparison((3, 0, 0), exact=True), 1)
        self.assertEqual(self.run_comparison((0, 0, 0), exact=True), 0)


class HardwareParityTests(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.root = Path(work.name)
        original = Image.new("RGB", (10, 8), (1, 2, 3))
        original.paste((8, 30, 90), (1, 0, 9, 8))
        original.save(self.root / "console.png")
        Image.new("RGB", (8, 8), (8, 30, 93)).save(self.root / "native.png")
        self.pair = dict(id="afton-jump-01", scene="Afton Jump Race", ps2="console.png",
                         native="native.png", ps2_field="recording A, field 302",
                         native_vblank=456, sync="identical race state and camera",
                         ps2_crop=[1, 0, 9, 8])
        self.manifest = dict(source="synthetic fixture", capture_chain="8-bit RGB fixture",
                             native_build="fixture build", pairs=[self.pair])
        self.manifest_file = self.root / "capture.json"
        self.output = self.root / "report"

    def compare(self):
        from p3_hardware_compare import compare

        self.manifest_file.write_text(json.dumps(self.manifest))
        compare(self.manifest_file, self.output)

    def test_preserves_differences_and_source_identity(self):
        self.compare()
        report = json.loads((self.output / "report.json").read_text())
        result, = report["pairs"]
        self.assertEqual(result["metrics"]["mae_rgb"], [0.0, 0.0, 3.0])
        self.assertEqual(result["metrics"]["p99"], 3.0)
        self.assertEqual(result["ps2_crop"], [1, 0, 9, 8])
        self.assertEqual(result["native_vblank"], 456)
        self.assertEqual(len(result["ps2_sha256"]), 64)
        self.assertEqual(result["ps2_source_size"], [10, 8])
        with Image.open(self.output / "afton-jump-01-difference.png") as difference:
            self.assertEqual(difference.getpixel((0, 0)), (0, 0, 3))
        with Image.open(self.output / "afton-jump-01-comparison.png") as sheet:
            self.assertEqual(sheet.size, (24, 8))

    def test_never_resizes_a_mismatched_frame(self):
        del self.pair["ps2_crop"]
        with self.assertRaisesRegex(ValueError, "same dimensions"):
            self.compare()
        self.assertFalse(self.output.exists())

    def test_cannot_omit_synchronization_evidence(self):
        del self.pair["sync"]
        with self.assertRaisesRegex(ValueError, "sync"):
            self.compare()
        self.assertFalse(self.output.exists())

    def test_rejects_automatic_16_bit_to_8_bit_conversion(self):
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", 10, 8, 16, 2, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(bytes(61 * 8))) + chunk(b"IEND", b"")
        (self.root / "console.png").write_bytes(png)
        with self.assertRaisesRegex(ValueError, "8-bit RGB"):
            self.compare()


if __name__ == "__main__":
    unittest.main()
