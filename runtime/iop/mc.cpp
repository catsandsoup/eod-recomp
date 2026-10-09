// mc.cpp - memory card server (mcserv.irx, RPC 0x80000400).
// First version: reports no memory card in either slot.
#include <cstring>

#include "../core/log.h"
#include "iop.h"

void iop_mc_init() {
    iop_register_server(0x80000400, "mcserv", [](const RpcCall &c, uint8_t *buf) -> uint32_t {
        int32_t r[4] = {0, 0, 0, 0};
        switch (c.fno) {
        case 0xFE:  // init: result, mcserv version, mcman version
            r[0] = 0;
            r[1] = 0x20F;
            r[2] = 0x20F;
            memcpy(buf, r, 12);
            return 12;
        case 0x01:  // GetInfo
            LOG(LOG_MC, "mc GetInfo port %d -> no card", (int)buf[4]);
            r[0] = -10;
            memcpy(buf, r, 4);
            return 4;
        default:
            LOG(LOG_MC, "mc fno %u -> no card", c.fno);
            r[0] = -10;
            memcpy(buf, r, 4);
            return 4;
        }
    });
}
