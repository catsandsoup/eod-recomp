// gs_hw_stub.cpp - no hardware renderer outside Windows.
#ifndef _WIN32
#include <cstdio>

#include "gs_hw.h"

namespace gs {
namespace hw {

bool g_on = false;

bool start(bool) { return false; }
void draw_triangle(const Env &, const Vertex &, const Vertex &, const Vertex &) {}
void draw_sprite(const Env &, const Vertex &, const Vertex &) {}
void draw_line(const Env &, const Vertex &, const Vertex &) {}
void draw_point(const Env &, const Vertex &) {}
void vram_written(uint32_t, uint32_t, uint32_t, int, int, int, int) {}
void vram_write_complete() {}
void vram_needed(uint32_t, uint32_t, uint32_t, int, int, int, int, const char *) {}
ClutSnapshot *clut_snapshot(uint32_t, uint32_t, uint32_t, int, int, int, int) { return nullptr; }
const uint8_t *clut_snapshot_page(ClutSnapshot *, uint32_t) { return nullptr; }
void clut_snapshot_free(ClutSnapshot *) {}
void present(bool) {}
void redraw() {}
bool read_display(std::vector<uint32_t> &, int &, int &) { return false; }
void bind_thread() {}
void vblank() {}
void reset() {}
void finish() {}
void stats_line(char *buf, size_t size) { snprintf(buf, size, "software renderer"); }

}  // namespace hw
}  // namespace gs
#endif
