#include "project.h"
#include <fstream>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

namespace eod::editor {

EditorProject::EditorProject() {
    InitializeDefaultComponentGroups();
    InitializeDefaultSockets();
}

void EditorProject::InitializeDefaultComponentGroups() {
    component_groups_.clear();
    component_groups_.reserve(32);

    struct GroupTemplate {
        int id;
        const char* name;
        bool paintable;
        bool detachable;
        const char* anchor;
        float threshold;
    };

    static const GroupTemplate templates[32] = {
        {0,  "Chassis Subframe / Unibody",   false, false, "Chassis Center of Mass", 0.0f},
        {1,  "Cockpit Interior / Rollcage",  false, false, "driver_seat",            0.0f},
        {2,  "Engine Hood",                  true,  true,  "hinge_hood",             0.1270f},
        {3,  "Right Front Fender",           true,  true,  "hinge_door_r",           0.0762f},
        {4,  "Left Front Fender",            true,  true,  "hinge_door_l",           0.0762f},
        {5,  "Right Passenger Door",         true,  true,  "hinge_door_r",           0.0762f},
        {6,  "Left Driver Door",             true,  true,  "hinge_door_l",           0.0762f},
        {7,  "Right Rear Quarter Panel",     true,  false, "Chassis Rear Right",     0.0f},
        {8,  "Left Rear Quarter Panel",      true,  false, "Chassis Rear Left",      0.0f},
        {9,  "Roof Panel",                   true,  false, "Chassis Roof",           0.0f},
        {10, "Engine Block & Radiator",      false, false, "engine_block",           0.0f},
        {11, "Trunk Decklid",                true,  true,  "hinge_decklid",          0.0381f},
        {12, "Front Bumper",                 true,  true,  "mount_bumper_f",         0.0762f},
        {13, "Rear Bumper",                  true,  true,  "mount_bumper_r",         0.0762f},
        {14, "Front Windshield Glass",       false, false, "A-Pillar Perimeter",     0.05f},
        {15, "Rear Window Glass",            false, false, "C-Pillar Perimeter",     0.05f},
        {16, "Left Side Window Glass",       false, false, "Door Frame Left",        0.04f},
        {17, "Right Side Window Glass",      false, false, "Door Frame Right",       0.04f},
        {18, "Headlight Assembly",           false, false, "Front Fascia",           0.06f},
        {19, "Taillight Assembly",           false, false, "Rear Fascia",            0.06f},
        {20, "Exhaust System",               false, true,  "exhaust_emitter_01",     0.15f},
        {21, "Auxiliary Prop 01",            false, true,  "aux_socket_01",          0.05f},
        {22, "Auxiliary Prop 02",            false, true,  "aux_socket_02",          0.05f},
        {23, "Auxiliary Prop 03",            false, true,  "aux_socket_03",          0.05f},
        {24, "Auxiliary Prop 04",            false, true,  "aux_socket_04",          0.05f},
        {25, "Auxiliary Prop 05",            false, true,  "aux_socket_05",          0.05f},
        {26, "Auxiliary Prop 06",            false, true,  "aux_socket_06",          0.05f},
        {27, "Auxiliary Prop 07",            false, true,  "aux_socket_07",          0.05f},
        {28, "Auxiliary Prop 08",            false, true,  "aux_socket_08",          0.05f},
        {29, "Auxiliary Prop 09",            false, true,  "aux_socket_09",          0.05f},
        {30, "Auxiliary Prop 10",            false, true,  "aux_socket_10",          0.05f},
        {31, "Auxiliary Prop 11",            false, true,  "aux_socket_11",          0.05f}
    };

    for (int i = 0; i < 32; ++i) {
        ComponentGroupInfo info;
        info.id = templates[i].id;
        info.semantic_name = templates[i].name;
        info.is_paintable = templates[i].paintable;
        info.is_detachable = templates[i].detachable;
        info.anchor_socket_name = templates[i].anchor;
        info.default_detachment_threshold = templates[i].threshold;
        info.is_assigned = false;
        component_groups_.push_back(info);
    }
}

void EditorProject::InitializeDefaultSockets() {
    sockets_.clear();
    sockets_.push_back({"wheel_fl", "wheel_socket", {-0.785f, 0.35f, 1.37f}, {0,0,0,1}, -1, 0.0f, true, false, 0.22f});
    sockets_.push_back({"wheel_fr", "wheel_socket", {0.785f, 0.35f, 1.37f}, {0,0,0,1}, -1, 0.0f, true, false, 0.22f});
    sockets_.push_back({"wheel_rl", "wheel_socket", {-0.785f, 0.35f, -1.37f}, {0,0,0,1}, -1, 0.0f, false, true, 0.20f});
    sockets_.push_back({"wheel_rr", "wheel_socket", {0.785f, 0.35f, -1.37f}, {0,0,0,1}, -1, 0.0f, false, true, 0.20f});

    sockets_.push_back({"hinge_hood", "panel_hinge", {0.0f, 0.85f, 1.15f}, {0,0,0,1}, 2, 0.1270f});
    sockets_.push_back({"hinge_door_l", "panel_hinge", {-0.85f, 0.65f, 0.20f}, {0,0,0,1}, 6, 0.0762f});
    sockets_.push_back({"hinge_door_r", "panel_hinge", {0.85f, 0.65f, 0.20f}, {0,0,0,1}, 5, 0.0762f});
    sockets_.push_back({"hinge_decklid", "panel_hinge", {0.0f, 0.92f, -1.85f}, {0,0,0,1}, 11, 0.0381f});
    sockets_.push_back({"mount_bumper_f", "panel_hinge", {0.0f, 0.42f, 2.25f}, {0,0,0,1}, 12, 0.0762f});
    sockets_.push_back({"mount_bumper_r", "panel_hinge", {0.0f, 0.45f, -2.30f}, {0,0,0,1}, 13, 0.0762f});

    sockets_.push_back({"hitch_trailer", "trailer_hitch", {0.0f, 0.38f, -2.45f}, {0,0,0,1}});
    sockets_.push_back({"weapon_roof", "weapon_hardpoint", {0.0f, 1.42f, 0.0f}, {0,0,0,1}});
    sockets_.push_back({"driver_seat", "interior_anchor", {-0.38f, 0.52f, -0.15f}, {0,0,0,1}});
}

void EditorProject::CreateNew(ProjectType type, const std::string& name, const std::string& identifier) {
    type_ = type;
    display_name_ = name;
    identifier_ = identifier;
    project_dir_.clear();
    InitializeDefaultComponentGroups();
    InitializeDefaultSockets();
}

void EditorProject::AddSocket(const SocketDefinition& socket) {
    for (auto& s : sockets_) {
        if (s.name == socket.name) {
            s = socket; // Replace existing
            return;
        }
    }
    sockets_.push_back(socket);
}

bool EditorProject::RemoveSocket(const std::string& socket_name) {
    auto it = std::remove_if(sockets_.begin(), sockets_.end(),
                             [&](const SocketDefinition& s) { return s.name == socket_name; });
    if (it != sockets_.end()) {
        sockets_.erase(it, sockets_.end());
        return true;
    }
    return false;
}

nlohmann::json EditorProject::SerializeVehicleManifest() const {
    nlohmann::json j;
    j["$schema"] = "https://eod-recomp.org/schemas/v1/vehicle.manifest.json";
    j["identifier"] = identifier_;
    j["display_name"] = display_name_;
    j["vehicle_class"] = vehicle_class_;
    j["manufacturer"] = manufacturer_;
    j["year"] = year_;

    j["career"] = {
        {"price_retail", 4200},
        {"resale_factor", 0.90},
        {"parts_tradein_factor", 0.75},
        {"unlock_rank", 49},
        {"dealer_stock_probability", 0.35},
        {"starting_condition", 0.85}
    };

    j["geometry"] = {
        {"source_model", source_model_path_},
        {"lod_chain", {
            {{"lod", 0}, {"max_distance_meters", 10.0}, {"cockpit_interior", true}},
            {{"lod", 1}, {"max_distance_meters", 25.0}, {"cockpit_interior", false}},
            {{"lod", 2}, {"max_distance_meters", 50.0}, {"cockpit_interior", false}},
            {{"lod", 3}, {"max_distance_meters", 100.0}, {"cockpit_interior", false}}
        }},
        {"shadow_proxy", "models/phantom_shadow.gltf"}
    };

    j["physics"] = {
        {"chassis", {
            {"mass_kg", physics_.mass_kg},
            {"wheelbase_meters", physics_.wheelbase_meters},
            {"track_front_meters", physics_.track_front_meters},
            {"track_rear_meters", physics_.track_rear_meters},
            {"center_of_mass", {physics_.center_of_mass.x, physics_.center_of_mass.y, physics_.center_of_mass.z}},
            {"moment_of_inertia", {physics_.moment_of_inertia.x, physics_.moment_of_inertia.y, physics_.moment_of_inertia.z}},
            {"aerodynamic_drag", physics_.aerodynamic_drag},
            {"frontal_area_m2", physics_.frontal_area_m2},
            {"chassis_health_max", physics_.chassis_health_max},
            {"damage_scale", physics_.damage_scale}
        }},
        {"engine", {
            {"displacement_liters", physics_.displacement_liters},
            {"peak_power_bhp", physics_.peak_power_bhp},
            {"peak_power_rpm", physics_.peak_power_rpm},
            {"peak_torque_nm", physics_.peak_torque_nm},
            {"peak_torque_rpm", physics_.peak_torque_rpm},
            {"idle_rpm", physics_.idle_rpm},
            {"redline_rpm", physics_.redline_rpm},
            {"soundbank", physics_.soundbank}
        }},
        {"tires", {
            {"tire_model", physics_.tire_model},
            {"nominal_friction", physics_.nominal_friction},
            {"removal_impulse_threshold_ns", physics_.removal_impulse_threshold_ns}
        }}
    };

    nlohmann::json sockets_arr = nlohmann::json::array();
    for (const auto& s : sockets_) {
        nlohmann::json sock;
        sock["name"] = s.name;
        sock["type"] = s.type;
        sock["position"] = {s.position.x, s.position.y, s.position.z};
        sock["rotation"] = {s.rotation[0], s.rotation[1], s.rotation[2], s.rotation[3]};
        if (s.part_group >= 0) {
            sock["part_group"] = s.part_group;
        }
        if (s.detachment_threshold_meters > 0.0f) {
            sock["detachment_threshold_meters"] = s.detachment_threshold_meters;
        }
        if (s.type == "wheel_socket") {
            sock["steerable"] = s.steerable;
            sock["driven"] = s.driven;
            sock["suspension_travel_meters"] = s.suspension_travel_meters;
        }
        sockets_arr.push_back(sock);
    }
    j["sockets"] = sockets_arr;

    return j;
}

nlohmann::json EditorProject::SerializeTrackManifest() const {
    nlohmann::json j;
    j["$schema"] = "https://eod-recomp.org/schemas/v1/track.manifest.json";
    j["identifier"] = identifier_;
    j["display_name"] = display_name_;
    j["track_type"] = track_data_.track_type;
    j["surface_type"] = track_data_.surface_type;
    j["length_meters"] = track_data_.length_meters;

    j["bounds"] = {
        {"min", {track_data_.bounds_min.x, track_data_.bounds_min.y, track_data_.bounds_min.z}},
        {"max", {track_data_.bounds_max.x, track_data_.bounds_max.y, track_data_.bounds_max.z}}
    };

    j["visual"] = {
        {"scene_model", track_data_.scene_model},
        {"skybox", track_data_.skybox},
        {"ambient_light", {0.25, 0.22, 0.20}},
        {"sun_direction", {-0.577, -0.577, -0.577}},
        {"sun_color", {1.0, 0.95, 0.85}}
    };

    j["collision"] = {
        {"terrain_mesh", track_data_.terrain_mesh},
        {"barrier_mesh", track_data_.barrier_mesh},
        {"static_mesh_shape", "jolt_mesh_shape"}
    };

    j["ai_navigation"] = {
        {"primary_spline", track_data_.primary_spline}
    };

    j["scenery_props"] = "scenery/placed_props.json";
    j["supported_events"] = track_data_.supported_events;

    return j;
}

nlohmann::json EditorProject::SerializeModPackageManifest() const {
    nlohmann::json j;
    j["schema_version"] = "1.0.0";
    j["identifier"] = identifier_;
    j["display_name"] = display_name_;
    j["author"] = manufacturer_;
    j["version"] = "1.0.0";
    j["project_type"] = (type_ == ProjectType::Vehicle) ? "vehicle" : ((type_ == ProjectType::Track) ? "track" : "mod");
    return j;
}

bool EditorProject::DeserializeVehicleManifest(const nlohmann::json& j, std::string& out_error) {
    try {
        if (j.contains("identifier")) identifier_ = j["identifier"].get<std::string>();
        if (j.contains("display_name")) display_name_ = j["display_name"].get<std::string>();
        if (j.contains("vehicle_class")) vehicle_class_ = j["vehicle_class"].get<std::string>();
        if (j.contains("manufacturer")) manufacturer_ = j["manufacturer"].get<std::string>();
        if (j.contains("year")) year_ = j["year"].get<int>();

        if (j.contains("geometry") && j["geometry"].contains("source_model")) {
            source_model_path_ = j["geometry"]["source_model"].get<std::string>();
        }

        if (j.contains("physics")) {
            const auto& p = j["physics"];
            if (p.contains("chassis")) {
                const auto& ch = p["chassis"];
                if (ch.contains("mass_kg")) physics_.mass_kg = ch["mass_kg"].get<float>();
                if (ch.contains("wheelbase_meters")) physics_.wheelbase_meters = ch["wheelbase_meters"].get<float>();
                if (ch.contains("track_front_meters")) physics_.track_front_meters = ch["track_front_meters"].get<float>();
                if (ch.contains("track_rear_meters")) physics_.track_rear_meters = ch["track_rear_meters"].get<float>();
                if (ch.contains("center_of_mass") && ch["center_of_mass"].size() == 3) {
                    physics_.center_of_mass = {ch["center_of_mass"][0].get<float>(),
                                              ch["center_of_mass"][1].get<float>(),
                                              ch["center_of_mass"][2].get<float>()};
                }
            }
            if (p.contains("engine")) {
                const auto& eng = p["engine"];
                if (eng.contains("displacement_liters")) physics_.displacement_liters = eng["displacement_liters"].get<float>();
                if (eng.contains("peak_power_bhp")) physics_.peak_power_bhp = eng["peak_power_bhp"].get<float>();
                if (eng.contains("idle_rpm")) physics_.idle_rpm = eng["idle_rpm"].get<float>();
                if (eng.contains("redline_rpm")) physics_.redline_rpm = eng["redline_rpm"].get<float>();
            }
        }

        if (j.contains("sockets") && j["sockets"].is_array()) {
            sockets_.clear();
            for (const auto& sj : j["sockets"]) {
                SocketDefinition s;
                s.name = sj.value("name", "unknown_socket");
                s.type = sj.value("type", "generic_socket");
                if (sj.contains("position") && sj["position"].size() == 3) {
                    s.position = {sj["position"][0].get<float>(), sj["position"][1].get<float>(), sj["position"][2].get<float>()};
                }
                if (sj.contains("rotation") && sj["rotation"].size() == 4) {
                    s.rotation[0] = sj["rotation"][0].get<float>();
                    s.rotation[1] = sj["rotation"][1].get<float>();
                    s.rotation[2] = sj["rotation"][2].get<float>();
                    s.rotation[3] = sj["rotation"][3].get<float>();
                }
                s.part_group = sj.value("part_group", -1);
                s.detachment_threshold_meters = sj.value("detachment_threshold_meters", 0.0762f);
                s.steerable = sj.value("steerable", false);
                s.driven = sj.value("driven", false);
                s.suspension_travel_meters = sj.value("suspension_travel_meters", 0.20f);
                sockets_.push_back(s);
            }
        }
        return true;
    } catch (const std::exception& e) {
        out_error = e.what();
        return false;
    }
}

bool EditorProject::DeserializeTrackManifest(const nlohmann::json& j, std::string& out_error) {
    try {
        if (j.contains("identifier")) identifier_ = j["identifier"].get<std::string>();
        if (j.contains("display_name")) display_name_ = j["display_name"].get<std::string>();
        if (j.contains("track_type")) track_data_.track_type = j["track_type"].get<std::string>();
        if (j.contains("surface_type")) track_data_.surface_type = j["surface_type"].get<std::string>();
        if (j.contains("length_meters")) track_data_.length_meters = j["length_meters"].get<float>();

        if (j.contains("bounds")) {
            const auto& b = j["bounds"];
            if (b.contains("min") && b["min"].size() == 3) {
                track_data_.bounds_min = {b["min"][0].get<float>(), b["min"][1].get<float>(), b["min"][2].get<float>()};
            }
            if (b.contains("max") && b["max"].size() == 3) {
                track_data_.bounds_max = {b["max"][0].get<float>(), b["max"][1].get<float>(), b["max"][2].get<float>()};
            }
        }
        return true;
    } catch (const std::exception& e) {
        out_error = e.what();
        return false;
    }
}

bool EditorProject::SaveToDirectory(const std::string& dir_path, std::string& out_error) {
    std::error_code ec;
    fs::create_directories(dir_path, ec);
    if (ec) {
        out_error = "Failed to create project directory: " + ec.message();
        return false;
    }

    project_dir_ = dir_path;

    // Save package manifest mod.json
    {
        std::ofstream mf(fs::path(dir_path) / "mod.json");
        if (!mf.is_open()) {
            out_error = "Failed to open mod.json for writing";
            return false;
        }
        mf << SerializeModPackageManifest().dump(2) << std::endl;
    }

    // Save vehicle manifest if vehicle or combined
    if (type_ == ProjectType::Vehicle || type_ == ProjectType::Combined) {
        std::ofstream vf(fs::path(dir_path) / "vehicle.manifest.json");
        if (!vf.is_open()) {
            out_error = "Failed to open vehicle.manifest.json for writing";
            return false;
        }
        vf << SerializeVehicleManifest().dump(2) << std::endl;
    }

    // Save track manifest if track or combined
    if (type_ == ProjectType::Track || type_ == ProjectType::Combined) {
        std::ofstream tf(fs::path(dir_path) / "track.manifest.json");
        if (!tf.is_open()) {
            out_error = "Failed to open track.manifest.json for writing";
            return false;
        }
        tf << SerializeTrackManifest().dump(2) << std::endl;
    }

    return true;
}

bool EditorProject::LoadFromDirectory(const std::string& dir_path, std::string& out_error) {
    std::error_code ec;
    if (!fs::exists(dir_path, ec) || !fs::is_directory(dir_path, ec)) {
        out_error = "Directory does not exist";
        return false;
    }

    project_dir_ = dir_path;

    fs::path vehicle_file = fs::path(dir_path) / "vehicle.manifest.json";
    fs::path track_file = fs::path(dir_path) / "track.manifest.json";

    if (fs::exists(vehicle_file)) {
        std::ifstream vf(vehicle_file);
        try {
            nlohmann::json j;
            vf >> j;
            type_ = ProjectType::Vehicle;
            return DeserializeVehicleManifest(j, out_error);
        } catch (const std::exception& e) {
            out_error = "Failed to parse vehicle.manifest.json: " + std::string(e.what());
            return false;
        }
    } else if (fs::exists(track_file)) {
        std::ifstream tf(track_file);
        try {
            nlohmann::json j;
            tf >> j;
            type_ = ProjectType::Track;
            return DeserializeTrackManifest(j, out_error);
        } catch (const std::exception& e) {
            out_error = "Failed to parse track.manifest.json: " + std::string(e.what());
            return false;
        }
    }

    out_error = "No manifest (vehicle.manifest.json or track.manifest.json) found in directory";
    return false;
}

} // namespace eod::editor
