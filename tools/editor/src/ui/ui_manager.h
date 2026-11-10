#pragma once

#include "menu_bar.h"
#include "toolbar.h"
#include "scene_hierarchy.h"
#include "properties_panel.h"
#include "asset_browser.h"
#include "viewport_panel.h"
#include "console_panel.h"
#include "status_bar.h"

namespace eod::editor {

class EditorProject;
class CommandStack;
class EditorCamera;
class ViewportRenderer;
class SchemaValidator;

class UIManager {
public:
    UIManager();
    ~UIManager() = default;

    void Render(EditorProject& project,
                CommandStack& command_stack,
                EditorCamera& camera,
                ViewportRenderer& renderer,
                SchemaValidator& validator,
                float fps,
                float frame_time_ms,
                bool& out_request_play,
                bool& out_request_exit);

    ConsolePanel& GetConsole() { return console_panel_; }
    AssetBrowser& GetAssetBrowser() { return asset_browser_; }

private:
    MenuBar menu_bar_;
    Toolbar toolbar_;
    SceneHierarchy scene_hierarchy_;
    PropertiesPanel properties_panel_;
    AssetBrowser asset_browser_;
    ViewportPanel viewport_panel_;
    ConsolePanel console_panel_;
    StatusBar status_bar_;

    HierarchySelection selection_;
    GizmoMode current_gizmo_mode_ = GizmoMode::Select;
    bool use_local_space_ = false;
    bool snap_enabled_ = false;
    bool first_frame_ = true;

    // Dialog state
    bool show_new_project_dialog_ = false;
    int new_project_type_idx_ = 0;
    char new_project_name_[128] = "My Custom Car";
    char new_project_id_[128] = "mod.chassis.custom_car";
};

} // namespace eod::editor
