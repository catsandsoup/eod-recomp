#include "menu_bar.h"
#include "../../third_party/imgui/imgui.h"
#include "../undo/command_stack.h"
#include "../project/project.h"

namespace eod::editor {

void MenuBar::Render(CommandStack& command_stack,
                    EditorProject& project,
                    bool& out_request_new,
                    bool& out_request_open,
                    bool& out_request_save,
                    bool& out_request_validate,
                    bool& out_request_play,
                    bool& out_request_exit) {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Project...", "Ctrl+N")) {
                out_request_new = true;
            }
            if (ImGui::MenuItem("Open Project...", "Ctrl+O")) {
                out_request_open = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save Project", "Ctrl+S")) {
                out_request_save = true;
            }
            if (ImGui::MenuItem("Build Mod Package...", "Ctrl+B")) {
                out_request_validate = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                out_request_exit = true;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit")) {
            std::string undo_label = "Undo " + (command_stack.CanUndo() ? ("(" + command_stack.GetUndoDescription() + ")") : "");
            if (ImGui::MenuItem(undo_label.c_str(), "Ctrl+Z", false, command_stack.CanUndo())) {
                command_stack.Undo();
            }

            std::string redo_label = "Redo " + (command_stack.CanRedo() ? ("(" + command_stack.GetRedoDescription() + ")") : "");
            if (ImGui::MenuItem(redo_label.c_str(), "Ctrl+Y", false, command_stack.CanRedo())) {
                command_stack.Redo();
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Clear Undo History", nullptr, false, command_stack.GetTotalHistoryCount() > 0)) {
                command_stack.Clear();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Scene Hierarchy", nullptr, &show_scene_hierarchy);
            ImGui::MenuItem("Properties Inspector", nullptr, &show_properties);
            ImGui::MenuItem("Asset Browser", nullptr, &show_asset_browser);
            ImGui::MenuItem("3D Viewport", nullptr, &show_viewport);
            ImGui::MenuItem("Console & Validator", nullptr, &show_console);
            ImGui::MenuItem("Status Bar", nullptr, &show_status_bar);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Tools")) {
            if (ImGui::MenuItem("Validate Mod Schema", "F7")) {
                out_request_validate = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Test Drive in Game (Direct Start)", "F5")) {
                out_request_play = true;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            ImGui::MenuItem("Modding Foundations Specifications", nullptr, false, false);
            ImGui::MenuItem("About Test Drive: EoD Modding Suite", nullptr, false, false);
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
}

} // namespace eod::editor
