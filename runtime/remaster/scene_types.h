// runtime/remaster/scene_types.h
// Pure semantic 3D scene types for the Halo 2 Anniversary / Saber3D-style Remaster engine.
// ZERO dependency on PS2 GS, VU1, VIF1, or vif_mesh.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace remaster {

// Clean host PBR vertex layout (48 bytes).
// All baked PS2 vertex colors / vertex lighting are stripped at load time.
struct PbrVertex {
    float pos[3];      // 12B: position (world space for static GRAF, model space for dynamic)
    float normal[3];   // 12B: real area-weighted smoothed unit normal
    float tangent[4];  // 16B: unit tangent xyz + handedness w (+1/-1)
    float uv[2];       //  8B: texture coordinates
};
static_assert(sizeof(PbrVertex) == 48, "PbrVertex must be 48 bytes");

enum class MaterialClass : uint32_t {
    Default = 0,
    TerrainDirt = 1,
    Asphalt = 2,
    CarPaint = 3,
    MetalChrome = 4,
    Glass = 5,
    Foliage = 6,
    BuildingConcrete = 7,
    Sky = 8,
    Water = 9,
    Emissive = 10,
    Rubber = 11,
    Interior = 12,
};

struct PbrMaterial {
    float base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // linear RGBA
    float roughness = 0.85f;
    float metallic = 0.0f;
    float ao = 1.0f;
    float emissive_scale = 0.0f;
    bool alpha_cutout = false;
    bool alpha_blend = false;
    bool alpha_additive = false;
    bool double_sided = false;
    bool is_sky = false;
    MaterialClass mat_class = MaterialClass::Default;
};

// Populated every frame directly from decompiled EE globals (SLUS_209.10)
// and optionally overridden by World Editor editor_environment::Override.
// Shadows are NEVER disabled when editor_override_active is true.
struct SceneEnvironment {
    float sun_dir[3] = {0.35f, 0.82f, -0.45f};   // unit vector TOWARD the sun (EOD world: +Y is up)
    float sun_color[3] = {1.0f, 0.98f, 0.90f};   // linear RGB
    float sun_intensity = 3.2f;                  // physical radiance multiplier
    float ambient_sky[3] = {0.35f, 0.48f, 0.68f}; // linear hemisphere zenith/sky color
    float ambient_ground[3] = {0.18f, 0.15f, 0.12f}; // linear hemisphere ground bounce
    float ambient_intensity = 0.65f;
    float shadow_strength = 1.0f;                // 0..1 direct-sun attenuation factor
    float fog_color[3] = {0.62f, 0.66f, 0.78f};  // linear RGB
    float fog_start = 160.0f;
    float fog_end = 900.0f;
    float fog_density = 0.35f;
    bool is_day = true;
    bool editor_override_active = false;
};

struct SceneCamera {
    float pos[3] = {0.0f, 5.0f, 0.0f};
    float rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // rows: right (+X), up (+Y), forward (+Z)
    float near_z = 0.5f;
    float far_z = 1500.0f;
    float fov_scale = 1.0f;
    float ee_aspect = 1.0f;
    float vp_aspect = 640.0f / 448.0f;
    float aspect = 16.0f / 9.0f;
    float view[16] = {};
    float proj[16] = {};
    float view_proj[16] = {};
    bool valid = false;
};

struct DynamicLight {
    float pos[3] = {0.0f, 0.0f, 0.0f};
    float radius = 25.0f;
    float color[3] = {1.0f, 0.9f, 0.75f}; // linear RGB
    float intensity = 1.0f;
    float dir[3] = {0.0f, -1.0f, 0.0f};   // world-space spotlight direction (-Y is down in EOD)
    float spot_cos = 0.0f;                // <= 0: omnidirectional point light; > 0: spotlight cos cutoff
};

// Per-vertex skinning / live car dent weights unpacked from 4SPS @150.
struct VertexSkin {
    uint16_t bone[2] = {0, 0};
    float weight[2] = {0.0f, 0.0f};
    uint8_t mode = 0; // 0 = rigid, 1 = car dent offset (MSCAL 0x5C), 2 = driver/ped bone matrix (MSCAL 0x63/0x496)
};

struct CpuMeshData {
    std::string asset_id;
    std::string texture_id;
    std::vector<PbrVertex> vertices;
    std::vector<VertexSkin> skin; // empty for rigid meshes; same size as vertices when skinned/dentable
    std::vector<uint32_t> indices;
    PbrMaterial material;
    uint32_t part_index = 0;
    int16_t layer_index = -1;
    float bounds_min[3] = {0, 0, 0};
    float bounds_max[3] = {0, 0, 0};
    float center[3] = {0, 0, 0};
    float radius = 0.0f;
};

struct StaticMeshInstance {
    std::string asset_id;
    std::string texture_id;
    uint32_t mesh_handle = 0;
    float center[3] = {0, 0, 0};
    float radius = 0.0f;
    uint32_t optional_terrain_mask = 0; // tested against *(u32*)(0x00495BE0 + 0x3C) when is_optional_terrain
    bool is_optional_terrain = false;
    bool is_sky = false;
    bool is_foliage = false;
    bool cast_shadow = true;
};

struct EntityInstance {
    std::string asset_id;
    uint32_t model_handle = 0;
    uint32_t batch_id = 0;
    std::vector<uint32_t> part_handles; // decoded AssetRegistry mesh handles for all mrModel parts
    float world_matrix[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}; // 3x4 row-major
    std::vector<float> dent_offsets;   // live car dent displacement vectors (3 floats/bone)
    std::vector<float> bone_palette;   // live skeletal 4x4 bone matrices (16 floats/bone)
    std::string material_override;
    bool is_player_vehicle = false;
    bool is_vehicle = false;
    bool cast_shadow = true;
};

struct HudQuad {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // normalized [0..1] screen coordinates
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    uint32_t rgba = 0xFFFFFFFFu;
    uint64_t texture_key = 0;
};

enum class DebugViewMode : uint32_t {
    Final = 0,
    RawAlbedo = 1,
    RoughnessMetallic = 2,
    WorldNormals = 3,
    DirectSunAndCsm = 4,
    ReflectionsOnly = 5,
    Count = 6,
};

// Backend-agnostic render output handle passed into remaster::render_frame().
struct OutputTarget {
    void *vk_command_buffer = nullptr;
    uint64_t vk_color_image = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Complete per-frame pure EE scene snapshot consumed by the Remaster renderer.
struct SceneFrame {
    uint64_t frame_index = 0;
    uint32_t track_generation = 0;
    uint32_t session_event_mask = 0xFFFFFFFFu;
    std::string track_name;
    SceneEnvironment env;
    SceneCamera camera;
    std::vector<EntityInstance> entities;
    std::vector<DynamicLight> lights;
    std::vector<HudQuad> hud_quads;
    float player_vehicle_pos[3] = {0, 0, 0};
    bool has_player_vehicle = false;
};

} // namespace remaster
