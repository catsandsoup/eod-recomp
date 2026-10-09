// hw.cpp - EE hardware register file: timers, INTC, DMAC, SBUS, FIFOs.
#include "hw.h"

#include <algorithm>
#include <cstring>

#include "../core/log.h"
#include "../core/memory.h"
#include "../gs/gs.h"
#include "../iop/sif.h"
#include "../kernel/kernel.h"
#include "../platform/platform.h"
#include "../vu/vif.h"
#include "vu.h"
#include "ipu.h"

namespace {

// ----------------------------------------------------------------- timers
struct Timer {
    uint32_t mode = 0;
    uint32_t comp = 0;
    uint32_t hold = 0;
    uint32_t base_count = 0;
    uint64_t base_us = 0;
};
Timer g_timers[4];

double timer_rate(const Timer &t) {
    switch (t.mode & 3) {
    case 0: return 147456000.0;
    case 1: return 147456000.0 / 16;
    case 2: return 147456000.0 / 256;
    default: return 15734.0;  // HBLANK
    }
}

uint32_t timer_count(const Timer &t) {
    if (!(t.mode & 0x80)) return t.base_count & 0xFFFF;
    double el = (double)(platform_time_us() - t.base_us) * 1e-6;
    return (uint32_t)(t.base_count + (uint64_t)(el * timer_rate(t))) & 0xFFFF;
}

void timer_write(int n, uint32_t reg, uint32_t v) {
    Timer &t = g_timers[n];
    switch (reg) {
    case 0x00:
        t.base_count = v & 0xFFFF;
        t.base_us = platform_time_us();
        break;
    case 0x10:
        t.base_count = timer_count(t);
        t.base_us = platform_time_us();
        t.mode = v & 0x3FF;
        if (v & 0x300) LOG(LOG_HW, "timer %d interrupts enabled (mode %x) - not emulated", n, v);
        break;
    case 0x20:
        t.comp = v & 0xFFFF;
        break;
    case 0x30:
        t.hold = v & 0xFFFF;
        break;
    }
}

uint32_t timer_read(int n, uint32_t reg) {
    Timer &t = g_timers[n];
    switch (reg) {
    case 0x00: return timer_count(t);
    case 0x10: return t.mode;
    case 0x20: return t.comp;
    case 0x30: return t.hold;
    }
    return 0;
}

// ------------------------------------------------------------------- DMAC
struct DmaChan {
    uint32_t chcr = 0, madr = 0, qwc = 0, tadr = 0, asr0 = 0, asr1 = 0, sadr = 0;
};
DmaChan g_dma[10];
uint32_t g_d_ctrl, g_d_stat, g_d_pcr, g_d_sqwc, g_d_rbsr, g_d_rbor, g_d_stadr;
uint32_t g_d_enable = 0x1201;

// incremental state of the IPU channels (0 = fromIPU, 1 = toIPU)
struct IpuDma {
    bool in_progress = false;  // MADR/QWC hold data still to transfer
    bool last = false;         // the channel ends when that data is done
};
IpuDma g_ipu_dma[2];

const uint32_t k_chan_base[10] = {0x8000, 0x9000, 0xA000, 0xB000, 0xB400, 0xC000, 0xC400, 0xC800, 0xD000, 0xD400};

int chan_from_addr(uint32_t off) {
    for (int i = 9; i >= 0; i--)
        if (off >= k_chan_base[i]) return (off - k_chan_base[i] < 0x100) ? i : -1;
    return -1;
}

// Host pointer to n quadwords at a DMA address (bit 31 = scratchpad).
uint8_t *dma_ptr(uint32_t addr) {
    if (addr & 0x80000000u) return g_scratchpad + (addr & 0x3FF0);
    addr &= 0x01FFFFF0;
    return g_ram + addr;
}

void dma_sink(int ch, const uint8_t *data, uint32_t qwc) {
    if (!qwc) return;
    switch (ch) {
    case DMA_VIF0:
        vif_write(0, reinterpret_cast<const uint32_t *>(data), qwc * 4);
        break;
    case DMA_VIF1:
        vif_write(1, reinterpret_cast<const uint32_t *>(data), qwc * 4);
        break;
    case DMA_GIF:
        gif_path3_enqueue(data, qwc);
        break;
    case DMA_SPR_TO: {
        DmaChan &c = g_dma[ch];
        uint32_t s = c.sadr & 0x3FF0;
        for (uint32_t i = 0; i < qwc; i++) {
            memcpy(g_scratchpad + s, data + i * 16, 16);
            s = (s + 16) & 0x3FF0;
        }
        c.sadr = s;
        break;
    }
    default:
        WARN("DMA sink for channel %d not implemented", ch);
    }
}

void dma_run_normal(int ch) {
    DmaChan &c = g_dma[ch];
    uint32_t qwc = c.qwc & 0xFFFF;
    LOG(LOG_DMA, "DMA ch%d normal madr=%08x qwc=%x", ch, c.madr, qwc);
    if (ch == DMA_SPR_FROM) {
        uint32_t s = c.sadr & 0x3FF0;
        for (uint32_t i = 0; i < qwc; i++) {
            memcpy(dma_ptr(c.madr), g_scratchpad + s, 16);
            s = (s + 16) & 0x3FF0;
            c.madr += 16;
        }
        c.sadr = s;
    } else {
        // the chunk may straddle the end of RAM in theory; copy in pieces
        dma_sink(ch, dma_ptr(c.madr), qwc);
        c.madr += qwc * 16;
    }
    c.qwc = 0;
}

void dma_run_chain(int ch) {
    DmaChan &c = g_dma[ch];
    bool tte = (c.chcr >> 6) & 1;
    bool tie = (c.chcr >> 7) & 1;
    int guard = 0;
    // a transfer already in progress (qwc != 0) first finishes the pending data
    if (c.qwc & 0xFFFF) {
        dma_sink(ch, dma_ptr(c.madr), c.qwc & 0xFFFF);
        c.madr += (c.qwc & 0xFFFF) * 16;
        c.qwc = 0;
    }
    for (;;) {
        if (++guard > 1000000) {
            WARN("DMA ch%d: runaway chain at %08x", ch, c.tadr);
            break;
        }
        uint8_t *tp = dma_ptr(c.tadr);
        uint64_t tag;
        memcpy(&tag, tp, 8);
        uint32_t qwc = (uint32_t)(tag & 0xFFFF);
        uint32_t id = (uint32_t)(tag >> 28) & 7;
        bool irq = (tag >> 31) & 1;
        uint32_t addr = (uint32_t)(tag >> 32) & 0x7FFFFFF0;
        if (tag >> 63) addr |= 0x80000000u;
        c.chcr = (c.chcr & 0xFFFF) | ((uint32_t)(tag >> 16) & 0xFFFF0000u);
        LOG(LOG_DMA, "DMA ch%d tag @%08x id=%d qwc=%x addr=%08x", ch, c.tadr, id, qwc, addr);
        if (tte && (ch == DMA_VIF0 || ch == DMA_VIF1)) {
            vif_write(ch, reinterpret_cast<const uint32_t *>(tp + 8), 2);
        } else if (tte && ch == DMA_GIF) {
            // GIF ignores TTE (would send the whole tag); real hw behaviour is undefined
        }
        bool end = false;
        uint32_t next = c.tadr + 16;
        switch (id) {
        case 0:  // refe
            dma_sink(ch, dma_ptr(addr), qwc);
            c.madr = addr + qwc * 16;
            end = true;
            break;
        case 1:  // cnt
            dma_sink(ch, dma_ptr(c.tadr + 16), qwc);
            c.madr = c.tadr + 16 + qwc * 16;
            next = c.madr;
            break;
        case 2:  // next
            dma_sink(ch, dma_ptr(c.tadr + 16), qwc);
            c.madr = c.tadr + 16 + qwc * 16;
            next = addr;
            break;
        case 3:  // ref
        case 4:  // refs
            dma_sink(ch, dma_ptr(addr), qwc);
            c.madr = addr + qwc * 16;
            break;
        case 5: {  // call
            dma_sink(ch, dma_ptr(c.tadr + 16), qwc);
            uint32_t ret = c.tadr + 16 + qwc * 16;
            int asp = (c.chcr >> 4) & 3;
            if (asp == 0) c.asr0 = ret;
            else if (asp == 1) c.asr1 = ret;
            else WARN("DMA ch%d call stack overflow", ch);
            c.chcr = (c.chcr & ~0x30u) | ((asp + 1) << 4);
            c.madr = ret;
            next = addr;
            break;
        }
        case 6: {  // ret
            dma_sink(ch, dma_ptr(c.tadr + 16), qwc);
            c.madr = c.tadr + 16 + qwc * 16;
            int asp = (c.chcr >> 4) & 3;
            if (asp == 0) {
                end = true;
            } else {
                asp--;
                next = asp == 0 ? c.asr0 : c.asr1;
                c.chcr = (c.chcr & ~0x30u) | (asp << 4);
            }
            break;
        }
        case 7:  // end
            dma_sink(ch, dma_ptr(c.tadr + 16), qwc);
            c.madr = c.tadr + 16 + qwc * 16;
            end = true;
            break;
        }
        c.tadr = next;
        c.qwc = 0;
        if (end || (irq && tie)) break;
    }
}

void dma_run_dest_chain(int ch) {
    // fromSPR destination chain: tags are read from the scratchpad source
    DmaChan &c = g_dma[ch];
    if (ch != DMA_SPR_FROM) {
        WARN("dest chain on ch%d unsupported", ch);
        return;
    }
    for (int guard = 0; guard < 100000; guard++) {
        uint32_t s = c.sadr & 0x3FF0;
        uint64_t tag;
        memcpy(&tag, g_scratchpad + s, 8);
        uint32_t qwc = tag & 0xFFFF;
        uint32_t id = (tag >> 28) & 7;
        uint32_t addr = (uint32_t)(tag >> 32) & 0x7FFFFFF0;
        bool irq = (tag >> 31) & 1;
        s = (s + 16) & 0x3FF0;
        for (uint32_t i = 0; i < qwc; i++) {
            memcpy(dma_ptr(addr), g_scratchpad + s, 16);
            s = (s + 16) & 0x3FF0;
            addr += 16;
        }
        c.sadr = s;
        c.madr = addr;
        if (id == 7 || (irq && ((c.chcr >> 7) & 1))) break;
    }
}

void dma_start(int ch) {
    DmaChan &c = g_dma[ch];
    if (!(g_d_ctrl & 1)) {
        LOG(LOG_DMA, "DMA ch%d start while DMAC disabled", ch);
    }
    int mode = (c.chcr >> 2) & 3;
    if (ch == DMA_SIF0 || ch == DMA_SIF1 || ch == DMA_SIF2) {
        // SIF DMA is driven by the kernel syscalls in this HLE.
        LOG(LOG_SIF, "direct SIF DMA ch%d chcr=%08x ignored", ch, c.chcr);
        c.chcr &= ~0x100u;
        return;
    }
    if (ch == DMA_IPU_FROM || ch == DMA_IPU_TO) {
        // paced by the IPU FIFOs: the IPU pulls/pushes data as it decodes
        g_ipu_dma[ch - DMA_IPU_FROM] = IpuDma();
        if (ch == DMA_IPU_FROM && !(c.qwc & 0xFFFF)) {
            dmac_channel_done(ch);
            return;
        }
        if (ch == DMA_IPU_TO && mode == 1 && (c.qwc & 0xFFFF)) {
            // resuming a chain: the tag in CHCR decides whether it ends after this data
            uint32_t id = (c.chcr >> 28) & 7;
            g_ipu_dma[1].in_progress = true;
            g_ipu_dma[1].last = id == 0 || id == 7 || ((c.chcr >> 31) && ((c.chcr >> 7) & 1));
        } else if (ch == DMA_IPU_TO && mode == 0) {
            g_ipu_dma[1].in_progress = true;
            g_ipu_dma[1].last = true;
        }
        ipu_dma_kick();
        return;
    }
    if (mode == 1) {
        if (ch == DMA_SPR_FROM)
            dma_run_dest_chain(ch);
        else
            dma_run_chain(ch);
    } else if (mode == 0) {
        dma_run_normal(ch);
    } else {
        // interleave (scratchpad only)
        uint32_t tqwc = g_d_sqwc >> 16 & 0xFF, sqwc = g_d_sqwc & 0xFF;
        uint32_t qwc = c.qwc & 0xFFFF;
        uint32_t s = c.sadr & 0x3FF0;
        LOG(LOG_DMA, "DMA ch%d interleave qwc=%x tqwc=%d sqwc=%d", ch, qwc, tqwc, sqwc);
        while (qwc) {
            for (uint32_t i = 0; i < tqwc && qwc; i++, qwc--) {
                if (ch == DMA_SPR_TO) memcpy(g_scratchpad + s, dma_ptr(c.madr), 16);
                else memcpy(dma_ptr(c.madr), g_scratchpad + s, 16);
                s = (s + 16) & 0x3FF0;
                c.madr += 16;
            }
            c.madr += sqwc * 16;
        }
        c.sadr = s;
        c.qwc = 0;
    }
    if (ch == DMA_GIF) {
        // completes (STR cleared) when PATH3 has consumed the data
        if (!gif_path3_pending()) dmac_channel_done(ch);
        else gif_path3_run(-1);
        return;
    }
    dmac_channel_done(ch);
    if (ch == DMA_VIF1 && gif_path3_pending()) gif_path3_run(-1);
}

uint32_t g_sio_line_len;
char g_sio_line[512];

}  // namespace

void dmac_channel_done(int ch) {
    g_dma[ch].chcr &= ~0x100u;
    g_d_stat |= 1u << ch;
    if (g_d_stat & (1u << (16 + ch))) dmac_raise(ch);
}

bool dma_ipu_active(int ch) { return (g_dma[ch].chcr & 0x100) != 0; }

uint32_t dma_ipu_to_fetch(uint8_t *dst, uint32_t maxqw) {
    DmaChan &c = g_dma[DMA_IPU_TO];
    IpuDma &st = g_ipu_dma[1];
    uint32_t got = 0;
    for (int guard = 0; got < maxqw && (c.chcr & 0x100) && guard < 10000; guard++) {
        if (!st.in_progress) {
            if (st.last) {
                dmac_channel_done(DMA_IPU_TO);
                break;
            }
            if ((c.chcr >> 2 & 3) != 1) {  // normal mode with QWC = 0
                dmac_channel_done(DMA_IPU_TO);
                break;
            }
            // read the next source chain tag
            uint64_t tag;
            memcpy(&tag, dma_ptr(c.tadr), 8);
            uint32_t qwc = (uint32_t)(tag & 0xFFFF);
            uint32_t id = (uint32_t)(tag >> 28) & 7;
            bool irq = (tag >> 31) & 1;
            uint32_t addr = (uint32_t)(tag >> 32) & 0x7FFFFFF0;
            if (tag >> 63) addr |= 0x80000000u;
            c.chcr = (c.chcr & 0xFFFF) | ((uint32_t)(tag >> 16) & 0xFFFF0000u);
            uint32_t next = c.tadr + 16;
            bool end = false;
            switch (id) {
            case 0: c.madr = addr; end = true; break;                    // refe
            case 1: c.madr = c.tadr + 16; next = c.madr + qwc * 16; break;  // cnt
            case 2: c.madr = c.tadr + 16; next = addr; break;                // next
            case 3: case 4: c.madr = addr; break;                            // ref(s)
            case 5: {                                                         // call
                c.madr = c.tadr + 16;
                int asp = (c.chcr >> 4) & 3;
                if (asp == 0) c.asr0 = c.madr + qwc * 16;
                else c.asr1 = c.madr + qwc * 16;
                c.chcr = (c.chcr & ~0x30u) | ((asp + 1) << 4);
                next = addr;
                break;
            }
            case 6: {                                                         // ret
                c.madr = c.tadr + 16;
                int asp = (c.chcr >> 4) & 3;
                if (asp == 0) {
                    end = true;
                } else {
                    asp--;
                    next = asp == 0 ? c.asr0 : c.asr1;
                    c.chcr = (c.chcr & ~0x30u) | (asp << 4);
                }
                break;
            }
            default: c.madr = c.tadr + 16; end = true; break;                 // end
            }
            c.tadr = next;
            c.qwc = qwc;
            st.in_progress = qwc != 0;
            st.last = end || (irq && ((c.chcr >> 7) & 1));
            continue;
        }
        uint32_t n = std::min<uint32_t>(c.qwc & 0xFFFF, maxqw - got);
        memcpy(dst + got * 16, dma_ptr(c.madr), n * 16);
        c.madr += n * 16;
        c.qwc -= n;
        got += n;
        if (!(c.qwc & 0xFFFF)) st.in_progress = false;
    }
    if ((c.chcr & 0x100) && !st.in_progress && st.last) dmac_channel_done(DMA_IPU_TO);
    return got;
}

uint32_t dma_ipu_from_store(const uint8_t *src, uint32_t n) {
    DmaChan &c = g_dma[DMA_IPU_FROM];
    if (!(c.chcr & 0x100)) return 0;
    uint32_t k = std::min<uint32_t>(n, c.qwc & 0xFFFF);
    for (uint32_t i = 0; i < k; i++) {
        memcpy(dma_ptr(c.madr), src + i * 16, 16);
        c.madr += 16;
    }
    c.qwc -= k;
    if (!(c.qwc & 0xFFFF)) dmac_channel_done(DMA_IPU_FROM);
    return k;
}

bool hw_dmac_channel_irq_enabled(int ch) { return (g_d_stat >> (16 + ch)) & 1; }

int hw_dmac_set_channel_irq(int ch, bool on) {
    if (ch < 0 || ch > 15) return -1;
    int was = (g_d_stat >> (16 + ch)) & 1;
    if (on) {
        g_d_stat |= 1u << (16 + ch);
        if (!was && (g_d_stat & (1u << ch))) dmac_raise(ch);
    } else {
        g_d_stat &= ~(1u << (16 + ch));
    }
    return was;
}

int hw_cpcond0() { return 1; }  // all transfers complete synchronously

void hw_init() {
    for (auto &c : g_dma) c = DmaChan();
    g_d_ctrl = 1;
}

uint32_t hw_read32(uint32_t p) {
    uint32_t off = p & 0xFFFF;
    if (off < 0x2000) return timer_read(off >> 11, off & 0x7F0);
    if (off >= 0x2000 && off < 0x3000) return ipu_read32(off);
    if (off >= 0x3000 && off < 0x3800) return gif_reg_read(off);
    if (off >= 0x3800 && off < 0x4000) return vif_reg_read(off >= 0x3C00 ? 1 : 0, off & 0x3FF);
    if (off >= 0x4000 && off < 0x8000) return 0;  // FIFOs (reads of VIF1/IPU out handled elsewhere)
    if (off >= 0x8000 && off < 0xE000) {
        int ch = chan_from_addr(off);
        if (ch < 0) return 0;
        DmaChan &c = g_dma[ch];
        switch ((off - k_chan_base[ch]) & 0xF0) {
        case 0x00: return c.chcr;
        case 0x10: return c.madr;
        case 0x20: return c.qwc;
        case 0x30: return c.tadr;
        case 0x40: return c.asr0;
        case 0x50: return c.asr1;
        case 0x80: return c.sadr;
        }
        return 0;
    }
    switch (off) {
    case 0xE000: return g_d_ctrl;
    case 0xE010: return g_d_stat;
    case 0xE020: return g_d_pcr;
    case 0xE030: return g_d_sqwc;
    case 0xE040: return g_d_rbsr;
    case 0xE050: return g_d_rbor;
    case 0xE060: return g_d_stadr;
    case 0xF000: return intc_stat();
    case 0xF010: return intc_mask();
    case 0xF200:
    case 0xF210:
    case 0xF220:
    case 0xF230:
    case 0xF240:
    case 0xF260:
        return sif_read_reg(off);
    case 0xF520: return g_d_enable;
    }
    LOG(LOG_HW, "hw read32 %08x", p);
    return 0;
}

void hw_read128(uint32_t p, uint32_t out[4]) {
    uint32_t off = p & 0xFFFF;
    if (off == 0x7000) {
        ipu_out_fifo(reinterpret_cast<uint8_t *>(out), 1);
        return;
    }
    if (off == 0x5000) {
        vif1_read_fifo(out);
        return;
    }
    for (int i = 0; i < 4; i++) out[i] = hw_read32(p + i * 4);
}

void hw_write8(uint32_t p, uint8_t v) {
    uint32_t off = p & 0xFFFF;
    if (off == 0xF180) {  // SIO TX: debug console
        if (v == '\n' || g_sio_line_len >= sizeof g_sio_line - 1) {
            g_sio_line[g_sio_line_len] = 0;
            LOG(LOG_GAME, "%s", g_sio_line);
            g_sio_line_len = 0;
        } else if (v != '\r') {
            g_sio_line[g_sio_line_len++] = (char)v;
        }
        return;
    }
    // byte writes to CHCR are used to start/stop DMA
    if (off >= 0x8000 && off < 0xE000) {
        int ch = chan_from_addr(off);
        if (ch >= 0 && ((off - k_chan_base[ch]) & 0xF0) == 0) {
            uint32_t sh = (off & 3) * 8;
            uint32_t cur = g_dma[ch].chcr;
            hw_write32(p & ~3u, (cur & ~(0xFFu << sh)) | ((uint32_t)v << sh));
            return;
        }
    }
    LOG(LOG_HW, "hw write8 %08x = %02x", p, v);
}

void hw_write16(uint32_t p, uint16_t v) {
    uint32_t off = p & 0xFFFF;
    if (off >= 0x8000 && off < 0xE000) {
        int ch = chan_from_addr(off);
        if (ch >= 0 && ((off - k_chan_base[ch]) & 0xF0) == 0) {
            uint32_t sh = (off & 2) * 8;
            uint32_t cur = g_dma[ch].chcr;
            hw_write32(p & ~3u, (cur & ~(0xFFFFu << sh)) | ((uint32_t)v << sh));
            return;
        }
    }
    hw_write32(p, v);
}

void hw_write32(uint32_t p, uint32_t v) {
    uint32_t off = p & 0xFFFF;
    if (off < 0x2000) {
        timer_write(off >> 11, off & 0x7F0, v);
        return;
    }
    if (off >= 0x2000 && off < 0x3000) {
        ipu_write32(off, v);
        return;
    }
    if (off >= 0x3000 && off < 0x3800) {
        gif_reg_write(off, v);
        return;
    }
    if (off >= 0x3800 && off < 0x4000) {
        vif_reg_write(off >= 0x3C00 ? 1 : 0, off & 0x3FF, v);
        return;
    }
    if (off >= 0x8000 && off < 0xE000) {
        int ch = chan_from_addr(off);
        if (ch < 0) return;
        DmaChan &c = g_dma[ch];
        switch ((off - k_chan_base[ch]) & 0xF0) {
        case 0x00: {
            bool start = (v & 0x100) && !(c.chcr & 0x100);
            c.chcr = v;
            LOG(LOG_DMA, "DMA ch%d CHCR=%08x madr=%08x qwc=%x tadr=%08x", ch, v, c.madr, c.qwc, c.tadr);
            if (start) dma_start(ch);
            break;
        }
        case 0x10: c.madr = v; break;
        case 0x20: c.qwc = v & 0xFFFF; break;
        case 0x30: c.tadr = v; break;
        case 0x40: c.asr0 = v; break;
        case 0x50: c.asr1 = v; break;
        case 0x80: c.sadr = v & 0x3FFF; break;
        }
        return;
    }
    switch (off) {
    case 0xE000:
        g_d_ctrl = v;
        if (v & 0xC) LOG(LOG_DMA, "D_CTRL MFIFO mode %d requested (not emulated)", (v >> 2) & 3);
        return;
    case 0xE010:
        g_d_stat &= ~(v & 0xFFFF);
        g_d_stat ^= v & 0xFFFF0000u;
        return;
    case 0xE020: g_d_pcr = v; return;
    case 0xE030: g_d_sqwc = v; return;
    case 0xE040: g_d_rbsr = v; return;
    case 0xE050: g_d_rbor = v; return;
    case 0xE060: g_d_stadr = v; return;
    case 0xF000: intc_write_stat(v); return;
    case 0xF010: intc_write_mask(v); return;
    case 0xF200:
    case 0xF210:
    case 0xF220:
    case 0xF230:
    case 0xF240:
    case 0xF260:
        sif_write_reg(off, v);
        return;
    case 0xF590: g_d_enable = v; return;
    case 0xF180: hw_write8(p, (uint8_t)v); return;
    }
    LOG(LOG_HW, "hw write32 %08x = %08x", p, v);
}

void hw_write64(uint32_t p, uint64_t v) {
    uint32_t off = p & 0xFFFF;
    if (off >= 0x4000 && off < 0x8000) {
        uint32_t q[4] = {(uint32_t)v, (uint32_t)(v >> 32), 0, 0};
        hw_write128(p, q);
        return;
    }
    hw_write32(p, (uint32_t)v);
    hw_write32(p + 4, (uint32_t)(v >> 32));
}

void hw_write128(uint32_t p, const uint32_t v[4]) {
    uint32_t off = p & 0xFFFF;
    switch (off & 0xF000) {
    case 0x4000:
        vif_write(0, v, 4);
        return;
    case 0x5000:
        vif_write(1, v, 4);
        return;
    case 0x6000:
        gif_transfer(GIF_PATH3, reinterpret_cast<const uint8_t *>(v), 1);
        return;
    case 0x7000:
        ipu_in_fifo(reinterpret_cast<const uint8_t *>(v), 1);
        return;
    }
    for (int i = 0; i < 4; i++) hw_write32(p + i * 4, v[i]);
}

void *hw_vu_mem_ptr(uint32_t phys) {
    uint32_t off = phys - 0x11000000;
    if (off < 0x1000) return g_vu0.micro + off;
    if (off >= 0x4000 && off < 0x5000) return g_vu0.mem + (off - 0x4000);
    if (off >= 0x8000 && off < 0xC000) return g_vu1.micro + (off - 0x8000);
    if (off >= 0xC000 && off < 0x10000) return g_vu1.mem + (off - 0xC000);
    return nullptr;
}
