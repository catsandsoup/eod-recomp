#include "camera.h"
#include <algorithm>

namespace eod::editor {

EditorCamera::EditorCamera() {
    ResetToDefault();
}

void EditorCamera::ResetToDefault() {
    orbit_state_.enabled = true;
    orbit_state_.active = true;
    orbit_state_.turntable_enabled = false; // default off in editor for precision editing
    orbit_state_.yaw = 0.785398f;           // 45 degrees
    orbit_state_.pitch = 0.35f;             // ~20 degrees elevation
    orbit_state_.distance = 6.0f;
    orbit_state_.target_yaw = orbit_state_.yaw;
    orbit_state_.target_pitch = orbit_state_.pitch;
    orbit_state_.target_distance = orbit_state_.distance;
    orbit_state_.center = {0.0f, 0.75f, 0.0f};

    flycam_pos_ = {0.0f, 1.5f, -6.0f};
    flycam_yaw_ = 0.0f;
    flycam_pitch_ = 0.2f;
}

void EditorCamera::FocusOn(const Vector3f& target, float distance) {
    orbit_state_.center = target;
    orbit_state_.target_distance = std::clamp(distance, orbit_state_.min_distance, orbit_state_.max_distance);
    orbit_state_.distance = orbit_state_.target_distance;
}

void EditorCamera::Update(float dt) {
    if (dt <= 0.0f) return;

    if (mode_ == CameraMode::Orbit) {
        // Turntable animation
        if (orbit_state_.turntable_enabled && !orbit_state_.user_controlling) {
            orbit_state_.target_yaw += orbit_state_.turntable_speed * dt;
            if (orbit_state_.target_yaw > 6.2831853f) {
                orbit_state_.target_yaw -= 6.2831853f;
                orbit_state_.yaw -= 6.2831853f;
            }
        }

        // Exponential smoothing towards targets
        float factor = 1.0f - std::exp(-15.0f * dt);
        orbit_state_.yaw += (orbit_state_.target_yaw - orbit_state_.yaw) * factor;
        orbit_state_.pitch += (orbit_state_.target_pitch - orbit_state_.pitch) * factor;
        orbit_state_.distance += (orbit_state_.target_distance - orbit_state_.distance) * factor;
    }
}

void EditorCamera::OnMouseDrag(float dx, float dy, bool is_orbit, bool is_pan) {
    if (mode_ == CameraMode::Orbit) {
        if (is_orbit) {
            orbit_state_.target_yaw += dx * orbit_state_.mouse_sensitivity_yaw;
            orbit_state_.target_pitch -= dy * orbit_state_.mouse_sensitivity_pitch;
            orbit_state_.target_pitch = std::clamp(orbit_state_.target_pitch, orbit_state_.min_pitch, orbit_state_.max_pitch);
        } else if (is_pan) {
            // Pan pivot along camera Right and Up vectors
            Vector3f eye = GetPosition();
            Vector3f fwd = (orbit_state_.center - eye).normalized();
            Vector3f world_up{0.0f, 1.0f, 0.0f};
            Vector3f right = cross(fwd, world_up).normalized();
            Vector3f up = cross(right, fwd).normalized();

            float pan_scale = orbit_state_.distance * 0.0015f;
            orbit_state_.center = orbit_state_.center - (right * (dx * pan_scale)) + (up * (dy * pan_scale));
        }
    } else {
        // Flycam look
        flycam_yaw_ += dx * 0.003f;
        flycam_pitch_ -= dy * 0.003f;
        flycam_pitch_ = std::clamp(flycam_pitch_, -1.5f, 1.5f);
    }
}

void EditorCamera::OnMouseScroll(float delta) {
    if (mode_ == CameraMode::Orbit) {
        orbit_state_.target_distance -= delta * orbit_state_.zoom_speed_wheel;
        orbit_state_.target_distance = std::clamp(orbit_state_.target_distance, orbit_state_.min_distance, orbit_state_.max_distance);
    } else {
        Vector3f fwd = GetForward();
        flycam_pos_ = flycam_pos_ + (fwd * (delta * 0.5f));
    }
}

Vector3f EditorCamera::GetPosition() const {
    if (mode_ == CameraMode::Orbit) {
        float cp = std::cos(orbit_state_.pitch);
        float sp = std::sin(orbit_state_.pitch);
        float cy = std::cos(orbit_state_.yaw);
        float sy = std::sin(orbit_state_.yaw);

        // Spherical to Cartesian coordinates around center pivot
        float ox = orbit_state_.distance * cp * sy;
        float oy = orbit_state_.distance * sp;
        float oz = orbit_state_.distance * cp * cy;

        return {orbit_state_.center.x + ox, orbit_state_.center.y + oy, orbit_state_.center.z + oz};
    }
    return flycam_pos_;
}

Vector3f EditorCamera::GetForward() const {
    if (mode_ == CameraMode::Orbit) {
        return (orbit_state_.center - GetPosition()).normalized();
    }
    float cp = std::cos(flycam_pitch_);
    float sp = std::sin(flycam_pitch_);
    float cy = std::cos(flycam_yaw_);
    float sy = std::sin(flycam_yaw_);
    return {cp * sy, sp, cp * cy};
}

Matrix4x4f EditorCamera::GetViewMatrix() const {
    Vector3f eye = GetPosition();
    Vector3f fwd = GetForward();
    Vector3f world_up{0.0f, 1.0f, 0.0f};

    Vector3f right = cross(fwd, world_up).normalized();
    Vector3f up = cross(right, fwd).normalized();

    Matrix4x4f v;
    // Row 0: Right
    v.m[0][0] = right.x;
    v.m[0][1] = right.y;
    v.m[0][2] = right.z;
    v.m[0][3] = -dot(right, eye);

    // Row 1: Up
    v.m[1][0] = up.x;
    v.m[1][1] = up.y;
    v.m[1][2] = up.z;
    v.m[1][3] = -dot(up, eye);

    // Row 2: -Forward (view space looks down -Z)
    v.m[2][0] = -fwd.x;
    v.m[2][1] = -fwd.y;
    v.m[2][2] = -fwd.z;
    v.m[2][3] = dot(fwd, eye);

    // Row 3: Identity row
    v.m[3][0] = 0.0f;
    v.m[3][1] = 0.0f;
    v.m[3][2] = 0.0f;
    v.m[3][3] = 1.0f;

    return v;
}

Matrix4x4f EditorCamera::GetProjectionMatrix(float aspect_ratio) const {
    if (aspect_ratio <= 0.0f) aspect_ratio = 1.0f;

    float tan_half_fov = std::tan(fov_y_radians_ * 0.5f);
    float f = 1.0f / tan_half_fov;

    Matrix4x4f p;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            p.m[r][c] = 0.0f;
        }
    }

    p.m[0][0] = f / aspect_ratio;
    p.m[1][1] = f;
    p.m[2][2] = far_z_ / (near_z_ - far_z_);
    p.m[2][3] = (near_z_ * far_z_) / (near_z_ - far_z_);
    p.m[3][2] = -1.0f;
    p.m[3][3] = 0.0f;

    return p;
}

Vector3f EditorCamera::ScreenToWorldRay(float mouse_x, float mouse_y, float viewport_w, float viewport_h) const {
    if (viewport_w <= 0.0f || viewport_h <= 0.0f) return {0.0f, 0.0f, -1.0f};

    // Normalized Device Coordinates (-1 to 1)
    float ndc_x = (2.0f * mouse_x) / viewport_w - 1.0f;
    float ndc_y = 1.0f - (2.0f * mouse_y) / viewport_h;

    float aspect = viewport_w / viewport_h;
    float tan_fov = std::tan(fov_y_radians_ * 0.5f);

    Vector3f fwd = GetForward();
    Vector3f world_up{0.0f, 1.0f, 0.0f};
    Vector3f right = cross(fwd, world_up).normalized();
    Vector3f up = cross(right, fwd).normalized();

    Vector3f ray_dir = (fwd + (right * (ndc_x * aspect * tan_fov)) + (up * (ndc_y * tan_fov))).normalized();
    return ray_dir;
}

bool EditorCamera::RayIntersectsGroundPlane(const Vector3f& ray_origin, const Vector3f& ray_dir, Vector3f& out_intersection) const {
    if (std::abs(ray_dir.y) < 1e-6f) return false;

    float t = -ray_origin.y / ray_dir.y;
    if (t < 0.0f) return false; // Behind ray

    out_intersection = ray_origin + (ray_dir * t);
    return true;
}

} // namespace eod::editor
