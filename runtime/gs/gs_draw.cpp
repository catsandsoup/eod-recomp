// gs_draw.cpp - software rasteriser implementing the GS pixel pipeline.
//
// Coverage: a pixel (x, y) is drawn when the point (x, y) of the 12.4
// vertex space lies inside the primitive (top-left fill rule); sprites
// cover [x0, x1) x [y0, y1).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../core/log.h"
#include "gs_mem.h"
#include "gs_state.h"
#include "gs_swizzle.h"

namespace gs {

bool g_trace;  // log every primitive of one frame (EOD_GS_TRACE=frame)
uint32_t g_reg_gen = 1;

namespace {

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
    // address tables
    const SwizzleTab *fbT, *zT, *texT[7];
    uint32_t pal[256];  // CLUT expanded to RGBA for the current texture
};

Env g_env;

void trace_prim(const char *kind, const Vertex *v, int n) {
    if (!g_trace) return;
    const Env &e = g_env;
    char buf[512];
    int k = snprintf(buf, sizeof buf,
                     "%s fb=%x/%d/%x tme=%d fst=%d tex=%x bw=%d psm=%x %dx%d tfx=%d tcc=%d cpsm=%x abe=%d mxl=%d mmin=%d "
                     "lcm=%d l=%d k=%g mip1=%x/%d csa=%d blend=%d%d%d%d/%x ate=%d/%d/%x/%d date=%d/%d z=%d/%d/%x/%x zm=%d "
                     "fge=%d fbmsk=%x iip=%d wms=%d/%d",
                     kind, e.fbp, e.fbw, e.fpsm, e.tme, e.fst, e.tbp, e.tbw, e.tpsm, e.tw, e.th, e.tfx, e.tcc, e.cpsm,
                     e.abe, e.mxl, e.mmin, e.lcm, e.l, e.k, e.mip_tbp[1], e.mip_tbw[1], e.csa, e.ba, e.bb, e.bc, e.bd,
                     e.bfix, e.ate, e.atst, e.aref, e.afail, e.date, e.datm, e.zte, e.ztst, e.zbp, e.zpsm, e.zmsk,
                     e.fge, e.fbmsk, e.iip, e.wms, e.wmt);
    for (int i = 0; i < n; i++)
        k += snprintf(buf + k, sizeof buf - k, " | %.1f,%.1f z%u c%02x%02x%02x%02x uv%d,%d st%.3f,%.3f,%.3f",
                      v[i].x / 16.0, v[i].y / 16.0, v[i].z, v[i].r, v[i].g, v[i].b, v[i].a, v[i].u, v[i].v, v[i].s,
                      v[i].t, v[i].q);
    log_write(LOG_GS, "%s", buf);
}


// ----------------------------------------------------------------- setup
uint32_t g_env_attr = ~0u, g_env_gen;

uint32_t clut_lookup(uint32_t idx, const Env &e, bool four_bit);

void setup_env(uint32_t attr) {
    attr &= ~7u;  // the primitive type doesn't matter here
    if (attr == g_env_attr && g_reg_gen == g_env_gen) return;
    g_env_attr = attr;
    g_env_gen = g_reg_gen;
    Env &e = g_env;
    const Context &c = st.ctx[(attr >> 9) & 1];
    e.fbp = bits(c.frame, 0, 9) * 32;
    e.fbw = bits(c.frame, 16, 6);
    e.fpsm = bits(c.frame, 24, 6);
    e.fbmsk = bits(c.frame, 32, 32);
    e.zbp = bits(c.zbuf, 0, 9) * 32;
    e.zpsm = bits(c.zbuf, 24, 4) | 0x30;
    e.zmsk = bits(c.zbuf, 32, 1);
    e.ate = bits(c.test, 0, 1);
    e.atst = (int)bits(c.test, 1, 3);
    e.aref = bits(c.test, 4, 8);
    e.afail = (int)bits(c.test, 12, 2);
    e.date = bits(c.test, 14, 1);
    e.datm = bits(c.test, 15, 1);
    e.zte = bits(c.test, 16, 1);
    e.ztst = (int)bits(c.test, 17, 2);
    if (!e.zte) e.ztst = 1;
    e.abe = (attr >> 6) & 1;
    e.ba = (int)bits(c.alpha, 0, 2);
    e.bb = (int)bits(c.alpha, 2, 2);
    e.bc = (int)bits(c.alpha, 4, 2);
    e.bd = (int)bits(c.alpha, 6, 2);
    e.bfix = bits(c.alpha, 32, 8);
    e.pabe = st.pabe & 1;
    e.colclamp = st.colclamp & 1;
    e.fba = c.fba & 1;
    e.tme = (attr >> 4) & 1;
    e.fst = (attr >> 8) & 1;
    e.fge = (attr >> 5) & 1;
    e.iip = (attr >> 3) & 1;
    uint64_t t0 = c.tex0;
    e.tbp = bits(t0, 0, 14);
    e.tbw = bits(t0, 14, 6);
    e.tpsm = bits(t0, 20, 6);
    e.tw = 1u << (bits(t0, 26, 4) > 10 ? 10 : bits(t0, 26, 4));
    e.th = 1u << (bits(t0, 30, 4) > 10 ? 10 : bits(t0, 30, 4));
    e.tcc = bits(t0, 34, 1);
    e.tfx = (int)bits(t0, 35, 2);
    e.cpsm = bits(t0, 51, 4);
    e.csa = bits(t0, 56, 5);
    e.wms = (int)bits(c.clamp, 0, 2);
    e.wmt = (int)bits(c.clamp, 2, 2);
    e.minu = (int)bits(c.clamp, 4, 10);
    e.maxu = (int)bits(c.clamp, 14, 10);
    e.minv = (int)bits(c.clamp, 24, 10);
    e.maxv = (int)bits(c.clamp, 34, 10);
    e.ta0 = bits(st.texa, 0, 8);
    e.ta1 = bits(st.texa, 32, 8);
    e.aem = bits(st.texa, 15, 1);
    e.lcm = (int)bits(c.tex1, 0, 1);
    e.mxl = (int)bits(c.tex1, 2, 3);
    e.bilinear_mag = bits(c.tex1, 5, 1);
    e.mmin = (int)bits(c.tex1, 6, 3);
    e.bilinear_min = e.mmin == 1 || e.mmin == 4 || e.mmin == 5;
    e.l = (int)bits(c.tex1, 19, 2);
    {
        int32_t k = (int32_t)(bits(c.tex1, 32, 12) << 20) >> 20;  // signed 7.4
        e.k = k / 16.0f;
    }
    e.mip_tbp[0] = e.tbp;
    e.mip_tbw[0] = e.tbw;
    e.mip_tbp[1] = bits(c.miptbp1, 0, 14);
    e.mip_tbw[1] = bits(c.miptbp1, 14, 6);
    e.mip_tbp[2] = bits(c.miptbp1, 20, 14);
    e.mip_tbw[2] = bits(c.miptbp1, 34, 6);
    e.mip_tbp[3] = bits(c.miptbp1, 40, 14);
    e.mip_tbw[3] = bits(c.miptbp1, 54, 6);
    e.mip_tbp[4] = bits(c.miptbp2, 0, 14);
    e.mip_tbw[4] = bits(c.miptbp2, 14, 6);
    e.mip_tbp[5] = bits(c.miptbp2, 20, 14);
    e.mip_tbw[5] = bits(c.miptbp2, 34, 6);
    e.mip_tbp[6] = bits(c.miptbp2, 40, 14);
    e.mip_tbw[6] = bits(c.miptbp2, 54, 6);
    e.scx0 = (int)bits(c.scissor, 0, 11);
    e.scx1 = (int)bits(c.scissor, 16, 11);
    e.scy0 = (int)bits(c.scissor, 32, 11);
    e.scy1 = (int)bits(c.scissor, 48, 11);
    e.fogr = bits(st.fogcol, 0, 8);
    e.fogg = bits(st.fogcol, 8, 8);
    e.fogb = bits(st.fogcol, 16, 8);
    e.fb16 = e.fpsm == PSMCT16 || e.fpsm == PSMCT16S;
    e.z16 = e.zpsm == PSMZ16 || e.zpsm == PSMZ16S;
    uint32_t full = e.fpsm == PSMCT24 ? 0xFFFFFFu : 0xFFFFFFFFu;
    e.write_none = (e.fbmsk & full) == full && e.zmsk;
    e.need_dst = e.date || (e.abe && (e.ba == 1 || e.bb == 1 || e.bd == 1 || e.bc == 1)) || e.fbmsk;
    e.need_z = e.ztst != 1 || !e.zmsk;
    e.fbT = swizzle_table(e.fpsm, e.fbw);
    e.zT = swizzle_table(e.zpsm, e.fbw);
    if (e.tme) {
        int levels = (e.mmin >= 2 && e.mxl > 0) ? e.mxl : 0;
        for (int i = 0; i <= levels && i < 7; i++) e.texT[i] = swizzle_table(e.tpsm, e.mip_tbw[i]);
        bool four = e.tpsm == PSMT4 || e.tpsm == PSMT4HL || e.tpsm == PSMT4HH;
        bool eight = e.tpsm == PSMT8 || e.tpsm == PSMT8H;
        if (four || eight)
            for (uint32_t i = 0, n = four ? 16 : 256; i < n; i++) e.pal[i] = clut_lookup(i, e, four);
    }
}

// ------------------------------------------------------------- texturing
inline uint32_t expand16(uint32_t c, const Env &e) {
    uint32_t r = (c & 0x1F) << 3, g = ((c >> 5) & 0x1F) << 3, b = ((c >> 10) & 0x1F) << 3;
    uint32_t a;
    if (c & 0x8000)
        a = e.ta1;
    else
        a = (e.aem && (c & 0x7FFF) == 0) ? 0 : e.ta0;
    return r | (g << 8) | (b << 16) | (a << 24);
}

bool g_debug_noclut = getenv("EOD_DEBUG_NOCLUT") != nullptr;

uint32_t clut_lookup(uint32_t idx, const Env &e, bool four_bit) {
    if (g_debug_noclut) {
        uint32_t g = four_bit ? idx * 17 : idx;
        return g | (g << 8) | (g << 16) | 0x80000000u;
    }
    if (e.cpsm == PSMCT32 || e.cpsm == PSMCT24) {
        uint32_t i = ((four_bit ? e.csa * 16 : 0) + idx) & 255;
        uint32_t c = st.clut[i] | ((uint32_t)st.clut[i + 256] << 16);
        if (e.cpsm == PSMCT24) {
            uint32_t a = (e.aem && (c & 0xFFFFFF) == 0) ? 0 : e.ta0;
            c = (c & 0xFFFFFF) | (a << 24);
        }
        return c;
    }
    uint32_t i = ((four_bit ? e.csa * 16 : 0) + idx) & 511;
    return expand16(st.clut[i], e);
}

inline uint32_t fetch(const Env &e, int u, int v, int lvl) {
    uint32_t a = e.texT[lvl]->addr(e.mip_tbp[lvl], u, v);
    switch (e.tpsm) {
    case PSMCT32:
    case PSMZ32:
        return word(a);
    case PSMCT24: {
        uint32_t c = word(a) & 0xFFFFFF;
        uint32_t al = (e.aem && c == 0) ? 0 : e.ta0;
        return c | (al << 24);
    }
    case PSMZ24:
        return (word(a) & 0xFFFFFF) | (e.ta0 << 24);
    case PSMCT16:
    case PSMCT16S:
    case PSMZ16:
    case PSMZ16S:
        return expand16(half(a), e);
    case PSMT8: return e.pal[byte(a)];
    case PSMT4: return e.pal[nibble(a)];
    case PSMT8H: return e.pal[word(a) >> 24];
    case PSMT4HL: return e.pal[(word(a) >> 24) & 15];
    case PSMT4HH: return e.pal[word(a) >> 28];
    }
    return 0;
}

inline int wrap(int c, int size, int mode, int mn, int mx) {
    switch (mode) {
    case 0:  // REPEAT
        return c & (size - 1);
    case 1:  // CLAMP
        return c < 0 ? 0 : c >= size ? size - 1 : c;
    case 2:  // REGION_CLAMP
        return c < mn ? mn : c > mx ? mx : c;
    default:  // REGION_REPEAT
        return (c & mn) | mx;
    }
}

// u, v in 1/16 texel units at mip level `lvl`.
uint32_t sample(const Env &e, int u, int v, bool bilinear, int lvl) {
    int tw = (int)e.tw >> lvl, th = (int)e.th >> lvl;
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    if (lvl) {
        u >>= lvl;
        v >>= lvl;
    }
    if (!bilinear) {
        int x = wrap(u >> 4, tw, e.wms, e.minu, e.maxu);
        int y = wrap(v >> 4, th, e.wmt, e.minv, e.maxv);
        return fetch(e, x, y, lvl);
    }
    u -= 8;
    v -= 8;
    int x0 = u >> 4, y0 = v >> 4;
    int fu = u & 15, fv = v & 15;
    int xa = wrap(x0, tw, e.wms, e.minu, e.maxu), xb = wrap(x0 + 1, tw, e.wms, e.minu, e.maxu);
    int ya = wrap(y0, th, e.wmt, e.minv, e.maxv), yb = wrap(y0 + 1, th, e.wmt, e.minv, e.maxv);
    uint32_t c00 = fetch(e, xa, ya, lvl), c10 = fetch(e, xb, ya, lvl);
    uint32_t c01 = fetch(e, xa, yb, lvl), c11 = fetch(e, xb, yb, lvl);
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int a = (c00 >> sh) & 255, b = (c10 >> sh) & 255, c = (c01 >> sh) & 255, d = (c11 >> sh) & 255;
        int top = a * (16 - fu) + b * fu;
        int bot = c * (16 - fu) + d * fu;
        int val = (top * (16 - fv) + bot * fv) >> 8;
        out |= (uint32_t)val << sh;
    }
    return out;
}

// ---------------------------------------------------------- frame buffer
// Per-row parts of the frame and depth buffer addresses.
struct RowAddr {
    uint32_t fb, fbc, z, zc;
    int y;
};

inline void row_addr(const Env &e, int y, RowAddr &ra) {
    ra.fb = e.fbT->row_base(e.fbp, y);
    ra.fbc = e.fbT->rowC[y & 2047];
    ra.z = e.zT->row_base(e.zbp, y);
    ra.zc = e.zT->rowC[y & 2047];
    ra.y = y;
}

inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

inline int ffloor(double d) {
    int i = (int)d;
    return i - (d < (double)i);
}

// ------------------------------------------------------------- pixel
struct Frag {
    int r, g, b, a;  // vertex colour 0-255
    uint32_t z;
    int fog;
    int u, v;        // texel coords (1/16) when textured
    int lod;         // mip level
};

// One pixel inside the scissor rectangle.
int g_dbg_x = -1, g_dbg_y = -1;  // EOD_DEBUG_PIXEL=x,y: log writes to that pixel in traced frames
struct DbgPixelInit {
    DbgPixelInit() {
        if (const char *p = getenv("EOD_DEBUG_PIXEL")) sscanf(p, "%d,%d", &g_dbg_x, &g_dbg_y);
    }
} g_dbg_pixel_init;

inline void shade_px(const Env &e, int x, const RowAddr &ra, const Frag &f) {
    stats.pixels++;
    if (g_trace && x == g_dbg_x && ra.y == g_dbg_y) {
        uint32_t t = e.tme ? sample(e, f.u, f.v, f.lod > 0 ? e.bilinear_min : e.bilinear_mag, f.lod > 0 ? f.lod : 0) : 0;
        log_write(LOG_GS, "PIXEL %d,%d: tex=%x/%d psm=%x %dx%d uv=%d,%d (%.2f,%.2f) lod=%d texel=%08x col=%02x%02x%02x%02x z=%u",
                  x, ra.y, e.tbp, e.tbw, e.tpsm, e.tw, e.th, f.u, f.v, f.u / 16.0, f.v / 16.0, f.lod, t, f.r, f.g, f.b, f.a, f.z);
    }
    // ---- depth test
    uint32_t za = 0;
    bool have_za = false;
    uint32_t z = f.z;
    if (e.zpsm == PSMZ24) z = z > 0xFFFFFF ? 0xFFFFFF : z;
    else if (e.z16) z = z > 0xFFFF ? 0xFFFF : z;
    if (e.ztst != 1) {
        if (e.ztst == 0) return;
        za = e.zT->addr_row(ra.z, ra.zc, x);
        have_za = true;
        uint32_t zb = e.z16 ? half(za) : (e.zpsm == PSMZ24 ? word(za) & 0xFFFFFF : word(za));
        if (e.ztst == 2 ? z < zb : z <= zb) return;
    }
    // ---- texture + colour
    int r = f.r, g = f.g, b = f.b, a = f.a;
    if (e.tme) {
        bool bl = f.lod > 0 ? e.bilinear_min : e.bilinear_mag;
        uint32_t t = sample(e, f.u, f.v, bl, f.lod > 0 ? f.lod : 0);
        int tr = t & 255, tg = (t >> 8) & 255, tb = (t >> 16) & 255, ta = t >> 24;
        switch (e.tfx) {
        case 0:  // MODULATE
            r = (tr * r) >> 7;
            g = (tg * g) >> 7;
            b = (tb * b) >> 7;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            if (e.tcc) a = (ta * a) >> 7;
            break;
        case 1:  // DECAL
            r = tr;
            g = tg;
            b = tb;
            if (e.tcc) a = ta;
            break;
        case 2:  // HIGHLIGHT
            r = ((tr * r) >> 7) + a;
            g = ((tg * g) >> 7) + a;
            b = ((tb * b) >> 7) + a;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            if (e.tcc) a = ta + a;
            break;
        default:  // HIGHLIGHT2
            r = ((tr * r) >> 7) + a;
            g = ((tg * g) >> 7) + a;
            b = ((tb * b) >> 7) + a;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            if (e.tcc) a = ta;
            break;
        }
        if (a > 255) a = 255;
    }
    if (e.fge) {
        int fo = f.fog;
        r = (fo * r + (255 - fo) * (int)e.fogr) >> 8;
        g = (fo * g + (255 - fo) * (int)e.fogg) >> 8;
        b = (fo * b + (255 - fo) * (int)e.fogb) >> 8;
    }
    // ---- alpha test
    bool write_fb = true, write_z = !e.zmsk, write_alpha = true;
    if (e.ate) {
        bool pass;
        uint32_t ua = (uint32_t)a;
        switch (e.atst) {
        case 0: pass = false; break;
        case 1: pass = true; break;
        case 2: pass = ua < e.aref; break;
        case 3: pass = ua <= e.aref; break;
        case 4: pass = ua == e.aref; break;
        case 5: pass = ua >= e.aref; break;
        case 6: pass = ua > e.aref; break;
        default: pass = ua != e.aref; break;
        }
        if (!pass) {
            switch (e.afail) {
            case 0: return;                                  // KEEP
            case 1: write_z = false; break;                  // FB_ONLY
            case 2: write_fb = false; break;                 // ZB_ONLY
            default: write_z = false; write_alpha = false;   // RGB_ONLY
            }
        }
    }
    // ---- destination colour
    uint32_t fa = 0, dst = 0;
    if (write_fb) {
        fa = e.fbT->addr_row(ra.fb, ra.fbc, x);
        if (e.need_dst) {
            if (e.fb16) {
                uint32_t c = half(fa);
                dst = ((c & 0x1F) << 3) | (((c >> 5) & 0x1F) << 11) | (((c >> 10) & 0x1F) << 19) |
                      ((c & 0x8000) ? 0x80000000u : 0);
            } else {
                dst = word(fa);
                if (e.fpsm == PSMCT24) dst = (dst & 0xFFFFFF) | 0x80000000u;
            }
        }
        if (e.date) {
            bool abit = (dst >> 31) & 1;
            if (abit != e.datm) return;
        }
    }
    // ---- blending
    if (write_fb && e.abe && !(e.pabe && a < 0x80)) {
        int cs[3] = {r, g, b}, cd[3] = {(int)(dst & 255), (int)((dst >> 8) & 255), (int)((dst >> 16) & 255)};
        int ad = (int)(dst >> 24);
        int cc = e.bc == 0 ? a : e.bc == 1 ? ad : (int)e.bfix;
        int out[3];
        for (int k = 0; k < 3; k++) {
            int A = e.ba == 0 ? cs[k] : e.ba == 1 ? cd[k] : 0;
            int B = e.bb == 0 ? cs[k] : e.bb == 1 ? cd[k] : 0;
            int D = e.bd == 0 ? cs[k] : e.bd == 1 ? cd[k] : 0;
            int v = (((A - B) * cc) >> 7) + D;
            if (e.colclamp)
                v = v < 0 ? 0 : v > 255 ? 255 : v;
            else
                v &= 255;
            out[k] = v;
        }
        r = out[0];
        g = out[1];
        b = out[2];
    } else if (!e.colclamp) {
        r &= 255;
        g &= 255;
        b &= 255;
    }
    // ---- write
    if (write_fb) {
        uint32_t oa = (uint32_t)(a | (e.fba ? 0x80 : 0)) & 255;
        uint32_t col = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | (oa << 24);
        uint32_t msk = e.fbmsk;
        if (!write_alpha) msk |= 0xFF000000u;
        if (e.fb16) {
            uint32_t c16 = ((col >> 3) & 0x1F) | (((col >> 11) & 0x1F) << 5) | (((col >> 19) & 0x1F) << 10) |
                           ((col >> 31) << 15);
            uint32_t m16 = ((msk >> 3) & 0x1F) | (((msk >> 11) & 0x1F) << 5) | (((msk >> 19) & 0x1F) << 10) |
                           ((msk >> 31) << 15);
            uint16_t &p = half(fa);
            p = (uint16_t)((c16 & ~m16) | (p & m16));
        } else {
            if (e.fpsm == PSMCT24) msk |= 0xFF000000u;
            uint32_t &p = word(fa);
            p = msk ? ((col & ~msk) | (p & msk)) : col;
        }
    }
    if (write_z && e.ztst != 0) {
        if (!have_za) za = e.zT->addr_row(ra.z, ra.zc, x);
        if (e.z16)
            half(za) = (uint16_t)z;
        else if (e.zpsm == PSMZ24)
            word(za) = (word(za) & 0xFF000000u) | z;
        else
            word(za) = z;
    }
}

void shade(const Env &e, int x, int y, const Frag &f) {
    if (x < e.scx0 || x > e.scx1 || y < e.scy0 || y > e.scy1) return;
    RowAddr ra;
    row_addr(e, y, ra);
    shade_px(e, x, ra, f);
}

// texture coordinates for a fragment
inline void tex_coords(const Env &e, float s, float t, float q, Frag &f) {
    if (e.fst) return;  // already in f.u / f.v
    if (q == 0.0f) q = 1e-30f;
    float iq = 1.0f / q;
    f.u = ffloor(s * iq * (float)(e.tw * 16));
    f.v = ffloor(t * iq * (float)(e.th * 16));
    static bool nomip = getenv("EOD_DEBUG_NOMIP") != nullptr;
    if (e.mmin >= 2 && e.mxl > 0 && !nomip) {
        float lod = e.lcm ? e.k : (log2f(1.0f / fabsf(q)) * (float)(1 << e.l) + e.k);
        int lvl = lod <= 0 ? 0 : (int)(lod + 0.5f);
        if (lvl > e.mxl) lvl = e.mxl;
        f.lod = lvl;
    }
}

}  // namespace

// ------------------------------------------------------------ primitives
static uint32_t current_attr() {
    return (st.prmodecont & 1) ? (uint32_t)st.prim : ((uint32_t)st.prmode & ~7u) | ((uint32_t)st.prim & 7);
}

void draw_sprite(const Vertex &a, const Vertex &b) {
    uint32_t attr = current_attr();
    setup_env(attr);
    Env &e = g_env;
    if (g_trace) {
        Vertex vv[2] = {a, b};
        trace_prim("SPRITE", vv, 2);
    }
    if (e.write_none) return;
    Vertex v0 = a, v1 = b;
    if (v1.x < v0.x) {
        std::swap(v0.x, v1.x);
        std::swap(v0.u, v1.u);
        std::swap(v0.s, v1.s);
    }
    if (v1.y < v0.y) {
        std::swap(v0.y, v1.y);
        std::swap(v0.v, v1.v);
        std::swap(v0.t, v1.t);
    }
    int x0 = (v0.x + 15) >> 4, x1 = (v1.x + 15) >> 4;
    int y0 = (v0.y + 15) >> 4, y1 = (v1.y + 15) >> 4;
    if (x0 < e.scx0) x0 = e.scx0;
    if (y0 < e.scy0) y0 = e.scy0;
    if (x1 > e.scx1 + 1) x1 = e.scx1 + 1;
    if (y1 > e.scy1 + 1) y1 = e.scy1 + 1;
    if (x0 >= x1 || y0 >= y1) return;
    Frag f;
    f.r = b.r;
    f.g = b.g;
    f.b = b.b;
    f.a = b.a;
    f.z = b.z;
    f.fog = b.f;
    f.lod = 0;
    double dx = (double)(v1.x - v0.x), dy = (double)(v1.y - v0.y);
    if (dx <= 0) dx = 1;
    if (dy <= 0) dy = 1;
    // texture coordinate gradients per pixel (in 1/16 texel units)
    double u0, du, w0, dw;
    if (e.fst) {
        du = (v1.u - v0.u) * 16.0 / dx;
        dw = (v1.v - v0.v) * 16.0 / dy;
        u0 = v0.u + (x0 * 16 - v0.x) * du / 16.0;
        w0 = v0.v + (y0 * 16 - v0.y) * dw / 16.0;
    } else {
        // STQ sprite: Q of the second vertex
        double q = b.q != 0 ? b.q : 1e-30;
        double s0 = v0.s / q * e.tw * 16, s1 = v1.s / q * e.tw * 16;
        double t0 = v0.t / q * e.th * 16, t1 = v1.t / q * e.th * 16;
        du = (s1 - s0) * 16.0 / dx;
        dw = (t1 - t0) * 16.0 / dy;
        u0 = s0 + (x0 * 16 - v0.x) * du / 16.0;
        w0 = t0 + (y0 * 16 - v0.y) * dw / 16.0;
    }
    if (e.ztst == 0) return;
    for (int y = y0; y < y1; y++) {
        RowAddr ra;
        row_addr(e, y, ra);
        f.v = ffloor(w0 + (y - y0) * dw);
        double u = u0;
        for (int x = x0; x < x1; x++, u += du) {
            f.u = ffloor(u);
            shade_px(e, x, ra, f);
        }
    }
}

void draw_triangle(const Vertex &a0, const Vertex &b0, const Vertex &c0) {
    uint32_t attr = current_attr();
    setup_env(attr);
    Env &e = g_env;
    if (g_trace) {
        Vertex vv[3] = {a0, b0, c0};
        trace_prim("TRI", vv, 3);
        static int dumped;
        if (!dumped && e.tme && e.tpsm == PSMT4 && !e.abe && getenv("EOD_DUMP_TEX")) {
            dumped = 1;
            if (FILE *f = fopen(getenv("EOD_DUMP_TEX"), "wb")) {
                fwrite(vram, 1, kVramSize, f);
                fclose(f);
            }
            log_write(LOG_GS, "dumped VRAM at TRI tex=%x bw=%d %dx%d cbp0=%x cbp1=%x", e.tbp, e.tbw, e.tw, e.th, st.cbp0,
                      st.cbp1);
        }
    }
    if (e.write_none) return;
    {
        // EOD_DEBUG_SKIP bits: 1 blended, 2 T8, 4 T4, 8 HIGHLIGHT2, 16 alpha-tested
        static int skip = getenv("EOD_DEBUG_SKIP") ? atoi(getenv("EOD_DEBUG_SKIP")) : 0;
        if (skip && (((skip & 1) && e.abe) || ((skip & 2) && e.tme && e.tpsm == PSMT8) ||
                     ((skip & 4) && e.tme && e.tpsm == PSMT4) || ((skip & 8) && e.tme && e.tfx == 3) ||
                     ((skip & 16) && e.ate)))
            return;
    }
    const Vertex *v[3] = {&a0, &b0, &c0};
    int64_t cross = (int64_t)(v[1]->x - v[0]->x) * (v[2]->y - v[0]->y) - (int64_t)(v[2]->x - v[0]->x) * (v[1]->y - v[0]->y);
    if (cross == 0) return;
    if (cross > 0) std::swap(v[1], v[2]);
    // bounding box (pixels)
    int minx = std::min(v[0]->x, std::min(v[1]->x, v[2]->x));
    int maxx = std::max(v[0]->x, std::max(v[1]->x, v[2]->x));
    int miny = std::min(v[0]->y, std::min(v[1]->y, v[2]->y));
    int maxy = std::max(v[0]->y, std::max(v[1]->y, v[2]->y));
    int px0 = (minx + 15) >> 4, px1 = maxx >> 4;
    int py0 = (miny + 15) >> 4, py1 = maxy >> 4;
    if (px0 < e.scx0) px0 = e.scx0;
    if (py0 < e.scy0) py0 = e.scy0;
    if (px1 > e.scx1) px1 = e.scx1;
    if (py1 > e.scy1) py1 = e.scy1;
    if (px0 > px1 || py0 > py1) return;

    // attribute planes: value(x,y) = base + dx*(X - X0) + dy*(Y - Y0) in pixels
    double X0 = v[0]->x / 16.0, Y0 = v[0]->y / 16.0;
    double x1 = v[1]->x / 16.0 - X0, y1 = v[1]->y / 16.0 - Y0;
    double x2 = v[2]->x / 16.0 - X0, y2 = v[2]->y / 16.0 - Y0;
    double D = x1 * y2 - x2 * y1;
    auto plane = [&](double a, double b, double c, double &ddx, double &ddy) {
        ddx = ((b - a) * y2 - (c - a) * y1) / D;
        ddy = ((c - a) * x1 - (b - a) * x2) / D;
    };
    enum { R, G, B, A, Z, F, S, T, Q, NA };
    double base[NA], gx[NA], gy[NA];
    double av[3][NA];
    for (int k = 0; k < 3; k++) {
        const Vertex &p = *v[k];
        av[k][R] = p.r;
        av[k][G] = p.g;
        av[k][B] = p.b;
        av[k][A] = p.a;
        av[k][Z] = (double)p.z;
        av[k][F] = p.f;
        if (e.fst) {
            av[k][S] = p.u;
            av[k][T] = p.v;
            av[k][Q] = 1;
        } else {
            av[k][S] = p.s;
            av[k][T] = p.t;
            av[k][Q] = p.q;
        }
    }
    for (int i = 0; i < NA; i++) {
        base[i] = av[0][i];
        plane(av[0][i], av[1][i], av[2][i], gx[i], gy[i]);
    }
    if (!e.iip) {
        // flat shading: colour of the last vertex
        base[R] = c0.r;
        base[G] = c0.g;
        base[B] = c0.b;
        base[A] = c0.a;
        gx[R] = gy[R] = gx[G] = gy[G] = gx[B] = gy[B] = gx[A] = gy[A] = 0;
    }

    // edges
    struct Edge {
        int64_t dx, dy, xa, ya;
        int bias;
    } ed[3];
    for (int k = 0; k < 3; k++) {
        const Vertex &pa = *v[k], &pb = *v[(k + 1) % 3];
        ed[k].dx = pb.x - pa.x;
        ed[k].dy = pb.y - pa.y;
        ed[k].xa = pa.x;
        ed[k].ya = pa.y;
        ed[k].bias = (ed[k].dy > 0 || (ed[k].dy == 0 && ed[k].dx < 0)) ? 0 : 1;
    }
    if (e.ztst == 0) return;
    const bool gour = e.iip, tex = e.tme, fog = e.fge, need_z = e.need_z, stq = e.tme && !e.fst;
    Frag f;
    f.lod = 0;
    f.z = 0;
    f.fog = 0;
    if (!gour) {
        f.r = clamp255((int)base[R]);
        f.g = clamp255((int)base[G]);
        f.b = clamp255((int)base[B]);
        f.a = clamp255((int)base[A]);
    }
    for (int y = py0; y <= py1; y++) {
        int64_t Y = (int64_t)y * 16;
        int xl = px0, xr = px1;
        bool empty = false;
        for (int k = 0; k < 3 && !empty; k++) {
            // E(X) = dy*(X - xa) - (Y - ya)*dx >= bias, X = 16*x
            const Edge &E = ed[k];
            int64_t c = -E.dy * E.xa - (Y - E.ya) * E.dx;
            int64_t A = E.dy * 16;
            if (A > 0) {
                // x >= ceil((bias - c) / A)
                int64_t num = E.bias - c;
                int64_t lo = num >= 0 ? (num + A - 1) / A : -((-num) / A);
                if (lo > xl) xl = (int)std::min<int64_t>(lo, 1 << 20);
            } else if (A < 0) {
                // x <= floor((c - bias) / -A)
                int64_t num = c - E.bias, den = -A;
                int64_t hi = num >= 0 ? num / den : -((-num + den - 1) / den);
                if (hi < xr) xr = (int)std::max<int64_t>(hi, -(1 << 20));
            } else if (c < E.bias) {
                empty = true;
            }
        }
        if (empty || xl > xr) continue;
        RowAddr ra;
        row_addr(e, y, ra);
        double fy = y - Y0;
        double fx = xl - X0;
        double val[NA];
        for (int i = 0; i < NA; i++) val[i] = base[i] + gx[i] * fx + gy[i] * fy;
        double r = val[R], g = val[G], b = val[B], a = val[A], zz = val[Z], fo = val[F];
        double s = val[S], t = val[T], q = val[Q];
        for (int x = xl; x <= xr; x++) {
            if (gour) {
                f.r = clamp255((int)r);
                f.g = clamp255((int)g);
                f.b = clamp255((int)b);
                f.a = clamp255((int)a);
                r += gx[R];
                g += gx[G];
                b += gx[B];
                a += gx[A];
            }
            if (need_z) {
                f.z = zz <= 0 ? 0 : zz >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)zz;
                zz += gx[Z];
            }
            if (fog) {
                f.fog = clamp255((int)fo);
                fo += gx[F];
            }
            if (tex) {
                if (stq) {
                    tex_coords(e, (float)s, (float)t, (float)q, f);
                    q += gx[Q];
                } else {
                    f.u = ffloor(s);
                    f.v = ffloor(t);
                }
                s += gx[S];
                t += gx[T];
            }
            shade_px(e, x, ra, f);
        }
    }
}

void draw_line(const Vertex &a, const Vertex &b) {
    uint32_t attr = current_attr();
    setup_env(attr);
    Env &e = g_env;
    if (e.write_none) return;
    double x0 = a.x / 16.0, y0 = a.y / 16.0, x1 = b.x / 16.0, y1 = b.y / 16.0;
    double len = std::max(fabs(x1 - x0), fabs(y1 - y0));
    int n = (int)ceil(len);
    if (n <= 0) n = 1;
    Frag f;
    f.lod = 0;
    for (int i = 0; i < n; i++) {
        double t = (double)i / n;
        int x = (int)floor(x0 + (x1 - x0) * t + 0.5), y = (int)floor(y0 + (y1 - y0) * t + 0.5);
        const Vertex &cv = e.iip ? a : b;
        f.r = e.iip ? (int)(a.r + (b.r - a.r) * t) : cv.r;
        f.g = e.iip ? (int)(a.g + (b.g - a.g) * t) : cv.g;
        f.b = e.iip ? (int)(a.b + (b.b - a.b) * t) : cv.b;
        f.a = e.iip ? (int)(a.a + (b.a - a.a) * t) : cv.a;
        f.z = (uint32_t)(a.z + ((double)b.z - a.z) * t);
        f.fog = (int)(a.f + (b.f - a.f) * t);
        if (e.tme) {
            if (e.fst) {
                f.u = (int)(a.u + (b.u - a.u) * t);
                f.v = (int)(a.v + (b.v - a.v) * t);
            } else {
                tex_coords(e, (float)(a.s + (b.s - a.s) * t), (float)(a.t + (b.t - a.t) * t),
                           (float)(a.q + (b.q - a.q) * t), f);
            }
        }
        shade(e, x, y, f);
    }
}

void draw_point(const Vertex &a) {
    uint32_t attr = current_attr();
    setup_env(attr);
    Env &e = g_env;
    if (e.write_none) return;
    Frag f;
    f.r = a.r;
    f.g = a.g;
    f.b = a.b;
    f.a = a.a;
    f.z = a.z;
    f.fog = a.f;
    f.lod = 0;
    if (e.tme) {
        if (e.fst) {
            f.u = a.u;
            f.v = a.v;
        } else {
            tex_coords(e, a.s, a.t, a.q, f);
        }
    }
    shade(e, (a.x + 8) >> 4, (a.y + 8) >> 4, f);
}

// ------------------------------------------------------------------ CLUT
void clut_load(uint64_t tex0) {
    uint32_t psm = bits(tex0, 20, 6);
    bool four = psm == PSMT4 || psm == PSMT4HL || psm == PSMT4HH;
    bool eight = psm == PSMT8 || psm == PSMT8H;
    if (!four && !eight) return;
    uint32_t cld = bits(tex0, 61, 3);
    uint32_t cbp = bits(tex0, 37, 14);
    switch (cld) {
    case 0: return;
    case 1: break;
    case 2: st.cbp0 = cbp; break;
    case 3: st.cbp1 = cbp; break;
    case 4:
        if (cbp == st.cbp0) return;
        st.cbp0 = cbp;
        break;
    case 5:
        if (cbp == st.cbp1) return;
        st.cbp1 = cbp;
        break;
    default: return;
    }
    uint32_t cpsm = bits(tex0, 51, 4), csm = bits(tex0, 55, 1), csa = bits(tex0, 56, 5);
    if (g_trace)
        log_write(LOG_GS, "CLUT load cld=%u cbp=%x cpsm=%x csm=%u csa=%u psm=%x", cld, cbp, cpsm, csm, csa, psm);
    int n = eight ? 256 : 16;
    uint32_t off = four ? csa * 16 : 0;
    uint32_t cbw = bits(st.texclut, 0, 6), cou = bits(st.texclut, 6, 6), cov = bits(st.texclut, 12, 10);
    for (int i = 0; i < n; i++) {
        int x, y;
        uint32_t bw = 1;
        if (csm == 0) {
            if (eight) {
                x = (i & 7) | ((i & 0x10) >> 1);
                y = ((i & 8) >> 3) | ((i & 0xE0) >> 4);
            } else {
                x = i & 7;
                y = i >> 3;
            }
        } else {
            x = (int)(cou * 16) + i;
            y = (int)cov;
            bw = cbw;
        }
        uint32_t c = read_pixel(cpsm == PSMCT24 ? PSMCT32 : cpsm, x, y, cbp, bw);
        if (cpsm == PSMCT32 || cpsm == PSMCT24) {
            uint32_t k = (off + i) & 255;
            st.clut[k] = (uint16_t)c;
            st.clut[k + 256] = (uint16_t)(c >> 16);
        } else {
            st.clut[(off + i) & 511] = (uint16_t)c;
        }
    }
}

}  // namespace gs
