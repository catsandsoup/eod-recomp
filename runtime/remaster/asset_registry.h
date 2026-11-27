// runtime/remaster/asset_registry.h
// Halo 2 Anniversary / Saber3D-style Asset Injection Registry.
// Keys every mesh and texture by clean semantic asset_id:
//   - track/<TRACK>/<mesh>
//   - car/<CAR>/<part>
//   - tex/<name>
// Supports loading optional external HD meshes (.obj / .glb) and PBR texture sets
// (_albedo, _normal, _roughness, _metallic, _ao) from a remaster/mod directory,
// falling back cleanly to the un-baked retail mesh + linear PBR defaults.
#pragma once

#include "scene_types.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace remaster {

struct CpuImageRgba8 {
    uint32_t width = 0;
    uint32_t height = 0;
    bool is_srgb = true;
    std::vector<uint8_t> pixels; // RGBA8 tightly packed
};

struct PbrTextureSet {
    std::string texture_id;
    bool has_hd_override = false;
    bool has_alpha_cutout = false;
    bool has_alpha_blend = false;
    bool has_alpha_additive = false;
    CpuImageRgba8 albedo;     // sRGB -> converted to linear on GPU or upload
    CpuImageRgba8 normal;     // linear tangent-space normal map (optional)
    CpuImageRgba8 roughness;  // linear R channel (optional)
    CpuImageRgba8 metallic;   // linear R channel (optional)
    CpuImageRgba8 ao;         // linear R channel (optional)
    PbrMaterial material;
};

class AssetRegistry {
public:
    static AssetRegistry &instance();

    // Configure root directory for HD asset injection (e.g., EOD_REMASTER_ASSETS or EOD_MOD_DIR/remaster).
    void set_remaster_root(const std::string &root_dir);
    std::string remaster_root() const;

    // Register a retail fallback mesh by semantic asset_id.
    // If an external HD mesh (.glb or .obj) exists under `<root>/<asset_id>.glb|.obj`,
    // it is loaded transparently in place of the retail mesh.
    uint32_t register_mesh(const std::string &asset_id, CpuMeshData retail_fallback);

    // Lookup registered mesh by handle or semantic asset_id.
    std::shared_ptr<const CpuMeshData> get_mesh(uint32_t handle) const;
    std::shared_ptr<const CpuMeshData> find_mesh(const std::string &asset_id) const;

    // Resolve a PBR texture set by semantic `tex/<name>` (or bare `<name>`).
    // Looks for `<root>/tex/<name>_albedo.tga/.ppm`, `_normal`, `_roughness`, `_metallic`, `_ao`.
    // Falls back to deterministic linear PBR default texture when no HD file is present.
    std::shared_ptr<const PbrTextureSet> resolve_texture(const std::string &texture_name,
                                                         bool is_sky = false,
                                                         bool is_vehicle = false);

    // Load an external .obj mesh into CpuMeshData (with smoothed normals & tangents).
    static bool load_obj_file(const std::string &path, const std::string &asset_id, CpuMeshData &out);

    // Load an external binary glTF 2.0 (.glb) mesh into CpuMeshData.
    static bool load_glb_file(const std::string &path, const std::string &asset_id, CpuMeshData &out);

    void clear();

private:
    AssetRegistry();
    mutable std::mutex mtx_;
    std::string root_dir_;
    uint32_t next_handle_ = 1;
    std::unordered_map<std::string, uint32_t> id_to_handle_;
    std::unordered_map<uint32_t, std::shared_ptr<CpuMeshData>> meshes_;
    std::unordered_map<std::string, std::shared_ptr<PbrTextureSet>> textures_;
};

} // namespace remaster
