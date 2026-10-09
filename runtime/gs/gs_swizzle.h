// gs_swizzle.h - precomputed address tables for fast pixel addressing.
//
// For every pixel format the GS address of (x, y) decomposes into
//   ((bp + rowB[y] + colB[x]) << shift) + colT[rowC[y] + colC[x]]
// (page/block part separable in x and y, plus the in-block column table),
// in the format's natural unit (32-bit word, halfword, byte or nibble).
#pragma once
#include <cstdint>

namespace gs {

struct SwizzleTab {
    uint32_t psm, bw;
    int shift;
    const uint16_t *colT;
    int32_t rowB[2048];
    int32_t colB[2048];
    uint16_t rowC[2048];
    uint16_t colC[2048];

    inline uint32_t row_base(uint32_t bp, int y) const { return bp + (uint32_t)rowB[y & 2047]; }
    inline uint32_t addr_row(uint32_t rowbase, uint32_t rowc, int x) const {
        x &= 2047;
        return ((rowbase + (uint32_t)colB[x]) << shift) + colT[rowc + colC[x]];
    }
    inline uint32_t addr(uint32_t bp, int x, int y) const {
        return addr_row(row_base(bp, y), rowC[y & 2047], x);
    }
};

// Returns the (cached) table for a pixel format and buffer width.
const SwizzleTab *swizzle_table(uint32_t psm, uint32_t bw);

}  // namespace gs
