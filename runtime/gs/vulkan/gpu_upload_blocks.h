#pragma once
#include "../gs_mem.h"
#include "../gs_swizzle.h"
#include <cstdint>
#include <utility>
#include <vector>
namespace gs::hw {
struct UploadBlockRect { int x, y, w, h; };
// complete_mask is the caller's proof that actual CPU writes replaced every
// storage bit of each selected 256-byte block. This maps those blocks back to
// owner pixels; it does not infer completeness from the owner's pixel format.
// In particular, a 24-bit owner still occupies 32-bit physical storage slots.
// Success replaces out; failure leaves out unchanged.
inline bool gpu_upload_blocks(uint32_t bp, uint32_t bw, uint32_t psm,
                              int width, int height, uint32_t page,
                              uint32_t complete_mask, std::vector<UploadBlockRect>& out) {
    int page_h = 32, block_w = 8, unit_bytes = 4;
    switch (psm) {
    case PSMCT32: case PSMCT24: case PSMZ32: case PSMZ24: break;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
        page_h = 64; block_w = 16; unit_bytes = 2; break;
    default: return false;
    }
    if ((bp & 31) || bw < 1 || bw > 32 || width <= 0 || height <= 0 ||
        width > 2048 || height > 2048 || uint32_t(width) > bw * 64 ||
        page >= kPages || !complete_mask)
        return false;
    const uint32_t page_rows = uint32_t((height + page_h - 1) / page_h);
    // A physical page must have at most one logical owner-page mapping.
    if (page_rows * bw > kPages) return false;
    const uint32_t logical_page = (page + kPages - ((bp >> 5) % kPages)) % kPages;
    const int x0 = int(logical_page % bw) * 64;
    const int y0 = int(logical_page / bw) * page_h;
    if (x0 >= width || y0 >= height) return false;

    const SwizzleTab* tab = swizzle_table(psm, bw);
    std::vector<UploadBlockRect> result;
    result.reserve(32);
    uint32_t mapped = 0, selected = 0;
    // Enumerate the entire candidate logical page, including clipped edges,
    // so duplicate physical mappings cannot silently satisfy a selected bit.
    // Existing GS addresses supply depth XOR and 16S block permutations.
    for (int y = y0; y < y0 + page_h; y += 8) {
        for (int x = x0; x < x0 + 64; x += block_w) {
            const uint32_t address = uint32_t(uint64_t(tab->addr(bp,x,y)) * unit_bytes % kVramSize);
            if (address / 8192 != page) return false;
            const uint32_t bit = 1u << ((address / 256) % 32);
            if (mapped & bit) return false;
            mapped |= bit;
            if (!(complete_mask & bit)) continue;
            if (x + block_w > width || y + 8 > height) return false;
            selected |= bit;
            result.push_back({x,y,block_w,8});
        }
    }
    if (selected != complete_mask) return false;
    out = std::move(result);
    return true;
}
} // namespace gs::hw
