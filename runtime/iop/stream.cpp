// stream.cpp - HLE of Monster Games' stream.irx ("MGI_Streamer", RPC
// 0x8000021A): disc access and audio streaming for the game.
//
// RPC functions (from the module's dispatcher at 0x0000):
//   1  StreamInit(cfg[64]): cfg[1] must be version 4.  Replies with the disc
//      table of contents: 256 entries x 24 bytes, built from the ISO9660
//      directories exactly like the module does (each directory's records
//      ".", "..", sorted names, then its subdirectories recursively).
//      Entry: [0] ISO flags (2 = dir), [1..15] name without ";1",
//      +0x10 LSN, +0x14 size in bytes.
//   2  Read{lsn, sectors, ee_buffer, ee_status}: sectors are copied into EE
//      memory and 0 is written to ee_status when done (the EE writes -1
//      before issuing the request).
//   3  Stream requests (audio)  - TODO
//   4  Debug command
//
// The "disc" is the game directory: SLUS_209.10, SYSTEM.CNF and DATA/.
// Sectors are assigned to files in TOC order.
#include <algorithm>
#include <cctype>
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
void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

constexpr int kMaxToc = 256;
constexpr uint32_t kSector = 2048;

struct DiscFile {
    std::string host_path;
    uint32_t lsn, sectors;
    uint64_t size;
    std::vector<uint8_t> synthetic;  // contents for files missing from the disc dump
};

struct TocEntry {
    uint8_t flags;
    std::string name;
    uint32_t lsn, size;
};

std::vector<DiscFile> g_files;  // sorted by lsn
std::vector<TocEntry> g_toc;
uint32_t g_next_lsn = 0x100;
bool g_built;

std::string upper(std::string s) {
    for (char &c : s) c = (char)toupper((unsigned char)c);
    return s;
}

struct Node {
    std::string name;  // ISO name (upper case)
    std::string host;
    bool dir;
    uint64_t size;
    uint32_t lsn = 0, sectors = 0;
    std::vector<Node> children;
    std::vector<uint8_t> synthetic;
};

// Sound banks the game cannot run without: an empty bank keeps it going
// (without those sounds) when the dump lacks them.
void add_fallbacks(Node &data) {
    const char *banks[] = {"SPEECH.BNK"};
    for (const char *b : banks) {
        bool have = false;
        for (auto &c : data.children)
            if (c.name == b) have = true;
        if (have) continue;
        Node n;
        n.name = b;
        n.dir = false;
        n.synthetic = {'k', 'n', 'a', 'B', 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        n.size = n.synthetic.size();
        WARN("stream: %s missing from the game data - using an empty sound bank", b);
        data.children.push_back(std::move(n));
    }
}

void scan(Node &n) {
    std::vector<DirEntryInfo> list;
    platform_list_dir(n.host.c_str(), list);
    for (auto &e : list) {
        Node c;
        c.name = upper(e.name);
        c.host = n.host + "/" + e.name;
        c.dir = e.is_dir;
        c.size = e.size;
        if (c.dir) scan(c);
        n.children.push_back(std::move(c));
    }
    // ISO9660 orders records by name
    std::sort(n.children.begin(), n.children.end(), [](const Node &a, const Node &b) { return a.name < b.name; });
}

void assign(Node &n) {
    if (n.dir) {
        n.lsn = g_next_lsn;
        n.sectors = 1;
        g_next_lsn += 1;
        for (auto &c : n.children) assign(c);
    } else {
        n.lsn = g_next_lsn;
        n.sectors = (uint32_t)((n.size + kSector - 1) / kSector);
        g_next_lsn += n.sectors ? n.sectors : 1;
        g_files.push_back({n.host, n.lsn, n.sectors, n.size, n.synthetic});
    }
}

// Mirrors the module's directory walk (sub_04e74).
void build_toc(const Node &dir, const Node *parent) {
    if ((int)g_toc.size() >= kMaxToc) return;
    size_t start = g_toc.size();
    g_toc.push_back({2, ".", dir.lsn, kSector});
    g_toc.push_back({2, "..", parent ? parent->lsn : dir.lsn, kSector});
    for (const Node &c : dir.children) {
        if ((int)g_toc.size() >= kMaxToc) {
            WARN("stream: too many entries for the TOC (%s)", c.host.c_str());
            break;
        }
        g_toc.push_back({(uint8_t)(c.dir ? 2 : 0), c.name, c.lsn, c.dir ? kSector : (uint32_t)c.size});
    }
    size_t end = g_toc.size();
    (void)start;
    (void)end;
    for (const Node &c : dir.children)
        if (c.dir) build_toc(c, &dir);
}

void build_disc() {
    if (g_built) return;
    g_built = true;
    Node root;
    root.name = "";
    root.host = g_game_root;
    root.dir = true;
    // only the disc contents, not whatever else lives next to it
    std::vector<DirEntryInfo> list;
    platform_list_dir(root.host.c_str(), list);
    for (auto &e : list) {
        std::string u = upper(e.name);
        if (u != "DATA" && u != "SLUS_209.10" && u != "SYSTEM.CNF") continue;
        Node c;
        c.name = u;
        c.host = root.host + "/" + e.name;
        c.dir = e.is_dir;
        c.size = e.size;
        if (c.dir) scan(c);
        if (c.dir && u == "DATA") {
            add_fallbacks(c);
            std::sort(c.children.begin(), c.children.end(),
                      [](const Node &a, const Node &b) { return a.name < b.name; });
        }
        root.children.push_back(std::move(c));
    }
    std::sort(root.children.begin(), root.children.end(),
              [](const Node &a, const Node &b) { return a.name < b.name; });
    assign(root);
    build_toc(root, nullptr);
    LOG(LOG_FILE, "stream: virtual disc with %zu files, %u sectors, %zu TOC entries", g_files.size(), g_next_lsn,
        g_toc.size());
}

// Reads `bytes` starting at sector `lsn` (may span files; gaps read as zero).
void disc_read(uint32_t lsn, uint8_t *out, uint32_t bytes) {
    memset(out, 0, bytes);
    uint64_t pos = (uint64_t)lsn * kSector, end = pos + bytes;
    for (const DiscFile &f : g_files) {
        uint64_t fs = (uint64_t)f.lsn * kSector, fe = fs + f.size;
        if (fe <= pos || fs >= end) continue;
        uint64_t a = std::max(pos, fs), b = std::min(end, fe);
        if (!f.synthetic.empty()) {
            memcpy(out + (a - pos), f.synthetic.data() + (a - fs), (size_t)(b - a));
            continue;
        }
        FILE *h = fopen(f.host_path.c_str(), "rb");
        if (!h) continue;
        fseek(h, (long)(a - fs), SEEK_SET);
        size_t got = fread(out + (a - pos), 1, (size_t)(b - a), h);
        (void)got;
        fclose(h);
    }
}

uint32_t g_cfg[16];

}  // namespace

void iop_stream_init() {
    iop_register_server(0x8000021A, "stream", [](const RpcCall &c, uint8_t *buf) -> uint32_t {
        switch (c.fno) {
        case 1: {  // StreamInit
            memcpy(g_cfg, buf, sizeof g_cfg);
            if (g_cfg[1] != 4) WARN("stream: protocol version %u (expected 4)", g_cfg[1]);
            build_disc();
            memset(buf, 0, kMaxToc * 24);
            for (size_t i = 0; i < g_toc.size(); i++) {
                uint8_t *e = buf + i * 24;
                e[0] = g_toc[i].flags;
                strncpy(reinterpret_cast<char *>(e + 1), g_toc[i].name.c_str(), 15);
                wr32(e + 0x10, g_toc[i].lsn);
                wr32(e + 0x14, g_toc[i].size);
            }
            return kMaxToc * 24;
        }
        case 2: {  // Read
            uint32_t lsn = rd32(buf), sectors = rd32(buf + 4), dst = rd32(buf + 8), status = rd32(buf + 12);
            uint32_t bytes = sectors * kSector;
            static std::vector<uint8_t> tmp;
            tmp.resize(bytes);
            disc_read(lsn, tmp.data(), bytes);
            ee_write_block(dst & 0x1FFFFFFF, tmp.data(), bytes);
            ee_w32(status & 0x01FFFFFF, 0);
            LOG(LOG_FILE, "stream read lsn %u x%u -> %08x", lsn, sectors, dst);
            return 0;
        }
        case 3: {  // stream (audio) requests
            uint32_t n = rd32(buf);
            static int warned;
            if (warned++ < 8) LOG(LOG_SND, "stream: %u audio stream requests (not implemented)", n);
            return 0;
        }
        case 4:  // debug command
            return 0;
        }
        WARN("stream: unknown function %u", c.fno);
        return 0;
    });
}
