// scene_tap.cpp - see scene_tap.h.
#include "scene_tap.h"

#include <cstdlib>
#include <cstring>
#include <mutex>

#include "../core/memory.h"

extern uint64_t kernel_vblank_count();

namespace scene_tap {
namespace {

bool g_on = [] {
    const char *e = std::getenv("EOD_SCENE_TAP");
    return e && *e && *e != '0';
}();

Frame g_building;
Frame g_published;
std::mutex g_mutex;
bool g_have = false;

float rf(uint32_t a) {
    const uint32_t u = MEM_LW(a);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
void read4(uint32_t a, float out[4]) {
    for (int i = 0; i < 4; ++i) out[i] = rf(a + 4u * (uint32_t)i);
}

constexpr uint32_t kCameraRotation = 0x3E57A8, kCameraPosition = 0x3E57CC;
constexpr uint32_t kViewport = 0x4D7C20, kNear = 0x4D7C10, kFar = 0x4D7C14, kFovScale = 0x4D7C1C;
constexpr uint32_t kAspect = 0x3DD658, kViewIndex = 0x3D1A78;
constexpr uint32_t kIdentityPacket = 0x4D7C50;  // 0x35E018(0): the identity matrix packet

void close_view() {
    if (!g_building.views.empty()) {
        View &v = g_building.views.back();
        v.object_count = (uint32_t)g_building.objects.size() - v.first_object;
    }
}

}  // namespace

bool enabled() { return g_on; }
void set_enabled(bool on) { g_on = on; }

void begin_view() {
    if (!g_on) return;
    // a new vblank starts a new frame: publish the previous one
    const uint64_t vb = kernel_vblank_count();
    if (vb != g_building.vblank && (!g_building.views.empty() || !g_building.objects.empty())) end_frame();
    g_building.vblank = vb;
    close_view();
    View v{};
    v.index = (int32_t)MEM_LW(kViewIndex);
    for (int i = 0; i < 9; ++i) v.rotation[i] = rf(kCameraRotation + 4u * (uint32_t)i);
    for (int i = 0; i < 3; ++i) v.position[i] = rf(kCameraPosition + 4u * (uint32_t)i);
    read4(kViewport, v.viewport);
    v.near_z = rf(kNear);
    v.far_z = rf(kFar);
    v.fov_scale = rf(kFovScale);
    v.aspect = rf(kAspect);
    v.first_object = (uint32_t)g_building.objects.size();
    g_building.views.push_back(v);
}

void object(uint32_t record, int32_t texture_key, int32_t layer) {
    if (!g_on) return;
    Object o{};
    o.batch = MEM_LW(record + 0x00);
    o.clip = MEM_LW(record + 0x08);
    o.matrix_packet = MEM_LW(record + 0x0C);
    o.material_packet = MEM_LW(record + 0x10);
    o.texture_key = texture_key;
    o.layer = layer;
    uint32_t packet = o.matrix_packet;
    if (packet == 0) packet = MEM_LW(kIdentityPacket);
    if (packet) {
        // matrix packet (0x35CD28): 3 marker words + UNPACK, then TOPS+0..6 as 7 quadwords
        for (int row = 0; row < 3; ++row)
            for (int c = 0; c < 4; ++c) o.model[row * 4 + c] = rf(packet + 0x10 + 16u * (uint32_t)c + 4u * (uint32_t)row);
        read4(packet + 0x50, o.colour_a);
        read4(packet + 0x60, o.colour_b);
        read4(packet + 0x70, o.light);
    } else {
        o.model[0] = o.model[5] = o.model[10] = 1.0f;
    }
    g_building.objects.push_back(o);
}

const Object *last_object() { return g_on && !g_building.objects.empty() ? &g_building.objects.back() : nullptr; }

void end_frame() {
    if (!g_on) return;
    close_view();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::swap(g_published, g_building);
        g_have = true;
    }
    g_building.views.clear();
    g_building.objects.clear();
}

bool latest(Frame &out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_have) return false;
    out = g_published;
    return true;
}

}  // namespace scene_tap
