#pragma once
// Persistent host-side CatalogCache for custom maps (mods/maps/*.eodmap) and
// custom gametypes (mods/gametypes/*.json). Header-only so both the runtime
// and standalone unit tests can scan and save custom content without EE RAM.
#include "mutator_schema.h"
#include "vfs/custom_map.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace catalog_cache {

struct CustomMapEntry {
    std::string filename;    // e.g. "sky_bowl.eodmap"
    std::string title;
    std::string base_arena;  // e.g. "Anoka" or "EmptyTrack"
    std::string author;
};

struct CustomGametypeEntry {
    std::string filename;    // e.g. "my_tag_mode.json"
    std::string path;
    mutator::GametypeConfig config;
};

class Cache {
public:
    void refresh(const std::string &mods_root) {
        mods_root_ = mods_root;
        maps_.clear();
        gametypes_.clear();
        if (mods_root.empty()) return;

        namespace fs = std::filesystem;
        std::error_code ec;

        const fs::path maps_dir = fs::u8path(mods_root) / "maps";
        for (fs::directory_iterator it(maps_dir, ec), end; !ec && it != end; it.increment(ec)) {
            const auto &entry = *it;
            if (entry.path().extension() != ".eodmap" || !fs::is_regular_file(entry.symlink_status(ec))) continue;
            std::string err;
            if (auto m = eod::vfs::inspect_map(entry.path(), err)) {
                CustomMapEntry item;
                item.filename = entry.path().filename().u8string();
                item.title = m->title.empty() ? item.filename : m->title;
                item.base_arena = m->base_arena;
                item.author = m->author;
                maps_.push_back(std::move(item));
            }
        }
        std::sort(maps_.begin(), maps_.end(), [](const CustomMapEntry &a, const CustomMapEntry &b) {
            return a.title < b.title;
        });

        ec.clear();
        const fs::path gt_dir = fs::u8path(mods_root) / "gametypes";
        for (fs::directory_iterator it(gt_dir, ec), end; !ec && it != end; it.increment(ec)) {
            const auto &entry = *it;
            if (entry.path().extension() != ".json" || !fs::is_regular_file(entry.symlink_status(ec))) continue;
            std::ifstream in(entry.path());
            if (!in) continue;
            std::ostringstream ss;
            ss << in.rdbuf();
            mutator::GametypeConfig cfg;
            std::string err;
            if (mutator::parse_gametype_json(ss.str(), cfg, err)) {
                CustomGametypeEntry item;
                item.filename = entry.path().filename().u8string();
                item.path = entry.path().u8string();
                item.config = std::move(cfg);
                gametypes_.push_back(std::move(item));
            }
        }
        std::sort(gametypes_.begin(), gametypes_.end(), [](const CustomGametypeEntry &a, const CustomGametypeEntry &b) {
            return a.config.name < b.config.name;
        });
    }

    bool save_gametype(const std::string &mods_root, const mutator::GametypeConfig &cfg,
                       std::string &out_path, std::string &err) {
        if (mods_root.empty()) {
            err = "Mods directory is not configured";
            return false;
        }
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path gt_dir = fs::u8path(mods_root) / "gametypes";
        fs::create_directories(gt_dir, ec);
        if (ec) {
            err = "Could not create mods/gametypes directory: " + ec.message();
            return false;
        }
        mutator::GametypeConfig copy = cfg;
        copy.sanitize();
        if (copy.id.empty()) copy.id = mutator::slugify_gametype_id(copy.name);
        if (copy.name.empty()) copy.name = copy.id;
        const std::string slug = mutator::slugify_gametype_id(copy.id);
        fs::path target = gt_dir / (slug + ".json");
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "Could not open " + target.u8string() + " for writing";
            return false;
        }
        const std::string text = mutator::serialize_gametype_json(copy);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            err = "Failed writing " + target.u8string();
            return false;
        }
        out.close();
        out_path = target.u8string();
        refresh(mods_root);
        return true;
    }

    const std::vector<CustomMapEntry> &maps() const { return maps_; }
    const std::vector<CustomGametypeEntry> &gametypes() const { return gametypes_; }
    const std::string &mods_root() const { return mods_root_; }

    const CustomGametypeEntry *find_gametype(const std::string &id) const {
        for (const auto &g : gametypes_) {
            if (g.config.id == id || g.filename == id) return &g;
        }
        return nullptr;
    }

private:
    std::string mods_root_;
    std::vector<CustomMapEntry> maps_;
    std::vector<CustomGametypeEntry> gametypes_;
};

inline Cache &global() {
    static Cache instance;
    return instance;
}

} // namespace catalog_cache
