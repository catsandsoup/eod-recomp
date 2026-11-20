#include "render/world_visibility.h"

#include <cmath>
#include <cstdio>

int main() {
    using namespace world_visibility;
    const Frustum view = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {},
                          1, 1, 1.25f, -2.25f};
    const float next = std::nextafter(6.0f, 7.0f);
    struct SphereCase {
        Vec3 centre;
        float radius;
        uint8_t byte;
        float depth;
    };
    const SphereCase spheres[] = {
        {{0, 0, 5}, 1, 1, 4},
        {{4, 0, 5}, 1, 1, 4},
        {{6, 0, 5}, 1, 0x81, 4},
        {{next, 0, 5}, 1, 0, 4},
        {{-6, 0, 5}, 1, 0x81, 4},
        {{0, -next, 5}, 1, 0, 4},
        {{0, 0, 2}, 1, 1, 1},
        {{0, 0, 1}, 1, 0x81, 1},
        {{0, 0, 0}, 0, 0, 1},
        {{0, 0, 8}, 1, 1, 7},
        {{0, 0, 9}, 1, 0x81, 8},
        {{0, 0, 11}, 1, 0, 10},
    };
    for (const auto &s : spheres) {
        const Result result = classify(view, s.centre, s.radius);
        if (result.visibility != s.byte || result.depth != s.depth) {
            std::fprintf(stderr, "sphere at (%g, %g, %g), r=%g: byte %02x depth %g\n",
                         s.centre.x, s.centre.y, s.centre.z, s.radius, result.visibility, result.depth);
            return 1;
        }
    }

    const Vec3 thresholds{4, 9, 16};
    for (int distance = 1; distance <= 4; ++distance) {
        const uint8_t expected[] = {0, 1, 5, 21};
        const auto byte = group_lod({float(distance), 0, 0}, {}, thresholds, 0, 1);
        if (byte != expected[distance - 1]) {
            std::fprintf(stderr, "LOD distance %d: byte %02x\n", distance, byte);
            return 1;
        }
    }
    return group_lod({2, 1, 2}, {1, 1, 2}, thresholds, -3, 2) == 1 ? 0 : 1;
}
