// runtime/remaster/graf_loader.h
// Loads static track geometry directly from resident GRAF (0x003D1C18) and embedded mrModel
// binary data in EE RAM into clean host PBR meshes (smoothed normals + tangents, zero baked
// PS2 vertex lighting). Completely independent of runtime/gs/*, runtime/vu/*, and vif_mesh.h.
#pragma once

#include "scene_types.h"
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace remaster {

struct LoadedWorld {
    std::string track_name;
    uint32_t graf_guest_addr = 0;
    std::vector<CpuMeshData> meshes;
    std::vector<StaticMeshInstance> instances;
    std::unordered_set<uint32_t> sky_model_addrs; // mrModel guest addresses for GRAF subtype Sky (4)
};

// Classify physically grounded PBR defaults from a retail texture name.
// Demolition derby cars: dirty/scratched paint roughness 0.60-0.85 (NO mirror clearcoat over dirt).
// Terrain/dirt: strictly matte 0.85-0.95. Glass/chrome: 0.05-0.20.
PbrMaterial classify_pbr_material(const std::string &texture_name, bool is_sky = false, bool is_vehicle = false);

// Compute area-weighted smooth vertex normals and tangents on an indexed triangle mesh.
// Welds coincident vertex positions within `weld_eps` so hard PS2 strip seams become smooth.
void compute_smooth_normals_and_tangents(std::vector<PbrVertex> &verts,
                                         const std::vector<uint32_t> &indices,
                                         float weld_eps = 1e-3f);

// Parse a resident GRAF display tree from EE RAM at `graf_addr` into clean PBR meshes.
// `ram` points to the 32MB EE RAM buffer (`ram_size` bytes).
bool load_world_from_graf(const uint8_t *ram, uint32_t ram_size, uint32_t graf_addr,
                          const std::string &track_name, LoadedWorld &out);

// Decode a standalone embedded/dynamic mrModel (4SPS parts) from EE RAM into CpuMeshData parts.
bool decode_mrmodel_from_ram(const uint8_t *ram, uint32_t ram_size, uint32_t model_addr,
                             const std::string &asset_prefix, bool is_vehicle,
                             std::vector<CpuMeshData> &out_parts);

} // namespace remaster
