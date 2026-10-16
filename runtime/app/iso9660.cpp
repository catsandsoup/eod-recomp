// iso9660.cpp - see iso9660.h.
#include "iso9660.h"

#include <cctype>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

bool ieq(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

std::string trim_right(const char *p, size_t n) {
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == 0)) n--;
    return std::string(p, n);
}

const uint8_t kSync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};

}  // namespace

Iso9660::~Iso9660() {
    if (fd_ >= 0) close(fd_);
}

bool Iso9660::open_file(const std::string &path, std::string *err) {
    if (fd_ >= 0) close(fd_);
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
        if (err) *err = "Cannot open " + path + ": " + strerror(errno);
        return false;
    }
    struct stat st;
    if (fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (err) *err = path + " is not a disc image file.";
        return false;
    }
    int fd = fd_;
    ReadFn rd = [fd](uint64_t off, void *buf, size_t n) {
        uint8_t *p = (uint8_t *)buf;
        while (n > 0) {
            ssize_t got = pread(fd, p, n, (off_t)off);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) return false;
            p += got;
            off += (uint64_t)got;
            n -= (size_t)got;
        }
        return true;
    };
    return open(rd, (uint64_t)st.st_size, err);
}

bool Iso9660::open(ReadFn read, uint64_t image_size, std::string *err) {
    read_ = std::move(read);
    image_size_ = image_size;
    auto fail = [&](const char *msg) {
        if (err) *err = msg;
        return false;
    };
    // Find the primary volume descriptor at sector 16: plain 2048-byte
    // sectors first, then raw 2352-byte CD sectors (sync + header + data).
    uint8_t pvd[2048];
    bool found = false;
    if (image_size_ >= 17 * 2048 && read_(16 * 2048, pvd, 2048) && pvd[0] == 1 && !memcmp(pvd + 1, "CD001", 5)) {
        raw_sector_ = 2048;
        data_offset_ = 0;
        found = true;
    } else if (image_size_ >= 17 * 2352) {
        uint8_t hdr[16];
        if (read_(16 * 2352, hdr, 16) && !memcmp(hdr, kSync, 12) && (hdr[15] == 1 || hdr[15] == 2)) {
            raw_sector_ = 2352;
            data_offset_ = hdr[15] == 1 ? 16 : 24;  // Mode 1, or Mode 2 Form 1 (subheader)
            found = read_sectors(16, 1, pvd) && pvd[0] == 1 && !memcmp(pvd + 1, "CD001", 5);
        }
    }
    if (!found) return fail("This file is not a disc image (no ISO 9660 file system found).");
    block_size_ = le16(pvd + 128);
    if (block_size_ != 2048) return fail("Unsupported disc image (logical block size is not 2048).");
    volume_id_ = trim_right((const char *)pvd + 40, 32);
    bool multi = false;
    if (!parse_record(pvd + 156, 34, root_, multi) || !root_.is_dir)
        return fail("The disc image's root directory is damaged.");
    root_.name = "";
    return true;
}

bool Iso9660::read_sectors(uint32_t lba, uint32_t count, uint8_t *out) {
    if (raw_sector_ == 2048) return read_((uint64_t)lba * 2048, out, (size_t)count * 2048);
    for (uint32_t i = 0; i < count; i++)
        if (!read_((uint64_t)(lba + i) * raw_sector_ + data_offset_, out + (size_t)i * 2048, 2048)) return false;
    return true;
}

bool Iso9660::parse_record(const uint8_t *r, size_t avail, Entry &e, bool &multi) {
    if (avail < 34 || r[0] < 34 || r[0] > avail) return false;
    uint8_t name_len = r[32];
    if (33u + name_len > r[0]) return false;
    e = Entry();
    uint8_t flags = r[25];
    e.is_dir = (flags & 2) != 0;
    multi = (flags & 0x80) != 0;
    e.extents.push_back({le32(r + 2), le32(r + 10)});
    e.size = le32(r + 10);
    const char *name = (const char *)r + 33;
    if (name_len == 1 && (name[0] == 0 || name[0] == 1)) {
        e.name = name[0] == 0 ? "." : "..";
        return true;
    }
    std::string n(name, name_len);
    size_t semi = n.find(';');
    if (semi != std::string::npos) n.resize(semi);
    if (!n.empty() && n.back() == '.') n.pop_back();  // "README." (no extension)
    if (n.empty() || n == "." || n == ".." || n.find_first_of("/\\") != std::string::npos) return false;
    e.name = n;
    return true;
}

bool Iso9660::list(const Entry &dir, std::vector<Entry> &out) {
    out.clear();
    if (!dir.is_dir || dir.extents.empty()) return false;
    uint32_t size = dir.extents[0].size;
    uint64_t sectors = ((uint64_t)size + 2047) / 2048;
    if (sectors == 0 || sectors > 65536) return false;
    std::vector<uint8_t> buf((size_t)sectors * 2048);
    if (!read_sectors(dir.extents[0].lba, sectors, buf.data())) return false;
    size_t pos = 0;
    bool pending_multi = false;
    while (pos < size) {
        size_t in_sector = 2048 - pos % 2048;
        const uint8_t *r = &buf[pos];
        if (r[0] == 0) {  // records never cross a sector: skip the padding
            pos += in_sector;
            continue;
        }
        Entry e;
        bool multi = false;
        if (!parse_record(r, in_sector, e, multi)) return false;
        pos += r[0];
        if (e.name == "." || e.name == "..") continue;
        if (pending_multi && !out.empty() && ieq(out.back().name, e.name)) {
            out.back().extents.push_back(e.extents[0]);
            out.back().size += e.size;
        } else {
            out.push_back(e);
        }
        pending_multi = multi;
    }
    return true;
}

bool Iso9660::find(const std::string &path, Entry &out) {
    Entry cur = root_;
    size_t pos = 0;
    while (pos < path.size()) {
        size_t slash = path.find_first_of("/\\", pos);
        std::string part = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        pos = slash == std::string::npos ? path.size() : slash + 1;
        if (part.empty() || part == ".") continue;
        std::vector<Entry> kids;
        if (!list(cur, kids)) return false;
        bool hit = false;
        for (Entry &k : kids) {
            std::string kn = k.name;
            if (ieq(kn, part)) {
                cur = k;
                hit = true;
                break;
            }
        }
        if (!hit) return false;
    }
    out = cur;
    return true;
}

bool Iso9660::list(const std::string &path, std::vector<Entry> &out) {
    Entry d;
    return find(path, d) && list(d, out);
}

bool Iso9660::read(const Entry &file, uint64_t off, void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf;
    if (off > file.size || n > file.size - off) return false;
    uint8_t sector[2048];
    for (const Extent &ex : file.extents) {
        if (n == 0) break;
        if (off >= ex.size) {
            off -= ex.size;
            continue;
        }
        uint64_t take = ex.size - off;
        if (take > n) take = n;
        if (raw_sector_ == 2048) {
            if (!read_((uint64_t)ex.lba * 2048 + off, p, (size_t)take)) return false;
        } else {
            uint64_t o = off, left = take;
            uint8_t *q = p;
            while (left > 0) {
                uint32_t lba = ex.lba + (uint32_t)(o / 2048), in = (uint32_t)(o % 2048);
                uint32_t chunk = 2048 - in;
                if (chunk > left) chunk = (uint32_t)left;
                if (!read_sectors(lba, 1, sector)) return false;
                memcpy(q, sector + in, chunk);
                q += chunk;
                o += chunk;
                left -= chunk;
            }
        }
        p += take;
        n -= (size_t)take;
        off = 0;
    }
    return n == 0;
}
