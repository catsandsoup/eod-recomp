// plume_pacing_cull_test.cpp - Unit tests & microbenchmark for Sprint 4P-2:
//   1. FramePacer decoupled 60 Hz simulation accumulator at 60 / 120 / 144 / 240 Hz
//   2. PoseInterpolator LERP + SLERP accuracy (< 1e-5), teleport/cut snap guard,
//      camera interpolation, and 256 unbounded simultaneous entities
//   3. Vectorized NEON/SSE2 6-plane frustum AABB culling 100% parity across 10,000
//      randomized AABBs + throughput benchmark (ns/AABB)
#include "core/frame_pacing.h"
#include "render/world_visibility.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

bool nearf(float a, float b, float eps = 1e-5f) {
    return std::fabs(a - b) <= eps;
}

void make_rot_y_3x4(float angle_rad, float tx, float ty, float tz, float out[12]) {
    const float c = std::cos(angle_rad);
    const float s = std::sin(angle_rad);
    out[0] = c;    out[1] = 0.0f; out[2] = s;    out[3] = tx;
    out[4] = 0.0f; out[5] = 1.0f; out[6] = 0.0f; out[7] = ty;
    out[8] = -s;   out[9] = 0.0f; out[10] = c;   out[11] = tz;
}

void make_rot_z_3x3(float angle_rad, float out[9]) {
    const float c = std::cos(angle_rad);
    const float s = std::sin(angle_rad);
    out[0] = c;    out[1] = -s;   out[2] = 0.0f;
    out[3] = s;    out[4] = c;    out[5] = 0.0f;
    out[6] = 0.0f; out[7] = 0.0f; out[8] = 1.0f;
}

bool check_orthonormal(const float m[12], float eps = 1e-5f) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const float d = m[i] * m[j] + m[4 + i] * m[4 + j] + m[8 + i] * m[8 + j];
            const float want = (i == j) ? 1.0f : 0.0f;
            if (!nearf(d, want, eps)) return false;
        }
    }
    return true;
}

bool test_frame_pacer() {
    using frame_pacing::FramePacer;

    // Default 60 Hz lockstep: interpolation disabled, alpha == 1.0
    FramePacer p60;
    p60.configure(60.0);
    if (p60.interpolation_enabled() || std::fabs(p60.alpha() - 1.0) > 1e-9) {
        std::fprintf(stderr, "FAIL: default 60 Hz must keep interpolation off and alpha=1\n");
        return false;
    }

    const double rates[] = {120.0, 144.0, 240.0};
    for (double hz : rates) {
        FramePacer p;
        p.configure(hz);
        if (!p.interpolation_enabled()) {
            std::fprintf(stderr, "FAIL: %.0f Hz should enable interpolation\n", hz);
            return false;
        }
        const int frames = static_cast<int>(hz);
        const double dt = 1.0 / hz;
        uint64_t steps = 0;
        double min_a = 1e9, max_a = -1e9;
        for (int f = 0; f < frames; ++f) {
            p.advance(dt);
            while (p.should_step_sim()) {
                if (!p.consume_sim_step()) return false;
                ++steps;
            }
            const double a = p.alpha();
            if (a < -1e-9 || a > 1.0 + 1e-9) {
                std::fprintf(stderr, "FAIL: %.0f Hz alpha out of [0,1]: %.6f\n", hz, a);
                return false;
            }
            if (a < min_a) min_a = a;
            if (a > max_a) max_a = a;
            p.note_present();
        }
        if (steps != 60 || p.sim_steps() != 60 || p.present_frames() != static_cast<uint64_t>(frames)) {
            std::fprintf(stderr, "FAIL: %.0f Hz produced %llu sim steps (expected 60)\n",
                         hz, (unsigned long long)steps);
            return false;
        }
        std::printf("[pacing] 60 -> %.0f Hz: %d presents, %llu sim steps, alpha in [%.3f, %.3f] OK\n",
                    hz, frames, (unsigned long long)steps, min_a, max_a);
    }
    return true;
}

bool test_pose_interpolator() {
    using frame_pacing::PoseInterpolator;
    using frame_pacing::RigidPose;

    PoseInterpolator interp;
    float m0[12], m1[12], mout[12];
    const float a0 = 0.0f;
    const float a1 = static_cast<float>(kPi * 0.5);  // 90 deg yaw
    make_rot_y_3x4(a0, 1.0f, 2.0f, 3.0f, m0);
    make_rot_y_3x4(a1, 5.0f, 6.0f, 11.0f, m1);

    interp.submit_entity_matrix3x4(42, m0, 1);
    interp.submit_entity_matrix3x4(42, m1, 2);

    // alpha = 0.0 -> exact prev
    if (!interp.evaluate_entity_matrix3x4(42, 0.0f, mout)) return false;
    for (int i = 0; i < 12; ++i) {
        if (!nearf(mout[i], m0[i], 1e-5f)) {
            std::fprintf(stderr, "FAIL: alpha=0 mismatch at [%d]: %.7f vs %.7f\n", i, mout[i], m0[i]);
            return false;
        }
    }

    // alpha = 1.0 -> exact curr
    if (!interp.evaluate_entity_matrix3x4(42, 1.0f, mout)) return false;
    for (int i = 0; i < 12; ++i) {
        if (!nearf(mout[i], m1[i], 1e-5f)) {
            std::fprintf(stderr, "FAIL: alpha=1 mismatch at [%d]: %.7f vs %.7f\n", i, mout[i], m1[i]);
            return false;
        }
    }

    // alpha = 0.5 -> exact midpoint pos + 45 deg yaw SLERP
    float mhalf[12];
    make_rot_y_3x4(static_cast<float>(kPi * 0.25), 3.0f, 4.0f, 7.0f, mhalf);
    if (!interp.evaluate_entity_matrix3x4(42, 0.5f, mout) || !check_orthonormal(mout, 1e-5f)) {
        std::fprintf(stderr, "FAIL: alpha=0.5 non-orthonormal\n");
        return false;
    }
    for (int i = 0; i < 12; ++i) {
        if (!nearf(mout[i], mhalf[i], 1e-5f)) {
            std::fprintf(stderr, "FAIL: alpha=0.5 mismatch at [%d]: %.7f vs %.7f\n", i, mout[i], mhalf[i]);
            return false;
        }
    }

    // Teleport snap guard (> 15 m): must snap cleanly to curr_pose at alpha=0.5
    float m_tele[12];
    make_rot_y_3x4(0.1f, 50.0f, 6.0f, 11.0f, m_tele);
    interp.submit_entity_matrix3x4(42, m_tele, 3);
    if (!interp.evaluate_entity_matrix3x4(42, 0.5f, mout)) return false;
    for (int i = 0; i < 12; ++i) {
        if (!nearf(mout[i], m_tele[i], 1e-5f)) {
            std::fprintf(stderr, "FAIL: teleport snap failed at [%d]: %.7f vs %.7f\n", i, mout[i], m_tele[i]);
            return false;
        }
    }

    // Camera interpolation + camera-cut snap guard (|dot| < 0.25 -> 160 deg flip)
    float r0[9], r1[9], rcut[9], rout[9];
    float p0[3] = {0.0f, 2.0f, 0.0f}, p1[3] = {4.0f, 2.0f, 6.0f}, pout[3]{};
    make_rot_z_3x3(0.0f, r0);
    make_rot_z_3x3(static_cast<float>(kPi / 3.0), r1);  // 60 deg
    interp.submit_camera(r0, p0, 1);
    interp.submit_camera(r1, p1, 2);
    if (!interp.evaluate_camera(0.5f, rout, pout)) return false;
    float rmid[9];
    make_rot_z_3x3(static_cast<float>(kPi / 6.0), rmid);  // 30 deg
    if (!nearf(pout[0], 2.0f) || !nearf(pout[1], 2.0f) || !nearf(pout[2], 3.0f)) {
        std::fprintf(stderr, "FAIL: camera position LERP mismatch\n");
        return false;
    }
    for (int i = 0; i < 9; ++i) {
        if (!nearf(rout[i], rmid[i], 1e-5f)) {
            std::fprintf(stderr, "FAIL: camera rotation SLERP mismatch at [%d]\n", i);
            return false;
        }
    }
    // 160 deg camera cut: cos(80 deg) = 0.1736 < 0.25 -> snaps to curr
    make_rot_z_3x3(static_cast<float>(kPi * 160.0 / 180.0 + kPi / 3.0), rcut);
    float pcut[3] = {5.0f, 2.0f, 6.0f};
    interp.submit_camera(rcut, pcut, 3);
    if (!interp.evaluate_camera(0.5f, rout, pout) || !nearf(pout[0], 5.0f)) {
        std::fprintf(stderr, "FAIL: camera orientation cut snap failed\n");
        return false;
    }

    // Unbounded capacity check: 256 simultaneous entities
    interp.clear();
    constexpr size_t kEntities = 256;
    for (size_t i = 0; i < kEntities; ++i) {
        float a[12], b[12];
        const float fi = static_cast<float>(i);
        make_rot_y_3x4(fi * 0.01f, fi * 0.5f, 0.0f, -fi * 0.25f, a);
        make_rot_y_3x4(fi * 0.01f + 0.2f, fi * 0.5f + 2.0f, 1.0f, -fi * 0.25f + 4.0f, b);
        interp.submit_entity_matrix3x4(1000 + i, a, 10);
        interp.submit_entity_matrix3x4(1000 + i, b, 11);
    }
    if (interp.entity_count() != kEntities) {
        std::fprintf(stderr, "FAIL: expected %zu entities, got %zu\n", kEntities, interp.entity_count());
        return false;
    }
    for (size_t i = 0; i < kEntities; ++i) {
        if (!interp.evaluate_entity_matrix3x4(1000 + i, 0.5f, mout)) return false;
        const float fi = static_cast<float>(i);
        if (!nearf(mout[3], fi * 0.5f + 1.0f, 1e-5f) || !nearf(mout[7], 0.5f, 1e-5f) ||
            !nearf(mout[11], -fi * 0.25f + 2.0f, 1e-5f)) {
            std::fprintf(stderr, "FAIL: entity %zu interpolation mismatch\n", i);
            return false;
        }
    }
    std::printf("[pose] LERP+SLERP (<1e-5), teleport/cut snap, camera & 256 entities OK\n");
    return true;
}

uint32_t xorshift32(uint32_t &s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

float randf(uint32_t &s, float lo, float hi) {
    const float u = static_cast<float>(xorshift32(s) & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
    return lo + (hi - lo) * u;
}

bool test_frustum_culling_simd() {
    using namespace world_visibility;

    // Perspective Vulkan reverse-Z clip rows (FOV 75 deg, aspect 16:9, near 0.5, far 400)
    const float f_y = 1.0f / std::tan(static_cast<float>(75.0 * kPi / 360.0));
    const float f_x = f_y / (16.0f / 9.0f);
    const float near_z = 0.5f, far_z = 400.0f;
    const float cx[4] = {f_x, 0.0f, 0.0f, 0.0f};
    const float cy[4] = {0.0f, f_y, 0.0f, 0.0f};
    const float cz[4] = {0.0f, 0.0f, -near_z / (far_z - near_z), (far_z * near_z) / (far_z - near_z)};
    const float cw[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    const Frustum6 frustum = frustum6_from_clip_rows(cx, cy, cz, cw);
    if (!frustum.valid) {
        std::fprintf(stderr, "FAIL: frustum6_from_clip_rows invalid\n");
        return false;
    }

    constexpr size_t kCount = 10000;
    std::vector<float> mins(kCount * 3), maxs(kCount * 3);
    std::vector<uint8_t> vis_batch(kCount, 0);
    uint32_t rng = 0xE0D40201u;
    for (size_t i = 0; i < kCount; ++i) {
        const float px = randf(rng, -350.0f, 350.0f);
        const float py = randf(rng, -250.0f, 250.0f);
        const float pz = randf(rng, -150.0f, 550.0f);
        const float ex = randf(rng, 0.1f, 25.0f);
        const float ey = randf(rng, 0.1f, 25.0f);
        const float ez = randf(rng, 0.1f, 25.0f);
        mins[i * 3 + 0] = px - ex; mins[i * 3 + 1] = py - ey; mins[i * 3 + 2] = pz - ez;
        maxs[i * 3 + 0] = px + ex; maxs[i * 3 + 1] = py + ey; maxs[i * 3 + 2] = pz + ez;
    }

    size_t mismatches = 0;
    size_t scalar_visible = 0;
    for (size_t i = 0; i < kCount; ++i) {
        const bool s_out = aabb_outside_frustum_scalar(frustum, &mins[i * 3], &maxs[i * 3]);
        const bool v_out = aabb_outside_frustum_simd(frustum, &mins[i * 3], &maxs[i * 3]);
        if (s_out != v_out) ++mismatches;
        if (!s_out) ++scalar_visible;
    }
    const size_t batch_visible = cull_aabbs_simd(frustum, mins.data(), maxs.data(), kCount, vis_batch.data());
    if (mismatches != 0 || batch_visible != scalar_visible) {
        std::fprintf(stderr, "FAIL: SIMD/scalar mismatch count=%zu batch_vis=%zu scalar_vis=%zu\n",
                     mismatches, batch_visible, scalar_visible);
        return false;
    }

    // Throughput benchmark: 100,000 AABB tests (10 passes over 10,000 randomized AABBs)
    constexpr int kPasses = 10;
    const auto t0 = std::chrono::steady_clock::now();
    volatile size_t sink = 0;
    for (int p = 0; p < kPasses; ++p) {
        sink += cull_aabbs_simd(frustum, mins.data(), maxs.data(), kCount, vis_batch.data());
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double total_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    const double ns_per_aabb = total_ns / static_cast<double>(kCount * kPasses);
    std::printf("[cull] backend=%s parity=%zu/%zu (0 mismatches) visible=%zu/%zu throughput=%.2f ns/AABB (sink=%zu)\n",
                simd_backend_name(), kCount - mismatches, kCount, scalar_visible, kCount, ns_per_aabb,
                static_cast<size_t>(sink));
    return true;
}

}  // namespace

int main() {
    if (!test_frame_pacer()) return 1;
    if (!test_pose_interpolator()) return 1;
    if (!test_frustum_culling_simd()) return 1;
    std::printf("PlumePacingCull: ALL PASSED\n");
    return 0;
}
