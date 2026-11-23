#pragma once

#include "ee.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define EOD_VIS_SIMD_NEON 1
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define EOD_VIS_SIMD_SSE2 1
#endif

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

// ---- Vectorized 6-Plane Frustum AABB Culling (NEON / SSE2 / Scalar) -------------------------
struct alignas(16) Frustum6 {
    // Half-space plane i: nx[i]*x + ny[i]*y + nz[i]*z + d[i] >= 0 is inside.
    // Lanes 6..7 duplicate planes 4..5 for branchless 2x4-wide SIMD evaluation.
    float nx[8]{};
    float ny[8]{};
    float nz[8]{};
    float d[8]{};
    bool valid = false;

    inline void set_plane(int idx, float a, float b, float c, float w) {
        if (idx < 0 || idx >= 6) return;
        nx[idx] = a;
        ny[idx] = b;
        nz[idx] = c;
        d[idx] = w;
        if (idx == 4) {
            nx[6] = a;
            ny[6] = b;
            nz[6] = c;
            d[6] = w;
        } else if (idx == 5) {
            nx[7] = a;
            ny[7] = b;
            nz[7] = c;
            d[7] = w;
        }
    }
};

// Extract the 6 Vulkan clip-space half-spaces (-w <= x <= w, -w <= y <= w, 0 <= z <= w)
// directly from the vertex shader's model->clip row vectors (cx, cy, cz, cw).
inline Frustum6 frustum6_from_clip_rows(const float cx[4], const float cy[4],
                                        const float cz[4], const float cw[4]) {
    Frustum6 f{};
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(cx[i]) || !std::isfinite(cy[i]) ||
            !std::isfinite(cz[i]) || !std::isfinite(cw[i])) {
            return f;
        }
    }
    f.set_plane(0, cw[0] + cx[0], cw[1] + cx[1], cw[2] + cx[2], cw[3] + cx[3]);  // Left
    f.set_plane(1, cw[0] - cx[0], cw[1] - cx[1], cw[2] - cx[2], cw[3] - cx[3]);  // Right
    f.set_plane(2, cw[0] + cy[0], cw[1] + cy[1], cw[2] + cy[2], cw[3] + cy[3]);  // Bottom
    f.set_plane(3, cw[0] - cy[0], cw[1] - cy[1], cw[2] - cy[2], cw[3] - cy[3]);  // Top
    f.set_plane(4, cz[0], cz[1], cz[2], cz[3]);                                  // Z >= 0
    f.set_plane(5, cw[0] - cz[0], cw[1] - cz[1], cw[2] - cz[2], cw[3] - cz[3]);  // Z <= W
    f.valid = true;
    return f;
}

inline const char *simd_backend_name() {
#if defined(EOD_VIS_SIMD_NEON)
    return "NEON";
#elif defined(EOD_VIS_SIMD_SSE2)
    return "SSE2";
#else
    return "Scalar";
#endif
}

// Reference scalar 6-plane p-vertex AABB culling test (returns true iff strictly outside).
inline bool aabb_outside_frustum_scalar(const Frustum6 &f, const float bmin[3], const float bmax[3]) {
    for (int i = 0; i < 6; ++i) {
        const float px = f.nx[i] >= 0.0f ? bmax[0] : bmin[0];
        const float py = f.ny[i] >= 0.0f ? bmax[1] : bmin[1];
        const float pz = f.nz[i] >= 0.0f ? bmax[2] : bmin[2];
        const float sum = ((f.nx[i] * px + f.ny[i] * py) + f.nz[i] * pz) + f.d[i];
        if (sum < 0.0f) return true;
    }
    return false;
}

// Vectorized 6-plane p-vertex AABB culling test matching scalar IEEE-754 operation order.
inline bool aabb_outside_frustum_simd(const Frustum6 &f, const float bmin[3], const float bmax[3]) {
#if defined(EOD_VIS_SIMD_NEON)
    const float32x4_t zero = vdupq_n_f32(0.0f);
    const float32x4_t minx = vdupq_n_f32(bmin[0]);
    const float32x4_t maxx = vdupq_n_f32(bmax[0]);
    const float32x4_t miny = vdupq_n_f32(bmin[1]);
    const float32x4_t maxy = vdupq_n_f32(bmax[1]);
    const float32x4_t minz = vdupq_n_f32(bmin[2]);
    const float32x4_t maxz = vdupq_n_f32(bmax[2]);
    auto eval4 = [&](int off) -> uint32x4_t {
        const float32x4_t nx = vld1q_f32(f.nx + off);
        const float32x4_t ny = vld1q_f32(f.ny + off);
        const float32x4_t nz = vld1q_f32(f.nz + off);
        const float32x4_t dd = vld1q_f32(f.d + off);
        const float32x4_t px = vbslq_f32(vcgeq_f32(nx, zero), maxx, minx);
        const float32x4_t py = vbslq_f32(vcgeq_f32(ny, zero), maxy, miny);
        const float32x4_t pz = vbslq_f32(vcgeq_f32(nz, zero), maxz, minz);
        const float32x4_t s0 = vaddq_f32(vmulq_f32(nx, px), vmulq_f32(ny, py));
        const float32x4_t s1 = vaddq_f32(s0, vmulq_f32(nz, pz));
        const float32x4_t sum = vaddq_f32(s1, dd);
        return vcltq_f32(sum, zero);
    };
    const uint32x4_t outside = vorrq_u32(eval4(0), eval4(4));
    return vmaxvq_u32(outside) != 0u;
#elif defined(EOD_VIS_SIMD_SSE2)
    const __m128 zero = _mm_setzero_ps();
    const __m128 minx = _mm_set1_ps(bmin[0]);
    const __m128 maxx = _mm_set1_ps(bmax[0]);
    const __m128 miny = _mm_set1_ps(bmin[1]);
    const __m128 maxy = _mm_set1_ps(bmax[1]);
    const __m128 minz = _mm_set1_ps(bmin[2]);
    const __m128 maxz = _mm_set1_ps(bmax[2]);
    auto sel = [](__m128 m, __m128 a, __m128 b) -> __m128 {
        return _mm_or_ps(_mm_and_ps(m, a), _mm_andnot_ps(m, b));
    };
    auto eval4 = [&](int off) -> __m128 {
        const __m128 nx = _mm_load_ps(f.nx + off);
        const __m128 ny = _mm_load_ps(f.ny + off);
        const __m128 nz = _mm_load_ps(f.nz + off);
        const __m128 dd = _mm_load_ps(f.d + off);
        const __m128 px = sel(_mm_cmpge_ps(nx, zero), maxx, minx);
        const __m128 py = sel(_mm_cmpge_ps(ny, zero), maxy, miny);
        const __m128 pz = sel(_mm_cmpge_ps(nz, zero), maxz, minz);
        const __m128 s0 = _mm_add_ps(_mm_mul_ps(nx, px), _mm_mul_ps(ny, py));
        const __m128 s1 = _mm_add_ps(s0, _mm_mul_ps(nz, pz));
        const __m128 sum = _mm_add_ps(s1, dd);
        return _mm_cmplt_ps(sum, zero);
    };
    const __m128 outside = _mm_or_ps(eval4(0), eval4(4));
    return _mm_movemask_ps(outside) != 0;
#else
    return aabb_outside_frustum_scalar(f, bmin, bmax);
#endif
}

// Batch helper: evaluates N packed [x,y,z] AABBs; writes 1 (visible) or 0 (culled) to out_visible.
inline size_t cull_aabbs_simd(const Frustum6 &f, const float *bmins_xyz, const float *bmaxs_xyz,
                              size_t count, uint8_t *out_visible) {
    size_t visible = 0;
    for (size_t i = 0; i < count; ++i) {
        const bool outside = aabb_outside_frustum_simd(f, bmins_xyz + i * 3, bmaxs_xyz + i * 3);
        const uint8_t vis = outside ? 0u : 1u;
        if (out_visible) out_visible[i] = vis;
        visible += vis;
    }
    return visible;
}

}  // namespace world_visibility
