#include "properties_panel.h"
#include "../../third_party/imgui/imgui.h"
#include "../project/project.h"
#include "../undo/command_stack.h"

namespace eod::editor {

void PropertiesPanel::Render(EditorProject& project,
                             CommandStack& command_stack,
                             HierarchySelection& selection) {
    if (ImGui::Begin("Properties Inspector")) {
        if (selection.type == SelectionType::ProjectRoot) {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "Project Manifest Properties");
            ImGui::Separator();

            char name_buf[128];
            snprintf(name_buf, sizeof(name_buf), "%s", project.GetName().c_str());
            if (ImGui::InputText("Display Name", name_buf, sizeof(name_buf))) {
                project.SetName(name_buf);
            }

            char id_buf[128];
            snprintf(id_buf, sizeof(id_buf), "%s", project.GetIdentifier().c_str());
            if (ImGui::InputText("Identifier", id_buf, sizeof(id_buf))) {
                project.SetIdentifier(id_buf);
            }

            // Vehicle class
            static const char* classes[] = {"compact", "midsize", "muscle", "special"};
            int current_cls = 1; // default midsize
            for (int i = 0; i < 4; ++i) {
                if (project.GetVehicleClass() == classes[i]) {
                    current_cls = i;
                    break;
                }
            }
            if (ImGui::Combo("Vehicle Class", &current_cls, classes, 4)) {
                project.SetVehicleClass(classes[current_cls]);
            }

            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "Core Chassis Physics");
            ImGui::Separator();

            auto& phys = project.GetPhysics();
            float mass = phys.mass_kg;
            if (ImGui::DragFloat("Mass (kg)", &mass, 5.0f, 500.0f, 5000.0f, "%.1f")) {
                phys.mass_kg = mass;
            }

            float wb = phys.wheelbase_meters;
            if (ImGui::DragFloat("Wheelbase (m)", &wb, 0.01f, 1.5f, 4.5f, "%.2f")) {
                phys.wheelbase_meters = wb;
            }

            float track_f = phys.track_front_meters;
            if (ImGui::DragFloat("Front Track (m)", &track_f, 0.01f, 1.0f, 2.5f, "%.2f")) {
                phys.track_front_meters = track_f;
            }

            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1.0f), "Engine & Drivetrain");
            ImGui::Separator();

            float bhp = phys.peak_power_bhp;
            if (ImGui::DragFloat("Peak Power (bhp)", &bhp, 5.0f, 50.0f, 1200.0f, "%.0f")) {
                phys.peak_power_bhp = bhp;
            }

            float idle = phys.idle_rpm;
            if (ImGui::DragFloat("Idle RPM", &idle, 25.0f, 500.0f, 2000.0f, "%.0f")) {
                phys.idle_rpm = idle;
            }

            float redline = phys.redline_rpm;
            if (ImGui::DragFloat("Redline RPM", &redline, 50.0f, 4000.0f, 12000.0f, "%.0f")) {
                phys.redline_rpm = redline;
            }

            float friction = phys.nominal_friction;
            if (ImGui::SliderFloat("Tire Friction", &friction, 0.4f, 1.8f, "%.2f")) {
                phys.nominal_friction = friction;
            }

        } else if (selection.type == SelectionType::ComponentGroup) {
            auto& groups = project.GetComponentGroups();
            if (selection.index >= 0 && selection.index < static_cast<int>(groups.size())) {
                auto& g = groups[selection.index];
                ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.4f, 1.0f), "Component Group [%02d]", g.id);
                ImGui::Separator();

                ImGui::LabelText("Part Name", "%s", g.semantic_name.c_str());
                ImGui::Checkbox("Paintable", &g.is_paintable);
                ImGui::Checkbox("Detachable", &g.is_detachable);

                if (g.is_detachable) {
                    ImGui::DragFloat("Detachment Threshold (m)", &g.default_detachment_threshold, 0.005f, 0.01f, 0.50f, "%.4f");
                }
                ImGui::LabelText("Anchor / Hinge", "%s", g.anchor_socket_name.c_str());

                char node_buf[128];
                snprintf(node_buf, sizeof(node_buf), "%s", g.gltf_node_name.c_str());
                if (ImGui::InputText("glTF Mesh Node", node_buf, sizeof(node_buf))) {
                    g.gltf_node_name = node_buf;
                    g.is_assigned = !g.gltf_node_name.empty();
                }
            }
        } else if (selection.type == SelectionType::Socket) {
            auto& sockets = project.GetSockets();
            if (selection.index >= 0 && selection.index < static_cast<int>(sockets.size())) {
                auto& s = sockets[selection.index];
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Socket: %s", s.name.c_str());
                ImGui::Separator();

                char name_buf[64];
                snprintf(name_buf, sizeof(name_buf), "%s", s.name.c_str());
                if (ImGui::InputText("Socket Name", name_buf, sizeof(name_buf))) {
                    s.name = name_buf;
                }

                ImGui::LabelText("Type", "%s", s.type.c_str());

                float pos[3] = {s.position.x, s.position.y, s.position.z};
                if (ImGui::DragFloat3("Position (X,Y,Z)", pos, 0.01f, -10.0f, 10.0f, "%.3f")) {
                    s.position = {pos[0], pos[1], pos[2]};
                }

                if (s.type == "panel_hinge") {
                    ImGui::DragInt("Part Group", &s.part_group, 1, 0, 31);
                    ImGui::DragFloat("Detachment Threshold (m)", &s.detachment_threshold_meters, 0.005f, 0.01f, 0.50f, "%.4f");
                }

                if (s.type == "wheel_socket") {
                    ImGui::Checkbox("Steerable", &s.steerable);
                    ImGui::SameLine();
                    ImGui::Checkbox("Driven", &s.driven);
                    ImGui::DragFloat("Suspension Travel (m)", &s.suspension_travel_meters, 0.01f, 0.05f, 0.60f, "%.2f");
                }
            }
        } else {
            ImGui::TextDisabled("Select an entity in the Scene Hierarchy to view its properties.");
        }
    }
    ImGui::End();
}

} // namespace eod::editor
