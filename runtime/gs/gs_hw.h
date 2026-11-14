// gs_hw.h - hardware (Direct3D 11) GS renderer interface.
//
// With the hardware renderer the GS front end (GIF parsing, registers,
// vertex queue, setup_env) is unchanged; primitives, VRAM transfers and the
// display go to the backend instead of the software rasteriser.  The CPU copy
// of GS memory (gs::vram) stays authoritative for everything the GPU hasn't
// drawn; the backend tracks which 8KB pages its render targets own and
// copies between the two when the game reads memory in another way than it
// was written.
#pragma once
#include <cstdint>
#include <vector>

#include "gs_env.h"
#include "gs_state.h"
#include "modern_scene.h"

namespace gs {
namespace hw {

extern bool g_on;  // the hardware renderer is active

// Creates the device (and the swap chain when there is a window).  false: use
// the software renderer.  Call before the GS thread starts.
bool start(bool with_window);

// Primitives of the current state (g_env, already set up for them).
void draw_triangle(const Env &e, const Vertex &a, const Vertex &b, const Vertex &c);
void draw_sprite(const Env &e, const Vertex &a, const Vertex &b);
void draw_line(const Env &e, const Vertex &a, const Vertex &b);
void draw_point(const Env &e, const Vertex &a);

// The CPU is about to write these actual buffered pixels atomically. For a
// host transfer this is each supplied chunk, not its declared TRXREG extent.
void vram_written(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h);
// All prepared CPU bytes are now supplied; upload proven complete storage
// blocks into retained GPU owners without reading their untouched pixels back.
#if defined(_WIN32) && !defined(EOD_VULKAN)
inline void vram_write_complete() {}  // Direct3D 11 keeps its synchronous uploads
#else
void vram_write_complete();
#endif
// The CPU is about to read GS memory there (transfer source, CLUT load):
// rendered contents are copied back to gs::vram first.
void vram_needed(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h, const char *why = "transfer");

// CLUT loads without a GPU wait: a snapshot of the GS memory pages under a CLUT
// load's source rectangle as they are at this command position. nullptr: the
// rectangle's bytes in gs::vram are final now (vram_needed semantics; read them
// directly). Otherwise the GPU part is copied asynchronously and
// clut_snapshot_page() returns an 8 KB page image (waiting on first use) with
// exactly the bytes a synchronous readback would have left in gs::vram then.
struct ClutSnapshot;
#if defined(_WIN32) && !defined(EOD_VULKAN)
inline ClutSnapshot *clut_snapshot(uint32_t, uint32_t, uint32_t, int, int, int, int) { return nullptr; }
inline const uint8_t *clut_snapshot_page(ClutSnapshot *, uint32_t) { return nullptr; }
inline void clut_snapshot_free(ClutSnapshot *) {}
#else
ClutSnapshot *clut_snapshot(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h);
const uint8_t *clut_snapshot_page(ClutSnapshot *s, uint32_t page);  // nullptr: page not captured
void clut_snapshot_free(ClutSnapshot *s);
#endif

// Composes and presents the displayed image (GS thread, at vblank when the
// display changed).  vsync: wait for the monitor.
void present(bool vsync);
// Presents the last composed image again (the game is paused in the in-game menu).
void redraw();
// The last composed image (screenshots, replays): RGBA, false if none.
bool read_display(std::vector<uint32_t> &rgba, int &w, int &h);
// The calling thread renders from now on (the GS thread, when there is one).
void bind_thread();
// Per-vblank housekeeping on the rendering thread (screenshot requests).
void vblank();
// GS memory was replaced (replay): forget what the render targets hold.
void reset();
// Submits everything and waits for the GPU (replay timing).
void finish();
// EOD_MODERN (modern_scene.h): the texture and blend state of a replaced batch, and the
// modern pass of a frame into the current frame buffer (takes ownership).
#if defined(_WIN32) || defined(EOD_VULKAN)
void modern_texture(int32_t batch, const Env &e);
void modern_draw(modern::Frame *f);
void modern_alpha(int32_t batch, uint32_t alpha);
#else
inline void modern_alpha(int32_t, uint32_t) {}
inline void modern_texture(int32_t, const Env &) {}
inline void modern_draw(modern::Frame *f) { delete f; }
#endif
#ifdef EOD_HW_SELECT
// Windows: the Vulkan backend (not Direct3D 11) is the one running
bool vulkan_active();
#endif
// Statistics line for the log (draw calls, uploads, conversions per frame).
void stats_line(char *buf, size_t size);

}  // namespace hw
}  // namespace gs
