#include "status_bar.h"
#include "../../third_party/imgui/imgui.h"
#include "../project/project.h"
#include "../undo/command_stack.h"

namespace eod::editor {

void StatusBar::Render(const EditorProject& project,
                       const CommandStack& command_stack,
                       float fps,
                       float frame_time_ms) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    float height = 24.0f;
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - height));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, height));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_MenuBar |
                             ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 2.0f));
    if (ImGui::Begin("##EditorStatusBar", nullptr, flags)) {
        // Dirty indicator
        if (command_stack.IsDirty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "● Unsaved Changes");
        } else {
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "✓ Saved");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        // Project Identifier
        ImGui::Text("Project: %s (%s)", project.GetName().c_str(), project.GetIdentifier().c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        // Undo count
        ImGui::Text("Undo: %zu | Redo: %zu", command_stack.GetUndoCount(), command_stack.GetRedoCount());

        // FPS and Telemetry on the right
        float telemetry_w = 200.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - telemetry_w);
        ImGui::Text("%.1f FPS (%.2f ms)", fps, frame_time_ms);

        ImGui::End();
    }
    ImGui::PopStyleVar();
}

} // namespace eod::editor
