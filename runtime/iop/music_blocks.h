#pragma once
#include <array>
#include <cstdint>
// The two stream voices use the top 48 KiB of SPU RAM. Mark each ADPCM
// block as it is filled, because speech and songs can share a ring.
namespace music_blocks {
constexpr uint32_t first = 0x1F4000, end = 0x200000;
inline std::array<bool,(end-first)/16> song{};
inline void reset() { song.fill(false); }
inline void set(uint32_t addr, bool music) {
    if (addr >= first && addr < end) song[(addr-first)/16] = music;
}
// Any RAM writer invalidates previous ownership before stream.cpp retags its blocks.
inline void clear(uint32_t addr, uint32_t bytes) {
    if (!bytes) return;
    // SPU RAM writes wrap at 2 MiB. Clear every partially overwritten block.
    if (bytes >= end) { reset(); return; }
    addr &= end - 1;
    auto range = [](uint32_t lo, uint32_t hi) {
        if (lo < first) lo = first;
        if (hi > end) hi = end;
        for (uint32_t block = lo & ~15u; block < hi; block += 16) set(block, false);
    };
    if (bytes <= end - addr) range(addr, addr + bytes);
    else { range(addr, end); range(0, bytes - (end - addr)); }
}
inline bool contains(uint32_t addr) {
    return addr >= first && addr < end && song[(addr-first)/16];
}
}
