#pragma once

#include "ee.h"

#include <cmath>
#include <cstdint>

namespace world_visibility {

struct Vec3 {
    float x, y, z;
};

struct Frustum {
    Vec3 rows[3];
    Vec3 translation;
    float horizontal_scale, radius_scale, depth_scale, depth_offset;
};

struct Result {
    float depth;
    uint16_t clip;
    uint8_t visibility;
};

// VU FMAC rounds the product before summing. The old runtime retries a
// special IEEE result with saturated operands, then saturates the result.
inline float accumulate(float sum, float a, float b) {
    const float result = sum + a * b;
    if (!ee_fspecial(result)) return result;
    return ee_fclamp(ee_fclamp(sum) + fmul_ps2(ee_fclamp(a), ee_fclamp(b)));
}

inline float dot(Vec3 a, Vec3 b) {
    return fadd_ps2(fadd_ps2(fmul_ps2(a.x, b.x), fmul_ps2(a.y, b.y)), fmul_ps2(a.z, b.z));
}

inline float transform(Vec3 row, Vec3 centre, float translation) {
    float sum = fmul_ps2(row.x, centre.x);
    sum = accumulate(sum, row.y, centre.y);
    sum = accumulate(sum, row.z, centre.z);
    return accumulate(sum, translation, 1.0f);
}

inline uint32_t clip(Vec3 point, float w) {
    w = std::fabs(w);
    return (point.x > w ? 0x01u : 0) | (point.x < -w ? 0x02u : 0) |
           (point.y > w ? 0x04u : 0) | (point.y < -w ? 0x08u : 0) |
           (point.z > w ? 0x10u : 0) | (point.z < -w ? 0x20u : 0);
}

inline Result classify(const Frustum &view, Vec3 centre, float radius) {
    const Vec3 camera{transform(view.rows[0], centre, view.translation.x),
                      transform(view.rows[1], centre, view.translation.y),
                      transform(view.rows[2], centre, view.translation.z)};
    const Vec3 plus{accumulate(camera.x, 0.0f, radius),
                    accumulate(camera.y, 0.0f, radius),
                    accumulate(camera.z, view.radius_scale, radius)};
    // MSUB has the same rounding/clamp rules as MADD with a signed product.
    const Vec3 minus{accumulate(camera.x, 0.0f, -radius),
                     accumulate(camera.y, 0.0f, -radius),
                     accumulate(camera.z, view.radius_scale, -radius)};
    const auto project = [&view](Vec3 endpoint) {
        return Vec3{fmul_ps2(endpoint.x, view.horizontal_scale),
                    fmul_ps2(endpoint.y, view.horizontal_scale),
                    fadd_ps2(fmul_ps2(endpoint.z, view.depth_scale), view.depth_offset)};
    };
    const uint32_t flags = (clip(project(plus), plus.z) << 6) | clip(project(minus), minus.z);
    const float depth = fadd_ps2(0.0f, minus.z);
    return {ee_fclamp(depth) > 1.0f ? depth : 1.0f, static_cast<uint16_t>(flags),
            static_cast<uint8_t>((flags & 0xBD0) ? 0 : (flags & 0x42F) ? 0x81 : 1)};
}

// 0x357E60: the group record's XYZ holds the three ordered distance thresholds.
inline uint8_t group_lod(Vec3 camera, Vec3 centre, Vec3 thresholds, float offset, float scale) {
    const Vec3 delta{fsub_ps2(camera.x, centre.x), fsub_ps2(camera.y, centre.y),
                     fsub_ps2(camera.z, centre.z)};
    const auto distance = fmul_ps2(fadd_ps2(dot(delta, delta), offset), scale);
    return static_cast<uint8_t>(~clip(thresholds, distance) & 0x15);
}

}  // namespace world_visibility
