// disc_import.cpp - see disc_import.h.
#include "disc_import.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

#include "iso9660.h"

namespace fs = std::filesystem;

namespace {

constexpr size_t kChunk = 4 << 20;

std::string basename_of(const std::string &p) {
    std::string s = p;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    size_t k = s.find_last_of('/');
    return k == std::string::npos ? s : s.substr(k + 1);
}

std::string lower_ext(const std::string &p) {
    std::string b = basename_of(p);
    size_t dot = b.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string e = b.substr(dot + 1);
    for (char &c : e) c = (char)tolower((unsigned char)c);
    return e;
}

std::string curly(const std::string &p) { return "“" + basename_of(p) + "”"; }

DiscResult result(DiscStatus s, const std::string &msg) {
    DiscResult r;
    r.status = s;
    r.message = msg;
    return r;
}

std::string gb(uint64_t bytes) {
    char buf[32];
    snprintf(buf, sizeof buf, "%.1f GB", (double)bytes / 1e9);
    return buf;
}

struct SrcFile {
    std::string rel;  // path in the source, with the source's spelling
    uint64_t size;
};

// Files of the disc, from a folder or an image.
struct DepthGuard { int &depth; DepthGuard(int &d): depth(d) { ++depth; } ~DepthGuard(){ --depth; } };

class Source {
  public:
    virtual ~Source() = default;
    // Resolves a path case-insensitively. real gets the source's spelling.
    virtual bool find(const std::string &rel, bool &is_dir, uint64_t &size, std::string &real) = 0;
    virtual bool list_files(const std::string &dir, std::vector<SrcFile> &out) = 0;  // recursive
    // Streams a file to cb in chunks; false on a read error or when cb says stop.
    virtual bool read_file(const SrcFile &f, const std::function<bool(const uint8_t *, size_t)> &cb) = 0;
};

// Case-insensitive name of a child of dir (empty when absent).
std::string ci_child(const std::string &dir, const std::string &name) {
    DIR *d = opendir(dir.c_str());
    if (!d) return "";
    std::string hit;
    while (dirent *e = readdir(d)) {
        if (!strcasecmp(e->d_name, name.c_str())) {
            hit = e->d_name;
            if (hit == name) break;
        }
    }
    closedir(d);
    return hit;
}

class FolderSource : public Source {
  public:
    explicit FolderSource(std::string root) : root_(std::move(root)) {}

    bool find(const std::string &rel, bool &is_dir, uint64_t &size, std::string &real) override {
        std::string cur = root_, out;
        size_t pos = 0;
        while (pos < rel.size()) {
            size_t slash = rel.find('/', pos);
            std::string part = rel.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
            pos = slash == std::string::npos ? rel.size() : slash + 1;
            std::string r = ci_child(cur, part);
            if (r.empty()) return false;
            cur += "/" + r;
            out += (out.empty() ? "" : "/") + r;
        }
        struct stat st;
        if (stat(cur.c_str(), &st) != 0) return false;
        is_dir = S_ISDIR(st.st_mode);
        size = (uint64_t)st.st_size;
        real = out;
        return true;
    }

    bool list_files(const std::string &dir, std::vector<SrcFile> &out) override {
        if (depth_ >= 64) return false;
        DepthGuard guard(depth_);
        DIR *d = opendir((root_ + "/" + dir).c_str());
        if (!d) return false;
        std::vector<std::string> names;
        while (dirent *e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, ".DS_Store")) continue;
            names.push_back(e->d_name);
        }
        closedir(d);
        for (const std::string &n : names) {
            std::string rel = dir + "/" + n;
            struct stat st;
            if (lstat((root_ + "/" + rel).c_str(), &st) != 0) return false;
            if (S_ISDIR(st.st_mode)) {
                if (!list_files(rel, out)) return false;
            } else if (S_ISREG(st.st_mode)) {
                out.push_back({rel, (uint64_t)st.st_size});
            }
        }
        return true;
    }

    bool read_file(const SrcFile &f, const std::function<bool(const uint8_t *, size_t)> &cb) override {
        FILE *in = fopen((root_ + "/" + f.rel).c_str(), "rb");
        if (!in) return false;
        std::vector<uint8_t> buf(kChunk);
        uint64_t left = f.size;
        bool ok = true;
        while (left > 0) {
            size_t want = left < kChunk ? (size_t)left : kChunk;
            if (fread(buf.data(), 1, want, in) != want) {
                ok = false;
                break;
            }
            left -= want;
            if (!cb(buf.data(), want)) {
                ok = false;
                break;
            }
        }
        fclose(in);
        return ok;
    }

  private:
    int depth_ = 0;
    std::string root_;
};

class IsoSource : public Source {
  public:
    bool open(const std::string &path, std::string *err) { return iso_.open_file(path, err); }

    bool find(const std::string &rel, bool &is_dir, uint64_t &size, std::string &real) override {
        Iso9660::Entry e;
        if (!iso_.find(rel, e)) return false;
        is_dir = e.is_dir;
        size = e.size;
        // rebuild the disc's spelling component by component
        real.clear();
        Iso9660::Entry cur = iso_.root();
        size_t pos = 0;
        while (pos < rel.size()) {
            size_t slash = rel.find('/', pos);
            std::string part = rel.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
            pos = slash == std::string::npos ? rel.size() : slash + 1;
            std::vector<Iso9660::Entry> kids;
            iso_.list(cur, kids);
            for (auto &k : kids)
                if (!strcasecmp(k.name.c_str(), part.c_str())) {
                    cur = k;
                    real += (real.empty() ? "" : "/") + k.name;
                    break;
                }
        }
        return true;
    }

    bool list_files(const std::string &dir, std::vector<SrcFile> &out) override {
        if (depth_ >= 64) return false;
        DepthGuard guard(depth_);
        std::vector<Iso9660::Entry> kids;
        if (!iso_.list(dir, kids)) return false;
        for (auto &k : kids) {
            std::string rel = dir + "/" + k.name;
            if (k.is_dir) {
                if (!list_files(rel, out)) return false;
            } else {
                out.push_back({rel, k.size});
            }
        }
        return true;
    }

    bool read_file(const SrcFile &f, const std::function<bool(const uint8_t *, size_t)> &cb) override {
        Iso9660::Entry e;
        if (!iso_.find(f.rel, e)) return false;
        std::vector<uint8_t> buf(kChunk);
        uint64_t off = 0;
        while (off < e.size) {
            size_t want = e.size - off < kChunk ? (size_t)(e.size - off) : kChunk;
            if (!iso_.read(e, off, buf.data(), want)) return false;
            off += want;
            if (!cb(buf.data(), want)) return false;
        }
        return true;
    }

  private:
    int depth_ = 0;
    Iso9660 iso_;
};

// The .bin named by the first FILE line of a .cue sheet.
std::string bin_of_cue(const std::string &cue) {
    FILE *f = fopen(cue.c_str(), "r");
    if (!f) return "";
    char line[1024];
    std::string bin;
    while (fgets(line, sizeof line, f)) {
        const char *p = strstr(line, "FILE");
        if (!p) continue;
        const char *a = strchr(p, '"');
        const char *b = a ? strchr(a + 1, '"') : nullptr;
        if (a && b) bin = std::string(a + 1, b);
        break;
    }
    fclose(f);
    if (bin.empty()) return "";
    if (bin[0] == '/') return bin;
    size_t slash = cue.find_last_of('/');
    return (slash == std::string::npos ? std::string(".") : cue.substr(0, slash)) + "/" + bin;
}

DiscResult check_elf(Source &src, const DiscId &id, const std::string &source_path, SrcFile &elf) {
    bool is_dir = false;
    uint64_t size = 0;
    std::string real;
    if (!src.find("SLUS_209.10", is_dir, size, real) || is_dir)
        return result(DiscStatus::NoElf,
                      "SLUS_209.10 was not found in " + curly(source_path) +
                          ". This does not look like Test Drive: Eve of Destruction (North American release, "
                          "SLUS-20910). If you chose a folder, choose the disc's top folder: the one that "
                          "contains SLUS_209.10, SYSTEM.CNF and DATA.");
    elf = {real, size};
    uint32_t crc = 0;
    if (!src.read_file(elf, [&](const uint8_t *p, size_t n) {
            crc = disc_crc32(p, n, crc);
            return true;
        }))
        return result(DiscStatus::IoError, "Could not read SLUS_209.10 from " + curly(source_path) + ".");
    if (size != id.elf_size || crc != id.elf_crc32) {
        char buf[512];
        snprintf(buf, sizeof buf,
                 "This copy of the game is a different version (SLUS_209.10: %llu bytes, checksum %08X; "
                 "expected %u bytes, checksum %08X). Eve of Destruction for Mac only runs the North American "
                 "release (NTSC-U, SLUS-20910, version 1.00): the game code was translated from that exact file.",
                 (unsigned long long)size, crc, id.elf_size, id.elf_crc32);
        return result(DiscStatus::WrongVersion, buf);
    }
    return DiscResult();
}

DiscResult no_data() {
    return result(DiscStatus::NoData,
                  "The DATA folder is missing: SLUS_209.10 was found, but not the DATA folder next to it. "
                  "Copy the whole disc, including the DATA folder, and try again.");
}

}  // namespace

uint32_t disc_crc32(const uint8_t *p, size_t n, uint32_t crc) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

DiscResult disc_check_folder(const std::string &dir, const DiscId &id) {
    struct stat st;
    if (dir.empty() || stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return result(DiscStatus::NotFound, "No game disc has been set up yet.");
    FolderSource src(dir);
    SrcFile elf;
    DiscResult r = check_elf(src, id, dir, elf);
    if (!r.ok()) return r;
    bool is_dir = false;
    uint64_t size = 0;
    std::string real;
    if (!src.find("DATA", is_dir, size, real) || !is_dir) return no_data();
    return DiscResult();
}

DiscResult disc_import(const std::string &source0, const std::string &dest, const DiscId &id,
                       const DiscProgress &progress) {
    std::string source = source0;
    while (source.size() > 1 && source.back() == '/') source.pop_back();
    struct stat st;
    if (stat(source.c_str(), &st) != 0)
        return result(DiscStatus::NotFound, curly(source) + " could not be found.");

    std::unique_ptr<Source> src;
    std::string ext = lower_ext(source);
    if (S_ISDIR(st.st_mode)) {
        // A folder: the disc's files, or a folder holding a single disc image.
        std::string elf = ci_child(source, "SLUS_209.10");
        if (elf.empty()) {
            std::vector<std::string> images;
            if (DIR *d = opendir(source.c_str())) {
                while (dirent *e = readdir(d)) {
                    std::string x = lower_ext(e->d_name);
                    if (x == "iso" && e->d_name[0] != '.') images.push_back(source + "/" + e->d_name);
                }
                closedir(d);
            }
            if (images.size() == 1) return disc_import(images[0], dest, id, progress);
        }
        src.reset(new FolderSource(source));
    } else {
        if (ext == "zip" || ext == "7z" || ext == "rar" || ext == "gz")
            return result(DiscStatus::NotDisc, curly(source) +
                                                   " is a compressed archive. Open it in Finder to extract the "
                                                   "disc image, then choose the .iso file inside.");
        std::string image = source;
        if (ext == "cue") {
            image = bin_of_cue(source);
            if (image.empty() || stat(image.c_str(), &st) != 0)
                return result(DiscStatus::NotFound, "The .bin file named in " + curly(source) + " was not found.");
        }
        auto iso = std::make_unique<IsoSource>();
        std::string err;
        if (!iso->open(image, &err))
            return result(DiscStatus::NotDisc,
                          curly(image) + " is not a disc image the game can use. Choose the game's .iso file, or "
                                          "a folder containing SLUS_209.10, SYSTEM.CNF and DATA.\n(" +
                              err + ")");
        src = std::move(iso);
    }

    SrcFile elf;
    DiscResult r = check_elf(*src, id, source, elf);
    if (!r.ok()) return r;
    bool is_dir = false;
    uint64_t size = 0;
    std::string data_dir;
    if (!src->find("DATA", is_dir, size, data_dir) || !is_dir) return no_data();

    std::vector<SrcFile> files;
    files.push_back(elf);
    std::string cnf;
    if (src->find("SYSTEM.CNF", is_dir, size, cnf) && !is_dir) files.push_back({cnf, size});
    if (!src->list_files(data_dir, files))
        return result(DiscStatus::IoError, "Could not read the DATA folder of " + curly(source) + ".");
    uint64_t total = 0;
    for (const SrcFile &f : files) total += f.size;

    // room for the copy (and a little slack)
    fs::path dest_path(dest);
    std::error_code ec;
    fs::create_directories(dest_path.parent_path(), ec);
    struct statvfs vfs;
    if (statvfs(dest_path.parent_path().c_str(), &vfs) == 0) {
        uint64_t free_bytes = (uint64_t)vfs.f_bavail * vfs.f_frsize;
        if (free_bytes < total + (64u << 20))
            return result(DiscStatus::NoSpace, "Not enough free disk space: the game files need " + gb(total) +
                                                   ", and " + gb(free_bytes) + " is free.");
    }

    std::string pattern = dest + ".partial.XXXXXX";
    std::vector<char> temp_name(pattern.begin(), pattern.end()); temp_name.push_back(0);
    if (!mkdtemp(temp_name.data()))
        return result(DiscStatus::IoError, "Could not create import folder: " + std::string(strerror(errno)));
    std::string tmp = temp_name.data();
    auto fail = [&](DiscStatus s, const std::string &msg) {
        std::error_code e2;
        fs::remove_all(tmp, e2);
        return result(s, msg);
    };

    uint64_t done = 0;
    bool cancelled = false;
    if (progress && !progress(0, total, files[0].rel)) return fail(DiscStatus::Cancelled, "Cancelled.");
    for (const SrcFile &f : files) {
        fs::path out_path = fs::path(tmp) / f.rel;
        fs::create_directories(out_path.parent_path(), ec);
        FILE *out = fopen(out_path.c_str(), "wb");
        if (!out)
            return fail(DiscStatus::IoError, "Could not write " + out_path.string() + ": " + strerror(errno));
        bool write_ok = true;
        uint32_t crc = 0;
        bool ok = src->read_file(f, [&](const uint8_t *p, size_t n) {
            if (fwrite(p, 1, n, out) != n) {
                write_ok = false;
                return false;
            }
            if (&f == &files[0]) crc = disc_crc32(p, n, crc);
            done += n;
            if (progress && !progress(done, total, f.rel)) {
                cancelled = true;
                return false;
            }
            return true;
        });
        if (fclose(out) != 0) write_ok = false;
        if (cancelled) return fail(DiscStatus::Cancelled, "Cancelled.");
        if (!write_ok)
            return fail(DiscStatus::IoError, "Could not write " + out_path.string() + " (is the disk full?).");
        if (!ok) return fail(DiscStatus::IoError, "Could not read " + f.rel + " from " + curly(source) + ".");
        if (&f == &files[0] && crc != id.elf_crc32)
            return fail(DiscStatus::IoError, "SLUS_209.10 changed while it was being copied. Try again.");
    }

    // swap the new copy in
    std::string old = dest + ".old";
    fs::remove_all(old, ec);
    if (fs::exists(dest, ec)) {
        fs::rename(dest, old, ec);
        if (ec) return fail(DiscStatus::IoError, "Could not replace " + dest + ": " + ec.message());
    }
    fs::rename(tmp, dest, ec);
    if (ec) {
        std::string message = ec.message();
        std::error_code restore;
        if (fs::exists(old)) fs::rename(old, dest, restore);
        return fail(DiscStatus::IoError, "Could not move the game files into " + dest + ": " + message);
    }
    fs::remove_all(old, ec);
    return DiscResult();
}
