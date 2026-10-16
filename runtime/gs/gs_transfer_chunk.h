// gs_transfer_chunk.h - the part of a host->local transfer rectangle that one
// chunk of IMAGE data fills (gs.cpp, xfer_data).
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace gs {

// Whole pixels in `bytes` more bytes of data at `bits_per_pixel`; for 24-bit
// formats `pending_bytes` of an incomplete pixel are left from the last chunk.
inline uint64_t transfer_pixels_supplied(unsigned bits_per_pixel, uint32_t bytes, unsigned pending_bytes) {
    if (!bits_per_pixel) return 0;
    uint64_t bits = uint64_t(bytes) * 8 + (bits_per_pixel == 24 ? uint64_t(pending_bytes) * 8 : 0);
    return bits / bits_per_pixel;
}

struct TransferWriteRect {
    int x, y, w, h;
};
struct TransferWriteRegions {
    std::array<TransferWriteRect, 3> rects{};
    unsigned count = 0;
};

// The pixels written next, from the current position (cursor_x, cursor_y) in a
// width x height transfer at (x0, y0): the rest of the current row, whole rows,
// then the start of a row - at most three rectangles. Clipped to the transfer.
inline TransferWriteRegions transfer_write_regions(int x0, int y0, int width, int height, int cursor_x,
                                                   int cursor_y, uint64_t pixels) {
    TransferWriteRegions out;
    if (width <= 0 || height <= 0 || cursor_x < 0 || cursor_x >= width || cursor_y < 0 || cursor_y >= height)
        return out;
    pixels = std::min(pixels, uint64_t(height - cursor_y) * width - cursor_x);
    if (cursor_x && pixels) {
        int head = int(std::min(pixels, uint64_t(width - cursor_x)));
        out.rects[out.count++] = {x0 + cursor_x, y0 + cursor_y, head, 1};
        pixels -= head;
        if (!pixels) return out;
        ++cursor_y;
    }
    if (pixels >= unsigned(width)) {
        int rows = int(pixels / width);
        out.rects[out.count++] = {x0, y0 + cursor_y, width, rows};
        pixels %= width;
        cursor_y += rows;
    }
    if (pixels) out.rects[out.count++] = {x0, y0 + cursor_y, int(pixels), 1};
    return out;
}

}  // namespace gs
