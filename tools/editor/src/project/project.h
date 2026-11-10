#pragma once

#include <string>
#include <vector>
#include <memory>
#include "../../../src/hle_showroom.h"
#include "../../third_party/nlohmann/json.hpp"

namespace eod::editor {

enum class ProjectType {
    Vehicle,
    Track,
    Event,
    Combined
};

struct SocketDefinition {
    std::string name;
    std::string type; // "wheel_socket", "panel_hinge", "trailer_hitch", "weapon_hardpoint", "interior_anchor", "camera_socket", "exhaust_socket"
    Vector3f position{0.0f, 0.0f, 0.0f};
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // Quaternion [X, Y, Z, W]
    int part_group = -1;
    float detachment_threshold_meters = 0.0762f;
    bool steerable = false;
    bool driven = false;
    float suspension_travel_meters = 0.20f;
};

struct ComponentGroupInfo {
    int id;
    std::string semantic_name;
    bool is_paintable;
    bool is_detachable;
    std::string anchor_socket_name;
    float default_detachment_threshold;
    bool is_assigned = false;
    std::string gltf_node_name;
};

struct VehiclePhysicsData {
    // Chassis
    float mass_kg = 1360.77f;
    float wheelbase_meters = 2.74f;
    float track_front_meters = 1.57f;
    float track_rear_meters = 1.57f;
    Vector3f center_of_mass{0.0f, 0.45f, 0.12f};
    Vector3f moment_of_inertia{2800.0f, 3100.0f, 750.0f};
    float aerodynamic_drag = 0.32f;
    float frontal_area_m2 = 1.95f;
    float chassis_health_max = 1.0f;
    float damage_scale = 0.65f;

    // Engine
    float displacement_liters = 5.7f;
    float peak_power_bhp = 385.0f;
    float peak_power_rpm = 5800.0f;
    float peak_torque_nm = 520.0f;
    float peak_torque_rpm = 3600.0f;
    float idle_rpm = 850.0f;
    float redline_rpm = 6500.0f;
    std::string soundbank = "audio/v8_heavy.json";

    // Tires
    std::string tire_model = "pacejka_magic_formula";
    float nominal_friction = 0.92f;
    float removal_impulse_threshold_ns = 45000.0f;
};

struct TrackVenueData {
    std::string track_type = "circuit";
    std::string surface_type = "dirt_clay";
    float length_meters = 1250.0f;
    Vector3f bounds_min{-500.0f, -10.0f, -500.0f};
    Vector3f bounds_max{500.0f, 45.0f, 500.0f};
    std::string scene_model = "geometry/track_scene.gltf";
    std::string skybox = "textures/skybox_sunset.hdr";
    std::string terrain_mesh = "geometry/terrain_collision.gltf";
    std::string barrier_mesh = "geometry/barrier_collision.gltf";
    std::string primary_spline = "navigation/centerline.spline.json";
    std::vector<std::string> supported_events = {"Race", "Figure8", "Derby", "TrailerRace", "Battle", "ChickenBattle"};
};

class EditorProject {
public:
    EditorProject();
    ~EditorProject() = default;

    void CreateNew(ProjectType type, const std::string& name, const std::string& identifier);
    bool LoadFromDirectory(const std::string& dir_path, std::string& out_error);
    bool SaveToDirectory(const std::string& dir_path, std::string& out_error);

    ProjectType GetType() const { return type_; }
    void SetType(ProjectType type) { type_ = type; }

    const std::string& GetName() const { return display_name_; }
    void SetName(const std::string& name) { display_name_ = name; }

    const std::string& GetIdentifier() const { return identifier_; }
    void SetIdentifier(const std::string& id) { identifier_ = id; }

    const std::string& GetProjectPath() const { return project_dir_; }

    // Vehicle data accessors
    const std::string& GetVehicleClass() const { return vehicle_class_; }
    void SetVehicleClass(const std::string& vc) { vehicle_class_ = vc; }
    VehiclePhysicsData& GetPhysics() { return physics_; }
    const VehiclePhysicsData& GetPhysics() const { return physics_; }

    std::vector<SocketDefinition>& GetSockets() { return sockets_; }
    const std::vector<SocketDefinition>& GetSockets() const { return sockets_; }
    void AddSocket(const SocketDefinition& socket);
    bool RemoveSocket(const std::string& socket_name);

    std::vector<ComponentGroupInfo>& GetComponentGroups() { return component_groups_; }
    const std::vector<ComponentGroupInfo>& GetComponentGroups() const { return component_groups_; }

    // Track data accessors
    TrackVenueData& GetTrackData() { return track_data_; }
    const TrackVenueData& GetTrackData() const { return track_data_; }

    // Serialization to conforming JSON
    nlohmann::json SerializeVehicleManifest() const;
    nlohmann::json SerializeTrackManifest() const;
    nlohmann::json SerializeModPackageManifest() const;

    bool DeserializeVehicleManifest(const nlohmann::json& j, std::string& out_error);
    bool DeserializeTrackManifest(const nlohmann::json& j, std::string& out_error);

private:
    void InitializeDefaultComponentGroups();
    void InitializeDefaultSockets();

    ProjectType type_ = ProjectType::Vehicle;
    std::string display_name_ = "Untitled Project";
    std::string identifier_ = "mod.custom.project";
    std::string project_dir_;

    // Vehicle fields
    std::string vehicle_class_ = "midsize";
    std::string manufacturer_ = "Custom Works";
    int year_ = 1974;
    std::string source_model_path_ = "models/vehicle.gltf";
    VehiclePhysicsData physics_;
    std::vector<SocketDefinition> sockets_;
    std::vector<ComponentGroupInfo> component_groups_;

    // Track fields
    TrackVenueData track_data_;
};

} // namespace eod::editor
