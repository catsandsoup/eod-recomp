// spu2.cpp - SPU2 emulation at the libsd level.
//
// Voices play 4-bit ADPCM ("VAG") from the 2MB sound RAM with pitch,
// ADSR envelope and volume; the AutoDMA input plays 16-bit PCM streamed
// from IOP memory by block transfers (interleaved 0x100 bytes left /
// 0x100 bytes right), raising a transfer interrupt every half buffer.
// Audio is produced at 48kHz in step with the wall clock.
#include "spu2.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "../core/log.h"
#include "../core/memory.h"
#include "../platform/platform.h"

namespace {

constexpr uint32_t kRamSize = 2 * 1024 * 1024;
constexpr int kRate = 48000;

uint8_t g_sram[kRamSize];

struct Envelope {
    int32_t level = 0;
    uint32_t counter = 0, increment = 0x8000;
    int32_t step = 0;
    int rate = 0;
    bool decreasing = false, exponential = false;

    void reset(int r, int mask, bool dec, bool exp) {
        rate = r;
        decreasing = dec;
        exponential = exp;
        counter = 0;
        increment = 0x8000;
        int base = 7 - (rate & 3);
        step = dec ? ~base : base;
        if (rate < 44) {
            step <<= (11 - (rate >> 2));
        } else if (rate >= 48) {
            increment >>= ((rate >> 2) - 11);
            if ((rate & mask) != mask) increment = std::max<uint32_t>(increment, 1);
        }
    }

    void tick() {
        uint32_t inc = increment;
        int32_t st = step;
        if (exponential) {
            if (decreasing) {
                st = (st * level) >> 15;
            } else if (level >= 0x6000) {
                if (rate < 40) {
                    st >>= 2;
                } else if (rate >= 44) {
                    inc >>= 2;
                } else {
                    st >>= 1;
                    inc >>= 1;
                }
            }
        }
        counter += inc;
        if (!(counter & 0x8000)) return;
        counter = 0;
        level = std::min(std::max(level + st, 0), 0x7FFF);
    }
};

enum Phase { OFF, ATTACK, DECAY, SUSTAIN, RELEASE };

struct Voice {
    // registers
    uint16_t voll = 0, volr = 0, pitch = 0, adsr1 = 0, adsr2 = 0;
    uint32_t ssa = 0, lsax = 0, nax = 0;
    int16_t fixed_l = 0, fixed_r = 0;  // last fixed volumes (sweeps hold them)
    // playback
    Phase phase = OFF;
    Envelope env;
    uint32_t cur = 0;   // byte address of the block being played
    uint8_t flags = 0;  // its loop flags
    int16_t dec[28];
    int16_t h1 = 0, h2 = 0;
    uint32_t counter = 0;  // fraction of the current sample (12 bits)
    int idx = 0;           // next sample in dec[]
    int16_t s_prev = 0, s_cur = 0;
};

struct BlockTrans {
    bool active = false, loop = false;
    uint32_t base = 0, size = 0;  // IOP memory buffer
    uint32_t pos = 0;             // stereo samples played
};

struct Core {
    Voice v[24];
    uint16_t mvoll = 0, mvolr = 0, bvoll = 0x7FFF, bvolr = 0x7FFF, evoll = 0, evolr = 0, avoll = 0, avolr = 0;
    uint16_t mmix = 0;
    uint32_t vmixl = 0xFFFFFF, vmixr = 0xFFFFFF, vmixel = 0, vmixer = 0, pmon = 0, non = 0, endx = 0;
    uint32_t esa = 0, eea = 0, tsa = 0, irqa = 0;
    uint16_t attr[8] = {};
    BlockTrans adma;
};

Core g_core[2];
void (*g_trans_cb)(int);
void (*g_irq_cb)(int);

const int kFilter[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

inline int16_t vol_value(uint16_t v, int16_t &fixed) {
    if (!(v & 0x8000)) fixed = (int16_t)(v << 1);  // 15-bit signed -> 16-bit
    return fixed;
}

void decode_block(Core &c, int core, Voice &v) {
    const uint8_t *b = g_sram + (v.cur & (kRamSize - 1) & ~15u);
    int shift = b[0] & 15, filt = (b[0] >> 4) & 7;
    if (shift > 12) shift = 9;
    if (filt > 4) filt = 0;
    v.flags = b[1];
    if (v.flags & 4) v.lsax = v.cur;  // loop start
    int f0 = kFilter[filt][0], f1 = kFilter[filt][1];
    for (int i = 0; i < 28; i++) {
        int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
        int s = (int16_t)(nib << 12) >> shift;
        s += (v.h1 * f0 + v.h2 * f1 + 32) >> 6;
        s = std::min(std::max(s, -32768), 32767);
        v.h2 = v.h1;
        v.h1 = (int16_t)s;
        v.dec[i] = (int16_t)s;
    }
    if ((c.attr[2] & 1) && (c.irqa & ~15u) == (v.cur & ~15u) && g_irq_cb) g_irq_cb(core);  // IRQ address hit
}

void key_on(Core &c, int core, int i) {
    Voice &v = c.v[i];
    v.cur = v.ssa & ~15u;
    v.h1 = v.h2 = 0;
    v.counter = 0;
    v.idx = 0;
    v.s_prev = v.s_cur = 0;
    decode_block(c, core, v);
    v.phase = ATTACK;
    v.env.level = 0;
    v.env.reset((v.adsr1 >> 8) & 0x7F, 0x7F, false, v.adsr1 & 0x8000);
    c.endx &= ~(1u << i);
}

void key_off(Core &c, int i) {
    Voice &v = c.v[i];
    if (v.phase == OFF) return;
    v.phase = RELEASE;
    v.env.reset((v.adsr2 & 0x1F) << 2, 0x1F << 2, true, v.adsr2 & 0x20);
}

void update_envelope(Voice &v) {
    switch (v.phase) {
    case OFF:
        return;
    case ATTACK:
        v.env.tick();
        if (v.env.level >= 0x7FFF) {
            v.phase = DECAY;
            v.env.reset(((v.adsr1 >> 4) & 15) << 2, 0x1F << 2, true, true);
        }
        break;
    case DECAY: {
        v.env.tick();
        int32_t sl = std::min(((v.adsr1 & 15) + 1) << 11, 0x7FFF);
        if (v.env.level <= sl) {
            v.phase = SUSTAIN;
            v.env.reset((v.adsr2 >> 6) & 0x7F, 0x7F, v.adsr2 & 0x4000, v.adsr2 & 0x8000);
        }
        break;
    }
    case SUSTAIN:
        v.env.tick();
        break;
    case RELEASE:
        v.env.tick();
        if (v.env.level <= 0) {
            v.env.level = 0;
            v.phase = OFF;
        }
        break;
    }
}

// Next interpolated sample of a voice (before envelope/volume).
int voice_sample(Core &c, int core, int i) {
    Voice &v = c.v[i];
    uint32_t step = std::min<uint32_t>(v.pitch, 0x3FFF);
    v.counter += step;
    while (v.counter >= 0x1000) {
        v.counter -= 0x1000;
        // advance one sample
        int idx = v.idx;
        v.s_prev = v.s_cur;
        v.s_cur = v.dec[idx];
        idx++;
        if (idx >= 28) {
            idx = 0;
            if (v.flags & 1) {  // loop end
                c.endx |= 1u << i;
                if (v.flags & 2) {
                    v.cur = v.lsax & ~15u;
                } else {
                    v.phase = OFF;
                    v.env.level = 0;
                    v.cur += 16;
                }
            } else {
                v.cur += 16;
            }
            if (v.phase != OFF) decode_block(c, core, v);
        }
        v.idx = idx;
    }
    return v.s_prev + (((v.s_cur - v.s_prev) * (int)v.counter) >> 12);
}

void adma_sample(Core &c, int ch, int &l, int &r) {
    BlockTrans &t = c.adma;
    l = r = 0;
    if (!t.active || t.size < 1024) return;
    uint32_t total = t.size / 4;  // stereo samples in the buffer
    uint32_t blk = t.pos / 128, i = t.pos % 128;
    uint32_t a = t.base + blk * 512 + i * 2;
    if (a + 258 <= IOP_RAM_SIZE) {
        l = (int16_t)(g_iop_ram[a] | (g_iop_ram[a + 1] << 8));
        r = (int16_t)(g_iop_ram[a + 256] | (g_iop_ram[a + 257] << 8));
    }
    t.pos++;
    if (t.pos == total / 2 || t.pos >= total) {
        if (t.pos >= total) {
            t.pos = 0;
            if (!t.loop) t.active = false;
        }
        if (g_trans_cb) g_trans_cb(ch);
    }
}

void mix(int16_t *out, int frames) {
    for (int n = 0; n < frames; n++) {
        int64_t out_l = 0, out_r = 0;
        for (int ci = 0; ci < 2; ci++) {
            Core &c = g_core[ci];
            int32_t dl = 0, dr = 0;
            for (int i = 0; i < 24; i++) {
                Voice &v = c.v[i];
                if (v.phase == OFF) continue;
                int s = voice_sample(c, ci, i);
                update_envelope(v);
                s = (s * v.env.level) >> 15;
                int vl = vol_value(v.voll, v.fixed_l), vr = vol_value(v.volr, v.fixed_r);
                if (c.vmixl & (1u << i)) dl += (s * vl) >> 15;
                if (c.vmixr & (1u << i)) dr += (s * vr) >> 15;
            }
            int al, ar;
            adma_sample(c, ci, al, ar);
            dl += (al * (int16_t)c.bvoll) >> 15;
            dr += (ar * (int16_t)c.bvolr) >> 15;
            int16_t fl = 0, fr = 0;
            out_l += ((int64_t)dl * vol_value(c.mvoll, fl)) >> 15;
            out_r += ((int64_t)dr * vol_value(c.mvolr, fr)) >> 15;
        }
        out[n * 2] = (int16_t)std::min<int64_t>(std::max<int64_t>(out_l, -32768), 32767);
        out[n * 2 + 1] = (int16_t)std::min<int64_t>(std::max<int64_t>(out_r, -32768), 32767);
    }
}

uint64_t g_t0, g_produced;
bool g_audio_open;

}  // namespace

void spu2_init() {
    memset(g_sram, 0, sizeof g_sram);
    for (Core &c : g_core) c = Core();
}

void spu2_set_callbacks(void (*trans_done)(int), void (*irq)(int)) {
    g_trans_cb = trans_done;
    g_irq_cb = irq;
}

void spu2_set_param(uint16_t entry, uint16_t value) {
    Core &c = g_core[entry & 1];
    int id = entry >> 8;
    if (!(entry & 0x80)) {  // voice parameter
        Voice &v = c.v[((entry >> 1) & 31) % 24];
        switch (id) {
        case 0: v.voll = value; break;
        case 1: v.volr = value; break;
        case 2: v.pitch = value; break;
        case 3: v.adsr1 = value; break;
        case 4: v.adsr2 = value; break;
        case 5: v.env.level = value & 0x7FFF; break;
        }
        return;
    }
    switch (id) {
    case 0x08: c.mmix = value; break;
    case 0x09: c.mvoll = value; break;
    case 0x0A: c.mvolr = value; break;
    case 0x0B: c.evoll = value; break;
    case 0x0C: c.evolr = value; break;
    case 0x0D: c.avoll = value; break;
    case 0x0E: c.avolr = value; break;
    case 0x0F: c.bvoll = value; break;
    case 0x10: c.bvolr = value; break;
    default: LOG(LOG_SND, "spu2: set param %04x = %04x", entry, value);
    }
}

uint16_t spu2_get_param(uint16_t entry) {
    Core &c = g_core[entry & 1];
    int id = entry >> 8;
    if (!(entry & 0x80)) {
        Voice &v = c.v[((entry >> 1) & 31) % 24];
        switch (id) {
        case 0: return v.voll;
        case 1: return v.volr;
        case 2: return v.pitch;
        case 3: return v.adsr1;
        case 4: return v.adsr2;
        case 5: return (uint16_t)v.env.level;
        case 6: return (uint16_t)v.fixed_l;
        case 7: return (uint16_t)v.fixed_r;
        }
        return 0;
    }
    switch (id) {
    case 0x08: return c.mmix;
    case 0x09: return c.mvoll;
    case 0x0A: return c.mvolr;
    case 0x0B: return c.evoll;
    case 0x0C: return c.evolr;
    case 0x0D: return c.avoll;
    case 0x0E: return c.avolr;
    case 0x0F: return c.bvoll;
    case 0x10: return c.bvolr;
    case 0x11: return c.mvoll;
    case 0x12: return c.mvolr;
    }
    return 0;
}

void spu2_set_switch(uint16_t entry, uint32_t value) {
    int ci = entry & 1;
    Core &c = g_core[ci];
    value &= 0xFFFFFF;
    switch (entry >> 8) {
    case 0x13: c.pmon = value; break;
    case 0x14: c.non = value; break;
    case 0x15:
        for (int i = 0; i < 24; i++)
            if (value & (1u << i)) key_on(c, ci, i);
        break;
    case 0x16:
        for (int i = 0; i < 24; i++)
            if (value & (1u << i)) key_off(c, i);
        break;
    case 0x17: c.endx = value; break;
    case 0x18: c.vmixl = value; break;
    case 0x19: c.vmixel = value; break;
    case 0x1A: c.vmixr = value; break;
    case 0x1B: c.vmixer = value; break;
    default: LOG(LOG_SND, "spu2: set switch %04x = %06x", entry, value);
    }
}

uint32_t spu2_get_switch(uint16_t entry) {
    Core &c = g_core[entry & 1];
    switch (entry >> 8) {
    case 0x13: return c.pmon;
    case 0x14: return c.non;
    case 0x17: return c.endx;
    case 0x18: return c.vmixl;
    case 0x19: return c.vmixel;
    case 0x1A: return c.vmixr;
    case 0x1B: return c.vmixer;
    }
    return 0;
}

void spu2_set_addr(uint16_t entry, uint32_t value) {
    Core &c = g_core[entry & 1];
    int id = entry >> 8;
    value &= kRamSize - 1;
    if (id >= 0x20) {
        Voice &v = c.v[((entry >> 1) & 31) % 24];
        if (id == 0x20) v.ssa = value;
        else if (id == 0x21) v.lsax = value;
        else if (id == 0x22) v.cur = value & ~15u;
        return;
    }
    switch (id) {
    case 0x1C: c.esa = value; break;
    case 0x1D: c.eea = value; break;
    case 0x1E: c.tsa = value; break;
    case 0x1F: c.irqa = value; break;
    }
}

uint32_t spu2_get_addr(uint16_t entry) {
    Core &c = g_core[entry & 1];
    int id = entry >> 8;
    if (id >= 0x20) {
        Voice &v = c.v[((entry >> 1) & 31) % 24];
        if (id == 0x20) return v.ssa;
        if (id == 0x21) return v.lsax;
        return v.cur;
    }
    switch (id) {
    case 0x1C: return c.esa;
    case 0x1D: return c.eea;
    case 0x1E: return c.tsa;
    case 0x1F: return c.irqa;
    }
    return 0;
}

void spu2_set_core_attr(uint16_t entry, uint16_t value) { g_core[entry & 1].attr[(entry >> 1) & 7] = value; }
uint16_t spu2_get_core_attr(uint16_t entry) { return g_core[entry & 1].attr[(entry >> 1) & 7]; }

int spu2_voice_trans(int channel, int mode, uint32_t iop_addr, uint32_t spu_addr, uint32_t size) {
    spu_addr &= kRamSize - 1;
    size = std::min(size, kRamSize - spu_addr);
    if (iop_addr + size > IOP_RAM_SIZE) size = IOP_RAM_SIZE > iop_addr ? IOP_RAM_SIZE - iop_addr : 0;
    if (mode & 1)
        memcpy(g_iop_ram + iop_addr, g_sram + spu_addr, size);
    else
        memcpy(g_sram + spu_addr, g_iop_ram + iop_addr, size);
    LOG(LOG_SND, "spu2: voice transfer ch%d %s iop %06x spu %06x size %x", channel, (mode & 1) ? "read" : "write",
        iop_addr, spu_addr, size);
    if (g_trans_cb) g_trans_cb(channel & 1);
    return (int)size;
}

int spu2_block_trans(int channel, int mode, uint32_t iop_addr, uint32_t size, uint32_t start) {
    BlockTrans &t = g_core[channel & 1].adma;
    int op = mode & 3;
    if (op == 2) {  // stop
        t.active = false;
        LOG(LOG_SND, "spu2: block transfer ch%d stopped", channel);
        return 0;
    }
    if (op == 1) {
        WARN("spu2: block transfer read not supported");
        return -1;
    }
    t.active = true;
    t.loop = (mode & 0x10) != 0;
    t.base = iop_addr;
    t.size = size & ~1023u;
    t.pos = 0;
    if (op == 3 && start >= iop_addr && start < iop_addr + size) t.pos = (start - iop_addr) / 512 * 128;
    LOG(LOG_SND, "spu2: block transfer ch%d iop %06x size %x%s", channel, iop_addr, size, t.loop ? " loop" : "");
    return 0;
}

uint32_t spu2_block_trans_status(int channel) {
    BlockTrans &t = g_core[channel & 1].adma;
    if (!t.active) return 0;
    uint32_t half = t.pos >= t.size / 8 ? 1 : 0;
    return (half << 24) | ((t.base + t.pos / 128 * 512) & 0xFFFFFF);
}

void spu2_update() {
    uint64_t now = platform_time_us();
    if (!g_t0) {
        g_t0 = now;
        g_audio_open = platform_audio_open(kRate);
        return;
    }
    uint64_t due = (now - g_t0) * kRate / 1000000;
    if (due - g_produced < 64) return;  // ~1.3ms granularity
    if (due - g_produced > kRate / 10) g_produced = due - kRate / 10;  // fell behind: drop
    int frames = (int)(due - g_produced);
    g_produced = due;
    static std::vector<int16_t> buf;
    buf.resize((size_t)frames * 2);
    mix(buf.data(), frames);
    if (g_audio_open && platform_audio_queued_frames() < kRate / 5) platform_audio_submit(buf.data(), frames);
}
