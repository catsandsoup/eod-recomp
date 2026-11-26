#include "custom_map.h"
#include "../../tools/editor/third_party/nlohmann/json.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string_view>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#elif defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/sha.h>
#endif

namespace eod::vfs {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using View = std::string_view;
constexpr size_t package_limit = 256u << 20;
constexpr std::array<const char *, 4> names{"manifest.json", "spawns.json", "scene.glb", "collision.eodcol"};

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

uint32_t u32(View data, size_t at) {
    require(at <= data.size() && data.size() - at >= 4, "Truncated map member");
    const auto *p = reinterpret_cast<const unsigned char *>(data.data() + at);
    return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

size_t octal(View field) {
    size_t value = 0;
    while (!field.empty() && (field.back() == '\0' || field.back() == ' ')) field.remove_suffix(1);
    require(!field.empty(), "Missing archive size or checksum");
    for (char c : field) {
        require(c >= '0' && c <= '7' && value <= package_limit, "Invalid archive field");
        value = value * 8 + size_t(c - '0');
    }
    return value;
}

std::array<View, 4> read_members(View bytes) {
    std::array<View, 4> members{};
    for (size_t n = 0; n < members.size(); ++n) {
        require(bytes.size() >= 512, "Truncated map header");
        View header = bytes.substr(0, 512);
        auto name = header.substr(0, header.substr(0, 100).find('\0'));
        auto it = std::find(names.begin(), names.end(), name);
        require(it != names.end() && header[156] == '0' && header[345] == 0 &&
                header.substr(257, 5) == "ustar", "Unexpected map member");
        size_t checksum = 0;
        for (size_t i = 0; i < 512; ++i)
            checksum += i >= 148 && i < 156 ? 32 : static_cast<unsigned char>(header[i]);
        require(checksum == octal(header.substr(148, 8)), "Invalid archive checksum");
        size_t size = octal(header.substr(124, 12));
        auto &member = members[it - names.begin()];
        bytes.remove_prefix(512);
        size_t padded = (size + 511) / 512 * 512;
        require(member.empty() && size > 0 && padded <= bytes.size(), "Duplicate or truncated map member");
        member = bytes.substr(0, size);
        bytes.remove_prefix(padded);
    }
    require(bytes.size() >= 1024 && bytes.find_first_not_of('\0') == View::npos, "Invalid archive end");
    return members;
}

std::string sha256(const std::string &input) {
    unsigned char result[32];
#ifdef __APPLE__
    CC_SHA256(input.data(), static_cast<CC_LONG>(input.size()), result);
#elif defined(_WIN32)
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    require(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0, "SHA-256 alg failed");
    NTSTATUS st = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    if (st == 0) st = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char *>(input.data())), static_cast<ULONG>(input.size()), 0);
    if (st == 0) st = BCryptFinishHash(hash, result, sizeof(result), 0);
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    require(st == 0, "SHA-256 failed");
#else
    SHA256(reinterpret_cast<const unsigned char *>(input.data()), input.size(), result);
#endif
    std::string hex;
    for (auto b : result) {
        constexpr char alphabet[] = "0123456789abcdef";
        hex += alphabet[b >> 4];
        hex += alphabet[b & 15];
    }
    return hex;
}

std::string digest(const std::array<View, 4> &members) {
    std::string input;
    for (View member : members) {
        uint64_t size = member.size();
        for (unsigned i = 0; i < 8; ++i) input.push_back(char(size >> (8 * i)));
        input.append(member.data(), member.size());
    }
    return sha256(input);
}

MapPoint point(const Json &value) {
    require(value.is_array() && value.size() == 3, "Expected a 3D point");
    MapPoint p = value.get<MapPoint>();
    for (float x : p) require(std::isfinite(x), "Non-finite coordinate");
    return p;
}

std::vector<MapSpawn> poses(const Json &array) {
    require(array.is_array(), "Expected an array of map poses");
    std::vector<MapSpawn> result;
    for (auto &record : array) {
        MapSpawn pose{point(record.at("position")), point(record.at("normal")), record.at("heading_deg").get<float>()};
        double length = std::hypot(double(pose.normal[0]), double(pose.normal[1]), double(pose.normal[2]));
        require(length > .999 && length < 1.001 && std::isfinite(pose.heading), "Invalid spawn orientation");
        result.push_back(pose);
    }
    return result;
}

void geometry(CustomMap &map, View scene, View col) {
    require(col.substr(0, 8) == View("EODCOL1\0", 8), "Unknown collision format");
    const uint64_t count = u32(col, 8), indices = u32(col, 12);
    require(indices % 3 == 0 && col.size() == 16 + count * 12 + indices * 4, "Invalid collision dimensions");
    require(scene.substr(0, 4) == "glTF" && u32(scene, 4) == 2 && u32(scene, 8) == scene.size() &&
            scene.substr(16, 4) == "JSON", "Invalid GLB header");
    size_t json_size = u32(scene, 12);
    require(json_size <= scene.size() - 20, "Truncated GLB JSON");
    auto doc = Json::parse(scene.substr(20, json_size));
    require(doc.at("asset").at("version") == "2.0", "Unsupported scene format");
    if (!count) {
        require(indices == 0 && !doc.contains("meshes") && scene.size() == 20 + json_size, "Invalid empty scene");
        return;
    }
    size_t at = 20 + json_size;
    require(u32(scene, at) == count * 24 + indices * 4 && scene.substr(at + 4, 4) == View("BIN\0", 4) &&
            scene.size() == at + 8 + count * 24 + indices * 4, "Invalid GLB binary payload");
    auto bin = scene.substr(at + 8);
    const auto &views = doc.at("bufferViews"), &accessors = doc.at("accessors");
    const auto &prim = doc.at("meshes").at(0).at("primitives").at(0);
    require(prim.at("attributes").at("POSITION") == 0 && prim.at("attributes").at("COLOR_0") == 1 &&
            prim.at("indices") == 2 && doc.at("buffers").at(0).at("byteLength") == bin.size(), "Invalid GLB primitive");
    for (size_t i = 0; i < 3; ++i) {
        require(views.at(i).at("buffer") == 0 && views.at(i).at("byteOffset") == count * 12 * i &&
                views.at(i).at("byteLength") == (i == 2 ? indices * 4 : count * 12) &&
                accessors.at(i).at("bufferView") == i && accessors.at(i).at("count") == (i == 2 ? indices : count) &&
                accessors.at(i).at("componentType") == (i == 2 ? 5125 : 5126) &&
                accessors.at(i).at("type") == (i == 2 ? "SCALAR" : "VEC3"), "Unexpected GLB layout");
    }
    require(bin.substr(0, count * 12) == col.substr(16, count * 12) &&
            bin.substr(count * 24) == col.substr(16 + count * 12), "Scene and collision geometry differ");
    for (auto *array : {&map.vertices, &map.colors}) {
        array->resize(count);
        for (auto &p : *array) for (float &x : p) {
            uint32_t bits = u32(bin, 0);
            std::memcpy(&x, &bits, 4);
            bin.remove_prefix(4);
            require(std::isfinite(x), "Non-finite vertex");
            if (array == &map.colors) require(x >= 0 && x <= 1, "Invalid vertex color");
        }
    }
    map.indices.reserve(indices);
    while (!bin.empty()) {
        uint32_t index = u32(bin, 0);
        require(index < count, "Invalid triangle index");
        map.indices.push_back(index);
        bin.remove_prefix(4);
    }
    require(map.collision.build(map.vertices, map.indices), "Invalid collision mesh");
}

// Keep extracted VFS files alive until the last reader releases this mount.
struct MountedMap : CustomMap {
    fs::path root;
    ~MountedMap() {
        std::error_code ec;
        if (!root.empty()) fs::remove_all(root, ec);
    }
};

std::string read_archive(const fs::path &archive) {
    std::ifstream in(archive, std::ios::binary | std::ios::ate);
    auto size = in.tellg();
    require(size > 0 && size <= std::streamoff(package_limit), "Missing, empty, or oversized map");
    std::string bytes(static_cast<size_t>(size), '\0');
    require(bool(in.seekg(0).read(bytes.data(), size)), "Cannot read map");
    return bytes;
}

std::shared_ptr<MountedMap> parse_map(const std::array<View, 4> &members) {
    auto manifest = Json::parse(members[0]);
    auto map = std::make_shared<MountedMap>();
    require(manifest.at("version") == 1, "Unsupported map version");
    manifest.at("title").get_to(map->title);
    manifest.at("author").get_to(map->author);
    manifest.at("base_arena").get_to(map->base_arena);
    require(!map->base_arena.empty() && map->base_arena.size() <= 64 &&
            map->base_arena.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_ -") ==
            std::string::npos, "Invalid arena ID");
    manifest.at("validated").get_to(map->validated);
    manifest.at("replace_retail_spawns").get_to(map->replace_retail_spawns);
    manifest.at("sha256").get_to(map->sha256);
    manifest.erase("sha256");
    auto canonical = manifest.dump(-1, ' ', true);
    auto hashed = members;
    hashed[0] = canonical;
    require(digest(hashed) == map->sha256, "Map content SHA-256 mismatch");
    auto spawns = Json::parse(members[1]);
    map->grid_starts = poses(spawns.at("grid_starts"));
    map->battle_respawns = poses(spawns.at("battle_respawns"));
    map->checkpoints = poses(spawns.at("checkpoints"));
    map->finish_gates = poses(spawns.at("finish_gates"));
    map->ai_waypoints = poses(spawns.at("ai_waypoints"));
    require(!map->replace_retail_spawns || !map->grid_starts.empty() || !map->battle_respawns.empty(),
            "A replacement map needs a starting pose");
    geometry(*map, members[2], members[3]);
    return map;
}
} // namespace

std::string map_archive_sha256(const fs::path &archive, std::string &error) {
    try {
        error.clear();
        return sha256(read_archive(archive));
    } catch (const std::exception &e) {
        error = e.what();
        return {};
    }
}

std::shared_ptr<const CustomMap> inspect_map(const fs::path &archive, std::string &error) {
    try {
        error.clear();
        auto bytes = read_archive(archive);
        return parse_map(read_members(bytes));
    } catch (const std::exception &e) {
        error = e.what();
        return {};
    }
}

bool mount_map(const fs::path &archive, Layer layer, std::string &error, const std::string &expected_hash) {
    try {
        auto bytes = read_archive(archive);
        if (!expected_hash.empty()) require(sha256(bytes) == expected_hash, "Archive SHA-256 mismatch");
        auto members = read_members(bytes);
        auto map = parse_map(members);
        auto root = fs::temp_directory_path() / ("eodmap-" + std::to_string(std::random_device{}())
                                                 + "-" + std::to_string(std::random_device{}()));
        require(fs::create_directory(root), "Cannot create map mount");
        map->root = root;
        for (size_t i = 0; i < members.size(); ++i) {
            std::ofstream out(root / names[i], std::ios::binary);
            out.write(members[i].data(), members[i].size());
            out.close();
            require(bool(out), "Cannot stage map member");
        }
        require(global_vfs().mount(layer, root.string(), map->sha256, map), "Cannot activate map mount");
        error.clear();
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
}
