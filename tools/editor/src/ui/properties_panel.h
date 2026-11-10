#pragma once

#include "scene_hierarchy.h"

namespace eod::editor {

class EditorProject;
class CommandStack;

class PropertiesPanel {
public:
    PropertiesPanel() = default;
    ~PropertiesPanel() = default;

    void Render(EditorProject& project,
                CommandStack& command_stack,
                HierarchySelection& selection);
};

} // namespace eod::editor
