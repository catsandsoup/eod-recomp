// native_renderer.h - the native renderer (docs/NATIVE_RENDERER.md, stage R1): the game's queued 3D
// objects (draw queue 0x34AF98, src/render_queue.cpp) drawn with plume from the game's own data
// (scene_tap records, the decoded batch geometry, the camera packet inputs), in the game's draw
// order, into the game's own frame and depth buffers. The GS draws everything else.
//
// Three threads take part:
//   EE      the flush hands each record over (ee_object) before the record's batch DMA is built;
//           the packet reserved for the batch's VIF data is tagged with the record
//           (ee_packet_reserved). Meshes are decoded here, once per batch (cached by address and
//           content hash).
//   VIF1    (the offload worker when it runs): when VU1 starts a program from a tagged batch
//           packet, the record goes into the GS command stream (OBJ), and any other program
//           start after it ends the record (OFF). So the GS learns, in its own command order,
//           which PATH1 draws belong to a native object.
//   GS      drops those draws (keeping their register state), takes the object's GS state at its
//           first draw (frame/depth buffer, tests, blending, TEX0), and draws pending objects
//           with plume into the same surfaces just before the next thing the GS draws itself.
//
// Off unless EOD_NATIVE=1 or [Video] NativeRenderer=1 (Vulkan backend only); when off no hook
// does anything and the GS command stream is unchanged.
#pragma once
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace gs {
struct Env;
struct Vertex;
}

namespace native {

bool enabled();

// ---- EE thread (src/render_queue.cpp) -------------------------------------------------------
void ee_frame_begin();                // RenderQueueBeginFrame
void ee_object(uint32_t record);      // after scene_tap::object, before EmitBatch (0x35CB88)
void ee_object_end();                 // after EmitBatch
// packet manager reserve (src/render_trace.cpp wrappers): addr, qwc, return address
void ee_packet_reserved(uint32_t addr, uint32_t qwc, uint32_t ra);

// ---- EE thread, track world (src/native_world.cpp, stage R2) --------------------------------
// WorldRenderPass 0x358BD0: the pass starts (its identity matrix packet), each world batch REF it
// writes (the REF qword, the 4SPS part, the matrix packet, ITOP 0x80 / 0x280), the texture
// record's packet commit (0x35CBB8 result: false = no packet space, nothing is drawn), the end.
void ee_world_pass(int32_t pass, uint32_t identity_packet);
void ee_world_leaf(uint32_t ref_qword, uint32_t part, uint32_t matrix_packet, uint32_t itop);
void ee_world_record(uint32_t slot, bool committed);
void ee_world_pass_end();
// WaterDraw 0x325728: begin (before ImmediateBegin 0x35DF10, whose packets are the water's), each
// vertex ImmediateVertex 0x361708 accepted (colour word, strip flag = draws a triangle, guest
// address of {xyz, colour, -, st}), end (after ImmediateEnd 0x361BC8; drawn = false when
// ImmediateBegin had no packet space).
void ee_water_begin(uint32_t surface);
// Every vertex ImmediateVertex 0x361708 accepts while a WaterDraw is open: the tiles 0x325728 draws
// itself and the subdivided tiles of its 0x324928 subtree (xyz, st, colour word, strip flag).
bool ee_in_water();
void ee_water_vertex(const float xyz[3], const float st[2], uint32_t colour, uint32_t strip);
void ee_water_end(bool drawn);
// ---- EE thread, projected shadows & flares (src/native_shadows_flares.cpp, stage R4 Phase A1) ----
// ShadowBegin 0x34C7D0 (before 0x35DA30 overwrites viewport/aspect with the PS2 320x512 cap):
// snapshots main-viewport camera/projection and shadow intensity; ShadowEmitBatchPass 0x35D170
// tags each shadow batch DMA REF; ShadowEnd 0x34C798 closes the pass.
void ee_shadow_begin(int32_t alpha);
void ee_shadow_opened(bool opened);
void ee_shadow_batch(uint32_t batch, uint32_t pass, uint32_t clip_flag, uint32_t mat_pkt, uint32_t ref_qword);
void ee_shadow_end();
// FlareRender 0x304D50: each texture bucket emits 2D quads through 0x35DF10 / 0x361708 / 0x361BC8.
void ee_flare_begin(uint32_t tex_slot);
bool ee_in_flare();
void ee_flare_vertex(const float xyz[3], const float st[2], uint32_t colour, uint32_t strip);
void ee_flare_end(bool drawn);
// ---- EE thread, VU1 uploads (src/bone_pose.cpp, stage R3) ----------------------------------
// Every UNPACK + MSCAL 0x24D8 packet the readable 0x35A810 builds: bone palettes (0x35DBD8, V4-32
// to VU1 0x90 + 4*first), car dent offsets (0x35DD28, V3-32 to 0x90), the sun quad (0x35CBD8,
// V3-32 to 0xAC). `src` is the guest data the packet copies (count * comps words); the native
// renderer keeps its own copy (it never reads VU1 memory) and applies it to its model of the VU1
// bone qwords when the packet becomes the material in effect (*(0x4D7C54)) for a draw.
void ee_vu1_upload(uint32_t packet, uint32_t qword, uint32_t comps, uint32_t count, uint32_t src);
// P2: the palette fed for `packet` against the pose array 0x34C9F8 was given (bones[count][12])
void ee_pose_check(uint32_t packet, uint32_t bones, uint32_t count);
// GS thread: what the EE submitted per EE frame before `before` (world leaves committed, water
// draws), removed from the EE's list.
struct Expected {
    uint32_t world = 0, water = 0, queue = 0, shadow = 0, flare = 0;  // per EE frame P1 counters
};
void take_expected(uint64_t before, std::map<uint64_t, Expected> &out);
// EOD_NATIVE_ONLY=1: the GS emulator draws nothing (missing native paths show as missing geometry)
bool native_only();

// ---- VIF1 thread (runtime/vu/vif.cpp) -------------------------------------------------------
void vu1_start(uint32_t program, uint32_t itop);

// ---- GS command stream ----------------------------------------------------------------------
// One queue record as the GS receives it.
enum : uint32_t { KIND_QUEUE = 0, KIND_WORLD = 1, KIND_WATER = 2, KIND_SHADOW = 3, KIND_FLARE = 4, KIND_COUNT = 5 };
struct Packet {
    uint64_t frame;            // EE vblank of the frame it was queued in
    uint32_t kind;             // KIND_*: draw-queue record, track world leaf, water surface
    uint32_t pass;             // world pass (0x358BD0 argument)
    uint32_t serial;           // record number within that frame (draw order)
    uint32_t mesh;             // mesh id (0: no geometry decoded)
    uint32_t batch;
    uint32_t itop;             // VU1 ITOP: vertex alpha is its low byte
    int32_t texture_key, layer;
    uint32_t program;          // VU1 program the record's first start ran
    float model[12];           // row-major 3x4 model matrix (scene_tap)
    float colour_a[4], colour_b[4], light[4];
    // the camera packet inputs (0x35B3D0) at flush time
    float rotation[9], position[3], viewport[4], near_z, far_z, fov_scale, aspect;
};

struct Vertex {                // GPU vertex (28 bytes)
    float p[3];
    float uv[2];
    int8_t n[4];               // byte - 128 of the normal; [3] = 1 when present
    uint8_t c[4];              // vertex colour bytes (0x80 = 1.0)
};
static_assert(sizeof(Vertex) == 28, "vertex layout");

struct Mesh {                  // a decoded batch (EE -> GS; the GS owns it)
    uint32_t id;
    uint32_t replaces;         // id of the mesh this batch had before (0 = none)
    uint32_t batch;
    bool skinned;              // the batch has skinned vertices (VU1 0x318/0x24B0 matrices, 0x2E0 dents)
    bool transient = false;    // one draw's geometry (water, camera-facing parts, posed skins): released after use
    bool posed = false;        // skinned vertices posed with the palette / dents in effect (R3)
    uint32_t bind = 0;         // posed: the bind-pose mesh id (P2 check)
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

enum : uint32_t { CMD_OBJ = 0, CMD_OFF = 1, CMD_MESH = 2 };

// Model space -> GS screen (x, y in GS primitive coordinates, z in GS depth units, w) as
// a[row][col] for column vectors: the camera packet 0x35B3D0 (viewport x projection, VU1 q5..8),
// the view matrix (q13..16) and the model matrix (TOPS+0..3), as the VU1 setup program 0x0000
// combines them into vf4..vf7 (which are the columns of a).
void screen_matrix(const Packet &p, double a[4][4]);

// producer of the GS command stream (VIF1 thread): queue a command in GS order (runtime/gs/gs.cpp)
void post(uint32_t kind, const void *data, uint32_t size);

// GS thread
void gs_command(uint32_t kind, const void *data, uint32_t size);
// a primitive the GS is about to draw: true = it belongs to a native object (do not draw)
// (b, c: the other vertices of a triangle, for the P2 vertex check EOD_NATIVE_SKIN_CHECK=1)
bool gs_draw(const gs::Env &e, const gs::Vertex &first, const gs::Vertex *b = nullptr, const gs::Vertex *c = nullptr);
// a VRAM transfer or other non-draw use of GS memory follows: draw what is pending first
void gs_barrier();
void gs_vblank(uint64_t frame);

}  // namespace native
