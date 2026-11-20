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
#define XXH_INLINE_ALL
#include <tuple>
#include <map>
#include <xxhash.h>
#include "scene_emit.h"
#include "core/trace.h"

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
#include "../gs/gs.h"
#include "vu.h"

extern uint32_t g_vif1_top, g_vif1_itop;
uint64_t kernel_vblank_count();
extern uint8_t *g_ram;

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
#if defined(_WIN32) || defined(EOD_VULKAN)
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
uint64_t now_ns_raw() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
// producer cost statistics only matter for the EOD_SCENE_EMIT file (QueryPerformanceCounter per VU1 run)
uint64_t now_ns() { return g_write ? now_ns_raw() : 0; }
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
    std::vector<uint8_t> pred;     // EOD_MODERN_SKIPCHECK: valpha predicted from the inputs
    std::vector<uint32_t> in_w;    // EOD_MODERN_SKIPCHECK: each input vertex's first qword .w
    bool skipped = false;          // the program was not run (cached results)
    uint64_t state_key = 0;        // every VU1 input a program's GS state output depends on
    std::vector<std::vector<uint8_t>> state_rec;  // a real run's PATH1 kicks without vertices
    bool state_ok = true;          // every kick could be reduced exactly (else: never skipped)
    std::vector<float> skin_xy;    // EOD_MODERN_SKINCHECK: the skinned vertices projected (GS pixels)
    std::vector<float> kick_xy;    // EOD_MODERN_SKINCHECK: the XYZ the program kicked (GS pixels)
    uint64_t tag;                  // EOD_MODERN_SKIPCHECK: the GIF tag template (VU1 qword 23)
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

// Input vertex k of a batch: v = model-space position (TOP+199 offset applied) and s, t (TOP+198
// offset applied); a = normal (int8) or colour, and flags (bit0 ADC, bit1 normal).
void input_vertex(const VUState *vu, uint32_t top, uint32_t k, const float off[4], const float uvo[4], float v[5],
                  uint8_t a[4]) {
    float p[4], s[4];
    uint32_t ni[4];
    memcpy(ni, qword(vu, top + 1 + 3 * k), 16);
    memcpy(p, qword(vu, top + 2 + 3 * k), 16);
    memcpy(s, qword(vu, top + 3 + 3 * k), 16);
    v[0] = p[0] + off[0], v[1] = p[1] + off[1], v[2] = p[2] + off[2], v[3] = s[0] + uvo[0], v[4] = s[1] + uvo[1];
    const bool normal = (ni[0] & 0xFFFFFF00u) == 0x47800000u;  // offset-mode V4-8 (STROW 65536)
    for (int i = 0; i < 3; i++) a[i] = normal ? (uint8_t)(int8_t)((int)(ni[i] & 0xFF) - 128) : (uint8_t)ni[i];
    const uint32_t w = ni[3] & 0xFFFF;
    a[3] = (uint8_t)(((w & 0x8000) || w == 0x80) | (normal ? 2 : 0));
}
void modern_vertex(const float v[5], const uint8_t a[4], modern::Vertex &mv) {
    const bool normal = (a[3] & 2) != 0;
    memcpy(mv.p, v, 12);
    memcpy(mv.uv, v + 3, 8);
    for (int i = 0; i < 3; i++) {
        mv.n[i] = normal ? (int8_t)a[i] : 0;
        mv.c[i] = normal ? 0x80 : a[i];
    }
    mv.n[3] = normal ? 127 : 0;
    mv.c[3] = 0x80;
}

// ---- bones (VU1 microcode, config/vu fd0ce30ab0ec263c)
// Per vertex k the program reads a skin word at TOP+149+n-k: .x, .y = bone a, b (the low 16 bits
// of the word, an absolute VU1 qword address), .z, .w = weights a, b as floats biased by 32768.
//   0x24b0 / 0x318 (driver, pedestrians; body at 0x1ff0): bone i is a 3x4 matrix, columns at
//     qwords i+144..i+147; position = wa*(Ma p) + wb*(Mb p), normal = wa*(Ra n) + wb*(Rb n).
//   0x2e0 / 0xd48 / 0x15a8 (player car passes; bodies at 0x948/0xd70/0x10f0): bone i is a
//     translation at qword i+143 (damage dents); position = p + wa*Da + wb*Db, normal unchanged.
// The modern renderer draws the cached bind-pose mesh with these per-frame vertices.
enum SkinKind { SKIN_NONE, SKIN_MATRIX, SKIN_OFFSET };
SkinKind skin_kind(uint32_t pc) {
    switch (pc) {
    case 0x24b0: case 0x318: return SKIN_MATRIX;
    case 0x2e0: case 0xd48: case 0x15a8: return SKIN_OFFSET;
    }
    return SKIN_NONE;
}
uint32_t skin_index(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u & 0xFFFF;
}
// The batch's vertices with its bones applied; false when they are the bind pose (no dents) or
// the skin data is not usable (the cached mesh is drawn then).
bool skin_vertices(const VUState *vu, uint32_t pc, uint32_t top, uint32_t n, std::vector<modern::Vertex> &out) {
    const SkinKind kind = skin_kind(pc);
    if (kind == SKIN_NONE || !n) return false;
    EOD_ZONE("emit.skin");
    constexpr float kBias = 32768.0f;
    float off[4], uvo[4];
    memcpy(off, qword(vu, top + 199), 16);
    memcpy(uvo, qword(vu, top + 198), 16);
    out.resize(n);
    bool moved = kind == SKIN_MATRIX;
    for (uint32_t k = 0; k < n; k++) {
        float v[5], sk[4], pw[4];
        uint8_t a[4];
        input_vertex(vu, top, k, off, uvo, v, a);
        memcpy(pw, qword(vu, top + 2 + 3 * k), 16);  // .w: the position's w (the matrix translation weight)
        memcpy(sk, qword(vu, top + 149 + n - k), 16);
        const uint32_t ba = skin_index(sk[0]), bb = skin_index(sk[1]);
        const float wa = sk[2] - kBias, wb = sk[3] - kBias;
        if (!(std::fabs(wa) <= 4.0f && std::fabs(wb) <= 4.0f)) return false;
        modern::Vertex &mv = out[k];
        modern_vertex(v, a, mv);
        float p[3];
        if (kind == SKIN_OFFSET) {
            const float *da = (const float *)qword(vu, ba + 143), *db = (const float *)qword(vu, bb + 143);
            for (int i = 0; i < 3; i++) {
                const float d = wa * da[i] + wb * db[i];
                p[i] = v[i] + d;
                if (std::fabs(d) > 1e-6f) moved = true;
            }
        } else {
            const float *A[4], *B[4];
            for (int c = 0; c < 4; c++) A[c] = (const float *)qword(vu, ba + 144 + c), B[c] = (const float *)qword(vu, bb + 144 + c);
            float nrm[3] = {0, 0, 0};
            const bool has_n = (a[3] & 2) != 0;
            for (int i = 0; i < 3; i++) {
                const float pa = A[0][i] * v[0] + A[1][i] * v[1] + A[2][i] * v[2] + A[3][i] * pw[3];
                const float pb = B[0][i] * v[0] + B[1][i] * v[1] + B[2][i] * v[2] + B[3][i] * pw[3];
                p[i] = wa * pa + wb * pb;
                if (has_n) {
                    const float n0 = mv.n[0], n1 = mv.n[1], n2 = mv.n[2];
                    nrm[i] = wa * (A[0][i] * n0 + A[1][i] * n1 + A[2][i] * n2) + wb * (B[0][i] * n0 + B[1][i] * n1 + B[2][i] * n2);
                }
            }
            if (has_n) {
                const float l = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
                if (l > 1e-6f)
                    for (int i = 0; i < 3; i++) mv.n[i] = (int8_t)std::lround(std::clamp(nrm[i] / l, -1.0f, 1.0f) * 127.0f);
            }
        }
        for (int i = 0; i < 3; i++) {
            if (!(std::fabs(p[i]) < 1e6f)) return false;
            mv.p[i] = p[i];
        }
    }
    return moved;
}

// EOD_BONE_SRC=1 (diagnostic): for matrix-skinned batches, finds each bone's 3x4 matrix in EE RAM
// by content and logs where it is, so the EE code that builds the pose can be traced.
void bone_source_probe(const VUState *vu, uint32_t pc, uint32_t top, uint32_t n) {
    static const bool on = getenv("EOD_BONE_SRC") != nullptr;
    if (!on || skin_kind(pc) != SKIN_MATRIX) return;
    static uint64_t last_vb = ~0ull;
    static int in_vb = 0;
    if (kernel_vblank_count() % 120 != 0) return;
    if (last_vb != kernel_vblank_count()) last_vb = kernel_vblank_count(), in_vb = 0;
    if (in_vb++ >= 3) return;
    std::vector<uint32_t> bones;
    for (uint32_t k = 0; k < n; k++) {
        float sk[4];
        memcpy(sk, qword(vu, top + 149 + n - k), 16);
        for (int j = 0; j < 2; j++) {
            const uint32_t b = skin_index(sk[j]);
            if (std::find(bones.begin(), bones.end(), b) == bones.end()) bones.push_back(b);
        }
    }
    std::sort(bones.begin(), bones.end());
    const size_t ram = 32u << 20;
    fprintf(stderr, "[bonesrc] vb %llu pc %04x top %03x n %u bones %zu\n", (unsigned long long)kernel_vblank_count(), pc, top, n,
            bones.size());
    for (uint32_t b : bones) {
        const uint8_t *col0 = qword(vu, b + 144);
        float m[16];
        for (int c = 0; c < 4; c++) memcpy(m + 4 * c, qword(vu, b + 144 + c), 16);
        fprintf(stderr, "[bonesrc]   bone q%03x  c0 %.4f %.4f %.4f %.4f  c3 %.3f %.3f %.3f %.3f ->", b + 144, m[0], m[1], m[2],
                m[3], m[12], m[13], m[14], m[15]);
        int hits = 0;
        for (size_t a = 0; a + 64 <= ram && hits < 6; a += 16) {
            if (memcmp(g_ram + a, col0, 12) != 0) continue;
            const bool full = memcmp(g_ram + a, m, 64) == 0;
            fprintf(stderr, " %08zx%s", a, full ? "(64)" : "");
            hits++;
        }
        fprintf(stderr, "\n");
    }
}

// Decodes a new mesh into a GEOM record. False if the run's input is not the
// batch layout (docs/SCENE_CAPTURE.md section 2).
bool decode(const VUState *vu, uint32_t top, uint32_t n, uint64_t id) {
    EOD_ZONE("emit.decode");
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
        float v[5];
        uint8_t a[4];
        input_vertex(vu, top, k, off, uvo, v, a);
        for (int i = 0; i < 3; i++)
            if (!(std::fabs(v[i]) < 1e6f)) {
                g_out.resize(at);
                return false;
            }
        put(v, 20);
        put(a, 4);
        if (g_modern) {
            modern_vertex(v, a, mesh.v[k]);
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
// EOD_MODERN_SKIPCHECK=1: how well the inputs predict what each program kicks, per program
bool skip_check() {
    static const bool on = getenv("EOD_MODERN_SKIPCHECK") != nullptr;
    return on;
}
struct CheckStats { uint64_t batches, drawn, full, verts, alpha_bad, abe_bad, tag_bad; };
std::map<uint32_t, CheckStats> g_check;
void check_prediction(const Batch &b) {
    CheckStats &c = g_check[b.pc << 12 | b.itop];
    c.batches++;
    if (b.kicks) c.drawn++;
    if (b.valpha.size() != b.pred.size()) return;
    c.full++;
    static int samples;
    for (size_t k = 0; k + 1 < b.pred.size(); k += 2) {
        c.verts++;
        if (b.valpha[k] != b.pred[k]) {
            c.alpha_bad++;
            if (samples++ < 12)
                fprintf(stderr, "[skipcheck] pc %04x itop %03x vertex %zu alpha kicked %02x predicted %02x\n", b.pc, b.itop,
                        k / 2, b.valpha[k], b.pred[k]);
        }
        if (b.valpha[k + 1] != b.pred[k + 1]) c.abe_bad++;
        // detail: (program, input .w, tag ABE) -> kicked (alpha, abe) histogram
        static std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, uint64_t> hist;
        if (hist.size() < 4000)
            hist[{b.pc, b.in_w[k / 2] & 0x8000FFFFu, (uint32_t)(b.pred[k + 1] != 0), (uint32_t)b.valpha[k] << 8 | b.valpha[k + 1]}]++;
        static uint64_t n_seen;
        if (++n_seen == 20000000)
            for (auto &h : hist)
                if (h.second > 200)
                    fprintf(stderr, "[skipdetail] pc %04x in.w %08x tagABE %u -> kicked a %02x abe %02x x%llu\n", std::get<0>(h.first),
                            std::get<1>(h.first), std::get<2>(h.first), std::get<3>(h.first) >> 8, std::get<3>(h.first) & 0xFF,
                            (unsigned long long)h.second);
    }
}
void check_report() {
    if (!skip_check()) return;
    static uint64_t frames;
    if (++frames % 600) return;
    for (auto &e : g_check)
        fprintf(stderr, "[skipcheck] pc %04x itop %03x: %llu batches, %llu drawn, %llu 1:1, %llu verts, alpha mismatches %llu, blend mismatches %llu\n",
                e.first >> 12, e.first & 0xFFF, (unsigned long long)e.second.batches, (unsigned long long)e.second.drawn,
                (unsigned long long)e.second.full, (unsigned long long)e.second.verts, (unsigned long long)e.second.alpha_bad,
                (unsigned long long)e.second.abe_bad);
}

// ---- VU1 skip for replaced batches (EOD_MODERN_SKIPVU=1; off by default, not exact: see skip_enabled)
// A replaced batch's program output is thrown away except three things the modern renderer reads:
// that it kicked (the game never culls inside these programs), the per-vertex alpha and blend
// flags it kicked, and the GS register writes its kicks make. A real run's kicks are recorded
// reduced to their register effects (state_only); once a mesh's kicked alpha/blend pattern and
// its reduced kicks have repeated unchanged for the same state inputs, with no fade, the program is
// skipped: the GS receives the recorded register writes (the state a real run leaves, without
// vertices), the batch's texture and blend state are read from the registers at that point
// (modern::post_batch_state) and the cached alpha pattern is used.
bool skip_enabled() {
    static const bool on = [] {
        // off by default: a skipped run leaves the GS vertex attribute registers as the previous run
        // left them, and the game draws a few PATH2 primitives with that leftover colour (the
        // [skipproof] count); EOD_MODERN_SKIPVU=1 for measurement only
        const char *e = getenv("EOD_MODERN_SKIPVU");
        return e && atoi(e) != 0;
    }();
    return on;
}
bool skip_program(uint32_t pc) {
    switch (pc) {
    case 0x618: case 0x270: case 0x920: case 0x2a8: case 0x1f90: case 0x350: case 0x24b0: case 0x318:
        return true;  // world, props, facing sprites, driver; not the car paint passes (their alpha fades)
    }
    return false;
}
struct SkipEntry {
    std::vector<uint8_t> valpha;
    uint64_t state_key = 0;
    std::vector<std::vector<uint8_t>> state;
    uint32_t stable = 0;  // consecutive runs with an identical pattern
};
std::unordered_map<uint64_t, SkipEntry> g_skip;
uint64_t g_skip_hits, g_skip_runs, g_state_mismatch, g_state_compared;
uint64_t g_learn, g_learn_fade, g_learn_bad, g_learn_empty, g_learn_key;
void skip_learn(const Batch &b) {
    if (!skip_enabled() || !skip_program(b.pc) || b.skipped || b.valpha.size() != 2 * (size_t)b.n || !b.n) return;
    g_learn++;
    if (!b.state_ok) g_learn_bad++;
    else if (b.state_rec.empty()) g_learn_empty++;
    bool always_run = !b.state_ok || b.state_rec.empty();
    for (size_t k = 0; k < b.valpha.size() && !always_run; k += 2)
        if (b.valpha[k] != 0x80) always_run = true;
    if (always_run) {  // a fade, or kicks that cannot be reduced exactly
        g_skip.erase(b.geom);
        return;
    }
    if (g_skip.size() > 200000) g_skip.clear();
    SkipEntry &e = g_skip[b.geom];
    const bool same_inputs = e.state_key == b.state_key;
    if (!same_inputs) g_learn_key++;
    if (e.stable >= 2 && same_inputs) {  // a replay would have been used: was it exact?
        g_state_compared++;
        if (e.state != b.state_rec || e.valpha != b.valpha) g_state_mismatch++;
    }
    if (e.valpha == b.valpha && same_inputs && e.state == b.state_rec) {
        e.stable++;
    } else {
        e.valpha = b.valpha;
        e.state_key = b.state_key;
        e.state = b.state_rec;
        e.stable = 0;
    }
}

// A PATH1 packet reduced to its GS register writes other than vertex data: A+D tags are kept as
// they are; every other tag keeps its PRE/PRIM write with no loops. The vertex attribute registers
// (RGBAQ, Q, ST, UV, FOG) are left as they were: every geometry program writes them before each
// vertex, and the GS counts any drawn vertex that reads one a skipped run did not write
// (gs::attr_taint_reads, "[skipproof]" at exit; must stay 0). False when the packet holds other
// register writes among its vertices or an IMAGE transfer: that batch always runs.
bool state_only(const uint8_t *d, uint32_t qwc, std::vector<uint8_t> &out) {
    uint32_t p = 0;
    size_t last_tag = SIZE_MAX;
    auto vertex_reg = [](uint32_t r) {
        return r == 1 || r == 2 || r == 3 || r == 4 || r == 5 || r == 0xA || r == 0xC || r == 0xD || r == 0xF;
    };
    while (p < qwc) {
        uint64_t lo, hi;
        memcpy(&lo, d + p * 16, 8);
        memcpy(&hi, d + p * 16 + 8, 8);
        p++;
        const uint32_t nloop = (uint32_t)(lo & 0x7FFF), eop = (uint32_t)(lo >> 15) & 1, flg = (uint32_t)(lo >> 58) & 3;
        uint32_t nreg = (uint32_t)(lo >> 60) & 15;
        if (!nreg) nreg = 16;
        if (flg >= 2) return false;
        bool all_ad = flg == 0;
        for (uint32_t r = 0; r < nreg; r++)
            if (((hi >> (4 * r)) & 15) != 0xE) all_ad = false;
        const uint32_t words = flg == 0 ? nloop * nreg : (nloop * nreg + 1) / 2;
        if (p + words > qwc) return false;
        last_tag = out.size();
        if (all_ad) {
            out.insert(out.end(), d + (p - 1) * 16, d + (p + words) * 16);
        } else {
            for (uint32_t r = 0; r < nreg; r++)
                if (!vertex_reg((uint32_t)(hi >> (4 * r)) & 15)) return false;
            const uint64_t tq[2] = {(lo & ~(0x7FFFull | 1ull << 15)) | (uint64_t)eop << 15, hi};
            const uint8_t *tb = (const uint8_t *)tq;
            out.insert(out.end(), tb, tb + 16);
        }
        p += words;
        if (eop) break;
    }
    if (last_tag != SIZE_MAX) out[last_tag + 1] |= 0x80;  // EOP on the last tag (bit 15)
    return true;
}
// The state inputs of a run: the GS packet template and the values the programs patch it from
// (VU1 qwords 20-23, the input header and its TOP+198/199 qwords).
uint64_t state_inputs(const VUState *vu, uint32_t top) {
    uint64_t h = XXH3_64bits(qword(vu, 20), 64);
    h = mix(h, XXH3_64bits(qword(vu, top), 16));
    h = mix(h, XXH3_64bits(qword(vu, top + 198), 16));
    return mix(h, XXH3_64bits(qword(vu, top + 199), 16));
}
void skip_report() {
    static uint64_t frames;
    if (!skip_check() || ++frames % 600) return;
    fprintf(stderr, "[skipstate] replays checked %llu, wrong %llu; learned %llu: not reducible %llu, no kicks %llu, inputs changed %llu; skipped %llu of %llu runs\n",
            (unsigned long long)g_state_compared, (unsigned long long)g_state_mismatch, (unsigned long long)g_learn,
            (unsigned long long)g_learn_bad, (unsigned long long)g_learn_empty, (unsigned long long)g_learn_key,
            (unsigned long long)g_skip_hits, (unsigned long long)g_skip_runs);
}

// EOD_MODERN_SKINCHECK=1: the skinned vertices projected through the batch matrix against the XYZ
// the program kicked (the program then always runs), per program, every 600 frames
bool skin_check() {
    static const bool on = getenv("EOD_MODERN_SKINCHECK") != nullptr;
    return on;
}
// the XY of every XYZ2/XYZF2/XYZ3/XYZF3 a PATH1 packet writes, in GS pixels
void kick_xy(const uint8_t *d, uint32_t qwc, std::vector<float> &out) {
    uint32_t p = 0;
    while (p < qwc) {
        uint64_t lo, hi;
        memcpy(&lo, d + p * 16, 8);
        memcpy(&hi, d + p * 16 + 8, 8);
        p++;
        const uint32_t nloop = (uint32_t)(lo & 0x7FFF), eop = (uint32_t)(lo >> 15) & 1, flg = (uint32_t)(lo >> 58) & 3;
        uint32_t nreg = (uint32_t)(lo >> 60) & 15;
        if (!nreg) nreg = 16;
        if (flg == 0) {
            for (uint32_t l = 0; l < nloop; l++)
                for (uint32_t r = 0; r < nreg; r++, p++) {
                    if (p >= qwc) return;
                    const uint32_t reg = (uint32_t)(hi >> (4 * r)) & 15;
                    if (reg == 4 || reg == 5 || reg == 12 || reg == 13) {
                        uint32_t u[2];
                        memcpy(u, d + p * 16, 8);
                        out.insert(out.end(), {(float)(u[0] & 0xFFFF) / 16.0f, (float)(u[1] & 0xFFFF) / 16.0f});
                    }
                }
        } else if (flg == 1) {
            const uint32_t total = nloop * nreg;
            if (p + (total + 1) / 2 > qwc) return;
            for (uint32_t i = 0; i < total; i++) {
                uint64_t w;
                memcpy(&w, d + p * 16 + i * 8, 8);
                const uint32_t reg = (uint32_t)(hi >> (4 * (i % nreg))) & 15;
                if (reg == 4 || reg == 5 || reg == 12 || reg == 13)
                    out.insert(out.end(), {(float)(w & 0xFFFF) / 16.0f, (float)((w >> 16) & 0xFFFF) / 16.0f});
            }
            p += (total + 1) / 2;
        } else {
            p += nloop;
        }
        if (eop) break;
    }
}
struct SkinStats { uint64_t batches, compared, verts, over1; double max_px, sum_px; };
std::map<uint32_t, SkinStats> g_skin_stats;
void skin_compare(const Batch &b) {
    SkinStats &s = g_skin_stats[b.pc];
    s.batches++;
    if (b.skin_xy.empty() || b.skin_xy.size() != b.kick_xy.size()) return;
    s.compared++;
    for (size_t k = 0; k + 1 < b.skin_xy.size(); k += 2) {
        const double e = std::hypot(b.skin_xy[k] - b.kick_xy[k], b.skin_xy[k + 1] - b.kick_xy[k + 1]);
        s.verts++;
        s.sum_px += e;
        s.max_px = std::max(s.max_px, e);
        if (e > 1.0) s.over1++;
    }
}
void skin_report() {
    static uint64_t frames;
    if (++frames % 600) return;
    for (auto &e : g_skin_stats)
        fprintf(stderr, "[skincheck] pc %04x: %llu batches, %llu compared 1:1, %llu verts, mean %.3f px, max %.3f px, over 1 px %llu\n",
                e.first, (unsigned long long)e.second.batches, (unsigned long long)e.second.compared,
                (unsigned long long)e.second.verts, e.second.verts ? e.second.sum_px / e.second.verts : 0.0, e.second.max_px,
                (unsigned long long)e.second.over1);
}

void post_modern() {
    EOD_ZONE("emit.post_modern");
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
    for (Batch &b : g_batches) {
        if (b.mi < 0) continue;
        modern::Batch &m = f->batches[(size_t)b.mi];
        double M[4][4];
        model_of(b, cinv, M);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++) m.model[r * 4 + c] = (float)M[r][c];
        world_dir(M, b.q17, m.light);
        for (int c = 0; c < 4; c++) m.zrow[c] = b.mvp[c * 4 + 2];
        if (skip_check()) check_prediction(b);
        if (skin_check() && skin_kind(b.pc) != SKIN_NONE) skin_compare(b);
        skip_learn(b);
        if (b.valpha.size() == 2 * (size_t)b.n) m.valpha = std::move(b.valpha);  // 1:1 with the mesh (nothing clipped)
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
    check_report();
    skip_report();
    if (skin_check()) skin_report();
}

// End of a frame: camera vote, model matrices, records, write.
void flush_frame() {
    EOD_ZONE("emit.flush_frame");
    finish_pending();
    if (g_modern && !g_triggered) post_modern();
    g_triggered = false;
    if (g_modern) mark(-1);
    if (!(g_write && emit_range(g_frame))) {  // the records only feed the EOD_SCENE_EMIT file
        g_out.clear();
        g_batches.clear();
        return;
    }
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

bool capture_requested() { return g_path && *g_path; }
bool g_on = capture_requested() || g_modern;

// The sky dome: program 0x270 in ITOP mode 0x280 with the sky's own matrix (camera translation,
// no rotation), all drawn before the first world geometry of the frame. The same program and mode
// also draws terrain (most of the ground in Career free roam) with the camera matrix; those runs
// are world geometry and are replaced like any other.
bool g_sky_known, g_world_seen;
float g_sky_mvp[16];
bool is_sky_dome(const VUState *vu, uint32_t pc, uint32_t itop) {
    if (pc != 0x270 || itop != 0x280) {
        if (geometry_program(pc) && itop != 0x200) g_world_seen = true;
        return false;
    }
    float m[16];
    for (int c = 0; c < 4; c++) memcpy(m + c * 4, vu->vf[4 + c].f, 16);
    if (!g_sky_known && !g_world_seen) {
        memcpy(g_sky_mvp, m, sizeof m);
        g_sky_known = true;
        return true;
    }
    if (g_sky_known && !memcmp(g_sky_mvp, m, sizeof m)) return true;
    g_world_seen = true;
    return false;
}

bool observing_vu() { return g_frame_active; }

bool vu1_start(const VUState *vu, uint32_t pc) {
    if (!observing_vu()) return false;
    EOD_ZONE("emit.vu1_start");
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
    bool replaced = false, skip = false;
    const bool sky_dome = is_sky_dome(vu, pc, g_vif1_itop);
    if (geometry_program(pc)) {
        const uint32_t top = g_vif1_top;
        const uint32_t n = *(const uint32_t *)qword(vu, top) & 0x7FFF;
        if (n > 0 && n <= 120) {
            // content id: the vertex block and its offsets (static meshes repeat every frame)
            // (XXH3 over the contiguous vertex block: the per-qword mix cost ~0.6 us per run, about
            // 5,000 runs a frame on the producer thread)
            uint64_t id = mix(0, n);
            const uint32_t first = ((top + 1) * 16) & vu->mem_mask;
            if (first + 48u * n <= vu->mem_mask + 1u) {
                id = mix(id, XXH3_64bits(vu->mem + first, 48u * n));
            } else {
                for (uint32_t q = 1; q <= 3 * n; q++) {
                    const uint64_t *w = (const uint64_t *)qword(vu, top + q);
                    id = mix(mix(id, w[0]), w[1]);
                }
            }
            id = mix(id, XXH3_64bits(qword(vu, top + 198), 16));
            id = mix(id, XXH3_64bits(qword(vu, top + 199), 16));
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
                if (skip_check()) {
                    // prediction: alpha = the input colour's alpha byte (normal-lit vertices: 0x80),
                    // blend = PRIM.ABE of the tag template the program kicks
                    memcpy(&b.tag, qword(vu, 23), 8);
                    const uint8_t abe = ((b.tag >> 46) & 1) && ((b.tag >> 53) & 1) ? 255 : 0;
                    b.pred.resize(2 * (size_t)n);
                    b.in_w.resize(n);
                    for (uint32_t k = 0; k < n; k++) {
                        uint32_t ni[4];
                        memcpy(ni, qword(vu, top + 1 + 3 * k), 16);
                        const bool normal = (ni[0] & 0xFFFFFF00u) == 0x47800000u;
                        b.pred[2 * k] = normal ? 0x80 : (uint8_t)(ni[3] & 0xFF);
                        b.pred[2 * k + 1] = abe;
                        b.in_w[k] = ni[3] | (normal ? 0x80000000u : 0);
                    }
                }
                // the sky dome (0x270 in ITOP mode 0x280) stays with the GS: the modern pass starts from it
                if (g_live && !hud && !sky_dome && !g_triggered) {
                    if (!g_mf) g_mf = new modern::Frame();
                    b.mi = (int32_t)g_mf->batches.size();
                    modern::Batch m = {};
                    m.mesh = id;
                    m.pc = pc;
                    m.itop = b.itop;
                    g_mf->batches.push_back(m);
                    // bones: this frame's pose (the cached mesh is the bind pose)
                    if (skin_kind(pc) != SKIN_NONE) {
                        modern::Batch &mb = g_mf->batches.back();
                        if (!skin_vertices(vu, pc, top, n, mb.skinned)) mb.skinned.clear();
                        bone_source_probe(vu, pc, top, n);
                        if (skin_check() && !mb.skinned.empty()) {
                            b.skin_xy.reserve(2 * (size_t)n);
                            for (const modern::Vertex &sv : mb.skinned) {
                                float s[4];
                                for (int r = 0; r < 4; r++)
                                    s[r] = vu->vf[4].f[r] * sv.p[0] + vu->vf[5].f[r] * sv.p[1] + vu->vf[6].f[r] * sv.p[2] + vu->vf[7].f[r];
                                b.skin_xy.insert(b.skin_xy.end(), {s[0] / s[3], s[1] / s[3]});
                            }
                        }
                    }
                    mark(b.mi);
                    b.want_alpha = true;
                    replaced = true;
                }
                // a mesh whose kicks are known for these state inputs: the program is not run, its
                // register writes are replayed
                if (replaced && skip_enabled() && skip_program(pc)) b.state_key = state_inputs(vu, top);
                const bool cached = replaced && skip_enabled() && !skip_check() && !skin_check() && skip_program(pc);
                auto sk = cached ? g_skip.find(id) : g_skip.end();
                if (sk != g_skip.end() && sk->second.stable >= 2 && sk->second.state_key == b.state_key) {
                    b.skipped = true;
                    skip = true;
                }
                g_batches.push_back(std::move(b));
                g_pending = true;
                if (skip) {
                    for (const std::vector<uint8_t> &pk : sk->second.state)
                        gif_transfer(GIF_PATH1, pk.data(), (uint32_t)(pk.size() / 16));
                    Batch &cur = g_batches.back();
                    cur.valpha = sk->second.valpha;
                    modern::post_batch_state(cur.mi);
                    g_skip_hits++;
                }
            }
        }
    }
    // the game's own projected car shadows (blurred blobs composited onto the ground): the modern
    // pass casts real shadows, so the blobs are not drawn while it is live
    static const bool keep_blobs = getenv("EOD_MODERN_BLOBS") != nullptr;
    if (g_live && !g_triggered && !keep_blobs && (pc == 0x0020 || pc == 0x0338)) {
        mark(modern::kSuppressed);
        replaced = true;
        // their output is dropped anyway (the modern pass casts real shadows): not run at all
        if (skip_enabled()) skip = true;
    }
    if (g_modern && !replaced) mark(-1);
    // EOD_MODERN_LOGTAG=<frame>: a geometry run's input header (TOP+0, qwords 20-23) for comparison
    // with the GIF tags it kicks (gif() prints them)
    static const uint64_t tag_at = env_u64("EOD_MODERN_LOGTAG", 0);
    if (tag_at && g_frame >= tag_at && g_frame < tag_at + 4 && pc == 0x350) {
        static int dumped;
        if (dumped++ < 3)
            for (uint32_t q = 0; q < 14; q++) {
                const float *f = (const float *)qword(vu, g_vif1_top + q);
                const uint32_t *u = (const uint32_t *)f;
                fprintf(stderr, "[s350] run %u top+%u %g %g %g %g | %08x %08x %08x %08x\n", g_run, q, f[0], f[1], f[2], f[3], u[0], u[1], u[2], u[3]);
            }
    }
    if (tag_at && g_frame >= tag_at && g_frame < tag_at + 4 && geometry_program(pc)) {
        const uint64_t *t0 = (const uint64_t *)qword(vu, g_vif1_top);
        fprintf(stderr, "[tag] run %u pc %04x itop %03x top %03x in0 %016llx %016llx",
                g_run, pc, g_vif1_itop, g_vif1_top, (unsigned long long)t0[0], (unsigned long long)t0[1]);
        for (uint32_t q = 20; q < 24; q++) {
            const uint64_t *w = (const uint64_t *)qword(vu, q);
            fprintf(stderr, " q%u %016llx %016llx", q, (unsigned long long)w[0], (unsigned long long)w[1]);
        }
        fprintf(stderr, "\n");
    }
    // EOD_MODERN_LOGPROGS=<frame>: every VU1 program run in that frame, with whether it was replaced
    static const uint64_t log_at = env_u64("EOD_MODERN_LOGPROGS", 0);
    if (log_at && g_frame == log_at) {
        const uint32_t n = *(const uint32_t *)qword(vu, g_vif1_top) & 0x7FFF;
        fprintf(stderr, "[progs] run %u pc %04x itop %03x n %u geometry %d replaced %d live %d triggered %d\n", g_run, pc,
                g_vif1_itop, n, geometry_program(pc) ? 1 : 0, replaced ? 1 : 0, g_live ? 1 : 0, g_triggered ? 1 : 0);
    }
    g_ns += now_ns() - t;
    if (geometry_program(pc) && replaced) g_skip_runs++;
    return skip;
}

void gif(int path, const uint8_t *data, uint32_t qwc) {
    if (!g_frame_active || path != 1) return;
    EOD_ZONE("emit.gif");
    g_kicks++;
    static const uint64_t tag_at = env_u64("EOD_MODERN_LOGTAG", 0);
    if (tag_at && g_frame >= tag_at && g_frame < tag_at + 4 && qwc == 13 && !g_batches.empty() && g_batches.back().pc == 0x350) {
        static int dumped;
        if (dumped++ < 2)
            for (uint32_t q = 0; q < qwc; q++) {
                const uint32_t *u = (const uint32_t *)(data + q * 16);
                const float *f = (const float *)u;
                fprintf(stderr, "[k350] q%u %08x %08x %08x %08x | %g %g %g %g\n", q, u[0], u[1], u[2], u[3], f[0], f[1], f[2], f[3]);
            }
    }
    if (tag_at && g_frame >= tag_at && g_frame < tag_at + 4 && qwc) {
        const uint64_t *w = (const uint64_t *)data;
        fprintf(stderr, "[tag]   kick run %u qwc %u q0 %016llx %016llx q1 %016llx %016llx\n", g_run, qwc,
                (unsigned long long)w[0], (unsigned long long)w[1], qwc > 1 ? (unsigned long long)w[2] : 0ull,
                qwc > 1 ? (unsigned long long)w[3] : 0ull);
    }
    if (g_pending) {
        g_batches.back().kicks++;
        Batch &cur = g_batches.back();
        if (g_mf && cur.mi >= 0) g_mf->batches[(size_t)cur.mi].drawn = true;
        if (cur.want_alpha) {
            if (cur.valpha.empty()) cur.valpha.reserve(2 * (size_t)cur.n + 8);
            kick_alphas(data, qwc, cur.valpha);
        }
        if (!cur.skin_xy.empty()) kick_xy(data, qwc, cur.kick_xy);
        if (!cur.skipped && cur.mi >= 0 && cur.state_ok && skip_enabled() && skip_program(cur.pc)) {
            std::vector<uint8_t> pk;
            if (state_only(data, qwc, pk)) {
                if (!pk.empty()) cur.state_rec.push_back(std::move(pk));
            } else {
                cur.state_ok = false;
            }
        }
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
    EOD_ZONE("emit.vblank");
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
    g_sky_known = g_world_seen = false;
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
