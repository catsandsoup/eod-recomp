#!/usr/bin/env python3
"""VU micro-instruction disassembler (upper + lower).

usage: vudis.py <file>   lines of "<addr> <lower hex> <upper hex>"
"""
import sys
import struct

DEST = lambda w: ''.join(c for i, c in enumerate('xyzw') if (w >> (24 - i)) & 1)
BC = 'xyzw'


def ft(w): return (w >> 16) & 31
def fs(w): return (w >> 11) & 31
def fd(w): return (w >> 6) & 31


def upper(w):
    flags = ('I' if w >> 31 & 1 else '') + ('E' if w >> 30 & 1 else '') + ('M' if w >> 29 & 1 else '') + \
        ('D' if w >> 28 & 1 else '') + ('T' if w >> 27 & 1 else '')
    op = w & 0x3F
    d = DEST(w)
    names3 = {0x28: 'add', 0x2C: 'sub', 0x2A: 'mul', 0x29: 'madd', 0x2D: 'msub', 0x2B: 'max', 0x2F: 'mini',
              0x2E: 'opmsub', 0x20: 'addq', 0x22: 'addi', 0x24: 'subq', 0x26: 'subi', 0x1C: 'mulq',
              0x1E: 'muli', 0x21: 'maddq', 0x23: 'maddi', 0x25: 'msubq', 0x27: 'msubi', 0x1D: 'maxi', 0x1F: 'minii'}
    bcops = {0x00: 'add', 0x04: 'sub', 0x08: 'madd', 0x0C: 'msub', 0x10: 'max', 0x14: 'mini', 0x18: 'mul'}
    if op < 0x3C:
        if op < 0x1C:
            base = op & ~3
            return '%s%s.%s vf%d, vf%d, vf%d%s' % (bcops.get(base, '?%x' % op), BC[op & 3], d, fd(w), fs(w), ft(w),
                                                 BC[op & 3]) + (' ' + flags if flags else '')
        n = names3.get(op, '?%02x' % op)
        if op >= 0x28 or op in (0x2E,):
            return '%s.%s vf%d, vf%d, vf%d %s' % (n, d, fd(w), fs(w), ft(w), flags)
        return '%s.%s vf%d, vf%d %s' % (n, d, fd(w), fs(w), flags)
    # special (op 0x3C-0x3F): 11-bit opcode
    op2 = (w & 3) | ((w >> 4) & 0x7C)
    sp = {0x00: 'adda', 0x01: 'adda', 0x02: 'adda', 0x03: 'adda', 0x04: 'suba', 0x05: 'suba', 0x06: 'suba',
          0x07: 'suba', 0x08: 'madda', 0x09: 'madda', 0x0A: 'madda', 0x0B: 'madda', 0x0C: 'msuba', 0x0D: 'msuba',
          0x0E: 'msuba', 0x0F: 'msuba', 0x18: 'mula', 0x19: 'mula', 0x1A: 'mula', 0x1B: 'mula'}
    if op2 in sp:
        return '%s%s.%s ACC, vf%d, vf%d%s %s' % (sp[op2], BC[op2 & 3], d, fs(w), ft(w), BC[op2 & 3], flags)
    other = {0x10: 'itof0', 0x11: 'itof4', 0x12: 'itof12', 0x13: 'itof15', 0x14: 'ftoi0', 0x15: 'ftoi4',
             0x16: 'ftoi12', 0x17: 'ftoi15', 0x1C: 'mulaq', 0x1D: 'abs', 0x1E: 'mulai', 0x1F: 'clip', 0x20: 'addaq',
             0x21: 'maddaq', 0x22: 'addai', 0x23: 'maddai', 0x24: 'subaq', 0x25: 'msubaq', 0x26: 'subai',
             0x27: 'msubai', 0x28: 'adda', 0x29: 'madda', 0x2A: 'mula', 0x2C: 'suba', 0x2D: 'msuba', 0x2E: 'opmula',
             0x2F: 'nop'}
    n = other.get(op2, '?s%02x' % op2)
    if n == 'nop':
        return 'nop ' + flags
    return '%s.%s vf%d, vf%d %s' % (n, d, ft(w), fs(w), flags)


def lower(w, addr):
    if w >> 31:  # LOI handled by caller
        pass
    op = w >> 25
    it, is_, id_ = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    imm11 = w & 0x7FF
    simm11 = imm11 - 0x800 if imm11 & 0x400 else imm11
    d = DEST(w)
    if op == 0x40:  # lower op with 0x3C-0x3F specials
        f = w & 0x3F
        if f < 0x3C:
            m = {0x30: 'iadd', 0x31: 'isub', 0x32: 'iaddi', 0x34: 'iand', 0x35: 'ior'}
            if f == 0x32:
                return 'iaddi vi%d, vi%d, %d' % (it, is_, ((w >> 6) & 31) - (32 if w >> 10 & 1 else 0))
            return '%s vi%d, vi%d, vi%d' % (m.get(f, '?l%02x' % f), id_, is_, it)
        f2 = (w & 3) | ((w >> 4) & 0x7C)
        m2 = {0x30: 'move', 0x31: 'mr32', 0x34: 'lqi', 0x35: 'sqi', 0x36: 'lqd', 0x37: 'sqd', 0x38: 'div',
              0x39: 'sqrt', 0x3A: 'rsqrt', 0x3B: 'waitq', 0x3C: 'mtir', 0x3D: 'mfir', 0x3E: 'ilwr', 0x3F: 'iswr',
              0x40: 'rnext', 0x41: 'rget', 0x42: 'rinit', 0x43: 'rxor', 0x64: 'mfp', 0x68: 'xtop', 0x69: 'xitop',
              0x6C: 'xgkick', 0x70: 'esadd', 0x71: 'ersadd', 0x72: 'eleng', 0x73: 'erleng', 0x74: 'eatanxy',
              0x75: 'eatanxz', 0x76: 'esum', 0x78: 'esqrt', 0x79: 'ersqrt', 0x7A: 'ercpr', 0x7B: 'waitp',
              0x7C: 'esin', 0x7D: 'eatan', 0x7E: 'eexp'}
        n = m2.get(f2, '?L%02x' % f2)
        fsf, ftf = (w >> 21) & 3, (w >> 23) & 3
        if n in ('div', 'rsqrt'):
            return '%s Q, vf%d%s, vf%d%s' % (n, is_, BC[fsf], it, BC[ftf])
        if n == 'sqrt':
            return 'sqrt Q, vf%d%s' % (it, BC[ftf])
        if n in ('mtir',):
            return 'mtir vi%d, vf%d%s' % (it, is_, BC[fsf])
        if n in ('mfir',):
            return 'mfir.%s vf%d, vi%d' % (d, it, is_)
        if n in ('ilwr', 'iswr', 'lqi', 'sqi', 'lqd', 'sqd'):
            return '%s.%s vf/vi%d, (vi%d)' % (n, d, it, is_) if n[0] == 'i' else '%s.%s vf%d,vf%d vi%d' % (
                n, d, it, is_, is_)
        if n in ('move', 'mr32'):
            return '%s.%s vf%d, vf%d' % (n, d, it, is_)
        return '%s %s vi%d vi%d' % (n, d, it, is_)
    tgt = addr + 8 + simm11 * 8
    m = {0x00: 'lq', 0x01: 'sq', 0x04: 'ilw', 0x05: 'isw', 0x08: 'iaddiu', 0x09: 'isubiu', 0x10: 'fceq',
         0x11: 'fcset', 0x12: 'fcand', 0x13: 'fcor', 0x14: 'fseq', 0x15: 'fsset', 0x16: 'fsand', 0x17: 'fsor',
         0x18: 'fmeq', 0x1A: 'fmand', 0x1B: 'fmor', 0x1C: 'fcget', 0x20: 'b', 0x21: 'bal', 0x24: 'jr',
         0x25: 'jalr', 0x28: 'ibeq', 0x29: 'ibne', 0x2C: 'ibltz', 0x2D: 'ibgtz', 0x2E: 'iblez', 0x2F: 'ibgez'}
    n = m.get(op, '?op%02x' % op)
    if n in ('lq', 'sq', 'ilw', 'isw'):
        return '%s.%s %s%d, %d(vi%d)' % (n, d, 'vf' if n in ('lq', 'sq') else 'vi', it, simm11 * 1, is_)
    if n in ('iaddiu', 'isubiu'):
        return '%s vi%d, vi%d, 0x%x' % (n, it, is_, (w & 0x7FF) | ((w >> 10) & 0x7800))
    if n.startswith('ib') or n in ('b', 'bal'):
        return '%s vi%d, vi%d, 0x%04x' % (n, it, is_, tgt)
    if n in ('jr', 'jalr'):
        return '%s vi%d' % (n, is_)
    if n.startswith('f'):
        return '%s vi%d, 0x%x' % (n, it, w & 0xFFFFFF)
    if w == 0x8000033C:
        return 'nop'
    return n + ' %08x' % w


for line in open(sys.argv[1]):
    a, lo, hi = line.split()
    a, lo, hi = int(a, 16), int(lo, 16), int(hi, 16)
    u = upper(hi)
    if hi >> 31 & 1:
        l = 'loi %g' % struct.unpack('<f', struct.pack('<I', lo))[0]
    else:
        l = lower(lo, a)
    print('%04x  %-40s %s' % (a, u, l))
