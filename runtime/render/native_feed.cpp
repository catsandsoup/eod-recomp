// native_feed.cpp - EE and VIF1 side of the native renderer (native_renderer.h).
#include "native_renderer.h"
#include "editor_environment.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "../core/config.h"
#include "../core/diff_harness.h"
#include "../core/host_file.h"
#include "../core/limit_tracker.h"
#include "../core/memory.h"
#include "../hw/offload.h"
#include "scene_tap.h"
#include "sun_cascades.h"
#include "vif_mesh.h"
#include "vu.h"

extern uint64_t kernel_vblank_count();
extern uint32_t g_vif1_dma_tag;  // runtime/hw/hw.cpp: DMA tag VIF1 is reading

namespace native {

bool enabled() {
    static const bool on = [] {
        const char *e = std::getenv("EOD_NATIVE");
        const char *only = std::getenv("EOD_NATIVE_ONLY");
        const bool v = (e && *e ? *e != '0' : g_config.native_renderer) || (only && *only && *only != '0');
        if (v) {
            scene_tap::set_enabled(true);
            std::fprintf(stderr, "[native] renderer on: queued 3D objects, track world, water, shadows and flares drawn by plume\n");
        }
        return v;
    }();
    return on;
}

const editor_environment::Override *environment_override() {
#ifdef EOD_PLUME
    static const auto settings = [] {
        editor_environment::Override out;
        const char *root = std::getenv("EOD_MOD_DIR");
        if (!root || !*root) return out;
        const std::string path = std::string(root) + "/env_override.ini";
        FILE *file = host_fopen(path.c_str(), "rb");
        if (!file) return out;
        char data[8192];
        const size_t size = std::fread(data, 1, sizeof data, file);
        const bool complete = std::feof(file) != 0 && !std::ferror(file);
        std::fclose(file);
        if (complete) {
            std::istringstream stream(std::string(data, size));
            editor_environment::parse(stream, out);
        }
        return out;
    }();
    if (!settings.track.empty()) return &settings;
#endif
    return nullptr;
}

bool scene_clear_enabled() {
#ifdef EOD_PLUME
    static const bool enabled = [] {
        const char *value = std::getenv("EOD_NATIVE_SCENE_CLEAR");
        return !value || value[0] != '0';
    }();
    return enabled;
#else
    return false;
#endif
}

bool bootstrap_quad_enabled() {
#ifdef EOD_PLUME
    static const bool enabled = [] {
        const char *value = std::getenv("EOD_NATIVE_BOOT_QUAD");
        return !value || value[0] != '0';
    }();
    return enabled;
#else
    return false;
#endif
}

// Tag updates follow VIF1 DMA work in producer order. The optional VU1
// worker consumes the previous transfer before it sees a new value.
static BootstrapQuad g_bootstrap_quad;
static bool g_bootstrap_quad_pending;

void ee_tag_bootstrap_quad(const BootstrapQuad &quad) {
    offload_run([quad] {
        g_bootstrap_quad = quad;
        g_bootstrap_quad_pending = true;
    });
}

bool take_bootstrap_quad(uint32_t dma_tag, uint32_t qwc, BootstrapQuad &quad) {
    if (dma_tag != kBootstrapQuadDmaTag || qwc != kBootstrapQuadDirectQwc || !g_bootstrap_quad_pending)
        return false;
    g_bootstrap_quad_pending = false;
    quad = g_bootstrap_quad;
    return true;
}

bool bootstrap_raster_enabled() {
#ifdef EOD_PLUME
    static const bool enabled = [] {
        const char *value = std::getenv("EOD_NATIVE_BOOTSTRAP_RASTER");
        return value && value[0] == '1';
    }();
    return enabled && native_only();
#else
    return false;
#endif
}

bool display_copy_enabled() {
#ifdef EOD_PLUME
    static const bool enabled = [] {
        const char *value = std::getenv("EOD_NATIVE_DISPLAY_COPY");
        return !value || value[0] != '0';
    }();
    return enabled;
#else
    return false;
#endif
}

bool sun_cascades_enabled() {
#ifdef EOD_PLUME
    static const bool enabled = [] {
        const char *value = std::getenv("EOD_NATIVE_SUN_CASCADES");
        return !value || value[0] == '1';
    }();
    return enabled;
#else
    return false;
#endif
}

namespace {

bool full_shadow_pass_enabled() {
#ifdef EOD_PLUME
    static const bool on = [] {
        const char *value = std::getenv("EOD_NATIVE_SHADOW_PASS");
        return !value || value[0] != '0';
    }();
    return on && (native_only() || sun_cascades_enabled());
#else
    return false;
#endif
}

bool split_shadow_pass_enabled() {
#ifdef EOD_PLUME
    static const bool on = [] {
        const char *value = std::getenv("EOD_NATIVE_SPLIT_SHADOW_PASS");
        return value && value[0] == '1';
    }();
    return on;
#else
    return false;
#endif
}

bool shadow_pass_enabled() { return full_shadow_pass_enabled() || split_shadow_pass_enabled(); }

// EE-thread selection, latched before ShadowTargetOpen for both ordered DIRECT packets.
bool g_hybrid_shadow_cascades = false;
int g_retail_split_y = -1;

uint32_t shadow_direct_qwc(ShadowPass pass) {
    return shadow_prepares(pass) ? kShadowPrepareDirectQwc : kShadowCompositeDirectQwc;
}

ShadowPass shadow_reservation(uint32_t ra, uint32_t qwc) {
    const ShadowPass pass = ra == 0x362a34 ? ShadowPass::Prepare
                          : ra == 0x362f34 ? ShadowPass::Composite : ShadowPass::None;
    if (pass == ShadowPass::None || qwc != shadow_direct_qwc(pass) + 1) return ShadowPass::None;
    // 0x363218 saves the main viewport and the temporary target's format.
    constexpr uint32_t rt = 0x4d8bd8;
    if (MEM_LW(rt) != 320 || MEM_LW(rt + 0x10) != 0x19f || MEM_LW(rt + 0x14) != 2 ||
        MEM_LW(rt + 0x18) != 0x15f || MEM_LW(rt + 0x1c) != 0x32 ||
        MEM_LW(rt + 0x28) || MEM_LW(rt + 0x30) != 640)
        return ShadowPass::None;

    const uint32_t top = MEM_LW(rt + 0x2c);
    const uint32_t height = MEM_LW(rt + 0x34);
    if (MEM_LW(rt + 4) != height) return ShadowPass::None;
    if (!top && height == 448 && full_shadow_pass_enabled() && (native_only() || g_hybrid_shadow_cascades))
        return pass;
    if (!split_shadow_pass_enabled() || height != 224 || int(top) != g_retail_split_y) return ShadowPass::None;
    if (!top) return shadow_prepares(pass) ? ShadowPass::PrepareUpper : ShadowPass::CompositeUpper;
    if (top == 224) return shadow_prepares(pass) ? ShadowPass::PrepareLower : ShadowPass::CompositeLower;
    return ShadowPass::None;
}

bool g_editor_environment = false;

#ifdef EOD_PLUME
bool matches_loaded_graf(const editor_environment::Override &settings) {
    const uint32_t graf = ee_r32(0x003D1C18); // 0x214678: loaded track.grf data
    if (graf < 8 || graf >= EE_RAM_SIZE || (graf & 3) || ee_r32(graf - 4) != 0x4D474921) return false;
    const uint32_t record = ee_r32(graf - 8); // 0x130D90: record back-pointer, before payload
    constexpr uint32_t stride = 0x44;
    for (const uint32_t list : {0x0043A0E0u, 0x0043A0E4u}) {
        uint32_t archive = ee_r32(list);
        for (unsigned i = 0; i < 256 && archive && !(archive & 3) && archive <= EE_RAM_SIZE - 0x144; ++i) {
            const uint32_t entries = ee_r32(archive + 0x124), count = ee_r32(archive + 0x140);
            if (entries && record >= entries && record < EE_RAM_SIZE && !(record & 3) &&
                (record - entries) % stride == 0 && (record - entries) / stride < count) {
                char name[64]{};
                ee_read_string(archive, name, sizeof name);
                return editor_environment::applies(settings, name, true);
            }
            archive = ee_r32(archive + 0x120);
        }
    }
    return false;
}
#endif

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
// A vertex of a camera-facing batch (VU1 0x350, MSCAL 0x6A): p + wa C[a] + wb C[b], where C are the
// sun quad corners at VU1 qwords 0xAC..0xAF (bone 29..32 = qword 143 + bone).
struct FacingVertex {
    uint32_t vertex;
    uint8_t corner[2];      // bone - 29 (4 = not a corner: no offset)
    float weight[2];        // lane - 32768 = byte / 256
};
// A skinned vertex (R3, docs/NATIVE_RENDERER.md "Stage R3"): the skin word the program reads at
// TOP+149+n-k, bone fields as VU1 qword bases.
//   offset (0x2E0/0xD48/0x15A8, V4-8 or V1-8): p' = wa D[a] + wb D[b] + p, D = qword bone+143 xyz
//   matrix (0x318/0x24B0): p' = wa Pa + wb Pb, P = c0 x + c1 y + c2 z + c3 w with the columns at
//   qwords bone+144..147; the normal turns with c0..c2 the same way
struct SkinVertex {
    uint32_t vertex;
    uint8_t matrix;         // 1 = matrix palette, 0 = dent offsets
    uint16_t bone[2];
    float weight[2];        // lane - 32768
    float w;                // the position qword's w lane
    int8_t n[3];            // the attribute bytes - 128 (matrix: rotated with the palette)
    bool has_n;
};
struct CacheEntry {
    uint64_t hash = 0;
    uint64_t checked = ~0ull;  // vblank the hash was last compared
    uint32_t id = 0;
    // camera-facing parts and skinned batches keep their decoded geometry on the EE side and get a
    // transient mesh per draw
    std::vector<Vertex> base;
    std::vector<uint32_t> indices;
    std::vector<FacingVertex> facing;
    std::vector<SkinVertex> skin;
    int32_t k_delta = 0;  // the decoded mesh's TEX1.K delta (Mesh::k_delta), for the transient copies
    bool k_kick = false, k_mixed = false;
    std::vector<Mesh::KRun> k_runs;
};
std::unordered_map<uint32_t, CacheEntry> g_cache;

void copy_k(Mesh *m, const CacheEntry &c) {
    m->k_delta = c.k_delta;
    m->k_kick = c.k_kick;
    m->k_mixed = c.k_mixed;
    m->k_runs = c.k_runs;
}
uint32_t g_next_mesh = 1;
vifmesh::Decoder *g_decoder;
vifmesh::Mesh g_scratch;
std::vector<uint8_t> g_bytes;

// ---- shared EE <-> VIF1 (g_mutex) ----
std::mutex g_mutex;
std::map<uint32_t, SceneClear> g_scene_clear_packets;
std::map<uint32_t, DisplayCopy> g_display_copy_packets;
std::map<uint32_t, ShadowPass> g_shadow_pass_packets;
struct Tagged {
    uint32_t end;
    Packet packet;
    bool range;  // any DMA tag in [start, end) (immediate geometry); else the REF to batch + 0x38
    bool retire_world_leaf = false;
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

// ---- EE state: track world (0x358BD0) and water (0x325728) ----
constexpr uint32_t kImmBeginRa = 0x3614CC;      // reserve in 0x361468 (immediate geometry begin)
constexpr uint32_t kImmVertexRa = 0x3617E4;     // reserve in 0x361708 (next chunk)
constexpr uint32_t kImmMoreRa = 0x364DF0;       // reserve in 0x364D18 (strip continuation)
constexpr uint32_t kPartReserveRa = 0x364034;   // reserve in 0x363FF8 (particles)
constexpr uint32_t kSpriteReserveRa = 0x364584; // reserve in 0x364548 (simple effect sprites)
constexpr uint32_t kAtlasReserveRa = 0x3642F4;  // reserve in 0x3642B8 (atlas effect sprites)
constexpr uint32_t kMlist = 0x4D6F10;           // the mlist 0x358BD0 draws (set by 0x3582B8)
constexpr uint32_t kMaterialPacket = 0x4D7C54;  // material packet 0x35BA10 sends (sun quad for 0x6A)
bool g_in_water = false;
bool g_in_flare = false;
bool g_in_shadow = false;
bool g_in_particle = false;
bool g_in_sprite = false;
bool g_in_imm = false;
bool g_imm_cockpit = false;
bool g_imm_hud = false;
int32_t g_shadow_intensity = 0;
Packet g_shadow_cam;
int32_t g_world_pass = 0;
std::vector<std::pair<uint32_t, Packet>> g_leaves;  // this texture record's REFs, until its commit
Packet g_water;
Mesh *g_water_mesh = nullptr;
Packet g_flare;
Mesh *g_flare_mesh = nullptr;
Packet g_particle;
Mesh *g_particle_mesh = nullptr;
float g_particle_scale = 0.0f;
uint32_t g_particle_count = 0;
bool g_particle_alpha_set = false;
Packet g_sprite;
Mesh *g_sprite_mesh = nullptr;
uint32_t g_sprite_count = 0;
bool g_sprite_alpha_set = false;
Packet g_imm;
Mesh *g_imm_mesh = nullptr;
bool g_imm_alpha_set = false;
// counters (EE frame)
struct WorldCounts {
    uint32_t passes = 0, leaves = 0, facing = 0, no_quad = 0, nomesh = 0, dropped_records = 0, dropped_leaves = 0;
    uint32_t water_draws = 0, water_vertices = 0, water_tris = 0;
    uint32_t shadow_batches = 0, flare_draws = 0, flare_vertices = 0, flare_tris = 0;
    uint32_t particle_draws = 0, particle_quads = 0, sprite_draws = 0, sprite_quads = 0;
    uint32_t imm_draws = 0, imm_verts = 0, imm_tris = 0, cockpit_draws = 0, sphere_imm_draws = 0, hud_draws = 0;
};
WorldCounts g_wc;

// ---- EE state: VU1 uploads and skinning (stage R3) ----
constexpr uint32_t kVuQwords = 1024;            // VU1 data memory
struct Upload {
    uint32_t qword = 0, comps = 0, count = 0;
    uint64_t frame = 0;
    std::vector<uint32_t> data;                 // count * comps words, as the packet carries them
};
std::unordered_map<uint32_t, Upload> g_uploads;  // packet -> upload (the latest build of that packet)
uint32_t g_vu[kVuQwords][4];                     // the bone qwords as the uploads in effect left them
uint32_t g_vu_from[kVuQwords];                   // packet that wrote each qword (0 = none yet)
uint32_t g_applied = 0;                          // material packet last applied
uint64_t g_applied_frame = ~0ull;
struct SkinCounts {
    uint32_t records = 0, posed = 0, undented = 0, matrix = 0, offset = 0, facing = 0, no_upload = 0;
    uint32_t uploads = 0, palettes = 0, dents = 0, quads = 0, uncovered = 0;
};
SkinCounts g_sc;
struct SkinTotals {
    uint64_t pose_checks = 0, pose_words = 0, pose_mismatch = 0;
};
SkinTotals g_st;
void apply_material();
uint32_t posed_mesh(const CacheEntry &c, const Packet &p);
uint32_t facing_queue_mesh(const CacheEntry &c, uint32_t batch);

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
// World parts (track.grf, prelit V4-5) decode with raw strip winding (fix_winding = false); they
// are drawn without culling.
uint32_t mesh_for(uint32_t batch, uint64_t vb, bool world = false, CacheEntry **entry = nullptr) {
    CacheEntry &c = g_cache[batch];
    if (entry) *entry = &c;
    if (c.checked == vb && c.id) return c.id;
    const uint32_t qwc = MEM_LW(batch + 0x34);
    if (qwc == 0 || qwc > 0x10000) return 0;
    const uint32_t bytes = 0x38 + qwc * 16;
    g_bytes.resize(bytes);
    ee_read_block(batch, g_bytes.data(), bytes);
    const uint64_t h = XXH3_64bits(g_bytes.data(), bytes);
    c.checked = vb;
    if (c.id && c.hash == h) return c.id;
    c.base.clear();
    c.indices.clear();
    c.facing.clear();
    c.skin.clear();
    if (!g_decoder) g_decoder = new vifmesh::Decoder();
    g_scratch.clear();
    vifmesh::Options opt;
    opt.fix_winding = !world;
    opt.keep_shadow_extrusion_faces = true;
    const vifmesh::Status st = g_decoder->decode_vif(g_bytes.data() + 0x38, qwc * 16, g_scratch, opt);
    auto *m = new Mesh();
    m->id = g_next_mesh++;
    m->replaces = c.id;
    m->batch = batch;
    m->skinned = false;
    if (st != vifmesh::Status::Ok) g_mesh_fail++;
    bool has_shadow = false;
    for (size_t bi = 0; bi < g_scratch.batches.size(); bi++) {
        const vifmesh::Batch &b = g_scratch.batches[bi];
        // TEX1.K delta: VU1 `mtir vi1, vf13z` (batch qword 198 z, low 16 bits, signed), kicked when
        // not positive with K = template + z + 1000 (docs/NATIVE_RENDERER.md "Stage R5", A2)
        const int16_t z = (int16_t)(b.q198_zw[0] & 0xFFFF);
        const int32_t delta = (int16_t)(uint16_t)(z + 1000);
        if (bi == 0) {
            m->k_delta = delta;
            m->k_kick = z <= 0;
        } else if (m->k_kick != (z <= 0) || (m->k_kick && m->k_delta != delta)) {
            m->k_mixed = true;
        }
        // one decode, one part: the batch's indices are m->indices[first_index, + index_count)
        if (b.index_count) m->k_runs.push_back({b.first_index, b.index_count, delta, z <= 0});
        if (b.shadow) has_shadow = true;
        if (b.skin != vifmesh::Skin::None) {
            m->skinned = true;
            for (uint32_t v = b.first_vertex; v < b.first_vertex + b.vertex_count; v++) {
                const vifmesh::Vertex &s = g_scratch.vertices[v];
                if (!(s.flags & vifmesh::kHasSkin)) continue;
                SkinVertex k{};
                k.vertex = v;
                k.matrix = b.skin == vifmesh::Skin::Matrix ? 1 : 0;
                for (int j = 0; j < 2; j++) k.bone[j] = s.bone[j], k.weight[j] = s.weight[j];
                k.w = s.pos_w;
                k.has_n = (s.flags & vifmesh::kHasNormal) != 0;
                for (int j = 0; j < 3; j++) k.n[j] = (int8_t)std::lround(std::fmax(-128.0, std::fmin(127.0, (double)s.normal[j] * 127.0)));
                c.skin.push_back(k);
            }
        }
        if (b.program != 0x6A) continue;
        for (uint32_t v = b.first_vertex; v < b.first_vertex + b.vertex_count; v++) {
            const vifmesh::Vertex &s = g_scratch.vertices[v];
            FacingVertex f{v, {4, 4}, {0, 0}};
            if (s.flags & vifmesh::kHasAux) {
                for (int k = 0; k < 2; k++) {
                    const int corner = (int)s.aux[k] + 143 - 0xAC;  // lq 143(vi5): qword 143 + bone
                    f.corner[k] = corner >= 0 && corner < 4 ? (uint8_t)corner : 4;
                    f.weight[k] = (float)s.aux[2 + k] / 256.0f;  // V4-8 offset 32768.0f: lane - 32768
                }
            }
            c.facing.push_back(f);
        }
    }
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
    c.k_delta = m->k_delta;
    c.k_kick = m->k_kick;
    c.k_mixed = m->k_mixed;
    {
        // the runs must tile the index list (one part, batches in order)
        uint32_t next = 0;
        for (const Mesh::KRun &kr : m->k_runs) next = kr.first == next ? next + kr.count : ~0u;
        if (!m->k_mixed || g_scratch.parts.size() != 1 || next != m->indices.size()) {
            static uint32_t untiled = 0;  // mixed TEX1.K, runs not tiling the indices: drawn with the first K
            if (m->k_mixed && untiled++ < 3)
                std::fprintf(stderr, "[native] batch %08x: TEX1.K runs do not tile its indices (first run's K used)\n", batch);
            m->k_runs.clear();
        }
    }
    c.k_runs = m->k_runs;
    if (!c.facing.empty() || !c.skin.empty() || has_shadow) {
        c.base = m->vertices;
        c.indices = m->indices;
    }
    g_meshes_decoded++;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_new_meshes.push_back(m);
    }
    return c.id;
}

// What the EE submitted per frame, for the GS side's P1 check (take_expected).
using Expect = Expected;
std::mutex g_expect_mutex;
std::map<uint64_t, Expect> g_expect;

void roll(uint64_t vb) {
    if (vb == g_frame_vb) return;
    if (g_frame_vb != ~0ull) {
        std::lock_guard<std::mutex> lk(g_expect_mutex);
        Expect &e = g_expect[g_frame_vb];
        e.world += g_wc.leaves;
        e.water += g_wc.water_draws;
        e.queue += g_records;
        e.shadow += g_wc.shadow_batches;
        e.flare += g_wc.flare_draws;
        e.particle += g_wc.particle_draws;
        e.sprite += g_wc.sprite_draws;
        e.immediate += g_wc.imm_draws + g_wc.sphere_imm_draws;
        e.cockpit += g_wc.cockpit_draws;
        e.hud += g_wc.hud_draws;
    }
    if (g_frame_vb != ~0ull && log_every() && (log_every() == 1 || g_frame_vb / log_every() != vb / log_every())) {
        std::fprintf(stderr, "[native] ee vb %llu records %u tagged %u meshes decoded %u (failed %u) cached %zu\n",
                     (unsigned long long)g_frame_vb, g_records, g_tagged, g_meshes_decoded, g_mesh_fail, g_cache.size());
        std::fprintf(stderr,
                     "[native] ee vb %llu world passes %u leaves %u (camera-facing %u, no sun quad %u, no mesh %u) "
                     "dropped (no packet space) records %u leaves %u | water draws %u vertices %u triangles %u | "
                     "shadows %u flares %u (verts %u tris %u) | particles %u (%u) sprites %u (%u) imm %u+%u cockpit %u hud %u\n",
                     (unsigned long long)g_frame_vb, g_wc.passes, g_wc.leaves, g_wc.facing, g_wc.no_quad, g_wc.nomesh,
                     g_wc.dropped_records, g_wc.dropped_leaves, g_wc.water_draws, g_wc.water_vertices, g_wc.water_tris,
                     g_wc.shadow_batches, g_wc.flare_draws, g_wc.flare_vertices, g_wc.flare_tris,
                     g_wc.particle_draws, g_wc.particle_quads, g_wc.sprite_draws, g_wc.sprite_quads,
                     g_wc.imm_draws, g_wc.sphere_imm_draws, g_wc.cockpit_draws, g_wc.hud_draws);
    }
    if (g_frame_vb != ~0ull && log_every() && (log_every() == 1 || g_frame_vb / log_every() != vb / log_every())) {
        std::fprintf(stderr,
                     "[native] ee vb %llu skin records %u posed %u (matrix %u, dents %u, undented %u, no upload %u, "
                     "uncovered refs %u) camera-facing queue records %u | uploads %u (palettes %u, dents %u, sun quads %u) "
                     "kept %zu | P2 pose checks %llu words %llu mismatched %llu\n",
                     (unsigned long long)g_frame_vb, g_sc.records, g_sc.posed, g_sc.matrix, g_sc.offset, g_sc.undented,
                     g_sc.no_upload, g_sc.uncovered, g_sc.facing, g_sc.uploads, g_sc.palettes, g_sc.dents, g_sc.quads,
                     g_uploads.size(), (unsigned long long)g_st.pose_checks, (unsigned long long)g_st.pose_words,
                     (unsigned long long)g_st.pose_mismatch);
    }
    if (g_frame_vb != ~0ull) {
        // uploads are rebuilt every frame; packets not rebuilt for 16 vblanks are released
        for (auto it = g_uploads.begin(); it != g_uploads.end();)
            it = it->second.frame + 16 < vb ? g_uploads.erase(it) : std::next(it);
        limit_note("native VU1 upload packets", g_uploads.size(), 0, 0);
    }
    g_sc = SkinCounts();
    g_frame_vb = vb;
    g_serial = g_records = g_tagged = 0;
    g_meshes_decoded = g_mesh_fail = 0;
    g_wc = WorldCounts();
}

// Fog state as the decompiled setters left it (src/native_fog.cpp). Initial values: 0x360E18.
struct FogState {
    uint32_t flags = 0;            // *0x4D8BC8 before the first 0x365540 (bit 3 clear either way)
    uint32_t fogcol = 0xFF;        // 0x360E18: *0x4D8B80 = 0xFF
    float range[3] = {200.0f, 1000.0f, 0.5f};  // 0x360E18: 0x43480000, 0x447A0000, 0x3F000000
};
FogState g_fog;

// Texture LOD and blend state as the decompiled setters left it (src/native_texstate.cpp).
// Initial values: 0x360E18 / 0x365578 (sel 1, K 0, MXL 1, bias 1.0 -> (int)(16 - 1000) = -984).
struct TexState {
    int32_t k = 0;
    uint32_t mxl = 1, bias_k = 0xFC28FC28u, blend_sel = 1;
};
TexState g_tex;

// The camera packet inputs (0x35B3D0) as they are now.
void fill_camera(Packet &p) {
    p.editor_environment = g_editor_environment;
    p.fge = (g_fog.flags >> 3) & 1;  // 0x3620A8: PRIM.FGE = (flags & 8) << 2
    p.flags = g_fog.flags;
    p.blend_sel = g_tex.blend_sel;
    p.tex_k = g_tex.k;
    p.tex_mxl = g_tex.mxl;
    p.bias_k = g_tex.bias_k;
    p.fogcol = g_fog.fogcol & 0xFFFFFF;
    for (int i = 0; i < 3; i++) p.fog_range[i] = g_fog.range[i];
    for (int i = 0; i < 9; i++) p.rotation[i] = rf(kCameraRotation + 4u * (uint32_t)i);
    for (int i = 0; i < 3; i++) p.position[i] = rf(kCameraPosition + 4u * (uint32_t)i);
    for (int i = 0; i < 4; i++) p.viewport[i] = rf(kViewport + 4u * (uint32_t)i);
    p.near_z = rf(kNear);
    {
        // the GS 1 / Q is the native screen w / fw, fw = *0x4D7C1C * width * 0.5 (0x35B3D0 fVar7):
        // measured (EOD_NATIVE_QCHECK) to 5 digits on every textured vertex checked
        const float fw = rf(kFovScale) * rf(kViewport + 8) * 0.5f;
        p.w_scale = fw > 0.0f ? 1.0f / fw : 1.0f;
    }
    p.far_z = rf(kFar);
    p.fov_scale = rf(kFovScale);
    p.aspect = rf(kAspect);
}

// The model matrix and colours of a matrix packet (0x35CD28; layout as scene_tap::object).
void read_matrix_packet(uint32_t packet, Packet &p) {
    std::memset(p.model, 0, sizeof p.model);
    if (!packet) {
        p.model[0] = p.model[5] = p.model[10] = 1;
        return;
    }
    for (int row = 0; row < 3; ++row)
        for (int c = 0; c < 4; ++c) p.model[row * 4 + c] = rf(packet + 0x10 + 16u * (uint32_t)c + 4u * (uint32_t)row);
    for (int i = 0; i < 4; i++) {
        p.colour_a[i] = rf(packet + 0x50 + 4u * (uint32_t)i);
        p.colour_b[i] = rf(packet + 0x60 + 4u * (uint32_t)i);
        p.light[i] = rf(packet + 0x70 + 4u * (uint32_t)i);
    }
}

// Once per mlist (EOD_NATIVE_WORLD_LIST=1): which display objects each pass calls (vtable +0x14).
void describe_mlist(uint32_t fp) {
    static std::vector<uint32_t> seen;
    for (uint32_t s : seen)
        if (s == fp) return;
    seen.push_back(fp);
    const uint32_t list = MEM_LW(fp + 0x90);
    for (uint32_t pass = 0; pass < 5; pass++) {
        const uint32_t pr = fp + 0x38 + pass * 16;
        const int32_t tf = (int32_t)MEM_LW(pr), te = (int32_t)MEM_LW(pr + 4);
        const int32_t of = (int32_t)MEM_LW(pr + 8), oe = (int32_t)MEM_LW(pr + 12);
        std::map<uint32_t, uint32_t> fns;
        for (int32_t k = of; k < oe && k - of < 4096; k++) {
            const uint32_t obj = MEM_LW(list + 4u * (uint32_t)k);
            fns[obj ? MEM_LW(MEM_LW(obj) + 0x14) : 0]++;
        }
        std::string s;
        for (auto &f : fns) {
            char t[32];
            std::snprintf(t, sizeof t, " %06x:%u", f.first, f.second);
            s += t;
        }
        std::fprintf(stderr, "[native] world mlist %08x pass %u texture records %d..%d objects %d..%d draw fns:%s\n", fp,
                     pass, tf, te, of, oe, s.c_str());
        // the texture name of each record's first part (4SPS +8)
        std::string names;
        for (int32_t r = tf; r < te && r - tf < 512; r++) {
            const uint32_t rec = MEM_LW(fp + 0x34) + (uint32_t)r * 0x1C;
            const uint32_t part = MEM_LW(MEM_LW(fp + 0x2C) + MEM_LW(rec + 0x14) * 12 + 8);
            char n[17] = {};
            for (int k = 0; k < 16 && part; k++) n[k] = (char)MEM_LB(part + 8 + (uint32_t)k);
            names += " ";
            names += n;
        }
        std::fprintf(stderr, "[native] world mlist %08x pass %u textures:%s\n", fp, pass, names.c_str());
    }
}

}  // namespace

float bits_f(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

void screen_matrix(const Packet &p, double a[4][4], bool vu_rounding) {
    // 0x35B3D0, in its single-precision order
    const float n = p.near_z, f = p.far_z, w = p.viewport[2], h = p.viewport[3];
    const float fw = p.fov_scale * w * 0.5f;                                      // q5 x, y scale
    const float cx = p.viewport[0] + 2047.9375f + w * 0.5f - 320.0f;              // screen centre
    const float cy = p.viewport[1] + 2047.9375f + h * 0.5f - 224.0f;
    const float sy = bits_f(0xBF6EEEF0u) / p.aspect;                              // -0.9333 / aspect
    const float zk = f * n * bits_f(0x477FDB00u) / (f - n);                       // depth: zk / z + zb
    const float zb = (n * bits_f(0xC77FDC00u) + f) / (f - n);
    if (vu_rounding) {
        // VU1 setup 0x0000 rounds V * M before multiplying by P. The double-precision
        // (P * V) * M reconstruction can shift vertices across GS 12.4 fixed-point boundaries.
        float vm[3][4];
        for (int r = 0; r < 3; r++) {
            const float *rot = &p.rotation[r * 3];
            const float t = -(rot[0] * p.position[0] + rot[1] * p.position[1] + rot[2] * p.position[2]);
            for (int c = 0; c < 4; c++)
                vm[r][c] = rot[0] * p.model[c] + rot[1] * p.model[4 + c] + rot[2] * p.model[8 + c];
            vm[r][3] += t;
        }
        const float yf = sy * fw;
        for (int c = 0; c < 4; c++) {
            a[0][c] = fw * vm[0][c] + cx * vm[2][c];
            a[1][c] = yf * vm[1][c] + cy * vm[2][c];
            a[2][c] = zb * vm[2][c] + (c == 3 ? zk : 0.0f);
            a[3][c] = vm[2][c];
        }
        return;
    }
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

void ee_environment_world(bool drawing) {
    g_editor_environment = false;
#ifdef EOD_PLUME
    if (drawing && enabled()) {
        if (const auto *settings = environment_override())
            g_editor_environment = matches_loaded_graf(*settings);
    }
#else
    (void)drawing;
#endif
}

void ee_frame_begin() {
    if (!enabled()) return;
    roll(kernel_vblank_count());
}

void ee_object(uint32_t batch) {
    if (!enabled()) return;
    const uint64_t vb = kernel_vblank_count();
    if (g_frame_vb == ~0ull) roll(vb);
    const scene_tap::Object *o = scene_tap::last_object();
    Packet &p = g_current;
    std::memset(&p, 0, sizeof p);
    p.frame = g_frame_vb;  // frames run from one FrameBegin in a new vblank to the next (as EOD_RENDER_TRACE)
    p.serial = g_serial++;
    p.batch = batch;
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
    fill_camera(p);
    CacheEntry *c = nullptr;
    p.mesh = mesh_for(p.batch, vb, false, &c);
    if (c && p.mesh && !c->skin.empty()) p.mesh = posed_mesh(*c, p);
    else if (c && p.mesh && !c->facing.empty()) p.mesh = facing_queue_mesh(*c, p.batch);
    g_records++;
    g_in_object = true;
}

void ee_object_end() { g_in_object = false; }

void ee_packet_reserved(uint32_t addr, uint32_t qwc, uint32_t ra, const SceneClear *clear) {
    if (!addr) return;
    const bool batch = g_in_object && ra == kBatchPacketRa;
    const bool is_imm_ra = (ra == kImmBeginRa || ra == kImmVertexRa || ra == kImmMoreRa);
    const bool water = g_in_water && is_imm_ra;
    const bool flare = !water && g_in_flare && is_imm_ra;
    const bool imm = !water && !flare && g_in_imm && is_imm_ra;
    const bool part = g_in_particle && ra == kPartReserveRa;
    const bool spr = g_in_sprite && (ra == kSpriteReserveRa || ra == kAtlasReserveRa);
    const uint32_t end = addr + (qwc ? qwc : 1) * 16;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto clear_it = g_scene_clear_packets.lower_bound(addr);
    if (clear_it != g_scene_clear_packets.begin()) {
        auto prev = std::prev(clear_it);
        if (prev->first + (kSceneClearDirectQwc + 1) * 16 > addr)
            g_scene_clear_packets.erase(prev);
    }
    while (clear_it != g_scene_clear_packets.end() && clear_it->first < end)
        clear_it = g_scene_clear_packets.erase(clear_it);
    if (clear) g_scene_clear_packets.emplace(addr, *clear);
    auto copy_it = g_display_copy_packets.lower_bound(addr);
    if (copy_it != g_display_copy_packets.begin()) {
        auto prev = std::prev(copy_it);
        if (prev->first + (kDisplayCopyDirectQwc + 1) * 16 > addr)
            g_display_copy_packets.erase(prev);
    }
    while (copy_it != g_display_copy_packets.end() && copy_it->first < end)
        copy_it = g_display_copy_packets.erase(copy_it);
    if (shadow_pass_enabled() && !diff_in_original_side()) {
        auto shadow_it = g_shadow_pass_packets.lower_bound(addr);
        if (shadow_it != g_shadow_pass_packets.begin()) {
            auto prev = std::prev(shadow_it);
            if (prev->first + (shadow_direct_qwc(prev->second) + 1) * 16 > addr)
                g_shadow_pass_packets.erase(prev);
        }
        while (shadow_it != g_shadow_pass_packets.end() && shadow_it->first < end)
            shadow_it = g_shadow_pass_packets.erase(shadow_it);
        const ShadowPass pass = shadow_reservation(ra, qwc);
        if (pass != ShadowPass::None) g_shadow_pass_packets.emplace(addr, pass);
    }
    if (!batch && !water && !flare && !imm && !part && !spr && g_tags.empty()) return;
    // trim or drop older entries this range overlaps (trimming preserves a committed packet whose
    // unused reserved tail was reclaimed by the next PacketReserve at `addr > prev->first`)
    auto it = g_tags.lower_bound(addr);
    if (it != g_tags.begin()) {
        auto prev = std::prev(it);
        if (prev->second.end > addr) {
            if (prev->first < addr) prev->second.end = addr;
            else g_tags.erase(prev);
        }
    }
    it = g_tags.lower_bound(addr);
    while (it != g_tags.end() && it->first < end) it = g_tags.erase(it);
    if (water) {  // every VU1 start under this packet's DMA tags draws the water surface
        g_tags[addr] = Tagged{end, g_water, true};
        return;
    }
    if (flare) {  // every VU1 start under this packet's DMA tags draws the flare bucket
        g_tags[addr] = Tagged{end, g_flare, true};
        return;
    }
    if (imm) {  // general immediate geometry or cockpit quad
        g_tags[addr] = Tagged{end, g_imm, true};
        return;
    }
    if (part) {  // particle stream
        g_tags[addr] = Tagged{end, g_particle, true};
        return;
    }
    if (spr) {  // simple or atlas effect sprite stream
        g_tags[addr] = Tagged{end, g_sprite, true};
        return;
    }
    if (!batch) return;  // any other packet there replaces a record's tag
    g_tags[addr] = Tagged{end, g_current, false};
    g_tagged++;
}

bool take_scene_clear(uint32_t dma_tag, uint32_t qwc, SceneClear &clear) {
    if (qwc != kSceneClearDirectQwc || !scene_clear_enabled()) return false;
    std::lock_guard<std::mutex> lk(g_mutex);
    const auto found = g_scene_clear_packets.find(dma_tag);
    if (found == g_scene_clear_packets.end()) return false;
    clear = found->second;
    g_scene_clear_packets.erase(found);
    return true;
}

void ee_tag_display_copy(uint32_t addr, const DisplayCopy &copy) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_display_copy_packets.insert_or_assign(addr, copy);
}

bool take_display_copy(uint32_t dma_tag, uint32_t qwc, DisplayCopy &copy) {
    if (qwc != kDisplayCopyDirectQwc || !display_copy_enabled()) return false;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_display_copy_packets.find(dma_tag);
    if (it == g_display_copy_packets.end()) return false;
    copy = it->second;
    g_display_copy_packets.erase(it);
    return true;
}

ShadowPass take_shadow_pass(uint32_t dma_tag, uint32_t qwc) {
    if ((qwc != kShadowPrepareDirectQwc && qwc != kShadowCompositeDirectQwc) || !shadow_pass_enabled())
        return ShadowPass::None;
    std::lock_guard<std::mutex> lk(g_mutex);
    const auto found = g_shadow_pass_packets.find(dma_tag);
    if (found == g_shadow_pass_packets.end() || shadow_direct_qwc(found->second) != qwc)
        return ShadowPass::None;
    const ShadowPass pass = found->second;
    g_shadow_pass_packets.erase(found);
    return pass;
}

// ---- track world (0x358BD0, src/native_world.cpp) --------------------------------------------
void ee_world_pass(int32_t pass, uint32_t) {
    if (!enabled()) return;
    roll(kernel_vblank_count());
    g_world_pass = pass;
    g_leaves.clear();
    g_wc.passes++;
    static const bool describe = std::getenv("EOD_NATIVE_WORLD_LIST") != nullptr;
    if (describe && MEM_LW(kMlist)) describe_mlist(MEM_LW(kMlist));
}

void ee_world_pass_end() {
    g_leaves.clear();
    if (g_leaves.capacity() > 4096) g_leaves.shrink_to_fit();  // released when unused
}

namespace {
// The sun quad corners (VU1 qwords 0xAC..0xAF) from the material packet 0x35BA10 sends with the
// world packet (0x34C1F0 -> 0x35CBD8 -> 0x35A810(p, 4, 0x1D, 3, 4): V3-32 x4 to 0x8F + 0x1D).
bool sun_quad(float c[4][3], uint32_t &alpha) {
    const uint32_t p = MEM_LW(kMaterialPacket);
    if (!p || p >= 0x2000000) return false;
    const uint32_t unpack = MEM_LW(p + 4);
    if ((unpack & 0x6F0003FFu) != (0x68000000u | ((0x8Fu + 0x1Du) & 0x3FF)) || ((unpack >> 16) & 0xFF) != 4) return false;
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < 3; i++) c[k][i] = rf(p + 8 + 12u * (uint32_t)k + 4u * (uint32_t)i);
    alpha = MEM_LW(p + 8 + 8) & 0xFF;  // mfir.w of corner a's z: the low byte 0x80 the EE wrote
    return true;
}

// A camera-facing part as this view sees it: a transient mesh (released by the GS after use).
uint32_t facing_mesh(const CacheEntry &c, uint32_t part, uint32_t &alpha) {
    float q[4][3];
    if (!sun_quad(q, alpha)) {
        g_wc.no_quad++;
        return c.id;
    }
    auto *m = new Mesh();
    m->id = g_next_mesh++;
    m->replaces = 0;
    m->batch = part;
    m->skinned = false;
    m->transient = true;
    m->vertices = c.base;
    copy_k(m, c);
    m->indices = c.indices;
    for (const FacingVertex &f : c.facing) {
        Vertex &v = m->vertices[f.vertex];
        for (int k = 0; k < 2; k++)
            if (f.corner[k] < 4)
                for (int i = 0; i < 3; i++) v.p[i] += f.weight[k] * q[f.corner[k]][i];
    }
    std::lock_guard<std::mutex> lk(g_mutex);
    g_new_meshes.push_back(m);
    return m->id;
}
}  // namespace

namespace {
// The material packet in effect (*(0x4D7C54): set by 0x35DDC8 for a queue record, by 0x34C1F0 for
// the world passes) is the last upload the DMA chain sends VU1 before the next batch (0x35BA10 REFs
// it). Its words go into the model of the VU1 bone qwords.
void apply_material() {
    const uint32_t m = MEM_LW(kMaterialPacket);
    if (m == g_applied && g_applied_frame == g_frame_vb) return;
    g_applied = m;
    g_applied_frame = g_frame_vb;
    auto it = g_uploads.find(m);
    if (it == g_uploads.end()) return;
    const Upload &u = it->second;
    for (uint32_t i = 0; i < u.count; i++) {
        const uint32_t q = (u.qword + i) % kVuQwords;
        for (uint32_t c = 0; c < 4; c++) g_vu[q][c] = c < u.comps ? u.data[i * u.comps + c] : 0;
        g_vu_from[q] = m;
    }
}

inline float vf(uint32_t q, int c) {
    float f;
    std::memcpy(&f, &g_vu[q % kVuQwords][c], 4);
    return f;
}

// EOD_NATIVE_SKIN_CHECK=1: the bone qwords each posed record used, compared on the VIF1 thread with
// what VU1 holds when the record's program starts (verification only; the feed never reads VU1).
bool skin_check() {
    static const bool on = [] {
        const char *e = std::getenv("EOD_NATIVE_SKIN_CHECK");
        return e && *e && *e != '0';
    }();
    return on;
}
struct UsedQwords {
    std::vector<std::pair<uint32_t, std::array<uint32_t, 3>>> q;  // qword, xyz the host used
};
std::mutex g_used_mutex;
std::map<uint64_t, UsedQwords> g_used;  // (frame << 32 | serial) -> qwords

// A skinned batch as this record poses it: a transient mesh, or the bind-pose mesh when every dent
// it reads is zero (p + 0 = p).
uint32_t posed_mesh(const CacheEntry &c, const Packet &p) {
    apply_material();
    g_sc.records++;
    if (!g_uploads.count(g_applied)) g_sc.no_upload++;
    bool any = false, matrix = false;
    for (const SkinVertex &k : c.skin) {
        if (k.matrix) {
            any = matrix = true;
            break;
        }
        for (int j = 0; j < 2; j++) {
            const uint32_t q = (k.bone[j] + 143u) % kVuQwords;
            if (!g_vu_from[q]) g_sc.uncovered++;
            if ((g_vu[q][0] | g_vu[q][1] | g_vu[q][2]) & 0x7FFFFFFFu) any = true;
        }
    }
    if (!any) {
        g_sc.undented++;
        return c.id;
    }
    auto *m = new Mesh();
    m->id = g_next_mesh++;
    m->replaces = 0;
    m->batch = p.batch;
    m->skinned = true;
    m->transient = true;
    m->posed = true;
    m->bind = c.id;
    m->vertices = c.base;
    copy_k(m, c);
    m->indices = c.indices;
    UsedQwords used;
    auto note = [&](uint32_t q) {
        if (!skin_check()) return;
        q %= kVuQwords;
        for (auto &u : used.q)
            if (u.first == q) return;
        used.q.push_back({q, {g_vu[q][0], g_vu[q][1], g_vu[q][2]}});
    };
    for (const SkinVertex &k : c.skin) {
        Vertex &v = m->vertices[k.vertex];
        const float x = v.p[0], y = v.p[1], z = v.p[2], w = k.w;
        float out[3];
        if (k.matrix) {
            float P[2][3], N[2][3];
            for (int j = 0; j < 2; j++) {
                const uint32_t b = k.bone[j] + 144u;
                for (uint32_t cc = 0; cc < 4; cc++) {
                    note(b + cc);
                    if (!g_vu_from[(b + cc) % kVuQwords]) g_sc.uncovered++;
                }
                // mulax / madday / maddaz / maddw: c0 x + c1 y + c2 z + c3 w
                for (int i = 0; i < 3; i++) {
                    P[j][i] = vf(b, i) * x + vf(b + 1, i) * y + vf(b + 2, i) * z + vf(b + 3, i) * w;
                    N[j][i] = vf(b, i) * (k.n[0] / 128.0f) + vf(b + 1, i) * (k.n[1] / 128.0f) +
                              vf(b + 2, i) * (k.n[2] / 128.0f);
                }
            }
            // mulaz / maddw: wa Pa + wb Pb
            for (int i = 0; i < 3; i++) out[i] = P[0][i] * k.weight[0] + P[1][i] * k.weight[1];
            if (k.has_n)
                for (int i = 0; i < 3; i++) {
                    const float n = (N[0][i] * k.weight[0] + N[1][i] * k.weight[1]) * 128.0f;
                    v.n[i] = (int8_t)std::lround(std::fmax(-128.0f, std::fmin(127.0f, n)));
                }
        } else {
            const uint32_t a = k.bone[0] + 143u, b = k.bone[1] + 143u;
            note(a);
            note(b);
            // mulaz / maddaw / maddw: wa D[a] + wb D[b] + p * 1
            for (int i = 0; i < 3; i++) out[i] = vf(a, i) * k.weight[0] + vf(b, i) * k.weight[1] + v.p[i];
        }
        // the program projects (x, y, z, w) with the raw w lane: the same point is (x, y, z) / w
        const float iw = (w != 0.0f && w != 1.0f) ? 1.0f / w : 1.0f;
        for (int i = 0; i < 3; i++) v.p[i] = out[i] * iw;
    }
    if (matrix) g_sc.matrix++;
    else g_sc.offset++;
    g_sc.posed++;
    if (skin_check()) {
        std::lock_guard<std::mutex> lk(g_used_mutex);
        g_used[p.frame << 32 | p.serial] = std::move(used);
        while (g_used.size() > 65536) g_used.erase(g_used.begin());
    }
    std::lock_guard<std::mutex> lk(g_mutex);
    g_new_meshes.push_back(m);
    return m->id;
}

// A camera-facing batch (VU1 0x350) drawn through the queue: corners from the bone qwords in effect.
uint32_t facing_queue_mesh(const CacheEntry &c, uint32_t batch) {
    apply_material();
    auto *m = new Mesh();
    m->id = g_next_mesh++;
    m->replaces = 0;
    m->batch = batch;
    m->skinned = false;
    m->transient = true;
    m->vertices = c.base;
    copy_k(m, c);
    m->indices = c.indices;
    for (const FacingVertex &f : c.facing) {
        Vertex &v = m->vertices[f.vertex];
        for (int k = 0; k < 2; k++)
            if (f.corner[k] < 4)
                for (int i = 0; i < 3; i++) v.p[i] += f.weight[k] * vf(0xACu + f.corner[k], i);
    }
    g_sc.facing++;
    std::lock_guard<std::mutex> lk(g_mutex);
    g_new_meshes.push_back(m);
    return m->id;
}
}  // namespace

void ee_vu1_upload(uint32_t packet, uint32_t qword, uint32_t comps, uint32_t count, uint32_t src) {
    if (!enabled() || !packet) return;
    Upload &u = g_uploads[packet];
    u.qword = qword;
    u.comps = comps;
    u.count = count;
    u.frame = kernel_vblank_count();
    u.data.resize((size_t)count * comps);
    for (uint32_t i = 0; i < count * comps; i++) u.data[i] = MEM_LW(src + 4 * i);
    if (packet == g_applied) g_applied = 0;  // rebuilt: apply it again when it is in effect
    g_sc.uploads++;
    if (comps == 4) g_sc.palettes++;
    else if (qword == 0x90) g_sc.dents++;
    else if (qword == 0xAC) g_sc.quads++;
}

void ee_pose_check(uint32_t packet, uint32_t bones, uint32_t count) {
    if (!enabled()) return;
    auto it = g_uploads.find(packet);
    if (it == g_uploads.end()) return;
    const Upload &u = it->second;
    g_st.pose_checks++;
    for (uint32_t b = 0; b < count; b++)
        for (uint32_t r = 0; r < 4; r++)
            for (uint32_t c = 0; c < 4; c++) {
                // the fed qword r of bone b against row r of the 3x4 pose matrix (w: 0, 0, 0, 1)
                const size_t i = ((size_t)b * 4 + r) * 4 + c;
                const uint32_t fed = i < u.data.size() ? u.data[i] : ~0u;
                const uint32_t pose = c < 3 ? MEM_LW(bones + b * 0x30 + r * 12 + c * 4) : (r == 3 ? 0x3F800000u : 0u);
                g_st.pose_words++;
                if (fed != pose) g_st.pose_mismatch++;
            }
}

void ee_world_leaf(uint32_t ref_qword, uint32_t part, uint32_t matrix_packet, uint32_t itop) {
    if (!enabled()) return;
    apply_material();  // WorldPacketBegin (0x35BA10) sent the material in effect before these REFs
    const uint64_t vb = kernel_vblank_count();
    Packet p;
    std::memset(&p, 0, sizeof p);
    read_matrix_packet(matrix_packet, p);
    p.kind = KIND_WORLD;
    p.pass = (uint32_t)g_world_pass;
    p.frame = g_frame_vb;
    p.serial = g_serial++;
    p.batch = part;
    p.itop = itop;  // ITOP the EE wrote in the REF's VIF codes (vertex alpha = its low byte)
    p.texture_key = -1;
    p.layer = -1;
    fill_camera(p);
    // The world packet's 0x35BA10 call has committed these EE render-state
    // inputs through 0x3620A8. Bases here use the renderer's 256-byte units.
    p.world_surface = {
        (ee_r32(0x4D8B90) & 0x1FF) * 32,
        (ee_r32(0x4D8B98) >> 6) & 0x3F,
        ee_r32(0x4D8B94) & 0x3F,
        0,
        (ee_r32(0x4D8BD0) & 0x1FF) * 32,
        (ee_r32(0x4D8BD4) & 0xF) | 0x30,
        {int32_t(ee_r32(0x4D8B9C) & 0x7FF), int32_t(ee_r32(0x4D8BA4) & 0x7FF),
         int32_t(ee_r32(0x4D8BA0) & 0x7FF), int32_t(ee_r32(0x4D8BA8) & 0x7FF)},
        0x6BFF, 0x71FF,
    };
    CacheEntry *c = nullptr;
    p.mesh = mesh_for(part, vb, true, &c);
    if (p.mesh && c && c->k_kick && !c->k_mixed)
        p.world_lod = {uint16_t(p.tex_k + c->k_delta), uint8_t(p.tex_mxl & 7), true};
    if (!p.mesh) g_wc.nomesh++;
    if (c && !c->facing.empty() && p.mesh) {
        uint32_t alpha = 0x80;
        p.mesh = facing_mesh(*c, part, alpha);
        p.itop = (itop & ~0xFFu) | alpha;
        g_wc.facing++;
    }
    g_leaves.emplace_back(ref_qword, p);
}

// The texture record's packet commit: its REFs are what VIF1 will read; tag each with its leaf.
void ee_world_record(bool committed) {
    if (!enabled()) return;
    if (!committed) {
        g_wc.dropped_records++;
        g_wc.dropped_leaves += (uint32_t)g_leaves.size();
        g_leaves.clear();
        return;
    }
    static const char *retire = std::getenv("EOD_NATIVE_WORLD_RETIRE");
    const bool retire_enabled = (!retire || retire[0] != '0') && !diff_in_original_side();
    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto &l : g_leaves) {
        const uint32_t a = l.first;
        auto it = g_tags.lower_bound(a);
        if (it != g_tags.begin()) {
            auto prev = std::prev(it);
            if (prev->second.end > a) g_tags.erase(prev);
        }
        it = g_tags.lower_bound(a);
        while (it != g_tags.end() && it->first < a + 16) it = g_tags.erase(it);
        const Packet &leaf = l.second;
        const bool eligible = retire_enabled && leaf.itop == 0x80 && leaf.mesh != 0;
        g_tags[a] = Tagged{a + 16, leaf, false, eligible};
        g_wc.leaves++;
    }
    g_leaves.clear();
}

// ---- water surface (0x325728): immediate geometry 0x35DF10 / 0x361708 / 0x361BC8 -------------
// begin runs before ImmediateBegin, so the packets it reserves are the water's
void ee_water_begin(uint32_t surface) {
    if (!enabled()) return;
    roll(kernel_vblank_count());
    Packet &p = g_water;
    std::memset(&p, 0, sizeof p);
    read_matrix_packet(MEM_LW(0x4D7C48), p);  // the matrix packet in effect (0x35E018)
    p.kind = KIND_WATER;
    p.frame = g_frame_vb;
    p.serial = g_serial++;
    p.batch = surface;
    p.pass = g_wc.water_draws;  // the water draw's order in this EE frame: in split screen, the view
    p.itop = 0x200;  // 0x361468 writes ITOP 0x200 (0x04000200) before MSCAL 0x4E
    p.texture_key = (int32_t)MEM_LW(surface + 0x3C);
    p.layer = -1;
    fill_camera(p);
    delete g_water_mesh;
    g_water_mesh = new Mesh();
    g_water_mesh->id = g_next_mesh++;
    g_water_mesh->replaces = 0;
    g_water_mesh->batch = surface;
    g_water_mesh->skinned = false;
    g_water_mesh->transient = true;
    p.mesh = g_water_mesh->id;
    g_in_water = true;
}

// One vertex as 0x361708 builds it: colour halved to the GS scale with rounding, the packed word's
// bytes 2, 1, 0 are the R, G, B lanes; ADC (no triangle) for strip 0, so each tile is 2 triangles.
bool ee_in_water() { return g_in_water && g_water_mesh; }

void ee_water_vertex(const float xyz[3], const float st[2], uint32_t colour, uint32_t strip) {
    if (!g_in_water || !g_water_mesh) return;
    uint32_t c = (colour >> 1) & 0x7F7F7F7Fu;
    c += (c & 0x40404040u) >> 6;
    Vertex v{};
    for (int i = 0; i < 3; i++) v.p[i] = xyz[i];
    v.uv[0] = st[0];
    v.uv[1] = st[1];
    v.c[0] = (uint8_t)(c >> 16);
    v.c[1] = (uint8_t)(c >> 8);
    v.c[2] = (uint8_t)c;
    v.c[3] = (uint8_t)(c >> 24);
    Mesh &m = *g_water_mesh;
    const uint32_t i = (uint32_t)m.vertices.size();
    m.vertices.push_back(v);
    g_wc.water_vertices++;
    if (strip && i >= 2) {
        m.indices.push_back(i - 2);
        m.indices.push_back(i - 1);
        m.indices.push_back(i);
        g_wc.water_tris++;
    }
}

void ee_water_end(bool drawn) {
    if (!g_in_water) return;
    g_in_water = false;
    std::lock_guard<std::mutex> lk(g_mutex);
    // the tags this draw's packets got stay (they only match VU1 starts inside those packets)
    if (!drawn) {
        for (auto it = g_tags.begin(); it != g_tags.end();)
            it = it->second.range && it->second.packet.mesh == g_water.mesh ? g_tags.erase(it) : std::next(it);
        delete g_water_mesh;
        g_water_mesh = nullptr;
        return;
    }
    g_wc.water_draws++;
    {  // per viewport (split screen: one per view)
        static std::map<uint32_t, std::array<uint64_t, 3>> views;
        auto &n = views[g_water.pass];
        n[0]++;
        n[1] += g_water_mesh->vertices.size();
        n[2] += g_water_mesh->indices.size() / 3;
        if ((n[0] & 127) == 0)
            for (auto &v : views)
                std::fprintf(stderr, "[native] water submitted, view %u of the frame: draws %llu vertices %llu triangles %llu\n",
                             v.first + 1, (unsigned long long)v.second[0], (unsigned long long)v.second[1],
                             (unsigned long long)v.second[2]);
    }
    // the guest asks ImmediateBegin for 0x3000 vertices; the native mesh grows with the surface
    limit_note("water surface vertices (0x35DF10 request 0x3000)", g_water_mesh->vertices.size(), 0, 0x3000);
    g_new_meshes.push_back(g_water_mesh);
    g_water_mesh = nullptr;
}

// ---- projected shadows (0x34C7D0 / 0x35D170 / 0x34C798, src/native_shadows_flares.cpp) ---------
// Snapshots the main-viewport camera/projection before 0x363218 overwrites 0x4D7C20 with the PS2
// 320x512 shadow RT viewport. Each shadow batch is extruded on the EE using the exact VU1 0x0020
// formula (0x3BE4C0) and rendered by Plume into an adaptive full-RenderScale host shadow buffer.
void ee_fog_range(float start, float end, float density) {
    g_fog.range[0] = start;
    g_fog.range[1] = end;
    g_fog.range[2] = density;
}
void ee_fog_colour(uint32_t rgb) { g_fog.fogcol = rgb; }
void ee_render_flags(uint32_t flags) { g_fog.flags = flags; }
void ee_tex_lod(int32_t k_template, uint32_t mxl, uint32_t bias_k) {
    g_tex.k = k_template;
    g_tex.mxl = mxl;
    g_tex.bias_k = bias_k;
}
void ee_blend_select(uint32_t sel) { g_tex.blend_sel = sel; }

void ee_shadow_begin(int32_t alpha, bool retail_single_view, int retail_split_y) {
    if (!enabled()) return;
    roll(kernel_vblank_count());
    std::memset(&g_shadow_cam, 0, sizeof g_shadow_cam);
    fill_camera(g_shadow_cam);
    g_shadow_cam.retail_shadow_view = retail_single_view;
    g_retail_split_y = retail_split_y;
    g_hybrid_shadow_cascades = sun_cascades_enabled() && look_modern() && retail_single_view &&
                              !g_shadow_cam.editor_environment && !g_shadow_cam.fge &&
                              sun_cascades::daylight(g_shadow_cam.fogcol);
    // 0x363480: intensity = (alpha >> 1) + (alpha >> 7) in GS 0..128 alpha units
    const int32_t half = (alpha - (alpha >> 31)) >> 1;
    const int32_t adj = (alpha < 0 ? alpha + 0x7F : alpha) >> 7;
    g_shadow_intensity = half + adj;
    // record the PS2 320x512 cap vs the un-clamped main viewport size
    const uint32_t vw = (uint32_t)std::max(0.0f, g_shadow_cam.viewport[2]);
    const uint32_t vh = (uint32_t)std::max(0.0f, g_shadow_cam.viewport[3]);
    limit_note("shadow RT width (0x35DA30 320 cap)", vw, 0, 320);
    limit_note("shadow RT height (0x35DA30 512 cap)", vh, 0, 512);
    g_in_shadow = true;
}

void ee_shadow_opened(bool opened) {
    if (!opened) g_in_shadow = false;
}

namespace {
uint32_t shadow_extruded_mesh(const CacheEntry &c, const Packet &p) {
    if (c.base.empty() || c.indices.empty()) return 0;
    // VU1 0x3BE4C0 0x16f8..0x17b8: vf1 = -M^T * (light.xyz * light.w)
    double vf1[3];
    for (int col = 0; col < 3; col++) {
        double s = 0.0;
        for (int row = 0; row < 3; row++) s += (double)p.model[row * 4 + col] * (double)p.light[row];
        vf1[col] = -s * (double)p.light[3];
    }
    const double extrude = (double)p.colour_b[3];  // 0x35CD28 q[5].w = *(float*)0x4D7D18
    auto *m = new Mesh();
    m->id = g_next_mesh++;
    m->replaces = 0;
    m->batch = p.batch;
    m->skinned = false;
    m->transient = true;
    m->vertices = c.base;
    copy_k(m, c);
    for (Vertex &v : m->vertices) {
        // VU1 0x3BE4C0 0x0130..0x0170: d = dot(n, vf1), e = min(max(0, d) * 65535, extrude)
        const double d = (double)v.n[0] * vf1[0] + (double)v.n[1] * vf1[1] + (double)v.n[2] * vf1[2];
        if (d > 0.0) {
            const double e = std::fmin(d * 65535.0, extrude);
            v.p[0] += (float)(vf1[0] * e);
            v.p[1] += (float)(vf1[1] * e);
            v.p[2] += (float)(vf1[2] * e);
        }
        v.n[3] = 0;  // prelit
        v.c[0] = v.c[1] = v.c[2] = v.c[3] = 15;  // 0x363708 RGB 15 (the shader writes one 5-bit step)
    }
    m->indices = c.indices;
    std::lock_guard<std::mutex> lk(g_mutex);
    g_new_meshes.push_back(m);
    return m->id;
}
}  // namespace

void ee_shadow_batch(uint32_t batch, uint32_t pass, uint32_t clip_flag, uint32_t mat_pkt, uint32_t ref_qword) {
    if (!enabled() || !g_in_shadow || !ref_qword) return;
    roll(kernel_vblank_count());
    Packet p = g_shadow_cam;  // main-viewport camera/projection (not the 320x512 clamped RT!)
    const uint32_t mp = (mat_pkt == 0xFFFFFFFFu) ? MEM_LW(0x4D7C48) : mat_pkt;
    read_matrix_packet(mp, p);
    p.kind = KIND_SHADOW;
    p.frame = g_frame_vb;
    p.serial = g_serial++;
    p.batch = batch;
    p.pass = (uint32_t)g_shadow_intensity;
    // 0x363218: target size and the saved main view (0x35E300), read by 0x362968 and 0x362E70
    p.shadow_rt[0] = (int32_t)MEM_LW(0x4D8BD8);
    p.shadow_rt[1] = (int32_t)MEM_LW(0x4D8BDC);
    for (int i = 0; i < 4; i++) p.shadow_rt[2 + i] = (int32_t)MEM_LW(0x4D8C00 + 4 * i);
    p.itop = (pass & 1u) | (clip_flag ? 0x200u : 0u);
    p.texture_key = -1;
    p.layer = -1;
    CacheEntry *c = nullptr;
    const uint32_t base_id = mesh_for(batch, g_frame_vb, false, &c);
    p.mesh = (c && base_id) ? shadow_extruded_mesh(*c, p) : 0;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_tags.lower_bound(ref_qword);
    if (it != g_tags.begin()) {
        auto prev = std::prev(it);
        if (prev->second.end > ref_qword) g_tags.erase(prev);
    }
    it = g_tags.lower_bound(ref_qword);
    while (it != g_tags.end() && it->first < ref_qword + 16) it = g_tags.erase(it);
    g_tags[ref_qword] = Tagged{ref_qword + 16, p, false};
    g_wc.shadow_batches++;
}

void ee_shadow_end() {
    g_in_shadow = false;
}

// ---- sun glare & light flares (0x304D50, src/native_shadows_flares.cpp) ------------------------
void ee_flare_begin(uint32_t tex_slot) {
    if (!enabled()) return;
    roll(kernel_vblank_count());
    Packet &p = g_flare;
    std::memset(&p, 0, sizeof p);
    read_matrix_packet(MEM_LW(0x4D7C48), p);
    p.kind = KIND_FLARE;
    p.frame = g_frame_vb;
    p.serial = g_serial++;
    p.batch = tex_slot;
    p.pass = g_wc.flare_draws;
    p.itop = 0x280;  // 0x361468 clip flag 0x200 + va=0x80 (1.0); per-flare alpha folded into additive RGB
    p.texture_key = (int32_t)tex_slot;
    p.layer = -1;
    fill_camera(p);  // 0x34D9A8 already installed the 2D screen camera
    delete g_flare_mesh;
    g_flare_mesh = new Mesh();
    g_flare_mesh->id = g_next_mesh++;
    g_flare_mesh->replaces = 0;
    g_flare_mesh->batch = tex_slot;
    g_flare_mesh->skinned = false;
    g_flare_mesh->transient = true;
    p.mesh = g_flare_mesh->id;
    g_in_flare = true;
}

bool ee_in_flare() { return g_in_flare && g_flare_mesh; }

void ee_flare_vertex(const float xyz[3], const float st[2], uint32_t colour, uint32_t strip) {
    if (!g_in_flare || !g_flare_mesh) return;
    uint32_t c = (colour >> 1) & 0x7F7F7F7Fu;
    c += (c & 0x40404040u) >> 6;
    Vertex v{};
    for (int i = 0; i < 3; i++) v.p[i] = xyz[i];
    v.uv[0] = st[0];
    v.uv[1] = st[1];
    const uint32_t a = (c >> 24) & 0xFFu;
    v.c[0] = (uint8_t)std::min(128u, (((c >> 16) & 0xFFu) * a + 64u) >> 7);
    v.c[1] = (uint8_t)std::min(128u, (((c >> 8) & 0xFFu) * a + 64u) >> 7);
    v.c[2] = (uint8_t)std::min(128u, ((c & 0xFFu) * a + 64u) >> 7);
    v.c[3] = (uint8_t)a;
    Mesh &m = *g_flare_mesh;
    const uint32_t i = (uint32_t)m.vertices.size();
    m.vertices.push_back(v);
    g_wc.flare_vertices++;
    if (strip && i >= 2) {
        m.indices.push_back(i - 2);
        m.indices.push_back(i - 1);
        m.indices.push_back(i);
        g_wc.flare_tris++;
    }
}

void ee_flare_end(bool drawn) {
    if (!g_in_flare) return;
    g_in_flare = false;
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!drawn) {
        for (auto it = g_tags.begin(); it != g_tags.end();)
            it = it->second.range && it->second.packet.mesh == g_flare.mesh ? g_tags.erase(it) : std::next(it);
        delete g_flare_mesh;
        g_flare_mesh = nullptr;
        return;
    }
    g_wc.flare_draws++;
    g_new_meshes.push_back(g_flare_mesh);
    g_flare_mesh = nullptr;
}

// ---- particles (0x363B98 / 0x3639D0 / 0x363B68, VU1 0x08E0, src/native_effects_imm.cpp) --------
void ee_particle_begin(float scale) {
    if (!enabled()) return;
    roll(kernel_vblank_count());
    Packet &p = g_particle;
    std::memset(&p, 0, sizeof p);
    p.model[0] = p.model[5] = p.model[10] = 1.0f;  // world space (VU1 0x08E0 uses view-proj vf2..vf5)
    p.kind = KIND_PARTICLE;
    p.frame = g_frame_vb;
    p.serial = g_serial++;
    p.itop = 0x280;
    p.texture_key = -1;
    p.layer = -1;
    fill_camera(p);
    delete g_particle_mesh;
    g_particle_mesh = new Mesh();
    g_particle_mesh->id = g_next_mesh++;
    g_particle_mesh->replaces = 0;
    g_particle_mesh->skinned = false;
    g_particle_mesh->transient = true;
    p.mesh = g_particle_mesh->id;
    g_particle_scale = scale;
    g_particle_count = 0;
    g_particle_alpha_set = false;
    g_in_particle = true;
}

void ee_particle_add(int32_t col_rgb, int32_t col_alpha, float size, float x, float y, float z, float angle) {
    if (!g_in_particle || !g_particle_mesh) return;
    const float *rot = g_particle.rotation;
    const float rx = rot[0], ry = rot[1], rz = rot[2];  // camera right
    const float ux = rot[3], uy = rot[4], uz = rot[5];  // camera up
    const float c = std::cos(angle), s = std::sin(angle);
    // rotated camera right & up axes in world space
    const float rpx = c * rx - s * ux, rpy = c * ry - s * uy, rpz = c * rz - s * uz;
    const float upx = s * rx + c * ux, upy = s * ry + c * uy, upz = s * rz + c * uz;
    const float rgb_base = (float)((uint32_t)(col_rgb >> 1) & 0xFFu);
    const uint8_t alpha = (uint8_t)std::min(128u, (uint32_t)(col_alpha >> 1) & 0xFFu);
    if (!g_particle_alpha_set) {
        g_particle.itop = 0x200u | alpha;
        g_particle_alpha_set = true;
    }
    const float half_scale = 0.5f * g_particle_scale;
    // 4 corners: (-R'-U'), (+R'-U'), (-R'+U'), (+R'+U')
    static constexpr float kSx[4] = {-1.0f, 1.0f, -1.0f, 1.0f};
    static constexpr float kSy[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
    static constexpr float kU[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    static constexpr float kV[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    Mesh &m = *g_particle_mesh;
    if (m.billboards.size() < scene::kMaxEffectBillboardsPerStream &&
        g_wc.particle_quads + g_wc.sprite_quads < scene::kMaxEffectBillboardsPerVblank) {
        const auto rgb = (uint8_t)rgb_base;
        m.billboards.push_back({{x, y, z}, size, {0.0f, 1.0f, 1.0f, 0.0f},
                                angle, half_scale, {rgb, rgb, rgb, alpha}});
    }
    const uint32_t base = (uint32_t)m.vertices.size();
    for (int k = 0; k < 4; k++) {
        const float dx = kSx[k] * rpx + kSy[k] * upx;
        const float dy = kSx[k] * rpy + kSy[k] * upy;
        const float dz = kSx[k] * rpz + kSy[k] * upz;
        Vertex v{};
        v.p[0] = x + size * dx;
        v.p[1] = y + size * dy;
        v.p[2] = z + size * dz;
        v.uv[0] = kU[k];
        v.uv[1] = kV[k];
        float shade = 1.0f - half_scale * (1.0f - std::max(-1.0f, std::min(1.0f, dy)));
        if (shade < 0.0f) shade = 0.0f;
        if (shade > 1.0f) shade = 1.0f;
        const uint8_t rgb = (uint8_t)std::min(128.0f, rgb_base * shade);
        v.c[0] = v.c[1] = v.c[2] = rgb;
        v.c[3] = alpha;
        m.vertices.push_back(v);
    }
    m.indices.push_back(base + 0);
    m.indices.push_back(base + 1);
    m.indices.push_back(base + 2);
    m.indices.push_back(base + 1);
    m.indices.push_back(base + 3);
    m.indices.push_back(base + 2);
    g_particle_count++;
    g_wc.particle_quads++;
}

void ee_particle_end(bool drawn) {
    if (!g_in_particle) return;
    g_in_particle = false;
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!drawn || !g_particle_mesh || g_particle_mesh->indices.empty()) {
        for (auto it = g_tags.begin(); it != g_tags.end();)
            it = it->second.range && it->second.packet.mesh == g_particle.mesh ? g_tags.erase(it) : std::next(it);
        delete g_particle_mesh;
        g_particle_mesh = nullptr;
        return;
    }
    for (auto &kv : g_tags)
        if (kv.second.range && kv.second.packet.mesh == g_particle.mesh)
            kv.second.packet.itop = g_particle.itop;
    limit_note("particle stream count (0x363FF8 16/pkt cap)", g_particle_count, 0, 16);
    g_wc.particle_draws++;
    g_new_meshes.push_back(g_particle_mesh);
    g_particle_mesh = nullptr;
}

// ---- effect sprites (0x363F48/0x363DE8 VU1 0x01B8 & 0x363D78/0x363C18 VU1 0x05E0) --------------
void ee_sprite_begin(int32_t grid_size) {
    if (!enabled()) return;
    roll(kernel_vblank_count());
    Packet &p = g_sprite;
    std::memset(&p, 0, sizeof p);
    p.model[0] = p.model[5] = p.model[10] = 1.0f;  // world space (VU1 0x01B8/0x05E0 uses view-proj vf2..vf5)
    p.kind = KIND_SPRITE;
    p.pass = grid_size == 64 ? 8u : grid_size == 16 ? 4u : grid_size == 4 ? 2u : 1u;  // atlas grid dim (Modern VFX)
    p.frame = g_frame_vb;
    p.serial = g_serial++;
    p.itop = 0x280;
    p.texture_key = -1;
    p.layer = -1;
    fill_camera(p);
    delete g_sprite_mesh;
    g_sprite_mesh = new Mesh();
    g_sprite_mesh->id = g_next_mesh++;
    g_sprite_mesh->replaces = 0;
    g_sprite_mesh->skinned = false;
    g_sprite_mesh->transient = true;
    p.mesh = g_sprite_mesh->id;
    g_sprite_count = 0;
    g_sprite_alpha_set = false;
    g_in_sprite = true;
}

namespace {
void emit_sprite_quad(const uint32_t col[4], float x, float y, float z, float size,
                      float u0, float v0, float u1, float v1) {
    const float *rot = g_sprite.rotation;
    const float rx = rot[0], ry = rot[1], rz = rot[2];
    const float ux = rot[3], uy = rot[4], uz = rot[5];
    const uint8_t r = (uint8_t)std::min(128u, col[0] & 0xFFu);
    const uint8_t g = (uint8_t)std::min(128u, col[1] & 0xFFu);
    const uint8_t b = (uint8_t)std::min(128u, col[2] & 0xFFu);
    const uint8_t a = (uint8_t)std::min(128u, col[3] & 0xFFu);
    if (!g_sprite_alpha_set) {
        g_sprite.itop = 0x200u | a;
        g_sprite_alpha_set = true;
    }
    // 4 corners: top-left (-R+U), top-right (+R+U), bottom-left (-R-U), bottom-right (+R-U)
    const float sx[4] = {-1.0f, 1.0f, -1.0f, 1.0f};
    const float sy[4] = {1.0f, 1.0f, -1.0f, -1.0f};
    const float us[4] = {u0, u1, u0, u1};
    const float vs[4] = {v0, v0, v1, v1};
    Mesh &m = *g_sprite_mesh;
    if (m.billboards.size() < scene::kMaxEffectBillboardsPerStream &&
        g_wc.particle_quads + g_wc.sprite_quads < scene::kMaxEffectBillboardsPerVblank)
        m.billboards.push_back({{x, y, z}, size, {u0, v0, u1, v1}, 0.0f, 0.0f, {r, g, b, a}});
    const uint32_t base = (uint32_t)m.vertices.size();
    for (int k = 0; k < 4; k++) {
        Vertex v{};
        v.p[0] = x + size * (sx[k] * rx + sy[k] * ux);
        v.p[1] = y + size * (sx[k] * ry + sy[k] * uy);
        v.p[2] = z + size * (sx[k] * rz + sy[k] * uz);
        v.uv[0] = us[k];
        v.uv[1] = vs[k];
        v.c[0] = r;
        v.c[1] = g;
        v.c[2] = b;
        v.c[3] = a;
        m.vertices.push_back(v);
    }
    m.indices.push_back(base + 0);
    m.indices.push_back(base + 1);
    m.indices.push_back(base + 2);
    m.indices.push_back(base + 1);
    m.indices.push_back(base + 3);
    m.indices.push_back(base + 2);
    g_sprite_count++;
    g_wc.sprite_quads++;
}
}  // namespace

void ee_sprite_add(const uint32_t col[4], int32_t flip, float size, float x, float y, float z) {
    if (!g_in_sprite || !g_sprite_mesh) return;
    // flip 0: (0,0)-(1,1), 1: (1,0)-(0,1), 2: (1,1)-(0,0), 3: (0,1)-(1,0)
    const bool flip_u = (flip == 1 || flip == 2);
    const bool flip_v = (flip == 2 || flip == 3);
    emit_sprite_quad(col, x, y, z, size, flip_u ? 1.0f : 0.0f, flip_v ? 1.0f : 0.0f,
                     flip_u ? 0.0f : 1.0f, flip_v ? 0.0f : 1.0f);
}

void ee_atlas_sprite_add(const uint32_t col[4], int32_t tile, int32_t grid_shift, int32_t grid_mask,
                         float size, float x, float y, float z) {
    if (!g_in_sprite || !g_sprite_mesh) return;
    const float inv_dim = 1.0f / (float)(1 << (grid_shift & 31));
    const float u0 = (float)(tile & grid_mask) * inv_dim;
    const float v0 = (float)(tile >> (grid_shift & 31)) * inv_dim;
    emit_sprite_quad(col, x, y, z, size, u0, v0, u0 + inv_dim, v0 + inv_dim);
}

void ee_sprite_end(bool drawn) {
    if (!g_in_sprite) return;
    g_in_sprite = false;
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!drawn || !g_sprite_mesh || g_sprite_mesh->indices.empty()) {
        for (auto it = g_tags.begin(); it != g_tags.end();)
            it = it->second.range && it->second.packet.mesh == g_sprite.mesh ? g_tags.erase(it) : std::next(it);
        delete g_sprite_mesh;
        g_sprite_mesh = nullptr;
        return;
    }
    for (auto &kv : g_tags)
        if (kv.second.range && kv.second.packet.mesh == g_sprite.mesh)
            kv.second.packet.itop = g_sprite.itop;
    limit_note("sprite stream count (0x364548/0x3642B8 33/pkt cap)", g_sprite_count, 0, 33);
    g_wc.sprite_draws++;
    g_new_meshes.push_back(g_sprite_mesh);
    g_sprite_mesh = nullptr;
}

// ---- general immediate geometry & cockpit quad (0x35DF10 / 0x361708 / 0x361BC8 / 0x35CAA0) -----
void ee_imm_begin(bool cockpit, bool hud) {
    if (!enabled() || g_in_water || g_in_flare) return;
    roll(kernel_vblank_count());
    Packet &p = g_imm;
    std::memset(&p, 0, sizeof p);
    read_matrix_packet(MEM_LW(0x4D7C48), p);
    p.kind = cockpit ? KIND_COCKPIT : hud ? KIND_HUD : KIND_IMMEDIATE;
    p.frame = g_frame_vb;
    p.serial = g_serial++;
    p.itop = 0x280;
    p.texture_key = -1;
    p.layer = -1;
    fill_camera(p);
    delete g_imm_mesh;
    g_imm_mesh = new Mesh();
    g_imm_mesh->id = g_next_mesh++;
    g_imm_mesh->replaces = 0;
    g_imm_mesh->skinned = false;
    g_imm_mesh->transient = true;
    p.mesh = g_imm_mesh->id;
    g_imm_cockpit = cockpit;
    g_imm_hud = hud && !cockpit;
    g_imm_alpha_set = false;
    g_in_imm = true;
}

bool ee_in_imm() { return g_in_imm && g_imm_mesh; }

void ee_imm_vertex(const float xyz[3], const float st[2], uint32_t colour, uint32_t strip, float spark_age) {
    if (!g_in_imm || !g_imm_mesh) return;
    uint32_t c = (colour >> 1) & 0x7F7F7F7Fu;
    c += (c & 0x40404040u) >> 6;
    const uint8_t a = (uint8_t)((c >> 24) & 0xFFu);
    if (!g_imm_alpha_set) {
        g_imm.itop = 0x200u | a;
        g_imm_alpha_set = true;
    }
    Vertex v{};
    for (int i = 0; i < 3; i++) v.p[i] = xyz[i];
    v.uv[0] = st[0];
    v.uv[1] = st[1];
    v.c[0] = (uint8_t)(c >> 16);
    v.c[1] = (uint8_t)(c >> 8);
    v.c[2] = (uint8_t)c;
    v.c[3] = a;
    Mesh &m = *g_imm_mesh;
    if (std::isfinite(spark_age) && spark_age >= 0.0f) {
        // A prelit immediate vertex has no normal. Reserve zero for unknown age.
        v.n[0] = (int8_t)(1 + std::lround(std::min(1.0f, spark_age) * 126.0f));
        m.has_spark_age = true;
    }
    const uint32_t idx = (uint32_t)m.vertices.size();
    m.vertices.push_back(v);
    g_wc.imm_verts++;
    if (strip && idx >= 2) {
        m.indices.push_back(idx - 2);
        m.indices.push_back(idx - 1);
        m.indices.push_back(idx);
        g_wc.imm_tris++;
    }
}

void ee_imm_end(bool drawn) {
    if (!g_in_imm) return;
    g_in_imm = false;
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!drawn || !g_imm_mesh || g_imm_mesh->indices.empty()) {
        for (auto it = g_tags.begin(); it != g_tags.end();)
            it = it->second.range && it->second.packet.mesh == g_imm.mesh ? g_tags.erase(it) : std::next(it);
        delete g_imm_mesh;
        g_imm_mesh = nullptr;
        return;
    }
    for (auto &kv : g_tags)
        if (kv.second.range && kv.second.packet.mesh == g_imm.mesh)
            kv.second.packet.itop = g_imm.itop;
    limit_note("immediate geometry vertices (0x361468 60/chunk cap)", g_imm_mesh->vertices.size(), 0, 60);
    if (g_imm_cockpit) g_wc.cockpit_draws++;
    else if (g_imm_hud) g_wc.hud_draws++;
    else g_wc.imm_draws++;
    g_new_meshes.push_back(g_imm_mesh);
    g_imm_mesh = nullptr;
}

// ---- sphere-tested pre-built immediate geometry (0x361C30 <- 0x361ED8 <- 0x34EE70/0x34EEB8) ----
void ee_sphere_imm(uint32_t buf_obj, uint32_t ref_qword) {
    if (!enabled() || !buf_obj || !ref_qword) return;
    roll(kernel_vblank_count());
    const uint32_t buf = MEM_LW(buf_obj);
    const int32_t qwc = (int16_t)MEM_LH(buf_obj + 16);
    if (!buf || qwc <= 0) return;
    const uint32_t end_addr = buf + (uint32_t)qwc * 16u;
    auto *m = new Mesh();
    m->id = g_next_mesh++;
    m->replaces = 0;
    m->batch = buf;
    m->skinned = false;
    m->transient = true;
    uint8_t first_alpha = 0x80;
    bool have_alpha = false;
    // Decode 0x361C98 VIF chunks (VU1 0x0270 format)
    uint32_t p = buf;
    while (p + 32u <= end_addr) {
        // Expect 0x01000202, 0x05000000, 0x6102C0C6, tex, 0x6001C000, nverts|0x8000, 0x01000103, UNPACK V4-8
        if (MEM_LW(p + 16) != 0x6001C000u) { p += 4; continue; }
        const uint32_t nverts = MEM_LW(p + 20) & 0x7FFFu;
        const uint32_t col_base = p + 32u;
        uint32_t cur = col_base + nverts * 4u;
        if (cur + 12u > end_addr) break;
        if (MEM_LW(cur) == 0x20000000u) cur += 28u;  // optional STMASK + STROW (7 words)
        if (cur + 12u > end_addr) break;
        const uint32_t vert_base = cur + 12u;
        if (vert_base + nverts * 24u > end_addr) break;
        const uint32_t base_v = (uint32_t)m->vertices.size();
        for (uint32_t i = 0; i < nverts; i++) {
            const uint32_t cw = MEM_LW(col_base + i * 4u);
            const uint32_t va = vert_base + i * 24u;
            Vertex v{};
            v.p[0] = rf(va + 0);
            v.p[1] = rf(va + 4);
            v.p[2] = rf(va + 8);
            v.uv[0] = rf(va + 12);
            v.uv[1] = rf(va + 16);
            const uint8_t a = (uint8_t)(MEM_LW(va + 20) & 0xFFu);
            if (!have_alpha) { first_alpha = a; have_alpha = true; }
            v.c[0] = (uint8_t)(cw & 0xFFu);
            v.c[1] = (uint8_t)((cw >> 8) & 0xFFu);
            v.c[2] = (uint8_t)((cw >> 16) & 0xFFu);
            v.c[3] = a;
            m->vertices.push_back(v);
            const bool adc = (cw & 0x80000000u) != 0;
            if (!adc && i >= 2) {
                m->indices.push_back(base_v + i - 2);
                m->indices.push_back(base_v + i - 1);
                m->indices.push_back(base_v + i);
            }
        }
        p = vert_base + nverts * 24u + 4u;  // past MSCAL 0x4E
        p = (p + 15u) & ~15u;               // 16-byte chunk alignment
    }
    if (m->indices.empty()) {
        delete m;
        return;
    }
    Packet pkt{};
    read_matrix_packet(MEM_LW(0x4D7C48), pkt);
    pkt.kind = KIND_IMMEDIATE;
    pkt.frame = g_frame_vb;
    pkt.serial = g_serial++;
    pkt.batch = buf;
    pkt.itop = 0x200u | first_alpha;
    pkt.texture_key = -1;
    pkt.layer = -1;
    fill_camera(pkt);
    pkt.mesh = m->id;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_tags.lower_bound(ref_qword);
    if (it != g_tags.begin()) {
        auto prev = std::prev(it);
        if (prev->second.end > ref_qword) g_tags.erase(prev);
    }
    it = g_tags.lower_bound(ref_qword);
    while (it != g_tags.end() && it->first < ref_qword + 16) it = g_tags.erase(it);
    g_tags[ref_qword] = Tagged{ref_qword + 16, pkt, true};
    g_new_meshes.push_back(m);
    g_wc.sphere_imm_draws++;
}

void take_expected(uint64_t before, std::map<uint64_t, Expected> &out) {
    std::lock_guard<std::mutex> lk(g_expect_mutex);
    while (!g_expect.empty() && g_expect.begin()->first < before) {
        out[g_expect.begin()->first] = g_expect.begin()->second;
        g_expect.erase(g_expect.begin());
    }
}

int native_only_level() {
    static const int level = [] {
        const char *e = std::getenv("EOD_NATIVE_ONLY");
        return e && *e && *e != '0' ? std::atoi(e) : 0;
    }();
    return level;
}

bool native_only() {
    static const bool on = [] {
        const char *e = std::getenv("EOD_NATIVE_ONLY");
        const bool v = e && *e && *e != '0';
        if (v) std::fprintf(stderr, "[native] EOD_NATIVE_ONLY: the GS emulator draws nothing\n");
        return v;
    }();
    return on;
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

// P2 (EOD_NATIVE_SKIN_CHECK=1): the bone qwords a posed record used against VU1 data memory as the
// record's first program starts (the material packet REF precedes the batch in the DMA chain).
void vu_check(uint64_t key, uint32_t program) {
    UsedQwords u;
    {
        std::lock_guard<std::mutex> lk(g_used_mutex);
        auto it = g_used.find(key);
        if (it == g_used.end()) return;
        u = std::move(it->second);
        g_used.erase(it);
    }
    static uint64_t records = 0, words = 0, bad = 0, logged = 0, last = 0;
    records++;
    for (auto &q : u.q) {
        const uint8_t *m = g_vu1.mem + (size_t)q.first * 16;
        for (int c = 0; c < 3; c++) {
            uint32_t w;
            std::memcpy(&w, m + 4 * c, 4);
            words++;
            if (w != q.second[c]) {
                bad++;
                if (logged++ < 16)
                    std::fprintf(stderr, "[native] P2 vu mismatch frame %llu serial %u prog %04x qword %03x lane %d host %08x vu1 %08x\n",
                                 (unsigned long long)(key >> 32), (uint32_t)key, program, q.first, c, q.second[c], w);
            }
        }
    }
    if (records == 1 || records / 4096 != last) {
        last = records / 4096;
        std::fprintf(stderr, "[native] P2 bone qwords vs VU1: records %llu words %llu mismatched %llu\n",
                     (unsigned long long)records, (unsigned long long)words, (unsigned long long)bad);
    }
}

bool vu1_start(uint32_t program, uint32_t itop, WorldTextureLod &world_lod) {
    if (!enabled()) return false;
    const uint32_t tag = g_vif1_dma_tag;
    std::vector<Mesh *> meshes;
    Packet p;
    bool found = false;
    bool retire_world_leaf = false;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_tags.upper_bound(tag);
        if (it != g_tags.begin()) {
            --it;
            // the tag VIF1 reads must still be the record's DMA REF to its batch data
            if (tag < it->second.end &&
                (it->second.range || (MEM_LW(tag + 4) & 0x7FFFFFFF) == it->second.packet.batch + 0x38)) {
                p = it->second.packet;
                if (tag == it->first && it->second.retire_world_leaf) {
                    // Consume the commit at its first MSCAL. A continuation or stale
                    // reference may still use the marker but cannot retire another leaf.
                    it->second.retire_world_leaf = false;
                    retire_world_leaf = program == 0x0270 && itop == 0x80;
                }
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
        return false;
    }
    for (Mesh *m : meshes) post(CMD_MESH, &m, sizeof m);
    const uint64_t key = p.frame << 32 | p.serial;
    if (g_obj_active && key == g_sent_key) return false;  // another program of the same record
    if (skin_check() && p.kind == KIND_QUEUE) vu_check(key, program);
    if (p.kind == KIND_QUEUE) p.itop = itop;  // world, water, shadow, flare carry the ITOP their EE packet sets
    p.program = program;
    if (p.kind != KIND_SHADOW && p.kind != KIND_PARTICLE && p.kind != KIND_SPRITE) check(p);  // shadow uses main-viewport cam; particles/sprites use vf2..vf5
    post(CMD_OBJ, &p, sizeof p);
    g_obj_active = true;
    g_sent_key = key;
    if (retire_world_leaf) world_lod = p.world_lod;
    return retire_world_leaf;
}

}  // namespace native
