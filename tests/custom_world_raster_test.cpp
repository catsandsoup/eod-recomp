// No disc needed. Exercise the legacy custom-world projection and clipping.
#include "render/custom_world_raster.h"
#include <array>
#include <cstdio>
#include <vector>

int main() {
    using namespace native::custom_world;
    const double projection[4][4] = {{1, 0, 2, 0}, {0, 1, 3, 0}, {0, 0, 0, 200}, {0, 0, 1, 0}};
    auto vertex = [&](float x, float y, float z) {
        native::scene::Vertex v{};
        v.p[0] = x; v.p[1] = y; v.p[2] = z;
        v.c[0] = 127; v.c[1] = 34; v.c[2] = 56;
        return project(v, projection, 2, 3);
    };
    const ClipView view{0, 100, 0, 80, 1, 10};
    const auto a = vertex(20, 20, 2), b = vertex(80, 20, 2), c = vertex(20, 70, 2);
    std::vector<gs::Vertex> vertices;
    auto emit = [&](auto x, auto y, auto z) { vertices.insert(vertices.end(), {x, y, z}); };
    raster_triangle(a, b, c, view, emit);
    if (vertices.size() != 3 || vertices[0].x != 160 || vertices[0].y != 160 ||
        vertices[0].z != 100 || vertices[0].r != 127 || vertices[0].b != 56) return 1;
    vertices.clear();
    raster_triangle(a, b, vertex(20, 20, 0.5f), view, emit);
    if (vertices.size() != 6) return 2; // near-plane crossing creates a quad
    for (const auto &v : vertices) if (v.z > 200 || v.z < 100) return 3;
    vertices.clear();
    raster_triangle(vertex(-10, 20, 2), vertex(-20, 40, 2), vertex(-80, 30, 2), view, emit);
    if (!vertices.empty()) return 4;
    raster_triangle(vertex(-350, -200, 2), vertex(1500, -200, 2), vertex(-350, 1200, 2), view, emit);
    if (vertices.size() != 6) return 5; // fills the scissor even though all source corners are outside
    for (const auto &v : vertices)
        if (v.x < 0 || v.x > 1600 || v.y < 0 || v.y > 1280) return 6;
    vertices.clear();
    raster_triangle(vertex(20, 20, -1), vertex(40, 20, -1), vertex(30, 40, -1), view, emit);
    return vertices.empty() ? 0 : 7;
}
