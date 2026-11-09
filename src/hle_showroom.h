// hle_showroom.h - 3D Showroom, Yard Scene Actor Table & Camera Systems Modernization
// Decompiled reverse-engineering structures and modern 360-degree turntable orbit controls.
#pragma once
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>

// -----------------------------------------------------------------------------
// 3D Math Fundamentals
// -----------------------------------------------------------------------------

struct Vector3f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vector3f() = default;
    constexpr Vector3f(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vector3f operator+(const Vector3f& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vector3f operator-(const Vector3f& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vector3f operator*(float s) const { return {x * s, y * s, z * s}; }
    float length_sq() const { return x * x + y * y + z * z; }
    float length() const { return std::sqrt(length_sq()); }
    Vector3f normalized() const {
        float l = length();
        return l > 1e-6f ? (*this * (1.0f / l)) : Vector3f{0.0f, 0.0f, 0.0f};
    }
};

inline float dot(const Vector3f& a, const Vector3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vector3f cross(const Vector3f& a, const Vector3f& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

// 4x4 Row-Major Transform Matrix (0x40 bytes)
struct Matrix4x4f {
    float m[4][4] = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f}
    };
};

// 3x4 Camera View Matrix (0x30 bytes)
struct Matrix3x4f {
    float m[3][4] = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f}
    };
};

// -----------------------------------------------------------------------------
// Yard & Car Scene Actor Table (0x00495D80)
// -----------------------------------------------------------------------------

#pragma pack(push, 1)

// Individual 3D Actor in the AutoShop yard (stride 0x12C = 300 bytes)
struct YardActorEntry {
    int32_t active;               // +0x000: 1 = spawned & visible, 0 = inactive / hidden
    int32_t slot_id;              // +0x004: yard lot bay index (0..19)
    int32_t flags;                // +0x008: draw / interaction flags (0x50, 0x52, etc.)
    float wheel_transforms[40];   // +0x00C..+0x0AB: wheel/suspension node transforms (160 B)
    uint8_t transform_pad[8];     // +0x0AC..+0x0B3: alignment padding
    Matrix4x4f world_transform;   // +0x0B4..+0x0F3: 4x4 actor transformation matrix (64 B)
    Vector3f position;            // +0x0F4..+0x0FF: world origin position (12 B)
    float bounds_radius;          // +0x100..+0x103: collision / visibility sphere radius (4 B)
    uint8_t extra_geom[16];       // +0x104..+0x113: deformation / attach metadata (16 B)
    uint32_t visual_car_ptr;      // +0x114: pointer to VisualCar / CarObject instance (0x654 B)
    float lod_distance;           // +0x118: camera distance threshold for LOD selection
    int32_t paint_job_index;      // +0x11C: livery / paint job variation index
    int32_t vehicle_id;           // +0x120: vehicle identifier (e.g. 0..131)
    int32_t chassis_id;           // +0x124: base chassis ID (0..27)
    int32_t state;                // +0x128: actor animation/physics state
};
static_assert(sizeof(YardActorEntry) == 0x12C, "YardActorEntry must be exactly 0x12C (300) bytes");

// CarMgr Global Table (size 0x1818 = 6168 bytes, allocated in CarMgrInit f_002134A0)
struct CarMgrTable {
    YardActorEntry actors[20];    // +0x0000..+0x176F: 20 actor entries (20 * 0x12C = 0x1770 B)
    int32_t active_count;         // +0x1770: number of active scene cars
    int32_t total_yard_count;     // +0x1774: total registered yard slots (up to 16/20)
    int32_t slot_map[20];         // +0x1778..+0x17C7: slot-to-actor mapping table (80 B)
    int32_t slot_order[20];       // +0x17C8..+0x1817: display/priority order table (80 B)
};
static_assert(sizeof(CarMgrTable) == 0x1818, "CarMgrTable must be exactly 0x1818 (6168) bytes");

// Camera Track Entry in 0x0048BD90 table (stride 0x48 = 72 bytes)
struct CameraTrackEntry {
    char name[8];                 // +0x00: track identifier ("buy1".."buy5", "garage1".."garage4", etc.)
    Vector3f target;              // +0x08: LookAt target world coordinate (X, Y, Z)
    Vector3f up;                  // +0x14: camera up vector
    float angles[3];              // +0x20: pitch, yaw, roll angles
    Vector3f eye;                 // +0x2C: camera eye position (X, Y, Z)
    float fov;                    // +0x38: field of view in radians (e.g. 1.48 rad)
    float transition_time;        // +0x3C: blend/duration time (seconds)
    uint32_t flags;               // +0x40: track flags
    uint32_t reserved;            // +0x44: padding
};
static_assert(sizeof(CameraTrackEntry) == 0x48, "CameraTrackEntry must be exactly 0x48 (72) bytes");

// Camera Director Active Pose at 0x0048CBD4
struct CameraDirectorTrackPose {
    Vector3f target;              // +0x00: current blended target position (X, Y, Z)
    Vector3f up;                  // +0x0C: current blended up vector (X, Y, Z)
    float angles[3];              // +0x18: current blended orientation angles
    Vector3f eye;                 // +0x24: current blended camera eye position (X, Y, Z)
    float fov;                    // +0x30: current blended FOV (radians)
    float blend_progress;         // +0x34: transition time parameter
};

// Render Camera Context (passed as a0 to f_001C31A0, size 0xE4)
struct RenderCameraContext {
    Matrix3x4f view_matrix;       // +0x00..+0x2F: 3x4 view transform matrix
    float fov;                    // +0x30: horizontal/vertical FOV (radians)
    int32_t mode;                 // +0x34: camera mode (0..7 gameplay, 10..13 showroom/garage/menu)
    int32_t participant_slot;     // +0x38: focused car slot index
    int32_t viewport_id;          // +0x3C: viewport index (0 = single player)
    int16_t flags;                // +0x40: rendering flags
    int16_t pad42;                // +0x42
    uint32_t vehicle_ptr;         // +0x44: focused CarObject pointer
    int32_t flags2;               // +0x48: flags
    uint32_t pad4C[2];            // +0x4C
    Vector3f target_pos;          // +0x54: camera target position
};

#pragma pack(pop)

// -----------------------------------------------------------------------------
// Modern 360 Turntable & Orbit Camera Controller State
// -----------------------------------------------------------------------------

struct ShowroomOrbitState {
    bool enabled = true;          // master feature toggle
    bool active = false;          // true when camera is in showroom/garage mode
    bool user_controlling = false;// true when user is actively moving stick or mouse
    bool turntable_enabled = true;// true when automatic turntable rotation is on

    // Angles & distances (spherical coordinate system around pivot)
    float yaw = 0.0f;             // azimuth angle (radians)
    float pitch = 0.25f;          // elevation angle (radians, ~14 degrees)
    float distance = 5.2f;        // orbit radius (meters)

    // Desired targets for smooth exponential damping
    float target_yaw = 0.0f;
    float target_pitch = 0.25f;
    float target_distance = 5.2f;

    // Default pose captured from director track
    float default_yaw = 0.0f;
    float default_pitch = 0.25f;
    float default_distance = 5.2f;

    // Pivot center point
    Vector3f center = {0.0f, 0.65f, 0.0f};

    // Auto-recenter idle timer (seconds)
    float idle_timer = 0.0f;
    float recenter_timeout = 3.5f;

    // Turntable animation
    float turntable_speed = 0.35f;// radians per second (~3.3 RPM)
    float turntable_angle = 0.0f;

    // Input sensitivity tuning
    float stick_sensitivity_yaw = 2.4f;
    float stick_sensitivity_pitch = 1.6f;
    float mouse_sensitivity_yaw = 0.005f;
    float mouse_sensitivity_pitch = 0.0035f;
    float zoom_speed_triggers = 3.2f;
    float zoom_speed_wheel = 0.6f;

    // Constraints
    float min_pitch = -0.15f;     // ~ -8.5 degrees (ground grazing)
    float max_pitch = 1.15f;      // ~ +66.0 degrees (high overhead)
    float min_distance = 2.8f;    // close inspection
    float max_distance = 9.5f;    // wide yard overview
};

// -----------------------------------------------------------------------------
// Public Subsystem API
// -----------------------------------------------------------------------------

// Access global orbit camera state
ShowroomOrbitState& showroom_get_orbit_state();

// Frame update: evaluates pad/mouse inputs, recentering, turntable spin, and orbit pose
void showroom_update_camera(float dt);

// Injects modern orbit controls into RenderCameraContext if in showroom/garage mode
bool showroom_inject_camera_orbit(uint32_t camera_ctx_addr, uint32_t director_addr);

// Track playback notification (called from f_001D2DC8 wrap)
void showroom_on_play_track(const char* track_name, int blend_flag);

// Spawns/updates a dynamic 3D vehicle in the yard lot
bool showroom_spawn_yard_car(int slot_id, int vehicle_id, int paint_index, float rot_y = 0.0f);

// Updates the central showroom turntable rotation for the active car
void showroom_update_turntable(float dt);

// Populates the 4 yard bays from the player's personal owned cars in CDBupgr
bool showroom_populate_garage_lot();

// Mouse drag & scroll event injection from platform layer
void showroom_on_mouse_drag(float dx, float dy);
void showroom_on_mouse_wheel(float delta);

// Reset orbit to default track pose
void showroom_reset_orbit();
