#include <iostream>
#include <cassert>
#include <filesystem>
#include <cmath>

#include "../src/undo/command_stack.h"
#include "../src/validator/schema_validator.h"
#include "../src/project/project.h"
#include "../src/graphics/camera.h"
#include "../src/graphics/viewport_renderer.h"

namespace fs = std::filesystem;
using namespace eod::editor;

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "Assertion failed: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            std::exit(1); \
        } \
    } while(0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_NEAR(a, b, eps) ASSERT_TRUE(std::abs((a) - (b)) <= (eps))

void TestUndoRedoBasic() {
    std::cout << "[TEST] Running TestUndoRedoBasic..." << std::endl;
    CommandStack stack(1024 * 1024, 100);

    float value = 10.0f;
    auto cmd1 = std::make_unique<PropertyChangeCommand<float>>("Change Value", &value, 10.0f, 25.0f);
    stack.PushAndExecute(std::move(cmd1));

    ASSERT_NEAR(value, 25.0f, 0.001f);
    ASSERT_TRUE(stack.CanUndo());
    ASSERT_FALSE(stack.CanRedo());
    ASSERT_EQ(stack.GetUndoCount(), 1u);
    ASSERT_EQ(stack.GetUndoDescription(), "Change Value");
    ASSERT_TRUE(stack.IsDirty());

    stack.Undo();
    ASSERT_NEAR(value, 10.0f, 0.001f);
    ASSERT_FALSE(stack.CanUndo());
    ASSERT_TRUE(stack.CanRedo());
    ASSERT_EQ(stack.GetRedoCount(), 1u);

    stack.Redo();
    ASSERT_NEAR(value, 25.0f, 0.001f);
    ASSERT_TRUE(stack.CanUndo());
    ASSERT_FALSE(stack.CanRedo());

    std::cout << "  ✓ TestUndoRedoBasic passed." << std::endl;
}

void TestUndoRedoTransactions() {
    std::cout << "[TEST] Running TestUndoRedoTransactions..." << std::endl;
    CommandStack stack;

    float x = 0.0f, y = 0.0f, z = 0.0f;

    stack.BeginTransaction("Transform Translation");
    stack.PushAndExecute(std::make_unique<PropertyChangeCommand<float>>("Set X", &x, 0.0f, 5.0f));
    stack.PushAndExecute(std::make_unique<PropertyChangeCommand<float>>("Set Y", &y, 0.0f, 10.0f));
    stack.PushAndExecute(std::make_unique<PropertyChangeCommand<float>>("Set Z", &z, 0.0f, -2.5f));
    stack.EndTransaction();

    ASSERT_NEAR(x, 5.0f, 0.001f);
    ASSERT_NEAR(y, 10.0f, 0.001f);
    ASSERT_NEAR(z, -2.5f, 0.001f);
    ASSERT_EQ(stack.GetUndoCount(), 1u); // Grouped into single transaction

    stack.Undo();
    ASSERT_NEAR(x, 0.0f, 0.001f);
    ASSERT_NEAR(y, 0.0f, 0.001f);
    ASSERT_NEAR(z, 0.0f, 0.001f);

    stack.Redo();
    ASSERT_NEAR(x, 5.0f, 0.001f);
    ASSERT_NEAR(y, 10.0f, 0.001f);
    ASSERT_NEAR(z, -2.5f, 0.001f);

    // Test CancelTransaction
    stack.BeginTransaction("Cancelled Action");
    stack.PushAndExecute(std::make_unique<PropertyChangeCommand<float>>("Set X", &x, 5.0f, 99.0f));
    ASSERT_NEAR(x, 99.0f, 0.001f);
    stack.CancelTransaction();
    ASSERT_NEAR(x, 5.0f, 0.001f); // Rolled back
    ASSERT_EQ(stack.GetUndoCount(), 1u);

    std::cout << "  ✓ TestUndoRedoTransactions passed." << std::endl;
}

void TestUndoMemoryEviction() {
    std::cout << "[TEST] Running TestUndoMemoryEviction..." << std::endl;
    // Set small budget of 5 commands max
    CommandStack stack(1024 * 1024, 5);

    int val = 0;
    for (int i = 1; i <= 10; ++i) {
        stack.PushAndExecute(std::make_unique<PropertyChangeCommand<int>>("Increment", &val, val, i));
    }

    ASSERT_EQ(val, 10);
    ASSERT_EQ(stack.GetUndoCount(), 5u); // Evicted first 5
    ASSERT_EQ(stack.GetTotalHistoryCount(), 5u);

    // Can only undo 5 times back to 5
    for (int i = 0; i < 5; ++i) {
        stack.Undo();
    }
    ASSERT_EQ(val, 5);
    ASSERT_FALSE(stack.CanUndo());

    std::cout << "  ✓ TestUndoMemoryEviction passed." << std::endl;
}

void TestDirtyStateTracking() {
    std::cout << "[TEST] Running TestDirtyStateTracking..." << std::endl;
    CommandStack stack;
    ASSERT_FALSE(stack.IsDirty());

    int val = 0;
    stack.PushAndExecute(std::make_unique<PropertyChangeCommand<int>>("Step 1", &val, 0, 1));
    ASSERT_TRUE(stack.IsDirty());

    stack.MarkClean();
    ASSERT_FALSE(stack.IsDirty());

    stack.PushAndExecute(std::make_unique<PropertyChangeCommand<int>>("Step 2", &val, 1, 2));
    ASSERT_TRUE(stack.IsDirty());

    stack.Undo();
    ASSERT_FALSE(stack.IsDirty()); // Back to clean index

    stack.Redo();
    ASSERT_TRUE(stack.IsDirty());

    std::cout << "  ✓ TestDirtyStateTracking passed." << std::endl;
}

void TestProjectSerializationRoundtrip() {
    std::cout << "[TEST] Running TestProjectSerializationRoundtrip..." << std::endl;
    EditorProject project;
    project.CreateNew(ProjectType::Vehicle, "Thunder Phantom", "mod.chassis.thunder_phantom");
    project.SetVehicleClass("muscle");

    auto& phys = project.GetPhysics();
    phys.mass_kg = 1450.0f;
    phys.wheelbase_meters = 2.82f;
    phys.peak_power_bhp = 425.0f;
    phys.redline_rpm = 6800.0f;

    // Add custom socket
    SocketDefinition custom_socket;
    custom_socket.name = "light_bar_roof";
    custom_socket.type = "aux_light";
    custom_socket.position = {0.0f, 1.55f, 0.20f};
    project.AddSocket(custom_socket);

    fs::path temp_dir = fs::temp_directory_path() / "eod_editor_test_project";
    std::error_code ec;
    fs::remove_all(temp_dir, ec);

    std::string err;
    bool saved = project.SaveToDirectory(temp_dir.string(), err);
    ASSERT_TRUE(saved);

    // Reload into a fresh project
    EditorProject loaded;
    bool loaded_ok = loaded.LoadFromDirectory(temp_dir.string(), err);
    ASSERT_TRUE(loaded_ok);

    ASSERT_EQ(loaded.GetName(), "Thunder Phantom");
    ASSERT_EQ(loaded.GetIdentifier(), "mod.chassis.thunder_phantom");
    ASSERT_EQ(loaded.GetVehicleClass(), "muscle");
    ASSERT_NEAR(loaded.GetPhysics().mass_kg, 1450.0f, 0.01f);
    ASSERT_NEAR(loaded.GetPhysics().wheelbase_meters, 2.82f, 0.01f);
    ASSERT_NEAR(loaded.GetPhysics().peak_power_bhp, 425.0f, 0.01f);
    ASSERT_NEAR(loaded.GetPhysics().redline_rpm, 6800.0f, 0.01f);

    // Verify custom socket persisted
    bool found_socket = false;
    for (const auto& s : loaded.GetSockets()) {
        if (s.name == "light_bar_roof") {
            found_socket = true;
            ASSERT_NEAR(s.position.x, 0.0f, 0.001f);
            ASSERT_NEAR(s.position.y, 1.55f, 0.001f);
            ASSERT_NEAR(s.position.z, 0.20f, 0.001f);
            break;
        }
    }
    ASSERT_TRUE(found_socket);

    fs::remove_all(temp_dir, ec);
    std::cout << "  ✓ TestProjectSerializationRoundtrip passed." << std::endl;
}

void Test32ComponentGroupsIntegrity() {
    std::cout << "[TEST] Running Test32ComponentGroupsIntegrity..." << std::endl;
    EditorProject project;
    const auto& groups = project.GetComponentGroups();

    ASSERT_EQ(groups.size(), 32u);

    // Check specific required groups from docs/MODDING_SUITE_FOUNDATIONS.md Table 2.2
    ASSERT_EQ(groups[0].id, 0);
    ASSERT_EQ(groups[0].semantic_name, "Chassis Subframe / Unibody");
    ASSERT_FALSE(groups[0].is_detachable);

    ASSERT_EQ(groups[2].id, 2);
    ASSERT_EQ(groups[2].semantic_name, "Engine Hood");
    ASSERT_TRUE(groups[2].is_detachable);
    ASSERT_NEAR(groups[2].default_detachment_threshold, 0.1270f, 0.0001f);

    ASSERT_EQ(groups[5].id, 5);
    ASSERT_EQ(groups[5].semantic_name, "Right Passenger Door");
    ASSERT_TRUE(groups[5].is_detachable);

    ASSERT_EQ(groups[6].id, 6);
    ASSERT_EQ(groups[6].semantic_name, "Left Driver Door");
    ASSERT_TRUE(groups[6].is_detachable);

    ASSERT_EQ(groups[11].id, 11);
    ASSERT_EQ(groups[11].semantic_name, "Trunk Decklid");
    ASSERT_TRUE(groups[11].is_detachable);
    ASSERT_NEAR(groups[11].default_detachment_threshold, 0.0381f, 0.0001f);

    ASSERT_EQ(groups[12].id, 12);
    ASSERT_EQ(groups[12].semantic_name, "Front Bumper");
    ASSERT_TRUE(groups[12].is_detachable);

    ASSERT_EQ(groups[13].id, 13);
    ASSERT_EQ(groups[13].semantic_name, "Rear Bumper");
    ASSERT_TRUE(groups[13].is_detachable);

    std::cout << "  ✓ Test32ComponentGroupsIntegrity passed." << std::endl;
}

void TestSchemaValidation() {
    std::cout << "[TEST] Running TestSchemaValidation..." << std::endl;
    SchemaValidator validator;
    ValidationReport report;

    // 1. Valid vehicle manifest
    nlohmann::json valid_v = {
        {"identifier", "mod.chassis.phantom"},
        {"display_name", "Phantom"},
        {"vehicle_class", "midsize"},
        {"manufacturer", "Custom Works"},
        {"year", 1974},
        {"geometry", {{"source_model", "models/phantom.gltf"}}},
        {"physics", {
            {"chassis", {{"mass_kg", 1350.0}, {"wheelbase_meters", 2.74}, {"track_front_meters", 1.57}}},
            {"engine", {{"displacement_liters", 5.7}, {"peak_power_bhp", 380.0}, {"idle_rpm", 800.0}, {"redline_rpm", 6500.0}}},
            {"tires", {{"nominal_friction", 0.92}}}
        }},
        {"sockets", {
            {{"name", "wheel_fl"}, {"position", {-0.8, 0.35, 1.4}}},
            {{"name", "wheel_fr"}, {"position", {0.8, 0.35, 1.4}}},
            {{"name", "wheel_rl"}, {"position", {-0.8, 0.35, -1.4}}},
            {{"name", "wheel_rr"}, {"position", {0.8, 0.35, -1.4}}}
        }}
    };

    bool ok = validator.ValidateVehicleManifest(valid_v, "vehicle.manifest.json", "", report);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(report.IsValid());

    // 2. Invalid vehicle: negative mass, redline < idle, missing wheel sockets
    ValidationReport bad_report;
    nlohmann::json bad_v = valid_v;
    bad_v["physics"]["chassis"]["mass_kg"] = -50.0;
    bad_v["physics"]["engine"]["idle_rpm"] = 1200.0;
    bad_v["physics"]["engine"]["redline_rpm"] = 800.0; // redline < idle
    bad_v["sockets"] = {
        {{"name", "wheel_fl"}, {"position", {0, 0, 0}}} // missing fr, rl, rr
    };

    validator.ValidateVehicleManifest(bad_v, "vehicle.manifest.json", "", bad_report);
    ASSERT_FALSE(bad_report.IsValid());
    ASSERT_TRUE(bad_report.GetErrorCount() >= 3);

    // 3. Track manifest validation
    ValidationReport track_report;
    nlohmann::json valid_track = {
        {"identifier", "mod.track.thunder_valley"},
        {"display_name", "Thunder Valley"},
        {"length_meters", 1200.0},
        {"bounds", {
            {"min", {-500.0, -10.0, -500.0}},
            {"max", {500.0, 50.0, 500.0}}
        }}
    };
    ASSERT_TRUE(validator.ValidateTrackManifest(valid_track, "track.manifest.json", "", track_report));

    // Invalid track bounds (min > max)
    ValidationReport bad_track_report;
    nlohmann::json bad_track = valid_track;
    bad_track["bounds"]["min"] = {600.0, 0.0, 0.0};
    bad_track["bounds"]["max"] = {100.0, 0.0, 0.0};
    validator.ValidateTrackManifest(bad_track, "track.manifest.json", "", bad_track_report);
    ASSERT_FALSE(bad_track_report.IsValid());

    std::cout << "  ✓ TestSchemaValidation passed." << std::endl;
}

void TestCameraAndShowroomMath() {
    std::cout << "[TEST] Running TestCameraAndShowroomMath..." << std::endl;
    EditorCamera camera;
    camera.FocusOn({0.0f, 0.0f, 0.0f}, 5.0f);

    Vector3f pos = camera.GetPosition();
    ASSERT_NEAR(pos.y, 5.0f * std::sin(camera.GetOrbitState().pitch), 0.01f);

    Matrix4x4f view = camera.GetViewMatrix();
    // Check view matrix row 3 is [0, 0, 0, 1]
    ASSERT_NEAR(view.m[3][0], 0.0f, 0.001f);
    ASSERT_NEAR(view.m[3][1], 0.0f, 0.001f);
    ASSERT_NEAR(view.m[3][2], 0.0f, 0.001f);
    ASSERT_NEAR(view.m[3][3], 1.0f, 0.001f);

    // Test Ground Plane Raycast
    Vector3f ray_origin{0.0f, 10.0f, 0.0f};
    Vector3f ray_dir{0.0f, -1.0f, 0.0f};
    Vector3f hit;
    bool intersects = camera.RayIntersectsGroundPlane(ray_origin, ray_dir, hit);
    ASSERT_TRUE(intersects);
    ASSERT_NEAR(hit.x, 0.0f, 0.001f);
    ASSERT_NEAR(hit.y, 0.0f, 0.001f);
    ASSERT_NEAR(hit.z, 0.0f, 0.001f);

    std::cout << "  ✓ TestCameraAndShowroomMath passed." << std::endl;
}

int main(int /*argc*/, char* /*argv*/[]) {
    std::cout << "========================================" << std::endl;
    std::cout << "Test Drive: Eve of Destruction Editor Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    TestUndoRedoBasic();
    TestUndoRedoTransactions();
    TestUndoMemoryEviction();
    TestDirtyStateTracking();
    TestProjectSerializationRoundtrip();
    Test32ComponentGroupsIntegrity();
    TestSchemaValidation();
    TestCameraAndShowroomMath();

    std::cout << "\nAll 8 editor test suites passed successfully! (100% pass rate)" << std::endl;
    return 0;
}
