// gs_env.h - the GS drawing state decoded for one primitive (shared by the
// software rasteriser and the Direct3D 11 renderer).
#pragma once
#include <cstdint>
#include <cstring>

#include "gs_mem.h"
#include "gs_swizzle.h"

namespace gs {

struct TexEntry;

// 512-page bit set (hazard tracking between queued primitives).
struct PageSet {
    uint64_t w[kPages / 64];
    void clear() { memset(w, 0, sizeof w); }
    void add(uint32_t first, uint32_t count) {
        if (count >= kPages) {
            memset(w, 0xFF, sizeof w);
            return;
        }
        for (uint32_t i = 0; i < count; i++) {
            uint32_t p = (first + i) % kPages;
            w[p >> 6] |= 1ull << (p & 63);
        }
    }
    void add(const PageSet &o) {
        for (size_t i = 0; i < kPages / 64; i++) w[i] |= o.w[i];
    }
    bool any(const PageSet &o) const {
        uint64_t r = 0;
        for (size_t i = 0; i < kPages / 64; i++) r |= w[i] & o.w[i];
        return r != 0;
    }
};

enum { BL_GENERIC, BL_MIX, BL_ADD };

struct Env {
    // frame buffer
    uint32_t fbp, fbw, fpsm, fbmsk;
    uint32_t zbp, zpsm;
    bool zmsk;
    // tests
    bool ate;
    int atst, afail;
    uint32_t aref;
    bool date, datm;
    bool zte;
    int ztst;
    // blending
    bool abe, pabe, colclamp, fba;
    int ba, bb, bc, bd;
    uint32_t bfix;
    // texture
    bool tme, fst, tcc, bilinear_mag, bilinear_min;
    int tfx;
    uint32_t tbp, tbw, tpsm, tw, th, cpsm, csa;
    int wms, wmt, minu, maxu, minv, maxv;
    uint32_t ta0, ta1;
    bool aem;
    // mipmapping
    int mxl, lcm, l, mmin;
    float k;
    uint32_t mip_tbp[7], mip_tbw[7];
    // misc
    int scx0, scx1, scy0, scy1;
    bool fge, iip;
    uint32_t fogr, fogg, fogb;
    bool fb16, z16;
    bool write_none;  // e.g. FBMSK all set and ZMSK
    bool need_dst;    // the destination colour is read
    bool need_z;      // depth is tested or written
    uint32_t zmax;    // largest depth value of the format
    int pix_kind;     // shade_px_t variant: tme | depth test << 1 | depth write << 2
    int blend_kind;   // BL_MIX, BL_ADD or BL_GENERIC
    // address tables
    const SwizzleTab *fbT, *zT, *texT[7];
    uint32_t pal[256];  // CLUT expanded to RGBA for the current texture
    uint64_t pal_hash;  // of pal (the entries the texture format uses)
    uint32_t fpsm_mem;  // frame buffer format as stored (may be a depth format)
    // decoded texture levels (linear RGBA) when tcached
    bool tcached;
    TexEntry *tl[7];
    // pages read as texture, and of the frame and depth buffers
    PageSet tex_ps, fb_ps, z_ps;
};


// The state of the current primitive (gs_draw.cpp setup_env).
extern Env g_env;
// bumped whenever g_env is recomputed
extern uint32_t g_env_serial;
// bumped whenever the CLUT buffer may have changed
extern uint32_t g_clut_gen;
// One texel of level lvl in RGBA (any format; slow, see decode helpers).
uint32_t tex_fetch(const Env &e, int u, int v, int lvl);

}  // namespace gs
