// tools/tests/showroom_camera_test.cpp - Unit tests for 3D Showroom, Yard Scene Actor Table & Modern Camera Orbit Systems
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/cpu.h"
#include "core/memory.h"
#include "core/log.h"
#include "hle_showroom.h"
#include "career_upgrades.h"
#include "platform/platform.h"

// -----------------------------------------------------------------------------
// Emulated EE Environment & Stubs
// -----------------------------------------------------------------------------

EEState g_ee{};
uint8_t *g_ram = nullptr;

extern "C" uint32_t ee_read32_slow(uint32_t) { std::abort(); }
extern "C" void ee_write32_slow(uint32_t, uint32_t) { std::abort(); }
extern "C" uint8_t ee_read8_slow(uint32_t a) { return a < EE_RAM_SIZE ? g_ram[a] : 0; }
extern "C" void ee_write8_slow(uint32_t a, uint8_t v) { if (a < EE_RAM_SIZE) g_ram[a] = v; }
extern "C" void *ee_ptr(uint32_t a) { return a < EE_RAM_SIZE ? (g_ram + a) : nullptr; }
extern "C" void ee_call(uint32_t) {}

bool g_log_enabled[LOG_CAT_COUNT] = {};
void log_write(LogCat, const char *, ...) {}
std::string g_game_root;

void platform_show_hud_banner(const char *, int) {}
bool platform_find_file(const char *, const char *, char *, size_t) { return false; }

// Native function orig stubs
extern "C" void f_001C31A0_orig(void) {}
extern "C" void f_001D2DC8_orig(void) {}

static HostPadState s_mock_pad{};
void platform_get_pad(int, HostPadState *out) {
    if (out) *out = s_mock_pad;
}

// -----------------------------------------------------------------------------
// Test Helpers & Assertions
// -----------------------------------------------------------------------------

static int s_tests_passed = 0;
static int s_tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "[FAIL] Line %d: %s (%s)\n", __LINE__, msg, #cond); \
        s_tests_failed++; \
        return; \
    } \
} while (0)

#define RUN_TEST(fn) do { \
    std::printf("[RUN ] %s\n", #fn); \
    int before_fail = s_tests_failed; \
    fn(); \
    if (s_tests_failed == before_fail) { \
        std::printf("[PASS] %s\n", #fn); \
        s_tests_passed++; \
    } \
} while (0)

static bool approx_equal(float a, float b, float eps = 1e-3f) {
    return std::abs(a - b) <= eps;
}

static bool vec3_approx(const Vector3f& a, const Vector3f& b, float eps = 1e-3f) {
    return approx_equal(a.x, b.x, eps) && approx_equal(a.y, b.y, eps) && approx_equal(a.z, b.z, eps);
}

// -----------------------------------------------------------------------------
// Test 1: Struct Layouts, Sizes, and Byte Offsets
// -----------------------------------------------------------------------------
static void test_struct_layouts() {
    TEST_ASSERT(sizeof(YardActorEntry) == 0x12C, "YardActorEntry must be exactly 0x12C (300) bytes");
    TEST_ASSERT(sizeof(CarMgrTable) == 0x1818, "CarMgrTable must be exactly 0x1818 (6168) bytes");
    TEST_ASSERT(sizeof(CameraTrackEntry) == 0x48, "CameraTrackEntry must be exactly 0x48 (72) bytes");
    TEST_ASSERT(sizeof(Matrix4x4f) == 0x40, "Matrix4x4f must be 0x40 (64) bytes");
    TEST_ASSERT(sizeof(Matrix3x4f) == 0x30, "Matrix3x4f must be 0x30 (48) bytes");
    TEST_ASSERT(sizeof(Vector3f) == 0x0C, "Vector3f must be 0x0C (12) bytes");

    // Exact offsets in YardActorEntry matching decompiled MIPS
    TEST_ASSERT(offsetof(YardActorEntry, active) == 0x00, "YardActorEntry::active at +0x00");
    TEST_ASSERT(offsetof(YardActorEntry, slot_id) == 0x04, "YardActorEntry::slot_id at +0x04");
    TEST_ASSERT(offsetof(YardActorEntry, flags) == 0x08, "YardActorEntry::flags at +0x08");
    TEST_ASSERT(offsetof(YardActorEntry, wheel_transforms) == 0x0C, "YardActorEntry::wheel_transforms at +0x0C");
    TEST_ASSERT(offsetof(YardActorEntry, world_transform) == 0xB4, "YardActorEntry::world_transform at +0xB4");
    TEST_ASSERT(offsetof(YardActorEntry, position) == 0xF4, "YardActorEntry::position at +0xF4");
    TEST_ASSERT(offsetof(YardActorEntry, bounds_radius) == 0x100, "YardActorEntry::bounds_radius at +0x100");
    TEST_ASSERT(offsetof(YardActorEntry, visual_car_ptr) == 0x114, "YardActorEntry::visual_car_ptr at +0x114");
    TEST_ASSERT(offsetof(YardActorEntry, lod_distance) == 0x118, "YardActorEntry::lod_distance at +0x118");
    TEST_ASSERT(offsetof(YardActorEntry, paint_job_index) == 0x11C, "YardActorEntry::paint_job_index at +0x11C");
    TEST_ASSERT(offsetof(YardActorEntry, vehicle_id) == 0x120, "YardActorEntry::vehicle_id at +0x120");
    TEST_ASSERT(offsetof(YardActorEntry, chassis_id) == 0x124, "YardActorEntry::chassis_id at +0x124");
    TEST_ASSERT(offsetof(YardActorEntry, state) == 0x128, "YardActorEntry::state at +0x128");

    // Offsets in CarMgrTable
    TEST_ASSERT(offsetof(CarMgrTable, actors) == 0x00, "CarMgrTable::actors at +0x00");
    TEST_ASSERT(offsetof(CarMgrTable, active_count) == 0x1770, "CarMgrTable::active_count at +0x1770");
    TEST_ASSERT(offsetof(CarMgrTable, total_yard_count) == 0x1774, "CarMgrTable::total_yard_count at +0x1774");
    TEST_ASSERT(offsetof(CarMgrTable, slot_map) == 0x1778, "CarMgrTable::slot_map at +0x1778");
    TEST_ASSERT(offsetof(CarMgrTable, slot_order) == 0x17C8, "CarMgrTable::slot_order at +0x17C8");

    // Offsets in RenderCameraContext
    TEST_ASSERT(offsetof(RenderCameraContext, view_matrix) == 0x00, "RenderCameraContext::view_matrix at +0x00");
    TEST_ASSERT(offsetof(RenderCameraContext, fov) == 0x30, "RenderCameraContext::fov at +0x30");
    TEST_ASSERT(offsetof(RenderCameraContext, mode) == 0x34, "RenderCameraContext::mode at +0x34");
    TEST_ASSERT(offsetof(RenderCameraContext, participant_slot) == 0x38, "RenderCameraContext::participant_slot at +0x38");
    TEST_ASSERT(offsetof(RenderCameraContext, viewport_id) == 0x3C, "RenderCameraContext::viewport_id at +0x3C");
    TEST_ASSERT(offsetof(RenderCameraContext, flags) == 0x40, "RenderCameraContext::flags at +0x40");
    TEST_ASSERT(offsetof(RenderCameraContext, vehicle_ptr) == 0x44, "RenderCameraContext::vehicle_ptr at +0x44");
    TEST_ASSERT(offsetof(RenderCameraContext, target_pos) == 0x54, "RenderCameraContext::target_pos at +0x54");
}

// -----------------------------------------------------------------------------
// Test 2: Spherical Orbit Camera Math & View Matrix Orthonormality
// -----------------------------------------------------------------------------
static void test_orbit_camera_math() {
    auto& state = showroom_get_orbit_state();
    showroom_reset_orbit();

    state.center = {0.0f, 0.65f, 0.0f};
    state.yaw = 0.0f;
    state.pitch = 0.0f;
    state.distance = 5.0f;

    // Allocate mock RenderCameraContext in RAM
    const uint32_t kCtxAddr = 0x00100000;
    auto* ctx = reinterpret_cast<RenderCameraContext*>(ee_ptr(kCtxAddr));
    std::memset(ctx, 0, sizeof(RenderCameraContext));
    ctx->mode = 10; // Showroom inspection mode
    ctx->target_pos = state.center;

    bool injected = showroom_inject_camera_orbit(kCtxAddr, 0);
    TEST_ASSERT(injected, "showroom_inject_camera_orbit must succeed for mode 10");

    // Expected eye for yaw=0, pitch=0, dist=5: (0, 0.65, 5)
    auto* cam_pos = reinterpret_cast<Vector3f*>(ee_ptr(0x004CD31C));
    TEST_ASSERT(approx_equal(cam_pos->x, 0.0f), "Eye X should be 0.0");
    TEST_ASSERT(approx_equal(cam_pos->y, 0.65f), "Eye Y should be 0.65");
    TEST_ASSERT(approx_equal(cam_pos->z, 5.0f), "Eye Z should be 5.0");

    // Check View Matrix rows
    Vector3f r = {ctx->view_matrix.m[0][0], ctx->view_matrix.m[0][1], ctx->view_matrix.m[0][2]};
    Vector3f u = {ctx->view_matrix.m[1][0], ctx->view_matrix.m[1][1], ctx->view_matrix.m[1][2]};
    Vector3f f = {ctx->view_matrix.m[2][0], ctx->view_matrix.m[2][1], ctx->view_matrix.m[2][2]};

    // Right, Up, and Forward must be unit length
    TEST_ASSERT(approx_equal(r.length(), 1.0f), "Right vector must be unit length");
    TEST_ASSERT(approx_equal(u.length(), 1.0f), "Up vector must be unit length");
    TEST_ASSERT(approx_equal(f.length(), 1.0f), "Forward vector must be unit length");

    // Orthonormality (dot products = 0)
    TEST_ASSERT(approx_equal(dot(r, u), 0.0f), "Right and Up must be orthogonal");
    TEST_ASSERT(approx_equal(dot(u, f), 0.0f), "Up and Forward must be orthogonal");
    TEST_ASSERT(approx_equal(dot(r, f), 0.0f), "Right and Forward must be orthogonal");

    // Translation components must equal -dot(axis, eye)
    Vector3f eye = *cam_pos;
    TEST_ASSERT(approx_equal(ctx->view_matrix.m[0][3], -dot(r, eye)), "Translation X must match -dot(r, eye)");
    TEST_ASSERT(approx_equal(ctx->view_matrix.m[1][3], -dot(u, eye)), "Translation Y must match -dot(u, eye)");
    TEST_ASSERT(approx_equal(ctx->view_matrix.m[2][3], -dot(f, eye)), "Translation Z must match -dot(f, eye)");
}

// -----------------------------------------------------------------------------
// Test 3: Analog Stick, Mouse Drag, and Zoom Clamping
// -----------------------------------------------------------------------------
static void test_input_and_clamping() {
    auto& state = showroom_get_orbit_state();
    showroom_reset_orbit();

    // 1. Right stick horizontal deflection (rx = 255)
    s_mock_pad = {};
    s_mock_pad.connected = true;
    s_mock_pad.rx = 255;
    s_mock_pad.ry = 128;

    float initial_yaw = state.yaw;
    showroom_update_camera(0.1f);
    TEST_ASSERT(state.target_yaw > initial_yaw, "Right stick positive X must increase yaw");
    TEST_ASSERT(state.user_controlling, "User controlling flag should be true after stick input");
    TEST_ASSERT(state.idle_timer == 0.0f, "Idle timer should be reset on user input");

    // 2. Mouse drag
    showroom_on_mouse_drag(50.0f, -30.0f);
    TEST_ASSERT(state.target_yaw > initial_yaw, "Mouse drag X must advance yaw");
    TEST_ASSERT(state.target_pitch > state.default_pitch, "Mouse drag negative Y must pitch up");

    // 3. Extreme pitch clamping (min_pitch to max_pitch)
    showroom_on_mouse_drag(0.0f, -50000.0f); // High drag up
    TEST_ASSERT(approx_equal(state.target_pitch, state.max_pitch), "Pitch must clamp to max_pitch (1.15 rad)");

    showroom_on_mouse_drag(0.0f, 100000.0f); // High drag down
    TEST_ASSERT(approx_equal(state.target_pitch, state.min_pitch), "Pitch must clamp to min_pitch (-0.15 rad)");

    // 4. Zoom clamping via mouse wheel
    showroom_on_mouse_wheel(100.0f); // Massive zoom in
    TEST_ASSERT(approx_equal(state.target_distance, state.min_distance), "Distance must clamp to min_distance (2.8m)");

    showroom_on_mouse_wheel(-200.0f); // Massive zoom out
    TEST_ASSERT(approx_equal(state.target_distance, state.max_distance), "Distance must clamp to max_distance (9.5m)");
}

// -----------------------------------------------------------------------------
// Test 4: Idle Timer & Auto-Recentering
// -----------------------------------------------------------------------------
static void test_auto_recentering() {
    auto& state = showroom_get_orbit_state();
    showroom_reset_orbit();

    // Deflect camera
    showroom_on_mouse_drag(100.0f, 50.0f);
    showroom_on_mouse_wheel(-5.0f);
    TEST_ASSERT(state.user_controlling, "User controlling must be active");

    // Clear mock inputs
    s_mock_pad = {};
    s_mock_pad.connected = true;
    s_mock_pad.rx = 128;
    s_mock_pad.ry = 128;

    // Advance time before timeout (recenter_timeout = 3.5s)
    showroom_update_camera(2.0f);
    TEST_ASSERT(state.idle_timer >= 2.0f, "Idle timer should accumulate");
    TEST_ASSERT(state.user_controlling, "Auto-recentering should not trigger before timeout");

    // Advance time past timeout
    for (int i = 0; i < 40; ++i) {
        showroom_update_camera(0.2f);
    }

    // Should have interpolated back near default pose
    TEST_ASSERT(approx_equal(state.yaw, state.default_yaw, 0.05f), "Yaw must return near default pose");
    TEST_ASSERT(approx_equal(state.pitch, state.default_pitch, 0.05f), "Pitch must return near default pose");
    TEST_ASSERT(approx_equal(state.distance, state.default_distance, 0.1f), "Distance must return near default distance");
    TEST_ASSERT(!state.user_controlling, "User controlling must be cleared after recentering");
}

// -----------------------------------------------------------------------------
// Test 5: Dynamic Yard Actor Spawner & HomeMaster Integration
// -----------------------------------------------------------------------------
static void test_yard_actor_spawner() {
    // Setup CarMgrTable at 0x00495D80
    const uint32_t kCarMgrAddr = 0x00200000;
    ee_w32(0x00495D80, kCarMgrAddr);

    auto* mgr = reinterpret_cast<CarMgrTable*>(ee_ptr(kCarMgrAddr));
    std::memset(mgr, 0, sizeof(CarMgrTable));

    // Setup Homebase Master at 0x003CDA04
    const uint32_t kHomeAddr = 0x00210000;
    ee_w32(0x003CDA04, kHomeAddr);
    std::memset(ee_ptr(kHomeAddr), 0, 0xC04);

    // Spawn Vehicle ID 27 (NASCAR Cup car) into Yard Slot 2 with paint 3 and 90 deg rotation
    const float rot_90 = 1.5707963f;
    bool spawned = showroom_spawn_yard_car(2, 27, 3, rot_90);
    TEST_ASSERT(spawned, "showroom_spawn_yard_car must succeed");

    auto& actor = mgr->actors[2];
    TEST_ASSERT(actor.active == 1, "Actor slot 2 active flag must be 1");
    TEST_ASSERT(actor.slot_id == 2, "Actor slot_id must be 2");
    TEST_ASSERT(actor.vehicle_id == 27, "Actor vehicle_id must be 27");
    TEST_ASSERT(actor.paint_job_index == 3, "Actor paint_job_index must be 3");
    TEST_ASSERT(actor.flags == 0x52, "Actor flags must be 0x52");
    TEST_ASSERT(mgr->active_count >= 3, "mgr->active_count must be updated");

    // Check 4x4 matrix rotation around Y:
    // cos(90) ~ 0, sin(90) ~ 1
    TEST_ASSERT(approx_equal(actor.world_transform.m[0][0], 0.0f, 1e-3f), "m[0][0] ~ cos(90)");
    TEST_ASSERT(approx_equal(actor.world_transform.m[0][2], 1.0f, 1e-3f), "m[0][2] ~ sin(90)");
    TEST_ASSERT(approx_equal(actor.world_transform.m[2][0], -1.0f, 1e-3f), "m[2][0] ~ -sin(90)");
    TEST_ASSERT(approx_equal(actor.world_transform.m[2][2], 0.0f, 1e-3f), "m[2][2] ~ cos(90)");

    // Check HomeMaster dealer yard bay vehicle ID sync at home + 0x4BC + 4 * slot_id
    uint32_t home_yard_id = ee_r32(kHomeAddr + 0x4BC + 4 * 2);
    TEST_ASSERT(home_yard_id == 27, "HomeMaster bay 2 must reflect vehicle ID 27");
}

// -----------------------------------------------------------------------------
// Test 6: 360 Turntable Continuous Rotation
// -----------------------------------------------------------------------------
static void test_turntable_rotation() {
    auto& state = showroom_get_orbit_state();
    state.turntable_enabled = true;
    state.turntable_angle = 0.0f;
    state.turntable_speed = 0.35f; // ~0.35 rad/s

    const uint32_t kCarMgrAddr = 0x00200000;
    auto* mgr = reinterpret_cast<CarMgrTable*>(ee_ptr(kCarMgrAddr));
    mgr->actors[0].active = 1;

    // Advance 1.0 second
    showroom_update_turntable(1.0f);
    TEST_ASSERT(approx_equal(state.turntable_angle, 0.35f, 1e-3f), "Turntable angle should advance by speed*dt");

    float c = std::cos(0.35f);
    float s = std::sin(0.35f);
    TEST_ASSERT(approx_equal(mgr->actors[0].world_transform.m[0][0], c, 1e-3f), "Actor 0 m[0][0] must match cos(angle)");
    TEST_ASSERT(approx_equal(mgr->actors[0].world_transform.m[0][2], s, 1e-3f), "Actor 0 m[0][2] must match sin(angle)");

    // Advance past 2*pi and test wrap
    state.turntable_angle = 6.20f;
    showroom_update_turntable(1.0f);
    TEST_ASSERT(state.turntable_angle < 6.283185f, "Turntable angle must wrap around 2*pi");
}

// -----------------------------------------------------------------------------
// Test 7: CDBupgr Owned Garage Lot Population
// -----------------------------------------------------------------------------
static void test_garage_lot_population() {
    const uint32_t kCDBupgrAddr = 0x00300000;
    ee_w32(0x003D9398, kCDBupgrAddr);

    auto* cdb = reinterpret_cast<CDBupgr*>(ee_ptr(kCDBupgrAddr));
    std::memset(cdb, 0, sizeof(CDBupgr));
    cdb->version = 3;
    cdb->car_count = 3; // 3 cars owned in garage

    // Slot 0: Hornet starter (ID 65)
    cdb->cars[0].vehicle_id = 65;
    cdb->cars[0].flags = 1; // Owned

    // Slot 1: Outlaw (ID 12)
    cdb->cars[1].vehicle_id = 12;
    cdb->cars[1].flags = 1; // Owned

    // Slot 2: Phantom (ID 20)
    cdb->cars[2].vehicle_id = 20;
    cdb->cars[2].flags = 1; // Owned

    // Slot 3: Unowned
    cdb->cars[3].vehicle_id = 0;
    cdb->cars[3].flags = 0;

    bool populated = showroom_populate_garage_lot();
    TEST_ASSERT(populated, "showroom_populate_garage_lot must succeed");

    const uint32_t kCarMgrAddr = ee_r32(0x00495D80);
    auto* mgr = reinterpret_cast<CarMgrTable*>(ee_ptr(kCarMgrAddr));

    TEST_ASSERT(mgr->actors[0].vehicle_id == 65, "Lot bay 0 vehicle ID must match garage car 0 (65)");
    TEST_ASSERT(mgr->actors[1].vehicle_id == 12, "Lot bay 1 vehicle ID must match garage car 1 (12)");
    TEST_ASSERT(mgr->actors[2].vehicle_id == 20, "Lot bay 2 vehicle ID must match garage car 2 (20)");
}

// -----------------------------------------------------------------------------
// Test 8: Gameplay Camera Pass-Through (Zero Side Effects)
// -----------------------------------------------------------------------------
static void test_gameplay_camera_passthrough() {
    auto& state = showroom_get_orbit_state();
    state.enabled = true;

    const uint32_t kCtxAddr = 0x00100000;
    auto* ctx = reinterpret_cast<RenderCameraContext*>(ee_ptr(kCtxAddr));
    std::memset(ctx, 0, sizeof(RenderCameraContext));

    // Gameplay camera modes 0..7 (e.g. chase camera mode 0)
    ctx->mode = 0;
    ctx->view_matrix.m[0][0] = 999.0f; // Canary value

    bool injected = showroom_inject_camera_orbit(kCtxAddr, 0);
    TEST_ASSERT(!injected, "showroom_inject_camera_orbit must return false for gameplay camera");
    TEST_ASSERT(!state.active, "state.active must be false during gameplay");
    TEST_ASSERT(ctx->view_matrix.m[0][0] == 999.0f, "Gameplay camera view matrix must be left completely untouched");
}

// -----------------------------------------------------------------------------
// Main Entrypoint
// -----------------------------------------------------------------------------
int main() {
    std::printf("=== Eve of Destruction: 3D Showroom & Orbit Camera Test Suite ===\n");

    // Allocate 32 MB emulated EE RAM
    g_ram = static_cast<uint8_t*>(std::calloc(1, EE_RAM_SIZE));
    assert(g_ram != nullptr);

    RUN_TEST(test_struct_layouts);
    RUN_TEST(test_orbit_camera_math);
    RUN_TEST(test_input_and_clamping);
    RUN_TEST(test_auto_recentering);
    RUN_TEST(test_yard_actor_spawner);
    RUN_TEST(test_turntable_rotation);
    RUN_TEST(test_garage_lot_population);
    RUN_TEST(test_gameplay_camera_passthrough);

    std::printf("\n=== Summary: %d passed, %d failed ===\n", s_tests_passed, s_tests_failed);

    std::free(g_ram);
    return s_tests_failed == 0 ? 0 : 1;
}
