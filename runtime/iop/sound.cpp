// sound.cpp - HLE of sdrdrv.irx ("SDR driver", RPC 0x80000701): the EE's
// libsdr forwards libsd calls to the IOP as command numbers
// (sceSdRemote).  The arguments follow the command word at +4, +8, ...;
// the result goes back as the first word of a 16-byte reply.
//
// Interrupts (transfer completion, SPU2 IRQ) are reported to the EE by the
// driver's callback thread, which calls the EE's RPC server 0x80000704
// with a 64-byte block: word 0 = flags (0x100/0x200 transfer channel 0/1
// done, 0x400 SPU2 IRQ), word 1 = IRQ core.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "../core/log.h"
#include "iop.h"
#include "spu2.h"

namespace {

uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

bool g_cb_thread;         // the EE asked for the callback thread
bool g_trans_handler[2];  // transfer interrupt handler installed per channel
bool g_irq_handler;
uint32_t g_cb_flags, g_cb_info;

void post_callbacks() {
    if (!g_cb_thread || !g_cb_flags) return;
    uint8_t blk[64] = {};
    wr32(blk, g_cb_flags);
    wr32(blk + 4, g_cb_info);
    g_cb_flags = 0;
    iop_call_ee_rpc(0x80000704, 0, blk, sizeof blk);
}

void on_trans_done(int ch) {
    if (!g_trans_handler[ch & 1]) return;
    g_cb_flags |= 0x100u << (ch & 1);
    post_callbacks();
}

void on_irq(int core) {
    if (!g_irq_handler) return;
    g_cb_flags |= 0x400;
    g_cb_info = (uint32_t)core;
    post_callbacks();
}

uint16_t note_to_pitch(int center_note, int center_fine, int note, int fine) {
    double semis = (note - center_note) + (fine - center_fine) / 128.0;
    double p = 4096.0 * std::pow(2.0, semis / 12.0);
    return (uint16_t)std::min(p, 16383.0);
}

int pitch_to_note(int center_note, int center_fine, int pitch) {
    if (pitch <= 0) return 0;
    double semis = 12.0 * std::log2(pitch / 4096.0);
    int total = (int)std::lround((center_note + center_fine / 128.0 + semis) * 128.0);
    return ((total / 128) << 8) | (total & 127);  // note in the high byte, fine in the low byte
}

// libsd batch: {u16 func; u16 entry; u32 value}
int proc_batch(const uint8_t *b, uint32_t *rets, int num, uint32_t voice_mask, bool ex) {
    int n = 0;
    for (int v = 0; v < (ex ? 24 : 1); v++) {
        if (ex && !(voice_mask & (1u << v))) continue;
        for (int i = 0; i < num; i++) {
            uint16_t func = rd16(b + i * 8), entry = rd16(b + i * 8 + 2);
            uint32_t value = rd32(b + i * 8 + 4);
            if (ex) entry = (uint16_t)((entry & ~0x3Eu) | (v << 1));
            uint32_t r = 0;
            switch (func) {
            case 0x01: spu2_set_param(entry, (uint16_t)value); break;
            case 0x02: spu2_set_switch(entry, value); break;
            case 0x03: spu2_set_addr(entry, value); break;
            case 0x04: spu2_set_core_attr(entry, (uint16_t)value); break;
            case 0x05:  // write to IOP memory
                if (value + 4 <= IOP_RAM_SIZE) memcpy(g_iop_ram + value, &entry, 2);
                break;
            case 0x10: r = spu2_get_param(entry); break;
            case 0x12: r = spu2_get_switch(entry); break;
            case 0x13: r = spu2_get_addr(entry); break;
            case 0x14: r = spu2_get_core_attr(entry); break;
            default: LOG(LOG_SND, "sdr: batch function %x not handled", func); break;
            }
            if (rets) rets[n] = r;
            n++;
        }
    }
    return n;
}

uint32_t sdr_call(const RpcCall &c, uint8_t *buf) {
    uint32_t cmd = c.fno & 0xFFF0;
    const uint8_t *a = buf;
    int32_t ret = 0;
    LOG(LOG_SND, "sdr %04x: %08x %08x %08x %08x %08x", c.fno, rd32(a + 4), rd32(a + 8), rd32(a + 12), rd32(a + 16),
        rd32(a + 20));
    switch (cmd) {
    case 0x8000:  // sceSdInit
        LOG(LOG_SND, "sdr: init");
        break;
    case 0x8010: spu2_set_param(rd16(a + 4), rd16(a + 8)); break;
    case 0x8020: ret = spu2_get_param(rd16(a + 4)); break;
    case 0x8030: spu2_set_switch(rd16(a + 4), rd32(a + 8)); break;
    case 0x8040: ret = (int32_t)spu2_get_switch(rd16(a + 4)); break;
    case 0x8050: spu2_set_addr(rd16(a + 4), rd32(a + 8)); break;
    case 0x8060: ret = (int32_t)spu2_get_addr(rd16(a + 4)); break;
    case 0x8070: spu2_set_core_attr(rd16(a + 4), rd16(a + 8)); break;
    case 0x8080: ret = spu2_get_core_attr(rd16(a + 4)); break;
    case 0x8090: ret = note_to_pitch(rd16(a + 4), rd16(a + 8), rd16(a + 12), (int16_t)rd16(a + 16)); break;
    case 0x80A0: ret = pitch_to_note(rd16(a + 4), rd16(a + 8), rd16(a + 12)); break;
    case 0x80B0:    // sceSdProcBatch(batch, returns, num) with IOP pointers
    case 0x80C0: {  // sceSdProcBatchEx(batch, returns, num, voices)
        uint32_t bp = rd32(a + 4), rp = rd32(a + 8), num = rd32(a + 12);
        if (num > 512 || bp + num * 8 > IOP_RAM_SIZE) break;
        static uint32_t rets[512 * 24];
        ret = proc_batch(g_iop_ram + bp, rets, (int)num, rd32(a + 16), cmd == 0x80C0);
        if (rp && rp + (uint32_t)ret * 4 <= IOP_RAM_SIZE) memcpy(g_iop_ram + rp, rets, (size_t)ret * 4);
        break;
    }
    case 0x81C0:    // batch sent inline; results come back after the count
    case 0x81D0: {
        uint32_t num = rd16(a + 2);
        uint32_t mask = cmd == 0x81D0 ? rd32(a + 4) : 0;
        if (num > 0x200) num = 0x200;
        static uint8_t batch[0x200 * 8];
        memcpy(batch, a + 8, num * 8);
        static uint32_t rets[0x200 * 24];
        int n = proc_batch(batch, rets, (int)num, mask, cmd == 0x81D0);
        uint32_t bytes = std::min<uint32_t>(4 + (uint32_t)n * 4, 0x10000);
        memset(buf, 0, bytes);
        wr32(buf, (uint32_t)n);
        memcpy(buf + 4, rets, bytes - 4);
        return std::max<uint32_t>(bytes, 16);
    }
    case 0x80D0:  // sceSdVoiceTrans(channel, mode, iop, spu, size)
        ret = spu2_voice_trans((int16_t)rd16(a + 4), rd16(a + 8), rd32(a + 12), rd32(a + 16), rd32(a + 20));
        break;
    case 0x80E0:  // sceSdBlockTrans(channel, mode, iop, size, start)
        ret = spu2_block_trans((int16_t)rd16(a + 4), rd16(a + 8), rd32(a + 12), rd32(a + 16), rd32(a + 20));
        break;
    case 0x80F0:  // sceSdVoiceTransStatus: transfers complete immediately
        ret = 1;
        break;
    case 0x8100:  // sceSdBlockTransStatus
        ret = (int32_t)spu2_block_trans_status((int16_t)rd16(a + 4));
        break;
    case 0x8160: {  // sceSdSetTransIntrHandler(channel, enable)
        int ch = rd32(a + 4) ? 1 : 0;
        g_trans_handler[ch] = rd32(a + 8) != 0;
        LOG(LOG_SND, "sdr: transfer handler ch%d %s", ch, g_trans_handler[ch] ? "on" : "off");
        break;
    }
    case 0x8170:  // sceSdSetSpu2IntrHandler(enable)
        g_irq_handler = rd32(a + 4) != 0;
        break;
    case 0x8180:  // sceSdStopTrans(channel)
        spu2_block_trans((int)rd32(a + 4), 2, 0, 0, 0);
        break;
    case 0x8130: case 0x8140: case 0x8150: case 0x8190: case 0x81A0: case 0x81B0:
        // effects (reverb) are not emulated
        if (cmd == 0x8140) {
            memset(buf, 0, 32);
            return 32;
        }
        break;
    case 0xE620:  // create the callback thread
        g_cb_thread = true;
        LOG(LOG_SND, "sdr: callback thread started");
        post_callbacks();
        break;
    case 0xE630:
        g_cb_thread = false;
        break;
    default:
        WARN("sdr: unknown command %04x", c.fno);
        break;
    }
    memset(buf, 0, 16);
    wr32(buf, (uint32_t)ret);
    return 16;
}

}  // namespace

void iop_sound_init() {
    spu2_init();
    spu2_set_callbacks(on_trans_done, on_irq);
    iop_register_server(0x80000701, "sdrdrv", sdr_call);
}

void iop_sound_update() { spu2_update(); }
