#include "netplay_names.h"
#include "netplay_car.h"
#include "../runtime/core/memory.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace netplay::names {
namespace {

bool in_ram(uint32_t ptr, uint32_t bytes) {
    return ptr && !(ptr & 3u) && bytes <= EE_RAM_SIZE && ptr <= EE_RAM_SIZE - bytes;
}

float number(uint32_t addr) {
    float result;
    const uint32_t bits = ee_r32(addr);
    std::memcpy(&result, &bits, sizeof result);
    return result;
}

Point point(uint32_t addr) { return {number(addr), number(addr + 4), number(addr + 8)}; }
float dot(const Point &a, const Point &b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
Point difference(const Point &a, const Point &b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }

} // namespace

Camera read_world_camera() {
    // Sample WorldToScreen (0x34DB80)'s camera and projection before the HUD
    // or shadow passes install their own camera into these same globals.
    return {point(0x4CD31C), {point(0x4CD2F8), point(0x4CD304), point(0x4CD310)},
            number(0x4CD368), number(0x4CD36C),
            static_cast<float>(ee_r32(0x3DC978)), static_cast<float>(ee_r32(0x3DC97C))};
}

Point roof_anchor(uint32_t car) {
    const uint32_t pose = car + netplay_car::kPose;
    Point anchor = point(pose + 36);
    const Point up_components = {number(pose + 4), number(pose + 16), number(pose + 28)};
    // Native damage bones, 0x18CA98: the model's rest positions capture the shape
    // of the selected car, including a bus and an overturned car.
    const uint32_t model = ee_r32(car + 0x25C);
    float height = 0.75f;
    if (in_ram(model, 32)) {
        const uint32_t count = ee_r32(model + 0x1C);
        if (count <= 128 && in_ram(model, 32 + count * 0x34))
            for (uint32_t i = 0; i < count; ++i)
                height = std::max(height, dot(up_components, point(model + 0x20 + i * 0x34 + 0x1C)));
    }
    anchor[1] += std::min(height, 8.f) + 0.30f;
    return anchor;
}

bool project(const Camera &cam, const Point &world, Tag &out) {
    const Point offset = difference(world, cam.position);
    const float depth = dot(cam.axes[2], offset);
    if (!(cam.width > 0 && cam.height > 0 && cam.horizontal_scale > 0 && cam.vertical_scale < 0 &&
          depth > 0.1f)) return false;
    out.distance = std::sqrt(dot(offset, offset));
    if (!(out.distance <= 150.f)) return false;
    out.x = 0.5f + dot(cam.axes[0], offset) * cam.horizontal_scale / (depth * cam.width);
    out.y = 0.5f + dot(cam.axes[1], offset) * cam.vertical_scale / (depth * cam.height);
    return out.x >= 0 && out.x <= 1 && out.y >= 0 && out.y <= 1;
}

bool intersects(const TerrainFace &face, const Point &from, const Point &to) {
    const float a = dot(face.normal, from) + face.offset;
    const float b = dot(face.normal, to) + face.offset;
    if ((a > 0) == (b > 0) || a == b) return false;
    const float t = a / (a - b);
    if (t <= 0.001f || t >= 0.999f) return false;
    const float x = from[0] + (to[0] - from[0]) * t;
    const float z = from[2] + (to[2] - from[2]) * t;
    for (const auto &edge : face.edges)
        if (edge[0] * x + edge[1] * z + edge[2] < -0.001f) return false;
    return true;
}

bool intersects(const Box &box, const Point &from, const Point &to) {
    const Point origin = difference(from, box.center), direction = difference(to, from);
    float enter = 0.001f, leave = 0.999f;
    for (int i = 0; i < 3; ++i) {
        const float axis_origin = dot(origin, box.axes[i]);
        const float delta = dot(direction, box.axes[i]);
        const float half = box.half_extents[i];
        if (std::fabs(delta) < 0.00001f) {
            if (std::fabs(axis_origin) > half) return false;
        } else {
            float start = (-half - axis_origin) / delta;
            float end = (half - axis_origin) / delta;
            if (start > end) std::swap(start, end);
            enter = std::max(enter, start);
            leave = std::min(leave, end);
            if (enter > leave) return false;
        }
    }
    return true;
}

bool intersects(const Tube &tube, const Point &from, const Point &to) {
    // Native TUBE has spherical ends at +/- half_length on its local Z axis.
    // Find the closest points between the open sightline and that finite centreline.
    const Point origin = difference(from, tube.center), direction = difference(to, from);
    const float length_squared = dot(direction, direction);
    if (!(length_squared > 0)) return false;
    const float axial_origin = dot(tube.axis, origin);
    const float axial_delta = dot(tube.axis, direction);
    const float along_ray = dot(direction, origin);
    const float radial_speed_squared = length_squared - axial_delta * axial_delta;
    float t = 0.001f;
    if (radial_speed_squared > length_squared * 0.000001f)
        t = std::clamp((axial_delta * axial_origin - along_ray) / radial_speed_squared, t, 0.999f);
    const float axial = std::clamp(axial_origin + axial_delta * t, -tube.half_length, tube.half_length);
    t = std::clamp((axial_delta * axial - along_ray) / length_squared, 0.001f, 0.999f);
    Point separation;
    for (int i = 0; i < 3; ++i) separation[i] = origin[i] + t * direction[i] - axial * tube.axis[i];
    return dot(separation, separation) <= tube.radius * tube.radius;
}

void Occluders::load() {
    terrain_.clear();
    boxes_.clear();
    tubes_.clear();
    // SCOL v4 and SOBL v3 layouts are used by 0x226768 and 0x1A00B8.
    const uint32_t scol = ee_r32(0x3D2098);
    if (in_ram(scol, 12)) {
        const uint32_t count = ee_r32(scol), edges = ee_r32(scol + 4), planes = ee_r32(scol + 8);
        if (count <= 32768 && in_ram(edges, count * 32) && in_ram(planes, count * 20)) {
            terrain_.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t row = edges + i * 32, plane = planes + i * 20;
                TerrainFace face{};
                for (int j = 0; j < 3; ++j) {
                    const uint32_t edge = row + j * 8;
                    face.edges[j] = {int16_t(ee_r16(edge)) / 32767.f, int16_t(ee_r16(edge + 2)) / 32767.f,
                                     number(edge + 4)};
                }
                face.normal = point(plane);
                face.offset = number(plane + 12);
                face.mask = ee_r16(row + 30);
                terrain_.push_back(face);
            }
        }
    }
    for (uint32_t table : {0x4473FCu, 0x447400u}) {
        const uint32_t solid = ee_r32(table);
        if (!in_ram(solid, 20)) continue;
        const uint32_t count = ee_r32(solid), boxes = ee_r32(solid + 8);
        if (count > 32768 || !in_ram(boxes, count * 0x108)) continue;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t box = boxes + i * 0x108;
            const uint32_t shape = ee_r32(box + 0x38);
            if (shape == 0x424F5820) // BOX
                boxes_.push_back({point(box + 0x24), {point(box), point(box + 12), point(box + 24)}, point(box + 0x60)});
            else if (shape == 0x54554245) { // TUBE
                Tube tube = {point(box + 0x24), point(box + 24), number(box + 0x60), number(box + 0x64)};
                if (tube.radius > 0 && tube.half_length >= 0) tubes_.push_back(tube);
            }
        }
    }
}

bool Occluders::blocked(const Point &from, const Point &to) const {
    for (const Box &box : boxes_) if (intersects(box, from, to)) return true;
    for (const Tube &tube : tubes_) if (intersects(tube, from, to)) return true;
    const uint32_t mask = ee_r32(0x3D1C70);
    for (const TerrainFace &face : terrain_)
        if ((!face.mask || (mask & face.mask)) && intersects(face, from, to)) return true;
    return false;
}

} // namespace netplay::names
