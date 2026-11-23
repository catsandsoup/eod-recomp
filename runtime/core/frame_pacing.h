// frame_pacing.h - Decoupled 60 Hz simulation pacing and rigid pose interpolation
// for 60 / 120 / 144 / 240 Hz / uncapped presentation (Track C0, Sprint 4P-2).
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace frame_pacing {

struct RigidPose {
    float pos[3] = {0.0f, 0.0f, 0.0f};
    float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // (x, y, z, w) unit quaternion
    float scale[3] = {1.0f, 1.0f, 1.0f};

    static inline void normalize_quat(float q[4]) {
        const double n = std::sqrt(double(q[0]) * q[0] + double(q[1]) * q[1] +
                                   double(q[2]) * q[2] + double(q[3]) * q[3]);
        if (n > 1e-12) {
            const double inv = 1.0 / n;
            for (int i = 0; i < 4; ++i) q[i] = static_cast<float>(q[i] * inv);
        } else {
            q[0] = q[1] = q[2] = 0.0f;
            q[3] = 1.0f;
        }
    }

    static inline void mat3_to_quat(const float r[9], float q[4]) {
        const double m00 = r[0], m01 = r[1], m02 = r[2];
        const double m10 = r[3], m11 = r[4], m12 = r[5];
        const double m20 = r[6], m21 = r[7], m22 = r[8];
        const double tr = m00 + m11 + m22;
        double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
        if (tr > 0.0) {
            const double s = std::sqrt(tr + 1.0) * 2.0;
            w = 0.25 * s;
            x = (m21 - m12) / s;
            y = (m02 - m20) / s;
            z = (m10 - m01) / s;
        } else if (m00 > m11 && m00 > m22) {
            const double s = std::sqrt(std::max(1e-12, 1.0 + m00 - m11 - m22)) * 2.0;
            w = (m21 - m12) / s;
            x = 0.25 * s;
            y = (m01 + m10) / s;
            z = (m02 + m20) / s;
        } else if (m11 > m22) {
            const double s = std::sqrt(std::max(1e-12, 1.0 + m11 - m00 - m22)) * 2.0;
            w = (m02 - m20) / s;
            x = (m01 + m10) / s;
            y = 0.25 * s;
            z = (m12 + m21) / s;
        } else {
            const double s = std::sqrt(std::max(1e-12, 1.0 + m22 - m00 - m11)) * 2.0;
            w = (m10 - m01) / s;
            x = (m02 + m20) / s;
            y = (m12 + m21) / s;
            z = 0.25 * s;
        }
        q[0] = static_cast<float>(x);
        q[1] = static_cast<float>(y);
        q[2] = static_cast<float>(z);
        q[3] = static_cast<float>(w);
        normalize_quat(q);
    }

    static inline void quat_to_mat3(const float q[4], float r[9]) {
        const double x = q[0], y = q[1], z = q[2], w = q[3];
        const double xx = x * x, yy = y * y, zz = z * z;
        const double xy = x * y, xz = x * z, yz = y * z;
        const double wx = w * x, wy = w * y, wz = w * z;
        r[0] = static_cast<float>(1.0 - 2.0 * (yy + zz));
        r[1] = static_cast<float>(2.0 * (xy - wz));
        r[2] = static_cast<float>(2.0 * (xz + wy));
        r[3] = static_cast<float>(2.0 * (xy + wz));
        r[4] = static_cast<float>(1.0 - 2.0 * (xx + zz));
        r[5] = static_cast<float>(2.0 * (yz - wx));
        r[6] = static_cast<float>(2.0 * (xz - wy));
        r[7] = static_cast<float>(2.0 * (yz + wx));
        r[8] = static_cast<float>(1.0 - 2.0 * (xx + yy));
    }

    static inline RigidPose from_matrix3x4(const float m[12]) {
        RigidPose p;
        p.pos[0] = m[3];
        p.pos[1] = m[7];
        p.pos[2] = m[11];
        float r[9];
        for (int c = 0; c < 3; ++c) {
            const float c0 = m[c], c1 = m[4 + c], c2 = m[8 + c];
            const float len = std::sqrt(c0 * c0 + c1 * c1 + c2 * c2);
            p.scale[c] = len;
            const float inv = len > 1e-7f ? (1.0f / len) : 0.0f;
            r[c] = c0 * inv;
            r[3 + c] = c1 * inv;
            r[6 + c] = c2 * inv;
        }
        const float det = r[0] * (r[4] * r[8] - r[5] * r[7]) -
                          r[1] * (r[3] * r[8] - r[5] * r[6]) +
                          r[2] * (r[3] * r[7] - r[4] * r[6]);
        if (det < 0.0f) {
            p.scale[0] = -p.scale[0];
            r[0] = -r[0];
            r[3] = -r[3];
            r[6] = -r[6];
        }
        if (std::fabs(det) > 1e-6f) mat3_to_quat(r, p.quat);
        return p;
    }

    inline void to_matrix3x4(float m[12]) const {
        float r[9];
        quat_to_mat3(quat, r);
        for (int row = 0; row < 3; ++row) {
            for (int c = 0; c < 3; ++c) m[row * 4 + c] = r[row * 3 + c] * scale[c];
            m[row * 4 + 3] = pos[row];
        }
    }

    static inline RigidPose from_matrix4x4(const float m[16]) { return from_matrix3x4(m); }

    inline void to_matrix4x4(float m[16]) const {
        to_matrix3x4(m);
        m[12] = m[13] = m[14] = 0.0f;
        m[15] = 1.0f;
    }

    static inline RigidPose from_camera(const float rot3x3[9], const float cam_pos[3]) {
        RigidPose p;
        for (int i = 0; i < 3; ++i) p.pos[i] = cam_pos[i];
        mat3_to_quat(rot3x3, p.quat);
        return p;
    }

    inline void to_camera(float rot3x3[9], float cam_pos[3]) const {
        quat_to_mat3(quat, rot3x3);
        for (int i = 0; i < 3; ++i) cam_pos[i] = pos[i];
    }
};

struct EntityPosePair {
    RigidPose prev{};
    RigidPose curr{};
    uint64_t last_sim_tick = 0;
    bool initialized = false;
    bool has_prev = false;
};

class FramePacer {
public:
    static constexpr double kSimHz = 60.0;
    static constexpr double kSimDt = 1.0 / kSimHz;
    static constexpr double kMaxFrameDt = 0.25;

    void configure(double target_fps, bool interpolate_override = false, bool has_override = false) {
        target_fps_ = target_fps < 0.0 ? 0.0 : target_fps;
        const bool want_interp = has_override ? interpolate_override
                                              : (target_fps_ == 0.0 || target_fps_ > kSimHz);
        interp_enabled_.store(want_interp, std::memory_order_release);
        reset();
    }

    void configure_from_env() {
        double fps = kSimHz;
        bool fps_set = false;
        if (const char *v = std::getenv("EOD_TARGET_FPS"); v && *v) {
            fps_set = true;
            if (std::strcmp(v, "uncapped") == 0 || std::strcmp(v, "unlimited") == 0) {
                fps = 0.0;
            } else {
                char *end = nullptr;
                const double parsed = std::strtod(v, &end);
                fps = (end != v && parsed >= 0.0) ? parsed : kSimHz;
            }
        }
        bool has_override = false;
        bool override_val = false;
        if (const char *v = std::getenv("EOD_INTERPOLATE"); v && *v) {
            has_override = true;
            override_val = (v[0] != '0');
        }
        if (!fps_set && !has_override) {
            configure(kSimHz, false, true);
        } else {
            configure(fps, override_val, has_override);
        }
    }

    void reset() {
        accumulator_ = 0.0;
        sim_steps_ = 0;
        present_frames_ = 0;
    }

    void advance(double frame_dt_sec) {
        const double clamped = std::clamp(frame_dt_sec, 0.0, kMaxFrameDt);
        accumulator_ += clamped;
    }

    bool should_step_sim() const { return accumulator_ + 1e-9 >= kSimDt; }

    bool consume_sim_step() {
        if (!should_step_sim()) return false;
        accumulator_ -= kSimDt;
        if (accumulator_ < 0.0) accumulator_ = 0.0;
        ++sim_steps_;
        return true;
    }

    double alpha() const {
        if (!interpolation_enabled()) return 1.0;
        return std::clamp(accumulator_ / kSimDt, 0.0, 1.0);
    }

    bool interpolation_enabled() const { return interp_enabled_.load(std::memory_order_acquire); }
    double target_fps() const { return target_fps_; }
    double present_dt() const { return target_fps_ > 0.0 ? (1.0 / target_fps_) : 0.0; }
    uint64_t sim_steps() const { return sim_steps_; }
    uint64_t present_frames() const { return present_frames_; }
    void note_present() { ++present_frames_; }

private:
    double target_fps_ = kSimHz;
    double accumulator_ = 0.0;
    uint64_t sim_steps_ = 0;
    uint64_t present_frames_ = 0;
    std::atomic<bool> interp_enabled_{false};
};

class PoseInterpolator {
public:
    static constexpr float kTeleportDist = 15.0f;
    static constexpr float kCutQuatDotMin = 0.25f;

    static inline RigidPose interpolate_pose(const RigidPose &a, const RigidPose &b, float alpha) {
        const float dx = b.pos[0] - a.pos[0];
        const float dy = b.pos[1] - a.pos[1];
        const float dz = b.pos[2] - a.pos[2];
        const float dist2 = dx * dx + dy * dy + dz * dz;
        double dot = double(a.quat[0]) * b.quat[0] + double(a.quat[1]) * b.quat[1] +
                     double(a.quat[2]) * b.quat[2] + double(a.quat[3]) * b.quat[3];
        if (dist2 > kTeleportDist * kTeleportDist || std::fabs(dot) < kCutQuatDotMin) return b;
        if (alpha <= 0.0f) return a;
        if (alpha >= 1.0f) return b;

        const double t = static_cast<double>(alpha);
        RigidPose out;
        for (int i = 0; i < 3; ++i) {
            out.pos[i] = static_cast<float>((1.0 - t) * a.pos[i] + t * b.pos[i]);
            out.scale[i] = static_cast<float>((1.0 - t) * a.scale[i] + t * b.scale[i]);
        }
        double qb[4] = {b.quat[0], b.quat[1], b.quat[2], b.quat[3]};
        if (dot < 0.0) {
            dot = -dot;
            for (int i = 0; i < 4; ++i) qb[i] = -qb[i];
        }
        double w0, w1;
        if (dot > 0.9995) {
            w0 = 1.0 - t;
            w1 = t;
        } else {
            const double theta = std::acos(std::clamp(dot, -1.0, 1.0));
            const double inv_sin = 1.0 / std::sin(theta);
            w0 = std::sin((1.0 - t) * theta) * inv_sin;
            w1 = std::sin(t * theta) * inv_sin;
        }
        for (int i = 0; i < 4; ++i) out.quat[i] = static_cast<float>(w0 * a.quat[i] + w1 * qb[i]);
        RigidPose::normalize_quat(out.quat);
        return out;
    }

    void begin_sim_tick(uint64_t sim_tick) {
        if (sim_tick > cur_tick_ && (sim_tick % 60 == 0)) {
            for (auto it = entities_.begin(); it != entities_.end();) {
                it = (it->second.last_sim_tick + 120 < sim_tick) ? entities_.erase(it) : std::next(it);
            }
        }
        cur_tick_ = sim_tick;
    }

    void submit_entity(uint64_t entity_id, const RigidPose &pose, uint64_t sim_tick) {
        auto &e = entities_[entity_id];
        if (!e.initialized) {
            e.prev = pose;
            e.curr = pose;
            e.last_sim_tick = sim_tick;
            e.initialized = true;
            e.has_prev = false;
        } else if (e.last_sim_tick != sim_tick) {
            e.prev = e.curr;
            e.curr = pose;
            e.last_sim_tick = sim_tick;
            e.has_prev = true;
        } else {
            e.curr = pose;
        }
    }

    void submit_entity_matrix3x4(uint64_t entity_id, const float model[12], uint64_t sim_tick) {
        submit_entity(entity_id, RigidPose::from_matrix3x4(model), sim_tick);
    }

    bool evaluate_entity(uint64_t entity_id, float alpha, RigidPose &out) const {
        const auto it = entities_.find(entity_id);
        if (it == entities_.end()) return false;
        out = it->second.has_prev ? interpolate_pose(it->second.prev, it->second.curr, alpha)
                                  : it->second.curr;
        return true;
    }

    bool evaluate_entity_matrix3x4(uint64_t entity_id, float alpha, float out_model[12]) const {
        RigidPose pose;
        if (!evaluate_entity(entity_id, alpha, pose)) return false;
        pose.to_matrix3x4(out_model);
        return true;
    }

    void submit_camera(const float rot3x3[9], const float pos[3], uint64_t sim_tick) {
        const RigidPose cam = RigidPose::from_camera(rot3x3, pos);
        if (!camera_init_) {
            camera_.prev = cam;
            camera_.curr = cam;
            camera_.has_prev = false;
            camera_.last_sim_tick = sim_tick;
            camera_init_ = true;
        } else if (camera_.last_sim_tick != sim_tick) {
            camera_.prev = camera_.curr;
            camera_.curr = cam;
            camera_.has_prev = true;
            camera_.last_sim_tick = sim_tick;
        } else {
            camera_.curr = cam;
        }
    }

    bool evaluate_camera(float alpha, float out_rot3x3[9], float out_pos[3]) const {
        if (!camera_init_) return false;
        const RigidPose p = camera_.has_prev ? interpolate_pose(camera_.prev, camera_.curr, alpha)
                                             : camera_.curr;
        p.to_camera(out_rot3x3, out_pos);
        return true;
    }

    size_t entity_count() const { return entities_.size(); }
    void clear() {
        entities_.clear();
        camera_init_ = false;
        camera_ = {};
        cur_tick_ = 0;
    }

private:
    std::unordered_map<uint64_t, EntityPosePair> entities_;
    EntityPosePair camera_{};
    bool camera_init_ = false;
    uint64_t cur_tick_ = 0;
};

inline FramePacer &global_pacer() {
    static FramePacer pacer;
    return pacer;
}

}  // namespace frame_pacing
