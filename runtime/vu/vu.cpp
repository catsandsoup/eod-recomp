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
uint32_t mac_for(const VUReg &r, int dest) {
    uint32_t m = 0;
    for (int c = 0; c < 4; c++) {
        if (!has(dest, c)) continue;
        uint32_t u = r.u[c];
        int bit = 3 - c;
        if ((u & 0x7FFFFFFF) == 0) m |= 1u << bit;               // zero
        if (u & 0x80000000) m |= 1u << (4 + bit);                 // sign
        if ((u & 0x7F800000) == 0 && (u & 0x7FFFFF)) m |= 1u << (8 + bit);  // underflow
        if ((u & 0x7F800000) == 0x7F800000) m |= 1u << (12 + bit);           // overflow
    }
    return m;
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

void commit_upper(Ctx &c) {
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

void xgkick(VUState *vu, uint32_t addr_qw) {
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

inline uint64_t ready_of(const VUState *vu, const RegUse &u) {
    if (u.reg == 0) return 0;
    uint64_t r = 0;
    for (int k = 0; k < 4; k++)
        if ((u.mask >> (3 - k)) & 1)
            if (vu->vf_ready[u.reg][k] > r) r = vu->vf_ready[u.reg][k];
    return r;
}

inline void mark_write(VUState *vu, int reg, int mask, uint64_t when) {
    if (!reg) return;
    for (int k = 0; k < 4; k++)
        if ((mask >> (3 - k)) & 1) vu->vf_ready[reg][k] = when;
}

}  // namespace

// --------------------------------------------------------------- micro mode
void vu_execute(VUState *vu, uint32_t start_pc) {
    uint32_t pc = start_pc & vu->micro_mask & ~7u;
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
        // stall until every register this pair reads has been written
        RegUse uses[4];
        RegUse lw = {0, 0};
        int nu = upper_reads(hi, uses);
        if (!(hi & 0x80000000u)) nu += lower_reads(lo, uses + nu, &lw);
        uint64_t need = 0;
        for (int k = 0; k < nu; k++) {
            uint64_t r = ready_of(vu, uses[k]);
            if (r > need) need = r;
        }
        if (need > vu->cycle) vu->cycle = need;
        // WAITQ stalls the whole pair: the upper instruction sees the new Q
        if (!(hi & 0x80000000u) && (lo >> 25) == 0x40 && (lo & 0x3C) == 0x3C &&
            ((lo & 3) | ((lo >> 4) & 0x7C)) == 0x3B)
            flush_q(vu);
        sync_pipes(vu);
        // upper first (computed against old state), committed after lower
        c.up_write = false;
        c.mac_set = false;
        c.branch = false;
        upper(c, hi);
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
        if (up_vf) mark_write(vu, up_reg, up_dest, vu->cycle + 4);
        if (lw.reg) mark_write(vu, lw.reg, lw.mask, vu->cycle + 4);
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
