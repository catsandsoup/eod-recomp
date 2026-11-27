// runtime/remaster/scene_bridge.cpp
// Pure EE Scene Graph Bridge — ZERO VU1 / VIF1 / GS dependency.
#include "scene_bridge.h"
#include "asset_registry.h"
#include "ee_texture_decoder.h"
#include "../core/memory.h"
#include "../render/editor_environment.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace remaster {

namespace {

inline bool valid_addr(uint32_t addr, uint32_t bytes, uint32_t ram_size) {
    return addr <= ram_size && bytes <= ram_size - addr;
}

inline uint32_t rd_u32(const uint8_t *ram, uint32_t addr) {
    uint32_t v = 0;
    std::memcpy(&v, ram + addr, 4);
    return v;
}

inline float rd_f32(const uint8_t *ram, uint32_t addr) {
    float v = 0.0f;
    std::memcpy(&v, ram + addr, 4);
    return v;
}

inline float srgb_to_linear(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline void unpack_rrggbb_linear(uint32_t rgb, float out[3]) {
    out[0] = srgb_to_linear(static_cast<float>((rgb >> 16) & 0xFFu) / 255.0f);
    out[1] = srgb_to_linear(static_cast<float>((rgb >> 8) & 0xFFu) / 255.0f);
    out[2] = srgb_to_linear(static_cast<float>(rgb & 0xFFu) / 255.0f);
}

inline void unpack_bbggrr_linear(uint32_t bgr, float out[3]) {
    out[0] = srgb_to_linear(static_cast<float>(bgr & 0xFFu) / 255.0f);
    out[1] = srgb_to_linear(static_cast<float>((bgr >> 8) & 0xFFu) / 255.0f);
    out[2] = srgb_to_linear(static_cast<float>((bgr >> 16) & 0xFFu) / 255.0f);
}

void mul_mat4(const float a[16], const float b[16], float out[16]) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
    }
}

struct CachedModelPart {
    uint32_t mesh_handle = 0;
    int16_t layer_index = -1;
};

struct BridgeState {
    std::mutex mtx;
    SceneFrame building{};
    std::shared_ptr<const SceneFrame> published;
    std::shared_ptr<const LoadedWorld> world;
    uint32_t loaded_graf_addr = 0;
    uint32_t track_generation = 0;
    std::unordered_map<uint32_t, std::vector<float>> bone_palettes;
    std::unordered_map<uint32_t, std::vector<float>> dent_offsets;
    std::unordered_map<uint64_t, std::vector<CachedModelPart>> dynamic_model_parts; // key = (model_handle<<32)|header_addr
    std::unordered_map<uint64_t, bool> dynamic_model_is_sky;                        // suppress sky double-draw/shadow-cast
    bool in_water = false;
    WaterMesh cur_water{};
};

BridgeState &state() {
    static BridgeState s;
    return s;
}

} // namespace

SceneEnvironment extract_environment_from_ram(const uint8_t *ram, uint32_t ram_size,
                                              const editor_environment::Override *editor_override) {
    SceneEnvironment env{};
    // Bright, crisp daylight defaults (EOD world: +Y is UP, +X right, +Z forward).
    // Strong direct sun vs. cool sky-fill ambient (~3.5:1 ratio) gives crisp grounded shadows!
    env.sun_dir[0] = 0.35f; env.sun_dir[1] = 0.82f; env.sun_dir[2] = -0.45f;
    env.sun_intensity = 5.6f;
    env.ambient_sky[0] = 0.62f; env.ambient_sky[1] = 0.66f; env.ambient_sky[2] = 0.72f;
    env.ambient_ground[0] = 0.30f; env.ambient_ground[1] = 0.28f; env.ambient_ground[2] = 0.25f;
    env.ambient_intensity = 0.58f;
    env.shadow_strength = 1.0f;

    if (ram && valid_addr(ee_addr::kSunDir, 12, ram_size)) {
        // 0x4CD2E8 (written by Sun slot 6 0x2236B0 -> 0x34E880) stores the DOWNWARD light-travel
        // ray in EOD world space (+Y is UP; e.g. day default (0, -0.8575, +0.5145)).
        // NEGATE it so env.sun_dir points TOWARD the sun (dy > 0).
        float dx = -rd_f32(ram, ee_addr::kSunDir + 0);
        float dy = -rd_f32(ram, ee_addr::kSunDir + 4);
        float dz = -rd_f32(ram, ee_addr::kSunDir + 8);
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (std::isfinite(len) && len > 1e-4f) {
            dx /= len; dy /= len; dz /= len;
            // Ensure sun is always above the horizon (+Y is UP)
            if (dy < 0.0f) { dx = -dx; dy = -dy; dz = -dz; }
            if (dy < 0.15f) {
                dy = 0.15f;
                const float nlen = std::sqrt(dx * dx + dy * dy + dz * dz);
                dx /= nlen; dy /= nlen; dz /= nlen;
            }
            env.sun_dir[0] = dx;
            env.sun_dir[1] = dy;
            env.sun_dir[2] = dz;
        }
    }

    if (ram && valid_addr(ee_addr::kSunIsDay, 4, ram_size)) {
        env.is_day = (rd_u32(ram, ee_addr::kSunIsDay) != 0);
    }

    if (ram && valid_addr(ee_addr::kSunColor, 4, ram_size)) {
        const uint32_t sc = rd_u32(ram, ee_addr::kSunColor) & 0x00FFFFFFu;
        if (sc != 0) unpack_rrggbb_linear(sc, env.sun_color);
    }

    if (env.is_day) {
        // Strictly neutral daylight white (ZERO yellow/orange push)
        env.sun_color[0] = env.sun_color[0] * 0.25f + 1.00f * 0.75f;
        env.sun_color[1] = env.sun_color[1] * 0.25f + 0.99f * 0.75f;
        env.sun_color[2] = env.sun_color[2] * 0.25f + 0.97f * 0.75f;
    } else {
        // Night tracks (e.g. Anoka Night): soft cool-white moon/stadium overhead fill
        env.sun_color[0] = 0.85f;
        env.sun_color[1] = 0.90f;
        env.sun_color[2] = 1.00f;
    }

    if (ram && valid_addr(ee_addr::kSunIntensity, 4, ram_size)) {
        const float raw_int = rd_f32(ram, ee_addr::kSunIntensity);
        if (std::isfinite(raw_int) && raw_int > 0.01f && raw_int <= 4.0f) {
            env.sun_intensity = env.is_day ? (raw_int * 5.2f) : 0.75f;
        } else if (!env.is_day) {
            env.sun_intensity = 0.75f;
        }
    } else if (!env.is_day) {
        env.sun_intensity = 0.75f;
    }

    if (!env.is_day) {
        // Readable cool-night stadium hemisphere ambient: infield grass, dirt oval, guardrails,
        // bridge, and cars stay clearly legible while keeping the night sky genuinely dark.
        env.ambient_intensity = 0.85f;
        env.ambient_sky[0] = 0.22f; env.ambient_sky[1] = 0.27f; env.ambient_sky[2] = 0.36f;
        env.ambient_ground[0] = 0.12f; env.ambient_ground[1] = 0.13f; env.ambient_ground[2] = 0.15f;
    }

    if (ram && valid_addr(ee_addr::kAmbientColor, 4, ram_size)) {
        const uint32_t amb = rd_u32(ram, ee_addr::kAmbientColor);
        if ((amb & 0x00FFFFFFu) != 0) {
            float amb_lin[3];
            unpack_rrggbb_linear(amb, amb_lin);
            if (env.is_day) {
                // Clean, neutral daylight hemisphere fill (ZERO blue cast in concrete/billboard shadows)
                const float amb_luma = 0.2126f * amb_lin[0] + 0.7152f * amb_lin[1] + 0.0722f * amb_lin[2];
                const float amb_scale = std::clamp(0.78f + amb_luma * 0.45f, 0.70f, 1.15f);
                env.ambient_sky[0] = 0.62f * amb_scale;
                env.ambient_sky[1] = 0.66f * amb_scale;
                env.ambient_sky[2] = 0.72f * amb_scale;
                env.ambient_ground[0] = 0.30f * amb_scale;
                env.ambient_ground[1] = 0.28f * amb_scale;
                env.ambient_ground[2] = 0.25f * amb_scale;
            } else {
                // Gently modulate night ambient with track's EE ambient while preserving full readability
                env.ambient_sky[0] = 0.22f + amb_lin[0] * 0.12f;
                env.ambient_sky[1] = 0.27f + amb_lin[1] * 0.14f;
                env.ambient_sky[2] = 0.36f + amb_lin[2] * 0.18f;
                env.ambient_ground[0] = 0.12f + amb_lin[0] * 0.08f;
                env.ambient_ground[1] = 0.13f + amb_lin[1] * 0.08f;
                env.ambient_ground[2] = 0.15f + amb_lin[2] * 0.09f;
            }
        }
    }

    // Physical CSM shadows always fully occlude DIRECT sunlight (1.0); hemisphere ambient fills shadows.
    env.shadow_strength = 1.0f;

    if (ram && valid_addr(ee_addr::kFogColor, 4, ram_size)) {
        unpack_bbggrr_linear(rd_u32(ram, ee_addr::kFogColor), env.fog_color);
    }

    if (ram && valid_addr(ee_addr::kFogRange, 12, ram_size)) {
        const float fs = rd_f32(ram, ee_addr::kFogRange + 0);
        const float fe = rd_f32(ram, ee_addr::kFogRange + 4);
        const float fd = rd_f32(ram, ee_addr::kFogRange + 8);
        if (std::isfinite(fs) && std::isfinite(fe) && fe > fs) {
            env.fog_start = std::max(1.0f, fs);
            env.fog_end = fe;
            env.fog_density = std::clamp(fd, 0.0f, 1.0f);
        }
    }

    // Twinmotion/Lumion-style World Editor override: cleanly overlays any field while
    // ALWAYS keeping real CSM shadows active (NO !editor_environment shadow disable!).
    if (editor_override && !editor_override->track.empty()) {
        env.editor_override_active = true;
        const float slen = std::sqrt(editor_override->sun_dir[0] * editor_override->sun_dir[0] +
                                     editor_override->sun_dir[1] * editor_override->sun_dir[1] +
                                     editor_override->sun_dir[2] * editor_override->sun_dir[2]);
        if (slen > 1e-4f) {
            env.sun_dir[0] = editor_override->sun_dir[0] / slen;
            env.sun_dir[1] = editor_override->sun_dir[1] / slen;
            env.sun_dir[2] = editor_override->sun_dir[2] / slen;
        }
        for (int k = 0; k < 3; ++k) {
            env.sun_color[k] = srgb_to_linear(editor_override->sun_color[k]);
            env.fog_color[k] = srgb_to_linear(editor_override->fog_color[k]);
        }
        env.ambient_intensity = std::clamp(editor_override->ambient_intensity, 0.05f, 2.0f);
        env.fog_start = editor_override->fog_start;
        env.fog_end = std::max(editor_override->fog_start + 1.0f, editor_override->fog_end);
        env.fog_density = std::clamp(editor_override->fog_density, 0.0f, 1.0f);
        env.shadow_strength = 1.0f; // Shadows remain fully active with World Editor overrides!
    }

    return env;
}

void rebuild_camera_projection(SceneCamera &cam, uint32_t target_width, uint32_t target_height) {
    cam.aspect = (target_height > 0)
                     ? (static_cast<float>(target_width) / static_cast<float>(target_height))
                     : (16.0f / 9.0f);
    // Match 0x35B3D0 (native_feed.cpp:901, 936):
    //   sy = -0.93333334 / ee_aspect; y_ndc = sy * fov_scale * (vp_w / vp_h) * (y_v / z_v)
    // EOD view space is +Y UP; Vulkan NDC is -Y UP, so proj[5] MUST be NEGATIVE (-fy)!
    const float ee_asp = (std::isfinite(cam.ee_aspect) && cam.ee_aspect > 0.1f && cam.ee_aspect < 10.0f)
                             ? cam.ee_aspect
                             : 1.0f;
    const float vp_asp = (std::isfinite(cam.vp_aspect) && cam.vp_aspect > 0.1f && cam.vp_aspect < 10.0f)
                             ? cam.vp_aspect
                             : (640.0f / 448.0f);
    const float fy = cam.fov_scale * (0.93333334f / ee_asp) * vp_asp;
    const float fx = fy / std::max(0.1f, cam.aspect);
    // Reversed-Z projection (near -> 1.0, far -> 0.0) for D32_FLOAT:
    // Eliminates far-plane depth quantization across large tracks (near=0.35m, far=9200m).
    const float z_range = std::max(1.0f, cam.far_z - cam.near_z);
    const float a = -cam.near_z / z_range;
    const float b = (cam.far_z * cam.near_z) / z_range;
    const float proj[16] = {
        fx,   0.0f, 0.0f, 0.0f,
        0.0f, -fy,  0.0f, 0.0f,
        0.0f, 0.0f, a,    b,
        0.0f, 0.0f, 1.0f, 0.0f
    };
    std::memcpy(cam.proj, proj, sizeof(proj));
    mul_mat4(cam.proj, cam.view, cam.view_proj);
}

SceneCamera extract_camera_from_ram(const uint8_t *ram, uint32_t ram_size,
                                    uint32_t target_width, uint32_t target_height) {
    SceneCamera cam{};
    if (!ram || !valid_addr(ee_addr::kCameraRotation, 48, ram_size)) return cam;

    for (int i = 0; i < 9; ++i) cam.rotation[i] = rd_f32(ram, ee_addr::kCameraRotation + i * 4);
    for (int i = 0; i < 3; ++i) cam.pos[i] = rd_f32(ram, ee_addr::kCameraPosition + i * 4);

    if (valid_addr(ee_addr::kNearZ, 16, ram_size)) {
        const float nz = rd_f32(ram, ee_addr::kNearZ);
        const float fz = rd_f32(ram, ee_addr::kFarZ);
        const float fs = rd_f32(ram, ee_addr::kFovScale);
        if (std::isfinite(nz) && nz > 0.01f) cam.near_z = nz;
        if (std::isfinite(fz) && fz > cam.near_z + 1.0f) cam.far_z = std::max(fz, 8000.0f);
        if (std::isfinite(fs) && fs > 0.05f && fs < 10.0f) cam.fov_scale = fs;
    }
    if (valid_addr(ee_addr::kAspect, 4, ram_size)) {
        const float asp = rd_f32(ram, ee_addr::kAspect);
        if (std::isfinite(asp) && asp > 0.1f && asp < 10.0f) cam.ee_aspect = asp;
    }
    if (valid_addr(ee_addr::kViewport, 16, ram_size)) {
        const float vw = rd_f32(ram, ee_addr::kViewport + 8);
        const float vh = rd_f32(ram, ee_addr::kViewport + 12);
        if (std::isfinite(vw) && std::isfinite(vh) && vw > 16.0f && vh > 16.0f) {
            cam.vp_aspect = vw / vh;
        }
    }

    // Build row-major View matrix: P_view = rot * (P_world - cam.pos)
    // EOD camera space: +X right, +Y UP, +Z forward.
    const float *rot = cam.rotation;
    const float *P = cam.pos;
    const float tx = -(rot[0] * P[0] + rot[1] * P[1] + rot[2] * P[2]);
    const float ty = -(rot[3] * P[0] + rot[4] * P[1] + rot[5] * P[2]);
    const float tz = -(rot[6] * P[0] + rot[7] * P[1] + rot[8] * P[2]);
    const float view[16] = {
        rot[0], rot[1], rot[2], tx,
        rot[3], rot[4], rot[5], ty,
        rot[6], rot[7], rot[8], tz,
        0.0f,   0.0f,   0.0f,   1.0f
    };
    std::memcpy(cam.view, view, sizeof(view));

    rebuild_camera_projection(cam, target_width, target_height);
    cam.valid = std::isfinite(cam.pos[0]) && std::isfinite(cam.pos[1]) && std::isfinite(cam.pos[2]);
    return cam;
}

void extract_flare_lights_from_ram(const uint8_t *ram, uint32_t ram_size,
                                   bool is_day, std::vector<DynamicLight> &out_lights) {
    if (!ram || !valid_addr(ee_addr::kFlareCount, 4, ram_size)) return;
    const uint32_t count = std::min<uint32_t>(rd_u32(ram, ee_addr::kFlareCount), 24u);
    if (count == 0 || !valid_addr(ee_addr::kFlareArray, count * 0x2C, ram_size)) return;

    // Per SLUS_209.10:0x305330..0x305338, FlareSource pos is at entry +0x04, +0x08, +0x0C.
    // Compute track centroid beneath floodlight towers to aim stadium lights downward (-Y) & inward.
    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    uint32_t valid_count = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t base = ee_addr::kFlareArray + i * 0x2C;
        const float px = rd_f32(ram, base + 0x04);
        const float py = rd_f32(ram, base + 0x08);
        const float pz = rd_f32(ram, base + 0x0C);
        if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)) continue;
        ax += px; ay += py; az += pz;
        ++valid_count;
    }
    if (valid_count == 0) return;
    const float inv_n = 1.0f / static_cast<float>(valid_count);
    ax *= inv_n;
    ay = ay * inv_n - 14.0f; // -Y is DOWN toward track surface (+Y is UP)
    az *= inv_n;

    const float gain = is_day ? 0.15f : 1.0f;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t base = ee_addr::kFlareArray + i * 0x2C;
        const float px = rd_f32(ram, base + 0x04);
        const float py = rd_f32(ram, base + 0x08);
        const float pz = rd_f32(ram, base + 0x0C);
        if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz)) continue;

        float dx = ax - px, dy = ay - py, dz = az - pz;
        const float dlen = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dlen > 1e-4f) { dx /= dlen; dy /= dlen; dz /= dlen; }
        else { dx = 0.0f; dy = -1.0f; dz = 0.0f; }

        DynamicLight l{};
        l.pos[0] = px; l.pos[1] = py; l.pos[2] = pz;
        l.radius = is_day ? 60.0f : 160.0f;
        // Neutral metal-halide stadium white (ZERO orange/amber tint)
        l.color[0] = 0.95f; l.color[1] = 0.97f; l.color[2] = 1.00f;
        l.intensity = (is_day ? 1.2f : 4.5f) * gain;
        l.dir[0] = dx; l.dir[1] = dy; l.dir[2] = dz;
        l.spot_cos = is_day ? 0.22f : -0.25f;
        out_lights.push_back(l);
    }
}

void ee_begin_frame(uint64_t vblank) {
    auto &s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.building = SceneFrame{};
    s.building.frame_index = vblank;
    s.building.track_generation = s.track_generation;
    if (s.world) s.building.track_name = s.world->track_name;
    s.bone_palettes.clear();
    s.dent_offsets.clear();
    s.in_water = false;
    s.cur_water = WaterMesh{};
}

void ee_sync_world_from_ram(const std::string &track_name) {
    const uint32_t graf_addr = valid_addr(ee_addr::kGrafPtr, 4, EE_RAM_SIZE)
                                   ? rd_u32(g_ram, ee_addr::kGrafPtr)
                                   : 0;
    if (!graf_addr) return;

    auto &s = state();
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        if (s.loaded_graf_addr == graf_addr && s.world) return;
    }

    auto world = std::make_shared<LoadedWorld>();
    if (load_world_from_graf(g_ram, EE_RAM_SIZE, graf_addr, track_name.empty() ? "TRACK" : track_name, *world)) {
        std::lock_guard<std::mutex> lk(s.mtx);
        s.world = std::move(world);
        s.loaded_graf_addr = graf_addr;
        ++s.track_generation;
    }
}

void ee_record_bone_palette(uint32_t packet_addr, const float *matrices_4x4, uint32_t bone_count) {
    if (!packet_addr || !matrices_4x4 || bone_count == 0) return;
    auto &s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    auto &dst = s.bone_palettes[packet_addr];
    dst.assign(matrices_4x4, matrices_4x4 + static_cast<size_t>(bone_count) * 16);
}

void ee_record_dent_offsets(uint32_t packet_addr, const float *offsets_xyz, uint32_t count) {
    if (!packet_addr || !offsets_xyz || count == 0) return;
    auto &s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    auto &dst = s.dent_offsets[packet_addr];
    dst.assign(offsets_xyz, offsets_xyz + static_cast<size_t>(count) * 3);
}

void ee_submit_entity(uint32_t model_handle, const float world_matrix[12],
                      uint32_t material_packet, bool is_player_vehicle,
                      uint32_t layer_mask) {
    if (!world_matrix || model_handle == 0xFFFFFFFFu) return;
    auto &s = state();
    std::lock_guard<std::mutex> lk(s.mtx);

    EntityInstance inst{};
    inst.model_handle = model_handle;
    inst.asset_id = "entity/model_" + std::to_string(model_handle);
    std::memcpy(inst.world_matrix, world_matrix, sizeof(inst.world_matrix));
    inst.is_player_vehicle = is_player_vehicle;
    inst.cast_shadow = true;

    if (material_packet && material_packet != 0xFFFFFFFFu) {
        if (auto bit = s.bone_palettes.find(material_packet); bit != s.bone_palettes.end())
            inst.bone_palette = bit->second;
        if (auto dit = s.dent_offsets.find(material_packet); dit != s.dent_offsets.end()) {
            inst.dent_offsets = dit->second;
            inst.is_vehicle = true;
        }
    }

    // Decode & cache the dynamic mrModel directly from EE RAM (*(0x4C96C0) + model_handle * 0x14 + 0x08)
    // so every vehicle, wheel, driver, and prop renders with real smooth PBR meshes and casts CSM shadows!
    if (valid_addr(ee_addr::kModelTablePtr, 4, EE_RAM_SIZE)) {
        const uint32_t tbl = rd_u32(g_ram, ee_addr::kModelTablePtr);
        const uint32_t slot_addr = tbl + model_handle * 0x14u;
        if (tbl && model_handle < 4096u && valid_addr(slot_addr, 0x14, EE_RAM_SIZE)) {
            const uint32_t hdr_addr = rd_u32(g_ram, slot_addr + 0x08u);
            const uint32_t keys_addr = rd_u32(g_ram, slot_addr + 0x10u);
            if (hdr_addr && hdr_addr < EE_RAM_SIZE) {
                if (s.world && s.world->sky_model_addrs.count(hdr_addr)) {
                    // Sky dome is already rendered from static GRAF with camera-centering & no depth-write.
                    return;
                }
                const uint64_t key = (static_cast<uint64_t>(model_handle) << 32) | hdr_addr;
                auto it = s.dynamic_model_parts.find(key);
                if (it == s.dynamic_model_parts.end()) {
                    std::vector<CpuMeshData> parts;
                    const std::string prefix = (inst.is_vehicle ? "car/model_" : "entity/model_") +
                                               std::to_string(model_handle);
                    std::vector<CachedModelPart> cached;
                    bool any_sky = false;
                    if (decode_mrmodel_from_ram(g_ram, EE_RAM_SIZE, hdr_addr, prefix, inst.is_vehicle, parts)) {
                        cached.reserve(parts.size());
                        for (auto &part : parts) {
                            // Bind per-instance EE texture slot (CT.RES car liveries & dynamic textures)
                            if (keys_addr && valid_addr(keys_addr + part.part_index * 4u, 4u, EE_RAM_SIZE)) {
                                const int32_t tex_slot = static_cast<int32_t>(rd_u32(g_ram, keys_addr + part.part_index * 4u));
                                if (tex_slot >= 0 && tex_slot < 512) {
                                    std::string slot_name = get_ee_texture_slot_name(g_ram, EE_RAM_SIZE, tex_slot);
                                    if (!slot_name.empty()) {
                                        part.texture_id = "slot/" + std::to_string(tex_slot) + "/" + slot_name;
                                        part.material = classify_pbr_material(slot_name, false, inst.is_vehicle);
                                    }
                                }
                            }
                            if (part.material.is_sky) any_sky = true;
                            const std::string aid = part.asset_id;
                            const int16_t lidx = part.layer_index;
                            cached.push_back({AssetRegistry::instance().register_mesh(aid, std::move(part)), lidx});
                        }
                    }
                    it = s.dynamic_model_parts.emplace(key, std::move(cached)).first;
                    s.dynamic_model_is_sky[key] = any_sky;
                }
                if (s.dynamic_model_is_sky[key]) {
                    // Sky dome is already rendered from static GRAF with camera-centering & no depth-write.
                    // Suppress dynamic submission so it is never double-drawn or shadow-cast.
                    return;
                }
                inst.part_handles.reserve(it->second.size());
                for (const auto &cp : it->second) {
                    if (layer_mask != 0xFFFFFFFFu && cp.layer_index >= 0) {
                        const uint32_t bit = 1u << (static_cast<uint32_t>(cp.layer_index) & 31u);
                        if ((layer_mask & bit) == 0u) continue;
                    }
                    inst.part_handles.push_back(cp.mesh_handle);
                }
            }
        }
    }

    // Detect player 0's vehicle from EE participant table 0x447458 (CarObject position at +0x88..+0x90).
    // EE world_matrix is 4x3 row-vector: translation is in world_matrix[9], [10], [11].
    const float ex = world_matrix[9];
    const float ey = world_matrix[10];
    const float ez = world_matrix[11];
    if (!inst.is_player_vehicle && valid_addr(ee_addr::kParticipantTbl, 4, EE_RAM_SIZE)) {
        const uint32_t car0 = rd_u32(g_ram, ee_addr::kParticipantTbl);
        if (car0 && valid_addr(car0 + 0x88u, 12, EE_RAM_SIZE)) {
            const float px = rd_f32(g_ram, car0 + 0x88u);
            const float py = rd_f32(g_ram, car0 + 0x8Cu);
            const float pz = rd_f32(g_ram, car0 + 0x90u);
            const float dx = ex - px, dy = ey - py, dz = ez - pz;
            if (std::isfinite(dx) && std::isfinite(dy) && std::isfinite(dz) &&
                (dx * dx + dy * dy + dz * dz) < 6.25f) {
                inst.is_player_vehicle = true;
                inst.is_vehicle = true;
            }
        }
    }

    if (inst.is_player_vehicle || (inst.is_vehicle && !s.building.has_player_vehicle)) {
        s.building.player_vehicle_pos[0] = ex;
        s.building.player_vehicle_pos[1] = ey;
        s.building.player_vehicle_pos[2] = ez;
        s.building.has_player_vehicle = true;
    }

    s.building.entities.push_back(std::move(inst));
}

void ee_submit_dynamic_light(const DynamicLight &light) {
    auto &s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (s.building.lights.size() < 64) s.building.lights.push_back(light);
}

void ee_submit_hud_quad(const HudQuad &quad) {
    auto &s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.building.hud_quads.push_back(quad);
}

void ee_water_begin(uint32_t /*surface_addr*/, int32_t texture_slot) {
    auto &s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.cur_water = WaterMesh{};
    if (texture_slot >= 0 && texture_slot < 512) {
        std::string slot_name = get_ee_texture_slot_name(g_ram, EE_RAM_SIZE, texture_slot);
        if (slot_name.empty()) slot_name = "water";
        s.cur_water.texture_id = "slot/" + std::to_string(texture_slot) + "/" + slot_name;
    } else {
        s.cur_water.texture_id = "tex/water";
    }
    s.cur_water.vertices.reserve(512);
    s.cur_water.indices.reserve(1024);
    s.in_water = true;
}

bool ee_in_water() {
    auto &s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    return s.in_water;
}

void ee_water_vertex(const float xyz[3], const float st[2], uint32_t /*colour*/, uint32_t strip) {
    if (!xyz || !st) return;
    if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) || !std::isfinite(xyz[2])) return;
    auto &s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (!s.in_water) return;

    PbrVertex v{};
    v.pos[0] = xyz[0];
    v.pos[1] = xyz[1];
    v.pos[2] = xyz[2];
    v.normal[0] = 0.0f;
    v.normal[1] = 1.0f;
    v.normal[2] = 0.0f;
    v.tangent[0] = 1.0f;
    v.tangent[1] = 0.0f;
    v.tangent[2] = 0.0f;
    v.tangent[3] = 1.0f;
    v.uv[0] = std::isfinite(st[0]) ? st[0] : 0.0f;
    v.uv[1] = std::isfinite(st[1]) ? st[1] : 0.0f;

    const uint32_t idx = static_cast<uint32_t>(s.cur_water.vertices.size());
    s.cur_water.vertices.push_back(v);
    if (strip != 0u && idx >= 2u) {
        s.cur_water.indices.push_back(idx - 2u);
        s.cur_water.indices.push_back(idx - 1u);
        s.cur_water.indices.push_back(idx);
    }
}

void ee_water_end(bool drawn) {
    auto &s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (!s.in_water) return;
    s.in_water = false;

    if (!drawn || s.cur_water.vertices.empty() || s.cur_water.indices.size() < 3u) {
        s.cur_water = WaterMesh{};
        return;
    }

    auto &verts = s.cur_water.vertices;
    auto &inds = s.cur_water.indices;

    // Ensure every triangle in the EE water strip is consistently oriented +Y UP so
    // alternating strip triangles never cancel out during smooth normal accumulation.
    for (size_t t = 0; t + 2 < inds.size(); t += 3) {
        const auto &p0 = verts[inds[t + 0]].pos;
        const auto &p1 = verts[inds[t + 1]].pos;
        const auto &p2 = verts[inds[t + 2]].pos;
        const float e1x = p1[0] - p0[0], e1z = p1[2] - p0[2];
        const float e2x = p2[0] - p0[0], e2z = p2[2] - p0[2];
        const float ny = e1z * e2x - e1x * e2z;
        if (ny < 0.0f) {
            std::swap(inds[t + 1], inds[t + 2]);
        }
    }

    // Weld adjacent water tile corners and compute smooth area-weighted wave normals (+Y UP).
    compute_smooth_normals_and_tangents(verts, inds, 1e-2f);

    float bmin[3] = {verts[0].pos[0], verts[0].pos[1], verts[0].pos[2]};
    float bmax[3] = {bmin[0], bmin[1], bmin[2]};
    for (const auto &v : verts) {
        for (int k = 0; k < 3; ++k) {
            bmin[k] = std::min(bmin[k], v.pos[k]);
            bmax[k] = std::max(bmax[k], v.pos[k]);
        }
    }
    for (int k = 0; k < 3; ++k) s.cur_water.center[k] = 0.5f * (bmin[k] + bmax[k]);
    float max_r2 = 0.0f;
    for (const auto &v : verts) {
        const float dx = v.pos[0] - s.cur_water.center[0];
        const float dy = v.pos[1] - s.cur_water.center[1];
        const float dz = v.pos[2] - s.cur_water.center[2];
        max_r2 = std::max(max_r2, dx * dx + dy * dy + dz * dz);
    }
    s.cur_water.radius = std::sqrt(max_r2);

    s.building.water_meshes.push_back(std::move(s.cur_water));
    s.cur_water = WaterMesh{};
}

void ee_end_frame(const editor_environment::Override *editor_override) {
    auto &s = state();
    std::lock_guard<std::mutex> lk(s.mtx);

    s.building.env = extract_environment_from_ram(g_ram, EE_RAM_SIZE, editor_override);
    s.building.camera = extract_camera_from_ram(g_ram, EE_RAM_SIZE, 1280, 720);
    s.building.lights.clear();
    extract_flare_lights_from_ram(g_ram, EE_RAM_SIZE, s.building.env.is_day, s.building.lights);

    // Per SLUS_209.10:0x211150 + 0x223090, 0x00495BE0 is the session struct DIRECTLY (not a pointer).
    // Active event mode bitmask is at 0x00495BE0 + 0x3C = 0x00495C1C.
    if (valid_addr(ee_addr::kSessionEventMask, 4, EE_RAM_SIZE)) {
        s.building.session_event_mask = rd_u32(g_ram, ee_addr::kSessionEventMask);
    }

    s.published = std::make_shared<SceneFrame>(s.building);
}

std::shared_ptr<const SceneFrame> latest_scene_frame() {
    auto &s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    return s.published;
}

std::shared_ptr<const LoadedWorld> current_loaded_world() {
    auto &s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    return s.world;
}

} // namespace remaster
