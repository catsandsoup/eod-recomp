"""IMAG: interface raster images (`*.img`): buttons, icons, HUD dials, backdrops.

Contract: docs/FORMAT_VIEW_CONTRACT.md. Layout and loader trace: docs/IMAG_INTERFACE_IMAGES.md.

File layout (little endian):

    0x00  u32       width in pixels (the loader panics above 512)
    0x04  u32       height in pixels (same limit)
    0x08  u32       frame count N
    0x0C  u32       pixel layout, 4 in every retail file
    0x10  u8        storage kind, 1 = 8-bit indexed in every retail file
          3 bytes   flags 0xE4 0x40 0x00, not read by the game
    0x14  u32       bytes after the 64-byte header (frames only)
    0x18  u32       build address; the loader overwrites it with the frame table address
    0x1C  u32       build address; the loader overwrites it with the frame table end
    0x20  N x u32   frame offsets from the start of the file (the loader adds the load address)
    ...   header padding up to the first frame (a copy of the offsets, 256 x N, build bytes)
    per frame: 256 x RGBA palette (1,024 bytes, already in GS order, alpha 0xFF opaque)
               width x height index bytes, row-major, top row first

Loader: ImageLoad 0x00349078 / 0x0034A0F8 (ResourceGet type 'IMAG') -> relocation
0x00349C18 -> upload 0x00349CA8, which for kind 1 calls 0x00350210 (palette copy + CT8 upload).
"""
import hashlib
import os
import struct
import sys
import tempfile
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tex import read_png, write_png  # noqa: E402

FOURCCS = ("IMAG",)

HEADER_SIZE = 0x40
MAX_SIDE = 512
PALETTE_BYTES = 1024


def _parse(data):
    if len(data) < HEADER_SIZE:
        raise ValueError("shorter than the 64-byte header")
    width, height, count, layout = struct.unpack_from("<4I", data, 0)
    kind = data[0x10]
    if not (1 <= width <= MAX_SIDE and 1 <= height <= MAX_SIDE):
        raise ValueError(f"size {width}x{height} is outside the 1 to {MAX_SIDE} range the game accepts")
    if not 1 <= count <= 64 or 0x20 + 4 * count > len(data):
        raise ValueError(f"frame count {count} does not fit the file")
    offsets = list(struct.unpack_from(f"<{count}I", data, 0x20))
    stride = PALETTE_BYTES + width * height
    if kind != 1 or layout != 4:
        raise ValueError(f"storage kind {kind} / layout {layout} is not the 8-bit indexed form")
    if offsets[0] < 0x20 + 4 * count or offsets[0] % 64:
        raise ValueError("first frame overlaps the header")
    for i, o in enumerate(offsets):
        if o != offsets[0] + i * stride:
            raise ValueError("frames are not packed back to back")
    if offsets[-1] + stride > len(data):
        raise ValueError("the last frame runs past the end of the file")
    return width, height, count, offsets, stride


def _frame_rgba(data, width, height, offset):
    pal = data[offset:offset + PALETTE_BYTES]
    colours = [pal[i:i + 4] for i in range(0, PALETTE_BYTES, 4)]
    pixels = data[offset + PALETTE_BYTES:offset + PALETTE_BYTES + width * height]
    return b"".join(colours[i] for i in pixels)


def _cache_dir(context):
    cache = context.get("cache")
    return Path(cache) if cache else Path(tempfile.gettempdir()) / "eod-format-cache"


def _png(cache, key, width, height, rgba):
    cache.mkdir(parents=True, exist_ok=True)
    path = cache / (key + ".png")
    if not path.exists():
        write_png(str(path), width, height, rgba)
    return str(path)


def decode(data, context):
    width, height, count, offsets, stride = _parse(data)
    member = str(context.get("member") or "")
    sha = hashlib.sha256(data).hexdigest()
    cache = _cache_dir(context)
    frames = [_png(cache, sha if i == 0 else f"{sha}-{i}", width, height,
                   _frame_rgba(data, width, height, o)) for i, o in enumerate(offsets)]
    fields, blocks = [], []

    def field(fid, label, ftype, value, offset, size, explanation, evidence, confidence="high", unit=""):
        fields.append(dict(id=fid, group="Image", label=label, type=ftype, unit=unit, value=value,
                           offset=offset, size=size, editable=False, confidence=confidence,
                           explanation=explanation, evidence=evidence))

    ev = "ImageLoad 0x00349078 and upload 0x00349CA8 (gen/rec_097.c)"
    field("header.width", "Width", "u32", width, 0, 4,
          "Image width. The loader refuses anything above 512. Replacement images keep this size.", ev, "verified", "px")
    field("header.height", "Height", "u32", height, 4, 4,
          "Image height. The loader refuses anything above 512. Replacement images keep this size.", ev, "verified", "px")
    field("header.frames", "Number of frames", "u32", count, 8, 4,
          "Pictures stored one after another, each with its own 256-colour palette (button states, animation steps, "
          "or separate pictures a screen picks between).", ev + ": frame loop from 0x00349E2C", "high")
    field("header.layout", "Pixel layout code", "u32", struct.unpack_from("<I", data, 12)[0], 12, 4,
          "4 in every retail file: 256-colour indexed pixels, 8 bits each.", "Branch at 0x00349CA8 (4 selects the indexed path)", "high")
    field("header.kind", "Storage kind", "u8", data[0x10], 0x10, 1,
          "1 in every retail file: palette plus one byte per pixel.", "0x00349D68 sends kind 1 to 0x00350210", "high")
    field("header.size", "Frame data size", "u32", struct.unpack_from("<I", data, 0x14)[0], 0x14, 4,
          "Bytes after the 64-byte header (all frames). The game does not read it.", "Retail data pattern", "medium", "bytes")
    blocks.append(dict(name="Header flags", offset=0x11, size=3, known=True,
                       explanation="Constant E4 40 00 in every retail file. The game does not read it."))
    blocks.append(dict(name="Build addresses", offset=0x18, size=8, known=True,
                       explanation="Addresses from the tool that built the file. The loader replaces them with the frame table "
                                   "location when the file is loaded, so the stored values never matter."))
    blocks.append(dict(name="Frame offsets", offset=0x20, size=4 * count, known=True,
                       explanation="Where each frame starts in the file. The loader turns them into memory addresses."))
    first = offsets[0]
    pad = first - (0x20 + 4 * count)
    if pad:
        blocks.append(dict(name="Header padding", offset=0x20 + 4 * count, size=pad, known=True,
                           explanation="A second copy of the frame offsets, the total palette entry count and build bytes. "
                                       "The game does not read this area."))
    for i, o in enumerate(offsets):
        tag = f"Frame {i + 1}" if count > 1 else "Image"
        blocks.append(dict(name=f"{tag} palette", offset=o, size=PALETTE_BYTES, known=True,
                           explanation="256 colours as red, green, blue, alpha bytes. Alpha 255 is fully opaque, 0 is transparent."))
        blocks.append(dict(name=f"{tag} pixels", offset=o + PALETTE_BYTES, size=width * height, known=True,
                           explanation=f"{width} x {height} palette indices, one byte each, left to right, top row first."))
    end = offsets[-1] + stride
    if end < len(data):
        blocks.append(dict(name="Trailing bytes", offset=end, size=len(data) - end, known=False))
    blocks.sort(key=lambda b: b["offset"])

    summary = (f"A {width} x {height} picture"
               + (f" with {count} frames" if count > 1 else "")
               + " the game draws on menus, the in-race HUD and buttons. To change it, replace it with a PNG of the "
                 "same size; it is reduced to 256 colours per frame.")
    how = [dict(text="The game requests pictures by name through the resource loader (ResourceGet type IMAG) and "
                     "uploads each frame as a 256-colour texture.", confidence="verified")]
    view = dict(schema="eod.imag/1", fourcc="IMAG", title="Interface image", confidence="verified", howWeKnow=how,
                summary=summary, editable=True, fields=fields, blocks=blocks,
                preview=dict(kind="image", png=frames[0], width=width, height=height), warnings=[])
    if count > 1:
        view["preview"]["frames"] = frames
    return view


def _quantise(pixels, size=256):
    """Same method as texture_edit.import_png: most frequent colours, then nearest by RGBA distance."""
    colours = Counter(pixels[i:i + 4] for i in range(0, len(pixels), 4))
    selected = [c for c, _ in colours.most_common(size)]
    cache = {c: i for i, c in enumerate(selected)}
    for c in colours:
        if c not in cache:
            cache[c] = min(range(len(selected)), key=lambda i: sum((a - b) ** 2 for a, b in zip(c, selected[i])))
    return selected, bytes(cache[pixels[i:i + 4]] for i in range(0, len(pixels), 4))


def encode(data, edits):
    width, height, count, offsets, stride = _parse(data)
    out = bytearray(data)
    for key, value in edits.items():
        if key == "image.png":
            frame = 0
        elif key.startswith("frame.") and key.endswith(".png") and key[6:-4].isdigit():
            frame = int(key[6:-4])
        else:
            raise ValueError(f"'{key}' is not an editable item. Use image.png (or frame.N.png for later frames).")
        if frame >= count:
            raise ValueError(f"This image has {count} frame(s); frame {frame} does not exist.")
        try:
            w, h, pixels = read_png(str(value))
        except (OSError, ValueError) as error:
            raise ValueError(f"The PNG could not be read ({error}). Use an 8-bit PNG without interlacing.") from None
        if (w, h) != (width, height):
            raise ValueError(f"The PNG is {w} x {h}; this image is {width} x {height}. Resize it to match.")
        o = offsets[frame]
        if pixels == _frame_rgba(data, width, height, o):
            continue
        selected, indices = _quantise(pixels)
        for i, (r, g, b, a) in enumerate(selected):
            out[o + 4 * i:o + 4 * i + 4] = bytes((r, g, b, a))
        out[o + PALETTE_BYTES:o + stride] = indices
    return bytes(out)
