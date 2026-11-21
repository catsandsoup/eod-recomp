// vu.cpp - Vector Unit interpreter (VU0 macro + micro mode, VU1 micro mode).
//
// Instruction pairs execute as on hardware: the upper (FMAC) result is
// computed from the old register state and committed after the lower
// instruction, Q/P results become visible after their latency, branches
// have a delay slot and the E bit ends the program after the next pair.
#include "../core/r7_audit.h"
#include "../render/native_renderer.h"
#include "../render/world_visibility.h"
#include "vu.h"
#include "core/trace.h"
#include "../core/prof.h"

#include "sse_compat.h"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <set>
#include <utility>
#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <chrono>
#include <vector>
#include <cstdio>
#include <cstdlib>

#include "../core/log.h"
#include "../gs/gs.h"
#include "../hw/offload.h"
#include "../kernel/kernel.h"
#include "ee.h"
#include "scene_capture.h"
#include "scene_emit.h"
#include "vif.h"

#ifdef EOD_VULKAN
#define XXH_INLINE_ALL
#include <xxhash.h>
#endif

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

// Flag results in flight (micro mode): visible 4 cycles after issue.
__attribute__((always_inline)) inline void flag_apply(VUState *vu, const VUFlagUpd &u) {
    if (u.kind == 0) {
        vu->mac = u.val;
        set_status_from_mac(vu, u.val);
    } else {
        vu->clip = ((vu->clip << 6) | u.val) & 0xFFFFFF;
    }
}

// Makes the flag results that have arrived by the current cycle visible.
__attribute__((always_inline)) inline void vu_flag_sync(VUState *vu) {
    while (vu->fq_n && vu->fq[vu->fq_head].ready <= vu->cycle) {
        flag_apply(vu, vu->fq[vu->fq_head]);
        vu->fq_head = (vu->fq_head + 1) & 7;
        vu->fq_n--;
    }
}

// End of a micro program: everything in flight arrives.
inline void vu_flag_flush(VUState *vu) {
    while (vu->fq_n) {
        flag_apply(vu, vu->fq[vu->fq_head]);
        vu->fq_head = (vu->fq_head + 1) & 7;
        vu->fq_n--;
    }
}

__attribute__((always_inline)) inline void vu_flag_push(VUState *vu, uint32_t kind, uint32_t val) {
    VUFlagUpd u = {vu->cycle + 4, val, kind};
    if (!vu->flag_pipe) {  // macro mode: no visible delay
        flag_apply(vu, u);
        return;
    }
    vu_flag_sync(vu);
    if (vu->fq_n == 8) {  // (can't happen: at most 4 are in flight)
        flag_apply(vu, vu->fq[vu->fq_head]);
        vu->fq_head = (vu->fq_head + 1) & 7;
        vu->fq_n--;
    }
    vu->fq[(vu->fq_head + vu->fq_n) & 7] = u;
    vu->fq_n++;
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
    if (c.mac_set) vu_flag_push(vu, 0, c.mac);
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
            else if (f != f) r.s[k] = (int32_t)0x80000000;  // NaN: as x86 cvttss
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
        vu_flag_push(vu, 1, f);
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

__attribute__((always_inline)) inline void sync_pipes(VUState *vu) {
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

// DIV / SQRT / RSQRT on the selected fields' values
void lower_div_f(VUState *vu, int kind, float fs, float ft) {
    fs = vu_clamp(fs);
    ft = vu_clamp(ft);
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

void lower_div(VUState *vu, uint32_t ins, int kind) {
    int fsf = (ins >> 21) & 3, ftf = (ins >> 23) & 3;
    lower_div_f(vu, kind, vu->vf[FS(ins)].f[fsf], vu->vf[FT(ins)].f[ftf]);
}

bool g_vu_mute_kick;  // differential checks execute the reference without sending it to GS

struct VuKickCheck {
    struct Packet {
        unsigned address, qwords;
        const VUReg *data;
    } expected[2];
    unsigned seen = 0;
    bool equal = true;
};
thread_local VuKickCheck *g_vu_kick_check;

void xgkick(VUState *vu, uint32_t addr_qw) {
    if (g_vu_mute_kick) {
        if (auto *check = g_vu_kick_check) {
            const unsigned index = check->seen++;
            if (index >= 2 || check->expected[index].address != addr_qw) {
                check->equal = false;
            } else {
                const auto &packet = check->expected[index];
                if (memcmp(vu->mem + 16 * addr_qw, packet.data, 16 * packet.qwords))
                    check->equal = false;
            }
        }
        return;
    }
    // send a GIF packet (possibly several tags) from VU1 memory via PATH1
    uint32_t a = addr_qw & 0x3FF;
    // walk tags until EOP to find the length (wrapping at 16KB)
    static thread_local uint8_t buf[0x4000];  // (not on the stack: 16 KB there means a 4-page probe per kick)
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
    if (a + n <= 0x400) {  // contiguous: straight from VU memory
        gif_transfer(GIF_PATH1, vu->mem + a * 16, n);
        return;
    }
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

void lower_efu(VUState *vu, uint32_t ins, uint32_t ext, const VUReg &s);

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
    lower_efu(vu, ins, ext, vu->vf[fs]);
}

// EFU instructions (ESUM, ERSQRT, ...) on the value of fs
void lower_efu(VUState *vu, uint32_t ins, uint32_t ext, const VUReg &s) {
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
uint64_t micro_image_id(const VUState &vu) {
    // Use the same fingerprint as existing EOD_VU_DUMP files.
    static uint32_t seen_gen[2] = {~0u, ~0u};
    static uint64_t current[2];
    const int unit = vu.index;
    if (seen_gen[unit] != g_vu_micro_gen[unit]) {
        uint64_t hash = 1469598103934665603ull;
        for (uint32_t i = 0; i <= vu.micro_mask; ++i)
            hash = (hash ^ vu.micro[i]) * 1099511628211ull;
        seen_gen[unit] = g_vu_micro_gen[unit];
        current[unit] = hash;
    }
    return current[unit];
}

bool census_enabled() {
    static const bool enabled = [] {
        const char *setting = getenv("EOD_R7_VU_CENSUS");
        return setting && setting[0] == '1' && setting[1] == '\0';
    }();
    return enabled;
}

struct CensusKey {
    std::string_view kind;
    uint32_t address;
    uint64_t image;
    uint32_t context;

    bool operator<(const CensusKey &other) const {
        return std::tie(kind, address, image, context) <
               std::tie(other.kind, other.address, other.image, other.context);
    }
};

// VU0 belongs to the EE. VU1 belongs to the offload path, which also runs
// synchronously when no worker is active. Report only after the worker joins.
struct Census {
    std::map<CensusKey, uint64_t> rows;
    uint64_t dropped = 0;
};
Census g_census[2];

void census_add(int unit, const CensusKey &key) {
    constexpr size_t kMaxRows = 16384;
    auto &census = g_census[unit];
    auto it = census.rows.lower_bound(key);
    if (it != census.rows.end() && !(key < it->first)) {
        ++it->second;
    } else if (census.rows.size() < kMaxRows) {
        census.rows.emplace_hint(it, key, 1);
    } else {
        ++census.dropped;
    }
}

void census_micro(const VUState &vu, uint32_t pc, VuLaunch launch, uint32_t ee_ra) {
    static constexpr const char *names[] = {"direct", "mscal", "mscalf", "mscnt", "vcallms", "vcallmsr", "cmsar1"};
    const bool vif = launch == VuLaunch::Mscal || launch == VuLaunch::Mscalf || launch == VuLaunch::Mscnt;
    const uint32_t context = vif && vu.index == 1 ? g_vif1_itop : ee_ra;
    census_add(vu.index, {names[static_cast<unsigned>(launch)], pc, micro_image_id(vu), context});
}

void census_retire(const VUState &vu, uint32_t pc, const char *kind) {
    if (census_enabled())
        census_add(vu.index, {kind, pc, micro_image_id(vu), g_vif1_itop});
}

// EOD_VU_DUMP=dir: writes each distinct VU micro memory image that runs
// (dir/vu<n>_<hash>.bin) and lists the entry points (dir/vu_entries.txt):
// the input of tools/vurecomp.py.
void vu_dump(VUState *vu, uint32_t pc) {
    static const char *dir = getenv("EOD_VU_DUMP");
    if (!dir) return;
    static uint32_t seen_gen[2] = {~0u, ~0u};
    static std::set<std::pair<uint64_t, uint32_t>> entries;
    int n = vu->index;
    uint32_t size = vu->micro_mask + 1;
    const uint64_t hash = micro_image_id(*vu);
    if (seen_gen[n] != g_vu_micro_gen[n]) {
        seen_gen[n] = g_vu_micro_gen[n];
        char path[1024];
        snprintf(path, sizeof path, "%s/vu%d_%016llx.bin", dir, n, (unsigned long long)hash);
        if (FILE *f = fopen(path, "rb")) {
            fclose(f);
        } else if ((f = fopen(path, "wb"))) {
            fwrite(vu->micro, 1, size, f);
            fclose(f);
        }
    }
    if (entries.insert({hash ^ (uint64_t)n, pc}).second) {
        char path[1024];
        snprintf(path, sizeof path, "%s/vu_entries.txt", dir);
        if (FILE *f = fopen(path, "a")) {
            fprintf(f, "vu%d %016llx %04x\n", n, (unsigned long long)hash, pc);
            fclose(f);
        }
    }
}
}  // namespace

void vu_report_census() {
    if (!census_enabled()) return;
    for (int unit = 0; unit < 2; ++unit) {
        const auto &census = g_census[unit];
        for (const auto &[key, count] : census.rows)
            fprintf(stderr, "[r7-vu] vu=%d kind=%s address=%08x image=%016llx context=%08x count=%llu\n",
                    unit, key.kind.data(), key.address, (unsigned long long)key.image, key.context,
                    (unsigned long long)count);
        fprintf(stderr, "[r7-vu] vu=%d rows=%zu dropped=%llu\n",
                unit, census.rows.size(), (unsigned long long)census.dropped);
    }
}

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
    vu->flag_pipe = 1;
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
        vu_flag_sync(vu);
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
    vu_flag_flush(vu);
    vu->flag_pipe = 0;
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

// The same on the compiled code's local cycle counter `cyc` (vu->cycle is
// brought up to date before helpers that use it): compiled code checks the
// scoreboard only in a block's first three pairs (later ones can only wait
// for writes of the block itself, resolved at compile time) and records only
// the writes of its last three pairs (the only ones a successor can see).
#define VU_STALL_C(ulo, uhi)                                                                  \
    do {                                                                                      \
        for (uint64_t dd = 1; dd <= 3; dd++) {                                                \
            uint64_t ic = cyc - dd;                                                           \
            int sl = (int)(ic & 3);                                                           \
            if (sb.stamp[sl] == ic && ((sb.lo[sl] & (ulo)) | (sb.hi[sl] & (uhi)))) {          \
                if (cyc < ic + 4) cyc = ic + 4;                                               \
                break;                                                                        \
            }                                                                                 \
        }                                                                                     \
    } while (0)
#define VU_SCORE_C(wlo, whi)                       \
    do {                                           \
        int sl = (int)(cyc & 3);                   \
        if (sb.stamp[sl] != cyc) {                 \
            sb.stamp[sl] = cyc;                    \
            sb.lo[sl] = sb.hi[sl] = 0;             \
        }                                          \
        sb.lo[sl] |= (wlo);                        \
        sb.hi[sl] |= (whi);                        \
    } while (0)

// The public interpreter build excludes the captured reference tables.
#ifndef EOD_PUBLIC_VU_INTERPRETER
#if __has_include("vu_rec.inc")
#define VU_REC 1
#endif
#endif

#ifdef VU_REC
namespace {
// Helpers for the compiled programs: upper FMAC results as SSE vectors
// (same IEEE single operations as apply_t, lanes with an Inf/NaN result
// redone by the scalar code), committed under the dest mask.
alignas(16) const uint32_t g_vu_dmask[16][4] = {
#define M(d) {(d) & 8 ? ~0u : 0u, (d) & 4 ? ~0u : 0u, (d) & 2 ? ~0u : 0u, (d) & 1 ? ~0u : 0u}
    M(0), M(1), M(2), M(3), M(4), M(5), M(6), M(7), M(8), M(9), M(10), M(11), M(12), M(13), M(14), M(15)
#undef M
};

template <ArOp OP, int SRC>
__attribute__((always_inline)) inline __m128 up_val(VUState *vu, uint32_t ins) {
    __m128 a = _mm_loadu_ps(vu->vf[FS(ins)].f);
    __m128 b;
    if (SRC == S_VEC) b = _mm_loadu_ps(vu->vf[FT(ins)].f);
    else b = _mm_set1_ps(SRC == S_BC ? vu->vf[FT(ins)].f[ins & 3] : SRC == S_Q ? vu->q : vu->i);
    if (OP == OP_MAX || OP == OP_MIN) {
        // compare the clamped values (Inf/NaN patterns are +-FLT_MAX), return the operand's bits
        const __m128i ex = _mm_set1_epi32(0x7F800000);
        const __m128 sign = _mm_castsi128_ps(_mm_set1_epi32((int)0x80000000u));
        const __m128 big = _mm_castsi128_ps(_mm_set1_epi32(0x7F7FFFFF));
        auto clamp = [&](__m128 x) {
            __m128 sp = _mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(x), ex), ex));
            __m128 c = _mm_or_ps(_mm_and_ps(x, sign), big);
            return _mm_or_ps(_mm_and_ps(sp, c), _mm_andnot_ps(sp, x));
        };
        __m128 m = OP == OP_MAX ? _mm_cmpgt_ps(clamp(a), clamp(b)) : _mm_cmplt_ps(clamp(a), clamp(b));
        return _mm_or_ps(_mm_and_ps(m, a), _mm_andnot_ps(m, b));
    }
    __m128 acc = _mm_loadu_ps(vu->acc.f), r;
    switch (OP) {
    case OP_ADD: r = _mm_add_ps(a, b); break;
    case OP_SUB: r = _mm_sub_ps(a, b); break;
    case OP_MUL: r = _mm_mul_ps(a, b); break;
    case OP_MADD: r = _mm_add_ps(acc, _mm_mul_ps(a, b)); break;
    default: r = _mm_sub_ps(acc, _mm_mul_ps(a, b)); break;
    }
    const __m128 ex = _mm_castsi128_ps(_mm_set1_epi32(0x7F800000));
    if (__builtin_expect(_mm_movemask_ps(_mm_cmpeq_ps(_mm_and_ps(r, ex), ex)) != 0, 0)) {
        VUReg s, t, ac, o;
        _mm_storeu_ps(s.f, a);
        _mm_storeu_ps(t.f, b);
        _mm_storeu_ps(ac.f, acc);
        for (int k = 0; k < 4; k++) o.f[k] = apply(OP, ac.f[k], s.f[k], t.f[k]);
        r = _mm_loadu_ps(o.f);
    }
    return r;
}

template <int DEST>
__attribute__((always_inline)) inline void vu_store(float *dst, __m128 v) {
    if (DEST == 15) {
        _mm_storeu_ps(dst, v);
    } else {
        __m128 m = _mm_load_ps(reinterpret_cast<const float *>(g_vu_dmask[DEST]));
        _mm_storeu_ps(dst, _mm_or_ps(_mm_and_ps(m, v), _mm_andnot_ps(m, _mm_loadu_ps(dst))));
    }
}

__attribute__((always_inline)) inline void vu_flags(VUState *vu, __m128 v, int dest) {
    VUReg r;
    _mm_storeu_ps(r.f, v);
    vu_flag_push(vu, 0, mac_for(r, dest));
}

// ITOF0/4/12/15, FTOI0/4/12/15 (saturating, truncating), ABS, CLIP
__attribute__((always_inline)) inline __m128 vu_itof(VUState *vu, int fs, int n) {
    static const float sc[4] = {1.0f, 1.0f / 16, 1.0f / 4096, 1.0f / 32768};
    __m128i s = _mm_loadu_si128(reinterpret_cast<const __m128i *>(vu->vf[fs].f));
    return _mm_mul_ps(_mm_cvtepi32_ps(s), _mm_set1_ps(sc[n]));
}

__attribute__((always_inline)) inline __m128 vu_ftoi(VUState *vu, int fs, int n) {
    static const float sc[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
    __m128 f = _mm_mul_ps(_mm_loadu_ps(vu->vf[fs].f), _mm_set1_ps(sc[n]));
    // cvttps gives 0x80000000 out of range (as the scalar conversion does
    // for NaN and large negatives); large positives saturate to 0x7FFFFFFF
    __m128i r = _mm_cvttps_epi32(f);
    __m128i big = _mm_castps_si128(_mm_cmpge_ps(f, _mm_set1_ps(2147483647.0f)));
    return _mm_castsi128_ps(_mm_xor_si128(r, big));
}

__attribute__((always_inline)) inline __m128 vu_abs(VUState *vu, int fs) {
    return _mm_and_ps(_mm_loadu_ps(vu->vf[fs].f), _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)));
}

__attribute__((always_inline)) inline void vu_clip(VUState *vu, int fs, int ft) {
    const VUReg &s = vu->vf[fs];
    float w = fabsf(vu->vf[ft].f[3]);
    uint32_t f = 0;
    if (s.f[0] > w) f |= 0x01;
    if (s.f[0] < -w) f |= 0x02;
    if (s.f[1] > w) f |= 0x04;
    if (s.f[1] < -w) f |= 0x08;
    if (s.f[2] > w) f |= 0x10;
    if (s.f[2] < -w) f |= 0x20;
    vu_flag_push(vu, 1, f);
}

// Helpers on register values: the compiled programs keep the VF registers
// they use in locals for the length of a block.
template <ArOp OP>
__attribute__((always_inline)) inline __m128 vu_fmac(__m128 a, __m128 b, __m128 acc) {
    if (OP == OP_MAX || OP == OP_MIN) {
        const __m128i ex = _mm_set1_epi32(0x7F800000);
        const __m128 sign = _mm_castsi128_ps(_mm_set1_epi32((int)0x80000000u));
        const __m128 big = _mm_castsi128_ps(_mm_set1_epi32(0x7F7FFFFF));
        auto clamp = [&](__m128 x) {
            __m128 sp = _mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(x), ex), ex));
            __m128 c = _mm_or_ps(_mm_and_ps(x, sign), big);
            return _mm_or_ps(_mm_and_ps(sp, c), _mm_andnot_ps(sp, x));
        };
        __m128 m = OP == OP_MAX ? _mm_cmpgt_ps(clamp(a), clamp(b)) : _mm_cmplt_ps(clamp(a), clamp(b));
        return _mm_or_ps(_mm_and_ps(m, a), _mm_andnot_ps(m, b));
    }
    __m128 r;
    switch (OP) {
    case OP_ADD: r = _mm_add_ps(a, b); break;
    case OP_SUB: r = _mm_sub_ps(a, b); break;
    case OP_MUL: r = _mm_mul_ps(a, b); break;
    case OP_MADD: r = _mm_add_ps(acc, _mm_mul_ps(a, b)); break;
    default: r = _mm_sub_ps(acc, _mm_mul_ps(a, b)); break;
    }
    const __m128 ex = _mm_castsi128_ps(_mm_set1_epi32(0x7F800000));
    if (__builtin_expect(_mm_movemask_ps(_mm_cmpeq_ps(_mm_and_ps(r, ex), ex)) != 0, 0)) {
        VUReg s, t, ac, o;
        _mm_storeu_ps(s.f, a);
        _mm_storeu_ps(t.f, b);
        _mm_storeu_ps(ac.f, acc);
        for (int k = 0; k < 4; k++) o.f[k] = apply(OP, ac.f[k], s.f[k], t.f[k]);
        r = _mm_loadu_ps(o.f);
    }
    return r;
}

// OPMULA (acc = fs x ft) / OPMSUB (fd = acc - fs x ft), x/y/z
template <bool SUB>
__attribute__((always_inline)) inline __m128 vu_opm(__m128 a, __m128 b, __m128 acc) {
    VUReg s, t, ac, r;
    _mm_storeu_ps(s.f, a);
    _mm_storeu_ps(t.f, b);
    _mm_storeu_ps(ac.f, acc);
    if (SUB) {
        r.f[0] = vu_clamp(ac.f[0] - s.f[1] * t.f[2]);
        r.f[1] = vu_clamp(ac.f[1] - s.f[2] * t.f[0]);
        r.f[2] = vu_clamp(ac.f[2] - s.f[0] * t.f[1]);
    } else {
        r.f[0] = vu_clamp(s.f[1] * t.f[2]);
        r.f[1] = vu_clamp(s.f[2] * t.f[0]);
        r.f[2] = vu_clamp(s.f[0] * t.f[1]);
    }
    r.f[3] = 0.0f;
    return _mm_loadu_ps(r.f);
}

template <int DEST>
__attribute__((always_inline)) inline __m128 vu_blend(__m128 old, __m128 v) {
    if (DEST == 15) return v;
    const __m128 m = _mm_castsi128_ps(_mm_setr_epi32(DEST & 8 ? -1 : 0, DEST & 4 ? -1 : 0, DEST & 2 ? -1 : 0,
                                                     DEST & 1 ? -1 : 0));
    return _mm_or_ps(_mm_and_ps(m, v), _mm_andnot_ps(m, old));
}

__attribute__((always_inline)) inline uint32_t vu_lane_u(__m128 v, int k) {
    VUReg r;
    _mm_storeu_ps(r.f, v);
    return r.u[k];
}

__attribute__((always_inline)) inline float vu_lane_f(__m128 v, int k) {
    VUReg r;
    _mm_storeu_ps(r.f, v);
    return r.f[k];
}

__attribute__((always_inline)) inline __m128 vu_itof_v(__m128 s, int n) {
    static const float sc[4] = {1.0f, 1.0f / 16, 1.0f / 4096, 1.0f / 32768};
    return _mm_mul_ps(_mm_cvtepi32_ps(_mm_castps_si128(s)), _mm_set1_ps(sc[n]));
}

__attribute__((always_inline)) inline __m128 vu_ftoi_v(__m128 s, int n) {
    static const float sc[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
    __m128 f = _mm_mul_ps(s, _mm_set1_ps(sc[n]));
    __m128i r = _mm_cvttps_epi32(f);
    __m128i big = _mm_castps_si128(_mm_cmpge_ps(f, _mm_set1_ps(2147483647.0f)));
    return _mm_castsi128_ps(_mm_xor_si128(r, big));
}

__attribute__((always_inline)) inline __m128 vu_abs_v(__m128 s) {
    return _mm_and_ps(s, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)));
}

__attribute__((always_inline)) inline void vu_clip_v(VUState *vu, __m128 sv, __m128 tv) {
    VUReg s;
    _mm_storeu_ps(s.f, sv);
    float w = fabsf(vu_lane_f(tv, 3));
    uint32_t f = 0;
    if (s.f[0] > w) f |= 0x01;
    if (s.f[0] < -w) f |= 0x02;
    if (s.f[1] > w) f |= 0x04;
    if (s.f[1] < -w) f |= 0x08;
    if (s.f[2] > w) f |= 0x10;
    if (s.f[2] < -w) f |= 0x20;
    vu_flag_push(vu, 1, f);
}

// VUREC_COUNT=1 tools/vurecomp.py: counts block executions (printed by --vu-bench)
struct BlkStat {
    uint64_t hits, pairs;
};
std::map<std::pair<std::string, uint32_t>, BlkStat> g_blk_stats;
inline void vu_blk_hit(const char *prog, uint32_t pc, int n) {
    BlkStat &b = g_blk_stats[{prog, pc}];
    b.hits++;
    b.pairs += n;
}

// VU1 data memory (VU0 also maps VU1's registers, see vu_qword)
inline uint8_t *vu1_qword(VUState *vu, uint32_t addr_qw) { return vu->mem + ((addr_qw * 16) & 0x3FFF); }

#include "vu_rec.inc"
}  // namespace

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
#endif

#if defined(EOD_VULKAN) || defined(VU_REC)
// Scoreboards that will stall later code the same way: the writes stamped
// in the three cycles before `cycle` (older slots are never looked at).
static bool sb_same(const Scoreboard &a, const Scoreboard &b, uint64_t cycle) {
    for (uint64_t dd = 1; dd <= 3; dd++) {
        uint64_t ic = cycle - dd;
        int sl = (int)(ic & 3);
        uint64_t alo = a.stamp[sl] == ic ? a.lo[sl] : 0, ahi = a.stamp[sl] == ic ? a.hi[sl] : 0;
        uint64_t blo = b.stamp[sl] == ic ? b.lo[sl] : 0, bhi = b.stamp[sl] == ic ? b.hi[sl] : 0;
        if (alo != blo || ahi != bhi) return false;
    }
    return true;
}
#endif

// EOD_VU_REC=file [EOD_VU_REC_AT=vblank] [EOD_VU_REC_N=n]: records the state
// before each micro program run (n runs from that vblank on) for
// --vu-bench, which replays them and times the micro programs alone.
extern uint64_t kernel_vblank_count();
namespace {
struct VuRecHdr {
    uint32_t magic, index, pc, top, itop, micro_follows;
};
const uint32_t kVuRecMagic = 0x31525556;  // "VUR1"

void vu_rec_hook(VUState *vu, uint32_t pc) {
    static const char *path = getenv("EOD_VU_REC");
    if (!path) return;
    static FILE *f = fopen(path, "wb");
    static uint64_t at = getenv("EOD_VU_REC_AT") ? strtoull(getenv("EOD_VU_REC_AT"), nullptr, 0) : 0;
    static long left = getenv("EOD_VU_REC_N") ? atol(getenv("EOD_VU_REC_N")) : 5000;
    static uint32_t gen[2] = {~0u, ~0u};
    if (!f || left <= 0) return;
    if (kernel_vblank_count() < at) return;
    VuRecHdr h = {kVuRecMagic, (uint32_t)vu->index, pc, g_vif1_top, g_vif1_itop, 0};
    h.micro_follows = gen[vu->index] != g_vu_micro_gen[vu->index];
    gen[vu->index] = g_vu_micro_gen[vu->index];
    fwrite(&h, sizeof h, 1, f);
    fwrite(vu, sizeof *vu, 1, f);
    fwrite(&g_score[vu->index], sizeof(Scoreboard), 1, f);
    fwrite(vu->mem, 1, vu->mem_mask + 1, f);
    if (h.micro_follows) fwrite(vu->micro, 1, vu->micro_mask + 1, f);
    if (--left == 0) {
        fclose(f);
        f = nullptr;
        LOG(LOG_VU, "EOD_VU_REC: done");
    }
}
}  // namespace

namespace {
bool vu_dispatch_observed() {
    static const bool trace = getenv("EOD_VU_REC") || getenv("EOD_VU_DUMP") ||
                              getenv("EOD_VU_STATS") || getenv("EOD_VU_CHECK");
    return trace || g_log_enabled[LOG_VU] || scene_capture::g_on || scene_emit::observing_vu();
}
}  // namespace

bool vu_retire_mscal_nop(VUState *vu, uint32_t pc) {
    constexpr uint32_t pairs[] = {0x8000033Cu, 0x400002FFu, 0x8000033Cu, 0x000002FFu};
    if (vu_dispatch_observed() ||
        vu->index != 1 || pc != 0x24D8 || std::memcmp(vu->micro + pc, pairs, sizeof(pairs)) != 0)
        return false;

    // Both paths recognize the live instruction pairs. Only reference builds
    // can dispatch a compiled version of those pairs.
#ifdef VU_REC
    const VuProg *compiled = vu_rec_find(vu, pc);
#else
    const VuProg *compiled = nullptr;
#endif
    const auto retire = [compiled, pc](VUState *state) {
        if (compiled) {
            // The generated NOP pairs advance cycles without per-pair pipe synchronization.
            state->cycle += 2;
        } else {
            for (int pair = 0; pair < 2; ++pair) {
                sync_pipes(state);
                vu_flag_sync(state);
                ++state->cycle;
            }
        }
        flush_q(state);
        flush_p(state);
        vu_flag_flush(state);
        state->flag_pipe = 0;
        state->tpc = pc + 8;
        state->running = 0;
    };

    static const bool check = getenv("EOD_NATIVE_CAMERA_NOP_CHECK") != nullptr;
    if (check) {
        VUState expected;
        std::memcpy(&expected, vu, sizeof(expected));
        const Scoreboard scoreboard = g_score[vu->index];
        uint8_t memory[0x4000];
        std::memcpy(memory, vu->mem, sizeof(memory));
        retire(&expected);
        if (compiled) compiled->fn(vu, pc);
        else vu_interp(vu, pc);
        if (std::memcmp(vu, &expected, sizeof(expected)) ||
            std::memcmp(&g_score[vu->index], &scoreboard, sizeof(scoreboard)) ||
            std::memcmp(vu->mem, memory, sizeof(memory)))
            fatal("MSCAL no-op retirement differs from VU1 execution at vblank %llu (%s)",
                  (unsigned long long)kernel_vblank_count(), compiled ? "compiled" : "interpreted");
    } else {
        retire(vu);
    }
    census_retire(*vu, pc, "retired_nop");
    return true;
}

#ifdef EOD_VULKAN
namespace {
constexpr uint32_t kWorldLeafPc = 0x270;
constexpr uint32_t kWorldLeafTpc = 0x920;
constexpr uint32_t kClippedWorldContinuation = 0x618;
constexpr unsigned kWorldStripOffset = 200;
constexpr unsigned kWorldMaxVertices = 60;
constexpr unsigned kWorldStripQwords = 1 + 3 * kWorldMaxVertices;
constexpr unsigned kWorldStripBlend = 0x00200000u;
static_assert(0x270 + kWorldStripOffset + kWorldStripQwords <= 0x400);
constexpr uint64_t kWorldFirstFourCycles = 104;
constexpr uint64_t kWorldVertexLoopCycles = 13;

struct WorldStripTransportStats {
    uint64_t packets = 0, qwords = 0;
};
WorldStripTransportStats g_world_strip_direct[2];

struct WorldLeafVertices {
    // GS PACKED retains the raw fog/ADC word and the input ST padding word.
    VUReg st[kWorldMaxVertices]{}, rgba[kWorldMaxVertices]{}, xyzf[kWorldMaxVertices]{};
    VUReg projection[kWorldMaxVertices]{}, acc_last{};
    VUReg last_input_position{}, last_st{}, last_unscaled_st{};
    float q[kWorldMaxVertices]{};
    float last_header = 0, penultimate_header = 0;
};

struct WorldLeafRetirement {
    VUState after{};
    VUReg tex1[2]{}, strip[kWorldStripQwords]{};
    unsigned strip_qwords = 0;
};

bool matches_world_image(VUState *vu) {
    constexpr XXH128_hash_t image_hash{0x2329B43F09230E29ull, 0xC8C64EAE4BB1B7EBull};
    if (vu->index != 1 || vu->mem_mask != 0x3FFF || vu->micro_mask != 0x3FFF)
        return false;
    static thread_local uint32_t generation;
    static thread_local bool checked, matches;
    if (!checked || generation != g_vu_micro_gen[1]) {
        generation = g_vu_micro_gen[1];
        matches = XXH128_isEqual(XXH3_128bits(vu->micro, 0x4000), image_hash);
        checked = true;
    }
    return matches;
}

bool matches_world_leaf_program(VUState *vu, uint32_t pc) {
    return pc == kWorldLeafPc && matches_world_image(vu);
}

VUReg world_qword(const VUState &vu, unsigned qw) {
    VUReg reg;
    std::memcpy(&reg, vu.mem + 16 * qw, sizeof reg);
    return reg;
}

bool world_leaf_interpreted(VUState *vu) {
#ifdef VU_REC
    return vu_rec_find(vu, kWorldLeafPc) == nullptr;
#else
    (void)vu;
    return true;
#endif
}

bool derive_world_leaf(const VUState &before, unsigned top, bool interpreted, WorldLeafRetirement &out) {
    constexpr uint32_t kOne = 0x3F800000;
    constexpr uint32_t kAlmostTwo = 0x3FFFFFFF;
    if (before.vi[0] != 0 || before.vf[0].u[0] || before.vf[0].u[1] ||
        before.vf[0].u[2] || before.vf[0].u[3] != kOne ||
        before.q_busy || before.p_busy || before.fq_n || before.flag_pipe || before.running)
        return false;

    // q19/q20 are the live strip and TEX1 tag templates. q21 supplies the TEX1 address.
    const VUReg source_tag = world_qword(before, top);
    const VUReg tag_template = world_qword(before, 19);
    const VUReg tex_base = world_qword(before, 22);
    const VUReg st_base = world_qword(before, top + 198);
    const VUReg pos_base = world_qword(before, top + 199);
    out.tex1[0] = world_qword(before, 20);
    out.tex1[1] = world_qword(before, 21);
    const unsigned count = source_tag.u[0] & 0x7FFF;
    const int16_t lod = static_cast<int16_t>(st_base.u[2]);
    if ((source_tag.u[0] & ~0x7FFFu) != 0x8000 || count < 4 || count > kWorldMaxVertices ||
        // PRIM.ABE may differ. Both strip templates use the same packed register list.
        (tag_template.u[1] & ~kWorldStripBlend) != 0x300E4000 ||
        (tag_template.u[2] & 0xFFF) != 0x412 ||
        out.tex1[0].u[0] != 0x8001 || out.tex1[0].u[1] != 0x10000000 ||
        (out.tex1[0].u[2] & 15) != 0xE || (out.tex1[1].u[2] & 0xFF) != 0x14 ||
        lod > 0 || !std::isnormal(before.vf[3].f[3]))
        return false;

    const uint64_t cycles = kWorldFirstFourCycles + kWorldVertexLoopCycles * (count - 4);
    if (before.cycle > UINT64_MAX - cycles) return false;

    out.tex1[1].u[1] = static_cast<uint16_t>(tex_base.u[1] + lod + 1000u);
    out.strip_qwords = 1 + 3 * count;
    out.strip[0] = tag_template;
    out.strip[0].u[0] = source_tag.u[0];

    WorldLeafVertices vertices;
    uint32_t sticky = 0;
    const auto record_mac = [&](const VUReg &value, int lanes) {
        if (!interpreted) return;
        const uint32_t mac = mac_for(value, lanes);
        if (mac & 0x000F) sticky |= 0x001;
        if (mac & 0x00F0) sticky |= 0x002;
        if (mac & 0x0F00) sticky |= 0x004;
        if (mac & 0xF000) sticky |= 0x008;
    };
    VUReg last_projected[2]{};
    float fog_scale;
    std::memcpy(&fog_scale, &kAlmostTwo, sizeof fog_scale);
    for (unsigned i = 0; i < count; ++i) {
        const unsigned src = top + 1 + 3 * i;
        const VUReg color = world_qword(before, src);
        VUReg pos = world_qword(before, src + 1);
        const VUReg uv = world_qword(before, src + 2);
        if (pos.u[3] != kOne || color.u[3] > 0xFFFF) return false;
        for (unsigned lane = 0; lane < 3; ++lane)
            pos.f[lane] += pos_base.f[lane];
        record_mac(pos, 0xE);  // ADD.xyz position + bank base
        VUReg &acc = vertices.acc_last;
        for (unsigned lane = 0; lane < 4; ++lane)
            acc.f[lane] = before.vf[4].f[lane] * pos.f[0];
        record_mac(acc, 0xF);  // MULAx
        for (unsigned lane = 0; lane < 4; ++lane)
            acc.f[lane] += before.vf[5].f[lane] * pos.f[1];
        record_mac(acc, 0xF);  // MADDAy
        for (unsigned lane = 0; lane < 4; ++lane)
            acc.f[lane] += before.vf[6].f[lane] * pos.f[2];
        record_mac(acc, 0xF);  // MADDAz
        VUReg &p = vertices.projection[i];
        for (unsigned lane = 0; lane < 4; ++lane)
            p.f[lane] = acc.f[lane] + before.vf[7].f[lane];
        record_mac(p, 0xF);  // MADDw
        if (!std::isnormal(p.f[3])) return false;
        const float q = 1.f / p.f[3];
        if (!std::isnormal(q)) return false;
        vertices.q[i] = q;
        // SQ.xyz leaves the current bank's ST W exactly as it was before the leaf.
        vertices.st[i] = world_qword(before, top + kWorldStripOffset + 1 + 3 * i);
        vertices.rgba[i] = color;
        vertices.rgba[i].u[3] = 0x80;
        vertices.xyzf[i].f[3] = static_cast<float>(color.u[3]) * fog_scale;
        VUReg st_input{}, unscaled_st{}, projected{};
        for (unsigned lane = 0; lane < 3; ++lane) {
            st_input.f[lane] = lane < 2 ? uv.f[lane] + st_base.f[lane] : 1.f;
            unscaled_st.f[lane] = st_input.f[lane] * q;
            vertices.st[i].f[lane] = unscaled_st.f[lane] * before.vf[3].f[3];
            projected.f[lane] = p.f[lane] * q;
            const float xyz = projected.f[lane] * 16.f;
            if (!std::isfinite(vertices.st[i].f[lane]) || !std::isfinite(xyz) ||
                xyz < -2147483648.f || xyz >= 2147483648.f)
                return false;
            vertices.xyzf[i].s[lane] = static_cast<int32_t>(xyz);
        }
        record_mac(st_input, 0xC);  // ADD.xy texture coordinates
        record_mac(unscaled_st, 0xE);  // MULq.xyz texture coordinates
        record_mac(projected, 0xE);  // MULq.xyz projected position
        record_mac(vertices.st[i], 0xE);  // MULw.xyz texture scale
        record_mac(vertices.xyzf[i], 1);  // MUL.w fog/ADC
        out.strip[1 + 3 * i] = vertices.st[i];
        out.strip[2 + 3 * i] = vertices.rgba[i];
        out.strip[3 + 3 * i] = vertices.xyzf[i];
        if (i == count - 2) {
            vertices.penultimate_header = static_cast<float>(color.u[3]);
            last_projected[0] = projected;
        }
        if (i == count - 1) {
            last_projected[1] = projected;
            vertices.last_header = static_cast<float>(color.u[3]);
            vertices.last_input_position = pos;
            vertices.last_st = st_input;
            vertices.last_unscaled_st = unscaled_st;
        }
    }

    // Preserve untouched lanes and pipeline fields for the next VU1 launch.
    auto &vu = out.after;
    vu = before;
    vu.vf[1].u[3] = kAlmostTwo;
    vu.vf[2].f[3] = vertices.penultimate_header;
    std::memcpy(vu.vf[3].u, vertices.last_st.u, 12);
    vu.vf[13] = st_base;
    vu.vf[13].f[3] = vertices.last_header;
    vu.vf[14].u[3] = world_qword(before, top + 8).u[3];
    for (unsigned lane = 0; lane < 3; ++lane)
        vu.vf[14].f[lane] = vertices.projection[count - 2].f[lane] * vertices.q[count - 2];
    vu.vf[15] = vertices.last_input_position;
    vu.vf[16] = vertices.xyzf[count - 1];
    vu.vf[17] = vertices.xyzf[count - 3];
    vu.vf[18] = vertices.xyzf[count - 2];
    vu.vf[19] = vertices.xyzf[count - 3];
    vu.vf[20] = vertices.st[count - 1];
    vu.vf[20].u[3] = world_qword(before, top + 5).u[3];
    vu.vf[21] = vertices.last_unscaled_st;
    vu.vf[21].u[3] = vertices.xyzf[0].u[3];
    vu.vi[1] = 0x80;
    vu.vi[2] = 20;
    vu.vi[3] = top + kWorldStripOffset;
    vu.vi[4] = 0;
    vu.vi[5] = out.tex1[1].u[1];
    vu.acc = vertices.acc_last;
    vu.q = vu.q_pending = vertices.q[count - 1];
    vu.i = fog_scale;
    vu.status &= ~0x30u;
    vu.cycle += cycles;
    vu.q_ready = vu.cycle - 25;
    vu.tpc = kWorldLeafTpc;

    if (interpreted) {
        // The interpreter records ten flag-producing operations per vertex.
        // They are all drained on return. Keep their sticky flags and the
        // exact final ring contents for the subsequent legacy programs.
        vu.status |= sticky << 6;
        const uint32_t last_macs[] = {
            mac_for(vertices.last_st, 0xC),                      // 0x0848
            mac_for(last_projected[0], 0xE),                  // 0x0850
            mac_for(vertices.xyzf[count - 2], 1),           // 0x0858
            mac_for(vertices.st[count - 2], 0xE),            // 0x0860
            mac_for(vertices.last_unscaled_st, 0xE),         // 0x0868
            mac_for(last_projected[1], 0xE),                  // 0x0870
            mac_for(vertices.xyzf[count - 1], 1),           // 0x0878
            mac_for(vertices.st[count - 1], 0xE),            // 0x0888
        };
        constexpr unsigned cycles_to_exit[] = {25, 24, 23, 22, 21, 20, 19, 17};
        vu.fq_head = (vu.fq_head + 10 * count) & 7;
        for (unsigned i = 0; i < 8; ++i)
            vu.fq[(vu.fq_head + i) & 7] = {vu.cycle - cycles_to_exit[i], last_macs[i], 0};
        vu.mac = last_macs[7];
        set_status_from_mac(&vu, vu.mac);
    }
    // The final scheduled VF result has aged out of the three live scoreboard slots.
    return true;
}

void write_world_leaf_memory(const WorldLeafRetirement &result, uint8_t *memory, unsigned top) {
    std::memcpy(memory + 21 * 16 + 4, &result.tex1[1].u[1], 4);
    std::memcpy(memory + (top + kWorldStripOffset) * 16, result.strip, result.strip_qwords * 16);
}

native::WorldStripTerminal world_strip_terminal(const WorldLeafRetirement &result) {
    native::WorldStripTerminal tail{};
    tail.prim = (result.strip[0].u[1] >> 15) & 0x7FF;
    tail.vertices = (result.strip_qwords - 1) / 3;
    for (unsigned i = 0; i < tail.vertices; ++i) {
        const VUReg &xyzf = result.strip[3 + i * 3];
        const bool adc = (xyzf.u[3] & 0x8000) != 0;
        tail.adc_vertices += adc;
        tail.triangle_attempts += i >= 2 && !adc;
    }
    for (unsigned i = 0; i < 3; ++i) {
        const VUReg *vertex = &result.strip[1 + (tail.vertices - 3 + i) * 3];
        const VUReg &stq = vertex[0], &rgba = vertex[1], &xyzf = vertex[2];
        const uint32_t color = (rgba.u[0] & 255) | ((rgba.u[1] & 255) << 8) |
                               ((rgba.u[2] & 255) << 16) | ((rgba.u[3] & 255) << 24);
        tail.fifo[i] = {
            uint16_t(xyzf.u[0]), uint16_t(xyzf.u[1]), (xyzf.u[2] >> 4) & 0xFFFFFF, color,
            stq.u[0], stq.u[1], stq.u[2], uint8_t(xyzf.u[3] >> 4),
        };
    }
    return tail;
}

void materialize_world_leaf(const WorldLeafRetirement &result, VUState &vu, unsigned top,
                            const native::WorldTextureLod &lod) {
    vu = result.after;
    write_world_leaf_memory(result, vu.mem, top);
    static const char *transport = std::getenv("EOD_NATIVE_WORLD_LOD");
    // Compare the complete packet, including ignored bits, before choosing the
    // typed transport. A partial PATH1 packet can still need the original bytes.
    const uint64_t kick[] = {0x1000000000008001ull, 0xE, native::world_tex1(lod), 0x14};
    if ((!transport || transport[0] != '0') && lod.valid && !g_vu_mute_kick &&
        !std::memcmp(kick, result.tex1, sizeof kick))
        native::post(native::CMD_WORLD_TEXTURE_LOD, &lod, sizeof lod);
    else
        xgkick(&vu, 20);
    static const bool strip_check = std::getenv("EOD_NATIVE_WORLD_STRIP_CHECK") != nullptr;
    if (!g_vu_mute_kick && (strip_check || native::world_strip_mode())) {
        const auto tail = world_strip_terminal(result);
        native::post(native::CMD_WORLD_STRIP_CHECK, &tail, sizeof tail);
    }
    static const char *direct = std::getenv("EOD_NATIVE_WORLD_STRIP_DIRECT");
    if (!direct || direct[0] != '1' || g_vu_mute_kick || (result.strip[0].u[1] & kWorldStripBlend)) {
        xgkick(&vu, top + kWorldStripOffset);
        return;
    }
    // The admitted EOP/PACKED tag fixes the complete, non-wrapping span.
    // gif_transfer consumes it inline or copies it into the ordered worker queue.
    gif_transfer(GIF_PATH1, reinterpret_cast<const uint8_t *>(result.strip), result.strip_qwords);
    auto &stats = g_world_strip_direct[top == 0x270];
    ++stats.packets;
    stats.qwords += result.strip_qwords;
}

bool check_world_leaf(const WorldLeafRetirement &expected, VUState &vu, unsigned top, bool verbose) {
    uint8_t memory[0x4000];
    memcpy(memory, vu.mem, sizeof memory);
    write_world_leaf_memory(expected, memory, top);
    const Scoreboard score = g_score[1];

    VuKickCheck kicks{{{20, 2, expected.tex1},
                       {top + kWorldStripOffset, expected.strip_qwords, expected.strip}}};
    const bool already_muted = g_vu_mute_kick;
    g_vu_mute_kick = true;
    g_vu_kick_check = &kicks;
#ifdef VU_REC
    if (const auto *compiled = vu_rec_find(&vu, kWorldLeafPc))
        compiled->fn(&vu, kWorldLeafPc);
    else
#endif
        vu_interp(&vu, kWorldLeafPc);
    g_vu_kick_check = nullptr;
    g_vu_mute_kick = already_muted;

    const size_t state_bytes = sizeof vu;
    const bool same_state = memcmp(&vu, &expected.after, state_bytes) == 0;
    const bool same_memory = memcmp(vu.mem, memory, sizeof memory) == 0;
    const bool same_score = sb_same(g_score[1], score, vu.cycle);
    const bool same_kicks = kicks.equal && kicks.seen == 2;
    if (same_state && same_memory && same_score && same_kicks) return true;
    if (verbose) {
        const auto *state_data = reinterpret_cast<const uint8_t *>(&expected.after);
        const auto *actual = reinterpret_cast<const uint8_t *>(&vu);
        const auto state_diff = std::mismatch(state_data, state_data + state_bytes, actual).first;
        const auto memory_diff = std::mismatch(memory, memory + sizeof memory, vu.mem).first;
        fprintf(stderr, "world leaf: %u vertices, state byte %zu, memory byte %zu, scoreboard %s, kicks %s, cycles %llu/%llu\n",
                (expected.strip_qwords - 1) / 3, static_cast<size_t>(state_diff - state_data),
                static_cast<size_t>(memory_diff - memory), same_score ? "same" : "different",
                same_kicks ? "same" : "different", (unsigned long long)expected.after.cycle,
                (unsigned long long)vu.cycle);
    }
    return false;
}

struct ClippedWorldStrip {
    VUState after;
    VUReg triangle_tag;
    VUReg strip[kWorldStripQwords];
    unsigned qwords;
};

VUReg world_transform(const VUReg *matrix, const VUReg &position,
                      VUReg &acc, uint32_t *macs) {
    for (unsigned lane = 0; lane < 4; ++lane)
        acc.f[lane] = matrix[0].f[lane] * position.f[0];
    macs[0] = mac_for(acc, 0xF);
    for (unsigned column = 1; column <= 2; ++column) {
        for (unsigned lane = 0; lane < 4; ++lane)
            acc.f[lane] += matrix[column].f[lane] * position.f[column];
        macs[column] = mac_for(acc, 0xF);
    }
    VUReg projected;
    for (unsigned lane = 0; lane < 4; ++lane)
        projected.f[lane] = acc.f[lane] + matrix[3].f[lane];
    macs[3] = mac_for(projected, 0xF);
    return projected;
}

bool derive_clipped_world_strip(const VUState &before, unsigned top, unsigned count,
                                ClippedWorldStrip &out) {
    const VUReg &zero = before.vf[0];
    if (before.vi[0] || zero.u[0] || zero.u[1] || zero.u[2] || zero.u[3] != 0x3F800000 ||
        before.q_busy || before.p_busy || before.fq_n || before.flag_pipe || before.running ||
        !std::isnormal(before.vf[3].f[3]) || before.cycle > UINT64_MAX - (25 + 57 * count))
        return false;
    const VUReg st_base = world_qword(before, top + 198);
    const VUReg position_base = world_qword(before, top + 199);
    const VUReg tag = world_qword(before, 19);
    if (static_cast<int16_t>(st_base.u[2]) <= 0 ||
        (tag.u[1] & ~kWorldStripBlend) != 0x300E4000 || (tag.u[2] & 0xFFF) != 0x412)
        return false;

    const unsigned output = top + kWorldStripOffset;
    out.qwords = 1 + 3 * count;
    out.strip[0] = tag;
    out.strip[0].u[0] = 0x8000 | count;
    out.triangle_tag = world_qword(before, 18);

    auto &vu = out.after;
    vu = before;
    // Positive K bypasses TEX1. Both projections use the same position
    // base, and ST retains the world matrix packet's scale in vf3.w.
    std::memcpy(vu.vf[3].u, position_base.u, 12);
    std::memcpy(vu.vf[13].u, st_base.u, 12);
    vu.vf[14] = vu.vf[15] = {};
    vu.clip = 0xFFFFFF;
    vu.vi[2] = 0;
    uint64_t cycle = before.cycle + 22;  // 0x0618 through 0x0448
    uint32_t sticky_macs = 0xF;  // the zero subtraction at 0x0440
    for (unsigned i = 0; i < count; ++i) {
        const unsigned vertex = 1 + 3 * i;
        const VUReg rgba = world_qword(before, top + vertex);
        VUReg position = world_qword(before, top + vertex + 1);
        const VUReg texcoord = world_qword(before, top + vertex + 2);
        if (position.u[3] != 0x3F800000) return false;
        for (unsigned lane = 0; lane < 3; ++lane)
            position.f[lane] += position_base.f[lane];
        const VUReg st_input{{texcoord.f[0] + st_base.f[0],
                              texcoord.f[1] + st_base.f[1], 1.f, 0.f}};
        uint32_t macs[14];
        macs[0] = mac_for(position, 0xE);
        macs[1] = mac_for(st_input, 0xC);
        const VUReg screen = world_transform(vu.vf + 4, position, vu.acc, macs + 2);
        const VUReg clip = world_transform(vu.vf + 8, position, vu.acc, macs + 6);
        if (!std::isnormal(screen.f[3]) || !std::isfinite(clip.f[3])) return false;
        const float q = 1.f / screen.f[3];
        if (!std::isnormal(q)) return false;

        const float extent = std::fabs(clip.f[3]);
        uint32_t code = 0;
        for (unsigned lane = 0; lane < 3; ++lane) {
            if (!std::isfinite(clip.f[lane])) return false;
            code |= uint32_t(clip.f[lane] > extent) << (lane * 2);
            code |= uint32_t(clip.f[lane] < -extent) << (lane * 2 + 1);
        }
        vu.clip = ((vu.clip << 6) | code) & 0xFFFFFF;
        macs[10] = code;

        unsigned vertex_cycles = 39;
        const uint16_t input_adc = uint16_t(rgba.u[3]);
        if (!input_adc) {
            vertex_cycles += 3;
            const uint32_t planes = vu.clip & 0x3FFFF;
            vu.vi[2] = planes ? 0x8000 : 0x7FFF;
            if (planes) {
                const uint32_t shared = planes & (planes >> 6) & (planes >> 12);
                if (!shared) return false;  // 0x05B8 requires polygon expansion
                // VI4 accumulates the first five FCOR results, excluding +X.
                vu.vi[4] = __builtin_popcount(shared & 0x3E);
                vertex_cycles += 15;
            }
        }

        auto &st = out.strip[vertex];
        auto &color = out.strip[vertex + 1];
        auto &xyzf = out.strip[vertex + 2];
        st.u[3] = world_qword(before, output + vertex).u[3];  // SQ.xyz leaves W
        color = rgba;
        color.s[3] = int16_t(0x280 + uint16_t(texcoord.u[2]));
        vu.vi[13] = uint16_t(input_adc + 0x7F80) | vu.vi[2];
        xyzf.s[3] = int16_t(vu.vi[13]);
        VUReg projected{}, unscaled{};
        for (unsigned lane = 0; lane < 3; ++lane) {
            unscaled.f[lane] = st_input.f[lane] * q;
            st.f[lane] = unscaled.f[lane] * before.vf[3].f[3];
            projected.f[lane] = screen.f[lane] * q;
            const float pixel = projected.f[lane] * 16.f;
            if (!std::isfinite(st.f[lane]) || !std::isfinite(pixel) ||
                pixel < -2147483648.f || pixel >= 2147483648.f)
                return false;
            xyzf.s[lane] = static_cast<int32_t>(pixel);
        }
        macs[11] = mac_for(unscaled, 0xE);
        macs[12] = mac_for(projected, 0xE);
        macs[13] = mac_for(st, 0xE);
        for (unsigned flag = 0; flag < 14; ++flag)
            if (flag != 10) sticky_macs |= macs[flag];
        vu.vf[20] = vu.vf[15];  // Older homogeneous positions for the clipping branch.
        vu.vf[15] = vu.vf[14];
        vu.vf[14] = clip;

        if (i + 1 == count) {
            vu.vf[16] = xyzf;
            vu.vf[17] = st;
            vu.vf[17].u[3] = screen.u[3];
            vu.vf[18] = unscaled;
            vu.vf[18].u[3] = color.u[3];
            vu.q = vu.q_pending = q;
            vu.q_ready = cycle + 22;  // DIV at 0x04b8, seven cycles to arrive
            vu.fq_head = (vu.fq_head + 1 + 14 * count) & 7;
            constexpr unsigned ready[] = {16, 17, 18, 19, 23, 26, 29, 30};
            for (unsigned flag = 0; flag < 8; ++flag)
                vu.fq[(vu.fq_head + flag) & 7] = {cycle + ready[flag], macs[flag + 6], flag == 4};
            vu.mac = macs[13];
        }
        cycle += vertex_cycles;
    }
    vu.vi[1] = 0x7F80;
    vu.vi[3] = output;
    vu.vi[7] = 0;
    vu.vi[12] = top + out.qwords;
    vu.cycle = cycle + 3;  // 0x0608 kick, E bit and its delay
    vu.tpc = kClippedWorldContinuation;
    vu.status &= ~0x30u;
    set_status_from_mac(&vu, sticky_macs);
    set_status_from_mac(&vu, vu.mac);
    return true;
}

void write_clipped_world_strip(const ClippedWorldStrip &strip, uint8_t *memory, unsigned top) {
    std::memcpy(memory + 23 * 16, &strip.triangle_tag, 16);
    std::memcpy(memory + (top + kWorldStripOffset) * 16, strip.strip, strip.qwords * 16);
}

void check_clipped_world_strip(const ClippedWorldStrip &strip, VUState &vu, unsigned top) {
    uint8_t memory[0x4000];
    std::memcpy(memory, vu.mem, sizeof memory);
    write_clipped_world_strip(strip, memory, top);
    const Scoreboard score = g_score[1];
    VuKickCheck kicks{{{top + kWorldStripOffset, strip.qwords, strip.strip}, {}}};
    const bool muted = g_vu_mute_kick;
    g_vu_mute_kick = true;
    g_vu_kick_check = &kicks;
    vu_interp(&vu, kClippedWorldContinuation);
    g_vu_kick_check = nullptr;
    g_vu_mute_kick = muted;
    if (memcmp(&vu, &strip.after, sizeof vu) || memcmp(vu.mem, memory, sizeof memory) ||
        !sb_same(score, g_score[1], vu.cycle) || !kicks.equal || kicks.seen != 1) {
        const auto *expected = reinterpret_cast<const uint8_t *>(&strip.after);
        const auto *actual = reinterpret_cast<const uint8_t *>(&vu);
        const auto state_diff = std::mismatch(expected, expected + sizeof vu, actual).first - expected;
        const auto mem_diff = std::mismatch(memory, memory + sizeof memory, vu.mem).first - memory;
        fatal("clipped world VU1 mismatch at vblank %llu, TOP %x, state byte %u, memory byte %u, cycles %llu/%llu, kicks %u (%d)",
              (unsigned long long)kernel_vblank_count(), top, unsigned(state_diff), unsigned(mem_diff),
              (unsigned long long)strip.after.cycle, (unsigned long long)vu.cycle, kicks.seen, kicks.equal);
    }
}

}  // namespace
#endif

void vu_report_world_strip_transport() {
#ifdef EOD_VULKAN
    const auto &low = g_world_strip_direct[0];
    const auto &high = g_world_strip_direct[1];
    if (low.packets || high.packets)
        std::fprintf(stderr, "[native] world strip direct PATH1: TOP 0e0 %llu packets %llu qwords, TOP 270 %llu packets %llu qwords\n",
                     (unsigned long long)low.packets, (unsigned long long)low.qwords,
                     (unsigned long long)high.packets, (unsigned long long)high.qwords);
#endif
}

bool vu_retire_world_leaf(VUState *vu, uint32_t pc, uint32_t top, const native::WorldTextureLod &lod) {
#ifdef EOD_VULKAN
    // Portable capture and original-path diagnostics must see the original dispatch.
    static const bool diagnostic = getenv("EOD_VU_INTERP") || getenv("EOD_GS_DUMP") || getenv("EOD_NATIVE_CHECK") ||
        getenv("EOD_VERTEX_CHECK") || getenv("EOD_NATIVE_SKIN_CHECK") ||
        getenv("EOD_NATIVE_QCHECK") || getenv("EOD_VIF_CHECK");
    if (diagnostic || vu_dispatch_observed() || pc != kWorldLeafPc ||
        vu->index != 1 || vu->mem_mask != 0x3FFF || vu->micro_mask != 0x3FFF ||
        (top != 0xE0 && top != 0x270) || !sb_same(g_score[1], Scoreboard{}, vu->cycle))
        return false;

    if (!matches_world_leaf_program(vu, pc)) return false;

    WorldLeafRetirement result;
    if (!derive_world_leaf(*vu, top, world_leaf_interpreted(vu), result)) return false;
    static const bool check = getenv("EOD_NATIVE_WORLD_RETIRE_CHECK") != nullptr;
    if (check && !check_world_leaf(result, *vu, top, true))
        fatal("world VU1 retirement differs at vblank %llu", (unsigned long long)kernel_vblank_count());
    materialize_world_leaf(result, *vu, top, lod);
    census_retire(*vu, pc, "retired_world");
    return true;
#else
    return false;
#endif
}

bool vu_retire_clipped_world_strip(VUState *vu, uint32_t top, unsigned vertices) {
#ifdef EOD_VULKAN
    static const char *setting = getenv("EOD_NATIVE_WORLD_CLIPPED");
    static const bool diagnostic = getenv("EOD_VU_INTERP") || getenv("EOD_GS_DUMP") || getenv("EOD_NATIVE_CHECK") ||
        getenv("EOD_VERTEX_CHECK") || getenv("EOD_NATIVE_SKIN_CHECK") ||
        getenv("EOD_NATIVE_QCHECK") || getenv("EOD_VIF_CHECK");
    if ((setting && setting[0] == '0') || diagnostic || vu_dispatch_observed() ||
        vu->tpc != kClippedWorldContinuation || g_vif1_itop != 0x280 ||
        (top != 0xE0 && top != 0x270) || vertices < 4 || vertices > kWorldMaxVertices ||
        !matches_world_image(vu) || !sb_same(g_score[1], Scoreboard{}, vu->cycle))
        return false;
#ifdef VU_REC
    if (vu_rec_find(vu, kClippedWorldContinuation)) return false;
#endif
    ClippedWorldStrip strip;
    if (!derive_clipped_world_strip(*vu, top, vertices, strip)) return false;
    static const bool check = getenv("EOD_NATIVE_WORLD_CLIPPED_CHECK") != nullptr;
    if (check) check_clipped_world_strip(strip, *vu, top);
    *vu = strip.after;
    write_clipped_world_strip(strip, vu->mem, top);
    xgkick(vu, top + kWorldStripOffset);
    census_retire(*vu, kClippedWorldContinuation, "retired_world_clipped");
    return true;
#else
    return false;
#endif
}

bool vu_retire_queue_dent(VUState *vu, unsigned top, native::QueueDentChunk &candidate) {
#ifdef EOD_PLUME
    static const bool diagnostic = getenv("EOD_VU_INTERP") || getenv("EOD_GS_DUMP") ||
        getenv("EOD_NATIVE_CHECK") || getenv("EOD_VERTEX_CHECK") || getenv("EOD_NATIVE_SKIN_CHECK") ||
        getenv("EOD_NATIVE_QCHECK") || getenv("EOD_VIF_CHECK") ||
        getenv("EOD_GS_DUMP_COLD_START") || getenv("EOD_GS_CENSUS") || getenv("EOD_GS_TRACE") ||
        getenv("EOD_NATIVE_PASSES") || getenv("EOD_R5A2_OFF") || getenv("EOD_GS_SKIPALL") ||
        getenv("EOD_MODERN");
    if (diagnostic || vu_dispatch_observed() || native::native_only() || scene_emit::capture_requested()) return false;
    const auto tag = world_qword(*vu, 19);
    const unsigned vertices = candidate.vertices, overwrite = candidate.overwrite;
    const auto origin = world_qword(*vu, top + 199);
    const auto uv_origin = world_qword(*vu, top + 198);
    if (g_vu_mute_kick || !matches_world_image(vu) ||
        vu->running || vu->flag_pipe || vu->fq_n || vu->p_busy || vu->q_busy || (vu->status & 0x30) ||
        !sb_same(g_score[1], Scoreboard{}, vu->cycle) || vu->tpc != 0x0D48 ||
        tag.u[1] != 0x300E4000 || (tag.u[2] & 0xFFF) != 0x412 ||
        int16_t(uv_origin.u[2]) <= 0 || vertices < 3 || vertices > 48 ||
        world_qword(*vu, top).u[0] != (0x8000u | vertices))
        return false;
#ifdef VU_REC
    if (vu_rec_find(vu, 0x0D48)) return false;
#endif
    VUReg output[48][3];
    for (unsigned i = std::min(overwrite, vertices - 3); i < vertices; ++i) {
        auto p = world_qword(*vu, top + 2 + 3 * i);
        const auto n = world_qword(*vu, top + 1 + 3 * i);
        const auto st = world_qword(*vu, top + 3 + 3 * i);
        const auto skin = world_qword(*vu, top + 149 + vertices - i);
        const auto dent0 = world_qword(*vu, 143 + uint16_t(skin.u[0]));
        const auto dent1 = world_qword(*vu, 143 + uint16_t(skin.u[1]));
        const float w0 = skin.f[2] - 32768.f, w1 = skin.f[3] - 32768.f;
        float light[3], halfway[3];
        for (unsigned axis = 0; axis < 3; ++axis) {
            p.f[axis] = (dent0.f[axis] * w0 + dent1.f[axis] * w1) + (p.f[axis] + origin.f[axis]);
            const float normal = n.f[axis] - vu->vf[13].f[0];
            light[axis] = normal * vu->vf[12].f[axis];
            halfway[axis] = normal * vu->vf[14].f[axis];
        }
        float screen[4];
        for (unsigned axis = 0; axis < 4; ++axis)
            screen[axis] = ((vu->vf[4].f[axis] * p.f[0] + vu->vf[5].f[axis] * p.f[1]) +
                            vu->vf[6].f[axis] * p.f[2]) + vu->vf[7].f[axis] * p.f[3];
        if (!std::isnormal(screen[3])) return false;
        const float q = 1.f / screen[3], scale = vu->vf[3].f[3];
        const float shade = (light[0] + light[1]) + light[2];
        const float half = (halfway[0] + halfway[1]) + halfway[2];
        const float alpha = std::max(0.f, (std::max(1.f, shade) * 1152.f) * half - 1056.f);
        auto &out = output[i];
        out[0] = VUReg{{((st.f[0] + uv_origin.f[0]) * q) * scale,
                        ((st.f[1] + uv_origin.f[1]) * q) * scale,
                        (st.f[2] * q) * scale, 0.f}};
        const auto convertible = [](float a) { return -2147483648.f <= a && a < 2147483648.f; };
        if (!convertible(alpha)) return false;
        for (unsigned axis = 0; axis < 3; ++axis) {
            const float color = std::min(vu->vf[1].f[axis] + std::max(0.f, shade) * vu->vf[2].f[axis],
                                         vu->vf[1].f[3]);
            const float xy = (screen[axis] * q) * 16.f;
            if (!convertible(color) || !convertible(xy) || !std::isfinite(out[0].f[axis])) return false;
            out[1].s[axis] = int32_t(color);
            out[2].s[axis] = int32_t(xy);
        }
        out[1].s[3] = int32_t(alpha);
        out[2].u[3] = n.u[3];
    }
    auto &tail = candidate.omission.terminal;
    tail.prim = (tag.u[1] >> 15) & 0x7FF;
    tail.vertices = vertices;
    candidate.omission.tag_high = uint64_t(tag.u[3]) << 32 | tag.u[2];
    for (unsigned i = 0; i < vertices; ++i) {
        const auto attr = world_qword(*vu, top + 1 + 3 * i);
        const bool adc = (attr.u[3] & 0x8000) != 0;
        tail.adc_vertices += adc;
        tail.triangle_attempts += i >= 2 && !adc;
    }
    for (unsigned i = 0; i < 3; ++i) {
        const auto *vertex = output[vertices - 3 + i];
        const auto &stq = vertex[0], &rgba = vertex[1], &xyzf = vertex[2];
        const uint32_t color = (rgba.u[0] & 255) | ((rgba.u[1] & 255) << 8) |
                               ((rgba.u[2] & 255) << 16) | ((rgba.u[3] & 255) << 24);
        tail.fifo[i] = {uint16_t(xyzf.u[0]), uint16_t(xyzf.u[1]), (xyzf.u[2] >> 4) & 0xFFFFFF, color,
                        stq.u[0], stq.u[1], stq.u[2], uint8_t(xyzf.u[3] >> 4)};
    }
    if (!tail.triangle_attempts) return false;
    if (!candidate.omission.reference) {
        // The opposite-bank successor replaces temporary registers. Preserve
        // only the scratch suffix the later same-bank continuation cannot cover.
        for (unsigned i = overwrite; i < vertices; ++i) {
            auto *dst = vu->mem + 16 * (top + 201 + 3 * i);
            std::memcpy(dst, output[i], 12);
            std::memcpy(dst + 16, output[i] + 1, 32);
        }
        census_retire(*vu, 0x0D48, "retired_queue_dent");
    }
    return true;
#else
    (void)vu;
    (void)top;
    (void)candidate;
    return false;
#endif
}

static constexpr uint32_t kMlistVisibilityPc = 0x40;

static bool is_mlist_visibility_image(const VUState &vu) {
    // FNV-1a of the retail entry through its E-bit delay slot (0x40..0x117).
    constexpr uint32_t end = 0x118;
    if (vu.micro_mask < end - 1) return false;
    uint64_t fingerprint = 0xcbf29ce484222325ull;
    for (uint32_t address = kMlistVisibilityPc; address < end; ++address)
        fingerprint = (fingerprint ^ vu.micro[address]) * 0x100000001b3ull;
    return fingerprint == 0x2a3140a9ff711e39ull;
}

void vu_execute(VUState *vu, uint32_t start_pc, VuLaunch launch, uint32_t ee_ra) {
    EOD_ZONE("vu_execute");
    ProfScope prof(P_EE_VU);
    uint32_t pc = start_pc & vu->micro_mask & ~7u;
    vu_rec_hook(vu, pc);
    vu_dump(vu, pc);
    if (scene_capture::g_on && vu->index == 1) scene_capture::vu1_start(vu, pc);
    if (scene_emit::g_on && vu->index == 1 && scene_emit::vu1_start(vu, pc)) return;
    r7_audit::add(vu->index == 1 ? r7_audit::Counter::Vu1 : r7_audit::Counter::Vu0Micro);
    if (census_enabled()) census_micro(*vu, pc, launch, ee_ra);
    if (vu->index == 0 && pc == kMlistVisibilityPc && r7_audit::enabled()) {
        static uint32_t checked_generation = ~0u;
        static bool is_mlist;
        if (checked_generation != g_vu_micro_gen[0]) {
            checked_generation = g_vu_micro_gen[0];
            is_mlist = is_mlist_visibility_image(*vu);
        }
        if (is_mlist) r7_audit::add(r7_audit::Counter::Vu0Mlist);
    }
#ifdef VU_REC
    if (const VuProg *p = vu_rec_find(vu, pc)) {
        static bool check = getenv("EOD_VU_CHECK") != nullptr;
        static const bool stats = getenv("EOD_VU_STATS") != nullptr;
        if (stats && !check) {  // EOD_VU_STATS=1: elapsed time per compiled VU1 entry
            using Clock = std::chrono::steady_clock;
            static uint64_t cnt[0x800], ns[0x800];
            static auto last = Clock::now();
            const auto start = Clock::now();
            p->fn(vu, pc);
            const auto end = Clock::now();
            if (vu->index == 1) {
                cnt[(pc >> 3) & 0x7FF]++;
                ns[(pc >> 3) & 0x7FF] += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            }
            if (end - last >= std::chrono::seconds(5)) {
                last = end;
                uint64_t total = 0;
                for (const uint64_t entry : ns) total += entry;
                for (int i = 0; i < 0x800; i++)
                    if (ns[i] > total / 100)
                        fprintf(stderr, "[vu-stats] VU1 %04x runs %llu, %.1f%% of VU1 time, %.2f us/run\n", i << 3,
                                (unsigned long long)cnt[i], 100.0 * ns[i] / total, ns[i] / 1000.0 / (double)cnt[i]);
                std::fill(std::begin(cnt), std::end(cnt), 0);
                std::fill(std::begin(ns), std::end(ns), 0);
            }
            return;
        }
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
                    memcmp(vu->mem, mem_after, msize) == 0 && sb_same(g_score[vu->index], sb_after, vu->cycle);
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
#endif
    // EOD_VU_STATS=1: which programs run interpreted (no compiled match), with their time
    static const bool stats = getenv("EOD_VU_STATS") != nullptr;
    if (stats) {
        static uint64_t cnt[2][0x800], tk[2][0x800], last;
        const uint64_t t0 = prof_ticks();
        vu_interp(vu, pc);
        const uint64_t t1 = prof_ticks();
        cnt[vu->index & 1][(pc >> 3) & 0x7FF]++;
        tk[vu->index & 1][(pc >> 3) & 0x7FF] += t1 - t0;
        if (!last) last = t1;
        if (t1 - last > 30000000000ull) {
            last = t1;
            for (int u = 0; u < 2; u++)
                for (int i = 0; i < 0x800; i++)
                    if (cnt[u][i] > 1000)
                        fprintf(stderr, "[vu-stats] VU%d %04x interpreted %llu runs, %.0f Mticks\n", u, i << 3,
                                (unsigned long long)cnt[u][i], tk[u][i] / 1e6);
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
    const bool call = f == 0x38 || f == 0x39;
    if (!call) {
        r7_audit::add(r7_audit::Counter::Vu0Macro);
        if (census_enabled()) census_add(0, {"macro", ins, 0, g_ee.r[31].uw[0]});
    }
    if (f < 0x30) {
        upper(c, ins);
        commit_upper(c);
    } else if (f >= 0x30 && f <= 0x35) {
        lower_op(c, ins);
    } else if (call) {
        uint32_t addr = f == 0x38 ? ((ins >> 6) & 0x7FFF) * 8 : vu->cmsar * 8;
        vu_execute(vu, addr, f == 0x38 ? VuLaunch::Vcallms : VuLaunch::Vcallmsr, g_ee.r[31].uw[0]);
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

// --vu-bench FILE [--repeat N]: replays an EOD_VU_REC recording and prints
// the time spent in the micro programs and a hash of their results.
int vu_bench(const char *path, int repeat) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "vu-bench: cannot open %s\n", path);
        return 1;
    }
    struct Rec {
        VuRecHdr h;
        VUState st;
        Scoreboard sb;
        std::vector<uint8_t> mem, micro;
    };
    std::vector<Rec> recs;
    for (;;) {
        Rec r;
        if (fread(&r.h, sizeof r.h, 1, f) != 1) break;
        if (r.h.magic != kVuRecMagic || r.h.index > 1 || fread(&r.st, sizeof r.st, 1, f) != 1 ||
            fread(&r.sb, sizeof r.sb, 1, f) != 1) {
            fprintf(stderr, "vu-bench: bad record %zu\n", recs.size());
            return 1;
        }
        r.mem.resize(r.st.mem_mask + 1);
        if (fread(r.mem.data(), 1, r.mem.size(), f) != r.mem.size()) break;
        if (r.h.micro_follows) {
            r.micro.resize(r.st.micro_mask + 1);
            if (fread(r.micro.data(), 1, r.micro.size(), f) != r.micro.size()) break;
        }
        recs.push_back(std::move(r));
    }
    fclose(f);
    _mm_setcsr(_mm_getcsr() | 0x8040);  // as the game runs (EE FPU flushes denormals)
    g_vu_mute_kick = true;
    size_t calls[2] = {0, 0};
    uint64_t cycles[2] = {0, 0};
    double ns[2] = {0, 0};
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&](const void *p, size_t n) {
        const uint8_t *b = static_cast<const uint8_t *>(p);
        for (size_t k = 0; k < n; k++) hash = (hash ^ b[k]) * 1099511628211ull;
    };
    std::vector<double> pass;  // VU1 time of each pass
#ifdef EOD_VULKAN
    const bool world_check = getenv("EOD_NATIVE_WORLD_BENCH_CHECK") != nullptr;
    size_t world_matched = 0, world_mismatched = 0, world_unsupported = 0;
#endif
    const bool mlist_check = getenv("EOD_MLIST_VISIBILITY_BENCH_CHECK") != nullptr;
    size_t mlist_matched = 0, mlist_mismatched = 0, mlist_unsupported = 0;
    for (int rep = 0; rep < repeat; rep++) {
        double before = ns[1];
        for (const Rec &r : recs) {
            VUState *vu = r.h.index ? &g_vu1 : &g_vu0;
            uint8_t *mem = vu->mem, *micro = vu->micro;
            *vu = r.st;
            vu->mem = mem;
            vu->micro = micro;
            memcpy(mem, r.mem.data(), r.mem.size());
            if (r.h.micro_follows) {
                memcpy(micro, r.micro.data(), r.micro.size());
                g_vu_micro_gen[r.h.index]++;
            }
            g_score[r.h.index] = r.sb;
            g_vif1_top = r.h.top;
            g_vif1_itop = r.h.itop;
#ifdef EOD_VULKAN
            if (world_check && rep == 0 && r.h.index == 1 && r.h.pc == kWorldLeafPc &&
                r.h.itop == 0x80 && (r.h.top == 0xE0 || r.h.top == 0x270)) {
                WorldLeafRetirement expected;
                if (!matches_world_leaf_program(vu, r.h.pc) ||
                    !sb_same(r.sb, Scoreboard{}, vu->cycle) || !derive_world_leaf(*vu, r.h.top, world_leaf_interpreted(vu), expected)) {
                    ++world_unsupported;
                    continue;
                }
                if (check_world_leaf(expected, *vu, r.h.top, world_mismatched < 8))
                    ++world_matched;
                else
                    ++world_mismatched;
                continue;
            }
#endif
            if (mlist_check) {
                if (rep != 0 || r.h.index != 0 || r.h.pc != kMlistVisibilityPc) continue;
                if (!is_mlist_visibility_image(*vu)) {
                    ++mlist_unsupported;
                    continue;
                }
                const auto *vf = vu->vf;
                const world_visibility::Frustum view{
                    {{vf[20].f[0], vf[21].f[0], vf[22].f[0]},
                     {vf[20].f[1], vf[21].f[1], vf[22].f[1]},
                     {vf[20].f[2], vf[21].f[2], vf[22].f[2]}},
                    {vf[23].f[0], vf[23].f[1], vf[23].f[2]},
                    vf[24].f[0], vf[26].f[2], vf[24].f[2], vf[26].f[3]};
                const auto expected = world_visibility::classify(
                    view, {vf[30].f[0], vf[30].f[1], vf[30].f[2]}, vf[30].f[3]);
                vu_execute(vu, r.h.pc);
                if (expected.clip == (vu->clip & 0xFFF) && f2u(expected.depth) == vu->vf[31].u[0]) {
                    ++mlist_matched;
                } else if (mlist_mismatched++ < 8) {
                    fprintf(stderr, "mlist-bench: clip %03x / %03x, depth %08x / %08x\n",
                            expected.clip, vu->clip & 0xFFF, f2u(expected.depth), vu->vf[31].u[0]);
                }
                continue;
            }
            uint64_t c0 = vu->cycle;
            auto t0 = std::chrono::steady_clock::now();
            vu_execute(vu, r.h.pc);
            auto t1 = std::chrono::steady_clock::now();
            ns[r.h.index] += std::chrono::duration<double, std::nano>(t1 - t0).count();
            calls[r.h.index]++;
            cycles[r.h.index] += vu->cycle - c0;
            if (rep == 0) {
                mix(vu->vf, sizeof vu->vf);
                mix(vu->vi, 16 * 4);
                mix(&vu->acc, sizeof vu->acc);
                mix(&vu->clip, 4);
                mix(&vu->cycle, 8);
                mix(mem, r.mem.size());
            }
        }
        pass.push_back(ns[1] - before);
    }
    bool checks_requested = mlist_check;
    bool checks_passed = true;
#ifdef EOD_VULKAN
    if (world_check) {
        fprintf(stderr, "world-bench: %zu matched, %zu mismatched, %zu unsupported\n",
                world_matched, world_mismatched, world_unsupported);
        checks_requested = true;
        checks_passed = world_matched && !world_mismatched;
    }
#endif
    if (mlist_check) {
        fprintf(stderr, "mlist-bench: %zu matched, %zu mismatched, %zu unsupported\n",
                mlist_matched, mlist_mismatched, mlist_unsupported);
        checks_passed = checks_passed && mlist_matched && !mlist_mismatched && !mlist_unsupported;
    }
    if (checks_requested) return checks_passed ? 0 : 1;
    std::sort(pass.begin(), pass.end());
#ifdef VU_REC
    if (!g_blk_stats.empty()) {
        std::vector<std::pair<uint64_t, std::string>> top;
        uint64_t total = 0;
        for (auto &kv : g_blk_stats) {
            total += kv.second.pairs;
            char line[200];
            snprintf(line, sizeof line, "%s %04x: %llu runs, %llu pairs", kv.first.first.c_str(), kv.first.second,
                     (unsigned long long)kv.second.hits, (unsigned long long)kv.second.pairs);
            top.push_back({kv.second.pairs, line});
        }
        std::sort(top.rbegin(), top.rend());
        for (size_t k = 0; k < top.size() && k < 40; k++)
            printf("  %5.1f%%  %s\n", 100.0 * top[k].first / total, top[k].second.c_str());
    }
#endif
    printf("vu-bench: %zu records x %d: VU0 %zu runs %.3f ms, VU1 %zu runs %.3f ms; VU1 per pass: fastest %.3f ms, "
           "median %.3f ms (%.2f ns per VU1 cycle); result hash %016llx\n",
           recs.size(), repeat, calls[0], ns[0] / 1e6, calls[1], ns[1] / 1e6, pass.empty() ? 0 : pass[0] / 1e6,
           pass.empty() ? 0 : pass[pass.size() / 2] / 1e6, cycles[1] ? ns[1] / cycles[1] : 0.0,
           (unsigned long long)hash);
    return 0;
}

extern "C" void vu0_sync(void) {}

extern "C" uint32_t vu0_cfc2(int reg) {
    r7_audit::add(r7_audit::Counter::Vu0Control);
    if (census_enabled()) census_add(0, {"cfc2", static_cast<uint32_t>(reg), 0, g_ee.r[31].uw[0]});
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
    case REG_VPU_STAT: return offload_busy() ? 0x100 : 0;  // VBS1: VU1 (on the worker) busy
    case REG_CMSAR1: offload_sync(); return g_vu1.cmsar;
    }
    return 0;
}

extern "C" void vu0_ctc2(int reg, uint32_t v) {
    r7_audit::add(r7_audit::Counter::Vu0Control);
    if (census_enabled()) census_add(0, {"ctc2", static_cast<uint32_t>(reg), 0, g_ee.r[31].uw[0]});
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
    case REG_CMSAR1: {
        const uint32_t ee_ra = g_ee.r[31].uw[0];
        offload_run([v, ee_ra] {
            g_vu1.cmsar = v & 0xFFFF;
            vu_execute(&g_vu1, (v & 0xFFFF) * 8, VuLaunch::Cmsar1, ee_ra);
        });
        break;
    }
    default:
        break;
    }
}
