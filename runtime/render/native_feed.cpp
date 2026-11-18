// native_feed.cpp - EE and VIF1 side of the native renderer (native_renderer.h).
#include "native_renderer.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "../core/config.h"
#include "../core/memory.h"
#include "scene_tap.h"
#include "vif_mesh.h"
#include "vu.h"

extern uint64_t kernel_vblank_count();
extern uint32_t g_vif1_dma_tag;  // runtime/hw/hw.cpp: DMA tag VIF1 is reading

namespace native {

bool enabled() {
    static const bool on = [] {
        const char *e = std::getenv("EOD_NATIVE");
        const bool v = e && *e ? *e != '0' : g_config.native_renderer;
        if (v) {
            scene_tap::set_enabled(true);
            std::fprintf(stderr, "[native] renderer on: queued 3D objects drawn by plume\n");
        }
        return v;
    }();
    return on;
}

namespace {

constexpr uint32_t kBatchPacketRa = 0x3613E0;  // reserve in 0x3613C0: the DMA REF to a batch's VIF data

// Camera packet inputs (0x35B3D0)
constexpr uint32_t kCameraRotation = 0x3E57A8, kCameraPosition = 0x3E57CC;
constexpr uint32_t kViewport = 0x4D7C20, kNear = 0x4D7C10, kFar = 0x4D7C14, kFovScale = 0x4D7C1C;
constexpr uint32_t kAspect = 0x3DD658;

float rf(uint32_t a) {
    const uint32_t u = MEM_LW(a);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// ---- mesh cache (EE thread) ----
struct CacheEntry {
    uint64_t hash = 0;
    uint64_t checked = ~0ull;  // vblank the hash was last compared
    uint32_t id = 0;
};
std::unordered_map<uint32_t, CacheEntry> g_cache;
uint32_t g_next_mesh = 1;
vifmesh::Decoder *g_decoder;
vifmesh::Mesh g_scratch;
std::vector<uint8_t> g_bytes;

// ---- shared EE <-> VIF1 (g_mutex) ----
std::mutex g_mutex;
struct Tagged {
    uint32_t end;
    Packet packet;
};
std::map<uint32_t, Tagged> g_tags;     // batch packet start -> record; latest writer wins
std::vector<Mesh *> g_new_meshes;      // decoded, not yet in the GS stream

// ---- EE state ----
bool g_in_object = false;
Packet g_current;
uint64_t g_frame_vb = ~0ull;
uint32_t g_serial = 0;
// counters (EE frame)
uint32_t g_records = 0, g_tagged = 0, g_meshes_decoded = 0, g_mesh_fail = 0;

// ---- VIF1 state ----
uint64_t g_sent_key = ~0ull;  // (frame << 32 | serial) of the record whose kicks are being sent
bool g_obj_active = false;

uint64_t log_every() {
    static const uint64_t n = [] {
        const char *e = std::getenv("EOD_NATIVE_LOG");
        return e ? std::strtoull(e, nullptr, 10) : 300ull;
    }();
    return n;
}

bool read_guest(void *, uint64_t addr, void *dst, size_t n) {
    ee_read_block((uint32_t)addr, dst, (uint32_t)n);
    return true;
}

// The record's batch as a mesh id, decoding it when it is new or its data changed.
uint32_t mesh_for(uint32_t batch, uint64_t vb) {
    CacheEntry &c = g_cache[batch];
    if (c.checked == vb && c.id) return c.id;
    const uint32_t qwc = MEM_LW(batch + 0x34);
    if (qwc == 0 || qwc > 0x10000) return 0;
    const uint32_t bytes = 0x38 + qwc * 16;
    g_bytes.resize(bytes);
    ee_read_block(batch, g_bytes.data(), bytes);
    const uint64_t h = XXH3_64bits(g_bytes.data(), bytes);
    c.checked = vb;
    if (c.id && c.hash == h) return c.id;
    if (!g_decoder) g_decoder = new vifmesh::Decoder();
    g_scratch.clear();
    vifmesh::Options opt;
    const vifmesh::Status st = g_decoder->decode_vif(g_bytes.data() + 0x38, qwc * 16, g_scratch, opt);
    auto *m = new Mesh();
    m->id = g_next_mesh++;
    m->replaces = c.id;
    m->batch = batch;
    m->skinned = false;
    if (st != vifmesh::Status::Ok) g_mesh_fail++;
    for (const vifmesh::Batch &b : g_scratch.batches)
        if (b.skin == vifmesh::Skin::Matrix || b.skin == vifmesh::Skin::Offset) m->skinned = true;
    m->vertices.resize(g_scratch.vertices.size());
    for (size_t i = 0; i < g_scratch.vertices.size(); i++) {
        const vifmesh::Vertex &s = g_scratch.vertices[i];
        Vertex &d = m->vertices[i];
        std::memcpy(d.p, s.pos, 12);
        std::memcpy(d.uv, s.uv, 8);
        if (s.flags & vifmesh::kHasNormal) {
            for (int k = 0; k < 3; k++)
                d.n[k] = (int8_t)std::lround(std::fmax(-128.0, std::fmin(127.0, (double)s.normal[k] * 127.0)));
            d.n[3] = 1;
        } else {
            d.n[0] = d.n[1] = d.n[2] = d.n[3] = 0;
        }
        std::memcpy(d.c, s.color, 4);
    }
    // indices are part-relative; one part per batch
    m->indices.reserve(g_scratch.indices.size());
    for (const vifmesh::Part &p : g_scratch.parts)
        for (uint32_t i = 0; i < p.index_count; i++)
            m->indices.push_back(p.first_vertex + g_scratch.indices[p.first_index + i]);
    c.hash = h;
    c.id = m->id;
    g_meshes_decoded++;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_new_meshes.push_back(m);
    }
    return c.id;
}

void roll(uint64_t vb) {
    if (vb == g_frame_vb) return;
    if (g_frame_vb != ~0ull && log_every() && g_frame_vb / log_every() != vb / log_every())
        std::fprintf(stderr, "[native] ee vb %llu records %u tagged %u meshes decoded %u (failed %u) cached %zu\n",
                     (unsigned long long)g_frame_vb, g_records, g_tagged, g_meshes_decoded, g_mesh_fail, g_cache.size());
    g_frame_vb = vb;
    g_serial = g_records = g_tagged = 0;
    g_meshes_decoded = g_mesh_fail = 0;
}

}  // namespace

float bits_f(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

void screen_matrix(const Packet &p, double a[4][4]) {
    // 0x35B3D0, in its single-precision order
    const float n = p.near_z, f = p.far_z, w = p.viewport[2], h = p.viewport[3];
    const float fw = p.fov_scale * w * 0.5f;                                      // q5 x, y scale
    const float cx = p.viewport[0] + 2047.9375f + w * 0.5f - 320.0f;              // screen centre
    const float cy = p.viewport[1] + 2047.9375f + h * 0.5f - 224.0f;
    const float sy = bits_f(0xBF6EEEF0u) / p.aspect;                              // -0.9333 / aspect
    const float zk = f * n * bits_f(0x477FDB00u) / (f - n);                       // depth: zk / z + zb
    const float zb = (n * bits_f(0xC77FDC00u) + f) / (f - n);
    // view: R (x - c), rows of R at 0x3E57A8
    double V[4][4] = {};
    for (int r = 0; r < 3; r++) {
        double t = 0;
        for (int c = 0; c < 3; c++) {
            V[r][c] = p.rotation[r * 3 + c];
            t += (double)p.rotation[r * 3 + c] * p.position[c];
        }
        V[r][3] = -t;
    }
    V[3][3] = 1;
    // viewport x projection (q5..8): x' = fw x + cx z, y' = sy fw y + cy z, z' = zk + zb z, w' = z
    const double P[4][4] = {{fw, 0, cx, 0}, {0, (double)sy * fw, cy, 0}, {0, 0, zb, zk}, {0, 0, 1, 0}};
    double M[4][4] = {};
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) M[r][c] = p.model[r * 4 + c];
    M[3][3] = 1;
    double PV[4][4];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            PV[r][c] = 0;
            for (int k = 0; k < 4; k++) PV[r][c] += P[r][k] * V[k][c];
        }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            a[r][c] = 0;
            for (int k = 0; k < 4; k++) a[r][c] += PV[r][k] * M[k][c];
        }
}

void ee_frame_begin() {
    if (!enabled()) return;
    roll(kernel_vblank_count());
}

void ee_object(uint32_t record) {
    if (!enabled()) return;
    const uint64_t vb = kernel_vblank_count();
    if (g_frame_vb == ~0ull) roll(vb);
    const scene_tap::Object *o = scene_tap::last_object();
    Packet &p = g_current;
    std::memset(&p, 0, sizeof p);
    p.frame = g_frame_vb;  // frames run from one FrameBegin in a new vblank to the next (as EOD_RENDER_TRACE)
    p.serial = g_serial++;
    p.batch = MEM_LW(record);
    if (o) {
        std::memcpy(p.model, o->model, sizeof p.model);
        std::memcpy(p.colour_a, o->colour_a, 16);
        std::memcpy(p.colour_b, o->colour_b, 16);
        std::memcpy(p.light, o->light, 16);
        p.texture_key = o->texture_key;
        p.layer = o->layer;
    } else {
        p.model[0] = p.model[5] = p.model[10] = 1;
    }
    for (int i = 0; i < 9; i++) p.rotation[i] = rf(kCameraRotation + 4u * (uint32_t)i);
    for (int i = 0; i < 3; i++) p.position[i] = rf(kCameraPosition + 4u * (uint32_t)i);
    for (int i = 0; i < 4; i++) p.viewport[i] = rf(kViewport + 4u * (uint32_t)i);
    p.near_z = rf(kNear);
    p.far_z = rf(kFar);
    p.fov_scale = rf(kFovScale);
    p.aspect = rf(kAspect);
    p.mesh = mesh_for(p.batch, vb);
    g_records++;
    g_in_object = true;
}

void ee_object_end() { g_in_object = false; }

void ee_packet_reserved(uint32_t addr, uint32_t qwc, uint32_t ra) {
    if (!addr) return;
    const bool batch = g_in_object && ra == kBatchPacketRa;
    const uint32_t end = addr + (qwc ? qwc : 1) * 16;
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!batch && g_tags.empty()) return;
    // drop older entries this range overlaps
    auto it = g_tags.lower_bound(addr);
    if (it != g_tags.begin()) {
        auto prev = std::prev(it);
        if (prev->second.end > addr) g_tags.erase(prev);
    }
    it = g_tags.lower_bound(addr);
    while (it != g_tags.end() && it->first < end) it = g_tags.erase(it);
    if (!batch) return;  // any other packet there replaces a record's tag
    g_tags[addr] = Tagged{end, g_current};
    g_tagged++;
}

// EOD_NATIVE_CHECK=1: the derived model -> screen matrix against the one VU1 uses for the record
// (vf4..vf7 after the setup program 0x0000), as the largest error relative to the column size.
void check(const Packet &p) {
    static const bool on = std::getenv("EOD_NATIVE_CHECK") != nullptr;
    if (!on) return;
    static double worst = 0;
    static uint64_t n = 0, last_vb = 0;
    double a[4][4];
    screen_matrix(p, a);
    double err = 0;
    for (int c = 0; c < 4; c++) {
        double mag = 0, d = 0;
        for (int r = 0; r < 4; r++) {
            mag = std::fmax(mag, std::fabs(a[r][c]));
            d = std::fmax(d, std::fabs(a[r][c] - (double)g_vu1.vf[4 + c].f[r]));
        }
        err = std::fmax(err, d / std::fmax(mag, 1e-6));
    }
    worst = std::fmax(worst, err);
    n++;
    if (p.frame / 300 > last_vb / 300 || n == 1) {
        std::fprintf(stderr, "[native] check vb %llu: %llu records, worst relative matrix error %.3g (this %.3g)\n",
                     (unsigned long long)p.frame, (unsigned long long)n, worst, err);
        last_vb = p.frame;
    }
}

void vu1_start(uint32_t program, uint32_t itop) {
    if (!enabled()) return;
    const uint32_t tag = g_vif1_dma_tag;
    std::vector<Mesh *> meshes;
    Packet p;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_tags.upper_bound(tag);
        if (it != g_tags.begin()) {
            --it;
            // the tag VIF1 reads must still be the record's DMA REF to its batch data
            if (tag < it->second.end && (MEM_LW(tag + 4) & 0x7FFFFFFF) == it->second.packet.batch + 0x38) {
                p = it->second.packet;
                found = true;
            }
        }
        if (found && !g_new_meshes.empty()) meshes.swap(g_new_meshes);
    }
    if (!found) {
        if (g_obj_active) {
            post(CMD_OFF, nullptr, 0);
            g_obj_active = false;
            g_sent_key = ~0ull;
        }
        return;
    }
    for (Mesh *m : meshes) post(CMD_MESH, &m, sizeof m);
    const uint64_t key = p.frame << 32 | p.serial;
    if (g_obj_active && key == g_sent_key) return;  // another program of the same record
    p.itop = itop;
    p.program = program;
    check(p);
    post(CMD_OBJ, &p, sizeof p);
    g_obj_active = true;
    g_sent_key = key;
}

}  // namespace native
