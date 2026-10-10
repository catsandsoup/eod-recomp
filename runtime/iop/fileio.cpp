// fileio.cpp - IOP FILEIO service (RPC 0x80000001) as used by the SDK 2.8
// EE library (sceOpen/sceRead/...).
//
// Protocol (reverse engineered from the EE side, see sceFsInit at 003B3B28
// and the completion handler at 003B35F0):
//  * every request starts with {sema id, EE result address, result size}
//    and carries the index of a 0x440-byte completion slot;
//  * the RPC reply is a single non-zero word meaning "accepted";
//  * on completion the IOP fills the slot {sema, cmd, dest, size, data...}
//    in EE memory and sends SIF command 0x80000011 with opt = slot index;
//    the EE handler copies `data` to `dest` and signals the semaphore.
// Paths ("host0:data\x", "cdrom0:\DATA\X;1") map onto the game directory.
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../core/log.h"
#include "../platform/platform.h"
#include "iop.h"

namespace {

uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

constexpr int kMaxFiles = 64;
FILE *g_files[kMaxFiles];
uint32_t g_slot_base;  // EE address of completion slot 0
bool g_slot_indexed;

enum Cmd { CMD_OPEN = 0, CMD_CLOSE = 1, CMD_READ = 2, CMD_WRITE = 3, CMD_LSEEK = 4, CMD_IOCTL = 5,
           CMD_REMOVE = 6, CMD_MKDIR = 7, CMD_RMDIR = 8, CMD_DOPEN = 9, CMD_DCLOSE = 10, CMD_DREAD = 11,
           CMD_GETSTAT = 12, CMD_CHSTAT = 13 };

// SCE errno values are negative POSIX-style codes
constexpr int kENOENT = -2, kEBADF = -9, kEMFILE = -24, kEINVAL = -22;

std::string strip_device(const char *path, std::string *dev) {
    std::string p = path;
    size_t colon = p.find(':');
    if (colon != std::string::npos) {
        if (dev) *dev = p.substr(0, colon);
        p = p.substr(colon + 1);
    }
    size_t semi = p.find(';');
    if (semi != std::string::npos) p = p.substr(0, semi);
    for (char &c : p)
        if (c == '\\') c = '/';
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    return p;
}

// Resolves a PS2 path to a host path. For writes, the file may not exist.
bool resolve(const char *ps2path, std::string &host, bool for_write) {
    std::string rel = strip_device(ps2path, nullptr);
    char found[1024];
    if (platform_find_file(g_game_root.c_str(), rel.c_str(), found, sizeof found)) {
        host = found;
        return true;
    }
    if (!for_write) return false;
    // resolve the directory part case-insensitively, keep the file name
    size_t slash = rel.find_last_of('/');
    std::string dir = slash == std::string::npos ? "" : rel.substr(0, slash);
    std::string name = slash == std::string::npos ? rel : rel.substr(slash + 1);
    if (dir.empty()) {
        host = g_game_root + "/" + name;
        return true;
    }
    if (!platform_find_file(g_game_root.c_str(), dir.c_str(), found, sizeof found)) return false;
    host = std::string(found) + "/" + name;
    return true;
}

// Writes a completion slot and queues the 0x80000011 command.
void complete(const uint8_t *req, uint32_t slot, uint32_t cmd, int32_t result, const void *extra = nullptr,
              uint32_t extra_size = 0) {
    uint32_t idx = g_slot_indexed ? slot : 0;
    uint32_t a = g_slot_base + idx * 0x440;
    uint8_t s[0x440] = {};
    memcpy(s + 0, req + 0, 4);  // sema
    uint32_t c = cmd;
    memcpy(s + 4, &c, 4);
    memcpy(s + 8, req + 4, 4);   // result destination
    memcpy(s + 12, req + 8, 4);  // result size
    memcpy(s + 16, &result, 4);
    if (extra_size) memcpy(s + 20, extra, extra_size);
    ee_write_block(a, s, 20 + extra_size);
    iop_send_cmd(0x80000011, slot, nullptr, 0);
}

int file_open(const char *name, uint32_t flags) {
    int fd = -1;
    for (int i = 0; i < kMaxFiles; i++)
        if (!g_files[i]) {
            fd = i;
            break;
        }
    if (fd < 0) return kEMFILE;
    bool wr = (flags & 3) != 1;
    std::string host;
    if (!resolve(name, host, wr && (flags & 0x200))) {
        LOG(LOG_FILE, "open '%s' -> not found", name);
        return kENOENT;
    }
    const char *mode = "rb";
    if (wr) {
        if (flags & 0x400) mode = (flags & 3) == 3 ? "w+b" : "wb";
        else if (flags & 0x100) mode = (flags & 3) == 3 ? "a+b" : "ab";
        else if (flags & 0x200) mode = "r+b";
        else mode = "r+b";
    }
    FILE *f = fopen(host.c_str(), mode);
    if (!f && wr && (flags & 0x200)) f = fopen(host.c_str(), "w+b");
    if (!f) {
        LOG(LOG_FILE, "open '%s' (%s) failed: %s", name, host.c_str(), strerror(errno));
        return kENOENT;
    }
    g_files[fd] = f;
    LOG(LOG_FILE, "open '%s' flags=%x -> fd %d", name, flags, fd + 3);
    return fd + 3;  // keep 0-2 free (stdin/out/err)
}

FILE *file_get(int32_t fd) {
    fd -= 3;
    if (fd < 0 || fd >= kMaxFiles) return nullptr;
    return g_files[fd];
}

}  // namespace

// Direct host access helpers for other HLE modules.
FILE *iop_fileio_host_file(int32_t fd) { return file_get(fd); }

void iop_fileio_init() {
    iop_register_server(0x80000001, "fileio", [](const RpcCall &c, uint8_t *buf) -> uint32_t {
        uint32_t accepted = 1;
        if (c.fno == 0xFF) {
            g_slot_base = rd32(buf);
            g_slot_indexed = true;
            LOG(LOG_FILE, "fileio init: slots at %08x", g_slot_base);
            memcpy(buf, "2800", 4);
            uint32_t mode = 2;  // per-request completion slots
            memcpy(buf + 4, &mode, 4);
            return 8;
        }
        switch (c.fno) {
        case CMD_OPEN: {
            char name[1025] = {};
            memcpy(name, buf + 0x14, 1024);
            int r = file_open(name, rd32(buf + 0xC));
            complete(buf, rd32(buf + 0x414), CMD_OPEN, r);
            break;
        }
        case CMD_CLOSE: {
            int32_t fd = (int32_t)rd32(buf + 0xC);
            FILE *f = file_get(fd);
            int r = 0;
            if (f) {
                fclose(f);
                g_files[fd - 3] = nullptr;
            } else {
                r = kEBADF;
            }
            LOG(LOG_FILE, "close %d", fd);
            complete(buf, rd32(buf + 0x10), CMD_CLOSE, r);
            break;
        }
        case CMD_READ: {
            int32_t fd = (int32_t)rd32(buf + 0xC);
            uint32_t dst = rd32(buf + 0x10), size = rd32(buf + 0x14);
            FILE *f = file_get(fd);
            int32_t r;
            if (!f) {
                r = kEBADF;
            } else {
                static std::vector<uint8_t> tmp;
                tmp.resize(size);
                r = (int32_t)fread(tmp.data(), 1, size, f);
                ee_write_block(dst, tmp.data(), (uint32_t)r);
            }
            LOG(LOG_FILE, "read fd %d -> %08x size %u = %d", fd, dst, size, r);
            // no unaligned head/tail fixups: everything was written directly
            uint8_t extra[16] = {};
            complete(buf, rd32(buf + 0x1C), CMD_READ, r, extra, sizeof extra);
            break;
        }
        case CMD_WRITE: {
            // {.., fd, buf, size, mis, aligned[16], slot}
            int32_t fd = (int32_t)rd32(buf + 0xC);
            uint32_t src = rd32(buf + 0x10), size = rd32(buf + 0x14), mis = rd32(buf + 0x18);
            FILE *f = file_get(fd);
            int32_t r;
            if (fd == 1 || fd == 2) {
                std::string s(size, '\0');
                if (mis > 16) mis = 16;
                memcpy(&s[0], buf + 0x1C, mis < size ? mis : size);
                if (size > mis) ee_read_block(src + mis, &s[mis], size - mis);
                LOG(LOG_GAME, "%s", s.c_str());
                r = (int32_t)size;
            } else if (!f) {
                r = kEBADF;
            } else {
                std::vector<uint8_t> tmp(size);
                if (mis > 16) mis = 16;
                memcpy(tmp.data(), buf + 0x1C, mis < size ? mis : size);
                if (size > mis) ee_read_block(src + mis, tmp.data() + mis, size - mis);
                r = (int32_t)fwrite(tmp.data(), 1, size, f);
            }
            complete(buf, rd32(buf + 0x2C), CMD_WRITE, r);
            break;
        }
        case CMD_LSEEK: {
            int32_t fd = (int32_t)rd32(buf + 0xC);
            int32_t off = (int32_t)rd32(buf + 0x10);
            int whence = (int)rd32(buf + 0x14);
            FILE *f = file_get(fd);
            int32_t r;
            if (!f) {
                r = kEBADF;
            } else if (fseek(f, off, whence == 2 ? SEEK_END : whence == 1 ? SEEK_CUR : SEEK_SET) != 0) {
                r = kEINVAL;
            } else {
                r = (int32_t)ftell(f);
            }
            LOG(LOG_FILE, "lseek fd %d %d/%d = %d", fd, off, whence, r);
            complete(buf, rd32(buf + 0x18), CMD_LSEEK, r);
            break;
        }
        default: {
            char hex[3 * 32 + 1] = {};
            for (uint32_t i = 0; i < 32 && i < c.in_size; i++) snprintf(hex + 3 * i, 4, "%02x ", buf[i]);
            WARN("fileio: unimplemented command %u (in %u): %s", c.fno, c.in_size, hex);
            accepted = 0;  // EE returns an error immediately
        }
        }
        memcpy(buf, &accepted, 4);
        return 4;
    });
}
