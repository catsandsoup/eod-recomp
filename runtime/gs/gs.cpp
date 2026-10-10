// gs.cpp - GIF front end, GS registers, local memory transfers and display.
#include "gs.h"
#include "gs_hw.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <pthread.h>
#endif

#include "../core/config.h"
#include "../core/log.h"
#include "../hw/hw.h"
#include "../kernel/kernel.h"
#include "../platform/platform.h"
#include "gs_mem.h"
#include "gs_state.h"
#include "gs_swizzle.h"

namespace gs {
extern bool g_trace;

State st;
Stats stats;

// Game frames: the display buffer starts being drawn after other buffers
// were (a flip, or a copy of the finished frame into a fixed display buffer).
std::atomic<uint64_t> g_flips{0};
// all game frames, drawn or skipped
std::atomic<uint64_t> g_game_flips{0};
// frame skipping: vblanks issued by the EE and composed by the GS thread
std::atomic<uint64_t> g_ee_vblanks{0}, g_gs_vblanks{0};

namespace {
std::atomic<uint64_t> g_csr{0x551B8000ull};  // GS_CSR (shared by the EE and the GS thread)

// ------------------------------------------------------------ GIF parsing
struct PathState {
    bool in_packet = false;
    uint64_t tag_lo = 0, tag_hi = 0;
    uint32_t nloop = 0;   // remaining loops
    uint32_t nreg = 0;
    uint32_t reg = 0;     // current register index within the loop
    uint32_t flg = 0;
    bool eop = false;
};
PathState g_path[4];

// host -> local transfer state
struct Transfer {
    bool active = false;
    uint32_t dbp, dbw, dpsm;
    int x0, y0, w, h;
    int x, y;
    // partial pixel bytes for 24-bit and sub-word formats
    uint8_t pend[16];
    int pend_n = 0;
    // address table and the current row
    const SwizzleTab *T = nullptr;
    int ry = -1;
    uint32_t rb = 0, rc = 0;
} g_xfer;

void vertex_kick(bool draw, uint64_t xyz, bool has_fog, uint8_t fog);

void write_reg(uint32_t reg, uint64_t v);

uint32_t gif_reg_ctrl, gif_reg_mode;

// --------------------------------------------------------------- transfer
void xfer_start() {
    if (replay_frozen()) {
        g_xfer.active = false;
        return;
    }
    uint32_t dir = bits(st.trxdir, 0, 2);
    uint32_t dbp = bits(st.bitbltbuf, 32, 14), dbw = bits(st.bitbltbuf, 48, 6), dpsm = bits(st.bitbltbuf, 56, 6);
    uint32_t sbp = bits(st.bitbltbuf, 0, 14), sbw = bits(st.bitbltbuf, 16, 6), spsm = bits(st.bitbltbuf, 24, 6);
    int w = (int)bits(st.trxreg, 0, 12), h = (int)bits(st.trxreg, 32, 12);
    int sx = (int)bits(st.trxpos, 0, 11), sy = (int)bits(st.trxpos, 16, 11);
    int dx = (int)bits(st.trxpos, 32, 11), dy = (int)bits(st.trxpos, 48, 11);
    stats.transfers++;
    census_xfer((int)dir, sbp, sbw, spsm, dbp, dbw, dpsm, sx, sy, dx, dy, w, h);
    if (hw::g_on) {
        if (dir == 1 || dir == 2) hw::vram_needed(sbp, sbw, spsm, sx, sy, w, h);
        if (dir == 0 || dir == 2) hw::vram_written(dbp, dbw, dpsm, dx, dy, w, h);
    }
    if (dir == 2) raster_vram_read(sbp, sbw, spsm, sx, sy, w, h);
    if (dir == 1) raster_sync();
    if (dir == 0 || dir == 2) {
        raster_vram_write(dbp, dbw, dpsm, dx, dy, w, h);
        vram_mark(dbp, dbw, dpsm, dx, dy, w, h);
    }
    if (dir == 0) {
        g_xfer.active = true;
        g_xfer.dbp = dbp;
        g_xfer.dbw = dbw;
        g_xfer.dpsm = dpsm;
        g_xfer.x0 = dx;
        g_xfer.y0 = dy;
        g_xfer.w = w;
        g_xfer.h = h;
        g_xfer.x = 0;
        g_xfer.y = 0;
        g_xfer.pend_n = 0;
        g_xfer.T = swizzle_table(dpsm, dbw);
        g_xfer.ry = -1;
        LOG(LOG_GSXFER, "transfer host->local bp=%x bw=%d psm=%x at %d,%d size %dx%d", dbp, dbw, dpsm, dx, dy, w, h);
    } else if (dir == 2) {
        LOG(LOG_GSXFER, "transfer local->local %x(%x)->%x(%x) %dx%d", sbp, spsm, dbp, dpsm, w, h);
        // overlapping copies: go through a temporary buffer
        std::vector<uint32_t> tmp((size_t)w * h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                tmp[(size_t)y * w + x] = read_pixel(spsm, (sx + x) & 2047, (sy + y) & 2047, sbp, sbw);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                write_pixel(dpsm, (dx + x) & 2047, (dy + y) & 2047, dbp, dbw, tmp[(size_t)y * w + x]);
    } else if (dir == 1) {
        static unsigned n;
        if (++n <= 8 || g_log_enabled[LOG_GSXFER])
            LOG(LOG_GS, "transfer local->host requested (%dx%d) - not supported yet", w, h);
    }
}

inline void xfer_put(uint32_t v) {
    Transfer &t = g_xfer;
    if (t.y >= t.h) return;
    int py = (t.y0 + t.y) & 2047;
    if (py != t.ry) {
        t.ry = py;
        t.rb = t.T->row_base(t.dbp, py);
        t.rc = t.T->rowC[py];
    }
    uint32_t a = t.T->addr_row(t.rb, t.rc, (t.x0 + t.x) & 2047);
    switch (t.dpsm) {
    case PSMCT32: case PSMZ32: word(a) = v; break;
    case PSMCT24: case PSMZ24: word(a) = (word(a) & 0xFF000000u) | (v & 0xFFFFFF); break;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: half(a) = (uint16_t)v; break;
    case PSMT8: byte(a) = (uint8_t)v; break;
    case PSMT4: set_nibble(a, v); break;
    case PSMT8H: word(a) = (word(a) & 0x00FFFFFFu) | (v << 24); break;
    case PSMT4HL: word(a) = (word(a) & 0xF0FFFFFFu) | ((v & 0xF) << 24); break;
    case PSMT4HH: word(a) = (word(a) & 0x0FFFFFFFu) | ((v & 0xF) << 28); break;
    default: write_pixel(t.dpsm, (t.x0 + t.x) & 2047, py, t.dbp, t.dbw, v); break;
    }
    if (++t.x >= t.w) {
        t.x = 0;
        t.y++;
    }
}

void xfer_data(const uint8_t *p, uint32_t n) {
    Transfer &t = g_xfer;
    if (!t.active) return;
    if (t.dpsm == PSMCT32 || t.dpsm == PSMZ32) {
        // whole runs of a row at a time
        uint32_t i = 0;
        while (i + 4 <= n && t.y < t.h) {
            int py = (t.y0 + t.y) & 2047;
            if (py != t.ry) {
                t.ry = py;
                t.rb = t.T->row_base(t.dbp, py);
                t.rc = t.T->rowC[py];
            }
            int run = std::min<int>(t.w - t.x, (int)((n - i) / 4));
            for (int k = 0; k < run; k++, i += 4) {
                uint32_t v;
                memcpy(&v, p + i, 4);
                word(t.T->addr_row(t.rb, t.rc, (t.x0 + t.x + k) & 2047)) = v;
            }
            t.x += run;
            if (t.x >= t.w) {
                t.x = 0;
                t.y++;
            }
        }
        if (t.y >= t.h) t.active = false;
        return;
    }
    switch (bits_per_pixel(t.dpsm)) {
    case 32:
        for (uint32_t i = 0; i + 4 <= n; i += 4) {
            uint32_t v;
            memcpy(&v, p + i, 4);
            xfer_put(v);
        }
        break;
    case 24:
        for (uint32_t i = 0; i < n; i++) {
            t.pend[t.pend_n++] = p[i];
            if (t.pend_n == 3) {
                xfer_put(t.pend[0] | (t.pend[1] << 8) | (t.pend[2] << 16));
                t.pend_n = 0;
            }
        }
        break;
    case 16:
        for (uint32_t i = 0; i + 2 <= n; i += 2) xfer_put(p[i] | (p[i + 1] << 8));
        break;
    case 8:
        for (uint32_t i = 0; i < n; i++) xfer_put(p[i]);
        break;
    case 4:
        for (uint32_t i = 0; i < n; i++) {
            xfer_put(p[i] & 0xF);
            xfer_put(p[i] >> 4);
        }
        break;
    }
    if (t.y >= t.h) t.active = false;
}

// ------------------------------------------------------------- registers
void tex0_write(int c, uint64_t v) {
    st.ctx[c].tex0 = v;
    clut_load(v);
}

void write_reg(uint32_t reg, uint64_t v) {
    // vertex data and image transfer words don't affect the drawing state
    if (reg > 0x05 && reg != 0x0A && reg != 0x0C && reg != 0x0D && reg != 0x54) g_reg_gen++;
    switch (reg) {
    case 0x00:  // PRIM
        st.prim = v;
        st.vcount = 0;
        break;
    case 0x01: {  // RGBAQ
        st.rgbaq = v;
        uint32_t q = (uint32_t)(v >> 32);
        memcpy(&st.q, &q, 4);
        break;
    }
    case 0x02: st.st = v; break;
    case 0x03: st.uv = v; break;
    case 0x04: vertex_kick(true, v, true, (uint8_t)(v >> 56)); break;   // XYZF2
    case 0x05: vertex_kick(true, v, false, 0); break;                   // XYZ2
    case 0x06: tex0_write(0, v); break;
    case 0x07: tex0_write(1, v); break;
    case 0x08: st.ctx[0].clamp = v; break;
    case 0x09: st.ctx[1].clamp = v; break;
    case 0x0A: st.fog = v; break;
    case 0x0C: vertex_kick(false, v, true, (uint8_t)(v >> 56)); break;  // XYZF3
    case 0x0D: vertex_kick(false, v, false, 0); break;                  // XYZ3
    case 0x14: st.ctx[0].tex1 = v; break;
    case 0x15: st.ctx[1].tex1 = v; break;
    case 0x16:
    case 0x17: {  // TEX2: PSM, CBP, CPSM, CSM, CSA, CLD of TEX0
        int c = reg - 0x16;
        const uint64_t mask = 0xFFFFFFE003F00000ull;
        st.ctx[c].tex2 = v;
        tex0_write(c, (st.ctx[c].tex0 & ~mask) | (v & mask));
        break;
    }
    case 0x18: st.ctx[0].xyoffset = v; break;
    case 0x19: st.ctx[1].xyoffset = v; break;
    case 0x1A: st.prmodecont = v; break;
    case 0x1B: st.prmode = v; break;
    case 0x1C: st.texclut = v; break;
    case 0x22: st.scanmsk = v; break;
    case 0x34: st.ctx[0].miptbp1 = v; break;
    case 0x35: st.ctx[1].miptbp1 = v; break;
    case 0x36: st.ctx[0].miptbp2 = v; break;
    case 0x37: st.ctx[1].miptbp2 = v; break;
    case 0x3B: st.texa = v; break;
    case 0x3D: st.fogcol = v; break;
    case 0x3F: break;  // TEXFLUSH
    case 0x40: st.ctx[0].scissor = v; break;
    case 0x41: st.ctx[1].scissor = v; break;
    case 0x42: st.ctx[0].alpha = v; break;
    case 0x43: st.ctx[1].alpha = v; break;
    case 0x44: st.dimx = v; break;
    case 0x45: st.dthe = v; break;
    case 0x46: st.colclamp = v; break;
    case 0x47: st.ctx[0].test = v; break;
    case 0x48: st.ctx[1].test = v; break;
    case 0x49: st.pabe = v; break;
    case 0x4A: st.ctx[0].fba = v; break;
    case 0x4B: st.ctx[1].fba = v; break;
    case 0x4C: st.ctx[0].frame = v; break;
    case 0x4D: st.ctx[1].frame = v; break;
    case 0x4E: st.ctx[0].zbuf = v; break;
    case 0x4F: st.ctx[1].zbuf = v; break;
    case 0x50: st.bitbltbuf = v; break;
    case 0x51: st.trxpos = v; break;
    case 0x52: st.trxreg = v; break;
    case 0x53:
        st.trxdir = v;
        xfer_start();
        break;
    case 0x54: {  // HWREG
        uint8_t b[8];
        memcpy(b, &v, 8);
        xfer_data(b, 8);
        break;
    }
    case 0x60: {  // SIGNAL
        uint32_t id = (uint32_t)(v >> 32), mask = (uint32_t)v;
        st.siglblid = (st.siglblid & ~(uint64_t)mask) | (id & mask);
        g_csr |= 1;
        if (!(st.imr & 0x100)) intc_raise(INTC_GS);
        break;
    }
    case 0x61:  // FINISH
        g_csr |= 2;
        if (!(st.imr & 0x200)) intc_raise(INTC_GS);
        break;
    case 0x62: {  // LABEL
        uint32_t id = (uint32_t)(v >> 32), mask = (uint32_t)v;
        st.siglblid = (st.siglblid & ~((uint64_t)mask << 32)) | ((uint64_t)(id & mask) << 32);
        break;
    }
    default:
        LOG(LOG_GS, "write to unknown GS register %02x = %016llx", reg, (unsigned long long)v);
    }
}

// --------------------------------------------------------------- vertices
uint32_t prim_attr() {
    // PRMODECONT.AC = 1: attributes come from PRIM; 0: from PRMODE
    return (st.prmodecont & 1) ? (uint32_t)st.prim : ((uint32_t)st.prmode & ~7u) | ((uint32_t)st.prim & 7);
}

void vertex_kick(bool draw, uint64_t xyz, bool has_fog, uint8_t fog) {
    uint32_t attr = prim_attr();
    int ctxt = (attr >> 9) & 1;
    const Context &c = st.ctx[ctxt];
    Vertex v;
    int32_t ofx = (int32_t)bits(c.xyoffset, 0, 16), ofy = (int32_t)bits(c.xyoffset, 32, 16);
    v.x = (int32_t)(xyz & 0xFFFF) - ofx;
    v.y = (int32_t)((xyz >> 16) & 0xFFFF) - ofy;
    v.z = has_fog ? (uint32_t)((xyz >> 32) & 0xFFFFFF) : (uint32_t)(xyz >> 32);
    v.f = has_fog ? fog : (uint8_t)(st.fog >> 56);
    v.r = (uint8_t)st.rgbaq;
    v.g = (uint8_t)(st.rgbaq >> 8);
    v.b = (uint8_t)(st.rgbaq >> 16);
    v.a = (uint8_t)(st.rgbaq >> 24);
    uint32_t s = (uint32_t)st.st, t = (uint32_t)(st.st >> 32);
    memcpy(&v.s, &s, 4);
    memcpy(&v.t, &t, 4);
    v.q = st.q;
    v.u = (int32_t)bits(st.uv, 0, 14);
    v.v = (int32_t)bits(st.uv, 16, 14);

    uint32_t type = st.prim & 7;
    // queue handling per primitive type
    switch (type) {
    case 0:  // point
        if (draw) {
            stats.prims++;
            draw_point(v);
        }
        st.vcount = 0;
        return;
    case 1:  // line
    case 6:  // sprite
        st.vq[st.vcount++] = v;
        if (st.vcount == 2) {
            if (draw) {
                stats.prims++;
                if (type == 1) draw_line(st.vq[0], st.vq[1]);
                else draw_sprite(st.vq[0], st.vq[1]);
            }
            st.vcount = 0;
        }
        return;
    case 2:  // line strip
        if (st.vcount < 2) {
            st.vq[st.vcount++] = v;
        } else {
            st.vq[0] = st.vq[1];
            st.vq[1] = v;
        }
        if (st.vcount == 2 && draw) {
            stats.prims++;
            draw_line(st.vq[0], st.vq[1]);
        }
        return;
    case 3:  // triangle
        st.vq[st.vcount++] = v;
        if (st.vcount == 3) {
            if (draw) {
                stats.prims++;
                draw_triangle(st.vq[0], st.vq[1], st.vq[2]);
            }
            st.vcount = 0;
        }
        return;
    case 4:  // triangle strip
        if (st.vcount < 3) {
            st.vq[st.vcount++] = v;
        } else {
            st.vq[0] = st.vq[1];
            st.vq[1] = st.vq[2];
            st.vq[2] = v;
        }
        if (st.vcount == 3 && draw) {
            stats.prims++;
            draw_triangle(st.vq[0], st.vq[1], st.vq[2]);
        }
        return;
    case 5:  // triangle fan
        if (st.vcount < 3) {
            st.vq[st.vcount++] = v;
        } else {
            st.vq[1] = st.vq[2];
            st.vq[2] = v;
        }
        if (st.vcount == 3 && draw) {
            stats.prims++;
            draw_triangle(st.vq[0], st.vq[1], st.vq[2]);
        }
        return;
    default:
        st.vcount = 0;
        return;
    }
}

// ------------------------------------------------------------ GIF packets
void packed_reg(uint32_t r, uint64_t lo, uint64_t hi) {
    switch (r) {
    case 0x0:  // PRIM
        write_reg(0x00, lo & 0x7FF);
        break;
    case 0x1: {  // RGBAQ: R,G,B,A in the low byte of each word; Q from ST
        uint64_t v = (lo & 0xFF) | (((lo >> 32) & 0xFF) << 8) | ((hi & 0xFF) << 16) | (((hi >> 32) & 0xFF) << 24);
        uint32_t q;
        memcpy(&q, &st.q, 4);
        st.rgbaq = v | ((uint64_t)q << 32);
        break;
    }
    case 0x2: {  // ST (+Q)
        st.st = lo;
        uint32_t q = (uint32_t)hi;
        memcpy(&st.q, &q, 4);
        break;
    }
    case 0x3:  // UV
        st.uv = (lo & 0x3FFF) | (((lo >> 32) & 0x3FFF) << 16);
        break;
    case 0x4: {  // XYZF2
        uint64_t x = lo & 0xFFFF, y = (lo >> 32) & 0xFFFF;
        uint64_t z = (hi >> 4) & 0xFFFFFF, f = (hi >> 36) & 0xFF;
        bool adc = (hi >> 47) & 1;
        uint64_t v = x | (y << 16) | (z << 32) | (f << 56);
        write_reg(adc ? 0x0C : 0x04, v);
        break;
    }
    case 0x5: {  // XYZ2
        uint64_t x = lo & 0xFFFF, y = (lo >> 32) & 0xFFFF, z = hi & 0xFFFFFFFF;
        bool adc = (hi >> 47) & 1;
        write_reg(adc ? 0x0D : 0x05, x | (y << 16) | (z << 32));
        break;
    }
    case 0xA:  // FOG
        st.fog = ((hi >> 36) & 0xFF) << 56;
        break;
    case 0xC: {  // XYZF3
        uint64_t x = lo & 0xFFFF, y = (lo >> 32) & 0xFFFF;
        uint64_t z = (hi >> 4) & 0xFFFFFF, f = (hi >> 36) & 0xFF;
        write_reg(0x0C, x | (y << 16) | (z << 32) | (f << 56));
        break;
    }
    case 0xD: {  // XYZ3
        uint64_t x = lo & 0xFFFF, y = (lo >> 32) & 0xFFFF, z = hi & 0xFFFFFFFF;
        write_reg(0x0D, x | (y << 16) | (z << 32));
        break;
    }
    case 0xE:  // A+D
        write_reg((uint32_t)(hi & 0xFF), lo);
        break;
    case 0xF:  // NOP
        break;
    default:  // TEX0_1/2, CLAMP_1/2 etc: register = descriptor
        write_reg(r, lo);
        break;
    }
}

void gif_process(int path, const uint8_t *data, uint32_t qwc) {
    PathState &ps = g_path[path];
    for (uint32_t i = 0; i < qwc; i++) {
        const uint8_t *q = data + i * 16;
        uint64_t lo, hi;
        memcpy(&lo, q, 8);
        memcpy(&hi, q + 8, 8);
        if (!ps.in_packet || ps.nloop == 0) {
            // new GIF tag
            ps.tag_lo = lo;
            ps.tag_hi = hi;
            ps.nloop = (uint32_t)(lo & 0x7FFF);
            ps.eop = (lo >> 15) & 1;
            ps.flg = (uint32_t)((lo >> 58) & 3);
            ps.nreg = (uint32_t)((lo >> 60) & 0xF);
            if (ps.nreg == 0) ps.nreg = 16;
            ps.reg = 0;
            ps.in_packet = true;
            if ((lo >> 46) & 1) write_reg(0x00, (lo >> 47) & 0x7FF);  // PRE: set PRIM
            if (ps.flg == 1 && ps.nloop) {
                // REGLIST: 64-bit units; handled below by splitting qwords
            }
            if (ps.nloop == 0) {
                if (ps.eop) ps.in_packet = false;
            }
            continue;
        }
        switch (ps.flg) {
        case 0: {  // PACKED
            uint32_t r = (uint32_t)((ps.tag_hi >> (ps.reg * 4)) & 0xF);
            packed_reg(r, lo, hi);
            if (++ps.reg == ps.nreg) {
                ps.reg = 0;
                ps.nloop--;
            }
            break;
        }
        case 1: {  // REGLIST: two 64-bit register values per qword
            uint64_t vals[2] = {lo, hi};
            for (int k = 0; k < 2 && ps.nloop; k++) {
                uint32_t r = (uint32_t)((ps.tag_hi >> (ps.reg * 4)) & 0xF);
                if (r < 0xE) write_reg(r, vals[k]);
                if (++ps.reg == ps.nreg) {
                    ps.reg = 0;
                    ps.nloop--;
                }
            }
            break;
        }
        default:  // IMAGE
            xfer_data(q, 16);
            ps.nloop--;
            break;
        }
        if (ps.nloop == 0 && ps.eop) ps.in_packet = false;
    }
}

// --------------------------------------------------------------- display
std::vector<uint32_t> g_frame;
int g_frame_w, g_frame_h;
uint64_t g_frames;

inline uint32_t to_rgba(uint32_t psm, uint32_t v) {
    switch (psm) {
    case PSMCT32:
        return v | 0xFF000000u;
    case PSMCT24:
        return v | 0xFF000000u;
    case PSMCT16:
    case PSMCT16S:
        return ((v & 0x1F) << 3) | (((v >> 5) & 0x1F) << 11) | (((v >> 10) & 0x1F) << 19) | 0xFF000000u;
    }
    return v | 0xFF000000u;
}

// Reads one display circuit into an RGBA image.
bool read_circuit(int n, std::vector<uint32_t> &img, int &w, int &h) {
    uint64_t fb = st.dispfb[n], disp = st.display[n];
    uint32_t fbp = bits(fb, 0, 9) * 32, fbw = bits(fb, 9, 6), psm = bits(fb, 15, 5);
    int dbx = (int)bits(fb, 32, 11), dby = (int)bits(fb, 43, 11);
    int magh = (int)bits(disp, 23, 4) + 1, magv = (int)bits(disp, 27, 2) + 1;
    int dw = (int)bits(disp, 32, 12) + 1, dh = (int)bits(disp, 44, 11) + 1;
    w = dw / magh;
    h = dh / magv;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) return false;
    census_display(fbp, fbw, psm, w, h);
    // field mode with interlace: only half the lines are in memory
    bool interlaced = st.smode2 & 1;
    bool field_mode = (st.smode2 >> 1) & 1;
    int lines = (interlaced && field_mode) ? h / 2 : h;
    img.resize((size_t)w * h);
    const SwizzleTab *T = swizzle_table(psm, fbw);
    bool b16 = psm == PSMCT16 || psm == PSMCT16S;
    for (int y = 0; y < h; y++) {
        int sy = (interlaced && field_mode) ? y / 2 : y;
        if (sy >= lines) sy = lines - 1;
        uint32_t *row = &img[(size_t)y * w];
        if (y > 0 && sy == ((interlaced && field_mode) ? (y - 1) / 2 : y - 1)) {
            memcpy(row, row - w, (size_t)w * 4);  // line doubling
            continue;
        }
        int py = (dby + sy) & 2047;
        uint32_t rb = T->row_base(fbp, py), rc = T->rowC[py];
        if (b16) {
            for (int x = 0; x < w; x++) row[x] = to_rgba(psm, half(T->addr_row(rb, rc, (dbx + x) & 2047)));
        } else {
            for (int x = 0; x < w; x++) row[x] = word(T->addr_row(rb, rc, (dbx + x) & 2047)) | 0xFF000000u;
        }
    }
    return true;
}

// Has anything the display shows changed since the last composition?
bool display_changed() {
    static uint64_t last_regs[8];
    static uint32_t last_clock;
    static bool valid;
    uint64_t regs[8] = {st.pmode, st.dispfb[0], st.dispfb[1], st.display[0], st.display[1], st.smode2, st.bgcolor, 0};
    raster_mark_pages();
    bool changed = !valid || memcmp(regs, last_regs, sizeof regs) != 0;
    for (int n = 0; n < 2 && !changed; n++) {
        if (!((st.pmode >> n) & 1)) continue;
        uint64_t fb = st.dispfb[n], disp = st.display[n];
        uint32_t fbp = bits(fb, 0, 9) * 32, fbw = bits(fb, 9, 6), psm = bits(fb, 15, 5);
        int dby = (int)bits(fb, 43, 11), dh = (int)bits(disp, 44, 11) + 1;
        uint32_t first, count;
        page_range(fbp, fbw, psm, 0, 0, (int)(fbw ? fbw : 1) * 64, std::min(2048, dby + dh * 2), first, count);
        for (uint32_t i = 0; i < count && !changed; i++)
            if (g_page_gen[(first + i) % kPages] > last_clock) changed = true;
    }
    memcpy(last_regs, regs, sizeof regs);
    last_clock = g_vram_clock;
    valid = true;
    return changed;
}

// Composes the displayed image into g_frame; false when it is unchanged.
bool compose_display() {
    raster_sync();
    if (!display_changed()) return false;
    bool en1 = st.pmode & 1, en2 = (st.pmode >> 1) & 1;
    std::vector<uint32_t> a, b;
    int wa = 0, ha = 0, wb = 0, hb = 0;
    bool ha_ok = en1 && read_circuit(0, a, wa, ha);
    bool hb_ok = en2 && read_circuit(1, b, wb, hb);
    if (!ha_ok && !hb_ok) {
        g_frame_w = 640;
        g_frame_h = 448;
        g_frame.assign(640 * 448, 0xFF000000u);
        return true;
    }
    if (ha_ok && hb_ok) {
        // blend circuit 1 over circuit 2
        bool mmod = (st.pmode >> 5) & 1;
        uint32_t alp = bits(st.pmode, 8, 8);
        int w = wa < wb ? wa : wb, h = ha < hb ? ha : hb;
        g_frame.resize((size_t)w * h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t p1 = a[(size_t)y * wa + x], p2 = b[(size_t)y * wb + x];
                uint32_t al = mmod ? alp : 255;  // per-pixel alpha would need the raw value
                // x / 255 == (x * 0x8081) >> 23 for x < 65536
                uint32_t r = ((((p1 & 0xFF) * al) + ((p2 & 0xFF) * (255 - al))) * 0x8081u) >> 23;
                uint32_t g = (((((p1 >> 8) & 0xFF) * al) + (((p2 >> 8) & 0xFF) * (255 - al))) * 0x8081u) >> 23;
                uint32_t bb = (((((p1 >> 16) & 0xFF) * al) + (((p2 >> 16) & 0xFF) * (255 - al))) * 0x8081u) >> 23;
                g_frame[(size_t)y * w + x] = r | (g << 8) | (bb << 16) | 0xFF000000u;
            }
        g_frame_w = w;
        g_frame_h = h;
        return true;
    }
    if (ha_ok) {
        g_frame.swap(a);
        g_frame_w = wa;
        g_frame_h = ha;
    } else {
        g_frame.swap(b);
        g_frame_w = wb;
        g_frame_h = hb;
    }
    return true;
}

}  // namespace
}  // namespace gs

using namespace gs;

extern int g_opt_frames;
extern int g_opt_screenshot_every;
extern std::string g_opt_screenshot_dir;

void priv_apply(uint32_t off, uint64_t v);
void gs_flush();
void gs_sync();
void gs_frame_end(uint64_t frame);

// ------------------------------------------------------------ GS thread
// With a second CPU core the GIF stream is executed on a worker thread:
// the EE side appends commands (GIF data, privileged register writes,
// vblank) to chunks that the worker runs in order.  The EE only waits for
// it when it reads state the worker produces.
namespace {

enum : uint32_t { CMD_GIF = 1, CMD_PRIV, CMD_VBLANK, CMD_PATH_RESET };

bool g_threaded;
// vblanks the GS thread may trail the EE (beyond that the EE waits; with
// frame skipping the GS normally catches up by dropping frames first)
uint64_t g_gs_max_lag = getenv("EOD_GS_LAG") ? (uint64_t)atoi(getenv("EOD_GS_LAG")) : 12;
std::atomic<uint64_t> g_gs_busy_us, g_ee_wait_us, g_ee_sync_us;  // EOD_FPS statistics
// never destroyed: the worker may still be waiting on them when the process exits
std::mutex &g_q_mtx = *new std::mutex;
std::condition_variable &g_q_cv = *new std::condition_variable, &g_q_cv_done = *new std::condition_variable;
std::deque<std::vector<uint8_t>> &g_q = *new std::deque<std::vector<uint8_t>>;
std::vector<uint8_t> g_chunk;
uint64_t g_pushed, g_done;  // chunks (guarded by g_q_mtx)
uint64_t g_frames_done;     // vblanks composed by the worker (guarded by g_q_mtx)

// last composed frame (presented on the EE thread)
std::mutex &g_pub_mtx = *new std::mutex;
std::vector<uint32_t> g_pub;
int g_pub_w, g_pub_h;

void push_cmd(uint32_t kind, uint32_t a, uint32_t b, const void *data, uint32_t size) {
    uint32_t h[4] = {kind, a, b, size};
    size_t at = g_chunk.size();
    g_chunk.resize(at + 16 + ((size + 15) & ~15u));
    memcpy(&g_chunk[at], h, 16);
    if (size) memcpy(&g_chunk[at + 16], data, size);
    if (g_chunk.size() >= 256 * 1024) gs_flush();
}

}  // namespace

// Publishes the display and does the per-frame bookkeeping (GS side).
void gs_frame_end(uint64_t frame) {
    if (hw::g_on) {
        if (display_changed()) {
            // no vsync wait while the GS thread trails the EE
            uint64_t lag = g_ee_vblanks.load() - frame;
            hw::present(g_config.vsync && lag < 2);
        }
        hw::vblank();
    } else if (compose_display()) {
        std::lock_guard<std::mutex> lk(g_pub_mtx);
        g_pub.swap(g_frame);
        g_pub_w = g_frame_w;
        g_pub_h = g_frame_h;
    }
    {
        static int trace_at = getenv("EOD_GS_TRACE") ? atoi(getenv("EOD_GS_TRACE")) : -1;
        if (trace_at >= 0) gs::g_trace = (int)frame >= trace_at && (int)frame < trace_at + 4;
        if (gs::g_trace) fprintf(stderr, "GS trace on at frame %d\n", (int)frame);
    }
    if ((frame % 60) == 0) {
        raster_collect_stats();
        LOG(LOG_GS, "frames to %llu: %u prims, %u pixels, %u transfers, %u texels decoded, %u uncached samples, %u env setups per vblank",
            (unsigned long long)frame, stats.prims / 60, stats.pixels / 60, stats.transfers / 60, stats.decoded / 60,
            stats.slow_samples / 60, stats.env_setups / 60);
        LOG(LOG_GS, "  raster time %.2f ms per vblank, %.1f ns per pixel", stats.raster_ns / 60 / 1e6,
            stats.pixels ? (double)stats.raster_ns / stats.pixels : 0.0);
        LOG(LOG_GS, "  raster jobs %u, barriers: other %u, tex-after-draw %u, draw-over-read %u, layout %u, "
            "tex-setup %u, clut %u, transfer %u (per vblank)", stats.jobs / 60, stats.barriers[0] / 60,
            stats.barriers[1] / 60, stats.barriers[2] / 60, stats.barriers[3] / 60, stats.barriers[4] / 60,
            stats.barriers[5] / 60, stats.barriers[6] / 60);
        for (int k = 0; k < 64; k++)
            if (stats.kind_px[k] / 60 > 5000)
                LOG(LOG_GS, "  tri kind gour=%d tex=%d stq=%d fog=%d zt=%d zw=%d: %u pixels per vblank", k & 1, (k >> 1) & 1,
                    (k >> 2) & 1, (k >> 3) & 1, (k >> 4) & 1, (k >> 5) & 1, stats.kind_px[k] / 60);
        if (hw::g_on && g_log_enabled[LOG_GS]) {  // totals over the last 60 vblanks
            char line[512];
            hw::stats_line(line, sizeof line);
            LOG(LOG_GS, "  %s", line);
        }
        stats = Stats();
    }
}

void gs_flush() {
    if (!g_threaded || g_chunk.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_q_mtx);
        g_q.push_back(std::move(g_chunk));
        g_pushed++;
    }
    g_q_cv.notify_one();
    g_chunk.clear();
    g_chunk.reserve(64 * 1024);
}

void gs_sync() {
    if (!g_threaded) return;
    gs_flush();
    uint64_t t = platform_time_us();
    std::unique_lock<std::mutex> lk(g_q_mtx);
    g_q_cv_done.wait(lk, [] { return g_done == g_pushed; });
    g_ee_wait_us += platform_time_us() - t;
    g_ee_sync_us += platform_time_us() - t;
}

// ------------------------------------------------------------ capture / replay
// EOD_GS_DUMP=file EOD_GS_DUMP_AT=vblank [EOD_GS_DUMP_N=count] records the GS
// state and VRAM after that vblank, then the commands of the next `count`
// vblanks; `eod --gs-replay file` replays them alone (renderer benchmarks).
namespace {
FILE *g_dump;
uint64_t g_dump_end;
const char kDumpMagic[8] = {'E', 'O', 'D', 'G', 'S', 'D', '0', '1'};

void dump_state(FILE *f) {
    uint32_t sizes[3] = {(uint32_t)sizeof st, (uint32_t)sizeof g_path, (uint32_t)sizeof g_xfer};
    fwrite(kDumpMagic, 1, 8, f);
    fwrite(sizes, 4, 3, f);
    fwrite(&st, sizeof st, 1, f);
    fwrite(g_path, sizeof g_path, 1, f);
    fwrite(&g_xfer, sizeof g_xfer, 1, f);
    fwrite(vram, 1, kVramSize, f);
}

bool load_state(FILE *f) {
    char magic[8];
    uint32_t sizes[3];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, kDumpMagic, 8) || fread(sizes, 4, 3, f) != 3 ||
        sizes[0] != sizeof st || sizes[1] != sizeof g_path || sizes[2] != sizeof g_xfer)
        return false;
    bool ok = fread(&st, sizeof st, 1, f) == 1 && fread(g_path, sizeof g_path, 1, f) == 1 &&
              fread(&g_xfer, sizeof g_xfer, 1, f) == 1 && fread(vram, 1, kVramSize, f) == kVramSize;
    if (g_xfer.active) g_xfer.T = swizzle_table(g_xfer.dpsm, g_xfer.dbw);
    g_xfer.ry = -1;
    return ok;
}
}  // namespace

// Runs queued GS commands; returns the last vblank seen.
uint64_t run_cmds(const uint8_t *c, size_t size, bool live) {
    uint64_t frames = 0;
    for (size_t p = 0; p < size;) {
        uint32_t h[4];
        memcpy(h, &c[p], 16);
        const uint8_t *d = &c[p + 16];
        size_t len = 16 + ((h[3] + 15) & ~15u);
        if (live && g_dump) fwrite(&c[p], 1, len, g_dump);
        switch (h[0]) {
        case CMD_GIF: gif_process((int)h[1], d, h[3] / 16); break;
        case CMD_PRIV: {
            uint64_t v;
            memcpy(&v, d, 8);
            priv_apply(h[1], v);
            break;
        }
        case CMD_VBLANK:
            g_gs_vblanks = h[1];
            gs_frame_end(h[1]);
            frames = h[1];
            if (live) {
                // EOD_GS_DUMP_AT may list several vblanks (a,b,c): one file per
                // capture, named <EOD_GS_DUMP>.<vblank> when there are several
                static const char *dump = getenv("EOD_GS_DUMP");
                static std::vector<uint64_t> at_list = [] {
                    std::vector<uint64_t> v;
                    if (const char *a = getenv("EOD_GS_DUMP_AT"))
                        for (const char *p = a; *p;) {
                            char *e;
                            uint64_t n = strtoull(p, &e, 10);
                            if (e == p) break;
                            v.push_back(n);
                            p = *e == ',' ? e + 1 : e;
                        }
                    return v;
                }();
                if (g_dump && h[1] >= g_dump_end) {
                    fclose(g_dump);
                    g_dump = nullptr;
                    fprintf(stderr, "GS dump written\n");
                }
                if (dump && !g_dump && std::find(at_list.begin(), at_list.end(), h[1]) != at_list.end()) {
                    raster_sync();
                    std::string name = dump;
                    if (at_list.size() > 1) name += "." + std::to_string(h[1]);
                    g_dump = fopen(name.c_str(), "wb");
                    if (g_dump) dump_state(g_dump);
                    g_dump_end = h[1] + (getenv("EOD_GS_DUMP_N") ? strtoull(getenv("EOD_GS_DUMP_N"), nullptr, 10) : 4);
                }
            }
            break;
        case CMD_PATH_RESET:
            for (auto &ps : g_path) ps = PathState();
            break;
        }
        p += len;
    }
    return frames;
}

// eod --gs-replay FILE [--repeat N]: replays a capture, prints the time
// and writes the last frame to FILE.ppm.
int gs_replay(const char *path, int repeat) {
    mem_init();
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "can't open %s\n", path);
        return 1;
    }
    long start = 0;
    {
        std::vector<uint8_t> tmp;
        if (!load_state(f)) {
            fprintf(stderr, "%s is not a GS capture from this build\n", path);
            return 1;
        }
        start = ftell(f);
    }
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    std::vector<uint8_t> cmds((size_t)(end - start));
    fseek(f, start, SEEK_SET);
    if (fread(cmds.data(), 1, cmds.size(), f) != cmds.size()) return 1;
    if ((g_config.renderer == "d3d11" || g_config.renderer == "warp") && hw::start(false)) fprintf(stderr, "replaying with Direct3D 11\n");
    int hw = platform_cpu_count();
    int n = hw >= 4 ? hw - 2 : hw == 3 ? 1 : 0;
    if (const char *r = getenv("EOD_GS_RASTER")) n = atoi(r);
    if (hw::g_on) n = 0;
    raster_start(n);
    double best = 1e30;
    for (int r = 0; r < repeat; r++) {
        fseek(f, 0, SEEK_SET);
        load_state(f);
        raster_reset();
        if (hw::g_on) hw::reset();
        // EOD_GS_REPLAY_TRACE: log every primitive of the first replay
        static const bool replay_trace = getenv("EOD_GS_REPLAY_TRACE") != nullptr;
        gs::g_trace = replay_trace && r == 0;
        uint64_t t0 = platform_time_us();
        run_cmds(cmds.data(), cmds.size(), false);
        raster_sync();
        if (const char *dump = getenv("EOD_GS_REPLAY_DUMP")) {  // the GS memory after the replay
            if (hw::g_on)
                for (uint32_t p = 0; p < kPages; p++) hw::vram_needed(p * 32, 1, PSMCT32, 0, 0, 64, 32);
            if (FILE *df = fopen(dump, "wb")) {
                fwrite(vram, 1, kVramSize, df);
                fclose(df);
            }
        }
        g_prim_counter = 0;
        if (hw::g_on) {
            hw::finish();
            char line[512];
            hw::stats_line(line, sizeof line);
            fprintf(stderr, "%s\n", line);
        }
        double ms = (platform_time_us() - t0) / 1000.0;
        best = std::min(best, ms);
        fprintf(stderr, "replay %d: %.1f ms\n", r + 1, ms);
    }
    fclose(f);
    raster_collect_stats();
    fprintf(stderr, "best %.1f ms, %d raster workers; per replay: %u prims, %u pixels, %llu game frames\n", best, n,
            stats.prims / repeat, stats.pixels / repeat, (unsigned long long)(g_flips.load() / repeat));
    uint32_t tri = 0;
    for (int k = 0; k < 64; k++) {
        tri += stats.kind_px[k];
        if (stats.kind_px[k] / repeat > 20000)
            fprintf(stderr, "  triangles gour=%d tex=%d stq=%d fog=%d zt=%d zw=%d: %u pixels\n", k & 1, (k >> 1) & 1,
                    (k >> 2) & 1, (k >> 3) & 1, (k >> 4) & 1, (k >> 5) & 1, stats.kind_px[k] / repeat);
    }
    fprintf(stderr, "  sprites/lines: %u pixels\n", (stats.pixels - tri) / repeat);
    std::string shot = std::string(path) + ".ppm";
    gs_screenshot(shot.c_str());
    return 0;
}

static void gs_worker() {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "eod-gs");
#endif
    hw::bind_thread();
    for (;;) {
        std::vector<uint8_t> c;
        {
            std::unique_lock<std::mutex> lk(g_q_mtx);
            g_q_cv.wait(lk, [] { return !g_q.empty(); });
            c = std::move(g_q.front());
            g_q.pop_front();
        }
        uint64_t t_start = platform_time_us();
        uint64_t frames = run_cmds(c.data(), c.size(), true);
        g_gs_busy_us += platform_time_us() - t_start;
        {
            std::lock_guard<std::mutex> lk(g_q_mtx);
            g_done++;
            if (frames) g_frames_done = frames;
        }
        g_q_cv_done.notify_all();
    }
}

void gs_thread_start() {
    const char *e = getenv("EOD_GS_THREAD");
    bool want = e ? atoi(e) != 0 : platform_cpu_count() >= 2;
    if (!want) return;
    g_threaded = true;
    std::thread(gs_worker).detach();
    // raster workers on the cores left after the EE and GS threads
    // (EOD_GS_RASTER=n overrides, 0 disables)
    int hw = platform_cpu_count();
    int n = hw >= 4 ? hw - 2 : hw == 3 ? 1 : 0;
    if (const char *r = getenv("EOD_GS_RASTER")) n = atoi(r);
    if (n > 4) n = 4;
    if (hw::g_on) n = 0;  // the GPU draws
    raster_start(n);
    LOG(LOG_GS, "GS runs on its own thread, %d raster workers", n > 0 ? n : 0);
}

void gs_init() {
    mem_init();
    memset(&st, 0, sizeof st);
    st.imr = 0xFF00;
    st.prmodecont = 1;
    if (g_config.renderer == "d3d11" || g_config.renderer == "warp") {
        if (hw::start(platform_native_window() != nullptr && !getenv("EOD_D3D_NOWINDOW")))
            LOG(LOG_GS, "Direct3D 11 renderer");
        else
            LOG(LOG_GS, "Direct3D 11 is not available - using the software renderer");
    }
    gs_thread_start();
}

void gif_transfer(int path, const uint8_t *data, uint32_t qwc) {
    if (!g_threaded) {
        gif_process(path, data, qwc);
        return;
    }
    push_cmd(CMD_GIF, (uint32_t)path, 0, data, qwc * 16);
}

namespace {
std::vector<uint8_t> g_p3q;  // queued PATH3 data
size_t g_p3pos;

// Follows GIF tags to find packet boundaries (without executing them).
struct GifTracker {
    bool in_packet = false;
    uint32_t left = 0;  // data quadwords left in the current tag
    bool eop = false;
    void feed(const uint8_t *q) {
        if (!in_packet || left == 0) {
            uint64_t lo;
            memcpy(&lo, q, 8);
            uint32_t nloop = (uint32_t)(lo & 0x7FFF), flg = (uint32_t)((lo >> 58) & 3);
            uint32_t nreg = (uint32_t)((lo >> 60) & 0xF);
            if (!nreg) nreg = 16;
            eop = (lo >> 15) & 1;
            left = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
            in_packet = true;
            if (left == 0 && eop) in_packet = false;
            return;
        }
        if (--left == 0 && eop) in_packet = false;
    }
} g_p3track;
bool g_p3_vif_mask, g_p3_mode_mask;
}  // namespace

void gif_path3_enqueue(const uint8_t *data, uint32_t qwc) {
    g_p3q.insert(g_p3q.end(), data, data + (size_t)qwc * 16);
}

bool gif_path3_pending() { return g_p3pos < g_p3q.size(); }

void gif_set_path3_mask(bool masked) { g_p3_vif_mask = masked; }

bool gif_path3_run(int max_packets) {
    if (g_p3_vif_mask || g_p3_mode_mask) return !gif_path3_pending();
    int done = 0;
    size_t start = g_p3pos;
    while (g_p3pos < g_p3q.size()) {
        g_p3track.feed(&g_p3q[g_p3pos]);
        g_p3pos += 16;
        if (!g_p3track.in_packet) {  // packet finished (EOP)
            if (max_packets >= 0 && ++done >= max_packets) break;
        }
    }
    if (g_p3pos > start) gif_transfer(GIF_PATH3, &g_p3q[start], (uint32_t)((g_p3pos - start) / 16));
    if (g_p3pos >= g_p3q.size()) {
        g_p3q.clear();
        g_p3pos = 0;
        // the GIF DMA channel completes once its data has been consumed
        dmac_channel_done(DMA_GIF);
        return true;
    }
    return false;
}

uint32_t gif_reg_read(uint32_t off) {
    switch (off) {
    case 0x3000: return gif_reg_ctrl;
    case 0x3010: return gif_reg_mode;
    case 0x3020: return 0;  // STAT: idle
    }
    return 0;
}

void gif_reg_write(uint32_t off, uint32_t v) {
    if (off == 0x3000) {
        gif_reg_ctrl = v;
        if (v & 1) {
            g_p3track = GifTracker();
            if (g_threaded) push_cmd(CMD_PATH_RESET, 0, 0, nullptr, 0);
            else
                for (auto &p : g_path) p = PathState();
        }
    } else if (off == 0x3010) {
        gif_reg_mode = v;
        g_p3_mode_mask = v & 1;  // M3R
        if (!g_p3_mode_mask) gif_path3_run(-1);
    }
}

void gs_set_crt(int interlace, int mode, int ffmd) {
    LOG(LOG_GS, "SetGsCrt(interlace=%d, mode=%d, ffmd=%d)", interlace, mode, ffmd);
    gs_priv_write64(0x0020, (interlace & 1) | ((ffmd & 1) << 1));
}

uint32_t gs_get_imr() { return (uint32_t)st.imr; }
void gs_put_imr(uint32_t v) { st.imr = v; }

uint64_t gs_priv_read64(uint32_t p) {
    uint32_t off = p & 0x1FFF;
    if (off == 0x1000) {
        gs_flush();  // let the GS thread reach pending FINISH/SIGNAL
        return g_csr;
    }
    if (off == 0x1010) return st.imr;
    if (off == 0x1080) {
        static uint32_t n;
        if ((++n & 255) == 1) LOG(LOG_GS, "SIGLBLID read #%u", n);
        gs_sync();
        return st.siglblid;
    }
    return 0;
}

void gs_priv_write64(uint32_t p, uint64_t v) {
    uint32_t off = p & 0x1FFF;
    if (off == 0x1000) {
        // CSR: writing 1 clears SIGNAL/FINISH/HSINT/VSINT/EDWINT
        g_csr &= ~(v & 0x1F);
        if (v & 0x200) {  // RESET
            if (g_threaded) push_cmd(CMD_PATH_RESET, 0, 0, nullptr, 0);
            else
                for (auto &ps : g_path) ps = PathState();
        }
        return;
    }
    if (off == 0x1010) {
        st.imr = v;
        return;
    }
    if (g_threaded) {
        push_cmd(CMD_PRIV, off, 0, &v, 8);
        return;
    }
    priv_apply(off, v);
}

void priv_apply(uint32_t off, uint64_t v) {
    switch (off) {
    case 0x0000: st.pmode = v; break;
    case 0x0010: st.smode1 = v; break;
    case 0x0020: st.smode2 = v; break;
    case 0x0030: st.srfsh = v; break;
    case 0x0040: st.synch1 = v; break;
    case 0x0050: st.synch2 = v; break;
    case 0x0060: st.syncv = v; break;
    case 0x0070: st.dispfb[0] = v; break;
    case 0x0080: st.display[0] = v; break;
    case 0x0090: st.dispfb[1] = v; break;
    case 0x00A0: st.display[1] = v; break;
    case 0x00B0: st.extbuf = v; break;
    case 0x00C0: st.extdata = v; break;
    case 0x00D0: st.extwrite = v; break;
    case 0x00E0: st.bgcolor = v; break;
    case 0x1040: st.busdir = v; break;
    case 0x1080: st.siglblid = v; break;
    default: break;
    }
}

void gs_priv_write32(uint32_t p, uint32_t v) {
    uint32_t off = p & 0x1FFF;
    uint64_t cur = 0;
    switch (off & ~7u) {
    case 0x1000: cur = g_csr; break;
    case 0x1010: cur = st.imr; break;
    default: break;
    }
    if (off & 4)
        gs_priv_write64(off & ~7u, (cur & 0xFFFFFFFFull) | ((uint64_t)v << 32));
    else if ((off & ~7u) == 0x1000)
        gs_priv_write64(off, v);  // CSR: only the written bits matter
    else
        gs_priv_write64(off, (cur & ~0xFFFFFFFFull) | v);
}

bool gs_screenshot(const char *path) {
    std::vector<uint32_t> img;
    int w = 0, h = 0;
    if (hw::g_on) {
        if (!hw::read_display(img, w, h)) return false;
    } else {
        std::lock_guard<std::mutex> lk(g_pub_mtx);
        if (g_pub.empty()) return false;
        img = g_pub;
        w = g_pub_w;
        h = g_pub_h;
    }
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (uint32_t px : img) {
        uint8_t rgb[3] = {(uint8_t)px, (uint8_t)(px >> 8), (uint8_t)(px >> 16)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return true;
}

void gs_vblank() {
    // VSINT, field toggle
    g_csr |= 8;
    g_csr ^= 0x2000;
    if (!(st.imr & 0x800)) intc_raise(INTC_GS);
    g_frames++;
    g_ee_vblanks = g_frames;
    log_frame(g_frames);
    if (g_threaded) {
        uint64_t f = g_frames;
        push_cmd(CMD_VBLANK, (uint32_t)f, 0, nullptr, 0);
        gs_flush();
        // don't let the GS thread fall more than a couple of frames behind
        uint64_t t = platform_time_us();
        std::unique_lock<std::mutex> lk(g_q_mtx);
        g_q_cv_done.wait(lk, [] { return g_frames_done + g_gs_max_lag >= g_frames; });
        g_ee_wait_us += platform_time_us() - t;
    } else {
        gs_frame_end(g_frames);
    }
    {
        std::lock_guard<std::mutex> lk(g_pub_mtx);
        if (!g_pub.empty()) platform_present(g_pub.data(), g_pub_w, g_pub_h, g_pub_w);
    }
    {
        // speed in the window title, every second
        static uint64_t tt0, tf0, tv0;
        uint64_t tnow = platform_time_us();
        if (!tt0) tt0 = tnow, tf0 = g_flips, tv0 = g_frames;
        if (tnow - tt0 >= 1000000) {
            double sec = (double)(tnow - tt0) / 1e6;
            char buf[96];
            snprintf(buf, sizeof buf, "%.0f%% speed, %.0f fps", (double)(g_frames - tv0) / sec * (100.0 / 59.94),
                     (double)(g_flips - tf0) / sec);
            platform_set_status(buf);
            tt0 = tnow, tf0 = g_flips, tv0 = g_frames;
        }
    }
    {
        // EOD_FPS=1: print the game's frame rate every 5 seconds
        static bool show = getenv("EOD_FPS") != nullptr;
        static uint64_t t0, f0, g0, v0, i0;
        if (show) {
            extern std::atomic<uint64_t> g_ee_idle_us;
            uint64_t now = platform_time_us();
            if (!t0) t0 = now, f0 = g_flips, g0 = g_game_flips, v0 = g_frames, i0 = g_ee_idle_us;
            if (now - t0 >= 5000000) {
                double sec = (double)(now - t0) / 1e6;
                fprintf(stderr,
                        "fps: %.1f vblank/s, game %.1f frames/s, drawn %.1f frames/s, EE busy %.0f%%, GS busy %.0f%%, "
                        "EE waiting %.0f%% (%.0f%% in syncs)\n",
                        (double)(g_frames - v0) / sec, (double)(g_game_flips - g0) / sec, (double)(g_flips - f0) / sec,
                        100.0 - (double)(g_ee_idle_us - i0) / (sec * 1e4), g_gs_busy_us / (sec * 1e4),
                        g_ee_wait_us / (sec * 1e4), g_ee_sync_us / (sec * 1e4));
                g_ee_sync_us = 0;
                g_gs_busy_us = 0;
                g_ee_wait_us = 0;
                t0 = now;
                f0 = g_flips;
                g0 = g_game_flips;
                v0 = g_frames;
                i0 = g_ee_idle_us;
            }
        }
    }
    if (g_opt_screenshot_every && (g_frames % g_opt_screenshot_every) == 0) {
        char path[1024];
        snprintf(path, sizeof path, "%s/frame_%05llu.ppm", g_opt_screenshot_dir.c_str(), (unsigned long long)g_frames);
        gs_screenshot(path);
        if (getenv("EOD_DUMP_VRAM")) {  // raw 4MB VRAM next to the screenshot
            gs_sync();
            snprintf(path, sizeof path, "%s/vram_%05llu.bin", g_opt_screenshot_dir.c_str(), (unsigned long long)g_frames);
            if (FILE *f = fopen(path, "wb")) {
                fwrite(vram, 1, kVramSize, f);
                fclose(f);
            }
        }
    }
    if (g_opt_frames && (int)g_frames >= g_opt_frames) {
        LOG(LOG_CORE, "reached %d frames - exiting", g_opt_frames);
        exit(0);
    }
}
