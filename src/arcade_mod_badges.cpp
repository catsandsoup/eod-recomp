// SLUS_209.10 display hooks and their inputs are documented in UI_MENU_ENGINE.md.
#include "core/cpu.h"
#include "core/memory.h"
#include "gs/texture_replacements.h"
#include "iop/iop.h"
#include "platform/platform.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string_view>
#include <vector>

namespace {
constexpr char kBadge[] = " [MOD]";
using BadgeBytes = std::array<char, sizeof kBadge>;

std::string lower(std::string name) {
    for (char &c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name;
}

std::string find_file(const std::string &root, const std::string &relative) {
    char path[1024];
    if (root.empty() || !platform_find_file(root.c_str(), relative.c_str(), path, sizeof path)) return {};
    return path;
}

uint32_t read_le32(const char *p) {
    const auto *b = reinterpret_cast<const uint8_t *>(p);
    return b[0] | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
}

// Version 6 0TSR directory, shared by .TRK, .CAR and .RES. Compare member
// payloads so identical archives copied or repacked into an overlay stay retail.
struct Archive {
    struct Member {
        uint64_t offset;
        uint32_t size, type, version;
    };
    std::filesystem::path path;
    std::map<std::string, Member> members;
    bool valid = false;

    explicit Archive(const std::string &filename) : path(std::filesystem::u8path(filename)) {
        if (filename.empty()) return;
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        const auto file_size = file.tellg();
        constexpr uint32_t entry_size = 0x44;
        char header[0x28];
        file.seekg(0);
        if (!file.read(header, sizeof header) || std::memcmp(header, "0TSR", 4) || read_le32(header + 8) != 6) return;
        const uint32_t directory = read_le32(header + 20), count = read_le32(header + 24);
        const uint64_t directory_end = uint64_t(directory) + uint64_t(count) * entry_size;
        if (directory < sizeof header || directory_end > uint64_t(file_size)) return;
        file.seekg(directory);
        char entry[entry_size];
        for (uint32_t i = 0; i < count; ++i) {
            if (!file.read(entry, sizeof entry)) return;
            std::string name(entry, std::find(entry, entry + 32, '\0'));
            Member member{uint64_t(directory) + read_le32(entry + 44), read_le32(entry + 40),
                          read_le32(entry + 32), read_le32(entry + 36)};
            if (name.empty() || member.offset + member.size > uint64_t(file_size) ||
                !members.emplace(lower(std::move(name)), member).second) return;
        }
        valid = true;
    }
};

bool member_changed(const Archive &active, const Archive &retail, const std::string &name,
                    std::ifstream &active_file, std::ifstream &retail_file) {
    const auto a = active.members.find(name), r = retail.members.find(name);
    if (a == active.members.end() || r == retail.members.end())
        return a != active.members.end() || r != retail.members.end();
    const auto &after = a->second, &before = r->second;
    if (after.size != before.size || after.type != before.type || after.version != before.version) return true;
    active_file.seekg(static_cast<std::streamoff>(after.offset));
    retail_file.seekg(static_cast<std::streamoff>(before.offset));
    std::array<char, 16384> left, right;
    for (size_t remaining = after.size; remaining;) {
        const size_t bytes = std::min(remaining, left.size());
        if (!active_file.read(left.data(), bytes) || !retail_file.read(right.data(), bytes)) return false;
        if (std::memcmp(left.data(), right.data(), bytes)) return true;
        remaining -= bytes;
    }
    return false;
}

// The native ISO filename convention is also used by the Creation Kit.
std::string archive_name(const std::string &key) {
    if (key.size() <= 8) return key;
    return key.substr(0, 6) + "_1";
}

std::string asset_key(std::string key) {
    if (key.empty() || key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
        return {};
    return lower(std::move(key));
}

class Profile {
public:
    bool track(const std::string &key) {
        const std::string stem = asset_key(key);
        return !stem.empty() && (changed("DATA/TRACKS/" + archive_name(stem) + ".TRK") ||
                                 changed("DATA/RES/COMMON.RES", stem + "_eves.val"));
    }

    bool vehicle(const std::string &key, uint32_t number) {
        const std::string stem = asset_key(key);
        if (stem.empty()) return false;
        const std::string paint = std::to_string(number);
        return changed("DATA/CAR/" + archive_name(stem) + ".CAR") ||
               changed("DATA/RES/CT.RES", paint + ".tex") || changed("DATA/RES/CT.RES", paint + ".tm0") ||
               changed("DATA/RES/UICAR.RES", "uicar_" + paint + ".tex") ||
               changed("DATA/RES/UICAR.RES", "uicar_" + paint + ".tm0");
    }

private:
    bool changed(const std::string &archive, const std::string &member = {}) {
        const char *root = std::getenv("EOD_MOD_DIR");
        const std::string selected = root ? root : "";
        if (selected != active_root_ || g_game_root != retail_root_) {
            active_root_ = selected;
            retail_root_ = g_game_root;
            archives_.clear();
            results_.clear();
            host_archives_.clear();
            host_assets_.clear();
            if (!selected.empty()) load_host_assets();
        }
        if (active_root_.empty()) return false;
        const std::string path = lower(archive);
        const std::string asset = path + "#" + member;
        const char *host_textures = std::getenv("EOD_HOST_TEXTURES");
        if ((!host_textures || std::strcmp(host_textures, "0") != 0) &&
            (member.empty() ? host_archives_.count(path) : host_assets_.count(asset))) return true;
        const auto [result, inserted] = results_.try_emplace(asset, false);
        if (!inserted) return result->second;
        auto files = archives_.find(path);
        if (files == archives_.end()) {
            const std::string active_file = find_file(active_root_, archive);
            const std::string retail_file = active_file.empty() ? "" : find_file(retail_root_, archive);
            files = archives_.try_emplace(path, active_file, retail_file).first;
        }
        const Archive &active = files->second.first;
        const Archive &retail = files->second.second;
        if (!active.valid) return false;
        if (!retail.valid) {
            result->second = retail.path.empty() && (member.empty() || active.members.count(member));
        } else {
            std::ifstream active_file(active.path, std::ios::binary), retail_file(retail.path, std::ios::binary);
            if (!active_file || !retail_file) return false;
            auto differs = [&](const std::string &name) { return member_changed(active, retail, name, active_file, retail_file); };
            result->second = member.empty()
                ? active.members.size() != retail.members.size() ||
                      std::any_of(active.members.begin(), active.members.end(), [&](const auto &entry) { return differs(entry.first); })
                : differs(member);
        }
        return result->second;
    }

    void load_host_assets() {
        const auto assets = gs::hosttex::Store::active_asset_ids(active_root_,
            [](const std::string &root, const std::string &rel, std::string &out) {
                out = find_file(root, rel);
                return !out.empty();
            });
        for (const auto &asset : assets) {
            const std::string id = lower(asset);
            const auto split = id.find('#');
            if (split == std::string::npos || split + 1 == id.size()) continue;
            host_archives_.insert(id.substr(0, split));
            host_assets_.insert(id);
        }
    }

    std::string active_root_, retail_root_;
    std::map<std::string, std::pair<Archive, Archive>> archives_;
    std::map<std::string, bool> results_;
    std::set<std::string> host_archives_, host_assets_;
};

Profile &profile() {
    static Profile active;
    return active;
}

bool valid_ram(uint32_t address, uint32_t bytes) {
    return address != 0 && address <= EE_RAM_SIZE - bytes;
}

std::string read_label(uint32_t address, uint32_t bytes) {
    if (!valid_ram(address, bytes)) return {};
    const auto *start = reinterpret_cast<const char *>(g_ram + address);
    const char *end = std::find(start, start + bytes, '\0');
    return end == start + bytes ? std::string{} : std::string(start, end);
}

uint32_t append_badge(uint32_t address, uint32_t capacity, BadgeBytes *original = nullptr) {
    const auto label = read_label(address, capacity);
    constexpr std::string_view badge = kBadge;
    if (label.empty() || label.size() + sizeof kBadge > capacity ||
        (label.size() >= badge.size() && label.compare(label.size() - badge.size(), badge.size(), badge) == 0)) return 0;
    const uint32_t tail = address + static_cast<uint32_t>(label.size());
    if (original) ee_read_block(tail, original->data(), original->size());
    ee_write_block(tail, kBadge, sizeof kBadge);
    return tail;
}
} // namespace

void arcade_mod_badges_run_menu(uint32_t caller, void (*run_menu)()) {
    constexpr struct MenuRows { uint32_t caller, labels, ids, count, capacity; } layouts[] = {
        {0x002C7ACC, 0x24, 0xC8, 0x16C, 41},
        {0x002C8304, 0x10, 0xC0, 0x374, 44},
        {0x002C8A20, 0x20, 0xD0, 0x264, 44},
    };
    struct LabelTail { uint32_t address; BadgeBytes original; };
    std::vector<LabelTail> tails;
    const uint32_t sp = g_ee.r[29].uw[0];
    for (const auto &layout : layouts) {
        if (caller != layout.caller || !valid_ram(sp, layout.count + 4)) continue;
        const uint32_t rows = ee_r32(sp + layout.count);
        const uint32_t eves = ee_r32(0x003C4CF4);
        if (rows > layout.capacity || eves > 64) break;
        for (uint32_t row = 0; row < rows; ++row) {
            const uint32_t id = ee_r32(sp + layout.ids + row * 4);
            if (id >= eves || !profile().track(read_label(0x003C4D08 + id * 0x84 + 8, 32))) continue;
            LabelTail tail{};
            tail.address = append_badge(ee_r32(sp + layout.labels + row * 4), 0x50, &tail.original);
            if (tail.address) tails.push_back(tail);
        }
        break;
    }
    run_menu();
    for (const auto &tail : tails) ee_write_block(tail.address, tail.original.data(), tail.original.size());
}

extern "C" void f_002A8EF0_orig();

extern "C" void f_002A8EF0() {
    const uint32_t caller = g_ee.r[31].uw[0];
    if (caller == 0x002A7FF4 || caller == 0x002B9EAC) {
        const uint32_t widget = g_ee.r[19].uw[0], chassis = g_ee.r[20].uw[0];
        if (valid_ram(widget, 0xA8) && valid_ram(chassis, 0x58) && g_ee.r[5].uw[0] == widget + 0x6A) {
            const uint32_t paint = ee_r32(widget + 0x24);
            if (paint < 10 && profile().vehicle(read_label(widget + 0x49, 0x21), ee_r32(chassis + 4 + 8 * paint)))
                append_badge(widget + 0x6A, 0x22);
        }
    }
    f_002A8EF0_orig();
}
