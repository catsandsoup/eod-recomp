#include "viewport_panel.h"
#include "../../third_party/imgui/imgui.h"
#include "../graphics/camera.h"
#include "../graphics/viewport_renderer.h"
#include "../project/project.h"

namespace eod::editor {

ViewportPanel::ViewportPanel() = default;

void ViewportPanel::Render(EditorCamera& camera,
                           ViewportRenderer& renderer,
                           EditorProject& project,
                           const HierarchySelection& selection,
                           GizmoMode /*current_mode*/) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("3D Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        is_hovered_ = ImGui::IsWindowHovered();
        is_focused_ = ImGui::IsWindowFocused();

        ImVec2 screen_pos = ImGui::GetCursorScreenPos();
        ImVec2 size = ImGui::GetContentRegionAvail();

        if (size.x > 10.0f && size.y > 10.0f) {
            // Handle Camera Mouse Drag
            ImGuiIO& io = ImGui::GetIO();
            if (is_hovered_) {
                // Scroll zoom
                if (std::abs(io.MouseWheel) > 0.001f) {
                    camera.OnMouseScroll(io.MouseWheel);
                }

                // Right button = orbit, Middle button = pan
                bool is_right_down = ImGui::IsMouseDown(ImGuiMouseButton_Right);
                bool is_middle_down = ImGui::IsMouseDown(ImGuiMouseButton_Middle);

                if (is_right_down || is_middle_down) {
                    if (was_dragging_) {
                        float dx = io.MousePos.x - last_mouse_x_;
                        float dy = io.MousePos.y - last_mouse_y_;
                        camera.OnMouseDrag(dx, dy, is_right_down, is_middle_down);
                    }
                    last_mouse_x_ = io.MousePos.x;
                    last_mouse_y_ = io.MousePos.y;
                    was_dragging_ = true;
                } else {
                    was_dragging_ = false;
                }
            } else {
                was_dragging_ = false;
            }

            // Populate primitives to render
            renderer.ClearPrimitives();

            // Sockets rendering (Diamond markers)
            const auto& sockets = project.GetSockets();
            for (size_t i = 0; i < sockets.size(); ++i) {
                const auto& s = sockets[i];
                bool is_selected = (selection.type == SelectionType::Socket && selection.index == static_cast<int>(i));

                uint32_t color = 0xFF00DDAA; // cyan/green default
                float size_marker = 0.12f;

                if (s.type == "wheel_socket") {
                    color = 0xFF5588FF; // orange for wheels
                    size_marker = 0.16f;
                } else if (s.type == "panel_hinge") {
                    color = 0xFFE555E5; // magenta for hinges
                }

                if (is_selected) {
                    color = 0xFF33FFFF; // bright yellow if selected
                    size_marker *= 1.3f;
                }

                renderer.AddDiamondSocket(s.position, size_marker, color);
            }

            // Draw into window draw list
            ImDrawList* draw_list = ImGui::GetWindowDrawList();

            // Background dark gradient
            draw_list->AddRectFilled(screen_pos, ImVec2(screen_pos.x + size.x, screen_pos.y + size.y), 0xFF181818);

            // Render 3D Grid, Axes, and Sockets
            renderer.Render(draw_list, screen_pos.x, screen_pos.y, size.x, size.y, camera);

            // HUD Overlay inside Viewport
            char cam_hud[128];
            Vector3f cam_pos = camera.GetPosition();
            snprintf(cam_hud, sizeof(cam_hud), "Camera: [%.1f, %.1f, %.1f] | Dist: %.1fm%s",
                     cam_pos.x, cam_pos.y, cam_pos.z, camera.GetDistance(),
                     camera.IsTurntableEnabled() ? " [Turntable ON]" : "");

            draw_list->AddText(ImVec2(screen_pos.x + 12.0f, screen_pos.y + 12.0f), 0xAAFFFFFF, cam_hud);

            const char* help_hint = "Right-drag: Orbit | Middle-drag: Pan | Scroll: Zoom";
            draw_list->AddText(ImVec2(screen_pos.x + 12.0f, screen_pos.y + size.y - 24.0f), 0x66FFFFFF, help_hint);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace eod::editor
