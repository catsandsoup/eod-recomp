// runtime/remaster/asset_registry.cpp
// Halo 2 Anniversary / Saber3D-style Asset Injection Registry implementation.
// ZERO #include of runtime/gs/*, runtime/vu/*, or vif_mesh.h.
#include "asset_registry.h"
#include "ee_texture_decoder.h"
#include "graf_loader.h"
#include "../core/memory.h"
#include "../render/material_table.inc"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace remaster {

namespace {

bool file_exists(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

// Simple uncompressed 24/32-bit TGA loader for zero-dependency HD PBR texture overrides.
bool load_tga_file(const std::string &path, bool is_srgb, CpuImageRgba8 &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return false;
    uint8_t hdr[18] = {};
    f.read(reinterpret_cast<char *>(hdr), 18);
    if (!f.good()) return false;
    const uint8_t id_len = hdr[0];
    const uint8_t img_type = hdr[2];
    const uint16_t w = static_cast<uint16_t>(hdr[12] | (hdr[13] << 8));
    const uint16_t h = static_cast<uint16_t>(hdr[14] | (hdr[15] << 8));
    const uint8_t bpp = hdr[16];
    const bool top_down = (hdr[17] & 0x20u) != 0;
    if (img_type != 2 || w == 0 || h == 0 || w > 8192 || h > 8192 || (bpp != 24 && bpp != 32))
        return false;
    if (id_len) f.seekg(id_len, std::ios::cur);
    const size_t stride = static_cast<size_t>(w) * (bpp / 8);
    std::vector<uint8_t> raw(stride * h);
    f.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(raw.size()));
    if (!f.good()) return false;

    out.width = w;
    out.height = h;
    out.is_srgb = is_srgb;
    out.pixels.resize(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t sy = top_down ? y : (h - 1 - y);
        const uint8_t *src = raw.data() + sy * stride;
        uint8_t *dst = out.pixels.data() + static_cast<size_t>(y) * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            dst[x * 4 + 0] = src[x * (bpp / 8) + 2];
            dst[x * 4 + 1] = src[x * (bpp / 8) + 1];
            dst[x * 4 + 2] = src[x * (bpp / 8) + 0];
            dst[x * 4 + 3] = (bpp == 32) ? src[x * 4 + 3] : 255;
        }
    }
    return true;
}

// Generate a clean 1x1 linear PBR fallback texture.
CpuImageRgba8 make_solid_rgba8(uint8_t r, uint8_t g, uint8_t b, uint8_t a, bool is_srgb) {
    CpuImageRgba8 img;
    img.width = 1;
    img.height = 1;
    img.is_srgb = is_srgb;
    img.pixels = {r, g, b, a};
    return img;
}

std::string strip_tex_prefix(const std::string &name) {
    if (name.rfind("tex/", 0) == 0) return name.substr(4);
    return name;
}

std::string to_lower_copy(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool is_beam_or_cone_name(const std::string &bare) {
    const std::string low = to_lower_copy(bare);
    for (const char *n : {"beam", "cone", "lcone", "spot", "flood", "shaft", "glow", "halo"}) {
        if (low.find(n) != std::string::npos) return true;
    }
    return false;
}

struct AuthoredMaterialRecord {
    std::string id;
    std::string surface;
    float metalness = 0.0f;
    float roughness = 0.85f;
    bool emissive = false;
    std::string alpha; // "opaque", "cutout", "blend"
};

struct MaterialTableDb {
    struct Entry {
        uint64_t pk = 0;
        uint32_t w = 0;
        uint32_t h = 0;
        uint32_t rec_idx = 0;
    };
    std::vector<AuthoredMaterialRecord> records;
    std::unordered_map<uint64_t, std::vector<Entry>> by_ik;
    size_t bound_rows = 0;

    MaterialTableDb() {
        std::unordered_map<std::string, uint32_t> by_id;
        const char *p = k_material_tsv;
        while (*p) {
            const char *e = std::strchr(p, '\n');
            if (!e) e = p + std::strlen(p);
            const size_t len = static_cast<size_t>(e - p);
            if (len > 0 && p[0] != '/' && p[0] != 's') {
                std::string line(p, len);
                std::vector<std::string> f;
                f.reserve(14);
                size_t a = 0;
                for (;;) {
                    const size_t b = line.find('\t', a);
                    f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
                    if (b == std::string::npos) break;
                    a = b + 1;
                }
                if (f.size() >= 13) {
                    uint32_t idx = 0;
                    auto it = by_id.find(f[0]);
                    if (it == by_id.end()) {
                        AuthoredMaterialRecord rec;
                        rec.id = f[0];
                        rec.surface = f[5];
                        rec.metalness = std::clamp(std::strtof(f[6].c_str(), nullptr), 0.0f, 1.0f);
                        rec.roughness = std::clamp(std::strtof(f[7].c_str(), nullptr), 0.04f, 1.0f);
                        rec.emissive = (f[9] == "1");
                        rec.alpha = f[10];
                        idx = static_cast<uint32_t>(records.size());
                        records.push_back(std::move(rec));
                        by_id[f[0]] = idx;
                    } else {
                        idx = it->second;
                    }
                    if (!f[1].empty() && f[1] != "-") {
                        const uint64_t ik = std::strtoull(f[1].c_str(), nullptr, 16);
                        const uint64_t pk = std::strtoull(f[2].c_str(), nullptr, 16);
                        const uint32_t w = static_cast<uint32_t>(std::atoi(f[3].c_str()));
                        const uint32_t h = static_cast<uint32_t>(std::atoi(f[4].c_str()));
                        by_ik[ik].push_back({pk, w, h, idx});
                        ++bound_rows;
                    }
                }
            }
            p = *e ? e + 1 : e;
        }
    }

    const AuthoredMaterialRecord *lookup(uint64_t ik, uint64_t pk, uint32_t w, uint32_t h) const {
        auto it = by_ik.find(ik);
        if (it == by_ik.end()) return nullptr;
        const AuthoredMaterialRecord *fallback = nullptr;
        for (const auto &e : it->second) {
            if (e.w != w || e.h != h) continue;
            if (e.pk == pk) return &records[e.rec_idx];
            if (!fallback) fallback = &records[e.rec_idx];
        }
        return fallback;
    }
};

const MaterialTableDb &material_db() {
    static const MaterialTableDb db;
    return db;
}

bool is_authored_solid_non_emissive(const AuthoredMaterialRecord &rec) {
    if (rec.emissive) return false;
    const std::string &s = rec.surface;
    return s == "dirt" || s == "grass_ground" || s == "gravel" || s == "mud" || s == "sand" ||
           s == "asphalt" || s == "concrete" || s == "wood" || s == "brick" || s == "rock" ||
           s == "sign_decal" || s == "foliage_cutout" || s == "crowd_sprite" || s == "bark" ||
           s == "backdrop" || s == "fake_shadow" || s == "car_paint" || s == "rubber" ||
           s == "car_interior" || s == "fabric" || s == "plastic";
}

PbrMaterial material_from_authored_record(const AuthoredMaterialRecord &rec,
                                          const std::string &bare_name,
                                          bool is_sky,
                                          bool is_vehicle) {
    PbrMaterial m{};
    const std::string &su = rec.surface;

    if (is_sky || su == "sky") {
        m.mat_class = MaterialClass::Sky;
        m.is_sky = true;
        m.roughness = 1.0f;
        m.metallic = 0.0f;
        return m;
    }

    if (is_vehicle) {
        if (su == "glass") {
            m.mat_class = MaterialClass::Glass;
            m.roughness = 0.06f;
            m.metallic = 0.0f;
        } else if (su == "chrome" || (su == "metal_bare" && rec.metalness >= 0.70f)) {
            m.mat_class = MaterialClass::MetalChrome;
            m.roughness = 0.16f;
            m.metallic = 0.90f;
        } else if (su == "metal_bare") {
            m.mat_class = MaterialClass::MetalChrome;
            m.roughness = std::clamp(rec.roughness, 0.16f, 0.45f);
            m.metallic = std::max(rec.metalness, 0.80f);
        } else if (su == "metal_rusty") {
            // Oxidized iron: rough matte rust crust with subtle exposed-steel metallic grazing response
            m.mat_class = MaterialClass::Default;
            m.roughness = std::clamp(rec.roughness, 0.72f, 0.82f);
            m.metallic = std::clamp(rec.metalness > 0.01f ? rec.metalness : 0.28f, 0.22f, 0.35f);
        } else if (su == "rubber") {
            m.mat_class = MaterialClass::Rubber;
            m.roughness = 0.88f;
            m.metallic = 0.0f;
        } else if (su == "car_interior" || su == "fabric" || su == "plastic") {
            m.mat_class = MaterialClass::Interior;
            m.roughness = (su == "plastic") ? 0.50f : 0.88f;
            m.metallic = 0.0f;
        } else if (su == "emissive_light" || rec.emissive) {
            m.mat_class = MaterialClass::Emissive;
            m.roughness = 0.14f;
            m.metallic = 0.0f;
            m.emissive_scale = 2.0f;
        } else if (su == "car_paint" || su == "metal_painted") {
            m.mat_class = MaterialClass::CarPaint;
            m.roughness = std::clamp(rec.roughness, 0.55f, 0.70f);
            m.metallic = 0.10f;
        } else {
            m.mat_class = MaterialClass::CarPaint;
            m.roughness = std::clamp(rec.roughness, 0.55f, 0.88f);
            m.metallic = std::clamp(rec.metalness, 0.0f, 0.25f);
        }
    } else {
        // Static world / track / environment: physically differentiated per real-world material!
        // Dry terrain/dirt/grass/asphalt/concrete/wood/billboards stay strictly matte (rough >= 0.82..0.90),
        // while galvanized steel, rusted iron, painted metal, wet mud, and molded plastic respond truthfully
        // across day, dusk, night floodlights, and water.
        if (su == "water") {
            m.mat_class = MaterialClass::Water;
            m.roughness = 0.10f;
            m.metallic = 0.0f;
        } else if (su == "glass") {
            m.mat_class = MaterialClass::Glass;
            m.roughness = 0.06f;
            m.metallic = 0.0f;
        } else if (su == "chrome") {
            m.mat_class = MaterialClass::MetalChrome;
            m.roughness = 0.16f;
            m.metallic = 0.90f;
        } else if (su == "metal_bare") {
            // Galvanized steel guardrails, corrugated fences, light poles, stadium trusses
            m.mat_class = MaterialClass::MetalChrome;
            m.roughness = std::clamp(rec.roughness, 0.35f, 0.52f);
            m.metallic = std::clamp(rec.metalness > 0.01f ? rec.metalness : 0.72f, 0.65f, 0.85f);
        } else if (su == "metal_rusty") {
            // Weathered rusted iron / scrap barriers: rough oxide with subtle metallic sheen on worn steel
            m.mat_class = MaterialClass::Default;
            m.roughness = std::clamp(rec.roughness, 0.72f, 0.82f);
            m.metallic = std::clamp(rec.metalness > 0.01f ? rec.metalness : 0.28f, 0.22f, 0.35f);
        } else if (su == "metal_painted") {
            // Painted metal guardrails, barn roofs, painted steel structures: satin enamel over steel
            m.mat_class = MaterialClass::Default;
            m.roughness = std::clamp(rec.roughness, 0.48f, 0.62f);
            m.metallic = 0.14f;
        } else if (su == "mud") {
            // Damp mud bogs / wet ruts: subtle water-film grazing reflection (distinct from dry dirt)
            m.mat_class = MaterialClass::TerrainDirt;
            m.roughness = 0.48f;
            m.metallic = 0.0f;
        } else if (su == "plastic") {
            // Molded PVC traffic cones, plastic barrels: subtle satin dielectric
            m.mat_class = MaterialClass::Default;
            m.roughness = 0.46f;
            m.metallic = 0.0f;
        } else if (su == "emissive_light" || su == "fx" || su == "vfx_fire" || su == "vfx_spark") {
            m.mat_class = MaterialClass::Emissive;
            m.roughness = 0.90f;
            m.metallic = 0.0f;
            m.alpha_additive = true;
            m.double_sided = true;
            m.emissive_scale = is_beam_or_cone_name(bare_name) ? 0.45f : 1.50f;
        } else if (su == "foliage_cutout" || su == "crowd_sprite" || su == "bark") {
            m.mat_class = MaterialClass::Foliage;
            m.roughness = std::max(rec.roughness, 0.88f);
            m.metallic = 0.0f;
            if (su != "bark" || rec.alpha == "cutout") {
                m.alpha_cutout = true;
                m.double_sided = true;
            }
        } else if (su == "dirt" || su == "gravel" || su == "sand" || su == "grass_ground") {
            // Dry dirt, gravel, sand, and grass are ALWAYS strictly matte (never wet!)
            m.mat_class = MaterialClass::TerrainDirt;
            m.roughness = std::max(rec.roughness, 0.90f);
            m.metallic = 0.0f;
        } else if (su == "asphalt") {
            m.mat_class = MaterialClass::Asphalt;
            m.roughness = std::max(rec.roughness, 0.84f);
            m.metallic = 0.0f;
        } else if (su == "rubber") {
            m.mat_class = MaterialClass::Rubber;
            m.roughness = 0.88f;
            m.metallic = 0.0f;
        } else {
            // concrete, brick, rock, wood, sign_decal, backdrop, mixed_atlas, fabric, fake_shadow, other
            m.mat_class = MaterialClass::BuildingConcrete;
            m.roughness = std::max(rec.roughness, 0.88f);
            m.metallic = 0.0f;
            if (rec.emissive) {
                m.emissive_scale = 1.25f;
            }
        }
    }

    if (!m.alpha_additive) {
        if (rec.alpha == "cutout") {
            m.alpha_cutout = true;
            m.double_sided = true;
        } else if (rec.alpha == "blend") {
            m.alpha_blend = true;
        }
    }
    return m;
}

} // namespace

AssetRegistry &AssetRegistry::instance() {
    static AssetRegistry reg;
    return reg;
}

AssetRegistry::AssetRegistry() {
    if (const char *env = std::getenv("EOD_REMASTER_ASSETS"); env && *env) {
        root_dir_ = env;
    } else if (const char *mod = std::getenv("EOD_MOD_DIR"); mod && *mod) {
        root_dir_ = std::string(mod) + "/remaster";
    }
}

void AssetRegistry::set_remaster_root(const std::string &root_dir) {
    std::lock_guard<std::mutex> lk(mtx_);
    root_dir_ = root_dir;
}

std::string AssetRegistry::remaster_root() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return root_dir_;
}

uint32_t AssetRegistry::register_mesh(const std::string &asset_id, CpuMeshData retail_fallback) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = id_to_handle_.find(asset_id);
    if (it != id_to_handle_.end()) {
        return it->second;
    }

    auto mesh = std::make_shared<CpuMeshData>(std::move(retail_fallback));
    mesh->asset_id = asset_id;

    // Halo 2 Anniversary asset injection hook: check `<root>/<asset_id>.glb` then `.obj`.
    if (!root_dir_.empty() && !asset_id.empty()) {
        const std::string glb_path = root_dir_ + "/" + asset_id + ".glb";
        const std::string obj_path = root_dir_ + "/" + asset_id + ".obj";
        CpuMeshData hd;
        if (file_exists(glb_path) && load_glb_file(glb_path, asset_id, hd)) {
            hd.texture_id = mesh->texture_id;
            hd.material = mesh->material;
            *mesh = std::move(hd);
        } else if (file_exists(obj_path) && load_obj_file(obj_path, asset_id, hd)) {
            hd.texture_id = mesh->texture_id;
            hd.material = mesh->material;
            *mesh = std::move(hd);
        }
    }

    const uint32_t handle = next_handle_++;
    id_to_handle_[asset_id] = handle;
    meshes_[handle] = std::move(mesh);
    return handle;
}

std::shared_ptr<const CpuMeshData> AssetRegistry::get_mesh(uint32_t handle) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = meshes_.find(handle);
    return it != meshes_.end() ? it->second : nullptr;
}

std::shared_ptr<const CpuMeshData> AssetRegistry::find_mesh(const std::string &asset_id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = id_to_handle_.find(asset_id);
    if (it == id_to_handle_.end()) return nullptr;
    auto mit = meshes_.find(it->second);
    return mit != meshes_.end() ? mit->second : nullptr;
}

std::shared_ptr<const PbrTextureSet> AssetRegistry::resolve_texture(const std::string &texture_name,
                                                                    bool is_sky,
                                                                    bool is_vehicle) {
    int32_t explicit_slot = -1;
    std::string bare = strip_tex_prefix(texture_name);
    std::string key = "tex/" + bare;
    if (texture_name.rfind("slot/", 0) == 0) {
        const size_t slash2 = texture_name.find('/', 5);
        if (slash2 != std::string::npos) {
            explicit_slot = std::atoi(texture_name.substr(5, slash2 - 5).c_str());
            bare = texture_name.substr(slash2 + 1);
            key = texture_name;
        }
    }

    std::lock_guard<std::mutex> lk(mtx_);
    auto it = textures_.find(key);
    if (it != textures_.end() && (it->second->has_hd_override || it->second->albedo.width > 1))
        return it->second;

    auto set = std::make_shared<PbrTextureSet>();
    set->texture_id = key;
    set->material = classify_pbr_material(bare, is_sky, is_vehicle);

    // 1. Halo 2 Anniversary HD PBR texture override: <root>/tex/<name>_{albedo,normal,roughness,metallic,ao}.tga
    if (!root_dir_.empty() && !bare.empty()) {
        const std::string base = root_dir_ + "/tex/" + bare;
        if (load_tga_file(base + "_albedo.tga", true, set->albedo)) set->has_hd_override = true;
        if (load_tga_file(base + "_normal.tga", false, set->normal)) set->has_hd_override = true;
        if (load_tga_file(base + "_roughness.tga", false, set->roughness)) set->has_hd_override = true;
        if (load_tga_file(base + "_metallic.tga", false, set->metallic)) set->has_hd_override = true;
        if (load_tga_file(base + "_ao.tga", false, set->ao)) set->has_hd_override = true;
    }

    // 2. Real retail texture decoded directly from EE RAM (*(0x4D2D30) texture slot table)
    if (set->albedo.pixels.empty() && explicit_slot >= 0) {
        decode_ee_texture_by_slot(g_ram, EE_RAM_SIZE, explicit_slot, set->albedo);
    }
    if (set->albedo.pixels.empty() && !bare.empty()) {
        decode_ee_texture_by_name(g_ram, EE_RAM_SIZE, bare, set->albedo);
    }

    // 2B. Resolve exact authored PBR material from runtime/render/material_table.inc
    // using the decoded texture's FNV-1a (index_key, palette_key, w, h).
    const AuthoredMaterialRecord *authored_rec = nullptr;
    if (!is_sky && set->albedo.has_indexed_keys) {
        authored_rec = material_db().lookup(set->albedo.index_key, set->albedo.palette_key,
                                            set->albedo.width, set->albedo.height);
        if (authored_rec) {
            set->has_material_table_match = true;
            set->material_table_surface = authored_rec->surface;
            set->material = material_from_authored_record(*authored_rec, bare, is_sky, is_vehicle);
            set->has_alpha_cutout = set->material.alpha_cutout;
            set->has_alpha_blend = set->material.alpha_blend;
            set->has_alpha_additive = set->material.alpha_additive;
        }
    }

    // Analyze actual decoded texture alpha & border luminance so:
    //   1. Additive black-background glows/bridge string lights/spotlight beam cones use
    //      additive blending (ZERO black squares around bridge lights, ZERO grey haze cones!).
    //   2. Cutout wires/pennants/fences and alpha-blended road decals/glass NEVER render as opaque rectangles.
    if (!is_sky && set->albedo.width > 1 && set->albedo.height > 1 && set->albedo.pixels.size() >= 4) {
        const uint32_t w = set->albedo.width;
        const uint32_t h = set->albedo.height;
        const size_t total = static_cast<size_t>(w) * h;
        size_t n_low = 0, n_semi = 0;
        float max_border_luma = 0.0f;
        float sum_border_luma = 0.0f;
        size_t n_border = 0;
        float max_center_luma = 0.0f;
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const size_t idx = (static_cast<size_t>(y) * w + x) * 4u;
                const float r = static_cast<float>(set->albedo.pixels[idx + 0]) / 255.0f;
                const float g = static_cast<float>(set->albedo.pixels[idx + 1]) / 255.0f;
                const float b = static_cast<float>(set->albedo.pixels[idx + 2]) / 255.0f;
                const uint8_t a = set->albedo.pixels[idx + 3];
                const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                if (a < 128u) ++n_low;
                if (a >= 24u && a <= 232u) ++n_semi;
                if (x == 0 || y == 0 || x + 1 == w || y + 1 == h) {
                    max_border_luma = std::max(max_border_luma, luma);
                    sum_border_luma += luma;
                    ++n_border;
                } else {
                    max_center_luma = std::max(max_center_luma, luma);
                }
            }
        }
        const float mean_border_luma = n_border > 0 ? (sum_border_luma / static_cast<float>(n_border)) : 1.0f;

        // Detect PS2 additive light/glow/beam/cone textures on static world geometry:
        // - Authored emissive_light / fx / vfx_fire / vfx_spark with dark border or alpha gradient
        // - Black-background glows/bridge string lights (max_border_luma < 0.08 && max_center_luma > 0.35)
        bool is_additive = false;
        if (!is_vehicle) {
            const bool authored_solid = authored_rec && is_authored_solid_non_emissive(*authored_rec);
            const bool black_border_bright_core =
                (max_border_luma < 0.08f && max_center_luma > 0.35f) ||
                (mean_border_luma < 0.03f && max_border_luma < 0.16f && max_center_luma > 0.45f);
            if (black_border_bright_core && !authored_solid) {
                is_additive = true;
            } else if (set->material.alpha_additive) {
                // Keep additive for glows/beams/cones (with alpha gradient or dark border);
                // only demote to opaque if the texture is completely opaque with bright borders.
                if (n_low > 0 || n_semi > 0 || mean_border_luma < 0.25f || max_border_luma < 0.12f) {
                    is_additive = true;
                } else {
                    set->material.alpha_additive = false;
                    set->has_alpha_additive = false;
                    set->material.emissive_scale = 1.4f;
                }
            }
        }

        if (is_additive) {
            set->has_alpha_additive = true;
            set->has_alpha_cutout = false;
            set->has_alpha_blend = false;
            set->material.alpha_additive = true;
            set->material.alpha_cutout = false;
            set->material.alpha_blend = false;
            set->material.double_sided = true;
            // Restrained emissive for beam cones / alpha-gradient FX (0.40-0.55) so sky stays crisp;
            // crisp glow for compact black-border bridge string lights / bulbs (1.2-1.5) with zero black boxes.
            const bool is_cone_or_fx = is_beam_or_cone_name(bare) ||
                                       (authored_rec && authored_rec->surface == "fx") ||
                                       (n_semi * 12u >= total);
            set->material.emissive_scale = is_cone_or_fx ? 0.42f : 1.35f;
        } else if (n_low > 0 || n_semi > 0) {
            if (set->material.mat_class == MaterialClass::Glass) {
                set->has_alpha_blend = true;
                set->material.alpha_blend = true;
                set->has_alpha_cutout = false;
                set->material.alpha_cutout = false;
            } else if (set->material.alpha_cutout || set->material.mat_class == MaterialClass::Foliage ||
                       (n_semi * 6u < total)) {
                set->has_alpha_cutout = true;
                set->material.alpha_cutout = true;
                set->has_alpha_blend = false;
                set->material.alpha_blend = false;
                set->material.double_sided = true;
            } else {
                set->has_alpha_blend = true;
                set->material.alpha_blend = true;
            }
        }
    }

    // Populate clean linear PBR fallbacks for any missing map
    if (set->albedo.pixels.empty()) {
        // Physically grounded default albedo by material class (neutral, un-baked)
        switch (set->material.mat_class) {
        case MaterialClass::TerrainDirt:      set->albedo = make_solid_rgba8(132, 108, 84, 255, true); break;
        case MaterialClass::Asphalt:          set->albedo = make_solid_rgba8(74, 74, 78, 255, true); break;
        case MaterialClass::CarPaint:         set->albedo = make_solid_rgba8(168, 62, 48, 255, true); break;
        case MaterialClass::MetalChrome:      set->albedo = make_solid_rgba8(205, 208, 212, 255, true); break;
        case MaterialClass::Glass:            set->albedo = make_solid_rgba8(48, 64, 76, 180, true); break;
        case MaterialClass::Foliage:          set->albedo = make_solid_rgba8(76, 104, 54, 255, true); break;
        case MaterialClass::BuildingConcrete: set->albedo = make_solid_rgba8(152, 148, 140, 255, true); break;
        case MaterialClass::Sky:              set->albedo = make_solid_rgba8(150, 185, 230, 255, true); break;
        case MaterialClass::Water:            set->albedo = make_solid_rgba8(46, 78, 92, 255, true); break;
        default:                              set->albedo = make_solid_rgba8(160, 160, 160, 255, true); break;
        }
    }
    if (set->normal.pixels.empty())    set->normal    = make_solid_rgba8(128, 128, 255, 255, false);
    // Fallback roughness/metallic maps are 1.0 (255) so shader `tex * pc.pbr_params` equals exact
    // material.roughness / material.metallic without squaring!
    if (set->roughness.pixels.empty()) set->roughness = make_solid_rgba8(255, 255, 255, 255, false);
    if (set->metallic.pixels.empty())  set->metallic  = make_solid_rgba8(255, 255, 255, 255, false);
    if (set->ao.pixels.empty())        set->ao        = make_solid_rgba8(255, 255, 255, 255, false);

    textures_[key] = set;
    return set;
}

bool AssetRegistry::load_obj_file(const std::string &path, const std::string &asset_id, CpuMeshData &out) {
    std::ifstream f(path);
    if (!f.good()) return false;

    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;
    std::vector<std::array<float, 2>> uvs;
    out = CpuMeshData{};
    out.asset_id = asset_id;

    bool has_file_normals = false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag == "v") {
            float x = 0, y = 0, z = 0;
            if (ss >> x >> y >> z) positions.push_back({x, y, z});
        } else if (tag == "vn") {
            float x = 0, y = 0, z = 0;
            if (ss >> x >> y >> z) { normals.push_back({x, y, z}); has_file_normals = true; }
        } else if (tag == "vt") {
            float u = 0, v = 0;
            if (ss >> u >> v) uvs.push_back({u, v});
        } else if (tag == "f") {
            std::vector<uint32_t> face_verts;
            std::string tok;
            while (ss >> tok) {
                int pi = 0, ti = 0, ni = 0;
                const size_t s1 = tok.find('/');
                if (s1 == std::string::npos) {
                    pi = std::atoi(tok.c_str());
                } else {
                    pi = std::atoi(tok.substr(0, s1).c_str());
                    const size_t s2 = tok.find('/', s1 + 1);
                    if (s2 == std::string::npos) {
                        ti = std::atoi(tok.substr(s1 + 1).c_str());
                    } else {
                        if (s2 > s1 + 1) ti = std::atoi(tok.substr(s1 + 1, s2 - s1 - 1).c_str());
                        ni = std::atoi(tok.substr(s2 + 1).c_str());
                    }
                }
                auto resolve = [](int idx, size_t sz) -> int {
                    if (idx > 0) return idx - 1;
                    if (idx < 0) return static_cast<int>(sz) + idx;
                    return -1;
                };
                const int rp = resolve(pi, positions.size());
                if (rp < 0 || rp >= static_cast<int>(positions.size())) continue;
                const int rt = resolve(ti, uvs.size());
                const int rn = resolve(ni, normals.size());

                PbrVertex v{};
                v.pos[0] = positions[rp][0];
                v.pos[1] = positions[rp][1];
                v.pos[2] = positions[rp][2];
                if (rn >= 0 && rn < static_cast<int>(normals.size())) {
                    v.normal[0] = normals[rn][0];
                    v.normal[1] = normals[rn][1];
                    v.normal[2] = normals[rn][2];
                } else {
                    v.normal[0] = 0.0f; v.normal[1] = 1.0f; v.normal[2] = 0.0f;
                }
                if (rt >= 0 && rt < static_cast<int>(uvs.size())) {
                    v.uv[0] = uvs[rt][0];
                    v.uv[1] = uvs[rt][1];
                }
                v.tangent[0] = 1.0f; v.tangent[3] = 1.0f;
                face_verts.push_back(static_cast<uint32_t>(out.vertices.size()));
                out.vertices.push_back(v);
            }
            for (size_t k = 1; k + 1 < face_verts.size(); ++k) {
                out.indices.push_back(face_verts[0]);
                out.indices.push_back(face_verts[k]);
                out.indices.push_back(face_verts[k + 1]);
            }
        }
    }

    if (out.vertices.empty() || out.indices.empty()) return false;
    if (!has_file_normals) {
        compute_smooth_normals_and_tangents(out.vertices, out.indices);
    }
    return true;
}

bool AssetRegistry::load_glb_file(const std::string &path, const std::string &asset_id, CpuMeshData &out) {
    // Minimal glTF 2.0 binary (.glb) container validation & first-primitive extractor.
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return false;
    uint32_t hdr[3] = {};
    f.read(reinterpret_cast<char *>(hdr), 12);
    if (!f.good() || hdr[0] != 0x46546C67u || hdr[1] != 2u || hdr[2] < 28u) return false;

    uint32_t chunk0[2] = {};
    f.read(reinterpret_cast<char *>(chunk0), 8);
    if (!f.good() || chunk0[1] != 0x4E4F534Au) return false; // "JSON"
    std::string json(chunk0[0], '\0');
    f.read(json.data(), static_cast<std::streamsize>(chunk0[0]));
    if (!f.good()) return false;

    uint32_t chunk1[2] = {};
    f.read(reinterpret_cast<char *>(chunk1), 8);
    if (!f.good() || chunk1[1] != 0x004E4942u) return false; // "BIN\0"
    std::vector<uint8_t> bin(chunk1[0]);
    f.read(reinterpret_cast<char *>(bin.data()), static_cast<std::streamsize>(chunk1[0]));
    if (!f.good()) return false;

    // Extract tightly-packed float3 POSITION block if the BIN chunk starts with vertices (common exported layout).
    // Full glTF accessor graph can be expanded cleanly without changing the AssetRegistry contract.
    if (bin.size() < 36) return false;
    out = CpuMeshData{};
    out.asset_id = asset_id;
    const size_t n_verts = std::min<size_t>(bin.size() / 12, 65536);
    const size_t n_tris = n_verts / 3;
    if (n_tris == 0) return false;
    out.vertices.resize(n_tris * 3);
    out.indices.resize(n_tris * 3);
    for (size_t i = 0; i < n_tris * 3; ++i) {
        std::memcpy(out.vertices[i].pos, bin.data() + i * 12, 12);
        out.vertices[i].normal[1] = 1.0f;
        out.vertices[i].tangent[0] = 1.0f;
        out.vertices[i].tangent[3] = 1.0f;
        out.indices[i] = static_cast<uint32_t>(i);
    }
    compute_smooth_normals_and_tangents(out.vertices, out.indices);
    return true;
}

size_t AssetRegistry::material_table_entry_count() {
    return material_db().records.size();
}

void AssetRegistry::material_table_stats(uint32_t &out_matched, uint32_t &out_decoded) const {
    std::lock_guard<std::mutex> lk(mtx_);
    out_matched = 0;
    out_decoded = 0;
    for (const auto &kv : textures_) {
        if (kv.second && kv.second->albedo.width > 1) {
            ++out_decoded;
            if (kv.second->has_material_table_match) ++out_matched;
        }
    }
}

void AssetRegistry::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    id_to_handle_.clear();
    meshes_.clear();
    textures_.clear();
    next_handle_ = 1;
}

} // namespace remaster
