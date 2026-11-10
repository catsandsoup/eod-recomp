#include "ui_manager.h"
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../project/project.h"
#include "../undo/command_stack.h"
#include "../graphics/camera.h"
#include "../graphics/viewport_renderer.h"
#include "../validator/schema_validator.h"

namespace eod::editor {

UIManager::UIManager() = default;

void UIManager::Render(EditorProject& project,
                       CommandStack& command_stack,
                       EditorCamera& camera,
                       ViewportRenderer& renderer,
                       SchemaValidator& validator,
                       float fps,
                       float frame_time_ms,
                       bool& out_request_play,
                       bool& out_request_exit) {
    bool req_new = false;
    bool req_open = false;
    bool req_save = false;
    bool req_validate = false;

    // 1. Menu bar
    menu_bar_.Render(command_stack, project, req_new, req_open, req_save, req_validate, out_request_play, out_request_exit);

    // 2. Toolbar
    toolbar_.Render(current_gizmo_mode_, use_local_space_, snap_enabled_, camera, out_request_play);

    // 3. Central DockSpace
    ImGuiViewport* viewport = ImGui::GetMainViewport();

    // Offset for toolbar and status bar
    float toolbar_height = 38.0f;
    float status_bar_height = 24.0f;
    ImVec2 dock_pos = ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + toolbar_height);
    ImVec2 dock_size = ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - toolbar_height - status_bar_height);

    ImGui::SetNextWindowPos(dock_pos);
    ImGui::SetNextWindowSize(dock_size);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags dock_flags = ImGuiWindowFlags_NoDocking |
                                 ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                                 ImGuiWindowFlags_NoNavFocus |
                                 ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin("MainDockSpaceHost", nullptr, dock_flags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspace_id = ImGui::GetID("EditorDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

    // Initial default dock layout
    if (first_frame_) {
        first_frame_ = false;

        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, dock_size);

        ImGuiID dock_center = dockspace_id;
        ImGuiID dock_bottom = ImGui::DockBuilderSplitNode(dock_center, ImGuiDir_Down, 0.28f, nullptr, &dock_center);
        ImGuiID dock_left = ImGui::DockBuilderSplitNode(dock_center, ImGuiDir_Left, 0.22f, nullptr, &dock_center);
        ImGuiID dock_right = ImGui::DockBuilderSplitNode(dock_center, ImGuiDir_Right, 0.28f, nullptr, &dock_center);
        ImGuiID dock_left_bottom = ImGui::DockBuilderSplitNode(dock_left, ImGuiDir_Down, 0.45f, nullptr, &dock_left);

        ImGui::DockBuilderDockWindow("Scene Hierarchy", dock_left);
        ImGui::DockBuilderDockWindow("Asset Browser", dock_left_bottom);
        ImGui::DockBuilderDockWindow("3D Viewport", dock_center);
        ImGui::DockBuilderDockWindow("Properties Inspector", dock_right);
        ImGui::DockBuilderDockWindow("Console & Validator", dock_bottom);

        ImGui::DockBuilderFinish(dockspace_id);
    }
    ImGui::End();

    // 4. Panels
    if (menu_bar_.show_scene_hierarchy) {
        scene_hierarchy_.Render(project, selection_);
    }

    if (menu_bar_.show_properties) {
        properties_panel_.Render(project, command_stack, selection_);
    }

    if (menu_bar_.show_asset_browser) {
        asset_browser_.Render();
    }

    if (menu_bar_.show_viewport) {
        viewport_panel_.Render(camera, renderer, project, selection_, current_gizmo_mode_);
    }

    if (menu_bar_.show_console) {
        bool req_val_from_console = false;
        console_panel_.Render(req_val_from_console);
        if (req_val_from_console) req_validate = true;
    }

    if (menu_bar_.show_status_bar) {
        status_bar_.Render(project, command_stack, fps, frame_time_ms);
    }

    // 5. Actions
    if (req_new) {
        show_new_project_dialog_ = true;
    }

    if (req_save) {
        std::string err;
        std::string save_dir = project.GetProjectPath();
        if (save_dir.empty()) {
            save_dir = "mods/" + project.GetIdentifier();
        }
        if (project.SaveToDirectory(save_dir, err)) {
            command_stack.MarkClean();
            console_panel_.Log(ValidationSeverity::Info, "Project successfully saved to: " + save_dir);
        } else {
            console_panel_.Log(ValidationSeverity::Error, "Failed to save project: " + err);
        }
    }

    if (req_validate) {
        ValidationReport report;
        nlohmann::json vj = project.SerializeVehicleManifest();
        validator.ValidateVehicleManifest(vj, "vehicle.manifest.json", project.GetProjectPath(), report);
        if (project.GetType() == ProjectType::Track || project.GetType() == ProjectType::Combined) {
            nlohmann::json tj = project.SerializeTrackManifest();
            validator.ValidateTrackManifest(tj, "track.manifest.json", project.GetProjectPath(), report);
        }
        console_panel_.SetValidationReport(report);
    }

    // New Project Dialog Modal
    if (show_new_project_dialog_) {
        ImGui::OpenPopup("New Mod Project");
    }

    if (ImGui::BeginPopupModal("New Mod Project", &show_new_project_dialog_, ImGuiWindowFlags_AlwaysAutoResize)) {
        static const char* types[] = {"Custom Vehicle", "Custom Track", "Combined Venue & Car"};
        ImGui::Combo("Project Type", &new_project_type_idx_, types, 3);
        ImGui::InputText("Display Name", new_project_name_, sizeof(new_project_name_));
        ImGui::InputText("Mod Identifier", new_project_id_, sizeof(new_project_id_));

        ImGui::Separator();
        if (ImGui::Button("Create Project", ImVec2(120, 0))) {
            ProjectType ptype = (new_project_type_idx_ == 0) ? ProjectType::Vehicle
                                : ((new_project_type_idx_ == 1) ? ProjectType::Track : ProjectType::Combined);
            project.CreateNew(ptype, new_project_name_, new_project_id_);
            command_stack.Clear();
            asset_browser_.SetRootDirectory("mods/" + std::string(new_project_id_));
            console_panel_.Log(ValidationSeverity::Info, "Created new mod project: " + std::string(new_project_name_));
            show_new_project_dialog_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            show_new_project_dialog_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace eod::editor
