// Display aspect for the Mac renderer's per-frame composition, and an opt-in
// projection trace.
//
// Widescreen follows upstream 1.0 (src/game_patches.cpp): the game's own
// display mode, projected at the drawable or selected fixed ratio (auto follows the game);
// movies stay 4:3. This fork's earlier race-only wide camera
// (docs/NATIVE_CAMERA_ASPECT.md) is retired. The aspect the GS command stream
// carries per frame is the one game_patches_vblank reports through
// platform_set_aspect.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>
#include "battle_driver.h"
#include "core/config.h"
#include "core/cpu.h"
#include "core/memory.h"
#include "platform/display_rect.h"
#include "platform/platform.h"
#include "platform/presentation.h"

extern "C" void f_0034E530_orig();
extern uint64_t kernel_vblank_count();
struct LodViewCounts { unsigned long long frames, cars, low0, records; };
extern LodViewCounts g_lod_view;  // src/hle_lod.cpp

int eod_presentation_aspect() {
    const int requested = presentation::requested.load(std::memory_order_relaxed);
    return requested >= 0 ? requested : display_aspect(g_config.aspect, presentation::active_drawable());
}

// Camera view of player 0 (EOD_CAMERA_TRACE, diagnostic). The physcam record of player p is
// 0x48CB80 + 0x1B8 p (0x1C2A4C clears four); +0x10 is the requested view (0x1D41B0 writes it),
// +0xC the view in effect (0x1C31A0 copies +0x10 there each frame and switches on it in 0x1D10C0).
// Names: the game's own view-name table 0x108D28 (strings "CameraNames:Cockpit".. 0x3EC6C8..0x3EC7D0
// mapped by 0x109288, then the replay names at 0x3EC4B0..0x3EC4E8). Values past 18 are the physcam
// types the race never offers as a view (0x13 menu frame 0x36E568, 0x1F front end 0x3698A8, ...).
static const char *camera_view_name(uint32_t v) {
    static const char *const kNames[] = {"Cockpit", "Bumper", "Chassis", "Rear", "NearChase", "Chase", "FarChase",
                                         "RearChase", "Overhead", "TV", "Aerial", "Blimp", "Custom", "Finish",
                                         "Pitfork", "Pit Box", "Pit Stop", "Swoop", "Twirl"};
    return v < sizeof kNames / sizeof kNames[0] ? kNames[v] : nullptr;
}
static void camera_view_trace() {
    static uint32_t last_view = 0xFFFFFFFFu, last_race = 0xFFFFFFFFu;
    static unsigned changes;
    const uint32_t view = ee_r32(0x48CB8C), race = ee_r32(0x3C411C);  // race: 0x3C4118 + 4, player 0's chosen view
    if (view == last_view && race == last_race) return;
    if (last_view != 0xFFFFFFFFu && g_lod_view.frames) {
        // what the car batch (0x218E68) drew while the previous view was in effect
        const char *pn = camera_view_name(last_view);
        std::fprintf(stderr,
                     "[camera] view %u (%s) held %llu car batches: %llu visual cars, %llu on Low0 (+0x54C = 0), %llu draw "
                     "records added by 0x218E68\n",
                     last_view, pn ? pn : "physcam type", g_lod_view.frames, g_lod_view.cars, g_lod_view.low0,
                     g_lod_view.records);
    }
    g_lod_view = LodViewCounts{};
    last_view = view, last_race = race;
    if (++changes > 4096) return;
    const char *vn = camera_view_name(view), *rn = camera_view_name(race);
    std::fprintf(stderr, "[camera] view %u (%s) requested %u race_camera %u (%s) change %u at vblank %llu\n", view,
                 vn ? vn : "physcam type", ee_r32(0x48CB90), race, rn ? rn : "-", changes,
                 (unsigned long long)kernel_vblank_count());
}

// EOD_CAMERA_KEYS="<vblank>:<hex key>,..." (test only): pushes each key once through the game's own
// key queue 0x33C168 (KeyPush: 0x4B5778, 4 halfwords, (port << 12) | code; read by 0x33C4E8 /
// 0x33C4F8). In a race 0x113B28 hands the key to 0x113918, which passes it on to 0x1151B0 (via
// 0x115AA8) for player 0's view 0x3C411C, applied by 0x113D90 -> 0x1D41B0 each frame. 0x1151B0's
// keys are the development keyboard's (no retail code queues them; the pad queues only 0x1B,
// 0x43, 0x53, 0x54, 0x58 through 0x333380): 0x374 Rear (3), 0x375 Chase (5), 0x376 FarChase (6),
// 0x377 RearChase (7), 0x378 Overhead (8), 0x37A TV (9); 0x170 cycles {0, 1}, 0x172 {4, 5, 6},
// 0x173 {9, 7, 8} (tables 0x3EDC00..0x3EDC2C). The pad's "view" input (Triangle) is pad event 0
// (0x184280 -> 0x179608 -> 0x1136C0), which cycles 0x3EDC30 = {1, 0, 4, 5, 2}.
extern "C" void f_0033C168(void);
static void camera_keys() {
    static std::vector<std::pair<uint64_t, uint32_t>> keys = [] {
        std::vector<std::pair<uint64_t, uint32_t>> k;
        const char *e = std::getenv("EOD_CAMERA_KEYS");
        for (const char *p = e; p && *p;) {
            char *end;
            const uint64_t at = std::strtoull(p, &end, 10);
            if (*end != ':') break;
            const uint32_t key = (uint32_t)std::strtoul(end + 1, &end, 16);
            k.emplace_back(at, key);
            p = *end ? end + 1 : end;
        }
        return k;
    }();
    static size_t next = 0;
    while (next < keys.size() && kernel_vblank_count() >= keys[next].first) {
        const EEState saved = g_ee;
        g_ee.r[4].ud[0] = keys[next].second;
        f_0033C168();
        g_ee = saved;
        std::fprintf(stderr, "[camera] key %03x queued through 0x33C168 at vblank %llu\n", keys[next].second,
                     (unsigned long long)kernel_vblank_count());
        next++;
    }
}

namespace {
struct FreeCamState {
    bool active = false;
    uint64_t last_vblank = 0;
    float pos[3] = {0.0f, 15.0f, 0.0f};
    float yaw = 0.0f;
    float pitch = -0.25f;
};
FreeCamState g_free_cam{};

static float cam_rf(uint32_t addr) {
    const uint32_t u = ee_r32(addr);
    float v;
    std::memcpy(&v, &u, sizeof(v));
    return v;
}
static void cam_wf(uint32_t addr, float v) {
    uint32_t u;
    std::memcpy(&u, &v, sizeof(u));
    ee_w32(addr, u);
}

static void apply_free_cam_override() {
    if (!battle_driver_is_free_cam()) {
        g_free_cam.active = false;
        return;
    }
    constexpr uint32_t kCameraRotation = 0x3E57A8;
    constexpr uint32_t kCameraPosition = 0x3E57CC;
    const uint64_t vb = kernel_vblank_count();
    if (!g_free_cam.active) {
        g_free_cam.active = true;
        g_free_cam.last_vblank = vb;
        g_free_cam.pos[0] = cam_rf(kCameraPosition + 0);
        g_free_cam.pos[1] = cam_rf(kCameraPosition + 4);
        g_free_cam.pos[2] = cam_rf(kCameraPosition + 8);
        const float fx = cam_rf(kCameraRotation + 24);
        const float fy = cam_rf(kCameraRotation + 28);
        const float fz = cam_rf(kCameraRotation + 32);
        if (std::isfinite(fx) && std::isfinite(fy) && std::isfinite(fz)) {
            g_free_cam.yaw = std::atan2(fx, fz);
            g_free_cam.pitch = std::clamp(std::asin(std::clamp(fy, -0.99f, 0.99f)), -1.45f, 1.45f);
        }
    }
    if (vb != g_free_cam.last_vblank) {
        const float dt = std::clamp(static_cast<float>(vb - g_free_cam.last_vblank) * (1.0f / 60.0f), 0.0f, 0.1f);
        g_free_cam.last_vblank = vb;
        HostPadState pad{};
        platform_get_pad(0, &pad);
        auto axis = [](uint8_t v) -> float {
            const float f = (static_cast<float>(v) - 128.0f) / 127.0f;
            return std::fabs(f) < 0.15f ? 0.0f : f;
        };
        float move_f = -axis(pad.ly);
        float move_r = axis(pad.lx);
        if (pad.buttons & PAD_UP) move_f += 1.0f;
        if (pad.buttons & PAD_DOWN) move_f -= 1.0f;
        if (pad.buttons & PAD_RIGHT) move_r += 1.0f;
        if (pad.buttons & PAD_LEFT) move_r -= 1.0f;
        float move_u = 0.0f;
        if (pad.buttons & PAD_R1) move_u += 1.0f;
        if (pad.buttons & PAD_L1) move_u -= 1.0f;

        float look_x = axis(pad.rx);
        float look_y = -axis(pad.ry);
        if (pad.buttons & PAD_CIRCLE) look_x += 0.8f;
        if (pad.buttons & PAD_SQUARE) look_x -= 0.8f;
        if (pad.buttons & PAD_TRIANGLE) look_y += 0.8f;
        if (pad.buttons & PAD_CROSS) look_y -= 0.8f;

        g_free_cam.yaw += look_x * 2.1f * dt;
        g_free_cam.pitch = std::clamp(g_free_cam.pitch + look_y * 1.6f * dt, -1.45f, 1.45f);

        const float cy = std::cos(g_free_cam.yaw), sy = std::sin(g_free_cam.yaw);
        const float cp = std::cos(g_free_cam.pitch), sp = std::sin(g_free_cam.pitch);
        const float fwd[3] = {sy * cp, sp, cy * cp};
        const float right[3] = {cy, 0.0f, -sy};
        const float speed = (pad.buttons & PAD_R2) ? 90.0f : ((pad.buttons & PAD_L2) ? 12.0f : 35.0f);
        for (int i = 0; i < 3; ++i) {
            g_free_cam.pos[i] += (fwd[i] * move_f + right[i] * move_r) * speed * dt;
        }
        g_free_cam.pos[1] += move_u * speed * dt;
    }

    const float cy = std::cos(g_free_cam.yaw), sy = std::sin(g_free_cam.yaw);
    const float cp = std::cos(g_free_cam.pitch), sp = std::sin(g_free_cam.pitch);
    const float fx = sy * cp, fy = sp, fz = cy * cp;
    const float rx = cy, ry = 0.0f, rz = -sy;
    const float ux = -sy * sp, uy = cp, uz = -cy * sp;
    // Rows of camera rotation matrix R at 0x3E57A8 (right, up, forward) and position at 0x3E57CC
    cam_wf(kCameraRotation + 0, rx);  cam_wf(kCameraRotation + 4, ry);  cam_wf(kCameraRotation + 8, rz);
    cam_wf(kCameraRotation + 12, ux); cam_wf(kCameraRotation + 16, uy); cam_wf(kCameraRotation + 20, uz);
    cam_wf(kCameraRotation + 24, fx); cam_wf(kCameraRotation + 28, fy); cam_wf(kCameraRotation + 32, fz);
    cam_wf(kCameraPosition + 0, g_free_cam.pos[0]);
    cam_wf(kCameraPosition + 4, g_free_cam.pos[1]);
    cam_wf(kCameraPosition + 8, g_free_cam.pos[2]);
}
} // namespace

// Diagnostic only (EOD_CAMERA_TRACE): the native projection calculation is unchanged.
extern "C" void f_0034E530() {
    static const bool trace=std::getenv("EOD_CAMERA_TRACE") != nullptr;
    if (trace) camera_view_trace();
    if (trace) camera_keys();
    static unsigned count;
    static float last[3];
    const bool changed = g_ee.fpr[12].f != last[0] || g_ee.fpr[13].f != last[1] || g_ee.fpr[14].f != last[2];
    if (trace && (count < 16 || (changed && count < 512))) {  // every view change (near/far/fov) after the first 16
        ++count;
        last[0] = g_ee.fpr[12].f, last[1] = g_ee.fpr[13].f, last[2] = g_ee.fpr[14].f;
        float xscale,base;uint32_t xb=ee_r32(0x3dd658),bb=ee_r32(0x3dd65c);
        std::memcpy(&xscale,&xb,sizeof xscale);std::memcpy(&base,&bb,sizeof base);
        std::fprintf(stderr,"[camera] projection ra=%08x near=%.9g far=%.9g fov=%.9g xscale=%.9g base=%.9g mode=%u wide=%u letterbox=%u\n",
            g_ee.r[31].uw[0],double(g_ee.fpr[12].f),double(g_ee.fpr[13].f),double(g_ee.fpr[14].f),
            double(xscale),double(base),ee_r32(0x3dd660),ee_r32(0x3dd64c),ee_r32(0x3dd650));
    }
    f_0034E530_orig();
    apply_free_cam_override();
}
