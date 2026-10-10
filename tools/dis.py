#!/usr/bin/env python3
"""Annotated disassembler for SLUS_209.10.

usage: dis.py <hexaddr> [count]       disassemble count instructions
       dis.py -f <hexaddr>            disassemble the whole function
       dis.py -x <hexaddr>            list callers of a function

Annotations: symbol names from config/symbols.txt, string literals and
data addresses built with lui/addiu pairs.
"""
import json
import os
import struct
import sys

import rabbitizer as rz

rz.config.pseudos_enablePseudos = False
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
data = open(os.path.join(ROOT, "SLUS_209.10"), "rb").read()


def word(a):
    return struct.unpack_from("<I", data, a - 0x100000 + 0x1000)[0]


def load_symbols():
    syms = {}
    p = os.path.join(ROOT, "config", "symbols.txt")
    if os.path.exists(p):
        for line in open(p):
            line = line.split("#")[0].split()
            if len(line) >= 2:
                syms[int(line[0], 16)] = line[1]
    return syms


SYMS = load_symbols()


def name(a):
    return SYMS.get(a, "f_%08X" % a)


def string_at(a):
    off = a - 0x100000 + 0x1000
    if not (0x3C3480 <= a < 0x432E84):
        return None
    end = data.find(b"\0", off, off + 200)
    if end <= off:
        return None
    s = data[off:end]
    if all(32 <= c < 127 or c in (9, 10) for c in s) and len(s) >= 2:
        return s.decode().replace("\n", "\\n")
    return None


def annotate(i, hi):
    op = i.getOpcodeName()
    note = ""
    if op in ("jal", "j"):
        note = name(i.getInstrIndexAsVram())
    elif op == "lui":
        hi[i.rt.value] = (i.getImmediate() << 16) & 0xFFFFFFFF
    elif op in ("addiu", "ori") and i.rs.value in hi:
        v = (hi[i.rs.value] + (i.getProcessedImmediate() if op == "addiu" else i.getImmediate())) & 0xFFFFFFFF
        s = string_at(v)
        if s is not None:
            note = '"%s"' % s
        elif v in SYMS:
            note = SYMS[v]
        elif 0x3C3480 <= v < 0x4E9370:
            note = "&%08x" % v
    elif (op.startswith("l") or op.startswith("s")) and (i.doesLoad() or i.doesStore()) and i.rs.value in hi and op not in ("lui", "sll", "srl", "sra", "slt", "sltu", "slti", "sltiu", "sllv", "srlv", "srav", "sub.s", "sqrt.s"):
        try:
            v = (hi[i.rs.value] + i.getProcessedImmediate()) & 0xFFFFFFFF
            note = SYMS.get(v, "[%08x]" % v)
        except Exception:
            pass
    return note


def dis(va, n):
    hi = {}
    for k in range(n):
        a = va + 4 * k
        w = word(a)
        i = rz.Instruction(w, a, rz.InstrCategory.R5900)
        note = annotate(i, hi)
        lab = ("%s:\n" % name(a)) if a in SYMS else ""
        print("%s%08x %08x %-40s %s" % (lab, a, w, i.disassemble(), ("; " + note) if note else ""))


def func_extent(va):
    fj = json.load(open(os.path.join(ROOT, "build", "functions.json")))
    for f in fj["functions"]:
        if f["addr"] == va:
            return f["size"] // 4
    return 64


def callers(va):
    text_lo, size = 0x100000, 0x2BC290
    out = []
    for k in range(size // 4):
        a = text_lo + 4 * k
        w = word(a)
        if (w >> 26) == 3 and ((w & 0x3FFFFFF) << 2) == va:
            out.append(a)
    return out


if __name__ == "__main__":
    if sys.argv[1] == "-f":
        va = int(sys.argv[2], 16)
        dis(va, func_extent(va))
    elif sys.argv[1] == "-x":
        va = int(sys.argv[2], 16)
        for c in callers(va):
            print("%08x" % c)
    else:
        va = int(sys.argv[1], 16)
        dis(va, int(sys.argv[2]) if len(sys.argv) > 2 else 40)
