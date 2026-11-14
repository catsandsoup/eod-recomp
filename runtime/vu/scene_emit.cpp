// scene_emit.cpp - see scene_emit.h. Producer stream (little endian):
//   "EODEMIT1", then records {u32 tag, u32 bytes, payload}:
//   'GEOM' u64 id, u32 n, u32 0, then n x {f32 x, y, z, s, t; u8 a, b, c, flags}
//          flags bit0 = ADC (no triangle ends here), bit1 = a,b,c is a normal
//          (int8, /127) else a,b,c = vertex colour RGB (0x80 = 1.0). Model space,
//          the TOP+198/199 offsets already applied. Sent the first time a mesh is seen.
//   'BATC' u32 pc, itop, kick0, kicks; u64 geom; f32 model[12] (rows of the 3x4 world
//          matrix); f32 light[4] (q17 rotated to world, normalised; .w = q17.w);
//          f32 dir2[4] (vf12 rotated to world); f32 mvp[16] (the batch's model->GS
//          screen matrix, columns vf4..vf7)
//   'CAMR' u64 frame, f32 cam[16] (columns vf4..vf7 of a world batch: world -> GS screen)
//   'KICK' u32 run, u32 qwc, packet (EOD_SCENE_EMIT_VERIFY frame only; PATH1)
//   'VBLK' u64 frame; u32 batches, geoms; u64 producer ns this frame
// file.tex: "EODETEX1", records 'TVBL' u64 frame, u64 fogcol, u64 GS ns this frame;
//   'TROW' u32 kick, tbp, psm, tw, th, fbp; u64 index_key, palette_key, xyoffset.
// Batches whose run kicked nothing are dropped. The camera is the most common
// x/y/w screen rows among the frame's batches (docs/SCENE_CAPTURE.md section 2);
// each batch's model matrix is inv(camera_xyw) * batch_xyw.
#include "scene_emit.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/log.h"
#include "../gs/modern_scene.h"
#include "vu.h"

extern uint32_t g_vif1_top, g_vif1_itop;

namespace scene_emit {

namespace {
const char *g_path = getenv("EOD_SCENE_EMIT");
uint64_t env_u64(const char *name, uint64_t def) {
    const char *v = getenv(name);
    return v && *v ? strtoull(v, nullptr, 10) : def;
}
const uint64_t g_from = env_u64("EOD_SCENE_EMIT_FROM", 0);
const uint64_t g_count = env_u64("EOD_SCENE_EMIT_N", ~0ull);
const uint64_t g_verify = env_u64("EOD_SCENE_EMIT_VERIFY", ~0ull);
const bool g_nowrite = env_u64("EOD_SCENE_EMIT_NOWRITE", 0) != 0;
const bool g_all = env_u64("EOD_SCENE_EMIT_ALL", 0) != 0;  // diagnostic: every program with the batch layout
// EOD_MODERN=1: also feed the modern renderer live (read here directly: the
// static initialisation order across files is not defined)
const bool g_modern = [] {  // as modern::g_on (initialised in another file)
#ifdef _WIN32
    const char *v = getenv("EOD_MODERN");
    return !(v && strcmp(v, "0") == 0);
#else
    return false;
#endif
}();
const bool g_write = g_path && *g_path && !g_nowrite;
bool emit_range(uint64_t f) { return g_path && *g_path && f >= g_from && f - g_from < g_count; }
bool g_live;  // the modern renderer draws this frame (latched at the vblank)
// the emitter decodes a frame for the modern renderer only while it draws: Original costs nothing
bool in_range(uint64_t f) { return (g_modern && g_live) || emit_range(f); }

constexpr uint32_t fourcc(const char (&s)[5]) {
    return (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24;
}
uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}

// Geometry micro programs by entry (docs/SCENE_CAPTURE.md section 1; 0x350 = facing
// sprites, seen on Woodly). Shadows
// (0x0020, 0x0338), particles, state packets and uploads are not batches here.
bool geometry_program(uint32_t pc) {
    if (g_all) return pc != 0x2538;
    switch (pc) {
    case 0x270: case 0x618: case 0x920: case 0x2a8: case 0x1f90: case 0x2e0: case 0xd48: case 0x15a8:
    case 0x24b0: case 0x318: case 0x350:
        return true;
    }
    return false;
}

struct Batch {
    uint32_t pc, itop, kick0, kicks;
    uint64_t geom;
    float mvp[16];  // column-major: column c = vf[4 + c]
    float q17[4], vf12[4];
    int32_t mi;  // index in the modern frame (-1: not replaced)
    uint32_t n;  // vertices
    bool want_alpha;               // collect the kicked per-vertex alpha (car passes)
    std::vector<uint8_t> valpha;
};

// For every vertex a PATH1 packet draws, in order (PACKED and REGLIST GIF tags): the alpha of
// the RGBAQ in force when its XYZ2/XYZF2/XYZ3/XYZF3 is written, and whether its primitive blends
// (PRIM.ABE, which the game switches within one batch).
void kick_alphas(const uint8_t *d, uint32_t qwc, std::vector<uint8_t> &out) {
    uint32_t p = 0;
    uint8_t a = 0x80, abe = 0;
    while (p < qwc) {
        uint64_t lo, hi;
        memcpy(&lo, d + p * 16, 8);
        memcpy(&hi, d + p * 16 + 8, 8);
        p++;
        const uint32_t nloop = (uint32_t)(lo & 0x7FFF), eop = (uint32_t)(lo >> 15) & 1, flg = (uint32_t)(lo >> 58) & 3;
        if ((lo >> 46) & 1) abe = (uint8_t)((lo >> 53) & 1);  // PRE: PRIM bit 6 (ABE)
        uint32_t nreg = (uint32_t)(lo >> 60) & 15;
        if (!nreg) nreg = 16;
        if (flg == 0) {
            for (uint32_t l = 0; l < nloop; l++)
                for (uint32_t r = 0; r < nreg; r++, p++) {
                    if (p >= qwc) return;
                    const uint32_t reg = (uint32_t)(hi >> (4 * r)) & 15;
                    if (reg == 1) a = d[p * 16 + 12];
                    else if (reg == 0) abe = (d[p * 16] >> 6) & 1;
                    else if (reg == 4 || reg == 5 || reg == 12 || reg == 13) out.insert(out.end(), {a, abe ? (uint8_t)255 : (uint8_t)0});
                }
        } else if (flg == 1) {
            const uint32_t total = nloop * nreg;
            if (p + (total + 1) / 2 > qwc) return;
            for (uint32_t i = 0; i < total; i++) {
                uint64_t w;
                memcpy(&w, d + p * 16 + i * 8, 8);
                const uint32_t reg = (uint32_t)(hi >> (4 * (i % nreg))) & 15;
                if (reg == 1) a = (uint8_t)(w >> 24);
                else if (reg == 0) abe = (uint8_t)((w >> 6) & 1);
                else if (reg == 4 || reg == 5 || reg == 12 || reg == 13) out.insert(out.end(), {a, abe ? (uint8_t)255 : (uint8_t)0});
            }
            p += (total + 1) / 2;
        } else {
            p += nloop;
        }
        if (eop) break;
    }
}

// modern feed (producer)
modern::Frame *g_mf;
bool g_triggered;      // this frame's modern pass is queued
int32_t g_marker = -1; // last batch marker sent to the GS
void mark(int32_t i) {
    if (i == g_marker) return;
    g_marker = i;
    modern::post_batch(i);
}

// ---- producer state
FILE *g_f;
bool g_open_failed;
uint64_t g_frame;          // vblank count of the frame being recorded
bool g_frame_active;       // this frame is in range
std::vector<uint8_t> g_out;  // this frame's records
std::vector<Batch> g_batches;
bool g_pending;            // g_batches.back() waits for its kick count
uint32_t g_kicks;          // PATH1 packets since the vblank
uint32_t g_run;            // VU1 runs since the vblank
std::unordered_map<uint64_t, bool> g_geoms;  // id -> decodes as a batch
uint32_t g_new_geoms;
uint64_t g_ns;             // producer time this frame
// cost summary
struct Cost {
    std::vector<uint64_t> ns;
    uint64_t bytes = 0;
} g_cost, g_gs_cost;

void put(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    g_out.insert(g_out.end(), b, b + n);
}
void rec(uint32_t tag, uint32_t bytes) {
    uint32_t h[2] = {tag, bytes};
    put(h, 8);
}

const uint8_t *qword(const VUState *vu, uint32_t q) { return vu->mem + ((q * 16) & vu->mem_mask); }

void report(const char *what, Cost &c) {
    if (c.ns.empty()) return;
    std::vector<uint64_t> v = c.ns;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (uint64_t x : v) sum += (double)x;
    fprintf(stderr, "[scene-emit] %s: %zu frames, mean %.3f ms, p50 %.3f, p95 %.3f, max %.3f; %.1f KB/frame\n", what,
            v.size(), sum / v.size() / 1e6, v[v.size() / 2] / 1e6, v[v.size() * 95 / 100] / 1e6, v.back() / 1e6,
            c.bytes / 1024.0 / v.size());
}

struct AtExit {
    ~AtExit() {
        report("producer", g_cost);
        report("gs", g_gs_cost);
    }
} g_at_exit;

// Decodes a new mesh into a GEOM record. False if the run's input is not the
// batch layout (docs/SCENE_CAPTURE.md section 2).
bool decode(const VUState *vu, uint32_t top, uint32_t n, uint64_t id) {
    float off[4], uvo[4];
    memcpy(off, qword(vu, top + 199), 16);
    memcpy(uvo, qword(vu, top + 198), 16);
    size_t at = g_out.size();
    modern::Mesh mesh;
    mesh.id = id;
    if (g_modern) mesh.v.resize(n);
    rec(fourcc("GEOM"), 16 + n * 24);
    uint32_t hdr[2] = {n, 0};
    put(&id, 8);
    put(hdr, 8);
    for (uint32_t k = 0; k < n; k++) {
        float p[4], s[4];
        uint32_t ni[4];
        memcpy(ni, qword(vu, top + 1 + 3 * k), 16);
        memcpy(p, qword(vu, top + 2 + 3 * k), 16);
        memcpy(s, qword(vu, top + 3 + 3 * k), 16);
        float v[5] = {p[0] + off[0], p[1] + off[1], p[2] + off[2], s[0] + uvo[0], s[1] + uvo[1]};
        for (int i = 0; i < 3; i++)
            if (!(std::fabs(v[i]) < 1e6f)) {
                g_out.resize(at);
                return false;
            }
        uint8_t a[4];
        const bool normal = (ni[0] & 0xFFFFFF00u) == 0x47800000u;  // offset-mode V4-8 (STROW 65536)
        for (int i = 0; i < 3; i++) a[i] = normal ? (uint8_t)(int8_t)((int)(ni[i] & 0xFF) - 128) : (uint8_t)ni[i];
        const uint32_t w = ni[3] & 0xFFFF;
        a[3] = (uint8_t)(((w & 0x8000) || w == 0x80) | (normal ? 2 : 0));
        put(v, 20);
        put(a, 4);
        if (g_modern) {
            modern::Vertex &mv = mesh.v[k];
            memcpy(mv.p, v, 12);
            memcpy(mv.uv, v + 3, 8);
            for (int i = 0; i < 3; i++) {
                mv.n[i] = normal ? (int8_t)a[i] : 0;
                mv.c[i] = normal ? 0x80 : a[i];
            }
            mv.n[3] = normal ? 127 : 0;
            mv.c[3] = 0x80;
            // strip: a triangle ends at every vertex without ADC (no culling, so winding is free)
            if (k >= 2 && !(a[3] & 1)) mesh.idx.insert(mesh.idx.end(), {k - 2, k - 1, k});
        }
    }
    if (!g_write) g_out.resize(at);  // only the modern feed wants it
    if (g_modern) {
        if (!g_mf) g_mf = new modern::Frame();
        g_mf->meshes.push_back(std::move(mesh));
    }
    return true;
}

void finish_pending() {
    if (!g_pending) return;
    g_pending = false;
    if (g_batches.back().kicks == 0) g_batches.pop_back();
}

// Rows x, y, w of a column-major screen matrix as a 4x4 affine map (row 3 = 0 0 0 1).
void xyw(const float *m, double out[4][4]) {
    const int rows[3] = {0, 1, 3};
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) out[r][c] = m[c * 4 + rows[r]];
    out[3][0] = out[3][1] = out[3][2] = 0;
    out[3][3] = 1;
}
bool invert4(const double a[4][4], double inv[4][4]) {
    double m[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) m[r][c] = c < 4 ? a[r][c] : (c - 4 == r);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (std::fabs(m[r][c]) > std::fabs(m[p][c])) p = r;
        if (std::fabs(m[p][c]) < 1e-12) return false;
        for (int k = 0; k < 8; k++) std::swap(m[c][k], m[p][k]);
        const double d = m[c][c];
        for (int k = 0; k < 8; k++) m[c][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c) {
                const double f = m[r][c];
                for (int k = 0; k < 8; k++) m[r][k] -= f * m[c][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) inv[r][c] = m[r][c + 4];
    return true;
}
void world_dir(const double M[4][4], const float *v, float *out) {
    double w[3], len = 0;
    for (int r = 0; r < 3; r++) {
        w[r] = M[r][0] * v[0] + M[r][1] * v[1] + M[r][2] * v[2];
        len += w[r] * w[r];
    }
    len = len > 0 ? std::sqrt(len) : 1;
    for (int r = 0; r < 3; r++) out[r] = (float)(w[r] / len);
    out[3] = v[3];
}

// camera = the most common x/y/w rows (rounded to 0.01); SIZE_MAX if none
size_t camera_vote() {
    std::unordered_map<uint64_t, std::pair<uint32_t, size_t>> votes;
    size_t best = SIZE_MAX;
    uint32_t best_n = 0;
    for (size_t i = 0; i < g_batches.size(); i++) {
        uint64_t h = 0;
        for (int c = 0; c < 4; c++)
            for (int r : {0, 1, 3}) h = mix(h, (uint64_t)(int64_t)std::llround(g_batches[i].mvp[c * 4 + r] * 100.0));
        auto &v = votes[h];
        if (v.first++ == 0) v.second = i;
        if (v.first > best_n) best_n = v.first, best = v.second;
    }
    return best;
}

void model_of(const Batch &b, const double cinv[4][4], double M[4][4]) {
    double bx[4][4];
    xyw(b.mvp, bx);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += cinv[r][k] * bx[k][c];
            M[r][c] = s;
        }
}

// The modern frame so far: camera, model matrices, roles, layers; queued for the GS.
void post_modern() {
    if (!g_mf) g_mf = new modern::Frame();
    modern::Frame *f = g_mf;
    g_mf = nullptr;
    mark(-1);
    const size_t best = camera_vote();
    double cam[4][4], cinv[4][4];
    if (best == SIZE_MAX || f->batches.empty() || (xyw(g_batches[best].mvp, cam), !invert4(cam, cinv))) {
        f->batches.clear();
        modern::post_draw(f);  // (the meshes still go to the backend cache)
        return;
    }
    memcpy(f->cam, g_batches[best].mvp, 64);
    std::vector<uint64_t> zrows;  // world layers in order of first appearance
    for (const Batch &b : g_batches) {
        if (b.mi < 0) continue;
        modern::Batch &m = f->batches[(size_t)b.mi];
        double M[4][4];
        model_of(b, cinv, M);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++) m.model[r * 4 + c] = (float)M[r][c];
        world_dir(M, b.q17, m.light);
        for (int c = 0; c < 4; c++) m.zrow[c] = b.mvp[c * 4 + 2];
        if (b.valpha.size() == 2 * (size_t)b.n) m.valpha = b.valpha;  // 1:1 with the mesh (nothing clipped)
        double dev = 0, rot = 0;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) dev = std::max(dev, std::fabs(M[r][c] - (r == c)));
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) rot = std::max(rot, std::fabs(M[r][c] - (r == c)));
        m.role = dev < 1e-3 ? modern::WORLD : (b.itop == 0x280 && rot < 1e-3) ? modern::SKY : modern::OBJECT;
        m.layer = 0;
        if (m.role == modern::WORLD) {
            uint64_t z = 0;
            for (int c = 0; c < 4; c++) z = mix(z, (uint64_t)(int64_t)std::llround(b.mvp[c * 4 + 2] * 16.0));
            auto it = std::find(zrows.begin(), zrows.end(), z);
            m.layer = (uint32_t)(it - zrows.begin());
            if (it == zrows.end()) zrows.push_back(z);
        }
    }
    modern::post_draw(f);
}

// End of a frame: camera vote, model matrices, records, write.
void flush_frame() {
    finish_pending();
    if (g_modern && !g_triggered) post_modern();
    g_triggered = false;
    if (g_modern) mark(-1);
    const size_t best = camera_vote();
    double cam[4][4], cinv[4][4];
    const bool have_cam = best != SIZE_MAX && (xyw(g_batches[best].mvp, cam), invert4(cam, cinv));
    if (have_cam) {
        rec(fourcc("CAMR"), 8 + 64);
        put(&g_frame, 8);
        put(g_batches[best].mvp, 64);
    }
    for (const Batch &b : g_batches) {
        if (!have_cam) break;
        double M[4][4];
        model_of(b, cinv, M);
        float model[12], light[4], dir2[4];
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++) model[r * 4 + c] = (float)M[r][c];
        world_dir(M, b.q17, light);
        world_dir(M, b.vf12, dir2);
        rec(fourcc("BATC"), 16 + 8 + 48 + 16 + 16 + 64);
        uint32_t h[4] = {b.pc, b.itop, b.kick0, b.kicks};
        put(h, 16);
        put(&b.geom, 8);
        put(model, 48);
        put(light, 16);
        put(dir2, 16);
        put(b.mvp, 64);
    }
    uint32_t counts[2] = {(uint32_t)g_batches.size(), g_new_geoms};
    rec(fourcc("VBLK"), 8 + 8 + 8);
    put(&g_frame, 8);
    put(counts, 8);
    put(&g_ns, 8);  // the frame's cost before the write (the write is timed below)
    uint64_t t = now_ns();
    if (g_write && emit_range(g_frame)) {
        if (!g_f && !g_open_failed) {
            g_f = fopen(g_path, "wb");
            if (!g_f) {
                WARN("EOD_SCENE_EMIT: cannot write %s", g_path);
                g_open_failed = true;
            } else {
                fwrite("EODEMIT1", 1, 8, g_f);
            }
        }
        if (g_f) fwrite(g_out.data(), 1, g_out.size(), g_f);
    }
    g_ns += now_ns() - t;
    g_cost.ns.push_back(g_ns);
    g_cost.bytes += g_out.size();
    g_out.clear();
    g_batches.clear();
}
}  // namespace

bool g_on = (g_path && *g_path) || g_modern;

void vu1_start(const VUState *vu, uint32_t pc) {
    if (!g_frame_active) return;
    const uint64_t t = now_ns();
    finish_pending();
    g_run++;
    // the HUD: the modern pass goes into the frame buffer before it (docs/SCENE_CAPTURE.md section 4)
    // (the same program also draws camera-space batches in that mode earlier in the frame; the HUD's
    // matrix faces the screen: its w row is (0, 0, 1, 365), with no x or y term)
    const bool hud = pc == 0x270 && g_vif1_itop == 0x200 && vu->vf[4].f[3] == 0.0f && vu->vf[5].f[3] == 0.0f;
    // ... or the first particle program after the scene (they come after the last car): the GS then
    // draws the particles over the modern image, tested against the depth it writes
    const bool particles = (pc == 0x1b8 || pc == 0x8e0 || pc == 0x5e0) && g_mf && !g_mf->batches.empty();
    if (g_live && (hud || particles) && !g_triggered) {
        post_modern();
        g_triggered = true;
    }
    bool replaced = false;
    if (geometry_program(pc)) {
        const uint32_t top = g_vif1_top;
        const uint32_t n = *(const uint32_t *)qword(vu, top) & 0x7FFF;
        if (n > 0 && n <= 120) {
            // content id: the vertex block and its offsets (static meshes repeat every frame)
            uint64_t id = mix(0, n);
            for (uint32_t q = 1; q <= 3 * n; q++) {
                const uint64_t *w = (const uint64_t *)qword(vu, top + q);
                id = mix(mix(id, w[0]), w[1]);
            }
            for (uint32_t q : {198u, 199u}) {
                const uint64_t *w = (const uint64_t *)qword(vu, top + q);
                id = mix(mix(id, w[0]), w[1]);
            }
            auto it = g_geoms.find(id);
            bool ok;
            if (it == g_geoms.end()) {
                ok = decode(vu, top, n, id);
                g_geoms.emplace(id, ok);
                if (ok) g_new_geoms++;
            } else {
                ok = it->second;
            }
            if (ok) {
                Batch b;
                b.pc = pc;
                b.itop = g_vif1_itop;
                b.kick0 = g_kicks;
                b.kicks = 0;
                b.geom = id;
                for (int c = 0; c < 4; c++) memcpy(b.mvp + c * 4, vu->vf[4 + c].f, 16);
                memcpy(b.q17, qword(vu, 17), 16);
                memcpy(b.vf12, vu->vf[12].f, 16);
                b.mi = -1;
                b.n = n;
                b.want_alpha = false;
                // the sky dome (0x270 in ITOP mode 0x280) stays with the GS: the modern pass starts from it
                const bool sky_dome = pc == 0x270 && b.itop == 0x280;
                if (g_live && !hud && !sky_dome && !g_triggered) {
                    if (!g_mf) g_mf = new modern::Frame();
                    b.mi = (int32_t)g_mf->batches.size();
                    modern::Batch m = {};
                    m.mesh = id;
                    m.pc = pc;
                    m.itop = b.itop;
                    g_mf->batches.push_back(m);
                    mark(b.mi);
                    b.want_alpha = true;
                    replaced = true;
                }
                g_batches.push_back(b);
                g_pending = true;
            }
        }
    }
    // the game's own projected car shadows (blurred blobs composited onto the ground): the modern
    // pass casts real shadows, so the blobs are not drawn while it is live
    static const bool keep_blobs = getenv("EOD_MODERN_BLOBS") != nullptr;
    if (g_live && !g_triggered && !keep_blobs && (pc == 0x0020 || pc == 0x0338)) {
        mark(modern::kSuppressed);
        replaced = true;
    }
    if (g_modern && !replaced) mark(-1);
    g_ns += now_ns() - t;
}

void gif(int path, const uint8_t *data, uint32_t qwc) {
    if (!g_frame_active || path != 1) return;
    g_kicks++;
    if (g_pending) {
        g_batches.back().kicks++;
        Batch &cur = g_batches.back();
        if (g_mf && cur.mi >= 0) g_mf->batches[(size_t)cur.mi].drawn = true;
        if (cur.want_alpha) kick_alphas(data, qwc, cur.valpha);
    }
    if (g_frame >= g_verify && g_frame - g_verify < 4) {  // a few vblanks: the game draws every 2-3
        const uint64_t t = now_ns();
        rec(fourcc("KICK"), 8 + qwc * 16);
        uint32_t h[2] = {g_run, qwc};
        put(h, 8);
        put(data, qwc * 16);
        g_ns += now_ns() - t;
    }
}

void vblank(uint64_t frame) {
    if (!g_on) return;
    if (g_frame_active) flush_frame();
    // the frame that starts now gets the number of the next vblank (as the
    // GS side and --screenshot-every count it)
    g_frame = frame + 1;
    if (g_modern) {
        static const uint64_t flip = env_u64("EOD_MODERN_FLIP_EVERY", 0);  // diagnosis: alternate renderers
        if (flip && g_frame % flip == 0) modern::toggle();
        g_live = modern::live();
    }
    g_frame_active = in_range(g_frame);
    g_kicks = g_run = g_new_geoms = 0;
    g_ns = 0;
    if (!emit_range(g_frame) && g_f && g_frame > g_from) {
        fclose(g_f);
        g_f = nullptr;
        fprintf(stderr, "[scene-emit] wrote %s\n", g_path);
    }
}

// ---- GS side
namespace {
FILE *g_tex;
bool g_tex_failed;
bool g_gs_active;
uint32_t g_gs_kicks, g_gs_path;
uint64_t g_gs_frame, g_gs_ns;
std::vector<uint8_t> g_tex_out;
std::unordered_map<uint64_t, uint64_t> g_keys;
void tput(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    g_tex_out.insert(g_tex_out.end(), b, b + n);
}
}  // namespace

void gs_vblank(uint64_t frame, uint64_t fogcol) {
    if (!g_on) return;
    if (g_gs_active) {
        const uint64_t t = now_ns();
        uint32_t h[2] = {fourcc("TVBL"), 24};
        tput(h, 8);
        tput(&g_gs_frame, 8);
        tput(&fogcol, 8);
        tput(&g_gs_ns, 8);
        if (g_write) {
            if (!g_tex && !g_tex_failed) {
                std::string p = std::string(g_path) + ".tex";
                g_tex = fopen(p.c_str(), "wb");
                if (g_tex)
                    fwrite("EODETEX1", 1, 8, g_tex);
                else
                    g_tex_failed = true;
            }
            if (g_tex) fwrite(g_tex_out.data(), 1, g_tex_out.size(), g_tex);
        }
        g_gs_ns += now_ns() - t;
        g_gs_cost.ns.push_back(g_gs_ns);
        g_gs_cost.bytes += g_tex_out.size();
        g_tex_out.clear();
    }
    // gs_frame_end(frame) closes frame `frame`; the next one is frame + 1
    g_gs_frame = frame + 1;
    g_gs_active = emit_range(g_gs_frame);
    g_gs_kicks = 0;
    g_gs_ns = 0;
    if (!g_gs_active && g_tex && g_gs_frame > g_from) {
        fclose(g_tex);
        g_tex = nullptr;
    }
}

void gs_packet(int path) {
    g_gs_path = (uint32_t)path;
    if (g_gs_active && path == 1) g_gs_kicks++;
}

bool gs_wants_texture() { return g_gs_active && g_gs_path == 1; }

void gs_texture(uint32_t tbp, uint32_t psm, uint32_t tw, uint32_t th, uint64_t index_key, uint64_t palette_key,
                uint64_t xyoffset, uint32_t fbp) {
    uint32_t h[2] = {fourcc("TROW"), 24 + 24};
    tput(h, 8);
    uint32_t w[6] = {g_gs_kicks, tbp, psm, tw, th, fbp};
    tput(w, 24);
    tput(&index_key, 8);
    tput(&palette_key, 8);
    tput(&xyoffset, 8);
}

bool gs_key_lookup(uint64_t raw, uint64_t &index_key) {
    auto it = g_keys.find(raw);
    if (it == g_keys.end()) return false;
    index_key = it->second;
    return true;
}
void gs_key_store(uint64_t raw, uint64_t index_key) {
    if (g_keys.size() > (1u << 16)) g_keys.clear();
    g_keys[raw] = index_key;
}
void gs_cost_ns(uint64_t ns) { g_gs_ns += ns; }

}  // namespace scene_emit
