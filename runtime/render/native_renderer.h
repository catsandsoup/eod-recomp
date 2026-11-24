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
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <map>
#include "native_scene_types.h"
#include <utility>
#include <vector>

namespace gs {
struct Env;
struct Vertex;
}
namespace editor_environment { struct Override; }

namespace native {

bool enabled();
// stage R6 (docs/NATIVE_RENDERER.md "Stage R6"): Classic (R5 shaders, default) or Modern look;
// EOD_LOOK=modern at start, F9 / L1+R1+R3 through modern::toggle_from_input.
// The toggle returns the new look, which takes effect at the next flush.
bool look_available();
bool toggle_look();
bool look_modern();
bool sun_cascades_enabled();  // opt-in for the validated ordered scene path
// Prepare native pipelines after GS device creation and before the guest starts.
bool prepare();
bool plume_ready();
bool vu1_bypass_enabled();
bool vu1_bypass_dma_tag(uint32_t tag);
const editor_environment::Override *environment_override();
void ee_environment_world(bool drawing);

// ---- EE thread (src/render_queue.cpp) -------------------------------------------------------
void ee_frame_begin();                // RenderQueueBeginFrame
void ee_object(uint32_t batch);       // after scene_tap::object, before EmitBatch (0x35CB88)
void ee_object_end();                 // after EmitBatch
// packet manager reserve (src/render_trace.cpp wrappers): addr, qwc, return address
// 0x361098's aligned clear inputs, in the EE's whole-pixel XY coordinates.
enum class SceneClearTarget : uint32_t { ColorAndDepth, Depth, Bootstrap, RectColorDepth, RectDepth };
struct SceneClear {
    int32_t left, top, right, bottom;
    uint32_t rgba, zbuf;
    SceneClearTarget target;
};
constexpr uint32_t kSceneClearDirectQwc = 45;
inline bool is_rect_clear(SceneClearTarget target) {
    return target == SceneClearTarget::RectColorDepth || target == SceneClearTarget::RectDepth;
}
inline uint32_t scene_clear_qwc(SceneClearTarget target) { return is_rect_clear(target) ? 6u : kSceneClearDirectQwc; }
bool scene_clear_enabled();
void ee_packet_reserved(uint32_t addr, uint32_t qwc, uint32_t ra, const SceneClear *clear = nullptr);
bool take_scene_clear(uint32_t dma_tag, uint32_t qwc, SceneClear &clear);
struct DisplayCopy {
    uint32_t source, fix, caller;
};
constexpr uint32_t kDisplayCopyDirectQwc = 88;
struct BootstrapQuad {
    uint32_t xy[4][2], uv[4][2];  // unsigned 12.4 XYZ and UV fields
};
constexpr uint32_t kBootstrapQuadDirectQwc = 14;
constexpr uint32_t kBootstrapQuadDmaTag = 0x4d7650;
bool bootstrap_quad_enabled();
void ee_tag_bootstrap_quad(const BootstrapQuad &quad);
bool take_bootstrap_quad(uint32_t dma_tag, uint32_t qwc, BootstrapQuad &quad);
bool display_copy_enabled();
bool bootstrap_raster_enabled();
void ee_tag_display_copy(uint32_t addr, const DisplayCopy &copy);
bool take_display_copy(uint32_t dma_tag, uint32_t qwc, DisplayCopy &copy);
enum class ShadowPass : uint32_t {
    None, Prepare, Composite, PrepareUpper, CompositeUpper, PrepareLower, CompositeLower,
    PrepareTopLeft, CompositeTopLeft, PrepareTopRight, CompositeTopRight,
    PrepareBottomLeft, CompositeBottomLeft, PrepareBottomRight, CompositeBottomRight
};
struct ShadowViewport { int x, y, width, height; };
inline constexpr ShadowViewport kShadowViewports[] = {
    {0, 0, 640, 448}, {0, 0, 640, 224}, {0, 224, 640, 224},
    {0, 0, 320, 224}, {320, 0, 320, 224}, {0, 224, 320, 224}, {320, 224, 320, 224}
};
constexpr unsigned shadow_view_index(ShadowPass pass) { return (unsigned(pass) - 1) / 2; }
constexpr bool shadow_prepares(ShadowPass pass) { return (unsigned(pass) & 1) != 0; }
constexpr uint32_t kShadowPrepareDirectQwc = 77;
constexpr uint32_t shadow_composite_qwc(unsigned width) { return 9 + 4 * (width / 32); }
constexpr uint32_t shadow_direct_qwc(ShadowPass pass) {
    return shadow_prepares(pass) ? kShadowPrepareDirectQwc :
        shadow_composite_qwc(kShadowViewports[shadow_view_index(pass)].width);
}
ShadowPass take_shadow_pass(uint32_t dma_tag, uint32_t qwc);

// ---- EE thread, track world (src/native_world.cpp, stage R2) --------------------------------
// WorldRenderPass 0x358BD0: the pass starts (its identity matrix packet), each world batch REF it
// writes (the REF qword, the 4SPS part, the matrix packet, ITOP 0x80 / 0x280), the texture
// record's packet commit (0x35CBB8 result: false = no packet space, nothing is drawn), the end.
void ee_world_pass(int32_t pass, uint32_t identity_packet);
void ee_world_leaf(uint32_t ref_qword, uint32_t part, uint32_t matrix_packet, uint32_t itop);
void ee_world_record(bool committed);
void ee_world_material(int32_t slot);
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
// Fog state from the decompiled setters (src/native_fog.cpp, docs/NATIVE_RENDERER.md "Stage R5"):
// GsFogWriteTables 0x3653F8 (range), 0x365428 (FOGCOL, state 0x4D8B80), 0x365540 (render flags
// 0x4D8BC8: bit 3 = GS PRIM.FGE). Never read back from GS registers.
void ee_fog_range(float start, float end, float density);
void ee_fog_colour(uint32_t rgb);
void ee_render_flags(uint32_t flags);
uint64_t fog_fge_draws();             // native draws that had FGE set (retail: 0)
// Texture LOD and blend state from the decompiled setters (src/native_texstate.cpp, stage R5 A2):
// TextureSetLodState 0x3652A8 (TEX1.K template *0x4D8BAC in 1/16, MXL *0x4D8BBC), LodBiasSet
// 0x365308 (*0x4D8BB8: the particle/sprite K delta, both halves), BlendSelect 0x3654E0 (*0x4D8B78).
void ee_tex_lod(int32_t k_template, uint32_t mxl, uint32_t bias_k);
void ee_blend_select(uint32_t sel);
void ee_shadow_begin(int32_t alpha, bool retail_single_view, int retail_split_y, bool retail_quad_view);
void ee_shadow_opened(bool opened);
void ee_shadow_batch(uint32_t batch, uint32_t pass, uint32_t clip_flag, uint32_t mat_pkt, uint32_t ref_qword);
void ee_shadow_end();
// FlareRender 0x304D50: each texture bucket emits 2D quads through 0x35DF10 / 0x361708 / 0x361BC8.
void ee_flare_begin(uint32_t tex_slot);
bool ee_in_flare();
void ee_flare_vertex(const float xyz[3], const float st[2], uint32_t colour, uint32_t strip);
void ee_flare_end(bool drawn);
// ---- EE thread, particles, sprites, immediate & cockpit quad (src/native_effects_imm.cpp, R4 A2) ----
void ee_particle_begin(float scale);
void ee_particle_add(int32_t col_rgb, int32_t col_alpha, float size, float x, float y, float z, float angle);
void ee_particle_end(bool drawn);
void ee_sprite_begin(int32_t grid_size);
void ee_sprite_add(const uint32_t col[4], int32_t flip, float size, float x, float y, float z);
void ee_atlas_sprite_add(const uint32_t col[4], int32_t tile, int32_t grid_shift, int32_t grid_mask,
                         float size, float x, float y, float z);
void ee_sprite_end(bool drawn);
// hud: native 2D (R7 A1b): 0x35CAA0 from the rect / sprite leaves 0x11E730 / 0x29E138 and the
// text flush 0x29A2B8 -> 0x35DF10; drawn as KIND_HUD (never relit by Modern)
void ee_imm_begin(bool cockpit, bool hud = false);
bool ee_in_imm();
void ee_imm_vertex(const float xyz[3], const float st[2], uint32_t colour, uint32_t strip, float spark_age);
void ee_imm_end(bool drawn);
void ee_sphere_imm(uint32_t buf_obj, uint32_t ref_qword);
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
    uint32_t particle = 0, sprite = 0, immediate = 0, cockpit = 0;  // R4 Phase A2 P1 counters
    uint32_t hud = 0;                                                // R7 A1b P1 counter
};
void ee_finish_submissions();
Expected ee_pending_at_shutdown(uint64_t frame, const std::vector<bool> &received);
void take_expected(uint64_t before, std::map<uint64_t, Expected> &out);
// EOD_NATIVE_ONLY=1: the GS emulator draws nothing (missing native paths show as missing geometry)
bool native_only();
int native_only_level();  // EOD_NATIVE_ONLY value (2: the GS keeps drawing sprites)
// Last 3D target Plume rendered to (used by EOD_NATIVE_ONLY=1 capture when the GS copy sprite is dropped)
bool last_target(uint32_t &fbp, uint32_t &fbw, uint32_t &psm);

// ---- VIF1 thread (runtime/vu/vif.cpp) -------------------------------------------------------
// Posts the object marker before its geometry. True once for a committed world-leaf REF
// eligible for VIF and VU validation.
struct WorldTextureLod {
    uint16_t bias;       // signed 12-bit K in sixteenths, transported as the EE's 16-bit value
    uint8_t max_level;
    bool valid;
};
constexpr uint64_t world_tex1(const WorldTextureLod &lod) {
    return 0x120 | (uint64_t(lod.max_level) << 2) | (uint64_t(lod.bias) << 32);
}

// Terminal attributes and FIFO of the exact non-clipped world strip.
// Positions are in the GS input grid, before the ordered viewport offset.
struct WorldStripTerminal {
    struct Vertex {
        uint16_t x, y;
        uint32_t z, rgba;
        uint32_t s, t, q;  // float bit patterns (latch comparison is exact)
        uint8_t fog;
    };
    Vertex fifo[3];
    uint32_t prim, vertices, adc_vertices, triangle_attempts;
};

struct QueueStripOmission {
    uint64_t frame;
    uint32_t serial;
    WorldStripTerminal terminal;
    uint64_t tag_high;
    bool reference;
};
struct QueueDentChunk {
    unsigned vertices, overwrite;
    QueueStripOmission omission;
};
bool queue_dent_candidate(size_t launch_word, QueueDentChunk &candidate);
void queue_dent_selected(uint32_t top, const QueueStripOmission &omission);
bool gs_queue_dent_accepted(const QueueStripOmission &omission, const gs::Env &env, uint64_t xy);
bool queue_dent_check_enabled();
void check_queue_dent_input(uint32_t top, size_t launch_word, bool continuation);

bool vu1_start(uint32_t program, uint32_t itop, bool continuation, WorldTextureLod &world_lod,
               bool &clipped_world_continuation, bool *bypass_vu1 = nullptr);

// ---- GS command stream ----------------------------------------------------------------------
// One queue record as the GS receives it.
enum : uint32_t {
    KIND_QUEUE = 0,
    KIND_WORLD = 1,
    KIND_WATER = 2,
    KIND_SHADOW = 3,
    KIND_FLARE = 4,
    KIND_PARTICLE = 5,
    KIND_SPRITE = 6,
    KIND_IMMEDIATE = 7,
    KIND_COCKPIT = 8,
    KIND_HUD = 9,  // native 2D: race HUD rects, frame sprites, text (R7 A1b)
    KIND_COUNT = 10
};
struct DrawSurface {
    uint32_t color_base, color_pitch, color_format, color_mask;
    uint32_t depth_base, depth_format;
    int32_t scissor[4];         // inclusive left, top, right, bottom
    uint16_t origin_x, origin_y; // 12.4 screen origin
};

struct WorldTextureBinding {
    uint32_t colors[16];
    uint64_t hash;
    uint64_t index_version;
    uint16_t bases[7];
    uint8_t pitches[7];
    uint8_t levels, function;
};

constexpr uint32_t kWorldTextureSide = 128;
constexpr uint32_t kWorldTextureLevels = 7;
constexpr uint32_t world_texture_index_bytes(uint32_t levels) {
    uint32_t bytes = 0;
    for (uint32_t level = 0; level < levels; ++level) {
        const uint32_t side = kWorldTextureSide >> level;
        bytes += side * side / 2;
    }
    return bytes;
}

struct WorldTextureIndices {
    uint64_t version, mip_hashes[kWorldTextureLevels];
    uint32_t levels;
    uint8_t data[world_texture_index_bytes(kWorldTextureLevels)];
};

constexpr uint32_t kQueueTextureWidth = 256;
constexpr uint32_t kQueueTextureHeight = 128;
constexpr uint32_t kQueueTextureLevels = 6;
constexpr uint32_t queue_texture_offset(uint32_t level) {
    uint32_t bytes = 0;
    for (uint32_t l = 0; l < level; ++l)
        bytes += (kQueueTextureWidth >> l) * (kQueueTextureHeight >> l);
    return bytes;
}

struct QueueTexture {
    std::array<uint32_t, 256> colors;
    std::array<uint8_t, queue_texture_offset(kQueueTextureLevels)> indices;
    uint64_t palette_hash, content_hash;
    uint8_t function;
};
struct QueueReady {
    std::atomic<bool> ready{false};
};
struct QueueMaterial {
    std::shared_ptr<QueueReady> readiness;
    std::shared_ptr<const QueueTexture> texture;
    std::array<uint16_t, kQueueTextureLevels> bases;
    DrawSurface surface;
    uint32_t triangles = 0;
    int32_t bias = 0;
};

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
    // fog in effect when it was queued (0x4D8BC8 bit 3, FOGCOL 0x4D8B80, range 0x4D8B84..8C)
    uint32_t fge, fogcol;
    float fog_range[3];
    bool editor_environment;
    bool retail_shadow_view;    // 0x30A580, one active retail viewport at ShadowBegin
    // the rest of the EE render state in effect when it was queued (stage R5 A2): render flags
    // *0x4D8BC8 (0x365540), blend select *0x4D8B78 (0x3654E0), TEX1 K template *0x4D8BAC and MXL
    // *0x4D8BBC (0x3652A8), the particle/sprite K delta *0x4D8BB8 (0x365308)
    uint32_t flags, blend_sel, tex_mxl, bias_k;
    int32_t tex_k;
    // 1 / (fov * width / 2) (0x35B3D0 fVar7): the native screen w times this is the VU1 w, i.e.
    // the GS 1 / Q the texture LOD uses (EOD_NATIVE_QCHECK)
    float w_scale;
    // projected shadow target (stage R5 A3), as 0x363218 left it: width *0x4D8BD8, height *0x4D8BDC
    // (0x35DA30: min(view w, 320), min(view h, 512)) and the main view *0x4D8C00..0x4D8C0C (x, y, w, h)
    int32_t shadow_rt[6];
    DrawSurface world_surface;  // EE RenderStateCommit inputs, sampled at the world REF
    WorldTextureLod world_lod;
    WorldTextureBinding world_texture;
};

using Vertex = scene::Vertex;

struct Mesh {                  // a decoded batch (EE -> GS; the GS owns it)
    uint32_t id;
    uint32_t replaces;         // id of the mesh this batch had before (0 = none)
    uint32_t batch;
    bool skinned;              // the batch has skinned vertices (VU1 0x318/0x24B0 matrices, 0x2E0 dents)
    bool transient = false;    // one draw's geometry (water, camera-facing parts, posed skins): released after use
    bool posed = false;        // skinned vertices posed with the palette / dents in effect (R3)
    uint32_t bind = 0;         // posed: the bind-pose mesh id (P2 check)
    // TEX1.K delta of the batch's VU1 runs (stage R5 A2): each program start reads the batch
    // qword 198 z (mtir vi1, vf13z at VU1 0x03B8 / 0x0DA0 / 0x1600 / 0x1920) and, when it is not
    // positive, kicks TEX1 with K = template + z + 1000 (iaddiu 0x3E8, iadd, isw.y 21, xgkick 20)
    int32_t k_delta = 0;       // z + 1000 (1/16 units) of the first run
    bool k_kick = false;       // the first run kicks TEX1 (z <= 0)
    bool k_mixed = false;      // later runs have other values: drawn run by run (k_runs)
    struct KRun {
        uint32_t first, count;  // index range of the run's triangles
        int32_t delta;
        bool kick;
    };
    std::vector<KRun> k_runs;  // every VU1 run, when k_mixed
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<scene::EffectBillboard> billboards;  // retained for a live switch to Modern
    bool has_spark_age = false;
};

enum : uint32_t { CMD_OBJ = 0, CMD_OFF = 1, CMD_MESH = 2, CMD_SCENE_CLEAR = 3, CMD_DISPLAY_COPY = 4,
                  CMD_BOOTSTRAP_QUAD = 5, CMD_WORLD_TEXTURE_LOD = 6, CMD_WORLD_TEXTURE_INDICES = 7,
                  CMD_WORLD_STRIP_CHECK = 8, CMD_QUEUE_MATERIAL = 9, CMD_QUEUE_STRIP = 10,
                  CMD_DIRECT_FINALIZE = 11, CMD_TEXTURE_UPLOAD = 12, CMD_MATERIAL_STATE = 13, CMD_SHADOW_BLEND = 14, CMD_TEXTURE_FENCE = 15,
                  CMD_TRANSIENT_MESH = 16 };

// Model space -> GS screen (x, y in GS primitive coordinates, z in GS depth units, w) as
// a[row][col] for column vectors: the camera packet 0x35B3D0 (viewport x projection, VU1 q5..8),
// the view matrix (q13..16) and the model matrix (TOPS+0..3), as the VU1 setup program 0x0000
// combines them into vf4..vf7 (which are the columns of a).
void screen_matrix(const Packet &p, double a[4][4], bool vu_rounding = false);

// producer of the GS command stream (VIF1 thread): queue a command in GS order (runtime/gs/gs.cpp)
void post(uint32_t kind, const void *data, uint32_t size);

// GS thread
void gs_command(uint32_t kind, const void *data, uint32_t size);
bool gs_clear_scene(const gs::Env &env, const SceneClear &clear, const int *rect = nullptr);
bool gs_draw_bootstrap_quad(const gs::Env &env, const Vertex (&vertices)[4]);
bool gs_shadow_boundary(ShadowPass pass);
// GS worker: conditionally accepts the verified strip as one native world object.
bool gs_world_strip_ready();
void gs_before_frame_skip();
bool gs_submit_world_strip(const gs::Env &env, uint64_t xy, uint32_t triangles, bool reference);
bool gs_check_world_strip_draw(uint64_t xy, uint32_t triangles);
int world_strip_mode();  // 0: original, 1: omit when admitted, 2: verify the original path when admitted
void gs_report_world_strips(uint64_t expected);
// a primitive the GS is about to draw: true = it belongs to a native object (do not draw)
// (b, c: the other vertices of a triangle, for the P2 vertex check EOD_NATIVE_SKIN_CHECK=1)
bool gs_draw(const gs::Env &e, const gs::Vertex &first, const gs::Vertex *b = nullptr, const gs::Vertex *c = nullptr);
// a VRAM transfer or other non-draw use of GS memory follows: draw what is pending first
void gs_barrier();
// The GS FIFO may overwrite movie pages when no earlier native work is queued.
bool gs_movie_clear_ready();
void gs_vblank(uint64_t frame);
void gs_finish_submissions(uint64_t frame);

}  // namespace native
