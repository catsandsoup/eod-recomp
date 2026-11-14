// gs_hw_select.cpp - Windows builds with both hardware GS backends: Vulkan (the
// default) and Direct3D 11. Each backend lives in its own namespace
// (gs::hw::vulkan, gs::hw::d3d11); start() picks one from Renderer= and every
// other entry point forwards to it.
#ifdef EOD_HW_SELECT
#include <string>

#include "../core/config.h"
#include "gs_hw.h"

namespace gs {
namespace hw {

bool g_on;

#define EOD_HW_DECLARE                                                                                   \
    bool start(bool with_window);                                                                      \
    void draw_triangle(const Env &e, const Vertex &a, const Vertex &b, const Vertex &c);               \
    void draw_sprite(const Env &e, const Vertex &a, const Vertex &b);                                  \
    void draw_line(const Env &e, const Vertex &a, const Vertex &b);                                    \
    void draw_point(const Env &e, const Vertex &a);                                                    \
    void vram_written(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h);             \
    void vram_needed(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h, const char *why); \
    void present(bool vsync);                                                                          \
    void redraw();                                                                                     \
    bool read_display(std::vector<uint32_t> &rgba, int &w, int &h);                                    \
    void bind_thread();                                                                                \
    void vblank();                                                                                     \
    void reset();                                                                                      \
    void finish();                                                                                     \
    void stats_line(char *buf, size_t size);                                                           \
    void modern_texture(int32_t batch, const Env &e);                                                  \
    void modern_draw(modern::Frame *f);                                                                \
    void modern_alpha(int32_t batch, uint32_t alpha);

namespace d3d11 {
EOD_HW_DECLARE
}
namespace vulkan {
EOD_HW_DECLARE
struct ClutSnapshot;
void vram_write_complete();
ClutSnapshot *clut_snapshot(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h);
const uint8_t *clut_snapshot_page(ClutSnapshot *s, uint32_t page);
void clut_snapshot_free(ClutSnapshot *s);
}

namespace {
bool g_vk;  // the Vulkan backend is the one running
}

bool start(bool with_window) {
    const std::string &r = g_config.renderer;
    if (r == "vulkan") {
        g_vk = true;
        if (vulkan::start(with_window)) return true;
        // a machine without a working Vulkan driver keeps hardware rendering
        g_vk = false;
        fprintf(stderr, "vulkan is not available - using d3d11\n");
    }
    g_vk = false;
    return d3d11::start(with_window);
}

#define EOD_HW_FORWARD(call) (g_vk ? vulkan::call : d3d11::call)

void draw_triangle(const Env &e, const Vertex &a, const Vertex &b, const Vertex &c) { EOD_HW_FORWARD(draw_triangle(e, a, b, c)); }
void draw_sprite(const Env &e, const Vertex &a, const Vertex &b) { EOD_HW_FORWARD(draw_sprite(e, a, b)); }
void draw_line(const Env &e, const Vertex &a, const Vertex &b) { EOD_HW_FORWARD(draw_line(e, a, b)); }
void draw_point(const Env &e, const Vertex &a) { EOD_HW_FORWARD(draw_point(e, a)); }
void vram_written(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h) {
    EOD_HW_FORWARD(vram_written(bp, bw, psm, x, y, w, h));
}
void vram_write_complete() {
    if (g_vk) vulkan::vram_write_complete();
}
void vram_needed(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h, const char *why) {
    EOD_HW_FORWARD(vram_needed(bp, bw, psm, x, y, w, h, why));
}
ClutSnapshot *clut_snapshot(uint32_t bp, uint32_t bw, uint32_t psm, int x, int y, int w, int h) {
    return g_vk ? (ClutSnapshot *)vulkan::clut_snapshot(bp, bw, psm, x, y, w, h) : nullptr;
}
const uint8_t *clut_snapshot_page(ClutSnapshot *s, uint32_t page) {
    return g_vk ? vulkan::clut_snapshot_page((vulkan::ClutSnapshot *)s, page) : nullptr;
}
void clut_snapshot_free(ClutSnapshot *s) {
    if (g_vk) vulkan::clut_snapshot_free((vulkan::ClutSnapshot *)s);
}
void present(bool vsync) { EOD_HW_FORWARD(present(vsync)); }
void redraw() { EOD_HW_FORWARD(redraw()); }
bool read_display(std::vector<uint32_t> &rgba, int &w, int &h) { return EOD_HW_FORWARD(read_display(rgba, w, h)); }
void bind_thread() { EOD_HW_FORWARD(bind_thread()); }
void vblank() { EOD_HW_FORWARD(vblank()); }
void reset() { EOD_HW_FORWARD(reset()); }
void finish() { EOD_HW_FORWARD(finish()); }
void stats_line(char *buf, size_t size) { EOD_HW_FORWARD(stats_line(buf, size)); }
void modern_texture(int32_t batch, const Env &e) { EOD_HW_FORWARD(modern_texture(batch, e)); }
void modern_draw(modern::Frame *f) { EOD_HW_FORWARD(modern_draw(f)); }
void modern_alpha(int32_t batch, uint32_t alpha) { EOD_HW_FORWARD(modern_alpha(batch, alpha)); }

bool vulkan_active() { return g_on && g_vk; }

}  // namespace hw
}  // namespace gs
#endif  // EOD_HW_SELECT
