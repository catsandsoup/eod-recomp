// disc_import.h - checks and installs the player's own game disc files.
//
// The source is a disc image (.iso, raw .bin or its .cue) or a folder that
// holds the disc's files (a copied disc or a mounted volume). The files the
// port reads - SLUS_209.10, SYSTEM.CNF and DATA/ - are copied into a
// destination folder (the app's Application Support "disc" folder), after
// checking that SLUS_209.10 is the exact release the port was built from.
// Messages are written for players, not developers.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

struct DiscId {
    uint32_t elf_size;   // size and CRC-32 of SLUS_209.10 (gen/table.c)
    uint32_t elf_crc32;
};

enum class DiscStatus {
    Ok,
    NotFound,      // the source path does not exist
    NotDisc,       // not a disc image and not a folder with the game's files
    NoElf,         // no SLUS_209.10: a different game, or not the disc's top folder
    WrongVersion,  // SLUS_209.10 with another CRC: another region or revision
    NoData,        // SLUS_209.10 without the DATA folder
    NoSpace,       // not enough free disk space for the copy
    IoError,       // read or write failure
    Cancelled,
};

struct DiscResult {
    DiscStatus status = DiscStatus::Ok;
    std::string message;  // for the player; empty when Ok
    bool ok() const { return status == DiscStatus::Ok; }
};

uint32_t disc_crc32(const uint8_t *p, size_t n, uint32_t crc = 0);

// Checks an installed disc folder: SLUS_209.10 with the expected CRC and a
// DATA folder (case-insensitive names).
DiscResult disc_check_folder(const std::string &dir, const DiscId &id);

// done/total in bytes; file is the path being copied. Return false to cancel.
// Called from the importing thread.
using DiscProgress = std::function<bool(uint64_t done, uint64_t total, const std::string &file)>;

// Validates source, then copies SLUS_209.10, SYSTEM.CNF and DATA/ into dest
// (built in dest + ".partial" and swapped in at the end, so a failed or
// cancelled import leaves any previous install untouched).
DiscResult disc_import(const std::string &source, const std::string &dest, const DiscId &id,
                       const DiscProgress &progress = nullptr);
