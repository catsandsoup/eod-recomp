// Native replay state layouts shared with procedural fixture generation.
// EODGSD01 stores these host ABI structures; changing their layout invalidates
// existing captures. Inactive transfer pointers are never dereferenced on load.
#pragma once
#include "gs_swizzle.h"
#include <cstdint>
namespace gs {
struct PathState {
    bool in_packet = false;
    uint64_t tag_lo = 0, tag_hi = 0;
    uint32_t nloop = 0; // remaining loops
    uint32_t nreg = 0;
    uint32_t reg = 0; // current register index within the loop
    uint32_t flg = 0;
    bool eop = false;
};

// host -> local transfer state
struct Transfer {
    bool active = false;
    uint32_t dbp, dbw, dpsm;
    int x0, y0, w, h;
    int x, y;
    // partial pixel bytes for 24-bit and sub-word formats
    uint8_t pend[16];
    int pend_n = 0;
    // address table and the current row
    const SwizzleTab *T = nullptr;
    int ry = -1;
    uint32_t rb = 0, rc = 0;
    // 32-bit: within a block row, pixels 2m and 2m+1 are adjacent words (pairs stored at once);
    // 16, 8 and 4-bit: even pixels and the next one are adjacent in memory
    bool pairs = false;
    // 32-bit: the pairs of rows 2r and 2r+1 at the same x make aligned 16-byte groups
    bool quads = false;
};
// The capture format stores Transfer raw: the two flags live in former tail padding.
static_assert(sizeof(void *) != 8 || sizeof(Transfer) == 88, "GS capture layout changed");

} // namespace gs
