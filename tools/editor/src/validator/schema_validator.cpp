#include "schema_validator.h"
#include <fstream>
#include <filesystem>
#include <cmath>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace eod::editor {

void ValidationReport::AddError(std::string rule_id, std::string file_path, std::string json_pointer, std::string message) {
    issues_.push_back({ValidationSeverity::Error, std::move(rule_id), std::move(file_path), std::move(json_pointer), std::move(message)});
    ++error_count_;
}

void ValidationReport::AddWarning(std::string rule_id, std::string file_path, std::string json_pointer, std::string message) {
    issues_.push_back({ValidationSeverity::Warning, std::move(rule_id), std::move(file_path), std::move(json_pointer), std::move(message)});
    ++warning_count_;
}

void ValidationReport::AddInfo(std::string rule_id, std::string file_path, std::string json_pointer, std::string message) {
    issues_.push_back({ValidationSeverity::Info, std::move(rule_id), std::move(file_path), std::move(json_pointer), std::move(message)});
    ++info_count_;
}

void ValidationReport::Clear() {
    issues_.clear();
    error_count_ = 0;
    warning_count_ = 0;
    info_count_ = 0;
}

void ValidationReport::Print(std::ostream& os) const {
    os << "Validation Summary: "
       << error_count_ << " error(s), "
       << warning_count_ << " warning(s), "
       << info_count_ << " info note(s)\n";

    for (const auto& issue : issues_) {
        const char* prefix = "[INFO]";
        if (issue.severity == ValidationSeverity::Error) prefix = "[ERROR]";
        else if (issue.severity == ValidationSeverity::Warning) prefix = "[WARN]";

        os << prefix << " [" << issue.rule_id << "] "
           << issue.file_path;
        if (!issue.json_pointer.empty()) {
            os << " (at " << issue.json_pointer << ")";
        }
        os << ": " << issue.message << "\n";
    }
}

void SchemaValidator::CheckAssetExists(const std::string& relative_path,
                                       const std::string& base_dir,
                                       const std::string& manifest_path,
                                       const std::string& json_ptr,
                                       const std::string& rule_id,
                                       ValidationReport& report) const {
    if (relative_path.empty()) {
        report.AddError(rule_id, manifest_path, json_ptr, "Asset path is empty");
        return;
    }

    if (base_dir.empty()) {
        return; // No root directory provided for relative asset verification
    }

    fs::path full_path = fs::path(base_dir) / relative_path;
    std::error_code ec;
    if (!fs::exists(full_path, ec)) {
        report.AddError(rule_id, manifest_path, json_ptr, "Referenced asset file does not exist: " + relative_path);
        return;
    }

    auto file_size = fs::file_size(full_path, ec);
    if (!ec && file_size == 0) {
        report.AddError(rule_id, manifest_path, json_ptr, "Referenced asset file is empty (0 bytes): " + relative_path);
    }
}

void SchemaValidator::CheckAudioFormat(const std::string& audio_path,
                                       const std::string& manifest_path,
                                       const std::string& json_ptr,
                                       ValidationReport& report) const {
    std::ifstream file(audio_path, std::ios::binary);
    if (!file.is_open()) return;

    char header[44];
    file.read(header, 44);
    if (file.gcount() < 44) {
        report.AddWarning("val_media_compliance", manifest_path, json_ptr, "Audio file is too small to be a valid WAV");
        return;
    }

    // Check "RIFF" and "WAVE"
    if (std::string(header, 4) != "RIFF" || std::string(header + 8, 4) != "WAVE") {
        report.AddWarning("val_media_compliance", manifest_path, json_ptr, "Audio stream is not a standard RIFF/WAVE file");
        return;
    }

    uint32_t sample_rate = *reinterpret_cast<uint32_t*>(header + 24);
    uint16_t bits_per_sample = *reinterpret_cast<uint16_t*>(header + 34);

    if (sample_rate != 48000) {
        report.AddWarning("val_media_compliance", manifest_path, json_ptr,
                          "Audio sample rate is " + std::to_string(sample_rate) + " Hz (project standard is 48000 Hz)");
    }
    if (bits_per_sample != 16) {
        report.AddWarning("val_media_compliance", manifest_path, json_ptr,
                          "Audio bit depth is " + std::to_string(bits_per_sample) + "-bit (expected 16-bit PCM)");
    }
}

bool SchemaValidator::ValidateVehicleManifest(const nlohmann::json& j,
                                             const std::string& manifest_path,
                                             const std::string& base_dir,
                                             ValidationReport& report) const {
    size_t prev_errors = report.GetErrorCount();

    // 1. Root Metadata
    if (!j.contains("identifier") || !j["identifier"].is_string()) {
        report.AddError("val_manifest_syntax", manifest_path, "/identifier", "Missing or non-string 'identifier'");
    }
    if (!j.contains("display_name") || !j["display_name"].is_string()) {
        report.AddError("val_manifest_syntax", manifest_path, "/display_name", "Missing or non-string 'display_name'");
    }
    if (j.contains("vehicle_class")) {
        std::string vclass = j["vehicle_class"].get<std::string>();
        static const std::set<std::string> valid_classes = {"compact", "midsize", "muscle", "special"};
        if (valid_classes.find(vclass) == valid_classes.end()) {
            report.AddError("val_vehicle_class", manifest_path, "/vehicle_class",
                            "Invalid vehicle class '" + vclass + "'. Must be one of: compact, midsize, muscle, special");
        }
    } else {
        report.AddError("val_manifest_syntax", manifest_path, "/vehicle_class", "Missing 'vehicle_class'");
    }

    // 2. Geometry
    if (!j.contains("geometry") || !j["geometry"].is_object()) {
        report.AddError("val_manifest_syntax", manifest_path, "/geometry", "Missing 'geometry' section");
    } else {
        const auto& geom = j["geometry"];
        if (!geom.contains("source_model") || !geom["source_model"].is_string()) {
            report.AddError("val_manifest_syntax", manifest_path, "/geometry/source_model", "Missing 'source_model' path");
        } else {
            CheckAssetExists(geom["source_model"].get<std::string>(), base_dir, manifest_path, "/geometry/source_model", "val_asset_existence", report);
        }

        if (geom.contains("lod_chain") && geom["lod_chain"].is_array()) {
            if (geom["lod_chain"].empty()) {
                report.AddWarning("val_lod_chain", manifest_path, "/geometry/lod_chain", "LOD chain is empty");
            }
        }
    }

    // 3. Physics
    if (!j.contains("physics") || !j["physics"].is_object()) {
        report.AddError("val_manifest_syntax", manifest_path, "/physics", "Missing 'physics' section");
    } else {
        const auto& phys = j["physics"];

        // Chassis
        if (!phys.contains("chassis") || !phys["chassis"].is_object()) {
            report.AddError("val_manifest_syntax", manifest_path, "/physics/chassis", "Missing 'chassis' physics section");
        } else {
            const auto& ch = phys["chassis"];
            if (!ch.contains("mass_kg") || !ch["mass_kg"].is_number() || ch["mass_kg"].get<double>() <= 0.0) {
                report.AddError("val_physics_parameters", manifest_path, "/physics/chassis/mass_kg",
                                "Vehicle mass_kg must be greater than 0");
            }
            if (!ch.contains("wheelbase_meters") || !ch["wheelbase_meters"].is_number() || ch["wheelbase_meters"].get<double>() <= 0.0) {
                report.AddError("val_physics_parameters", manifest_path, "/physics/chassis/wheelbase_meters",
                                "Wheelbase must be greater than 0");
            }
            if (!ch.contains("track_front_meters") || !ch["track_front_meters"].is_number() || ch["track_front_meters"].get<double>() <= 0.0) {
                report.AddError("val_physics_parameters", manifest_path, "/physics/chassis/track_front_meters",
                                "Track front meters must be greater than 0");
            }
            if (ch.contains("center_of_mass") && (!ch["center_of_mass"].is_array() || ch["center_of_mass"].size() != 3)) {
                report.AddError("val_physics_parameters", manifest_path, "/physics/chassis/center_of_mass",
                                "center_of_mass must be a 3-element float array [X, Y, Z]");
            }
        }

        // Engine
        if (!phys.contains("engine") || !phys["engine"].is_object()) {
            report.AddError("val_manifest_syntax", manifest_path, "/physics/engine", "Missing 'engine' physics section");
        } else {
            const auto& eng = phys["engine"];
            double idle = eng.value("idle_rpm", 0.0);
            double redline = eng.value("redline_rpm", 0.0);
            if (idle <= 0.0) {
                report.AddError("val_physics_parameters", manifest_path, "/physics/engine/idle_rpm", "idle_rpm must be positive");
            }
            if (redline <= idle) {
                report.AddError("val_physics_parameters", manifest_path, "/physics/engine/redline_rpm",
                                "redline_rpm (" + std::to_string(redline) + ") must exceed idle_rpm (" + std::to_string(idle) + ")");
            }
            if (eng.value("peak_power_bhp", 0.0) <= 0.0) {
                report.AddError("val_physics_parameters", manifest_path, "/physics/engine/peak_power_bhp", "peak_power_bhp must be positive");
            }
        }

        // Tires
        if (phys.contains("tires") && phys["tires"].is_object()) {
            const auto& tires = phys["tires"];
            if (tires.value("nominal_friction", 0.0) <= 0.0) {
                report.AddError("val_physics_parameters", manifest_path, "/physics/tires/nominal_friction", "nominal_friction must be positive");
            }
        }
    }

    // 4. Sockets & Wheel Verification
    if (!j.contains("sockets") || !j["sockets"].is_array()) {
        report.AddError("val_manifest_syntax", manifest_path, "/sockets", "Missing 'sockets' array");
    } else {
        const auto& sockets = j["sockets"];
        std::set<std::string> socket_names;
        bool has_fl = false, has_fr = false, has_rl = false, has_rr = false;

        for (size_t i = 0; i < sockets.size(); ++i) {
            const auto& sock = sockets[i];
            std::string ptr = "/sockets/" + std::to_string(i);

            if (!sock.contains("name") || !sock["name"].is_string()) {
                report.AddError("val_manifest_syntax", manifest_path, ptr + "/name", "Socket missing 'name'");
                continue;
            }

            std::string name = sock["name"].get<std::string>();
            socket_names.insert(name);

            if (name == "wheel_fl") has_fl = true;
            else if (name == "wheel_fr") has_fr = true;
            else if (name == "wheel_rl") has_rl = true;
            else if (name == "wheel_rr") has_rr = true;

            if (sock.contains("position")) {
                if (!sock["position"].is_array() || sock["position"].size() != 3) {
                    report.AddError("val_manifest_syntax", manifest_path, ptr + "/position", "Socket position must be 3-element array [X,Y,Z]");
                }
            } else {
                report.AddError("val_manifest_syntax", manifest_path, ptr + "/position", "Socket missing 'position'");
            }

            // Hinge detachment threshold check
            if (sock.value("type", "") == "panel_hinge") {
                if (!sock.contains("detachment_threshold_meters") || sock["detachment_threshold_meters"].get<double>() <= 0.0) {
                    report.AddWarning("val_detachable_panel", manifest_path, ptr + "/detachment_threshold_meters",
                                      "Panel hinge has missing or non-positive detachment_threshold_meters");
                }
            }
        }

        if (!has_fl || !has_fr || !has_rl || !has_rr) {
            report.AddError("val_wheel_sockets", manifest_path, "/sockets",
                            "Vehicle must define all 4 wheel sockets: wheel_fl, wheel_fr, wheel_rl, wheel_rr");
        }
    }

    return report.GetErrorCount() == prev_errors;
}

bool SchemaValidator::ValidateTrackManifest(const nlohmann::json& j,
                                           const std::string& manifest_path,
                                           const std::string& base_dir,
                                           ValidationReport& report) const {
    size_t prev_errors = report.GetErrorCount();

    if (!j.contains("identifier") || !j["identifier"].is_string()) {
        report.AddError("val_manifest_syntax", manifest_path, "/identifier", "Missing 'identifier'");
    }
    if (!j.contains("display_name") || !j["display_name"].is_string()) {
        report.AddError("val_manifest_syntax", manifest_path, "/display_name", "Missing 'display_name'");
    }
    if (j.value("length_meters", 0.0) <= 0.0) {
        report.AddError("val_manifest_syntax", manifest_path, "/length_meters", "length_meters must be greater than 0");
    }

    // Bounds
    if (j.contains("bounds") && j["bounds"].is_object()) {
        const auto& bounds = j["bounds"];
        if (bounds.contains("min") && bounds.contains("max") &&
            bounds["min"].is_array() && bounds["max"].is_array() &&
            bounds["min"].size() == 3 && bounds["max"].size() == 3) {
            for (size_t i = 0; i < 3; ++i) {
                if (bounds["min"][i].get<double>() >= bounds["max"][i].get<double>()) {
                    report.AddError("val_track_bounds", manifest_path, "/bounds",
                                    "Bounding box min[" + std::to_string(i) + "] >= max[" + std::to_string(i) + "]");
                }
            }
        } else {
            report.AddError("val_track_bounds", manifest_path, "/bounds", "bounds must have 3-element 'min' and 'max' arrays");
        }
    }

    // Visual model
    if (j.contains("visual") && j["visual"].is_object()) {
        const auto& vis = j["visual"];
        if (vis.contains("scene_model")) {
            CheckAssetExists(vis["scene_model"].get<std::string>(), base_dir, manifest_path, "/visual/scene_model", "val_asset_existence", report);
        }
    }

    // Collision
    if (j.contains("collision") && j["collision"].is_object()) {
        const auto& col = j["collision"];
        if (col.contains("terrain_mesh")) {
            CheckAssetExists(col["terrain_mesh"].get<std::string>(), base_dir, manifest_path, "/collision/terrain_mesh", "val_asset_existence", report);
        }
    }

    // Navigation spline
    if (j.contains("ai_navigation") && j["ai_navigation"].is_object()) {
        const auto& nav = j["ai_navigation"];
        if (nav.contains("primary_spline")) {
            std::string spline_path = nav["primary_spline"].get<std::string>();
            CheckAssetExists(spline_path, base_dir, manifest_path, "/ai_navigation/primary_spline", "val_asset_existence", report);

            // Also validate the spline file content if present
            if (!base_dir.empty()) {
                fs::path full_spline = fs::path(base_dir) / spline_path;
                if (fs::exists(full_spline)) {
                    std::ifstream sf(full_spline);
                    try {
                        nlohmann::json sj = nlohmann::json::parse(sf);
                        ValidateSpline(sj, full_spline.string(), report);
                    } catch (const std::exception& e) {
                        report.AddError("val_spline_syntax", full_spline.string(), "", "Failed to parse spline JSON: " + std::string(e.what()));
                    }
                }
            }
        }
    }

    // Supported events
    if (j.contains("supported_events")) {
        if (!j["supported_events"].is_array() || j["supported_events"].empty()) {
            report.AddWarning("val_supported_events", manifest_path, "/supported_events", "Track defines no supported events");
        }
    }

    return report.GetErrorCount() == prev_errors;
}

bool SchemaValidator::ValidateEventManifest(const nlohmann::json& j,
                                           const std::string& manifest_path,
                                           const std::string& /*base_dir*/,
                                           ValidationReport& report) const {
    size_t prev_errors = report.GetErrorCount();

    if (!j.contains("identifier") || !j["identifier"].is_string()) {
        report.AddError("val_manifest_syntax", manifest_path, "/identifier", "Missing 'identifier'");
    }
    if (!j.contains("category") || !j["category"].is_string()) {
        report.AddError("val_manifest_syntax", manifest_path, "/category", "Missing 'category'");
    }

    if (j.contains("rules") && j["rules"].is_object()) {
        const auto& rules = j["rules"];
        if (rules.value("time_limit_seconds", 0.0) <= 0.0 && !rules.value("no_time_limit", false)) {
            report.AddWarning("val_event_rules", manifest_path, "/rules/time_limit_seconds", "time_limit_seconds is non-positive");
        }
    }

    if (j.contains("restrictions") && j["restrictions"].is_object()) {
        const auto& rest = j["restrictions"];
        if (rest.value("max_participants", 0) <= 0) {
            report.AddError("val_event_restrictions", manifest_path, "/restrictions/max_participants",
                            "max_participants must be greater than 0");
        }
    }

    return report.GetErrorCount() == prev_errors;
}

bool SchemaValidator::ValidateProjectileManifest(const nlohmann::json& j,
                                                const std::string& manifest_path,
                                                const std::string& base_dir,
                                                ValidationReport& report) const {
    size_t prev_errors = report.GetErrorCount();

    if (!j.contains("identifier") || !j["identifier"].is_string()) {
        report.AddError("val_manifest_syntax", manifest_path, "/identifier", "Missing 'identifier'");
    }

    if (j.contains("kinematics") && j["kinematics"].is_object()) {
        const auto& k = j["kinematics"];
        if (k.value("mass_kg", 0.0) <= 0.0) {
            report.AddError("val_projectile_kinematics", manifest_path, "/kinematics/mass_kg", "mass_kg must be greater than 0");
        }
    }

    if (j.contains("detonation") && j["detonation"].is_object()) {
        const auto& det = j["detonation"];
        if (det.value("blast_radius_meters", 0.0) <= 0.0) {
            report.AddError("val_projectile_detonation", manifest_path, "/detonation/blast_radius_meters",
                            "blast_radius_meters must be greater than 0");
        }
        if (det.value("peak_impulse_force_n", 0.0) <= 0.0) {
            report.AddError("val_projectile_detonation", manifest_path, "/detonation/peak_impulse_force_n",
                            "peak_impulse_force_n must be greater than 0");
        }
        if (det.contains("audio") && det["audio"].is_object()) {
            for (auto& item : det["audio"].items()) {
                if (item.value().is_string()) {
                    CheckAssetExists(item.value().get<std::string>(), base_dir, manifest_path,
                                     "/detonation/audio/" + item.key(), "val_asset_existence", report);
                }
            }
        }
    }

    return report.GetErrorCount() == prev_errors;
}

bool SchemaValidator::ValidatePlacedProps(const nlohmann::json& j,
                                         const std::string& manifest_path,
                                         const std::string& base_dir,
                                         ValidationReport& report) const {
    size_t prev_errors = report.GetErrorCount();

    if (!j.contains("instances") || !j["instances"].is_array()) {
        report.AddError("val_props_syntax", manifest_path, "/instances", "Missing 'instances' array");
        return false;
    }

    const auto& instances = j["instances"];
    for (size_t i = 0; i < instances.size(); ++i) {
        const auto& inst = instances[i];
        std::string ptr = "/instances/" + std::to_string(i);

        if (!inst.contains("identifier") || !inst["identifier"].is_string()) {
            report.AddError("val_props_syntax", manifest_path, ptr + "/identifier", "Prop missing 'identifier'");
        }
        if (inst.contains("model") && inst["model"].is_string()) {
            CheckAssetExists(inst["model"].get<std::string>(), base_dir, manifest_path, ptr + "/model", "val_asset_existence", report);
        }
        if (inst.contains("position") && (!inst["position"].is_array() || inst["position"].size() != 3)) {
            report.AddError("val_props_syntax", manifest_path, ptr + "/position", "Prop position must be 3-element float array");
        }
        if (inst.contains("properties") && inst["properties"].is_object()) {
            const auto& p = inst["properties"];
            if (p.value("mass_kg", 0.0) <= 0.0) {
                report.AddError("val_props_physics", manifest_path, ptr + "/properties/mass_kg", "Prop mass_kg must be greater than 0");
            }
        }
    }

    return report.GetErrorCount() == prev_errors;
}

bool SchemaValidator::ValidateSpline(const nlohmann::json& j,
                                     const std::string& manifest_path,
                                     ValidationReport& report) const {
    size_t prev_errors = report.GetErrorCount();

    if (!j.contains("nodes") || !j["nodes"].is_array()) {
        report.AddError("val_spline_syntax", manifest_path, "/nodes", "Missing 'nodes' array");
        return false;
    }

    const auto& nodes = j["nodes"];
    if (nodes.size() < 3) {
        report.AddError("val_spline_continuity", manifest_path, "/nodes",
                        "Spline must contain at least 3 nodes (found " + std::to_string(nodes.size()) + ")");
    }

    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        std::string ptr = "/nodes/" + std::to_string(i);

        if (!node.contains("position") || !node["position"].is_array() || node["position"].size() != 3) {
            report.AddError("val_spline_syntax", manifest_path, ptr + "/position", "Node position must be 3-element array");
        }

        if (node.contains("tangent") && node["tangent"].is_array() && node["tangent"].size() == 3) {
            double tx = node["tangent"][0].get<double>();
            double ty = node["tangent"][1].get<double>();
            double tz = node["tangent"][2].get<double>();
            double len_sq = tx * tx + ty * ty + tz * tz;
            if (len_sq < 1e-6) {
                report.AddError("val_spline_continuity", manifest_path, ptr + "/tangent", "Node tangent vector is zero length");
            }
        } else {
            report.AddError("val_spline_syntax", manifest_path, ptr + "/tangent", "Node tangent must be 3-element array");
        }

        if (node.value("recommended_speed_mps", 0.0) <= 0.0) {
            report.AddWarning("val_spline_speed", manifest_path, ptr + "/recommended_speed_mps",
                              "recommended_speed_mps is non-positive");
        }
        if (node.value("track_width_left_meters", 0.0) <= 0.0 || node.value("track_width_right_meters", 0.0) <= 0.0) {
            report.AddWarning("val_spline_width", manifest_path, ptr, "Corridor width left/right must be positive");
        }
    }

    return report.GetErrorCount() == prev_errors;
}

bool SchemaValidator::ValidateFile(const std::string& file_path,
                                   const std::string& base_dir,
                                   ValidationReport& report) const {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        report.AddError("val_io_error", file_path, "", "Failed to open file for validation");
        return false;
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (const std::exception& e) {
        report.AddError("val_manifest_syntax", file_path, "", "Invalid JSON syntax: " + std::string(e.what()));
        return false;
    }

    std::string filename = fs::path(file_path).filename().string();
    if (filename == "vehicle.manifest.json" || j.contains("vehicle_class")) {
        return ValidateVehicleManifest(j, file_path, base_dir, report);
    } else if (filename == "track.manifest.json" || j.contains("track_type")) {
        return ValidateTrackManifest(j, file_path, base_dir, report);
    } else if (filename == "event.manifest.json" || j.contains("category_bitmask")) {
        return ValidateEventManifest(j, file_path, base_dir, report);
    } else if (filename == "projectile.manifest.json" || j.contains("kinematics")) {
        return ValidateProjectileManifest(j, file_path, base_dir, report);
    } else if (filename == "placed_props.json" || j.contains("instances")) {
        return ValidatePlacedProps(j, file_path, base_dir, report);
    } else if (filename.find(".spline.json") != std::string::npos || j.contains("spline_id")) {
        return ValidateSpline(j, file_path, report);
    }

    report.AddWarning("val_unknown_schema", file_path, "", "Unrecognized manifest file type; basic JSON syntax verified");
    return true;
}

bool SchemaValidator::ValidateModProject(const std::string& project_dir,
                                        ValidationReport& report) const {
    std::error_code ec;
    if (!fs::exists(project_dir, ec) || !fs::is_directory(project_dir, ec)) {
        report.AddError("val_io_error", project_dir, "", "Project directory does not exist or is not a directory");
        return false;
    }

    bool found_manifest = false;
    for (const auto& entry : fs::recursive_directory_iterator(project_dir, ec)) {
        if (!entry.is_regular_file()) continue;

        std::string filename = entry.path().filename().string();
        if (filename.find(".manifest.json") != std::string::npos ||
            filename == "placed_props.json" ||
            filename.find(".spline.json") != std::string::npos ||
            filename == "mod.json") {
            found_manifest = true;
            ValidateFile(entry.path().string(), project_dir, report);
        }
    }

    if (!found_manifest) {
        report.AddWarning("val_empty_project", project_dir, "", "No manifest files (*.manifest.json, placed_props.json) found in directory");
    }

    return report.IsValid();
}

bool SchemaValidator::RunSelfTest() {
    SchemaValidator validator;
    ValidationReport report;

    // Test 1: Conforming vehicle manifest
    nlohmann::json valid_vehicle = {
        {"identifier", "mod.chassis.test_gt"},
        {"display_name", "Test GT"},
        {"vehicle_class", "midsize"},
        {"manufacturer", "Test Works"},
        {"year", 1974},
        {"geometry", {
            {"source_model", "models/test.gltf"},
            {"lod_chain", nlohmann::json::array()}
        }},
        {"physics", {
            {"chassis", {
                {"mass_kg", 1400.0},
                {"wheelbase_meters", 2.75},
                {"track_front_meters", 1.55},
                {"center_of_mass", {0.0, 0.45, 0.10}}
            }},
            {"engine", {
                {"displacement_liters", 5.0},
                {"peak_power_bhp", 350.0},
                {"idle_rpm", 800.0},
                {"redline_rpm", 6500.0}
            }},
            {"tires", {
                {"nominal_friction", 0.95}
            }}
        }},
        {"sockets", {
            {{"name", "wheel_fl"}, {"position", {-0.8, 0.35, 1.4}}},
            {{"name", "wheel_fr"}, {"position", {0.8, 0.35, 1.4}}},
            {{"name", "wheel_rl"}, {"position", {-0.8, 0.35, -1.4}}},
            {{"name", "wheel_rr"}, {"position", {0.8, 0.35, -1.4}}}
        }}
    };

    if (!validator.ValidateVehicleManifest(valid_vehicle, "test_vehicle.manifest.json", "", report)) {
        return false;
    }

    // Test 2: Intentional error case (negative mass, redline < idle, missing wheel sockets)
    ValidationReport bad_report;
    nlohmann::json invalid_vehicle = valid_vehicle;
    invalid_vehicle["physics"]["chassis"]["mass_kg"] = -100.0;
    invalid_vehicle["physics"]["engine"]["redline_rpm"] = 500.0;
    invalid_vehicle["physics"]["engine"]["idle_rpm"] = 900.0;
    invalid_vehicle["sockets"] = nlohmann::json::array(); // zero sockets

    validator.ValidateVehicleManifest(invalid_vehicle, "invalid_vehicle.manifest.json", "", bad_report);
    if (bad_report.IsValid() || bad_report.GetErrorCount() < 3) {
        return false; // Expected at least 3 errors
    }

    // Test 3: Spline validation
    ValidationReport spline_report;
    nlohmann::json valid_spline = {
        {"spline_id", "test_line"},
        {"nodes", {
            {{"position", {0.0, 0.0, 0.0}}, {"tangent", {1.0, 0.0, 0.0}}, {"recommended_speed_mps", 30.0}, {"track_width_left_meters", 5.0}, {"track_width_right_meters", 5.0}},
            {{"position", {50.0, 0.0, 0.0}}, {"tangent", {1.0, 0.0, 0.0}}, {"recommended_speed_mps", 30.0}, {"track_width_left_meters", 5.0}, {"track_width_right_meters", 5.0}},
            {{"position", {100.0, 0.0, 0.0}}, {"tangent", {1.0, 0.0, 0.0}}, {"recommended_speed_mps", 30.0}, {"track_width_left_meters", 5.0}, {"track_width_right_meters", 5.0}}
        }}
    };
    if (!validator.ValidateSpline(valid_spline, "test.spline.json", spline_report)) {
        return false;
    }

    return true;
}

} // namespace eod::editor
