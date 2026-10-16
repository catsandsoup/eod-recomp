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
};

} // namespace gs
