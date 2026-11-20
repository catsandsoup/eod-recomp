// vif_mesh.cpp - see vif_mesh.h and docs/VIF_MESH_DECODER.md.
//
// The VIF interpreter follows tools/vif_mesh.py run_vif/decode_part step for step (including its
// treatment of masked lanes missing from the data and of qwords no UNPACK wrote) so the output is
// bit-identical; positions/UVs/normals are computed in double as the Python does and rounded to
// float once (f32+f32 and int/127 in double are exact or correctly rounded, so this equals the
// float result). Winding and degenerate tests use the unrounded doubles, again as the Python.

#include "vif_mesh.h"

#include <algorithm>
#include <cstring>

#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

namespace vifmesh {

const char *status_name(Status s) {
    switch (s) {
    case Status::Ok: return "ok";
    case Status::Truncated: return "truncated";
    case Status::UnknownCommand: return "unknown VIF command";
    case Status::FillMode: return "fill-mode STCYCL";
    case Status::BadAddress: return "UNPACK address out of range";
    case Status::BadHeader: return "bad header";
    case Status::NotPart: return "not a 4SPS part";
    case Status::ReadFailed: return "guest read failed";
    }
    return "?";
}

namespace {

constexpr uint32_t kQwords = 2048;  // emulated VU1 data memory (real: 1024; vif_mesh.py is unbounded)
constexpr uint32_t kEmpty = 0xE3E3E3E3u;
constexpr uint32_t kSkinBase = 150;  // per-vertex word block (UNPACK @150)
constexpr uint32_t kProgCar = 0x5C;      // MSCAL 0x5C -> VU1 0x2e0 (dent offsets)
constexpr uint32_t kProgDriver = 0x63;   // MSCAL 0x63 -> VU1 0x318 (bone matrices)
constexpr uint32_t kProgShadow = 4;      // MSCAL 4 -> VU1 0x20 (shadow source, 2-qword stride)

inline uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
inline float as_f32(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}

}  // namespace

struct Decoder::State {
    // VIF registers
    uint32_t cl = 1, wl = 1, mode = 0, mask = 0;
    uint32_t row[4] = {0, 0, 0, 0}, col[4] = {0, 0, 0, 0};
    int64_t last_prog = -1;
    // VU memory for the current batch. A qword exists when stamp == gen (vif_mesh.py's dict).
    uint32_t mem[kQwords][4];
    uint32_t raw[kQwords][4];     // unpacked value before STROW/mask (vif_mesh.py st.raw)
    uint8_t rawmask[kQwords];     // lanes of raw[] that were written (others are None)
    uint32_t stamp[kQwords];
    uint32_t gen = 1;
    // the UNPACK that wrote the per-vertex word block at 150 in this batch
    uint8_t aux_cmd = 0;
    uint32_t aux_n = 0;
    // per-batch scratch (capacity reused)
    std::vector<double> dpos, dnrm;

    State() { reset_part(); memset(stamp, 0, sizeof stamp); }
    void reset_part() {
        cl = wl = 1;
        mode = mask = 0;
        memset(row, 0, sizeof row);
        memset(col, 0, sizeof col);
        last_prog = -1;
        next_batch();
    }
    void next_batch() {
        if (++gen == 0) {  // wrapped: forget every stamp
            memset(stamp, 0, sizeof stamp);
            gen = 1;
        }
        aux_cmd = 0;
        aux_n = 0;
    }
    bool has(uint32_t a) const { return a < kQwords && stamp[a] == gen; }
};

Decoder::Decoder() : s_(new State()) {}
Decoder::~Decoder() { delete s_; }

namespace {

// Strip with ADC (tools/vif_mesh.py strip_triangles): vertex i >= 2 without ADC closes
// (i-2, i-1, i); the order alternates with the parity inside the strip, which restarts at the
// first ADC vertex of a run.
template <class Emit>
void strip_triangles(const Vertex *v, uint32_t n, Emit emit) {
    uint32_t start = 0;
    for (uint32_t i = 0; i < n; i++) {
        const bool adc = (v[i].flags & kAdc) != 0;
        if (adc && (i == 0 || !(v[i - 1].flags & kAdc))) start = i;
        if (i >= 2 && !adc) {
            if (((i - start) % 2) == 0) emit(i - 2, i - 1, i);
            else emit(i - 1, i - 2, i);
        }
    }
}

}  // namespace

// Emits the batch whose MSCAL was just seen (vif_mesh.py decode_part.on_batch + load_mesh).
struct BatchEmitter {
    static void run(Decoder::State &s, uint32_t program, Mesh &out, Part &part, const Options &opt) {
        if (!s.has(0)) return;
        const uint32_t count = s.mem[0][0] & 0x7FFF;
        static const uint32_t zero[4] = {0, 0, 0, 0};
        const uint32_t *q198 = s.has(198) ? s.mem[198] : zero;
        const uint32_t *q199 = s.has(199) ? s.mem[199] : zero;
        const double soff = as_f32(q198[0]), toff = as_f32(q198[1]);
        const double pox = as_f32(q199[0]), poy = as_f32(q199[1]), poz = as_f32(q199[2]);
        const bool shadow = program == kProgShadow;
        const uint32_t stride = shadow ? 2 : 3;

        Batch b = {};
        b.program = program;
        b.declared = count;
        b.first_vertex = (uint32_t)out.vertices.size();
        b.first_index = (uint32_t)out.indices.size();
        b.pos_offset[0] = (float)pox, b.pos_offset[1] = (float)poy, b.pos_offset[2] = (float)poz;
        b.uv_offset[0] = (float)soff, b.uv_offset[1] = (float)toff;
        b.q198_zw[0] = q198[2], b.q198_zw[1] = q198[3];
        b.shadow = shadow ? 1 : 0;
        b.aux_format = s.aux_cmd;
        const bool aux = !shadow && s.aux_cmd != 0;
        const bool aux_v4 = aux && (s.aux_cmd & 0xF) == 0xE;   // V4-8
        const bool aux_v1 = aux && (s.aux_cmd & 0xF) == 0x2;   // V1-8
        if (aux && program == kProgCar) b.skin = aux_v4 ? Skin::Offset : aux_v1 ? Skin::Partial : Skin::None;
        else if (aux_v4 && program == kProgDriver) b.skin = Skin::Matrix;

        s.dpos.clear();
        s.dnrm.clear();
        uint32_t nnormal = 0, ncolor = 0;
        uint32_t n = 0;
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t an = 1 + stride * i, ap = 2 + stride * i, at = 3 + stride * i;
            if (!s.has(ap)) break;
            Vertex v = {};
            const uint32_t *pq = s.mem[ap];
            const double px = (double)as_f32(pq[0]) + pox, py = (double)as_f32(pq[1]) + poy,
                         pz = (double)as_f32(pq[2]) + poz;
            v.pos[0] = (float)px, v.pos[1] = (float)py, v.pos[2] = (float)pz;
            v.pos_w = as_f32(pq[3]);  // the programs add qword 199 to xyz only
            if (!shadow && s.has(at)) {
                const uint32_t *tq = s.mem[at];
                v.uv[0] = (float)((double)as_f32(tq[0]) + soff);
                v.uv[1] = (float)((double)as_f32(tq[1]) + toff);
            }
            double pn[3] = {0.0, 1.0, 0.0};
            if (s.has(an) && (s.rawmask[an] & 1)) {
                const uint32_t *nr = s.raw[an];
                for (int c = 0; c < 3; c++) pn[c] = ((double)((int64_t)nr[c] - 128)) / 127.0;
                if ((s.rawmask[an] & 8) && (nr[3] & 0x80)) v.flags |= kAdc;
                if ((s.mem[an][0] & 0xFFFFFF00u) == 0x47800000u) {  // V4-8 with the normal STROW
                    for (int c = 0; c < 3; c++) v.normal[c] = (float)pn[c];
                    memset(v.color, 0x80, 4);
                    v.flags |= kHasNormal;
                    nnormal++;
                } else {  // colour lanes (V4-5: 5 bits << 3; V4-8: the byte)
                    for (int c = 0; c < 3; c++) v.color[c] = (uint8_t)nr[c];
                    v.color[3] = 0x80;
                    v.flags |= kHasColor;
                    ncolor++;
                }
            } else {
                v.normal[1] = 1.0f;
                memset(v.color, 0x80, 4);
                v.flags |= kNoAttr;
            }
            // per-vertex word: the VU reads TOP+149+count-k for vertex k (reverse order)
            if (aux) {
                const uint32_t e = count - 1 - i;
                const uint32_t aq = kSkinBase + e;
                if (e < s.aux_n && s.has(aq)) {
                    for (int c = 0; c < 4; c++) v.aux[c] = (s.rawmask[aq] >> c & 1) ? (uint8_t)s.raw[aq][c] : 0;
                    v.flags |= kHasAux;
                    if (b.skin == Skin::Offset || b.skin == Skin::Matrix) {
                        const uint32_t *w = s.mem[aq];
                        v.bone[0] = (uint16_t)(w[0] & 0xFFFF);
                        v.bone[1] = (uint16_t)(w[1] & 0xFFFF);
                        v.weight[0] = as_f32(w[2]) - 32768.0f;
                        v.weight[1] = as_f32(w[3]) - 32768.0f;
                        v.flags |= kHasSkin;
                    } else if (b.skin == Skin::Partial) {
                        // V1-8: the VIF writes the byte to all four lanes, each plus its STROW
                        // lane, so the program reads a full skin word (bone a = bone b)
                        const uint32_t *w = s.mem[aq];
                        v.bone[0] = (uint16_t)(w[0] & 0xFFFF);
                        v.bone[1] = (uint16_t)(w[1] & 0xFFFF);
                        v.weight[0] = as_f32(w[2]) - 32768.0f;
                        v.weight[1] = as_f32(w[3]) - 32768.0f;
                        v.flags |= kHasSkin;
                    }
                }
            }
            out.vertices.push_back(v);
            s.dpos.insert(s.dpos.end(), {px, py, pz});
            s.dnrm.insert(s.dnrm.end(), {pn[0], pn[1], pn[2]});
            n++;
        }
        b.vertex_count = n;
        b.attr = nnormal >= ncolor ? (nnormal ? Attr::Normal : Attr::None) : Attr::Color;

        // triangles (vif_mesh.py triangulate_batch, then load_mesh's degenerate filter)
        const Vertex *bv = out.vertices.data() + b.first_vertex;
        const double *P = s.dpos.data(), *N = s.dnrm.data();
        const uint32_t base = b.first_vertex - part.first_vertex;
        auto same = [&](uint32_t x, uint32_t y) {
            return P[3 * x] == P[3 * y] && P[3 * x + 1] == P[3 * y + 1] && P[3 * x + 2] == P[3 * y + 2];
        };
        auto push = [&](uint32_t a, uint32_t bb, uint32_t c) {
            if (opt.drop_degenerate && !(shadow && opt.keep_shadow_extrusion_faces) &&
                (same(a, bb) || same(bb, c) || same(a, c))) return;
            out.indices.push_back(a + base);
            out.indices.push_back(bb + base);
            out.indices.push_back(c + base);
        };
        if (shadow) {
            // continuous parity including ADC vertices; inward normals, never flipped
            for (uint32_t i = 2; i < n; i++) {
                if (bv[i].flags & kAdc) continue;
                if (i % 2 == 0) push(i - 2, i - 1, i);
                else push(i - 1, i - 2, i);
            }
        } else {
            strip_triangles(bv, n, [&](uint32_t a, uint32_t bb, uint32_t c) {
                if (opt.fix_winding) {
                    const double *pa = P + 3 * a, *pb = P + 3 * bb, *pc = P + 3 * c;
                    const double e1[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
                    const double e2[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
                    const double fn[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                          e1[0] * e2[1] - e1[1] * e2[0]};
                    double vn[3];
                    for (int k = 0; k < 3; k++) vn[k] = N[3 * a + k] + N[3 * bb + k] + N[3 * c + k];
                    if (fn[0] * vn[0] + fn[1] * vn[1] + fn[2] * vn[2] < 0) {
                        push(a, c, bb);
                        return;
                    }
                }
                push(a, bb, c);
            });
        }
        b.index_count = (uint32_t)out.indices.size() - b.first_index;
        out.batches.push_back(b);
    }
};

Status Decoder::decode_vif(const uint8_t *vif, size_t size, Mesh &out, const Options &opt, const char *texture,
                           uint32_t group, uint32_t slot, uint32_t source_offset) {
    State &s = *s_;
    s.reset_part();
    Part part = {};
    if (texture) {
        size_t k = 0;
        for (; k < 16 && texture[k]; k++) part.texture[k] = texture[k];
        part.texture[k] = 0;
    }
    part.group = group;
    part.slot = slot;
    part.source_offset = source_offset;
    part.first_batch = (uint32_t)out.batches.size();
    part.first_vertex = (uint32_t)out.vertices.size();
    part.first_index = (uint32_t)out.indices.size();

    Status st = Status::Ok;
    size_t p = 0;
    while (p + 4 <= size) {
        const uint32_t w = rd32(vif + p);
        const uint32_t cmd = (w >> 24) & 0x7F, num = (w >> 16) & 0xFF, imm = w & 0xFFFF;
        p += 4;
        if (cmd >= 0x60) {  // UNPACK
            const uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3;
            const bool masked = (cmd >> 4) & 1;
            const uint32_t n = num ? num : 256;
            const uint32_t comps = vl == 3 ? 4 : vn + 1;
            const uint32_t esize = vl == 3 ? 2 : (vl == 0 ? 4 : vl == 1 ? 2 : 1);
            const uint32_t bpe = vl == 3 ? 2 : esize * (vn + 1);
            const size_t nbytes = ((size_t)n * bpe + 3) & ~(size_t)3;
            const bool usn = (imm >> 14) & 1;
            const uint32_t addr = imm & 0x3FF;
            if (p + (size_t)n * bpe > size) { st = Status::Truncated; break; }
            if (s.wl > s.cl) { st = Status::FillMode; break; }
            if (s.wl == 0) { st = Status::FillMode; break; }
            const uint8_t *d = vif + p;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t e[4] = {0, 0, 0, 0};
                if (vl == 3) {
                    uint16_t v;
                    memcpy(&v, d + 2 * i, 2);
                    e[0] = (v & 0x1Fu) << 3, e[1] = ((v >> 5) & 0x1Fu) << 3, e[2] = ((v >> 10) & 0x1Fu) << 3,
                    e[3] = (uint32_t)(v >> 15) << 7;
                } else {
                    const uint8_t *q = d + (size_t)i * bpe;
                    for (uint32_t c = 0; c < comps; c++) {
                        if (esize == 4) e[c] = rd32(q + 4 * c);
                        else if (esize == 2) {
                            uint16_t h;
                            memcpy(&h, q + 2 * c, 2);
                            e[c] = usn ? h : (uint32_t)(int32_t)(int16_t)h;
                        } else {
                            e[c] = usn ? q[c] : (uint32_t)(int32_t)(int8_t)q[c];
                        }
                    }
                }
                const uint32_t a = addr + (i / s.wl) * s.cl + (i % s.wl);
                const uint32_t cyc = i % s.wl;
                if (a >= kQwords) { st = Status::BadAddress; break; }
                if (s.stamp[a] != s.gen) {
                    s.stamp[a] = s.gen;
                    memset(s.mem[a], 0, 16);
                    s.rawmask[a] = 0;
                }
                uint32_t *mq = s.mem[a];
                // The lanes the VIF writes beyond the element's components (runtime/vu/vif.cpp, as
                // the hardware): V1 (S) repeats x in every lane, V2 writes (x, y, x, y), V3 writes
                // w = 0; then STMOD / STMASK apply to all four lanes. raw[] keeps only the element's
                // own components (vif_mesh.py).
                uint32_t lane[4] = {e[0], e[1], e[2], e[3]};
                if (vl != 3) {
                    if (comps == 1) lane[1] = lane[2] = lane[3] = e[0];
                    else if (comps == 2) lane[2] = e[0], lane[3] = e[1];
                    else if (comps == 3) lane[3] = 0;
                }
                for (uint32_t c = 0; c < 4; c++) {
                    uint32_t m = masked ? (s.mask >> (std::min(cyc, 3u) * 8 + c * 2)) & 3 : 0;
                    if (m == 0) {
                        uint32_t v = lane[c];
                        if (c < comps) {
                            s.raw[a][c] = v;
                            s.rawmask[a] |= (uint8_t)(1u << c);
                        }
                        if (s.mode == 1) v = v + s.row[c];
                        else if (s.mode == 2) {
                            v = v + s.row[c];
                            s.row[c] = v;
                        }
                        mq[c] = v;
                    } else if (m == 1) {
                        mq[c] = s.row[c];
                    } else if (m == 2) {
                        mq[c] = s.col[std::min(cyc, 3u)];
                    }
                }
            }
            if (st != Status::Ok) break;
            if (addr == kSkinBase) {
                s.aux_cmd = (uint8_t)(0x60 | (cmd & 0x1F));
                s.aux_n = n;
            }
            p += nbytes;
        } else if (cmd == 0x00) {
        } else if (cmd == 0x01) {  // STCYCL
            s.cl = imm & 0xFF;
            s.wl = (imm >> 8) & 0xFF;
        } else if (cmd == 0x05) {  // STMOD
            s.mode = imm & 3;
        } else if (cmd == 0x20) {  // STMASK
            if (p + 4 > size) { st = Status::Truncated; break; }
            s.mask = rd32(vif + p);
            p += 4;
        } else if (cmd == 0x30 || cmd == 0x31) {  // STROW / STCOL
            if (p + 16 > size) { st = Status::Truncated; break; }
            uint32_t *r = cmd == 0x30 ? s.row : s.col;
            for (int c = 0; c < 4; c++) r[c] = rd32(vif + p + 4 * c);
            p += 16;
        } else if (cmd == 0x14 || cmd == 0x15 || cmd == 0x17) {  // MSCAL / MSCALF / MSCNT
            const int64_t prog = cmd != 0x17 ? (int64_t)imm : s.last_prog;
            s.last_prog = prog;
            BatchEmitter::run(s, (uint32_t)prog, out, part, opt);
            s.next_batch();
        } else if (cmd == 0x02 || cmd == 0x03 || cmd == 0x04 || cmd == 0x06 || cmd == 0x07 || cmd == 0x10 ||
                   cmd == 0x11 || cmd == 0x13) {
        } else if (cmd == 0x4A) {  // MPG
            p += (size_t)(num ? num : 256) * 8;
        } else if (cmd == 0x50 || cmd == 0x51) {  // DIRECT / DIRECTHL
            p += (size_t)imm * 16;
        } else {
            st = Status::UnknownCommand;
            break;
        }
    }
    part.batch_count = (uint32_t)out.batches.size() - part.first_batch;
    part.vertex_count = (uint32_t)out.vertices.size() - part.first_vertex;
    part.index_count = (uint32_t)out.indices.size() - part.first_index;
    part.status = st;
    out.parts.push_back(part);
    return st;
}

Status Decoder::decode_part(const uint8_t *part, size_t avail, Mesh &out, const Options &opt, uint32_t slot,
                            uint32_t source_offset) {
    if (avail < 0x38 || memcmp(part, "4SPS", 4) != 0) {
        Part p = {};
        p.slot = slot;
        p.source_offset = source_offset;
        p.first_batch = (uint32_t)out.batches.size();
        p.first_vertex = (uint32_t)out.vertices.size();
        p.first_index = (uint32_t)out.indices.size();
        p.status = avail < 0x38 ? Status::Truncated : Status::NotPart;
        out.parts.push_back(p);
        return p.status;
    }
    const uint32_t size = rd32(part + 4);
    char tex[17];
    memcpy(tex, part + 8, 16);
    tex[16] = 0;
    const uint32_t group = rd32(part + 0x18);
    const size_t end = std::min<size_t>(size, avail);
    Status st = decode_vif(part + 0x38, end > 0x38 ? end - 0x38 : 0, out, opt, tex, group, slot, source_offset);
    if (st == Status::Ok && size > avail) out.parts.back().status = st = Status::Truncated;
    return st;
}

Status Decoder::decode_modl(const uint8_t *data, size_t size, Mesh &out, const Options &opt) {
    if (size < 0x58) return Status::BadHeader;
    const uint32_t rec_tbl = rd32(data + 0x14);
    if (rec_tbl < 0x58 || rec_tbl > size) return Status::BadHeader;
    const uint32_t nslots = (rec_tbl - 0x58) / 4;
    Status worst = Status::Ok;
    for (uint32_t slot = 0; slot < nslots; slot++) {
        const uint32_t o = rd32(data + 0x58 + 4 * slot);
        if (o == kEmpty || (uint64_t)o + 8 > size) continue;
        if (memcmp(data + o, "4SPS", 4) != 0) continue;  // TJRM marker table, others
        const Status st = decode_part(data + o, size - o, out, opt, slot, o);
        if (st != Status::Ok) worst = st;
    }
    return worst;
}

Status Decoder::decode_mr_model(const uint8_t *data, size_t size, size_t off, Mesh &out, const Options &opt) {
    if (off + 0x40 > size) return Status::BadHeader;
    const uint32_t nparts = rd32(data + off), tbl = rd32(data + off + 4);
    if (nparts > 4096 || off + tbl + 4ull * nparts > size) return Status::BadHeader;
    Status worst = Status::Ok;
    for (uint32_t i = 0; i < nparts; i++) {
        const uint64_t p = off + (uint64_t)rd32(data + off + tbl + 4 * i);
        if (p + 8 > size) {
            worst = Status::BadHeader;
            continue;
        }
        const Status st = decode_part(data + p, size - p, out, opt, i, (uint32_t)p);
        if (st != Status::Ok) worst = st;
    }
    return worst;
}

Status Decoder::decode_part_guest(ReadFn read, void *ctx, uint64_t addr, Mesh &out, const Options &opt,
                                  uint32_t slot) {
    uint8_t hdr[0x38];
    if (!read || !read(ctx, addr, hdr, sizeof hdr)) return Status::ReadFailed;
    if (memcmp(hdr, "4SPS", 4) != 0) return decode_part(hdr, sizeof hdr, out, opt, slot, (uint32_t)addr);
    const uint32_t qwc = rd32(hdr + 0x34);
    if (qwc > (1u << 20)) return Status::BadHeader;
    guest_.resize(0x38 + (size_t)qwc * 16);
    memcpy(guest_.data(), hdr, sizeof hdr);
    if (qwc && !read(ctx, addr + 0x38, guest_.data() + 0x38, (size_t)qwc * 16)) return Status::ReadFailed;
    return decode_part(guest_.data(), guest_.size(), out, opt, slot, (uint32_t)addr);
}

}  // namespace vifmesh
