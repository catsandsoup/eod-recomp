// ps2mc.h - reads PS2 memory card images (PCSX2 .ps2 files: 8 MB cards, with
// or without the 16 ECC bytes per page) and copies saves out of them.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Ps2McFile {
    std::string name;
    std::vector<uint8_t> data;
};

struct Ps2McSave {
    std::string dir;  // e.g. "BASLUS-20910DRIVEN"
    std::vector<Ps2McFile> files;
};

// Reads the card image at path and returns the top-level save folders whose
// names start with prefix (all of them if empty). False with a message in err
// if the file isn't a readable memory card image.
bool ps2mc_read_saves(const std::string &path, const std::string &prefix, std::vector<Ps2McSave> &out,
                      std::string &err);

// Writes the saves into dir (one folder each, existing files replaced).
// Returns the number written; err describes the first failure.
int ps2mc_write_saves(const std::string &dir, const std::vector<Ps2McSave> &saves, std::string &err);
