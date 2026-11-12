// Track draw distance (docs/LOD_CULLING_INVESTIGATION.md, "Draw distance").
//
// [Video] DrawDistance=1..8 (EOD_DRAW_DISTANCE overrides). 1 = native, no
// guest writes. Above 1, two native rejection distances grow by the factor:
//  * f_00214678 (GRAF load): the final cutoff of every mlist LOD group
//    (squared, f_00357E60), so scenery groups stop vanishing at 100-1000 units.
//    Level-transition distances stay native.
//  * f_00212B58 (camera build, in hle_lod.cpp): the projection far plane that
//    feeds the CPU sphere test 0034E308 and the VU0 mlist frustum test. The
//    fog range written by the same call is put back to its native values.
// EOD_CULL_TRACE=1 logs, for sampled frames, which mlist test rejected what.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "core/config.h"
#include "core/cpu.h"
#include "core/memory.h"
#include "draw_distance.h"
#include "ee.h"

extern "C" void f_00214678_orig();
extern "C" void f_003582B8_orig();

namespace {
constexpr uint32_t kGraf = 0x3d1c18;          // loaded track.grf header {root, mlistA, mlistB}
constexpr uint32_t kFog = 0x4d8b80;           // +4 start, +8 end, +0xC 0.5 (GsFogWriteTables)
constexpr uint32_t kCamera = 0x4cd2f8;        // 3x4: side, up, forward rows, then position
constexpr uint32_t kNearFar = 0x4cd370;
constexpr uint32_t kVblank = 0x3e579c;        // guest vblank counter (FrameWaitVblank 0035C418)

bool ram(uint32_t a, uint32_t size) { return a && !(a & 3) && a < EE_RAM_SIZE && size <= EE_RAM_SIZE - a; }
float loadf(uint32_t a) { const uint32_t u = ee_r32(a); float f; std::memcpy(&f, &u, 4); return f; }
void storef(uint32_t a, float f) { uint32_t u; std::memcpy(&u, &f, 4); ee_w32(a, u); }
bool trace() { static const bool on = std::getenv("EOD_CULL_TRACE") != nullptr; return on; }

float configured() {
    static const float value = [] {
        float v = 1;
        if (const char *env = std::getenv("EOD_DRAW_DISTANCE")) {
            if (!eod_draw::parse(env, v)) { std::fprintf(stderr, "[draw] invalid EOD_DRAW_DISTANCE '%s'; using 1\n", env); v = 1; }
        } else {
            v = g_config.draw_distance;
            if (!std::isfinite(v) || v < eod_draw::kMin || v > eod_draw::kMax) v = 1;
        }
        if (v != 1) std::fprintf(stderr, "[draw] draw distance x%.9g\n", double(v));
        return v;
    }();
    return value;
}

struct Mlist { uint32_t base, spheres, ns, groups, ng, models, nm; };
bool mlist(uint32_t m, Mlist &out) {
    if (!ram(m, 0x98)) return false;
    out = {m, ee_r32(m + 0x14), ee_r32(m + 0x10), ee_r32(m + 0x1c), ee_r32(m + 0x18), ee_r32(m + 0x24), ee_r32(m + 0x20)};
    return out.ns < 0x1000 && out.ng < 0x1000 && out.nm < 0x2000 &&
           (!out.ns || ram(out.spheres, out.ns * 16)) && (!out.ng || ram(out.groups, out.ng * 32)) &&
           (!out.nm || ram(out.models, out.nm * 32));
}

// Diagnostic replica of 0034E308 (no guest call): 1 when the sphere passes.
bool in_view(const float c[3], float r) {
    float d[3], row[3][3];
    for (int i = 0; i < 3; ++i) d[i] = c[i] - loadf(kCamera + 0x24 + i * 4);
    for (int k = 0; k < 3; ++k) for (int i = 0; i < 3; ++i) row[k][i] = loadf(kCamera + k * 12 + i * 4);
    const float depth = d[0] * row[2][0] + d[1] * row[2][1] + d[2] * row[2][2];
    if (depth < loadf(kNearFar) - r || depth > loadf(kNearFar + 4) + r) return false;
    const float bound = std::fmax(depth * loadf(0x4cd378), 0.0f) + r * loadf(0x4cd380);
    for (int k = 0; k < 2; ++k)
        if (std::fabs(d[0] * row[k][0] + d[1] * row[k][1] + d[2] * row[k][2]) > bound) return false;
    return true;
}
}  // namespace

float eod_draw_distance() { return configured(); }

// Called by f_00212B58 after the native camera build when the far plane was
// scaled: put the fog range back to what the native source distance gives
// (start 0.1*q, end 2*q, q = distance / clamp(views,1,4); 00212C0C-00212C34).
void eod_draw_distance_native_fog(float source) {
    int views = int(ee_r32(0x3d1a74));
    views = views < 1 ? 1 : views > 4 ? 4 : views;
    const float q = fdiv_ps2(source, float(views));
    uint32_t tenth_bits = 0x3dcccccd; float tenth; std::memcpy(&tenth, &tenth_bits, 4);
    storef(kFog + 4, fmul_ps2(q, tenth));
    storef(kFog + 8, fadd_ps2(q, q));
}

extern "C" void f_00214678() {
    f_00214678_orig();
    const float k = configured();
    if (k == 1) return;
    const uint32_t graf = ee_r32(kGraf);
    if (!ram(graf, 12)) return;
    // Native relocation (f_00358F50) is unguarded, so each loader call works on
    // freshly loaded bytes: scaling once here cannot compound.
    for (unsigned which = 1; which <= 2; ++which) {
        Mlist m;
        if (!mlist(ee_r32(graf + which * 4), m)) continue;
        unsigned scaled = 0, kept = 0;
        for (uint32_t g = 0; g < m.ng; ++g) {
            const uint32_t rec = m.groups + g * 32;
            float t[3] = {loadf(rec), loadf(rec + 4), loadf(rec + 8)};
            const int slot = eod_draw::scale_cutoff(t, k);
            if (slot < 0) { ++kept; continue; }
            storef(rec + slot * 4, t[slot]);
            ++scaled;
        }
        std::fprintf(stderr, "[draw] mlist%c groups=%u cutoff_scaled=%u unchanged=%u factor=%.9g\n",
                     which == 1 ? 'A' : 'B', m.ng, scaled, kept, double(k));
    }
}

// Diagnostic only. Scratchpad bytes after the native calc: [0] root,
// [1..ns] cull spheres, then groups (f_00357E60 level mask), then models
// (0 = not drawn). EOD_CULL_TRACE=1 prints sampled frames with the nearest
// group-cut models that would pass the CPU frustum test; EOD_FRAME_LOG=<csv>
// writes every frame: guest vblank, interval since the previous calc (us), calc
// time (us), camera position/heading and the per-test counts.
namespace {
struct Counts { unsigned sph, drawn, lod, cut, cut_view, cut_far, hidden, frustum; float cut_min; };
FILE *frame_log() {
    static FILE *f = [] {
        const char *path = std::getenv("EOD_FRAME_LOG");
        FILE *out = path && *path ? std::fopen(path, "w") : nullptr;
        if (out) std::fprintf(out, "vblank,call,interval_us,calc_us,mlist,cam_x,cam_y,cam_z,heading_deg,far,models,drawn,lod_swap,group_cut,"
                                   "group_cut_in_view,group_cut_beyond_far,nearest_cut_in_view,parent_sphere_hidden,frustum\n");
        return out;
    }();
    return f;
}
uint64_t now_us() { return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
}  // namespace

extern "C" void f_003582B8() {
    const uint32_t m0 = g_ee.r[4].uw[0];
    FILE *csv = frame_log();
    const bool sampled_trace = trace();
    const uint64_t t0 = csv ? now_us() : 0;
    f_003582B8_orig();
    if (!csv && !sampled_trace) return;
    const uint64_t t1 = csv ? now_us() : 0;
    static unsigned calls, lines;
    static uint64_t previous;
    ++calls;
    const bool print = sampled_trace && lines < 400 && (calls <= 4 || calls % 120 == 0);
    Mlist m;
    if ((!csv && !print) || !mlist(m0, m) || 1 + m.ns + m.ng + m.nm > 0x1c00) return;
    const uint8_t *s = g_scratchpad;
    const uint32_t gbase = 1 + m.ns, mbase = gbase + m.ng;
    const float far = loadf(kNearFar + 4);
    float cam[3]; for (int i = 0; i < 3; ++i) cam[i] = loadf(kCamera + 0x24 + i * 4);
    Counts n{}; n.cut_min = -1;
    for (uint32_t i = 0; i < m.ns; ++i) n.sph += s[1 + i] != 0;
    struct Near { float d, cutoff; uint32_t idx; float c[3], r; } nearest[6];
    unsigned nn = 0;
    for (uint32_t i = 0; i < m.nm; ++i) {
        const uint32_t rec = m.models + i * 32;
        const uint32_t parent = ee_r32(rec + 16) & 0xffff, required = ee_r32(rec + 16) >> 16;
        if (s[mbase + i]) { ++n.drawn; continue; }
        const uint8_t pv = parent < mbase ? s[parent] & 0x7f : 0;
        if (pv == required) { ++n.frustum; continue; }
        if (parent < gbase) { ++n.hidden; continue; }
        // Group parent: another LOD level is chosen, or the group is past its
        // last finite level (mask with every finite level bit set).
        const uint32_t grec = m.groups + (parent - gbase) * 32;
        const float t[3] = {loadf(grec), loadf(grec + 4), loadf(grec + 8)};
        unsigned levels = 0; while (levels < 3 && t[levels] < eod_draw::kOpenEnded) ++levels;
        const uint8_t all = levels == 1 ? 1 : levels == 2 ? 5 : 0x15;
        if (pv != all) { ++n.lod; continue; }
        ++n.cut;
        const float c[3] = {loadf(rec), loadf(rec + 4), loadf(rec + 8)}, r = loadf(rec + 12);
        const float d = std::sqrt((c[0]-cam[0])*(c[0]-cam[0]) + (c[1]-cam[1])*(c[1]-cam[1]) + (c[2]-cam[2])*(c[2]-cam[2]));
        if (!in_view(c, r)) { if (d - r > far) ++n.cut_far; continue; }
        ++n.cut_view;
        if (n.cut_min < 0 || d < n.cut_min) n.cut_min = d;
        if (!print || (nn == 6 && nearest[5].d <= d)) continue;
        unsigned at = nn < 6 ? nn++ : 5;
        for (; at > 0 && nearest[at - 1].d > d; --at) nearest[at] = nearest[at - 1];
        nearest[at] = Near{d, std::sqrt(t[levels - 1]), i, {c[0], c[1], c[2]}, r};
    }
    const uint32_t graf = ee_r32(kGraf);
    const char which = ram(graf, 12) && m0 == ee_r32(graf + 8) ? 'B' : 'A';
    const float heading = float(std::atan2(double(loadf(kCamera + 0x18)), double(loadf(kCamera + 0x20))) * 180.0 / M_PI);
    if (csv) {
        std::fprintf(csv, "%llu,%u,%llu,%llu,%c,%.2f,%.2f,%.2f,%.1f,%.6g,%u,%u,%u,%u,%u,%u,%.1f,%u,%u\n",
                     (unsigned long long)ee_r32(kVblank), calls, (unsigned long long)(previous ? t0 - previous : 0),
                     (unsigned long long)(t1 - t0), which, double(cam[0]), double(cam[1]), double(cam[2]), double(heading),
                     double(far), m.nm, n.drawn, n.lod, n.cut, n.cut_view, n.cut_far, double(n.cut_min), n.hidden, n.frustum);
        previous = t0;
    }
    if (!print) return;
    ++lines;
    std::fprintf(stderr, "[cull] call=%u vblank=%llu mlist%c cam=%.1f,%.1f,%.1f heading=%.1f far=%.6g fog=%.6g..%.6g spheres=%u/%u models=%u drawn=%u lod_swap=%u "
                 "group_cut=%u group_cut_in_view=%u group_cut_beyond_far=%u nearest_cut_in_view=%.1f parent_sphere_hidden=%u frustum=%u\n",
                 calls, (unsigned long long)ee_r32(kVblank), which, double(cam[0]), double(cam[1]), double(cam[2]), double(heading), double(far),
                 double(loadf(kFog + 4)), double(loadf(kFog + 8)), n.sph, m.ns, m.nm, n.drawn, n.lod, n.cut, n.cut_view, n.cut_far,
                 double(n.cut_min), n.hidden, n.frustum);
    for (unsigned i = 0; i < nn; ++i)
        std::fprintf(stderr, "[cull]   cut model=%u at=%.1f,%.1f,%.1f r=%.1f distance=%.1f group_cutoff=%.1f\n", nearest[i].idx,
                     double(nearest[i].c[0]), double(nearest[i].c[1]), double(nearest[i].c[2]), double(nearest[i].r),
                     double(nearest[i].d), double(nearest[i].cutoff));
}
