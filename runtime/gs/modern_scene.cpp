// modern_scene.cpp - GS-thread side of EOD_MODERN (modern_scene.h).
#include "modern_scene.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gs_env.h"
#include "gs_hw.h"
#include "gs_state.h"

namespace modern {

bool g_on = [] {
    const char *v = getenv("EOD_MODERN");
    return v && *v && strcmp(v, "0") != 0;
}();

Target g_target;

namespace {
std::atomic<bool> g_live{true};
}
bool live() { return g_on && g_live.load(std::memory_order_relaxed); }
void disable() {
    if (g_live.exchange(false)) fprintf(stderr, "[modern] renderer unavailable: original\n");
}
void toggle() {
    if (!g_on) return;
    const bool on = !g_live.load();
    g_live = on;
    fprintf(stderr, "[modern] renderer: %s\n", on ? "modern" : "original");
}

namespace {
int g_path;
int32_t g_batch = -1;   // the replaced batch whose kicks are being processed
int32_t g_noted = -1;   // its texture was handed to the backend
}  // namespace

void gs_packet(int path) { g_path = path; }

void gs_batch(int32_t index) { g_batch = index; }

bool gs_replaced(const gs::Env &e, uint32_t alpha) {
    if (g_path != 1 || g_batch < 0) return false;
    g_target = {true, e.fbp, e.fbw, e.fpsm, gs::st.ctx[0].xyoffset, gs::st.ctx[0].scissor};
    if (g_noted != g_batch && gs::hw::g_on) {  // the batch's texture and blend state (first draw)
        gs::hw::modern_texture(g_batch, e);
        g_noted = g_batch;
    }
    if (gs::hw::g_on) gs::hw::modern_alpha(g_batch, alpha);
    return true;
}

void gs_draw(Frame *f) {
    g_batch = g_noted = -1;
    if (gs::hw::g_on)  // (new meshes go to the cache even when nothing is drawn)
        gs::hw::modern_draw(f);
    else
        delete f;
    g_target.valid = false;
}

}  // namespace modern
