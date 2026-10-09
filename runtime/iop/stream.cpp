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
//   3  Stream requests (audio): at +0x40, n (word 0) entries {lsn, bytes,
//      flags}: flags bits 8-11 = channel, bit 0 = clear the channel's queue
//      first.  The data (SPU ADPCM, mono) is fed into a looping ring buffer
//      in sound RAM played by one SPU2 voice per channel; the game sets
//      that voice's volume and pitch itself (see "Audio streaming" below).
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
#include "spu2.h"

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
    for (const DiscFile &f : g_files)
        LOG(LOG_FILE, "stream: lsn %7u-%7u %s", f.lsn, f.lsn + f.sectors, f.host_path.c_str());
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

// ------------------------------------------------------------ audio streaming
// As stream.irx's SpuChannel: voice `voice` of core 0 loops over a ring
// buffer in sound RAM (loop start flags on its first block, loop end+repeat
// on its last); the writer stays ahead of the voice's play address, with
// queued data or silence.
struct StreamChannel {
    int voice;
    uint32_t start, size;  // ring buffer in sound RAM
    uint16_t pitch;        // default; the game sets its own
    bool running = false;
    uint32_t wpos = 0;  // ring offset written up to
    struct Req {
        uint32_t lsn, bytes;
    };
    std::vector<Req> queue;
    uint32_t done = 0;  // bytes of queue[0] already fed
};
StreamChannel g_chan[2] = {{0, 0x1FC000, 0x4000, 0x1FD}, {1, 0x1F4000, 0x8000, 0x751}};

uint16_t voice_entry(int voice, uint16_t param) { return (uint16_t)(param | (voice << 1)); }

// Writes `n` bytes (multiple of 16) at ring offset `off` from the queue, or silence.
void feed(StreamChannel &c, uint32_t off, uint32_t n) {
    static std::vector<uint8_t> buf;
    buf.assign(n, 0);
    uint32_t got = 0;
    while (got < n && !c.queue.empty()) {
        StreamChannel::Req &r = c.queue.front();
        uint32_t take = std::min(n - got, r.bytes - c.done);
        // read whole sectors around the wanted bytes
        uint64_t pos = (uint64_t)r.lsn * kSector + c.done;
        uint32_t first = (uint32_t)(pos / kSector), skip = (uint32_t)(pos % kSector);
        uint32_t secs = (skip + take + kSector - 1) / kSector;
        static std::vector<uint8_t> tmp;
        tmp.resize((size_t)secs * kSector);
        disc_read(first, tmp.data(), secs * kSector);
        memcpy(buf.data() + got, tmp.data() + skip, take);
        got += take;
        c.done += take;
        if (c.done >= r.bytes) {
            c.queue.erase(c.queue.begin());
            c.done = 0;
        }
    }
    for (uint32_t i = 0; i < n; i += 16) {
        uint8_t &f = buf[i + 1];
        if (i >= got) {
            buf[i] = 0;
            f = 2;  // silence
            memset(&buf[i + 2], 0, 14);
        } else {
            f &= ~7u;
        }
        uint32_t o = off + i;
        if (o == 0) f |= 6;               // loop start
        if (o == c.size - 16) f |= 3;     // loop end + repeat
    }
    spu2_sram_write(c.start + off, buf.data(), n);
}

void start_channel(StreamChannel &c) {
    feed(c, 0, c.size);
    c.wpos = 0;  // the whole ring is ahead of the voice
    spu2_set_addr(voice_entry(c.voice, 0x2040), c.start);
    spu2_set_param(voice_entry(c.voice, 0x0000), 0x3FFF);
    spu2_set_param(voice_entry(c.voice, 0x0100), 0x3FFF);
    spu2_set_param(voice_entry(c.voice, 0x0200), c.pitch);
    spu2_set_switch(0x1500, 1u << c.voice);
    c.running = true;
}

}  // namespace

// Keeps the stream voices' ring buffers ahead of playback (called often).
void iop_stream_update() {
    for (StreamChannel &c : g_chan) {
        if (!c.running) continue;
        uint32_t nax = spu2_get_addr(voice_entry(c.voice, 0x2240));
        if (nax < c.start || nax >= c.start + c.size) continue;  // voice was moved: leave it
        uint32_t play = (nax - c.start) & ~15u;
        // writable: from wpos up to 0x200 bytes before the play position
        uint32_t limit = (play + c.size - 0x200) % c.size;
        uint32_t avail = (limit + c.size - c.wpos) % c.size;
        if (avail > c.size - 0x200) avail = 0;  // wpos is inside the guard zone
        avail &= ~0x3Fu;
        while (avail) {
            uint32_t n = std::min(avail, c.size - c.wpos);
            feed(c, c.wpos, n);
            c.wpos = (c.wpos + n) % c.size;
            avail -= n;
        }
    }
}

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
            uint32_t n = std::min<uint32_t>(rd32(buf), (c.in_size - 0x40) / 16);
            for (uint32_t i = 0; i < n; i++) {
                const uint8_t *e = buf + 0x40 + i * 16;
                uint32_t lsn = rd32(e), bytes = rd32(e + 4), flags = rd32(e + 8);
                uint32_t ch = (flags >> 8) & 15;
                LOG(LOG_SND, "stream: channel %u request lsn %u, %u bytes, flags %x", ch, lsn, bytes, flags);
                if (ch > 1) continue;
                StreamChannel &sc = g_chan[ch];
                if (flags & 1) {
                    sc.queue.clear();
                    sc.done = 0;
                }
                if (lsn != 0xFFFFFFFFu && bytes && bytes != 0xFFFFFFFFu) sc.queue.push_back({lsn, bytes});
                if (!sc.running && !sc.queue.empty()) start_channel(sc);
            }
            return 0;
        }
        case 4:  // debug command
            return 0;
        }
        WARN("stream: unknown function %u", c.fno);
        return 0;
    });
}
