#include "toolbar.h"
#include "../../third_party/imgui/imgui.h"
#include "../graphics/camera.h"

namespace eod::editor {

void Toolbar::Render(GizmoMode& current_mode,
                     bool& use_local_space,
                     bool& snap_enabled,
                     EditorCamera& camera,
                     bool& out_request_play) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + ImGui::GetFrameHeight()));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, 38.0f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_MenuBar |
                             ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
    if (ImGui::Begin("##EditorToolbar", nullptr, flags)) {
        // Gizmo mode buttons
        bool is_select = (current_mode == GizmoMode::Select);
        if (is_select) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.50f, 0.85f, 1.0f));
        if (ImGui::Button("Select (Q)")) current_mode = GizmoMode::Select;
        if (is_select) ImGui::PopStyleColor();
        ImGui::SameLine();

        bool is_trans = (current_mode == GizmoMode::Translate);
        if (is_trans) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.50f, 0.85f, 1.0f));
        if (ImGui::Button("Translate (W)")) current_mode = GizmoMode::Translate;
        if (is_trans) ImGui::PopStyleColor();
        ImGui::SameLine();

        bool is_rot = (current_mode == GizmoMode::Rotate);
        if (is_rot) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.50f, 0.85f, 1.0f));
        if (ImGui::Button("Rotate (E)")) current_mode = GizmoMode::Rotate;
        if (is_rot) ImGui::PopStyleColor();
        ImGui::SameLine();

        bool is_scale = (current_mode == GizmoMode::Scale);
        if (is_scale) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.50f, 0.85f, 1.0f));
        if (ImGui::Button("Scale (R)")) current_mode = GizmoMode::Scale;
        if (is_scale) ImGui::PopStyleColor();
        ImGui::SameLine();

        ImGui::TextDisabled("|");
        ImGui::SameLine();

        // Transform Space Toggle
        if (ImGui::Button(use_local_space ? "Space: Local" : "Space: World")) {
            use_local_space = !use_local_space;
        }
        ImGui::SameLine();

        // Snapping Toggle
        ImGui::Checkbox("Snap", &snap_enabled);
        ImGui::SameLine();

        ImGui::TextDisabled("|");
        ImGui::SameLine();

        // Camera Turntable
        bool turntable = camera.IsTurntableEnabled();
        if (ImGui::Checkbox("Turntable 360", &turntable)) {
            camera.SetTurntable(turntable);
        }
        ImGui::SameLine();

        if (ImGui::Button("Reset Camera")) {
            camera.ResetToDefault();
        }

        // Test Drive Button (Right Aligned)
        float btn_width = 150.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - btn_width - 12.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.65f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.80f, 0.32f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.55f, 0.20f, 1.0f));
        if (ImGui::Button("▶ Play in Game", ImVec2(btn_width, 0.0f))) {
            out_request_play = true;
        }
        ImGui::PopStyleColor(3);

        ImGui::End();
    }
    ImGui::PopStyleVar();
}

} // namespace eod::editor
