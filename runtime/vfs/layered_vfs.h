// layered_vfs.h - Live hot-mountable 4-layer virtual filesystem for Eve of Destruction.
#pragma once

#include <array>
#include <cstdint>
#include <shared_mutex>
#include <string>

namespace eod::vfs {

enum class Layer : uint8_t {
    RetailBase = 0,
    ActiveProfile = 1,
    ServerSession = 2,
    EphemeralPie = 3,
};

constexpr size_t kLayerCount = 4;

class LayeredVfs {
  public:
    LayeredVfs() = default;
    ~LayeredVfs() = default;

    LayeredVfs(const LayeredVfs &) = delete;
    LayeredVfs &operator=(const LayeredVfs &) = delete;

    // Live hot-mount (or atomically replace) a host directory at the specified layer.
    // Returns false if root_dir is empty or does not refer to an existing directory.
    bool mount(Layer layer, const std::string &root_dir, const std::string &tag = "");

    // Cleanly unmount a layer (e.g. ServerSession on disconnect, EphemeralPie on PIE stop).
    void unmount(Layer layer);

    // Unmount all layers.
    void clear();

    bool is_mounted(Layer layer) const;
    std::string layer_root(Layer layer) const;
    std::string layer_tag(Layer layer) const;

    // Case-insensitive resolution from EphemeralPie (3) down to RetailBase (0).
    // Rejects path traversal (".."), absolute paths, drive prefixes, and root escapes.
    bool resolve(const std::string &rel_path, std::string &out_host_path) const;

    // Case-insensitive resolution across overlay layers only: EphemeralPie (3) -> ActiveProfile (1).
    bool resolve_overlay_only(const std::string &rel_path, std::string &out_host_path) const;

  private:
    struct MountSlot {
        bool mounted = false;
        std::string root;
        std::string canonical_root;
        std::string tag;
    };

    bool resolve_range(const std::string &rel_path, int top_layer, int bottom_layer,
                       std::string &out_host_path) const;

    mutable std::shared_mutex mutex_;
    std::array<MountSlot, kLayerCount> layers_{};
};

// Process-wide hot-mountable VFS singleton used by IOP FILEIO and editor/netplay hooks.
LayeredVfs &global_vfs();

}  // namespace eod::vfs
