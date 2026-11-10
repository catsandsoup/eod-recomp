#pragma once

#include <string>

namespace eod::editor {

class EditorProject;

enum class SelectionType {
    None,
    ProjectRoot,
    ComponentGroup,
    Socket,
    SceneryProp
};

struct HierarchySelection {
    SelectionType type = SelectionType::ProjectRoot;
    int index = -1;
    std::string name;
};

class SceneHierarchy {
public:
    SceneHierarchy() = default;
    ~SceneHierarchy() = default;

    void Render(EditorProject& project, HierarchySelection& selection);
};

} // namespace eod::editor
