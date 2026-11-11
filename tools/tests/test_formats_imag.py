"""IMAG interface-image decoder: synthetic tests always run; retail tests need the disc (read only).

The disc is ~/Library/Application Support/TDEODRecomp/disc or EOD_DISC. Census of IMAG members comes
from scanning every archive directory, so no external census file is needed.
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
from formats import imag  # noqa: E402
from tex import read_png, write_png  # noqa: E402

DISC = Path(os.environ.get("EOD_DISC", "~/Library/Application Support/TDEODRecomp/disc")).expanduser()
RETAIL = (DISC / "DATA/RES/COMMON.RES").is_file()


def build(width, height, frames):
    """Synthetic IMAG. frames: list of (palette list of 256 RGBA tuples, index bytes)."""
    n = len(frames)
    first = (0x20 + 8 * n + 63) // 64 * 64
    stride = 1024 + width * height
    offsets = [first + i * stride for i in range(n)]
    out = bytearray(first)
    struct.pack_into("<4I", out, 0, width, height, n, 4)
    struct.pack_into("<I", out, 0x10, 0x40E401)
    struct.pack_into("<I", out, 0x14, n * stride)
    struct.pack_into(f"<{n}I", out, 0x20, *offsets)
    struct.pack_into(f"<{n}I", out, 0x20 + 4 * n, *offsets)
    for pal, idx in frames:
        out += b"".join(bytes(c) for c in pal) + bytes(idx)
    return bytes(out)


def pal(*colours):
    return list(colours) + [(0, 0, 0, 0)] * (256 - len(colours))


def coverage(view, size):
    spans = sorted((x["offset"], x["offset"] + x["size"]) for x in view["fields"] + view["blocks"])
    cursor = 0
    for start, stop in spans:
        if start != cursor:
            return False
        cursor = stop
    return cursor == size


def write_rgba(path, w, h, pixels):
    write_png(str(path), w, h, pixels)


class SyntheticTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)

    def sample(self):
        return build(4, 2, [(pal((255, 0, 0, 255), (0, 255, 0, 128)), [0, 1, 0, 1, 1, 0, 1, 0]),
                            (pal((0, 0, 255, 255)), [0] * 8)])

    def test_decode_covers_every_byte_and_previews(self):
        data = self.sample()
        view = formats.describe("IMAG", data, {"cache": str(self.dir)})
        self.assertEqual(view["title"], "Interface image")
        self.assertEqual(view["warnings"], [])
        self.assertTrue(coverage(view, len(data)))
        w, h, px = read_png(view["preview"]["png"])
        self.assertEqual((w, h), (4, 2))
        self.assertEqual(px[:8], bytes((255, 0, 0, 255, 0, 255, 0, 128)))
        self.assertEqual(len(view["preview"]["frames"]), 2)

    def test_unedited_round_trip(self):
        data = self.sample()
        view = formats.describe("IMAG", data, {"cache": str(self.dir)})
        self.assertEqual(formats.apply_edits("IMAG", data, {}), data)
        self.assertEqual(imag.encode(data, {"image.png": view["preview"]["png"]}), data)

    def test_replace_image(self):
        data = self.sample()
        red, blue = bytes((255, 0, 0, 255)), bytes((0, 0, 255, 255))
        path = self.dir / "new.png"
        write_rgba(path, 4, 2, (red + blue) * 4)
        out = imag.encode(data, {"image.png": str(path)})
        self.assertEqual(len(out), len(data))
        view = formats.describe("IMAG", out, {"cache": str(self.dir / "c")})
        self.assertEqual(read_png(view["preview"]["png"])[2], (red + blue) * 4)
        # Second frame untouched.
        self.assertEqual(out[imag._parse(out)[3][1]:], data[imag._parse(data)[3][1]:])

    def test_wrong_size_is_refused_plainly(self):
        path = self.dir / "big.png"
        write_rgba(path, 2, 2, bytes(16))
        with self.assertRaisesRegex(ValueError, "Resize"):
            imag.encode(self.sample(), {"image.png": str(path)})

    def test_unrecognised_falls_back_without_failing(self):
        view = formats.describe("IMAG", bytes(100), {})
        self.assertTrue(view["warnings"])


def retail_images():
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    found = []
    for archive in sorted((DISC / "DATA").rglob("*")):
        if archive.suffix.upper() not in (".RES", ".TRK") or not archive.is_file():
            continue
        try:
            entries = modding_backend.directory(archive)
        except Exception:
            continue
        for e in entries:
            if e["name"].lower().endswith(".img"):
                found.append((archive, e))
    return found


@unittest.skipUnless(RETAIL, "retail disc not available (set EOD_DISC)")
class RetailTests(unittest.TestCase):
    def test_all_retail_images(self):
        images = retail_images()
        self.assertEqual(len(images), 264)
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            for archive, e in images:
                data = modding_backend.payload(archive, e["name"], entries=[e], verify_crc=False)[0]
                label = f"{archive.name}#{e['name']}"
                view = formats.describe("IMAG", data, {"cache": str(tmp)})
                self.assertEqual(view["warnings"], [], label)
                self.assertTrue(coverage(view, len(data)), label)
                self.assertEqual(view["fields"][0]["value"], struct.unpack_from("<I", data, 0)[0])
                w, h, px = read_png(view["preview"]["png"])
                self.assertEqual((w, h), (view["fields"][0]["value"], view["fields"][1]["value"]), label)
                # No edits and an unchanged exported PNG are byte-identical.
                self.assertEqual(formats.apply_edits("IMAG", data, {}), data, label)
                self.assertEqual(imag.encode(data, {"image.png": view["preview"]["png"]}), data, label)

    def test_edited_png_is_pixel_identical_after_reencode(self):
        images = retail_images()
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            for archive, e in images[::9]:
                data = modding_backend.payload(archive, e["name"], entries=[e], verify_crc=False)[0]
                view = formats.describe("IMAG", data, {"cache": str(tmp)})
                w, h, px = read_png(view["preview"]["png"])
                edited = bytearray(px)
                # Reuse an existing colour so the image stays within 256 colours and is exactly representable.
                other = next((px[i:i + 4] for i in range(0, len(px), 4) if px[i:i + 4] != px[0:4]), None)
                if other is None:
                    continue
                edited[0:4] = other
                edited = bytes(edited)
                path = tmp / "edit.png"
                write_rgba(path, w, h, edited)
                out = imag.encode(data, {"image.png": str(path)})
                self.assertEqual(len(out), len(data))
                back = formats.describe("IMAG", out, {"cache": str(tmp / "o")})
                self.assertEqual(read_png(back["preview"]["png"])[2], edited, e["name"])


if __name__ == "__main__":
    unittest.main()
