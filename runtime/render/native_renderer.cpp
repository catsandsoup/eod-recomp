// native_renderer.cpp - GS-thread side of the native renderer (native_renderer.h): which GS draws
// belong to native objects, the objects' GS state, and the plume renderer that draws them.
#include "core/diff_harness.h"
#include "core/trace.h"
#include "native_renderer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "../gs/gs_env.h"
#include "../gs/gs_hw.h"
#include "../gs/gs_mem.h"
#include "../gs/gs_state.h"
#include "../gs/modern_scene.h"
#include "../gs/texture_replacements.h"

#ifdef EOD_PLUME
#include "plume_device.h"
#include "plume_vulkan.h"
#endif

namespace gs {
extern bool g_skip_frame;  // gs_draw.cpp
extern uint32_t g_env_serial;
}

namespace native {
namespace {
std::atomic<uint64_t> g_fge_draws{0};
std::atomic<uint64_t> g_mip_clamped{0};   // EE TEX1.MXL above the levels the GS state decodes
std::atomic<uint64_t> g_mip_textures{0};  // textures decoded with mip levels
std::atomic<uint64_t> g_mip_rt{0};        // lookups whose level n lies in GPU-rendered pages (levels < n used)
uint64_t g_runk_ok = 0, g_runk_bad = 0;  // EE per-run TEX1.K against the GS K sequence (run_k_check)
std::atomic<uint64_t> g_afail_second{0};
std::atomic<uint64_t> g_k_mixed{0};       // batches whose VU1 runs have different K deltas (first used)  // objects drawn with an alpha-test-failed second pass
std::atomic<uint64_t> g_blend_ad{0};      // ALPHA.C = Ad draws (DST_ALPHA factor: Ad/255, not Ad/128)  // native draws with PRIM.FGE set (src/native_fog.cpp)

// ---- per EE frame counters (the frame a record was queued in) ----
struct FrameStats {
    uint32_t objects = 0;     // records the GS was told about (OBJ)
    uint32_t drawn = 0;       // drawn by plume
    uint32_t empty = 0;       // drawn, but the batch decoded to no triangles
    uint32_t skipped = 0;     // frame skipped by the GS (FrameSkip): nothing drawn, as the GS
    uint32_t lost = 0;        // not drawn: no mesh, no surface or no renderer
    uint32_t textured = 0, untextured_rt = 0, skinned = 0, posed = 0, flushes = 0, mipmapped = 0;
    uint64_t suppressed = 0;  // GS primitives not drawn because they were a native object's
    // by Packet::kind (queue, world, water, shadow, flare)
    uint32_t k_objects[KIND_COUNT] = {}, k_drawn[KIND_COUNT] = {}, k_skipped[KIND_COUNT] = {}, k_lost[KIND_COUNT] = {};
};
std::map<uint64_t, FrameStats> g_frames;
uint64_t g_newest_frame = 0;
// P1 (docs/NATIVE_RENDERER_SUCCESS.md): frames whose submitted draws equal what Plume drew
struct Parity {
    uint64_t frames = 0, world_equal = 0, water_equal = 0, world_submitted = 0, world_drawn = 0, water_submitted = 0,
             water_drawn = 0, skipped_frames = 0;
    uint64_t queue_equal = 0, queue_submitted = 0, queue_drawn = 0;  // draw queue records (R1/R3)
    uint64_t shadow_equal = 0, shadow_submitted = 0, shadow_drawn = 0;  // projected shadows (R4 A1)
    uint64_t flare_equal = 0, flare_submitted = 0, flare_drawn = 0;     // sun/light flares (R4 A1)
    uint64_t particle_equal = 0, particle_submitted = 0, particle_drawn = 0;
    uint64_t sprite_equal = 0, sprite_submitted = 0, sprite_drawn = 0;
    uint64_t imm_equal = 0, imm_submitted = 0, imm_drawn = 0;
    uint64_t cockpit_equal = 0, cockpit_submitted = 0, cockpit_drawn = 0;
};
Parity g_parity;
uint64_t g_only_dropped = 0;  // EOD_NATIVE_ONLY: GS primitives not drawn
// EOD_NATIVE_ONLY: what the dropped GS primitives were, by (frame buffer, texture, primitive kind),
// with their screen box (px), printed every EOD_NATIVE_LOG frames: geometry with no native owner.
struct DropKey {
    uint32_t fbp, tbp, kind;
    bool operator<(const DropKey &o) const { return std::tie(fbp, tbp, kind) < std::tie(o.fbp, o.tbp, o.kind); }
};
struct DropStat {
    uint64_t prims = 0;
    int box[4] = {1 << 20, 1 << 20, -(1 << 20), -(1 << 20)};
};
std::map<DropKey, DropStat> g_drops;
void note_drop(const gs::Env &e, const gs::Vertex &v, const gs::Vertex *b, const gs::Vertex *c) {
    DropStat &d = g_drops[{(uint32_t)e.fbp, e.tme ? (uint32_t)e.tbp : 0xFFFFFFFFu, b ? (c ? 3u : 2u) : 1u}];
    d.prims++;
    for (const gs::Vertex *p : {&v, b, c}) {
        if (!p) continue;
        const int x = p->x >> 4, y = p->y >> 4;
        d.box[0] = std::min(d.box[0], x), d.box[1] = std::min(d.box[1], y);
        d.box[2] = std::max(d.box[2], x), d.box[3] = std::max(d.box[3], y);
    }
}
void report_drops(uint64_t frame) {
    for (auto &[k, d] : g_drops)
        std::fprintf(stderr, "[native] gs frame %llu native-only dropped fb %x tex %x %s: %llu prims box %d,%d..%d,%d\n",
                     (unsigned long long)frame, k.fbp, k.tbp, k.kind == 3 ? "tri" : k.kind == 2 ? "sprite/line" : "point",
                     (unsigned long long)d.prims, d.box[0], d.box[1], d.box[2], d.box[3]);
    g_drops.clear();
}

uint64_t log_every() {
    static const uint64_t n = [] {
        const char *e = std::getenv("EOD_NATIVE_LOG");
        return e ? std::strtoull(e, nullptr, 10) : 300ull;
    }();
    return n;
}

// ---- the object whose GS commands are arriving ----
bool g_active = false, g_finalized = false, g_have_env = false;
Packet g_cur;
gs::Env g_env;          // its GS state at its first draw
uint64_t g_xyoffset = 0;

double depth_range(uint32_t zpsm) {
    switch (zpsm & 0xF) {
    case 0x0: return 4294967296.0;
    case 0x1: return 16777216.0;
    default: return 65536.0;
    }
}

uint64_t context_xyoffset() {
    const uint64_t attr = (gs::st.prmodecont & 1) ? gs::st.prim : gs::st.prmode;
    return gs::st.ctx[(attr >> 9) & 1].xyoffset;
}

#ifdef EOD_PLUME
#include "native_shaders_spv.inc"
#include "native_modern_spv.inc"
#include "material_table.inc"

struct PushConstants {
    float cx[4], cy[4], cz[4], cw[4];
    float light[4], diffuse[4], ambient[4];
    uint32_t mode[4];
};
static_assert(sizeof(PushConstants) == 128, "push constants");
enum : uint32_t { M_TME = 0x1, M_TCC = 0x2, M_TFX_SHIFT = 2, M_ATE = 0x10, M_ATST_SHIFT = 5, M_AFAIL_DISCARD = 0x100,
                  M_CFIX = 0x200, M_SPEC = 0x400, M_FGE = 0x800,
                  // stage R5 A2: GS texture addressing (TEX0 size, TEX1, CLAMP) and the AFAIL second pass
                  M_TW_SHIFT = 12, M_TH_SHIFT = 16, M_MXL_SHIFT = 20, M_MMAG = 1u << 23, M_MMIN_SHIFT = 24,
                  M_WMS_SHIFT = 27, M_WMT_SHIFT = 29, M_FAILED = 1u << 31 };
// mode.z bits 16..27: TEX1.K (signed 7.4), 28..29: TEX1.L, 30: TEX1.LCM
enum : uint32_t { Z_K_SHIFT = 16, Z_L_SHIFT = 28, Z_LCM = 1u << 30 };

// blend equations (GS ALPHA a, b, d with C in the shader's second output)
enum Blend : uint32_t { B_OPAQUE, B_MIX, B_ADD, B_REVSUB, B_SCALE, B_DST_ADD, B_INV_MIX, B_KEEP, B_SHADOW, B_COUNT };
// mode.w of a projected-shadow draw (native_shaders.hlsl shadow_ps, stage R5 A3)
enum : uint32_t { S_ON = 1u << 31, S_COMPOSITE = 1u << 30 };
// pipeline key bit 15: generic GS blend (bits 16..17 src factor, 18..19 dst factor, 20..21 op,
// bit 22 C = destination alpha), see blend_classes
enum : uint32_t { K_GENERIC = 1u << 15, K_CDST = 1u << 22, K_NOCLIP = 1u << 23, K_MODERN = 1u << 24 };  // K_MODERN: stage R6 shaders  // K_NOCLIP: depth clamp, no z clipping (GS)

struct GpuMesh {
    std::unique_ptr<Mesh> cpu;
    std::unique_ptr<plume::RenderBuffer> buf;  // vertices, then indices
    uint64_t index_offset = 0;
};

struct Texture {
    std::unique_ptr<plume::RenderTexture> tex;
    std::unique_ptr<plume::RenderBuffer> staging;  // until the upload is recorded
    std::unique_ptr<plume::RenderDescriptorSet> sets[16];  // per sampler (8..15: the Modern look, stage R6)
    uint32_t w = 0, h = 0, levels = 1;
    uint64_t offset[7] = {};  // each level's place in the staging buffer
    uint64_t bytes = 0;       // staging size (texture_bytes accounting)
    uint64_t key = 0, last_use = 0;
    bool pending_upload = false;
    int32_t mat = -1;                 // material record (stage R6), -1: none
    uint32_t sky_top = 0, sky_hor = 0;  // a sky texture's mean upper / lower colour (RGBA8)
};
struct AddrEntry {
    Texture *tex = nullptr;
    uint32_t stamp = 0;  // g_vram_clock when the pages were last checked
};

struct Item {
    Packet p;
    double a[4][4];        // model -> GS screen
    float ox, oy, zscale;
    PushConstants pc;
    uint32_t pipeline;     // pipeline key
    Texture *tex;
    uint32_t sampler;
    int32_t sc[4];         // scissor, GS pixels (x0, y0, x1, y1 inclusive)
    GpuMesh *mesh;
    bool second = false;   // the alpha-test-failed pass of the previous item (not counted again)
    std::vector<int32_t> run_k;  // TEX1.K per VU1 run (Mesh::k_runs), when they differ
};

struct Target {
    uint32_t fbp, fbw, fpsm, zbp, zpsm, rows;
    bool depth;
    bool operator==(const Target &o) const {
        return fbp == o.fbp && fbw == o.fbw && fpsm == o.fpsm && zbp == o.zbp && zpsm == o.zpsm && rows == o.rows &&
               depth == o.depth;
    }
};

// Destroyed some vblanks later, when the GPU no longer reads them.
struct Trash {
    uint64_t vb;
    std::unique_ptr<plume::RenderBuffer> buf;
    std::unique_ptr<plume::RenderTexture> tex;
    std::unique_ptr<plume::RenderDescriptorSet> sets[16];
    std::unique_ptr<plume::RenderFramebuffer> fb;
};

struct Renderer {
    bool tried = false, ok = false;
    plume::VulkanDevice *dev = nullptr;
    std::unique_ptr<plume::RenderCommandQueue> queue;
    std::unique_ptr<plume::RenderCommandList> list;
    std::unique_ptr<plume::RenderPipelineLayout> layout;
    std::unique_ptr<plume::RenderShader> vs, ps, vs_modern, ps_modern;
    std::unique_ptr<plume::RenderSampler> samplers[16];
    std::unordered_map<uint32_t, std::unique_ptr<plume::RenderPipeline>> pipelines;
    Texture white;
    std::unordered_map<uint32_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, std::unique_ptr<Texture>> textures;  // by content
    std::unordered_map<uint64_t, AddrEntry> by_addr;
    XXH3_state_t *hash_state = XXH3_createState();
    uint64_t texture_bytes = 0, decoded = 0;
    // this vblank's surface wrappers and framebuffers (the backend may recycle its images later)
    std::unordered_map<uint64_t, std::unique_ptr<plume::RenderTexture>> wrappers;
    std::map<std::pair<uint64_t, uint64_t>, std::unique_ptr<plume::RenderFramebuffer>> framebuffers;
    std::deque<Trash> trash;
    uint64_t vb = 0;
    std::vector<Item> pending;
    Target target{};
    // adaptive full-RenderScale shadow accumulation buffer (removes PS2 320x512 cap)
    std::vector<Item> shadow_pending;
    Target main_target{};
    bool has_main_target = false;
    uint64_t main_xy = 0;
    int32_t main_sc[4] = {};
    Texture shadow_mask;
    std::unique_ptr<plume::RenderFramebuffer> shadow_fb;
    uint64_t shadow_fb_depth = 0;
    std::unique_ptr<plume::RenderBuffer> quad_buf;
    std::vector<Texture *> uploads;
    std::deque<std::pair<uint32_t, uint64_t>> transient;  // one-draw meshes (id, vblank they arrived)
};
Renderer g_r;

// ---- stage R6: material records and the Classic / Modern look (docs/NATIVE_RENDERER.md "Stage R6")
enum : uint32_t {
    C_GENERIC = 0, C_UNLIT = 1, C_PAINT = 2, C_GLASS = 3, C_WATER = 4, C_CHROME = 5,
    C_VFX_FIRE = 6, C_VFX_SMOKE = 7, C_VFX_SPARK = 8
};
struct Material {
    std::string id, surface;
    uint32_t cls = C_GENERIC, metal4 = 0, rough4 = 12;
    bool emissive = false, height = false, low = false;
    float conf = 0;
};
struct MatKey {
    uint64_t ik, pk;
    uint32_t w, h;
};
std::vector<Material> g_mats;
std::unordered_map<uint64_t, std::vector<std::pair<MatKey, int32_t>>> g_mat_by_ik;
size_t g_mat_records = 0, g_mat_bound = 0;
std::atomic<bool> g_modern{false};
std::atomic<uint32_t> g_look_toggles{0};
// statistics: draws drawn in each look, draws with a material, Modern draws whose texture has a
// low-confidence record (the 318 needs_human records)
uint64_t g_draws_textured = 0, g_draws_material = 0, g_draws_all = 0, g_textures_material = 0, g_textures_indexed = 0;
uint64_t g_draws_vfx[3] = {};  // Modern GPU VFX draws: fire, smoke, spark
uint64_t g_pk_fallback = 0, g_sets_late = 0;
std::map<int32_t, uint32_t> g_low_seen;  // record -> bbox lines logged
float g_sun_world[3] = {0.0f, 1.0f, 0.0f}, g_sun_col[3] = {64, 64, 64};
uint32_t g_sky_top = 0, g_sky_hor = 0;
bool g_sky_seen = false;

// ---- stage R6 A2: EOD_NATIVE_ONLY=1 Plume-side clear state (guardrail row 1) ----
struct OnlyClear {
    bool color = false, depth = false;
    float r = 0, g = 0, b = 0, a = 1, z = 0;
};
std::unordered_map<uint32_t, OnlyClear> g_only_clears;

// ---- stage R6 A2: Native Vulkan GPU Timestamp Timer (guardrail row 8, EOD_GPU_TIMING=1) ----
struct GpuTimer {
    static constexpr uint32_t kRing = 8;
    static constexpr uint32_t kQueries = 64;  // up to 32 native passes per vblank
    struct Slot {
        VkQueryPool pool = VK_NULL_HANDLE;
        uint32_t used = 0;
        uint32_t spans[kQueries / 2] = {};    // bit 0: is_shadow, upper bits: start query index
        uint32_t n_spans = 0;
        uint32_t draws = 0;
        uint64_t vb = 0;
        uint8_t look = 0;
        bool reset_done = false;
        bool pending = false;
    };
    bool enabled = false;
    double ts_period = 1.0;  // nanoseconds per tick
    Slot slots[kRing]{};
    uint32_t cur = 0;
    // Per-look GPU timing accumulators (0 = Classic, 1 = Modern)
    double sum_ms[2] = {}, max_ms[2] = {}, shadow_ms[2] = {};
    uint64_t frames[2] = {}, draws[2] = {};
};
GpuTimer g_gpu;

void gpu_timer_init() {
    auto env_on = [](const char *k) {
        const char *v = std::getenv(k);
        return v && *v && *v != '0';
    };
    if (!env_on("EOD_GPU_TIMING") && !env_on("EOD_GPU_PROF")) return;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(g_r.dev->physicalDevice, &props);
    if (!props.limits.timestampComputeAndGraphics || props.limits.timestampPeriod <= 0.0f) return;
    g_gpu.ts_period = (double)props.limits.timestampPeriod;
    VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = GpuTimer::kQueries;
    for (uint32_t i = 0; i < GpuTimer::kRing; i++) {
        if (vkCreateQueryPool(g_r.dev->vk, &ci, nullptr, &g_gpu.slots[i].pool) != VK_SUCCESS) {
            for (uint32_t j = 0; j < i; j++) {
                vkDestroyQueryPool(g_r.dev->vk, g_gpu.slots[j].pool, nullptr);
                g_gpu.slots[j].pool = VK_NULL_HANDLE;
            }
            return;
        }
    }
    g_gpu.enabled = true;
    std::fprintf(stderr, "[r6] native Vulkan GPU timer ready (timestampPeriod %.2f ns)\n", g_gpu.ts_period);
}

void gpu_timer_collect_slot(GpuTimer::Slot &s, bool wait) {
    if (!s.pending || !s.pool || s.used == 0) return;
    uint64_t ts[GpuTimer::kQueries]{};
    const VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT | (wait ? VK_QUERY_RESULT_WAIT_BIT : 0u);
    if (vkGetQueryPoolResults(g_r.dev->vk, s.pool, 0, s.used, sizeof ts, ts, sizeof(uint64_t), flags) != VK_SUCCESS)
        return;
    double main_ns = 0.0, sh_ns = 0.0;
    for (uint32_t i = 0; i < s.n_spans; i++) {
        const uint32_t q0 = s.spans[i] >> 1;
        const bool is_sh = (s.spans[i] & 1u) != 0;
        if (q0 + 1 < s.used && ts[q0 + 1] >= ts[q0]) {
            const double dt = (double)(ts[q0 + 1] - ts[q0]) * g_gpu.ts_period;
            if (is_sh) sh_ns += dt;
            else main_ns += dt;
        }
    }
    const double total_ms = (main_ns + sh_ns) / 1e6;
    const double sh_ms = sh_ns / 1e6;
    const int l = s.look ? 1 : 0;
    // Row 8 compares the looks over the same stretch of play: once a toggle has happened, the
    // frames before it (boot, menus, loading, pre-race in one look only) are dropped.
    static bool toggled_once = false;
    if (!toggled_once && g_look_toggles.load(std::memory_order_relaxed)) {
        toggled_once = true;
        for (int k = 0; k < 2; k++) g_gpu.sum_ms[k] = g_gpu.max_ms[k] = g_gpu.shadow_ms[k] = 0.0, g_gpu.frames[k] = g_gpu.draws[k] = 0;
    }
    g_gpu.sum_ms[l] += total_ms;
    g_gpu.shadow_ms[l] += sh_ms;
    g_gpu.max_ms[l] = std::max(g_gpu.max_ms[l], total_ms);
    g_gpu.frames[l]++;
    g_gpu.draws[l] += s.draws;
    EOD_PLOT("native_gpu_us", (int64_t)std::llround(total_ms * 1000.0));
    EOD_PLOT("native_gpu_shadow_us", (int64_t)std::llround(sh_ms * 1000.0));
    if (l) EOD_PLOT("native_gpu_modern_us", (int64_t)std::llround(total_ms * 1000.0));
    else EOD_PLOT("native_gpu_classic_us", (int64_t)std::llround(total_ms * 1000.0));
    s.pending = false;
    s.reset_done = false;
    s.used = 0;
    s.n_spans = 0;
    s.draws = 0;
}

uint32_t gpu_timer_begin(VkCommandBuffer cmd, bool is_shadow, bool modern) {
    if (!g_gpu.enabled) return ~0u;
    GpuTimer::Slot &s = g_gpu.slots[g_gpu.cur];
    if (s.pending) gpu_timer_collect_slot(s, true);
    if (!s.reset_done) {
        vkCmdResetQueryPool(cmd, s.pool, 0, GpuTimer::kQueries);
        s.reset_done = true;
        s.used = 0;
        s.n_spans = 0;
        s.draws = 0;
        s.look = modern ? 1 : 0;
    }
    if (s.used + 2 > GpuTimer::kQueries || s.n_spans >= GpuTimer::kQueries / 2) return ~0u;
    const uint32_t q0 = s.used;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s.pool, q0);
    s.spans[s.n_spans++] = (q0 << 1) | (is_shadow ? 1u : 0u);
    s.used += 2;
    return q0;
}

void gpu_timer_end(VkCommandBuffer cmd, uint32_t q0, uint32_t draw_count) {
    if (!g_gpu.enabled || q0 == ~0u) return;
    GpuTimer::Slot &s = g_gpu.slots[g_gpu.cur];
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.pool, q0 + 1);
    s.draws += draw_count;
}

void gpu_timer_end_vblank(uint64_t vb) {
    if (!g_gpu.enabled) return;
    for (uint32_t i = 0; i < GpuTimer::kRing; i++)
        if (g_gpu.slots[i].pending) gpu_timer_collect_slot(g_gpu.slots[i], false);
    GpuTimer::Slot &s = g_gpu.slots[g_gpu.cur];
    if (s.used > 0) {
        s.vb = vb;
        s.pending = true;
        g_gpu.cur = (g_gpu.cur + 1) % GpuTimer::kRing;
    }
}

void load_materials() {
    const char *text = k_material_tsv;
    std::string buf;
    if (const char *path = std::getenv("EOD_MATERIALS")) {  // a regenerated table without a rebuild
        if (FILE *f = std::fopen(path, "rb")) {
            char tmp[65536];
            size_t n;
            while ((n = std::fread(tmp, 1, sizeof tmp, f)) > 0) buf.append(tmp, n);
            std::fclose(f);
            text = buf.c_str();
        }
    }
    std::unordered_map<std::string, int32_t> by_id;
    const char *p = text;
    while (*p) {
        const char *e = std::strchr(p, '\n');
        if (!e) e = p + std::strlen(p);
        std::string line(p, e);
        p = *e ? e + 1 : e;
        if (line.empty() || line[0] == '/' || line[0] == 's') continue;
        std::vector<std::string> f;
        size_t a = 0;
        for (;;) {
            const size_t b = line.find('\t', a);
            f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        if (f.size() < 13) continue;
        int32_t idx;
        auto it = by_id.find(f[0]);
        if (it == by_id.end()) {
            Material m;
            m.id = f[0];
            m.surface = f[5];
            const float metal = std::strtof(f[6].c_str(), nullptr), rough = std::strtof(f[7].c_str(), nullptr);
            m.metal4 = (uint32_t)std::lround(std::clamp(metal, 0.0f, 1.0f) * 15.0f);
            m.rough4 = (uint32_t)std::lround(std::clamp(rough, 0.0f, 1.0f) * 15.0f);
            m.height = f[8] != "none";
            m.emissive = f[9] == "1";
            m.conf = std::strtof(f[11].c_str(), nullptr);
            m.low = f[12] == "1";
            const std::string &su = m.surface;
            if (su == "fx" || su == "emissive_light" || su == "sky" || su == "backdrop" || su == "crowd_sprite" ||
                su == "fake_shadow" || su == "ui")
                m.cls = C_UNLIT;
            else if (su == "vfx_fire" || su == "vfx_smoke" || su == "vfx_spark") {
                // Modern GPU VFX is on by default since R6 A5 (guardrails 4..8 pass with it);
                // EOD_MODERN_VFX=0 keeps these records unlit fx cards
                static const bool vfx = !std::getenv("EOD_MODERN_VFX") || *std::getenv("EOD_MODERN_VFX") != '0';
                m.cls = !vfx ? C_UNLIT : su == "vfx_fire" ? C_VFX_FIRE : su == "vfx_smoke" ? C_VFX_SMOKE : C_VFX_SPARK;
            }
            else if (su == "car_paint") m.cls = C_PAINT;
            else if (su == "glass") m.cls = C_GLASS, m.rough4 = std::min(m.rough4, 2u);
            else if (su == "water") m.cls = C_WATER, m.rough4 = std::min(m.rough4, 3u);
            else if (su == "chrome") m.cls = C_CHROME;
            idx = (int32_t)g_mats.size();
            g_mats.push_back(m);
            by_id[f[0]] = idx;
        } else {
            idx = it->second;
        }
        if (f[1] != "-") {
            MatKey k{std::strtoull(f[1].c_str(), nullptr, 16), std::strtoull(f[2].c_str(), nullptr, 16),
                     (uint32_t)std::atoi(f[3].c_str()), (uint32_t)std::atoi(f[4].c_str())};
            auto &v = g_mat_by_ik[k.ik];
            v.push_back({k, idx});
        }
    }
    g_mat_records = g_mats.size();
    std::unordered_set<int32_t> bound;
    for (auto &kv : g_mat_by_ik)
        for (auto &q : kv.second) bound.insert(q.second);
    g_mat_bound = bound.size();
    const char *look = std::getenv("EOD_LOOK");
    if (look && (look[0] == 'm' || look[0] == 'M')) g_modern = true;
    std::fprintf(stderr, "[r6] material records loaded: %zu (%zu with a runtime texture key); look %s\n", g_mat_records,
                 g_mat_bound, g_modern ? "Modern" : "Classic");
}

// The record of a texture being decoded: indexKey / paletteKey from GS memory as the host-texture
// path computes them (runtime/gs/gs_draw.cpp capture_texture), same size; else the same indexKey.
int32_t material_of(const gs::Env &e) {
    const bool eight = e.tpsm == gs::PSMT8 || e.tpsm == gs::PSMT8H;
    const bool four = e.tpsm == gs::PSMT4 || e.tpsm == gs::PSMT4HL || e.tpsm == gs::PSMT4HH;
    if (!eight && !four) return -1;
    g_textures_indexed++;
    static std::vector<uint8_t> plane;
    plane.resize((size_t)e.tw * e.th);
    for (uint32_t y = 0; y < e.th; y++)
        for (uint32_t x = 0; x < e.tw; x++)
            plane[(size_t)y * e.tw + x] = (uint8_t)gs::read_pixel(e.tpsm, (int)x, (int)y, e.mip_tbp[0], e.mip_tbw[0]);
    const uint64_t ik = gs::hosttex::index_key(eight ? 8 : 4, e.tw, e.th, plane.data());
    auto it = g_mat_by_ik.find(ik);
    if (it == g_mat_by_ik.end()) return -1;
    const uint64_t pk = gs::hosttex::palette_key(e.pal, eight ? 256 : 16);
    int32_t any = -1;
    for (const auto &q : it->second) {
        if (q.first.w != e.tw || q.first.h != e.th) continue;
        if (q.first.pk == pk) return q.second;
        if (any < 0) any = q.second;
    }
    if (any >= 0) g_pk_fallback++;
    return any;
}

plume::RenderDescriptorSetDesc set_desc() {
    static plume::RenderDescriptorRange ranges[2] = {
        plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 0, 1),
        plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 1, 1)};
    return plume::RenderDescriptorSetDesc(ranges, 2);
}

bool init() {
    if (g_r.tried) return g_r.ok;
    g_r.tried = true;
#ifdef EOD_HW_SELECT
    if (!gs::hw::vulkan_active()) {
        std::fprintf(stderr, "[native] needs the Vulkan renderer: the GS draws everything\n");
        return false;
    }
#endif
    auto *dev = static_cast<plume::VulkanDevice *>(plume_device::render_device());
    if (!dev) {
        std::fprintf(stderr, "[native] no plume device (EOD_PLUME=0?): the GS draws everything\n");
        return false;
    }
    g_r.dev = dev;
    g_r.queue = dev->createCommandQueue(plume::RenderCommandListType::DIRECT);
    g_r.list = g_r.queue->createCommandList();
    const plume::RenderDescriptorSetDesc sd = set_desc();
    plume::RenderPushConstantRange pcr(0, 0, 0, sizeof(PushConstants),
                                       plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);
    plume::RenderPipelineLayoutDesc ld(&pcr, 1, &sd, 1, false, true);
    g_r.layout = dev->createPipelineLayout(ld);
    g_r.vs = dev->createShader(k_native_vs_main, sizeof k_native_vs_main, "vs_main", plume::RenderShaderFormat::SPIRV);
    g_r.ps = dev->createShader(k_native_ps_main, sizeof k_native_ps_main, "ps_main", plume::RenderShaderFormat::SPIRV);
    g_r.vs_modern = dev->createShader(k_native_vs_modern, sizeof k_native_vs_modern, "vs_modern", plume::RenderShaderFormat::SPIRV);
    g_r.ps_modern = dev->createShader(k_native_ps_modern, sizeof k_native_ps_modern, "ps_modern", plume::RenderShaderFormat::SPIRV);
    // the Modern look's samplers (8..15): trilinear, anisotropic, every level the texture holds;
    // bit 1 / 2 the GS CLAMP.WMS / WMT (clamp, else repeat)
    for (uint32_t i = 8; i < 16; i++) {
        plume::RenderSamplerDesc s;
        s.minFilter = s.magFilter = plume::RenderFilter::LINEAR;
        s.mipmapMode = plume::RenderMipmapMode::LINEAR;
        s.addressU = (i & 2) ? plume::RenderTextureAddressMode::CLAMP : plume::RenderTextureAddressMode::WRAP;
        s.addressV = (i & 4) ? plume::RenderTextureAddressMode::CLAMP : plume::RenderTextureAddressMode::WRAP;
        s.addressW = plume::RenderTextureAddressMode::CLAMP;
        s.anisotropyEnabled = true;
        s.maxAnisotropy = 8;
        g_r.samplers[i] = dev->createSampler(s);
    }
    load_materials();
    for (uint32_t i = 0; i < 8; i++) {
        plume::RenderSamplerDesc s;
        s.minFilter = s.magFilter = (i & 1) ? plume::RenderFilter::LINEAR : plume::RenderFilter::NEAREST;
        s.mipmapMode = plume::RenderMipmapMode::NEAREST;
        s.addressU = (i & 2) ? plume::RenderTextureAddressMode::CLAMP : plume::RenderTextureAddressMode::WRAP;
        s.addressV = (i & 4) ? plume::RenderTextureAddressMode::CLAMP : plume::RenderTextureAddressMode::WRAP;
        s.addressW = plume::RenderTextureAddressMode::CLAMP;
        s.maxLOD = 0;
        g_r.samplers[i] = dev->createSampler(s);
    }
    if (!g_r.layout || !g_r.vs || !g_r.ps || !g_r.list || !g_r.vs_modern || !g_r.ps_modern) {
        std::fprintf(stderr, "[native] plume setup failed: the GS draws everything\n");
        return false;
    }
    // 1x1 texture for untextured objects (the set layout has a texture)
    g_r.white.tex = dev->createTexture(plume::RenderTextureDesc::Texture2D(1, 1, 1, plume::RenderFormat::R8G8B8A8_UNORM));
    g_r.white.staging = dev->createBuffer(plume::RenderBufferDesc::UploadBuffer(256));
    const uint32_t px = 0x80808080u;
    std::memcpy(g_r.white.staging->map(), &px, 4);
    g_r.white.staging->unmap();
    g_r.white.w = g_r.white.h = 1;
    g_r.white.pending_upload = true;
    g_r.uploads.push_back(&g_r.white);
    gpu_timer_init();
    g_r.ok = true;
    std::fprintf(stderr, "[native] plume renderer ready (%s)\n", dev->description.name.c_str());
    return true;
}

plume::RenderDescriptorSet *texture_set(Texture *t, uint32_t sampler) {
    auto make = [&](uint32_t i) {
        auto &s = t->sets[i];
        if (!s) {
            s = g_r.dev->createDescriptorSet(set_desc());
            s->setTexture(0, t->tex.get(), plume::RenderTextureLayout::SHADER_READ);
            s->setSampler(1, g_r.samplers[i].get());
        }
        return s.get();
    };
    // stage R6: a texture's Modern sets (8..15, one per CLAMP.WMS / WMT pair: the Modern look
    // addresses with the sampler) are made with its first set, so a look toggle creates none (P5)
    if (g_look_toggles.load() && !t->sets[sampler] && t->sets[0] && t->sets[8]) g_sets_late++;  // P5: must stay 0
    if (t != &g_r.shadow_mask && !t->sets[sampler])
        for (uint32_t i = sampler >= 8 ? 0 : 8; i < 16; i += 2) make(i == 0 ? 0 : i);  // and the Classic set 0
    return make(sampler);
}

plume::RenderPipeline *pipeline_make(uint32_t key);
// stage R6: every pipeline is made with its Classic / Modern twin, so a look toggle compiles nothing
// (P5); a projected-shadow pipeline (B_SHADOW) has no Modern twin
plume::RenderPipeline *pipeline(uint32_t key) {
    auto it = g_r.pipelines.find(key);
    if (it != g_r.pipelines.end() && it->second) return it->second.get();
    if ((key & 15) != B_SHADOW) pipeline_make(key ^ K_MODERN);
    return pipeline_make(key);
}
plume::RenderPipeline *pipeline_make(uint32_t key) {
    auto &slot = g_r.pipelines[key];
    if (slot) return slot.get();
    const uint32_t blend = key & 15, zfunc = (key >> 4) & 3, mask = (key >> 8) & 15;
    const uint32_t cull = (key >> 12) & 3;
    const bool zwrite = (key >> 6) & 1, depth = (key >> 7) & 1, blend_alpha = (key >> 14) & 1;
    plume::RenderInputSlot in(0, sizeof(Vertex));
    plume::RenderInputElement el[4] = {
        plume::RenderInputElement("POSITION", 0, 0, plume::RenderFormat::R32G32B32_FLOAT, 0, 0),
        plume::RenderInputElement("TEXCOORD", 0, 1, plume::RenderFormat::R32G32_FLOAT, 0, 12),
        plume::RenderInputElement("NORMAL", 0, 2, plume::RenderFormat::R8G8B8A8_SINT, 0, 20),
        plume::RenderInputElement("COLOR", 0, 3, plume::RenderFormat::R8G8B8A8_UNORM, 0, 24)};
    plume::RenderGraphicsPipelineDesc d;
    d.pipelineLayout = g_r.layout.get();
    d.vertexShader = (key & K_MODERN) ? g_r.vs_modern.get() : g_r.vs.get();
    d.pixelShader = (key & K_MODERN) ? g_r.ps_modern.get() : g_r.ps.get();
    d.inputSlots = &in;
    d.inputSlotsCount = 1;
    d.inputElements = el;
    d.inputElementsCount = 4;
    d.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    d.cullMode = cull == 1 ? plume::RenderCullMode::BACK : cull == 2 ? plume::RenderCullMode::FRONT : plume::RenderCullMode::NONE;
    d.depthClipEnabled = (key & K_NOCLIP) == 0;
    d.renderTargetCount = 1;
    d.renderTargetFormat[0] = plume::RenderFormat::R8G8B8A8_UNORM;
    if (depth) {
        d.depthTargetFormat = plume::RenderFormat::D32_FLOAT;
        d.depthEnabled = true;
        d.depthWriteEnabled = zwrite;
        d.depthFunction = zfunc == 0   ? plume::RenderComparisonFunction::ALWAYS
                          : zfunc == 1 ? plume::RenderComparisonFunction::GREATER_EQUAL
                          : zfunc == 2 ? plume::RenderComparisonFunction::GREATER
                                       : plume::RenderComparisonFunction::NEVER;
    }
    using B = plume::RenderBlend;
    using O = plume::RenderBlendOperation;
    plume::RenderBlendDesc &b = d.renderTargetBlend[0];
    b.blendEnabled = blend != B_OPAQUE;
    b.blendOp = (blend == B_REVSUB || blend == B_SHADOW) ? O::REV_SUBTRACT : O::ADD;
    if (key & K_GENERIC) {
        // GS (A - B) C + D as src * F(src) op dst * F(dst) (blend_classes): F 0 zero, 1 one, 2 C, 3 1 - C;
        // C is the shader's second output (As or FIX), or the destination alpha when K_CDST
        const bool cd = (key & K_CDST) != 0;
        auto factor = [&](uint32_t f) {
            switch (f & 3) {
            case 0: return B::ZERO;
            case 1: return B::ONE;
            case 2: return cd ? B::DEST_ALPHA : B::SRC1_ALPHA;
            default: return cd ? B::INV_DEST_ALPHA : B::INV_SRC1_ALPHA;
            }
        };
        const uint32_t sf = (key >> 16) & 3, df = (key >> 18) & 3, op = (key >> 20) & 3;
        b.blendEnabled = !(sf == 1 && df == 0 && op == 0);
        b.srcBlend = factor(sf);
        b.dstBlend = factor(df);
        b.blendOp = op == 1 ? O::SUBTRACT : op == 2 ? O::REV_SUBTRACT : O::ADD;
    } else
    switch (blend) {
    case B_MIX: b.srcBlend = B::SRC1_ALPHA, b.dstBlend = B::INV_SRC1_ALPHA; break;
    case B_ADD: b.srcBlend = B::SRC1_ALPHA, b.dstBlend = B::ONE; break;
    case B_REVSUB: b.srcBlend = B::SRC1_ALPHA, b.dstBlend = B::ONE; break;
    case B_SCALE: b.srcBlend = B::SRC1_ALPHA, b.dstBlend = B::ZERO; break;
    case B_DST_ADD: b.srcBlend = B::ONE, b.dstBlend = B::SRC1_ALPHA; break;
    case B_INV_MIX: b.srcBlend = B::INV_SRC1_ALPHA, b.dstBlend = B::SRC1_ALPHA; break;
    case B_KEEP: b.srcBlend = B::ZERO, b.dstBlend = B::ONE; break;
    case B_SHADOW: b.srcBlend = B::ONE, b.dstBlend = B::INV_SRC1_ALPHA; break;  // dst (1 - As/128) - src
    default: b.srcBlend = B::ONE, b.dstBlend = B::ZERO; break;
    }
    // the GS writes the source alpha as it is; shadow accumulation blends alpha with the same op
    if (blend_alpha) {
        b.srcBlendAlpha = B::ONE;
        b.dstBlendAlpha = B::ONE;
        b.blendOpAlpha = b.blendOp;
    } else {
        b.srcBlendAlpha = B::ONE;
        b.dstBlendAlpha = B::ZERO;
        b.blendOpAlpha = O::ADD;
    }
    b.renderTargetWriteMask = (uint8_t)mask;
    slot = g_r.dev->createGraphicsPipeline(d);
    return slot.get();
}

// The object's texture from GS memory (TEX0 as its first draw had it). The texture manager
// uploads each bucket's texture into one of two shared slots (TEX0 0x2BC4 / 0x2FC8 in races), so
// textures are cached by content: the bytes of the GS memory pages they occupy plus the TEX0
// fields that decide the decoding (block offset, width, format, size, CLUT, TEXA). An address
// whose pages were not written since it was last looked at reuses its entry without hashing.
// Render-target textures (pages a GPU surface holds newer than the CPU copy) are not decoded.
// `levels`: the mip levels beyond the base the draw's TEX1.MXL selects (stage R5 A2), each read at
// its MIPTBP address (0x35F9A0 writes MIPTBP1/2 from the level offsets 0x35F278 computes).
Texture *texture(const gs::Env &e, FrameStats &fs, uint32_t levels) {
    if (!e.tme || e.tw == 0 || e.th == 0 || e.tw > 1024 || e.th > 1024) return nullptr;
    // the GS state decodes levels only as far as its own TEX1 asks (gs_draw.cpp setup_env)
    const uint32_t decodable = (e.mmin >= 2 && e.mxl > 0) ? (uint32_t)std::min(e.mxl, 6) : 0;
    if (levels > decodable) {
        g_mip_clamped.fetch_add(1);
        levels = decodable;
    }
    uint32_t first, count;
    gs::page_range(e.tbp, e.tbw, e.tpsm, 0, 0, (int)e.tw, (int)e.th, first, count);
    if (!gs::hw::native_texture_on_cpu(first, count)) {
        fs.untextured_rt++;
        return nullptr;
    }
    for (uint32_t l = 1; l <= levels; l++) {
        uint32_t f2, c2;
        gs::page_range(e.mip_tbp[l], e.mip_tbw[l], e.tpsm, 0, 0, std::max(1, (int)e.tw >> l), std::max(1, (int)e.th >> l), f2, c2);
        if (!gs::hw::native_texture_on_cpu(f2, c2)) {  // a level in GPU-rendered pages: the levels above it only
            g_mip_rt.fetch_add(1);
            levels = l - 1;
            break;
        }
    }
    const bool indexed = e.tpsm == gs::PSMT8 || e.tpsm == gs::PSMT4 || e.tpsm == gs::PSMT8H ||
                         e.tpsm == gs::PSMT4HL || e.tpsm == gs::PSMT4HH;
    uint64_t k[5] = {(e.tbp & 31) | (uint64_t)e.tbw << 32, e.tpsm | (uint64_t)e.tw << 16 | (uint64_t)e.th << 32,
                     indexed ? e.pal_hash : 0,
                     e.tpsm == gs::PSMCT32 ? 0 : (e.ta0 | e.ta1 << 8 | (uint64_t)e.aem << 16), 0};
    k[4] = e.tbp;
    k[1] |= (uint64_t)levels << 48;
    uint64_t mk[6] = {};
    for (uint32_t l = 1; l <= levels; l++) mk[l - 1] = e.mip_tbp[l] | (uint64_t)e.mip_tbw[l] << 32;
    const uint64_t addr_key = XXH3_64bits(k, sizeof k) ^ (levels ? XXH3_64bits(mk, sizeof mk) : 0);
    AddrEntry &ae = g_r.by_addr[addr_key];
    if (ae.tex) {
        bool ok = true;
        for (uint32_t i = 0; i < count && ok; i++)
            if (gs::g_page_gen[(first + i) % gs::kPages] > ae.stamp) ok = false;
        for (uint32_t l = 1; l <= levels && ok; l++) {
            uint32_t f2, c2;
            gs::page_range(e.mip_tbp[l], e.mip_tbw[l], e.tpsm, 0, 0, std::max(1, (int)e.tw >> l), std::max(1, (int)e.th >> l), f2, c2);
            for (uint32_t i = 0; i < c2 && ok; i++)
                if (gs::g_page_gen[(f2 + i) % gs::kPages] > ae.stamp) ok = false;
        }
        if (ok) {
            ae.tex->last_use = g_r.vb;
            return ae.tex;
        }
    }
    // content key
    XXH3_state_t *st = g_r.hash_state;
    XXH3_64bits_reset(st);
    XXH3_64bits_update(st, k, sizeof k - sizeof k[4]);
    // the 256-byte GS memory blocks the texture covers (pages also hold other textures and CLUTs)
    {
        int bw = 8, bh = 8;
        switch (e.tpsm) {
        case gs::PSMCT16: case gs::PSMCT16S: bw = 16, bh = 8; break;
        case gs::PSMT8: bw = 16, bh = 16; break;
        case gs::PSMT4: bw = 32, bh = 16; break;
        default: break;  // 32-bit layouts (CT32, CT24, T8H, T4HL, T4HH)
        }
        for (uint32_t l = 0; l <= levels; l++) {
            const uint32_t bp = e.mip_tbp[l], bwid = e.mip_tbw[l];
            const int lw = std::max(1, (int)e.tw >> l), lh = std::max(1, (int)e.th >> l);
            for (int y = 0; y < lh; y += bh)
                for (int x = 0; x < lw; x += bw) {
                    uint32_t byte;
                    switch (e.tpsm) {
                    case gs::PSMCT16: byte = gs::addr16(x, y, bp, bwid) * 2; break;
                    case gs::PSMCT16S: byte = gs::addr16s(x, y, bp, bwid) * 2; break;
                    case gs::PSMT8: byte = gs::addr8(x, y, bp, bwid); break;
                    case gs::PSMT4: byte = gs::addr4(x, y, bp, bwid) / 2; break;
                    default: byte = gs::addr32(x, y, bp, bwid) * 4; break;
                    }
                    XXH3_64bits_update(st, gs::vram + ((byte & ~255u) & (gs::kVramSize - 1)), 256);
                }
        }
    }
    const uint64_t key = XXH3_64bits_digest(st);
    ae.stamp = gs::g_vram_clock;
    std::unique_ptr<Texture> &slot = g_r.textures[key];
    if (slot) {
        ae.tex = slot.get();
        slot->last_use = g_r.vb;
        return slot.get();
    }
    slot = std::make_unique<Texture>();
    Texture &t = *slot;
    ae.tex = &t;
    t.key = key;
    t.last_use = g_r.vb;
    t.w = e.tw;
    t.h = e.th;
    t.levels = levels + 1;
    uint64_t total = 0;
    for (uint32_t l = 0; l < t.levels; l++) {
        const uint32_t lw = std::max(1u, t.w >> l), lh = std::max(1u, t.h >> l);
        t.offset[l] = total;
        total += (uint64_t)((lw * 4 + 255) & ~255u) * lh;
        total = (total + 511) & ~511ull;  // placed-footprint offsets
    }
    t.staging = g_r.dev->createBuffer(plume::RenderBufferDesc::UploadBuffer(total));
    auto *dst = static_cast<uint8_t *>(t.staging->map());
    for (uint32_t l = 0; l < t.levels; l++) {
        const uint32_t lw = std::max(1u, t.w >> l), lh = std::max(1u, t.h >> l), row = (lw * 4 + 255) & ~255u;
        for (uint32_t v = 0; v < lh; v++) {
            uint32_t *line = reinterpret_cast<uint32_t *>(dst + t.offset[l] + (size_t)v * row);
            for (uint32_t u = 0; u < lw; u++) line[u] = gs::tex_fetch(e, (int)u, (int)v, (int)l);
        }
    }
    // stage R6: the material record, and a sky texture's upper / lower mean colour (the Modern IBL)
    t.mat = g_mats.empty() ? -1 : material_of(e);
    if (t.mat >= 0) g_textures_material++;
    if (t.mat >= 0 && g_mats[(size_t)t.mat].surface == "sky") {
        const uint32_t row = (t.w * 4 + 255) & ~255u;
        double sum[2][3] = {};
        for (uint32_t v = 0; v < t.h; v++) {
            const uint32_t *line = reinterpret_cast<const uint32_t *>(dst + (size_t)v * row);
            const int half = v * 2 < t.h ? 0 : 1;
            for (uint32_t u = 0; u < t.w; u++)
                for (int c = 0; c < 3; c++) sum[half][c] += (line[u] >> (8 * c)) & 0xFF;
        }
        const double n = (double)t.w * std::max(1u, t.h / 2);
        auto pack = [&](const double *s3) {
            uint32_t r = 0;
            for (int c = 0; c < 3; c++) r |= (uint32_t)std::min(255.0, s3[c] / n) << (8 * c);
            return r;
        };
        t.sky_top = pack(sum[0]);
        t.sky_hor = pack(sum[1]);
    }
    t.staging->unmap();
    t.tex = g_r.dev->createTexture(plume::RenderTextureDesc::Texture2D(t.w, t.h, t.levels, plume::RenderFormat::R8G8B8A8_UNORM));
    t.pending_upload = true;
    g_r.uploads.push_back(&t);
    g_r.texture_bytes += total;
    t.bytes = total;
    if (levels) g_mip_textures.fetch_add(1);
    g_r.decoded++;
    return &t;
}

// Textures unused for a while go once the cache is large (adaptive: it grows with what the
// loaded tracks and cars use, and releases what they stopped using).
void evict_textures() {
    if (g_r.texture_bytes < (256ull << 20)) return;
    for (auto it = g_r.textures.begin(); it != g_r.textures.end();) {
        Texture *t = it->second.get();
        if (t->last_use + 120 < g_r.vb && !t->pending_upload) {
            for (auto a = g_r.by_addr.begin(); a != g_r.by_addr.end();)
                a = a->second.tex == t ? g_r.by_addr.erase(a) : std::next(a);
            Trash tr{g_r.vb};
            tr.tex = std::move(t->tex);
            for (int i = 0; i < 16; i++) tr.sets[i] = std::move(t->sets[i]);
            g_r.trash.push_back(std::move(tr));
            g_r.texture_bytes -= t->bytes;
            it = g_r.textures.erase(it);
        } else {
            ++it;
        }
    }
}

void record_uploads(plume::RenderCommandList *cl) {
    for (Texture *t : g_r.uploads) {
        if (!t->pending_upload || !t->tex) continue;
        cl->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(t->tex.get(), plume::RenderTextureLayout::COPY_DEST));
        for (uint32_t l = 0; l < t->levels; l++) {
            const uint32_t lw = std::max(1u, t->w >> l), lh = std::max(1u, t->h >> l), row = (lw * 4 + 255) & ~255u;
            cl->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(t->tex.get(), l),
                                  plume::RenderTextureCopyLocation::PlacedFootprint(t->staging.get(), plume::RenderFormat::R8G8B8A8_UNORM,
                                                                                    lw, lh, 1, row / 4, t->offset[l]));
        }
        cl->barriers(plume::RenderBarrierStage::GRAPHICS,
                     plume::RenderTextureBarrier(t->tex.get(), plume::RenderTextureLayout::SHADER_READ));
        t->pending_upload = false;
        Trash tr{g_r.vb};  // the staging buffer outlives the copy
        tr.buf = std::move(t->staging);
        g_r.trash.push_back(std::move(tr));
    }
    g_r.uploads.clear();
}

GpuMesh *gpu_mesh(uint32_t id) {
    auto it = g_r.meshes.find(id);
    if (it == g_r.meshes.end()) return nullptr;
    GpuMesh &m = it->second;
    if (!m.buf && !m.cpu->indices.empty()) {
        const uint64_t vb = m.cpu->vertices.size() * sizeof(Vertex);
        m.index_offset = (vb + 15) & ~15ull;
        const uint64_t size = m.index_offset + m.cpu->indices.size() * 4;
        m.buf = g_r.dev->createBuffer(plume::RenderBufferDesc::UploadBuffer(
            size, plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::INDEX));
        auto *p = static_cast<uint8_t *>(m.buf->map());
        std::memcpy(p, m.cpu->vertices.data(), vb);
        std::memcpy(p + m.index_offset, m.cpu->indices.data(), m.cpu->indices.size() * 4);
        m.buf->unmap();
    }
    return &m;
}

plume::RenderTexture *wrapper(uint64_t image, uint32_t w, uint32_t h, bool depth) {
    const uint64_t key = image ^ ((uint64_t)w << 48) ^ ((uint64_t)h << 32) ^ (depth ? 1 : 0);
    auto &slot = g_r.wrappers[key];
    if (!slot) {
        auto *t = new plume::VulkanTexture(g_r.dev, (VkImage)image);
        t->desc = plume::RenderTextureDesc::Texture2D(w, h, 1, depth ? plume::RenderFormat::D32_FLOAT : plume::RenderFormat::R8G8B8A8_UNORM,
                                                      depth ? plume::RenderTextureFlag::DEPTH_TARGET : plume::RenderTextureFlag::RENDER_TARGET);
        t->imageFormat = depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        t->fillSubresourceRange();
        t->createImageView(t->imageFormat);
        slot.reset(t);
    }
    auto *t = static_cast<plume::VulkanTexture *>(slot.get());
    t->textureLayout = plume::RenderTextureLayout::GENERAL;  // the backend keeps every image there
    t->barrierStages = plume::RenderBarrierStage::ALL;
    return t;
}

// water surfaces drawn per viewport (split screen: one per view)
void water_view_drawn(size_t triangles, const int32_t sc[4], float ox, float oy) {
    // a view is told apart by where the GS puts it: scissor and XYOFFSET
    static std::map<std::array<int32_t, 6>, std::array<uint64_t, 2>> views;
    auto &n = views[{sc[0], sc[1], sc[2], sc[3], (int32_t)ox, (int32_t)oy}];
    n[0]++;
    n[1] += triangles;
    if ((n[0] & 127) == 0)
        for (auto &v : views)
            std::fprintf(stderr, "[native] water drawn by plume in the view at GS scissor %d,%d-%d,%d XYOFFSET %d,%d: draws %llu triangles %llu\n",
                         v.first[0], v.first[1], v.first[2], v.first[3], v.first[4], v.first[5],
                         (unsigned long long)v.second[0], (unsigned long long)v.second[1]);
}

void ensure_shadow_resources(uint32_t w, uint32_t h) {
    if (!g_r.quad_buf) {
        // full-screen NDC quad (vs_main with identity cx..cw maps p.xy directly to NDC)
        const Vertex qv[4] = {
            {{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}},
            {{ 1.0f, -1.0f, 0.0f}, {1.0f, 0.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}},
            {{-1.0f,  1.0f, 0.0f}, {0.0f, 1.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}},
            {{ 1.0f,  1.0f, 0.0f}, {1.0f, 1.0f}, {0, 0, 0, 0}, {0, 0, 0, 0}},
        };
        const uint32_t qi[6] = {0, 1, 2, 2, 1, 3};
        const uint64_t vb = sizeof qv, ib = sizeof qi;
        g_r.quad_buf = g_r.dev->createBuffer(plume::RenderBufferDesc::UploadBuffer(
            vb + ib, plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::INDEX));
        if (g_r.quad_buf) {
            uint8_t *p = (uint8_t *)g_r.quad_buf->map();
            std::memcpy(p, qv, vb);
            std::memcpy(p + vb, qi, ib);
            g_r.quad_buf->unmap();
        }
    }
    if (!g_r.shadow_mask.tex || g_r.shadow_mask.w != w || g_r.shadow_mask.h != h) {
        if (g_r.shadow_mask.tex) {
            Trash t{g_r.vb};
            t.tex = std::move(g_r.shadow_mask.tex);
            for (int i = 0; i < 16; i++) t.sets[i] = std::move(g_r.shadow_mask.sets[i]);
            g_r.trash.push_back(std::move(t));
        }
        if (g_r.shadow_fb) {
            Trash t{g_r.vb};
            t.fb = std::move(g_r.shadow_fb);
            g_r.trash.push_back(std::move(t));
            g_r.shadow_fb_depth = 0;
        }
        g_r.shadow_mask.tex = g_r.dev->createTexture(
            plume::RenderTextureDesc::ColorTarget(w, h, plume::RenderFormat::R8G8B8A8_UNORM));
        g_r.shadow_mask.w = w;
        g_r.shadow_mask.h = h;
    }
}

void flush_shadows() {
    if (g_r.shadow_pending.empty()) return;
    if (!g_r.has_main_target) {
        for (const Item &it : g_r.shadow_pending) {
            g_frames[it.p.frame].lost++;
            g_frames[it.p.frame].k_lost[KIND_SHADOW]++;
        }
        g_r.shadow_pending.clear();
        return;
    }
    const Target &tg = g_r.main_target;
    gs::Env e{};
    e.fbp = tg.fbp, e.fbw = tg.fbw, e.fpsm_mem = tg.fpsm, e.zbp = tg.zbp, e.zpsm = tg.zpsm, e.scy1 = (int)tg.rows - 1;
    gs::hw::NativeSurfaces s{};
    if (!gs::hw::native_begin(e, true, s) || !s.depth) {
        if (s.cmd) gs::hw::native_end();
        for (const Item &it : g_r.shadow_pending) {
            g_frames[it.p.frame].lost++;
            g_frames[it.p.frame].k_lost[KIND_SHADOW]++;
        }
        g_r.shadow_pending.clear();
        return;
    }
    // Stage R5 A3: the GS's own shadow target, w x h = min(view w, 320) x min(view h, 512) (0x35DA30,
    // 0x363218) times RenderScale. At 1x it is the PS2 target pixel for pixel; higher scales give the
    // adaptive finer target with the same mapping.
    const int sc = s.scale;
    const Packet &p0 = g_r.shadow_pending.front().p;
    int32_t rt_w = p0.shadow_rt[0], rt_h = p0.shadow_rt[1];
    int32_t vx = p0.shadow_rt[2], vy = p0.shadow_rt[3], vw = p0.shadow_rt[4], vh = p0.shadow_rt[5];
    if (rt_w <= 0 || rt_h <= 0 || vw <= 0 || vh <= 0 || rt_w > 4095 || rt_h > 4095) {
        rt_w = (int32_t)std::min<uint32_t>(s.gs_width, 320u), rt_h = (int32_t)std::min<uint32_t>(s.gs_height, 512u);
        vx = 0, vy = 0, vw = (int32_t)s.gs_width, vh = (int32_t)s.gs_height;
    }
    {
        static unsigned logged;
        if (logged++ < 8)
            std::fprintf(stderr, "[native] shadow target %dx%d view %d,%d %dx%d scale %d gs %ux%u items %zu frame %llu sc %d,%d-%d,%d\n", rt_w, rt_h,
                         vx, vy, vw, vh, sc, s.gs_width, s.gs_height, g_r.shadow_pending.size(),
                         (unsigned long long)p0.frame, g_r.main_sc[0], g_r.main_sc[1], g_r.main_sc[2], g_r.main_sc[3]);
    }
    ensure_shadow_resources((uint32_t)(rt_w * sc), (uint32_t)(rt_h * sc));
    if (!g_r.shadow_mask.tex || !g_r.quad_buf) {
        gs::hw::native_end();
        for (const Item &it : g_r.shadow_pending) {
            g_frames[it.p.frame].lost++;
            g_frames[it.p.frame].k_lost[KIND_SHADOW]++;
        }
        g_r.shadow_pending.clear();
        return;
    }
    g_frames[g_r.shadow_pending.front().p.frame].flushes++;
    auto *vl = static_cast<plume::VulkanCommandList *>(g_r.list.get());
    plume::RenderCommandList *cl = vl;
    const VkCommandBuffer own = vl->vk;
    vl->vk = (VkCommandBuffer)s.cmd;
    record_uploads(cl);

    plume::RenderTexture *color = wrapper(s.color, s.width, s.height, false);
    plume::RenderTexture *depth = wrapper(s.depth, s.width, s.height, true);
    plume::RenderTexture *smask = g_r.shadow_mask.tex.get();
    // the main depth, read by the volume pass as 0x362968's Z copy
    std::unique_ptr<plume::RenderDescriptorSet> zset = g_r.dev->createDescriptorSet(set_desc());
    zset->setTexture(0, depth, plume::RenderTextureLayout::SHADER_READ);
    zset->setSampler(1, g_r.samplers[2 | 4].get());
    const uint32_t geo = S_ON | (uint32_t)(sc & 63) << 24 | (uint32_t)(rt_h & 0xFFF) << 12 | (uint32_t)(rt_w & 0xFFF);
    const uint32_t view_xy = (uint32_t)(vx & 0xFFFF) | (uint32_t)(vy & 0xFFFF) << 16;
    const uint32_t view_wh = (uint32_t)(vw & 0xFFFF) | (uint32_t)(vh & 0xFFFF) << 16;

    // Pass 1: shadow volumes into the target (0x363708 counts, z test against the main depth)
    const uint32_t gpu_q = gpu_timer_begin((VkCommandBuffer)s.cmd, true, g_modern.load(std::memory_order_relaxed));
    uint32_t sh_draws = 0;
    plume::RenderTextureBarrier in1[2] = {plume::RenderTextureBarrier(smask, plume::RenderTextureLayout::COLOR_WRITE),
                                          plume::RenderTextureBarrier(depth, plume::RenderTextureLayout::SHADER_READ)};
    cl->barriers(plume::RenderBarrierStage::GRAPHICS, in1, 2);
    if (!g_r.shadow_fb) {
        const plume::RenderTexture *ca[1] = {smask};
        g_r.shadow_fb = g_r.dev->createFramebuffer(plume::RenderFramebufferDesc(ca, 1, nullptr));
        g_r.shadow_fb_depth = 0;
    }
    cl->setFramebuffer(g_r.shadow_fb.get());
    // main GS pixel x -> target pixel (x - vx) w / vw (0x363218 aspect and 0x35E200 viewport)
    const double kx = (double)rt_w / vw, ky = (double)rt_h / vh;
    const double gw = s.gs_width, gh = s.gs_height;
    cl->setViewports(plume::RenderViewport((float)(-vx * kx * sc), (float)(-vy * ky * sc), (float)(gw * kx * sc),
                                           (float)(gh * ky * sc)));
    cl->setScissors(plume::RenderRect(0, 0, rt_w * sc, rt_h * sc));
    cl->clearColor(0, plume::RenderColor(0, 0, 0, 0));  // 0x362968's Z write clears the target to 0
    cl->setGraphicsPipelineLayout(g_r.layout.get());
    cl->setGraphicsDescriptorSet(zset.get(), 0);
    uint32_t bound = ~0u;
    uint32_t intensity = 0;
    int32_t comp_sc[4] = {g_r.main_sc[0], g_r.main_sc[1], g_r.main_sc[2], g_r.main_sc[3]};
    for (Item &it : g_r.shadow_pending) {
        FrameStats &fs = g_frames[it.p.frame];
        intensity = it.p.pass;
        if (!it.mesh || !it.mesh->buf) {
            fs.drawn++;
            fs.k_drawn[KIND_SHADOW]++;
            fs.empty++;
            continue;
        }
        PushConstants &pc = it.pc;
        const double ox = (0.5 - it.ox) * 2.0 / gw - 1.0, oy = (0.5 - it.oy) * 2.0 / gh - 1.0;
        for (int c = 0; c < 4; c++) {
            pc.cx[c] = (float)(it.a[0][c] * 2.0 / gw + it.a[3][c] * ox);
            pc.cy[c] = (float)(it.a[1][c] * 2.0 / gh + it.a[3][c] * oy);
            pc.cz[c] = (float)(it.a[2][c] * it.zscale);
            pc.cw[c] = (float)it.a[3][c];
        }
        static const uint32_t test_variant = std::getenv("EOD_NATIVE_SHADOW_TEST") ? (uint32_t)std::atoi(std::getenv("EOD_NATIVE_SHADOW_TEST")) & 3u : 0u;
        pc.mode[0] = test_variant;  // diagnostic only (native_shaders.hlsl shadow_ps)
        pc.mode[1] = view_xy;
        pc.mode[2] = view_wh;
        pc.mode[3] = geo;
        if (it.pipeline != bound) {
            cl->setPipeline(pipeline(it.pipeline));
            bound = it.pipeline;
        }
        cl->setGraphicsPushConstants(0, &pc, 0, sizeof pc);
        const GpuMesh &m = *it.mesh;
        const plume::RenderVertexBufferView vbv(m.buf->at(0), (uint32_t)(m.cpu->vertices.size() * sizeof(Vertex)));
        const plume::RenderInputSlot slot(0, sizeof(Vertex));
        cl->setVertexBuffers(0, &vbv, 1, &slot);
        const plume::RenderIndexBufferView ibv(m.buf->at(m.index_offset), (uint32_t)(m.cpu->indices.size() * 4),
                                               plume::RenderFormat::R32_UINT);
        cl->setIndexBuffer(&ibv);
        cl->drawIndexedInstanced((uint32_t)m.cpu->indices.size(), 1, 0, 0, 0);
        sh_draws++;
        fs.drawn++;
        fs.k_drawn[KIND_SHADOW]++;
    }
    cl->setFramebuffer(nullptr);
    {
        Trash t{g_r.vb};
        t.sets[0] = std::move(zset);
        g_r.trash.push_back(std::move(t));
    }

    // Pass 2: composite onto the main colour (0x362E70)
    // diagnostic: EOD_NATIVE_SHADOW_OFF=1 no composite, =2 white mask (A/B against the replay's nortt mask)
    static const int shadow_off = std::getenv("EOD_NATIVE_SHADOW_OFF") ? std::atoi(std::getenv("EOD_NATIVE_SHADOW_OFF")) : 0;
    if (intensity > 0 && shadow_off != 1) {
        plume::RenderTextureBarrier in2[2] = {plume::RenderTextureBarrier(smask, plume::RenderTextureLayout::SHADER_READ),
                                              plume::RenderTextureBarrier(color, plume::RenderTextureLayout::COLOR_WRITE)};
        cl->barriers(plume::RenderBarrierStage::GRAPHICS, in2, 2);
        auto &cfb = g_r.framebuffers[{s.color, 0}];
        if (!cfb) {
            const plume::RenderTexture *ca[1] = {color};
            cfb = g_r.dev->createFramebuffer(plume::RenderFramebufferDesc(ca, 1, nullptr));
        }
        cl->setFramebuffer(cfb.get());
        cl->setViewports(plume::RenderViewport(0, 0, (float)s.width, (float)s.height));
        cl->setGraphicsPipelineLayout(g_r.layout.get());
        // no depth, RGB write mask (0x7), no cull
        cl->setPipeline(pipeline((shadow_off == 2 ? B_OPAQUE : B_SHADOW) | (0x7u << 8)));
        cl->setGraphicsDescriptorSet(texture_set(&g_r.shadow_mask, 2 | 4), 0);
        PushConstants cpc{};
        cpc.cx[0] = 1.0f; cpc.cy[1] = 1.0f; cpc.cz[2] = 1.0f; cpc.cw[3] = 1.0f;
        cpc.light[0] = (float)intensity;  // TEXA.TA0 = *0x4D8BF8
        cpc.mode[0] = shadow_off == 2 ? 1u : 0u;  // =2: shadow pixels painted white (diagnostic)
        cpc.mode[1] = view_xy;
        cpc.mode[2] = view_wh;
        cpc.mode[3] = geo | S_COMPOSITE;
        cl->setGraphicsPushConstants(0, &cpc, 0, sizeof cpc);
        // the strips cover the view (0x362E70: x from vx - 0.9375, 32 pixels each)
        const int32_t x0 = std::max({0, comp_sc[0], vx}) * sc, y0 = std::max({0, comp_sc[1], vy}) * sc;
        const int32_t x1 = std::min({(int32_t)gw, comp_sc[2] + 1, vx + vw}) * sc;
        const int32_t y1 = std::min({(int32_t)gh, comp_sc[3] + 1, vy + vh}) * sc;
        cl->setScissors(plume::RenderRect(x0, y0, std::max(x0, x1), std::max(y0, y1)));
        const plume::RenderVertexBufferView qvbv(g_r.quad_buf->at(0), (uint32_t)(4 * sizeof(Vertex)));
        const plume::RenderInputSlot qslot(0, sizeof(Vertex));
        cl->setVertexBuffers(0, &qvbv, 1, &qslot);
        const plume::RenderIndexBufferView qibv(g_r.quad_buf->at(4 * sizeof(Vertex)), 6 * 4, plume::RenderFormat::R32_UINT);
        cl->setIndexBuffer(&qibv);
        cl->drawIndexedInstanced(6, 1, 0, 0, 0);
        sh_draws++;
        cl->setFramebuffer(nullptr);
    }
    gpu_timer_end((VkCommandBuffer)s.cmd, gpu_q, sh_draws);
    plume::RenderTextureBarrier out[2] = {plume::RenderTextureBarrier(color, plume::RenderTextureLayout::GENERAL),
                                          plume::RenderTextureBarrier(depth, plume::RenderTextureLayout::GENERAL)};
    cl->barriers(plume::RenderBarrierStage::ALL, out, 2);
    vl->vk = own;
    gs::hw::native_end();
    g_r.shadow_pending.clear();
}

// ---- stage R6 at flush ----------------------------------------------------------------------
// Both looks: this frame's sun (the world light of a lit queue object: the VU1 vf12 source) and the
// sky colours (the texture the table tags `sky`), so a toggle has them at once.
void r6_observe(const Item &it) {
    if (it.p.kind == KIND_QUEUE && it.mesh && !it.mesh->cpu->vertices.empty() && it.mesh->cpu->vertices[0].n[3] != 0) {
        const double l = std::sqrt((double)it.p.light[0] * it.p.light[0] + (double)it.p.light[1] * it.p.light[1] +
                                   (double)it.p.light[2] * it.p.light[2]);
        if (l > 1e-6) {
            for (int c = 0; c < 3; c++) g_sun_world[c] = (float)(it.p.light[c] / l), g_sun_col[c] = it.p.colour_a[c];
        }
    }
    if (it.tex && it.tex->mat >= 0 && (it.tex->sky_top | it.tex->sky_hor)) {
        g_sky_top = it.tex->sky_top, g_sky_hor = it.tex->sky_hor;
        g_sky_seen = true;
    }
}

uint32_t oct_encode(double x, double y, double z) {
    const double s = std::fabs(x) + std::fabs(y) + std::fabs(z);
    if (s < 1e-12) return 0x7FF7FFu;  // (0, 0, 1)
    double u = x / s, v = y / s;
    if (z < 0) {
        const double nu = (1.0 - std::fabs(v)) * (u >= 0 ? 1 : -1), nv = (1.0 - std::fabs(u)) * (v >= 0 ? 1 : -1);
        u = nu, v = nv;
    }
    const uint32_t a = (uint32_t)std::lround((u * 0.5 + 0.5) * 4095.0), b = (uint32_t)std::lround((v * 0.5 + 0.5) * 4095.0);
    return a | b << 12;
}

// The Modern push constants of an item (native_modern.hlsl header): everything the game decides
// (blend, tests, alpha, fog, FIX) stays as the Classic item has it.
void modern_patch(const Item &it, const PushConstants &pc, PushConstants &out) {
    out = pc;
    if (pc.mode[0] & M_SPEC) {
        // the Classic highlight alpha (VU1 0x948 with the game's q17 half vector, guardrail rows 4/5):
        // light.w / diffuse.w / mode.w are reused for the sky below, so the half vector goes into
        // ambient.w bits 8..31 (octahedral 12 + 12) next to the ITOP alpha in bits 0..7
        float hz;
        std::memcpy(&hz, &pc.mode[3], 4);
        const uint32_t pk = ((uint32_t)pc.ambient[3] & 0xFFu) | oct_encode(pc.light[3], pc.diffuse[3], hz) << 8;
        std::memcpy(&out.ambient[3], &pk, 4);
    }
    const uint32_t kind = it.p.kind;
    const bool surface = kind == KIND_QUEUE || kind == KIND_WORLD || kind == KIND_WATER;
    uint32_t cls = C_UNLIT, metal = 0, rough = 12, em = 0, height = 0, rec = 0;
    if (it.tex && it.tex->mat >= 0) {
        const Material &m = g_mats[(size_t)it.tex->mat];
        if (m.cls >= C_VFX_FIRE && m.cls <= C_VFX_SPARK &&
            (kind == KIND_PARTICLE || kind == KIND_SPRITE || kind == KIND_IMMEDIATE || kind == KIND_QUEUE)) {
            // Modern-only GPU procedural fire, smoke & sparks (stage R6 A2)
            cls = m.cls;
            metal = (kind == KIND_SPRITE && it.p.pass >= 2 && it.p.pass <= 8) ? it.p.pass : 1u;  // atlas grid dim
            height = (kind == KIND_PARTICLE || kind == KIND_SPRITE) ? 1u : 0u;                   // radial quad mask
            // how the game's GS blend fades this card (stage R6 A5, guardrail 7): 1 by the source
            // alpha (Cs C + Cd (1 - C)), 2 by the source colour (Cd kept whole), 3 inverse (Cs (1 - C)
            // + Cd C, C -> 1); 0 no fade: an opaque, FIX or destination-alpha blend has no coverage
            // the shader can lower, so it is drawn exactly as Classic (masking it made the blocks)
            {
                const uint32_t key = it.pipeline, sf = (key >> 16) & 3, df = (key >> 18) & 3, op = (key >> 20) & 3;
                const bool c_src = !(key & K_CDST) && !(pc.mode[0] & M_CFIX);
                rough = 0;
                if (df == 1 && sf != 0 && (op == 0 || op == 2)) rough = 2;
                else if (c_src && op == 0 && sf == 2 && df == 3) rough = 1;
                else if (c_src && op == 0 && sf == 3 && df == 2) rough = 3;
                static const bool vlog = std::getenv("EOD_VFX_LOG") != nullptr;
                static std::unordered_set<uint64_t> seen;
                const uint64_t sig = (uint64_t)m.cls | (uint64_t)kind << 4 | (uint64_t)(sf | df << 2 | op << 4) << 8 |
                                     (uint64_t)((key & K_CDST) ? 1 : 0) << 14 | (uint64_t)((pc.mode[0] & M_CFIX) ? 1 : 0) << 15 |
                                     (uint64_t)((pc.mode[0] & M_ATE) ? 1 : 0) << 16 | (uint64_t)rough << 17;
                if (vlog && seen.insert(sig).second && seen.size() <= 64)
                    std::fprintf(stderr, "[r6] vfx class %u kind %u blend sf %u df %u op %u cdst %d cfix %d ate %d -> fade %u (record %d)\n",
                                 m.cls, kind, sf, df, op, (key & K_CDST) ? 1 : 0, (pc.mode[0] & M_CFIX) ? 1 : 0,
                                 (pc.mode[0] & M_ATE) ? 1 : 0, rough, it.tex->mat);
            }
            em = 1;
            rec = 1;
            out.mode[2] = (pc.mode[2] & 0xFFFFu) | metal << 16 | rough << 20 | em << 24 | height << 25 | rec << 26 | cls << 27;
            const float t_sec = (float)(g_r.vb % 60000ull) / 60.0f;
            std::memcpy(&out.mode[3], &t_sec, 4);
            if (!it.second) g_draws_vfx[cls - C_VFX_FIRE]++;
            return;
        }
    }
    if (surface && it.tex) {
        cls = C_GENERIC;
        if (it.tex->mat >= 0) {
            const Material &m = g_mats[(size_t)it.tex->mat];
            cls = m.cls, metal = m.metal4, rough = m.rough4, em = m.emissive, height = m.height, rec = 1;
        } else if (kind == KIND_WATER) {
            cls = C_WATER, rough = 2;
        }
    }
    out.mode[2] = (pc.mode[2] & 0xFFFFu) | metal << 16 | rough << 20 | em << 24 | height << 25 | rec << 26 | cls << 27;
    const float *M = it.p.model;  // row-major 3x4, model -> world
    double det = 0;
    for (int c = 0; c < 3; c++) det += std::fabs((double)M[c]) + std::fabs((double)M[4 + c]) + std::fabs((double)M[8 + c]);
    const bool has_m = det > 1e-9;
    const bool prelit = it.mesh && !it.mesh->cpu->vertices.empty() && it.mesh->cpu->vertices[0].n[3] == 0;
    // daylight (guardrail 4): the sky texture's mean is not the sky the game draws (night skies are
    // modulated down); the game's horizon is its FOGCOL. Sky light and the prelit sun are scaled by
    // FOGCOL / sky-horizon luminance (1 by day, where the fog matches the horizon).
    auto lum8 = [](uint32_t c) { return 0.2126 * (c & 0xFF) + 0.7152 * (c >> 8 & 0xFF) + 0.0722 * (c >> 16 & 0xFF); };
    const uint32_t fog = it.p.fogcol & 0xFFFFFF;
    uint32_t top = g_sky_top, hor = g_sky_hor;
    double day = 1.0;
    if (!g_sky_seen) {
        top = hor = fog;
    } else {
        day = std::min(1.0, lum8(fog) / std::max(1.0, lum8(hor)));
        auto scale = [day](uint32_t c) {
            uint32_t r = 0;
            for (int k = 0; k < 3; k++) r |= (uint32_t)((c >> (8 * k) & 0xFF) * day + 0.5) << (8 * k);
            return r;
        };
        top = scale(top), hor = scale(hor);
    }
    if (prelit) {
        for (int c = 0; c < 3; c++) {
            double l = 0;
            for (int r = 0; r < 3; r++) l += (double)(has_m ? M[r * 4 + c] : (r == c ? 1.0f : 0.0f)) * g_sun_world[r];
            out.light[c] = (float)l;
            out.diffuse[c] = (float)(g_sun_col[c] * day);
        }
    }
    std::memcpy(&out.light[3], &top, 4);
    std::memcpy(&out.diffuse[3], &hor, 4);
    // world up is -y: the game's sun (vf12 source, pointing to the light) has y < 0 by day (Dundas
    // day run: -0.857), so the GS world is y-down
    const double ux = has_m ? -M[4] : 0, uy = has_m ? -M[5] : -1, uz = has_m ? -M[6] : 0;  // M^T (0, -1, 0)
    out.mode[3] = oct_encode(ux, uy, uz);
}

// A low-confidence record's draw: its screen box (0..1 of the GS frame), for the in-context crops
// (cropped from that vblank's screenshot). At most 12 lines per record.
void r6_lowconf(const Item &it, double gw, double gh) {
    if (!it.tex || it.tex->mat < 0 || !g_mats[(size_t)it.tex->mat].low || !it.mesh) return;
    // on the vblanks a --screenshot-every EOD_R6_LOWCONF_EVERY run saves (default 60)
    static const uint64_t every = std::getenv("EOD_R6_LOWCONF_EVERY") ? std::strtoull(std::getenv("EOD_R6_LOWCONF_EVERY"), nullptr, 0) : 60;
    uint32_t &n = g_low_seen[it.tex->mat];
    if (n >= 12 || (every && g_r.vb % every)) return;
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (const Vertex &v : it.mesh->cpu->vertices) {
        double o[4];
        for (int r = 0; r < 4; r++) o[r] = it.a[r][0] * v.p[0] + it.a[r][1] * v.p[1] + it.a[r][2] * v.p[2] + it.a[r][3];
        if (o[3] <= 1e-6) continue;
        const double x = o[0] / o[3] - it.ox, y = o[1] / o[3] - it.oy;
        x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
    }
    x0 = std::max(0.0, x0), y0 = std::max(0.0, y0), x1 = std::min(gw, x1), y1 = std::min(gh, y1);
    if (x1 - x0 < 4 || y1 - y0 < 4) return;
    n++;
    const Material &m = g_mats[(size_t)it.tex->mat];
    // key: blend class (bits 0..3) / generic blend; mode.x: TCC, TFX, ATE, ATST (the game's render state:
    // the in-context evidence tools/material_lowconf.py reads)
    std::fprintf(stderr, "[r6] lowconf %s %s conf %.2f vb %llu box %.4f %.4f %.4f %.4f key %08x mx %08x\n", m.id.c_str(),
                 m.surface.c_str(), m.conf, (unsigned long long)g_r.vb, x0 / gw, y0 / gh, x1 / gw, y1 / gh, it.pipeline,
                 it.pc.mode[0]);
}

void flush() {
    if (!g_r.shadow_pending.empty()) flush_shadows();
    if (g_r.pending.empty()) return;
    const Target &tg = g_r.target;
    gs::Env e{};
    e.fbp = tg.fbp, e.fbw = tg.fbw, e.fpsm_mem = tg.fpsm, e.zbp = tg.zbp, e.zpsm = tg.zpsm, e.scy1 = (int)tg.rows - 1;
    gs::hw::NativeSurfaces s{};
    if (!gs::hw::native_begin(e, tg.depth, s)) {
        for (const Item &it : g_r.pending) g_frames[it.p.frame].lost++;
        g_r.pending.clear();
        return;
    }
    g_frames[g_r.pending.front().p.frame].flushes++;
    auto *vl = static_cast<plume::VulkanCommandList *>(g_r.list.get());
    plume::RenderCommandList *cl = vl;
    const VkCommandBuffer own = vl->vk;
    vl->vk = (VkCommandBuffer)s.cmd;
    record_uploads(cl);
    plume::RenderTexture *color = wrapper(s.color, s.width, s.height, false);
    plume::RenderTexture *depth = s.depth ? wrapper(s.depth, s.width, s.height, true) : nullptr;
    plume::RenderTextureBarrier in[2] = {plume::RenderTextureBarrier(color, plume::RenderTextureLayout::COLOR_WRITE),
                                         plume::RenderTextureBarrier(depth, plume::RenderTextureLayout::DEPTH_WRITE)};
    cl->barriers(plume::RenderBarrierStage::GRAPHICS, in, depth ? 2 : 1);
    const bool modern = g_modern.load(std::memory_order_relaxed);  // stage R6: one look per flush
    const uint32_t gpu_q = gpu_timer_begin((VkCommandBuffer)s.cmd, false, modern);
    uint32_t pass_draws = 0;
    auto &fb = g_r.framebuffers[{s.color, s.depth}];
    if (!fb) {
        const plume::RenderTexture *ca[1] = {color};
        fb = g_r.dev->createFramebuffer(plume::RenderFramebufferDesc(ca, 1, depth));
    }
    cl->setFramebuffer(fb.get());
    cl->setViewports(plume::RenderViewport(0, 0, (float)s.width, (float)s.height));
    if (native_only() && native_only_level() == 1) {
        auto cit = g_only_clears.find(tg.fbp);
        if (cit != g_only_clears.end()) {
            cl->setScissors(plume::RenderRect(0, 0, (int32_t)s.width, (int32_t)s.height));
            if (cit->second.color)
                cl->clearColor(0, plume::RenderColor(cit->second.r, cit->second.g, cit->second.b, cit->second.a));
            if (depth && cit->second.depth)
                cl->clearDepth(true, cit->second.z);
            g_only_clears.erase(cit);
        }
    }
    cl->setGraphicsPipelineLayout(g_r.layout.get());
    const double gw = s.gs_width, gh = s.gs_height;
    const int sc = s.scale;
    uint32_t bound = ~0u;
    PushConstants mpc;
    for (Item &it : g_r.pending) {
        FrameStats &fs = g_frames[it.p.frame];
        if (!it.mesh || !it.mesh->buf) {
            if (it.second) continue;
            fs.drawn++;
            fs.k_drawn[it.p.kind % KIND_COUNT]++;
            fs.empty++;
            continue;
        }
        if (!tg.depth) it.pipeline &= ~0xF0u;
        // GS screen -> this surface's clip space (the backend's convention: ndc = (p + 0.5) * 2 / size - 1)
        PushConstants &pc = it.pc;
        const double kx = (0.5 - it.ox) * 2.0 / gw - 1.0, ky = (0.5 - it.oy) * 2.0 / gh - 1.0;
        // clip = screen * w_scale (a uniform scale draws the same pixels): clip w is the VU1 w, 1 / GS Q
        const double ws = it.p.w_scale > 0 ? (double)it.p.w_scale : 1.0;
        for (int c = 0; c < 4; c++) {
            pc.cx[c] = (float)((it.a[0][c] * 2.0 / gw + it.a[3][c] * kx) * ws);
            pc.cy[c] = (float)((it.a[1][c] * 2.0 / gh + it.a[3][c] * ky) * ws);
            pc.cz[c] = (float)(it.a[2][c] * it.zscale * ws);
            pc.cw[c] = (float)(it.a[3][c] * ws);
        }
        r6_observe(it);
        if (!it.second) {
            g_draws_all++;
            if (it.tex) g_draws_textured++;
            if (it.tex && it.tex->mat >= 0) g_draws_material++;
            r6_lowconf(it, gw, gh);
        }
        const uint32_t key = modern ? it.pipeline | K_MODERN : it.pipeline;
        if (key != bound) {
            cl->setPipeline(pipeline(key));
            bound = key;
        }
        Texture *t = it.tex ? it.tex : &g_r.white;
        uint32_t samp = it.tex ? it.sampler : 0;
        if (modern) {
            const uint32_t mx = pc.mode[0];
            samp = 8 | (((mx >> M_WMS_SHIFT) & 3) ? 2u : 0u) | (((mx >> M_WMT_SHIFT) & 3) ? 4u : 0u);
            modern_patch(it, pc, mpc);
        }
        cl->setGraphicsDescriptorSet(texture_set(t, samp), 0);
        cl->setGraphicsPushConstants(0, modern ? &mpc : &pc, 0, sizeof pc);
        const int32_t x0 = std::max(0, it.sc[0]) * sc, y0 = std::max(0, it.sc[1]) * sc;
        const int32_t x1 = std::min((int32_t)gw, it.sc[2] + 1) * sc, y1 = std::min((int32_t)gh, it.sc[3] + 1) * sc;
        cl->setScissors(plume::RenderRect(x0, y0, std::max(x0, x1), std::max(y0, y1)));
        const GpuMesh &m = *it.mesh;
        const plume::RenderVertexBufferView vbv(m.buf->at(0), (uint32_t)(m.cpu->vertices.size() * sizeof(Vertex)));
        const plume::RenderInputSlot slot(0, sizeof(Vertex));
        cl->setVertexBuffers(0, &vbv, 1, &slot);
        const plume::RenderIndexBufferView ibv(m.buf->at(m.index_offset), (uint32_t)(m.cpu->indices.size() * 4),
                                               plume::RenderFormat::R32_UINT);
        cl->setIndexBuffer(&ibv);
        static const bool no_runs = std::getenv("EOD_R5A2_OFF") && (std::strtoul(std::getenv("EOD_R5A2_OFF"), nullptr, 0) & 8);
        if (!modern && !no_runs && !it.run_k.empty() && it.run_k.size() == m.cpu->k_runs.size()) {
            // one draw per VU1 run: each has its own TEX1.K
            for (size_t r = 0; r < it.run_k.size(); r++) {
                const Mesh::KRun &kr = m.cpu->k_runs[r];
                if (kr.first + kr.count > m.cpu->indices.size()) break;
                pc.mode[2] = (pc.mode[2] & ~(0xFFFu << Z_K_SHIFT)) | ((uint32_t)it.run_k[r] & 0xFFF) << Z_K_SHIFT;
                cl->setGraphicsPushConstants(0, &pc, 0, sizeof pc);
                cl->drawIndexedInstanced(kr.count, 1, kr.first, 0, 0);
                pass_draws++;
            }
        } else {
            cl->drawIndexedInstanced((uint32_t)m.cpu->indices.size(), 1, 0, 0, 0);
            pass_draws++;
        }
        if (it.second) continue;
        fs.drawn++;
        fs.k_drawn[it.p.kind % KIND_COUNT]++;
        if (it.p.kind == KIND_WATER) water_view_drawn(m.cpu->indices.size() / 3, it.sc, it.ox, it.oy);
        if (it.tex) fs.textured++;
        if (m.cpu->skinned) fs.skinned++;
        if (m.cpu->posed) fs.posed++;
    }
    cl->setFramebuffer(nullptr);  // ends the render pass
    gpu_timer_end((VkCommandBuffer)s.cmd, gpu_q, pass_draws);
    plume::RenderTextureBarrier out[2] = {plume::RenderTextureBarrier(color, plume::RenderTextureLayout::GENERAL),
                                          plume::RenderTextureBarrier(depth, plume::RenderTextureLayout::GENERAL)};
    cl->barriers(plume::RenderBarrierStage::ALL, out, depth ? 2 : 1);
    vl->vk = own;
    gs::hw::native_end();
    g_r.pending.clear();
}

// GS ALPHA (A - B) * C + D, A/B/D in {Cs, Cd, 0} and C in {As, Ad, FIX}, as fixed-function blending
// (pipeline key K_GENERIC): each colour's coefficient is k + j*C with k in {0, 1}, j in {-1, 0, 1}.
// Coefficients 1 + C cannot be expressed (counted, drawn as 1); two negative coefficients give 0
// (COLCLAMP = 1 clamps the GS result the same way).
std::atomic<uint64_t> g_blend_approx{0};
uint32_t blend_classes(bool abe, int a, int b, int c, int d) {
    if (!abe) return K_GENERIC | 1u << 16;  // src * 1 + dst * 0
    enum { ZERO, ONE, C, INV_C, NEG_C, ONE_PLUS_C };
    auto cls = [](int k, int j) {
        if (j == 0) return k ? ONE : ZERO;
        if (j > 0) return k ? ONE_PLUS_C : C;
        return k ? INV_C : NEG_C;
    };
    const int src = cls(d == 0, (a == 0) - (b == 0)), dst = cls(d == 1, (a == 1) - (b == 1));
    uint32_t sf, df, op = 0;
    auto f = [&](int x) -> uint32_t {
        if (x == ONE_PLUS_C) {
            g_blend_approx.fetch_add(1);
            return 1;
        }
        return x == ZERO ? 0 : x == ONE ? 1 : x == INV_C ? 3 : 2;
    };
    if (src == NEG_C && dst == NEG_C) sf = 0, df = 0;
    else if (src == NEG_C) sf = 2, df = f(dst), op = 2;  // dst * F - src * C
    else if (dst == NEG_C) sf = f(src), df = 2, op = 1;  // src * F - dst * C
    else sf = f(src), df = f(dst);
    return K_GENERIC | sf << 16 | df << 18 | op << 20 | (c == 1 ? K_CDST : 0u);
}

uint32_t blend_of(const gs::Env &e) {
    if (!e.abe) return B_OPAQUE;
    const int a = e.ba, b = e.bb, d = e.bd;  // 0 Cs, 1 Cd, 2 zero
    if (a == b) return d == 0 ? B_OPAQUE : d == 1 ? B_KEEP : B_OPAQUE;
    if (a == 0 && b == 1 && d == 1) return B_MIX;
    if (a == 0 && b == 2 && d == 1) return B_ADD;
    if (a == 2 && b == 0 && d == 1) return B_REVSUB;
    if (a == 0 && b == 2 && d == 2) return B_SCALE;
    if (a == 1 && b == 2 && d == 0) return B_DST_ADD;
    if (a == 1 && b == 0 && d == 0) return B_INV_MIX;
    return B_OPAQUE;
}

// The GS state RenderStateCommit 0x3620A8 builds from the EE state each packet carries (the
// decompiled setters' values, docs/NATIVE_RENDERER.md "Stage R5" A2 table). Frame and depth buffer
// surfaces and the scissor stay those of the object's first GS draw (stage R1).
struct EeState {
    bool ate = false;
    int atst = 1, afail = 0;
    uint32_t aref = 0;
    int ztst = 1;
    bool zwrite = false;
    bool abe = false;
    int ba = 0, bb = 1, bc = 0, bd = 1;
    uint32_t fix = 0;
    int wms = 0, wmt = 0;
    int32_t k = 0;   // TEX1.K, 1/16 (12-bit signed)
    uint32_t mxl = 0;
    bool mmag = true;
    int mmin = 4;
    bool k_known = true;  // false: the batch does not kick TEX1 and no earlier object set it
};

int32_t sext12(int32_t v) { return (int32_t)((uint32_t)v << 20) >> 20; }

EeState g_last_tex1;  // the TEX1 the last kick left in the GS (a batch with z > 0 does not kick)
bool g_have_tex1 = false;

EeState ee_state(const Packet &p, const Mesh *cpu) {
    EeState r;
    const uint32_t f = p.flags;
    // 0x3620A8 dirty 0x30: CLAMP_1 = !(flags & 0x10) | !(flags & 0x20) << 2 (REPEAT / CLAMP, bounds 0)
    r.wms = ((f >> 4) ^ 1) & 1;
    r.wmt = ((f >> 5) ^ 1) & 1;
    // dirty 0x241: TEST_1 = ztst << 17 | (flags & 0x200 ? (flags & 1 ? 0x1160B : 0x10000) : 0x12001)
    r.ztst = (f & 0x40) ? 2 : 1;
    if (!(f & 0x200)) r.ate = true, r.atst = 0, r.aref = 0, r.afail = 2;   // 0x12001: depth only
    else if (f & 1) r.ate = true, r.atst = 5, r.aref = 0x60, r.afail = 1;  // 0x1160B
    // dirty 0x80: ZBUF_1.ZMSK = !(flags & 0x80)
    r.zwrite = (f & 0x80) != 0;
    // PRIM.ABE = flags & 1 (GIFtag templates, dirty 0x109)
    r.abe = (f & 1) != 0;
    // dirty 0x80000: ALPHA_1 = sel ? 0x44 (Cs, Cd, As, Cd) : 0x48 (Cs, 0, As, Cd), FIX 0
    r.ba = 0, r.bb = p.blend_sel ? 1 : 2, r.bc = 0, r.bd = 1, r.fix = 0;
    // dirty 0x400: TEX1 = (K & 0xFFF) << 32 | 0x120 | (MXL & 7) << 2 -> VU1 qwords 20-22
    // (MMAG 1, MMIN 4 = LINEAR_MIPMAP_NEAREST, L 0, LCM 0); the batch program adds its delta
    r.mmag = true;
    r.mmin = 4;
    const uint32_t kind = p.kind % KIND_COUNT;
    if (kind == KIND_QUEUE || kind == KIND_WORLD) {
        if (cpu && cpu->k_kick) {
            r.k = sext12((p.tex_k & 0xFFF) + cpu->k_delta);
            r.mxl = p.tex_mxl & 7;
        } else if (g_have_tex1) {  // no kick: the GS keeps the last TEX1
            r.k = g_last_tex1.k;
            r.mxl = g_last_tex1.mxl;
        } else {
            r.k_known = false;
        }
        if (r.k_known) {
            g_last_tex1 = r;
            g_have_tex1 = true;
        }
        if (cpu && cpu->k_mixed) {
            g_k_mixed.fetch_add(1);
            // each run kicks its own TEX1 (or keeps the one before): the GS state after the object
            for (const Mesh::KRun &kr : cpu->k_runs)
                if (kr.kick) {
                    g_last_tex1.k = sext12((p.tex_k & 0xFFF) + kr.delta);
                    g_last_tex1.mxl = p.tex_mxl & 7;
                    g_have_tex1 = true;
                }
        }
    } else {
        // the particle / sprite / immediate headers carry *0x4D8BB8 = (int)(bias * 16 - 1000)
        // (0x364068, 0x364328, 0x3645B8, 0x361468, 0x361708, 0x364D18). Immediate geometry and
        // flares: K = template + word + 1000 (the batch programs' encoding, i.e. + bias * 16);
        // particles and sprites: K = template + word (no + 1000, so their level is always 0). Both
        // equal the GS TEX1 in every draw measured (docs/NATIVE_RENDERER.md "Stage R5" A2).
        const int32_t word = (int16_t)(uint16_t)(p.bias_k & 0xFFFF);
        r.k = sext12((p.tex_k & 0xFFF) + word + (kind == KIND_PARTICLE || kind == KIND_SPRITE ? 0 : 1000));
        r.mxl = p.tex_mxl & 7;
    }
    return r;
}

// EE state against the GS registers of the object's first draw (diagnostic only: nothing drawn
// depends on it). Mismatches per kind: test, blend, clamp, z, K, MXL.
uint64_t g_st_n[KIND_COUNT], g_st_mis[KIND_COUNT][6];
std::mutex g_st_mutex;
void compare_state(const EeState &r, const gs::Env &e, uint32_t kind) {
    std::lock_guard<std::mutex> lk(g_st_mutex);
    g_st_n[kind]++;
    const bool ate = e.ate && e.atst != 1;
    if (ate != (r.ate && r.atst != 1) || (ate && (e.atst != r.atst || e.aref != r.aref || e.afail != r.afail)))
        g_st_mis[kind][0]++;
    if (e.abe != r.abe || (e.abe && (e.ba != r.ba || e.bb != r.bb || e.bc != r.bc || e.bd != r.bd)))
        g_st_mis[kind][1]++;
    if (e.tme && (e.wms != r.wms || e.wmt != r.wmt)) g_st_mis[kind][2]++;
    if (e.ztst != r.ztst || (!e.zmsk) != r.zwrite) g_st_mis[kind][3]++;
    if (e.tme && e.mmin >= 2 && (int32_t)std::lround(e.k * 16.0f) != r.k) {
        if (g_st_mis[kind][4]++ < 3)
            std::fprintf(stderr, "[native] K mismatch kind %u: GS %d EE %d\n", kind, (int)std::lround(e.k * 16.0f), r.k);
    }
    if (e.tme && (e.mmin != r.mmin || e.bilinear_mag != r.mmag || (uint32_t)e.mxl != r.mxl)) g_st_mis[kind][5]++;
}

void state_report() {
    static const char *names[KIND_COUNT] = {"queue", "world", "water", "shadow", "flare", "particle", "sprite", "immediate", "cockpit"};
    std::lock_guard<std::mutex> lk(g_st_mutex);
    for (uint32_t k = 0; k < KIND_COUNT; k++) {
        if (!g_st_n[k]) continue;
        std::fprintf(stderr, "[native] EE state vs GS %s: %llu objects, mismatches test %llu blend %llu clamp %llu z %llu K %llu MXL/filter %llu\n",
                     names[k], (unsigned long long)g_st_n[k], (unsigned long long)g_st_mis[k][0], (unsigned long long)g_st_mis[k][1],
                     (unsigned long long)g_st_mis[k][2], (unsigned long long)g_st_mis[k][3], (unsigned long long)g_st_mis[k][4],
                     (unsigned long long)g_st_mis[k][5]);
    }
    std::fprintf(stderr, "[native] run K check: %llu objects match the GS K sequence, %llu do not\n",
                 (unsigned long long)g_runk_ok, (unsigned long long)g_runk_bad);
    std::fprintf(stderr, "[native] R5 A2: mip textures %llu (MXL clamped %llu, level in rendered pages %llu), alpha-fail second passes %llu, K mixed batches %llu, blend 1+C approximated %llu, Ad blends %llu\n",
                 (unsigned long long)g_mip_textures.load(), (unsigned long long)g_mip_clamped.load(), (unsigned long long)g_mip_rt.load(),
                 (unsigned long long)g_afail_second.load(), (unsigned long long)g_k_mixed.load(),
                 (unsigned long long)g_blend_approx.load(), (unsigned long long)g_blend_ad.load());
}

// EOD_NATIVE_QCHECK=1 (diagnostic): the GS Q of an object's first textured vertex against 1 / w of
// the native vertex nearest to it on screen (the LOD uses log2(1 / Q)); printed at exit.
std::vector<double> g_qratio;
std::vector<int32_t> g_gs_k;  // distinct consecutive GS TEX1.K values of the current object's draws
// the GS K sequence is a subsequence of the per-run K the native object uses
void run_k_check(const std::vector<int32_t> &run_k) {
    size_t j = 0;
    for (size_t i = 0; i < run_k.size() && j < g_gs_k.size(); i++)
        if (run_k[i] == g_gs_k[j]) j++;
    if (j == g_gs_k.size()) {
        g_runk_ok++;
        return;
    }
    if (g_runk_bad++ < 4) {
        std::string a, b;
        for (int32_t k : run_k) a += " " + std::to_string(k);
        for (int32_t k : g_gs_k) b += " " + std::to_string(k);
        std::fprintf(stderr, "[native] run K mismatch: native%s | GS%s\n", a.c_str(), b.c_str());
    }
}
void q_check(const gs::Env &e, const gs::Vertex &v) {
    static const bool on = std::getenv("EOD_NATIVE_QCHECK") != nullptr;
    if (!on || !e.tme || e.fst || g_qratio.size() >= 20000) return;
    if (g_cur.kind != KIND_QUEUE && g_cur.kind != KIND_WORLD) return;
    auto it = g_r.meshes.find(g_cur.mesh);
    if (it == g_r.meshes.end() || !it->second.cpu || !(v.q > 0)) return;
    static const bool hooked = (diff_add_exit_report([] {
        if (g_qratio.empty()) return;
        std::sort(g_qratio.begin(), g_qratio.end());
        const size_t n = g_qratio.size();
        std::fprintf(stderr, "[native] Q check: %zu vertices, (1/Q) / (w w_scale) p5 %.4f median %.4f p95 %.4f\n", n,
                     g_qratio[n / 20], g_qratio[n / 2], g_qratio[n - 1 - n / 20]);
    }), true);
    (void)hooked;
    double a[4][4];
    screen_matrix(g_cur, a);
    const double xo = (double)(g_xyoffset & 0xFFFF), yo = (double)((g_xyoffset >> 32) & 0xFFFF);
    double best = 1e30, bw = 0;
    for (const Vertex &mv : it->second.cpu->vertices) {
        double sc[4];
        for (int r = 0; r < 4; r++) sc[r] = a[r][0] * mv.p[0] + a[r][1] * mv.p[1] + a[r][2] * mv.p[2] + a[r][3];
        if (!(sc[3] > 0)) continue;
        const double dx = sc[0] / sc[3] * 16.0 - xo - v.x, dy = sc[1] / sc[3] * 16.0 - yo - v.y;
        const double d = dx * dx + dy * dy;
        if (d < best) best = d, bw = sc[3];
    }
    if (best < 4.0 && bw > 0) {
        g_qratio.push_back((1.0 / v.q) / (bw * (double)g_cur.w_scale));
        static int shown = 0;
        if (shown++ < 4)
            std::fprintf(stderr, "[native] Q check sample: ratio %.7f near %.5f far %.3f fov %.6f vp %.1f x %.1f aspect %.5f fw %.4f\n",
                         (1.0 / v.q) / bw, g_cur.near_z, g_cur.far_z, g_cur.fov_scale, g_cur.viewport[2], g_cur.viewport[3],
                         g_cur.aspect, g_cur.fov_scale * g_cur.viewport[2] * 0.5);
    }
}

// The current object is complete: queue it with the GS state it had.
void finalize() {
    if (!g_active || g_finalized) return;
    g_finalized = true;
    FrameStats &fs = g_frames[g_cur.frame];
    const uint32_t kind = g_cur.kind % KIND_COUNT;
    if (gs::g_skip_frame) {
        fs.skipped++;
        fs.k_skipped[kind]++;
        return;
    }
    if (!init()) {
        fs.lost++;
        fs.k_lost[kind]++;
        return;
    }
    if (modern::g_on && modern::live()) {  // the Modern renderer draws these batches itself
        fs.lost++;
        fs.k_lost[kind]++;
        return;
    }
    const gs::Env &e = g_have_env ? g_env : gs::current_env();
    const uint64_t xy = g_have_env ? g_xyoffset : context_xyoffset();
    GpuMesh *mesh = g_cur.mesh ? gpu_mesh(g_cur.mesh) : nullptr;
    if (g_cur.mesh && !mesh) {
        fs.lost++;
        fs.k_lost[kind]++;
        return;
    }
    if (kind == KIND_SHADOW) {
        // Flush any pending opaque world/queue draws so s.depth is populated before shadow volumes
        if (!g_r.pending.empty()) flush();
        if (!g_r.has_main_target) {
            fs.lost++;
            fs.k_lost[KIND_SHADOW]++;
            return;
        }
        Item it{};
        it.p = g_cur;
        it.mesh = mesh;
        screen_matrix(g_cur, it.a);
        it.ox = (float)(g_r.main_xy & 0xFFFF) / 16.0f;
        it.oy = (float)((g_r.main_xy >> 32) & 0xFFFF) / 16.0f;
        it.zscale = (float)(1.0 / depth_range(g_r.main_target.zpsm));
        // pass 1: one 5-bit step added, pass 0: one step subtracted (0x363708). The depth test is
        // the shader's (0x362968 Z copy, GEQUAL on the complement).
        const bool front = (g_cur.itop & 1u) != 0;
        const uint32_t blend = front ? B_ADD : B_REVSUB;
        // the decoder's shadow strips are wound so that culling BACK keeps one side and FRONT the
        // other (EOD_NATIVE_SHADOW_TEST=1, no depth test: the two passes cancel to 55 px of 307,200)
        const uint32_t cull = front ? 1u : 2u;
        // no depth attachment, color mask = 0xF, blend_alpha = 1
        // the GS clips nothing in z (VU1 guard-band clip only): the extruded far side stays (K_NOCLIP)
        it.pipeline = blend | (0xFu << 8) | (cull << 12) | (1u << 14) | K_NOCLIP;
        for (int i = 0; i < 4; i++) it.sc[i] = g_r.main_sc[i];
        g_r.shadow_pending.push_back(it);
        return;
    }
    if (!g_r.shadow_pending.empty()) flush_shadows();
    static const bool state_hooked = (diff_add_exit_report(state_report), true);
    (void)state_hooked;
    const EeState rs = ee_state(g_cur, mesh ? mesh->cpu.get() : nullptr);
    if (mesh && mesh->cpu->k_mixed && !mesh->cpu->k_runs.empty() && !g_gs_k.empty()) {
        std::vector<int32_t> rk;
        int32_t k = rs.k;
        for (const Mesh::KRun &kr : mesh->cpu->k_runs) {
            if (kr.kick) k = sext12((g_cur.tex_k & 0xFFF) + kr.delta);
            rk.push_back(k);
        }
        run_k_check(rk);
    }
    compare_state(rs, e, kind);
    const bool zwrite = rs.zwrite, ztest = rs.ztst >= 2;
    Target tg{e.fbp, e.fbw, e.fpsm_mem, e.zbp, e.zpsm, (uint32_t)e.scy1 + 1, zwrite || ztest};
    if (!g_r.pending.empty() && !(tg == g_r.target)) flush();
    g_r.target = tg;
    if (tg.depth) {
        g_r.main_target = tg;
        g_r.has_main_target = true;
        g_r.main_xy = xy;
        g_r.main_sc[0] = e.scx0; g_r.main_sc[1] = e.scy0; g_r.main_sc[2] = e.scx1; g_r.main_sc[3] = e.scy1;
    }
    Item it{};
    it.p = g_cur;
    it.mesh = mesh;
    screen_matrix(g_cur, it.a);
    it.ox = (float)(xy & 0xFFFF) / 16.0f;
    it.oy = (float)((xy >> 32) & 0xFFFF) / 16.0f;
    it.zscale = (float)(1.0 / depth_range(e.zpsm));
    PushConstants &pc = it.pc;
    // model-space light (VU1 vf12): the model axes . light, times light.w
    for (int c = 0; c < 3; c++) {
        double l = 0;
        for (int r = 0; r < 3; r++) l += (double)g_cur.model[r * 4 + c] * g_cur.light[r];
        pc.light[c] = (float)(l * g_cur.light[3]);
    }
    std::memcpy(pc.diffuse, g_cur.colour_a, 16);
    std::memcpy(pc.ambient, g_cur.colour_b, 16);
    pc.ambient[3] = (float)(g_cur.itop & 0xFF);
    // q17 (setup 0x0000): h = -normalize(M^T (camera forward + light)), the car highlight's half vector
    {
        double h[3], len = 0;
        for (int c = 0; c < 3; c++) {
            h[c] = 0;
            for (int r = 0; r < 3; r++) h[c] += (double)g_cur.model[r * 4 + c] * ((double)g_cur.rotation[6 + r] + g_cur.light[r]);
            len += h[c] * h[c];
        }
        const double k = len > 0 ? -1.0 / std::sqrt(len) : 0.0;
        pc.light[3] = (float)(h[0] * k);
        pc.diffuse[3] = (float)(h[1] * k);
        const float hz = (float)(h[2] * k);
        std::memcpy(&pc.mode[3], &hz, 4);
    }
    static const uint32_t ab = std::getenv("EOD_R5A2_OFF") ? (uint32_t)std::strtoul(std::getenv("EOD_R5A2_OFF"), nullptr, 0) : 0u;
    it.tex = texture(e, fs, rs.mmin >= 2 && !(ab & 1) ? rs.mxl : 0);
    uint32_t m = 0;
    if (it.tex) {
        // TEX0 TCC/TFX as the texture manager's TEX0 (0x35F9A0) left them; size from the decoded texture
        m |= M_TME | (e.tcc ? M_TCC : 0) | (uint32_t)(e.tfx & 3) << M_TFX_SHIFT;
        uint32_t lw = 0, lh = 0;
        while ((1u << lw) < it.tex->w) lw++;
        while ((1u << lh) < it.tex->h) lh++;
        m |= lw << M_TW_SHIFT | lh << M_TH_SHIFT | (it.tex->levels - 1) << M_MXL_SHIFT | (rs.mmag ? M_MMAG : 0) |
             (uint32_t)rs.mmin << M_MMIN_SHIFT | (uint32_t)rs.wms << M_WMS_SHIFT | (uint32_t)rs.wmt << M_WMT_SHIFT;
        it.sampler = 0;  // the shader addresses and filters the texels itself (GS bilinear, 4-bit weights)
        if (ab & 4) it.sampler = (e.bilinear_mag ? 1u : 0u) | (e.wms != 0 ? 2u : 0u) | (e.wmt != 0 ? 4u : 0u);
        if (it.tex->levels > 1) fs.mipmapped++;
    }
    if (rs.ate) m |= M_ATE | (uint32_t)(rs.atst & 7) << M_ATST_SHIFT | M_AFAIL_DISCARD;
    if (rs.bc == 2) m |= M_CFIX;
    if (g_cur.program == 0x2E0) m |= M_SPEC;  // car bodies: vertex alpha is the highlight
    pc.mode[1] = rs.aref & 0xFF;
    pc.mode[2] = (rs.fix & 0xFF) | ((uint32_t)rs.k & 0xFFF) << Z_K_SHIFT | ((ab & 4) ? 0x80000000u : 0u);
    if (it.tex && it.tex->levels > 1 && mesh && mesh->cpu->k_mixed && !mesh->cpu->k_runs.empty() &&
        (kind == KIND_QUEUE || kind == KIND_WORLD)) {
        int32_t k = rs.k;
        for (const Mesh::KRun &kr : mesh->cpu->k_runs) {
            if (kr.kick) k = sext12((g_cur.tex_k & 0xFFF) + kr.delta);
            it.run_k.push_back(k);
        }
    }
    if (g_cur.fge) {
        // GS PRIM.FGE from the render flags (0x365540 -> 0x4D8BC8 bit 3). No retail path sets it
        // (docs/NATIVE_RENDERER.md "Stage R5"), and VU1's XYZF2 w word is not a fog coefficient, so
        // there is no F to pass: count the draw and leave it unfogged. FOGCOL (0x365428, state
        // 0x4D8B80) rides in mode.y bits 8..31 for the shader's M_FGE stage.
        pc.mode[1] |= (g_cur.fogcol & 0xFFFFFF) << 8;
        if (g_fge_draws.fetch_add(1) == 0)
            std::fprintf(stderr, "[native] fog: FGE draw (batch %08x program %04x): per-vertex F not decoded\n",
                         g_cur.batch, g_cur.program);
    }
    pc.mode[0] = m;
    // mode.w is the half vector's z only for M_SPEC; otherwise its bit 31 is the shader's shadow
    // flag (S_ON), so a negative z would send an ordinary draw to shadow_ps
    if (!(m & M_SPEC)) pc.mode[3] = 0;
    // pipeline key: blend | z function << 4 | z write << 6 | depth << 7 | colour mask << 8
    const uint32_t zf = !ztest ? (e.ztst == 0 ? 3u : 0u) : e.ztst == 2 ? 1u : 2u;
    const uint32_t full = (e.fpsm & 0xF) == 0x1 ? 0xFFFFFFu : 0xFFFFFFFFu;
    uint32_t mask = 0;
    for (int ch = 0; ch < 4; ch++) {
        const uint32_t bits = 0xFFu << (ch * 8);
        if ((full & bits) && (e.fbmsk & bits) != bits) mask |= 1u << ch;
    }
    const uint32_t blend = blend_classes(rs.abe, rs.ba, rs.bb, rs.bc, rs.bd);
    if (rs.bc == 1) g_blend_ad.fetch_add(1);
    it.pipeline = blend | zf << 4 | (zwrite ? 1u : 0u) << 6 | (tg.depth ? 1u : 0u) << 7 | mask << 8;
    it.sc[0] = e.scx0, it.sc[1] = e.scy0, it.sc[2] = e.scx1, it.sc[3] = e.scy1;
    // Alpha test (TEST.ATE/ATST/AREF/AFAIL): the pixels that pass are drawn as the state says; the
    // ones that fail write what AFAIL keeps (FB_ONLY: colour, ZB_ONLY: depth, RGB_ONLY: RGB), drawn
    // as a second pass over the same object with the test inverted.
    if (ab & 2) {  // A/B: R4 alpha test (KEEP / ZB_ONLY discard, FB_ONLY / RGB_ONLY drawn as pass)
        if (rs.afail == 1 || rs.afail == 3) it.pc.mode[0] &= ~M_AFAIL_DISCARD;
        g_r.pending.push_back(it);
        return;
    }
    if (!rs.ate || rs.atst == 1) {
        it.pc.mode[0] &= ~(M_ATE | M_AFAIL_DISCARD);
        g_r.pending.push_back(it);
        return;
    }
    uint32_t fail_mask = 0;
    bool fail_z = false;
    switch (rs.afail) {
    case 1: fail_mask = mask; break;                    // FB_ONLY
    case 2: fail_z = zwrite; break;                     // ZB_ONLY
    case 3: fail_mask = mask & 7u; break;               // RGB_ONLY
    default: break;                                     // KEEP
    }
    if (rs.afail == 1 && !zwrite) {  // the failing pixels write what the passing ones do: one pass
        it.pc.mode[0] &= ~(M_ATE | M_AFAIL_DISCARD);
        g_r.pending.push_back(it);
        return;
    }
    if (rs.atst != 0) g_r.pending.push_back(it);  // NEVER: no pixel passes
    if (fail_mask || fail_z) {
        Item f = it;
        f.second = true;
        f.pc.mode[0] |= M_FAILED;
        f.pipeline = (it.pipeline & ~(0x40u | 0xF00u)) | (fail_z ? 0x40u : 0u) | fail_mask << 8;
        g_r.pending.push_back(f);
        g_afail_second.fetch_add(1);
    }
}

void end_vblank(uint64_t vb) {
    g_r.vb = vb;
    gpu_timer_end_vblank(vb);
    evict_textures();
    if (log_every() > 1 && vb % log_every() == 0 && g_r.ok)
        std::fprintf(stderr, "[native] vb %llu textures decoded %llu (total), cached %zu (%llu MB), meshes %zu\n",
                     (unsigned long long)vb, (unsigned long long)g_r.decoded, g_r.textures.size(),
                     (unsigned long long)(g_r.texture_bytes >> 20), g_r.meshes.size());
    if (g_r.shadow_fb) {
        Trash t{vb};
        t.fb = std::move(g_r.shadow_fb);
        g_r.trash.push_back(std::move(t));
        g_r.shadow_fb_depth = 0;
    }
    for (auto &w : g_r.framebuffers) {
        Trash t{vb};
        t.fb = std::move(w.second);
        g_r.trash.push_back(std::move(t));
    }
    g_r.framebuffers.clear();
    for (auto &w : g_r.wrappers) {
        Trash t{vb};
        t.tex = std::move(w.second);
        g_r.trash.push_back(std::move(t));
    }
    g_r.wrappers.clear();
    while (!g_r.trash.empty() && g_r.trash.front().vb + 8 <= vb) g_r.trash.pop_front();
    // one-draw geometry (water, camera-facing parts) goes 8 vblanks after it arrived (drawn by then)
    while (!g_r.transient.empty() && g_r.transient.front().second + 8 <= vb) {
        auto it = g_r.meshes.find(g_r.transient.front().first);
        if (it != g_r.meshes.end()) {
            Trash t{vb};
            t.buf = std::move(it->second.buf);
            g_r.trash.push_back(std::move(t));
            g_r.meshes.erase(it);
        }
        g_r.transient.pop_front();
    }
}
// ---- P2 vertex check (EOD_NATIVE_SKIN_CHECK=1, stage R3) ----
// For an object whose mesh was posed (skinned palette or dents), every vertex of every triangle the
// GS receives for it (the VU1 program's output, the GS reference) is compared with the nearest
// posed vertex projected through the same model -> GS screen matrix: XYZ = ftoi4(X), so the GS
// window coordinate is X * 16 - XYOFFSET in 1/16 px. The bind-pose mesh is measured the same way to
// show the pose mattered. Clip-path records (ITOP 0x200) are counted apart: the clipper makes new
// vertices on the guard band.
struct VertexCheck {
    uint64_t objects = 0, vertices = 0, within = 0, bind_off = 0;
    uint64_t clip_objects = 0, clip_vertices = 0, clip_within = 0;
    double worst = 0, sum = 0;
    uint64_t hist[4097] = {};  // |error| in 1/1024 px bins (0..4 px), last bin: above 4 px
};
VertexCheck g_vck;
// p-th percentile of the non-clip vertex errors, px (upper edge of the bin it falls in)
double vertex_check_percentile(double p) {
    if (!g_vck.vertices) return 0.0;
    const uint64_t want = (uint64_t)std::ceil(p * (double)g_vck.vertices);
    uint64_t acc = 0;
    for (int i = 0; i < 4097; i++) {
        acc += g_vck.hist[i];
        if (acc >= want) return i == 4096 ? g_vck.worst : (i + 1) / 1024.0;
    }
    return g_vck.worst;
}
struct ProjGrid {
    uint64_t key = ~0ull;           // (frame << 32 | serial) of the object
    bool on = false, clip = false, posed = false;
    uint32_t batch = 0, program = 0;
    std::unordered_map<uint64_t, std::vector<std::pair<double, double>>> mine, bind, gs;
    std::vector<std::pair<double, double>> mine_list;  // the native vertices (window, 1/16 px)
    int sc[4] = {0, 0, 0, 0};       // scissor (px) the GS clips the object's triangles to
    uint64_t gs_n = 0, gs_miss = 0;
    // triangle check (geometric, so duplicated vertices do not matter)
    std::vector<std::array<double, 7>> ntri;  // corners (window, 1/16 px), [6] = counted (in scissor, > 1 px^2)
    std::vector<uint8_t> nmatch;
    std::unordered_map<uint64_t, std::vector<uint32_t>> tgrid;
    uint64_t gs_t = 0, gs_t_miss = 0;
};
ProjGrid g_pg;
uint64_t g_tri = 0, g_tri_miss = 0, g_mtri = 0, g_mtri_miss = 0;
// per batch (EOD_NATIVE_SKIN_CHECK report): GS vertices with no native vertex within 1/16 px, and
// native vertices inside the scissor with no GS vertex within 1/16 px (geometry the GS never drew)
struct BatchCheck {
    uint32_t program = 0;
    bool posed = false;
    uint64_t objects = 0, gs = 0, gs_miss = 0, mine = 0, mine_miss = 0;
    uint64_t tri = 0, tri_miss = 0, mtri = 0, mtri_miss = 0;  // GS triangles not in the mesh / mesh triangles the GS never drew
};
std::unordered_map<uint32_t, BatchCheck> g_bck;
uint64_t g_mine = 0, g_mine_miss = 0;

bool vertex_check_on() {
    static const bool on = [] {
        const char *e = std::getenv("EOD_NATIVE_SKIN_CHECK");
        const char *f = std::getenv("EOD_VERTEX_CHECK");  // the name the R3/R4 suites used
        return (e && *e && *e != '0') || (f && *f && *f != '0');
    }();
    return on;
}

void project_into(const Mesh &m, const double a[4][4], double xo, double yo,
                  std::unordered_map<uint64_t, std::vector<std::pair<double, double>>> &grid,
                  std::vector<std::pair<double, double>> *list = nullptr) {
    grid.clear();
    if (list) list->clear();
    for (const Vertex &v : m.vertices) {
        double s[4];
        for (int r = 0; r < 4; r++) s[r] = a[r][0] * v.p[0] + a[r][1] * v.p[1] + a[r][2] * v.p[2] + a[r][3];
        if (!(s[3] > 0)) continue;
        const double x = s[0] / s[3] * 16.0 - xo, y = s[1] / s[3] * 16.0 - yo;  // window, 1/16 px
        if (!(std::fabs(x) < 1e7 && std::fabs(y) < 1e7)) continue;
        const uint64_t k = (uint64_t)(uint32_t)(int32_t)std::floor(x / 16.0) << 32 | (uint32_t)(int32_t)std::floor(y / 16.0);
        grid[k].push_back({x, y});
        if (list) list->push_back({x, y});
    }
}

void grid_add(std::unordered_map<uint64_t, std::vector<std::pair<double, double>>> &grid, double x, double y) {
    const uint64_t k = (uint64_t)(uint32_t)(int32_t)std::floor(x / 16.0) << 32 | (uint32_t)(int32_t)std::floor(y / 16.0);
    grid[k].push_back({x, y});
}

double nearest(const std::unordered_map<uint64_t, std::vector<std::pair<double, double>>> &grid, double x, double y) {
    const int32_t cx = (int32_t)std::floor(x / 16.0), cy = (int32_t)std::floor(y / 16.0);
    double best = 1e30;
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++) {
            const uint64_t k = (uint64_t)(uint32_t)(cx + dx) << 32 | (uint32_t)(cy + dy);
            auto it = grid.find(k);
            if (it == grid.end()) continue;
            // per axis: the GS keeps x and y each in 1/16 px steps (ftoi4)
            for (auto &p : it->second) best = std::fmin(best, std::fmax(std::fabs(p.first - x), std::fabs(p.second - y)));
        }
    return best / 16.0;  // px
}

uint64_t tri_key(uint32_t a, uint32_t b, uint32_t c) {
    if (a > b) std::swap(a, b);
    if (b > c) std::swap(b, c);
    if (a > b) std::swap(a, b);
    return (uint64_t)a << 42 | (uint64_t)b << 21 | c;
}

// The object just finished: which of its native vertices inside the scissor the GS never received.
void vertex_check_close() {
    if (!g_pg.on || g_pg.clip || !g_pg.gs_n) return;
    BatchCheck &bc = g_bck[g_pg.batch];
    bc.program = g_pg.program;
    bc.posed = g_pg.posed;
    bc.objects++;
    bc.gs += g_pg.gs_n;
    bc.gs_miss += g_pg.gs_miss;
    bc.tri += g_pg.gs_t;
    bc.tri_miss += g_pg.gs_t_miss;
    g_tri += g_pg.gs_t;
    g_tri_miss += g_pg.gs_t_miss;
    for (size_t t = 0; t < g_pg.ntri.size(); t++) {
        if (g_pg.ntri[t][6] == 0) continue;
        bc.mtri++;
        g_mtri++;
        if (!g_pg.nmatch[t]) bc.mtri_miss++, g_mtri_miss++;
    }
    for (auto &p : g_pg.mine_list) {
        const double x = p.first / 16.0, y = p.second / 16.0;
        if (x < g_pg.sc[0] + 1 || x > g_pg.sc[2] - 1 || y < g_pg.sc[1] + 1 || y > g_pg.sc[3] - 1) continue;
        const bool miss = nearest(g_pg.gs, p.first, p.second) > 1.0 / 16.0 + 1e-9;
        bc.mine++;
        bc.mine_miss += miss;
        g_mine++;
        g_mine_miss += miss;
    }
}

void vertex_check(const gs::Vertex *const *vs, int n) {
    const uint64_t key = g_cur.frame << 32 | g_cur.serial;
    if (key != g_pg.key) {
        vertex_check_close();
        g_pg.key = key;
        g_pg.on = false;
        g_pg.gs.clear();
        g_pg.gs_n = g_pg.gs_miss = 0;
        if (g_cur.kind != KIND_QUEUE || !g_cur.mesh) return;
        auto it = g_r.meshes.find(g_cur.mesh);
        if (it == g_r.meshes.end() || !it->second.cpu) return;
        double a[4][4];
        screen_matrix(g_cur, a);
        const double xo = (double)(g_xyoffset & 0xFFFF), yo = (double)((g_xyoffset >> 32) & 0xFFFF);
        project_into(*it->second.cpu, a, xo, yo, g_pg.mine, &g_pg.mine_list);
        {
            // native triangles in GS window coordinates (1/16 px), indexed by the cell of each corner
            const Mesh &m = *it->second.cpu;
            g_pg.ntri.clear();
            g_pg.nmatch.clear();
            g_pg.tgrid.clear();
            g_pg.gs_t = g_pg.gs_t_miss = 0;
            std::vector<std::pair<double, double>> scr(m.vertices.size(), {1e30, 1e30});
            for (uint32_t i = 0; i < m.vertices.size(); i++) {
                const Vertex &v = m.vertices[i];
                double s[4];
                for (int r = 0; r < 4; r++) s[r] = a[r][0] * v.p[0] + a[r][1] * v.p[1] + a[r][2] * v.p[2] + a[r][3];
                if (!(s[3] > 0)) continue;
                const double x = s[0] / s[3] * 16.0 - xo, y = s[1] / s[3] * 16.0 - yo;
                if (std::fabs(x) < 1e7 && std::fabs(y) < 1e7) scr[i] = {x, y};
            }
            const double x0 = (g_env.scx0 + 1) * 16.0, y0 = (g_env.scy0 + 1) * 16.0;
            const double x1 = (g_env.scx1 - 1) * 16.0, y1 = (g_env.scy1 - 1) * 16.0;
            for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
                std::array<double, 7> tr;
                bool in = true, ok = true;
                for (int k = 0; k < 3; k++) {
                    const uint32_t q = m.indices[t + k];
                    if (q >= scr.size() || scr[q].first > 1e29) { ok = false; break; }
                    tr[2 * k] = scr[q].first, tr[2 * k + 1] = scr[q].second;
                    in = in && tr[2 * k] > x0 && tr[2 * k] < x1 && tr[2 * k + 1] > y0 && tr[2 * k + 1] < y1;
                }
                if (!ok) continue;
                const double area = (tr[2] - tr[0]) * (tr[5] - tr[1]) - (tr[4] - tr[0]) * (tr[3] - tr[1]);
                tr[6] = in && std::fabs(area) > 2.0 * 256.0 ? 1.0 : 0.0;  // counted when inside and > 1 px^2
                const uint32_t id = (uint32_t)g_pg.ntri.size();
                g_pg.ntri.push_back(tr);
                g_pg.nmatch.push_back(0);
                for (int k = 0; k < 3; k++) {
                    const uint64_t c = (uint64_t)(uint32_t)(int32_t)std::floor(tr[2 * k] / 16.0) << 32 |
                                       (uint32_t)(int32_t)std::floor(tr[2 * k + 1] / 16.0);
                    auto &cell = g_pg.tgrid[c];
                    if (cell.empty() || cell.back() != id) cell.push_back(id);
                }
            }
        }
        g_pg.bind.clear();
        g_pg.posed = it->second.cpu->posed;
        if (g_pg.posed) {
            auto b = g_r.meshes.find(it->second.cpu->bind);
            if (b != g_r.meshes.end() && b->second.cpu) project_into(*b->second.cpu, a, xo, yo, g_pg.bind);
        }
        g_pg.batch = g_cur.batch;
        g_pg.program = g_cur.program;
        g_pg.sc[0] = g_env.scx0, g_pg.sc[1] = g_env.scy0, g_pg.sc[2] = g_env.scx1, g_pg.sc[3] = g_env.scy1;
        g_pg.clip = (g_cur.itop & 0x200) != 0;
        g_pg.on = true;
        (g_pg.clip ? g_vck.clip_objects : g_vck.objects)++;
    }
    if (!g_pg.on) return;
    if (n == 3 && !g_pg.clip) {
        // a GS triangle (> 1 px^2) matches a native triangle when every corner of one is within
        // 1/8 px of a corner of the other (any order)
        const double gx[3] = {(double)vs[0]->x, (double)vs[1]->x, (double)vs[2]->x};
        const double gy[3] = {(double)vs[0]->y, (double)vs[1]->y, (double)vs[2]->y};
        const double area = (gx[1] - gx[0]) * (gy[2] - gy[0]) - (gx[2] - gx[0]) * (gy[1] - gy[0]);
        if (std::fabs(area) > 2.0 * 256.0) {
            g_pg.gs_t++;
            bool found = false;
            const int32_t cx = (int32_t)std::floor(gx[0] / 16.0), cy = (int32_t)std::floor(gy[0] / 16.0);
            for (int dx = -1; dx <= 1 && !found; dx++)
                for (int dy = -1; dy <= 1 && !found; dy++) {
                    auto it = g_pg.tgrid.find((uint64_t)(uint32_t)(cx + dx) << 32 | (uint32_t)(cy + dy));
                    if (it == g_pg.tgrid.end()) continue;
                    for (uint32_t id : it->second) {
                        const auto &tr = g_pg.ntri[id];
                        bool all = true;
                        for (int k = 0; k < 3 && all; k++) {
                            bool any = false;
                            for (int j = 0; j < 3; j++)
                                any = any || (std::fabs(tr[2 * j] - gx[k]) <= 2.0 && std::fabs(tr[2 * j + 1] - gy[k]) <= 2.0);
                            all = any;
                        }
                        if (all) {
                            found = true;
                            g_pg.nmatch[id] = 1;
                            break;
                        }
                    }
                }
            if (!found) g_pg.gs_t_miss++;
        }
    }
    for (int i = 0; i < n; i++) {
        const double x = vs[i]->x, y = vs[i]->y;
        const double d = nearest(g_pg.mine, x, y);
        const bool ok = d < 1.0 / 16.0 + 1e-9;
        if (g_pg.clip) {
            g_vck.clip_vertices++;
            g_vck.clip_within += ok;
            continue;
        }
        grid_add(g_pg.gs, x, y);
        g_pg.gs_n++;
        g_pg.gs_miss += !ok;
        g_vck.vertices++;
        g_vck.within += ok;
        g_vck.sum += std::fmin(d, 1e6);
        g_vck.worst = std::fmax(g_vck.worst, std::fmin(d, 1e6));
        g_vck.hist[d < 4.0 ? (int)(d * 1024.0) : 4096]++;
        if (ok && !g_pg.bind.empty() && nearest(g_pg.bind, x, y) > 1.0 / 16.0) g_vck.bind_off++;
    }
}

void vertex_check_report() {
    if (!vertex_check_on()) return;
    vertex_check_close();
    g_pg.on = false;
    std::fprintf(stderr,
                 "[native] P2 final vertices vs GS (all queue objects): objects %llu GS vertices %llu within 1/16 px %llu (worst %.4f px, p99 %.4f px, mean %.4f px; "
                 "bind pose off by > 1/16 px at %llu of them) | native vertices in scissor %llu with no GS vertex within 1/16 px %llu | "
                 "clip-path objects %llu vertices %llu within %llu | GS triangles %llu not in the native mesh %llu | "
                 "native triangles in scissor %llu never drawn by the GS %llu\n",
                 (unsigned long long)g_vck.objects, (unsigned long long)g_vck.vertices, (unsigned long long)g_vck.within,
                 g_vck.worst, vertex_check_percentile(0.99), g_vck.vertices ? g_vck.sum / (double)g_vck.vertices : 0.0, (unsigned long long)g_vck.bind_off,
                 (unsigned long long)g_mine, (unsigned long long)g_mine_miss,
                 (unsigned long long)g_vck.clip_objects, (unsigned long long)g_vck.clip_vertices, (unsigned long long)g_vck.clip_within,
                 (unsigned long long)g_tri, (unsigned long long)g_tri_miss, (unsigned long long)g_mtri, (unsigned long long)g_mtri_miss);
    std::vector<std::pair<uint32_t, BatchCheck>> v(g_bck.begin(), g_bck.end());
    std::sort(v.begin(), v.end(), [](auto &x, auto &y) { return x.second.tri_miss + x.second.mtri_miss > y.second.tri_miss + y.second.mtri_miss; });
    for (size_t i = 0; i < v.size() && i < 400; i++) {
        const BatchCheck &c = v[i].second;
        if (!c.tri && !c.mtri) continue;
        std::fprintf(stderr, "[native] P2 worst batch %08x program %04x posed %d objects %llu: GS vertices %llu missed %llu | native %llu missed %llu | "
                     "GS triangles %llu not in mesh %llu | mesh triangles %llu not drawn by GS %llu\n",
                     v[i].first, c.program, (int)c.posed, (unsigned long long)c.objects, (unsigned long long)c.gs,
                     (unsigned long long)c.gs_miss, (unsigned long long)c.mine, (unsigned long long)c.mine_miss,
                     (unsigned long long)c.tri, (unsigned long long)c.tri_miss, (unsigned long long)c.mtri, (unsigned long long)c.mtri_miss);
    }
}
#else
void finalize() {
    if (g_active && !g_finalized) {
        g_frames[g_cur.frame].lost++;
        g_frames[g_cur.frame].k_lost[g_cur.kind % KIND_COUNT]++;
    }
    g_finalized = true;
}
void flush() {}
void end_vblank(uint64_t) {}
#endif

void begin_object(const Packet &p) {
    finalize();
    g_cur = p;
    g_active = true;
    g_finalized = false;
    g_have_env = false;
    g_gs_k.clear();
    g_frames[p.frame].objects++;
    g_frames[p.frame].k_objects[p.kind % KIND_COUNT]++;
    g_newest_frame = std::max(g_newest_frame, p.frame);
}

void report(uint64_t vb) {
    const uint64_t every = log_every();
    // EE frames older than the newest one the GS has seen are complete on both sides
    std::map<uint64_t, Expected> expected;
    take_expected(g_newest_frame, expected);
    std::vector<uint64_t> frames;
    for (auto &e : expected) frames.push_back(e.first);
    for (auto &f : g_frames)
        if (f.first < g_newest_frame && !expected.count(f.first)) frames.push_back(f.first);
    std::sort(frames.begin(), frames.end());
    const bool log_parity = std::getenv("EOD_NATIVE_PARITY_LOG") != nullptr;
    for (const uint64_t frame : frames) {
        auto it = g_frames.find(frame);
        FrameStats none;
        const FrameStats &s = it != g_frames.end() ? it->second : none;
        auto e = expected.find(frame);
        const uint32_t world = e != expected.end() ? e->second.world : 0, water = e != expected.end() ? e->second.water : 0;
        const uint32_t queue = e != expected.end() ? e->second.queue : 0;
        const uint32_t shadow = e != expected.end() ? e->second.shadow : 0, flare = e != expected.end() ? e->second.flare : 0;
        const uint32_t particle = e != expected.end() ? e->second.particle : 0;
        const uint32_t sprite = e != expected.end() ? e->second.sprite : 0;
        const uint32_t imm = e != expected.end() ? e->second.immediate : 0;
        const uint32_t cockpit = e != expected.end() ? e->second.cockpit : 0;
        if (s.skipped) {
            g_parity.skipped_frames++;
        } else {
            g_parity.frames++;
            g_parity.world_submitted += world;
            g_parity.world_drawn += s.k_drawn[KIND_WORLD];
            g_parity.water_submitted += water;
            g_parity.water_drawn += s.k_drawn[KIND_WATER];
            const bool we = s.k_drawn[KIND_WORLD] == world && s.k_objects[KIND_WORLD] == world;
            const bool ae = s.k_drawn[KIND_WATER] == water && s.k_objects[KIND_WATER] == water;
            g_parity.world_equal += we;
            g_parity.water_equal += ae;
            const bool qe = s.k_drawn[KIND_QUEUE] == queue && s.k_objects[KIND_QUEUE] == queue;
            g_parity.queue_equal += qe;
            g_parity.queue_submitted += queue;
            g_parity.queue_drawn += s.k_drawn[KIND_QUEUE];
            const bool se = s.k_drawn[KIND_SHADOW] == shadow && s.k_objects[KIND_SHADOW] == shadow;
            const bool fe = s.k_drawn[KIND_FLARE] == flare && s.k_objects[KIND_FLARE] == flare;
            g_parity.shadow_equal += se;
            g_parity.shadow_submitted += shadow;
            g_parity.shadow_drawn += s.k_drawn[KIND_SHADOW];
            g_parity.flare_equal += fe;
            g_parity.flare_submitted += flare;
            g_parity.flare_drawn += s.k_drawn[KIND_FLARE];
            const bool pe = s.k_drawn[KIND_PARTICLE] == particle && s.k_objects[KIND_PARTICLE] == particle;
            const bool spe = s.k_drawn[KIND_SPRITE] == sprite && s.k_objects[KIND_SPRITE] == sprite;
            const bool ie = s.k_drawn[KIND_IMMEDIATE] == imm && s.k_objects[KIND_IMMEDIATE] == imm;
            const bool ce = s.k_drawn[KIND_COCKPIT] == cockpit && s.k_objects[KIND_COCKPIT] == cockpit;
            g_parity.particle_equal += pe;
            g_parity.particle_submitted += particle;
            g_parity.particle_drawn += s.k_drawn[KIND_PARTICLE];
            g_parity.sprite_equal += spe;
            g_parity.sprite_submitted += sprite;
            g_parity.sprite_drawn += s.k_drawn[KIND_SPRITE];
            g_parity.imm_equal += ie;
            g_parity.imm_submitted += imm;
            g_parity.imm_drawn += s.k_drawn[KIND_IMMEDIATE];
            g_parity.cockpit_equal += ce;
            g_parity.cockpit_submitted += cockpit;
            g_parity.cockpit_drawn += s.k_drawn[KIND_COCKPIT];
            if (log_parity && !qe)
                std::fprintf(stderr, "[native] parity frame %llu queue records submitted %u objects %u drawn %u lost %u\n",
                             (unsigned long long)frame, queue, s.k_objects[KIND_QUEUE], s.k_drawn[KIND_QUEUE],
                             s.k_lost[KIND_QUEUE]);
            if (log_parity && (!we || !ae))
                std::fprintf(stderr,
                             "[native] parity frame %llu world submitted %u objects %u drawn %u lost %u | water submitted %u "
                             "objects %u drawn %u lost %u\n",
                             (unsigned long long)frame, world, s.k_objects[KIND_WORLD], s.k_drawn[KIND_WORLD],
                             s.k_lost[KIND_WORLD], water, s.k_objects[KIND_WATER], s.k_drawn[KIND_WATER], s.k_lost[KIND_WATER]);
            if (log_parity && (!se || !fe))
                std::fprintf(stderr,
                             "[native] parity frame %llu shadow submitted %u objects %u drawn %u lost %u | flare submitted %u "
                             "objects %u drawn %u lost %u\n",
                             (unsigned long long)frame, shadow, s.k_objects[KIND_SHADOW], s.k_drawn[KIND_SHADOW],
                             s.k_lost[KIND_SHADOW], flare, s.k_objects[KIND_FLARE], s.k_drawn[KIND_FLARE], s.k_lost[KIND_FLARE]);
            if (log_parity && (!pe || !spe || !ie || !ce))
                std::fprintf(stderr,
                             "[native] parity frame %llu particle %u/%u/%u sprite %u/%u/%u imm %u/%u/%u cockpit %u/%u/%u\n",
                             (unsigned long long)frame, particle, s.k_objects[KIND_PARTICLE], s.k_drawn[KIND_PARTICLE],
                             sprite, s.k_objects[KIND_SPRITE], s.k_drawn[KIND_SPRITE],
                             imm, s.k_objects[KIND_IMMEDIATE], s.k_drawn[KIND_IMMEDIATE],
                             cockpit, s.k_objects[KIND_COCKPIT], s.k_drawn[KIND_COCKPIT]);
        }
        if (every && (every == 1 || frame % every < 2)) {
            std::fprintf(stderr,
                         "[native] gs frame %llu objects %u drawn %u (empty %u, textured %u, skinned %u of them posed %u, "
                         "render-target texture untextured %u) skipped %u lost %u suppressed GS prims %llu render passes %u (gs vb %llu)\n",
                         (unsigned long long)frame, s.objects, s.drawn, s.empty, s.textured, s.skinned, s.posed, s.untextured_rt,
                         s.skipped, s.lost, (unsigned long long)s.suppressed, s.flushes, (unsigned long long)vb);
            std::fprintf(stderr, "[native] gs frame %llu fog: FGE draws %llu\n", (unsigned long long)frame,
                         (unsigned long long)g_fge_draws.load());
            std::fprintf(stderr,
                         "[native] gs frame %llu queue records submitted %u drawn %u | P1 frames queue equal %llu/%llu "
                         "(records drawn %llu of %llu)\n",
                         (unsigned long long)frame, queue, s.k_drawn[KIND_QUEUE], (unsigned long long)g_parity.queue_equal,
                         (unsigned long long)g_parity.frames, (unsigned long long)g_parity.queue_drawn,
                         (unsigned long long)g_parity.queue_submitted);
            std::fprintf(stderr,
                         "[native] gs frame %llu world leaves submitted %u drawn %u (lost %u) water submitted %u drawn %u | "
                         "P1 frames world equal %llu/%llu water equal %llu/%llu (leaves drawn %llu of %llu, water %llu of %llu) "
                         "frame-skipped %llu | native-only dropped GS prims %llu\n",
                         (unsigned long long)frame, world, s.k_drawn[KIND_WORLD], s.k_lost[KIND_WORLD], water,
                         s.k_drawn[KIND_WATER], (unsigned long long)g_parity.world_equal, (unsigned long long)g_parity.frames,
                         (unsigned long long)g_parity.water_equal, (unsigned long long)g_parity.frames,
                         (unsigned long long)g_parity.world_drawn, (unsigned long long)g_parity.world_submitted,
                         (unsigned long long)g_parity.water_drawn, (unsigned long long)g_parity.water_submitted,
                         (unsigned long long)g_parity.skipped_frames, (unsigned long long)g_only_dropped);
            if (native_only()) report_drops(frame);
            std::fprintf(stderr,
                         "[native] gs frame %llu shadows submitted %u drawn %u (lost %u) flares submitted %u drawn %u (lost %u) | "
                         "P1 frames shadow equal %llu/%llu flare equal %llu/%llu (shadows %llu of %llu, flares %llu of %llu)\n",
                         (unsigned long long)frame, shadow, s.k_drawn[KIND_SHADOW], s.k_lost[KIND_SHADOW], flare,
                         s.k_drawn[KIND_FLARE], s.k_lost[KIND_FLARE], (unsigned long long)g_parity.shadow_equal,
                         (unsigned long long)g_parity.frames, (unsigned long long)g_parity.flare_equal,
                         (unsigned long long)g_parity.frames, (unsigned long long)g_parity.shadow_drawn,
                         (unsigned long long)g_parity.shadow_submitted, (unsigned long long)g_parity.flare_drawn,
                         (unsigned long long)g_parity.flare_submitted);
            std::fprintf(stderr,
                         "[native] gs frame %llu particles %u/%u sprites %u/%u imm %u/%u cockpit %u/%u | "
                         "P1 equal p=%llu/%llu s=%llu/%llu i=%llu/%llu c=%llu/%llu "
                         "(totals p=%llu/%llu s=%llu/%llu i=%llu/%llu c=%llu/%llu)\n",
                         (unsigned long long)frame, s.k_drawn[KIND_PARTICLE], particle, s.k_drawn[KIND_SPRITE], sprite,
                         s.k_drawn[KIND_IMMEDIATE], imm, s.k_drawn[KIND_COCKPIT], cockpit,
                         (unsigned long long)g_parity.particle_equal, (unsigned long long)g_parity.frames,
                         (unsigned long long)g_parity.sprite_equal, (unsigned long long)g_parity.frames,
                         (unsigned long long)g_parity.imm_equal, (unsigned long long)g_parity.frames,
                         (unsigned long long)g_parity.cockpit_equal, (unsigned long long)g_parity.frames,
                         (unsigned long long)g_parity.particle_drawn, (unsigned long long)g_parity.particle_submitted,
                         (unsigned long long)g_parity.sprite_drawn, (unsigned long long)g_parity.sprite_submitted,
                         (unsigned long long)g_parity.imm_drawn, (unsigned long long)g_parity.imm_submitted,
                         (unsigned long long)g_parity.cockpit_drawn, (unsigned long long)g_parity.cockpit_submitted);
        }
        if (it != g_frames.end()) g_frames.erase(it);
    }
}

}  // namespace

void gs_command(uint32_t kind, const void *data, uint32_t size) {
    switch (kind) {
    case CMD_OBJ: {
        Packet p;
        if (size < sizeof p) return;
        std::memcpy(&p, data, sizeof p);
        begin_object(p);
        break;
    }
    case CMD_OFF:
        finalize();
        g_active = false;
        break;
    case CMD_MESH: {
        Mesh *m;
        std::memcpy(&m, data, sizeof m);
#ifdef EOD_PLUME
        if (m->transient) g_r.transient.emplace_back(m->id, g_r.vb);
        if (m->replaces) {
            auto it = g_r.meshes.find(m->replaces);
            if (it != g_r.meshes.end()) {
                for (const Item &pi : g_r.pending)
                    if (pi.mesh == &it->second) {
                        flush();  // an object still to be drawn uses the old geometry
                        break;
                    }
                for (const Item &pi : g_r.shadow_pending)
                    if (pi.mesh == &it->second) {
                        flush_shadows();
                        break;
                    }
                Trash t{g_r.vb};
                t.buf = std::move(it->second.buf);
                g_r.trash.push_back(std::move(t));
                g_r.meshes.erase(it);
            }
        }
        g_r.meshes[m->id].cpu.reset(m);
#else
        delete m;
#endif
        break;
    }
    default: break;
    }
}

bool gs_draw(const gs::Env &e, const gs::Vertex &v, const gs::Vertex *b, const gs::Vertex *c) {
    if (!g_active) {
#ifdef EOD_PLUME
        if (!g_r.pending.empty() || !g_r.shadow_pending.empty()) flush();  // before the GS draws something of its own
#endif
        // EOD_NATIVE_ONLY=2: the GS still draws sprites (2D, HUD, the frame copy to the display
        // buffer), so the 3D scene on screen is Plume's alone and screenshots can be compared (P3).
        if (native_only() && !(native_only_level() == 2 && b && !c)) {
#ifdef EOD_PLUME
            // EOD_NATIVE_ONLY=1: intercept untextured Z/color clear sprites so Plume clears its
            // own target at the start of the next flush (zero GS primitives drawn, no stale depth).
            if (native_only_level() == 1 && b && !c && !e.tme && e.ztst == 1 && !e.zmsk) {
                OnlyClear &oc = g_only_clears[(uint32_t)e.fbp];
                oc.depth = true;
                oc.z = (float)((double)v.z / depth_range(e.zpsm));
                if ((e.fbmsk & 0xFFFFFFu) != 0xFFFFFFu) {
                    oc.color = true;
                    oc.r = v.r / 255.0f;
                    oc.g = v.g / 255.0f;
                    oc.b = v.b / 255.0f;
                    oc.a = v.a / 255.0f;
                }
            }
#endif
            g_only_dropped++;
            note_drop(e, v, b, c);
            return true;
        }
        return false;
    }
#ifdef EOD_PLUME
    if (!g_r.tried) init();
    if (!g_r.ok || (modern::g_on && modern::live())) {  // no renderer: the GS draws the object
        if (native_only()) {
            g_only_dropped++;
            note_drop(e, v, b, c);
            return true;
        }
        return false;
    }
#else
    if (native_only()) {
        g_only_dropped++;
        note_drop(e, v, b, c);
        return true;
    }
    return false;
#endif
    if (!g_have_env && !g_finalized) {
        g_env = e;
        g_xyoffset = context_xyoffset();
        g_have_env = true;
#ifdef EOD_PLUME
        q_check(e, v);
#endif
    }
#ifdef EOD_PLUME
    if (vertex_check_on()) {
        static const bool hooked = (diff_add_exit_report(vertex_check_report), true);
        (void)hooked;
        const gs::Vertex *vs[3] = {&v, b, c};
        vertex_check(vs, b && c ? 3 : 1);
    }
#endif
    static const uint64_t dbg = std::getenv("EOD_NATIVE_PASSES") ? std::strtoull(std::getenv("EOD_NATIVE_PASSES"), nullptr, 10) : 0;
    static uint64_t dbg_frame = 0;
    if (dbg && !dbg_frame && g_cur.frame >= dbg) dbg_frame = g_cur.frame;
    if (dbg && g_cur.frame == dbg_frame) {
        static uint32_t last_serial = ~0u, last_obj = ~0u, prims = 0;
        if (gs::g_env_serial != last_serial || g_cur.serial != last_obj) {
            std::fprintf(stderr, "[native] pass obj %u batch %08x prog %04x mesh %u: tme %d tbp %x tbw %u psm %x %ux%u tfx %d tcc %d abe %d abcd %d%d%d%d fix %u ate %d atst %d ztst %d zmsk %d fbmsk %x fbp %x rgba %u %u %u %u mine %.0f %.0f %.0f itop %x (prev run %u prims)\n",
                         g_cur.serial, g_cur.batch, g_cur.program, g_cur.mesh, e.tme, e.tbp, e.tbw, e.tpsm, e.tw, e.th, e.tfx, e.tcc,
                         e.abe, e.ba, e.bb, e.bc, e.bd, e.bfix, e.ate, e.atst, e.ztst, e.zmsk, e.fbmsk, e.fbp, v.r, v.g, v.b, v.a, g_cur.colour_b[0], g_cur.colour_b[1], g_cur.colour_b[2], g_cur.itop, prims);
            last_serial = gs::g_env_serial;
            last_obj = g_cur.serial;
            prims = 0;
        }
        prims++;
    }
    g_frames[g_cur.frame].suppressed++;
    if (e.tme && e.mmin >= 2) {  // the TEX1.K sequence the GS sees within the object (run check)
        const int32_t k = (int32_t)std::lround(e.k * 16.0f);
        if (g_gs_k.empty() || g_gs_k.back() != k) g_gs_k.push_back(k);
    }
    return true;
}

// A GS memory transfer starts (TRXDIR written; BITBLTBUF/TRXPOS/TRXREG are set). Pending objects
// are drawn first only when it touches their frame or depth buffer: the texture manager's uploads
// between buckets do not (the objects' textures were decoded when they were queued).
void gs_barrier() {
    if (g_active) finalize();
#ifdef EOD_PLUME
    if (!g_r.shadow_pending.empty()) flush_shadows();
    if (g_r.pending.empty()) return;
    const Target &tg = g_r.target;
    uint32_t ff, fc, zf = 0, zc = 0;
    gs::page_range(tg.fbp, tg.fbw, tg.fpsm, 0, 0, (int)tg.fbw * 64, (int)tg.rows, ff, fc);
    if (tg.depth) gs::page_range(tg.zbp, tg.fbw, tg.zpsm, 0, 0, (int)tg.fbw * 64, (int)tg.rows, zf, zc);
    const uint64_t blt = gs::st.bitbltbuf, pos = gs::st.trxpos, reg = gs::st.trxreg;
    const uint32_t dir = (uint32_t)(gs::st.trxdir & 3);
    const int w = (int)(reg & 0xFFF), h = (int)((reg >> 32) & 0xFFF);
    auto overlaps = [&](uint32_t bp, uint32_t bw, uint32_t psm, int x, int y) {
        uint32_t f, c;
        gs::page_range(bp, bw, psm, x, y, w, h, f, c);
        auto hit = [&](uint32_t a0, uint32_t ac) {
            for (uint32_t i = 0; i < c; i++)
                if (((f + i - a0) % gs::kPages) < ac) return true;
            return false;
        };
        return hit(ff, fc) || (zc && hit(zf, zc));
    };
    bool touch = false;
    if (dir != 0)  // local -> host or local -> local: the source is read
        touch |= overlaps((uint32_t)(blt & 0x3FFF), (uint32_t)((blt >> 16) & 0x3F), (uint32_t)((blt >> 24) & 0x3F),
                          (int)(pos & 0x7FF), (int)((pos >> 16) & 0x7FF));
    if (dir != 1)  // host -> local or local -> local: the destination is written
        touch |= overlaps((uint32_t)((blt >> 32) & 0x3FFF), (uint32_t)((blt >> 48) & 0x3F), (uint32_t)((blt >> 56) & 0x3F),
                          (int)((pos >> 32) & 0x7FF), (int)((pos >> 48) & 0x7FF));
    if (touch) flush();
#endif
}

// ---- stage R6: look toggle, P5 numbers ------------------------------------------------------
namespace {
struct LookTiming {
    std::chrono::steady_clock::time_point last{};
    double sum[2] = {}, max[2] = {};
    uint64_t n[2] = {};
    double after_toggle_max = 0;   // the frame a toggle took effect in
    uint32_t pending_toggle = 0;   // frames left to watch after a toggle
    size_t pipes_at_first = 0, sets_at_first = 0, pipes_now = 0;
    uint64_t sets_created = 0;
    uint64_t tex_bytes_first = 0;
} g_lt;
uint64_t count_sets() {
    uint64_t n = 0;
    for (auto &t : g_r.textures)
        for (auto &s : t.second->sets) n += s ? 1 : 0;
    return n;
}
void r6_report() {
    if (!g_r.ok) return;
    if (g_gpu.enabled) {
        for (uint32_t i = 0; i < GpuTimer::kRing; i++)
            if (g_gpu.slots[i].pending) gpu_timer_collect_slot(g_gpu.slots[i], false);
    }
    std::fprintf(stderr,
                 "[r6] materials: %zu records (%zu keyed); textures decoded %llu, indexed %llu, with a record %llu; "
                 "draws %llu, textured %llu, with a record %llu (%.1f%% of draws, %.1f%% of textured); palette fallbacks %llu; "
                 "low-confidence records seen %zu; Modern VFX draws fire %llu smoke %llu spark %llu\n",
                 g_mat_records, g_mat_bound, (unsigned long long)g_r.decoded, (unsigned long long)g_textures_indexed,
                 (unsigned long long)g_textures_material, (unsigned long long)g_draws_all, (unsigned long long)g_draws_textured,
                 (unsigned long long)g_draws_material, g_draws_all ? 100.0 * g_draws_material / g_draws_all : 0.0,
                 g_draws_textured ? 100.0 * g_draws_material / g_draws_textured : 0.0, (unsigned long long)g_pk_fallback,
                 g_low_seen.size(), (unsigned long long)g_draws_vfx[0], (unsigned long long)g_draws_vfx[1],
                 (unsigned long long)g_draws_vfx[2]);
    if (g_gpu.enabled && (g_gpu.frames[0] || g_gpu.frames[1])) {
        const double cm = g_gpu.frames[0] ? g_gpu.sum_ms[0] / g_gpu.frames[0] : 0.0;
        const double mm = g_gpu.frames[1] ? g_gpu.sum_ms[1] / g_gpu.frames[1] : 0.0;
        const double cs = g_gpu.frames[0] ? g_gpu.shadow_ms[0] / g_gpu.frames[0] : 0.0;
        const double ms = g_gpu.frames[1] ? g_gpu.shadow_ms[1] / g_gpu.frames[1] : 0.0;
        const double cd = g_gpu.frames[0] ? (double)g_gpu.draws[0] / g_gpu.frames[0] : 0.0;
        const double md = g_gpu.frames[1] ? (double)g_gpu.draws[1] / g_gpu.frames[1] : 0.0;
        std::fprintf(stderr,
                     "[r6] GPU timing: Classic mean %.3f ms (max %.3f, shadow %.3f, %llu frames, %.0f draws/f) | "
                     "Modern mean %.3f ms (max %.3f, shadow %.3f, %llu frames, %.0f draws/f) | delta %+.3f ms\n",
                     cm, g_gpu.max_ms[0], cs, (unsigned long long)g_gpu.frames[0], cd,
                     mm, g_gpu.max_ms[1], ms, (unsigned long long)g_gpu.frames[1], md,
                     (g_gpu.frames[0] && g_gpu.frames[1]) ? (mm - cm) : 0.0);
    }
    for (int l = 0; l < 2; l++)
        if (g_lt.n[l])
            std::fprintf(stderr, "[look] %s frames %llu, mean %.2f ms, max %.2f ms\n", l ? "Modern" : "Classic",
                         (unsigned long long)g_lt.n[l], g_lt.sum[l] / g_lt.n[l], g_lt.max[l]);
    std::fprintf(stderr, "[look] sets made for a texture that already had its Classic and Modern sets: %llu\n", (unsigned long long)g_sets_late);
    std::fprintf(stderr, "[look] toggles %u, worst frame after a toggle %.2f ms, pipelines %zu, sun world %.3f %.3f %.3f\n",
                 g_look_toggles.load(), g_lt.after_toggle_max, g_r.pipelines.size(), g_sun_world[0], g_sun_world[1], g_sun_world[2]);
}
}  // namespace

// any thread (F9 on the window thread): only flips the look; gs_vblank does the bookkeeping
void toggle_look() {
    g_modern = !g_modern.load();
    ++g_look_toggles;
}
namespace {
void toggled(uint32_t n) {  // GS thread
    const bool m = g_modern.load();
    g_lt.pending_toggle = 2;
    if (n == 1) {
        g_lt.pipes_at_first = g_r.pipelines.size();
        g_lt.sets_at_first = (size_t)count_sets();
        g_lt.tex_bytes_first = g_r.texture_bytes;
    }
    if (n == 1 || n % 10 == 0)
        std::fprintf(stderr, "[look] toggle %u -> %s at vb %llu: pipelines %zu (first toggle %zu), sets %llu (first %zu), textures %.1f MB (first %.1f)\n",
                     n, m ? "Modern" : "Classic", (unsigned long long)g_r.vb, g_r.pipelines.size(), g_lt.pipes_at_first,
                     (unsigned long long)count_sets(), g_lt.sets_at_first, g_r.texture_bytes / 1048576.0,
                     g_lt.tex_bytes_first / 1048576.0);
}
}  // namespace
bool look_modern() { return g_modern.load(); }

bool last_target(uint32_t &fbp, uint32_t &fbw, uint32_t &psm) {
#ifdef EOD_PLUME
    if (!g_r.has_main_target) return false;
    fbp = g_r.main_target.fbp;
    fbw = g_r.main_target.fbw;
    psm = g_r.main_target.fpsm;
    return true;
#else
    (void)fbp; (void)fbw; (void)psm;
    return false;
#endif
}

void gs_vblank(uint64_t frame) {
    if (g_active) finalize();
    g_active = false;
    flush();
    end_vblank(frame);
    report(frame);
    // stage R6: frame time per look (GS thread, vblank to vblank), the test toggle (P5)
    {
        static const bool hooked = (diff_add_exit_report(r6_report), true);
        (void)hooked;
        const auto now = std::chrono::steady_clock::now();
        if (g_lt.last.time_since_epoch().count() && g_r.ok) {
            const double ms = std::chrono::duration<double, std::milli>(now - g_lt.last).count();
            const int l = g_modern.load() ? 1 : 0;
            g_lt.sum[l] += ms, g_lt.n[l]++, g_lt.max[l] = std::max(g_lt.max[l], ms);
            static const bool flog = std::getenv("EOD_LOOK_FRAME_LOG") != nullptr;
            if (flog) std::fprintf(stderr, "[lookf] vb %llu %s %.2f ms\n", (unsigned long long)frame, l ? "M" : "C", ms);
            if (g_lt.pending_toggle) {
                g_lt.after_toggle_max = std::max(g_lt.after_toggle_max, ms);
                g_lt.pending_toggle--;
            }
        }
        g_lt.last = now;
        static const long every = std::getenv("EOD_LOOK_TOGGLE_EVERY") ? std::atol(std::getenv("EOD_LOOK_TOGGLE_EVERY")) : 0;
        static const long from = std::getenv("EOD_LOOK_TOGGLE_FROM") ? std::atol(std::getenv("EOD_LOOK_TOGGLE_FROM")) : 600;
        static const long count = std::getenv("EOD_LOOK_TOGGLE_COUNT") ? std::atol(std::getenv("EOD_LOOK_TOGGLE_COUNT")) : 100;
        if (every > 0 && g_r.ok && (long)frame >= from && ((long)frame - from) % every == 0 && (long)g_look_toggles.load() < count)
            toggle_look();
        static uint32_t seen = 0;
        while (seen != g_look_toggles.load()) toggled(++seen);
        if (g_r.ok && frame % 1200 == 0) r6_report();
    }
}

uint64_t fog_fge_draws() { return g_fge_draws.load(); }

}  // namespace native
