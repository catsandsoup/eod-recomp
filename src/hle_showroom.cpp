// hle_showroom.cpp - 3D Showroom, Yard Scene Actor Table & Camera Systems Modernization
// Decompiled reverse-engineering structures and modern 360-degree turntable orbit controls.
#include "hle_showroom.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/cpu.h"
#include "core/memory.h"
#include "core/config.h"
#include "core/log.h"
#include "funcs.h"
#include "career_upgrades.h"
#include "platform/platform.h"

extern "C" {
void f_001C31A0_orig(void);
void f_001D2DC8_orig(void);
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

inline bool sane(uint32_t p, uint32_t span) {
    return p && !(p & 3) && p <= EE_RAM_SIZE - span;
}

static ShowroomOrbitState s_orbit_state;

} // namespace

ShowroomOrbitState& showroom_get_orbit_state() {
    return s_orbit_state;
}

void showroom_reset_orbit() {
    s_orbit_state.target_yaw = s_orbit_state.default_yaw;
    s_orbit_state.target_pitch = s_orbit_state.default_pitch;
    s_orbit_state.target_distance = s_orbit_state.default_distance;
    s_orbit_state.yaw = s_orbit_state.default_yaw;
    s_orbit_state.pitch = s_orbit_state.default_pitch;
    s_orbit_state.distance = s_orbit_state.default_distance;
    s_orbit_state.idle_timer = 0.0f;
    s_orbit_state.user_controlling = false;
}

void showroom_on_mouse_drag(float dx, float dy) {
    if (!s_orbit_state.enabled) return;
    s_orbit_state.target_yaw += dx * s_orbit_state.mouse_sensitivity_yaw;
    s_orbit_state.target_pitch -= dy * s_orbit_state.mouse_sensitivity_pitch;
    s_orbit_state.target_pitch = std::clamp(s_orbit_state.target_pitch, s_orbit_state.min_pitch, s_orbit_state.max_pitch);
    s_orbit_state.user_controlling = true;
    s_orbit_state.idle_timer = 0.0f;
}

void showroom_on_mouse_wheel(float delta) {
    if (!s_orbit_state.enabled) return;
    s_orbit_state.target_distance -= delta * s_orbit_state.zoom_speed_wheel;
    s_orbit_state.target_distance = std::clamp(s_orbit_state.target_distance, s_orbit_state.min_distance, s_orbit_state.max_distance);
    s_orbit_state.user_controlling = true;
    s_orbit_state.idle_timer = 0.0f;
}

void showroom_on_play_track(const char* track_name, int blend_flag) {
    (void)blend_flag;
    if (!track_name) return;

    // Check if track belongs to showroom / autoshop / garage inspection modes
    const bool is_showroom_track = (std::strstr(track_name, "buy") != nullptr ||
                                    std::strstr(track_name, "garage") != nullptr ||
                                    std::strstr(track_name, "malt") != nullptr);
    if (!is_showroom_track) return;

    // Active track pose evaluated into 0x0048CBD4
    const uint32_t pose_addr = 0x0048CBD4;
    if (sane(pose_addr, sizeof(CameraDirectorTrackPose))) {
        auto* pose = reinterpret_cast<const CameraDirectorTrackPose*>(ee_ptr(pose_addr));
        Vector3f delta = pose->eye - pose->target;
        float dist = delta.length();
        if (dist > 0.1f) {
            s_orbit_state.center = pose->target;
            s_orbit_state.default_distance = dist;
            s_orbit_state.default_pitch = std::asin(std::clamp(delta.y / dist, -0.99f, 0.99f));
            s_orbit_state.default_yaw = std::atan2(delta.x, delta.z);
            showroom_reset_orbit();
        }
    }
}

void showroom_update_camera(float dt) {
    if (!s_orbit_state.enabled) return;
    if (dt <= 0.0f) dt = 1.0f / 60.0f;

    HostPadState pad;
    platform_get_pad(0, &pad);

    float stick_x = 0.0f;
    float stick_y = 0.0f;
    float zoom_in = 0.0f;
    float zoom_out = 0.0f;

    if (pad.connected) {
        int rx_raw = static_cast<int>(pad.rx) - 128;
        int ry_raw = static_cast<int>(pad.ry) - 128;
        const int kDeadzone = 14;
        if (std::abs(rx_raw) > kDeadzone) {
            stick_x = static_cast<float>(rx_raw > 0 ? rx_raw - kDeadzone : rx_raw + kDeadzone) / (128.0f - kDeadzone);
        }
        if (std::abs(ry_raw) > kDeadzone) {
            stick_y = static_cast<float>(ry_raw > 0 ? ry_raw - kDeadzone : ry_raw + kDeadzone) / (128.0f - kDeadzone);
        }

        if (pad.buttons & PAD_L2) zoom_in = 1.0f;
        else if (pad.pressure[10] > 16) zoom_in = pad.pressure[10] / 255.0f;

        if (pad.buttons & PAD_R2) zoom_out = 1.0f;
        else if (pad.pressure[11] > 16) zoom_out = pad.pressure[11] / 255.0f;

        static bool s_prev_r3 = false;
        bool r3_pressed = (pad.buttons & PAD_R3) && !s_prev_r3;
        s_prev_r3 = (pad.buttons & PAD_R3);
        if (r3_pressed) {
            showroom_reset_orbit();
            s_orbit_state.turntable_enabled = !s_orbit_state.turntable_enabled;
        }
    }

    const bool has_input = (std::abs(stick_x) > 0.01f || std::abs(stick_y) > 0.01f || zoom_in > 0.01f || zoom_out > 0.01f);
    if (has_input) {
        s_orbit_state.target_yaw += stick_x * s_orbit_state.stick_sensitivity_yaw * dt;
        s_orbit_state.target_pitch -= stick_y * s_orbit_state.stick_sensitivity_pitch * dt;
        s_orbit_state.target_distance += (zoom_out - zoom_in) * s_orbit_state.zoom_speed_triggers * dt;
        s_orbit_state.user_controlling = true;
        s_orbit_state.idle_timer = 0.0f;
    } else {
        s_orbit_state.idle_timer += dt;
        if (s_orbit_state.idle_timer > s_orbit_state.recenter_timeout) {
            float diff_yaw = s_orbit_state.default_yaw - s_orbit_state.target_yaw;
            const float pi = static_cast<float>(M_PI);
            while (diff_yaw > pi) diff_yaw -= 2.0f * pi;
            while (diff_yaw < -pi) diff_yaw += 2.0f * pi;
            s_orbit_state.target_yaw += diff_yaw * std::min(1.0f, 1.5f * dt);
            s_orbit_state.target_pitch += (s_orbit_state.default_pitch - s_orbit_state.target_pitch) * std::min(1.0f, 1.5f * dt);
            s_orbit_state.target_distance += (s_orbit_state.default_distance - s_orbit_state.target_distance) * std::min(1.0f, 1.5f * dt);
            if (std::abs(diff_yaw) < 0.01f &&
                std::abs(s_orbit_state.default_pitch - s_orbit_state.target_pitch) < 0.01f &&
                std::abs(s_orbit_state.default_distance - s_orbit_state.target_distance) < 0.05f) {
                s_orbit_state.user_controlling = false;
            }
        }
    }

    s_orbit_state.target_pitch = std::clamp(s_orbit_state.target_pitch, s_orbit_state.min_pitch, s_orbit_state.max_pitch);
    s_orbit_state.target_distance = std::clamp(s_orbit_state.target_distance, s_orbit_state.min_distance, s_orbit_state.max_distance);

    const float kSmoothing = 12.0f;
    s_orbit_state.yaw += (s_orbit_state.target_yaw - s_orbit_state.yaw) * std::min(1.0f, kSmoothing * dt);
    s_orbit_state.pitch += (s_orbit_state.target_pitch - s_orbit_state.pitch) * std::min(1.0f, kSmoothing * dt);
    s_orbit_state.distance += (s_orbit_state.target_distance - s_orbit_state.distance) * std::min(1.0f, kSmoothing * dt);
}

bool showroom_inject_camera_orbit(uint32_t camera_ctx_addr, uint32_t director_addr) {
    (void)director_addr;
    if (!s_orbit_state.enabled) return false;
    if (!sane(camera_ctx_addr, sizeof(RenderCameraContext))) return false;
    auto* ctx = reinterpret_cast<RenderCameraContext*>(ee_ptr(camera_ctx_addr));

    // Modes 10..13 (0xA..0xD) are the showroom / autoshop / garage inspection modes
    const bool is_showroom_mode = (ctx->mode >= 10 && ctx->mode <= 13);
    if (!is_showroom_mode) {
        s_orbit_state.active = false;
        return false;
    }
    s_orbit_state.active = true;

    Vector3f center = s_orbit_state.center;
    if (ctx->target_pos.length_sq() > 0.01f) {
        center = ctx->target_pos;
    } else {
        const uint32_t mgr_ptr = ee_r32(0x00495D80);
        if (sane(mgr_ptr, sizeof(CarMgrTable))) {
            auto* mgr = reinterpret_cast<CarMgrTable*>(ee_ptr(mgr_ptr));
            int slot = ctx->participant_slot;
            if (slot >= 0 && slot < 20 && mgr->actors[slot].active) {
                center = mgr->actors[slot].position + Vector3f(0.0f, 0.65f, 0.0f);
            }
        }
    }

    float cos_p = std::cos(s_orbit_state.pitch);
    float sin_p = std::sin(s_orbit_state.pitch);
    float sin_y = std::sin(s_orbit_state.yaw);
    float cos_y = std::cos(s_orbit_state.yaw);

    Vector3f eye;
    eye.x = center.x + s_orbit_state.distance * sin_y * cos_p;
    eye.y = center.y + s_orbit_state.distance * sin_p;
    eye.z = center.z + s_orbit_state.distance * cos_y * cos_p;

    Vector3f fwd = (center - eye).normalized();
    Vector3f world_up = {0.0f, 1.0f, 0.0f};
    Vector3f right = cross(world_up, fwd).normalized();
    if (right.length_sq() < 1e-4f) {
        world_up = {0.0f, 0.0f, 1.0f};
        right = cross(world_up, fwd).normalized();
    }
    Vector3f up = cross(fwd, right).normalized();

    // 3x4 Camera View Matrix:
    // Row 0: right vector and -dot(right, eye)
    // Row 1: up vector and -dot(up, eye)
    // Row 2: fwd vector and -dot(fwd, eye)
    ctx->view_matrix.m[0][0] = right.x;
    ctx->view_matrix.m[0][1] = right.y;
    ctx->view_matrix.m[0][2] = right.z;
    ctx->view_matrix.m[0][3] = -dot(right, eye);

    ctx->view_matrix.m[1][0] = up.x;
    ctx->view_matrix.m[1][1] = up.y;
    ctx->view_matrix.m[1][2] = up.z;
    ctx->view_matrix.m[1][3] = -dot(up, eye);

    ctx->view_matrix.m[2][0] = fwd.x;
    ctx->view_matrix.m[2][1] = fwd.y;
    ctx->view_matrix.m[2][2] = fwd.z;
    ctx->view_matrix.m[2][3] = -dot(fwd, eye);

    // Update active camera world position at 0x004CD31C for LOD calculation and culling
    const uint32_t cam_world_pos_addr = 0x004CD31C;
    if (sane(cam_world_pos_addr, sizeof(Vector3f))) {
        std::memcpy(ee_ptr(cam_world_pos_addr), &eye, sizeof(Vector3f));
    }

    return true;
}

bool showroom_spawn_yard_car(int slot_id, int vehicle_id, int paint_index, float rot_y) {
    if (slot_id < 0 || slot_id >= 20) return false;
    const uint32_t mgr_ptr = ee_r32(0x00495D80);
    if (!sane(mgr_ptr, sizeof(CarMgrTable))) return false;
    auto* mgr = reinterpret_cast<CarMgrTable*>(ee_ptr(mgr_ptr));

    auto& entry = mgr->actors[slot_id];
    entry.active = 1;
    entry.slot_id = slot_id;
    entry.flags = 0x52;
    entry.vehicle_id = vehicle_id;
    entry.paint_job_index = paint_index;
    if (slot_id >= mgr->active_count) {
        mgr->active_count = slot_id + 1;
    }

    float c = std::cos(rot_y), s = std::sin(rot_y);
    entry.world_transform.m[0][0] = c;
    entry.world_transform.m[0][1] = 0.0f;
    entry.world_transform.m[0][2] = s;
    entry.world_transform.m[0][3] = 0.0f;

    entry.world_transform.m[1][0] = 0.0f;
    entry.world_transform.m[1][1] = 1.0f;
    entry.world_transform.m[1][2] = 0.0f;
    entry.world_transform.m[1][3] = 0.0f;

    entry.world_transform.m[2][0] = -s;
    entry.world_transform.m[2][1] = 0.0f;
    entry.world_transform.m[2][2] = c;
    entry.world_transform.m[2][3] = 0.0f;

    entry.world_transform.m[3][0] = entry.position.x;
    entry.world_transform.m[3][1] = entry.position.y;
    entry.world_transform.m[3][2] = entry.position.z;
    entry.world_transform.m[3][3] = 1.0f;

    // Sync HomeMaster dealer yard bay vehicle ID (home + 0x4BC + 4 * slot_id)
    const uint32_t home_ptr = ee_r32(0x003CDA04);
    if (sane(home_ptr, 0xC04)) {
        ee_w32(home_ptr + 0x4BC + 4 * slot_id, static_cast<uint32_t>(vehicle_id));
    }

    return true;
}

void showroom_update_turntable(float dt) {
    if (!s_orbit_state.turntable_enabled) return;
    s_orbit_state.turntable_angle += s_orbit_state.turntable_speed * dt;
    const float two_pi = 2.0f * static_cast<float>(M_PI);
    while (s_orbit_state.turntable_angle >= two_pi) {
        s_orbit_state.turntable_angle -= two_pi;
    }

    const uint32_t mgr_ptr = ee_r32(0x00495D80);
    if (sane(mgr_ptr, sizeof(CarMgrTable))) {
        auto* mgr = reinterpret_cast<CarMgrTable*>(ee_ptr(mgr_ptr));
        if (mgr->actors[0].active) {
            float c = std::cos(s_orbit_state.turntable_angle);
            float s = std::sin(s_orbit_state.turntable_angle);
            mgr->actors[0].world_transform.m[0][0] = c;
            mgr->actors[0].world_transform.m[0][2] = s;
            mgr->actors[0].world_transform.m[2][0] = -s;
            mgr->actors[0].world_transform.m[2][2] = c;
        }
    }
}

bool showroom_populate_garage_lot() {
    const uint32_t cdb_addr = ee_r32(0x003D9398);
    if (!sane(cdb_addr, sizeof(eod::CDBupgr))) return false;

    auto* cdb = reinterpret_cast<eod::CDBupgr*>(ee_ptr(cdb_addr));
    uint32_t count = std::min(4u, cdb->car_count);
    for (uint32_t i = 0; i < count; ++i) {
        if (cdb->cars[i].flags & 1) { // Owned slot
            showroom_spawn_yard_car(static_cast<int>(i), static_cast<int>(cdb->cars[i].vehicle_id), static_cast<int>(i), 0.0f);
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
// Native Recomp HLE Wrappers
// -----------------------------------------------------------------------------

extern "C" void f_001C31A0(void) {
    const uint32_t camera_ctx_addr = arg_u32(0);
    const uint32_t director_addr = arg_u32(1);
    f_001C31A0_orig();
    showroom_inject_camera_orbit(camera_ctx_addr, director_addr);
}

extern "C" void f_001D2DC8(void) {
    const uint32_t name_ptr = arg_u32(0);
    const int blend_flag = static_cast<int>(arg_u32(1));
    char track_name[64] = {};
    if (name_ptr && sane(name_ptr, 1)) {
        ee_read_string(name_ptr, track_name, sizeof(track_name));
    }
    f_001D2DC8_orig();
    showroom_on_play_track(track_name, blend_flag);
}
