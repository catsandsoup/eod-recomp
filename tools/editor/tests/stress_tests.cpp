#include <iostream>
#include <iomanip>
#include <cassert>
#include <cmath>
#include <vector>
#include <string>
#include <filesystem>
#include <chrono>

#include "../src/graphics/camera.h"
#include "../src/graphics/viewport_renderer.h"
#include "../src/project/project.h"
#include "../src/validator/schema_validator.h"

namespace fs = std::filesystem;
using namespace eod::editor;

// -----------------------------------------------------------------------------
// General 4x4 Matrix Utilities for Invertibility & Condition Analysis
// -----------------------------------------------------------------------------

double Mat4x4Determinant(const Matrix4x4f& mat) {
    const float (*m)[4] = mat.m;

    double d00 = m[1][1] * (m[2][2] * m[3][3] - m[2][3] * m[3][2]) -
                 m[1][2] * (m[2][1] * m[3][3] - m[2][3] * m[3][1]) +
                 m[1][3] * (m[2][1] * m[3][2] - m[2][2] * m[3][1]);

    double d01 = m[1][0] * (m[2][2] * m[3][3] - m[2][3] * m[3][2]) -
                 m[1][2] * (m[2][0] * m[3][3] - m[2][3] * m[3][0]) +
                 m[1][3] * (m[2][0] * m[3][2] - m[2][2] * m[3][0]);

    double d02 = m[1][0] * (m[2][1] * m[3][3] - m[2][3] * m[3][1]) -
                 m[1][1] * (m[2][0] * m[3][3] - m[2][3] * m[3][0]) +
                 m[1][3] * (m[2][0] * m[3][1] - m[2][1] * m[3][0]);

    double d03 = m[1][0] * (m[2][1] * m[3][2] - m[2][2] * m[3][1]) -
                 m[1][1] * (m[2][0] * m[3][2] - m[2][2] * m[3][0]) +
                 m[1][2] * (m[2][0] * m[3][1] - m[2][1] * m[3][0]);

    return m[0][0] * d00 - m[0][1] * d01 + m[0][2] * d02 - m[0][3] * d03;
}

bool Mat4x4Inverse(const Matrix4x4f& in, Matrix4x4f& out) {
    double det = Mat4x4Determinant(in);
    if (std::abs(det) < 1e-9) {
        return false;
    }

    double inv_det = 1.0 / det;
    const float (*m)[4] = in.m;

    // Row 0
    out.m[0][0] =  (m[1][1]*(m[2][2]*m[3][3] - m[2][3]*m[3][2]) - m[1][2]*(m[2][1]*m[3][3] - m[2][3]*m[3][1]) + m[1][3]*(m[2][1]*m[3][2] - m[2][2]*m[3][1])) * inv_det;
    out.m[0][1] = -(m[0][1]*(m[2][2]*m[3][3] - m[2][3]*m[3][2]) - m[0][2]*(m[2][1]*m[3][3] - m[2][3]*m[3][1]) + m[0][3]*(m[2][1]*m[3][2] - m[2][2]*m[3][1])) * inv_det;
    out.m[0][2] =  (m[0][1]*(m[1][2]*m[3][3] - m[1][3]*m[3][2]) - m[0][2]*(m[1][1]*m[3][3] - m[1][3]*m[3][1]) + m[0][3]*(m[1][1]*m[3][2] - m[1][2]*m[3][1])) * inv_det;
    out.m[0][3] = -(m[0][1]*(m[1][2]*m[2][3] - m[1][3]*m[2][2]) - m[0][2]*(m[1][1]*m[2][3] - m[1][3]*m[2][1]) + m[0][3]*(m[1][1]*m[2][2] - m[1][2]*m[2][1])) * inv_det;

    // Row 1
    out.m[1][0] = -(m[1][0]*(m[2][2]*m[3][3] - m[2][3]*m[3][2]) - m[1][2]*(m[2][0]*m[3][3] - m[2][3]*m[3][0]) + m[1][3]*(m[2][0]*m[3][2] - m[2][2]*m[3][0])) * inv_det;
    out.m[1][1] =  (m[0][0]*(m[2][2]*m[3][3] - m[2][3]*m[3][2]) - m[0][2]*(m[2][0]*m[3][3] - m[2][3]*m[3][0]) + m[0][3]*(m[2][0]*m[3][2] - m[2][2]*m[3][0])) * inv_det;
    out.m[1][2] = -(m[0][0]*(m[1][2]*m[3][3] - m[1][3]*m[3][2]) - m[0][2]*(m[1][0]*m[3][3] - m[1][3]*m[3][0]) + m[0][3]*(m[1][0]*m[3][2] - m[1][2]*m[3][0])) * inv_det;
    out.m[1][3] =  (m[0][0]*(m[1][2]*m[2][3] - m[1][3]*m[2][2]) - m[0][2]*(m[1][0]*m[2][3] - m[1][3]*m[2][0]) + m[0][3]*(m[1][0]*m[2][2] - m[1][2]*m[2][0])) * inv_det;

    // Row 2
    out.m[2][0] =  (m[1][0]*(m[2][1]*m[3][3] - m[2][3]*m[3][1]) - m[1][1]*(m[2][0]*m[3][3] - m[2][3]*m[3][0]) + m[1][3]*(m[2][0]*m[3][1] - m[2][1]*m[3][0])) * inv_det;
    out.m[2][1] = -(m[0][0]*(m[2][1]*m[3][3] - m[2][3]*m[3][1]) - m[0][1]*(m[2][0]*m[3][3] - m[2][3]*m[3][0]) + m[0][3]*(m[2][0]*m[3][1] - m[2][1]*m[3][0])) * inv_det;
    out.m[2][2] =  (m[0][0]*(m[1][1]*m[3][3] - m[1][3]*m[3][1]) - m[0][1]*(m[1][0]*m[3][3] - m[1][3]*m[3][0]) + m[0][3]*(m[1][0]*m[3][1] - m[1][1]*m[3][0])) * inv_det;
    out.m[2][3] = -(m[0][0]*(m[1][1]*m[2][3] - m[1][3]*m[2][1]) - m[0][1]*(m[1][0]*m[2][3] - m[1][3]*m[2][0]) + m[0][3]*(m[1][0]*m[2][1] - m[1][1]*m[2][0])) * inv_det;

    // Row 3
    out.m[3][0] = -(m[1][0]*(m[2][1]*m[3][2] - m[2][2]*m[3][1]) - m[1][1]*(m[2][0]*m[3][2] - m[2][2]*m[3][0]) + m[1][2]*(m[2][0]*m[3][1] - m[2][1]*m[3][0])) * inv_det;
    out.m[3][1] =  (m[0][0]*(m[2][1]*m[3][2] - m[2][2]*m[3][1]) - m[0][1]*(m[2][0]*m[3][2] - m[2][2]*m[3][0]) + m[0][2]*(m[2][0]*m[3][1] - m[2][1]*m[3][0])) * inv_det;
    out.m[3][2] = -(m[0][0]*(m[1][1]*m[3][2] - m[1][2]*m[3][1]) - m[0][1]*(m[1][0]*m[3][2] - m[1][2]*m[3][0]) + m[0][2]*(m[1][0]*m[3][1] - m[1][1]*m[3][0])) * inv_det;
    out.m[3][3] =  (m[0][0]*(m[1][1]*m[2][2] - m[1][2]*m[2][1]) - m[0][1]*(m[1][0]*m[2][2] - m[1][2]*m[2][0]) + m[0][2]*(m[1][0]*m[2][1] - m[1][1]*m[2][0])) * inv_det;

    return true;
}

double MatrixMultiplicationError(const Matrix4x4f& a, const Matrix4x4f& b_inv) {
    // Computes ||a * b_inv - I||_F (Frobenius norm)
    double err_sq = 0.0;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += (double)a.m[r][k] * (double)b_inv.m[k][c];
            }
            double expected = (r == c) ? 1.0 : 0.0;
            double diff = sum - expected;
            err_sq += diff * diff;
        }
    }
    return std::sqrt(err_sq);
}

// -----------------------------------------------------------------------------
// Test 1: Math Parity & Boundary Clamping Stress Test
// -----------------------------------------------------------------------------

void RunCameraMathStressTest() {
    std::cout << "\n========================================================" << std::endl;
    std::cout << "[STRESS TEST 1] Camera Math Parity, Bounds & Invertibility" << std::endl;
    std::cout << "========================================================" << std::endl;

    EditorCamera camera;
    const ShowroomOrbitState& state = camera.GetOrbitState();

    std::cout << "\n[1.1] Inspecting Current OrbitState Boundaries:" << std::endl;
    std::cout << "  orbit_state_.min_pitch = " << state.min_pitch << " rad (" << (state.min_pitch * 180.0f / M_PI) << " deg)" << std::endl;
    std::cout << "  orbit_state_.max_pitch = " << state.max_pitch << " rad (" << (state.max_pitch * 180.0f / M_PI) << " deg)" << std::endl;
    std::cout << "  orbit_state_.min_distance = " << state.min_distance << " meters" << std::endl;
    std::cout << "  orbit_state_.max_distance = " << state.max_distance << " meters" << std::endl;

    // Check against Dispatch criteria:
    // Pitch bounds: -89 to +89 degrees (~ -1.5533 to +1.5533 rad)
    // Distance bounds: min 0.5m, max 100m
    float req_min_pitch = -89.0f * M_PI / 180.0f;
    float req_max_pitch = 89.0f * M_PI / 180.0f;
    float req_min_dist = 0.5f;
    float req_max_dist = 100.0f;

    std::cout << "\n[1.2] Checking Dispatch Requirements vs Code Implementation:" << std::endl;
    bool pitch_parity = (std::abs(state.min_pitch - req_min_pitch) < 0.05f) &&
                        (std::abs(state.max_pitch - req_max_pitch) < 0.05f);
    bool dist_parity = (std::abs(state.min_distance - req_min_dist) < 0.1f) &&
                       (std::abs(state.max_distance - req_max_dist) < 0.1f);

    std::cout << "  Required Pitch Bounds: [-89 deg, +89 deg] -> [" << req_min_pitch << ", " << req_max_pitch << "]" << std::endl;
    std::cout << "  Actual Pitch Bounds:   [" << (state.min_pitch * 180.0f / M_PI) << " deg, " << (state.max_pitch * 180.0f / M_PI) << " deg]" << std::endl;
    std::cout << "  Pitch Parity Match:    " << (pitch_parity ? "PASS" : "FAIL (MISMATCH)") << std::endl;

    std::cout << "  Required Dist Bounds:  [0.5m, 100.0m]" << std::endl;
    std::cout << "  Actual Dist Bounds:    [" << state.min_distance << "m, " << state.max_distance << "m]" << std::endl;
    std::cout << "  Distance Parity Match: " << (dist_parity ? "PASS" : "FAIL (MISMATCH)") << std::endl;

    // Stress test mouse drag and scroll clamping
    std::cout << "\n[1.3] Testing Interaction Boundary Clamping:" << std::endl;
    camera.ResetToDefault();
    // Huge drag upwards (should clamp at max_pitch)
    camera.OnMouseDrag(0.0f, -10000.0f, true, false);
    std::cout << "  Pitch after huge upward drag: " << camera.GetOrbitState().target_pitch << " rad" << std::endl;

    // Huge drag downwards (should clamp at min_pitch)
    camera.OnMouseDrag(0.0f, 20000.0f, true, false);
    std::cout << "  Pitch after huge downward drag: " << camera.GetOrbitState().target_pitch << " rad" << std::endl;

    // Huge scroll in (zoom in)
    camera.OnMouseScroll(1000.0f);
    std::cout << "  Distance after huge zoom-in: " << camera.GetOrbitState().target_distance << " m" << std::endl;

    // Huge scroll out (zoom out)
    camera.OnMouseScroll(-2000.0f);
    std::cout << "  Distance after huge zoom-out: " << camera.GetOrbitState().target_distance << " m" << std::endl;

    // 1.4 Test View/Projection Matrix Invertibility across Pitch/Yaw/Distance spectrum
    std::cout << "\n[1.4] Testing Matrix Invertibility & Gimbal Lock across Parameter Space:" << std::endl;

    struct TestConfig {
        float yaw;
        float pitch;
        float distance;
        const char* label;
    };

    std::vector<TestConfig> test_configs = {
        {0.0f, 0.0f, 5.0f, "Standard Eye Level"},
        {0.785f, 0.35f, 6.0f, "Default Showroom 45-deg"},
        {3.14159f, 0.5f, 2.8f, "Orbit Min Distance (2.8m)"},
        {0.0f, -0.15f, 9.5f, "Orbit Min Pitch (-8.5 deg)"},
        {0.0f, 1.15f, 9.5f, "Orbit Max Pitch (+65.9 deg)"},
        {0.0f, req_min_pitch, 0.5f, "Dispatch Min Boundary (-89 deg, 0.5m)"},
        {0.0f, req_max_pitch, 100.0f, "Dispatch Max Boundary (+89 deg, 100m)"},
        {0.0f, 1.570796f, 5.0f, "Exact Overhead Gimbal Point (+90 deg)"},
        {0.0f, -1.570796f, 5.0f, "Exact Nadir Gimbal Point (-90 deg)"}
    };

    int nan_count = 0;
    int singular_count = 0;

    for (const auto& cfg : test_configs) {
        ShowroomOrbitState& s = camera.GetOrbitState();
        s.yaw = cfg.yaw;
        s.target_yaw = cfg.yaw;
        s.pitch = cfg.pitch;
        s.target_pitch = cfg.pitch;
        s.distance = cfg.distance;
        s.target_distance = cfg.distance;

        Matrix4x4f view = camera.GetViewMatrix();
        Matrix4x4f proj = camera.GetProjectionMatrix(16.0f / 9.0f);

        double det_v = Mat4x4Determinant(view);
        double det_p = Mat4x4Determinant(proj);

        bool v_nan = false;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                if (std::isnan(view.m[r][c]) || std::isinf(view.m[r][c])) v_nan = true;
                if (std::isnan(proj.m[r][c]) || std::isinf(proj.m[r][c])) v_nan = true;
            }
        }

        Matrix4x4f inv_v;
        bool inv_ok = Mat4x4Inverse(view, inv_v);
        double err = inv_ok ? MatrixMultiplicationError(view, inv_v) : 999.0;

        if (v_nan) ++nan_count;
        if (!inv_ok || std::abs(det_v) < 1e-4) ++singular_count;

        std::cout << "  [" << cfg.label << "] Pitch: " << std::fixed << std::setprecision(2)
                  << (cfg.pitch * 180.0f / M_PI) << " deg, Dist: " << cfg.distance << "m"
                  << " | det(View)=" << std::setprecision(4) << det_v
                  << " | Invertible=" << (inv_ok ? "YES" : "NO")
                  << " | InvErr=" << std::scientific << std::setprecision(2) << err
                  << " | NaN=" << (v_nan ? "DETECTED!" : "None") << std::endl;
    }

    std::cout << "\n  Summary for 1.4: " << nan_count << " NaNs, " << singular_count << " singular/degenerate matrices." << std::endl;
}

// -----------------------------------------------------------------------------
// Test 2: Project Serialization & Deserialization Roundtrip Fidelity
// -----------------------------------------------------------------------------

void RunProjectSerializationStressTest() {
    std::cout << "\n========================================================" << std::endl;
    std::cout << "[STRESS TEST 2] Project Serialization Byte-Exact Roundtrip" << std::endl;
    std::cout << "========================================================" << std::endl;

    fs::path temp_base = fs::temp_directory_path() / "eod_challenger_roundtrip";
    std::error_code ec;
    fs::remove_all(temp_base, ec);
    fs::create_directories(temp_base, ec);

    // 2.1 Vehicle Manifest Comprehensive Roundtrip
    std::cout << "\n[2.1] Testing Vehicle Manifest Comprehensive Roundtrip:" << std::endl;
    EditorProject proj1;
    proj1.CreateNew(ProjectType::Vehicle, "Crusher Magnum 500", "mod.chassis.crusher_magnum");
    proj1.SetVehicleClass("heavy");

    auto& p = proj1.GetPhysics();
    p.mass_kg = 1750.0f;
    p.wheelbase_meters = 2.95f;
    p.track_front_meters = 1.62f;
    p.track_rear_meters = 1.65f;
    p.center_of_mass = {0.05f, 0.48f, 0.15f};
    p.moment_of_inertia = {3200.0f, 3600.0f, 950.0f}; // Custom inertia
    p.aerodynamic_drag = 0.38f;                       // Custom drag
    p.frontal_area_m2 = 2.15f;                         // Custom frontal area
    p.chassis_health_max = 1.5f;                       // Heavy duty chassis
    p.damage_scale = 0.45f;                            // Reinforced armor
    p.displacement_liters = 7.0f;                      // 427ci big block
    p.peak_power_bhp = 490.0f;
    p.peak_power_rpm = 5600.0f;
    p.peak_torque_nm = 680.0f;
    p.peak_torque_rpm = 3800.0f;
    p.idle_rpm = 950.0f;
    p.redline_rpm = 6200.0f;
    p.soundbank = "audio/big_block_v8.json";
    p.tire_model = "brush_contact";
    p.nominal_friction = 1.05f;                        // High grip racing tires
    p.removal_impulse_threshold_ns = 55000.0f;

    // Add 2 custom sockets
    SocketDefinition s1;
    s1.name = "winch_front_bumper";
    s1.type = "weapon_hardpoint";
    s1.position = {0.0f, 0.45f, 2.40f};
    proj1.AddSocket(s1);

    SocketDefinition s2;
    s2.name = "rollcage_crossbar";
    s2.type = "interior_anchor";
    s2.position = {0.0f, 1.25f, -0.30f};
    proj1.AddSocket(s2);

    fs::path dir_cycle1 = temp_base / "cycle1";
    fs::path dir_cycle2 = temp_base / "cycle2";
    fs::path dir_cycle3 = temp_base / "cycle3";

    std::string err;
    bool s_ok = proj1.SaveToDirectory(dir_cycle1.string(), err);
    assert(s_ok);

    // Cycle 2: Load from cycle 1, save to cycle 2
    EditorProject proj2;
    bool l_ok = proj2.LoadFromDirectory(dir_cycle1.string(), err);
    assert(l_ok);
    proj2.SaveToDirectory(dir_cycle2.string(), err);

    // Cycle 3: Load from cycle 2, save to cycle 3
    EditorProject proj3;
    proj3.LoadFromDirectory(dir_cycle2.string(), err);
    proj3.SaveToDirectory(dir_cycle3.string(), err);

    // Read raw JSON text from cycle 1 and cycle 2
    std::ifstream f1(dir_cycle1 / "vehicle.manifest.json");
    std::stringstream buf1;
    buf1 << f1.rdbuf();
    std::string text1 = buf1.str();

    std::ifstream f2(dir_cycle2 / "vehicle.manifest.json");
    std::stringstream buf2;
    buf2 << f2.rdbuf();
    std::string text2 = buf2.str();

    std::ifstream f3(dir_cycle3 / "vehicle.manifest.json");
    std::stringstream buf3;
    buf3 << f3.rdbuf();
    std::string text3 = buf3.str();

    bool byte_exact_1_2 = (text1 == text2);
    bool byte_exact_2_3 = (text2 == text3);

    std::cout << "  Vehicle Cycle 1 size: " << text1.size() << " bytes" << std::endl;
    std::cout << "  Vehicle Cycle 2 size: " << text2.size() << " bytes" << std::endl;
    std::cout << "  Vehicle Cycle 3 size: " << text3.size() << " bytes" << std::endl;
    std::cout << "  Cycle 1 vs Cycle 2 Byte-Exact Match: " << (byte_exact_1_2 ? "PASS (EXACT)" : "FAIL (DIFF)") << std::endl;
    std::cout << "  Cycle 2 vs Cycle 3 Byte-Exact Match: " << (byte_exact_2_3 ? "PASS (EXACT)" : "FAIL (DIFF)") << std::endl;

    if (!byte_exact_1_2) {
        std::cout << "\n  [DISCREPANCY DETECTED] Investigating lost fields between Cycle 1 and Cycle 2:" << std::endl;
        nlohmann::json j1 = nlohmann::json::parse(text1);
        nlohmann::json j2 = nlohmann::json::parse(text2);

        // Check tire nominal friction
        float f1_fric = j1["physics"]["tires"]["nominal_friction"];
        float f2_fric = j2["physics"]["tires"]["nominal_friction"];
        std::cout << "    physics.tires.nominal_friction: Cycle 1 = " << f1_fric << " vs Cycle 2 = " << f2_fric
                  << (f1_fric == f2_fric ? " (MATCH)" : " (LOST -> RESET TO DEFAULT!)") << std::endl;

        // Check moment of inertia
        auto f1_moi = j1["physics"]["chassis"]["moment_of_inertia"];
        auto f2_moi = j2["physics"]["chassis"]["moment_of_inertia"];
        std::cout << "    physics.chassis.moment_of_inertia: Cycle 1 = " << f1_moi.dump() << " vs Cycle 2 = " << f2_moi.dump()
                  << (f1_moi == f2_moi ? " (MATCH)" : " (LOST -> RESET TO DEFAULT!)") << std::endl;

        // Check aerodynamic drag
        float f1_drag = j1["physics"]["chassis"]["aerodynamic_drag"];
        float f2_drag = j2["physics"]["chassis"]["aerodynamic_drag"];
        std::cout << "    physics.chassis.aerodynamic_drag: Cycle 1 = " << f1_drag << " vs Cycle 2 = " << f2_drag
                  << (f1_drag == f2_drag ? " (MATCH)" : " (LOST -> RESET TO DEFAULT!)") << std::endl;

        // Check engine peak torque
        float f1_tq = j1["physics"]["engine"]["peak_torque_nm"];
        float f2_tq = j2["physics"]["engine"]["peak_torque_nm"];
        std::cout << "    physics.engine.peak_torque_nm: Cycle 1 = " << f1_tq << " vs Cycle 2 = " << f2_tq
                  << (f1_tq == f2_tq ? " (MATCH)" : " (LOST -> RESET TO DEFAULT!)") << std::endl;

        // Check soundbank
        std::string f1_snd = j1["physics"]["engine"]["soundbank"];
        std::string f2_snd = j2["physics"]["engine"]["soundbank"];
        std::cout << "    physics.engine.soundbank: Cycle 1 = " << f1_snd << " vs Cycle 2 = " << f2_snd
                  << (f1_snd == f2_snd ? " (MATCH)" : " (LOST -> RESET TO DEFAULT!)") << std::endl;
    }

    // 2.2 Track Manifest Roundtrip
    std::cout << "\n[2.2] Testing Track Manifest Comprehensive Roundtrip:" << std::endl;
    EditorProject track_proj1;
    track_proj1.CreateNew(ProjectType::Track, "Devil's Bowl Speedway", "mod.track.devils_bowl");
    auto& td = track_proj1.GetTrackData();
    td.track_type = "figure8";
    td.surface_type = "asphalt_slick";
    td.length_meters = 1850.0f;
    td.scene_model = "custom_geometry/devils_bowl_highpoly.gltf";
    td.skybox = "textures/stormy_night.hdr";
    td.terrain_mesh = "custom_geometry/devils_bowl_collision.gltf";
    td.barrier_mesh = "custom_geometry/concrete_k_rails.gltf";
    td.primary_spline = "splines/figure8_crossing.spline.json";
    td.supported_events = {"Figure8", "Derby", "TrailerRace"};

    fs::path t_cycle1 = temp_base / "track_cycle1";
    fs::path t_cycle2 = temp_base / "track_cycle2";

    track_proj1.SaveToDirectory(t_cycle1.string(), err);
    EditorProject track_proj2;
    track_proj2.LoadFromDirectory(t_cycle1.string(), err);
    track_proj2.SaveToDirectory(t_cycle2.string(), err);

    std::ifstream tf1(t_cycle1 / "track.manifest.json");
    std::stringstream tbuf1;
    tbuf1 << tf1.rdbuf();
    std::string ttext1 = tbuf1.str();

    std::ifstream tf2(t_cycle2 / "track.manifest.json");
    std::stringstream tbuf2;
    tbuf2 << tf2.rdbuf();
    std::string ttext2 = tbuf2.str();

    bool track_byte_exact = (ttext1 == ttext2);
    std::cout << "  Track Cycle 1 size: " << ttext1.size() << " bytes" << std::endl;
    std::cout << "  Track Cycle 2 size: " << ttext2.size() << " bytes" << std::endl;
    std::cout << "  Track Manifest Byte-Exact Match: " << (track_byte_exact ? "PASS (EXACT)" : "FAIL (DIFF)") << std::endl;

    if (!track_byte_exact) {
        std::cout << "\n  [DISCREPANCY DETECTED] Investigating lost fields in Track Manifest:" << std::endl;
        nlohmann::json tj1 = nlohmann::json::parse(ttext1);
        nlohmann::json tj2 = nlohmann::json::parse(ttext2);

        std::cout << "    visual.scene_model: Cycle 1 = " << tj1["visual"]["scene_model"]
                  << " vs Cycle 2 = " << tj2["visual"]["scene_model"]
                  << (tj1["visual"]["scene_model"] == tj2["visual"]["scene_model"] ? " (MATCH)" : " (LOST -> RESET TO DEFAULT!)") << std::endl;
        std::cout << "    collision.barrier_mesh: Cycle 1 = " << tj1["collision"]["barrier_mesh"]
                  << " vs Cycle 2 = " << tj2["collision"]["barrier_mesh"]
                  << (tj1["collision"]["barrier_mesh"] == tj2["collision"]["barrier_mesh"] ? " (MATCH)" : " (LOST -> RESET TO DEFAULT!)") << std::endl;
        std::cout << "    ai_navigation.primary_spline: Cycle 1 = " << tj1["ai_navigation"]["primary_spline"]
                  << " vs Cycle 2 = " << tj2["ai_navigation"]["primary_spline"]
                  << (tj1["ai_navigation"]["primary_spline"] == tj2["ai_navigation"]["primary_spline"] ? " (MATCH)" : " (LOST -> RESET TO DEFAULT!)") << std::endl;
        std::cout << "    supported_events: Cycle 1 = " << tj1["supported_events"].dump()
                  << " vs Cycle 2 = " << tj2["supported_events"].dump()
                  << (tj1["supported_events"] == tj2["supported_events"] ? " (MATCH)" : " (LOST -> RESET TO DEFAULT!)") << std::endl;
    }

    fs::remove_all(temp_base, ec);
}

// -----------------------------------------------------------------------------
// Main Test Runner
// -----------------------------------------------------------------------------

int main() {
    std::cout << "========================================================" << std::endl;
    std::cout << "EOD MODDING EDITOR — EMPIRICAL CHALLENGER STRESS SUITE" << std::endl;
    std::cout << "Target: Milestone 1 Viewport, Math Parity & Roundtrip" << std::endl;
    std::cout << "========================================================" << std::endl;

    RunCameraMathStressTest();
    RunProjectSerializationStressTest();

    return 0;
}
