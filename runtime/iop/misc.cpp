// misc.cpp - small IOP services: iopheap, multitap, Logitech USB devices.
#include <cstring>

#include "../core/log.h"
#include "../platform/pad_script.h"
#include "iop.h"

namespace {
uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

bool g_tap_open;
}  // namespace

bool iop_port_has_multitap(unsigned port) { return port == 0 && g_tap_open; }

void iop_misc_init() {
    // iopheap: sceSifAllocIopHeap / sceSifFreeIopHeap / sceSifLoadIopHeap
    iop_register_server(0x80000003, "iopheap", [](const RpcCall &c, uint8_t *buf) -> uint32_t {
        switch (c.fno) {
        case 1: {
            uint32_t size = rd32(buf);
            uint32_t a = iop_alloc(size);
            LOG(LOG_IOP, "AllocIopHeap(%u) = %08x", size, a);
            wr32(buf, a);
            return 4;
        }
        case 2:
            wr32(buf, 0);
            return 4;
        default:
            WARN("iopheap fno %u not implemented", c.fno);
            wr32(buf, 0);
            return 4;
        }
    });

    // Retail libmtap (EE 0x390D20–0x391178) sends 128-byte blocks, fno 1.
    // MTAPMAN.IRX's port open succeeds even if its subsequent connection query is false.
    g_tap_open = false;
    constexpr uint32_t open = 0x80000901, close = 0x80000902, connection = 0x80000903;
    constexpr uint32_t priority = 0x80000904, version = 0x80000905;
    auto mtap = [](const RpcCall &c, uint8_t *buf) -> uint32_t {
        constexpr uint32_t packet_size = 128;
        const uint32_t port = c.in_size >= 4 ? rd32(buf) : UINT32_MAX;
        memset(buf, 0, packet_size);
        if (c.fno != 1) return packet_size;
        uint32_t result = 0;
        switch (c.sid) {
        case open:
            if (port == 0) {
                g_tap_open = pad_script_spec(2) || pad_script_spec(3) || pad_script_spec(4);
                LOG(LOG_PAD, "multitap port 0 %s", g_tap_open ? "connected (scripted)" : "absent");
            }
            result = port < 4;
            break;
        case close:
            if (port == 0) g_tap_open = false;
            result = port < 4;
            break;
        case connection:
            result = iop_port_has_multitap(port);
            break;
        case priority:
            // This SDK call is unused by EoD. There are no multitap IOP threads to reprioritize.
            return packet_size;
        case version:
            wr32(buf, 0x302);  // MTAPMAN.IRX version at 0x19C4
            return packet_size;
        }
        wr32(buf + 4, result);
        return packet_size;
    };
    for (uint32_t sid : {open, close, connection, priority, version})
        iop_register_server(sid, "mtapman", mtap);

    // Logitech USB wheel/keyboard (lgdev.irx, liblgdev): report the expected
    // module version and no devices.
    iop_register_server(0x046D046D, "lgdev", [](const RpcCall &c, uint8_t *buf) -> uint32_t {
        memset(buf, 0, c.recv_size);
        if (c.fno == 12) wr32(buf + 4, 0x010B1900);  // lgDevInit: module version
        else LOG(LOG_IOP, "lgdev fno %u", c.fno);
        return c.recv_size;
    });
}
