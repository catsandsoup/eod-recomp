#pragma once
#include "layered_vfs.h"
#include "../../src/custom_map_collision.h"
#include <array>
#include <filesystem>
#include <vector>

namespace eod::vfs {
using MapPoint = std::array<float, 3>;
struct MapSpawn {
    MapPoint position, normal;
    float heading;
};
struct CustomMap {
    std::string title, author, base_arena, sha256;
    bool replace_retail_spawns = true, validated = false;
    std::vector<MapSpawn> grid_starts, battle_respawns, checkpoints, finish_gates, ai_waypoints;
    std::vector<MapPoint> vertices, colors;
    std::vector<uint32_t> indices;
    physics::CustomMapCollision collision;
};
std::shared_ptr<const CustomMap> inspect_map(const std::filesystem::path &archive, std::string &error);
std::string map_archive_sha256(const std::filesystem::path &archive, std::string &error);
bool mount_map(const std::filesystem::path &archive, Layer layer, std::string &error,
               const std::string &expected_hash = {});
inline std::shared_ptr<const CustomMap> active_map() { return global_vfs().current_map(); }
}
