// mc.cpp - memory card server (mcserv.irx, RPC 0x80000400), backed by a
// host directory per port: <game dir>/memcard/port0, port1.
//
// Request layouts (from the SDK 2.8 libmc in the executable):
//   descParam (48 bytes): +0 fd/port, +4 port/slot, +8 slot, +0xC size,
//     +0x10 offset/free flag, +0x14 origin/type flag/mis, +0x18 buffer,
//     +0x1C EE address of the 0xC0-byte "end parameter" block, +0x20 data
//   nameParam (0x414 bytes): +0 port, +4 slot, +8 flags, +0x18 name
// All calls complete immediately; the reply word is the result.
#include <cstdlib>
#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../core/log.h"
#include "../platform/platform.h"
#include "iop.h"

#ifdef _WIN32
#include <direct.h>
#define mkdir_host(p) _mkdir(p)
#else
#define mkdir_host(p) mkdir(p, 0755)
#endif

namespace {

uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

enum { MC_NOENTRY = -4, MC_DENIED = -5, MC_FULL = -3, MC_UPLIMIT = -7 };

struct McFile {
    FILE *f = nullptr;
    int port = 0;
};
McFile g_fds[32];
bool g_seen[2];
std::string g_cwd[2] = {"/", "/"};

std::string port_root(int port) {
    // EOD_MEMCARD=dir puts the cards elsewhere (e.g. a fresh card for tests)
    const char *env = getenv("EOD_MEMCARD");
    std::string r = env ? std::string(env) : g_game_root + "/memcard";
    mkdir_host(r.c_str());
    r += port ? "/port1" : "/port0";
    mkdir_host(r.c_str());
    return r;
}

std::string host_path(int port, const char *name) {
    std::string n = name;
    if (n.empty() || n[0] != '/') n = g_cwd[port & 1] + (g_cwd[port & 1].back() == '/' ? "" : "/") + n;
    for (char &c : n)
        if (c == '\\') c = '/';
    return port_root(port & 1) + n;
}

bool exists(const std::string &p, bool *is_dir = nullptr) {
    struct stat s;
    if (stat(p.c_str(), &s) != 0) return false;
    if (is_dir) *is_dir = (s.st_mode & S_IFDIR) != 0;
    return true;
}

int mc_open(int port, const char *name, uint32_t flags) {
    std::string p = host_path(port, name);
    if (flags & 0x40) {  // mkdir
        bool dir;
        if (exists(p, &dir)) return MC_NOENTRY;
        return mkdir_host(p.c_str()) == 0 ? 0 : MC_NOENTRY;
    }
    int fd = -1;
    for (int i = 0; i < 32; i++)
        if (!g_fds[i].f) {
            fd = i;
            break;
        }
    if (fd < 0) return MC_UPLIMIT;
    const char *mode = "rb";
    bool create = flags & 0x200;
    bool write = (flags & 3) != 1;
    if (create) mode = "w+b";
    else if (write) mode = "r+b";
    if (!create && !exists(p)) return MC_NOENTRY;
    FILE *f = fopen(p.c_str(), mode);
    if (!f) return MC_NOENTRY;
    g_fds[fd].f = f;
    g_fds[fd].port = port;
    LOG(LOG_MC, "mc open '%s' flags %x -> %d", name, flags, fd);
    return fd;
}

}  // namespace

void iop_mc_init() {
    iop_register_server(0x80000400, "mcserv", [](const RpcCall &c, uint8_t *buf) -> uint32_t {
        int32_t r = 0;
        switch (c.fno) {
        case 0xFE: {  // init: result, mcserv version, mcman version
            int32_t v[3] = {0, 0x20F, 0x20F};
            memcpy(buf, v, 12);
            return 12;
        }
        case 0x01: {  // GetInfo(port, slot): type/free/format into the end parameter block
            int port = (int)rd32(buf + 4) & 1;
            uint32_t param = rd32(buf + 0x1C);
            uint8_t end[0xC0] = {};
            wr32(end + 0x00, 2);     // PS2 memory card
            wr32(end + 0x04, 8000);  // free clusters (KB)
            wr32(end + 0x90, 1);     // formatted
            if (param) ee_write_block(param, end, sizeof end);
            port_root(port);
            r = g_seen[port] ? 0 : -1;  // -1: new (formatted) card
            g_seen[port] = true;
            LOG(LOG_MC, "mc GetInfo port %d -> %d", port, r);
            break;
        }
        case 0x02: {  // Open / Mkdir
            int port = (int)rd32(buf) & 1;
            uint32_t flags = rd32(buf + 8);
            char name[1024] = {};
            memcpy(name, buf + 0x18, 1023);
            r = mc_open(port, name, flags);
            break;
        }
        case 0x03: {  // Close(fd)
            int fd = (int)rd32(buf);
            if (fd >= 0 && fd < 32 && g_fds[fd].f) {
                fclose(g_fds[fd].f);
                g_fds[fd].f = nullptr;
            } else {
                r = MC_DENIED;
            }
            break;
        }
        case 0x04: {  // Seek(fd, offset, origin)
            int fd = (int)rd32(buf);
            int32_t off = (int32_t)rd32(buf + 0x10);
            int whence = (int)rd32(buf + 0x14);
            if (fd >= 0 && fd < 32 && g_fds[fd].f) {
                fseek(g_fds[fd].f, off, whence == 2 ? SEEK_END : whence == 1 ? SEEK_CUR : SEEK_SET);
                r = (int32_t)ftell(g_fds[fd].f);
            } else {
                r = MC_DENIED;
            }
            break;
        }
        case 0x05: {  // Read(fd, buffer, size): data straight into EE memory
            int fd = (int)rd32(buf);
            uint32_t size = rd32(buf + 0xC), dst = rd32(buf + 0x18), param = rd32(buf + 0x1C);
            if (fd >= 0 && fd < 32 && g_fds[fd].f) {
                std::vector<uint8_t> tmp(size);
                r = (int32_t)fread(tmp.data(), 1, size, g_fds[fd].f);
                ee_write_block(dst, tmp.data(), (uint32_t)r);
            } else {
                r = MC_DENIED;
            }
            if (param) {  // no unaligned head/tail fix-ups needed
                uint8_t z[16] = {};
                ee_write_block(param, z, sizeof z);
            }
            break;
        }
        case 0x06: {  // Write(fd, buffer, size): head bytes inline, rest from EE memory
            int fd = (int)rd32(buf);
            uint32_t size = rd32(buf + 0xC), mis = rd32(buf + 0x14), src = rd32(buf + 0x18);
            if (fd >= 0 && fd < 32 && g_fds[fd].f) {
                if (mis > 16) mis = 16;
                std::vector<uint8_t> tmp(mis + size);
                memcpy(tmp.data(), buf + 0x20, mis);
                if (size) ee_read_block(src, tmp.data() + mis, size);
                r = (int32_t)fwrite(tmp.data(), 1, tmp.size(), g_fds[fd].f);
                fflush(g_fds[fd].f);
            } else {
                r = MC_DENIED;
            }
            break;
        }
        case 0x0A:  // Flush
        case 0x0C:  // ChDir (not used by this game)
            r = 0;
            break;
        case 0x0F: {  // Delete(port, slot, name)
            int port = (int)rd32(buf) & 1;
            char name[1024] = {};
            memcpy(name, buf + 0x18, 1023);
            std::string p = host_path(port, name);
            r = (remove(p.c_str()) == 0) ? 0 : MC_NOENTRY;
            LOG(LOG_MC, "mc delete '%s' -> %d", name, r);
            break;
        }
        case 0x10:  // Format
            r = 0;
            break;
        default:
            WARN("mcserv: unhandled function %u", c.fno);
            r = MC_DENIED;
        }
        wr32(buf, (uint32_t)r);
        return 4;
    });
}
