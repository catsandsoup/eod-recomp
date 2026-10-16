#!/usr/bin/env python3
"""Ahead-of-time compiler for the game's VU micro programs.

Input: config/vu/ - micro memory images (vu<n>_<hash>.bin) and the entry
points that ran (vu_entries.txt), as written by the runtime with
EOD_VU_DUMP=<dir>.  Output: gen/vu_rec.inc, included by runtime/vu/vu.cpp.

Each entry point becomes a C++ function that executes the instruction
pairs reachable from it exactly like the interpreter does (same helpers,
same stall/scoreboard and Q/P pipeline model), minus the decoding.  The
runtime uses a function only while micro memory holds the code it was
compiled from (checked per range of words); anything else - including a
jump to a target not known here - continues in the interpreter.
"""
import argparse
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def dest_of(i): return (i >> 21) & 15
def FT(i): return (i >> 16) & 31
def FS(i): return (i >> 11) & 31
def FD(i): return (i >> 6) & 31
def bcmask(c): return 8 >> c
def imm11(i): return ((i & 0x7FF) ^ 0x400) - 0x400
def imm15(i): return ((i >> 10) & 0x7800) | (i & 0x7FF)
def upper_ext(i): return ((i >> 6) & 31) << 2 | (i & 3)


# ------------------------------------------------------------- register use
def upper_reads(ins):
    op = ins & 0x3F
    dest = dest_of(ins)
    if op < 0x3C:
        if op == 0x2E:
            return [(FS(ins), 0xE), (FT(ins), 0xE)]
        out = [(FS(ins), dest)]
        if op < 0x1C:
            out.append((FT(ins), bcmask(ins & 3)))
        elif op >= 0x28:
            out.append((FT(ins), dest))
        return out
    ext = upper_ext(ins)
    if ext == 0x2F:
        return []
    if ext == 0x1F:
        return [(FS(ins), 0xE), (FT(ins), 1)]
    if ext == 0x2E:
        return [(FS(ins), 0xE), (FT(ins), 0xE)]
    out = [(FS(ins), dest)]
    if ext < 0x10 or 0x18 <= ext < 0x1C:
        out.append((FT(ins), bcmask(ins & 3)))
    elif ext >= 0x28:
        out.append((FT(ins), dest))
    return out


def lower_reads(ins):
    """(reads, (write reg, write mask))"""
    op = ins >> 25
    dest = dest_of(ins)
    if op == 0x00:
        return [], (FT(ins), dest)
    if op == 0x01:
        return [(FS(ins), dest)], (0, 0)
    if op == 0x40:
        f = ins & 0x3F
        if f < 0x3C:
            return [], (0, 0)
        ext = upper_ext(ins)
        fsf, ftf = (ins >> 21) & 3, (ins >> 23) & 3
        if ext in (0x30, 0x31):
            return [(FS(ins), 0xF if ext == 0x31 else dest)], (FT(ins), dest)
        if ext in (0x34, 0x36):
            return [], (FT(ins), dest)
        if ext in (0x35, 0x37):
            return [(FS(ins), dest)], (0, 0)
        if ext in (0x38, 0x3A):
            return [(FS(ins), bcmask(fsf)), (FT(ins), bcmask(ftf))], (0, 0)
        if ext == 0x39:
            return [(FT(ins), bcmask(ftf))], (0, 0)
        if ext in (0x3C, 0x42, 0x43):
            return [(FS(ins), bcmask(fsf))], (0, 0)
        if ext in (0x3D, 0x40, 0x41, 0x64):
            return [], (FT(ins), dest)
        if ext >= 0x70:
            return [(FS(ins), 0xF)], (0, 0)
        return [], (0, 0)
    return [], (0, 0)


def upper_vf_write(ins):
    """VF register and mask the upper instruction writes (0 if none / ACC)."""
    op = ins & 0x3F
    if op < 0x3C:
        if op <= 0x2D or op == 0x2F:
            return FD(ins), dest_of(ins)
        if op == 0x2E:
            return FD(ins), 0xE
        return 0, 0
    ext = upper_ext(ins)
    if 0x10 <= ext <= 0x17 or ext == 0x1D:
        return FT(ins), dest_of(ins)
    return 0, 0


def reg_bits(uses):
    lo = hi = 0
    for reg, mask in uses:
        if not reg:
            continue
        if reg < 16:
            lo |= (mask & 15) << (reg * 4)
        else:
            hi |= (mask & 15) << ((reg - 16) * 4)
    return lo, hi


# ------------------------------------------------------------- upper code
FMAC_BC = {0: "OP_ADD", 1: "OP_SUB", 2: "OP_MADD", 3: "OP_MSUB", 4: "OP_MAX", 5: "OP_MIN", 6: "OP_MUL"}
FMAC_OP = {0x1C: ("OP_MUL", "S_Q"), 0x1D: ("OP_MAX", "S_I"), 0x1E: ("OP_MUL", "S_I"), 0x1F: ("OP_MIN", "S_I"),
           0x20: ("OP_ADD", "S_Q"), 0x21: ("OP_MADD", "S_Q"), 0x22: ("OP_ADD", "S_I"), 0x23: ("OP_MADD", "S_I"),
           0x24: ("OP_SUB", "S_Q"), 0x25: ("OP_MSUB", "S_Q"), 0x26: ("OP_SUB", "S_I"), 0x27: ("OP_MSUB", "S_I"),
           0x28: ("OP_ADD", "S_VEC"), 0x29: ("OP_MADD", "S_VEC"), 0x2A: ("OP_MUL", "S_VEC"),
           0x2B: ("OP_MAX", "S_VEC"), 0x2C: ("OP_SUB", "S_VEC"), 0x2D: ("OP_MSUB", "S_VEC"),
           0x2F: ("OP_MIN", "S_VEC")}
FMAC_ACC_BC = {0: "OP_ADD", 1: "OP_SUB", 2: "OP_MADD", 3: "OP_MSUB", 6: "OP_MUL"}
FMAC_ACC = {0x1C: ("OP_MUL", "S_Q"), 0x1E: ("OP_MUL", "S_I"), 0x20: ("OP_ADD", "S_Q"), 0x21: ("OP_MADD", "S_Q"),
            0x22: ("OP_ADD", "S_I"), 0x23: ("OP_MADD", "S_I"), 0x24: ("OP_SUB", "S_Q"), 0x25: ("OP_MSUB", "S_Q"),
            0x26: ("OP_SUB", "S_I"), 0x27: ("OP_MSUB", "S_I"), 0x28: ("OP_ADD", "S_VEC"),
            0x29: ("OP_MADD", "S_VEC"), 0x2A: ("OP_MUL", "S_VEC"), 0x2C: ("OP_SUB", "S_VEC"),
            0x2D: ("OP_MSUB", "S_VEC")}


def upper_fmac(ins):
    """(op, src, to_acc) for an FMAC upper instruction compiled inline, else None."""
    op = ins & 0x3F
    if op < 0x3C:
        if op < 0x1C:
            return FMAC_BC[op >> 2], "S_BC", False
        if op in FMAC_OP:
            return FMAC_OP[op] + (False,)
        return None
    ext = upper_ext(ins)
    if (ext >> 2) in FMAC_ACC_BC:
        return FMAC_ACC_BC[ext >> 2], "S_BC", True
    if ext in FMAC_ACC:
        return FMAC_ACC[ext] + (True,)
    return None


def upper_misc(ins):
    """(pre code, value expression or None, target reg) for ITOF/FTOI/ABS/CLIP, else None."""
    if (ins & 0x3F) < 0x3C:
        return None
    ext = upper_ext(ins)
    fs, ft = FS(ins), FT(ins)
    if 0x10 <= ext <= 0x13:
        return "", "vu_itof(vu, %d, %d)" % (fs, ext & 3), ft
    if 0x14 <= ext <= 0x17:
        return "", "vu_ftoi(vu, %d, %d)" % (fs, ext & 3), ft
    if ext == 0x1D:
        return "", "vu_abs(vu, %d)" % fs, ft
    if ext == 0x1F:
        return "vu_clip(vu, %d, %d);" % (fs, ft), None, 0
    return None


def upper_is_nop(ins):
    return (ins & 0x3F) >= 0x3C and upper_ext(ins) == 0x2F


# ------------------------------------------------------------- lower code
def comps(dest):
    return [k for k in range(4) if (dest >> (3 - k)) & 1]


def lower_code(ins, pc, vu=1):
    """C code for a non-branch lower instruction."""
    op = ins >> 25
    qw = "vu1_qword" if vu == 1 else "vu_qword"
    it, is_, dest = FT(ins), FS(ins), dest_of(ins)
    if op == 0x00:  # LQ
        if not it or not dest:
            return ""
        if dest == 15:
            return "memcpy(&vu->vf[%d], %s(vu, vu->vi[%d] + %d), 16);" % (it, qw, is_, imm11(ins))
        body = "".join("vu->vf[%d].u[%d] = m.u[%d]; " % (it, k, k) for k in comps(dest))
        return "{ VUReg m; memcpy(&m, %s(vu, vu->vi[%d] + %d), 16); %s}" % (qw, is_, imm11(ins), body)
    if op == 0x01:  # SQ
        if dest == 15:
            return "memcpy(%s(vu, vu->vi[%d] + %d), &vu->vf[%d], 16);" % (qw, it, imm11(ins), is_)
        body = "".join("memcpy(p + %d, &vu->vf[%d].u[%d], 4); " % (k * 4, is_, k) for k in comps(dest))
        return "{ uint8_t *p = %s(vu, vu->vi[%d] + %d); %s}" % (qw, it, imm11(ins), body)
    if op == 0x04:  # ILW: the last selected field wins
        if not it or not dest:
            return ""
        k = comps(dest)[-1]
        return "{ uint16_t v; memcpy(&v, %s(vu, vu->vi[%d] + %d) + %d, 2); set_vi(vu, %d, v); }" % (
            qw, is_, imm11(ins), k * 4, it)
    if op == 0x05:  # ISW
        body = "".join("memcpy(p + %d, &v, 4); " % (k * 4) for k in comps(dest))
        return "{ uint8_t *p = %s(vu, vu->vi[%d] + %d); uint32_t v = vu->vi[%d] & 0xFFFF; %s}" % (
            qw, is_, imm11(ins), it, body)
    if op in FLAG_OPS:
        return FLAG_OPS[op](ins, it, is_)
    if op == 0x08:
        return "set_vi(vu, %d, vu->vi[%d] + %d);" % (it, is_, imm15(ins)) if it else ""
    if op == 0x09:
        return "set_vi(vu, %d, vu->vi[%d] - %d);" % (it, is_, imm15(ins)) if it else ""
    if op == 0x40:
        f = ins & 0x3F
        id_ = FD(ins)
        if f == 0x30:
            return "set_vi(vu, %d, vu->vi[%d] + vu->vi[%d]);" % (id_, is_, it) if id_ else ""
        if f == 0x31:
            return "set_vi(vu, %d, vu->vi[%d] - vu->vi[%d]);" % (id_, is_, it) if id_ else ""
        if f == 0x32:
            imm = (ins >> 6) & 31
            if imm & 0x10:
                imm -= 32
            return "set_vi(vu, %d, vu->vi[%d] + %d);" % (it, is_, imm) if it else ""
        if f == 0x34:
            return "set_vi(vu, %d, vu->vi[%d] & vu->vi[%d]);" % (id_, is_, it) if id_ else ""
        if f == 0x35:
            return "set_vi(vu, %d, vu->vi[%d] | vu->vi[%d]);" % (id_, is_, it) if id_ else ""
        if f >= 0x3C:
            ext = upper_ext(ins)
            if ext == 0x30:  # MOVE (and the lower NOP)
                if not it or not dest:
                    return ""
                return "".join("vu->vf[%d].f[%d] = vu->vf[%d].f[%d]; " % (it, k, is_, k) for k in comps(dest))
            if ext == 0x31:  # MR32
                if not it:
                    return ""
                body = "".join("vu->vf[%d].f[%d] = s.f[%d]; " % (it, k, (k + 1) & 3) for k in comps(dest))
                return "{ VUReg s = vu->vf[%d]; %s}" % (is_, body)
            if ext in (0x34, 0x36):  # LQI / LQD
                pre = "set_vi(vu, %d, vu->vi[%d] - 1); " % (is_, is_) if ext == 0x36 else ""
                post = "set_vi(vu, %d, vu->vi[%d] + 1); " % (is_, is_) if ext == 0x34 else ""
                if it and dest:
                    body = "".join("vu->vf[%d].u[%d] = m.u[%d]; " % (it, k, k) for k in comps(dest))
                    mid = "VUReg m; memcpy(&m, %s(vu, vu->vi[%d]), 16); %s" % (qw, is_, body)
                else:
                    mid = ""
                return "{ %s%s%s}" % (pre, mid, post)
            if ext in (0x35, 0x37):  # SQI / SQD
                pre = "set_vi(vu, %d, vu->vi[%d] - 1); " % (it, it) if ext == 0x37 else ""
                post = "set_vi(vu, %d, vu->vi[%d] + 1); " % (it, it) if ext == 0x35 else ""
                body = "".join("memcpy(p + %d, &vu->vf[%d].u[%d], 4); " % (k * 4, is_, k) for k in comps(dest))
                return "{ %suint8_t *p = %s(vu, vu->vi[%d]); %s%s}" % (pre, qw, it, body, post)
            if ext == 0x3E:  # ILWR
                if not it or not dest:
                    return ""
                k = comps(dest)[-1]
                return "{ uint16_t v; memcpy(&v, %s(vu, vu->vi[%d]) + %d, 2); set_vi(vu, %d, v); }" % (qw, is_, k * 4, it)
            if ext == 0x3F:  # ISWR
                body = "".join("memcpy(p + %d, &v, 4); " % (k * 4) for k in comps(dest))
                return "{ uint8_t *p = %s(vu, vu->vi[%d]); uint32_t v = vu->vi[%d] & 0xFFFF; %s}" % (qw, is_, it, body)
            if ext == 0x64:  # MFP
                if not it:
                    return ""
                return "".join("vu->vf[%d].f[%d] = vu->p; " % (it, k) for k in comps(dest))
            if ext == 0x68:
                return "set_vi(vu, %d, g_vif1_top);" % it if it else ""
            if ext == 0x69:
                return "set_vi(vu, %d, g_vif1_itop);" % it if it else ""
            if ext == 0x7B:
                return "flush_p(vu);"
            if ext == 0x3B:
                return "flush_q(vu);"
            if ext == 0x38:
                return "lower_div(vu, 0x%08Xu, 0);" % ins
            if ext == 0x39:
                return "lower_div(vu, 0x%08Xu, 1);" % ins
            if ext == 0x3A:
                return "lower_div(vu, 0x%08Xu, 2);" % ins
            if ext == 0x6C:
                return "xgkick(vu, vu->vi[%d]);" % is_
            if ext == 0x3D:  # MFIR
                if not it:
                    return ""
                return "".join("vu->vf[%d].s[%d] = (int16_t)vu->vi[%d]; " % (it, k, is_) for k in comps(dest))
            if ext == 0x3C:  # MTIR
                return "set_vi(vu, %d, vu->vf[%d].u[%d]);" % (it, is_, (ins >> 21) & 3) if it else ""
    return "lower(c, 0x%08Xu, 0x%04Xu);" % (ins, pc)


class Cache:
    """VF registers (and ACC) held in locals within a block: r<N> / racc.

    A register is loaded from vu->vf on its first read, written back at the
    end of the block (if changed) and before the generic helpers that use
    vu->vf directly."""

    def __init__(self):
        self.state = {}  # reg -> "clean" / "dirty"; absent: not loaded
        self.used = set()

    @staticmethod
    def name(r):
        return "racc" if r == "acc" else "r%d" % r

    @staticmethod
    def mem(r):
        return "vu->acc.f" if r == "acc" else "vu->vf[%d].f" % r

    def read(self, r, body):
        if r not in self.state:
            body.append("%s = _mm_loadu_ps(%s);" % (self.name(r), self.mem(r)))
            self.state[r] = "clean"
        self.used.add(r)
        return self.name(r)

    def write(self, r, expr, dest, body):
        if r == 0 or not dest:
            return
        n = self.name(r)
        if dest == 15:
            body.append("%s = %s;" % (n, expr))
        else:
            self.read(r, body)
            body.append("%s = vu_blend<%d>(%s, %s);" % (n, dest, n, expr))
        self.state[r] = "dirty"
        self.used.add(r)

    def flush(self, body):
        for r in sorted(self.state, key=str):
            if self.state[r] == "dirty":
                body.append("_mm_storeu_ps(%s, %s);" % (self.mem(r), self.name(r)))
                self.state[r] = "clean"

    def invalidate(self):
        self.state = {}


def bc(v, k):
    """Component k of vector expression v broadcast."""
    return "_mm_shuffle_ps(%s, %s, 0x%02X)" % (v, v, k * 0x55)


def lower_code_c(ins, pc, vu, cache, body):
    """Appends the code of a non-branch lower instruction using the register
    cache; returns "generic" if it went through lower(c, ...)."""
    op = ins >> 25
    qw = "vu1_qword" if vu == 1 else "vu_qword"
    it, is_, dest = FT(ins), FS(ins), dest_of(ins)

    def load(addr):
        cache.write(it, "_mm_loadu_ps(reinterpret_cast<const float *>(%s))" % addr, dest, body)

    def store(addr):
        v = cache.read(is_, body)
        if dest == 15:
            body.append("_mm_storeu_ps(reinterpret_cast<float *>(%s), %s);" % (addr, v))
        else:
            body.append("{ float *p = reinterpret_cast<float *>(%s); _mm_storeu_ps(p, vu_blend<%d>(_mm_loadu_ps(p), %s)); }"
                        % (addr, dest, v))

    if op == 0x00:  # LQ
        if it and dest:
            load("%s(vu, vu->vi[%d] + %d)" % (qw, is_, imm11(ins)))
        return None
    if op == 0x01:  # SQ
        if dest:
            store("%s(vu, vu->vi[%d] + %d)" % (qw, it, imm11(ins)))
        return None
    if op == 0x40 and (ins & 0x3F) >= 0x3C:
        ext = upper_ext(ins)
        if ext == 0x30:  # MOVE (and the lower NOP)
            if it and dest:
                cache.write(it, cache.read(is_, body), dest, body)
            return None
        if ext == 0x31:  # MR32
            if it and dest:
                s = cache.read(is_, body)
                cache.write(it, "_mm_shuffle_ps(%s, %s, 0x39)" % (s, s), dest, body)
            return None
        if ext in (0x34, 0x36):  # LQI / LQD
            if ext == 0x36:
                body.append("set_vi(vu, %d, vu->vi[%d] - 1);" % (is_, is_))
            if it and dest:
                load("%s(vu, vu->vi[%d])" % (qw, is_))
            if ext == 0x34:
                body.append("set_vi(vu, %d, vu->vi[%d] + 1);" % (is_, is_))
            return None
        if ext in (0x35, 0x37):  # SQI / SQD
            if ext == 0x37:
                body.append("set_vi(vu, %d, vu->vi[%d] - 1);" % (it, it))
            if dest:
                store("%s(vu, vu->vi[%d])" % (qw, it))
            if ext == 0x35:
                body.append("set_vi(vu, %d, vu->vi[%d] + 1);" % (it, it))
            return None
        if ext in (0x38, 0x39, 0x3A):  # DIV / SQRT / RSQRT
            fsf, ftf = (ins >> 21) & 3, (ins >> 23) & 3
            fs_v = "vu_lane_f(%s, %d)" % (cache.read(is_, body), fsf) if ext != 0x39 else "0.0f"
            ft_v = "vu_lane_f(%s, %d)" % (cache.read(it, body), ftf)
            body.append("lower_div_f(vu, %d, %s, %s);" % (ext - 0x38, fs_v, ft_v))
            return None
        if ext == 0x3C:  # MTIR
            if it:
                body.append("set_vi(vu, %d, vu_lane_u(%s, %d));" % (it, cache.read(is_, body), (ins >> 21) & 3))
            return None
        if ext == 0x3D:  # MFIR
            if it and dest:
                cache.write(it, "_mm_castsi128_ps(_mm_set1_epi32((int16_t)vu->vi[%d]))" % is_, dest, body)
            return None
        if ext == 0x64:  # MFP
            if it and dest:
                cache.write(it, "_mm_set1_ps(vu->p)", dest, body)
            return None
        if ext >= 0x70 and ext != 0x7B:  # EFU
            body.append("{ VUReg s; _mm_storeu_ps(s.f, %s); lower_efu(vu, 0x%08Xu, 0x%X, s); }"
                        % (cache.read(is_, body), ins, ext))
            return None
    code = lower_code(ins, pc, vu)
    if not code.startswith("lower(c,"):
        # integer / flag / control instructions: no VF registers involved
        if code:
            body.append(code)
        return None
    cache.flush(body)
    body.append(code)
    cache.invalidate()
    return "generic"


def _fimm(ins):
    return "0x%X" % (((ins >> 10) & 0x800) | (ins & 0x7FF))


FLAG_OPS = {
    0x10: lambda i, t, s: "set_vi(vu, 1, (vu->clip & 0xFFFFFF) == 0x%X);" % (i & 0xFFFFFF),
    0x11: lambda i, t, s: "vu->clip = 0x%X;" % (i & 0xFFFFFF),
    0x12: lambda i, t, s: "set_vi(vu, 1, (vu->clip & 0x%X) != 0);" % (i & 0xFFFFFF),
    0x13: lambda i, t, s: "set_vi(vu, 1, ((vu->clip | 0x%X) & 0xFFFFFF) == 0xFFFFFF);" % (i & 0xFFFFFF),
    0x14: lambda i, t, s: "set_vi(vu, %d, (vu->status & 0xFFF) == %s);" % (t, _fimm(i)),
    0x15: lambda i, t, s: "vu->status = (vu->status & 0x3F) | (%s & 0xFC0);" % _fimm(i),
    0x16: lambda i, t, s: "set_vi(vu, %d, vu->status & %s);" % (t, _fimm(i)),
    0x17: lambda i, t, s: "set_vi(vu, %d, vu->status | %s);" % (t, _fimm(i)),
    0x18: lambda i, t, s: "set_vi(vu, %d, (vu->mac & 0xFFFF) == (vu->vi[%d] & 0xFFFF));" % (t, s),
    0x1A: lambda i, t, s: "set_vi(vu, %d, vu->mac & vu->vi[%d]);" % (t, s),
    0x1B: lambda i, t, s: "set_vi(vu, %d, vu->mac | vu->vi[%d]);" % (t, s),
    0x1C: lambda i, t, s: "set_vi(vu, %d, vu->clip & 0xFFF);" % t,
}

BRANCH_OPS = {0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F}
FLAG_READS = {0x14, 0x15, 0x16, 0x17, 0x18, 0x1A, 0x1B}  # FS*, FM* lower ops


# helpers that read vu->cycle, and those that may advance it (stalls)
CYC_CALLS = ("sync_pipes", "flush_q", "flush_p", "lower_div", "lower(c", "upper(c", "commit_upper", "vu_flags(", "lower_efu",
             "vu_clip(", "vu_clip_v(", "vu_flag_sync")
CYC_ADVANCE = ("flush_q", "flush_p", "lower_div", "lower(c", "upper(c", "lower_efu")


def is_branch(lo, hi):
    return not (hi & 0x80000000) and (lo >> 25) in BRANCH_OPS


def has_ebit(hi):
    return bool(hi & 0x40000000)


# ------------------------------------------------------------- programs
class Unsupported(Exception):
    pass


class Prog:
    def __init__(self, vu, image, entries, flags):
        self.vu = vu
        self.img = image
        self.mask = len(image) - 1
        self.entries = sorted(entries)
        self.flags = flags

    def pair(self, pc):
        pc &= self.mask
        return struct.unpack_from("<II", self.img, pc)

    def target(self, pc, lo):
        return (pc + 8 + imm11(lo) * 8) & self.mask

    def analyze(self):
        """Reachable pairs (with a label) and the words the code depends on."""
        labels = set()
        used = set()
        work = list(self.entries)
        jr_targets = set()
        while work:
            pc = work.pop() & self.mask
            if pc in labels:
                continue
            labels.add(pc)
            lo, hi = self.pair(pc)
            used.add(pc)
            if has_ebit(hi):
                d = (pc + 8) & self.mask
                used.add(d)
                if is_branch(lo, hi):
                    raise Unsupported("branch with E bit at %04x" % pc)
                continue
            if is_branch(lo, hi):
                d = (pc + 8) & self.mask
                dlo, dhi = self.pair(d)
                used.add(d)
                if is_branch(dlo, dhi) or has_ebit(dhi):
                    raise Unsupported("branch or E bit in a delay slot at %04x" % d)
                op = lo >> 25
                if op in (0x21, 0x25):
                    jr_targets.add((pc + 16) & self.mask)
                if op in (0x24, 0x25):
                    pass  # dynamic target
                else:
                    work.append(self.target(pc, lo))
                if op not in (0x20, 0x21, 0x24, 0x25):
                    work.append((pc + 16) & self.mask)
                continue
            work.append((pc + 8) & self.mask)
            if len(labels) > 8192:
                raise Unsupported("too large")
        # return addresses may be jumped to
        for t in jr_targets:
            if t not in labels:
                work = [t]
                while work:
                    pc = work.pop() & self.mask
                    if pc in labels:
                        continue
                    labels.add(pc)
                    lo, hi = self.pair(pc)
                    used.add(pc)
                    if has_ebit(hi):
                        used.add((pc + 8) & self.mask)
                        continue
                    if is_branch(lo, hi):
                        d = (pc + 8) & self.mask
                        dlo, dhi = self.pair(d)
                        used.add(d)
                        if is_branch(dlo, dhi) or has_ebit(dhi):
                            raise Unsupported("branch or E bit in a delay slot at %04x" % d)
                        op = lo >> 25
                        if op not in (0x24, 0x25):
                            work.append(self.target(pc, lo))
                        if op not in (0x20, 0x21, 0x24, 0x25):
                            work.append((pc + 16) & self.mask)
                        continue
                    work.append((pc + 8) & self.mask)
        self.labels = labels
        self.used = used

    def reads_flags(self):
        for pc in self.used:
            lo, hi = self.pair(pc)
            if not (hi & 0x80000000) and (lo >> 25) in FLAG_READS:
                return True
        return False

    def pair_regs(self, pc):
        """(read lo, read hi, write lo, write hi) VF register bits of pair pc (for stalls)."""
        lo, hi = self.pair(pc)
        ibit = bool(hi & 0x80000000)
        uses = upper_reads(hi)
        lw = (0, 0)
        if not ibit:
            r, lw = lower_reads(lo)
            uses = uses + r
        ulo, uhi = reg_bits(uses)
        wlo, whi = reg_bits([lw, upper_vf_write(hi)])
        return ulo, uhi, wlo, whi

    def pair_code(self, pc, out, ind, ctx):
        """Code executing pair pc (not its control flow), as pair ctx['j'] of a block of ctx['n'].

        The cycle counter is the local `cyc`.  Stalls: in the block's first
        three pairs the scoreboard is checked for writes from before the block;
        writes of earlier pairs of the block (at most three back) are compared
        with their issue cycles, kept in locals.  Only the last three pairs
        record their writes in the scoreboard.  vu->cycle is updated before
        helpers that use it and read back after those that may stall."""
        lo, hi = self.pair(pc)
        ibit = bool(hi & 0x80000000)
        j, n = ctx["j"], ctx["n"]
        ulo, uhi, wlo, whi = self.pair_regs(pc)
        waitq = not ibit and (lo >> 25) == 0x40 and (lo & 0x3C) == 0x3C and upper_ext(lo) == 0x3B
        out.append(ind + "{  // %04x: %08x %08x" % (pc, hi, lo))
        ind0, ind = ind, ind + "    "
        if j < 3 and (ulo or uhi):
            out.append(ind + "VU_STALL_C(0x%xull, 0x%xull);" % (ulo, uhi))
        for i in ctx["src"]:
            out.append(ind + "if (cyc < t%d + 4) cyc = t%d + 4;" % (i, i))
        body = []
        cache = ctx["cache"]
        if waitq:
            body.append("flush_q(vu);")
        fm = upper_fmac(hi)
        um = upper_misc(hi)
        up_generic = fm is None and um is None and not upper_is_nop(hi)
        op = None if ibit else lo >> 25
        lower_inline = op is not None and op not in BRANCH_OPS
        lo_ext = upper_ext(lo) if lower_inline and op == 0x40 and (lo & 0x3F) >= 0x3C else None
        lo_efu = lo_ext is not None and lo_ext >= 0x70 and lo_ext != 0x7B
        lo_generic = lower_inline and not lo_efu and lower_code(lo, pc, self.vu).startswith("lower(c,")
        # Q/P results are applied lazily: only code that may read them syncs
        # (the divider/EFU instructions sync themselves when busy)
        need_sync = up_generic or (fm is not None and fm[1] == "S_Q") or lo_generic or lo_ext == 0x64
        if need_sync:
            body.append("sync_pipes(vu);")
        # flag instructions see the flag results issued 4+ cycles earlier
        if lower_inline and (op in FLAG_OPS or lo_generic):
            body.append("vu_flag_sync(vu);")
        # upper: computed against the old registers, committed after the lower
        commit = None
        if fm is not None:
            o, src, acc = fm
            dest = dest_of(hi)
            a = cache.read(FS(hi), body)
            if src == "S_VEC":
                b = cache.read(FT(hi), body)
            elif src == "S_BC":
                b = bc(cache.read(FT(hi), body), hi & 3)
            elif src == "S_Q":
                b = "_mm_set1_ps(vu->q)"
            else:
                b = "_mm_set1_ps(vu->i)"
            ac = cache.read("acc", body) if o in ("OP_MADD", "OP_MSUB") else "_mm_setzero_ps()"
            body.append("__m128 ur = vu_fmac<%s>(%s, %s, %s);" % (o, a, b, ac))
            commit = ("acc" if acc else FD(hi), dest, self.flags and o not in ("OP_MAX", "OP_MIN"))
        elif (hi & 0x3F) == 0x2E or ((hi & 0x3F) >= 0x3C and upper_ext(hi) == 0x2E):  # OPMSUB / OPMULA
            sub = (hi & 0x3F) == 0x2E
            a, b = cache.read(FS(hi), body), cache.read(FT(hi), body)
            ac = cache.read("acc", body) if sub else "_mm_setzero_ps()"
            body.append("__m128 ur = vu_opm<%s>(%s, %s, %s);" % ("true" if sub else "false", a, b, ac))
            commit = (FD(hi) if sub else "acc", 0xE, self.flags)
            up_generic = False
        elif um is not None:
            ext = upper_ext(hi)
            fs_, ft_ = FS(hi), FT(hi)
            if ext == 0x1F:  # CLIP
                body.append("vu_clip_v(vu, %s, %s);" % (cache.read(fs_, body), cache.read(ft_, body)))
            elif ft_ and dest_of(hi):
                s = cache.read(fs_, body)
                val = ("vu_itof_v(%s, %d)" % (s, ext & 3) if ext <= 0x13 else
                       "vu_ftoi_v(%s, %d)" % (s, ext & 3) if ext <= 0x17 else "vu_abs_v(%s)" % s)
                body.append("__m128 ur = %s;" % val)
                commit = (ft_, dest_of(hi), False)
        elif up_generic:
            cache.flush(body)
            body.append("c.up_write = false;")
            body.append("c.mac_set = false;")
            body.append("upper(c, 0x%08Xu);" % hi)

        def do_commit():
            if commit is not None:
                reg, dest, flags = commit
                if reg == "acc" or reg:
                    cache.write(reg, "ur", dest, body)
                if flags:
                    body.append("vu_flags(vu, ur, %d);" % dest)
            elif up_generic:
                cache.flush(body)  # the lower's results first: the upper's win
                body.append("commit_upper(c);")
                cache.invalidate()

        if ibit:
            do_commit()
            body.append("{ uint32_t b = 0x%08Xu; memcpy(&vu->i, &b, 4); }" % lo)
        else:
            if op in BRANCH_OPS:
                it, is_ = FT(lo), FS(lo)
                if op == 0x20:
                    pass
                elif op == 0x21:
                    if it:
                        body.append("set_vi(vu, %d, %d);" % (it, (pc + 16) // 8))
                elif op in (0x24, 0x25):
                    body.append("jr = vu->vi[%d] * 8;" % is_)
                    if op == 0x25 and it:
                        body.append("set_vi(vu, %d, %d);" % (it, (pc + 16) // 8))
                else:
                    cond = {0x28: "(uint16_t)vu->vi[%d] == (uint16_t)vu->vi[%d]" % (is_, it),
                            0x29: "(uint16_t)vu->vi[%d] != (uint16_t)vu->vi[%d]" % (is_, it),
                            0x2C: "(int16_t)vu->vi[%d] < 0" % is_,
                            0x2D: "(int16_t)vu->vi[%d] > 0" % is_,
                            0x2E: "(int16_t)vu->vi[%d] <= 0" % is_,
                            0x2F: "(int16_t)vu->vi[%d] >= 0" % is_}[op]
                    body.append("take = %s;" % cond)
            else:
                lower_code_c(lo, pc, self.vu, cache, body)
            do_commit()
        uses_cyc = any(any(k in b for k in CYC_CALLS) for b in body)
        advances = any(any(k in b for k in CYC_ADVANCE) for b in body)
        if uses_cyc:
            out.append(ind + "vu->cycle = cyc;")
        out += [ind + b for b in body]
        if advances:
            out.append(ind + "cyc = vu->cycle;")
        if j in ctx["needt"]:
            out.append(ind + "t%d = cyc;" % j)
        if j >= n - 3 and (wlo or whi):
            out.append(ind + "VU_SCORE_C(0x%xull, 0x%xull);" % (wlo, whi))
        out.append(ind + "cyc++;")
        out.append(ind0 + "}")

    def blocks(self):
        """Basic blocks: {leader: [pcs]} (a branch's delay slot belongs to its block)."""
        leaders = set(e & self.mask for e in self.entries)
        for pc in self.labels:
            lo, hi = self.pair(pc)
            if is_branch(lo, hi):
                op = lo >> 25
                if op not in (0x24, 0x25):
                    leaders.add(self.target(pc, lo))
                if op not in (0x20, 0x24):  # fall-through, or the return address of BAL/JALR
                    leaders.add((pc + 16) & self.mask)
        leaders &= self.labels
        out = {}
        for ld in sorted(leaders):
            pcs = []
            pc = ld
            while True:
                lo, hi = self.pair(pc)
                pcs.append(pc)
                if has_ebit(hi) or is_branch(lo, hi):
                    pcs.append((pc + 8) & self.mask)  # E-bit successor / delay slot
                    break
                nxt = (pc + 8) & self.mask
                if nxt in leaders or nxt not in self.labels:
                    break
                pc = nxt
            out[ld] = pcs
        return out

    def emit(self, name):
        out = ["void %s(VUState *vu, uint32_t entry) {" % name,
               "    Ctx c;",
               "    c.vu = vu;",
               "    Scoreboard &sb = g_score[vu->index];",
               "    bool take = false;",
               "    uint32_t jr = 0;",
               "    uint64_t cyc;",
               "    (void)take;",
               "    (void)jr;",
               "    (void)sb;",
               "    vu->running = 1;",
               "    vu->flag_pipe = 1;",
               "    switch (entry) {"]
        bl = self.blocks()
        for e in self.entries:
            out.append("    case 0x%04X: goto L_%04X;" % (e, e))
        out.append("    default: vu_interp(vu, entry); return;")
        out.append("    }")
        for ld, pcs in sorted(bl.items()):
            n = len(pcs)
            regs = [self.pair_regs(p) for p in pcs]
            src = []
            needt = set()
            for j in range(n):
                rl, rh = regs[j][0], regs[j][1]
                s_ = [i for i in range(max(0, j - 3), j) if (regs[i][2] & rl) or (regs[i][3] & rh)]
                src.append(s_)
                needt.update(s_)
            out.append("L_%04X: {" % ld)
            if os.environ.get("VUREC_COUNT"):  # profiling: block execution counts (see vu_blk_hit)
                out.append('    vu_blk_hit("%s", 0x%04X, %d);' % (name, ld, n))
            out.append("    cyc = vu->cycle;")
            if needt:
                out.append("    uint64_t %s;" % ", ".join("t%d = 0" % i for i in sorted(needt)))
            cache = Cache()
            code = []
            for j, pc in enumerate(pcs):
                self.pair_code(pc, code, "    ", {"j": j, "n": n, "src": src[j], "needt": needt, "cache": cache})
            if cache.used:
                out.append("    __m128 %s;" % ", ".join(Cache.name(r) for r in sorted(cache.used, key=str)))
            out += code
            tail_code = []
            cache.flush(tail_code)
            out += ["    " + t for t in tail_code]
            out.append("    vu->cycle = cyc;")
            last = pcs[-2] if n >= 2 else pcs[-1]
            lo, hi = self.pair(last)
            tail = pcs[-1]
            tlo, thi = self.pair(tail)
            if n >= 2 and has_ebit(hi):
                out.append("    vu->tpc = 0x%04X;" % tail)
                out.append("    goto end;")
            elif n >= 2 and is_branch(lo, hi):
                op = lo >> 25
                after = (last + 16) & self.mask
                if op in (0x24, 0x25):
                    out.append("    goto dispatch;")
                elif op in (0x20, 0x21):
                    out.append("    goto L_%04X;" % self.target(last, lo))
                else:
                    out.append("    if (take) goto L_%04X;" % self.target(last, lo))
                    out.append("    goto L_%04X;" % after)
            else:
                nxt = (tail + 8) & self.mask
                if nxt in bl:
                    out.append("    goto L_%04X;" % nxt)
                else:
                    out.append("    vu_interp(vu, 0x%04X);" % nxt)
                    out.append("    return;")
            out.append("}")
        if any(l.strip() == "goto dispatch;" for l in out):  # a jump through a register
            out.append("dispatch:")
            out.append("    switch (jr & 0x%X) {" % self.mask)
            for ld in sorted(bl):
                out.append("    case 0x%04X: goto L_%04X;" % (ld, ld))
            out.append("    default: vu_interp(vu, jr & 0x%X); return;" % self.mask)
            out.append("    }")
        out.append("end:")
        out.append("    flush_q(vu);")
        out.append("    flush_p(vu);")
        out.append("    vu_flag_flush(vu);")
        out.append("    vu->flag_pipe = 0;")
        out.append("    vu->running = 0;")
        out.append("}")
        return out

    def ranges(self):
        addrs = sorted(self.used)
        rs = []
        for a in addrs:
            if rs and rs[-1][1] == a:
                rs[-1][1] = a + 8
            else:
                rs.append([a, a + 8])
        return rs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", default=os.path.join(ROOT, "config", "vu"))
    ap.add_argument("--out", default=os.path.join(ROOT, "gen", "vu_rec.inc"))
    args = ap.parse_args()

    entries = []
    path = os.path.join(args.inp, "vu_entries.txt")
    if os.path.exists(path):
        for line in open(path):
            p = line.split()
            if len(p) == 3:
                entries.append((int(p[0][2:]), p[1], int(p[2], 16)))
    entries = sorted(set(entries))

    out = ["// generated by tools/vurecomp.py - do not edit", ""]
    table = []
    nfail = 0
    per_image = {}
    for vu, h, pc in entries:
        per_image.setdefault((vu, h), []).append(pc)
    for (vu, h), pcs in sorted(per_image.items()):
        img = open(os.path.join(args.inp, "vu%d_%s.bin" % (vu, h)), "rb").read()
        # drop entries the compiler can't handle, then compile the rest together
        ok = []
        for pc in pcs:
            try:
                Prog(vu, img, [pc], True).analyze()
                ok.append(pc)
            except Unsupported as e:
                print("vu%d %s %04x: not compiled (%s)" % (vu, h, pc, e))
                nfail += 1
        if not ok:
            continue
        p = Prog(vu, img, ok, True)
        p.analyze()
        p.flags = p.reads_flags()
        name = "vu%d_%s" % (vu, h[:12])
        out += p.emit(name)
        # every entry checks all the code of the function: a jump through a
        # register may reach code compiled for another entry
        rs = p.ranges()
        for i, (a, b) in enumerate(rs):
            words = struct.unpack_from("<%dI" % ((b - a) // 4), img, a)
            out.append("const uint32_t %s_w%d[] = {%s};" % (name, i, ", ".join("0x%08Xu" % w for w in words)))
        out.append("const VuRange %s_r[] = {%s};" % (
            name, ", ".join("{0x%04X, %d, %s_w%d}" % (a, (b - a) // 4, name, i) for i, (a, b) in enumerate(rs))))
        for pc in ok:
            table.append("    {%d, 0x%04X, %s, %d, %s_r}," % (vu, pc, name, len(rs), name))
        out.append("")
    out.append("const VuProg g_vu_progs[] = {")
    out += table or ["    {-1, 0, nullptr, 0, nullptr},"]
    out.append("};")
    out.append("const int g_vu_nprogs = %d;" % len(table))
    text = "\n".join(out) + "\n"
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    if not (os.path.exists(args.out) and open(args.out).read() == text):
        open(args.out, "w").write(text)
    print("vurecomp: %d programs compiled, %d not" % (len(table), nfail))


if __name__ == "__main__":
    main()
