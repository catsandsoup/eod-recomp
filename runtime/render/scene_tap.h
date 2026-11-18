// scene_tap.h - the native renderer's per-frame input, taken from the game's own render data
// (docs/NATIVE_RENDERER.md, "Data in"). Filled by the decompiled draw queue (src/render_queue.cpp)
// as it emits each record, so objects arrive in the game's draw order with the values the game
// computed for them, not values inferred at the VU1 boundary.
//
// Off unless enabled (scene_tap::set_enabled); when off, the hooks are a single branch.
#pragma once
#include <cstdint>
#include <vector>

namespace scene_tap {

// One draw-queue record as the flush (0x34AF98) emits it.
struct Object {
    uint32_t batch;            // guest address of the batch (mesh key: static VIF data at +0x38)
    uint32_t matrix_packet;    // guest address of the matrix packet (0 = identity)
    uint32_t material_packet;  // guest address of the material packet, 0xFFFFFFFF = none
    int32_t texture_key;       // the bucket key: texture slot in *(0x4D2D30), -1 = none
    int32_t layer;             // flush layer (0..9, -1 = immediate)
    uint32_t clip;             // the frustum test's "partly outside" flag
    float model[12];           // row-major 3x4: [r*4+0..2] rotation, [r*4+3] translation (matrix packet +0x10)
    float colour_a[4];         // TOPS+4: rgb * 128/255 (0x34E8A8 setter), w 0
    float colour_b[4];         // TOPS+5: rgb * 128/255 (0x34F5A0 setter), w = shadow size
    float light[4];            // TOPS+6: light direction xyz, w = -*(0x3DD648)
};

// The camera of one view (one split-screen viewport) at the time its queue was flushed.
struct View {
    int32_t index;             // 0x3D1A78: view number within the frame
    float rotation[9];         // camera rotation 0x3E57A8 (rows)
    float position[3];         // camera translation 0x3E57CC
    float viewport[4];         // 0x4D7C20..2C: x, y, width, height
    float near_z, far_z;       // 0x4D7C10, 0x4D7C14
    float fov_scale;           // 0x4D7C1C
    float aspect;              // 0x3DD658
    uint32_t first_object, object_count;  // range in Frame::objects
};

struct Frame {
    uint64_t vblank = 0;
    std::vector<View> views;
    std::vector<Object> objects;
};

bool enabled();
void set_enabled(bool on);

// Game-side hooks (EE thread).
void begin_view();                       // RenderQueueBeginFrame (0x34C9C0)
void object(uint32_t record, int32_t texture_key, int32_t layer);  // per record, as the flush emits it
void end_frame();
// the record object() just added (EE thread), or null
const Object *last_object();                        // publish the frame (begin_view does it when the vblank changes)

// Renderer side: the most recent complete frame (copy; safe from another thread).
bool latest(Frame &out);

}  // namespace scene_tap
