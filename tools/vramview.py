#!/usr/bin/env python3
"""Decode a region of a GS VRAM dump (EOD_DUMP_VRAM) to a PNG.

usage: vramview.py vram.bin out.png bp bw psm w h [cbp cpsm]
  bp/cbp in blocks (hex), psm/cpsm hex (0 CT32, 2 CT16, 13 T8, 14 T4, ...)
Uses the swizzle tables of runtime/gs/gs_mem.cpp.
"""
import os
import re
import struct
import sys

from PIL import Image

SRC = os.path.join(os.path.dirname(__file__), '..', 'runtime', 'gs', 'gs_mem.cpp')


def tables():
    s = open(SRC).read()
    t = {}
    for m in re.finditer(r'(\w+)\[(\d+)\]\[(\d+)\] = \{(.*?)\};', s, re.S):
        name, r, c = m.group(1), int(m.group(2)), int(m.group(3))
        nums = [int(x) for x in re.findall(r'\d+', m.group(4))]
        t[name] = [nums[i * c:(i + 1) * c] for i in range(r)]
    return t


T = tables()


def addr32(x, y, bp, bw):
    page = (bp >> 5) + (y >> 5) * bw + (x >> 6)
    block = (bp & 31) + T['blockTable32'][(y >> 3) & 3][(x >> 3) & 7]
    return (((page << 5) + block) << 6) + T['columnTable32'][y & 7][x & 7]


def addr16(x, y, bp, bw):
    page = (bp >> 5) + (y >> 6) * bw + (x >> 6)
    block = (bp & 31) + T['blockTable16'][(y >> 3) & 7][(x >> 4) & 3]
    return (((page << 5) + block) << 7) + T['columnTable16'][y & 7][x & 15]


def addr8(x, y, bp, bw):
    page = (bp >> 5) + (y >> 6) * (bw >> 1) + (x >> 7)
    block = (bp & 31) + T['blockTable8'][(y >> 4) & 3][(x >> 4) & 7]
    return (((page << 5) + block) << 8) + T['columnTable8'][y & 15][x & 15]


def addr4(x, y, bp, bw):
    page = (bp >> 5) + (y >> 7) * (bw >> 1) + (x >> 7)
    block = (bp & 31) + T['blockTable4'][(y >> 4) & 7][(x >> 5) & 3]
    return (((page << 5) + block) << 9) + T['columnTable4'][y & 15][x & 31]


def main():
    vram = open(sys.argv[1], 'rb').read()
    out = sys.argv[2]
    bp, bw, psm = int(sys.argv[3], 16), int(sys.argv[4]), int(sys.argv[5], 16)
    w, h = int(sys.argv[6]), int(sys.argv[7])
    clut = None
    if len(sys.argv) > 8:
        cbp, cpsm = int(sys.argv[8], 16), int(sys.argv[9], 16)
        n = 256 if psm == 0x13 else 16
        clut = []
        for i in range(n):
            if psm == 0x13:
                cx, cy = (i & 7) | ((i & 0x10) >> 1), ((i & 8) >> 3) | ((i & 0xE0) >> 4)
            else:
                cx, cy = i & 7, i >> 3
            if cpsm == 0:
                a = addr32(cx, cy, cbp, 1) * 4
                clut.append(struct.unpack_from('<I', vram, a & 0x3FFFFF)[0])
            else:
                a = addr16(cx, cy, cbp, 1) * 2
                v = struct.unpack_from('<H', vram, a & 0x3FFFFF)[0]
                clut.append(((v & 31) << 3) | (((v >> 5) & 31) << 11) | (((v >> 10) & 31) << 19))
    img = Image.new('RGB', (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            if psm in (0, 1):
                v = struct.unpack_from('<I', vram, (addr32(x, y, bp, bw) * 4) & 0x3FFFFF)[0]
            elif psm == 2:
                c = struct.unpack_from('<H', vram, (addr16(x, y, bp, bw) * 2) & 0x3FFFFF)[0]
                v = ((c & 31) << 3) | (((c >> 5) & 31) << 11) | (((c >> 10) & 31) << 19)
            elif psm == 0x13:
                i = vram[addr8(x, y, bp, bw) & 0x3FFFFF]
                v = clut[i] if clut else i * 0x010101
            elif psm == 0x14:
                a = addr4(x, y, bp, bw)
                i = (vram[(a >> 1) & 0x3FFFFF] >> ((a & 1) * 4)) & 15
                v = clut[i] if clut else i * 0x111111
            px[x, y] = (v & 255, (v >> 8) & 255, (v >> 16) & 255)
    img.save(out)


main()
