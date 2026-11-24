// vif_mesh.h - decodes EOD model geometry (4SPS parts: prebuilt VIF1 streams for the VU1 model
// programs) into renderer-ready meshes. C++ port of tools/vif_mesh.py; format and evidence in
// docs/VIF_MESH_DECODER.md.
//
// No game or runtime dependencies, C++17, no exceptions across the API (every entry point
// returns a Status). Decoding appends to a Mesh; reuse one Mesh and one Decoder (clear() keeps
// the capacity) to decode a whole track without per-part or per-vertex allocations.
//
// Inputs, all little-endian guest data:
//   decode_vif         a raw VIF stream: a draw-queue batch's data at +0x38, qwc*16 bytes
//                      (qwc at +0x34); this is exactly a 4SPS part's body.
//   decode_part        a 4SPS part header (texture[16] at +8, group at +0x18, VIF from +0x38).
//   decode_modl        a stand-alone .mod member (part slots from 0x58 to header word 0x14).
//   decode_mr_model    an embedded mrModel (+0 nParts, +4 part table, offsets model-relative),
//                      as in track.grf.
//   decode_part_guest  a 4SPS part in guest memory, read through a callback.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vifmesh {

enum class Status : uint8_t {
    Ok = 0,
    Truncated,       // a command or its data runs past the end of the input
    UnknownCommand,  // a VIF command the decoder does not know (tools/vif_mesh.py rejects it too)
    FillMode,        // STCYCL with WL > CL (fill mode): not used by retail data, not decoded
    BadAddress,      // an UNPACK writes past the emulated VU memory (2048 qwords)
    BadHeader,       // a model or part header is inconsistent (offsets out of range)
    NotPart,         // the part magic is not '4SPS'
    ReadFailed,      // the guest-memory callback failed
};
const char *status_name(Status s);

// Vertex flags
enum : uint32_t {
    kAdc = 1u << 0,         // ADC: this vertex does not close a triangle (strip restart)
    kHasNormal = 1u << 1,   // normal[] is valid (V4-8 normal unpack, STROW 65536.0f)
    kHasColor = 1u << 2,    // color[] holds a vertex colour (V4-5 / V4-8 without the normal row)
    kHasSkin = 1u << 3,     // bone[]/weight[] are valid (two-bone skin word read by the VU program)
    kHasAux = 1u << 4,      // aux[] holds the raw per-vertex word at qword 150.. (any program)
    kNoAttr = 1u << 5,      // no attribute qword: normal defaults to (0,1,0) as in vif_mesh.py
};

struct Vertex {
    float pos[3];       // model space (world space for track geometry): row + u + qword199 offset
    float pos_w;        // the position qword's w lane as the VIF wrote it (V3: 0 + STROW w in
                        // offset mode); the skinned programs use it as the translation weight
    float normal[3];    // (byte - 128) / 127 as tools/vif_mesh.py (the VU itself uses /128)
    float uv[2];        // s, t: row + u + qword198 offset (GS convention, t down)
    uint8_t color[4];   // PS2 scale, not changed: 0x80 = 1.0. RGB from the data; A = 0x80.
                        // Normal-lit vertices get 0x80,0x80,0x80,0x80.
    uint16_t bone[2];   // skin word .x/.y low 16 bits: an absolute VU1 qword base the program
                        // adds to (dents: translation at bone+143; matrices: columns bone+144..147)
    float weight[2];    // skin word .z/.w minus 32768 (= byte / 256)
    uint8_t aux[4];     // raw bytes of this vertex's word at qword 150.. (VU order, see below)
    uint32_t flags;
};

enum class Attr : uint8_t { None, Normal, Color };
enum class Skin : uint8_t {
    None,
    Offset,   // program 0x2e0 (MSCAL 0x5C, cars): bone = dent slot, p + wa*D[a] + wb*D[b]
    Matrix,   // program 0x318 (MSCAL 0x63, drivers/pedestrians): wa*(Ma p) + wb*(Mb p)
    Partial,  // program 0x2e0 with a one-byte (V1-8) skin unpack: the VIF repeats the byte in all
              // four lanes (+ STROW), so bone a = bone b and both weights come from the row
};

struct Batch {
    uint32_t program;        // MSCAL immediate (VU1 byte address / 8); MSCNT repeats the previous
    uint32_t declared;       // qword 0 count (& 0x7FFF); vertex_count is smaller when data ends
    uint32_t first_vertex;   // into Mesh::vertices
    uint32_t vertex_count;
    uint32_t first_index;    // into Mesh::indices
    uint32_t index_count;
    float pos_offset[3];     // qword 199 xyz
    float uv_offset[2];      // qword 198 xy
    uint32_t q198_zw[2];     // qword 198 zw raw (program flags; not interpreted)
    Attr attr;
    Skin skin;
    uint8_t aux_format;      // 0 none, else the UNPACK cmd (0x60..0x7F) that wrote qword 150..
    uint8_t shadow;          // program 4 (shadow volume source: 2-qword stride, no UV)
};

// Source lanes of one dent strip before the per-object pose, projection and
// lighting. XYZ origins and texture K belong to the chunk, not the part.
struct DentVertex {
    uint32_t normal[3];
    uint16_t adc;
    uint16_t bone[2];
    float position[4];
    float texture[3];
    float weight[2];  // already unbiased
};

struct DentChunk {
    bool ordered = true;
    uint32_t launch_word;  // word offset in the relocated 4SPS payload
    bool continuation;    // false: MSCAL 0x5C, true: MSCNT
    uint32_t strip;
    float position_origin[3];
    float texture_origin[2];
    uint32_t texture_k;
    std::vector<DentVertex> vertices;  // source strip order, including ADC and degenerates
};

struct Part {
    char texture[17];        // part +8, NUL padded (16 chars max)
    uint32_t group;          // part +0x18: low 16 bits damage/visibility group, bit 16 body paint
    uint32_t slot;           // slot (.mod) or part index (mrModel) or caller value
    uint32_t source_offset;  // part offset inside the input buffer (or guest address low bits)
    uint32_t first_batch, batch_count;
    uint32_t first_vertex, vertex_count;
    uint32_t first_index, index_count;  // indices are relative to first_vertex
    Status status;
};

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;   // triangle list, 3 per triangle, part-relative
    std::vector<Batch> batches;
    std::vector<Part> parts;
    std::vector<DentChunk> dent_chunks;
    void clear() { vertices.clear(); indices.clear(); batches.clear(); parts.clear(); dent_chunks.clear(); }
};

struct Options {
    // tools/vif_mesh.py contract: orient each strip triangle so its face normal agrees with the
    // summed vertex "normals" (for colour batches these are colour bytes read as normals, which
    // is what the Python preview does). false = raw strip parity winding.
    bool fix_winding = true;
    // drop triangles with two equal positions (tools/vif_mesh.py load_mesh/world_graf)
    bool drop_degenerate = true;
    // Shadow side faces can have equal bind positions with different normals.
    bool keep_shadow_extrusion_faces = false;
    // The native draw queue retains the exact per-launch source geometry.
    // Offline visual export uses the flattened preview mesh.
    bool keep_dent_chunks = false;
    // Camera-facing quads expand coincident bind positions using their per-vertex corner indices.
    bool keep_camera_facing_faces = false;
};

// Guest-memory reader: copy n bytes at guest address addr to dst; return false on failure.
using ReadFn = bool (*)(void *ctx, uint64_t addr, void *dst, size_t n);

class Decoder {
public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;

    // One part from its VIF stream. texture may be null. Appends one Part, also on error: its
    // status says why and the batches decoded before the error are kept (tools/vif_mesh.py
    // raises instead and loses the whole part).
    Status decode_vif(const uint8_t *vif, size_t size, Mesh &out, const Options &opt = Options(),
                      const char *texture = nullptr, uint32_t group = 0, uint32_t slot = 0,
                      uint32_t source_offset = 0);
    // A 4SPS part: avail = bytes readable from part (the VIF ends at part + size field).
    Status decode_part(const uint8_t *part, size_t avail, Mesh &out, const Options &opt = Options(),
                       uint32_t slot = 0, uint32_t source_offset = 0);
    // Stand-alone .mod: every 4SPS slot in the header table (empty = 0xE3E3E3E3, others skipped).
    Status decode_modl(const uint8_t *data, size_t size, Mesh &out, const Options &opt = Options());
    // Embedded mrModel at data + model_offset (data = the containing member, e.g. track.grf).
    Status decode_mr_model(const uint8_t *data, size_t size, size_t model_offset, Mesh &out,
                           const Options &opt = Options());
    // A 4SPS part (or draw-queue batch) in guest memory: header at addr, qwc at +0x34.
    Status decode_part_guest(ReadFn read, void *ctx, uint64_t addr, Mesh &out, const Options &opt = Options(),
                             uint32_t slot = 0);

    struct State;  // opaque VIF/VU1 emulation state (defined in vif_mesh.cpp)

private:
    State *s_;
    std::vector<uint8_t> guest_;
};

}  // namespace vifmesh
