#!/usr/bin/env python3
"""PS2 TEX texture decompiler and compiler for Test Drive: Eve of Destruction.

Supports 8-bit indexed textures in native upload order (.tex, .tm0).
Dimensions: powers of two 16-1024, with height at most twice width.
Decompiles to PNG image and metadata JSON.
Compiles PNG or raw image to PS2 .tex/.tm0.
"""

import argparse
import json
import os
import struct
import sys
import zlib

BLOCK_TABLE_8 = [
    [0, 1, 4, 5, 16, 17, 20, 21],
    [2, 3, 6, 7, 18, 19, 22, 23],
    [8, 9, 12, 13, 24, 25, 28, 29],
    [10, 11, 14, 15, 26, 27, 30, 31],
]

COLUMN_TABLE_8 = [
    [0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54],
    [8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62],
    [33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23],
    [41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31],
    [96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86],
    [104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94],
    [65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119],
    [73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127],
    [128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182],
    [136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190],
    [161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151],
    [169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159],
    [224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214],
    [232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222],
    [193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247],
    [201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255],
]


def gs_addr8(x: int, y: int, bw: int) -> int:
    page = (y >> 6) * max(1, (bw + 1) >> 1) + (x >> 7)
    block = BLOCK_TABLE_8[(y >> 4) & 3][(x >> 4) & 7]
    return (((page << 5) + block) << 8) + COLUMN_TABLE_8[y & 15][x & 15]


def upload_addr8(x: int, y: int, width: int) -> int:
    """Retail TEX byte address: PSMT8 pixels arranged for a PSMCT32 upload.

    This is compact upload order, not the absolute GS VRAM address gs_addr8.
    Verified against retail uicar_296.tm0 and CT.RES/20.tm0 reference images.
    """
    block = (y & ~15) * width + (x & ~15) * 2
    swap = (((y + 2) >> 2) & 1) * 4
    row = (((y & ~3) >> 1) + (y & 1)) & 7
    column = row * width * 2 + ((x + swap) & 7) * 4
    lane = ((y >> 1) & 1) + ((x >> 2) & 2)
    return block + column + lane


def write_png(filename: str, width: int, height: int, rgba_data: bytes):
    """Writes raw RGBA bytes as a standard PNG image."""
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr_data = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    ihdr_crc = zlib.crc32(b"IHDR" + ihdr_data)
    ihdr = struct.pack(">I4s", len(ihdr_data), b"IHDR") + ihdr_data + struct.pack(">I", ihdr_crc)

    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw.extend(rgba_data[y * width * 4 : (y + 1) * width * 4])
    compressed = zlib.compress(bytes(raw))
    idat_crc = zlib.crc32(b"IDAT" + compressed)
    idat = struct.pack(">I4s", len(compressed), b"IDAT") + compressed + struct.pack(">I", idat_crc)

    iend_crc = zlib.crc32(b"IEND")
    iend = struct.pack(">I4s", 0, b"IEND") + struct.pack(">I", iend_crc)

    with open(filename, "wb") as f:
        f.write(sig + ihdr + idat + iend)


def read_png(filename: str) -> tuple:
    """Reads a PNG and returns (width, height, rgba_bytes)."""
    with open(filename, "rb") as f:
        data = f.read()

    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{filename} is not a valid PNG")

    pos = 8
    width = height = bit_depth = color_type = 0
    idat_chunks = []

    while pos < len(data):
        length, chunk_type = struct.unpack(">I4s", data[pos : pos + 8])
        pos += 8
        chunk_data = data[pos : pos + length]
        pos += length + 4  # skip crc

        if chunk_type == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(">IIBB", chunk_data[:10])
        elif chunk_type == b"IDAT":
            idat_chunks.append(chunk_data)
        elif chunk_type == b"IEND":
            break

    if bit_depth != 8 or color_type != 6:
        raise ValueError("PNG input must be 8-bit RGBA (export as RGBA before compiling)")
    if data[28] != 0:
        raise ValueError("Interlaced PNG is not supported")
    raw = zlib.decompress(b"".join(idat_chunks))
    if len(raw) != height * (width * 4 + 1):
        raise ValueError("Invalid PNG scanline length")
    # Unfilter PNG scanlines (supports basic filters)
    stride = width * 4
    rgba = bytearray(width * height * 4)

    raw_pos = 0
    prev_row = bytearray(stride)

    for y in range(height):
        filter_type = raw[raw_pos]
        raw_pos += 1
        row = bytearray(raw[raw_pos : raw_pos + stride])
        raw_pos += stride

        if filter_type == 0:  # None
            cur_row = row
        elif filter_type == 1:  # Sub
            cur_row = bytearray(stride)
            for i in range(stride):
                left = cur_row[i - 4] if i >= 4 else 0
                cur_row[i] = (row[i] + left) & 0xFF
        elif filter_type == 2:  # Up
            cur_row = bytearray(stride)
            for i in range(stride):
                cur_row[i] = (row[i] + prev_row[i]) & 0xFF
        elif filter_type == 3:  # Average
            cur_row = bytearray(stride)
            for i in range(stride):
                left = cur_row[i - 4] if i >= 4 else 0
                up = prev_row[i]
                cur_row[i] = (row[i] + ((left + up) >> 1)) & 0xFF
        elif filter_type == 4:  # Paeth
            cur_row = bytearray(stride)
            for i in range(stride):
                a = cur_row[i - 4] if i >= 4 else 0
                b = prev_row[i]
                c = prev_row[i - 4] if i >= 4 else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                cur_row[i] = (row[i] + pr) & 0xFF
        else:
            raise ValueError(f"Unsupported PNG filter {filter_type}")

        rgba[y * stride : (y + 1) * stride] = cur_row
        prev_row = cur_row

    return width, height, bytes(rgba)


def decompile_tex(tex_bytes: bytes) -> tuple:
    """Decompiles a PS2 TEX file to (width, height, rgba_data, meta)."""
    if len(tex_bytes) < 0x20:
        raise ValueError("Truncated TEX header")
    w, h, levels, fmt, flags = struct.unpack("<IIIII", tex_bytes[:20])
    pal_off = struct.unpack("<I", tex_bytes[0x18:0x1C])[0]
    mip0_off = struct.unpack("<I", tex_bytes[0x1C:0x20])[0]

    if w < 16 or h < 16 or w > 1024 or h > 1024 or w & (w - 1) or h & (h - 1):
        raise ValueError("Only power-of-two TEX dimensions from 16 to 1024 are supported")
    if h > 2 * w:
        raise ValueError("Tall TEX aspect ratios above 2:1 require an unverified narrow-mip upload layout")
    if pal_off + 1024 > len(tex_bytes) or mip0_off + w * h > len(tex_bytes):
        raise ValueError("Truncated TEX palette or image")
    palette = []
    for p in range(256):
        a, r, g, b = tex_bytes[pal_off + p * 4 : pal_off + (p + 1) * 4]
        palette.append((r, g, b, a))

    bw = (w + 63) // 64
    mip0_data = tex_bytes[mip0_off :]

    rgba = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            idx = mip0_data[upload_addr8(x, y, w) if h >= 32 else y * w + x]
            r, g, b, a = palette[idx]
            off = (y * w + x) * 4
            rgba[off : off + 4] = bytes([r, g, b, a])

    meta = {
        "width": w,
        "height": h,
        "levels": levels,
        "format": fmt,
        "flags": flags,
        "header_word5": struct.unpack_from("<I", tex_bytes, 0x14)[0],
    }
    return w, h, bytes(rgba), meta


def compile_tex(width: int, height: int, rgba_bytes: bytes, fmt: int = 0x201, flags: int = 0x320600, header_word5: int = 0) -> bytes:
    """Compiles RGBA pixels to PS2 TEX 8-bit palettized GS-swizzled format."""
    if width < 16 or height < 16 or width > 1024 or height > 1024 or width & (width - 1) or height & (height - 1):
        raise ValueError("TEX dimensions must be powers of two from 16 to 1024")
    if height > 2 * width:
        raise ValueError("Tall TEX aspect ratios above 2:1 require an unverified narrow-mip upload layout")
    if len(rgba_bytes) != width * height * 4:
        raise ValueError("RGBA byte count does not match dimensions")
    # Build palette of up to 256 unique colors (quantize if more)
    colors = {}
    for i in range(0, len(rgba_bytes), 4):
        c = (rgba_bytes[i], rgba_bytes[i + 1], rgba_bytes[i + 2], rgba_bytes[i + 3])
        colors[c] = colors.get(c, 0) + 1

    sorted_colors = sorted(colors.keys(), key=lambda c: colors[c], reverse=True)
    if len(sorted_colors) > 256:
        # Keep top 256 colors
        palette_list = sorted_colors[:256]
    else:
        palette_list = sorted_colors
        while len(palette_list) < 256:
            palette_list.append((0, 0, 0, 0))

    color_to_idx = {c: i for i, c in enumerate(palette_list[:256])}

    def nearest_color(c):
        if c in color_to_idx:
            return color_to_idx[c]
        # find nearest Euclidean match
        r1, g1, b1, a1 = c
        best_i = 0
        best_dist = float("inf")
        for i, (r2, g2, b2, a2) in enumerate(palette_list[:256]):
            dist = (r1 - r2) ** 2 + (g1 - g2) ** 2 + (b1 - b2) ** 2 + (a1 - a2) ** 2
            if dist < best_dist:
                best_dist = dist
                best_i = i
        color_to_idx[c] = best_i
        return best_i

    # Map image to 8-bit indices
    indexed_pixels = bytearray(width * height)
    for i in range(width * height):
        c = (rgba_bytes[i * 4], rgba_bytes[i * 4 + 1], rgba_bytes[i * 4 + 2], rgba_bytes[i * 4 + 3])
        indexed_pixels[i] = nearest_color(c)

    # Swizzle mip 0
    bw = (width + 63) // 64
    swizzled_mip0 = bytearray(width * height)
    for y in range(height):
        for x in range(width):
            address = upload_addr8(x, y, width) if height >= 32 else y * width + x
            swizzled_mip0[address] = indexed_pixels[y * width + x]

    # Calculate mip chain
    levels = 1
    cur_w, cur_h = width, height
    while min(cur_w, cur_h) > 2:
        levels += 1
        cur_w = max(1, cur_w // 2)
        cur_h = max(1, cur_h // 2)

    # Header offsets
    pal_off = 0x80
    mip_offsets = []
    cur_data_off = 0x480  # palette is 0x80 to 0x480 (1024 bytes)

    # Build mips
    all_mips = [swizzled_mip0]
    mip_offsets.append(cur_data_off)
    cur_data_off += len(swizzled_mip0)

    cur_w, cur_h = width, height
    prev_indexed = indexed_pixels
    for lev in range(1, levels):
        nw, nh = max(1, cur_w // 2), max(1, cur_h // 2)
        n_indexed = bytearray(nw * nh)
        for ny in range(nh):
            for nx in range(nw):
                n_indexed[ny * nw + nx] = prev_indexed[(ny * 2) * cur_w + (nx * 2)]

        # Native f_0035EE38 switches upload path when mip height < 32.
        if nh >= 32:
            nbw = (nw + 63) // 64
            n_swiz = bytearray(nw * nh)
            for ny in range(nh):
                for nx in range(nw):
                    n_swiz[upload_addr8(nx, ny, nw)] = n_indexed[ny * nw + nx]
        else:
            # linear with minimum 128-byte block if not last mip
            alloc_sz = nw * nh
            if lev < levels - 1 and alloc_sz < 128:
                alloc_sz = 128
            n_swiz = bytearray(alloc_sz)
            n_swiz[:nw * nh] = n_indexed

        mip_offsets.append(cur_data_off)
        cur_data_off += len(n_swiz)
        all_mips.append(n_swiz)
        cur_w, cur_h = nw, nh
        prev_indexed = n_indexed

    # Build the native header with a zero offset sentinel after the mip list.
    pal_off = 0x80
    tex_head = bytearray(0x80)
    struct.pack_into("<IIIII", tex_head, 0, width, height, levels, fmt, flags)
    struct.pack_into("<I", tex_head, 0x18, pal_off)
    struct.pack_into("<I", tex_head, 0x14, header_word5)
    # Retail stores a zero sentinel after the final mip offset.
    for i, off in enumerate(mip_offsets):
        struct.pack_into("<I", tex_head, 0x1C + i * 4, off)

    # Palette: 256 * 4 bytes [A, R, G, B]
    pal_bytes = bytearray(1024)
    for i, (r, g, b, a) in enumerate(palette_list[:256]):
        pal_bytes[i * 4 : (i + 1) * 4] = bytes([a, r, g, b])

    out = bytearray()
    out.extend(tex_head)
    out.extend(pal_bytes)
    for m in all_mips:
        out.extend(m)

    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description="PS2 TEX compiler/decompiler")
    subparsers = parser.add_subparsers(dest="cmd", required=True)

    p_dec = subparsers.add_parser("decompile", help="Decompile .tex to PNG and JSON")
    p_dec.add_argument("input", help="Input .tex/.tm0 file")
    p_dec.add_argument("output_png", help="Output .png file")

    p_com = subparsers.add_parser("compile", help="Compile PNG to .tex")
    p_com.add_argument("input_png", help="Input .png file")
    p_com.add_argument("output_tex", help="Output .tex/.tm0 file")

    args = parser.parse_args()
    if args.cmd == "decompile":
        with open(args.input, "rb") as f:
            tex_data = f.read()
        w, h, rgba, meta = decompile_tex(tex_data)
        write_png(args.output_png, w, h, rgba)
        meta_file = os.path.splitext(args.output_png)[0] + ".json"
        with open(meta_file, "w") as f:
            json.dump(meta, f, indent=2)
        print(f"Decompiled {args.input} -> {args.output_png} ({w}x{h})")

    elif args.cmd == "compile":
        w, h, rgba = read_png(args.input_png)
        meta_file = os.path.splitext(args.input_png)[0] + ".json"
        fmt = 0x201
        flags = 0x320600
        header_word5 = 0
        if os.path.exists(meta_file):
            with open(meta_file, "r") as f:
                meta = json.load(f)
                fmt = meta.get("format", fmt)
                flags = meta.get("flags", flags)
                header_word5 = meta.get("header_word5", header_word5)
        tex_bytes = compile_tex(w, h, rgba, fmt=fmt, flags=flags, header_word5=header_word5)
        with open(args.output_tex, "wb") as f:
            f.write(tex_bytes)
        print(f"Compiled {args.input_png} -> {args.output_tex} ({len(tex_bytes)} bytes)")


if __name__ == "__main__":
    main()
