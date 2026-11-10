// texture_replacements.cpp - see texture_replacements.h.
#include "texture_replacements.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gs {
namespace hosttex {

uint64_t index_key(uint32_t bits, uint32_t width, uint32_t height, const uint8_t *indices) {
    uint8_t head[16];
    memcpy(head, "EODTEX1", 7);
    head[7] = (uint8_t)bits;
    for (int i = 0; i < 4; i++) {
        head[8 + i] = (uint8_t)(width >> (8 * i));
        head[12 + i] = (uint8_t)(height >> (8 * i));
    }
    return fnv1a64(indices, (size_t)width * height, fnv1a64(head, sizeof head));
}

uint64_t palette_key(const uint32_t *pal, uint32_t n) {
    uint64_t h = kFnvOffset;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t rgb[3] = {(uint8_t)pal[i], (uint8_t)(pal[i] >> 8), (uint8_t)(pal[i] >> 16)};
        h = fnv1a64(rgb, 3, h);
    }
    return h;
}

size_t htx_level_offset(uint32_t w, uint32_t h, uint32_t level) {
    size_t off = kHtxHeaderBytes;
    for (uint32_t i = 0; i < level; i++) off += (size_t)std::max(1u, w >> i) * std::max(1u, h >> i) * 4;
    return off;
}

size_t htx_total_bytes(uint32_t w, uint32_t h, uint32_t levels) { return htx_level_offset(w, h, levels); }

uint32_t full_chain_levels(uint32_t w, uint32_t h) {
    uint32_t m = std::max(w, h), n = 1;
    while (m > 1) {
        m >>= 1;
        n++;
    }
    return n;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool parse_htx_header(const uint8_t *p, size_t file_bytes, HtxHeader &out, std::string &why) {
    if (file_bytes < kHtxHeaderBytes || memcmp(p, "EODHTX1\0", 8) != 0) {
        why = "not an EODHTX1 file";
        return false;
    }
    out.width = rd16(p + 8);
    out.height = rd16(p + 10);
    out.levels = rd16(p + 12);
    out.flags = rd16(p + 14);
    if (!out.width || !out.height || out.width > 4096 || out.height > 4096) {
        why = "dimensions outside 1..4096";
        return false;
    }
    if (out.flags != 0) {
        why = "unsupported flags";
        return false;
    }
    if (out.levels != full_chain_levels(out.width, out.height)) {
        why = "levels " + std::to_string(out.levels) + " do not cover the full chain (" +
              std::to_string(full_chain_levels(out.width, out.height)) + ")";
        return false;
    }
    size_t need = htx_total_bytes(out.width, out.height, out.levels);
    if (file_bytes != need) {
        why = "file has " + std::to_string(file_bytes) + " bytes, header needs " + std::to_string(need);
        return false;
    }
    return true;
}

void convert_texels(const uint8_t *src, uint8_t *dst, size_t n, bool gs_domain, uint32_t &alpha_lo,
                    uint32_t &alpha_hi) {
    uint8_t lo = (uint8_t)std::min<uint32_t>(alpha_lo, 255), hi = (uint8_t)std::min<uint32_t>(alpha_hi, 255);
    if (!gs_domain) {
        memcpy(dst, src, n * 4);
        for (size_t i = 0; i < n; i++) {
            uint8_t a = src[i * 4 + 3];
            lo = std::min(lo, a);
            hi = std::max(hi, a);
        }
    } else {
        uint8_t lut[256];
        for (int a = 0; a < 256; a++) lut[a] = to_gs_alpha((uint8_t)a);
        for (size_t i = 0; i < n; i++) {
            uint8_t a = lut[src[i * 4 + 3]];
            dst[i * 4 + 0] = src[i * 4 + 0];
            dst[i * 4 + 1] = src[i * 4 + 1];
            dst[i * 4 + 2] = src[i * 4 + 2];
            dst[i * 4 + 3] = a;
            lo = std::min(lo, a);
            hi = std::max(hi, a);
        }
    }
    alpha_lo = lo;
    alpha_hi = hi;
}

std::string hex64(uint64_t v) {
    char b[17];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}

bool parse_hex64(const std::string &s, uint64_t &out) {
    if (s.size() != 16) return false;
    uint64_t v = 0;
    for (char c : s) {
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
        if (d < 0) return false;  // contract: lowercase hex
        v = v << 4 | (uint64_t)d;
    }
    out = v;
    return true;
}

static bool pow2(uint32_t v) { return v && !(v & (v - 1)); }

static bool safe_relative(const std::string &p) {
    if (p.empty() || p[0] == '/' || p[0] == '\\' || p.find(':') != std::string::npos) return false;
    size_t pos = 0;
    while (pos <= p.size()) {
        size_t s = p.find_first_of("/\\", pos);
        std::string part = p.substr(pos, s == std::string::npos ? std::string::npos : s - pos);
        if (part == "..") return false;
        if (s == std::string::npos) break;
        pos = s + 1;
    }
    return true;
}

Store::~Store() {
    wait_prefetch();
    close_all();
}

void Store::close_all() {
    for (Entry &e : entries_) {
        if (!e.data) continue;
#ifdef _WIN32
        delete[] e.data;
#else
        if (e.mapped)
            munmap((void *)e.data, e.bytes);
        else
            delete[] e.data;
#endif
        e.data = nullptr;
    }
    entries_.clear();
    dims_.clear();
}

bool Store::open_file(Entry &e, std::string &why) {
#ifndef _WIN32
    int fd = ::open(e.path.c_str(), O_RDONLY);
    if (fd < 0) {
        why = "file not found";
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)kHtxHeaderBytes) {
        ::close(fd);
        why = "file too small";
        return false;
    }
    void *m = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) {
        why = "mmap failed";
        return false;
    }
    e.data = static_cast<const uint8_t *>(m);
    e.bytes = (size_t)st.st_size;
    e.mapped = true;
#else
    std::ifstream f(e.path, std::ios::binary | std::ios::ate);
    if (!f) {
        why = "file not found";
        return false;
    }
    size_t n = (size_t)f.tellg();
    f.seekg(0);
    uint8_t *buf = new uint8_t[n ? n : 1];
    f.read(reinterpret_cast<char *>(buf), (std::streamsize)n);
    e.data = buf;
    e.bytes = n;
    e.mapped = false;
#endif
    HtxHeader h;
    if (!parse_htx_header(e.data, e.bytes, h, why)) return false;
    if (h.width % e.orig_w || h.height % e.orig_h || h.width / e.orig_w != h.height / e.orig_h ||
        !pow2(h.width / e.orig_w)) {
        why = std::to_string(h.width) + "x" + std::to_string(h.height) + " is not " + std::to_string(e.orig_w) +
              "x" + std::to_string(e.orig_h) + " times one power-of-two factor";
        return false;
    }
    e.w = h.width;
    e.h = h.height;
    e.levels = h.levels;
    e.scale = h.width / e.orig_w;
    return true;
}

size_t Store::load(const std::string &root, const LogFn &log, const FindFn &find) {
    wait_prefetch();
    close_all();
    std::string hosttex, manifest;
    auto resolve = [&](const std::string &dir, const std::string &rel, std::string &out) {
        if (find) return find(dir, rel, out);
        out = dir + "/" + rel;
#ifndef _WIN32
        struct stat st;
        return stat(out.c_str(), &st) == 0;
#else
        return true;
#endif
    };
    if (!resolve(root, "HOSTTEX", hosttex)) {
        log("host textures: no HOSTTEX directory in " + root);
        return 0;
    }
    if (!resolve(hosttex, "replacements.tsv", manifest)) {
        log("host textures: no manifest " + hosttex + "/replacements.tsv");
        return 0;
    }
    std::ifstream f(manifest, std::ios::binary);
    if (!f) {
        log("host textures: no manifest " + manifest);
        return 0;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return parse(ss.str(), hosttex, log, find);
}

size_t Store::parse(const std::string &text, const std::string &hosttex_dir, const LogFn &log, const FindFn &find) {
    std::istringstream in(text);
    std::string line;
    int n = 0;
    bool header = false;
    while (std::getline(in, line)) {
        n++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (n == 1) {
            if (line.size() >= 3 && (uint8_t)line[0] == 0xEF && (uint8_t)line[1] == 0xBB && (uint8_t)line[2] == 0xBF)
                line.erase(0, 3);
            if (line != "eod-host-textures 1") {
                log("host textures: manifest header is not 'eod-host-textures 1'; nothing loaded");
                return 0;
            }
            header = true;
            continue;
        }
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> col;
        size_t pos = 0;
        while (true) {
            size_t t = line.find('\t', pos);
            col.push_back(line.substr(pos, t == std::string::npos ? std::string::npos : t - pos));
            if (t == std::string::npos) break;
            pos = t + 1;
        }
        auto skip = [&](const std::string &why) {
            log("host textures: line " + std::to_string(n) + " skipped: " + why);
        };
        if (col.size() != 7) {
            skip("expected 7 tab-separated fields, found " + std::to_string(col.size()));
            continue;
        }
        Entry e;
        e.line = n;
        if (!parse_hex64(col[0], e.index_key)) {
            skip("indexKey is not 16 lowercase hex digits");
            continue;
        }
        if (col[1] == "*")
            e.any_palette = true;
        else if (!parse_hex64(col[1], e.palette_key)) {
            skip("paletteKey is not 16 lowercase hex digits or '*'");
            continue;
        }
        char *end = nullptr;
        unsigned long ow = strtoul(col[2].c_str(), &end, 10);
        bool okw = end && !*end && !col[2].empty();
        unsigned long oh = strtoul(col[3].c_str(), &end, 10);
        bool okh = end && !*end && !col[3].empty();
        if (!okw || !okh || ow == 0 || oh == 0 || ow > 1024 || oh > 1024) {
            skip("origWidth/origHeight must be 1..1024");
            continue;
        }
        e.orig_w = (uint32_t)ow;
        e.orig_h = (uint32_t)oh;
        e.file = col[4];
        e.asset_id = col[5];
        e.package_id = col[6];
        if (!safe_relative(e.file)) {
            skip("file must be a relative path inside HOSTTEX: " + e.file);
            continue;
        }
        if (find) {
            if (!find(hosttex_dir, e.file, e.path)) {
                skip(e.asset_id + " (" + e.package_id + "): file not found: " + e.file + "; native texture kept");
                continue;
            }
        } else {
            e.path = hosttex_dir + "/" + e.file;
        }
        std::string why;
        if (!open_file(e, why)) {
            skip(e.asset_id + " (" + e.package_id + "): " + e.file + ": " + why + "; native texture kept");
            if (e.data) {
#ifndef _WIN32
                if (e.mapped) munmap((void *)e.data, e.bytes);
                else
#endif
                    delete[] e.data;
            }
            continue;
        }
        for (const Entry &o : entries_)
            if (o.index_key == e.index_key && o.any_palette == e.any_palette && o.palette_key == e.palette_key)
                log("host textures: line " + std::to_string(n) + " " + e.asset_id + " has the same keys as line " +
                    std::to_string(o.line) + " " + o.asset_id + " (ambiguous; the earlier line is used)");
        char msg[512];
        snprintf(msg, sizeof msg, "host textures: %s (%s) %ux%u -> %ux%u x%u, %u levels, %zu bytes, keys %s %s",
                 e.asset_id.c_str(), e.package_id.c_str(), e.orig_w, e.orig_h, e.w, e.h, e.scale, e.levels, e.bytes,
                 col[0].c_str(), col[1].c_str());
        log(msg);
        uint64_t d = (uint64_t)e.orig_w << 32 | e.orig_h;
        if (std::find(dims_.begin(), dims_.end(), d) == dims_.end()) dims_.push_back(d);
        entries_.push_back(std::move(e));
    }
    if (!header) log("host textures: empty manifest");
    return entries_.size();
}

bool Store::has_dims(uint32_t w, uint32_t h) const {
    uint64_t d = (uint64_t)w << 32 | h;
    return std::find(dims_.begin(), dims_.end(), d) != dims_.end();
}

int Store::find(uint64_t ik, uint64_t pk, uint32_t w, uint32_t h, std::string &why) const {
    int wildcard = -1;
    bool index_match = false;
    for (size_t i = 0; i < entries_.size(); i++) {
        const Entry &e = entries_[i];
        if (e.index_key != ik || e.orig_w != w || e.orig_h != h) continue;
        index_match = true;
        if (!e.any_palette && e.palette_key == pk) return (int)i;
        if (e.any_palette && wildcard < 0) wildcard = (int)i;
    }
    if (wildcard < 0 && index_match) why = "paletteKey differs";
    return wildcard;
}

void Store::prefetch(uint64_t cpu_budget, const LogFn &log) {
    wait_prefetch();
    // Mapped pages are read on demand anyway; this only warms them off the
    // GS thread. Entries beyond the budget stay mapped and fault on upload.
    std::vector<std::pair<const uint8_t *, size_t>> work;
    uint64_t total = 0;
    for (const Entry &e : entries_) {
        if (!e.mapped) {
            total += e.bytes;
            continue;
        }
        if (total + e.bytes > cpu_budget) break;
        total += e.bytes;
        work.push_back({e.data, e.bytes});
    }
    prefetched_ = 0;
    auto *t = new std::thread([this, work, log, cpu_budget] {
        auto t0 = std::chrono::steady_clock::now();
        uint64_t sum = 0, bytes = 0;
        for (auto &w : work) {
#ifndef _WIN32
            madvise((void *)w.first, w.second, MADV_WILLNEED);
#endif
            for (size_t o = 0; o < w.second; o += 4096) sum += w.first[o];
            bytes += w.second;
            prefetched_ = bytes;
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        prefetch_ms_ = ms;
        static volatile uint64_t sink;
        sink = sum;
        char msg[256];
        snprintf(msg, sizeof msg,
                 "host textures: %zu of %zu files resident in CPU memory, %.1f MB in %.1f ms (budget %llu MB)",
                 work.size(), entries_.size(), bytes / 1048576.0, ms, (unsigned long long)(cpu_budget >> 20));
        log(msg);
    });
    thread_ = t;
}

void Store::wait_prefetch() {
    if (!thread_) return;
    auto *t = static_cast<std::thread *>(thread_);
    if (t->joinable()) t->join();
    delete t;
    thread_ = nullptr;
}

}  // namespace hosttex
}  // namespace gs
