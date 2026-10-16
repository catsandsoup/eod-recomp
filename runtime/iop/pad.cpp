// pad.cpp - padman.irx HLE (RPC 0x80000100/0x80000101) for the SDK 2.8
// "libpad" (protocol version 4).
//
// Commands arrive as a 128-byte block with the command code in word 0 and
// the result returned in word 3.  After scePadPortOpen the IOP writes a
// 128-byte pad_data structure into one of the two halves of the EE pad
// area every frame (the EE picks the half with the newer frame counter):
//   0x00 data[32]      raw controller reply (mode id, buttons, sticks...)
//   0x20 actDirections[2], 0x28 actAlignments[2], 0x30 actData[32]
//   0x50 modeTable[4]  0x58 frame  0x60 length
//   0x64 modeConfig 0x65 modeCurId 0x66 model 0x67 buttonDataReady
//   0x68 nrOfModes 0x69 modeCurOffs 0x6A nrOfActuators 0x6B numActComb
//   0x6D mode 0x6E lock 0x6F actLevel 0x70 state 0x71 reqState
//   0x72 currentTask 0x73 runTask
#include <cstring>

#include "../core/log.h"
#include "../platform/platform.h"
#include "iop.h"

namespace {

enum { PAD_STATE_DISCONN = 0, PAD_STATE_STABLE = 6 };
enum { PAD_RSTAT_COMPLETE = 0, PAD_RSTAT_FAILED = 1, PAD_RSTAT_BUSY = 2 };

struct Port {
    bool open = false;
    uint32_t pad_area = 0;   // EE address of 2 x 128 bytes
    uint32_t iop_buf = 0;    // IOP address receiving EE actuator data
    uint32_t frame = 0;
    bool analog = true;      // DualShock analog mode
    bool pressure = false;   // pressure sensitive buttons enabled
    uint8_t lock = 0;
    uint8_t req_state = PAD_RSTAT_COMPLETE;
    uint8_t act_align[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
};

Port g_ports[2][4];  // [port][multitap slot]

uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

void write_pad_data(int port, int slot) {
    Port &p = g_ports[port][slot];
    HostPadState hs;
    platform_get_pad(port, &hs);
    if (slot != 0) hs.connected = false;  // no multitap
    uint8_t d[128] = {};
    uint8_t mode = !p.analog ? 0x41 : p.pressure ? 0x79 : 0x73;
    uint16_t btn = (uint16_t)~hs.buttons;  // active low
    d[0] = 0x00;
    d[1] = mode;
    d[2] = (uint8_t)btn;
    d[3] = (uint8_t)(btn >> 8);
    d[4] = hs.rx;
    d[5] = hs.ry;
    d[6] = hs.lx;
    d[7] = hs.ly;
    memcpy(d + 8, hs.pressure, 12);
    uint32_t len = mode == 0x41 ? 4 : mode == 0x73 ? 8 : 20;
    // actuators: 0 = small motor (on/off), 1 = large motor (0-255)
    memcpy(d + 0x28, p.act_align, 6);
    const uint8_t act[8] = {0, 1, 0, 50, 1, 1, 1, 100};
    memcpy(d + 0x30, act, 8);
    // mode table: digital, DualShock
    d[0x50] = 4;
    d[0x52] = 7;
    wr32(d + 0x58, ++p.frame);
    wr32(d + 0x60, len);
    d[0x64] = 2;      // modeConfig: configurable device
    d[0x65] = mode;   // modeCurId
    d[0x66] = 3;      // model: DualShock 2
    d[0x67] = 1;      // buttonDataReady
    d[0x68] = 2;      // nrOfModes
    d[0x69] = p.analog ? 1 : 0;
    d[0x6A] = 2;      // nrOfActuators
    d[0x6B] = 1;      // numActComb
    d[0x6D] = p.analog ? 1 : 0;
    d[0x6E] = p.lock;
    d[0x70] = hs.connected ? PAD_STATE_STABLE : PAD_STATE_DISCONN;
    d[0x71] = p.req_state;
    d[0x72] = 1;      // currentTask
    if (!hs.connected) d[1] = 0;
    ee_write_block(p.pad_area + (p.frame & 1) * 128, d, 128);

    // vibration values the EE may DMA here (2 x 32 bytes); libpad 2.8 sends
    // them with SET_ACTDIR instead (below), so this only reports a surprise
    if (p.iop_buf) {
        static bool reported;
        const uint8_t *a = g_iop_ram + p.iop_buf;
        for (int i = 0; i < 64 && !reported; i++)
            if (a[i]) {
                reported = true;
                LOG(LOG_PAD, "pad %d: actuator data in the IOP buffer (unhandled)", port);
            }
    }
}

// scePadSetActDirect: one byte per actuator slot, routed by the alignment
// from scePadSetActAlign (0 = small motor, on/off; 1 = large motor, 0-255).
void set_actuators(int port, int slot, const uint8_t *data) {
    Port &p = g_ports[port][slot];
    uint8_t small = 0, large = 0;
    for (int i = 0; i < 6; i++) {
        if (p.act_align[i] == 0) small = data[i];
        else if (p.act_align[i] == 1) large = data[i];
    }
    if (slot != 0) return;  // no multitap
    LOG(LOG_PAD, "pad %d vibration small %d large %d", port, small, large);
    platform_set_vibration(port, small ? 1 : 0, large);
}

}  // namespace

// Called every vblank.
void iop_pad_vblank() {
    for (int i = 0; i < 2; i++)
        for (int s = 0; s < 4; s++)
            if (g_ports[i][s].open && g_ports[i][s].pad_area) write_pad_data(i, s);
}

void iop_pad_init() {
    auto handler = [](const RpcCall &c, uint8_t *buf) -> uint32_t {
        uint32_t cmd = rd32(buf);
        int port = (int)(rd32(buf + 4) & 1);
        int slot = (int)(rd32(buf + 8) & 3);
        uint32_t result = 1;
        switch (cmd) {
        case 0x01: {  // OPEN(port, slot, padArea)
            Port &p = g_ports[port][slot];
            p.open = true;
            p.pad_area = rd32(buf + 0x10);
            if (!p.iop_buf) p.iop_buf = iop_alloc(64);
            p.req_state = PAD_RSTAT_COMPLETE;
            LOG(LOG_PAD, "pad open port %d slot %d area %08x", port, rd32(buf + 8), p.pad_area);
            wr32(buf + 0x14, p.iop_buf);
            write_pad_data(port, slot);
            break;
        }
        case 0x06: {  // SET_MMODE(port, slot, mode, lock)
            Port &p = g_ports[port][slot];
            p.analog = rd32(buf + 0xC) != 0;
            p.lock = (uint8_t)rd32(buf + 0x10);
            p.req_state = PAD_RSTAT_COMPLETE;
            LOG(LOG_PAD, "pad %d main mode %s lock %d", port, p.analog ? "analog" : "digital", p.lock);
            break;
        }
        case 0x07:  // SET_ACTDIR(port, slot, data[6]): vibration
            set_actuators(port, slot, buf + 0xC);
            g_ports[port][slot].req_state = PAD_RSTAT_COMPLETE;
            break;
        case 0x08: {  // SET_ACTALIGN(port, slot, align[6])
            Port &p = g_ports[port][slot];
            memcpy(p.act_align, buf + 0xC, 6);
            p.req_state = PAD_RSTAT_COMPLETE;
            break;
        }
        case 0x09:  // GET_BTNMASK
            result = 0x3FFFF;
            break;
        case 0x0A: {  // SET_BTNINFO: enables pressure sensitive buttons
            Port &p = g_ports[port][slot];
            p.pressure = rd32(buf + 0xC) != 0;
            p.req_state = PAD_RSTAT_COMPLETE;
            LOG(LOG_PAD, "pad %d pressure mode %d", port, p.pressure);
            break;
        }
        case 0x0B:  // SET_VREF
            break;
        case 0x0C:  // GET_PORTMAX
            result = 2;
            break;
        case 0x0D:  // GET_SLOTMAX
            result = 1;
            break;
        case 0x0E:  // CLOSE
            g_ports[port][slot].open = false;
            if (slot == 0) platform_set_vibration(port, 0, 0);
            break;
        case 0x0F:  // END
        case 0x10:  // INIT
            break;
        case 0x12:  // GET_MODVER: libpad 4.x
            result = 0x400;
            break;
        default:
            LOG(LOG_PAD, "padman command %x", cmd);
        }
        (void)c;
        wr32(buf + 0xC, result);
        return 128;
    };
    iop_register_server(0x80000100, "padman", handler);
    iop_register_server(0x80000101, "padman_ext", handler);
}
