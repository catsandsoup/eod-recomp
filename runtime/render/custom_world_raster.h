// Homogeneous clipping for the legacy GS rasterizer. Plume clips its static mesh on the GPU.
#pragma once
#include "native_scene_types.h"
#include "gs/gs_state.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace native::custom_world {

struct ClipVertex {
    std::array<double, 4> xyzw{};
    std::array<double, 3> rgb{};
};
struct ClipView {
    double left, right, top, bottom, near_z, far_z;
};

inline ClipVertex project(const scene::Vertex &vertex, const double matrix[4][4], double ox, double oy) {
    ClipVertex v;
    for (int row = 0; row < 4; ++row) {
        const auto *m = matrix[row];
        v.xyzw[row] = m[0] * vertex.p[0] + m[1] * vertex.p[1] + m[2] * vertex.p[2] + m[3];
    }
    v.xyzw[0] -= ox * v.xyzw[3];
    v.xyzw[1] -= oy * v.xyzw[3];
    for (int i = 0; i < 3; ++i) v.rgb[i] = vertex.c[i];
    return v;
}

inline double distance(const ClipVertex &v, const ClipView &view, unsigned plane) {
    const auto &p = v.xyzw;
    switch (plane) {
    case 0: return p[3] - view.near_z;
    case 1: return view.far_z - p[3];
    case 2: return p[0] - view.left * p[3];
    case 3: return view.right * p[3] - p[0];
    case 4: return p[1] - view.top * p[3];
    default: return view.bottom * p[3] - p[1];
    }
}

inline ClipVertex interpolate(const ClipVertex &a, const ClipVertex &b, double t) {
    ClipVertex out;
    for (int i = 0; i < 4; ++i) out.xyzw[i] = a.xyzw[i] + t * (b.xyzw[i] - a.xyzw[i]);
    for (int i = 0; i < 3; ++i) out.rgb[i] = a.rgb[i] + t * (b.rgb[i] - a.rgb[i]);
    return out;
}

inline gs::Vertex raster_vertex(const ClipVertex &v) {
    const double iw = 1.0 / v.xyzw[3];
    gs::Vertex out{};
    out.x = int32_t(std::lround(v.xyzw[0] * iw * 16.0));
    out.y = int32_t(std::lround(v.xyzw[1] * iw * 16.0));
    out.z = uint32_t(std::clamp(v.xyzw[2] * iw, 0.0, double(UINT32_MAX)));
    out.r = uint8_t(std::lround(v.rgb[0]));
    out.g = uint8_t(std::lround(v.rgb[1]));
    out.b = uint8_t(std::lround(v.rgb[2]));
    out.a = 0x80;
    out.q = float(iw);
    return out;
}

template <class Emit>
void raster_triangle(const ClipVertex &a, const ClipVertex &b, const ClipVertex &c,
                     const ClipView &view, Emit emit) {
    // Each clipping plane adds at most one vertex to a convex polygon.
    std::array<ClipVertex, 9> polygon{a, b, c}, scratch;
    size_t count = 3;
    for (unsigned plane = 0; plane < 6; ++plane) {
        size_t used = 0;
        auto previous = polygon[count - 1];
        double d0 = distance(previous, view, plane);
        for (size_t i = 0; i < count; ++i) {
            const auto &current = polygon[i];
            const double d1 = distance(current, view, plane);
            if ((d0 < 0 && d1 > 0) || (d0 > 0 && d1 < 0))
                scratch[used++] = interpolate(previous, current, d0 / (d0 - d1));
            if (d1 >= 0) scratch[used++] = current;
            previous = current;
            d0 = d1;
        }
        if (used < 3) return;
        std::copy_n(scratch.begin(), used, polygon.begin());
        count = used;
    }
    const auto first = raster_vertex(polygon[0]);
    auto previous = raster_vertex(polygon[1]);
    for (size_t i = 2; i < count; ++i) {
        const auto current = raster_vertex(polygon[i]);
        emit(first, previous, current);
        previous = current;
    }
}
}
