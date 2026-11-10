#pragma once

#include <functional>
#include <string>

namespace eod::editor {

class CommandStack;
class EditorProject;
class SchemaValidator;

class MenuBar {
public:
    MenuBar() = default;
    ~MenuBar() = default;

    void Render(CommandStack& command_stack,
                EditorProject& project,
                bool& out_request_new,
                bool& out_request_open,
                bool& out_request_save,
                bool& out_request_validate,
                bool& out_request_play,
                bool& out_request_exit);

    bool show_scene_hierarchy = true;
    bool show_properties = true;
    bool show_asset_browser = true;
    bool show_viewport = true;
    bool show_console = true;
    bool show_status_bar = true;
};

} // namespace eod::editor
