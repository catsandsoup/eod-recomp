// disc_import.cpp - see disc_import.h.
#include "disc_import.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <vector>

#include "iso9660.h"

namespace fs = std::filesystem;

namespace {

constexpr size_t kChunk = 4 << 20;

std::string basename_of(const std::string &p) {
    std::string s = p;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    size_t k = s.find_last_of("/\\");
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

bool ci_equal(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

FILE *open_read(const std::string &path) {
#ifdef _WIN32
    return _wfopen(fs::u8path(path).c_str(), L"rb");
#else
    return fopen(path.c_str(), "rb");
#endif
}

// Lock the destination, rather than the UI: every importer entry point shares it.
// The lock file is deliberately retained; unlinking it would allow a second inode
// to be locked by another process while the first process still holds its lock.
class ImportLock {
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int handle_ = -1;
#endif
public:
    explicit ImportLock(const fs::path &path) {
#ifdef _WIN32
        handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
        handle_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
        if (handle_ >= 0 && flock(handle_, LOCK_EX | LOCK_NB) != 0) { close(handle_); handle_ = -1; }
#endif
    }
    bool acquired() const {
#ifdef _WIN32
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return handle_ >= 0;
#endif
    }
    ~ImportLock() {
#ifdef _WIN32
        if (acquired()) CloseHandle(handle_);
#else
        if (acquired()) close(handle_);
#endif
    }
};

std::string ci_child(const std::string &dir, const std::string &name) {
    std::error_code ec;
    std::string hit;
    for (fs::directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
        auto child = it->path().filename().u8string();
        if (ci_equal(child, name)) { hit = child; if (hit == name) break; }
    }
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
        std::error_code ec;
        auto path = fs::u8path(cur);
        is_dir = fs::is_directory(path, ec);
        if (ec) return false;
        size = is_dir ? 0 : fs::file_size(path, ec);
        if (ec) return false;
        real = out;
        return true;
    }

    bool list_files(const std::string &dir, std::vector<SrcFile> &out) override {
        if (depth_ >= 64) return false;
        DepthGuard guard(depth_);
        std::error_code ec;
        auto path = fs::u8path(root_) / fs::u8path(dir);
        for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
            const auto &entry = *it;
            auto name = entry.path().filename().u8string();
            if (name == ".DS_Store") continue;
            if (entry.is_symlink(ec)) continue;
            if (ec) return false;
            std::string rel = dir + "/" + name;
            if (entry.is_directory(ec)) {
                if (!list_files(rel, out)) return false;
            } else if (entry.is_regular_file(ec)) {
                auto size = entry.file_size(ec);
                if (ec) return false;
                out.push_back({rel, size});
            }
            if (ec) return false;
        }
        if (ec) return false;
        return true;
    }

    bool read_file(const SrcFile &f, const std::function<bool(const uint8_t *, size_t)> &cb) override {
        FILE *in = open_read(root_ + "/" + f.rel);
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
                if (ci_equal(k.name, part)) {
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
    FILE *f = open_read(cue);
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
    return (fs::u8path(cue).parent_path() / fs::u8path(bin)).u8string();
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
                 "expected %u bytes, checksum %08X). Eve of Destruction only runs the North American "
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
    static const auto table = [] {
        std::array<uint32_t, 256> table{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        return table;
    }();
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

DiscResult disc_recover_install(const std::string &dest) try {
    const auto path = fs::absolute(fs::u8path(dest));
    const auto old = fs::u8path(path.u8string() + ".old");
    if (fs::exists(path) || !fs::exists(old)) return {};
    ImportLock lock(fs::u8path(path.u8string() + ".import-lock"));
    if (!lock.acquired()) return result(DiscStatus::IoError, "Another copy may be installing the game disc. Try again when it finishes.");
    if (!fs::exists(path) && fs::exists(old)) fs::rename(old, path);
    return {};
} catch (const fs::filesystem_error &e) {
    return result(DiscStatus::IoError, "Could not restore the previous game installation: " + e.code().message());
}

DiscResult disc_check_folder(const std::string &dir, const DiscId &id) {
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(fs::u8path(dir), ec))
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
                       const DiscProgress &progress) try {
    std::string source = source0;
    while (source.size() > 1 && source.back() == '/') source.pop_back();
    if (!fs::exists(fs::u8path(source)))
        return result(DiscStatus::NotFound, curly(source) + " could not be found.");

    std::unique_ptr<Source> src;
    std::string ext = lower_ext(source);
    if (fs::is_directory(fs::u8path(source))) {
        // A folder: the disc's files, or a folder holding a single disc image.
        std::string elf = ci_child(source, "SLUS_209.10");
        if (elf.empty()) {
            std::vector<std::string> images;
            for (const auto &entry : fs::directory_iterator(fs::u8path(source))) {
                const auto name = entry.path().filename().u8string();
                if (!name.empty() && name[0] != '.' && lower_ext(name) == "iso" && entry.is_regular_file())
                    images.push_back(entry.path().u8string());
            }
            if (images.size() == 1) return disc_import(images[0], dest, id, progress);
        }
        src.reset(new FolderSource(source));
    } else {
        if (ext == "zip" || ext == "7z" || ext == "rar" || ext == "gz")
            return result(DiscStatus::NotDisc, curly(source) +
                                                   " is a compressed archive. Unzip it first to extract the "
                                                   "disc image, then choose the .iso file inside.");
        std::string image = source;
        if (ext == "cue") {
            image = bin_of_cue(source);
            if (image.empty() || !fs::exists(fs::u8path(image)))
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
    fs::path dest_path = fs::absolute(fs::u8path(dest));
    std::error_code ec;
    fs::create_directories(dest_path.parent_path(), ec);
    if (ec) return result(DiscStatus::IoError, "Could not create the installation folder: " + ec.message());
    ImportLock lock(fs::u8path(dest_path.u8string() + ".import-lock"));
    if (!lock.acquired()) return result(DiscStatus::IoError,
        "Could not open the installation folder for writing. Another copy may be installing the disc; close it and try again, or check folder permissions.");
    const fs::path old = fs::u8path(dest_path.u8string() + ".old");
    // Recover a prior process interrupted between the two final renames.
    if (!fs::exists(dest_path) && fs::exists(old)) fs::rename(old, dest_path);
    // A killed import can leave its uniquely named temporary copy. The lock
    // proves no writer is using these reserved paths, so reclaim it before the
    // free-space check rather than making the next attempt need twice the room.
    const std::string partial_prefix = dest_path.filename().u8string() + ".partial.";
    for (const auto &entry : fs::directory_iterator(dest_path.parent_path())) {
        if (entry.path().filename().u8string().compare(0, partial_prefix.size(), partial_prefix) == 0)
            fs::remove_all(entry.path());
    }
    const auto space = fs::space(dest_path.parent_path(), ec);
    if (!ec) {
        const uint64_t free_bytes = space.available;
        if (free_bytes < total + (64u << 20))
            return result(DiscStatus::NoSpace, "Not enough free disk space: the game files need " + gb(total) +
                                                   ", and " + gb(free_bytes) + " is free.");
    }

    fs::path tmp_path;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        tmp_path = fs::u8path(dest_path.u8string() + ".partial." + std::to_string(nonce) + "." + std::to_string(attempt));
        if (fs::create_directory(tmp_path, ec)) break;
        if (ec) return result(DiscStatus::IoError, "Could not create import folder: " + ec.message());
        tmp_path.clear();
    }
    if (tmp_path.empty()) return result(DiscStatus::IoError, "Could not create a temporary import folder. Try again.");
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code e; fs::remove_all(path, e); } } cleanup{tmp_path};
    auto fail = [&](DiscStatus s, const std::string &msg) {
        std::error_code e2;
        fs::remove_all(tmp_path, e2);
        return result(s, msg);
    };

    uint64_t done = 0;
    bool cancelled = false;
    if (progress && !progress(0, total, files[0].rel)) return fail(DiscStatus::Cancelled, "Cancelled.");
    for (const SrcFile &f : files) {
        fs::path out_path = tmp_path / fs::u8path(f.rel);
        fs::create_directories(out_path.parent_path(), ec);
#ifdef _WIN32
        FILE *out = _wfopen(out_path.c_str(), L"wb");
#else
        FILE *out = fopen(out_path.c_str(), "wb");
#endif
        if (!out)
            return fail(DiscStatus::IoError, "Could not write " + out_path.u8string() + ": " + strerror(errno));
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
            return fail(DiscStatus::IoError, "Could not write " + out_path.u8string() + " (is the disk full?).");
        if (!ok) return fail(DiscStatus::IoError, "Could not read " + f.rel + " from " + curly(source) + ".");
        if (&f == &files[0] && crc != id.elf_crc32)
            return fail(DiscStatus::IoError, "SLUS_209.10 changed while it was being copied. Try again.");
    }

    // swap the new copy in
    fs::remove_all(old, ec);
    if (ec) return fail(DiscStatus::IoError, "Could not remove the previous installation backup: " + ec.message());
    if (fs::exists(dest_path, ec)) {
        fs::rename(dest_path, old, ec);
        if (ec) return fail(DiscStatus::IoError, "Could not replace " + dest + ": " + ec.message());
    }
    fs::rename(tmp_path, dest_path, ec);
    if (ec) {
        std::string message = ec.message();
        std::error_code restore;
        if (fs::exists(old)) fs::rename(old, dest_path, restore);
        return fail(DiscStatus::IoError, "Could not move the game files into " + dest + ": " + message);
    }
    fs::remove_all(old, ec);
    return DiscResult();
} catch (const fs::filesystem_error &e) {
    return result(DiscStatus::IoError, "Could not access the selected disc or installation folder: " + e.code().message());
}
