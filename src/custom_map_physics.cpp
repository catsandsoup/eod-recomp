// Portable mesh contacts at the native wheel sampler and car pose producer.
#include "battle_layout.h"
#include "core/cpu.h"
#include "core/memory.h"
#include "netplay_hooks.h"
#include "vfs/custom_map.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

extern "C" void f_00212500_orig();
extern "C" void f_001DE7B0_orig();

namespace {
using eod::physics::CustomMapCollision;
using eod::physics::MapContact;
using P = eod::physics::MapPoint;

thread_local const CustomMapCollision* wheel_mesh{};
constexpr uint32_t kMaxCars = 20;
bool ram(uint32_t ptr, uint32_t size) {
    return ptr && !(ptr & 3u) && ptr <= EE_RAM_SIZE - size;
}
P point(uint32_t ptr) {
    P p;
    ee_read_block(ptr, p.data(), sizeof p);
    return p;
}
void write_point(uint32_t ptr, const P& p) {
    ee_write_block(ptr, p.data(), sizeof p);
}
P add(P a, P b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
P sub(P a, P b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
P scale(P a, float v) {
    return {a[0] * v, a[1] * v, a[2] * v};
}
float dot(P a, P b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
P cross(P a, P b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

struct BodyBox {
    P center, half;
};
bool chassis_box(uint32_t index, BodyBox& box) {
    // CarBase constructor 0x185390 appends cars in the body array's order.
    const uint32_t car = ee_r32(0x3CE4F8 + 4 * index);
    if (!ram(car, 0x260)) return false;
    const uint32_t model = ee_r32(car + 0x25C);
    if (!ram(model, 0x20)) return false;
    // Bone.mvl rest positions are car-local metres, the same positions used by
    // 0x18CA98 to find the part of this particular chassis that took a hit.
    const uint32_t count = ee_r32(model + 0x1C);
    if (!count || count > 128 || !ram(model, 0x20 + count * 0x34)) return false;
    P lo = point(model + 0x3C), hi = lo;
    for (uint32_t i = 0; i < count; ++i) {
        const P p = point(model + 0x3C + i * 0x34);
        for (unsigned d = 0; d < 3; ++d) {
            if (!std::isfinite(p[d])) return false;
            lo[d] = std::min(lo[d], p[d]);
            hi[d] = std::max(hi[d], p[d]);
        }
    }
    box = {scale(add(lo, hi), .5f), scale(sub(hi, lo), .5f)};
    return box.half[0] > 0 && box.half[1] > 0 && box.half[2] > 0;
}
using Basis = std::array<P, 3>;
Basis axes(uint32_t body) {
    return {point(body + 0x10), point(body + 0x20), point(body + 0x30)};
}
P world_vector(const Basis& axis, P v) {
    return add(add(scale(axis[0], v[0]), scale(axis[1], v[1])), scale(axis[2], v[2]));
}
void resolve_body(const CustomMapCollision& mesh, uint32_t body, uint32_t wheels, const BodyBox& box,
                  P start) {
    const Basis basis = axes(body);
    const P center_offset = world_vector(basis, box.center);
    const P original = add(point(body), center_offset);
    P end = original, velocity = point(body + 0x50), angular = point(body + 0x60);
    std::array<float, 4> inverse;
    ee_read_block(body + 0x80, inverse.data(), sizeof inverse);
    bool collided = false;
    // Advance to impact and retain the remaining displacement along the
    // contact plane. Multiple iterations permit a wall/floor corner.
    for (unsigned iteration = 0; iteration < 4; ++iteration) {
        MapContact contact;
        if (!mesh.sweep_box(start, end, basis, box.half, contact)) break;
        const P move = sub(end, start);
        const P impact = add(start, scale(move, contact.fraction));
        const P lever = sub(contact.point, sub(impact, center_offset));
        const P arm = cross(lever, contact.normal);
        const P inertia =
            world_vector(basis, {dot(arm, basis[0]) * inverse[0], dot(arm, basis[1]) * inverse[1],
                                 dot(arm, basis[2]) * inverse[2]});
        const float inward = dot(add(velocity, cross(angular, lever)), contact.normal);
        const float effective_inverse_mass = inverse[3] + dot(arm, inertia);
        if (inward < 0 && effective_inverse_mass > 0) {
            const float impulse = -inward / effective_inverse_mass;
            velocity = add(velocity, scale(contact.normal, impulse * inverse[3]));
            angular = add(angular, scale(inertia, impulse));
        }
        P remainder = sub(end, impact);
        remainder = sub(remainder, scale(contact.normal, std::min(0.f, dot(remainder, contact.normal))));
        start = add(impact, scale(contact.normal, contact.penetration + .001f));
        end = add(start, remainder);
        collided = true;
    }
    if (!collided) return;
    const P correction = sub(end, original);
    write_point(body, sub(end, center_offset));
    write_point(body + 0x50, velocity);
    write_point(body + 0x60, angular);
    // Wheel hubs are already in world space in the producer's output.
    for (unsigned w = 0; w < 4; ++w) {
        const uint32_t hub = wheels + 0x40 + w * 0x10;
        write_point(hub, add(point(hub), correction));
    }
}
} // namespace

extern "C" void f_00212500() {
    if (!wheel_mesh) {
        f_00212500_orig();
        return;
    }
    // a0 is the retail SCOL cache, a1 is xyz in/out, a2 is the output
    // normal and a3 is the material. Preserve the native index for next tick.
    const uint32_t sample = g_ee.r[5].uw[0], normal = g_ee.r[6].uw[0], material = g_ee.r[7].uw[0];
    P top = point(sample);
    f_00212500_orig();
    top[1] += .05f; // permit small penetration, while allowing travel below a bridge
    const float retail_y = g_ee.r[2].ud[0] ? point(sample)[1] : top[1] - 10.f;
    MapContact contact;
    if (wheel_mesh->ground(top, retail_y, contact)) {
        write_point(sample, contact.point);
        write_point(normal, contact.normal);
        ee_w32(material, 6); // asphalt
        g_ee.r[2].sd[0] = 1;
    }
}

extern "C" void f_001DE7B0() {
    const auto map = eod::vfs::active_map();
    if (!map) {
        f_001DE7B0_orig();
        return;
    }
    // Hold one immutable mount across every vehicle, both wheel sample calls
    // and chassis response, including a concurrent editor reload/disconnect.
    const auto* previous = wheel_mesh;
    wheel_mesh = &map->collision;
    const uint32_t count = ee_r32(0x494D28), bodies = ee_r32(0x494D30);
    const uint32_t wheels = ee_r32(0x494D34), disabled = ee_r32(0x494D3C);
    const bool valid = count <= kMaxCars && count == ee_r32(0x3CE548) && ram(bodies, count * 0xB0) &&
                       ram(wheels, count * 0x1F0) && ram(disabled, count * 4);
    struct Chassis {
        BodyBox box;
        P start;
        bool active;
    };
    std::array<Chassis, kMaxCars> cars{};
    if (valid) {
        for (uint32_t i = 0; i < count; ++i) {
            auto& car = cars[i];
            if (ee_r32(disabled + 4 * i) || !chassis_box(i, car.box)) continue;
            const uint32_t body = bodies + 0xB0 * i;
            car.start = add(point(body), world_vector(axes(body), car.box.center));
            car.active = true;
        }
    }
    f_001DE7B0_orig();
    wheel_mesh = previous;
    if (valid) {
        for (uint32_t i = 0; i < count; ++i)
            if (cars[i].active)
                resolve_body(map->collision, bodies + 0xB0 * i, wheels + 0x1F0 * i, cars[i].box,
                             cars[i].start);
    }
}

// The player-controller loop checks the race mode, then compares the current
// simulation frame against the countdown deadline (0x1B8DA8..0x1B8DC8).
// Moving the deadline leaves native controller release, start bookkeeping,
// and the BattleMaster start notification in their normal order.
#include "decomp_race_lifecycle.h"

extern "C" void f_00176798() {
    eod_race_00176798();
    if (g_ee.r[31].uw[0] != 0x001B8DA8 || g_ee.r[2].sw[0] != 2) return;
    static const bool editor = [] {
        const char* value = std::getenv("EOD_EDITOR_MAP_TEST");
        return value && std::strcmp(value, "1") == 0;
    }();
    if (!editor || (netplay_hooks::active && netplay_hooks::active()) || ee_r32(0x3C41AC) != 1) return;
    const uint32_t master = ee_r32(0x3CF194);
    if (!ram(master, battle_layout::kMasterSize) || ee_r32(master) != battle_layout::kMasterVtable ||
        ee_r32(master + 4) != 0 || ee_r32(master + battle_layout::kCount) != 1 || !eod::vfs::active_map())
        return;
    const uint32_t now = ee_r32(0x3CDA8C);
    ee_w32(0x4850A8, now);
}
