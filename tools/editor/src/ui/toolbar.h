#pragma once

namespace eod::editor {

enum class GizmoMode {
    Select,
    Translate,
    Rotate,
    Scale
};

class EditorCamera;

class Toolbar {
public:
    Toolbar() = default;
    ~Toolbar() = default;

    void Render(GizmoMode& current_mode,
                bool& use_local_space,
                bool& snap_enabled,
                EditorCamera& camera,
                bool& out_request_play);
};

} // namespace eod::editor
