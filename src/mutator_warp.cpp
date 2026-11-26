#include "mutator_warp.h"
#include "mutator_engine.h"
#include "mutator_hooks.h"
#include "netplay_car.h"
#include "core/memory.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mutator {
namespace {

constexpr uint32_t kBodyCountAddr = 0x00494D28;
constexpr uint32_t kBodyArrayAddr = 0x00494D30;
constexpr uint32_t kWheelArrayAddr = 0x00494D34;
constexpr uint32_t kEntityListAddr = 0x003CE4F8;
constexpr uint32_t kEntityCountAddr = 0x003CE548;
constexpr uint32_t kDriverTableAddr = 0x00485230;
constexpr uint32_t kDriverCountAddr = 0x0048522C;

constexpr float kPi = 3.14159265358979323846f;

bool valid_ram(uint32_t a, uint32_t bytes) {
    return a && !(a & 3u) && bytes <= EE_RAM_SIZE && a <= EE_RAM_SIZE - bytes;
}

void wf(uint32_t a, float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, 4);
    ee_w32(a, u);
}

int find_body_index(uint32_t car) {
    if (!valid_ram(car, 0x1C34)) return -1;
    const uint32_t count = ee_r32(kEntityCountAddr);
    const uint32_t bodies = ee_r32(kBodyCountAddr);
    if (!count || count > 32u || bodies != count) return -1;
    for (uint32_t i = 0; i < count; ++i) {
        if (ee_r32(kEntityListAddr + i * 4u) == car) return static_cast<int>(i);
    }
    return -1;
}

void fallback_repair_car(uint32_t car) {
    if (!valid_ram(car, 0x1C34)) return;
    wf(car + 0x1BECu, 0.0f);       // scalar damage = 0
    ee_w32(car + 0x1BE8u, 0u);     // wreck state = intact
    wf(car + 0x1BF4u, 0.0f);       // damage flash = 0
    ee_w32(car + 0x1C04u, 0xFFFFFFFFu); // lost-wheel side = -1
    // Clear wheel-tossed bit (0x80) on all 4 wheels
    constexpr uint32_t kWheelFlagOffsets[4] = {0x1A50u, 0x1AA4u, 0x1AF8u, 0x1B4Cu};
    for (uint32_t off : kWheelFlagOffsets) {
        ee_w32(car + off, ee_r32(car + off) & ~0x80u);
    }
    // Reattach all 8 detachable panels (state = 0)
    for (uint32_t i = 0; i < 8u; ++i) {
        ee_w32(car + 0x13E8u + i * 0x18u + 0x14u, 0u);
    }
}

void zero_wheel_set(uint32_t body_idx) {
    const uint32_t base = ee_r32(kWheelArrayAddr);
    if (!base) return;
    const uint32_t w = base + body_idx * 0x1F0u;
    if (!valid_ram(w, 0x1F0u)) return;
    // WheelSet layout (src/vehicle_dynamics.cpp):
    //   +0x00 travel, +0x10 angle, +0x20 travelRate, +0x30 spinRate,
    //   +0x90..+0xCF slip[4]
    for (uint32_t off : {0x00u, 0x20u, 0x30u, 0x90u, 0xA0u, 0xB0u, 0xC0u}) {
        for (uint32_t lane = 0; lane < 4u; ++lane) wf(w + off + lane * 4u, 0.0f);
    }
}

void clear_recovery_timers(uint32_t slot) {
    const uint32_t count = ee_r32(kDriverCountAddr);
    if (slot >= count || count > kMaxSlots) return;
    const uint32_t person = ee_r32(kDriverTableAddr + slot * 4u);
    if (!valid_ram(person, 200u)) return;
    ee_w32(person + 192u, 0u);     // clear HUD wrong-way warning flag
    wf(person + 196u, -1.0f);      // clear expired recovery deadline
}

} // namespace

WarpPose make_yaw_pose(float x, float y, float z, float yaw_rad) {
    WarpPose p;
    const float c = std::cos(yaw_rad);
    const float s = std::sin(yaw_rad);
    // Right (+X rotated by yaw), Up (+Y), Forward (+Z rotated by yaw)
    p.pose[0] = c;   p.pose[1] = 0.0f; p.pose[2] = -s;
    p.pose[3] = 0.0f; p.pose[4] = 1.0f; p.pose[5] = 0.0f;
    p.pose[6] = s;   p.pose[7] = 0.0f; p.pose[8] = c;
    p.pose[9] = x;   p.pose[10] = y;   p.pose[11] = z;
    return p;
}

bool warp_slot(uint32_t slot, const WarpPose &target, bool repair) {
    if (slot >= kMaxSlots) return false;
    for (int i = 0; i < 12; ++i) {
        if (!std::isfinite(target.pose[i])) return false;
    }
    const uint32_t car = netplay_car::car_of_slot(slot);
    if (!car || !valid_ram(car, 0x1C34)) return false;

    if (repair) {
        if (mutator_hooks::native_repair_car) {
            mutator_hooks::native_repair_car(car);
        } else {
            fallback_repair_car(car);
        }
    }

    // 1. Write RigidBody master record & zero linear/angular velocity + force/torque
    const int body_idx = find_body_index(car);
    if (body_idx >= 0) {
        const uint32_t bodies = ee_r32(kBodyArrayAddr);
        const uint32_t B = bodies + static_cast<uint32_t>(body_idx) * 0xB0u;
        if (valid_ram(B, 0xB0u)) {
            for (int i = 0; i < 3; ++i) wf(B + static_cast<uint32_t>(i) * 4u, target.pose[9 + i]);
            for (int r = 0; r < 3; ++r) {
                for (int i = 0; i < 3; ++i) {
                    wf(B + 0x10u + static_cast<uint32_t>(r) * 0x10u + static_cast<uint32_t>(i) * 4u,
                       target.pose[r * 3 + i]);
                }
            }
            float q[4] = {};
            netplay_car::matrix_to_quat(target.pose, q);
            for (int i = 0; i < 4; ++i) wf(B + 0x40u + static_cast<uint32_t>(i) * 4u, q[i]);
            // Zero linearVelocity (+0x50), angularVelocity (+0x60), force (+0x90), torque (+0xA0)
            for (uint32_t off : {0x50u, 0x60u, 0x90u, 0xA0u}) {
                for (int i = 0; i < 3; ++i) wf(B + off + static_cast<uint32_t>(i) * 4u, 0.0f);
            }
        }
        // 2. Zero WheelSet travel/spin/slip to avoid one-frame suspension impulse spikes
        zero_wheel_set(static_cast<uint32_t>(body_idx));
    }

    // 3. Write Car copies & propagate scene/spatial-hash bounds
    for (int i = 0; i < 12; ++i) wf(car + netplay_car::kPose + static_cast<uint32_t>(i) * 4u, target.pose[i]);
    for (int i = 0; i < 3; ++i) {
        wf(car + netplay_car::kVel + static_cast<uint32_t>(i) * 4u, 0.0f);
        wf(car + netplay_car::kAngVel + static_cast<uint32_t>(i) * 4u, 0.0f);
    }
    if (mutator_hooks::native_set_car_pose) {
        mutator_hooks::native_set_car_pose(car, target.pose);
    }
    if (mutator_hooks::native_reset_slot_hud) {
        mutator_hooks::native_reset_slot_hud(slot);
    }

    // 4. Clear wrong-way recovery timers & bump warp generation for instant puppet snap
    clear_recovery_timers(slot);
    mutator_hooks::slots[slot].warp_gen = static_cast<uint8_t>(mutator_hooks::slots[slot].warp_gen + 1u);
    return true;
}

uint32_t warp_formation_to_zone(const WarpZone &zone, uint32_t slot_count, bool repair, uint32_t seed) {
    float center[3] = {0.0f, 0.0f, 0.0f};
    if (!Engine::resolve_anchor(zone.anchor, zone.origin, zone.spline_pct, center)) return 0;

    const uint32_t count = std::min<uint32_t>(slot_count, kMaxSlots);
    if (!count) return 0;

    // Ensure at least ~6 m arc spacing between adjacent cars in a circle
    const float min_radius = (count <= 1) ? 0.0f : (static_cast<float>(count) * 6.0f) / (2.0f * kPi);
    const float radius = std::max(zone.radius_m, min_radius);
    const float base_angle = static_cast<float>(seed & 0xFFu) * (2.0f * kPi / 256.0f);

    uint32_t warped = 0;
    for (uint32_t slot = 0; slot < count; ++slot) {
        const float theta = base_angle + (2.0f * kPi * static_cast<float>(slot)) / static_cast<float>(count);
        const float x = center[0] + std::cos(theta) * radius;
        const float z = center[2] + std::sin(theta) * radius;
        float y = center[1];
        if (mutator_hooks::ground_snap_y) {
            float snapped = y;
            if (mutator_hooks::ground_snap_y(x, z, snapped)) y = snapped;
        }
        // Face inward toward arena center or tangentially around the circle
        const float dx = zone.face_center ? (center[0] - x) : -std::sin(theta);
        const float dz = zone.face_center ? (center[2] - z) : std::cos(theta);
        const float yaw = std::atan2(dx, dz);
        if (warp_slot(slot, make_yaw_pose(x, y + 0.35f, z, yaw), repair)) {
            ++warped;
        }
    }
    if (warped > 0) {
        mutator_hooks::global_warp_gen = static_cast<uint8_t>(mutator_hooks::global_warp_gen + 1u);
    }
    return warped;
}

} // namespace mutator
