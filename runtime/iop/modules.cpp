// modules.cpp - IOP module loading (loadfile RPC server) and registry of
// the HLE replacements for each IRX the game loads.
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "../core/log.h"
#include "iop.h"

std::string g_game_root = ".";

namespace {

std::string basename_lower(const std::string &path) {
    size_t p = path.find_last_of("\\/:");
    std::string b = p == std::string::npos ? path : path.substr(p + 1);
    size_t semi = b.find(';');
    if (semi != std::string::npos) b = b.substr(0, semi);
    for (char &c : b) c = (char)tolower((unsigned char)c);
    return b;
}

int g_next_module_id = 1;

}  // namespace

int iop_load_module(const std::string &path, const std::string &args) {
    std::string name = basename_lower(path);
    LOG(LOG_IOP, "load module %s (%s) args='%s'", name.c_str(), path.c_str(), args.c_str());
    // Each emulated module registers its RPC servers once at startup; the
    // load request only needs to succeed.
    return g_next_module_id++;
}

void iop_modules_init() {
    // loadfile: sceSifLoadModule & friends
    iop_register_server(0x80000006, "loadfile", [](const RpcCall &c, uint8_t *buf) -> uint32_t {
        // layout: int result/arglen; int modres; char path[252]; char args[252]
        char path[253] = {}, args[253] = {};
        memcpy(path, buf + 8, 252);
        int arglen;
        memcpy(&arglen, buf, 4);
        if (arglen > 0 && arglen <= 252) memcpy(args, buf + 8 + 252, arglen);
        int result = 0;
        if (c.fno == 0xFF) {  // module version query: SDK 2.8
            memcpy(buf, "2800", 4);
            return 4;
        }
        {
            char hex[3 * 48 + 1] = {};
            for (uint32_t i = 0; i < 48 && i < c.in_size; i++) snprintf(hex + 3 * i, 4, "%02x ", buf[i]);
            LOG(LOG_IOP, "loadfile fno %d in=%u recv=%u: %s", c.fno, c.in_size, c.recv_size, hex);
        }
        switch (c.fno) {
        case 0:  // LF_F_MOD_LOAD
        case 6:  // LF_F_MOD_BUF_LOAD
            result = iop_load_module(path, args);
            break;
        case 1:  // LF_F_ELF_LOAD
            WARN("loadfile: ELF load '%s' not supported", path);
            result = -1;
            break;
        default: {
            char hex[3 * 48 + 1] = {};
            for (uint32_t i = 0; i < 48 && i < c.in_size; i++) snprintf(hex + 3 * i, 4, "%02x ", buf[i]);
            WARN("loadfile fno %d in=%u recv=%u: %s", c.fno, c.in_size, c.recv_size, hex);
            result = 0;
        }
        }
        int modres = 0;
        memcpy(buf, &result, 4);
        memcpy(buf + 4, &modres, 4);
        return 8;
    });
    iop_fileio_init();
    iop_cdvd_init();
    iop_pad_init();
    iop_mc_init();
    iop_sound_init();
    iop_misc_init();
    iop_stream_init();
}
