// iso9660.h - read-only ISO 9660 image reader (no external tools).
//
// Enough of ECMA-119 for PS2 game discs: the primary volume descriptor, the
// directory tree (case-insensitive lookup, ";1" versions stripped,
// multi-extent files joined) and file data. Images with 2048-byte sectors
// (.iso) and raw 2352-byte sectors (Mode 1 / Mode 2 Form 1 .bin) are both
// accepted. Reads go through a callback, so tests can feed an in-memory image.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Iso9660 {
  public:
    // Reads n bytes at byte offset off of the image; false on a short read.
    using ReadFn = std::function<bool(uint64_t off, void *buf, size_t n)>;

    struct Extent {
        uint32_t lba;
        uint32_t size;
    };
    struct Entry {
        std::string name;  // as on the disc, without ";1" (e.g. "SLUS_209.10")
        bool is_dir = false;
        uint64_t size = 0;
        std::vector<Extent> extents;  // one, except for multi-extent files
    };

    Iso9660() = default;
    ~Iso9660();
    Iso9660(const Iso9660 &) = delete;
    Iso9660 &operator=(const Iso9660 &) = delete;

    // Opens an image file on disk. On failure err says why (for the user).
    bool open_file(const std::string &path, std::string *err);
    // Opens an image through a read callback of image_size bytes.
    bool open(ReadFn read, uint64_t image_size, std::string *err);

    const std::string &volume_id() const { return volume_id_; }
    uint32_t sector_size() const { return raw_sector_; }

    // Path lookup from the root, '/'-separated, case-insensitive ("" or "/"
    // is the root directory itself).
    bool find(const std::string &path, Entry &out);
    // Lists a directory entry's children (without "." and "..").
    bool list(const Entry &dir, std::vector<Entry> &out);
    bool list(const std::string &path, std::vector<Entry> &out);
    // Reads n bytes at offset off of a file.
    bool read(const Entry &file, uint64_t off, void *buf, size_t n);
    const Entry &root() const { return root_; }

  private:
    bool read_sectors(uint32_t lba, uint32_t count, uint8_t *out);
    bool parse_record(const uint8_t *r, size_t len, Entry &e, bool &multi);

    ReadFn read_;
    uint64_t image_size_ = 0;
    int fd_ = -1;
    uint32_t raw_sector_ = 2048;  // bytes per sector in the image
    uint32_t data_offset_ = 0;    // user data offset inside a raw sector
    uint32_t block_size_ = 2048;  // logical block size from the PVD
    std::string volume_id_;
    Entry root_;
};
