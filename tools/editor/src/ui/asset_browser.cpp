#include "asset_browser.h"
#include "../../third_party/imgui/imgui.h"
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace eod::editor {

void AssetBrowser::Render() {
    if (ImGui::Begin("Asset Browser")) {
        if (root_dir_.empty()) {
            ImGui::TextDisabled("No mod project open.");
            ImGui::End();
            return;
        }

        // Current path breadcrumb / navigation
        ImGui::Text("Path: %s", current_dir_.c_str());
        if (current_dir_ != root_dir_) {
            ImGui::SameLine();
            if (ImGui::Button("↑ Up")) {
                current_dir_ = fs::path(current_dir_).parent_path().string();
            }
        }
        ImGui::Separator();

        std::error_code ec;
        if (!fs::exists(current_dir_, ec)) {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "Directory does not exist");
            ImGui::End();
            return;
        }

        // List subdirectories and files
        for (const auto& entry : fs::directory_iterator(current_dir_, ec)) {
            std::string name = entry.path().filename().string();
            if (name.empty() || name[0] == '.') continue; // hide hidden files

            if (entry.is_directory()) {
                if (ImGui::Selectable(("📁 " + name).c_str(), false, ImGuiSelectableFlags_NoAutoClosePopups)) {
                    current_dir_ = entry.path().string();
                    break;
                }
            } else {
                std::string ext = entry.path().extension().string();
                std::string icon = "📄 ";
                if (ext == ".gltf" || ext == ".glb") icon = "🚗 ";
                else if (ext == ".png" || ext == ".jpg" || ext == ".hdr") icon = "🖼️ ";
                else if (ext == ".wav" || ext == ".ogg") icon = "🎵 ";
                else if (ext == ".json") icon = "⚙️ ";

                bool is_selected = (selected_path_ == entry.path().string());
                if (ImGui::Selectable((icon + name).c_str(), is_selected)) {
                    selected_path_ = entry.path().string();
                }
            }
        }
    }
    ImGui::End();
}

} // namespace eod::editor
