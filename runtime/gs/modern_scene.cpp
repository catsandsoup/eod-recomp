// modern_scene.cpp - GS-thread side of EOD_MODERN (modern_scene.h).
#include "modern_scene.h"
#include "../platform/platform.h"
#include "../render/native_renderer.h"
#include "../ui/overlay.h"
#ifdef EOD_PLUME
#include "../render/plume_device.h"
#endif
#include "core/trace.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "gs_env.h"
#include "gs_hw.h"
#include "gs_state.h"
#include "../core/config.h"

namespace modern {

// available with a hardware backend that draws it (Direct3D 11 or Vulkan) unless EOD_MODERN=0; EOD_MODERN=1 also
// switches it on at start. Whether it draws is the config's ModernRenderer (Graphics panel).
#ifndef EOD_R7_RELEASE
bool g_on = [] {
#if defined(_WIN32) || defined(EOD_VULKAN)
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
bool available() {
#ifdef EOD_PLUME
    // The legacy GS-traffic pass would claim the same batches before native Plume can draw them.
    if (native::enabled() && plume_device::render_device()) return false;
#endif
    return g_on && gs::hw::g_on && !g_failed.load(std::memory_order_relaxed);
}
bool live() {
    if (!available()) return false;
    env_start();
    return g_config.modern_renderer;
}
#endif
bool look_switch_available() { return native::look_available() || available(); }
#ifndef EOD_R7_RELEASE
void disable() {
    if (!g_failed.exchange(true)) fprintf(stderr, "[modern] renderer unavailable: original\n");
}

#endif
namespace {
std::optional<RendererLook> toggle_look() {
    if (native::look_available())
        return native::toggle_look() ? RendererLook::Modern : RendererLook::Classic;
#ifdef EOD_R7_RELEASE
    return std::nullopt;
#else
    if (!available()) return std::nullopt;
    env_start();
    const bool enabled = g_config.modern_renderer.toggle();
    fprintf(stderr, "[modern] renderer: %s\n", enabled ? "modern" : "original");
    return enabled ? RendererLook::Modern : RendererLook::Original;
#endif
}
}  // namespace

void toggle() { (void)toggle_look(); }

void toggle_from_input(const char *source) {
    if (const auto look = toggle_look()) {
        std::fprintf(stderr, "[look] %s selected %s\n", source, overlay_renderer_look_label(*look));
        platform_show_hud_banner(overlay_renderer_look_label(*look));
        overlay_show_renderer_look(*look);
    }
}

#ifndef EOD_R7_RELEASE
namespace {
int g_path;
int32_t g_batch = -1;   // the replaced batch whose kicks are being processed
int32_t g_noted = -1;   // its texture was handed to the backend
}  // namespace

void gs_packet(int path) { g_path = path; }

bool gs_path1_has_replacement() { return g_batch != -1; }

void gs_batch(int32_t index) {
    g_batch = index;
    gs::attr_mark();
}

static bool gs_replaced_(const gs::Env &e, uint32_t alpha);
bool gs_replaced(const gs::Env &e, uint32_t alpha) {
    const bool r = gs_replaced_(e, alpha);
    if (gs::g_stale_prim && !r) {
        if (gs::g_stale_visible++ < 8)
            fprintf(stderr, "[skipproof] drawn stale primitive: path %d tme %u abe %u fbp %x fbmsk %x tfx %d tcc %u fge %u zbp %x zmsk %u\n", g_path,
                    e.tme, e.abe, e.fbp, e.fbmsk, e.tfx, e.tcc, e.fge, e.zbp, e.zmsk);
    }
    gs::g_stale_prim = false;
    return r;
}
static bool gs_replaced_(const gs::Env &e, uint32_t alpha) {
    if (g_path != 1 || !gs_path1_has_replacement()) return false;
    if (g_batch == kSuppressed) return true;
    g_target = {true, e.fbp, e.fbw, e.fpsm, gs::st.ctx[0].xyoffset, gs::st.ctx[0].scissor, e.zbp, e.zpsm};
    if (g_noted != g_batch && gs::hw::g_on) {  // the batch's texture and blend state (first draw)
        gs::hw::modern_texture(g_batch, e);
        g_noted = g_batch;
    }
    if (gs::hw::g_on) gs::hw::modern_alpha(g_batch, alpha);
    return true;
}

void gs_batch_state(int32_t index) {
    gs::taint_attributes();
    if (index < 0 || !gs::hw::g_on) return;
    const gs::Env &e = gs::current_env();
    g_target = {true, e.fbp, e.fbw, e.fpsm, gs::st.ctx[0].xyoffset, gs::st.ctx[0].scissor, e.zbp, e.zpsm};
    if (g_noted != index) {
        gs::hw::modern_texture(index, e);
        g_noted = index;
    }
}

void gs_draw(Frame *f) {
    EOD_ZONE("modern.gs_draw");
    g_batch = g_noted = -1;
    if (gs::hw::g_on)  // (new meshes go to the cache even when nothing is drawn)
        gs::hw::modern_draw(f);
    else
        delete f;
    g_target.valid = false;
}

#endif

}  // namespace modern
