// ps2mc.cpp - PS2 memory card image reader (see ps2mc.h).
//
// Layout: a superblock in page 0 ("Sony PS2 Memory Card Format "), clusters
// of pages_per_cluster pages, a two-level FAT (the superblock lists the
// indirect FAT clusters; FAT entries have bit 31 set when the cluster is in
// use and 0x7FFFFFFF ends a chain) and 512-byte directory entries. File and
// directory clusters are numbered from alloc_offset.
#include "ps2mc.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <direct.h>
#define mkdir_host(p) _mkdir(p)
#else
#define mkdir_host(p) mkdir(p, 0755)
#endif

namespace {

uint16_t rd16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

struct Card {
    std::vector<uint8_t> img;
    uint32_t page_len = 0, ppc = 0, raw_page = 0, clusters = 0, alloc_off = 0, root = 0;
    uint32_t ifc[32] = {};
    uint32_t cl_size = 0;

    // Cluster n (absolute) or false if it lies outside the image.
    bool cluster(uint32_t n, std::vector<uint8_t> &out) const {
        out.resize(cl_size);
        for (uint32_t i = 0; i < ppc; i++) {
            size_t off = ((size_t)n * ppc + i) * raw_page;
            if (off + page_len > img.size()) return false;
            memcpy(out.data() + (size_t)i * page_len, img.data() + off, page_len);
        }
        return true;
    }

    // FAT entry of cluster n (relative to alloc_offset); 0 when unreadable.
    uint32_t fat(uint32_t n) const {
        uint32_t per = cl_size / 4;
        uint32_t fc = n / per, ind = fc / per;
        if (ind >= 32) return 0;
        std::vector<uint8_t> c;
        if (!cluster(ifc[ind], c)) return 0;
        uint32_t fat_cl = rd32(&c[(fc % per) * 4]);
        if (!cluster(fat_cl, c)) return 0;
        return rd32(&c[(n % per) * 4]);
    }

    // The bytes of a chain starting at relative cluster first (length bytes at most).
    bool read_chain(uint32_t first, uint32_t length, std::vector<uint8_t> &out) const {
        out.clear();
        std::vector<uint8_t> c;
        uint32_t n = first;
        for (uint32_t guard = 0; out.size() < length; guard++) {
            if (guard > clusters || !cluster(alloc_off + n, c)) return false;
            out.insert(out.end(), c.begin(), c.end());
            uint32_t e = fat(n);
            if (!(e & 0x80000000u) || (e & 0x7FFFFFFFu) == 0x7FFFFFFFu) break;
            n = e & 0x7FFFFFFFu;
        }
        if (out.size() < length) return false;
        out.resize(length);
        return true;
    }
};

struct DirEntry {
    uint16_t mode;
    uint32_t length, cluster;
    std::string name;
};

enum { DF_EXISTS = 0x8000, DF_DIRECTORY = 0x0020 };

bool read_dir(const Card &c, uint32_t first, uint32_t count, std::vector<DirEntry> &out) {
    std::vector<uint8_t> b;
    if (count > 4096 || !c.read_chain(first, count * 512, b)) return false;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = &b[(size_t)i * 512];
        DirEntry d;
        d.mode = rd16(e);
        d.length = rd32(e + 4);
        d.cluster = rd32(e + 0x10);
        char name[33] = {};
        memcpy(name, e + 0x40, 32);
        d.name = name;
        out.push_back(d);
    }
    return true;
}

bool safe_name(const std::string &n) {
    return !n.empty() && n != "." && n != ".." && n.find_first_of("/\\:*?\"<>|") == std::string::npos;
}

}  // namespace

bool ps2mc_read_saves(const std::string &path, const std::string &prefix, std::vector<Ps2McSave> &out,
                      std::string &err) {
    Card c;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        err = "can't open " + path;
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0x200 || size > (64 << 20)) {
        fclose(f);
        err = "not a PS2 memory card image (unexpected size)";
        return false;
    }
    c.img.resize((size_t)size);
    size_t got = fread(c.img.data(), 1, c.img.size(), f);
    fclose(f);
    if (got != c.img.size() || memcmp(c.img.data(), "Sony PS2 Memory Card Format ", 28) != 0) {
        err = "not a PS2 memory card image (PCSX2 .ps2 file expected)";
        return false;
    }
    const uint8_t *sb = c.img.data();
    c.page_len = rd16(sb + 0x28);
    c.ppc = rd16(sb + 0x2A);
    c.clusters = rd32(sb + 0x30);
    c.alloc_off = rd32(sb + 0x34);
    c.root = rd32(sb + 0x3C);
    for (int i = 0; i < 32; i++) c.ifc[i] = rd32(sb + 0x50 + i * 4);
    if (c.page_len != 512 || c.ppc == 0 || c.ppc > 16 || c.clusters == 0) {
        err = "unsupported memory card layout";
        return false;
    }
    c.cl_size = c.page_len * c.ppc;
    uint64_t pages = (uint64_t)c.clusters * c.ppc;
    c.raw_page = (uint64_t)size >= pages * (c.page_len + 16) ? c.page_len + 16 : c.page_len;  // ECC bytes or not

    // the first entry (".") holds the number of entries in the directory
    std::vector<DirEntry> head, root;
    if (!read_dir(c, c.root, 1, head) || !read_dir(c, c.root, head[0].length, root)) {
        err = "can't read the card's root directory";
        return false;
    }
    for (const DirEntry &d : root) {
        if (!(d.mode & DF_EXISTS) || !(d.mode & DF_DIRECTORY) || !safe_name(d.name)) continue;
        if (d.name.compare(0, prefix.size(), prefix) != 0) continue;
        std::vector<DirEntry> files;
        if (!read_dir(c, d.cluster, d.length, files)) {
            err = "can't read folder " + d.name;
            return false;
        }
        Ps2McSave s;
        s.dir = d.name;
        for (const DirEntry &e : files) {
            if (!(e.mode & DF_EXISTS) || (e.mode & DF_DIRECTORY) || !safe_name(e.name)) continue;
            Ps2McFile file;
            file.name = e.name;
            if (e.length && !c.read_chain(e.cluster, e.length, file.data)) {
                err = "can't read " + d.name + "/" + e.name;
                return false;
            }
            s.files.push_back(std::move(file));
        }
        out.push_back(std::move(s));
    }
    return true;
}

int ps2mc_write_saves(const std::string &dir, const std::vector<Ps2McSave> &saves, std::string &err) {
    int n = 0;
    mkdir_host(dir.c_str());
    for (const Ps2McSave &s : saves) {
        std::string d = dir + "/" + s.dir;
        mkdir_host(d.c_str());
        for (const Ps2McFile &file : s.files) {
            std::string p = d + "/" + file.name;
            FILE *f = fopen(p.c_str(), "wb");
            if (!f || fwrite(file.data.data(), 1, file.data.size(), f) != file.data.size()) {
                if (f) fclose(f);
                err = "can't write " + p;
                return n;
            }
            fclose(f);
        }
        n++;
    }
    return n;
}
