// vif_mesh_test.cpp - decodes every model in the retail data with runtime/render/vif_mesh and
// compares it with a tools/vif_mesh.py reference dump.
//
//   vif_mesh_test <DISC/DATA> <ref.bin> [--verbose]
//
// ref.bin is written by the reference dumper (see docs/VIF_MESH_DECODER.md, "Test"): one record
// per part, keyed "<archive rel path>|<member>|modl|<slot>" or "...|mr@<hex model offset>|<index>".
// Positions, UVs, normals (colour bytes for colour batches) and ADC flags are compared bitwise,
// triangle lists exactly; skin words are compared with the Python's raw qword-150 lanes after
// the VU's reverse pairing. Exit code 0 only when nothing differs.
//
// Build (standalone):
//   c++ -std=c++17 -O2 -I runtime tools/tests/vif_mesh_test.cpp runtime/render/vif_mesh.cpp -o vif_mesh_test.exe
#include "render/vif_mesh.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace vifmesh;

namespace {

uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
uint32_t fbits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

bool read_file(const fs::path &p, std::vector<uint8_t> &out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    out.resize((size_t)f.tellg());
    f.seekg(0);
    f.read((char *)out.data(), (std::streamsize)out.size());
    return (bool)f;
}

struct Member {
    std::string archive, name, type;
    std::vector<uint8_t> data;
};

// 0TSR v6 archive directory (tools/modding_backend.py directory()): header 40 bytes, directory
// at +20 (offset, count), 68-byte entries {name[32], fourcc reversed[4], .., +40 length,
// position (relative to the directory), crc}.
bool archive_members(const fs::path &path, const std::string &rel, std::vector<Member> &out) {
    std::vector<uint8_t> d;
    if (!read_file(path, d) || d.size() < 40 || memcmp(d.data(), "0TSR", 4) || rd32(&d[8]) != 6) return false;
    const uint32_t off = rd32(&d[20]), count = rd32(&d[24]);
    if (off < 40 || (uint64_t)off + count * 68ull > d.size()) return false;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = &d[off + 68 * i];
        std::string name((const char *)e, strnlen((const char *)e, 32));
        std::string type;
        for (int k = 3; k >= 0; k--)
            if (e[32 + k] != ' ' && e[32 + k]) type += (char)e[32 + k];
        const uint32_t len = rd32(e + 40), pos = rd32(e + 44);
        if (type != "MODL" && type != "GRAF") continue;
        if ((uint64_t)off + pos + len > d.size()) return false;
        Member m{rel, name, type, std::vector<uint8_t>(d.begin() + off + pos, d.begin() + off + pos + len)};
        out.push_back(std::move(m));
    }
    return true;
}

// track.grf display list (docs/WORLD_FORMAT.md): unique model offsets of all leaves.
bool graf_models(const std::vector<uint8_t> &d, std::vector<uint32_t> &models) {
    if (d.size() < 12) return false;
    std::set<uint32_t> seen_nodes, seen_models;
    std::vector<uint32_t> stack{rd32(&d[0])};
    while (!stack.empty()) {
        uint32_t n = stack.back();
        stack.pop_back();
        while (n) {
            if (n % 4 || n + 8 > d.size() || !seen_nodes.insert(n).second) return false;
            const uint32_t t = rd32(&d[n]);
            if (t == 0) {
                const uint32_t count = rd32(&d[n + 0x14]);
                if (count > 4096 || n + 0x1C + 8ull * count > d.size()) return false;
                for (uint32_t i = 0; i < count; i++) stack.push_back(rd32(&d[n + 0x1C + 8 * i]));
            } else if (t == 0x101 || t == 0x102 || t == 0x103) {
                stack.push_back(rd32(&d[n + 8]));
            } else if (t == 4) {
                const uint32_t m = rd32(&d[n + 8]);
                if (m && m + 0x18 <= d.size()) {
                    const uint32_t mo = m + rd32(&d[m + 8]);
                    if (seen_models.insert(mo).second) models.push_back(mo);
                }
            } else {
                return false;
            }
            n = rd32(&d[n + 4]);
        }
    }
    return true;
}

// ---- reference records (tools/vif_mesh.py)
struct RefPart {
    uint32_t status;
    char tex[17];
    uint32_t group;
    std::vector<std::pair<uint32_t, uint32_t>> batches;  // mscal, nverts
    const uint8_t *verts;  // nv x 46 bytes
    uint32_t nv;
    const uint32_t *tris;  // 3 x nt (unaligned-safe copy below)
    uint32_t nt;
};
constexpr size_t kRefVert = 8 * 4 + 2 + 16;

bool load_ref(const std::vector<uint8_t> &r, std::unordered_map<std::string, RefPart> &out) {
    size_t p = 0;
    auto need = [&](size_t n) { return p + n <= r.size(); };
    while (p < r.size()) {
        if (!need(8) || memcmp(&r[p], "PREF", 4)) return false;
        const uint32_t kl = rd32(&r[p + 4]);
        p += 8;
        if (!need(kl + 4 + 16 + 8)) return false;
        std::string key((const char *)&r[p], kl);
        p += kl;
        RefPart rp = {};
        rp.status = rd32(&r[p]);
        memcpy(rp.tex, &r[p + 4], 16);
        rp.tex[16] = 0;
        rp.group = rd32(&r[p + 20]);
        const uint32_t nb = rd32(&r[p + 24]);
        p += 28;
        if (!need(8ull * nb + 4)) return false;
        for (uint32_t i = 0; i < nb; i++) rp.batches.push_back({rd32(&r[p + 8 * i]), rd32(&r[p + 8 * i + 4])});
        p += 8ull * nb;
        rp.nv = rd32(&r[p]);
        p += 4;
        if (!need(kRefVert * rp.nv + 4)) return false;
        rp.verts = &r[p];
        p += kRefVert * rp.nv;
        rp.nt = rd32(&r[p]);
        p += 4;
        if (!need(12ull * rp.nt)) return false;
        rp.tris = (const uint32_t *)&r[p];
        p += 12ull * rp.nt;
        out.emplace(std::move(key), rp);
    }
    return true;
}

struct Stats {
    uint64_t archives = 0, members = 0, models = 0, parts = 0, batches = 0, verts = 0, tris = 0;
    uint64_t part_errors = 0, missing_ref = 0, unvisited_ref = 0;
    uint64_t mism_parts = 0, mism_pos = 0, mism_uv = 0, mism_nrm = 0, mism_col = 0, mism_adc = 0, mism_tri = 0,
             mism_hdr = 0;
    uint64_t v_normal = 0, v_color = 0, v_none = 0;
    uint64_t skin_cmp = 0, skin_match = 0, skin_mismatch = 0, skin_py_only = 0, skin_overlap = 0;
    std::map<std::string, uint64_t> skin_kinds;
    std::map<uint32_t, uint64_t> wsum;  // weight bytes a+b
    uint64_t matrix_bone_not4 = 0, matrix_verts = 0, offset_verts = 0;
    uint32_t bone_min = 0xFFFF, bone_max = 0, mbone_min = 0xFFFF, mbone_max = 0;
    uint64_t order_vu_better = 0, order_fwd_better = 0, order_tie = 0;
};

double spread(const Vertex *v, uint32_t n, bool vu_order, const Vertex *auxsrc) {
    // mean distance to the centroid of the vertices sharing the same skin word, pairing vertex i
    // with the word of vertex (vu_order ? i : n-1-i) as stored (auxsrc already holds VU order)
    std::map<uint32_t, std::vector<uint32_t>> g;
    for (uint32_t i = 0; i < n; i++) {
        const Vertex &s = auxsrc[vu_order ? i : n - 1 - i];
        g[(uint32_t)s.aux[0] | s.aux[1] << 8 | s.aux[2] << 16].push_back(i);
    }
    double tot = 0;
    uint64_t cnt = 0;
    for (auto &kv : g) {
        if (kv.second.size() < 2) continue;
        double c[3] = {0, 0, 0};
        for (uint32_t i : kv.second)
            for (int k = 0; k < 3; k++) c[k] += v[i].pos[k];
        for (int k = 0; k < 3; k++) c[k] /= kv.second.size();
        for (uint32_t i : kv.second)
            tot += std::sqrt((v[i].pos[0] - c[0]) * (v[i].pos[0] - c[0]) + (v[i].pos[1] - c[1]) * (v[i].pos[1] - c[1]) +
                             (v[i].pos[2] - c[2]) * (v[i].pos[2] - c[2]));
        cnt += kv.second.size();
    }
    return cnt ? tot / cnt : 0;
}

void compare_part(const std::string &key, const Mesh &m, const Part &pt, const RefPart *rp, Stats &S, bool verbose) {
    S.parts++;
    S.batches += pt.batch_count;
    S.verts += pt.vertex_count;
    S.tris += pt.index_count / 3;
    if (pt.status != Status::Ok) {
        S.part_errors++;
        printf("ERROR %s: %s\n", key.c_str(), status_name(pt.status));
    }
    // skin statistics
    for (uint32_t bi = 0; bi < pt.batch_count; bi++) {
        const Batch &b = m.batches[pt.first_batch + bi];
        const Vertex *bv = &m.vertices[b.first_vertex];
        static const char *names[] = {"none", "offset(0x2e0)", "matrix(0x318)", "partial(0x2e0 V1-8)"};
        if (b.aux_format) {
            char k[64];
            snprintf(k, sizeof k, "prog %#x aux cmd %#x skin %s", b.program * 8, b.aux_format, names[(int)b.skin]);
            S.skin_kinds[k] += b.vertex_count;
        }
        if (b.skin == Skin::Offset || b.skin == Skin::Matrix) {
            for (uint32_t i = 0; i < b.vertex_count; i++) {
                const Vertex &v = bv[i];
                if (!(v.flags & kHasSkin)) continue;
                S.wsum[v.aux[2] + v.aux[3]]++;
                if (b.skin == Skin::Matrix) {
                    S.matrix_verts++;
                    for (int j = 0; j < 2; j++) {
                        if (v.bone[j] % 4) S.matrix_bone_not4++;
                        S.mbone_min = std::min<uint32_t>(S.mbone_min, v.bone[j]);
                        S.mbone_max = std::max<uint32_t>(S.mbone_max, v.bone[j]);
                    }
                } else {
                    S.offset_verts++;
                    for (int j = 0; j < 2; j++) {
                        S.bone_min = std::min<uint32_t>(S.bone_min, v.bone[j]);
                        S.bone_max = std::max<uint32_t>(S.bone_max, v.bone[j]);
                    }
                }
            }
            if (b.vertex_count >= 4) {
                const double a = spread(bv, b.vertex_count, true, bv), f = spread(bv, b.vertex_count, false, bv);
                if (a < f - 1e-9) S.order_vu_better++;
                else if (f < a - 1e-9) S.order_fwd_better++;
                else S.order_tie++;
            }
        }
    }
    if (!rp) {
        S.missing_ref++;
        printf("NOREF %s\n", key.c_str());
        return;
    }
    bool bad = false;
    auto report = [&](const char *what, uint32_t i) {
        if (verbose || !bad) printf("DIFF %s: %s at vertex/tri %u\n", key.c_str(), what, i);
        bad = true;
    };
    if (strncmp(rp->tex, pt.texture, 16) || rp->group != pt.group || rp->batches.size() != pt.batch_count ||
        rp->nv != pt.vertex_count) {
        S.mism_hdr++;
        report("header (texture/group/batch or vertex count)", 0);
    } else {
        for (uint32_t bi = 0; bi < pt.batch_count; bi++) {
            const Batch &b = m.batches[pt.first_batch + bi];
            if (rp->batches[bi].first != b.program || rp->batches[bi].second != b.vertex_count) {
                S.mism_hdr++;
                report("batch program/count", bi);
            }
        }
        uint32_t vi = 0;
        for (uint32_t bi = 0; bi < pt.batch_count; bi++) {
            const Batch &b = m.batches[pt.first_batch + bi];
            for (uint32_t k = 0; k < b.vertex_count; k++, vi++) {
                const Vertex &v = m.vertices[pt.first_vertex + vi];
                const uint8_t *r = rp->verts + kRefVert * vi;
                float rf[8];
                memcpy(rf, r, 32);
                const uint8_t radc = r[32], rmask = r[33];
                uint32_t rsk[4];
                memcpy(rsk, r + 34, 16);
                for (int c = 0; c < 3; c++)
                    if (fbits(rf[c]) != fbits(v.pos[c])) { S.mism_pos++; report("position", vi); break; }
                for (int c = 0; c < 2; c++)
                    if (fbits(rf[3 + c]) != fbits(v.uv[c])) { S.mism_uv++; report("uv", vi); break; }
                if (v.flags & kHasNormal) {
                    S.v_normal++;
                    for (int c = 0; c < 3; c++)
                        if (fbits(rf[5 + c]) != fbits(v.normal[c])) { S.mism_nrm++; report("normal", vi); break; }
                } else if (v.flags & kHasColor) {
                    S.v_color++;  // vif_mesh.py reads the colour lanes as a normal: (c - 128) / 127
                    for (int c = 0; c < 3; c++)
                        if (fbits(rf[5 + c]) != fbits((float)((double)((int)v.color[c] - 128) / 127.0))) {
                            S.mism_col++;
                            report("colour", vi);
                            break;
                        }
                } else {
                    S.v_none++;
                    if (rf[5] != 0.0f || rf[6] != 1.0f || rf[7] != 0.0f) { S.mism_nrm++; report("default normal", vi); }
                }
                if ((radc != 0) != ((v.flags & kAdc) != 0)) { S.mism_adc++; report("adc", vi); }
                (void)rmask;
                (void)rsk;
            }
        }
        // skin words: python vertex j carries raw qword 150+j; the VU pairs vertex k with 150+n-1-k
        vi = 0;
        for (uint32_t bi = 0; bi < pt.batch_count; bi++) {
            const Batch &b = m.batches[pt.first_batch + bi];
            const uint32_t n = b.vertex_count;
            // python reads raw qword 150+j for every vertex j: vertex qwords (3n+1 >= 150) or the
            // offset qwords 198/199 (n >= 49) then show up as "skin"
            const bool overlap = 3 * b.declared + 1 >= 150 || b.declared + 150 > 198;
            for (uint32_t k = 0; k < n; k++) {
                const Vertex &v = m.vertices[pt.first_vertex + vi + k];
                if (!(v.flags & kHasAux)) {
                    if (rp->verts[kRefVert * (vi + k) + 33]) {
                        if (overlap) S.skin_overlap++;
                        else S.skin_py_only++;
                    }
                    continue;
                }
                if (n != b.declared) continue;
                const uint8_t *r = rp->verts + kRefVert * (vi + (n - 1 - k));
                const uint8_t rmask = r[33];
                uint32_t rsk[4];
                memcpy(rsk, r + 34, 16);
                S.skin_cmp++;
                bool ok = true;
                for (int c = 0; c < 4; c++) {
                    const uint32_t mine = v.aux[c];
                    if ((rmask >> c & 1) ? (rsk[c] & 0xFF) != mine || rsk[c] > 0xFF : mine != 0) ok = false;
                }
                if (ok) S.skin_match++;
                else {
                    S.skin_mismatch++;
                    report("skin word", vi + k);
                }
            }
            vi += n;
        }
        const uint32_t nt = pt.index_count / 3;
        if (nt != rp->nt) {
            S.mism_tri++;
            report("triangle count", nt);
        } else {
            for (uint32_t t = 0; t < 3 * nt; t++) {
                uint32_t rv;
                memcpy(&rv, (const uint8_t *)rp->tris + 4 * t, 4);
                if (rv != m.indices[pt.first_index + t]) {
                    S.mism_tri++;
                    report("triangle index", t / 3);
                    break;
                }
            }
        }
    }
    if (bad) S.mism_parts++;
}

struct MemCtx {
    const uint8_t *base;
    size_t size;
};
bool mem_read(void *ctx, uint64_t addr, void *dst, size_t n) {
    const MemCtx *c = (const MemCtx *)ctx;
    if (addr > c->size || n > c->size - addr) return false;
    memcpy(dst, c->base + addr, n);
    return true;
}

bool shadow_extrusion_faces() {
    const uint32_t stream[] = {
        0x6001c000, 0x8003,           // shadow strip header
        0x01000102,                  // STCYCL 1:2, alternating normals and positions
        0x20000000, 0x40404040,       // STMASK, position W comes from STROW
        0x30000000, 0, 0, 0, 0x3f800000,
        0x7803c002, 0, 0, 0, 0, 0, 0, 0x3f800000, 0, 0,
        0x30000000, 0x47800000, 0x47800000, 0x47800000, 0,
        0x05000001,                  // STMOD adds the normal encoding bias
        0x6e03c001, 0x80808000, 0x808080ff, 0x00808080,  // distinct normals at the shared position
        0x14000004,                  // MSCAL 4
    };
    Decoder decoder;
    Mesh mesh;
    auto decode = [&](const Options &options) {
        mesh.clear();
        return decoder.decode_vif(reinterpret_cast<const uint8_t *>(stream), sizeof stream,
                                  mesh, options) == Status::Ok &&
               mesh.vertices.size() == 3 && mesh.batches.size() == 1 && mesh.batches[0].shadow &&
               mesh.vertices[0].normal[0] < 0 && mesh.vertices[1].normal[0] > 0;
    };
    Options options;
    if (!decode(options) || !mesh.indices.empty()) return false;
    options.keep_shadow_extrusion_faces = true;
    return decode(options) && mesh.indices == std::vector<uint32_t>{0, 1, 2};
}

bool camera_facing_faces() {
    uint32_t stream[] = {
        0x6001c000, 0x8004,                // four vertices at the same uploaded position
        0x01000103,                        // STCYCL 1:3, normal/position/UV
        0x6804c002, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0x6e04c001, 0x80808080, 0x80808080, 0x00808080, 0x00808080,
        0x01000101,
        0x6e04c096, 0x00800020, 0x0080001f, 0x0080001e, 0x0080001d, // reversed V4-8 corners
        0x1400006a,
    };
    Decoder decoder;
    Mesh mesh;
    Options options;
    auto decode = [&] {
        mesh.clear();
        return decoder.decode_vif(reinterpret_cast<const uint8_t *>(stream), sizeof stream,
                                  mesh, options) == Status::Ok && mesh.vertices.size() == 4;
    };
    if (!decode() || !mesh.indices.empty()) return false;
    options.keep_camera_facing_faces = true;
    if (!decode() || mesh.indices != std::vector<uint32_t>{0, 1, 2, 2, 1, 3}) return false;
    for (uint32_t i = 0; i < 4; i++) {
        if (!(mesh.vertices[i].flags & kHasAux) || mesh.vertices[i].aux[0] != 29 + i) return false;
    }
    stream[sizeof stream / sizeof *stream - 1] = 0x1400004e;
    return decode() && mesh.indices.empty();  // static geometry still filters equal positions
}

}  // namespace

int main(int argc, char **argv) {
    if (!shadow_extrusion_faces()) {
        fprintf(stderr, "shadow side face was lost or retained in the wrong mode\n");
        return 1;
    }
    if (!camera_facing_faces()) {
        fprintf(stderr, "camera-facing quad indices do not survive corner expansion\n");
        return 1;
    }
    if (argc == 2 && (!strcmp(argv[1], "--shadow-extrusion") || !strcmp(argv[1], "--camera-facing"))) return 0;
    if (argc < 3) {
        fprintf(stderr, "usage: %s <DISC/DATA> <ref.bin> [--verbose] | --shadow-extrusion\n", argv[0]);
        return 2;
    }
    const fs::path data_dir = argv[1];
    const bool verbose = argc > 3 && !strcmp(argv[3], "--verbose");
    std::vector<uint8_t> refbuf;
    std::unordered_map<std::string, RefPart> ref;
    if (!read_file(argv[2], refbuf) || !load_ref(refbuf, ref)) {
        fprintf(stderr, "cannot read reference %s\n", argv[2]);
        return 2;
    }

    // load every MODL/GRAF member
    std::vector<fs::path> archives;
    for (auto &e : fs::recursive_directory_iterator(data_dir)) {
        if (!e.is_regular_file()) continue;
        std::string ext = e.path().extension().string();
        for (char &c : ext) c = (char)toupper((unsigned char)c);
        if (ext == ".CAR" || ext == ".RES" || ext == ".TRK") archives.push_back(e.path());
    }
    std::sort(archives.begin(), archives.end());
    Stats S;
    std::vector<Member> members;
    for (auto &a : archives) {
        std::string rel = fs::relative(a, data_dir).generic_string();
        if (!archive_members(a, rel, members)) printf("ERROR archive %s unreadable\n", rel.c_str());
        S.archives++;
    }
    S.members = members.size();
    std::vector<std::vector<uint32_t>> graf(members.size());
    for (size_t i = 0; i < members.size(); i++)
        if (members[i].type == "GRAF" && !graf_models(members[i].data, graf[i]))
            printf("ERROR %s|%s: display list\n", members[i].archive.c_str(), members[i].name.c_str());

    // timed decode of everything (no comparison), best of 3; one Mesh per member, reused
    Decoder dec;
    Mesh mesh;
    Options opt;
    double best = 1e30;
    uint64_t tverts = 0;
    for (int rep = 0; rep < 3; rep++) {
        tverts = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < members.size(); i++) {
            mesh.clear();
            const Member &mb = members[i];
            if (mb.type == "MODL") dec.decode_modl(mb.data.data(), mb.data.size(), mesh, opt);
            else
                for (uint32_t mo : graf[i]) dec.decode_mr_model(mb.data.data(), mb.data.size(), mo, mesh, opt);
            tverts += mesh.vertices.size();
        }
        best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }

    // decode again and compare part by part
    std::set<std::string> visited;
    for (size_t i = 0; i < members.size(); i++) {
        const Member &mb = members[i];
        auto run = [&](const std::string &label) {
            for (const Part &pt : mesh.parts) {
                const std::string key = mb.archive + "|" + mb.name + "|" + label + "|" + std::to_string(pt.slot);
                auto it = ref.find(key);
                visited.insert(key);
                compare_part(key, mesh, pt, it == ref.end() ? nullptr : &it->second, S, verbose);
            }
        };
        if (mb.type == "MODL") {
            mesh.clear();
            if (mb.data.size() >= 4 && !memcmp(mb.data.data(), "MODL", 4)) continue;
            const Status st = dec.decode_modl(mb.data.data(), mb.data.size(), mesh, opt);
            if (st == Status::BadHeader) printf("ERROR %s|%s: header\n", mb.archive.c_str(), mb.name.c_str());
            S.models++;
            run("modl");
        } else {
            for (uint32_t mo : graf[i]) {
                mesh.clear();
                const Status st = dec.decode_mr_model(mb.data.data(), mb.data.size(), mo, mesh, opt);
                if (st == Status::BadHeader) printf("ERROR %s|%s@%x: header\n", mb.archive.c_str(), mb.name.c_str(), mo);
                S.models++;
                char lab[32];
                snprintf(lab, sizeof lab, "mr@%x", mo);
                run(lab);
            }
        }
    }
    for (auto &kv : ref)
        if (!visited.count(kv.first)) {
            S.unvisited_ref++;
            printf("UNVISITED %s\n", kv.first.c_str());
        }

    // guest-reader path: every part of the first car model through the callback must equal the
    // buffer decode
    uint64_t guest_parts = 0, guest_diff = 0;
    for (const Member &mb : members) {
        if (mb.type != "MODL" || mb.archive.rfind("CAR/", 0) != 0) continue;
        Mesh a, b;
        dec.decode_modl(mb.data.data(), mb.data.size(), a, opt);
        MemCtx ctx{mb.data.data(), mb.data.size()};
        for (const Part &pt : a.parts) {
            dec.decode_part_guest(mem_read, &ctx, pt.source_offset, b, opt, pt.slot);
            guest_parts++;
        }
        if (a.vertices.size() != b.vertices.size() || a.indices != b.indices ||
            memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(Vertex)))
            guest_diff++;
        if (guest_parts > 200) break;
    }

    printf("\narchives %llu, MODL/GRAF members %llu, models %llu, parts %llu, batches %llu, vertices %llu, "
           "triangles %llu\n",
           (unsigned long long)S.archives, (unsigned long long)S.members, (unsigned long long)S.models,
           (unsigned long long)S.parts, (unsigned long long)S.batches, (unsigned long long)S.verts,
           (unsigned long long)S.tris);
    printf("decode all: %.1f ms best of 3 (%llu vertices, %.1f M vertices/s)\n", best * 1e3,
           (unsigned long long)tverts, tverts / best / 1e6);
    printf("reference parts %zu, missing in reference %llu, reference parts not decoded %llu, decoder errors %llu\n",
           ref.size(), (unsigned long long)S.missing_ref, (unsigned long long)S.unvisited_ref,
           (unsigned long long)S.part_errors);
    printf("mismatching parts %llu (header %llu, position %llu, uv %llu, normal %llu, colour %llu, adc %llu, "
           "triangles %llu)\n",
           (unsigned long long)S.mism_parts, (unsigned long long)S.mism_hdr, (unsigned long long)S.mism_pos,
           (unsigned long long)S.mism_uv, (unsigned long long)S.mism_nrm, (unsigned long long)S.mism_col,
           (unsigned long long)S.mism_adc, (unsigned long long)S.mism_tri);
    printf("vertex attributes: normal %llu, colour %llu, none %llu\n", (unsigned long long)S.v_normal,
           (unsigned long long)S.v_color, (unsigned long long)S.v_none);
    printf("skin words (VU reverse pairing vs python raw lanes): compared %llu, equal %llu, differ %llu; "
           "python-only %llu (+%llu where python reads vertex qwords as skin)\n",
           (unsigned long long)S.skin_cmp, (unsigned long long)S.skin_match, (unsigned long long)S.skin_mismatch,
           (unsigned long long)S.skin_py_only, (unsigned long long)S.skin_overlap);
    for (auto &kv : S.skin_kinds) printf("  qword-150 data: %s: %llu vertices\n", kv.first.c_str(), (unsigned long long)kv.second);
    printf("offset skin: %llu vertices, bone field %u..%u; matrix skin: %llu vertices, bone field %u..%u, "
           "not a multiple of 4: %llu\n",
           (unsigned long long)S.offset_verts, S.bone_min, S.bone_max, (unsigned long long)S.matrix_verts,
           S.mbone_min, S.mbone_max, (unsigned long long)S.matrix_bone_not4);
    printf("weight byte sums:");
    for (auto &kv : S.wsum)
        if (kv.second * 1000 >= S.offset_verts + S.matrix_verts) printf(" %u:%llu", kv.first, (unsigned long long)kv.second);
    uint64_t other = 0;
    for (auto &kv : S.wsum)
        if (kv.second * 1000 < S.offset_verts + S.matrix_verts) other += kv.second;
    printf(" other:%llu\n", (unsigned long long)other);
    printf("skin pairing check (same-word vertex clusters tighter): VU order %llu, forward order %llu, tie %llu "
           "batches\n",
           (unsigned long long)S.order_vu_better, (unsigned long long)S.order_fwd_better, (unsigned long long)S.order_tie);
    printf("guest-reader path: %llu parts, %llu models differ\n", (unsigned long long)guest_parts,
           (unsigned long long)guest_diff);
    const bool pass = !S.mism_parts && !S.missing_ref && !S.unvisited_ref && !S.part_errors && !guest_diff;
    printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
