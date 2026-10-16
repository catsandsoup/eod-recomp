// gs_transfer_chunk_test - transfer_write_regions covers exactly the pixels a
// chunk writes (5000 random transfers, positions and sizes), and
// transfer_pixels_supplied counts whole pixels (24-bit leftovers included).
#include "gs/gs_transfer_chunk.h"
#include <cstdio>
#include <random>
#include <set>
#include <utility>
int main() {
    std::mt19937 rng(0x4348554e);
    for (int i = 0; i < 5000; i++) {
        int width = 1 + rng() % 128, height = 1 + rng() % 64, x = rng() % width, y = rng() % height;
        unsigned pixels = rng() % (width * height + 100);
        auto result = gs::transfer_write_regions(7, 13, width, height, x, y, pixels);
        std::set<std::pair<int, int>> expected, actual;
        for (unsigned k = 0; k < pixels && y < height; k++) {
            expected.insert({7 + x, 13 + y});
            if (++x == width) {
                x = 0;
                ++y;
            }
        }
        for (unsigned r = 0; r < result.count; r++) {
            const auto &rect = result.rects[r];
            for (int py = rect.y; py < rect.y + rect.h; py++)
                for (int px = rect.x; px < rect.x + rect.w; px++)
                    if (!actual.insert({px, py}).second) return 2;  // overlapping rectangles
        }
        if (actual != expected) return 1;
    }
    if (gs::transfer_pixels_supplied(24, 1, 1) != 0 || gs::transfer_pixels_supplied(24, 1, 2) != 1 ||
        gs::transfer_pixels_supplied(24, 8, 2) != 3 || gs::transfer_pixels_supplied(32, 3, 0) != 0 ||
        gs::transfer_pixels_supplied(4, 16, 0) != 32)
        return 3;
    auto cut_short = gs::transfer_write_regions(0, 0, 64, 32, 0, 0, 4);
    if (cut_short.count != 1 || cut_short.rects[0].w != 4 || cut_short.rects[0].h != 1) return 4;
    puts("transfer chunks: passed");
}
