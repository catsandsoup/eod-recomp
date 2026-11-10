#pragma once

#include "scene_hierarchy.h"
#include "toolbar.h"

namespace eod::editor {

class EditorCamera;
class ViewportRenderer;
class EditorProject;

class ViewportPanel {
public:
    ViewportPanel();
    ~ViewportPanel() = default;

    void Render(EditorCamera& camera,
                ViewportRenderer& renderer,
                EditorProject& project,
                const HierarchySelection& selection,
                GizmoMode current_mode);

    bool IsHovered() const { return is_hovered_; }
    bool IsFocused() const { return is_focused_; }

private:
    bool is_hovered_ = false;
    bool is_focused_ = false;
    float last_mouse_x_ = 0.0f;
    float last_mouse_y_ = 0.0f;
    bool was_dragging_ = false;
};

} // namespace eod::editor
