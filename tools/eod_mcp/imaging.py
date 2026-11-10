"""PNG helpers (stdlib only): encode, downscale, contact sheet, diff, PSNR."""
import math
import struct
import zlib

MAX_SIDE = 1024
MAX_BYTES = 1_500_000


def png_bytes(w, h, rgba):
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)
        raw += rgba[y * stride:(y + 1) * stride]

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


def png_size(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError("not a PNG")
    return struct.unpack(">II", data[16:24])


def scale(w, h, rgba, nw, nh):
    """Nearest-neighbour resample (keeps palette art crisp)."""
    if (nw, nh) == (w, h):
        return rgba
    out = bytearray(nw * nh * 4)
    for y in range(nh):
        sy = y * h // nh
        row = rgba[sy * w * 4:(sy + 1) * w * 4]
        for x in range(nw):
            sx = x * w // nw
            out[(y * nw + x) * 4:(y * nw + x) * 4 + 4] = row[sx * 4:sx * 4 + 4]
    return bytes(out)


def fit(w, h, rgba, limit=MAX_SIDE):
    """Downscale to <= limit per side and <= MAX_BYTES; returns (w, h, png)."""
    while True:
        k = max(w, h) / limit
        if k > 1:
            nw, nh = max(1, int(w / k)), max(1, int(h / k))
            w, h, rgba = nw, nh, scale(w, h, rgba, nw, nh)
        data = png_bytes(w, h, rgba)
        if len(data) <= MAX_BYTES or limit <= 64:
            return w, h, data
        limit = int(limit * 0.7)


def contact_sheet(items, cell=256, cols=4):
    """items: [(w, h, rgba)] -> (w, h, rgba) grid, each cell letterboxed on grey."""
    n = len(items)
    cols = min(cols, n) or 1
    rows = math.ceil(n / cols)
    W, H = cols * cell, rows * cell
    sheet = bytearray(b"\x30\x30\x30\xff" * (W * H))
    for i, (w, h, rgba) in enumerate(items):
        k = min(cell / w, cell / h)
        nw, nh = max(1, int(w * k)), max(1, int(h * k))
        img = scale(w, h, rgba, nw, nh)
        ox, oy = (i % cols) * cell + (cell - nw) // 2, (i // cols) * cell + (cell - nh) // 2
        for y in range(nh):
            o = ((oy + y) * W + ox) * 4
            sheet[o:o + nw * 4] = img[y * nw * 4:(y + 1) * nw * 4]
    return W, H, bytes(sheet)


def side_by_side(a, b):
    (aw, ah, ap), (bw, bh, bp) = a, b
    h = max(ah, bh)
    ap2 = scale(aw, ah, ap, max(1, aw * h // ah), h)
    bp2 = scale(bw, bh, bp, max(1, bw * h // bh), h)
    aw2, bw2 = len(ap2) // 4 // h, len(bp2) // 4 // h
    out = bytearray()
    for y in range(h):
        out += ap2[y * aw2 * 4:(y + 1) * aw2 * 4] + bp2[y * bw2 * 4:(y + 1) * bw2 * 4]
    return aw2 + bw2, h, bytes(out)


def psnr(a, b):
    if len(a) != len(b) or not a:
        return None
    se = sum((x - y) ** 2 for x, y in zip(a, b))
    if se == 0:
        return float("inf")
    return 10 * math.log10(255 * 255 * len(a) / se)


def diff_image(w, h, a, b):
    """Red-on-dark difference map; returns (rgba, changed_pixel_fraction)."""
    out = bytearray(w * h * 4)
    changed = 0
    for i in range(w * h):
        o = i * 4
        d = max(abs(a[o] - b[o]), abs(a[o + 1] - b[o + 1]), abs(a[o + 2] - b[o + 2]))
        if d:
            changed += 1
            out[o:o + 4] = bytes((255, max(0, 255 - d * 2), 0, 255))
        else:
            out[o:o + 4] = bytes((a[o] // 4, a[o + 1] // 4, a[o + 2] // 4, 255))
    return bytes(out), changed / (w * h)
