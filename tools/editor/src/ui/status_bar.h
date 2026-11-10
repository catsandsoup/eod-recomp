#pragma once

#include <string>

namespace eod::editor {

class EditorProject;
class CommandStack;

class StatusBar {
public:
    StatusBar() = default;
    ~StatusBar() = default;

    void Render(const EditorProject& project,
                const CommandStack& command_stack,
                float fps,
                float frame_time_ms);
};

} // namespace eod::editor
