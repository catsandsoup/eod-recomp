// hle_nascar_class.h - Native standalone NASCAR Class 4 & unbounded livery/wheel registry.
// Replaces hardcoded PS2 4-class (0..3), 10-paint-per-chassis, and 8-wheel-class limits.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace eod::nascar {

constexpr uint32_t kClassCompact = 0;
constexpr uint32_t kClassMidsize = 1;
constexpr uint32_t kClassMuscle  = 2;
constexpr uint32_t kClassSpecial = 3;
constexpr uint32_t kClassNascar  = 4;

// Bit 24 in the 64-bit upgrade compatibility mask unlocks the Dirt Tyre Kit
// for NASCAR Class 4 stock cars on dirt circuits.
constexpr uint64_t kDirtTyreKitBit = (1ull << 24);

// Default 64-bit upgrade masks per vehicle class (distinct per class; Class 4 never wraps % 4).
constexpr uint64_t kCompactDefaultUpgradeMask = 0x00000000000000FFull;
constexpr uint64_t kMidsizeDefaultUpgradeMask = 0x0000000000000FFFull;
constexpr uint64_t kMuscleDefaultUpgradeMask  = 0x000000000000FFFFull;
constexpr uint64_t kSpecialDefaultUpgradeMask = 0x00000000000FFFFFull;
constexpr uint64_t kNascarDefaultUpgradeMask  = 0x0000000000FFFFFFull | kDirtTyreKitBit;

// Dirt surface grip multiplier for Class 4 NASCAR stock cars equipped with the Dirt Tyre Kit.
constexpr float kNascarDirtTyreGripMultiplier = 1.28f;

struct VehicleClassDef {
    uint32_t id = 0;
    std::string name;
    uint64_t upgrade_mask = 0;
    bool isolated_ai_pool = false;
};

struct LiveryPageEntry {
    uint32_t vehicle_id = 0;
    std::string driver;
    std::string car_number;
    std::string tex_name;
};

struct WheelClassAsset {
    std::string key;
    std::string family;
    std::string slow_mesh;
    std::string fast_mesh;
    float diameter_m = 0.0f;
};

class VehicleClassRegistry {
public:
    static VehicleClassRegistry& instance();

    // Resets the registry back to the default 5 vehicle classes (0..4),
    // the 5 pre-registered NASCAR wheel families, and empty chassis/livery maps.
    void reset_defaults();

    // Class registration & lookup (supports 0..4 defaults plus arbitrary custom mod classes).
    bool register_class(VehicleClassDef def);
    const VehicleClassDef* find_class(uint32_t class_id) const;
    size_t class_count() const;
    std::vector<VehicleClassDef> all_classes() const;

    // Cycles cleanly across all registered classes in ascending ID order (including Class 4).
    uint32_t cycle_class(uint32_t current, int dir) const;

    // Chassis -> Class mapping.
    void register_chassis_class(uint32_t chassis_id, uint32_t class_id);
    uint32_t lookup_chassis_class(uint32_t chassis_id, uint32_t fallback = kClassCompact) const;

    // Unbounded per-chassis livery registry and pagination (lifts the PS2 10-paint cap).
    void register_livery(uint32_t chassis_id, LiveryPageEntry entry);
    void clear_liveries(uint32_t chassis_id);
    const std::vector<LiveryPageEntry>& liveries_for_chassis(uint32_t chassis_id) const;
    std::vector<LiveryPageEntry> livery_page(uint32_t chassis_id,
                                             size_t page_idx,
                                             size_t page_size = 10) const;
    size_t livery_page_count(uint32_t chassis_id, size_t page_size = 10) const;

    // Unbounded wheel-class registry (lifts the unchecked 8-slot EE buffer cap at 0x21F988).
    // Pre-registers all 5 NASCAR families: cup, truck, modified, dirt, pace.
    void register_wheel_class(WheelClassAsset asset);
    const WheelClassAsset* find_wheel_class(const std::string& key) const;
    size_t wheel_class_count() const;

    // Wheel-mesh helpers for the EE loader/draw hooks in hle_nascar_wheels.cpp.
    // Matches registered 3-character prefixes (<pfx><0..4>.mod -> <pfx><0..4>f.mod)
    // as well as exact custom slow_mesh -> fast_mesh entries.
    bool resolve_fast_wheel_mesh(const char* slow_mesh_name, std::string& out_fast_mesh) const;
    bool is_registered_wheel_prefix(const char* prefix) const;

    // Returns 1.28f for Class 4 NASCAR stock cars equipped with the Dirt Tyre Kit on dirt surfaces,
    // and 1.0f otherwise.
    float dirt_tyre_grip_multiplier(uint32_t chassis_id,
                                    bool has_dirt_tyre_kit,
                                    bool on_dirt_surface) const;

    // Resolves the full 64-bit upgrade compatibility mask for any registered class (>= 4 never wraps % 4).
    uint64_t resolve_part_compatibility_mask(uint32_t chassis_class) const;

private:
    VehicleClassRegistry();
    void register_defaults_locked();
    void index_wheel_prefix_locked(const WheelClassAsset& asset);

    mutable std::mutex mutex_;
    std::map<uint32_t, VehicleClassDef> classes_;
    std::unordered_map<uint32_t, uint32_t> chassis_classes_;
    std::unordered_map<uint32_t, std::vector<LiveryPageEntry>> liveries_;
    std::unordered_map<std::string, WheelClassAsset> wheel_classes_;
    std::unordered_map<std::string, std::string> wheel_prefix_to_key_;
    std::unordered_map<std::string, std::string> slow_mesh_to_key_;
};

} // namespace eod::nascar
