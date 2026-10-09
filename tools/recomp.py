#!/usr/bin/env python3
"""Static recompiler: MIPS R5900 (PS2 EE) machine code -> C.

Reads build/functions.json (from analyze.py) and writes:
  <out>/funcs.h        prototypes for every recompiled function
  <out>/rec_NNN.c      function bodies
  <out>/table.c        address -> function lookup table (indirect calls)

Every EE function becomes `void f_XXXXXXXX(void)` operating on the global
CPU state `g_ee` declared in runtime/include/ee.h.  Semantics of the more
involved instructions live in inline helpers in ee.h so that the generated
code stays readable and the behaviour can be tuned in one place.
"""
import argparse
import json
import os
import sys

import rabbitizer as rz

sys.path.insert(0, os.path.dirname(__file__))
from elf import Elf  # noqa: E402

rz.config.pseudos_enablePseudos = False
CAT = rz.InstrCategory.R5900
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


def hx(v):
    return "0x%X" % (v & 0xFFFFFFFF)


def simm(v):
    """C literal for a signed immediate."""
    return "%d" % v


class Unsupported(Exception):
    pass


# ---------------------------------------------------------------- registers
def R(n):
    return "R[%d]" % n


def U32(n):
    return "0u" if n == 0 else "R[%d].uw[0]" % n


def S32(n):
    return "0" if n == 0 else "R[%d].sw[0]" % n


def U64(n):
    return "0ull" if n == 0 else "R[%d].ud[0]" % n


def S64(n):
    return "0ll" if n == 0 else "R[%d].sd[0]" % n


def set64(n, expr):
    if n == 0:
        return ""
    return "R[%d].ud[0] = (uint64_t)(%s);" % (n, expr)


def set_s32(n, expr):
    """Write a 32-bit result sign-extended to 64 bits."""
    if n == 0:
        return ""
    return "R[%d].sd[0] = (int64_t)(int32_t)(%s);" % (n, expr)


def F(n):
    return "FPR[%d].f" % n


def FU(n):
    return "FPR[%d].u" % n


# ------------------------------------------------------------ instructions
class Gen:
    def __init__(self, elf, funcs, tables, hle):
        self.elf = elf
        self.funcs = funcs          # set of function start addresses
        self.tables = tables        # jr_addr -> targets
        self.hle = hle              # addr -> replaced (body not generated)

    def word(self, va):
        return self.elf.read32(va)

    def addr_expr(self, base, off):
        if base == 0:
            return hx(off)
        if off == 0:
            return "R[%d].uw[0]" % base
        return "(R[%d].uw[0] + %s)" % (base, simm(off))

    def op(self, va):
        """Return C for one non-control-flow instruction."""
        w = self.word(va)
        i = rz.Instruction(w, va, CAT)
        name = i.getOpcodeName()
        rs = (w >> 21) & 31
        rt = (w >> 16) & 31
        rd = (w >> 11) & 31
        sa = (w >> 6) & 31
        imm = w & 0xFFFF
        si = sx16(imm)
        ea = self.addr_expr(rs, si)

        # COP1 encodings rabbitizer doesn't know
        if (w & 0xFFE0003F) == 0x46000004:
            return "%s = fsqrt_ps2(%s);" % (F(sa), F(rt))
        if (w >> 26) == 0x10 and rd == 25 and rs in (0, 4):
            if rs == 0:
                return set_s32(rt, "ee_mfpc(%d)" % (w & 0x7FF))
            return "ee_mtpc(%d, %s);" % (w & 0x7FF, U32(rt))

        m = getattr(self, "i_" + name.replace(".", "_"), None)
        if m is None:
            if not i.isValid():
                return "ee_bad_instruction(%s, %s);" % (hx(va), hx(w))
            raise Unsupported("%08x: %s" % (va, i.disassemble()))
        return m(w, va, rs, rt, rd, sa, imm, si, ea)

    # ---- ALU
    def i_nop(self, *a):
        return ""

    def i_sync(self, *a):
        return ""

    def i_sync_p(self, *a):
        return ""

    def i_cache(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "ee_cache(%d, %s);" % (rt, ea)

    def i_lui(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rt, "(int64_t)(int32_t)%s" % hx(imm << 16))

    def i_addiu(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set_s32(rt, "%s + %s" % (U32(rs), hx(si)))

    i_addi = i_addiu

    def i_daddiu(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rt, "%s + (uint64_t)(int64_t)%s" % (U64(rs), simm(si)))

    i_daddi = i_daddiu

    def i_slti(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rt, "%s < %sll" % (S64(rs), simm(si)))

    def i_sltiu(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rt, "%s < (uint64_t)(int64_t)%s" % (U64(rs), simm(si)))

    def i_andi(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rt, "%s & %s" % (U64(rs), hx(imm)))

    def i_ori(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rt, "%s | %s" % (U64(rs), hx(imm)))

    def i_xori(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rt, "%s ^ %s" % (U64(rs), hx(imm)))

    def i_addu(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set_s32(rd, "%s + %s" % (U32(rs), U32(rt)))

    i_add = i_addu

    def i_subu(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set_s32(rd, "%s - %s" % (U32(rs), U32(rt)))

    i_sub = i_subu

    def i_daddu(self, w, va, rs, rt, rd, sa, imm, si, ea):
        if rt == 0:
            return set64(rd, U64(rs))
        if rs == 0:
            return set64(rd, U64(rt))
        return set64(rd, "%s + %s" % (U64(rs), U64(rt)))

    i_dadd = i_daddu

    def i_dsubu(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rd, "%s - %s" % (U64(rs), U64(rt)))

    i_dsub = i_dsubu

    def i_and(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rd, "%s & %s" % (U64(rs), U64(rt)))

    def i_or(self, w, va, rs, rt, rd, sa, imm, si, ea):
        if rt == 0:
            return set64(rd, U64(rs))
        return set64(rd, "%s | %s" % (U64(rs), U64(rt)))

    def i_xor(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rd, "%s ^ %s" % (U64(rs), U64(rt)))

    def i_nor(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rd, "~(%s | %s)" % (U64(rs), U64(rt)))

    def i_slt(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rd, "%s < %s" % (S64(rs), S64(rt)))

    def i_sltu(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rd, "%s < %s" % (U64(rs), U64(rt)))

    def i_movz(self, w, va, rs, rt, rd, sa, imm, si, ea):
        if rd == 0:
            return ""
        return "if (%s == 0) %s" % (U64(rt), set64(rd, U64(rs)))

    def i_movn(self, w, va, rs, rt, rd, sa, imm, si, ea):
        if rd == 0:
            return ""
        return "if (%s != 0) %s" % (U64(rt), set64(rd, U64(rs)))

    # shifts
    def i_sll(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set_s32(rd, "%s << %d" % (U32(rt), sa))

    def i_srl(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set_s32(rd, "%s >> %d" % (U32(rt), sa))

    def i_sra(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set_s32(rd, "%s >> %d" % (S32(rt), sa))

    def i_sllv(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set_s32(rd, "%s << (%s & 31)" % (U32(rt), U32(rs)))

    def i_srlv(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set_s32(rd, "%s >> (%s & 31)" % (U32(rt), U32(rs)))

    def i_srav(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set_s32(rd, "%s >> (%s & 31)" % (S32(rt), U32(rs)))

    def i_dsll(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rd, "%s << %d" % (U64(rt), sa))

    def i_dsrl(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rd, "%s >> %d" % (U64(rt), sa))

    def i_dsra(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rd, "%s >> %d" % (S64(rt), sa))

    def i_dsll32(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rd, "%s << %d" % (U64(rt), sa + 32))

    def i_dsrl32(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rd, "%s >> %d" % (U64(rt), sa + 32))

    def i_dsra32(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rd, "%s >> %d" % (S64(rt), sa + 32))

    def i_dsllv(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rd, "%s << (%s & 63)" % (U64(rt), U32(rs)))

    def i_dsrlv(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rd, "%s >> (%s & 63)" % (U64(rt), U32(rs)))

    def i_dsrav(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rd, "%s >> (%s & 63)" % (S64(rt), U32(rs)))

    # multiply / divide (pipeline 0 and 1)
    def _mul(self, kind, pipe, rs, rt, rd):
        return "ee_%s(%d, %d, %d, %d);" % (kind, pipe, rd, rs, rt)

    def i_mult(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return self._mul("mult", 0, rs, rt, rd)

    def i_multu(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return self._mul("multu", 0, rs, rt, rd)

    def i_mult1(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return self._mul("mult", 1, rs, rt, rd)

    def i_multu1(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return self._mul("multu", 1, rs, rt, rd)

    def i_madd(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return self._mul("madd", 0, rs, rt, rd)

    def i_maddu(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return self._mul("maddu", 0, rs, rt, rd)

    def i_madd1(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return self._mul("madd", 1, rs, rt, rd)

    def i_maddu1(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return self._mul("maddu", 1, rs, rt, rd)

    def i_div(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "ee_div(0, %d, %d);" % (rs, rt)

    def i_divu(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "ee_divu(0, %d, %d);" % (rs, rt)

    def i_div1(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "ee_div(1, %d, %d);" % (rs, rt)

    def i_divu1(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "ee_divu(1, %d, %d);" % (rs, rt)

    def i_mfhi(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rd, "g_ee.hi.ud[0]")

    def i_mflo(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rd, "g_ee.lo.ud[0]")

    def i_mfhi1(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rd, "g_ee.hi.ud[1]")

    def i_mflo1(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rd, "g_ee.lo.ud[1]")

    def i_mthi(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "g_ee.hi.ud[0] = %s;" % U64(rs)

    def i_mtlo(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "g_ee.lo.ud[0] = %s;" % U64(rs)

    def i_mthi1(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "g_ee.hi.ud[1] = %s;" % U64(rs)

    def i_mtlo1(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "g_ee.lo.ud[1] = %s;" % U64(rs)

    def i_mfsa(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set64(rd, "g_ee.sa")

    def i_mtsa(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "g_ee.sa = %s & 127;" % U32(rs)

    def i_mtsab(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "g_ee.sa = ((%s ^ %s) & 15) * 8;" % (U32(rs), hx(imm))

    def i_mtsah(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "g_ee.sa = ((%s ^ %s) & 7) * 16;" % (U32(rs), hx(imm))

    # ---- loads / stores
    def _load(self, fn, rt, ea, signed64):
        if rt == 0:
            return "(void)%s(%s);" % (fn, ea)
        return set64(rt, "%s(%s)" % (fn, ea)) if not signed64 else set64(rt, "(int64_t)%s(%s)" % (fn, ea))

    def i_lb(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return self._load("(int8_t)MEM_LB", rt, ea, True)

    def i_lbu(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return self._load("MEM_LB", rt, ea, False)

    def i_lh(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return self._load("(int16_t)MEM_LH", rt, ea, True)

    def i_lhu(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return self._load("MEM_LH", rt, ea, False)

    def i_lw(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return self._load("(int32_t)MEM_LW", rt, ea, True)

    def i_lwu(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return self._load("MEM_LW", rt, ea, False)

    def i_ld(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return self._load("MEM_LD", rt, ea, False)

    def i_lq(self, w, va, rs, rt, rd, sa, imm, si, ea):
        if rt == 0:
            return "(void)MEM_LW(%s);" % ea
        return "MEM_LQ(%s, &R[%d]);" % (ea, rt)

    def i_lwl(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "" if rt == 0 else "ee_lwl(%d, %s);" % (rt, ea)

    def i_lwr(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "" if rt == 0 else "ee_lwr(%d, %s);" % (rt, ea)

    def i_ldl(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "" if rt == 0 else "ee_ldl(%d, %s);" % (rt, ea)

    def i_ldr(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "" if rt == 0 else "ee_ldr(%d, %s);" % (rt, ea)

    def i_sb(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "MEM_SB(%s, (uint8_t)%s);" % (ea, U32(rt))

    def i_sh(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "MEM_SH(%s, (uint16_t)%s);" % (ea, U32(rt))

    def i_sw(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "MEM_SW(%s, %s);" % (ea, U32(rt))

    def i_sd(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "MEM_SD(%s, %s);" % (ea, U64(rt))

    def i_sq(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "MEM_SQ(%s, &R[%d]);" % (ea, rt)

    def i_swl(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "ee_swl(%d, %s);" % (rt, ea)

    def i_swr(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "ee_swr(%d, %s);" % (rt, ea)

    def i_sdl(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "ee_sdl(%d, %s);" % (rt, ea)

    def i_sdr(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "ee_sdr(%d, %s);" % (rt, ea)

    def i_lwc1(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "%s = MEM_LW(%s);" % (FU(rt), ea)

    def i_swc1(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "MEM_SW(%s, %s);" % (ea, FU(rt))

    def i_lqc2(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "MEM_LQ(%s, (R128*)&VU0.vf[%d]);" % (ea, rt) if rt else "(void)0;"

    def i_sqc2(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "MEM_SQ(%s, (const R128*)&VU0.vf[%d]);" % (ea, rt)

    # ---- system
    def i_syscall(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "ee_syscall(%s);" % hx(va)

    def i_break(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "ee_break(%s, %s);" % (hx(va), hx(w))

    def i_ei(self, *a):
        return "ee_ei();"

    def i_di(self, *a):
        return "ee_di();"

    def i_mfc0(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set_s32(rt, "ee_mfc0(%d)" % rd) if rt else "(void)ee_mfc0(%d);" % rd

    def i_mtc0(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "ee_mtc0(%d, %s);" % (rd, U32(rt))

    # ---- FPU
    def i_mfc1(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set_s32(rt, FU(rd))

    def i_mtc1(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "%s = %s;" % (FU(rd), U32(rt))

    def i_cfc1(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set_s32(rt, "ee_cfc1(%d)" % rd)

    def i_ctc1(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "ee_ctc1(%d, %s);" % (rd, U32(rt))

    def _f3(self, w):
        return (w >> 6) & 31, (w >> 11) & 31, (w >> 16) & 31   # fd, fs, ft

    def i_add_s(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "%s = fadd_ps2(%s, %s);" % (F(fd), F(fs), F(ft))

    def i_sub_s(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "%s = fsub_ps2(%s, %s);" % (F(fd), F(fs), F(ft))

    def i_mul_s(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "%s = fmul_ps2(%s, %s);" % (F(fd), F(fs), F(ft))

    def i_div_s(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "%s = fdiv_ps2(%s, %s);" % (F(fd), F(fs), F(ft))

    def i_sqrt_s(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "%s = fsqrt_ps2(%s);" % (F(fd), F(ft))

    def i_rsqrt_s(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "%s = frsqrt_ps2(%s, %s);" % (F(fd), F(fs), F(ft))

    def i_abs_s(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "%s = %s & 0x7FFFFFFFu;" % (FU(fd), FU(fs))

    def i_neg_s(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "%s = %s ^ 0x80000000u;" % (FU(fd), FU(fs))

    def i_mov_s(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "%s = %s;" % (FU(fd), FU(fs))

    def i_max_s(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "%s = fmax_ps2(%s, %s);" % (F(fd), F(fs), F(ft))

    def i_min_s(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "%s = fmin_ps2(%s, %s);" % (F(fd), F(fs), F(ft))

    def i_adda_s(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "FACC = fadd_ps2(%s, %s);" % (F(fs), F(ft))

    def i_suba_s(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "FACC = fsub_ps2(%s, %s);" % (F(fs), F(ft))

    def i_mula_s(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "FACC = fmul_ps2(%s, %s);" % (F(fs), F(ft))

    def i_madda_s(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "FACC = fadd_ps2(FACC, fmul_ps2(%s, %s));" % (F(fs), F(ft))

    def i_msuba_s(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "FACC = fsub_ps2(FACC, fmul_ps2(%s, %s));" % (F(fs), F(ft))

    def i_madd_s(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "%s = fadd_ps2(FACC, fmul_ps2(%s, %s));" % (F(fd), F(fs), F(ft))

    def i_msub_s(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "%s = fsub_ps2(FACC, fmul_ps2(%s, %s));" % (F(fd), F(fs), F(ft))

    def i_cvt_s_w(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "%s = (float)FPR[%d].s;" % (F(fd), fs)

    def i_cvt_w_s(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "FPR[%d].s = fcvtw_ps2(%s);" % (fd, F(fs))

    def i_c_eq_s(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "FCC = (%s == %s);" % (F(fs), F(ft))

    def i_c_lt_s(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "FCC = (%s < %s);" % (F(fs), F(ft))

    def i_c_le_s(self, w, *a):
        fd, fs, ft = self._f3(w)
        return "FCC = (%s <= %s);" % (F(fs), F(ft))

    def i_c_f_s(self, w, *a):
        return "FCC = 0;"

    # ---- COP2 (VU0 macro mode)
    def i_qmfc2_ni(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "" if rt == 0 else "memcpy(&R[%d], &VU0.vf[%d], 16);" % (rt, rd)

    i_qmfc2_i = i_qmfc2_ni

    def i_qmtc2_ni(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "" if rd == 0 else "memcpy(&VU0.vf[%d], &R[%d], 16);" % (rd, rt)

    def i_qmtc2_i(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "" if rd == 0 else "vu0_sync(); memcpy(&VU0.vf[%d], &R[%d], 16);" % (rd, rt)

    def i_cfc2_ni(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set_s32(rt, "vu0_cfc2(%d)" % rd) if rt else ""

    def i_cfc2_i(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return set_s32(rt, "(vu0_sync(), vu0_cfc2(%d))" % rd) if rt else "vu0_sync();"

    def i_ctc2_ni(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "vu0_ctc2(%d, %s);" % (rd, U32(rt))

    def i_ctc2_i(self, w, va, rs, rt, rd, sa, imm, si, ea):
        return "vu0_sync(); vu0_ctc2(%d, %s);" % (rd, U32(rt))

    def vu_macro(self, w, va):
        return "vu0_macro(%s);" % hx(w)

    # ---- MMI (128-bit parallel)
    def _mmi(self, name, w):
        rs = (w >> 21) & 31
        rt = (w >> 16) & 31
        rd = (w >> 11) & 31
        sa = (w >> 6) & 31
        return "mmi_%s(%d, %d, %d, %d);" % (name, rd, rs, rt, sa)


MMI_OPS = """paddh padduw paddw pand pcgth pcpyh pcpyld pcpyud pextlb pextlh pextlw
pextub pextuw phmadh pinth pmaxh pminh pnor por ppacb ppach psllh psrah psrlh
psrlw psubb psubw pxor qfsrv paddb psubh paddsw psubsw pcgtw pcgtb pceqw pceqh
pceqb pmaxw pminw pextlb pexeh pexew prevh pexch pexcw pmfhi pmflo pmthi pmtlo
pcpyh pcpyld pcpyud pinteh pmultw pmultuw pmaddw pmadduw psllvw psrlvw psravw
psraw psllw paddub psubub pabsh pabsw padsbh pmulth pmaddh phmsbh pdivw pdivuw
pdivbw pmfhl.lw pmfhl.uw pmfhl.slw pmfhl.lh pmfhl.sh pmthl.lw pext5 ppac5 plzcw""".split()


def _make_mmi(name):
    def f(self, w, *a):
        return self._mmi(name.replace(".", "_"), w)
    return f


for _n in MMI_OPS:
    setattr(Gen, "i_" + _n.replace(".", "_"), _make_mmi(_n))


# --------------------------------------------------------- function emitter
class FuncEmitter:
    def __init__(self, gen, start, addrs, kind):
        self.g = gen
        self.start = start
        self.addrs = addrs
        self.aset = set(addrs)
        self.kind = kind
        self.labels = set()
        self.calls = set()
        self.lines = []

    def ins(self, va):
        return rz.Instruction(self.g.word(va), va, CAT)

    def is_func(self, va):
        return va in self.g.funcs

    def local(self, va):
        """Can va be reached with a goto inside this function?"""
        return va in self.aset and (va == self.start or not self.is_func(va))

    def ref(self, va):
        self.calls.add(va)
        return "f_%08X" % va

    def jump_to(self, target, back_edge_check=True):
        """C statement(s) transferring control to target (no delay slot)."""
        if self.local(target):
            self.labels.add(target)
            if target <= self.cur and back_edge_check:
                return "{ IRQ_CHECK(); goto L_%08X; }" % target
            return "goto L_%08X;" % target
        if target in self.g.funcs:
            return "{ %s(); return; }" % self.ref(target)
        return "{ ee_jump_unknown(%s); return; }" % hx(target)

    def collect_labels(self):
        for va in self.addrs:
            i = self.ins(va)
            if i.isBranch() or i.getOpcodeName() == "j":
                t = i.getBranchVramGeneric() if i.isBranch() else i.getInstrIndexAsVram()
                if self.local(t):
                    self.labels.add(t)
            if va in self.g.tables:
                for t in self.g.tables[va]["targets"]:
                    if self.local(t):
                        self.labels.add(t)

    def delay(self, va):
        """C for the delay slot instruction at va."""
        i = self.ins(va)
        if i.hasDelaySlot():
            return "ee_bad_instruction(%s, 0); /* branch in delay slot */" % hx(va)
        return self.g_op(va)

    def g_op(self, va):
        i = self.ins(va)
        w = i.getRaw()
        # COP2 macro instructions (CO bit set)
        if (w >> 26) == 0x12 and (w >> 25) & 1:
            return self.g.vu_macro(w, va)
        return self.g.op(va)

    def branch_cond(self, i, w):
        name = i.getOpcodeName()
        rs = (w >> 21) & 31
        rt = (w >> 16) & 31
        base = name[:-1] if i.isBranchLikely() else name
        if base == "beq":
            return "%s == %s" % (U64(rs), U64(rt))
        if base == "bne":
            return "%s != %s" % (U64(rs), U64(rt))
        if base == "blez":
            return "%s <= 0" % S64(rs)
        if base == "bgtz":
            return "%s > 0" % S64(rs)
        if base == "bltz":
            return "%s < 0" % S64(rs)
        if base == "bgez":
            return "%s >= 0" % S64(rs)
        if base == "bc1f":
            return "!FCC"
        if base == "bc1t":
            return "FCC"
        if base == "bc0f":
            return "!ee_cpcond0()"
        if base == "bc0t":
            return "ee_cpcond0()"
        raise Unsupported(name)

    def emit(self):
        g = self.g
        out = self.lines
        self.collect_labels()
        body = []
        addrs = self.addrs
        n = len(addrs)
        k = 0
        if addrs and addrs[0] != self.start:
            # code at lower addresses was pulled in (tail jumps): enter at the top
            self.labels.add(self.start)
            body.append("  goto L_%08X;" % self.start)
        while k < n:
            va = addrs[k]
            self.cur = va
            i = self.ins(va)
            w = i.getRaw()
            name = i.getOpcodeName()
            lab = ("L_%08X:;" % va) if va in self.labels else None
            if lab:
                body.append(lab)
            if name == "eret":
                body.append("  ee_eret();")
                body.append("  return;")
                k += 1
                continue
            if not i.hasDelaySlot():
                if not i.isValid() and not ((w & 0xFFE0003F) == 0x46000004 or ((w >> 26) == 0x10 and ((w >> 11) & 31) == 25)):
                    body.append("  ee_bad_instruction(%s, %s);" % (hx(va), hx(w)))
                    body.append("  return;")
                    k += 1
                    continue
                c = self.g_op(va)
                if c:
                    body.append("  " + c)
                if name == "eret":
                    body.append("  return;")
                    k += 1
                    continue
                nxt = va + 4
                k += 1
                if (k >= n or addrs[k] != nxt):
                    body.append("  " + self.jump_to(nxt, False))
                continue

            # control transfer with delay slot
            dva = va + 4
            dcode = self.delay(dva)
            after = va + 8
            k += 1
            if k < n and addrs[k] == dva:
                k += 1  # consume delay slot
            dslot_is_target = dva in self.labels
            if name == "jal":
                t = i.getInstrIndexAsVram()
                body.append("  R[31].ud[0] = %s; %s" % (hx(after), dcode))
                if t in g.funcs:
                    body.append("  %s();" % self.ref(t))
                else:
                    body.append("  ee_call_unknown(%s);" % hx(t))
                self.fallthrough(body, after, k)
            elif name == "jalr":
                rs = (w >> 21) & 31
                rd = (w >> 11) & 31
                body.append("  { uint32_t t = %s; %s %s ee_call(t); }" % (
                    U32(rs), ("R[%d].ud[0] = %s;" % (rd, hx(after))) if rd else "", dcode))
                self.fallthrough(body, after, k)
            elif name == "jr":
                rs = (w >> 21) & 31
                if rs == 31:
                    body.append("  %s" % dcode)
                    body.append("  return;")
                elif va in g.tables:
                    body.append("  { uint32_t t = %s; %s" % (U32(rs), dcode))
                    body.append("    switch (t) {")
                    for t in sorted(set(g.tables[va]["targets"])):
                        body.append("    case %s: %s" % (hx(t), self.jump_to(t)))
                    body.append("    default: ee_jump(t); return; } }")
                else:
                    body.append("  { uint32_t t = %s; %s ee_jump(t); return; }" % (U32(rs), dcode))
            elif name == "j":
                t = i.getInstrIndexAsVram()
                body.append("  %s" % dcode)
                body.append("  " + self.jump_to(t))
            elif i.isBranch():
                t = i.getBranchVramGeneric()
                cond = self.branch_cond(i, w)
                likely = i.isBranchLikely()
                uncond = i.isUnconditionalBranch()
                if uncond:
                    body.append("  %s" % dcode)
                    body.append("  " + self.jump_to(t))
                elif likely:
                    body.append("  if (%s) { %s %s }" % (cond, dcode, self.jump_to(t)))
                    self.fallthrough(body, after, k)
                else:
                    body.append("  { int c_ = %s; %s if (c_) %s }" % (cond, dcode, self.jump_to(t)))
                    self.fallthrough(body, after, k)
            else:
                raise Unsupported("%08x %s" % (va, i.disassemble()))
            if dslot_is_target:
                # something branches straight into our delay slot: emit it again
                body.append("  " + self.jump_to(after, False))
                body.append("L_%08X:;" % dva)
                body.append("  " + dcode)
                body.append("  " + self.jump_to(after, False))
        return body

    def fallthrough(self, body, after, k):
        if k >= len(self.addrs) or self.addrs[k] != after:
            body.append("  " + self.jump_to(after, False))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "gen"))
    ap.add_argument("--bucket", type=lambda x: int(x, 0), default=0x6000, help="bytes of code per output file")
    ap.add_argument("--only", help="comma separated list of function addresses (debug)")
    args = ap.parse_args()

    elf = Elf(os.path.join(ROOT, "SLUS_209.10"))
    fj = json.load(open(os.path.join(ROOT, "build", "functions.json")))
    funcs = {f["addr"]: f for f in fj["functions"]}
    tables = {int(k): v for k, v in fj["jump_tables"].items()}
    hle = load_hle(os.path.join(ROOT, "config", "hle.txt"))
    gen = Gen(elf, set(funcs), tables, hle)

    os.makedirs(args.out, exist_ok=True)
    only = set(int(x, 16) for x in args.only.split(",")) if args.only else None

    files = []
    errors = 0
    for addr in sorted(funcs):
        if only and addr not in only:
            continue
        f = funcs[addr]
        mode = hle.get(addr, "")
        if mode == "replace":
            continue
        fname = "f_%08X_orig" % addr if mode == "wrap" else "f_%08X" % addr
        addrs = [a for (lo, hi) in f["ranges"] for a in range(lo, hi, 4)]
        fe = FuncEmitter(gen, addr, addrs, f["kind"])
        try:
            body = fe.emit()
        except Unsupported as e:
            print("unsupported in %08x: %s" % (addr, e))
            errors += 1
            body = ["  ee_bad_instruction(%s, 0); /* unsupported: %s */" % (hx(addr), str(e).replace("*/", ""))]
        text = ["", "/* %08X  (%s, %d instructions) */" % (addr, f["kind"], len(addrs)),
                "void %s(void) {" % fname, "  EE_FUNC_ENTRY(0x%08X);" % addr]
        text += [l for l in body if l.strip()]
        text.append("}")
        # bucket by address so that local changes only touch one file
        bucket = (addr - 0x100000) // args.bucket
        while len(files) <= bucket:
            files.append([])
        files[bucket].append("\n".join(text))

    hdr = ["/* generated by tools/recomp.py - do not edit */", "#pragma once", "#ifdef __cplusplus",
           'extern "C" {', "#endif"]
    for addr in sorted(funcs):
        hdr.append("void f_%08X(void);" % addr)
        if hle.get(addr) == "wrap":
            hdr.append("void f_%08X_orig(void);" % addr)
    hdr += ["#ifdef __cplusplus", "}", "#endif", ""]
    write_if_changed(os.path.join(args.out, "funcs.h"), "\n".join(hdr))

    for n, chunk in enumerate(files):
        src = ["/* generated by tools/recomp.py - do not edit */", '#include "ee.h"', '#include "funcs.h"', ""]
        src += chunk
        write_if_changed(os.path.join(args.out, "rec_%03d.c" % n), "\n".join(src) + "\n")
    # remove stale files
    for fn in os.listdir(args.out):
        if fn.startswith("rec_") and fn.endswith(".c") and int(fn[4:7]) >= len(files):
            os.remove(os.path.join(args.out, fn))

    tab = ["/* generated by tools/recomp.py - do not edit */", '#include "ee.h"', '#include "funcs.h"', "",
           "const EEFuncEntry ee_func_table[] = {"]
    for addr in sorted(funcs):
        tab.append("  { 0x%08X, f_%08X }," % (addr, addr))
    import binascii
    tab += ["};", "const unsigned ee_func_table_size = %d;" % len(funcs),
            "const uint32_t ee_elf_crc32 = 0x%08X;" % (binascii.crc32(elf.data) & 0xFFFFFFFF),
            "const uint32_t ee_elf_size = %d;" % len(elf.data), ""]
    write_if_changed(os.path.join(args.out, "table.c"), "\n".join(tab))
    print("wrote %d files, %d functions, %d errors" % (len(files), len(funcs), errors))
    return 1 if errors else 0


def load_hle(path):
    """config/hle.txt: '<addr> replace|wrap [name]'.

    replace: no code is generated; the runtime defines f_<addr>.
    wrap:    the original is generated as f_<addr>_orig and the runtime
             defines f_<addr> (which may call the original).
    """
    out = {}
    if os.path.exists(path):
        for line in open(path):
            line = line.split("#")[0].strip()
            if line:
                parts = line.split()
                mode = parts[1] if len(parts) > 1 else "replace"
                if mode not in ("replace", "wrap"):
                    raise SystemExit("hle.txt: bad mode %r" % mode)
                out[int(parts[0], 16)] = mode
    return out


def write_if_changed(path, text):
    if os.path.exists(path) and open(path).read() == text:
        return
    open(path, "w").write(text)


if __name__ == "__main__":
    sys.exit(main())
