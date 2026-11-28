// runtime/remaster/graf_loader.cpp
// Self-contained GRAF + 4SPS geometry loader for the Remaster engine.
// ZERO #include of runtime/gs/*, runtime/vu/*, or vif_mesh.h.
#include "graf_loader.h"
#include "asset_registry.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace remaster {

namespace {

constexpr uint32_t kNodeGroup  = 0x000;
constexpr uint32_t kNodeLeaf   = 0x004;
constexpr uint32_t kNodeCondA  = 0x101;
constexpr uint32_t kNodeSphere = 0x102;
constexpr uint32_t kNodeCondB  = 0x103;

constexpr uint32_t kSubtypeSimpleModel     = 0;
constexpr uint32_t kSubtypePointFacing     = 1;
constexpr uint32_t kSubtypeAxisFacing      = 2;
constexpr uint32_t kSubtypeSky             = 4;
constexpr uint32_t kSubtypeMultiFacing     = 5;
constexpr uint32_t kSubtypeOldFlag         = 6;
constexpr uint32_t kSubtypeOptionalTerrain = 20;
constexpr uint32_t kSubtypeOldBanner       = 21;
constexpr uint32_t kSubtypeLeaderBoard     = 24;
constexpr uint32_t kSubtypeCrossFader      = 25;

// Only static world-space GRAF display subtypes are emitted by graf_loader.
// Dynamic / animated / unspawned / marker subtypes (Dyno=3, Rotator=7, Crowd/People=8/10/11,
// Sun/Env=9/14..19, Animation/Pit-Arm=12/13, Paths=22/30, Linear/Hermite Drones=23/31,
// FlareSource=26, Fireworks/Effect=27/29, Water=28, Fabric=32, FacingTest=33) are either
// positioned & submitted dynamically at runtime via 0x34BDA8 or non-visual markers.
inline bool is_static_graf_subtype(uint32_t subtype) {
    switch (subtype) {
    case kSubtypeSimpleModel:
    case kSubtypePointFacing:
    case kSubtypeAxisFacing:
    case kSubtypeSky:
    case kSubtypeMultiFacing:
    case kSubtypeOldFlag:
    case kSubtypeOptionalTerrain:
    case kSubtypeOldBanner:
    case kSubtypeLeaderBoard:
    case kSubtypeCrossFader:
        return true;
    default:
        return false;
    }
}

inline bool in_bounds(uint32_t addr, uint32_t bytes, uint32_t ram_size) {
    return addr <= ram_size && bytes <= ram_size - addr;
}

inline uint32_t rd_u32(const uint8_t *ram, uint32_t addr) {
    uint32_t v = 0;
    std::memcpy(&v, ram + addr, 4);
    return v;
}

inline int32_t rd_s32(const uint8_t *ram, uint32_t addr) {
    int32_t v = 0;
    std::memcpy(&v, ram + addr, 4);
    return v;
}

inline float rd_f32(const uint8_t *ram, uint32_t addr) {
    float v = 0.0f;
    std::memcpy(&v, ram + addr, 4);
    return v;
}

inline int16_t rd_s16(const uint8_t *ram, uint32_t addr) {
    int16_t v = 0;
    std::memcpy(&v, ram + addr, 2);
    return v;
}

inline uint16_t rd_u16(const uint8_t *ram, uint32_t addr) {
    uint16_t v = 0;
    std::memcpy(&v, ram + addr, 2);
    return v;
}

inline std::string to_lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline bool contains_any(const std::string &haystack, std::initializer_list<const char *> needles) {
    for (const char *n : needles) {
        if (haystack.find(n) != std::string::npos) return true;
    }
    return false;
}

// Quantized spatial key for welding coincident vertices across strip boundaries.
struct WeldKey {
    int32_t x, y, z;
    bool operator==(const WeldKey &o) const { return x == o.x && y == o.y && z == o.z; }
};
struct WeldKeyHash {
    size_t operator()(const WeldKey &k) const noexcept {
        uint64_t h = static_cast<uint64_t>(static_cast<uint32_t>(k.x)) * 73856093ull ^
                     static_cast<uint64_t>(static_cast<uint32_t>(k.y)) * 19349663ull ^
                     static_cast<uint64_t>(static_cast<uint32_t>(k.z)) * 83492791ull;
        return static_cast<size_t>(h ^ (h >> 33));
    }
};

// Lightweight binary extractor for the 4SPS geometry payload embedded in GRAF/mrModel.
// Extracts clean float3 positions, retail unit normals (for winding orientation), float2 UVs,
// and @150 skin/dent weights — strips all baked PS2 vertex colors.
struct RawTriVertex {
    float pos[3];
    float nrm[3];
    float uv[2];
    VertexSkin skin;
    bool adc;
};

inline float bits_to_f32(uint32_t u) {
    float f = 0.0f;
    std::memcpy(&f, &u, 4);
    return f;
}

// Parse 4SPS binary geometry payload (starts at part_addr + 0x38, ends at part_addr + part_size).
// Matches tools/vif_mesh.py:24-39, 150-191, 227-260 exactly (self-contained, zero GS/VU/vif_mesh includes).
void decode_4sps_geometry(const uint8_t *ram, uint32_t ram_size, uint32_t part_addr,
                          uint32_t part_size, CpuMeshData &mesh) {
    if (part_size <= 0x38 || !in_bounds(part_addr, part_size, ram_size)) return;
    if (std::memcmp(ram + part_addr, "4SPS", 4) != 0) return;

    constexpr uint32_t kSlots = 1024;
    uint32_t slot_data[kSlots][4] = {};
    uint32_t raw_data[kSlots][4] = {};
    uint8_t slot_valid[kSlots] = {};
    uint8_t raw_valid[kSlots] = {};
    uint8_t slot_vl[kSlots] = {};
    uint32_t row_reg[4] = {};
    uint32_t col_reg[4] = {};
    uint32_t cl = 1, wl = 1, mode = 0, mask = 0;
    int32_t last_mscal = -1;
    bool has_any_skin = false;

    uint32_t cursor = part_addr + 0x38;
    const uint32_t end = part_addr + part_size;

    auto flush_batch = [&]() {
        if (!slot_valid[0]) return;
        // Skip shadow-volume parts (MSCAL 4, stride 2) so they never corrupt visible geometry.
        if (last_mscal == 4) return;

        const uint32_t nverts = slot_data[0][0] & 0x7FFFu;
        if (nverts < 3 || nverts > 256) return;

        float soff = 0.0f, toff = 0.0f;
        if (slot_valid[198]) {
            const float su = bits_to_f32(slot_data[198][0]);
            const float sv = bits_to_f32(slot_data[198][1]);
            if (std::isfinite(su)) soff = su;
            if (std::isfinite(sv)) toff = sv;
        }
        float offset[3] = {0.0f, 0.0f, 0.0f};
        if (slot_valid[199]) {
            for (int k = 0; k < 3; ++k) {
                const float f = bits_to_f32(slot_data[199][k]);
                offset[k] = std::isfinite(f) ? f : 0.0f;
            }
        }

        uint8_t batch_skin_mode = 0;
        if (last_mscal == 0x5C || last_mscal == 0x1A9 || last_mscal == 0x2B5) batch_skin_mode = 1; // car dent offsets (VU1 0x2E0/0xD48/0x15A8)
        else if (last_mscal == 0x63 || last_mscal == 0x496) batch_skin_mode = 2; // driver/ped bone matrices

        std::vector<RawTriVertex> strip;
        strip.reserve(nverts);
        for (uint32_t i = 0; i < nverts; ++i) {
            const uint32_t q_nrm = 1u + 3u * i;
            const uint32_t q_pos = 2u + 3u * i;
            const uint32_t q_uv  = 3u + 3u * i;
            // VU1 microcode reads skin quadwords in reverse order: TOP + 149 + nverts - i.
            const uint32_t q_skn = 149u + nverts - i;
            if (q_pos >= kSlots || !slot_valid[q_pos]) break;

            RawTriVertex v{};
            const float px = bits_to_f32(slot_data[q_pos][0]);
            const float py = bits_to_f32(slot_data[q_pos][1]);
            const float pz = bits_to_f32(slot_data[q_pos][2]);
            v.pos[0] = (std::isfinite(px) ? px : 0.0f) + offset[0];
            v.pos[1] = (std::isfinite(py) ? py : 0.0f) + offset[1];
            v.pos[2] = (std::isfinite(pz) ? pz : 0.0f) + offset[2];

            bool has_real_nrm = false;
            if (q_nrm < kSlots && (raw_valid[q_nrm] & 1u)) {
                // Raw ADC is in bit 7 (0x80) of the raw w-lane BEFORE STROW adds 0x7F80 (both V4-8 and V4-5).
                v.adc = (raw_data[q_nrm][3] & 0x80u) != 0;
                if (slot_vl[q_nrm] == 2u) {
                    // V4-8: genuine packed unit normal
                    v.nrm[0] = (static_cast<float>(raw_data[q_nrm][0] & 0xFFu) - 128.0f) / 127.0f;
                    v.nrm[1] = (static_cast<float>(raw_data[q_nrm][1] & 0xFFu) - 128.0f) / 127.0f;
                    v.nrm[2] = (static_cast<float>(raw_data[q_nrm][2] & 0xFFu) - 128.0f) / 127.0f;
                    has_real_nrm = true;
                } else {
                    // V4-5: prelit PS2 world vertex color (stripped); default normal +Y UP
                    v.nrm[0] = 0.0f;
                    v.nrm[1] = 1.0f;
                    v.nrm[2] = 0.0f;
                }
            } else {
                v.nrm[0] = 0.0f;
                v.nrm[1] = 1.0f; // +Y is UP in EOD world
                v.nrm[2] = 0.0f;
                v.adc = false;
            }
            (void)has_real_nrm;

            if (q_uv < kSlots && slot_valid[q_uv]) {
                const float u = bits_to_f32(slot_data[q_uv][0]);
                const float w = bits_to_f32(slot_data[q_uv][1]);
                v.uv[0] = (std::isfinite(u) ? u : 0.0f) + soff;
                v.uv[1] = (std::isfinite(w) ? w : 0.0f) + toff;
            }

            if (batch_skin_mode != 0 && q_skn < kSlots && slot_valid[q_skn]) {
                v.skin.mode = batch_skin_mode;
                v.skin.bone[0] = static_cast<uint16_t>(slot_data[q_skn][0] & 0xFFFFu);
                v.skin.bone[1] = static_cast<uint16_t>(slot_data[q_skn][1] & 0xFFFFu);
                // VU1 skin weights are IEEE-754 floats biased by 32768.0f (STROW 0x47000000 + raw).
                const float wa = bits_to_f32(slot_data[q_skn][2]) - 32768.0f;
                const float wb = bits_to_f32(slot_data[q_skn][3]) - 32768.0f;
                if (std::isfinite(wa) && std::isfinite(wb) &&
                    std::fabs(wa) <= 4.0f && std::fabs(wb) <= 4.0f) {
                    v.skin.weight[0] = wa;
                    v.skin.weight[1] = wb;
                } else {
                    v.skin.weight[0] = static_cast<float>(raw_data[q_skn][2] & 0xFFu) / 256.0f;
                    v.skin.weight[1] = static_cast<float>(raw_data[q_skn][3] & 0xFFu) / 256.0f;
                }
                const float pw = bits_to_f32(slot_data[q_pos][3]);
                v.skin.pos_w = (std::isfinite(pw) && std::fabs(pw) <= 16.0f) ? pw : 1.0f;
                has_any_skin = true;
            }
            strip.push_back(v);
        }
        if (strip.size() < 3) return;

        const uint32_t base_v = static_cast<uint32_t>(mesh.vertices.size());
        for (const auto &sv : strip) {
            PbrVertex pv{};
            pv.pos[0] = sv.pos[0];
            pv.pos[1] = sv.pos[1];
            pv.pos[2] = sv.pos[2];
            pv.normal[0] = sv.nrm[0];
            pv.normal[1] = sv.nrm[1];
            pv.normal[2] = sv.nrm[2];
            pv.tangent[0] = 1.0f;
            pv.tangent[1] = 0.0f;
            pv.tangent[2] = 0.0f;
            pv.tangent[3] = 1.0f;
            pv.uv[0] = sv.uv[0];
            pv.uv[1] = sv.uv[1];
            mesh.vertices.push_back(pv);
            mesh.skin.push_back(sv.skin);
        }

        uint32_t run_start = 0;
        for (uint32_t i = 0; i < strip.size(); ++i) {
            if (strip[i].adc && (i == 0 || !strip[i - 1].adc)) run_start = i;
            if (i >= 2 && !strip[i].adc) {
                uint32_t a = i - 2, b = i - 1, c = i;
                if (((i - run_start) & 1u) != 0) std::swap(a, b);
                // Drop degenerate triangles
                const auto &pa = strip[a].pos, &pb = strip[b].pos, &pc = strip[c].pos;
                const float e1x = pb[0] - pa[0], e1y = pb[1] - pa[1], e1z = pb[2] - pa[2];
                const float e2x = pc[0] - pa[0], e2y = pc[1] - pa[1], e2z = pc[2] - pa[2];
                const float gx = e1y * e2z - e1z * e2y;
                const float gy = e1z * e2x - e1x * e2z;
                const float gz = e1x * e2y - e1y * e2x;
                if (gx * gx + gy * gy + gz * gz < 1e-12f) continue;

                // Orient triangle winding to agree with stored vertex normals (matches tools/vif_mesh.py:fix_winding)
                const float vnx = strip[a].nrm[0] + strip[b].nrm[0] + strip[c].nrm[0];
                const float vny = strip[a].nrm[1] + strip[b].nrm[1] + strip[c].nrm[1];
                const float vnz = strip[a].nrm[2] + strip[b].nrm[2] + strip[c].nrm[2];
                if (gx * vnx + gy * vny + gz * vnz < 0.0f) {
                    std::swap(b, c);
                }
                mesh.indices.push_back(base_v + a);
                mesh.indices.push_back(base_v + b);
                mesh.indices.push_back(base_v + c);
            }
        }
    };

    while (cursor + 4 <= end) {
        const uint32_t code = rd_u32(ram, cursor);
        cursor += 4;
        const uint32_t cmd = (code >> 24) & 0x7Fu;
        const uint32_t num = (code >> 16) & 0xFFu;
        const uint32_t imm = code & 0xFFFFu;

        if (cmd == 0x00 || cmd == 0x02 || cmd == 0x03 || cmd == 0x04 ||
            cmd == 0x06 || cmd == 0x07 || cmd == 0x10 || cmd == 0x11 || cmd == 0x13) {
            continue;
        } else if (cmd == 0x01) { // STCYCL
            cl = imm & 0xFFu;
            wl = (imm >> 8) & 0xFFu;
            if (wl == 0) wl = 1;
            if (cl == 0) cl = 1;
        } else if (cmd == 0x05) { // STMOD
            mode = imm & 3u;
        } else if (cmd == 0x20) { // STMASK
            if (cursor + 4 > end) break;
            mask = rd_u32(ram, cursor);
            cursor += 4;
        } else if (cmd == 0x30) { // STROW
            if (cursor + 16 > end) break;
            for (int k = 0; k < 4; ++k) row_reg[k] = rd_u32(ram, cursor + k * 4);
            cursor += 16;
        } else if (cmd == 0x31) { // STCOL
            if (cursor + 16 > end) break;
            for (int k = 0; k < 4; ++k) col_reg[k] = rd_u32(ram, cursor + k * 4);
            cursor += 16;
        } else if (cmd == 0x14 || cmd == 0x15 || cmd == 0x17) { // MSCAL / MSCALF / MSCNT
            if (cmd != 0x17) last_mscal = static_cast<int32_t>(imm);
            flush_batch();
            std::memset(slot_valid, 0, sizeof(slot_valid));
            std::memset(raw_valid, 0, sizeof(raw_valid));
            std::memset(slot_vl, 0, sizeof(slot_vl));
            std::memset(slot_data, 0, sizeof(slot_data));
            std::memset(raw_data, 0, sizeof(raw_data));
        } else if (cmd == 0x4A) { // MPG
            const uint32_t n = num ? num : 256u;
            cursor += n * 8u;
        } else if (cmd == 0x50 || cmd == 0x51) { // DIRECT / DIRECTHL
            const uint32_t qwc = imm ? imm : 65536u;
            cursor += qwc * 16u;
        } else if (cmd >= 0x60) { // UNPACK data block
            const uint32_t vn = (cmd >> 2) & 3u;
            const uint32_t vl = cmd & 3u;
            const bool masked = ((cmd >> 4) & 1u) != 0;
            const uint32_t comps = vn + 1u;
            const uint32_t n = num ? num : 256u;
            const uint32_t bpe = (vl == 3u) ? 2u : ((vl == 0u ? 4u : (vl == 1u ? 2u : 1u)) * comps);
            const uint32_t byte_len = (n * bpe + 3u) & ~3u;
            const uint32_t base_addr = imm & 0x3FFu;
            const bool usn = ((imm >> 14) & 1u) != 0;
            if (cursor + byte_len > end) break;

            uint32_t data_pos = cursor;
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t e[4] = {0, 0, 0, 0};
                uint32_t e_len = comps;
                if (vl == 3u) { // V4-5 (16-bit packed)
                    const uint16_t pk = rd_u16(ram, data_pos);
                    data_pos += 2;
                    e[0] = (pk & 0x1Fu) << 3;
                    e[1] = ((pk >> 5) & 0x1Fu) << 3;
                    e[2] = ((pk >> 10) & 0x1Fu) << 3;
                    e[3] = ((pk >> 15) & 1u) << 7;
                    e_len = 4;
                } else {
                    for (uint32_t c = 0; c < comps; ++c) {
                        if (vl == 0u) {
                            e[c] = rd_u32(ram, data_pos);
                            data_pos += 4;
                        } else if (vl == 1u) {
                            const uint16_t h = rd_u16(ram, data_pos);
                            data_pos += 2;
                            e[c] = usn ? static_cast<uint32_t>(h)
                                       : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(h)));
                        } else {
                            const uint8_t b = ram[data_pos++];
                            e[c] = usn ? static_cast<uint32_t>(b)
                                       : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(b)));
                        }
                    }
                }
                const uint32_t dst = (wl <= cl) ? (base_addr + (i / wl) * cl + (i % wl)) : (base_addr + i);
                const uint32_t cyc = (wl <= cl) ? (i % wl) : i;
                if (dst < kSlots) {
                    const uint32_t cyc_clamped = std::min(cyc, 3u);
                    for (uint32_t c = 0; c < 4u; ++c) {
                        uint32_t m = masked ? ((mask >> (cyc_clamped * 8u + c * 2u)) & 3u) : 0u;
                        if (c >= e_len) {
                            if (masked) m = 1u;
                            else continue; // Do NOT overwrite un-supplied lanes when unmasked
                        }
                        if (m == 0u) {
                            uint32_t v = e[c];
                            raw_data[dst][c] = v;
                            raw_valid[dst] |= static_cast<uint8_t>(1u << c);
                            if (mode == 1u) {
                                v = v + row_reg[c];
                            } else if (mode == 2u) {
                                v = v + row_reg[c];
                                row_reg[c] = v;
                            }
                            slot_data[dst][c] = v;
                        } else if (m == 1u) {
                            slot_data[dst][c] = row_reg[c];
                        } else if (m == 2u) {
                            slot_data[dst][c] = col_reg[cyc_clamped];
                        }
                        // m == 3: write-protect (leave slot_data[dst][c] unchanged)
                    }
                    slot_valid[dst] = 1;
                    slot_vl[dst] = static_cast<uint8_t>(vl);
                }
            }
            cursor += byte_len;
        } else {
            break;
        }
    }
    if (!has_any_skin) mesh.skin.clear();
}

void compute_bounds(CpuMeshData &mesh) {
    if (mesh.vertices.empty()) return;
    float bmin[3] = {mesh.vertices[0].pos[0], mesh.vertices[0].pos[1], mesh.vertices[0].pos[2]};
    float bmax[3] = {bmin[0], bmin[1], bmin[2]};
    for (const auto &v : mesh.vertices) {
        for (int k = 0; k < 3; ++k) {
            bmin[k] = std::min(bmin[k], v.pos[k]);
            bmax[k] = std::max(bmax[k], v.pos[k]);
        }
    }
    for (int k = 0; k < 3; ++k) {
        mesh.bounds_min[k] = bmin[k];
        mesh.bounds_max[k] = bmax[k];
        mesh.center[k] = 0.5f * (bmin[k] + bmax[k]);
    }
    float max_r2 = 0.0f;
    for (const auto &v : mesh.vertices) {
        const float dx = v.pos[0] - mesh.center[0];
        const float dy = v.pos[1] - mesh.center[1];
        const float dz = v.pos[2] - mesh.center[2];
        max_r2 = std::max(max_r2, dx * dx + dy * dy + dz * dz);
    }
    mesh.radius = std::sqrt(max_r2);
}

} // namespace

PbrMaterial classify_pbr_material(const std::string &texture_name, bool is_sky, bool is_vehicle) {
    PbrMaterial m{};
    const std::string low = to_lower(texture_name);

    if (is_sky || contains_any(low, {"sky", "cloud", "horizon", "ho-"})) {
        m.mat_class = MaterialClass::Sky;
        m.roughness = 1.0f;
        m.metallic = 0.0f;
        m.is_sky = true;
        return m;
    }

    if (contains_any(low, {"carshad", "shopshad", "-shad", "_shad", "shadow.tex", "shad.tex"}) ||
        low == "shadow") {
        m.mat_class = MaterialClass::Default;
        m.roughness = 0.92f;
        m.metallic = 0.0f;
        m.is_fake_shadow = true;
        return m;
    }

    if (!is_vehicle) {
        // ====================================================================
        // STATIC WORLD GEOMETRY: STRICTLY MATTE BY DEFAULT (roughness = 0.92f).
        // Terrain/dirt/grass/asphalt/walls/structures are checked FIRST so they
        // can NEVER accidentally match vehicle/detail/light/car/metal keywords.
        // ONLY explicitly matched Glass, Water, or Chrome may have rough < 0.85!
        // ====================================================================
        if (contains_any(low, {"tree", "trew", "leaf", "leaves", "bush", "brush", "foliage", "shrub", "weed", "reed",
                               "fence", "chain", "hedge", "w_mesh", "w_chain"})) {
            m.mat_class = MaterialClass::Foliage;
            m.roughness = 0.90f;
            m.metallic = 0.0f;
            m.alpha_cutout = true;
            m.double_sided = true;
            return m;
        }

        if (contains_any(low, {"dirt", "drt", "mud", "gravel", "grav", "grass", "grs", "sand", "soil",
                               "hill", "terrain", "ground", "hay", "bale", "trkwear", "ramp", "mound",
                               "infield", "field", "turf", "sod", "earth", "clay", "lawn", "oval"})) {
            m.mat_class = MaterialClass::TerrainDirt;
            m.roughness = 0.92f;
            m.metallic = 0.0f;
            return m;
        }

        if (contains_any(low, {"road", "asph", "tarmac", "pave", "track", "strip", "croad", "stfinlin"})) {
            m.mat_class = MaterialClass::Asphalt;
            m.roughness = 0.88f;
            m.metallic = 0.0f;
            return m;
        }

        if (contains_any(low, {"wall", "conc", "cement", "brick", "wood", "plywd", "barn", "stand",
                               "bleach", "roof", "bldg", "barrier", "jersey", "post", "pole", "tower",
                               "bridge", "brdg", "sign", "billb", "banner", "crate", "tire", "tyre"})) {
            m.mat_class = MaterialClass::BuildingConcrete;
            m.roughness = 0.90f;
            m.metallic = 0.0f;
            return m;
        }

        // Additive light glows, bridge string lights, flares, stars, and stadium spotlight beam cones
        if (contains_any(low, {"flare", "glow", "beam", "cone", "lcone", "halo", "star", "glare", "corona",
                               "bulb", "lamp", "lite", "light", "neon", "spot", "flood", "shaft"})) {
            m.mat_class = MaterialClass::Emissive;
            m.roughness = 0.92f;
            m.metallic = 0.0f;
            m.alpha_additive = true;
            m.double_sided = true;
            m.emissive_scale = contains_any(low, {"beam", "cone", "lcone", "spot", "flood", "shaft", "glow", "halo"})
                                   ? 0.42f
                                   : 0.65f;
            return m;
        }

        if (contains_any(low, {"water", "h2o", "puddle", "pond", "lake", "river"})) {
            m.mat_class = MaterialClass::Water;
            m.roughness = 0.10f;
            m.metallic = 0.0f;
            return m;
        }

        if (contains_any(low, {"glass", "window", "hwindows", "win-"})) {
            m.mat_class = MaterialClass::Glass;
            m.roughness = 0.08f;
            m.metallic = 0.0f;
            return m;
        }

        if (contains_any(low, {"chrome"})) {
            m.mat_class = MaterialClass::MetalChrome;
            m.roughness = 0.18f;
            m.metallic = 0.90f;
            return m;
        }

        // Strictly matte default for ALL unmatched static world geometry
        m.mat_class = MaterialClass::Default;
        m.roughness = 0.92f;
        m.metallic = 0.0f;
        return m;
    }

    // ========================================================================
    // VEHICLES & DYNAMIC CHARACTERS (is_vehicle == true)
    // ========================================================================
    if (contains_any(low, {"glass", "wind", "windshield", "window", "hwindows", "win-", "siren", "taxilite"})) {
        m.mat_class = MaterialClass::Glass;
        m.roughness = 0.06f;
        m.metallic = 0.0f;
        return m;
    }

    if (contains_any(low, {"light", "lamp", "glow", "neon", "headl", "taill", "brake", "flare", "lite"})) {
        m.mat_class = MaterialClass::Emissive;
        m.roughness = 0.14f;
        m.metallic = 0.0f;
        m.emissive_scale = 2.0f;
        return m;
    }

    if (contains_any(low, {"chrome", "bumper", "mirror", "rim", "wheel_m", "steel", "grille",
                           "c_detail", "exdetail", "rollbar", "exhaust", "pipe"})) {
        m.mat_class = MaterialClass::MetalChrome;
        m.roughness = 0.16f;
        m.metallic = 0.90f;
        return m;
    }

    if (contains_any(low, {"engine", "chassis", "detail"})) {
        m.mat_class = MaterialClass::MetalChrome;
        m.roughness = 0.25f;
        m.metallic = 0.85f;
        return m;
    }

    if (contains_any(low, {"tire", "tyre", "tread", "rubber", "bustire", "comtire", "mustire"})) {
        m.mat_class = MaterialClass::Rubber;
        m.roughness = 0.88f;
        m.metallic = 0.0f;
        return m;
    }

    if (contains_any(low, {"intr", "bintr", "interior", "cockpit", "dash", "dpanel", "seat",
                           "drvr_anm", "g_driver", "driver"})) {
        m.mat_class = MaterialClass::Interior;
        m.roughness = 0.88f;
        m.metallic = 0.0f;
        return m;
    }

    if (contains_any(low, {"drvr_hlm", "helmet"})) {
        m.mat_class = MaterialClass::Default;
        m.roughness = 0.22f;
        m.metallic = 0.0f;
        return m;
    }

    if (contains_any(low, {"rust", "burn", "junker", "bbl_burn"})) {
        m.mat_class = MaterialClass::Default;
        m.roughness = 0.80f;
        m.metallic = 0.25f;
        return m;
    }

    // Demolition derby car paint: worn enamel catches subtle grazing sky/track reflections
    m.mat_class = MaterialClass::CarPaint;
    m.roughness = 0.60f;
    m.metallic = 0.12f;
    return m;
}

void compute_smooth_normals_and_tangents(std::vector<PbrVertex> &verts,
                                         const std::vector<uint32_t> &indices,
                                         float weld_eps) {
    if (verts.empty() || indices.size() < 3) return;
    const float inv_eps = weld_eps > 1e-6f ? (1.0f / weld_eps) : 1000.0f;

    // 1. Accumulate area-weighted face normals per welded spatial position
    std::unordered_map<WeldKey, std::array<float, 3>, WeldKeyHash> welded_n;
    welded_n.reserve(verts.size());

    std::vector<std::array<float, 3>> tan1(verts.size(), {0.0f, 0.0f, 0.0f});
    std::vector<std::array<float, 3>> tan2(verts.size(), {0.0f, 0.0f, 0.0f});

    auto key_of = [&](const PbrVertex &v) -> WeldKey {
        return {static_cast<int32_t>(std::lround(v.pos[0] * inv_eps)),
                static_cast<int32_t>(std::lround(v.pos[1] * inv_eps)),
                static_cast<int32_t>(std::lround(v.pos[2] * inv_eps))};
    };

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
        if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size()) continue;
        const PbrVertex &v0 = verts[i0];
        const PbrVertex &v1 = verts[i1];
        const PbrVertex &v2 = verts[i2];

        const float e1x = v1.pos[0] - v0.pos[0], e1y = v1.pos[1] - v0.pos[1], e1z = v1.pos[2] - v0.pos[2];
        const float e2x = v2.pos[0] - v0.pos[0], e2y = v2.pos[1] - v0.pos[1], e2z = v2.pos[2] - v0.pos[2];

        // Area-weighted unnormalized cross product
        const float nx = e1y * e2z - e1z * e2y;
        const float ny = e1z * e2x - e1x * e2z;
        const float nz = e1x * e2y - e1y * e2x;

        for (uint32_t idx : {i0, i1, i2}) {
            auto &acc = welded_n[key_of(verts[idx])];
            acc[0] += nx;
            acc[1] += ny;
            acc[2] += nz;
        }

        // Tangent / bitangent accumulation
        const float du1 = v1.uv[0] - v0.uv[0], dv1 = v1.uv[1] - v0.uv[1];
        const float du2 = v2.uv[0] - v0.uv[0], dv2 = v2.uv[1] - v0.uv[1];
        const float det = du1 * dv2 - du2 * dv1;
        if (std::fabs(det) > 1e-8f) {
            const float r = 1.0f / det;
            const float tx = (e1x * dv2 - e2x * dv1) * r;
            const float ty = (e1y * dv2 - e2y * dv1) * r;
            const float tz = (e1z * dv2 - e2z * dv1) * r;
            const float bx = (e2x * du1 - e1x * du2) * r;
            const float by = (e2y * du1 - e1y * du2) * r;
            const float bz = (e2z * du1 - e1z * du2) * r;
            for (uint32_t idx : {i0, i1, i2}) {
                tan1[idx][0] += tx; tan1[idx][1] += ty; tan1[idx][2] += tz;
                tan2[idx][0] += bx; tan2[idx][1] += by; tan2[idx][2] += bz;
            }
        }
    }

    // 2. Normalize smooth normals and Gram-Schmidt orthogonalize tangents
    for (size_t i = 0; i < verts.size(); ++i) {
        auto it = welded_n.find(key_of(verts[i]));
        float nx = 0.0f, ny = 1.0f, nz = 0.0f;
        if (it != welded_n.end()) {
            const float len = std::sqrt(it->second[0] * it->second[0] +
                                        it->second[1] * it->second[1] +
                                        it->second[2] * it->second[2]);
            if (len > 1e-6f) {
                nx = it->second[0] / len;
                ny = it->second[1] / len;
                nz = it->second[2] / len;
            }
        }
        verts[i].normal[0] = nx;
        verts[i].normal[1] = ny;
        verts[i].normal[2] = nz;

        const float tx = tan1[i][0], ty = tan1[i][1], tz = tan1[i][2];
        const float ndt = nx * tx + ny * ty + nz * tz;
        float ox = tx - nx * ndt;
        float oy = ty - ny * ndt;
        float oz = tz - nz * ndt;
        const float tlen = std::sqrt(ox * ox + oy * oy + oz * oz);
        if (tlen > 1e-6f) {
            ox /= tlen; oy /= tlen; oz /= tlen;
            const float cx = ny * oz - nz * oy;
            const float cy = nz * ox - nx * oz;
            const float cz = nx * oy - ny * ox;
            const float sign = (cx * tan2[i][0] + cy * tan2[i][1] + cz * tan2[i][2]) < 0.0f ? -1.0f : 1.0f;
            verts[i].tangent[0] = ox;
            verts[i].tangent[1] = oy;
            verts[i].tangent[2] = oz;
            verts[i].tangent[3] = sign;
        } else {
            // Construct arbitrary orthonormal tangent when UVs are degenerate
            const float ux = std::fabs(ny) < 0.9f ? 0.0f : 1.0f;
            const float uy = std::fabs(ny) < 0.9f ? 1.0f : 0.0f;
            float ax = uy * nz, ay = -ux * nz, az = ux * ny - uy * nx;
            const float alen = std::sqrt(ax * ax + ay * ay + az * az);
            if (alen > 1e-6f) { ax /= alen; ay /= alen; az /= alen; }
            else { ax = 1.0f; ay = 0.0f; az = 0.0f; }
            verts[i].tangent[0] = ax;
            verts[i].tangent[1] = ay;
            verts[i].tangent[2] = az;
            verts[i].tangent[3] = 1.0f;
        }
    }
}

bool decode_mrmodel_from_ram(const uint8_t *ram, uint32_t ram_size, uint32_t model_addr,
                             const std::string &asset_prefix, bool is_vehicle,
                             std::vector<CpuMeshData> &out_parts) {
    if (!ram || !model_addr || !in_bounds(model_addr, 0x40, ram_size)) return false;

    const uint32_t n_parts = rd_u32(ram, model_addr + 0x00);
    const uint32_t part_tbl_field = rd_u32(ram, model_addr + 0x04);
    if (n_parts == 0 || n_parts > 4096) return false;

    // After EE fixup (f_0034B530), partTable may be a relocated guest pointer (>= model_addr)
    // or a model-relative offset (< ram_size). Support both seamlessly.
    const uint32_t part_tbl = (part_tbl_field >= model_addr && part_tbl_field < ram_size)
                                  ? part_tbl_field
                                  : (model_addr + part_tbl_field);
    if (!in_bounds(part_tbl, n_parts * 4, ram_size)) return false;

    for (uint32_t i = 0; i < n_parts; ++i) {
        const uint32_t p_field = rd_u32(ram, part_tbl + i * 4);
        if (!p_field) continue;
        const uint32_t part_addr = (p_field >= model_addr && p_field < ram_size)
                                       ? p_field
                                       : (model_addr + p_field);
        if (!in_bounds(part_addr, 0x38, ram_size)) continue;
        if (std::memcmp(ram + part_addr, "4SPS", 4) != 0) continue;

        const uint32_t part_size = rd_u32(ram, part_addr + 0x04);
        char tex_buf[17] = {};
        std::memcpy(tex_buf, ram + part_addr + 0x08, 16);
        const std::string tex_name(tex_buf);

        CpuMeshData mesh;
        mesh.part_index = i;
        mesh.layer_index = rd_s16(ram, part_addr + 0x18);
        mesh.texture_id = "tex/" + tex_name;
        mesh.asset_id = asset_prefix + "/part_" + std::to_string(i) + "_" + tex_name;
        mesh.material = classify_pbr_material(tex_name, false, is_vehicle);

        decode_4sps_geometry(ram, ram_size, part_addr, part_size, mesh);
        if (mesh.vertices.empty() || mesh.indices.empty()) continue;

        compute_smooth_normals_and_tangents(mesh.vertices, mesh.indices);
        compute_bounds(mesh);
        out_parts.push_back(std::move(mesh));
    }
    return !out_parts.empty();
}

bool load_world_from_graf(const uint8_t *ram, uint32_t ram_size, uint32_t graf_addr,
                          const std::string &track_name, LoadedWorld &out) {
    out = LoadedWorld{};
    out.track_name = track_name;
    out.graf_guest_addr = graf_addr;
    if (!ram || !graf_addr || !in_bounds(graf_addr, 12, ram_size)) return false;

    const uint32_t root_field = rd_u32(ram, graf_addr);
    if (!root_field) return false;
    const uint32_t root = (root_field >= graf_addr && root_field < ram_size) ? root_field : (graf_addr + root_field);

    std::unordered_set<uint32_t> visited;
    uint32_t leaf_counter = 0;

    auto resolve_ptr = [&](uint32_t base, uint32_t field) -> uint32_t {
        if (!field) return 0;
        if (field >= graf_addr && field < ram_size) return field;
        return base + field;
    };

    // Per SLUS_209.10:0x214468 (init_display_list), condition nodes 0x101/0x103 test
    // *(u32*)0x003D1B34: when non-zero, include child iff value != -500; else iff value == -500 || value >= 0.
    const bool cond_flag = in_bounds(0x003D1B34u, 4, ram_size) && (rd_u32(ram, 0x003D1B34u) != 0);

    auto walk = [&](auto &&self, uint32_t node_addr) -> void {
        while (node_addr && in_bounds(node_addr, 12, ram_size)) {
            if (!visited.insert(node_addr).second) break;
            const uint32_t type = rd_u32(ram, node_addr + 0x00);
            const uint32_t next = resolve_ptr(graf_addr, rd_u32(ram, node_addr + 0x04));

            if (type == kNodeGroup) {
                if (in_bounds(node_addr, 0x18, ram_size)) {
                    const uint32_t count = rd_u32(ram, node_addr + 0x14);
                    // Child 0 is the nearest / highest-detail LOD level (per docs/WORLD_FORMAT.md).
                    if (count > 0 && count < 64 && in_bounds(node_addr + 0x18, count * 8, ram_size)) {
                        const uint32_t child = resolve_ptr(graf_addr, rd_u32(ram, node_addr + 0x1C));
                        if (child) self(self, child);
                    }
                }
            } else if (type == kNodeSphere) {
                const uint32_t child = resolve_ptr(graf_addr, rd_u32(ram, node_addr + 0x08));
                if (child) self(self, child);
            } else if (type == kNodeCondA || type == kNodeCondB) {
                const uint32_t val_off = (type == kNodeCondA) ? 0x0Cu : 0x1Cu;
                bool include = true;
                if (in_bounds(node_addr, val_off + 4u, ram_size)) {
                    const int32_t val = rd_s32(ram, node_addr + val_off);
                    include = cond_flag ? (val != -500) : (val == -500 || val >= 0);
                }
                if (include) {
                    const uint32_t child = resolve_ptr(graf_addr, rd_u32(ram, node_addr + 0x08));
                    if (child) self(self, child);
                }
            } else if (type == kNodeLeaf) {
                const uint32_t rec = resolve_ptr(graf_addr, rd_u32(ram, node_addr + 0x08));
                if (rec && in_bounds(rec, 0x30, ram_size)) {
                    const uint32_t kind = rd_u32(ram, rec + 0x00);
                    const uint32_t model_field = rd_u32(ram, rec + 0x08);
                    const uint32_t subtype = (kind == 1) ? rd_u32(ram, rec + 0x10) : 0;
                    // Skip dynamic/animated/unspawned/marker display objects (e.g. Linear Drone semi-trucks,
                    // unspawned Dynos at origin, invisible path/flare/effect markers).
                    if (kind == 1 && !is_static_graf_subtype(subtype)) {
                        node_addr = next;
                        continue;
                    }
                    const bool is_sky = (kind == 1 && subtype == kSubtypeSky);
                    const bool is_opt = (kind == 1 && subtype == kSubtypeOptionalTerrain);
                    uint32_t opt_mask = 0;
                    if (is_opt) {
                        const int32_t mode_val = rd_s32(ram, rec + 0x2C);
                        // After fixup (f_002230F8), mode is already bitmask `1 << (mode - 1)`.
                        opt_mask = static_cast<uint32_t>(mode_val);
                    }

                    // Skip non-drawable records with no embedded model
                    if (model_field != 0) {
                        const uint32_t model_addr = resolve_ptr(rec, model_field);
                        if (is_sky) out.sky_model_addrs.insert(model_addr);
                        std::vector<CpuMeshData> parts;
                        const std::string prefix = "track/" + track_name + "/leaf_" + std::to_string(leaf_counter++);
                        if (decode_mrmodel_from_ram(ram, ram_size, model_addr, prefix, false, parts)) {
                            for (auto &part : parts) {
                                if (is_sky) {
                                    part.material = classify_pbr_material(part.texture_id, true, false);
                                }
                                StaticMeshInstance inst{};
                                inst.asset_id = part.asset_id;
                                inst.texture_id = part.texture_id;
                                inst.center[0] = part.center[0];
                                inst.center[1] = part.center[1];
                                inst.center[2] = part.center[2];
                                inst.radius = part.radius;
                                inst.optional_terrain_mask = opt_mask;
                                inst.is_optional_terrain = is_opt;
                                inst.is_sky = part.material.is_sky;
                                inst.is_foliage = part.material.alpha_cutout;
                                inst.cast_shadow = !inst.is_sky && !part.material.alpha_additive &&
                                                   (part.material.mat_class != MaterialClass::Water);
                                inst.mesh_handle = AssetRegistry::instance().register_mesh(part.asset_id, part);
                                out.meshes.push_back(std::move(part));
                                out.instances.push_back(std::move(inst));
                            }
                        }
                    }
                }
            }
            node_addr = next;
        }
    };

    walk(walk, root);
    return !out.instances.empty();
}

} // namespace remaster
