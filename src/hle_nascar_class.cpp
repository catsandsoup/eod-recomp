// hle_nascar_class.cpp - Native standalone NASCAR Class 4 & unbounded livery/wheel registry.
#include "hle_nascar_class.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace eod::nascar {

namespace {

bool is_standard_lod_wheel_mesh(const char* name, size_t len) {
    return len == 8 &&
           name[3] >= '0' && name[3] <= '4' &&
           std::memcmp(name + 4, ".mod", 4) == 0;
}

bool is_standard_fast_lod_wheel_mesh(const std::string& name) {
    return name.size() == 9 &&
           name[3] >= '0' && name[3] <= '4' &&
           name[4] == 'f' &&
           std::memcmp(name.data() + 5, ".mod", 4) == 0;
}

} // namespace

VehicleClassRegistry& VehicleClassRegistry::instance() {
    static VehicleClassRegistry s_registry;
    return s_registry;
}

VehicleClassRegistry::VehicleClassRegistry() {
    std::lock_guard<std::mutex> lock(mutex_);
    register_defaults_locked();
}

void VehicleClassRegistry::reset_defaults() {
    std::lock_guard<std::mutex> lock(mutex_);
    classes_.clear();
    chassis_classes_.clear();
    liveries_.clear();
    wheel_classes_.clear();
    wheel_prefix_to_key_.clear();
    slow_mesh_to_key_.clear();
    register_defaults_locked();
}

void VehicleClassRegistry::register_defaults_locked() {
    classes_[kClassCompact] = VehicleClassDef{
        kClassCompact, "Compact", kCompactDefaultUpgradeMask, false};
    classes_[kClassMidsize] = VehicleClassDef{
        kClassMidsize, "Midsize", kMidsizeDefaultUpgradeMask, false};
    classes_[kClassMuscle] = VehicleClassDef{
        kClassMuscle, "Muscle", kMuscleDefaultUpgradeMask, false};
    classes_[kClassSpecial] = VehicleClassDef{
        kClassSpecial, "Special", kSpecialDefaultUpgradeMask, false};
    classes_[kClassNascar] = VehicleClassDef{
        kClassNascar, "Stock Car (NASCAR)", kNascarDefaultUpgradeMask, true};

    // Pre-register all 5 NASCAR wheel families (lifts the unchecked 8-slot EE buffer cap at 0x21F988).
    const WheelClassAsset default_wheels[] = {
        {"cup",      "cup",      "ncu0.mod", "ncu0f.mod", 0.711f},
        {"truck",    "truck",    "ntr0.mod", "ntr0f.mod", 0.724f},
        {"modified", "modified", "nmo0.mod", "nmo0f.mod", 0.686f},
        {"dirt",     "dirt",     "ndt0.mod", "ndt0f.mod", 0.737f},
        {"pace",     "pace",     "npc0.mod", "npc0f.mod", 0.698f},
    };
    for (const auto& w : default_wheels) {
        auto [it, inserted] = wheel_classes_.insert_or_assign(w.key, w);
        (void)inserted;
        index_wheel_prefix_locked(it->second);
    }
}

void VehicleClassRegistry::index_wheel_prefix_locked(const WheelClassAsset& asset) {
    if (!asset.slow_mesh.empty()) {
        slow_mesh_to_key_[asset.slow_mesh] = asset.key;
        if (is_standard_lod_wheel_mesh(asset.slow_mesh.c_str(), asset.slow_mesh.size())) {
            wheel_prefix_to_key_[asset.slow_mesh.substr(0, 3)] = asset.key;
        }
    }
}

bool VehicleClassRegistry::register_class(VehicleClassDef def) {
    if (def.name.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    classes_[def.id] = std::move(def);
    return true;
}

const VehicleClassDef* VehicleClassRegistry::find_class(uint32_t class_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = classes_.find(class_id);
    return it != classes_.end() ? &it->second : nullptr;
}

size_t VehicleClassRegistry::class_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return classes_.size();
}

std::vector<VehicleClassDef> VehicleClassRegistry::all_classes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<VehicleClassDef> out;
    out.reserve(classes_.size());
    for (const auto& kv : classes_) {
        out.push_back(kv.second);
    }
    return out;
}

uint32_t VehicleClassRegistry::cycle_class(uint32_t current, int dir) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (classes_.empty()) {
        return current;
    }
    std::vector<uint32_t> ids;
    ids.reserve(classes_.size());
    for (const auto& kv : classes_) {
        ids.push_back(kv.first);
    }
    const auto it = std::find(ids.begin(), ids.end(), current);
    const ptrdiff_t n = static_cast<ptrdiff_t>(ids.size());
    if (it == ids.end()) {
        return dir < 0 ? ids.back() : ids.front();
    }
    const ptrdiff_t idx = it - ids.begin();
    const ptrdiff_t next = ((idx + static_cast<ptrdiff_t>(dir)) % n + n) % n;
    return ids[static_cast<size_t>(next)];
}

void VehicleClassRegistry::register_chassis_class(uint32_t chassis_id, uint32_t class_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    chassis_classes_[chassis_id] = class_id;
}

uint32_t VehicleClassRegistry::lookup_chassis_class(uint32_t chassis_id, uint32_t fallback) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = chassis_classes_.find(chassis_id);
    return it != chassis_classes_.end() ? it->second : fallback;
}

void VehicleClassRegistry::register_livery(uint32_t chassis_id, LiveryPageEntry entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    liveries_[chassis_id].push_back(std::move(entry));
}

void VehicleClassRegistry::clear_liveries(uint32_t chassis_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    liveries_.erase(chassis_id);
}

const std::vector<LiveryPageEntry>& VehicleClassRegistry::liveries_for_chassis(uint32_t chassis_id) const {
    static const std::vector<LiveryPageEntry> kEmpty;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = liveries_.find(chassis_id);
    return it != liveries_.end() ? it->second : kEmpty;
}

std::vector<LiveryPageEntry> VehicleClassRegistry::livery_page(uint32_t chassis_id,
                                                               size_t page_idx,
                                                               size_t page_size) const {
    if (page_size == 0) {
        return {};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = liveries_.find(chassis_id);
    if (it == liveries_.end()) {
        return {};
    }
    const auto& entries = it->second;
    const size_t start = page_idx * page_size;
    if (start >= entries.size()) {
        return {};
    }
    const size_t end = std::min(entries.size(), start + page_size);
    return std::vector<LiveryPageEntry>(entries.begin() + static_cast<ptrdiff_t>(start),
                                        entries.begin() + static_cast<ptrdiff_t>(end));
}

size_t VehicleClassRegistry::livery_page_count(uint32_t chassis_id, size_t page_size) const {
    if (page_size == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = liveries_.find(chassis_id);
    if (it == liveries_.end() || it->second.empty()) {
        return 0;
    }
    return (it->second.size() + page_size - 1) / page_size;
}

void VehicleClassRegistry::register_wheel_class(WheelClassAsset asset) {
    if (asset.key.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto [it, inserted] = wheel_classes_.insert_or_assign(asset.key, std::move(asset));
    (void)inserted;
    index_wheel_prefix_locked(it->second);
}

const WheelClassAsset* VehicleClassRegistry::find_wheel_class(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = wheel_classes_.find(key);
    return it != wheel_classes_.end() ? &it->second : nullptr;
}

size_t VehicleClassRegistry::wheel_class_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return wheel_classes_.size();
}

bool VehicleClassRegistry::resolve_fast_wheel_mesh(const char* slow_mesh_name,
                                                   std::string& out_fast_mesh) const {
    if (!slow_mesh_name || slow_mesh_name[0] == '\0') {
        return false;
    }
    const size_t len = std::strlen(slow_mesh_name);
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. Standard NASCAR/custom LOD wheel pattern: <3-char prefix><0..4>.mod -> <3-char prefix><0..4>f.mod
    if (is_standard_lod_wheel_mesh(slow_mesh_name, len)) {
        const std::string prefix(slow_mesh_name, 3);
        const auto pit = wheel_prefix_to_key_.find(prefix);
        if (pit != wheel_prefix_to_key_.end()) {
            const auto wit = wheel_classes_.find(pit->second);
            if (wit != wheel_classes_.end() && !wit->second.fast_mesh.empty()) {
                if (is_standard_fast_lod_wheel_mesh(wit->second.fast_mesh)) {
                    out_fast_mesh = wit->second.fast_mesh;
                    out_fast_mesh[3] = slow_mesh_name[3];
                    return true;
                }
                out_fast_mesh.assign(slow_mesh_name, 4);
                out_fast_mesh.append("f.mod");
                return true;
            }
        }
    }

    // 2. Exact match for arbitrary custom wheel meshes registered via register_wheel_class().
    const auto sit = slow_mesh_to_key_.find(std::string(slow_mesh_name, len));
    if (sit != slow_mesh_to_key_.end()) {
        const auto wit = wheel_classes_.find(sit->second);
        if (wit != wheel_classes_.end() && !wit->second.fast_mesh.empty()) {
            out_fast_mesh = wit->second.fast_mesh;
            return true;
        }
    }
    return false;
}

bool VehicleClassRegistry::is_registered_wheel_prefix(const char* prefix) const {
    if (!prefix || prefix[0] == '\0' || prefix[1] == '\0' || prefix[2] == '\0' || prefix[3] != '\0') {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return wheel_prefix_to_key_.find(std::string(prefix, 3)) != wheel_prefix_to_key_.end();
}

float VehicleClassRegistry::dirt_tyre_grip_multiplier(uint32_t chassis_id,
                                                      bool has_dirt_tyre_kit,
                                                      bool on_dirt_surface) const {
    if (!has_dirt_tyre_kit || !on_dirt_surface) {
        return 1.0f;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto cit = chassis_classes_.find(chassis_id);
    if (cit == chassis_classes_.end() || cit->second != kClassNascar) {
        return 1.0f;
    }
    const auto cls = classes_.find(cit->second);
    if (cls == classes_.end() || (cls->second.upgrade_mask & kDirtTyreKitBit) == 0) {
        return 1.0f;
    }
    return kNascarDirtTyreGripMultiplier;
}

uint64_t VehicleClassRegistry::resolve_part_compatibility_mask(uint32_t chassis_class) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = classes_.find(chassis_class);
    return it != classes_.end() ? it->second.upgrade_mask : 0ull;
}

} // namespace eod::nascar
