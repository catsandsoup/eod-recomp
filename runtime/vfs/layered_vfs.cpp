// layered_vfs.cpp - Thread-safe live hot-mountable 4-layer VFS implementation.
#include "layered_vfs.h"

#include <cctype>
#include <filesystem>
#include <mutex>
#include <vector>

namespace eod::vfs {

namespace {

namespace fs = std::filesystem;

constexpr size_t layer_index(Layer layer) { return static_cast<size_t>(layer); }

bool equals_ascii_nocase(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Validate and split a relative path into safe components.
// Rejects empty paths, embedded NUL, leading slashes, Windows drive letters, and ".." segments.
bool split_safe_rel_path(const std::string &rel_path, std::vector<std::string> &out_parts) {
    out_parts.clear();
    if (rel_path.empty()) return false;
    if (rel_path.find('\0') != std::string::npos) return false;
    if (rel_path[0] == '/' || rel_path[0] == '\\') return false;
    if (rel_path.size() >= 2 &&
        std::isalpha(static_cast<unsigned char>(rel_path[0])) && rel_path[1] == ':') {
        return false;
    }

    size_t pos = 0;
    const size_t n = rel_path.size();
    while (pos < n) {
        while (pos < n && (rel_path[pos] == '/' || rel_path[pos] == '\\')) ++pos;
        if (pos >= n) break;
        size_t end = pos;
        while (end < n && rel_path[end] != '/' && rel_path[end] != '\\') ++end;
        std::string part = rel_path.substr(pos, end - pos);
        if (part == "..") return false;
        if (part.find(':') != std::string::npos) return false;
        if (part != ".") out_parts.push_back(std::move(part));
        pos = end;
    }
    return !out_parts.empty();
}

bool is_within_canonical_root(const fs::path &candidate, const fs::path &canonical_root) {
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(candidate, ec);
    if (ec) return false;

    auto root_it = canonical_root.begin();
    auto cand_it = resolved.begin();
    for (; root_it != canonical_root.end(); ++root_it, ++cand_it) {
        if (cand_it == resolved.end()) return false;
        if (*root_it != *cand_it) return false;
    }
    return true;
}

bool find_case_insensitive_file(const fs::path &root, const fs::path &canonical_root,
                                const std::vector<std::string> &parts,
                                std::string &out_host_path) {
    fs::path current = root;
    std::error_code ec;

    for (size_t i = 0; i < parts.size(); ++i) {
        if (!fs::is_directory(current, ec) || ec) return false;

        bool matched = false;
        fs::path next_path;
        for (const auto &entry : fs::directory_iterator(current, ec)) {
            if (ec) return false;
            const std::string fname = entry.path().filename().string();
            if (equals_ascii_nocase(fname, parts[i])) {
                next_path = entry.path();
                matched = true;
                break;
            }
        }
        if (!matched) return false;
        current = std::move(next_path);
    }

    if (!fs::exists(current, ec) || ec) return false;
    if (fs::is_directory(current, ec) || ec) return false;
    if (!is_within_canonical_root(current, canonical_root)) return false;

    out_host_path = current.string();
    return true;
}

}  // namespace

bool LayeredVfs::mount(Layer layer, const std::string &root_dir, const std::string &tag) {
    const size_t idx = layer_index(layer);
    if (idx >= kLayerCount || root_dir.empty()) return false;

    std::error_code ec;
    fs::path root_path(root_dir);
    if (!fs::is_directory(root_path, ec) || ec) return false;

    fs::path canonical = fs::weakly_canonical(root_path, ec);
    if (ec) return false;

    std::string clean_root = root_path.lexically_normal().string();
    while (clean_root.size() > 1 && (clean_root.back() == '/' || clean_root.back() == '\\')) {
        clean_root.pop_back();
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    layers_[idx].mounted = true;
    layers_[idx].root = std::move(clean_root);
    layers_[idx].canonical_root = canonical.string();
    layers_[idx].tag = tag;
    return true;
}

void LayeredVfs::unmount(Layer layer) {
    const size_t idx = layer_index(layer);
    if (idx >= kLayerCount) return;

    std::unique_lock<std::shared_mutex> lock(mutex_);
    layers_[idx] = MountSlot{};
}

void LayeredVfs::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    for (auto &slot : layers_) slot = MountSlot{};
}

bool LayeredVfs::is_mounted(Layer layer) const {
    const size_t idx = layer_index(layer);
    if (idx >= kLayerCount) return false;

    std::shared_lock<std::shared_mutex> lock(mutex_);
    return layers_[idx].mounted;
}

std::string LayeredVfs::layer_root(Layer layer) const {
    const size_t idx = layer_index(layer);
    if (idx >= kLayerCount) return {};

    std::shared_lock<std::shared_mutex> lock(mutex_);
    return layers_[idx].mounted ? layers_[idx].root : std::string{};
}

std::string LayeredVfs::layer_tag(Layer layer) const {
    const size_t idx = layer_index(layer);
    if (idx >= kLayerCount) return {};

    std::shared_lock<std::shared_mutex> lock(mutex_);
    return layers_[idx].mounted ? layers_[idx].tag : std::string{};
}

bool LayeredVfs::resolve(const std::string &rel_path, std::string &out_host_path) const {
    return resolve_range(rel_path, static_cast<int>(Layer::EphemeralPie),
                         static_cast<int>(Layer::RetailBase), out_host_path);
}

bool LayeredVfs::resolve_overlay_only(const std::string &rel_path,
                                      std::string &out_host_path) const {
    return resolve_range(rel_path, static_cast<int>(Layer::EphemeralPie),
                         static_cast<int>(Layer::ActiveProfile), out_host_path);
}

bool LayeredVfs::resolve_range(const std::string &rel_path, int top_layer, int bottom_layer,
                               std::string &out_host_path) const {
    std::vector<std::string> parts;
    if (!split_safe_rel_path(rel_path, parts)) return false;

    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (int i = top_layer; i >= bottom_layer; --i) {
        const MountSlot &slot = layers_[static_cast<size_t>(i)];
        if (!slot.mounted) continue;
        if (find_case_insensitive_file(fs::path(slot.root), fs::path(slot.canonical_root),
                                       parts, out_host_path)) {
            return true;
        }
    }
    return false;
}

LayeredVfs &global_vfs() {
    static LayeredVfs instance;
    return instance;
}

}  // namespace eod::vfs
