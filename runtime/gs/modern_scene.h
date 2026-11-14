// modern_scene.h - EOD_MODERN=1: the modern renderer draws the race's 3D scene
// (sky, world, cars, props) from the decoded VU1 batches (runtime/vu/scene_emit),
// and the GS keeps drawing everything else (HUD, 2D, menus) on top.
// docs/SCENE_CAPTURE.md "Modern renderer".
//
// Flow, all in GS command order:
//   producer, each replaced VU1 batch: batch(i)  -> the GS draws of the following
//     PATH1 kicks are not drawn; the first textured one names batch i's texture
//   producer, a run that is not replaced: batch(-1)
//   producer, at the HUD (first 0x270 run in ITOP mode 0x200) or the vblank:
//     draw(frame) -> the backend renders the frame into the current frame buffer
// Off unless EOD_MODERN=1; Original rendering never sees any of it.
#pragma once
#include <cstdint>
#include <vector>

namespace gs {
struct Env;
}

namespace modern {

struct Vertex {
    float p[3];      // model space
    float uv[2];
    int8_t n[4];     // normal (x, y, z / 127), [3] = 127 when present
    uint8_t c[4];    // vertex colour and alpha (0x80 = 1.0) when there is no normal
};
static_assert(sizeof(Vertex) == 28, "vertex layout");

struct Mesh {
    uint64_t id;
    std::vector<Vertex> v;
    std::vector<uint32_t> idx;  // triangle list from the strip flags
};

enum Role : uint32_t { SKY = 0, WORLD = 1, OBJECT = 2 };

struct Batch {
    uint64_t mesh;
    float model[12];   // rows of the 3x4 world matrix
    float light[4];    // world light direction, .w = ambient (VU1 qword 17)
    uint32_t pc, itop, role, layer;
    bool drawn;        // the game kicked something for it
    std::vector<uint8_t> valpha;  // per vertex: the kicked alpha and 255 if its primitive blends
                                  // (empty: the kick was not 1:1 with the mesh)
};

struct Frame {
    float cam[16];     // world -> GS screen, column-major (columns vf4..vf7)
    std::vector<Batch> batches;  // index = the batch(i) numbers of this frame
    std::vector<Mesh> meshes;    // meshes first seen this frame
};

extern bool g_on;  // EOD_MODERN=1: the modern renderer is available
// It draws (true) or the original GS draws everything (false); switched live
// with F9 or L1+R1+R3, read by the producer once per frame.
bool live();
void toggle();
void disable();  // the modern renderer cannot run here: Original from now on

// The frame buffer the replaced draws of this frame went to (GS thread).
struct Target {
    bool valid;
    uint32_t fbp, fbw, fpsm;
    uint64_t xyoffset, scissor;
};
extern Target g_target;

// GS thread
void gs_packet(int path);
void gs_batch(int32_t index);
// true: this draw belongs to a replaced batch (the caller skips it); its texture is noted
// alpha: the draw's vertex alpha (0x80 = 1.0; the car damage layer fades in with it)
bool gs_replaced(const gs::Env &e, uint32_t alpha);
void gs_draw(Frame *f);  // takes ownership

// Producer thread: queue the commands above in GS order
void post_batch(int32_t index);
void post_draw(Frame *f);

}  // namespace modern
