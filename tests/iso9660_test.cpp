// iso9660_test.cpp - unit tests for runtime/app/iso9660 and disc_import.
//
// Builds small ISO 9660 images in memory (no disc data involved): plain
// 2048-byte sectors and raw 2352-byte Mode 2 Form 1 sectors, with nested
// directories, a directory spanning two sectors, a multi-extent file and a
// name without extension. Then imports them into a temp folder the way the
// app's first-run flow does and checks every failure the player can hit.
//
//   iso9660_test            run the tests (exit 0 = pass)
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../runtime/app/disc_import.h"
#include "../runtime/app/iso9660.h"

namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

namespace {

void put32(uint8_t *p, uint32_t v) {  // both-endian 32-bit field
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
        p[7 - i] = (uint8_t)(v >> (8 * i));
    }
}
void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

std::vector<uint8_t> record(const std::string &name, uint32_t lba, uint32_t size, uint8_t flags) {
    size_t nlen = name.size();
    size_t len = 33 + nlen + (nlen % 2 == 0 ? 1 : 0);
    std::vector<uint8_t> r(len, 0);
    r[0] = (uint8_t)len;
    put32(&r[2], lba);
    put32(&r[10], size);
    r[25] = flags;
    put16(&r[28], 1);
    r[32] = (uint8_t)nlen;
    memcpy(&r[33], name.data(), nlen);
    return r;
}

struct Node {
    std::string name;  // on-disc spelling, files with ";1"
    bool dir = false;
    std::vector<uint8_t> data;
    std::vector<Node> kids;
    uint32_t lba = 0, size = 0;
    bool split = false;  // write as two extents (multi-extent file)
};

struct Builder {
    std::vector<uint8_t> img;
    uint32_t next = 18;

    uint32_t alloc(uint32_t bytes) {
        uint32_t lba = next;
        next += (bytes + 2047) / 2048 ? (bytes + 2047) / 2048 : 1;
        if (img.size() < (size_t)next * 2048) img.resize((size_t)next * 2048, 0);
        return lba;
    }

    // Lays out directories (records packed, never crossing a sector).
    std::vector<uint8_t> dir_bytes(const Node &d, uint32_t self_lba, uint32_t self_size, uint32_t parent_lba,
                                   uint32_t parent_size) {
        std::vector<std::vector<uint8_t>> recs;
        recs.push_back(record(std::string(1, '\0'), self_lba, self_size, 2));
        recs.push_back(record(std::string(1, '\1'), parent_lba, parent_size, 2));
        for (const Node &k : d.kids) {
            if (k.split) {
                uint32_t half = 2048;  // first extent: one full sector
                recs.push_back(record(k.name, k.lba, half, 0x80));
                recs.push_back(record(k.name, k.lba + 1, k.size - half, 0));
            } else {
                recs.push_back(record(k.name, k.lba, k.size, k.dir ? 2 : 0));
            }
        }
        std::vector<uint8_t> out;
        for (auto &r : recs) {
            size_t room = 2048 - out.size() % 2048;
            if (r.size() > room) out.resize(out.size() + room, 0);
            out.insert(out.end(), r.begin(), r.end());
        }
        out.resize((out.size() + 2047) / 2048 * 2048, 0);
        return out;
    }

    void place(Node &d) {  // assigns LBAs depth first
        for (Node &k : d.kids) {
            if (k.dir) {
                k.size = 2048 * (uint32_t)((k.kids.size() * 48 + 200) / 2048 + 1);
                k.lba = alloc(k.size);
                place(k);
            } else {
                k.size = (uint32_t)k.data.size();
                k.lba = alloc(k.size);
                memcpy(&img[(size_t)k.lba * 2048], k.data.data(), k.data.size());
            }
        }
    }

    void write_dirs(Node &d, uint32_t parent_lba, uint32_t parent_size) {
        std::vector<uint8_t> b = dir_bytes(d, d.lba, d.size, parent_lba, parent_size);
        if (b.size() > d.size) {
            fprintf(stderr, "test builder: directory too small\n");
            exit(2);
        }
        memcpy(&img[(size_t)d.lba * 2048], b.data(), b.size());
        for (Node &k : d.kids)
            if (k.dir) write_dirs(k, d.lba, d.size);
    }

    std::vector<uint8_t> build(Node &root) {
        img.assign(18 * 2048, 0);
        root.dir = true;
        root.size = 2048 * (uint32_t)((root.kids.size() * 48 + 200) / 2048 + 1);
        root.lba = alloc(root.size);
        place(root);
        write_dirs(root, root.lba, root.size);
        uint8_t *pvd = &img[16 * 2048];
        pvd[0] = 1;
        memcpy(pvd + 1, "CD001", 5);
        pvd[6] = 1;
        memset(pvd + 8, ' ', 64);
        memcpy(pvd + 40, "SLUS_20910", 10);
        put32(pvd + 80, next);
        put16(pvd + 128, 2048);
        std::vector<uint8_t> rr = record(std::string(1, '\0'), root.lba, root.size, 2);
        memcpy(pvd + 156, rr.data(), 34);
        uint8_t *term = &img[17 * 2048];
        term[0] = 255;
        memcpy(term + 1, "CD001", 5);
        return img;
    }
};

// 2048-byte sectors -> raw Mode 2 Form 1 sectors (sync, header, subheader).
std::vector<uint8_t> to_raw(const std::vector<uint8_t> &img) {
    size_t n = img.size() / 2048;
    std::vector<uint8_t> raw(n * 2352, 0);
    for (size_t i = 0; i < n; i++) {
        uint8_t *s = &raw[i * 2352];
        s[0] = 0;
        memset(s + 1, 0xFF, 10);
        s[11] = 0;
        s[15] = 2;
        memcpy(s + 24, &img[i * 2048], 2048);
    }
    return raw;
}

std::vector<uint8_t> pattern(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        v[i] = (uint8_t)(seed >> 16);
    }
    return v;
}

Node file(const std::string &name, std::vector<uint8_t> data) {
    Node n;
    n.name = name;
    n.data = std::move(data);
    return n;
}

Node make_tree(bool with_data, const std::vector<uint8_t> &elf) {
    Node root;
    root.kids.push_back(file("SLUS_209.10;1", elf));
    root.kids.push_back(file("SYSTEM.CNF;1", std::vector<uint8_t>((const uint8_t *)"BOOT2 = cdrom0:\\SLUS_209.10;1\r\n",
                                                                  (const uint8_t *)"BOOT2 = cdrom0:\\SLUS_209.10;1\r\n" + 31)));
    root.kids.push_back(file("README.;1", pattern(10, 1)));
    if (with_data) {
        Node data;
        data.name = "DATA";
        data.dir = true;
        data.kids.push_back(file("A.BIN;1", pattern(5000, 2)));
        Node big = file("BIG.DAT;1", pattern(2048 + 777, 3));
        big.split = true;
        data.kids.push_back(big);
        for (int i = 0; i < 60; i++) {  // pushes DATA's directory into a second sector
            char nm[32];
            snprintf(nm, sizeof nm, "F%02d.TXT;1", i);
            data.kids.push_back(file(nm, pattern((size_t)i * 37, 100 + i)));
        }
        Node sub;
        sub.name = "SUB";
        sub.dir = true;
        sub.kids.push_back(file("B.BIN;1", pattern(9000, 4)));
        data.kids.push_back(sub);
        root.kids.push_back(data);
    }
    return root;
}

Iso9660::ReadFn mem_reader(const std::vector<uint8_t> &img) {
    return [&img](uint64_t off, void *buf, size_t n) {
        if (off + n > img.size()) return false;
        memcpy(buf, &img[off], n);
        return true;
    };
}

std::vector<uint8_t> read_all(Iso9660 &iso, const std::string &path) {
    Iso9660::Entry e;
    if (!iso.find(path, e) || e.is_dir) return {};
    std::vector<uint8_t> v(e.size);
    if (!iso.read(e, 0, v.data(), v.size())) return {};
    return v;
}

std::vector<uint8_t> slurp(const std::string &path) {
    std::vector<uint8_t> v;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return v;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n);
    fclose(f);
    return v;
}

void spit(const std::string &path, const std::vector<uint8_t> &v) {
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(v.data(), 1, v.size(), f);
    fclose(f);
}

void test_reader(const std::vector<uint8_t> &img, uint32_t sector, const std::vector<uint8_t> &elf) {
    Iso9660 iso;
    std::string err;
    CHECK(iso.open(mem_reader(img), img.size(), &err));
    CHECK(iso.sector_size() == sector);
    CHECK(iso.volume_id() == "SLUS_20910");
    std::vector<Iso9660::Entry> root;
    CHECK(iso.list("/", root));
    CHECK(root.size() == 4);
    CHECK(read_all(iso, "SLUS_209.10") == elf);
    CHECK(read_all(iso, "slus_209.10") == elf);  // case-insensitive
    CHECK(read_all(iso, "README") == pattern(10, 1));
    CHECK(read_all(iso, "DATA/A.BIN") == pattern(5000, 2));
    CHECK(read_all(iso, "/data/sub/b.bin") == pattern(9000, 4));
    CHECK(read_all(iso, "DATA/BIG.DAT") == pattern(2048 + 777, 3));  // multi-extent, joined
    CHECK(read_all(iso, "DATA/F59.TXT") == pattern(59 * 37, 159));    // second directory sector
    std::vector<Iso9660::Entry> data;
    CHECK(iso.list("DATA", data));
    CHECK(data.size() == 63);  // A, BIG (once), 60 x F, SUB
    Iso9660::Entry e;
    CHECK(!iso.find("DATA/NOPE.BIN", e));
    CHECK(iso.find("DATA/SUB", e) && e.is_dir);
    // partial read across the extent boundary
    CHECK(iso.find("DATA/BIG.DAT", e));
    uint8_t mid[100];
    CHECK(iso.read(e, 2000, mid, sizeof mid));
    std::vector<uint8_t> big = pattern(2048 + 777, 3);
    CHECK(!memcmp(mid, &big[2000], sizeof mid));
    CHECK(!iso.read(e, big.size() - 10, mid, 20));  // past the end
}

}  // namespace

int main() {
    std::vector<uint8_t> elf = pattern(70000, 42);
    DiscId id = {(uint32_t)elf.size(), disc_crc32(elf.data(), elf.size())};

    Node tree = make_tree(true, elf);
    std::vector<uint8_t> img = Builder().build(tree);
    std::vector<uint8_t> raw = to_raw(img);

    test_reader(img, 2048, elf);
    test_reader(raw, 2352, elf);

    // not an image
    {
        std::vector<uint8_t> junk = pattern(40 * 2048, 9);
        Iso9660 iso;
        std::string err;
        CHECK(!iso.open(mem_reader(junk), junk.size(), &err));
        CHECK(!err.empty());
    }

    // CRC helper matches the standard check value
    CHECK(disc_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);
    CHECK(disc_crc32((const uint8_t *)"6789", 4, disc_crc32((const uint8_t *)"12345", 5)) == 0xCBF43926u);

    // imports
    const char *tdir = getenv("TMPDIR");
    std::string tmpl = std::string(tdir && *tdir ? tdir : "/tmp") + "/eod_iso_test_XXXXXX";
    std::vector<char> tbuf(tmpl.begin(), tmpl.end());
    tbuf.push_back(0);
    std::string tmp = mkdtemp(tbuf.data());
    std::string iso_path = tmp + "/game.iso", bin_path = tmp + "/game.bin";
    spit(iso_path, img);
    spit(bin_path, raw);
    {
        FILE *f = fopen((tmp + "/game.cue").c_str(), "w");
        fprintf(f, "FILE \"game.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n");
        fclose(f);
    }

    std::string dest = tmp + "/support/disc";
    int calls = 0;
    uint64_t last_done = 0, last_total = 0;
    DiscResult r = disc_import(iso_path, dest, id, [&](uint64_t done, uint64_t total, const std::string &) {
        calls++;
        CHECK(done >= last_done);
        last_done = done;
        last_total = total;
        return true;
    });
    CHECK(r.ok());
    CHECK(calls > 1 && last_done == last_total);
    CHECK(slurp(dest + "/SLUS_209.10") == elf);
    CHECK(slurp(dest + "/DATA/SUB/B.BIN") == pattern(9000, 4));
    CHECK(slurp(dest + "/DATA/BIG.DAT") == pattern(2048 + 777, 3));
    CHECK(fs::exists(dest + "/SYSTEM.CNF"));
    CHECK(!fs::exists(dest + "/README"));  // only what the game needs
    CHECK(!fs::exists(dest + ".partial"));
    CHECK(disc_check_folder(dest, id).ok());

    // the raw .bin through its .cue, replacing the previous install
    r = disc_import(tmp + "/game.cue", dest, id);
    CHECK(r.ok());
    CHECK(slurp(dest + "/DATA/A.BIN") == pattern(5000, 2));

    // a folder source (the installed copy), and a folder holding one .iso
    std::string dest2 = tmp + "/support2/disc";
    fs::create_directory_symlink(dest + "/DATA", dest + "/DATA/loop");
    CHECK(disc_import(dest, dest2, id).ok());
    CHECK(!fs::exists(dest2 + "/DATA/loop"));
    CHECK(slurp(dest2 + "/DATA/F07.TXT") == pattern(7 * 37, 107));
    fs::create_directories(tmp + "/isodir");
    spit(tmp + "/isodir/Some Game.iso", img);
    CHECK(disc_import(tmp + "/isodir", tmp + "/support3/disc", id).ok());

    // failures the player can hit
    DiscId wrong = id;
    wrong.elf_crc32 ^= 1;
    r = disc_import(iso_path, tmp + "/w/disc", wrong);
    CHECK(r.status == DiscStatus::WrongVersion && !r.message.empty());
    CHECK(!fs::exists(tmp + "/w/disc"));
    CHECK(disc_check_folder(dest, wrong).status == DiscStatus::WrongVersion);

    Node nodata = make_tree(false, elf);
    spit(tmp + "/nodata.iso", Builder().build(nodata));
    r = disc_import(tmp + "/nodata.iso", tmp + "/n/disc", id);
    CHECK(r.status == DiscStatus::NoData);
    fs::create_directories(tmp + "/folder_nodata");
    spit(tmp + "/folder_nodata/slus_209.10", elf);
    CHECK(disc_import(tmp + "/folder_nodata", tmp + "/n2/disc", id).status == DiscStatus::NoData);
    CHECK(disc_check_folder(tmp + "/folder_nodata", id).status == DiscStatus::NoData);

    fs::create_directories(tmp + "/empty");
    CHECK(disc_import(tmp + "/empty", tmp + "/e/disc", id).status == DiscStatus::NoElf);
    spit(tmp + "/game.zip", pattern(100, 5));
    CHECK(disc_import(tmp + "/game.zip", tmp + "/z/disc", id).status == DiscStatus::NotDisc);
    spit(tmp + "/junk.iso", pattern(50000, 6));
    CHECK(disc_import(tmp + "/junk.iso", tmp + "/j/disc", id).status == DiscStatus::NotDisc);
    CHECK(disc_import(tmp + "/missing.iso", tmp + "/m/disc", id).status == DiscStatus::NotFound);
    CHECK(disc_check_folder(tmp + "/nothing", id).status == DiscStatus::NotFound);

    // cancelling keeps the previous install
    r = disc_import(iso_path, dest, id, [](uint64_t done, uint64_t, const std::string &) { return done == 0; });
    CHECK(r.status == DiscStatus::Cancelled);
    CHECK(!fs::exists(dest + ".partial"));
    CHECK(disc_check_folder(dest, id).ok());

    std::error_code ec;
    fs::remove_all(tmp, ec);
    if (g_failures) {
        fprintf(stderr, "iso9660_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("iso9660_test: all tests passed\n");
    return 0;
}
