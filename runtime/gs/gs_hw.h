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
// The CPU is about to read GS memory there (transfer source, CLUT load):
// rendered contents are copied back to gs::vram first.
void vram_needed(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h);

// Composes and presents the displayed image (GS thread, at vblank when the
// display changed).  vsync: wait for the monitor.
void present(bool vsync);
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
// Statistics line for the log (draw calls, uploads, conversions per frame).
void stats_line(char *buf, size_t size);

}  // namespace hw
}  // namespace gs
