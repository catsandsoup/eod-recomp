// modern_scene.cpp - GS-thread side of EOD_MODERN (modern_scene.h).
#include "modern_scene.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gs_env.h"
#include "gs_hw.h"
#include "gs_state.h"
#include "../core/config.h"

namespace modern {

// available on Windows (the d3d11 backend draws it) unless EOD_MODERN=0; EOD_MODERN=1 also
// switches it on at start. Whether it draws is the config's ModernRenderer (Graphics panel).
bool g_on = [] {
#ifdef _WIN32
    const char *v = getenv("EOD_MODERN");
    return !(v && strcmp(v, "0") == 0);
#else
    return false;
#endif
}();

Target g_target;

namespace {
std::atomic<bool> g_failed{false};
void env_start() {
    static bool once = [] {
        const char *v = getenv("EOD_MODERN");
        if (v && *v && strcmp(v, "0") != 0) g_config.modern_renderer = true;
        return true;
    }();
    (void)once;
}
}  // namespace
bool available() { return g_on && gs::hw::g_on && !g_failed.load(std::memory_order_relaxed); }
bool live() {
    if (!available()) return false;
    env_start();
    return g_config.modern_renderer;
}
void disable() {
    if (!g_failed.exchange(true)) fprintf(stderr, "[modern] renderer unavailable: original\n");
}
void toggle() {
    if (!available()) return;
    env_start();
    g_config.modern_renderer = !g_config.modern_renderer;
    fprintf(stderr, "[modern] renderer: %s\n", g_config.modern_renderer ? "modern" : "original");
}

namespace {
int g_path;
int32_t g_batch = -1;   // the replaced batch whose kicks are being processed
int32_t g_noted = -1;   // its texture was handed to the backend
}  // namespace

void gs_packet(int path) { g_path = path; }

void gs_batch(int32_t index) { g_batch = index; }

bool gs_replaced(const gs::Env &e, uint32_t alpha) {
    if (g_path != 1 || g_batch == -1) return false;
    if (g_batch == kSuppressed) return true;
    g_target = {true, e.fbp, e.fbw, e.fpsm, gs::st.ctx[0].xyoffset, gs::st.ctx[0].scissor, e.zbp, e.zpsm};
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
