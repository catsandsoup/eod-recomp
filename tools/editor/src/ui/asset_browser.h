#pragma once

#include <string>
#include <filesystem>

namespace eod::editor {

class AssetBrowser {
public:
    AssetBrowser() = default;
    ~AssetBrowser() = default;

    void SetRootDirectory(const std::string& dir) { root_dir_ = dir; current_dir_ = dir; }
    void Render();

    const std::string& GetSelectedPath() const { return selected_path_; }

private:
    std::string root_dir_;
    std::string current_dir_;
    std::string selected_path_;
};

} // namespace eod::editor
