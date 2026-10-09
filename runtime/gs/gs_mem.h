// gs_mem.h - GS local memory (4MB) and the swizzled pixel layouts.
#pragma once
#include <cstdint>

namespace gs {

constexpr uint32_t kVramSize = 4 * 1024 * 1024;
extern uint8_t *vram;  // 4MB, accessed as 32-bit words / 16-bit / bytes / nibbles

enum PSM : uint32_t {
    PSMCT32 = 0x00,
    PSMCT24 = 0x01,
    PSMCT16 = 0x02,
    PSMCT16S = 0x0A,
    PSMT8 = 0x13,
    PSMT4 = 0x14,
    PSMT8H = 0x1B,
    PSMT4HL = 0x24,
    PSMT4HH = 0x2C,
    PSMZ32 = 0x30,
    PSMZ24 = 0x31,
    PSMZ16 = 0x32,
    PSMZ16S = 0x3A,
};

void mem_init();

// Address of pixel (x,y) in the unit natural for the format:
// 32-bit formats -> word index, 16-bit -> halfword index, 8-bit -> byte
// index, 4-bit -> nibble index.  bp = block pointer, bw = width / 64.
uint32_t addr32(int x, int y, uint32_t bp, uint32_t bw);
uint32_t addr32z(int x, int y, uint32_t bp, uint32_t bw);
uint32_t addr16(int x, int y, uint32_t bp, uint32_t bw);
uint32_t addr16s(int x, int y, uint32_t bp, uint32_t bw);
uint32_t addr16z(int x, int y, uint32_t bp, uint32_t bw);
uint32_t addr16sz(int x, int y, uint32_t bp, uint32_t bw);
uint32_t addr8(int x, int y, uint32_t bp, uint32_t bw);
uint32_t addr4(int x, int y, uint32_t bp, uint32_t bw);

// Raw access by format (values in the format's native width).
uint32_t read_pixel(uint32_t psm, int x, int y, uint32_t bp, uint32_t bw);
void write_pixel(uint32_t psm, int x, int y, uint32_t bp, uint32_t bw, uint32_t v);

inline uint32_t &word(uint32_t a) { return reinterpret_cast<uint32_t *>(vram)[a & 0xFFFFF]; }
inline uint16_t &half(uint32_t a) { return reinterpret_cast<uint16_t *>(vram)[a & 0x1FFFFF]; }
inline uint8_t &byte(uint32_t a) { return vram[a & 0x3FFFFF]; }
inline uint32_t nibble(uint32_t a) { return (vram[(a >> 1) & 0x3FFFFF] >> ((a & 1) * 4)) & 0xF; }
inline void set_nibble(uint32_t a, uint32_t v) {
    uint8_t &b = vram[(a >> 1) & 0x3FFFFF];
    b = (a & 1) ? (uint8_t)((b & 0x0F) | (v << 4)) : (uint8_t)((b & 0xF0) | (v & 0xF));
}

inline int bits_per_pixel(uint32_t psm) {
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: return 8;
    case PSMT4: return 4;
    case PSMCT24: case PSMZ24: return 24;
    default: return 32;
    }
}

}  // namespace gs
