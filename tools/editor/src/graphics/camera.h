#pragma once

#include "../../../src/hle_showroom.h"
#include <cmath>

namespace eod::editor {

enum class CameraMode {
    Orbit,
    Flycam
};

class EditorCamera {
public:
    EditorCamera();

    void Update(float dt);

    // Interaction handling
    void OnMouseDrag(float dx, float dy, bool is_orbit, bool is_pan);
    void OnMouseScroll(float delta);

    void SetMode(CameraMode mode) { mode_ = mode; }
    CameraMode GetMode() const { return mode_; }

    void FocusOn(const Vector3f& target, float distance = 5.0f);
    void SetTurntable(bool enabled) { orbit_state_.turntable_enabled = enabled; }
    bool IsTurntableEnabled() const { return orbit_state_.turntable_enabled; }
    void ResetToDefault();

    // Matrix calculations
    Matrix4x4f GetViewMatrix() const;
    Matrix4x4f GetProjectionMatrix(float aspect_ratio) const;

    // Spatial queries
    Vector3f GetPosition() const;
    Vector3f GetForward() const;
    Vector3f GetPivot() const { return orbit_state_.center; }
    float GetDistance() const { return orbit_state_.distance; }

    // Screen picking / Raycasting
    Vector3f ScreenToWorldRay(float mouse_x, float mouse_y, float viewport_w, float viewport_h) const;
    bool RayIntersectsGroundPlane(const Vector3f& ray_origin, const Vector3f& ray_dir, Vector3f& out_intersection) const;

    ShowroomOrbitState& GetOrbitState() { return orbit_state_; }
    const ShowroomOrbitState& GetOrbitState() const { return orbit_state_; }

private:
    CameraMode mode_ = CameraMode::Orbit;
    ShowroomOrbitState orbit_state_;

    // Flycam parameters
    Vector3f flycam_pos_{0.0f, 1.5f, -4.0f};
    float flycam_yaw_ = 0.0f;
    float flycam_pitch_ = 0.2f;
    float flycam_speed_ = 5.0f;

    // Lens parameters
    float fov_y_radians_ = 1.04719755f; // ~60 degrees
    float near_z_ = 0.1f;
    float far_z_ = 1000.0f;
};

} // namespace eod::editor
