// vu.cpp - Vector Unit interpreter (VU0 macro + micro mode, VU1 micro mode).
//
// Instruction pairs execute as on hardware: the upper (FMAC) result is
// computed from the old register state and committed after the lower
// instruction, Q/P results become visible after their latency, branches
// have a delay slot and the E bit ends the program after the next pair.
#include "vu.h"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <set>
#include <utility>
#include <cstdio>
#include <cstdlib>

#include "../core/log.h"
#include "../gs/gs.h"
#include "../kernel/kernel.h"
#include "ee.h"
#include "vif.h"

VUState g_vu0, g_vu1;

static uint8_t g_vu0_mem[0x1000 + 64], g_vu0_micro[0x1000 + 64];
static uint8_t g_vu1_mem[0x4000 + 64], g_vu1_micro[0x4000 + 64];

// VIF1 TOP/ITOP for XTOP/XITOP (set by vif.cpp)
uint32_t g_vif1_top, g_vif1_itop;

void vu_init() {
    memset(&g_vu0, 0, sizeof g_vu0);
    memset(&g_vu1, 0, sizeof g_vu1);
    g_vu0.mem = g_vu0_mem;
    g_vu0.mem_mask = 0xFFF;
    g_vu0.micro = g_vu0_micro;
    g_vu0.micro_mask = 0xFFF;
    g_vu0.index = 0;
    g_vu1.mem = g_vu1_mem;
    g_vu1.mem_mask = 0x3FFF;
    g_vu1.micro = g_vu1_micro;
    g_vu1.micro_mask = 0x3FFF;
    g_vu1.index = 1;
    for (VUState *v : {&g_vu0, &g_vu1}) {
        v->vf[0].f[3] = 1.0f;
        v->q = v->p = 1.0f;
        v->r = 0x3F800000;
    }
}

namespace {

// control register indices in vi[]
enum { REG_STATUS = 16, REG_MAC = 17, REG_CLIP = 18, REG_R = 20, REG_I = 21, REG_Q = 22, REG_P = 23,
       REG_TPC = 26, REG_CMSAR0 = 27, REG_FBRST = 28, REG_VPU_STAT = 29, REG_CMSAR1 = 31 };

inline float vu_clamp(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    if ((u & 0x7F800000u) == 0x7F800000u) {
        u = (u & 0x80000000u) | 0x7F7FFFFFu;
        memcpy(&f, &u, 4);
    }
    return f;
}

inline uint32_t f2u(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

struct Ctx {
    VUState *vu;
    // pending upper result
    bool up_write = false;
    int up_reg = 0;
    VUReg up_val;
    int up_dest = 0;
    bool up_acc = false;
    // flags computed by upper
    uint32_t mac = 0;
    bool mac_set = false;
    // branch
    bool branch = false;
    uint32_t branch_target = 0;
    bool ebit = false;
    bool stop = false;
};

inline int dest_of(uint32_t i) { return (i >> 21) & 15; }  // bit3 = x ... bit0 = w
inline bool has(int dest, int c) { return (dest >> (3 - c)) & 1; }
inline int FT(uint32_t i) { return (i >> 16) & 31; }
inline int FS(uint32_t i) { return (i >> 11) & 31; }
inline int FD(uint32_t i) { return (i >> 6) & 31; }
inline int BC(uint32_t i) { return i & 3; }

// MAC flag update for the written components of result r.
inline uint32_t mac_for(const VUReg &r, int dest) {
    uint32_t m = 0;
    for (int c = 0; c < 4; c++) {
        uint32_t u = r.u[c];
        int bit = 3 - c;
        uint32_t ex = (u >> 23) & 0xFF;
        m |= (uint32_t)((u & 0x7FFFFFFF) == 0) << bit;                    // zero
        m |= (u >> 31) << (4 + bit);                                      // sign
        m |= (uint32_t)(ex == 0 && (u & 0x7FFFFF) != 0) << (8 + bit);     // underflow
        m |= (uint32_t)(ex == 0xFF) << (12 + bit);                        // overflow
    }
    return m & ((uint32_t)dest * 0x1111u);
}

void set_status_from_mac(VUState *vu, uint32_t mac) {
    uint32_t st = vu->status & 0xFF0;  // keep sticky + D/I
    if (mac & 0x000F) st |= 0x001;
    if (mac & 0x00F0) st |= 0x002;
    if (mac & 0x0F00) st |= 0x004;
    if (mac & 0xF000) st |= 0x008;
    st |= (st & 0xF) << 6;  // sticky
    vu->status = (vu->status & ~0xFFFu) | (st & 0xFFF);
}

// --------------------------------------------------------------- upper
void upper_result(Ctx &c, int reg, const VUReg &v, int dest, bool acc) {
    c.up_write = true;
    c.up_reg = reg;
    c.up_val = v;
    c.up_dest = dest;
    c.up_acc = acc;
    c.mac = mac_for(v, dest);
    c.mac_set = true;
}

__attribute__((always_inline)) inline void commit_upper(Ctx &c) {
    if (!c.up_write) return;
    VUState *vu = c.vu;
    VUReg *dst = c.up_acc ? &vu->acc : &vu->vf[c.up_reg];
    if (c.up_acc || c.up_reg != 0)
        for (int k = 0; k < 4; k++)
            if (has(c.up_dest, k)) dst->f[k] = c.up_val.f[k];
    if (c.mac_set) {
        vu->mac = c.mac;
        set_status_from_mac(vu, c.mac);
    }
    c.up_write = false;
    c.mac_set = false;
}

enum ArOp { OP_ADD, OP_SUB, OP_MUL, OP_MADD, OP_MSUB, OP_MAX, OP_MIN };

inline bool vu_special(float f) { return (f2u(f) & 0x7F800000u) == 0x7F800000u; }

// The VU has no Inf/NaN: such bit patterns are huge ordinary numbers, so a
// special result is recomputed with saturated operands (e.g. 0xFFFFFFFF * 0 = 0).
inline float apply(ArOp op, float acc, float a, float b) {
    float r;
    switch (op) {
    case OP_ADD: r = a + b; break;
    case OP_SUB: r = a - b; break;
    case OP_MUL: r = a * b; break;
    case OP_MADD: r = acc + a * b; break;
    case OP_MSUB: r = acc - a * b; break;
    case OP_MAX: return vu_clamp(a) > vu_clamp(b) ? a : b;  // returns the operand's bits (used as a move)
    case OP_MIN: return vu_clamp(a) < vu_clamp(b) ? a : b;
    default: return 0;
    }
    if (!vu_special(r)) return r;
    a = vu_clamp(a);
    b = vu_clamp(b);
    acc = vu_clamp(acc);
    switch (op) {
    case OP_ADD: return vu_clamp(a + b);
    case OP_SUB: return vu_clamp(a - b);
    case OP_MUL: return vu_clamp(a * b);
    case OP_MADD: return vu_clamp(acc + vu_clamp(a * b));
    default: return vu_clamp(acc - vu_clamp(a * b));
    }
}

// Generic FMAC: src2 is either a vector (bc<0), a broadcast component, or a scalar
void fmac(Ctx &c, uint32_t ins, ArOp op, int bc, const float *scalar, bool to_acc) {
    VUState *vu = c.vu;
    int dest = dest_of(ins);
    const VUReg &s = vu->vf[FS(ins)];
    const VUReg &t = vu->vf[FT(ins)];
    VUReg r;
    for (int k = 0; k < 4; k++) {
        float b = scalar ? *scalar : (bc >= 0 ? t.f[bc] : t.f[k]);
        r.f[k] = has(dest, k) ? apply(op, vu->acc.f[k], s.f[k], b) : 0.0f;
    }
    if (op == OP_MAX || op == OP_MIN) {
        // MAX/MINI don't touch flags
        c.up_write = true;
        c.up_reg = to_acc ? 0 : FD(ins);
        c.up_val = r;
        c.up_dest = dest;
        c.up_acc = to_acc;
        return;
    }
    upper_result(c, to_acc ? 0 : FD(ins), r, dest, to_acc);
}

// FMAC specialized on the operation and the second operand: a vector
// (S_VEC), a broadcast component (S_BC), Q (S_Q) or I (S_I).
enum Src { S_VEC, S_BC, S_Q, S_I };

template <ArOp OP>
inline float apply_t(float acc, float a, float b) {
    float r = 0.0f;
    switch (OP) {
    case OP_ADD: r = a + b; break;
    case OP_SUB: r = a - b; break;
    case OP_MUL: r = a * b; break;
    case OP_MADD: r = acc + a * b; break;
    case OP_MSUB: r = acc - a * b; break;
    case OP_MAX: return vu_clamp(a) > vu_clamp(b) ? a : b;
    default: return vu_clamp(a) < vu_clamp(b) ? a : b;
    }
    if (__builtin_expect(vu_special(r), 0)) return apply(OP, acc, a, b);
    return r;
}

template <ArOp OP, int SRC, bool ACC, bool FLAGS = true>
__attribute__((always_inline)) inline void up_fmac(Ctx &c, uint32_t ins) {
    VUState *vu = c.vu;
    const VUReg &s = vu->vf[FS(ins)];
    const VUReg &t = vu->vf[FT(ins)];
    float b[4];
    if (SRC == S_VEC) {
        b[0] = t.f[0], b[1] = t.f[1], b[2] = t.f[2], b[3] = t.f[3];
    } else {
        float v = SRC == S_BC ? t.f[ins & 3] : SRC == S_Q ? vu->q : vu->i;
        b[0] = b[1] = b[2] = b[3] = v;
    }
    VUReg r;
    for (int k = 0; k < 4; k++) r.f[k] = apply_t<OP>(vu->acc.f[k], s.f[k], b[k]);
    int dest = dest_of(ins);
    c.up_write = true;
    c.up_reg = ACC ? 0 : FD(ins);
    c.up_val = r;
    c.up_dest = dest;
    c.up_acc = ACC;
    if (OP != OP_MAX && OP != OP_MIN && FLAGS) {  // MAX/MINI don't touch flags
        c.mac = mac_for(r, dest);
        c.mac_set = true;
    }
}

void upper(Ctx &c, uint32_t ins);
void up_nop(Ctx &, uint32_t) {}

typedef void (*UpFn)(Ctx &c, uint32_t ins);

// Handler for an upper instruction (the generic decoder for the rare ones).
UpFn decode_upper(uint32_t ins) {
    uint32_t op = ins & 0x3F;
#define F(o, src, acc) &up_fmac<o, src, acc>
    if (op < 0x3C) {
        switch (op >> 2) {
        case 0: return F(OP_ADD, S_BC, false);
        case 1: return F(OP_SUB, S_BC, false);
        case 2: return F(OP_MADD, S_BC, false);
        case 3: return F(OP_MSUB, S_BC, false);
        case 4: return F(OP_MAX, S_BC, false);
        case 5: return F(OP_MIN, S_BC, false);
        case 6: return F(OP_MUL, S_BC, false);
        }
        switch (op) {
        case 0x1C: return F(OP_MUL, S_Q, false);
        case 0x1D: return F(OP_MAX, S_I, false);
        case 0x1E: return F(OP_MUL, S_I, false);
        case 0x1F: return F(OP_MIN, S_I, false);
        case 0x20: return F(OP_ADD, S_Q, false);
        case 0x21: return F(OP_MADD, S_Q, false);
        case 0x22: return F(OP_ADD, S_I, false);
        case 0x23: return F(OP_MADD, S_I, false);
        case 0x24: return F(OP_SUB, S_Q, false);
        case 0x25: return F(OP_MSUB, S_Q, false);
        case 0x26: return F(OP_SUB, S_I, false);
        case 0x27: return F(OP_MSUB, S_I, false);
        case 0x28: return F(OP_ADD, S_VEC, false);
        case 0x29: return F(OP_MADD, S_VEC, false);
        case 0x2A: return F(OP_MUL, S_VEC, false);
        case 0x2B: return F(OP_MAX, S_VEC, false);
        case 0x2C: return F(OP_SUB, S_VEC, false);
        case 0x2D: return F(OP_MSUB, S_VEC, false);
        case 0x2F: return F(OP_MIN, S_VEC, false);
        }
        return &upper;
    }
    uint32_t ext = ((ins >> 6) & 31) << 2 | (ins & 3);
    switch (ext >> 2) {
    case 0: return F(OP_ADD, S_BC, true);
    case 1: return F(OP_SUB, S_BC, true);
    case 2: return F(OP_MADD, S_BC, true);
    case 3: return F(OP_MSUB, S_BC, true);
    case 6: return F(OP_MUL, S_BC, true);
    }
    switch (ext) {
    case 0x1C: return F(OP_MUL, S_Q, true);
    case 0x1E: return F(OP_MUL, S_I, true);
    case 0x20: return F(OP_ADD, S_Q, true);
    case 0x21: return F(OP_MADD, S_Q, true);
    case 0x22: return F(OP_ADD, S_I, true);
    case 0x23: return F(OP_MADD, S_I, true);
    case 0x24: return F(OP_SUB, S_Q, true);
    case 0x25: return F(OP_MSUB, S_Q, true);
    case 0x26: return F(OP_SUB, S_I, true);
    case 0x27: return F(OP_MSUB, S_I, true);
    case 0x28: return F(OP_ADD, S_VEC, true);
    case 0x29: return F(OP_MADD, S_VEC, true);
    case 0x2A: return F(OP_MUL, S_VEC, true);
    case 0x2C: return F(OP_SUB, S_VEC, true);
    case 0x2D: return F(OP_MSUB, S_VEC, true);
    case 0x2F: return &up_nop;
    }
#undef F
    return &upper;
}

void upper(Ctx &c, uint32_t ins) {
    VUState *vu = c.vu;
    uint32_t op = ins & 0x3F;
    float q = vu->q, ii = vu->i;
    if (op < 0x3C) {
        switch (op) {
        case 0x00: case 0x01: case 0x02: case 0x03: fmac(c, ins, OP_ADD, BC(ins), nullptr, false); return;
        case 0x04: case 0x05: case 0x06: case 0x07: fmac(c, ins, OP_SUB, BC(ins), nullptr, false); return;
        case 0x08: case 0x09: case 0x0A: case 0x0B: fmac(c, ins, OP_MADD, BC(ins), nullptr, false); return;
        case 0x0C: case 0x0D: case 0x0E: case 0x0F: fmac(c, ins, OP_MSUB, BC(ins), nullptr, false); return;
        case 0x10: case 0x11: case 0x12: case 0x13: fmac(c, ins, OP_MAX, BC(ins), nullptr, false); return;
        case 0x14: case 0x15: case 0x16: case 0x17: fmac(c, ins, OP_MIN, BC(ins), nullptr, false); return;
        case 0x18: case 0x19: case 0x1A: case 0x1B: fmac(c, ins, OP_MUL, BC(ins), nullptr, false); return;
        case 0x1C: fmac(c, ins, OP_MUL, -1, &q, false); return;
        case 0x1D: fmac(c, ins, OP_MAX, -1, &ii, false); return;
        case 0x1E: fmac(c, ins, OP_MUL, -1, &ii, false); return;
        case 0x1F: fmac(c, ins, OP_MIN, -1, &ii, false); return;
        case 0x20: fmac(c, ins, OP_ADD, -1, &q, false); return;
        case 0x21: fmac(c, ins, OP_MADD, -1, &q, false); return;
        case 0x22: fmac(c, ins, OP_ADD, -1, &ii, false); return;
        case 0x23: fmac(c, ins, OP_MADD, -1, &ii, false); return;
        case 0x24: fmac(c, ins, OP_SUB, -1, &q, false); return;
        case 0x25: fmac(c, ins, OP_MSUB, -1, &q, false); return;
        case 0x26: fmac(c, ins, OP_SUB, -1, &ii, false); return;
        case 0x27: fmac(c, ins, OP_MSUB, -1, &ii, false); return;
        case 0x28: fmac(c, ins, OP_ADD, -1, nullptr, false); return;
        case 0x29: fmac(c, ins, OP_MADD, -1, nullptr, false); return;
        case 0x2A: fmac(c, ins, OP_MUL, -1, nullptr, false); return;
        case 0x2B: fmac(c, ins, OP_MAX, -1, nullptr, false); return;
        case 0x2C: fmac(c, ins, OP_SUB, -1, nullptr, false); return;
        case 0x2D: fmac(c, ins, OP_MSUB, -1, nullptr, false); return;
        case 0x2E: {  // OPMSUB fd = acc - fs * ft (cross)
            const VUReg &s = vu->vf[FS(ins)], &t = vu->vf[FT(ins)];
            VUReg r = {};
            r.f[0] = vu_clamp(vu->acc.f[0] - s.f[1] * t.f[2]);
            r.f[1] = vu_clamp(vu->acc.f[1] - s.f[2] * t.f[0]);
            r.f[2] = vu_clamp(vu->acc.f[2] - s.f[0] * t.f[1]);
            upper_result(c, FD(ins), r, 0xE, false);
            return;
        }
        case 0x2F: fmac(c, ins, OP_MIN, -1, nullptr, false); return;
        default:
            WARN("VU%d: unknown upper op %08x", vu->index, ins);
            return;
        }
    }
    uint32_t ext = ((ins >> 6) & 31) << 2 | (ins & 3);
    switch (ext) {
    case 0x00: case 0x01: case 0x02: case 0x03: fmac(c, ins, OP_ADD, BC(ins), nullptr, true); return;
    case 0x04: case 0x05: case 0x06: case 0x07: fmac(c, ins, OP_SUB, BC(ins), nullptr, true); return;
    case 0x08: case 0x09: case 0x0A: case 0x0B: fmac(c, ins, OP_MADD, BC(ins), nullptr, true); return;
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: fmac(c, ins, OP_MSUB, BC(ins), nullptr, true); return;
    case 0x10: case 0x11: case 0x12: case 0x13: {  // ITOF0/4/12/15
        static const float sc[4] = {1.0f, 1.0f / 16, 1.0f / 4096, 1.0f / 32768};
        const VUReg &s = vu->vf[FS(ins)];
        VUReg r;
        for (int k = 0; k < 4; k++) r.f[k] = (float)s.s[k] * sc[ext & 3];
        c.up_write = true;
        c.up_reg = FT(ins);
        c.up_val = r;
        c.up_dest = dest_of(ins);
        c.up_acc = false;
        return;
    }
    case 0x14: case 0x15: case 0x16: case 0x17: {  // FTOI0/4/12/15
        static const float sc[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
        const VUReg &s = vu->vf[FS(ins)];
        VUReg r;
        for (int k = 0; k < 4; k++) {
            float f = s.f[k] * sc[ext & 3];
            if (f >= 2147483647.0f) r.s[k] = 0x7FFFFFFF;
            else if (f <= -2147483648.0f) r.s[k] = (int32_t)0x80000000;
            else r.s[k] = (int32_t)f;
        }
        c.up_write = true;
        c.up_reg = FT(ins);
        c.up_val = r;
        c.up_dest = dest_of(ins);
        c.up_acc = false;
        return;
    }
    case 0x18: case 0x19: case 0x1A: case 0x1B: fmac(c, ins, OP_MUL, BC(ins), nullptr, true); return;
    case 0x1C: fmac(c, ins, OP_MUL, -1, &q, true); return;
    case 0x1D: {  // ABS
        const VUReg &s = vu->vf[FS(ins)];
        VUReg r;
        for (int k = 0; k < 4; k++) r.u[k] = s.u[k] & 0x7FFFFFFF;
        c.up_write = true;
        c.up_reg = FT(ins);
        c.up_val = r;
        c.up_dest = dest_of(ins);
        c.up_acc = false;
        return;
    }
    case 0x1E: fmac(c, ins, OP_MUL, -1, &ii, true); return;
    case 0x1F: {  // CLIP fs.xyz, ft.w
        const VUReg &s = vu->vf[FS(ins)];
        float w = fabsf(vu->vf[FT(ins)].f[3]);
        uint32_t f = 0;
        if (s.f[0] > w) f |= 0x01;
        if (s.f[0] < -w) f |= 0x02;
        if (s.f[1] > w) f |= 0x04;
        if (s.f[1] < -w) f |= 0x08;
        if (s.f[2] > w) f |= 0x10;
        if (s.f[2] < -w) f |= 0x20;
        vu->clip = ((vu->clip << 6) | f) & 0xFFFFFF;
        return;
    }
    case 0x20: fmac(c, ins, OP_ADD, -1, &q, true); return;
    case 0x21: fmac(c, ins, OP_MADD, -1, &q, true); return;
    case 0x22: fmac(c, ins, OP_ADD, -1, &ii, true); return;
    case 0x23: fmac(c, ins, OP_MADD, -1, &ii, true); return;
    case 0x24: fmac(c, ins, OP_SUB, -1, &q, true); return;
    case 0x25: fmac(c, ins, OP_MSUB, -1, &q, true); return;
    case 0x26: fmac(c, ins, OP_SUB, -1, &ii, true); return;
    case 0x27: fmac(c, ins, OP_MSUB, -1, &ii, true); return;
    case 0x28: fmac(c, ins, OP_ADD, -1, nullptr, true); return;
    case 0x29: fmac(c, ins, OP_MADD, -1, nullptr, true); return;
    case 0x2A: fmac(c, ins, OP_MUL, -1, nullptr, true); return;
    case 0x2C: fmac(c, ins, OP_SUB, -1, nullptr, true); return;
    case 0x2D: fmac(c, ins, OP_MSUB, -1, nullptr, true); return;
    case 0x2E: {  // OPMULA acc = fs * ft (cross)
        const VUReg &s = vu->vf[FS(ins)], &t = vu->vf[FT(ins)];
        VUReg r = {};
        r.f[0] = vu_clamp(s.f[1] * t.f[2]);
        r.f[1] = vu_clamp(s.f[2] * t.f[0]);
        r.f[2] = vu_clamp(s.f[0] * t.f[1]);
        upper_result(c, 0, r, 0xE, true);
        return;
    }
    case 0x2F:  // NOP
        return;
    default:
        WARN("VU%d: unknown upper ext op %08x", vu->index, ins);
    }
}

// --------------------------------------------------------------- lower
inline uint8_t *vu_qword(VUState *vu, uint32_t addr_qw) {
    if (vu->index == 0 && (addr_qw & 0x400)) {
        // VU0 can see VU1's registers at 0x4000 - only VF regs supported
        uint32_t r = addr_qw & 0x3F;
        if (r < 32) return reinterpret_cast<uint8_t *>(&g_vu1.vf[r]);
        return reinterpret_cast<uint8_t *>(&g_vu1.vi[(r - 32) & 31]);
    }
    return vu->mem + ((addr_qw * 16) & vu->mem_mask);
}

inline void set_vi(VUState *vu, int r, uint32_t v) {
    if (r) vu->vi[r] = v & 0xFFFF;
}

inline int16_t vis(VUState *vu, int r) { return (int16_t)vu->vi[r]; }

void sync_pipes(VUState *vu) {
    if (vu->q_busy && vu->cycle >= vu->q_ready) {
        vu->q = vu->q_pending;
        vu->q_busy = 0;
    }
    if (vu->p_busy && vu->cycle >= vu->p_ready) {
        vu->p = vu->p_pending;
        vu->p_busy = 0;
    }
}

void q_pipeline_set(VUState *vu, float v, int latency) {
    if (vu->q_busy) {  // divider busy: stall until the previous result is out
        if (vu->cycle < vu->q_ready) vu->cycle = vu->q_ready;
        sync_pipes(vu);
    }
    vu->q_pending = v;
    vu->q_ready = vu->cycle + latency;
    vu->q_busy = 1;
}

void p_pipeline_set(VUState *vu, float v, int latency) {
    if (vu->p_busy) {
        if (vu->cycle < vu->p_ready) vu->cycle = vu->p_ready;
        sync_pipes(vu);
    }
    vu->p_pending = v;
    vu->p_ready = vu->cycle + latency;
    vu->p_busy = 1;
}

void flush_q(VUState *vu) {
    if (vu->q_busy) {
        if (vu->cycle < vu->q_ready) vu->cycle = vu->q_ready;
        vu->q = vu->q_pending;
        vu->q_busy = 0;
    }
}

void flush_p(VUState *vu) {
    if (vu->p_busy) {
        if (vu->cycle < vu->p_ready) vu->cycle = vu->p_ready;
        vu->p = vu->p_pending;
        vu->p_busy = 0;
    }
}

void lower_div(VUState *vu, uint32_t ins, int kind) {
    int fsf = (ins >> 21) & 3, ftf = (ins >> 23) & 3;
    float fs = vu_clamp(vu->vf[FS(ins)].f[fsf]), ft = vu_clamp(vu->vf[FT(ins)].f[ftf]);
    float r;
    vu->status &= ~0x30u;
    if (kind == 0) {  // DIV
        if (ft == 0.0f) {
            vu->status |= fs == 0.0f ? 0x10 : 0x20;
            uint32_t u = ((f2u(fs) ^ f2u(ft)) & 0x80000000u) | 0x7F7FFFFFu;
            memcpy(&r, &u, 4);
        } else {
            r = vu_clamp(fs / ft);
        }
        q_pipeline_set(vu, r, 7);
    } else if (kind == 1) {  // SQRT
        if (ft < 0) vu->status |= 0x10;
        r = sqrtf(fabsf(ft));
        q_pipeline_set(vu, r, 7);
    } else {  // RSQRT
        if (ft == 0.0f) {
            vu->status |= 0x20;
            uint32_t u = (f2u(fs) & 0x80000000u) | 0x7F7FFFFFu;
            memcpy(&r, &u, 4);
        } else {
            if (ft < 0) vu->status |= 0x10;
            r = vu_clamp(fs / sqrtf(fabsf(ft)));
        }
        q_pipeline_set(vu, r, 13);
    }
    vu->status |= (vu->status & 0x30) << 6;
}

bool g_vu_mute_kick;  // EOD_VU_CHECK: the second run of a program sends nothing

void xgkick(VUState *vu, uint32_t addr_qw) {
    if (g_vu_mute_kick) return;
    // send a GIF packet (possibly several tags) from VU1 memory via PATH1
    uint32_t a = addr_qw & 0x3FF;
    // walk tags until EOP to find the length (wrapping at 16KB)
    uint8_t buf[0x4000];
    uint32_t n = 0;
    for (;;) {
        if (n >= 0x400) break;
        uint64_t tag;
        memcpy(&tag, vu->mem + ((a + n) & 0x3FF) * 16, 8);
        uint32_t nloop = tag & 0x7FFF;
        bool eop = (tag >> 15) & 1;
        uint32_t flg = (tag >> 58) & 3;
        uint32_t nreg = (tag >> 60) & 15;
        if (!nreg) nreg = 16;
        uint32_t qw;
        if (flg == 0) qw = nloop * nreg;
        else if (flg == 1) qw = (nloop * nreg + 1) / 2;
        else qw = nloop;
        n += 1 + qw;
        if (eop) break;
    }
    if (n > 0x400) n = 0x400;
    for (uint32_t k = 0; k < n; k++) memcpy(buf + k * 16, vu->mem + ((a + k) & 0x3FF) * 16, 16);
    gif_transfer(GIF_PATH1, buf, n);
}

uint32_t rand_next(uint32_t r) {
    // VU random: 23-bit LFSR (taps per PCSX2)
    int x = (r >> 4) & 1;
    int y = (r >> 22) & 1;
    r <<= 1;
    r ^= x ^ y;
    return (r & 0x7FFFFF) | 0x3F800000;
}

void lower_ext(Ctx &c, uint32_t ins, uint32_t ext) {
    VUState *vu = c.vu;
    int dest = dest_of(ins);
    int ft = FT(ins), fs = FS(ins);
    switch (ext) {
    case 0x30:  // MOVE ft, fs
        if (ft)
            for (int k = 0; k < 4; k++)
                if (has(dest, k)) vu->vf[ft].f[k] = vu->vf[fs].f[k];
        return;
    case 0x31: {  // MR32
        if (!ft) return;
        VUReg s = vu->vf[fs];
        float r[4] = {s.f[1], s.f[2], s.f[3], s.f[0]};
        for (int k = 0; k < 4; k++)
            if (has(dest, k)) vu->vf[ft].f[k] = r[k];
        return;
    }
    case 0x34:  // LQI ft, (is++)
    case 0x36: {  // LQD ft, (--is)
        int is = fs;
        if (ext == 0x36) set_vi(vu, is, vu->vi[is] - 1);
        uint8_t *p = vu_qword(vu, vu->vi[is]);
        if (ft) {
            VUReg m;
            memcpy(&m, p, 16);
            for (int k = 0; k < 4; k++)
                if (has(dest, k)) vu->vf[ft].u[k] = m.u[k];
        }
        if (ext == 0x34) set_vi(vu, is, vu->vi[is] + 1);
        return;
    }
    case 0x35:  // SQI fs, (it++)
    case 0x37: {  // SQD fs, (--it)
        int it = ft;
        if (ext == 0x37) set_vi(vu, it, vu->vi[it] - 1);
        uint8_t *p = vu_qword(vu, vu->vi[it]);
        for (int k = 0; k < 4; k++)
            if (has(dest, k)) memcpy(p + k * 4, &vu->vf[fs].u[k], 4);
        if (ext == 0x35) set_vi(vu, it, vu->vi[it] + 1);
        return;
    }
    case 0x38: lower_div(vu, ins, 0); return;
    case 0x39: lower_div(vu, ins, 1); return;
    case 0x3A: lower_div(vu, ins, 2); return;
    case 0x3B: flush_q(vu); return;  // WAITQ
    case 0x3C: {  // MTIR it, fs.fsf
        int fsf = (ins >> 21) & 3;
        set_vi(vu, ft, vu->vf[fs].u[fsf]);
        return;
    }
    case 0x3D:  // MFIR ft, is
        if (ft)
            for (int k = 0; k < 4; k++)
                if (has(dest, k)) vu->vf[ft].s[k] = (int16_t)vu->vi[fs];
        return;
    case 0x3E: {  // ILWR it, (is)
        uint8_t *p = vu_qword(vu, vu->vi[fs]);
        for (int k = 0; k < 4; k++)
            if (has(dest, k)) {
                uint16_t v;
                memcpy(&v, p + k * 4, 2);
                set_vi(vu, ft, v);
            }
        return;
    }
    case 0x3F: {  // ISWR it, (is)
        uint8_t *p = vu_qword(vu, vu->vi[fs]);
        uint32_t v = vu->vi[ft] & 0xFFFF;
        for (int k = 0; k < 4; k++)
            if (has(dest, k)) memcpy(p + k * 4, &v, 4);
        return;
    }
    case 0x40:  // RNEXT
        vu->r = rand_next(vu->r);
        // fallthrough
    case 0x41:  // RGET
        if (ft)
            for (int k = 0; k < 4; k++)
                if (has(dest, k)) vu->vf[ft].u[k] = vu->r;
        return;
    case 0x42: {  // RINIT R, fs.fsf
        int fsf = (ins >> 21) & 3;
        vu->r = 0x3F800000 | (vu->vf[fs].u[fsf] & 0x7FFFFF);
        return;
    }
    case 0x43: {  // RXOR
        int fsf = (ins >> 21) & 3;
        vu->r = 0x3F800000 | ((vu->r ^ vu->vf[fs].u[fsf]) & 0x7FFFFF);
        return;
    }
    case 0x64:  // MFP
        if (ft)
            for (int k = 0; k < 4; k++)
                if (has(dest, k)) vu->vf[ft].f[k] = vu->p;
        return;
    case 0x68:  // XTOP
        set_vi(vu, ft, g_vif1_top);
        return;
    case 0x69:  // XITOP
        set_vi(vu, ft, g_vif1_itop);
        return;
    case 0x6C:  // XGKICK is
        xgkick(vu, vu->vi[fs]);
        return;
    case 0x7B:  // WAITP
        flush_p(vu);
        return;
    default:
        break;
    }
    // EFU
    const VUReg &s = vu->vf[fs];
    int fsf = (ins >> 21) & 3;
    float x = vu_clamp(s.f[0]), y = vu_clamp(s.f[1]), z = vu_clamp(s.f[2]), w = vu_clamp(s.f[3]);
    float v = vu_clamp(s.f[fsf]);
    float r;
    switch (ext) {
    case 0x70: r = x * x + y * y + z * z; p_pipeline_set(vu, r, 10); return;               // ESADD
    case 0x71: r = 1.0f / (x * x + y * y + z * z); p_pipeline_set(vu, r, 17); return;      // ERSADD
    case 0x72: r = sqrtf(x * x + y * y + z * z); p_pipeline_set(vu, r, 17); return;         // ELENG
    case 0x73: r = 1.0f / sqrtf(x * x + y * y + z * z); p_pipeline_set(vu, r, 23); return;  // ERLENG
    case 0x74: r = atanf(y / x); p_pipeline_set(vu, r, 53); return;                         // EATANxy
    case 0x75: r = atanf(z / x); p_pipeline_set(vu, r, 53); return;                         // EATANxz
    case 0x76: r = x + y + z + w; p_pipeline_set(vu, r, 11); return;                        // ESUM
    case 0x78: r = sqrtf(fabsf(v)); p_pipeline_set(vu, r, 11); return;                      // ESQRT
    case 0x79: r = 1.0f / sqrtf(fabsf(v)); p_pipeline_set(vu, vu_clamp(r), 17); return;    // ERSQRT
    case 0x7A: r = 1.0f / v; p_pipeline_set(vu, vu_clamp(r), 11); return;                  // ERCPR
    case 0x7C: r = sinf(v); p_pipeline_set(vu, r, 28); return;                              // ESIN
    case 0x7D: r = atanf(v); p_pipeline_set(vu, r, 53); return;                             // EATAN
    case 0x7E: r = expf(-v); p_pipeline_set(vu, r, 43); return;                             // EEXP
    }
    WARN("VU%d: unknown lower ext op %08x (ext %02x)", vu->index, ins, ext);
}

void lower_op(Ctx &c, uint32_t ins) {
    VUState *vu = c.vu;
    uint32_t op = ins & 0x3F;
    int id = FD(ins), is = FS(ins), it = FT(ins);
    switch (op) {
    case 0x30: set_vi(vu, id, vu->vi[is] + vu->vi[it]); return;  // IADD
    case 0x31: set_vi(vu, id, vu->vi[is] - vu->vi[it]); return;  // ISUB
    case 0x32: {                                                  // IADDI it, is, imm5
        int imm = (ins >> 6) & 31;
        if (imm & 0x10) imm -= 32;
        set_vi(vu, it, vu->vi[is] + imm);
        return;
    }
    case 0x34: set_vi(vu, id, vu->vi[is] & vu->vi[it]); return;  // IAND
    case 0x35: set_vi(vu, id, vu->vi[is] | vu->vi[it]); return;  // IOR
    case 0x3C: case 0x3D: case 0x3E: case 0x3F:
        lower_ext(c, ins, ((ins >> 6) & 31) << 2 | (ins & 3));
        return;
    }
    WARN("VU%d: unknown lower op %08x", vu->index, ins);
}

inline int32_t imm11(uint32_t ins) { return (int32_t)(ins << 21) >> 21; }
inline int32_t imm15(uint32_t ins) { return (int32_t)(((ins >> 10) & 0x7800) | (ins & 0x7FF)); }

void lower(Ctx &c, uint32_t ins, uint32_t pc) {
    VUState *vu = c.vu;
    uint32_t op = ins >> 25;
    int it = FT(ins), is = FS(ins);
    int dest = dest_of(ins);
    switch (op) {
    case 0x00: {  // LQ ft, imm(is)
        uint8_t *p = vu_qword(vu, vu->vi[is] + imm11(ins));
        if (it) {
            VUReg m;
            memcpy(&m, p, 16);
            for (int k = 0; k < 4; k++)
                if (has(dest, k)) vu->vf[it].u[k] = m.u[k];
        }
        return;
    }
    case 0x01: {  // SQ fs, imm(it)
        uint8_t *p = vu_qword(vu, vu->vi[it] + imm11(ins));
        for (int k = 0; k < 4; k++)
            if (has(dest, k)) memcpy(p + k * 4, &vu->vf[is].u[k], 4);
        return;
    }
    case 0x04: {  // ILW it, imm(is)
        uint8_t *p = vu_qword(vu, vu->vi[is] + imm11(ins));
        for (int k = 0; k < 4; k++)
            if (has(dest, k)) {
                uint16_t v;
                memcpy(&v, p + k * 4, 2);
                set_vi(vu, it, v);
            }
        return;
    }
    case 0x05: {  // ISW it, imm(is)
        uint8_t *p = vu_qword(vu, vu->vi[is] + imm11(ins));
        uint32_t v = vu->vi[it] & 0xFFFF;
        for (int k = 0; k < 4; k++)
            if (has(dest, k)) memcpy(p + k * 4, &v, 4);
        return;
    }
    case 0x08: set_vi(vu, it, vu->vi[is] + imm15(ins)); return;  // IADDIU
    case 0x09: set_vi(vu, it, vu->vi[is] - imm15(ins)); return;  // ISUBIU
    case 0x10: set_vi(vu, 1, (vu->clip & 0xFFFFFF) == (ins & 0xFFFFFF)); return;  // FCEQ
    case 0x11: vu->clip = ins & 0xFFFFFF; return;                                  // FCSET
    case 0x12: set_vi(vu, 1, (vu->clip & ins & 0xFFFFFF) != 0); return;           // FCAND
    case 0x13: set_vi(vu, 1, ((vu->clip | ins) & 0xFFFFFF) == 0xFFFFFF); return;  // FCOR
    case 0x14: set_vi(vu, it, (vu->status & 0xFFF) == ((ins >> 10 & 0x800) | (ins & 0x7FF))); return;  // FSEQ
    case 0x15: vu->status = (vu->status & 0x3F) | (((ins >> 10 & 0x800) | (ins & 0x7FF)) & 0xFC0); return;  // FSSET
    case 0x16: set_vi(vu, it, vu->status & ((ins >> 10 & 0x800) | (ins & 0x7FF))); return;  // FSAND
    case 0x17: set_vi(vu, it, vu->status | ((ins >> 10 & 0x800) | (ins & 0x7FF))); return;  // FSOR
    case 0x18: set_vi(vu, it, (vu->mac & 0xFFFF) == (vu->vi[is] & 0xFFFF)); return;  // FMEQ
    case 0x1A: set_vi(vu, it, vu->mac & vu->vi[is]); return;                         // FMAND
    case 0x1B: set_vi(vu, it, vu->mac | vu->vi[is]); return;                         // FMOR
    case 0x1C: set_vi(vu, it, vu->clip & 0xFFF); return;                             // FCGET
    case 0x20:  // B
    case 0x21:  // BAL
        if (op == 0x21) set_vi(vu, it, (pc + 16) / 8);
        c.branch = true;
        c.branch_target = pc + 8 + imm11(ins) * 8;
        return;
    case 0x24:  // JR is
    case 0x25:  // JALR it, is
        c.branch = true;
        c.branch_target = vu->vi[is] * 8;
        if (op == 0x25) set_vi(vu, it, (pc + 16) / 8);
        return;
    case 0x28: case 0x29: case 0x2C: case 0x2D: case 0x2E: case 0x2F: {
        bool take;
        int16_t a = vis(vu, is), b = vis(vu, it);
        switch (op) {
        case 0x28: take = (uint16_t)a == (uint16_t)b; break;
        case 0x29: take = (uint16_t)a != (uint16_t)b; break;
        case 0x2C: take = a < 0; break;
        case 0x2D: take = a > 0; break;
        case 0x2E: take = a <= 0; break;
        default: take = a >= 0; break;
        }
        if (take) {
            c.branch = true;
            c.branch_target = pc + 8 + imm11(ins) * 8;
        }
        return;
    }
    case 0x40:
        lower_op(c, ins);
        return;
    }
    WARN("VU%d: unknown lower instruction %08x at %04x", vu->index, ins, pc);
}

// --------------------------------------------------- hazard / stall model
struct RegUse {
    int reg;
    int mask;  // bit3 = x ... bit0 = w
};

inline int bcmask(int c) { return 8 >> c; }

// VF registers read by an upper instruction.
int upper_reads(uint32_t ins, RegUse *out) {
    uint32_t op = ins & 0x3F;
    int dest = dest_of(ins);
    int n = 0;
    if (op < 0x3C) {
        if (op >= 0x2E && op != 0x2F) {  // OPMSUB: cross product
            if (op == 0x2E) {
                out[n++] = {FS(ins), 0xE};
                out[n++] = {FT(ins), 0xE};
                return n;
            }
        }
        out[n++] = {FS(ins), dest};
        if (op < 0x1C) out[n++] = {FT(ins), bcmask(BC(ins))};
        else if (op >= 0x28) out[n++] = {FT(ins), dest};
        return n;
    }
    uint32_t ext = ((ins >> 6) & 31) << 2 | (ins & 3);
    if (ext == 0x2F) return 0;  // NOP
    if (ext == 0x1F) {          // CLIP
        out[n++] = {FS(ins), 0xE};
        out[n++] = {FT(ins), 1};
        return n;
    }
    if (ext == 0x2E) {  // OPMULA
        out[n++] = {FS(ins), 0xE};
        out[n++] = {FT(ins), 0xE};
        return n;
    }
    out[n++] = {FS(ins), dest};
    if (ext < 0x10 || (ext >= 0x18 && ext < 0x1C)) out[n++] = {FT(ins), bcmask(BC(ins))};
    else if (ext >= 0x28) out[n++] = {FT(ins), dest};
    return n;
}

// VF registers read / written by a micro-mode lower instruction.
int lower_reads(uint32_t ins, RegUse *out, RegUse *write) {
    uint32_t op = ins >> 25;
    int dest = dest_of(ins);
    int n = 0;
    write->reg = 0;
    switch (op) {
    case 0x00: write->reg = FT(ins); write->mask = dest; return 0;  // LQ
    case 0x01: out[n++] = {FS(ins), dest}; return n;               // SQ
    case 0x40: {
        uint32_t f = ins & 0x3F;
        if (f < 0x3C) return 0;
        uint32_t ext = ((ins >> 6) & 31) << 2 | (ins & 3);
        int fsf = (ins >> 21) & 3, ftf = (ins >> 23) & 3;
        switch (ext) {
        case 0x30: case 0x31:  // MOVE, MR32
            out[n++] = {FS(ins), ext == 0x31 ? 0xF : dest};
            write->reg = FT(ins);
            write->mask = dest;
            return n;
        case 0x34: case 0x36: write->reg = FT(ins); write->mask = dest; return 0;  // LQI/LQD
        case 0x35: case 0x37: out[n++] = {FS(ins), dest}; return n;              // SQI/SQD
        case 0x38: case 0x3A:  // DIV, RSQRT
            out[n++] = {FS(ins), bcmask(fsf)};
            out[n++] = {FT(ins), bcmask(ftf)};
            return n;
        case 0x39: out[n++] = {FT(ins), bcmask(ftf)}; return n;  // SQRT
        case 0x3C: case 0x42: case 0x43: out[n++] = {FS(ins), bcmask(fsf)}; return n;  // MTIR/RINIT/RXOR
        case 0x3D: case 0x40: case 0x41: case 0x64:  // MFIR, RNEXT, RGET, MFP
            write->reg = FT(ins);
            write->mask = dest;
            return 0;
        default:
            if (ext >= 0x70) out[n++] = {FS(ins), 0xF};  // EFU
            return n;
        }
    }
    }
    return 0;
}

// Decoded instruction pairs, revalidated against micro memory on every use.
struct DecPair {
    uint32_t lo, hi;
    bool valid, waitq;
    int nu;
    RegUse uses[4];
    RegUse lw;
    UpFn up;
    uint64_t use_lo, use_hi;  // VF components read (scoreboard bits, 4 per register)
    uint64_t lw_lo, lw_hi;    // VF components written by the lower instruction
};
DecPair g_dec[2][0x4000 / 8];

inline void reg_bits(int reg, int mask, uint64_t &lo, uint64_t &hi) {
    if (!reg) return;
    if (reg < 16) lo |= (uint64_t)(mask & 15) << (reg * 4);
    else hi |= (uint64_t)(mask & 15) << ((reg - 16) * 4);
}

// FMAC results are readable 4 cycles after issue: slot (cycle & 3) holds the
// VF components written by the pair issued at `stamp`.
struct Scoreboard {
    uint64_t lo[4], hi[4], stamp[4];
};
Scoreboard g_score[2];

void decode_pair(DecPair &d, uint32_t lo, uint32_t hi) {
    d.lo = lo;
    d.hi = hi;
    d.valid = true;
    d.lw = {0, 0};
    d.nu = upper_reads(hi, d.uses);
    if (!(hi & 0x80000000u)) d.nu += lower_reads(lo, d.uses + d.nu, &d.lw);
    // drop reads of VF0 (constant)
    int n = 0;
    for (int k = 0; k < d.nu; k++)
        if (d.uses[k].reg) d.uses[n++] = d.uses[k];
    d.nu = n;
    // WAITQ stalls the whole pair: the upper instruction sees the new Q
    d.waitq = !(hi & 0x80000000u) && (lo >> 25) == 0x40 && (lo & 0x3C) == 0x3C &&
              ((lo & 3) | ((lo >> 4) & 0x7C)) == 0x3B;
    d.up = decode_upper(hi);
    d.use_lo = d.use_hi = d.lw_lo = d.lw_hi = 0;
    for (int k = 0; k < d.nu; k++) reg_bits(d.uses[k].reg, d.uses[k].mask, d.use_lo, d.use_hi);
    reg_bits(d.lw.reg, d.lw.mask, d.lw_lo, d.lw_hi);
}

}  // namespace

// Bumped when micro memory is loaded (VIF MPG), for EOD_VU_DUMP.
uint32_t g_vu_micro_gen[2];

namespace {
// EOD_VU_DUMP=dir: writes each distinct VU micro memory image that runs
// (dir/vu<n>_<hash>.bin) and lists the entry points (dir/vu_entries.txt):
// the input of tools/vurecomp.py.
void vu_dump(VUState *vu, uint32_t pc) {
    static const char *dir = getenv("EOD_VU_DUMP");
    if (!dir) return;
    static uint32_t seen_gen[2] = {~0u, ~0u};
    static uint64_t cur_hash[2];
    static std::set<std::pair<uint64_t, uint32_t>> entries;
    int n = vu->index;
    uint32_t size = vu->micro_mask + 1;
    if (seen_gen[n] != g_vu_micro_gen[n]) {
        seen_gen[n] = g_vu_micro_gen[n];
        uint64_t h = 1469598103934665603ull;
        for (uint32_t i = 0; i < size; i++) h = (h ^ vu->micro[i]) * 1099511628211ull;
        cur_hash[n] = h;
        char path[1024];
        snprintf(path, sizeof path, "%s/vu%d_%016llx.bin", dir, n, (unsigned long long)h);
        if (FILE *f = fopen(path, "rb")) {
            fclose(f);
        } else if ((f = fopen(path, "wb"))) {
            fwrite(vu->micro, 1, size, f);
            fclose(f);
        }
    }
    if (entries.insert({cur_hash[n] ^ (uint64_t)n, pc}).second) {
        char path[1024];
        snprintf(path, sizeof path, "%s/vu_entries.txt", dir);
        if (FILE *f = fopen(path, "a")) {
            fprintf(f, "vu%d %016llx %04x\n", n, (unsigned long long)cur_hash[n], pc);
            fclose(f);
        }
    }
}
}  // namespace

// --------------------------------------------------------------- micro mode
// Interprets from `pc` until the program ends.
static void vu_interp(VUState *vu, uint32_t pc) {
    uint32_t start_pc = pc;
    Ctx c;
    c.vu = vu;
    bool in_delay = false;
    uint32_t delay_target = 0;
    int end_countdown = -1;
    uint64_t guard = 0;
    vu->running = 1;
    LOG(LOG_VU, "VU%d start at %04x", vu->index, pc);
    for (;;) {
        if (++guard > 50000000) {
            WARN("VU%d: program at %04x does not terminate", vu->index, start_pc);
            break;
        }
        uint32_t lo, hi;
        memcpy(&lo, vu->micro + pc, 4);
        memcpy(&hi, vu->micro + pc + 4, 4);
        DecPair &d = g_dec[vu->index][pc >> 3];
        if (!d.valid || d.lo != lo || d.hi != hi) decode_pair(d, lo, hi);
        // stall until every register this pair reads has been written
        Scoreboard &sb = g_score[vu->index];
        if (d.use_lo | d.use_hi) {
            for (uint64_t dd = 1; dd <= 3; dd++) {
                uint64_t ic = vu->cycle - dd;
                int sl = (int)(ic & 3);
                if (sb.stamp[sl] == ic && ((sb.lo[sl] & d.use_lo) | (sb.hi[sl] & d.use_hi))) {
                    vu->cycle = ic + 4;
                    break;
                }
            }
        }
        if (d.waitq) flush_q(vu);
        sync_pipes(vu);
        const uint64_t lw_lo = d.lw_lo, lw_hi = d.lw_hi;
        // upper first (computed against old state), committed after lower
        c.up_write = false;
        c.mac_set = false;
        c.branch = false;
        d.up(c, hi);
        bool up_vf = c.up_write && !c.up_acc;
        int up_reg = c.up_reg, up_dest = c.up_dest;
        if (hi & 0x80000000u) {
            // I bit: lower word is a float immediate for the I register
            commit_upper(c);
            memcpy(&vu->i, &lo, 4);
        } else {
            lower(c, lo, pc);
            commit_upper(c);
        }
        {
            uint64_t wlo = lw_lo, whi = lw_hi;
            if (up_vf) reg_bits(up_reg, up_dest, wlo, whi);
            if (wlo | whi) {
                int sl = (int)(vu->cycle & 3);
                if (sb.stamp[sl] != vu->cycle) {
                    sb.stamp[sl] = vu->cycle;
                    sb.lo[sl] = sb.hi[sl] = 0;
                }
                sb.lo[sl] |= wlo;
                sb.hi[sl] |= whi;
            }
        }
        vu->cycle++;
        if (hi & 0x40000000u) end_countdown = 1;  // E bit: one more pair
        if (hi & 0x08000000u) { /* T bit: debug */ }
        if (hi & 0x10000000u) { /* D bit: debug */ }
        uint32_t next = (pc + 8) & vu->micro_mask;
        if (in_delay) {
            next = delay_target & vu->micro_mask;
            in_delay = false;
        }
        if (c.branch) {
            in_delay = true;
            delay_target = c.branch_target;
        }
        if (end_countdown == 0) break;
        if (end_countdown > 0) end_countdown--;
        pc = next;
    }
    flush_q(vu);
    flush_p(vu);
    vu->tpc = pc;
    vu->running = 0;
}

// Micro programs compiled ahead of time by tools/vurecomp.py (gen/vu_rec.inc):
// used when the code in micro memory matches what was compiled.
namespace {
struct VuRange {
    uint16_t addr, n;  // byte address, words
    const uint32_t *words;
};
struct VuProg {
    int vu;
    uint32_t pc;
    void (*fn)(VUState *vu, uint32_t entry);
    int nranges;
    const VuRange *ranges;
};
}  // namespace

// Scoreboard stall / write for compiled code (same as vu_interp's).
#define VU_STALL(ulo, uhi)                                                                    \
    do {                                                                                      \
        for (uint64_t dd = 1; dd <= 3; dd++) {                                                \
            uint64_t ic = vu->cycle - dd;                                                     \
            int sl = (int)(ic & 3);                                                           \
            if (sb.stamp[sl] == ic && ((sb.lo[sl] & (ulo)) | (sb.hi[sl] & (uhi)))) {          \
                vu->cycle = ic + 4;                                                           \
                break;                                                                        \
            }                                                                                 \
        }                                                                                     \
    } while (0)
#define VU_SCORE(wlo, whi)                         \
    do {                                           \
        int sl = (int)(vu->cycle & 3);             \
        if (sb.stamp[sl] != vu->cycle) {           \
            sb.stamp[sl] = vu->cycle;              \
            sb.lo[sl] = sb.hi[sl] = 0;             \
        }                                          \
        sb.lo[sl] |= (wlo);                        \
        sb.hi[sl] |= (whi);                        \
    } while (0)

#if __has_include("vu_rec.inc")
#define VU_REC 1
namespace {
#include "vu_rec.inc"
}  // namespace
#else
namespace {
const VuProg g_vu_progs[1] = {{-1, 0, nullptr, 0, nullptr}};
const int g_vu_nprogs = 0;
}  // namespace
#endif

namespace {
struct RecSlot {
    uint32_t gen = ~0u;
    const VuProg *prog = nullptr;
};
RecSlot g_rec_slot[2][0x4000 / 8];

bool vu_rec_matches(const VUState *vu, const VuProg &p) {
    for (int i = 0; i < p.nranges; i++) {
        const VuRange &r = p.ranges[i];
        if (memcmp(vu->micro + r.addr, r.words, (size_t)r.n * 4) != 0) return false;
    }
    return true;
}

const VuProg *vu_rec_find(VUState *vu, uint32_t pc) {
    static bool off = getenv("EOD_VU_INTERP") != nullptr;
    if (off || !g_vu_nprogs) return nullptr;
    RecSlot &s = g_rec_slot[vu->index][pc >> 3];
    if (s.gen == g_vu_micro_gen[vu->index]) return s.prog;
    s.gen = g_vu_micro_gen[vu->index];
    s.prog = nullptr;
    for (int i = 0; i < g_vu_nprogs; i++) {
        const VuProg &p = g_vu_progs[i];
        if (p.vu == vu->index && p.pc == pc && vu_rec_matches(vu, p)) {
            s.prog = &p;
            break;
        }
    }
    return s.prog;
}
}  // namespace

void vu_execute(VUState *vu, uint32_t start_pc) {
    uint32_t pc = start_pc & vu->micro_mask & ~7u;
    vu_dump(vu, pc);
    if (const VuProg *p = vu_rec_find(vu, pc)) {
        static bool check = getenv("EOD_VU_CHECK") != nullptr;
        if (!check) {
            p->fn(vu, pc);
            return;
        }
        // run the compiled program, then the interpreter from the same state, and compare
        // (except MAC/status, which programs that never read them don't compute)
        static VUState before;
        static uint8_t mem_before[0x4000];
        static Scoreboard sb_before;
        uint32_t msize = vu->mem_mask + 1;
        before = *vu;
        memcpy(mem_before, vu->mem, msize);
        sb_before = g_score[vu->index];
        p->fn(vu, pc);
        static VUState after;
        static uint8_t mem_after[0x4000];
        after = *vu;
        memcpy(mem_after, vu->mem, msize);
        Scoreboard sb_after = g_score[vu->index];
        *vu = before;
        memcpy(vu->mem, mem_before, msize);
        g_score[vu->index] = sb_before;
        g_vu_mute_kick = true;
        vu_interp(vu, pc);
        g_vu_mute_kick = false;
        bool same = memcmp(vu->vf, after.vf, sizeof vu->vf) == 0 && memcmp(vu->vi, after.vi, sizeof vu->vi) == 0 &&
                    memcmp(&vu->acc, &after.acc, sizeof vu->acc) == 0 && vu->q == after.q && vu->p == after.p &&
                    vu->i == after.i && vu->clip == after.clip && vu->cycle == after.cycle && vu->tpc == after.tpc &&
                    memcmp(vu->mem, mem_after, msize) == 0 &&
                    memcmp(&g_score[vu->index], &sb_after, sizeof sb_after) == 0;
        static int reports;
        if (!same && reports++ < 20) {
            WARN("VU%d program %04x: compiled code differs from the interpreter", vu->index, pc);
            for (int r = 0; r < 32; r++)
                if (memcmp(&vu->vf[r], &after.vf[r], 16))
                    WARN("  vf%d interp %08x %08x %08x %08x compiled %08x %08x %08x %08x", r, vu->vf[r].u[0],
                         vu->vf[r].u[1], vu->vf[r].u[2], vu->vf[r].u[3], after.vf[r].u[0], after.vf[r].u[1],
                         after.vf[r].u[2], after.vf[r].u[3]);
            for (int r = 0; r < 32; r++)
                if (vu->vi[r] != after.vi[r]) WARN("  vi%d interp %x compiled %x", r, vu->vi[r], after.vi[r]);
            WARN("  cycle %llu/%llu status %x/%x mac %x/%x clip %x/%x q %g/%g tpc %x/%x mem %s",
                 (unsigned long long)vu->cycle, (unsigned long long)after.cycle, vu->status, after.status, vu->mac,
                 after.mac, vu->clip, after.clip, vu->q, after.q, vu->tpc, after.tpc,
                 memcmp(vu->mem, mem_after, msize) ? "differs" : "same");
        }
        return;
    }
    vu_interp(vu, pc);
}

// --------------------------------------------------------------- macro mode
extern "C" void vu0_macro(uint32_t ins) {
    VUState *vu = &g_vu0;
    Ctx c;
    c.vu = vu;
    // macro mode has no pipeline delays visible to software (interlocks)
    flush_q(vu);
    flush_p(vu);
    uint32_t f = ins & 0x3F;
    if (f < 0x30) {
        upper(c, ins);
        commit_upper(c);
    } else if (f >= 0x30 && f <= 0x35) {
        lower_op(c, ins);
    } else if (f == 0x38 || f == 0x39) {
        uint32_t addr = f == 0x38 ? ((ins >> 6) & 0x7FFF) * 8 : vu->cmsar * 8;
        vu_execute(vu, addr);
    } else if (f >= 0x3C) {
        uint32_t ext = ((ins >> 6) & 31) << 2 | (ins & 3);
        if (ext < 0x30) {
            upper(c, ins);
            commit_upper(c);
        } else {
            lower_ext(c, ins, ext);
        }
    } else {
        WARN("VU0 macro: unknown %08x", ins);
    }
    flush_q(vu);
    flush_p(vu);
}

extern "C" void vu0_sync(void) {}

extern "C" uint32_t vu0_cfc2(int reg) {
    VUState *vu = &g_vu0;
    if (reg < 16) return vu->vi[reg] & 0xFFFF;
    switch (reg) {
    case REG_STATUS: return vu->status;
    case REG_MAC: return vu->mac;
    case REG_CLIP: return vu->clip;
    case REG_R: return vu->r;
    case REG_I: return f2u(vu->i);
    case REG_Q: return f2u(vu->q);
    case REG_P: return f2u(vu->p);
    case REG_TPC: return vu->tpc / 8;
    case REG_CMSAR0: return vu->cmsar;
    case REG_VPU_STAT: return 0;  // nothing ever runs asynchronously
    case REG_CMSAR1: return g_vu1.cmsar;
    }
    return 0;
}

extern "C" void vu0_ctc2(int reg, uint32_t v) {
    VUState *vu = &g_vu0;
    if (reg < 16) {
        set_vi(vu, reg, v);
        return;
    }
    switch (reg) {
    case REG_STATUS: vu->status = (vu->status & 0x3F) | (v & 0xFC0); break;
    case REG_MAC: break;
    case REG_CLIP: vu->clip = v & 0xFFFFFF; break;
    case REG_R: vu->r = 0x3F800000 | (v & 0x7FFFFF); break;
    case REG_I: memcpy(&vu->i, &v, 4); break;
    case REG_Q: memcpy(&vu->q, &v, 4); break;
    case REG_P: memcpy(&vu->p, &v, 4); break;
    case REG_CMSAR0: vu->cmsar = v & 0xFFFF; break;
    case REG_FBRST:
        if (v & 2) { /* reset VU0 */ }
        if (v & 0x200) { /* reset VU1 */ }
        break;
    case REG_CMSAR1:
        g_vu1.cmsar = v & 0xFFFF;
        vu_execute(&g_vu1, (v & 0xFFFF) * 8);
        break;
    default:
        break;
    }
}
