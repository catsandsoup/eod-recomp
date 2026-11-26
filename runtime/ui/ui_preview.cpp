// ui_preview.cpp - see ui_preview.h.
#include "../core/host_file.h"
#include "ui_preview.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

#include "../platform/platform.h"

namespace ui_preview {
namespace {
std::string g_root;
ui_art::AddTexture g_add = nullptr;
std::map<std::string, Pic> g_cache;

uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

// One 0TSR v6 archive, indexed once; entries are read with fseek (CT.RES is 60 MB).
struct Index {
    std::string path;
    bool tried = false;
    struct E { uint32_t off, size; };
    std::map<std::string, E> entries;
    bool open(const char *rel, bool allow_mods = true) {
        if (tried) return !entries.empty();
        tried = true;
        char found[1024];
        const char *mods = getenv("EOD_MOD_DIR");
        if (allow_mods && mods && *mods && platform_find_file(mods, rel, found, sizeof found)) path = found;
        if (allow_mods && path.empty() && !g_root.empty()) {
            // Also probe <game_root>/../mods/combined and <game_root>/mods/combined for integrated mod packs
            const std::string candidates[] = {
                g_root + "/mods/combined",
                g_root + "/../mods/combined",
            };
            for (const auto &cand : candidates) {
                if (platform_find_file(cand.c_str(), rel, found, sizeof found)) { path = found; break; }
            }
        }
        if (path.empty()) {
            if (g_root.empty() || !platform_find_file(g_root.c_str(), rel, found, sizeof found)) return false;
            path = found;
        }
        FILE *f = host_fopen(found, "rb");
        if (!f) return false;
        uint8_t head[0x28];
        if (fread(head, 1, sizeof head, f) != sizeof head || memcmp(head, "0TSR", 4) != 0 || rd32(head + 8) != 6) { fclose(f); return false; }
        const uint32_t dir = rd32(head + 20), count = rd32(head + 24);
        if (count > 4096) { fclose(f); return false; }
        std::vector<uint8_t> d((size_t)count * 0x44);
        if (fseek(f, (long)dir, SEEK_SET) != 0 || fread(d.data(), 1, d.size(), f) != d.size()) { fclose(f); return false; }
        fclose(f);
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *e = &d[(size_t)i * 0x44];
            entries[std::string((const char *)e, strnlen((const char *)e, 32))] = {dir + rd32(e + 0x2C), rd32(e + 0x28)};
        }
        return true;
    }
    bool read(const std::string &name, std::vector<uint8_t> &out) {
        auto it = entries.find(name);
        if (it == entries.end() || it->second.size > (4u << 20)) return false;
        FILE *f = host_fopen(path.c_str(), "rb");
        if (!f) return false;
        out.resize(it->second.size);
        const bool ok = fseek(f, (long)it->second.off, SEEK_SET) == 0 && fread(out.data(), 1, out.size(), f) == out.size();
        fclose(f);
        return ok;
    }
} g_big, g_uicar, g_retail_uicar;

std::string slug(const std::string &name) {
    std::string s;
    for (char c : name) if (std::isalnum((unsigned char)c)) s.push_back((char)std::tolower((unsigned char)c));
    if (s.size() > 2 && s.substr(s.size() - 2) == "mp") s.resize(s.size() - 2);
    if (s == "home" || s == "homeba1") return "homebase";
    if (s == "woodly" || s == "afton") return "afton";
    if (s == "spring1") return "springfield";
    if (s == "dennis1") return "dennison";
    if (s == "hastin1") return "hastings";
    if (s == "mankat1") return "mankato";
    if (s == "bemidj1") return "bemidji";
    return s;
}
}  // namespace

void init(const std::string &game_root, ui_art::AddTexture add) { g_root = game_root; g_add = add; }

Pic track(const std::string &name) {
    const std::string key = "t:" + name;
    auto it = g_cache.find(key);
    if (it != g_cache.end()) return it->second;
    Pic p;
    std::vector<uint8_t> t;
    if (g_add && g_big.open("DATA/RES/UIBIG.RES") && (g_big.read("l-" + slug(name) + ".img", t) || g_big.read("l-default.img", t)) && t.size() >= 64 + 1024 + 512 * 512) {
        const uint32_t w = rd32(&t[0]), h = rd32(&t[4]);
        if (w == 512 && h == 512) {
            std::vector<uint32_t> rgba((size_t)w * h);
            uint32_t pal[256];
            for (int i = 0; i < 256; i++) { const uint8_t *c = &t[64 + 4 * i]; pal[i] = c[0] | (c[1] << 8) | (c[2] << 16) | 0xFF000000u; }
            for (size_t i = 0; i < rgba.size(); i++) rgba[i] = pal[t[64 + 1024 + i]];
            p.tex = g_add((int)w, (int)h, rgba.data());
            p.v0 = 132.0f / 512; p.v1 = 1;          // the map with the photo and the name label (the top band is a hint text)
            p.aspect = 512.0f / (512 - 132);
        }
    }
    g_cache[key] = p;
    return p;
}

Pic car(uint32_t number) {
    if (!number) return {};
    const std::string key = "c:" + std::to_string(number);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) return it->second;
    Pic p;
    std::vector<uint8_t> t;
    ui_art::Image img;
    const std::string tm0_entry = "uicar_" + std::to_string(number) + ".tm0";
    const std::string tex_entry = "uicar_" + std::to_string(number) + ".tex";
    // Check modded UICAR.RES first (Phantom, NASCAR Cup Cars, custom cars), then retail UICAR.RES.
    bool loaded = false;
    if (g_add) {
        if (g_uicar.open("DATA/RES/UICAR.RES", true)) {
            loaded = (g_uicar.read(tm0_entry, t) || g_uicar.read(tex_entry, t)) && ui_art::decode_tex(t, img);
        }
        if (!loaded && g_retail_uicar.open("DATA/RES/UICAR.RES", false)) {
            loaded = (g_retail_uicar.read(tm0_entry, t) || g_retail_uicar.read(tex_entry, t)) && ui_art::decode_tex(t, img);
        }
        if (loaded) {
            p.tex = g_add(img.w, img.h, img.rgba.data());
            p.aspect = (float)img.w / (float)img.h;
        }
    }
    g_cache[key] = p;
    return p;
}
}  // namespace ui_preview
