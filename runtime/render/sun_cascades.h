// Fixed camera-relative directional shadow volumes and native depth reprojection.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace native::sun_cascades {

using Vec3 = std::array<double, 3>;
using Rows = std::array<std::array<double, 4>, 4>;

constexpr uint32_t kResolution = 2048;
constexpr double kNearBlendStart = 18.0, kNearBlendEnd = 25.0;
constexpr double kFarFadeStart = 84.0, kFarFadeEnd = 104.0;

inline bool daylight(uint32_t fog) {
    // The initial supported race has a bright authored horizon. Keep black and dark night skies unchanged.
    return 0.2126 * (fog & 255) + 0.7152 * (fog >> 8 & 255) + 0.0722 * (fog >> 16 & 255) >= 24;
}

inline Vec3 add(Vec3 a, Vec3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline Vec3 sub(Vec3 a, Vec3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline Vec3 scale(Vec3 a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
inline double dot(Vec3 a, Vec3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline Vec3 unit(Vec3 a) { return scale(a, 1.0 / std::sqrt(dot(a, a))); }

struct Box {
    Vec3 center;
    double radius, half_depth;
};

struct View {
    std::array<Vec3, 3> axis;   // X and Y on the map, then depth traveling away from the sun
    std::array<Box, 2> box;
};

inline View make_view(Vec3 camera, Vec3 forward, Vec3 toward_sun) {
    View view{};
    const Vec3 z = scale(unit(toward_sun), -1.0);
    const Vec3 up = std::abs(z[1]) < 0.96 ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
    const Vec3 x = unit(cross(up, z));
    view.axis = {x, cross(z, x), z};
    // Near overlap for car bodies and the road, far reach for track buildings and banks.
    view.box = {Box{{}, 24.0, 72.0}, Box{{}, 120.0, 168.0}};
    for (size_t i = 0; i < view.box.size(); ++i) {
        Box &box = view.box[i];
        const Vec3 focus = add(camera, scale(unit(forward), i == 0 ? 12.0 : 52.0));
        const double steps[] = {2.0 * box.radius / kResolution, 2.0 * box.radius / kResolution, 1.0};
        for (size_t k = 0; k < 3; ++k)
            box.center[k] = std::round(dot(view.axis[k], focus) / steps[k]) * steps[k];
    }
    return view;
}

struct Reprojection {
    float rows[3][4]{};
    bool valid = false;
};

inline Reprojection make_reprojection(const Rows &clip, const std::array<Vec3, 3> &light_axis) {
    Reprojection out{};
    const Vec3 x{clip[0][0], clip[0][1], clip[0][2]};
    const Vec3 y{clip[1][0], clip[1][1], clip[1][2]};
    const Vec3 w{clip[3][0], clip[3][1], clip[3][2]};
    const Vec3 z{clip[2][0], clip[2][1], clip[2][2]};
    const double det = dot(x, cross(y, w));
    if (!std::isfinite(det) || std::abs(det) < 1e-12) return out;
    const Vec3 dx = scale(cross(y, w), 1 / det);
    const Vec3 dy = scale(cross(w, x), 1 / det);
    const Vec3 dw = scale(cross(x, y), 1 / det);
    const double depth_offset = dot(z, w) / dot(w, w);
    const double depth_numerator = clip[2][3] - depth_offset * clip[3][3];
    if (!std::isfinite(depth_numerator) || depth_numerator <= 0 || !std::isfinite(depth_offset)) return out;
    for (size_t i = 0; i < 3; ++i) {
        out.rows[i][0] = static_cast<float>(dot(light_axis[i], dx));
        out.rows[i][1] = static_cast<float>(dot(light_axis[i], dy));
        out.rows[i][2] = static_cast<float>(dot(light_axis[i], dw));
    }
    out.rows[0][3] = static_cast<float>(depth_numerator);
    out.rows[1][3] = static_cast<float>(depth_offset);
    out.valid = true;
    return out;
}

struct Bounds {
    Vec3 center{}, extent{};
};

inline Vec3 project(const View &view, const Box &box, Vec3 point) {
    return {(dot(view.axis[0], point) - box.center[0]) / box.radius,
            (dot(view.axis[1], point) - box.center[1]) / box.radius,
            0.5 + (dot(view.axis[2], point) - box.center[2]) / (2 * box.half_depth)};
}

// Project the native model transform and reject only when its source vertices' bound misses the box.
inline bool caster_rows(const View &view, const Box &box, const float (&model)[12],
                        const Bounds &bounds, float (&rows)[3][4]) {
    const double half[] = {box.radius, box.radius, box.half_depth};
    for (size_t i = 0; i < 3; ++i) {
        const Vec3 a = view.axis[i];
        const Vec3 local{a[0] * model[0] + a[1] * model[4] + a[2] * model[8],
                         a[0] * model[1] + a[1] * model[5] + a[2] * model[9],
                         a[0] * model[2] + a[1] * model[6] + a[2] * model[10]};
        const double t = a[0] * model[3] + a[1] * model[7] + a[2] * model[11] - box.center[i];
        const double extent = dot({std::abs(local[0]), std::abs(local[1]), std::abs(local[2])}, bounds.extent);
        if (std::abs(dot(local, bounds.center) + t) > half[i] + extent) return false;
        const double inv_extent = 1.0 / (half[i] * (i == 2 ? 2.0 : 1.0));
        for (size_t k = 0; k < 3; ++k) rows[i][k] = static_cast<float>(local[k] * inv_extent);
        rows[i][3] = static_cast<float>(t * inv_extent + (i == 2 ? 0.5 : 0));
    }
    return true;
}

}  // namespace native::sun_cascades
