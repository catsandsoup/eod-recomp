// Generates tiny GS command replays from public register definitions. No disc,
// game code, assets, or GPU context required. See test_gs_pixels.py.
#include "../../runtime/gs/gs_mem.h"
#include "../../runtime/gs/gs_replay_types.h"
#include "../../runtime/gs/gs_state.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Pair = std::pair<uint64_t, uint64_t>;
static_assert(sizeof(Pair) == 16, "GIF qword layout");
static uint64_t rgba(unsigned r, unsigned g, unsigned b, unsigned a = 128) {
    return r | uint64_t(g) << 8 | uint64_t(b) << 16 | uint64_t(a) << 24;
}
static uint64_t xyz(unsigned x, unsigned y, unsigned z = 1) { return x | uint64_t(y) << 16 | uint64_t(z) << 32; }
static void command(FILE *f, uint32_t kind, uint32_t arg, const void *data, uint32_t bytes) {
    uint32_t h[4] = {kind, arg, 0, bytes};
    std::fwrite(h, 1, sizeof h, f);
    if (bytes)
        std::fwrite(data, 1, bytes, f);
    const uint8_t pad[16] = {};
    if (bytes % 16)
        std::fwrite(pad, 1, 16 - bytes % 16, f);
}
int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: fixture CASE OUTPUT.gsd\n");
        return 2;
    }
    const std::string name = argv[1];
    gs::State state{};
    gs::PathState paths[4]{};
    gs::Transfer transfer{};
    state.prmodecont = 1;
    state.q = 1;
    state.colclamp = 1;
    state.ctx[0].frame = 1ull << 16;
    state.ctx[0].zbuf = 1ull << 32;
    state.ctx[0].scissor = 63ull << 16 | 63ull << 48;
    state.pmode = 1;
    state.dispfb[0] = 1ull << 9;
    state.display[0] = 63ull << 32 | 63ull << 44;
    state.texa = 128ull | 128ull << 32;
    constexpr unsigned textureBP = 128;
    state.ctx[0].tex0 =
        textureBP | 1ull << 14 | 1ull << 26 | 1ull << 30 | 1ull << 34 | 1ull << 35; // 2x2 CT32, DECAL/TCC
    state.ctx[0].tex1 = 1ull << 5;                                                  // linear magnification
    gs::mem_init();
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++)
            gs::write_pixel(gs::PSMCT32, x, y, 0, 1, rgba(32, 64, 96));
    gs::write_pixel(gs::PSMCT32, 0, 0, textureBP, 1, rgba(1, 21, 41));
    gs::write_pixel(gs::PSMCT32, 1, 0, textureBP, 1, rgba(4, 24, 44));
    gs::write_pixel(gs::PSMCT32, 0, 1, textureBP, 1, rgba(8, 28, 48));
    gs::write_pixel(gs::PSMCT32, 1, 1, textureBP, 1, rgba(12, 32, 52));
    std::vector<Pair> regs;
    auto reg = [&](uint64_t id, uint64_t value) { regs.emplace_back(value, id); };
    auto sprite = [&](unsigned prim, uint64_t color, unsigned uv) {
        reg(0, prim);
        reg(1, color);
        reg(3, uv | uint64_t(11) << 16);
        reg(5, xyz(8 * 16, 8 * 16));
        reg(3, uv | uint64_t(11) << 16);
        reg(5, xyz(40 * 16, 40 * 16));
    };
    if (name == "flat")
        sprite(6, rgba(37, 83, 126), 0);
    else if (name == "filter")
        sprite(6 | 1 << 4 | 1 << 8, rgba(128, 128, 128), 9);
    else if (name == "uvsnap") {
        gs::write_pixel(gs::PSMCT32, 0, 0, textureBP, 1, rgba(0, 0, 0));
        gs::write_pixel(gs::PSMCT32, 1, 0, textureBP, 1, rgba(255, 255, 255));
        gs::write_pixel(gs::PSMCT32, 0, 1, textureBP, 1, rgba(0, 0, 0));
        gs::write_pixel(gs::PSMCT32, 1, 1, textureBP, 1, rgba(255, 255, 255));
        reg(0, 6 | 1 << 4 | 1 << 8);
        reg(1, rgba(128, 128, 128));
        reg(3, 9 | 11ull << 16);
        reg(5, xyz(8 * 16, 8 * 16));
        reg(3, 31 | 11ull << 16);
        reg(5, xyz(40 * 16, 24 * 16));
        reg(3, 14 | 11ull << 16);
        reg(5, xyz(8 * 16, 32 * 16));
        reg(3, 14 | 11ull << 16);
        reg(5, xyz(40 * 16, 48 * 16));
    } else if (name == "gouraud") {
        reg(0, 3 | 1 << 3);
        reg(1, rgba(10, 20, 30));
        reg(5, xyz(8 * 16, 8 * 16));
        reg(1, rgba(27, 53, 79));
        reg(5, xyz(40 * 16, 8 * 16));
        reg(1, rgba(10, 20, 30));
        reg(5, xyz(8 * 16, 40 * 16));
    } else if (name == "blend") {
        reg(0x42, 0x44); // (Cs-Cd)*As/128 + Cd
        sprite(6 | 1 << 6, rgba(6, 11, 19, 85), 0);
    } else if (name == "fog") {
        reg(0x3D, rgba(128, 128, 128, 0));
        reg(0x0A, 255ull << 56);
        sprite(6 | 1 << 5, rgba(128, 128, 128), 0);
    } else {
        std::fprintf(stderr, "unknown case\n");
        return 2;
    }
    FILE *f = std::fopen(argv[2], "wb");
    if (!f)
        return 1;
    const char magic[8] = {'E', 'O', 'D', 'G', 'S', 'D', '0', '1'};
    uint32_t sizes[3] = {sizeof state, sizeof paths, sizeof transfer};
    std::fwrite(magic, 1, 8, f);
    std::fwrite(sizes, 1, sizeof sizes, f);
    std::fwrite(&state, 1, sizeof state, f);
    std::fwrite(paths, 1, sizeof paths, f);
    std::fwrite(&transfer, 1, sizeof transfer, f);
    std::fwrite(gs::vram, 1, gs::kVramSize, f);
    std::vector<Pair> gif;
    gif.emplace_back(regs.size() | 1ull << 15 | 1ull << 60, 0xE);
    gif.insert(gif.end(), regs.begin(), regs.end());
    command(f, 1, 3, gif.data(), gif.size() * 16);
    command(f, 3, 1, nullptr, 0);
    std::fclose(f);
    std::free(gs::vram);
}
