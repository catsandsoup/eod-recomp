// vif.cpp - VIF0/VIF1 command processors.
//
// Words arriving through DMA or the FIFOs are queued until a complete
// command (code + payload) is available, then executed.  Micro programs
// run synchronously on the VU interpreter.
#include "vif.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#include "../core/log.h"
#include "../gs/gs.h"
#include "../kernel/kernel.h"
#include "vu.h"

extern uint32_t g_vif1_top, g_vif1_itop;
extern uint32_t g_vu_micro_gen[2];  // vu.cpp

namespace {

struct Vif {
    int n;
    VUState *vu;
    std::vector<uint32_t> q;  // pending words
    size_t qpos = 0;
    // registers
    uint32_t stat = 0, err = 0, mark = 0, cycle = 0, mode = 0, num = 0, mask = 0, code = 0;
    uint32_t itops = 0, base = 0, ofst = 0, tops = 0, itop = 0, top = 0;
    uint32_t row[4] = {}, col[4] = {};
    bool dbf = false;
};

Vif g_vif[2];

uint32_t avail(const Vif &v) { return (uint32_t)(v.q.size() - v.qpos); }

void start_micro(Vif &v, uint32_t addr) {
    if (v.n == 1) {
        v.itop = v.itops;
        v.top = v.tops;
        v.dbf = !v.dbf;
        v.tops = v.dbf ? v.base + v.ofst : v.base;
        g_vif1_top = v.top;
        g_vif1_itop = v.itop;
    } else {
        v.itop = v.itops;
    }
    vu_execute(v.vu, addr);
}

// Number of data words an UNPACK consumes.
uint32_t unpack_words(const Vif &v, uint32_t c) {
    uint32_t vn = (c >> 26) & 3, vl = (c >> 24) & 3;
    uint32_t num = (c >> 16) & 0xFF;
    if (!num) num = 256;
    uint32_t cl = v.cycle & 0xFF, wl = (v.cycle >> 8) & 0xFF;
    if (!wl) wl = 256;  // hardware treats WL=0 as 256
    if (!cl) cl = 256;
    uint32_t inputs;
    if (wl <= cl)
        inputs = num;
    else
        inputs = (num / wl) * cl + ((num % wl) < cl ? (num % wl) : cl);
    uint32_t bits_per = (vl == 3) ? 16 : (32 >> vl) * (vn + 1);
    uint32_t bitsz = inputs * bits_per;
    return (bitsz + 31) / 32;
}

// Fast path for the common unpacks: no mask, WL <= CL, not V4-5.
template <uint32_t VN, uint32_t VL, bool USN>
void unpack_fast(Vif &v, const uint8_t *src, uint32_t addr, uint32_t num, uint32_t cl, uint32_t wl) {
    constexpr uint32_t eb = 4 >> VL, nc = VN + 1;
    uint8_t *mem = v.vu->mem;
    uint32_t qmask = (v.vu->mem_mask + 1) / 16 - 1;
    uint32_t dst = addr, cyc = 0;
    const uint32_t mode = v.mode;
    for (uint32_t i = 0; i < num; i++, src += eb * nc) {
        uint32_t comp[4];
        for (uint32_t k = 0; k < nc; k++) {
            if (VL == 0) {
                memcpy(&comp[k], src + k * 4, 4);
            } else if (VL == 1) {
                uint16_t h;
                memcpy(&h, src + k * 2, 2);
                comp[k] = USN ? h : (uint32_t)(int32_t)(int16_t)h;
            } else {
                comp[k] = USN ? src[k] : (uint32_t)(int32_t)(int8_t)src[k];
            }
        }
        uint32_t val[4];
        switch (VN) {
        case 0: val[0] = val[1] = val[2] = val[3] = comp[0]; break;
        case 1: val[0] = comp[0]; val[1] = comp[1]; val[2] = comp[0]; val[3] = comp[1]; break;
        case 2: val[0] = comp[0]; val[1] = comp[1]; val[2] = comp[2]; val[3] = 0; break;
        default: val[0] = comp[0]; val[1] = comp[1]; val[2] = comp[2]; val[3] = comp[3]; break;
        }
        if (mode == 1) {
            for (int k = 0; k < 4; k++) val[k] += v.row[k];
        } else if (mode == 2) {
            for (int k = 0; k < 4; k++) val[k] = v.row[k] += val[k];
        }
        memcpy(mem + (dst & qmask) * 16, val, 16);
        if (++cyc == wl) {
            cyc = 0;
            dst += cl - wl + 1;
        } else {
            dst++;
        }
    }
}

typedef void (*UnpackFn)(Vif &, const uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);
#define UF(vn, vl) &unpack_fast<vn, vl, false>, &unpack_fast<vn, vl, true>
const UnpackFn g_unpack_fast[4][3][2] = {
    {{UF(0, 0)}, {UF(0, 1)}, {UF(0, 2)}},
    {{UF(1, 0)}, {UF(1, 1)}, {UF(1, 2)}},
    {{UF(2, 0)}, {UF(2, 1)}, {UF(2, 2)}},
    {{UF(3, 0)}, {UF(3, 1)}, {UF(3, 2)}},
};
#undef UF

void do_unpack(Vif &v, uint32_t c, const uint32_t *data) {
    uint32_t vn = (c >> 26) & 3, vl = (c >> 24) & 3;
    bool masked = (c >> 28) & 1;
    uint32_t num = (c >> 16) & 0xFF;
    if (!num) num = 256;
    uint32_t imm = c & 0xFFFF;
    uint32_t addr = imm & 0x3FF;
    bool usn = (imm >> 14) & 1;
    bool flg = (imm >> 15) & 1;
    if (v.n == 1 && flg) addr += v.tops;
    uint32_t cl = v.cycle & 0xFF, wl = (v.cycle >> 8) & 0xFF;
    if (!wl) wl = 256;
    if (!cl) cl = 256;
    const uint8_t *src = reinterpret_cast<const uint8_t *>(data);
    static const bool check = getenv("EOD_VIF_CHECK") != nullptr;  // compare with the generic code
    static uint8_t fast_mem[0x4000];
    static uint32_t fast_row[4];
    if (!masked && wl <= cl && vl != 3) {
        if (!check) {
            g_unpack_fast[vn][vl][usn](v, src, addr, num, cl, wl);
            v.num = 0;
            return;
        }
        static uint8_t saved[0x4000];
        uint32_t saved_row[4];
        size_t msize = v.vu->mem_mask + 1;
        memcpy(saved, v.vu->mem, msize);
        memcpy(saved_row, v.row, 16);
        g_unpack_fast[vn][vl][usn](v, src, addr, num, cl, wl);
        memcpy(fast_mem, v.vu->mem, msize);
        memcpy(fast_row, v.row, 16);
        memcpy(v.vu->mem, saved, msize);
        memcpy(v.row, saved_row, 16);
    }
    uint32_t mem_qw = (v.vu->mem_mask + 1) / 16;
    uint32_t elem_bytes = vl == 3 ? 2 : (4 >> vl);
    uint32_t ncomp = vl == 3 ? 1 : vn + 1;
    uint32_t in_index = 0;  // input element counter (in elements)
    for (uint32_t i = 0; i < num; i++) {
        uint32_t cyc = i % wl;          // position within the write cycle
        uint32_t dst = addr + (i / wl) * (wl <= cl ? cl : wl) + cyc;
        if (wl > cl) dst = addr + i;    // filling write: contiguous
        bool has_input = (wl <= cl) || cyc < cl;
        uint32_t val[4] = {0, 0, 0, 0};
        if (has_input) {
            if (vl == 3) {  // V4-5 (RGBA5551)
                uint16_t p;
                memcpy(&p, src + in_index * 2, 2);
                val[0] = (p & 0x1F) << 3;
                val[1] = ((p >> 5) & 0x1F) << 3;
                val[2] = ((p >> 10) & 0x1F) << 3;
                val[3] = (p >> 15) << 7;
                in_index++;
            } else {
                uint32_t comp[4];
                for (uint32_t k = 0; k < ncomp; k++) {
                    const uint8_t *p = src + (in_index + k) * elem_bytes;
                    uint32_t e;
                    if (elem_bytes == 4) {
                        memcpy(&e, p, 4);
                    } else if (elem_bytes == 2) {
                        uint16_t h;
                        memcpy(&h, p, 2);
                        e = usn ? h : (uint32_t)(int32_t)(int16_t)h;
                    } else {
                        e = usn ? *p : (uint32_t)(int32_t)(int8_t)*p;
                    }
                    comp[k] = e;
                }
                in_index += ncomp;
                switch (vn) {
                case 0: val[0] = val[1] = val[2] = val[3] = comp[0]; break;
                case 1: val[0] = comp[0]; val[1] = comp[1]; val[2] = comp[0]; val[3] = comp[1]; break;
                case 2: val[0] = comp[0]; val[1] = comp[1]; val[2] = comp[2]; val[3] = 0; break;
                default: memcpy(val, comp, 16); break;
                }
            }
        }
        uint8_t *q = v.vu->mem + (dst % mem_qw) * 16;
        uint32_t row_sel = cyc < 3 ? cyc : 3;
        for (int k = 0; k < 4; k++) {
            uint32_t m = 0;
            if (masked) m = (v.mask >> (row_sel * 8 + k * 2)) & 3;
            if (!has_input && m == 0) m = 1;  // filling write without input data: row register
            uint32_t out;
            switch (m) {
            case 0:
                out = val[k];
                if (v.mode == 1) out += v.row[k];
                else if (v.mode == 2) {
                    v.row[k] += val[k];
                    out = v.row[k];
                }
                break;
            case 1: out = v.row[k]; break;
            case 2: out = v.col[row_sel]; break;
            default: continue;  // write protected
            }
            memcpy(q + k * 4, &out, 4);
        }
    }
    v.num = 0;
    if (check && !masked && wl <= cl && vl != 3 &&
        (memcmp(fast_mem, v.vu->mem, v.vu->mem_mask + 1) || memcmp(fast_row, v.row, 16)))
        WARN("VIF%d unpack %08x (cycle %x mode %d): fast path differs", v.n, c, v.cycle, v.mode);
}

// Executes the complete commands at the start of w[0..n); returns the
// number of words consumed.
size_t run(Vif &v, const uint32_t *w, size_t n) {
    size_t pos = 0;
    for (;;) {
        size_t left = n - pos;
        if (left == 0) break;
        uint32_t c = w[pos];
        uint32_t cmd = (c >> 24) & 0x7F;
        uint32_t imm = c & 0xFFFF;
        uint32_t numf = (c >> 16) & 0xFF;
        uint32_t need = 1;
        if (cmd == 0x20) need = 2;
        else if (cmd == 0x30 || cmd == 0x31) need = 5;
        else if (cmd == 0x4A) need = 1 + (numf ? numf : 256) * 2;
        else if (cmd == 0x50 || cmd == 0x51) need = 1 + (imm ? imm : 65536) * 4;
        else if ((cmd & 0x60) == 0x60) need = 1 + unpack_words(v, c);
        if (left < need) break;
        const uint32_t *d = &w[pos + 1];
        v.code = c;
        switch (cmd) {
        case 0x00: break;  // NOP
        case 0x01: v.cycle = imm; break;  // STCYCL
        case 0x02:  // OFFSET
            v.ofst = imm & 0x3FF;
            v.dbf = false;
            v.tops = v.base;
            break;
        case 0x03: v.base = imm & 0x3FF; break;  // BASE
        case 0x04: v.itops = imm & 0x3FF; break;  // ITOP
        case 0x05: v.mode = imm & 3; break;       // STMOD
        case 0x06:  // MSKPATH3: a mask takes effect at the next packet boundary
            LOG(LOG_VIF, "VIF1 MSKPATH3 %s", (imm & 0x8000) ? "mask" : "unmask");
            if (imm & 0x8000) {
                gif_set_path3_mask(true);
            } else {
                gif_set_path3_mask(false);
                gif_path3_run(1);
            }
            break;
        case 0x07: v.mark = imm; break;           // MARK
        case 0x10: case 0x11: case 0x13:  // FLUSHE/FLUSH/FLUSHA
            LOG(LOG_VIF, "VIF%d FLUSH%s", v.n, cmd == 0x13 ? "A" : cmd == 0x11 ? "" : "E");
            if (cmd == 0x13) gif_path3_run(-1);  // waits for PATH3 (if unmasked)
            break;
        case 0x14:  // MSCAL
            LOG(LOG_VIF, "VIF%d MSCAL %04x", v.n, imm * 8);
            start_micro(v, imm * 8);
            break;
        case 0x15: start_micro(v, imm * 8); break;       // MSCALF
        case 0x17: start_micro(v, v.vu->tpc); break;     // MSCNT
        case 0x20: v.mask = d[0]; break;                 // STMASK
        case 0x30: memcpy(v.row, d, 16); break;          // STROW
        case 0x31: memcpy(v.col, d, 16); break;          // STCOL
        case 0x4A: {                                      // MPG
            g_vu_micro_gen[v.n]++;
            uint32_t n = numf ? numf : 256;
            uint32_t a = imm * 8;
            for (uint32_t k = 0; k < n * 2; k++) {
                uint32_t w = d[k];
                memcpy(v.vu->micro + ((a + k * 4) & v.vu->micro_mask), &w, 4);
            }
            break;
        }
        case 0x50:
        case 0x51:  // DIRECT / DIRECTHL
            gif_transfer(GIF_PATH2, reinterpret_cast<const uint8_t *>(d), imm ? imm : 65536);
            break;
        default:
            if ((cmd & 0x60) == 0x60) {
                do_unpack(v, c, d);
            } else {
                WARN("VIF%d: unknown command %08x", v.n, c);
            }
        }
        if (c & 0x80000000u) {
            // IRQ bit: signal the EE (the VIF would stall until cleared)
            intc_raise(v.n ? INTC_VIF1 : INTC_VIF0);
        }
        pos += need;
    }
    return pos;
}

// Executes as many complete commands as are queued.
void process(Vif &v) {
    v.qpos += run(v, v.q.data() + v.qpos, v.q.size() - v.qpos);
    // compact the queue
    if (v.qpos == v.q.size()) {
        v.q.clear();
        v.qpos = 0;
    } else if (v.qpos > 65536) {
        v.q.erase(v.q.begin(), v.q.begin() + (long)v.qpos);
        v.qpos = 0;
    }
}

}  // namespace

void vif_init() {
    g_vif[0] = Vif();
    g_vif[0].n = 0;
    g_vif[0].vu = &g_vu0;
    g_vif[1] = Vif();
    g_vif[1].n = 1;
    g_vif[1].vu = &g_vu1;
}

void vif_write(int n, const uint32_t *words, uint32_t count) {
    Vif &v = g_vif[n & 1];
    if (avail(v) == 0) {
        // nothing pending: run straight from the caller's buffer, keep a partial command
        size_t used = run(v, words, count);
        v.q.assign(words + used, words + count);
        v.qpos = 0;
        return;
    }
    v.q.insert(v.q.end(), words, words + count);
    process(v);
}

uint32_t vif_reg_read(int n, uint32_t off) {
    Vif &v = g_vif[n & 1];
    switch (off) {
    case 0x00: return v.dbf ? 0x80 : 0;  // STAT: idle, DBF
    case 0x20: return v.err;
    case 0x30: return v.mark;
    case 0x40: return v.cycle;
    case 0x50: return v.mode;
    case 0x60: return v.num;
    case 0x70: return v.mask;
    case 0x80: return v.code;
    case 0x90: return v.itops;
    case 0xA0: return v.base;
    case 0xB0: return v.ofst;
    case 0xC0: return v.tops;
    case 0xD0: return v.itop;
    case 0xE0: return v.top;
    case 0x100: case 0x110: case 0x120: case 0x130: return v.row[(off - 0x100) >> 4];
    case 0x140: case 0x150: case 0x160: case 0x170: return v.col[(off - 0x140) >> 4];
    }
    return 0;
}

void vif_reg_write(int n, uint32_t off, uint32_t val) {
    Vif &v = g_vif[n & 1];
    switch (off) {
    case 0x10:  // FBRST
        if (val & 1) {  // reset
            v.q.clear();
            v.qpos = 0;
            v.mask = v.mode = v.cycle = 0;
        }
        break;
    case 0x20: v.err = val; break;
    case 0x30: v.mark = val; break;
    case 0x100: case 0x110: case 0x120: case 0x130: v.row[(off - 0x100) >> 4] = val; break;
    case 0x140: case 0x150: case 0x160: case 0x170: v.col[(off - 0x140) >> 4] = val; break;
    }
}

void vif1_read_fifo(uint32_t out[4]) { out[0] = out[1] = out[2] = out[3] = 0; }
