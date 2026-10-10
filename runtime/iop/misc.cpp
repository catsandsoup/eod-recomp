// misc.cpp - small IOP services: iopheap, multitap, Logitech USB devices.
#include <cstring>

#include "../core/log.h"
#include "iop.h"

namespace {
uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
}  // namespace

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

    // multitap (mtapman.irx): no multitap connected
    for (uint32_t sid = 0x80000901; sid <= 0x80000905; sid++) {
        iop_register_server(sid, "mtapman", [](const RpcCall &c, uint8_t *buf) -> uint32_t {
            memset(buf, 0, c.recv_size);
            return c.recv_size;
        });
    }

    // Logitech USB wheel/keyboard (lgdev.irx, liblgdev): report the expected
    // module version and no devices.
    iop_register_server(0x046D046D, "lgdev", [](const RpcCall &c, uint8_t *buf) -> uint32_t {
        memset(buf, 0, c.recv_size);
        if (c.fno == 12) wr32(buf + 4, 0x010B1900);  // lgDevInit: module version
        else LOG(LOG_IOP, "lgdev fno %u", c.fno);
        return c.recv_size;
    });
}
