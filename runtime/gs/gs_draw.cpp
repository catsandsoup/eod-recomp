// gs_draw.cpp - software rasteriser implementing the GS pixel pipeline.
//
// Coverage: a pixel (x, y) is drawn when the point (x, y) of the 12.4
// vertex space lies inside the primitive (top-left fill rule); sprites
// cover [x0, x1) x [y0, y1).
#include <algorithm>
#include <atomic>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#if defined(__linux__)
#include <pthread.h>
#endif

#include "../core/config.h"
#include "../core/log.h"
#include "../core/prof.h"
#include "../platform/platform.h"
#include "gs_env.h"
#include "gs_hw.h"
#include "gs_mem.h"
#include "gs_state.h"
#include "gs_swizzle.h"

namespace gs {

bool g_trace;  // log every primitive of one frame (EOD_GS_TRACE=frame)
extern std::atomic<uint64_t> g_flips, g_game_flips, g_ee_vblanks, g_gs_vblanks;
bool g_skip_frame;  // frame skipping: primitives of the current game frame are dropped
bool g_replaying;   // GS replay: never skip
uint32_t g_reg_gen = 1;
Env g_env;
uint32_t g_env_serial;
uint32_t g_clut_gen = 1;
// EOD_GS_REPLAY_STOP=n (replays): primitives after the n-th and later transfers are dropped, so the GS
// memory at that point can be dumped (EOD_GS_REPLAY_DUMP) and compared between renderers
uint64_t g_prim_counter;
uint64_t g_replay_stop = getenv("EOD_GS_REPLAY_STOP") ? strtoull(getenv("EOD_GS_REPLAY_STOP"), nullptr, 10) : 0;
bool replay_frozen() { return g_replay_stop && g_replaying && g_prim_counter >= g_replay_stop; }
static bool count_prim() {
    if (!g_replaying || !g_replay_stop) return true;
    if (g_prim_counter >= g_replay_stop) return false;
    g_prim_counter++;
    return true;
}

namespace {

// Statistics per rasterising thread (0 = GS thread, 1.. raster workers).
constexpr int kMaxWorkers = 4;
struct alignas(64) WStats {
    uint32_t pixels, decoded, slow_samples;
    uint64_t raster_ns;
    uint32_t kind_px[64];
};
WStats g_wstats[kMaxWorkers + 1];
thread_local int t_wid;

int g_nw;                          // raster workers (0: rasterise on the GS thread)
PageSet g_pend_read, g_pend_write;  // pages queued jobs read / write
void barrier();
int g_barrier_cause;  // statistics: what the next barrier is for
const struct Env *g_env_snap;  // snapshot of g_env referenced by queued jobs

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

// ------------------------------------------------------------ texture cache
// Textures are decoded to linear RGBA once and reused until a page they
// live in is written (transfer or rendering) or the CLUT/TEXA change.
bool g_draw_dirty;  // primitives were drawn with the current env since its pages were marked

void mark_env_written(const Env &e) {
    int h = e.scy1 + 1;
    vram_mark(e.fbp, e.fbw, e.fpsm_mem, 0, 0, (int)e.fbw * 64, h);
    if (!e.zmsk && e.ztst != 0) vram_mark(e.zbp, e.fbw, e.zpsm, 0, 0, (int)e.fbw * 64, h);
}

inline void note_draw() { g_draw_dirty = true; }

struct TexKey {
    uint32_t tbp, tbw, psm, w, h, pal_hash, texa;
    bool operator==(const TexKey &o) const {
        return tbp == o.tbp && tbw == o.tbw && psm == o.psm && w == o.w && h == o.h && pal_hash == o.pal_hash &&
               texa == o.texa;
    }
};
struct TexKeyHash {
    size_t operator()(const TexKey &k) const {
        uint64_t h = k.tbp * 0x9E3779B97F4A7C15ull;
        h ^= (k.tbw | (k.psm << 8) | (k.w << 16) | ((uint64_t)k.h << 32)) * 0xC2B2AE3D27D4EB4Full;
        h ^= ((uint64_t)k.pal_hash << 32 | k.texa) * 0x165667B19E3779F9ull;
        return (size_t)(h ^ (h >> 29));
    }
};
}  // namespace
// A texture level decoded on demand in 8x8 texel tiles.
struct TexEntry {
    std::vector<uint32_t> data;
    std::vector<uint8_t> valid;  // per tile
    int w, h, tiles_w, lvl;
    uint32_t stamp;  // g_vram_clock when the tiles were last invalidated
};
namespace {
// never destroyed: the GS thread may still be drawing while the process exits
std::unordered_map<TexKey, TexEntry, TexKeyHash> &g_tex_cache = *new std::unordered_map<TexKey, TexEntry, TexKeyHash>;
size_t g_tex_cache_bytes;

uint32_t fetch(const Env &e, int u, int v, int lvl);

bool pages_overlap(uint32_t f0, uint32_t c0, uint32_t f1, uint32_t c1) {
    for (uint32_t i = 0; i < c0; i++) {
        uint32_t p = (f0 + i) % kPages;
        if (((p - f1) % kPages) < c1) return true;
    }
    return false;
}

// Decoded level `lvl` of the current texture, or null when it can't be cached.
TexEntry *tex_lookup(const Env &e, int lvl) {
    int w = (int)e.tw >> lvl;
    int h = (int)e.th >> lvl;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w * h > 1024 * 1024) return nullptr;
    uint32_t first, count;
    page_range(e.mip_tbp[lvl], e.mip_tbw[lvl], e.tpsm, 0, 0, w, h, first, count);
    // a texture inside the buffers being drawn changes under our feet
    uint32_t ff, fc;
    page_range(e.fbp, e.fbw, e.fpsm_mem, 0, 0, (int)e.fbw * 64, e.scy1 + 1, ff, fc);
    if (pages_overlap(first, count, ff, fc)) return nullptr;
    if (!e.zmsk) {
        page_range(e.zbp, e.fbw, e.zpsm, 0, 0, (int)e.fbw * 64, e.scy1 + 1, ff, fc);
        if (pages_overlap(first, count, ff, fc)) return nullptr;
    }
    TexKey k = {e.mip_tbp[lvl], e.mip_tbw[lvl], e.tpsm, (uint32_t)w, (uint32_t)h, 0, 0};
    bool four = e.tpsm == PSMT4 || e.tpsm == PSMT4HL || e.tpsm == PSMT4HH;
    bool eight = e.tpsm == PSMT8 || e.tpsm == PSMT8H;
    if (four || eight) {
        k.pal_hash = (uint32_t)(e.pal_hash ^ (e.pal_hash >> 32));
    } else if (e.tpsm != PSMCT32 && e.tpsm != PSMZ32) {
        k.texa = e.ta0 | (e.ta1 << 8) | ((uint32_t)e.aem << 16);
    }
    auto it = g_tex_cache.find(k);
    if (it != g_tex_cache.end()) {
        TexEntry &t = it->second;
        bool ok = true;
        for (uint32_t i = 0; i < count && ok; i++)
            if (g_page_gen[(first + i) % kPages] >= t.stamp) ok = false;
        if (!ok) {
            std::fill(t.valid.begin(), t.valid.end(), 0);
            t.stamp = ++g_vram_clock;
        }
        return &t;
    }
    if (g_tex_cache_bytes > (96u << 20)) {
        barrier();
        g_tex_cache.clear();
        g_tex_cache_bytes = 0;
    }
    TexEntry &t = g_tex_cache.emplace(k, TexEntry()).first->second;
    g_tex_cache_bytes += (size_t)w * h * 4;
    t.w = w;
    t.h = h;
    t.lvl = lvl;
    t.tiles_w = (w + 7) >> 3;
    t.data.resize((size_t)w * h);
    t.valid.assign((size_t)t.tiles_w * ((h + 7) >> 3), 0);
    t.stamp = ++g_vram_clock;
    return &t;
}

// Raster workers may decode the same tile at once: they write the same
// texels, and the flag is published after the data.
inline uint32_t expand16(uint32_t c, const Env &e);

__attribute__((noinline)) void decode_tile(const Env &e, TexEntry &t, int tx, int ty) {
    int x0 = tx * 8, y0 = ty * 8;
    int x1 = std::min(x0 + 8, t.w), y1 = std::min(y0 + 8, t.h);
    g_wstats[t_wid].decoded += (uint32_t)((x1 - x0) * (y1 - y0));
    const SwizzleTab &T = *e.texT[t.lvl];
    const uint32_t bp = e.mip_tbp[t.lvl];
    for (int y = y0; y < y1; y++) {
        uint32_t *d = &t.data[(size_t)y * t.w];
        uint32_t rb = T.row_base(bp, y), rc = T.rowC[y & 2047];
        switch (e.tpsm) {
        case PSMCT32:
        case PSMZ32:
            for (int x = x0; x < x1; x++) d[x] = word(T.addr_row(rb, rc, x));
            break;
        case PSMT8:
            for (int x = x0; x < x1; x++) d[x] = e.pal[byte(T.addr_row(rb, rc, x))];
            break;
        case PSMT4:
            for (int x = x0; x < x1; x++) d[x] = e.pal[nibble(T.addr_row(rb, rc, x))];
            break;
        case PSMCT16:
        case PSMCT16S:
        case PSMZ16:
        case PSMZ16S:
            for (int x = x0; x < x1; x++) d[x] = expand16(half(T.addr_row(rb, rc, x)), e);
            break;
        default:
            for (int x = x0; x < x1; x++) d[x] = fetch(e, x, y, t.lvl);
            break;
        }
    }
    __atomic_store_n(&t.valid[ty * t.tiles_w + tx], (uint8_t)1, __ATOMIC_RELEASE);
}

inline uint32_t texel(const Env &e, TexEntry &t, int x, int y) {
    if (!__atomic_load_n(&t.valid[(y >> 3) * t.tiles_w + (x >> 3)], __ATOMIC_ACQUIRE)) decode_tile(e, t, x >> 3, y >> 3);
    return t.data[(size_t)y * t.w + x];
}

void setup_env(uint32_t attr) {
    attr &= ~7u;  // the primitive type doesn't matter here
    if (attr == g_env_attr && g_reg_gen == g_env_gen) return;
    g_env_attr = attr;
    g_env_gen = g_reg_gen;
    g_env_serial++;
    stats.env_setups++;
    ProfScope prof(P_GS_STATE);
    g_env_snap = nullptr;
    Env &e = g_env;
    if (g_draw_dirty) {
        mark_env_written(e);
        g_draw_dirty = false;
    }
    {
        // Game frames end with drawing into the displayed buffer (a flip, or
        // a copy of the finished image).  When the next frame starts and the
        // GS thread trails the EE, the whole frame is skipped (EOD_FRAMESKIP=0
        // disables; at most 7 frames in a row, so slow CPUs still see about
        // 8 drawn frames per second while the game keeps its speed).
        static bool presenting;
        static int skipped;
        static const int forced = getenv("EOD_FRAMESKIP") ? atoi(getenv("EOD_FRAMESKIP")) != 0 : -1;
        const bool enabled = forced >= 0 ? forced != 0 : g_config.frame_skip;  // (the in-game menu changes it)
        uint32_t fbp = bits(st.ctx[(attr >> 9) & 1].frame, 0, 9) * 32;
        uint32_t shown = bits(st.dispfb[(st.pmode & 2) ? 1 : 0], 0, 9) * 32;
        if (fbp != e.fbp && fbp == shown) {
            if (!g_skip_frame) g_flips++;
            g_game_flips++;
            presenting = true;
        } else if (fbp != shown && presenting) {
            presenting = false;
            uint64_t lag = g_ee_vblanks.load() - g_gs_vblanks.load();
            g_skip_frame = enabled && !g_replaying && lag >= 2 && lag < (1u << 30) && skipped < 7;
            static const bool skip_all = getenv("EOD_GS_SKIPALL") != nullptr;  // benchmarks: GS parsing only
            if (skip_all) g_skip_frame = true;
            skipped = g_skip_frame ? skipped + 1 : 0;
            if (g_skip_frame) stats.skipped++;
        }
    }
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
    // A depth format used as the frame buffer keeps its own memory layout
    // but stores colours like the matching colour format.
    e.fbT = swizzle_table(e.fpsm, e.fbw);
    e.fpsm_mem = e.fpsm;
    {
        uint32_t first, count;
        page_range(e.fbp, e.fbw, e.fpsm, 0, 0, (int)e.fbw * 64, e.scy1 + 1, first, count);
        e.fb_ps.clear();
        e.fb_ps.add(first, count);
        page_range(e.zbp, e.fbw, e.zpsm, 0, 0, (int)e.fbw * 64, e.scy1 + 1, first, count);
        e.z_ps.clear();
        e.z_ps.add(first, count);
    }
    if ((e.fpsm & 0x30) == 0x30) e.fpsm &= ~0x30u;
    e.fb16 = e.fpsm == PSMCT16 || e.fpsm == PSMCT16S;
    e.z16 = e.zpsm == PSMZ16 || e.zpsm == PSMZ16S;
    uint32_t full = e.fpsm == PSMCT24 ? 0xFFFFFFu : 0xFFFFFFFFu;
    e.write_none = ((e.fbmsk & full) == full && e.zmsk) || e.ztst == 0;
    e.zmax = e.z16 ? 0xFFFFu : e.zpsm == PSMZ24 ? 0xFFFFFFu : 0xFFFFFFFFu;
    e.blend_kind = (e.ba == 0 && e.bb == 1 && e.bd == 1) ? BL_MIX : (e.ba == 0 && e.bb == 2 && e.bd == 1) ? BL_ADD
                                                                                                    : BL_GENERIC;
    e.pix_kind = (e.tme ? 1 : 0) | (e.ztst >= 2 ? 2 : 0) | (!e.zmsk && e.ztst != 0 ? 4 : 0);
    e.need_dst = e.date || (e.abe && (e.ba == 1 || e.bb == 1 || e.bd == 1 || e.bc == 1)) || e.fbmsk;
    e.need_z = e.ztst != 1 || !e.zmsk;
    e.zT = swizzle_table(e.zpsm, e.fbw);
    if (e.tme) {
        int levels = (e.mmin >= 2 && e.mxl > 0) ? e.mxl : 0;
        for (int i = 0; i <= levels && i < 7; i++) e.texT[i] = swizzle_table(e.tpsm, e.mip_tbw[i]);
        bool four = e.tpsm == PSMT4 || e.tpsm == PSMT4HL || e.tpsm == PSMT4HH;
        bool eight = e.tpsm == PSMT8 || e.tpsm == PSMT8H;
        if (four || eight) {
            // the expanded palette only changes with the CLUT buffer and what selects / expands its entries
            static uint32_t pal_gen;
            static uint64_t pal_sel = ~0ull;
            uint64_t sel = (uint64_t)e.cpsm | (uint64_t)e.csa << 4 | (uint64_t)four << 9 | (uint64_t)e.ta0 << 10 |
                           (uint64_t)e.ta1 << 18 | (uint64_t)e.aem << 26;
            if (pal_gen != g_clut_gen || pal_sel != sel) {
                pal_gen = g_clut_gen;
                pal_sel = sel;
                uint32_t n = four ? 16 : 256;
                uint64_t h = 0x9E3779B97F4A7C15ull;
                for (uint32_t i = 0; i < n; i++) {
                    e.pal[i] = clut_lookup(i, e, four);
                    h = (h ^ e.pal[i]) * 0x100000001B3ull;
                    h ^= h >> 29;
                }
                e.pal_hash = h;
            }
        }
        if (hw::g_on) {  // the hardware renderer keeps its own textures
            e.tcached = false;
            return;
        }
        e.tex_ps.clear();
        for (int i = 0; i <= levels && i < 7; i++) {
            uint32_t first, count;
            int w = std::max(1, (int)e.tw >> i), h = std::max(1, (int)e.th >> i);
            page_range(e.mip_tbp[i], e.mip_tbw[i], e.tpsm, 0, 0, w, h, first, count);
            e.tex_ps.add(first, count);
        }
        // queued primitives drawing into this texture must finish first
        if (g_nw && e.tex_ps.any(g_pend_write)) {
            g_barrier_cause = 4;
            barrier();
        }
        static bool nocache = getenv("EOD_GS_NOTEXCACHE") != nullptr;
        e.tcached = !nocache;
        for (int i = 0; i <= levels && i < 7 && e.tcached; i++) {
            e.tl[i] = tex_lookup(e, i);
            if (!e.tl[i]) e.tcached = false;
        }
    } else {
        e.tcached = false;
    }
}

// ------------------------------------------------------------- census
// EOD_GS_CENSUS=file: tallies the GS features used (state combinations per
// primitive, rendered buffers read back as textures, transfers and display
// over rendered memory) and writes them at exit.  Development aid.
namespace census {
bool g_on = getenv("EOD_GS_CENSUS") != nullptr;
std::mutex &g_mtx = *new std::mutex;
std::unordered_map<std::string, uint64_t> &g_count = *new std::unordered_map<std::string, uint64_t>;
struct Owner {
    bool set;
    bool z;
    uint32_t bp, bw, psm;
};
Owner g_owner[kPages];

void add(const std::string &k, uint64_t n) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_count[k] += n;
}

void dump() {
    FILE *f = fopen(getenv("EOD_GS_CENSUS"), "w");
    if (!f) return;
    std::vector<std::pair<std::string, uint64_t>> v(g_count.begin(), g_count.end());
    std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (auto &kv : v) fprintf(f, "%12llu  %s\n", (unsigned long long)kv.second, kv.first.c_str());
    fclose(f);
}

void own(uint32_t bp, uint32_t bw, uint32_t psm, int w, int h, bool z) {
    uint32_t first, count;
    page_range(bp, bw, psm, 0, 0, w, h, first, count);
    for (uint32_t i = 0; i < count; i++) g_owner[(first + i) % kPages] = {true, z, bp, bw, psm};
}

std::string owners(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h) {
    uint32_t first, count;
    page_range(bp, bw, psm, x, y, w, h, first, count);
    std::string out;
    for (uint32_t i = 0; i < count; i++) {
        const Owner &o = g_owner[(first + i) % kPages];
        if (!o.set) continue;
        char b[64];
        snprintf(b, sizeof b, "%s%x/%d/%x", o.z ? "Z" : "F", o.bp, o.bw, o.psm);
        if (out.find(b) == std::string::npos) out += std::string(out.empty() ? "" : ",") + b;
    }
    return out;
}

void draw(const Env &e, const char *prim, uint64_t area) {
    static bool reg = (atexit(dump), true);
    (void)reg;
    char k[512];
    snprintf(k, sizeof k,
             "draw %s fb=%x/%d/%x msk=%x z=%x/%x zt=%d zm=%d ate=%d/%d/%x/%d date=%d/%d abe=%d %d%d%d%d fix=%x pabe=%d "
             "clamp=%d fba=%d tme=%d tex=%x/%x cpsm=%x tfx=%d tcc=%d wrap=%d%d mag=%d min=%d mxl=%d fge=%d iip=%d fst=%d",
             prim, e.fbp, e.fbw, e.fpsm_mem, e.fbmsk, e.zbp, e.zpsm, e.ztst, (int)e.zmsk, (int)e.ate, e.atst, e.aref,
             e.afail, (int)e.date, (int)e.datm, (int)e.abe, e.ba, e.bb, e.bc, e.bd, e.bfix, (int)e.pabe,
             (int)e.colclamp, (int)e.fba, (int)e.tme, e.tme ? e.tbp : 0, e.tme ? e.tpsm : 0, e.tme ? e.cpsm : 0,
             e.tme ? e.tfx : 0, e.tme ? (int)e.tcc : 0, e.tme ? e.wms : 0, e.tme ? e.wmt : 0,
             e.tme ? (int)e.bilinear_mag : 0, e.tme ? e.mmin : 0, e.tme ? e.mxl : 0, (int)e.fge, (int)e.iip,
             (int)e.fst);
    std::string key = k;
    add(key, 1);
    add("pixels " + key, area);
    int h = e.scy1 + 1, w = (int)e.fbw * 64;
    if (e.tme) {
        std::string o = owners(e.tbp, e.tbw, e.tpsm, 0, 0, (int)e.tw, (int)e.th);
        if (!o.empty()) {
            char t[256];
            snprintf(t, sizeof t, "tex-from-rendered tex=%x/%d/%x %dx%d <- %s (draw fb=%x/%x)", e.tbp, e.tbw, e.tpsm,
                     e.tw, e.th, o.c_str(), e.fbp, e.fpsm_mem);
            add(t, 1);
        }
    }
    uint32_t full = (e.fpsm_mem & 0xF) == PSMCT24 ? 0xFFFFFFu : 0xFFFFFFFFu;
    if ((e.fbmsk & full) != full) own(e.fbp, e.fbw, e.fpsm_mem, w, h, false);
    if (!e.zmsk && e.ztst != 0) own(e.zbp, e.fbw, e.zpsm, w, h, true);
}

}  // namespace census

void census_xfer_impl(int dir, uint32_t sbp, uint32_t sbw, uint32_t spsm, uint32_t dbp, uint32_t dbw, uint32_t dpsm,
                 int sx, int sy, int dx, int dy, int w, int h) {
    if (!census::g_on) return;
    char k[256];
    std::string so = dir == 0 ? "" : census::owners(sbp, sbw, spsm, sx, sy, w, h);
    std::string d = dir == 1 ? "" : census::owners(dbp, dbw, dpsm, dx, dy, w, h);
    snprintf(k, sizeof k, "xfer dir=%d src=%x/%d/%x [%s] dst=%x/%d/%x [%s] %dx%d", dir, dir ? sbp : 0, dir ? sbw : 0,
             dir ? spsm : 0, so.c_str(), dir != 1 ? dbp : 0, dir != 1 ? dbw : 0, dir != 1 ? dpsm : 0, d.c_str(), w,
             h);
    census::add(k, 1);
    if (dir != 1) {
        uint32_t first, count;
        page_range(dbp, dbw, dpsm, dx, dy, w, h, first, count);
        for (uint32_t i = 0; i < count; i++) census::g_owner[(first + i) % kPages].set = false;
    }
}

void census_display_impl(uint32_t fbp, uint32_t fbw, uint32_t psm, int w, int h) {
    if (!census::g_on) return;
    char k[256];
    snprintf(k, sizeof k, "display fb=%x/%d/%x %dx%d <- %s", fbp, fbw, psm, w, h,
             census::owners(fbp, fbw, psm, 0, 0, w, h).c_str());
    census::add(k, 1);
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

#if defined(__SSE2__)
// Same result as bilerp() below: ((c00*gu + c10*fu)*gv + (c01*gu + c11*fu)*fv) >> 8 per channel.
inline uint32_t bilerp_sse(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11, int fu, int fv) {
    const __m128i z = _mm_setzero_si128();
    __m128i top = _mm_unpacklo_epi8(_mm_unpacklo_epi32(_mm_cvtsi32_si128((int)c00), _mm_cvtsi32_si128((int)c10)), z);
    __m128i bot = _mm_unpacklo_epi8(_mm_unpacklo_epi32(_mm_cvtsi32_si128((int)c01), _mm_cvtsi32_si128((int)c11)), z);
    // horizontal weights: low 4 lanes (16 - fu), high 4 lanes fu
    __m128i wh = _mm_unpacklo_epi64(_mm_set1_epi16((short)(16 - fu)), _mm_set1_epi16((short)fu));
    top = _mm_mullo_epi16(top, wh);  // <= 255*16
    bot = _mm_mullo_epi16(bot, wh);
    top = _mm_add_epi16(top, _mm_srli_si128(top, 8));  // row sums in the low 4 lanes (<= 4080)
    bot = _mm_add_epi16(bot, _mm_srli_si128(bot, 8));
    __m128i v = _mm_add_epi16(_mm_mullo_epi16(top, _mm_set1_epi16((short)(16 - fv))),
                              _mm_mullo_epi16(bot, _mm_set1_epi16((short)fv)));  // <= 65280
    v = _mm_srli_epi16(v, 8);
    return (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(v, v));
}
#endif

inline uint32_t bilerp(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11, int fu, int fv) {
#if defined(__SSE2__)
    if (1) return bilerp_sse(c00, c10, c01, c11, fu, fv);
#endif
    const uint32_t M = 0x00FF00FFu;
    uint32_t gu = 16 - (uint32_t)fu, gv = 16 - (uint32_t)fv;
    uint32_t rb_t = (c00 & M) * gu + (c10 & M) * (uint32_t)fu;
    uint32_t rb_b = (c01 & M) * gu + (c11 & M) * (uint32_t)fu;
    uint32_t ag_t = ((c00 >> 8) & M) * gu + ((c10 >> 8) & M) * (uint32_t)fu;
    uint32_t ag_b = ((c01 >> 8) & M) * gu + ((c11 >> 8) & M) * (uint32_t)fu;
    uint32_t rb = ((rb_t * gv + rb_b * (uint32_t)fv) >> 8) & M;
    uint32_t ag = (ag_t * gv + ag_b * (uint32_t)fv) & ~M;
    return rb | ag;
}

// Sampling from a decoded texture level.
__attribute__((always_inline)) inline uint32_t sample_cached(const Env &e, int u, int v, bool bilinear, int lvl) {
    TexEntry &T = *e.tl[lvl];
    int tw = T.w, th = T.h;
    if (lvl) {
        u >>= lvl;
        v >>= lvl;
    }
    if (!bilinear) {
        int x = wrap(u >> 4, tw, e.wms, e.minu, e.maxu);
        int y = wrap(v >> 4, th, e.wmt, e.minv, e.maxv);
        return texel(e, T, x, y);
    }
    u -= 8;
    v -= 8;
    int x0 = u >> 4, y0 = v >> 4;
    if ((e.wms | e.wmt) == 0) {
        // REPEAT in both directions (sizes are powers of two)
        int xa = x0 & (tw - 1), xb = (x0 + 1) & (tw - 1), ya = y0 & (th - 1), yb = (y0 + 1) & (th - 1);
        const uint8_t *V = T.valid.data();
        int ra = (ya >> 3) * T.tiles_w, rb = (yb >> 3) * T.tiles_w, ca = xa >> 3, cb = xb >> 3;
        if (!(__atomic_load_n(&V[ra + ca], __ATOMIC_ACQUIRE) & __atomic_load_n(&V[ra + cb], __ATOMIC_ACQUIRE) &
              __atomic_load_n(&V[rb + ca], __ATOMIC_ACQUIRE) & __atomic_load_n(&V[rb + cb], __ATOMIC_ACQUIRE))) {
            texel(e, T, xa, ya);
            texel(e, T, xb, ya);
            texel(e, T, xa, yb);
            texel(e, T, xb, yb);
        }
        const uint32_t *r0 = &T.data[(size_t)ya * tw], *r1 = &T.data[(size_t)yb * tw];
        return bilerp(r0[xa], r0[xb], r1[xa], r1[xb], u & 15, v & 15);
    }
    int xa = wrap(x0, tw, e.wms, e.minu, e.maxu), xb = wrap(x0 + 1, tw, e.wms, e.minu, e.maxu);
    int ya = wrap(y0, th, e.wmt, e.minv, e.maxv), yb = wrap(y0 + 1, th, e.wmt, e.minv, e.maxv);
    if (((xa ^ xb) | (ya ^ yb)) < 8 && (xa >> 3) == (xb >> 3) && (ya >> 3) == (yb >> 3)) {
        // all four texels in one tile: one check
        texel(e, T, xa, ya);
        const uint32_t *r0 = &T.data[(size_t)ya * tw], *r1 = &T.data[(size_t)yb * tw];
        return bilerp(r0[xa], r0[xb], r1[xa], r1[xb], u & 15, v & 15);
    }
    return bilerp(texel(e, T, xa, ya), texel(e, T, xb, ya), texel(e, T, xa, yb), texel(e, T, xb, yb), u & 15, v & 15);
}

// u, v in 1/16 texel units at mip level `lvl`.
uint32_t sample(const Env &e, int u, int v, bool bilinear, int lvl) {
    if (e.tcached) return sample_cached(e, u, v, bilinear, lvl);
    g_wstats[t_wid].slow_samples++;
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
    return bilerp(c00, c10, c01, c11, fu, fv);
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
inline int ffloorf(float d) {
    int i = (int)d;
    return i - (d < (float)i);
}

// ------------------------------------------------------------- pixel
struct Frag {
    int r, g, b, a;  // vertex colour 0-255
    uint32_t z;
    int fog;
    int u = 0, v = 0;  // texel coords (1/16) when textured
    int lod;         // mip level
};

// One pixel inside the scissor rectangle.
int g_dbg_x = -1, g_dbg_y = -1;  // EOD_DEBUG_PIXEL=x,y: log writes to that pixel in traced frames
struct DbgPixelInit {
    DbgPixelInit() {
        if (const char *p = getenv("EOD_DEBUG_PIXEL")) sscanf(p, "%d,%d", &g_dbg_x, &g_dbg_y);
    }
} g_dbg_pixel_init;

// TEX: texture mapping, ZT: depth compare (GEQUAL/GREATER), ZW: depth write.
template <bool TEX, bool ZT, bool ZW>
__attribute__((always_inline)) inline void shade_px_t(const Env &e, int x, const RowAddr &ra, const Frag &f) {
    if (g_trace && x == g_dbg_x && ra.y == g_dbg_y) {
        uint32_t t = e.tme ? sample(e, f.u, f.v, f.lod > 0 ? e.bilinear_min : e.bilinear_mag, f.lod > 0 ? f.lod : 0) : 0;
        log_write(LOG_GS, "PIXEL %d,%d: tex=%x/%d psm=%x %dx%d uv=%d,%d (%.2f,%.2f) lod=%d texel=%08x col=%02x%02x%02x%02x z=%u",
                  x, ra.y, e.tbp, e.tbw, e.tpsm, e.tw, e.th, f.u, f.v, f.u / 16.0, f.v / 16.0, f.lod, t, f.r, f.g, f.b, f.a, f.z);
    }
    // ---- depth test
    uint32_t za = 0, z = 0;
    if (ZT || ZW) {
        z = f.z > e.zmax ? e.zmax : f.z;
        za = e.zT->addr_row(ra.z, ra.zc, x);
    }
    if (ZT) {
        uint32_t zb = e.z16 ? half(za) : word(za) & e.zmax;
        if (e.ztst == 2 ? z < zb : z <= zb) return;
    }
    // ---- texture + colour
    int r = f.r, g = f.g, b = f.b, a = f.a;
    if (TEX) {
        bool bl = f.lod > 0 ? e.bilinear_min : e.bilinear_mag;
        int lv = f.lod > 0 ? f.lod : 0;
        uint32_t t = e.tcached ? sample_cached(e, f.u, f.v, bl, lv) : sample(e, f.u, f.v, bl, lv);
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
    bool write_fb = true, write_z = ZW, write_alpha = true;
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
    if (write_fb && e.abe && !(e.pabe && a < 0x80) && e.blend_kind != BL_GENERIC) {
        int dr = (int)(dst & 255), dg = (int)((dst >> 8) & 255), db = (int)((dst >> 16) & 255);
        int cc = e.bc == 0 ? a : e.bc == 1 ? (int)(dst >> 24) : (int)e.bfix;
        if (e.blend_kind == BL_MIX) {  // (Cs - Cd) * C + Cd
            r = (((r - dr) * cc) >> 7) + dr;
            g = (((g - dg) * cc) >> 7) + dg;
            b = (((b - db) * cc) >> 7) + db;
        } else {  // BL_ADD: Cs * C + Cd
            r = ((r * cc) >> 7) + dr;
            g = ((g * cc) >> 7) + dg;
            b = ((b * cc) >> 7) + db;
        }
        if (e.colclamp) {
            r = clamp255(r);
            g = clamp255(g);
            b = clamp255(b);
        } else {
            r &= 255;
            g &= 255;
            b &= 255;
        }
    } else if (write_fb && e.abe && !(e.pabe && a < 0x80)) {
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
        if (g_trace && x == g_dbg_x && ra.y == g_dbg_y)
            log_write(LOG_GS, "PIXEL   dst=%08x -> %08x (wz=%d)", dst, e.fb16 ? half(fa) : word(fa), (int)write_z);
    }
    if (ZW && write_z) {
        if (e.z16)
            half(za) = (uint16_t)z;
        else if (e.zpsm == PSMZ24)
            word(za) = (word(za) & 0xFF000000u) | z;
        else
            word(za) = z;
    }
}

inline void shade_px(const Env &e, int x, const RowAddr &ra, const Frag &f) {
    switch (e.pix_kind) {
    case 0: shade_px_t<false, false, false>(e, x, ra, f); break;
    case 1: shade_px_t<true, false, false>(e, x, ra, f); break;
    case 2: shade_px_t<false, true, false>(e, x, ra, f); break;
    case 3: shade_px_t<true, true, false>(e, x, ra, f); break;
    case 4: shade_px_t<false, false, true>(e, x, ra, f); break;
    case 5: shade_px_t<true, false, true>(e, x, ra, f); break;
    case 6: shade_px_t<false, true, true>(e, x, ra, f); break;
    default: shade_px_t<true, true, true>(e, x, ra, f); break;
    }
}

void shade(const Env &e, int x, int y, const Frag &f) {
    if (x < e.scx0 || x > e.scx1 || y < e.scy0 || y > e.scy1) return;
    g_wstats[t_wid].pixels++;
    RowAddr ra;
    row_addr(e, y, ra);
    shade_px(e, x, ra, f);
}

inline bool getenv_nomip() {
    static bool nomip = getenv("EOD_DEBUG_NOMIP") != nullptr;
    return nomip;
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

// Sprite rows; texture coordinates step in 16.16 fixed point.
// Rows are dealt to raster workers in groups of 4: worker `wid` of `nw`
// draws the rows y with (y / 4) % nw == wid.
constexpr int kRowShift = 3;  // rows per group: 1 << kRowShift
inline bool skip_row(int &y, int wid, int nw) {
    if (nw > 1 && ((y >> kRowShift) % nw) != wid) {
        y |= (1 << kRowShift) - 1;
        return true;
    }
    return false;
}

template <bool TEX, bool ZT, bool ZW>
void sprite_rows(const Env &e, int x0, int x1, int y0, int y1, Frag f, double u0, double du, double w0, double dw,
                 int wid, int nw, WStats &ws) {
    const int64_t ui = (int64_t)(u0 * 65536.0), d = (int64_t)(du * 65536.0);
    for (int y = y0; y < y1; y++) {
        if (skip_row(y, wid, nw)) continue;
        ws.pixels += (uint32_t)(x1 - x0);
        RowAddr ra;
        row_addr(e, y, ra);
        if (TEX) f.v = ffloor(w0 + (y - y0) * dw);
        int64_t u = ui;
        for (int x = x0; x < x1; x++, u += d) {
            if (TEX) f.u = (int)(u >> 16);
            shade_px_t<TEX, ZT, ZW>(e, x, ra, f);
        }
    }
}

// Untextured, unblended, untested sprite (typically a clear): constant colour and depth.
void fill_rows(const Env &e, int x0, int x1, int y0, int y1, const Frag &f, int wid, int nw, WStats &ws) {
    uint32_t oa = (uint32_t)(f.a | (e.fba ? 0x80 : 0)) & 255;
    uint32_t col = (uint32_t)f.r | ((uint32_t)f.g << 8) | ((uint32_t)f.b << 16) | (oa << 24);
    uint32_t msk = e.fbmsk;
    if (!e.fb16 && e.fpsm == PSMCT24) msk |= 0xFF000000u;
    bool full_mask = e.fb16 ? (msk & 0x80F8F8F8u) == 0x80F8F8F8u : msk == 0xFFFFFFFFu;
    uint16_t c16 = (uint16_t)(((col >> 3) & 0x1F) | (((col >> 11) & 0x1F) << 5) | (((col >> 19) & 0x1F) << 10) |
                              ((col >> 31) << 15));
    uint16_t m16 = (uint16_t)(((msk >> 3) & 0x1F) | (((msk >> 11) & 0x1F) << 5) | (((msk >> 19) & 0x1F) << 10) |
                              ((msk >> 31) << 15));
    bool zw = !e.zmsk;
    uint32_t z = f.z > e.zmax ? e.zmax : f.z;
    for (int y = y0; y < y1; y++) {
        if (skip_row(y, wid, nw)) continue;
        ws.pixels += (uint32_t)(x1 - x0);
        RowAddr ra;
        row_addr(e, y, ra);
        if (!full_mask) {
            if (e.fb16) {
                for (int x = x0; x < x1; x++) {
                    uint16_t &p = half(e.fbT->addr_row(ra.fb, ra.fbc, x));
                    p = (uint16_t)((c16 & ~m16) | (p & m16));
                }
            } else if (!msk) {
                for (int x = x0; x < x1; x++) word(e.fbT->addr_row(ra.fb, ra.fbc, x)) = col;
            } else {
                for (int x = x0; x < x1; x++) {
                    uint32_t &p = word(e.fbT->addr_row(ra.fb, ra.fbc, x));
                    p = (col & ~msk) | (p & msk);
                }
            }
        }
        if (zw) {
            if (e.z16) {
                for (int x = x0; x < x1; x++) half(e.zT->addr_row(ra.z, ra.zc, x)) = (uint16_t)z;
            } else if (e.zpsm == PSMZ24) {
                for (int x = x0; x < x1; x++) {
                    uint32_t &p = word(e.zT->addr_row(ra.z, ra.zc, x));
                    p = (p & 0xFF000000u) | z;
                }
            } else {
                for (int x = x0; x < x1; x++) word(e.zT->addr_row(ra.z, ra.zc, x)) = z;
            }
        }
    }
}

// Fast log2 (piecewise linear in the mantissa, error < 0.09): enough to
// pick a mip level.
inline float fast_log2(float x) {
    uint32_t b;
    memcpy(&b, &x, 4);
    return (float)((int32_t)(b & 0x7FFFFFFFu) - (127 << 23)) * (1.0f / (1 << 23));
}

// Triangle set up for scan conversion: edges and attribute planes.
enum { A_R, A_G, A_B, A_A, A_Z, A_F, A_S, A_T, A_Q, A_N };
struct TriSpans {
    struct Edge {
        int64_t dx, dy, xa, ya;
        int bias;
    } ed[3];
    double base[A_N], gx[A_N], gy[A_N];
    double X0, Y0;
    int px0, px1, py0, py1;
};

// Scan converts a triangle.  GOUR: Gouraud colours, TEX: textured, STQ:
// perspective texture coordinates, FOG: fogging, ZT/ZW: depth test/write.
template <bool GOUR, bool TEX, bool STQ, bool FOG, bool ZT, bool ZW>
void tri_spans(const Env &e, const TriSpans &ts, int wid, int nw, WStats &ws) {
    constexpr int pk = (TEX ? 1 : 0) | (ZT ? 2 : 0) | (ZW ? 4 : 0);
    constexpr bool NEEDZ = ZT || ZW;
    const bool mip = STQ && e.mmin >= 2 && e.mxl > 0 && !getenv_nomip();
    const float tw16 = (float)(e.tw * 16), th16 = (float)(e.th * 16);
    const float lod_scale = (float)(1 << e.l);
    // fixed point steps (16.16) for colour and fog, 48.16 for depth
    const int32_t dr = (int32_t)(ts.gx[A_R] * 65536.0), dg = (int32_t)(ts.gx[A_G] * 65536.0);
    const int32_t db = (int32_t)(ts.gx[A_B] * 65536.0), da = (int32_t)(ts.gx[A_A] * 65536.0);
    const int32_t dfo = (int32_t)(ts.gx[A_F] * 65536.0);
    const int64_t dz = (int64_t)(ts.gx[A_Z] * 65536.0);
    const float ds = (float)ts.gx[A_S], dt = (float)ts.gx[A_T], dq = (float)ts.gx[A_Q];
    Frag f;
    f.lod = 0;
    f.z = 0;
    f.fog = 0;
    if (!GOUR) {
        f.r = clamp255((int)ts.base[A_R]);
        f.g = clamp255((int)ts.base[A_G]);
        f.b = clamp255((int)ts.base[A_B]);
        f.a = clamp255((int)ts.base[A_A]);
    }
    for (int y = ts.py0; y <= ts.py1; y++) {
        if (skip_row(y, wid, nw)) continue;
        int64_t Y = (int64_t)y * 16;
        int xl = ts.px0, xr = ts.px1;
        bool empty = false;
        for (int k = 0; k < 3 && !empty; k++) {
            // E(X) = dy*(X - xa) - (Y - ya)*dx >= bias, X = 16*x
            const TriSpans::Edge &E = ts.ed[k];
            int64_t c = -E.dy * E.xa - (Y - E.ya) * E.dx;
            int64_t A = E.dy * 16;
            if (A > 0) {
                int64_t num = E.bias - c;
                int64_t lo = num >= 0 ? (num + A - 1) / A : -((-num) / A);
                if (lo > xl) xl = (int)std::min<int64_t>(lo, 1 << 20);
            } else if (A < 0) {
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
        double fy = y - ts.Y0, fx = xl - ts.X0;
        auto at = [&](int i) { return ts.base[i] + ts.gx[i] * fx + ts.gy[i] * fy; };
        int32_t r = 0, g = 0, b = 0, a = 0, fo = 0;
        int64_t zz = 0;
        float s = 0, t = 0, q = 0;
        double su = 0, tv = 0;
        if (GOUR) {
            r = (int32_t)(std::max(-32768.0, std::min(32767.0, at(A_R))) * 65536.0);
            g = (int32_t)(std::max(-32768.0, std::min(32767.0, at(A_G))) * 65536.0);
            b = (int32_t)(std::max(-32768.0, std::min(32767.0, at(A_B))) * 65536.0);
            a = (int32_t)(std::max(-32768.0, std::min(32767.0, at(A_A))) * 65536.0);
        }
        if (FOG) fo = (int32_t)(std::max(-32768.0, std::min(32767.0, at(A_F))) * 65536.0);
        if (NEEDZ) {
            double z0 = at(A_Z);
            zz = (int64_t)(std::max(-1e13, std::min(1e13, z0)) * 65536.0);
        }
        if (TEX) {
            if (STQ) {
                s = (float)at(A_S);
                t = (float)at(A_T);
                q = (float)at(A_Q);
            } else {
                su = at(A_S);
                tv = at(A_T);
            }
        }
        for (int x = xl; x <= xr; x++) {
            if (GOUR) {
                f.r = clamp255(r >> 16);
                f.g = clamp255(g >> 16);
                f.b = clamp255(b >> 16);
                f.a = clamp255(a >> 16);
                r += dr;
                g += dg;
                b += db;
                a += da;
            }
            if (NEEDZ) {
                int64_t zi = zz >> 16;
                f.z = zi <= 0 ? 0 : zi >= 0xFFFFFFFFll ? 0xFFFFFFFFu : (uint32_t)zi;
                zz += dz;
            }
            if (FOG) {
                f.fog = clamp255(fo >> 16);
                fo += dfo;
            }
            if (TEX) {
                if (STQ) {
                    float qq = q == 0.0f ? 1e-30f : q;
                    float iq = 1.0f / qq;
                    f.u = ffloorf(s * iq * tw16);
                    f.v = ffloorf(t * iq * th16);
                    if (mip) {
                        float lod = e.lcm ? e.k : (-fast_log2(qq) * lod_scale + e.k);
                        int lvl = lod <= 0 ? 0 : (int)(lod + 0.5f);
                        f.lod = lvl > e.mxl ? e.mxl : lvl;
                    }
                    s += ds;
                    t += dt;
                    q += dq;
                } else {
                    f.u = ffloor(su);
                    f.v = ffloor(tv);
                    su += ts.gx[A_S];
                    tv += ts.gx[A_T];
                }
            }
            shade_px_t<TEX, ZT, ZW>(e, x, ra, f);
        }
        ws.pixels += (uint32_t)(xr - xl + 1);
        ws.kind_px[(GOUR ? 1 : 0) | (TEX ? 2 : 0) | (STQ ? 4 : 0) | (FOG ? 8 : 0) | (ZT ? 16 : 0) | (ZW ? 32 : 0)] +=
            (uint32_t)(xr - xl + 1);
    }
    (void)pk;
}

// ------------------------------------------------------------ raster workers
// With spare cores, primitives are queued as jobs and rasterised by worker
// threads that each own interleaved groups of rows; every worker runs all
// jobs in order, so each pixel still sees the primitives in GS order.  The
// GS thread waits for the workers (a barrier) only when memory that queued
// jobs read or write is about to be accessed another way: render-to-texture,
// transfers, CLUT loads, display readout, or a buffer aliased through a
// different layout.
enum JobKind : uint8_t { JOB_TRI, JOB_SPRITE, JOB_FILL };
struct Job {
    JobKind kind;
    int variant;
    const Env *env;
    TriSpans ts;
    int x0, x1, y0, y1;
    Frag f;
    double u0, du, w0, dw;
};

constexpr uint64_t kRing = 4096;
Job *g_ring;
std::atomic<uint64_t> g_head{0};  // jobs published
struct alignas(64) WorkerSync {
    std::atomic<uint64_t> done{0};
};
WorkerSync g_wsync[kMaxWorkers];
std::atomic<int> g_sleepers{0};
std::atomic<bool> g_producer_waiting{false};
std::mutex &g_wmtx = *new std::mutex;
std::condition_variable &g_wcv = *new std::condition_variable;  // workers wait for jobs
std::condition_variable &g_pcv = *new std::condition_variable;  // the GS thread waits for workers

struct View {
    uint32_t bp, bw, psm;
    PageSet pages;
};
View g_views[8];
int g_nviews;

// Env snapshots referenced by queued jobs (freed at barriers)
std::deque<Env> &g_env_arena = *new std::deque<Env>;

std::atomic<uint32_t> g_dbg_sleeps, g_dbg_notifies, g_dbg_psleeps;
uint64_t g_dbg_wait_ns;

// How long waiting threads spin before sleeping: long on real cores, short
// when they share cores (hyper-threading) with the EE and GS threads.
int g_spin_us = 3000;

// Spins until cond() holds or about `us` microseconds pass (thread wakeups
// can take far longer than the gaps between jobs).
template <class F>
bool spin_until(F cond, int us) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0;; i++) {
        if (cond()) return true;
        if ((i & 63) == 63) {
            if (std::chrono::steady_clock::now() - t0 > std::chrono::microseconds(us)) return false;
            if (i > 4096) std::this_thread::yield();
        }
#if defined(__i386__) || defined(__x86_64__)
        __builtin_ia32_pause();
#endif
    }
}

inline void cpu_relax() {
#if defined(__i386__) || defined(__x86_64__)
    __builtin_ia32_pause();
#endif
}

void run_job(const Job &j, int wid, int nw, WStats &ws) {
    const Env &e = *j.env;
    switch (j.kind) {
    case JOB_TRI:
        switch (j.variant) {
#define TRI_CASE(n)                                                                                                    \
    case n:                                                                                                            \
        tri_spans<(n & 1) != 0, (n & 2) != 0, (n & 4) != 0, (n & 8) != 0, (n & 16) != 0, (n & 32) != 0>(e, j.ts, wid, \
                                                                                                       nw, ws);       \
        break;
            TRI_CASE(0) TRI_CASE(1) TRI_CASE(2) TRI_CASE(3) TRI_CASE(6) TRI_CASE(7) TRI_CASE(8) TRI_CASE(9)
            TRI_CASE(10) TRI_CASE(11) TRI_CASE(14) TRI_CASE(15) TRI_CASE(16) TRI_CASE(17) TRI_CASE(18) TRI_CASE(19)
            TRI_CASE(22) TRI_CASE(23) TRI_CASE(24) TRI_CASE(25) TRI_CASE(26) TRI_CASE(27) TRI_CASE(30) TRI_CASE(31)
            TRI_CASE(32) TRI_CASE(33) TRI_CASE(34) TRI_CASE(35) TRI_CASE(38) TRI_CASE(39) TRI_CASE(40) TRI_CASE(41)
            TRI_CASE(42) TRI_CASE(43) TRI_CASE(46) TRI_CASE(47) TRI_CASE(48) TRI_CASE(49) TRI_CASE(50) TRI_CASE(51)
            TRI_CASE(54) TRI_CASE(55) TRI_CASE(56) TRI_CASE(57) TRI_CASE(58) TRI_CASE(59) TRI_CASE(62) TRI_CASE(63)
#undef TRI_CASE
        default: break;
        }
        break;
    case JOB_SPRITE: {
#define SPR(t, zt, zw) sprite_rows<t, zt, zw>(e, j.x0, j.x1, j.y0, j.y1, j.f, j.u0, j.du, j.w0, j.dw, wid, nw, ws)
        switch (j.variant) {
        case 0: SPR(false, false, false); break;
        case 1: SPR(true, false, false); break;
        case 2: SPR(false, true, false); break;
        case 3: SPR(true, true, false); break;
        case 4: SPR(false, false, true); break;
        case 5: SPR(true, false, true); break;
        case 6: SPR(false, true, true); break;
        default: SPR(true, true, true); break;
        }
#undef SPR
        break;
    }
    case JOB_FILL: fill_rows(e, j.x0, j.x1, j.y0, j.y1, j.f, wid, nw, ws); break;
    }
}

void raster_worker(int id) {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "eod-raster");
#endif
    t_wid = id + 1;
    uint64_t idx = 0;
    for (;;) {
        if (g_head.load() == idx) {
            spin_until([&] { return g_head.load() != idx; }, g_spin_us);
            if (g_head.load() == idx) {
                std::unique_lock<std::mutex> lk(g_wmtx);
                g_dbg_sleeps++;
                g_sleepers++;
                g_wcv.wait(lk, [&] { return g_head.load() != idx; });
                g_sleepers--;
            }
            continue;
        }
        if (g_log_enabled[LOG_GS]) {
            auto t0 = std::chrono::steady_clock::now();
            run_job(g_ring[idx % kRing], id, g_nw, g_wstats[id + 1]);
            g_wstats[id + 1].raster_ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - t0)
                                              .count();
        } else {
            run_job(g_ring[idx % kRing], id, g_nw, g_wstats[id + 1]);
        }
        idx++;
        g_wsync[id].done.store(idx);
        if (g_producer_waiting.load()) {
            std::lock_guard<std::mutex> lk(g_wmtx);
            g_pcv.notify_all();
        }
    }
}

uint64_t min_done() {
    uint64_t m = ~0ull;
    for (int i = 0; i < g_nw; i++) m = std::min(m, g_wsync[i].done.load());
    return m;
}

// Waits until the workers have run all jobs up to `upto`.
void wait_workers(uint64_t upto) {
    auto t0 = std::chrono::steady_clock::now();
    if (spin_until([&] { return min_done() >= upto; }, g_spin_us)) {
        g_dbg_wait_ns +=
            (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        return;
    }
    g_dbg_psleeps++;
    std::unique_lock<std::mutex> lk(g_wmtx);
    g_producer_waiting = true;
    g_pcv.wait(lk, [&] { return min_done() >= upto; });
    g_producer_waiting = false;
    g_dbg_wait_ns +=
        (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
}

void barrier() {
    if (!g_nw) return;
    stats.barriers[g_barrier_cause & 7]++;
    g_barrier_cause = 0;
    if (g_head.load() != min_done()) wait_workers(g_head.load());
    g_pend_read.clear();
    g_pend_write.clear();
    g_nviews = 0;
    g_env_arena.clear();
    g_env_snap = nullptr;
}

void push_job(Job &j) {
    uint64_t h = g_head.load();
    if (h - min_done() >= kRing) wait_workers(h - kRing + 1);
    if (!g_env_snap) {
        g_env_arena.push_back(g_env);
        g_env_snap = &g_env_arena.back();
    }
    j.env = g_env_snap;
    stats.jobs++;
    g_ring[h % kRing] = j;
    g_head.store(h + 1);
    if (g_sleepers.load()) {
        g_dbg_notifies++;
        std::lock_guard<std::mutex> lk(g_wmtx);
        g_wcv.notify_all();
    }
}

// Registers a buffer layout used by the queued jobs; false when it overlaps
// another layout's memory (rows wouldn't map to the same workers).
bool add_view(uint32_t bp, uint32_t bw, uint32_t psm, const PageSet &pages) {
    for (int i = 0; i < g_nviews; i++) {
        View &v = g_views[i];
        if (v.bp == bp && v.bw == bw && v.psm == psm) return true;
        if (v.pages.any(pages)) return false;
    }
    if (g_nviews == 8) return false;
    g_views[g_nviews++] = {bp, bw, psm, pages};
    return true;
}

// Queues a job for the current env (or runs it now without workers).
void submit(Job &j) {
    if (g_skip_frame) return;
    if (!g_nw || g_trace) {
        if (g_nw) barrier();
        j.env = &g_env;
        if (!g_log_enabled[LOG_GS]) {
            run_job(j, 0, 1, g_wstats[0]);
            return;
        }
        auto t0 = std::chrono::steady_clock::now();
        run_job(j, 0, 1, g_wstats[0]);
        g_wstats[0].raster_ns +=
            (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        return;
    }
    const Env &e = g_env;
    bool fb_w = true;  // write_none primitives never get here
    bool zt = e.ztst >= 2, zw = !e.zmsk && e.ztst != 0;
    PageSet wr;
    wr.clear();
    if (fb_w) wr.add(e.fb_ps);
    if (zw) wr.add(e.z_ps);
    bool hazard = (e.tme && e.tex_ps.any(g_pend_write)) || wr.any(g_pend_read);
    if (!hazard) {
        if (!add_view(e.fbp, e.fbw, e.fpsm_mem, e.fb_ps)) hazard = true;
        else if ((zt || zw) && !add_view(e.zbp, e.fbw, e.zpsm, e.z_ps)) hazard = true;
    }
    if (hazard) {
        g_barrier_cause = (e.tme && e.tex_ps.any(g_pend_write)) ? 1 : wr.any(g_pend_read) ? 2 : 3;
        barrier();
        add_view(e.fbp, e.fbw, e.fpsm_mem, e.fb_ps);
        if ((zt || zw) && !add_view(e.zbp, e.fbw, e.zpsm, e.z_ps)) {
            // the frame and depth buffers overlap each other: draw it here
            barrier();
            j.env = &g_env;
            run_job(j, 0, 1, g_wstats[0]);
            return;
        }
    }
    // buffers accessed through their own layout keep their order per row;
    // only texture reads can see another worker's rows
    if (e.tme) g_pend_read.add(e.tex_ps);
    g_pend_write.add(wr);
    if (zt) g_pend_write.add(e.z_ps);
    push_job(j);
}

}  // namespace

// ------------------------------------------------------------ primitives
static uint32_t current_attr() {
    return (st.prmodecont & 1) ? (uint32_t)st.prim : ((uint32_t)st.prmode & ~7u) | ((uint32_t)st.prim & 7);
}

void draw_sprite(const Vertex &a, const Vertex &b) {
    if (!count_prim()) return;
    uint32_t attr = current_attr();
    setup_env(attr);
    Env &e = g_env;
    if (census::g_on)
        census::draw(e, "sprite", (uint64_t)std::abs((b.x - a.x) >> 4) * (uint64_t)std::abs((b.y - a.y) >> 4));
    if (hw::g_on) {
        if (!e.write_none && !g_skip_frame) {
            hw::draw_sprite(e, a, b);
            note_draw();  // (the display is presented when its pages change)
        }
        return;
    }
    if (g_trace) {
        Vertex vv[2] = {a, b};
        trace_prim("SPRITE", vv, 2);
    }
    if (e.write_none || g_skip_frame) return;
    note_draw();
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
    Job job;
    job.kind = (!e.tme && !e.abe && !e.ate && !e.date && !e.fge && e.ztst == 1 && !g_trace) ? JOB_FILL : JOB_SPRITE;
    job.variant = e.pix_kind;
    job.x0 = x0;
    job.x1 = x1;
    job.y0 = y0;
    job.y1 = y1;
    job.f = f;
    job.u0 = u0;
    job.du = du;
    job.w0 = w0;
    job.dw = dw;
    submit(job);
}

void draw_triangle(const Vertex &a0, const Vertex &b0, const Vertex &c0) {
    if (!count_prim()) return;
    uint32_t attr = current_attr();
    setup_env(attr);
    Env &e = g_env;
    if (census::g_on) {
        int64_t ar = ((int64_t)(b0.x - a0.x) * (c0.y - a0.y) - (int64_t)(c0.x - a0.x) * (b0.y - a0.y)) / 512;
        census::draw(e, "tri", (uint64_t)(ar < 0 ? -ar : ar));
    }
    if (hw::g_on) {
        if (!e.write_none && !g_skip_frame) {
            hw::draw_triangle(e, a0, b0, c0);
            note_draw();  // (the display is presented when its pages change)
        }
        return;
    }
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
    if (e.write_none || g_skip_frame) return;
    note_draw();
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
    TriSpans ts;
    for (int k = 0; k < 3; k++) {
        ts.ed[k].dx = ed[k].dx;
        ts.ed[k].dy = ed[k].dy;
        ts.ed[k].xa = ed[k].xa;
        ts.ed[k].ya = ed[k].ya;
        ts.ed[k].bias = ed[k].bias;
    }
    for (int i = 0; i < NA; i++) {
        ts.base[i] = base[i];
        ts.gx[i] = gx[i];
        ts.gy[i] = gy[i];
    }
    ts.X0 = X0;
    ts.Y0 = Y0;
    ts.px0 = px0;
    ts.px1 = px1;
    ts.py0 = py0;
    ts.py1 = py1;
    Job job;
    job.kind = JOB_TRI;
    job.variant = (e.iip ? 1 : 0) | (e.tme ? 2 : 0) | (e.tme && !e.fst ? 4 : 0) | (e.fge ? 8 : 0) | ((e.pix_kind >> 1) << 4);
    job.ts = ts;
    submit(job);
}

void draw_line(const Vertex &a, const Vertex &b) {
    if (!count_prim()) return;
    uint32_t attr = current_attr();
    setup_env(attr);
    Env &e = g_env;
    if (census::g_on) census::draw(e, "line", 1);
    if (hw::g_on) {
        if (!e.write_none && !g_skip_frame) {
            hw::draw_line(e, a, b);
            note_draw();  // (the display is presented when its pages change)
        }
        return;
    }
    if (e.write_none) return;
    note_draw();
    barrier();
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
    if (!count_prim()) return;
    uint32_t attr = current_attr();
    setup_env(attr);
    Env &e = g_env;
    if (census::g_on) census::draw(e, "point", 1);
    if (hw::g_on) {
        if (!e.write_none && !g_skip_frame) {
            hw::draw_point(e, a);
            note_draw();  // (the display is presented when its pages change)
        }
        return;
    }
    if (e.write_none) return;
    note_draw();
    barrier();
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
    if (g_nw) {
        uint32_t first, count;
        page_range(cbp, csm ? bits(st.texclut, 0, 6) : 1, cpsm, 0, csm ? (int)bits(st.texclut, 12, 10) : 0, 256, 16,
                   first, count);
        PageSet ps;
        ps.clear();
        ps.add(first, count);
        if (ps.any(g_pend_write)) {
            g_barrier_cause = 5;
            barrier();
        }
    }
    if (g_trace)
        log_write(LOG_GS, "CLUT load cld=%u cbp=%x cpsm=%x csm=%u csa=%u psm=%x", cld, cbp, cpsm, csm, csa, psm);
    g_clut_gen++;
    g_reg_gen++;  // the drawing state uses the CLUT
    int n = eight ? 256 : 16;
    uint32_t off = four ? csa * 16 : 0;
    uint32_t cbw = bits(st.texclut, 0, 6), cou = bits(st.texclut, 6, 6), cov = bits(st.texclut, 12, 10);
    if (hw::g_on) {  // the CLUT may have been rendered
        if (csm == 0)
            hw::vram_needed(cbp, 1, cpsm == PSMCT24 ? PSMCT32 : cpsm, 0, 0, eight ? 16 : 8, eight ? 16 : 2, "clut");
        else
            hw::vram_needed(cbp, cbw, cpsm == PSMCT24 ? PSMCT32 : cpsm, (int)cou * 16, (int)cov, n, 1, "clut");
    }
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

// ------------------------------------------------------------ worker control
void raster_start(int n) {
    if (n > kMaxWorkers) n = kMaxWorkers;
    if (n <= 0) return;
    g_ring = new Job[kRing];
    g_nw = n;
    if (platform_core_count() < platform_cpu_count()) g_spin_us = 100;
    for (int i = 0; i < n; i++) std::thread(raster_worker, i).detach();
}

void raster_sync() { barrier(); }

void raster_reset() {
    barrier();
    g_replaying = true;
    g_env_attr = ~0u;
    g_draw_dirty = false;
    g_skip_frame = false;
    uint32_t c = ++g_vram_clock;
    for (auto &g : g_page_gen) g = c;
}

void raster_mark_pages() {
    if (g_draw_dirty) {
        mark_env_written(g_env);
        g_draw_dirty = false;
    }
}

void raster_vram_write(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h) {
    if (!g_nw) return;
    uint32_t first, count;
    page_range(bp, bw, psm, x, y, w, h, first, count);
    PageSet ps;
    ps.clear();
    ps.add(first, count);
    if (ps.any(g_pend_read) || ps.any(g_pend_write)) {
        g_barrier_cause = 6;
        barrier();
    }
}

void raster_vram_read(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h) {
    if (!g_nw) return;
    uint32_t first, count;
    page_range(bp, bw, psm, x, y, w, h, first, count);
    PageSet ps;
    ps.clear();
    ps.add(first, count);
    if (ps.any(g_pend_write)) barrier();
}

void raster_collect_stats() {
    LOG(LOG_GS, "  worker sleeps %u, notifies %u, GS thread sleeps %u, GS thread waited %.2f ms (per vblank)",
        g_dbg_sleeps.load() / 60, g_dbg_notifies.load() / 60, g_dbg_psleeps.load() / 60, g_dbg_wait_ns / 60 / 1e6);
    g_dbg_sleeps = g_dbg_notifies = g_dbg_psleeps = 0;
    g_dbg_wait_ns = 0;
    for (auto &w : g_wstats) {
        stats.pixels += w.pixels;
        stats.decoded += w.decoded;
        stats.slow_samples += w.slow_samples;
        stats.raster_ns += w.raster_ns;
        for (int k = 0; k < 64; k++) stats.kind_px[k] += w.kind_px[k];
        w = WStats();
    }
}

void census_xfer(int dir, uint32_t sbp, uint32_t sbw, uint32_t spsm, uint32_t dbp, uint32_t dbw, uint32_t dpsm,
                 int sx, int sy, int dx, int dy, int w, int h) {
    census_xfer_impl(dir, sbp, sbw, spsm, dbp, dbw, dpsm, sx, sy, dx, dy, w, h);
}

void census_display(uint32_t fbp, uint32_t fbw, uint32_t psm, int w, int h) { census_display_impl(fbp, fbw, psm, w, h); }

uint32_t tex_fetch(const Env &e, int u, int v, int lvl) { return fetch(e, u, v, lvl); }

}  // namespace gs
