// native_renderer.cpp - GS-thread side of the native renderer (native_renderer.h): which GS draws
// belong to native objects, the objects' GS state, and the plume renderer that draws them.
#include "core/diff_harness.h"
#include "native_renderer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
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
std::atomic<uint64_t> g_fge_draws{0};  // native draws with PRIM.FGE set (src/native_fog.cpp)

// ---- per EE frame counters (the frame a record was queued in) ----
struct FrameStats {
    uint32_t objects = 0;     // records the GS was told about (OBJ)
    uint32_t drawn = 0;       // drawn by plume
    uint32_t empty = 0;       // drawn, but the batch decoded to no triangles
    uint32_t skipped = 0;     // frame skipped by the GS (FrameSkip): nothing drawn, as the GS
    uint32_t lost = 0;        // not drawn: no mesh, no surface or no renderer
    uint32_t textured = 0, untextured_rt = 0, skinned = 0, posed = 0, flushes = 0;
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

struct PushConstants {
    float cx[4], cy[4], cz[4], cw[4];
    float light[4], diffuse[4], ambient[4];
    uint32_t mode[4];
};
static_assert(sizeof(PushConstants) == 128, "push constants");
enum : uint32_t { M_TME = 0x1, M_TCC = 0x2, M_TFX_SHIFT = 2, M_ATE = 0x10, M_ATST_SHIFT = 5, M_AFAIL_DISCARD = 0x100,
                  M_CFIX = 0x200, M_SPEC = 0x400, M_FGE = 0x800 };

// blend equations (GS ALPHA a, b, d with C in the shader's second output)
enum Blend : uint32_t { B_OPAQUE, B_MIX, B_ADD, B_REVSUB, B_SCALE, B_DST_ADD, B_INV_MIX, B_KEEP, B_COUNT };

struct GpuMesh {
    std::unique_ptr<Mesh> cpu;
    std::unique_ptr<plume::RenderBuffer> buf;  // vertices, then indices
    uint64_t index_offset = 0;
};

struct Texture {
    std::unique_ptr<plume::RenderTexture> tex;
    std::unique_ptr<plume::RenderBuffer> staging;  // until the upload is recorded
    std::unique_ptr<plume::RenderDescriptorSet> sets[8];  // per sampler
    uint32_t w = 0, h = 0;
    uint64_t key = 0, last_use = 0;
    bool pending_upload = false;
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
    std::unique_ptr<plume::RenderDescriptorSet> sets[8];
    std::unique_ptr<plume::RenderFramebuffer> fb;
};

struct Renderer {
    bool tried = false, ok = false;
    plume::VulkanDevice *dev = nullptr;
    std::unique_ptr<plume::RenderCommandQueue> queue;
    std::unique_ptr<plume::RenderCommandList> list;
    std::unique_ptr<plume::RenderPipelineLayout> layout;
    std::unique_ptr<plume::RenderShader> vs, ps;
    std::unique_ptr<plume::RenderSampler> samplers[8];
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
    if (!g_r.layout || !g_r.vs || !g_r.ps || !g_r.list) {
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
    g_r.ok = true;
    std::fprintf(stderr, "[native] plume renderer ready (%s)\n", dev->description.name.c_str());
    return true;
}

plume::RenderDescriptorSet *texture_set(Texture *t, uint32_t sampler) {
    auto &s = t->sets[sampler];
    if (!s) {
        s = g_r.dev->createDescriptorSet(set_desc());
        s->setTexture(0, t->tex.get(), plume::RenderTextureLayout::SHADER_READ);
        s->setSampler(1, g_r.samplers[sampler].get());
    }
    return s.get();
}

plume::RenderPipeline *pipeline(uint32_t key) {
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
    d.vertexShader = g_r.vs.get();
    d.pixelShader = g_r.ps.get();
    d.inputSlots = &in;
    d.inputSlotsCount = 1;
    d.inputElements = el;
    d.inputElementsCount = 4;
    d.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    d.cullMode = cull == 1 ? plume::RenderCullMode::BACK : cull == 2 ? plume::RenderCullMode::FRONT : plume::RenderCullMode::NONE;
    d.depthClipEnabled = true;
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
    b.blendOp = blend == B_REVSUB ? O::REV_SUBTRACT : O::ADD;
    switch (blend) {
    case B_MIX: b.srcBlend = B::SRC1_ALPHA, b.dstBlend = B::INV_SRC1_ALPHA; break;
    case B_ADD: b.srcBlend = B::SRC1_ALPHA, b.dstBlend = B::ONE; break;
    case B_REVSUB: b.srcBlend = B::SRC1_ALPHA, b.dstBlend = B::ONE; break;
    case B_SCALE: b.srcBlend = B::SRC1_ALPHA, b.dstBlend = B::ZERO; break;
    case B_DST_ADD: b.srcBlend = B::ONE, b.dstBlend = B::SRC1_ALPHA; break;
    case B_INV_MIX: b.srcBlend = B::INV_SRC1_ALPHA, b.dstBlend = B::SRC1_ALPHA; break;
    case B_KEEP: b.srcBlend = B::ZERO, b.dstBlend = B::ONE; break;
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
Texture *texture(const gs::Env &e, FrameStats &fs) {
    if (!e.tme || e.tw == 0 || e.th == 0 || e.tw > 1024 || e.th > 1024) return nullptr;
    uint32_t first, count;
    gs::page_range(e.tbp, e.tbw, e.tpsm, 0, 0, (int)e.tw, (int)e.th, first, count);
    if (!gs::hw::native_texture_on_cpu(first, count)) {
        fs.untextured_rt++;
        return nullptr;
    }
    const bool indexed = e.tpsm == gs::PSMT8 || e.tpsm == gs::PSMT4 || e.tpsm == gs::PSMT8H ||
                         e.tpsm == gs::PSMT4HL || e.tpsm == gs::PSMT4HH;
    uint64_t k[5] = {(e.tbp & 31) | (uint64_t)e.tbw << 32, e.tpsm | (uint64_t)e.tw << 16 | (uint64_t)e.th << 32,
                     indexed ? e.pal_hash : 0,
                     e.tpsm == gs::PSMCT32 ? 0 : (e.ta0 | e.ta1 << 8 | (uint64_t)e.aem << 16), 0};
    k[4] = e.tbp;
    const uint64_t addr_key = XXH3_64bits(k, sizeof k);
    AddrEntry &ae = g_r.by_addr[addr_key];
    if (ae.tex) {
        bool ok = true;
        for (uint32_t i = 0; i < count && ok; i++)
            if (gs::g_page_gen[(first + i) % gs::kPages] > ae.stamp) ok = false;
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
        for (int y = 0; y < (int)e.th; y += bh)
            for (int x = 0; x < (int)e.tw; x += bw) {
                uint32_t byte;
                switch (e.tpsm) {
                case gs::PSMCT16: byte = gs::addr16(x, y, e.tbp, e.tbw) * 2; break;
                case gs::PSMCT16S: byte = gs::addr16s(x, y, e.tbp, e.tbw) * 2; break;
                case gs::PSMT8: byte = gs::addr8(x, y, e.tbp, e.tbw); break;
                case gs::PSMT4: byte = gs::addr4(x, y, e.tbp, e.tbw) / 2; break;
                default: byte = gs::addr32(x, y, e.tbp, e.tbw) * 4; break;
                }
                XXH3_64bits_update(st, gs::vram + ((byte & ~255u) & (gs::kVramSize - 1)), 256);
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
    const uint32_t row = (t.w * 4 + 255) & ~255u;
    t.staging = g_r.dev->createBuffer(plume::RenderBufferDesc::UploadBuffer((uint64_t)row * t.h));
    auto *dst = static_cast<uint8_t *>(t.staging->map());
    for (uint32_t v = 0; v < t.h; v++) {
        uint32_t *line = reinterpret_cast<uint32_t *>(dst + (size_t)v * row);
        for (uint32_t u = 0; u < t.w; u++) line[u] = gs::tex_fetch(e, (int)u, (int)v, 0);
    }
    t.staging->unmap();
    t.tex = g_r.dev->createTexture(plume::RenderTextureDesc::Texture2D(t.w, t.h, 1, plume::RenderFormat::R8G8B8A8_UNORM));
    t.pending_upload = true;
    g_r.uploads.push_back(&t);
    g_r.texture_bytes += (uint64_t)t.w * t.h * 4;
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
            for (int i = 0; i < 8; i++) tr.sets[i] = std::move(t->sets[i]);
            g_r.trash.push_back(std::move(tr));
            g_r.texture_bytes -= (uint64_t)t->w * t->h * 4;
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
        const uint32_t row = (t->w * 4 + 255) & ~255u;
        cl->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(t->tex.get()),
                              plume::RenderTextureCopyLocation::PlacedFootprint(t->staging.get(), plume::RenderFormat::R8G8B8A8_UNORM,
                                                                                t->w, t->h, 1, row / 4));
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
            for (int i = 0; i < 8; i++) t.sets[i] = std::move(g_r.shadow_mask.sets[i]);
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
    ensure_shadow_resources(s.width, s.height);
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

    // Pass 1: accumulate extruded shadow volumes into full-RenderScale shadow_mask against main D32 depth
    plume::RenderTextureBarrier in1[2] = {plume::RenderTextureBarrier(smask, plume::RenderTextureLayout::COLOR_WRITE),
                                          plume::RenderTextureBarrier(depth, plume::RenderTextureLayout::DEPTH_WRITE)};
    cl->barriers(plume::RenderBarrierStage::GRAPHICS, in1, 2);
    if (!g_r.shadow_fb || g_r.shadow_fb_depth != s.depth) {
        if (g_r.shadow_fb) {
            Trash t{g_r.vb};
            t.fb = std::move(g_r.shadow_fb);
            g_r.trash.push_back(std::move(t));
        }
        const plume::RenderTexture *ca[1] = {smask};
        g_r.shadow_fb = g_r.dev->createFramebuffer(plume::RenderFramebufferDesc(ca, 1, depth));
        g_r.shadow_fb_depth = s.depth;
    }
    cl->setFramebuffer(g_r.shadow_fb.get());
    cl->setViewports(plume::RenderViewport(0, 0, (float)s.width, (float)s.height));
    cl->clearColor(0, plume::RenderColor(0, 0, 0, 0));
    cl->setGraphicsPipelineLayout(g_r.layout.get());
    const double gw = s.gs_width, gh = s.gs_height;
    const int sc = s.scale;
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
        const double kx = (0.5 - it.ox) * 2.0 / gw - 1.0, ky = (0.5 - it.oy) * 2.0 / gh - 1.0;
        for (int c = 0; c < 4; c++) {
            pc.cx[c] = (float)(it.a[0][c] * 2.0 / gw + it.a[3][c] * kx);
            pc.cy[c] = (float)(it.a[1][c] * 2.0 / gh + it.a[3][c] * ky);
            pc.cz[c] = (float)(it.a[2][c] * it.zscale);
            pc.cw[c] = (float)it.a[3][c];
        }
        if (it.pipeline != bound) {
            cl->setPipeline(pipeline(it.pipeline));
            bound = it.pipeline;
        }
        cl->setGraphicsDescriptorSet(texture_set(&g_r.white, 0), 0);
        cl->setGraphicsPushConstants(0, &pc, 0, sizeof pc);
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
        cl->drawIndexedInstanced((uint32_t)m.cpu->indices.size(), 1, 0, 0, 0);
        fs.drawn++;
        fs.k_drawn[KIND_SHADOW]++;
    }
    cl->setFramebuffer(nullptr);

    // Pass 2: composite shadow_mask onto main framebuffer color (0x362E70: AEM=1, ALPHA_1=0x44, FIX=intensity)
    if (intensity > 0) {
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
        // B_MIX, no depth, RGB write mask (0x7), no cull
        const uint32_t comp_pipe = B_MIX | (0x7u << 8);
        cl->setPipeline(pipeline(comp_pipe));
        cl->setGraphicsDescriptorSet(texture_set(&g_r.shadow_mask, 2 | 4), 0);  // nearest + clamp
        PushConstants cpc{};
        cpc.cx[0] = 1.0f; cpc.cy[1] = 1.0f; cpc.cz[2] = 1.0f; cpc.cw[3] = 1.0f;
        cpc.ambient[3] = 128.0f;  // va = 128 -> a = floor(t.a * 128 / 128) = t.a
        // M_TME | M_TCC | TFX=0(modulate) | M_ATE | ATST=6(GREATER) | M_AFAIL_DISCARD | M_CFIX
        cpc.mode[0] = M_TME | M_TCC | M_ATE | (6u << M_ATST_SHIFT) | M_AFAIL_DISCARD | M_CFIX;
        cpc.mode[1] = 0;  // aref = 0 (discard when shadow_mask.a == 0)
        cpc.mode[2] = intensity & 0xFFu;
        cl->setGraphicsPushConstants(0, &cpc, 0, sizeof cpc);
        const int32_t x0 = std::max(0, comp_sc[0]) * sc, y0 = std::max(0, comp_sc[1]) * sc;
        const int32_t x1 = std::min((int32_t)gw, comp_sc[2] + 1) * sc, y1 = std::min((int32_t)gh, comp_sc[3] + 1) * sc;
        cl->setScissors(plume::RenderRect(x0, y0, std::max(x0, x1), std::max(y0, y1)));
        const plume::RenderVertexBufferView qvbv(g_r.quad_buf->at(0), (uint32_t)(4 * sizeof(Vertex)));
        const plume::RenderInputSlot qslot(0, sizeof(Vertex));
        cl->setVertexBuffers(0, &qvbv, 1, &qslot);
        const plume::RenderIndexBufferView qibv(g_r.quad_buf->at(4 * sizeof(Vertex)), 6 * 4, plume::RenderFormat::R32_UINT);
        cl->setIndexBuffer(&qibv);
        cl->drawIndexedInstanced(6, 1, 0, 0, 0);
        cl->setFramebuffer(nullptr);
    }

    plume::RenderTextureBarrier out[2] = {plume::RenderTextureBarrier(color, plume::RenderTextureLayout::GENERAL),
                                          plume::RenderTextureBarrier(depth, plume::RenderTextureLayout::GENERAL)};
    cl->barriers(plume::RenderBarrierStage::ALL, out, 2);
    vl->vk = own;
    gs::hw::native_end();
    g_r.shadow_pending.clear();
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
    auto &fb = g_r.framebuffers[{s.color, s.depth}];
    if (!fb) {
        const plume::RenderTexture *ca[1] = {color};
        fb = g_r.dev->createFramebuffer(plume::RenderFramebufferDesc(ca, 1, depth));
    }
    cl->setFramebuffer(fb.get());
    cl->setViewports(plume::RenderViewport(0, 0, (float)s.width, (float)s.height));
    cl->setGraphicsPipelineLayout(g_r.layout.get());
    const double gw = s.gs_width, gh = s.gs_height;
    const int sc = s.scale;
    uint32_t bound = ~0u;
    for (Item &it : g_r.pending) {
        FrameStats &fs = g_frames[it.p.frame];
        if (!it.mesh || !it.mesh->buf) {
            fs.drawn++;
            fs.k_drawn[it.p.kind % KIND_COUNT]++;
            fs.empty++;
            continue;
        }
        if (!tg.depth) it.pipeline &= ~0xF0u;
        // GS screen -> this surface's clip space (the backend's convention: ndc = (p + 0.5) * 2 / size - 1)
        PushConstants &pc = it.pc;
        const double kx = (0.5 - it.ox) * 2.0 / gw - 1.0, ky = (0.5 - it.oy) * 2.0 / gh - 1.0;
        for (int c = 0; c < 4; c++) {
            pc.cx[c] = (float)(it.a[0][c] * 2.0 / gw + it.a[3][c] * kx);
            pc.cy[c] = (float)(it.a[1][c] * 2.0 / gh + it.a[3][c] * ky);
            pc.cz[c] = (float)(it.a[2][c] * it.zscale);
            pc.cw[c] = (float)it.a[3][c];
        }
        if (it.pipeline != bound) {
            cl->setPipeline(pipeline(it.pipeline));
            bound = it.pipeline;
        }
        Texture *t = it.tex ? it.tex : &g_r.white;
        cl->setGraphicsDescriptorSet(texture_set(t, it.tex ? it.sampler : 0), 0);
        cl->setGraphicsPushConstants(0, &pc, 0, sizeof pc);
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
        cl->drawIndexedInstanced((uint32_t)m.cpu->indices.size(), 1, 0, 0, 0);
        fs.drawn++;
        fs.k_drawn[it.p.kind % KIND_COUNT]++;
        if (it.p.kind == KIND_WATER) water_view_drawn(m.cpu->indices.size() / 3, it.sc, it.ox, it.oy);
        if (it.tex) fs.textured++;
        if (m.cpu->skinned) fs.skinned++;
        if (m.cpu->posed) fs.posed++;
    }
    cl->setFramebuffer(nullptr);  // ends the render pass
    plume::RenderTextureBarrier out[2] = {plume::RenderTextureBarrier(color, plume::RenderTextureLayout::GENERAL),
                                          plume::RenderTextureBarrier(depth, plume::RenderTextureLayout::GENERAL)};
    cl->barriers(plume::RenderBarrierStage::ALL, out, depth ? 2 : 1);
    vl->vk = own;
    gs::hw::native_end();
    g_r.pending.clear();
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
        PushConstants &pc = it.pc;
        pc.ambient[3] = 15.0f;  // va = 15 (accumulated in shadow_mask.a)
        pc.mode[0] = M_CFIX;
        pc.mode[2] = 128;       // FIX = 1.0
        // pass 1 (front faces): cull BACK (1), B_ADD; pass 0 (back faces): cull FRONT (2), B_REVSUB
        const bool front = (g_cur.itop & 1u) != 0;
        const uint32_t blend = front ? B_ADD : B_REVSUB;
        const uint32_t cull = front ? 1u : 2u;
        // zf = 1 (GEQUAL), zwrite = 0, depth = 1, color mask = 0xF, blend_alpha = 1
        it.pipeline = blend | (1u << 4) | (0u << 6) | (1u << 7) | (0xFu << 8) | (cull << 12) | (1u << 14);
        for (int i = 0; i < 4; i++) it.sc[i] = g_r.main_sc[i];
        g_r.shadow_pending.push_back(it);
        return;
    }
    if (!g_r.shadow_pending.empty()) flush_shadows();
    const bool zwrite = !e.zmsk && e.ztst != 0, ztest = e.ztst >= 2;
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
    it.tex = texture(e, fs);
    uint32_t m = 0;
    if (it.tex) {
        m |= M_TME | (e.tcc ? M_TCC : 0) | (uint32_t)(e.tfx & 3) << M_TFX_SHIFT;
        it.sampler = (e.bilinear_mag ? 1u : 0u) | (e.wms != 0 ? 2u : 0u) | (e.wmt != 0 ? 4u : 0u);
    }
    if (e.ate) {
        m |= M_ATE | (uint32_t)(e.atst & 7) << M_ATST_SHIFT;
        if (e.afail == 0 || e.afail == 2) m |= M_AFAIL_DISCARD;
    }
    if (e.bc == 2) m |= M_CFIX;
    if (g_cur.program == 0x2E0) m |= M_SPEC;  // car bodies: vertex alpha is the highlight
    pc.mode[1] = e.aref & 0xFF;
    pc.mode[2] = e.bfix & 0xFF;
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
    // pipeline key: blend | z function << 4 | z write << 6 | depth << 7 | colour mask << 8
    const uint32_t zf = !ztest ? (e.ztst == 0 ? 3u : 0u) : e.ztst == 2 ? 1u : 2u;
    const uint32_t full = (e.fpsm & 0xF) == 0x1 ? 0xFFFFFFu : 0xFFFFFFFFu;
    uint32_t mask = 0;
    for (int ch = 0; ch < 4; ch++) {
        const uint32_t bits = 0xFFu << (ch * 8);
        if ((full & bits) && (e.fbmsk & bits) != bits) mask |= 1u << ch;
    }
    it.pipeline = blend_of(e) | zf << 4 | (zwrite ? 1u : 0u) << 6 | (tg.depth ? 1u : 0u) << 7 | mask << 8;
    it.sc[0] = e.scx0, it.sc[1] = e.scy0, it.sc[2] = e.scx1, it.sc[3] = e.scy1;
    g_r.pending.push_back(it);
}

void end_vblank(uint64_t vb) {
    g_r.vb = vb;
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

void gs_vblank(uint64_t frame) {
    if (g_active) finalize();
    g_active = false;
    flush();
    end_vblank(frame);
    report(frame);
}

uint64_t fog_fge_draws() { return g_fge_draws.load(); }

}  // namespace native
