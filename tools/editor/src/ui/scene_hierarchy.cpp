#include "scene_hierarchy.h"
#include "../../third_party/imgui/imgui.h"
#include "../project/project.h"

namespace eod::editor {

void SceneHierarchy::Render(EditorProject& project, HierarchySelection& selection) {
    if (ImGui::Begin("Scene Hierarchy")) {
        // Project Root node
        ImGuiTreeNodeFlags root_flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                       ImGuiTreeNodeFlags_DefaultOpen |
                                       ImGuiTreeNodeFlags_SpanAvailWidth;
        if (selection.type == SelectionType::ProjectRoot) {
            root_flags |= ImGuiTreeNodeFlags_Selected;
        }

        std::string root_title = project.GetName() + " (" + project.GetIdentifier() + ")";
        bool root_open = ImGui::TreeNodeEx("RootProjectNode", root_flags, "%s", root_title.c_str());
        if (ImGui::IsItemClicked()) {
            selection.type = SelectionType::ProjectRoot;
            selection.index = -1;
            selection.name = project.GetIdentifier();
        }

        if (root_open) {
            // Vehicle hierarchy
            if (project.GetType() == ProjectType::Vehicle || project.GetType() == ProjectType::Combined) {
                // 32 Component Groups
                if (ImGui::TreeNodeEx("ComponentGroupsNode", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen, "32 Component Groups")) {
                    auto& groups = project.GetComponentGroups();
                    for (size_t i = 0; i < groups.size(); ++i) {
                        const auto& g = groups[i];
                        ImGuiTreeNodeFlags node_flags = ImGuiTreeNodeFlags_Leaf |
                                                       ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                       ImGuiTreeNodeFlags_SpanAvailWidth;

                        if (selection.type == SelectionType::ComponentGroup && selection.index == static_cast<int>(i)) {
                            node_flags |= ImGuiTreeNodeFlags_Selected;
                        }

                        char label[128];
                        snprintf(label, sizeof(label), "[%02d] %s%s", g.id, g.semantic_name.c_str(),
                                 g.is_detachable ? " (Detachable)" : "");

                        ImGui::TreeNodeEx((void*)(intptr_t)(100 + i), node_flags, "%s", label);
                        if (ImGui::IsItemClicked()) {
                            selection.type = SelectionType::ComponentGroup;
                            selection.index = static_cast<int>(i);
                            selection.name = g.semantic_name;
                        }
                    }
                    ImGui::TreePop();
                }

                // Sockets Branch
                if (ImGui::TreeNodeEx("SocketsNode", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen, "Sockets & Mounts")) {
                    auto& sockets = project.GetSockets();
                    for (size_t i = 0; i < sockets.size(); ++i) {
                        const auto& s = sockets[i];
                        ImGuiTreeNodeFlags node_flags = ImGuiTreeNodeFlags_Leaf |
                                                       ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                       ImGuiTreeNodeFlags_SpanAvailWidth;

                        if (selection.type == SelectionType::Socket && selection.index == static_cast<int>(i)) {
                            node_flags |= ImGuiTreeNodeFlags_Selected;
                        }

                        char label[128];
                        snprintf(label, sizeof(label), "◆ %s (%s)", s.name.c_str(), s.type.c_str());

                        ImGui::TreeNodeEx((void*)(intptr_t)(200 + i), node_flags, "%s", label);
                        if (ImGui::IsItemClicked()) {
                            selection.type = SelectionType::Socket;
                            selection.index = static_cast<int>(i);
                            selection.name = s.name;
                        }
                    }
                    ImGui::TreePop();
                }
            }

            // Track hierarchy
            if (project.GetType() == ProjectType::Track || project.GetType() == ProjectType::Combined) {
                const auto& td = project.GetTrackData();
                if (ImGui::TreeNodeEx("TerrainNode", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen, "Terrain & Mesh Collision")) {
                    if (ImGui::IsItemClicked()) {
                        selection.type = SelectionType::ProjectRoot;
                    }
                }
                if (ImGui::TreeNodeEx("SplineNode", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen, "AI Centerline Navigation Spline")) {
                    if (ImGui::IsItemClicked()) {
                        selection.type = SelectionType::ProjectRoot;
                    }
                }
            }

            ImGui::TreePop();
        }
    }
    ImGui::End();
}

} // namespace eod::editor
