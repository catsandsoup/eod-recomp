// Exact physical GS pages touched by a pixel rectangle, including unaligned BP.
#pragma once
#include "../gs_mem.h"
#include "../gs_swizzle.h"
#include <array>
namespace gs::hw {
struct PageCoverage {
    std::array<uint32_t, kPages> touched{};
    std::array<uint32_t, kPages> complete{}; // every bit of these blocks overwritten
    std::array<uint16_t, kPages> pages; // sparse iteration, no allocation
    uint16_t count = 0;
};
inline PageCoverage page_coverage(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h) {
    PageCoverage result;
    if (w <= 0 || h <= 0) return result;
    int block_w = 8, block_h = 8, unit_bits = 32;
    const bool whole_storage = psm != PSMCT24 && psm != PSMZ24 && psm != PSMT8H && psm != PSMT4HL && psm != PSMT4HH;
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
        block_w = 16; block_h = 8; unit_bits = 16; break;
    case PSMT8: block_w = 16; block_h = 16; unit_bits = 8; break;
    case PSMT4: block_w = 32; block_h = 16; unit_bits = 4; break;
    }
    const SwizzleTab *tab = swizzle_table(psm, bw);
    int x0 = x & ~(block_w - 1), y0 = y & ~(block_h - 1);
    for (int py = y0; py < y + h; py += block_h)
        for (int px = x0; px < x + w; px += block_w) {
            uint32_t address = uint32_t(uint64_t(tab->addr(bp, px, py)) * unit_bits / 8) % kVramSize;
            uint32_t page = address / 8192, mask = 1u << ((address / 256) % 32);
            if (!result.touched[page])
                result.pages[result.count++] = uint16_t(page);
            result.touched[page] |= mask;
            if (whole_storage && px >= x && py >= y && px + block_w <= x + w && py + block_h <= y + h)
                result.complete[page] |= mask;
        }
    return result;
}
} // namespace gs::hw
