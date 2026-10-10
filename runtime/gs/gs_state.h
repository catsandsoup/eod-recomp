// gs_state.h - GS register state shared by the GIF front end and the
// rasteriser.
#pragma once
#include <cstdint>

namespace gs {

struct Vertex {
    int32_t x, y;     // 12.4 fixed point, window coordinates (offset applied)
    uint32_t z;
    uint8_t r, g, b, a;
    uint8_t f;        // fog
    float s, t, q;
    int32_t u, v;     // 10.4 fixed point texel coordinates
};

struct Context {
    uint64_t tex0, tex1, tex2, clamp, miptbp1, miptbp2;
    uint64_t xyoffset, scissor, alpha, test, fba, frame, zbuf;
};

struct State {
    Context ctx[2];
    uint64_t prim, prmode, prmodecont, rgbaq, st, uv, fog;
    uint64_t texclut, scanmsk, texa, fogcol, dimx, dthe, colclamp, pabe;
    uint64_t bitbltbuf, trxpos, trxreg, trxdir;
    uint64_t signal, finish, label;
    float q;  // Q from ST/RGBAQ
    // privileged
    uint64_t pmode, smode1, smode2, srfsh, synch1, synch2, syncv;
    uint64_t dispfb[2], display[2], extbuf, extdata, extwrite, bgcolor;
    uint64_t csr, imr, busdir, siglblid;
    // vertex queue
    Vertex vq[3];
    int vcount;   // vertices accumulated for the current primitive
    int vfan;     // index of the fan centre
    // CLUT buffer (as on hardware: 32-bit entries split across two halves)
    uint16_t clut[512];
    uint32_t cbp0, cbp1;
};

extern State st;

// Bumped on every register write that can change how primitives are drawn
// (lets the rasteriser reuse its decoded state between primitives).
extern uint32_t g_reg_gen;

inline uint32_t bits(uint64_t v, int lo, int n) { return (uint32_t)((v >> lo) & ((1ull << n) - 1)); }

// rasteriser entry points (gs_draw.cpp)
void draw_point(const Vertex &a);
void draw_line(const Vertex &a, const Vertex &b);
void draw_triangle(const Vertex &a, const Vertex &b, const Vertex &c);
void draw_sprite(const Vertex &a, const Vertex &b);

// texture/CLUT helpers (gs_draw.cpp)
void clut_load(uint64_t tex0);

// raster worker threads (gs_draw.cpp): start, wait for all queued
// primitives, and order other VRAM accesses after the primitives they touch
void raster_start(int n);
void raster_sync();
// forgets cached state after VRAM/registers were replaced (GS replay)
void raster_reset();
// marks the pages drawn since the last env change as written (gs_mem.h page tracking)
void raster_mark_pages();
void raster_vram_write(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h);
void raster_vram_read(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h);
void raster_collect_stats();
// EOD_GS_CENSUS hooks (gs_draw.cpp)
void census_xfer(int dir, uint32_t sbp, uint32_t sbw, uint32_t spsm, uint32_t dbp, uint32_t dbw, uint32_t dpsm,
                 int sx, int sy, int dx, int dy, int w, int h);
void census_display(uint32_t fbp, uint32_t fbw, uint32_t psm, int w, int h);

// statistics for the current frame
struct Stats {
    uint32_t prims, pixels, transfers;
    uint32_t decoded, slow_samples, env_setups;
    uint32_t kind_px[64];  // triangle pixels per tri_spans variant
    uint32_t jobs, barriers[8];
    uint64_t raster_ns;
    uint32_t skipped;  // game frames skipped  // raster jobs queued, barriers by cause
};
extern Stats stats;

}  // namespace gs
