// runtime/remaster/remaster_renderer.cpp
// Self-contained Plume Forward+ Linear HDR PBR Renderer (Halo 2 Anniversary / Saber3D architecture).
// ZERO #include of runtime/gs/*, runtime/vu/*, or vif_mesh.h.
// ZERO PS2 GS register flags, ZERO 8-bit hdr_encode, ZERO fake RemasterPost stack.
#include "remaster_renderer.h"
#include "asset_registry.h"
#include "scene_bridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <vector>

#ifdef EOD_PLUME
#include "../render/plume_device.h"
#include <plume_vulkan.h>
#include "remaster_shaders_spv.inc"
#endif

namespace remaster {

namespace {

std::atomic<uint32_t> g_debug_mode{[]() -> uint32_t {
    if (const char *env = std::getenv("EOD_REMASTER_DEBUG"); env && *env) {
        const int v = std::atoi(env);
        if (v >= 0 && v < static_cast<int>(DebugViewMode::Count)) return static_cast<uint32_t>(v);
    }
    return 0u;
}()};

struct Vec3 {
    double x, y, z;
};

inline Vec3 v_add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 v_sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 v_scale(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double v_dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 v_cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec3 v_unit(Vec3 a) {
    const double len = std::sqrt(v_dot(a, a));
    return len > 1e-9 ? v_scale(a, 1.0 / len) : Vec3{0.0, 1.0, 0.0};
}

struct Frustum {
    float planes[6][4]; // (nx, ny, nz, d) normalized so dot(n, p) + d >= -r means inside
};

Frustum extract_frustum_from_vp(const float m[16]) {
    Frustum f{};
    auto set_plane = [&](int idx, float a, float b, float c, float d) {
        const float len = std::sqrt(a * a + b * b + c * c);
        if (len > 1e-6f) {
            const float inv = 1.0f / len;
            f.planes[idx][0] = a * inv;
            f.planes[idx][1] = b * inv;
            f.planes[idx][2] = c * inv;
            f.planes[idx][3] = d * inv;
        } else {
            f.planes[idx][0] = 0.0f;
            f.planes[idx][1] = 0.0f;
            f.planes[idx][2] = 0.0f;
            f.planes[idx][3] = 1.0f;
        }
    };
    // Row-major Vulkan clip space: -w<=x<=w, -w<=y<=w, 0<=z<=w
    set_plane(0, m[12] + m[0], m[13] + m[1], m[14] + m[2],  m[15] + m[3]);  // Left
    set_plane(1, m[12] - m[0], m[13] - m[1], m[14] - m[2],  m[15] - m[3]);  // Right
    set_plane(2, m[12] + m[4], m[13] + m[5], m[14] + m[6],  m[15] + m[7]);  // Bottom
    set_plane(3, m[12] - m[4], m[13] - m[5], m[14] - m[6],  m[15] - m[7]);  // Top
    set_plane(4, m[8],         m[9],         m[10],         m[11]);         // Near (z >= 0)
    set_plane(5, m[12] - m[8], m[13] - m[9], m[14] - m[10], m[15] - m[11]); // Far  (z <= w)
    return f;
}

inline bool frustum_intersects_sphere(const Frustum &f, float cx, float cy, float cz, float r) {
    for (int i = 0; i < 6; ++i) {
        const float dist = f.planes[i][0] * cx + f.planes[i][1] * cy + f.planes[i][2] * cz + f.planes[i][3];
        if (dist < -r) return false;
    }
    return true;
}

// Build a row-major look-at + perspective matrix for cubemap face rendering (90 deg FOV, +Y up, Vulkan [0,1] Z).
void build_cubemap_face_vp(const float center[3], uint32_t face, float near_z, float far_z, float out_vp[16]) {
    // Standard Vulkan cubemap face basis (+X, -X, +Y, -Y, +Z, -Z) in +Y UP world space
    static constexpr float kDirs[6][3] = {
        { 1,  0,  0}, {-1,  0,  0},
        { 0,  1,  0}, { 0, -1,  0},
        { 0,  0,  1}, { 0,  0, -1},
    };
    static constexpr float kUps[6][3] = {
        { 0,  1,  0}, { 0,  1,  0},
        { 0,  0, -1}, { 0,  0,  1},
        { 0,  1,  0}, { 0,  1,  0},
    };
    const Vec3 fwd = v_unit({kDirs[face][0], kDirs[face][1], kDirs[face][2]});
    const Vec3 up_ref = {kUps[face][0], kUps[face][1], kUps[face][2]};
    const Vec3 right = v_unit(v_cross(up_ref, fwd));
    const Vec3 up = v_cross(fwd, right);
    const Vec3 eye{center[0], center[1], center[2]};

    const float view[16] = {
        (float)right.x, (float)right.y, (float)right.z, (float)-v_dot(right, eye),
        (float)up.x,    (float)up.y,    (float)up.z,    (float)-v_dot(up, eye),
        (float)fwd.x,   (float)fwd.y,   (float)fwd.z,   (float)-v_dot(fwd, eye),
        0.0f,           0.0f,           0.0f,           1.0f
    };
    const float z_range = std::max(1.0f, far_z - near_z);
    const float a = far_z / z_range;
    const float b = -(far_z * near_z) / z_range;
    // Negative proj[5] maps +Y up in view space to -1 (top) in Vulkan NDC
    const float proj[16] = {
        1.0f,  0.0f, 0.0f, 0.0f,
        0.0f, -1.0f, 0.0f, 0.0f,
        0.0f,  0.0f, a,    b,
        0.0f,  0.0f, 1.0f, 0.0f
    };
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += proj[r * 4 + k] * view[k * 4 + c];
            out_vp[r * 4 + c] = s;
        }
    }
}

} // namespace

CsmCascadeSet compute_csm_cascades(const SceneCamera &cam, const SceneEnvironment &env, uint32_t resolution) {
    CsmCascadeSet out{};
    out.splits[0] = 24.0f;
    out.splits[1] = 80.0f;
    out.splits[2] = 260.0f;

    const Vec3 toward_sun = v_unit({env.sun_dir[0], env.sun_dir[1], env.sun_dir[2]});
    // Light ray travels away from the sun
    const Vec3 light_z = v_scale(toward_sun, -1.0);
    const Vec3 ref_up = std::fabs(light_z.y) < 0.96 ? Vec3{0.0, 1.0, 0.0} : Vec3{0.0, 0.0, 1.0};
    const Vec3 light_x = v_unit(v_cross(ref_up, light_z));
    const Vec3 light_y = v_cross(light_z, light_x);

    const Vec3 cam_pos{cam.pos[0], cam.pos[1], cam.pos[2]};
    const Vec3 cam_fwd = v_unit({cam.rotation[6], cam.rotation[7], cam.rotation[8]});

    static constexpr double kRadii[3]     = {26.0, 88.0, 270.0};
    static constexpr double kFocusDist[3] = {12.0, 46.0, 130.0};
    static constexpr double kHalfDepth[3] = {65.0, 160.0, 320.0};
    const double res = static_cast<double>(std::max(1u, resolution));

    for (uint32_t i = 0; i < CsmCascadeSet::kCascadeCount; ++i) {
        const Vec3 focus = v_add(cam_pos, v_scale(cam_fwd, kFocusDist[i]));
        const double texel_world = (2.0 * kRadii[i]) / res;
        const double cx = std::round(v_dot(light_x, focus) / texel_world) * texel_world;
        const double cy = std::round(v_dot(light_y, focus) / texel_world) * texel_world;
        const double cz = std::round(v_dot(light_z, focus));

        const double inv_r = 1.0 / kRadii[i];
        const double inv_2d = 0.5 / kHalfDepth[i];

        float *m = out.view_proj[i];
        // Row-major orthographic light-space matrix mapping world -> NDC [-1,1]x[-1,1]x[0,1]
        m[0]  = (float)(light_x.x * inv_r);
        m[1]  = (float)(light_x.y * inv_r);
        m[2]  = (float)(light_x.z * inv_r);
        m[3]  = (float)(-cx * inv_r);

        m[4]  = (float)(light_y.x * inv_r);
        m[5]  = (float)(light_y.y * inv_r);
        m[6]  = (float)(light_y.z * inv_r);
        m[7]  = (float)(-cy * inv_r);

        m[8]  = (float)(light_z.x * inv_2d);
        m[9]  = (float)(light_z.y * inv_2d);
        m[10] = (float)(light_z.z * inv_2d);
        m[11] = (float)(0.5 - cz * inv_2d);

        m[12] = 0.0f; m[13] = 0.0f; m[14] = 0.0f; m[15] = 1.0f;
    }
    return out;
}

std::array<float, 3> khronos_pbr_neutral_cpu(float r, float g, float b) {
    constexpr float kStartCompression = 0.8f - 0.04f;
    constexpr float kDesaturation = 0.15f;

    float c[3] = {std::max(0.0f, r), std::max(0.0f, g), std::max(0.0f, b)};
    const float x = std::min(c[0], std::min(c[1], c[2]));
    const float offset = (x < 0.08f) ? (x - 6.25f * x * x) : 0.04f;
    for (float &v : c) v -= offset;

    const float peak = std::max(c[0], std::max(c[1], c[2]));
    if (peak < kStartCompression) return {c[0], c[1], c[2]};

    constexpr float d = 1.0f - kStartCompression;
    const float new_peak = 1.0f - d * d / (peak + d - kStartCompression);
    const float scale = new_peak / peak;
    for (float &v : c) v *= scale;

    const float g_fac = 1.0f - 1.0f / (kDesaturation * (peak - new_peak) + 1.0f);
    for (float &v : c) v = v + (new_peak - v) * g_fac;
    return {c[0], c[1], c[2]};
}

DebugViewMode debug_view_mode() {
    return static_cast<DebugViewMode>(g_debug_mode.load(std::memory_order_relaxed));
}

void set_debug_view_mode(DebugViewMode mode) {
    const uint32_t v = static_cast<uint32_t>(mode) % static_cast<uint32_t>(DebugViewMode::Count);
    g_debug_mode.store(v, std::memory_order_relaxed);
    static constexpr const char *kNames[] = {
        "Final", "RawAlbedo", "RoughnessMetallic", "WorldNormals", "DirectSunAndCsm", "ReflectionsOnly"
    };
    std::fprintf(stderr, "[remaster] debug view -> %u (%s)\n", v, kNames[v]);
}

void cycle_debug_view_mode() {
    const uint32_t next = (g_debug_mode.load(std::memory_order_relaxed) + 1u) %
                          static_cast<uint32_t>(DebugViewMode::Count);
    set_debug_view_mode(static_cast<DebugViewMode>(next));
}

#ifdef EOD_PLUME

namespace {

struct alignas(16) FrameUBO {
    float view_proj[16];
    float csm_view_proj[3][16];
    float csm_splits[4];
    float cam_pos_debug[4];
    float sun_dir_intensity[4];
    float sun_color_day[4];
    float ambient_sky_int[4];
    float ambient_ground_pad[4];
    float fog_color_density[4];
    float fog_start_end_lights[4];
    float light_pos_rad[32][4];
    float light_col_int[32][4];
    float light_dir_pad[32][4];
};

struct alignas(16) DrawPC {
    float model[16];
    float base_color[4];
    float pbr_params[4];
    uint32_t flags[4];
};

struct GpuMesh {
    std::unique_ptr<plume::RenderBuffer> vbo;
    std::unique_ptr<plume::RenderBuffer> ibo;
    uint32_t vertex_count = 0;
    uint32_t index_count = 0;
    float center[3] = {0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    std::string texture_id;
    PbrMaterial material;
};

struct GpuTextureSet {
    uint32_t albedo_w = 0;
    bool has_alpha_cutout = false;
    bool has_alpha_blend = false;
    bool has_alpha_additive = false;
    float resolved_emissive_scale = 0.0f;
    std::unique_ptr<plume::RenderTexture> albedo, normal, roughness, metallic, ao;
    std::unique_ptr<plume::RenderDescriptorSet> ds;
};

struct StaticBatch {
    std::unique_ptr<plume::RenderBuffer> vbo;
    std::unique_ptr<plume::RenderBuffer> ibo;
    uint32_t vertex_count = 0;
    uint32_t index_count = 0;
    float center[3] = {0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    std::string texture_id;
    PbrMaterial material;
    uint32_t alpha_mode = 0; // 0=opaque, 1=cutout, 2=alpha_blend, 3=additive
    uint32_t optional_terrain_mask = 0;
    bool is_optional_terrain = false;
    bool is_sky = false;
    bool is_foliage = false;
    bool cast_shadow = true;
    bool double_sided = false;
};

struct PosedVboSlice {
    plume::RenderBuffer *buf = nullptr;
    uint64_t offset = 0;
};

struct HudVertex {
    float pos[2];
    float uv[2];
    float color[4];
};

struct OutputCacheEntry {
    std::unique_ptr<plume::RenderTexture> wrapper;
    std::unique_ptr<plume::RenderFramebuffer> fb;
};

struct RetiredResource {
    uint64_t frame_id = 0;
    std::unique_ptr<plume::RenderBuffer> buf;
    std::unique_ptr<plume::RenderTexture> tex;
    std::unique_ptr<plume::RenderFramebuffer> fb;
    std::unique_ptr<plume::RenderDescriptorSet> ds;
};

struct RemasterEngine {
    static constexpr uint32_t kCubeRes = 128;
    static constexpr uint32_t kFramesInFlight = 3;
    static constexpr uint64_t kDynVboSlotBytes = 8ull * 1024ull * 1024ull;
    static constexpr uint64_t kRetireFrames = 8;
    bool attempted = false;
    bool ready = false;
    plume::VulkanDevice *dev = nullptr;
    // Keep command queue alive for the entire lifetime of `list` (owning VkCommandPool!)
    std::unique_ptr<plume::RenderCommandQueue> queue;
    std::unique_ptr<plume::RenderCommandList> list;

    // Samplers
    std::unique_ptr<plume::RenderSampler> samp_linear;
    std::unique_ptr<plume::RenderSampler> samp_shadow;

    // Layouts & Pipelines
    std::unique_ptr<plume::RenderPipelineLayout> pbr_layout;
    std::unique_ptr<plume::RenderPipelineLayout> resolve_layout;
    std::unique_ptr<plume::RenderPipeline> pipe_csm_opaque;
    std::unique_ptr<plume::RenderPipeline> pipe_csm_alpha;
    std::unique_ptr<plume::RenderPipeline> pipe_cubemap;
    std::unique_ptr<plume::RenderPipeline> pipe_cubemap_sky;
    std::unique_ptr<plume::RenderPipeline> pipe_pbr_opaque;
    std::unique_ptr<plume::RenderPipeline> pipe_pbr_double_sided;
    std::unique_ptr<plume::RenderPipeline> pipe_pbr_alpha_blend;
    std::unique_ptr<plume::RenderPipeline> pipe_pbr_additive;
    std::unique_ptr<plume::RenderPipeline> pipe_pbr_sky;
    std::unique_ptr<plume::RenderPipeline> pipe_resolve;
    std::unique_ptr<plume::RenderPipeline> pipe_hud;

    // Pass 1: 3-Cascade CSM Shadow Maps (2048x2048 D32_FLOAT)
    std::unique_ptr<plume::RenderTexture> csm_depth[CsmCascadeSet::kCascadeCount];
    std::unique_ptr<plume::RenderFramebuffer> csm_fb[CsmCascadeSet::kCascadeCount];

    // Pass 2: Dynamic 128x128 Environment Reflection Cubemap (R16G16B16A16_FLOAT + D32_FLOAT)
    std::unique_ptr<plume::RenderTexture> env_cube;
    std::unique_ptr<plume::RenderTextureView> env_cube_view;
    std::unique_ptr<plume::RenderTextureView> env_face_views[6];
    std::unique_ptr<plume::RenderTexture> env_cube_depth;
    std::unique_ptr<plume::RenderFramebuffer> env_face_fb[6];
    uint32_t cube_face_cursor = 0;
    bool cube_faces_initialized = false;

    // Pass 3: True Linear HDR Framebuffer (R16G16B16A16_FLOAT) + D32_FLOAT Depth
    uint32_t hdr_w = 0, hdr_h = 0;
    std::unique_ptr<plume::RenderTexture> hdr_color;
    std::unique_ptr<plume::RenderTexture> hdr_depth;
    std::unique_ptr<plume::RenderFramebuffer> hdr_fb;

    // Pass 4: Cached Neutral Resolve + HUD Output Wrappers keyed by (vk_color_image, w, h)
    std::unordered_map<uint64_t, OutputCacheEntry> out_cache;
    plume::RenderTexture *cur_out_wrapper = nullptr;
    plume::RenderFramebuffer *cur_out_fb = nullptr;
    std::unique_ptr<plume::RenderDescriptorSet> resolve_ds;

    // Ring-buffered per-frame UBOs, descriptor sets, HUD VBO, and posed dynamic VBO (zero per-frame allocs!)
    std::unique_ptr<plume::RenderBuffer> frame_ubo[kFramesInFlight];
    std::unique_ptr<plume::RenderBuffer> cube_ubo[kFramesInFlight][6];
    std::unique_ptr<plume::RenderDescriptorSet> frame_ds[kFramesInFlight];
    std::unique_ptr<plume::RenderDescriptorSet> cube_ds[kFramesInFlight][6];
    std::unique_ptr<plume::RenderBuffer> hud_vbo[kFramesInFlight];
    std::unique_ptr<plume::RenderBuffer> dyn_vbo[kFramesInFlight];

    // Batched static world mega-VBO/IBOs + dynamic mesh/texture GPU caches & safe retirement queue
    std::vector<StaticBatch> static_batches;
    uint32_t batched_track_gen = 0;
    bool batches_have_all_tex = false;
    std::unordered_map<uint32_t, std::unique_ptr<GpuMesh>> gpu_meshes;
    std::unordered_map<std::string, std::unique_ptr<GpuTextureSet>> gpu_textures;
    std::vector<RetiredResource> retired;
    uint64_t render_frame_id = 0;
    uint32_t uploaded_track_gen = 0;
    bool logged_first_draw = false;
    double frame_ms_sum = 0.0;
    double frame_ms_max = 0.0;
    uint32_t frame_ms_count = 0;
};

RemasterEngine g_eng;

plume::RenderDescriptorSetDesc frame_ds_desc() {
    using namespace plume;
    static const RenderDescriptorRange ranges[] = {
        {RenderDescriptorRangeType::CONSTANT_BUFFER, 0, 1},
        {RenderDescriptorRangeType::TEXTURE, 1, 1},
        {RenderDescriptorRangeType::TEXTURE, 2, 1},
        {RenderDescriptorRangeType::TEXTURE, 3, 1},
        {RenderDescriptorRangeType::TEXTURE, 4, 1},
        {RenderDescriptorRangeType::SAMPLER, 5, 1},
        {RenderDescriptorRangeType::SAMPLER, 6, 1},
    };
    return RenderDescriptorSetDesc(ranges, 7);
}

plume::RenderDescriptorSetDesc material_ds_desc() {
    using namespace plume;
    static const RenderDescriptorRange ranges[] = {
        {RenderDescriptorRangeType::TEXTURE, 0, 1},
        {RenderDescriptorRangeType::TEXTURE, 1, 1},
        {RenderDescriptorRangeType::TEXTURE, 2, 1},
        {RenderDescriptorRangeType::TEXTURE, 3, 1},
        {RenderDescriptorRangeType::TEXTURE, 4, 1},
    };
    return RenderDescriptorSetDesc(ranges, 5);
}

plume::RenderDescriptorSetDesc resolve_ds_desc() {
    using namespace plume;
    static const RenderDescriptorRange ranges[] = {
        {RenderDescriptorRangeType::TEXTURE, 0, 1},
        {RenderDescriptorRangeType::SAMPLER, 1, 1},
    };
    return RenderDescriptorSetDesc(ranges, 2);
}

void retire_buffer(std::unique_ptr<plume::RenderBuffer> b) {
    if (!b) return;
    RetiredResource r{};
    r.frame_id = g_eng.render_frame_id;
    r.buf = std::move(b);
    g_eng.retired.push_back(std::move(r));
}

void retire_texture(std::unique_ptr<plume::RenderTexture> t) {
    if (!t) return;
    RetiredResource r{};
    r.frame_id = g_eng.render_frame_id;
    r.tex = std::move(t);
    g_eng.retired.push_back(std::move(r));
}

void retire_fb(std::unique_ptr<plume::RenderFramebuffer> f) {
    if (!f) return;
    RetiredResource r{};
    r.frame_id = g_eng.render_frame_id;
    r.fb = std::move(f);
    g_eng.retired.push_back(std::move(r));
}

void retire_ds(std::unique_ptr<plume::RenderDescriptorSet> d) {
    if (!d) return;
    RetiredResource r{};
    r.frame_id = g_eng.render_frame_id;
    r.ds = std::move(d);
    g_eng.retired.push_back(std::move(r));
}

void gc_retired_resources() {
    auto &vec = g_eng.retired;
    vec.erase(std::remove_if(vec.begin(), vec.end(),
                             [](const RetiredResource &r) {
                                 return g_eng.render_frame_id > r.frame_id + RemasterEngine::kRetireFrames;
                             }),
              vec.end());
}

// MUST only be called OUTSIDE any active render pass (in the pre-pass before Pass 1).
std::unique_ptr<plume::RenderTexture> upload_rgba8_texture(plume::RenderCommandList &list,
                                                           const CpuImageRgba8 &img) {
    using namespace plume;
    if (img.width == 0 || img.height == 0 || img.pixels.empty()) return nullptr;
    auto tex = g_eng.dev->createTexture(
        RenderTextureDesc::Texture2D(img.width, img.height, 1, RenderFormat::R8G8B8A8_UNORM));
    if (!tex) return nullptr;
    const size_t bytes = img.pixels.size();
    auto stage = g_eng.dev->createBuffer(RenderBufferDesc::UploadBuffer(bytes, RenderBufferFlag::NONE));
    if (!stage) return nullptr;
    if (void *p = stage->map()) {
        std::memcpy(p, img.pixels.data(), bytes);
        stage->unmap();
    }
    list.barriers(RenderBarrierStage::COPY, RenderTextureBarrier(tex.get(), RenderTextureLayout::COPY_DEST));
    list.copyTextureRegion(RenderTextureCopyLocation::Subresource(tex.get()),
                           RenderTextureCopyLocation::PlacedFootprint(
                               stage.get(), RenderFormat::R8G8B8A8_UNORM, img.width, img.height, 1, img.width));
    list.barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(tex.get(), RenderTextureLayout::SHADER_READ));
    // Keep staging buffer alive until GPU finishes executing the copy!
    retire_buffer(std::move(stage));
    return tex;
}

// MUST only be called OUTSIDE any active render pass (in the pre-pass before Pass 1).
GpuTextureSet *ensure_gpu_texture_set(plume::RenderCommandList &list, const std::string &tex_id,
                                      bool is_sky, bool is_vehicle) {
    auto it = g_eng.gpu_textures.find(tex_id);
    if (it != g_eng.gpu_textures.end() && it->second->albedo_w > 1) return it->second.get();

    auto cpu = AssetRegistry::instance().resolve_texture(tex_id, is_sky, is_vehicle);
    if (!cpu) return it != g_eng.gpu_textures.end() ? it->second.get() : nullptr;
    if (it != g_eng.gpu_textures.end() && cpu->albedo.width <= it->second->albedo_w)
        return it->second.get();

    auto set = std::make_unique<GpuTextureSet>();
    set->albedo_w  = cpu->albedo.width;
    set->has_alpha_cutout = cpu->has_alpha_cutout;
    set->has_alpha_blend  = cpu->has_alpha_blend;
    set->has_alpha_additive = cpu->has_alpha_additive;
    set->resolved_emissive_scale = cpu->material.emissive_scale;
    set->albedo    = upload_rgba8_texture(list, cpu->albedo);
    set->normal    = upload_rgba8_texture(list, cpu->normal);
    set->roughness = upload_rgba8_texture(list, cpu->roughness);
    set->metallic  = upload_rgba8_texture(list, cpu->metallic);
    set->ao        = upload_rgba8_texture(list, cpu->ao);
    set->ds        = g_eng.dev->createDescriptorSet(material_ds_desc());
    if (!set->albedo || !set->normal || !set->roughness || !set->metallic || !set->ao || !set->ds)
        return nullptr;

    const auto sr = plume::RenderTextureLayout::SHADER_READ;
    set->ds->setTexture(0, set->albedo.get(), sr);
    set->ds->setTexture(1, set->normal.get(), sr);
    set->ds->setTexture(2, set->roughness.get(), sr);
    set->ds->setTexture(3, set->metallic.get(), sr);
    set->ds->setTexture(4, set->ao.get(), sr);

    // If upgrading an existing fallback texture set, retire old GPU objects safely.
    if (it != g_eng.gpu_textures.end() && it->second) {
        retire_texture(std::move(it->second->albedo));
        retire_texture(std::move(it->second->normal));
        retire_texture(std::move(it->second->roughness));
        retire_texture(std::move(it->second->metallic));
        retire_texture(std::move(it->second->ao));
        retire_ds(std::move(it->second->ds));
    }

    auto *raw = set.get();
    g_eng.gpu_textures[tex_id] = std::move(set);
    return raw;
}

// Pure read-only lookup safe to call INSIDE an active render pass (zero barriers/copies).
GpuTextureSet *lookup_gpu_texture_set(const std::string &tex_id) {
    auto it = g_eng.gpu_textures.find(tex_id);
    return (it != g_eng.gpu_textures.end()) ? it->second.get() : nullptr;
}

GpuMesh *ensure_gpu_mesh(uint32_t mesh_handle) {
    auto it = g_eng.gpu_meshes.find(mesh_handle);
    if (it != g_eng.gpu_meshes.end()) return it->second.get();

    auto cpu = AssetRegistry::instance().get_mesh(mesh_handle);
    if (!cpu || cpu->vertices.empty() || cpu->indices.empty()) return nullptr;

    using namespace plume;
    const size_t vb_bytes = cpu->vertices.size() * sizeof(PbrVertex);
    const size_t ib_bytes = cpu->indices.size() * sizeof(uint32_t);
    auto vbo = g_eng.dev->createBuffer(RenderBufferDesc::UploadBuffer(vb_bytes, RenderBufferFlag::VERTEX));
    auto ibo = g_eng.dev->createBuffer(RenderBufferDesc::UploadBuffer(ib_bytes, RenderBufferFlag::INDEX));
    if (!vbo || !ibo) return nullptr;

    if (void *p = vbo->map()) { std::memcpy(p, cpu->vertices.data(), vb_bytes); vbo->unmap(); }
    if (void *p = ibo->map()) { std::memcpy(p, cpu->indices.data(), ib_bytes); ibo->unmap(); }

    auto gm = std::make_unique<GpuMesh>();
    gm->vbo = std::move(vbo);
    gm->ibo = std::move(ibo);
    gm->vertex_count = static_cast<uint32_t>(cpu->vertices.size());
    gm->index_count = static_cast<uint32_t>(cpu->indices.size());
    gm->center[0] = cpu->center[0];
    gm->center[1] = cpu->center[1];
    gm->center[2] = cpu->center[2];
    gm->radius = cpu->radius;
    gm->texture_id = cpu->texture_id;
    gm->material = cpu->material;

    auto *raw = gm.get();
    g_eng.gpu_meshes[mesh_handle] = std::move(gm);
    return raw;
}

bool ensure_hdr_targets(uint32_t w, uint32_t h) {
    if (w < 8 || h < 8) return false;
    if (g_eng.hdr_w == w && g_eng.hdr_h == h && g_eng.hdr_fb && g_eng.resolve_ds) return true;

    using namespace plume;
    retire_fb(std::move(g_eng.hdr_fb));
    retire_texture(std::move(g_eng.hdr_color));
    retire_texture(std::move(g_eng.hdr_depth));
    retire_ds(std::move(g_eng.resolve_ds));

    g_eng.hdr_color = g_eng.dev->createTexture(RenderTextureDesc::ColorTarget(w, h, RenderFormat::R16G16B16A16_FLOAT));
    g_eng.hdr_depth = g_eng.dev->createTexture(RenderTextureDesc::DepthTarget(w, h, RenderFormat::D32_FLOAT));
    if (!g_eng.hdr_color || !g_eng.hdr_depth) return false;

    const RenderTexture *c_att[] = {g_eng.hdr_color.get()};
    g_eng.hdr_fb = g_eng.dev->createFramebuffer(RenderFramebufferDesc(c_att, 1, g_eng.hdr_depth.get()));
    g_eng.resolve_ds = g_eng.dev->createDescriptorSet(resolve_ds_desc());
    if (!g_eng.hdr_fb || !g_eng.resolve_ds) return false;

    g_eng.resolve_ds->setTexture(0, g_eng.hdr_color.get(), RenderTextureLayout::SHADER_READ);
    g_eng.resolve_ds->setSampler(1, g_eng.samp_linear.get());
    g_eng.hdr_w = w;
    g_eng.hdr_h = h;
    return true;
}

bool ensure_output_wrapper(const OutputTarget &target) {
    if (!target.vk_color_image || target.width < 8 || target.height < 8) return false;

    using namespace plume;
    const uint64_t key = target.vk_color_image ^
                         (static_cast<uint64_t>(target.width) << 48) ^
                         (static_cast<uint64_t>(target.height) << 32);
    auto &entry = g_eng.out_cache[key];
    if (!entry.wrapper || !entry.fb) {
        auto wrap = std::make_unique<VulkanTexture>(g_eng.dev, reinterpret_cast<VkImage>(target.vk_color_image));
        wrap->desc = RenderTextureDesc::Texture2D(target.width, target.height, 1, RenderFormat::R8G8B8A8_UNORM,
                                                  RenderTextureFlag::RENDER_TARGET);
        wrap->imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
        wrap->fillSubresourceRange();
        wrap->createImageView(wrap->imageFormat);

        const RenderTexture *c_att[] = {wrap.get()};
        auto fb = g_eng.dev->createFramebuffer(RenderFramebufferDesc(c_att, 1));
        if (!fb) return false;
        entry.wrapper = std::move(wrap);
        entry.fb = std::move(fb);
    }
    auto *vk_tex = static_cast<VulkanTexture *>(entry.wrapper.get());
    vk_tex->textureLayout = RenderTextureLayout::GENERAL;
    vk_tex->barrierStages = RenderBarrierStage::ALL;
    g_eng.cur_out_wrapper = entry.wrapper.get();
    g_eng.cur_out_fb = entry.fb.get();
    return g_eng.cur_out_wrapper && g_eng.cur_out_fb;
}

// Build a posed vertex slice inside the persistent per-frame ring dynamic VBO (ZERO per-frame createBuffer calls!).
// Called once per frame in the pre-pass BEFORE Pass 1 so shadows, cubemap, and main pass all use it.
PosedVboSlice build_posed_vbo(const EntityInstance &ent, uint32_t mesh_handle,
                              plume::RenderBuffer *dyn_buf, uint8_t *dyn_base, uint64_t &dyn_cursor) {
    if (!dyn_buf || !dyn_base) return {};
    if (ent.dent_offsets.empty() && ent.bone_palette.empty()) return {};
    auto cpu = AssetRegistry::instance().get_mesh(mesh_handle);
    if (!cpu || cpu->vertices.empty() || cpu->skin.size() != cpu->vertices.size()) return {};

    static thread_local std::vector<PbrVertex> posed;
    posed = cpu->vertices;
    bool moved = false;

    if (!ent.bone_palette.empty()) {
        const size_t n_bones = ent.bone_palette.size() / 16u;
        for (size_t i = 0; i < posed.size(); ++i) {
            const auto &sk = cpu->skin[i];
            if (sk.mode != 2u) continue;
            const float x = posed[i].pos[0], y = posed[i].pos[1], z = posed[i].pos[2];
            const float nx = posed[i].normal[0], ny = posed[i].normal[1], nz = posed[i].normal[2];
            float px = 0.0f, py = 0.0f, pz = 0.0f;
            float rnx = 0.0f, rny = 0.0f, rnz = 0.0f;
            float wsum = 0.0f;
            for (int j = 0; j < 2; ++j) {
                const float w = sk.weight[j];
                if (w <= 0.0f) continue;
                const size_t bi = static_cast<size_t>(sk.bone[j]) / 4u;
                if (bi >= n_bones) continue;
                const float *m = ent.bone_palette.data() + bi * 16u;
                // bone_pose.cpp stores 4 column vectors (c0=m[0..3], c1=m[4..7], c2=m[8..11], c3=m[12..15])
                px  += w * (m[0] * x + m[4] * y + m[8]  * z + m[12]);
                py  += w * (m[1] * x + m[5] * y + m[9]  * z + m[13]);
                pz  += w * (m[2] * x + m[6] * y + m[10] * z + m[14]);
                rnx += w * (m[0] * nx + m[4] * ny + m[8]  * nz);
                rny += w * (m[1] * nx + m[5] * ny + m[9]  * nz);
                rnz += w * (m[2] * nx + m[6] * ny + m[10] * nz);
                wsum += w;
            }
            if (wsum > 1e-4f) {
                posed[i].pos[0] = px; posed[i].pos[1] = py; posed[i].pos[2] = pz;
                const float nlen = std::sqrt(rnx * rnx + rny * rny + rnz * rnz);
                if (nlen > 1e-5f) {
                    posed[i].normal[0] = rnx / nlen;
                    posed[i].normal[1] = rny / nlen;
                    posed[i].normal[2] = rnz / nlen;
                }
                moved = true;
            }
        }
    } else if (!ent.dent_offsets.empty()) {
        const size_t n_dents = ent.dent_offsets.size() / 3u;
        for (size_t i = 0; i < posed.size(); ++i) {
            const auto &sk = cpu->skin[i];
            if (sk.mode != 1u) continue;
            float dx = 0.0f, dy = 0.0f, dz = 0.0f;
            for (int j = 0; j < 2; ++j) {
                if (sk.bone[j] == 0u) continue;
                const size_t di = static_cast<size_t>(sk.bone[j] - 1u);
                if (di >= n_dents) continue;
                const float ox = ent.dent_offsets[di * 3u + 0u];
                const float oy = ent.dent_offsets[di * 3u + 1u];
                const float oz = ent.dent_offsets[di * 3u + 2u];
                if (std::isfinite(ox) && std::fabs(ox) <= 2.5f) dx += sk.weight[j] * ox;
                if (std::isfinite(oy) && std::fabs(oy) <= 2.5f) dy += sk.weight[j] * oy;
                if (std::isfinite(oz) && std::fabs(oz) <= 2.5f) dz += sk.weight[j] * oz;
            }
            if (dx * dx + dy * dy + dz * dz > 1e-8f) {
                posed[i].pos[0] += dx;
                posed[i].pos[1] += dy;
                posed[i].pos[2] += dz;
                moved = true;
            }
        }
        if (moved && !cpu->indices.empty()) {
            compute_smooth_normals_and_tangents(posed, cpu->indices);
        }
    }

    if (!moved) return {};

    const uint64_t bytes = static_cast<uint64_t>(posed.size()) * sizeof(PbrVertex);
    const uint64_t aligned_off = (dyn_cursor + 15ull) & ~15ull;
    if (aligned_off + bytes > RemasterEngine::kDynVboSlotBytes) return {};

    std::memcpy(dyn_base + aligned_off, posed.data(), static_cast<size_t>(bytes));
    dyn_cursor = aligned_off + bytes;
    return {dyn_buf, aligned_off};
}

// Merge static world GRAF instances into spatial-cell (96m) x material mega-VBO/IBOs.
// Slashes static draw calls by 5x-10x while preserving tight frustum & CSM cascade culling!
void rebuild_static_world_batches(const LoadedWorld &world) {
    for (auto &b : g_eng.static_batches) {
        retire_buffer(std::move(b.vbo));
        retire_buffer(std::move(b.ibo));
    }
    g_eng.static_batches.clear();

    struct BatchKey {
        int32_t cell_x = 0;
        int32_t cell_z = 0;
        std::string texture_id;
        uint32_t alpha_mode = 0;
        uint32_t optional_terrain_mask = 0;
        int32_t rough_q = 0;
        int32_t metal_q = 0;
        int32_t emissive_q = 0;
        bool is_optional_terrain = false;
        bool is_sky = false;
        bool is_foliage = false;
        bool cast_shadow = true;
        bool double_sided = false;

        bool operator==(const BatchKey &o) const {
            return cell_x == o.cell_x && cell_z == o.cell_z && alpha_mode == o.alpha_mode &&
                   optional_terrain_mask == o.optional_terrain_mask &&
                   rough_q == o.rough_q && metal_q == o.metal_q && emissive_q == o.emissive_q &&
                   is_optional_terrain == o.is_optional_terrain && is_sky == o.is_sky &&
                   is_foliage == o.is_foliage && cast_shadow == o.cast_shadow &&
                   double_sided == o.double_sided && texture_id == o.texture_id;
        }
    };
    struct BatchKeyHash {
        size_t operator()(const BatchKey &k) const noexcept {
            size_t h = std::hash<std::string>{}(k.texture_id);
            auto mix = [&](uint64_t v) {
                h ^= static_cast<size_t>(v * 0x9E3779B97F4A7C15ull) + 0x9e3779b9u + (h << 6) + (h >> 2);
            };
            mix(static_cast<uint32_t>(k.cell_x));
            mix(static_cast<uint32_t>(k.cell_z));
            mix(k.alpha_mode);
            mix(k.optional_terrain_mask);
            mix(static_cast<uint32_t>(k.rough_q));
            mix(static_cast<uint32_t>(k.metal_q));
            mix(static_cast<uint32_t>(k.emissive_q));
            mix((k.is_optional_terrain ? 1u : 0u) | (k.is_sky ? 2u : 0u) |
                (k.is_foliage ? 4u : 0u) | (k.cast_shadow ? 8u : 0u) | (k.double_sided ? 16u : 0u));
            return h;
        }
    };

    struct BatchBuild {
        BatchKey key;
        PbrMaterial mat;
        std::vector<PbrVertex> verts;
        std::vector<uint32_t> indices;
    };

    constexpr float kCellSize = 96.0f;
    std::unordered_map<BatchKey, size_t, BatchKeyHash> key_to_idx;
    std::vector<BatchBuild> builds;
    builds.reserve(256);

    for (const auto &inst : world.instances) {
        auto cpu = AssetRegistry::instance().get_mesh(inst.mesh_handle);
        if (!cpu || cpu->vertices.empty() || cpu->indices.empty()) continue;

        GpuTextureSet *gt = lookup_gpu_texture_set(cpu->texture_id);
        const bool is_additive = !inst.is_sky && (cpu->material.alpha_additive || (gt && gt->has_alpha_additive));
        const bool is_cutout = !inst.is_sky && !is_additive && (cpu->material.alpha_cutout || (gt && gt->has_alpha_cutout));
        const bool is_blend = !inst.is_sky && !is_additive && !is_cutout && (cpu->material.alpha_blend || (gt && gt->has_alpha_blend));
        const uint32_t alpha_mode = is_additive ? 3u : (is_cutout ? 1u : (is_blend ? 2u : 0u));

        PbrMaterial mat = cpu->material;
        if (is_additive) {
            mat.alpha_additive = true;
            mat.alpha_cutout = false;
            mat.alpha_blend = false;
            mat.double_sided = true;
            if (gt && gt->resolved_emissive_scale > 0.0f && mat.emissive_scale <= 0.0f) {
                mat.emissive_scale = gt->resolved_emissive_scale;
            }
            if (mat.emissive_scale <= 0.0f) mat.emissive_scale = 0.50f;
        }

        BatchKey key{};
        if (!inst.is_sky) {
            key.cell_x = static_cast<int32_t>(std::floor(inst.center[0] / kCellSize));
            key.cell_z = static_cast<int32_t>(std::floor(inst.center[2] / kCellSize));
        }
        key.texture_id = cpu->texture_id;
        key.alpha_mode = alpha_mode;
        key.optional_terrain_mask = inst.optional_terrain_mask;
        key.rough_q = static_cast<int32_t>(std::lround(mat.roughness * 100.0f));
        key.metal_q = static_cast<int32_t>(std::lround(mat.metallic * 100.0f));
        key.emissive_q = static_cast<int32_t>(std::lround(mat.emissive_scale * 100.0f));
        key.is_optional_terrain = inst.is_optional_terrain;
        key.is_sky = inst.is_sky;
        key.is_foliage = inst.is_foliage || is_cutout;
        key.cast_shadow = inst.cast_shadow && !is_additive && !is_blend;
        key.double_sided = mat.double_sided || is_cutout || is_additive;

        auto it = key_to_idx.find(key);
        size_t bi = 0;
        if (it == key_to_idx.end()) {
            bi = builds.size();
            key_to_idx[key] = bi;
            builds.push_back(BatchBuild{key, mat, {}, {}});
        } else {
            bi = it->second;
        }

        auto &dst = builds[bi];
        const uint32_t vbase = static_cast<uint32_t>(dst.verts.size());
        dst.verts.insert(dst.verts.end(), cpu->vertices.begin(), cpu->vertices.end());
        dst.indices.reserve(dst.indices.size() + cpu->indices.size());
        for (uint32_t idx : cpu->indices) {
            dst.indices.push_back(vbase + idx);
        }
    }

    using namespace plume;
    g_eng.static_batches.reserve(builds.size());
    for (auto &b : builds) {
        if (b.verts.empty() || b.indices.empty()) continue;
        const size_t vb_bytes = b.verts.size() * sizeof(PbrVertex);
        const size_t ib_bytes = b.indices.size() * sizeof(uint32_t);
        auto vbo = g_eng.dev->createBuffer(RenderBufferDesc::UploadBuffer(vb_bytes, RenderBufferFlag::VERTEX));
        auto ibo = g_eng.dev->createBuffer(RenderBufferDesc::UploadBuffer(ib_bytes, RenderBufferFlag::INDEX));
        if (!vbo || !ibo) continue;
        if (void *p = vbo->map()) { std::memcpy(p, b.verts.data(), vb_bytes); vbo->unmap(); }
        if (void *p = ibo->map()) { std::memcpy(p, b.indices.data(), ib_bytes); ibo->unmap(); }

        float bmin[3] = {b.verts[0].pos[0], b.verts[0].pos[1], b.verts[0].pos[2]};
        float bmax[3] = {bmin[0], bmin[1], bmin[2]};
        for (const auto &v : b.verts) {
            for (int k = 0; k < 3; ++k) {
                bmin[k] = std::min(bmin[k], v.pos[k]);
                bmax[k] = std::max(bmax[k], v.pos[k]);
            }
        }
        StaticBatch sb{};
        sb.vbo = std::move(vbo);
        sb.ibo = std::move(ibo);
        sb.vertex_count = static_cast<uint32_t>(b.verts.size());
        sb.index_count = static_cast<uint32_t>(b.indices.size());
        for (int k = 0; k < 3; ++k) sb.center[k] = 0.5f * (bmin[k] + bmax[k]);
        float max_r2 = 0.0f;
        for (const auto &v : b.verts) {
            const float dx = v.pos[0] - sb.center[0];
            const float dy = v.pos[1] - sb.center[1];
            const float dz = v.pos[2] - sb.center[2];
            max_r2 = std::max(max_r2, dx * dx + dy * dy + dz * dz);
        }
        sb.radius = std::sqrt(max_r2);
        sb.texture_id = std::move(b.key.texture_id);
        sb.material = b.mat;
        sb.alpha_mode = b.key.alpha_mode;
        sb.optional_terrain_mask = b.key.optional_terrain_mask;
        sb.is_optional_terrain = b.key.is_optional_terrain;
        sb.is_sky = b.key.is_sky;
        sb.is_foliage = b.key.is_foliage;
        sb.cast_shadow = b.key.cast_shadow;
        sb.double_sided = b.key.double_sided;
        g_eng.static_batches.push_back(std::move(sb));
    }

    // Sort by (is_sky, alpha_mode, double_sided, texture_id) to minimize pipeline & descriptor binds
    std::sort(g_eng.static_batches.begin(), g_eng.static_batches.end(),
              [](const StaticBatch &a, const StaticBatch &b) {
                  return std::tie(a.is_sky, a.alpha_mode, a.double_sided, a.texture_id) <
                         std::tie(b.is_sky, b.alpha_mode, b.double_sided, b.texture_id);
              });
}

} // namespace

bool initialize() {
    if (g_eng.attempted) return g_eng.ready;
    g_eng.attempted = true;

    auto *base_dev = plume_device::render_device();
    if (!base_dev) return false;
    g_eng.dev = static_cast<plume::VulkanDevice *>(base_dev);

    using namespace plume;
    namespace Stage = RenderShaderStageFlag;

    g_eng.queue = g_eng.dev->createCommandQueue(RenderCommandListType::DIRECT);
    if (!g_eng.queue) return false;
    g_eng.list = g_eng.queue->createCommandList();
    if (!g_eng.list) return false;

    // Samplers: linear wrap for PBR textures, linear clamp+comparison for CSM shadow maps
    RenderSamplerDesc sd{};
    sd.minFilter = sd.magFilter = RenderFilter::LINEAR;
    sd.mipmapMode = RenderMipmapMode::LINEAR;
    sd.addressU = sd.addressV = sd.addressW = RenderTextureAddressMode::WRAP;
    g_eng.samp_linear = g_eng.dev->createSampler(sd);

    RenderSamplerDesc shd{};
    shd.minFilter = shd.magFilter = RenderFilter::LINEAR;
    shd.mipmapMode = RenderMipmapMode::NEAREST;
    shd.addressU = shd.addressV = shd.addressW = RenderTextureAddressMode::CLAMP;
    shd.comparisonEnabled = true;
    shd.comparisonFunc = RenderComparisonFunction::LESS_EQUAL;
    g_eng.samp_shadow = g_eng.dev->createSampler(shd);
    if (!g_eng.samp_linear || !g_eng.samp_shadow) return false;

    // Pipeline Layouts
    const RenderPushConstantRange draw_pc(0, 0, 0, sizeof(DrawPC), Stage::VERTEX | Stage::PIXEL);
    const RenderDescriptorSetDesc pbr_sets[2] = {frame_ds_desc(), material_ds_desc()};
    g_eng.pbr_layout = g_eng.dev->createPipelineLayout(
        RenderPipelineLayoutDesc(&draw_pc, 1, pbr_sets, 2, false, true));

    const RenderDescriptorSetDesc res_set = resolve_ds_desc();
    g_eng.resolve_layout = g_eng.dev->createPipelineLayout(
        RenderPipelineLayoutDesc(nullptr, 0, &res_set, 1, false, true));
    if (!g_eng.pbr_layout || !g_eng.resolve_layout) return false;

    // Shaders
    auto vs_csm   = g_eng.dev->createShader(k_remaster_vs_csm_depth, sizeof k_remaster_vs_csm_depth, "vs_csm_depth", RenderShaderFormat::SPIRV);
    auto ps_csm_a = g_eng.dev->createShader(k_remaster_ps_csm_alpha, sizeof k_remaster_ps_csm_alpha, "ps_csm_alpha", RenderShaderFormat::SPIRV);
    auto vs_pbr   = g_eng.dev->createShader(k_remaster_vs_pbr_main, sizeof k_remaster_vs_pbr_main, "vs_pbr_main", RenderShaderFormat::SPIRV);
    auto ps_cube  = g_eng.dev->createShader(k_remaster_ps_pbr_cubemap, sizeof k_remaster_ps_pbr_cubemap, "ps_pbr_cubemap", RenderShaderFormat::SPIRV);
    auto ps_pbr   = g_eng.dev->createShader(k_remaster_ps_pbr_forward, sizeof k_remaster_ps_pbr_forward, "ps_pbr_forward", RenderShaderFormat::SPIRV);
    auto vs_quad  = g_eng.dev->createShader(k_remaster_vs_resolve_quad, sizeof k_remaster_vs_resolve_quad, "vs_resolve_quad", RenderShaderFormat::SPIRV);
    auto ps_res   = g_eng.dev->createShader(k_remaster_ps_neutral_resolve, sizeof k_remaster_ps_neutral_resolve, "ps_neutral_resolve", RenderShaderFormat::SPIRV);
    auto vs_hud   = g_eng.dev->createShader(k_remaster_vs_hud_2d, sizeof k_remaster_vs_hud_2d, "vs_hud_2d", RenderShaderFormat::SPIRV);
    auto ps_hud   = g_eng.dev->createShader(k_remaster_ps_hud_2d, sizeof k_remaster_ps_hud_2d, "ps_hud_2d", RenderShaderFormat::SPIRV);
    if (!vs_csm || !ps_csm_a || !vs_pbr || !ps_cube || !ps_pbr || !vs_quad || !ps_res || !vs_hud || !ps_hud)
        return false;

    const RenderInputSlot pbr_slot(0, sizeof(PbrVertex));
    const RenderInputElement pbr_elems[] = {
        RenderInputElement("POSITION", 0, 0, RenderFormat::R32G32B32_FLOAT,    0, offsetof(PbrVertex, pos)),
        RenderInputElement("NORMAL",   0, 1, RenderFormat::R32G32B32_FLOAT,    0, offsetof(PbrVertex, normal)),
        RenderInputElement("TANGENT",  0, 2, RenderFormat::R32G32B32A32_FLOAT, 0, offsetof(PbrVertex, tangent)),
        RenderInputElement("TEXCOORD", 0, 3, RenderFormat::R32G32_FLOAT,       0, offsetof(PbrVertex, uv)),
    };

    // Pass 1 Pipelines (CSM Opaque + CSM Alpha-Cutout Foliage)
    {
        RenderGraphicsPipelineDesc d{};
        d.pipelineLayout = g_eng.pbr_layout.get();
        d.vertexShader = vs_csm.get();
        d.inputSlots = &pbr_slot; d.inputSlotsCount = 1;
        d.inputElements = pbr_elems; d.inputElementsCount = 4;
        d.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        d.cullMode = RenderCullMode::NONE;
        d.depthTargetFormat = RenderFormat::D32_FLOAT;
        d.depthEnabled = d.depthWriteEnabled = true;
        d.depthFunction = RenderComparisonFunction::LESS_EQUAL;
        g_eng.pipe_csm_opaque = g_eng.dev->createGraphicsPipeline(d);

        d.pixelShader = ps_csm_a.get();
        g_eng.pipe_csm_alpha = g_eng.dev->createGraphicsPipeline(d);
    }

    // Pass 2 Pipeline (Cubemap Face HDR + Sky Dome no-depth-write)
    {
        RenderGraphicsPipelineDesc d{};
        d.pipelineLayout = g_eng.pbr_layout.get();
        d.vertexShader = vs_pbr.get();
        d.pixelShader = ps_cube.get();
        d.inputSlots = &pbr_slot; d.inputSlotsCount = 1;
        d.inputElements = pbr_elems; d.inputElementsCount = 4;
        d.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        d.cullMode = RenderCullMode::NONE;
        d.renderTargetCount = 1;
        d.renderTargetFormat[0] = RenderFormat::R16G16B16A16_FLOAT;
        d.renderTargetBlend[0].renderTargetWriteMask = 15;
        d.depthTargetFormat = RenderFormat::D32_FLOAT;
        d.depthEnabled = d.depthWriteEnabled = true;
        d.depthFunction = RenderComparisonFunction::LESS_EQUAL;
        g_eng.pipe_cubemap = g_eng.dev->createGraphicsPipeline(d);

        d.depthWriteEnabled = false;
        g_eng.pipe_cubemap_sky = g_eng.dev->createGraphicsPipeline(d);
    }

    // Pass 3 Pipelines (Forward+ Linear HDR PBR: opaque, double-sided foliage, sky dome no-depth-write)
    {
        RenderGraphicsPipelineDesc d{};
        d.pipelineLayout = g_eng.pbr_layout.get();
        d.vertexShader = vs_pbr.get();
        d.pixelShader = ps_pbr.get();
        d.inputSlots = &pbr_slot; d.inputSlotsCount = 1;
        d.inputElements = pbr_elems; d.inputElementsCount = 4;
        d.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        d.cullMode = RenderCullMode::NONE; // Mixed winding across PS2 4SPS strips; front-face normal flip in PS
        d.frontFace = RenderFrontFace::COUNTER_CLOCKWISE;
        d.renderTargetCount = 1;
        d.renderTargetFormat[0] = RenderFormat::R16G16B16A16_FLOAT;
        d.renderTargetBlend[0].renderTargetWriteMask = 15;
        d.depthTargetFormat = RenderFormat::D32_FLOAT;
        d.depthEnabled = d.depthWriteEnabled = true;
        d.depthFunction = RenderComparisonFunction::LESS_EQUAL;
        g_eng.pipe_pbr_opaque = g_eng.dev->createGraphicsPipeline(d);

        d.cullMode = RenderCullMode::NONE;
        g_eng.pipe_pbr_double_sided = g_eng.dev->createGraphicsPipeline(d);

        d.depthWriteEnabled = false;
        d.depthFunction = RenderComparisonFunction::LESS_EQUAL;
        g_eng.pipe_pbr_sky = g_eng.dev->createGraphicsPipeline(d);

        d.renderTargetBlend[0] = RenderBlendDesc::AlphaBlend();
        g_eng.pipe_pbr_alpha_blend = g_eng.dev->createGraphicsPipeline(d);

        // Additive pipeline for bridge string lights, flares, and stadium spotlight beam cones:
        // ONE + ONE with depthWriteEnabled=false so black borders add 0 and cones never wash out night sky.
        RenderBlendDesc add_blend{};
        add_blend.blendEnabled = true;
        add_blend.srcBlend = RenderBlend::ONE;
        add_blend.dstBlend = RenderBlend::ONE;
        add_blend.blendOp = RenderBlendOperation::ADD;
        add_blend.srcBlendAlpha = RenderBlend::ZERO;
        add_blend.dstBlendAlpha = RenderBlend::ONE;
        add_blend.blendOpAlpha = RenderBlendOperation::ADD;
        add_blend.renderTargetWriteMask = 15;
        d.renderTargetBlend[0] = add_blend;
        g_eng.pipe_pbr_additive = g_eng.dev->createGraphicsPipeline(d);
    }

    // Pass 4 Pipelines (Khronos PBR Neutral Resolve + 2D HUD Composite)
    {
        RenderGraphicsPipelineDesc d{};
        d.pipelineLayout = g_eng.resolve_layout.get();
        d.vertexShader = vs_quad.get();
        d.pixelShader = ps_res.get();
        d.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        d.cullMode = RenderCullMode::NONE;
        d.renderTargetCount = 1;
        d.renderTargetFormat[0] = RenderFormat::R8G8B8A8_UNORM;
        d.renderTargetBlend[0].renderTargetWriteMask = 15;
        g_eng.pipe_resolve = g_eng.dev->createGraphicsPipeline(d);

        const RenderInputSlot hud_slot(0, sizeof(HudVertex));
        const RenderInputElement hud_elems[] = {
            RenderInputElement("POSITION", 0, 0, RenderFormat::R32G32_FLOAT,       0, offsetof(HudVertex, pos)),
            RenderInputElement("TEXCOORD", 0, 1, RenderFormat::R32G32_FLOAT,       0, offsetof(HudVertex, uv)),
            RenderInputElement("COLOR",    0, 2, RenderFormat::R32G32B32A32_FLOAT, 0, offsetof(HudVertex, color)),
        };
        d.vertexShader = vs_hud.get();
        d.pixelShader = ps_hud.get();
        d.inputSlots = &hud_slot; d.inputSlotsCount = 1;
        d.inputElements = hud_elems; d.inputElementsCount = 3;
        d.renderTargetBlend[0] = RenderBlendDesc::AlphaBlend();
        g_eng.pipe_hud = g_eng.dev->createGraphicsPipeline(d);
    }

    // Pass 1 Targets: 3x 2048x2048 D32_FLOAT CSM Shadow Maps
    for (uint32_t i = 0; i < CsmCascadeSet::kCascadeCount; ++i) {
        g_eng.csm_depth[i] = g_eng.dev->createTexture(
            RenderTextureDesc::DepthTarget(CsmCascadeSet::kResolution, CsmCascadeSet::kResolution, RenderFormat::D32_FLOAT));
        if (!g_eng.csm_depth[i]) return false;
        g_eng.csm_fb[i] = g_eng.dev->createFramebuffer(RenderFramebufferDesc(nullptr, 0, g_eng.csm_depth[i].get()));
        if (!g_eng.csm_fb[i]) return false;
    }

    // Pass 2 Targets: 128x128 6-Face Dynamic Environment Cubemap (R16G16B16A16_FLOAT)
    {
        auto cdesc = RenderTextureDesc::Texture(
            RenderTextureDimension::TEXTURE_2D, RemasterEngine::kCubeRes, RemasterEngine::kCubeRes, 1, 1, 6,
            RenderFormat::R16G16B16A16_FLOAT, RenderTextureFlag::RENDER_TARGET | RenderTextureFlag::CUBE);
        g_eng.env_cube = g_eng.dev->createTexture(cdesc);
        g_eng.env_cube_depth = g_eng.dev->createTexture(
            RenderTextureDesc::DepthTarget(RemasterEngine::kCubeRes, RemasterEngine::kCubeRes, RenderFormat::D32_FLOAT));
        if (!g_eng.env_cube || !g_eng.env_cube_depth) return false;

        RenderTextureViewDesc cvd{};
        cvd.format = RenderFormat::R16G16B16A16_FLOAT;
        cvd.dimension = RenderTextureViewDimension::TEXTURE_CUBE;
        cvd.mipLevels = 1; cvd.mipSlice = 0; cvd.arraySize = 6; cvd.arrayIndex = 0;
        g_eng.env_cube_view = g_eng.env_cube->createTextureView(cvd);
        if (!g_eng.env_cube_view) return false;

        for (uint32_t f = 0; f < 6; ++f) {
            RenderTextureViewDesc fvd = RenderTextureViewDesc::Texture2D(RenderFormat::R16G16B16A16_FLOAT);
            fvd.mipLevels = 1; fvd.mipSlice = 0; fvd.arraySize = 1; fvd.arrayIndex = f;
            g_eng.env_face_views[f] = g_eng.env_cube->createTextureView(fvd);
            if (!g_eng.env_face_views[f]) return false;
            const RenderTexture *c_tex[] = {g_eng.env_cube.get()};
            const RenderTextureView *c_view[] = {g_eng.env_face_views[f].get()};
            RenderFramebufferDesc fbd{};
            fbd.colorAttachments = c_tex;
            fbd.colorAttachmentViews = c_view;
            fbd.colorAttachmentsCount = 1;
            fbd.depthAttachment = g_eng.env_cube_depth.get();
            g_eng.env_face_fb[f] = g_eng.dev->createFramebuffer(fbd);
            if (!g_eng.env_face_fb[f]) return false;
        }
    }

    // Ring-buffered per-frame UBOs + Descriptor Sets + HUD VBO + Posed Dynamic VBO (3 frames in flight)
    const auto sr = RenderTextureLayout::SHADER_READ;
    auto bind_frame_ds = [&](RenderDescriptorSet *ds, RenderBuffer *ubo) {
        ds->setBuffer(0, ubo, sizeof(FrameUBO));
        ds->setTexture(1, g_eng.csm_depth[0].get(), sr);
        ds->setTexture(2, g_eng.csm_depth[1].get(), sr);
        ds->setTexture(3, g_eng.csm_depth[2].get(), sr);
        ds->setTexture(4, g_eng.env_cube.get(), sr, g_eng.env_cube_view.get());
        ds->setSampler(5, g_eng.samp_linear.get());
        ds->setSampler(6, g_eng.samp_shadow.get());
    };
    for (uint32_t s = 0; s < RemasterEngine::kFramesInFlight; ++s) {
        g_eng.frame_ubo[s] = g_eng.dev->createBuffer(RenderBufferDesc::UploadBuffer(sizeof(FrameUBO), RenderBufferFlag::CONSTANT));
        g_eng.frame_ds[s]  = g_eng.dev->createDescriptorSet(frame_ds_desc());
        g_eng.hud_vbo[s]   = g_eng.dev->createBuffer(RenderBufferDesc::UploadBuffer(sizeof(HudVertex) * 6 * 512, RenderBufferFlag::VERTEX));
        g_eng.dyn_vbo[s]   = g_eng.dev->createBuffer(RenderBufferDesc::UploadBuffer(RemasterEngine::kDynVboSlotBytes, RenderBufferFlag::VERTEX));
        if (!g_eng.frame_ubo[s] || !g_eng.frame_ds[s] || !g_eng.hud_vbo[s] || !g_eng.dyn_vbo[s]) return false;
        bind_frame_ds(g_eng.frame_ds[s].get(), g_eng.frame_ubo[s].get());
        for (uint32_t f = 0; f < 6; ++f) {
            g_eng.cube_ubo[s][f] = g_eng.dev->createBuffer(RenderBufferDesc::UploadBuffer(sizeof(FrameUBO), RenderBufferFlag::CONSTANT));
            g_eng.cube_ds[s][f]  = g_eng.dev->createDescriptorSet(frame_ds_desc());
            if (!g_eng.cube_ubo[s][f] || !g_eng.cube_ds[s][f]) return false;
            bind_frame_ds(g_eng.cube_ds[s][f].get(), g_eng.cube_ubo[s][f].get());
        }
    }

    g_eng.ready = g_eng.pipe_csm_opaque && g_eng.pipe_csm_alpha && g_eng.pipe_cubemap && g_eng.pipe_cubemap_sky &&
                  g_eng.pipe_pbr_opaque && g_eng.pipe_pbr_double_sided && g_eng.pipe_pbr_alpha_blend &&
                  g_eng.pipe_pbr_additive && g_eng.pipe_pbr_sky && g_eng.pipe_resolve && g_eng.pipe_hud;
    if (g_eng.ready) {
        std::fprintf(stderr, "[remaster] Foundational Forward+ Linear HDR engine initialized (CSM 3x%u, Cubemap %u, Khronos PBR Neutral)\n",
                     CsmCascadeSet::kResolution, RemasterEngine::kCubeRes);
    }
    return g_eng.ready;
}

bool is_ready() { return g_eng.ready; }

bool render_frame(const OutputTarget &target) {
    if (!target.vk_command_buffer || !target.vk_color_image || target.width < 8 || target.height < 8)
        return false;
    if (!initialize()) return false;

    const auto t_frame_start = std::chrono::steady_clock::now();
    ++g_eng.render_frame_id;
    const uint32_t slot = static_cast<uint32_t>(g_eng.render_frame_id % RemasterEngine::kFramesInFlight);
    gc_retired_resources();

    if (!ensure_hdr_targets(target.width, target.height) || !ensure_output_wrapper(target))
        return false;
    if (!g_eng.list || !g_eng.hdr_color || !g_eng.hdr_depth || !g_eng.hdr_fb ||
        !g_eng.cur_out_wrapper || !g_eng.cur_out_fb || !g_eng.env_cube || !g_eng.env_cube_depth)
        return false;

    auto frame = latest_scene_frame();
    auto world = current_loaded_world();
    if (!frame || !frame->camera.valid) return false;

    // Rebuild camera projection for the exact render target dimensions (+Y UP -> -fy in Vulkan NDC)
    SceneCamera cam = frame->camera;
    rebuild_camera_projection(cam, target.width, target.height);

    using namespace plume;
    auto *vk_list = static_cast<VulkanCommandList *>(g_eng.list.get());
    const VkCommandBuffer saved_vk = vk_list->vk;
    vk_list->vk = static_cast<VkCommandBuffer>(target.vk_command_buffer);
    RenderCommandList &list = *g_eng.list;

    // ========================================================================
    // PRE-PASS (OUTSIDE ANY RENDER PASS): Upload textures, batch static world,
    // and bump-allocate posed entity vertices into persistent ring dyn_vbo[slot].
    // ZERO createBuffer() calls during steady-state gameplay!
    // ========================================================================
    if (world && (g_eng.uploaded_track_gen != frame->track_generation ||
                  g_eng.batched_track_gen != frame->track_generation)) {
        bool all_resolved = true;
        for (const auto &inst : world->instances) {
            auto cpu = AssetRegistry::instance().get_mesh(inst.mesh_handle);
            if (!cpu || cpu->indices.empty()) continue;
            GpuTextureSet *gt = ensure_gpu_texture_set(list, cpu->texture_id, inst.is_sky, false);
            if (!gt || gt->albedo_w <= 1) all_resolved = false;
        }
        const bool need_batch = (g_eng.batched_track_gen != frame->track_generation) ||
                                (all_resolved && !g_eng.batches_have_all_tex);
        if (need_batch) {
            rebuild_static_world_batches(*world);
            g_eng.batched_track_gen = frame->track_generation;
            g_eng.batches_have_all_tex = all_resolved;
        }
        if (all_resolved) g_eng.uploaded_track_gen = frame->track_generation;
    }

    std::unordered_map<uint64_t, PosedVboSlice> posed_vbos;
    uint8_t *dyn_base = nullptr;
    uint64_t dyn_cursor = 0;
    for (size_t ei = 0; ei < frame->entities.size(); ++ei) {
        const auto &ent = frame->entities[ei];
        const bool has_pose = !ent.dent_offsets.empty() || !ent.bone_palette.empty();
        for (uint32_t handle : ent.part_handles) {
            GpuMesh *gm = ensure_gpu_mesh(handle);
            if (!gm || gm->index_count == 0) continue;
            const std::string &tex_id = ent.material_override.empty() ? gm->texture_id : ent.material_override;
            ensure_gpu_texture_set(list, tex_id, false, ent.is_vehicle);
            if (has_pose) {
                if (!dyn_base && g_eng.dyn_vbo[slot]) {
                    dyn_base = static_cast<uint8_t *>(g_eng.dyn_vbo[slot]->map());
                }
                PosedVboSlice slice = build_posed_vbo(ent, handle, g_eng.dyn_vbo[slot].get(), dyn_base, dyn_cursor);
                if (slice.buf) {
                    const uint64_t pkey = (static_cast<uint64_t>(ei) << 32) | handle;
                    posed_vbos[pkey] = slice;
                }
            }
        }
    }
    if (dyn_base && g_eng.dyn_vbo[slot]) {
        g_eng.dyn_vbo[slot]->unmap();
    }

    // Build CSM cascades strictly from SceneEnvironment.sun_dir + SceneCamera
    const CsmCascadeSet csm = compute_csm_cascades(cam, frame->env, CsmCascadeSet::kResolution);

    FrameUBO ubo{};
    std::memcpy(ubo.view_proj, cam.view_proj, sizeof(ubo.view_proj));
    for (uint32_t i = 0; i < 3; ++i) std::memcpy(ubo.csm_view_proj[i], csm.view_proj[i], sizeof(ubo.csm_view_proj[i]));
    ubo.csm_splits[0] = csm.splits[0];
    ubo.csm_splits[1] = csm.splits[1];
    ubo.csm_splits[2] = csm.splits[2];
    ubo.csm_splits[3] = frame->env.shadow_strength;
    ubo.cam_pos_debug[0] = cam.pos[0];
    ubo.cam_pos_debug[1] = cam.pos[1];
    ubo.cam_pos_debug[2] = cam.pos[2];
    ubo.cam_pos_debug[3] = static_cast<float>(g_debug_mode.load(std::memory_order_relaxed));
    ubo.sun_dir_intensity[0] = frame->env.sun_dir[0];
    ubo.sun_dir_intensity[1] = frame->env.sun_dir[1];
    ubo.sun_dir_intensity[2] = frame->env.sun_dir[2];
    ubo.sun_dir_intensity[3] = frame->env.sun_intensity;
    ubo.sun_color_day[0] = frame->env.sun_color[0];
    ubo.sun_color_day[1] = frame->env.sun_color[1];
    ubo.sun_color_day[2] = frame->env.sun_color[2];
    ubo.sun_color_day[3] = frame->env.is_day ? 1.0f : 0.0f;
    ubo.ambient_sky_int[0] = frame->env.ambient_sky[0];
    ubo.ambient_sky_int[1] = frame->env.ambient_sky[1];
    ubo.ambient_sky_int[2] = frame->env.ambient_sky[2];
    ubo.ambient_sky_int[3] = frame->env.ambient_intensity;
    ubo.ambient_ground_pad[0] = frame->env.ambient_ground[0];
    ubo.ambient_ground_pad[1] = frame->env.ambient_ground[1];
    ubo.ambient_ground_pad[2] = frame->env.ambient_ground[2];
    ubo.fog_color_density[0] = frame->env.fog_color[0];
    ubo.fog_color_density[1] = frame->env.fog_color[1];
    ubo.fog_color_density[2] = frame->env.fog_color[2];
    ubo.fog_color_density[3] = frame->env.fog_density;
    ubo.fog_start_end_lights[0] = frame->env.fog_start;
    ubo.fog_start_end_lights[1] = frame->env.fog_end;
    const uint32_t n_lights = std::min<uint32_t>(static_cast<uint32_t>(frame->lights.size()), 32u);
    ubo.fog_start_end_lights[2] = static_cast<float>(n_lights);
    ubo.fog_start_end_lights[3] = 1.0f / static_cast<float>(CsmCascadeSet::kResolution);
    for (uint32_t i = 0; i < n_lights; ++i) {
        const auto &l = frame->lights[i];
        ubo.light_pos_rad[i][0] = l.pos[0]; ubo.light_pos_rad[i][1] = l.pos[1];
        ubo.light_pos_rad[i][2] = l.pos[2]; ubo.light_pos_rad[i][3] = l.radius;
        ubo.light_col_int[i][0] = l.color[0] * l.intensity;
        ubo.light_col_int[i][1] = l.color[1] * l.intensity;
        ubo.light_col_int[i][2] = l.color[2] * l.intensity;
        ubo.light_col_int[i][3] = l.spot_cos;
        ubo.light_dir_pad[i][0] = l.dir[0]; ubo.light_dir_pad[i][1] = l.dir[1]; ubo.light_dir_pad[i][2] = l.dir[2];
    }
    if (void *p = g_eng.frame_ubo[slot]->map()) {
        std::memcpy(p, &ubo, sizeof(ubo));
        g_eng.frame_ubo[slot]->unmap();
    }

    uint64_t main_pass_tris = 0;
    uint32_t csm_draws = 0, cube_draws = 0, main_draws = 0;
    std::vector<const StaticBatch *> visible_static_trans;
    visible_static_trans.reserve(64);

    auto draw_static_world = [&](bool shadow_pass, uint32_t cascade_idx, bool cubemap_pass,
                                 const float view_origin[3], bool transparent_pass,
                                 const Frustum &frustum, float max_dist) {
        if (g_eng.static_batches.empty()) return;
        const RenderInputSlot pbr_slot(0, sizeof(PbrVertex));
        RenderPipeline *cur_pipe = nullptr;
        RenderDescriptorSet *cur_mat_ds = nullptr;
        auto bind_pipe = [&](RenderPipeline *p) {
            if (p != cur_pipe) {
                list.setPipeline(p);
                cur_pipe = p;
            }
        };

        auto emit_batch = [&](const StaticBatch &sb) {
            if (sb.index_count == 0 || !sb.vbo || !sb.ibo) return;
            if (sb.is_optional_terrain && (frame->session_event_mask & sb.optional_terrain_mask) == 0)
                return;
            if (shadow_pass && !sb.cast_shadow) return;

            const bool is_cutout   = (sb.alpha_mode == 1u);
            const bool is_blend    = (sb.alpha_mode == 2u);
            const bool is_additive = (sb.alpha_mode == 3u);
            const bool is_trans    = is_blend || is_additive;

            if (shadow_pass) {
                if (is_trans) return;
            } else if (cubemap_pass) {
                if (is_trans) return;
            } else if (transparent_pass) {
                if (!is_trans) return;
            }

            if (!sb.is_sky) {
                if (shadow_pass) {
                    if (cascade_idx == 1u && sb.radius < 1.0f) return;
                    if (cascade_idx == 2u && (sb.radius < 3.5f || sb.is_foliage)) return;
                } else if (cubemap_pass) {
                    if (sb.radius < 0.8f) return;
                }
                if (max_dist > 0.0f && view_origin) {
                    const float dx = sb.center[0] - view_origin[0];
                    const float dy = sb.center[1] - view_origin[1];
                    const float dz = sb.center[2] - view_origin[2];
                    const float limit = max_dist + sb.radius;
                    if (dx * dx + dy * dy + dz * dz > limit * limit) return;
                }
                if (!frustum_intersects_sphere(frustum, sb.center[0], sb.center[1], sb.center[2], sb.radius))
                    return;
            }

            if (!shadow_pass && !cubemap_pass && !transparent_pass && is_trans) {
                visible_static_trans.push_back(&sb);
                return;
            }

            GpuTextureSet *gt = lookup_gpu_texture_set(sb.texture_id);
            if (!gt || !gt->ds) return;

            if (shadow_pass) {
                bind_pipe(is_cutout ? g_eng.pipe_csm_alpha.get() : g_eng.pipe_csm_opaque.get());
            } else if (cubemap_pass) {
                bind_pipe(sb.is_sky ? g_eng.pipe_cubemap_sky.get() : g_eng.pipe_cubemap.get());
            } else if (transparent_pass) {
                bind_pipe(is_additive ? g_eng.pipe_pbr_additive.get() : g_eng.pipe_pbr_alpha_blend.get());
            } else {
                if (sb.is_sky) {
                    bind_pipe(g_eng.pipe_pbr_sky.get());
                } else {
                    bind_pipe((sb.double_sided || is_cutout) ? g_eng.pipe_pbr_double_sided.get()
                                                             : g_eng.pipe_pbr_opaque.get());
                }
            }

            DrawPC pc{};
            pc.model[0] = pc.model[5] = pc.model[10] = pc.model[15] = 1.0f;
            if (sb.is_sky && view_origin) {
                pc.model[3]  = view_origin[0];
                pc.model[7]  = view_origin[1];
                pc.model[11] = view_origin[2];
            }
            std::memcpy(pc.base_color, sb.material.base_color, sizeof(pc.base_color));
            pc.pbr_params[0] = sb.material.roughness;
            pc.pbr_params[1] = sb.material.metallic;
            pc.pbr_params[2] = sb.material.ao;
            pc.pbr_params[3] = sb.material.emissive_scale;
            pc.flags[0] = sb.alpha_mode;
            pc.flags[1] = sb.is_sky ? 1u : 0u;
            pc.flags[2] = cascade_idx;

            if (gt->ds.get() != cur_mat_ds) {
                list.setGraphicsDescriptorSet(gt->ds.get(), 1);
                cur_mat_ds = gt->ds.get();
            }
            list.setGraphicsPushConstants(0, &pc);
            const RenderVertexBufferView vbv(sb.vbo.get(), sb.vertex_count * sizeof(PbrVertex));
            const RenderIndexBufferView ibv(sb.ibo.get(), sb.index_count * sizeof(uint32_t), RenderFormat::R32_UINT);
            list.setVertexBuffers(0, &vbv, 1, &pbr_slot);
            list.setIndexBuffer(&ibv);
            list.drawIndexedInstanced(sb.index_count, 1, 0, 0, 0);
            if (shadow_pass) ++csm_draws;
            else if (cubemap_pass) ++cube_draws;
            else {
                ++main_draws;
                main_pass_tris += sb.index_count / 3u;
            }
        };

        if (transparent_pass) {
            for (const StaticBatch *sb : visible_static_trans) emit_batch(*sb);
        } else {
            for (const auto &sb : g_eng.static_batches) emit_batch(sb);
        }
    };

    auto draw_dynamic_entities = [&](bool shadow_pass, uint32_t cascade_idx, bool cubemap_pass,
                                     bool transparent_pass, const Frustum &frustum,
                                     const float view_origin[3], float max_dist) {
        const RenderInputSlot pbr_slot(0, sizeof(PbrVertex));
        RenderPipeline *cur_pipe = nullptr;
        RenderDescriptorSet *cur_mat_ds = nullptr;
        auto bind_pipe = [&](RenderPipeline *p) {
            if (p != cur_pipe) {
                list.setPipeline(p);
                cur_pipe = p;
            }
        };
        for (size_t ei = 0; ei < frame->entities.size(); ++ei) {
            const auto &ent = frame->entities[ei];
            if (shadow_pass && !ent.cast_shadow) continue;
            if (cubemap_pass && ent.is_player_vehicle) continue; // Exclude player vehicle from its own reflection probe

            // Coarse entity-level distance & frustum cull (conservative 8m sphere covers any vehicle/prop)
            const float ex = ent.world_matrix[9], ey = ent.world_matrix[10], ez = ent.world_matrix[11];
            constexpr float kEntRadius = 8.0f;
            if (max_dist > 0.0f && view_origin) {
                const float dx = ex - view_origin[0];
                const float dy = ey - view_origin[1];
                const float dz = ez - view_origin[2];
                const float limit = max_dist + kEntRadius;
                if (dx * dx + dy * dy + dz * dz > limit * limit) continue;
            }
            if (!frustum_intersects_sphere(frustum, ex, ey, ez, kEntRadius)) continue;

            for (uint32_t handle : ent.part_handles) {
                auto mit = g_eng.gpu_meshes.find(handle);
                if (mit == g_eng.gpu_meshes.end() || !mit->second || mit->second->index_count == 0) continue;
                GpuMesh *gm = mit->second.get();
                const std::string &tex_id = ent.material_override.empty() ? gm->texture_id : ent.material_override;
                GpuTextureSet *gt = lookup_gpu_texture_set(tex_id);
                if (!gt || !gt->ds) continue;

                const bool is_additive = gm->material.alpha_additive || gt->has_alpha_additive;
                const bool is_cutout = !is_additive && (gm->material.alpha_cutout || gt->has_alpha_cutout);
                const bool is_blend = !is_additive && !is_cutout && (gm->material.alpha_blend || gt->has_alpha_blend);
                const bool is_trans = is_blend || is_additive;
                if (shadow_pass) {
                    if (is_trans) continue;
                    bind_pipe(is_cutout ? g_eng.pipe_csm_alpha.get() : g_eng.pipe_csm_opaque.get());
                } else if (cubemap_pass) {
                    if (is_trans) continue;
                    bind_pipe(g_eng.pipe_cubemap.get());
                } else if (transparent_pass) {
                    if (!is_trans) continue;
                    bind_pipe(is_additive ? g_eng.pipe_pbr_additive.get() : g_eng.pipe_pbr_alpha_blend.get());
                } else {
                    if (is_trans) continue;
                    bind_pipe((gm->material.double_sided || is_cutout) ? g_eng.pipe_pbr_double_sided.get()
                                                                       : g_eng.pipe_pbr_opaque.get());
                }

                // Transpose EE 4x3 row-vector matrix (m[0..2]=X, m[3..5]=Y, m[6..8]=Z, m[9..11]=T)
                // into 4x4 row-major matrix for HLSL mul(pc.model, float4(pos, 1.0)).
                DrawPC pc{};
                const float *m = ent.world_matrix;
                pc.model[0]  = m[0]; pc.model[1]  = m[3]; pc.model[2]  = m[6]; pc.model[3]  = m[9];
                pc.model[4]  = m[1]; pc.model[5]  = m[4]; pc.model[6]  = m[7]; pc.model[7]  = m[10];
                pc.model[8]  = m[2]; pc.model[9]  = m[5]; pc.model[10] = m[8]; pc.model[11] = m[11];
                pc.model[12] = 0.0f; pc.model[13] = 0.0f; pc.model[14] = 0.0f; pc.model[15] = 1.0f;
                std::memcpy(pc.base_color, gm->material.base_color, sizeof(pc.base_color));
                pc.pbr_params[0] = gm->material.roughness;
                pc.pbr_params[1] = gm->material.metallic;
                pc.pbr_params[2] = gm->material.ao;
                pc.pbr_params[3] = (is_additive && gm->material.emissive_scale <= 0.0f)
                                       ? (gt->resolved_emissive_scale > 0.0f ? gt->resolved_emissive_scale : 0.50f)
                                       : gm->material.emissive_scale;
                pc.flags[0] = is_additive ? 3u : (is_cutout ? 1u : (is_blend ? 2u : 0u));
                pc.flags[1] = 0u;
                pc.flags[2] = cascade_idx;

                RenderBuffer *vbo_ptr = gm->vbo.get();
                uint64_t vbo_offset = 0;
                if (!posed_vbos.empty()) {
                    const uint64_t pkey = (static_cast<uint64_t>(ei) << 32) | handle;
                    if (auto pit = posed_vbos.find(pkey); pit != posed_vbos.end() && pit->second.buf) {
                        vbo_ptr = pit->second.buf;
                        vbo_offset = pit->second.offset;
                    }
                }

                if (gt->ds.get() != cur_mat_ds) {
                    list.setGraphicsDescriptorSet(gt->ds.get(), 1);
                    cur_mat_ds = gt->ds.get();
                }
                list.setGraphicsPushConstants(0, &pc);
                const RenderVertexBufferView vbv(RenderBufferReference(vbo_ptr, vbo_offset),
                                                 gm->vertex_count * sizeof(PbrVertex));
                const RenderIndexBufferView ibv(gm->ibo.get(), gm->index_count * sizeof(uint32_t), RenderFormat::R32_UINT);
                list.setVertexBuffers(0, &vbv, 1, &pbr_slot);
                list.setIndexBuffer(&ibv);
                list.drawIndexedInstanced(gm->index_count, 1, 0, 0, 0);
                if (shadow_pass) ++csm_draws;
                else if (cubemap_pass) ++cube_draws;
                else {
                    ++main_draws;
                    main_pass_tris += gm->index_count / 3u;
                }
            }
        }
    };

    // ========================================================================
    // PASS 1: 3-Cascade Directional CSM Shadow Maps FIRST (with per-cascade culling)
    // ========================================================================
    list.setGraphicsPipelineLayout(g_eng.pbr_layout.get());
    list.setGraphicsDescriptorSet(g_eng.frame_ds[slot].get(), 0);
    for (uint32_t ci = 0; ci < CsmCascadeSet::kCascadeCount; ++ci) {
        if (!g_eng.csm_depth[ci] || !g_eng.csm_fb[ci]) continue;
        list.barriers(RenderBarrierStage::GRAPHICS,
                      RenderTextureBarrier(g_eng.csm_depth[ci].get(), RenderTextureLayout::DEPTH_WRITE));
        list.setFramebuffer(g_eng.csm_fb[ci].get());
        list.setViewports(RenderViewport(0.0f, 0.0f, (float)CsmCascadeSet::kResolution, (float)CsmCascadeSet::kResolution));
        list.setScissors(RenderRect(0, 0, (int32_t)CsmCascadeSet::kResolution, (int32_t)CsmCascadeSet::kResolution));
        list.clearDepth(true, 1.0f);
        if (frame->env.is_day || ci == 0u) {
            const Frustum csm_frustum = extract_frustum_from_vp(csm.view_proj[ci]);
            draw_static_world(true, ci, false, nullptr, false, csm_frustum, 0.0f);
            draw_dynamic_entities(true, ci, false, false, csm_frustum, nullptr, 0.0f);
        }
        list.setFramebuffer(nullptr);
        list.barriers(RenderBarrierStage::GRAPHICS,
                      RenderTextureBarrier(g_eng.csm_depth[ci].get(), RenderTextureLayout::SHADER_READ));
    }

    // ========================================================================
    // PASS 2: Dynamic 128x128 Environment Reflection Cubemap
    // (6 faces on first frame, then 1 face every 2 frames for locked 60 FPS!)
    // ========================================================================
    if (!g_eng.cube_faces_initialized || (g_eng.render_frame_id & 1u) == 0u) {
        const float *base_probe = frame->has_player_vehicle ? frame->player_vehicle_pos : cam.pos;
        const float probe_pos[3] = {base_probe[0], base_probe[1] + 1.4f, base_probe[2]};
        list.barriers(RenderBarrierStage::GRAPHICS,
                      RenderTextureBarrier(g_eng.env_cube.get(), RenderTextureLayout::COLOR_WRITE));
        list.barriers(RenderBarrierStage::GRAPHICS,
                      RenderTextureBarrier(g_eng.env_cube_depth.get(), RenderTextureLayout::DEPTH_WRITE));
        const uint32_t face_steps = g_eng.cube_faces_initialized ? 1u : 6u;
        for (uint32_t step = 0; step < face_steps; ++step) {
            const uint32_t face = (g_eng.cube_face_cursor + step) % 6u;
            if (!g_eng.env_face_fb[face] || !g_eng.cube_ubo[slot][face] || !g_eng.cube_ds[slot][face]) continue;
            FrameUBO cube_u = ubo;
            build_cubemap_face_vp(probe_pos, face, 1.0f, 600.0f, cube_u.view_proj);
            const Frustum cube_frustum = extract_frustum_from_vp(cube_u.view_proj);
            if (void *p = g_eng.cube_ubo[slot][face]->map()) {
                std::memcpy(p, &cube_u, sizeof(cube_u));
                g_eng.cube_ubo[slot][face]->unmap();
            }
            list.setFramebuffer(g_eng.env_face_fb[face].get());
            list.setViewports(RenderViewport(0.0f, 0.0f, (float)RemasterEngine::kCubeRes, (float)RemasterEngine::kCubeRes));
            list.setScissors(RenderRect(0, 0, (int32_t)RemasterEngine::kCubeRes, (int32_t)RemasterEngine::kCubeRes));
            const RenderColor sky_clear(frame->env.fog_color[0], frame->env.fog_color[1], frame->env.fog_color[2], 1.0f);
            list.clearColor(0, sky_clear);
            list.clearDepth(true, 1.0f);
            list.setGraphicsDescriptorSet(g_eng.cube_ds[slot][face].get(), 0);
            draw_static_world(false, 0, true, probe_pos, false, cube_frustum, 180.0f);
            draw_dynamic_entities(false, 0, true, false, cube_frustum, probe_pos, 120.0f);
            list.setFramebuffer(nullptr);
        }
        g_eng.cube_faces_initialized = true;
        g_eng.cube_face_cursor = (g_eng.cube_face_cursor + face_steps) % 6u;
        list.barriers(RenderBarrierStage::GRAPHICS,
                      RenderTextureBarrier(g_eng.env_cube.get(), RenderTextureLayout::SHADER_READ));
    }

    // ========================================================================
    // PASS 3: Forward+ Linear HDR PBR Geometry Pass (R16G16B16A16_FLOAT + D32_FLOAT)
    // ========================================================================
    const Frustum main_frustum = extract_frustum_from_vp(cam.view_proj);
    const RenderTextureBarrier hdr_in[] = {
        {g_eng.hdr_color.get(), RenderTextureLayout::COLOR_WRITE},
        {g_eng.hdr_depth.get(), RenderTextureLayout::DEPTH_WRITE},
    };
    list.barriers(RenderBarrierStage::GRAPHICS, hdr_in, 2);
    list.setFramebuffer(g_eng.hdr_fb.get());
    list.setViewports(RenderViewport(0.0f, 0.0f, (float)target.width, (float)target.height));
    list.setScissors(RenderRect(0, 0, (int32_t)target.width, (int32_t)target.height));
    const RenderColor bg_clear(frame->env.fog_color[0], frame->env.fog_color[1], frame->env.fog_color[2], 1.0f);
    list.clearColor(0, bg_clear);
    list.clearDepth(true, 1.0f);
    list.setGraphicsDescriptorSet(g_eng.frame_ds[slot].get(), 0);
    // Pass 3A: Opaque + Alpha-Cutout geometry (CPU frustum culled, batched static world)
    draw_static_world(false, 0, false, cam.pos, false, main_frustum, 0.0f);
    draw_dynamic_entities(false, 0, false, false, main_frustum, cam.pos, 0.0f);
    // Pass 3B: True Linear-HDR Alpha-Blended + Additive geometry (road decals, glass, bridge lights, spotlight cones)
    draw_static_world(false, 0, false, cam.pos, true, main_frustum, 0.0f);
    draw_dynamic_entities(false, 0, false, true, main_frustum, cam.pos, 0.0f);
    list.setFramebuffer(nullptr);

    // ========================================================================
    // PASS 4: Single Khronos PBR Neutral Linear-to-sRGB Resolve + 2D HUD Composite
    // ========================================================================
    const RenderTextureBarrier res_bar[] = {
        {g_eng.hdr_color.get(), RenderTextureLayout::SHADER_READ},
        {g_eng.cur_out_wrapper, RenderTextureLayout::COLOR_WRITE},
    };
    list.barriers(RenderBarrierStage::GRAPHICS, res_bar, 2);
    list.setFramebuffer(g_eng.cur_out_fb);
    list.setViewports(RenderViewport(0.0f, 0.0f, (float)target.width, (float)target.height));
    list.setScissors(RenderRect(0, 0, (int32_t)target.width, (int32_t)target.height));
    list.setGraphicsPipelineLayout(g_eng.resolve_layout.get());
    list.setPipeline(g_eng.pipe_resolve.get());
    list.setGraphicsDescriptorSet(g_eng.resolve_ds.get(), 0);
    list.drawInstanced(3, 1, 0, 0);

    // Composite crisp 2D HUD quads AFTER neutral resolve (never tonemapped)
    if (!frame->hud_quads.empty()) {
        const size_t max_q = std::min<size_t>(frame->hud_quads.size(), 512u);
        std::vector<HudVertex> hverts;
        hverts.reserve(max_q * 6);
        for (size_t i = 0; i < max_q; ++i) {
            const auto &q = frame->hud_quads[i];
            const float r = static_cast<float>(q.rgba & 0xFFu) / 255.0f;
            const float g = static_cast<float>((q.rgba >> 8) & 0xFFu) / 255.0f;
            const float b = static_cast<float>((q.rgba >> 16) & 0xFFu) / 255.0f;
            const float a = static_cast<float>((q.rgba >> 24) & 0xFFu) / 255.0f;
            const HudVertex v0{{q.x0, q.y0}, {q.u0, q.v0}, {r, g, b, a}};
            const HudVertex v1{{q.x1, q.y0}, {q.u1, q.v0}, {r, g, b, a}};
            const HudVertex v2{{q.x1, q.y1}, {q.u1, q.v1}, {r, g, b, a}};
            const HudVertex v3{{q.x0, q.y1}, {q.u0, q.v1}, {r, g, b, a}};
            hverts.push_back(v0); hverts.push_back(v1); hverts.push_back(v2);
            hverts.push_back(v0); hverts.push_back(v2); hverts.push_back(v3);
        }
        if (void *p = g_eng.hud_vbo[slot]->map()) {
            std::memcpy(p, hverts.data(), hverts.size() * sizeof(HudVertex));
            g_eng.hud_vbo[slot]->unmap();
        }
        list.setPipeline(g_eng.pipe_hud.get());
        const RenderInputSlot hud_slot(0, sizeof(HudVertex));
        const RenderVertexBufferView vbv(g_eng.hud_vbo[slot].get(), hverts.size() * sizeof(HudVertex));
        list.setVertexBuffers(0, &vbv, 1, &hud_slot);
        list.drawInstanced(static_cast<uint32_t>(hverts.size()), 1, 0, 0);
    }

    list.setFramebuffer(nullptr);
    list.barriers(RenderBarrierStage::ALL,
                  RenderTextureBarrier(g_eng.cur_out_wrapper, RenderTextureLayout::GENERAL));

    vk_list->vk = saved_vk;

    const double frame_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_frame_start).count();
    if (g_eng.render_frame_id > 30) {
        g_eng.frame_ms_sum += frame_ms;
        g_eng.frame_ms_max = std::max(g_eng.frame_ms_max, frame_ms);
        ++g_eng.frame_ms_count;
    }

    if ((!g_eng.logged_first_draw || g_eng.render_frame_id == 460) && main_pass_tris > 0) {
        g_eng.logged_first_draw = true;
        uint32_t decoded_tex = 0, cutout_tex = 0, blend_tex = 0, add_tex = 0;
        for (const auto &kv : g_eng.gpu_textures) {
            if (kv.second && kv.second->albedo_w > 1) {
                ++decoded_tex;
                if (kv.second->has_alpha_cutout) ++cutout_tex;
                if (kv.second->has_alpha_blend) ++blend_tex;
                if (kv.second->has_alpha_additive) ++add_tex;
            }
        }
        uint32_t veh_count = 0, drawn_ent = 0;
        for (const auto &e : frame->entities) {
            if (e.is_vehicle) ++veh_count;
            if (!e.part_handles.empty()) ++drawn_ent;
        }
        const double mean_ms = g_eng.frame_ms_count > 0 ? (g_eng.frame_ms_sum / g_eng.frame_ms_count) : frame_ms;
        std::fprintf(stderr,
                     "[remaster] frame %llu rendered (%ux%u, debug=%u): static_meshes=%zu static_batches=%zu "
                     "dynamic_entities=%zu (drawn=%u vehicles=%u) draws(csm=%u cube=%u main=%u total=%u) "
                     "cpu_ms(cur=%.2f mean=%.2f max=%.2f) event_mask=0x%x "
                     "decoded_retail_textures=%u/%zu (cutout=%u blend=%u add=%u) main_tris=%llu "
                     "is_day=%d sun=%.2f amb=%.2f lights=%u cam=(%.1f,%.1f,%.1f)\n",
                     (unsigned long long)g_eng.render_frame_id, target.width, target.height,
                     g_debug_mode.load(std::memory_order_relaxed),
                     world ? world->instances.size() : 0u, g_eng.static_batches.size(),
                     frame->entities.size(), drawn_ent, veh_count, csm_draws, cube_draws, main_draws,
                     csm_draws + cube_draws + main_draws, frame_ms, mean_ms, g_eng.frame_ms_max,
                     frame->session_event_mask, decoded_tex, g_eng.gpu_textures.size(),
                     cutout_tex, blend_tex, add_tex, (unsigned long long)main_pass_tris,
                     frame->env.is_day ? 1 : 0, frame->env.sun_intensity, frame->env.ambient_intensity,
                     n_lights, cam.pos[0], cam.pos[1], cam.pos[2]);
    }

    return true;
}

void shutdown() {
    g_eng = RemasterEngine{};
}

#else

bool initialize() { return false; }
bool is_ready() { return false; }
bool render_frame(const OutputTarget &) { return false; }
void shutdown() {}

#endif

} // namespace remaster
