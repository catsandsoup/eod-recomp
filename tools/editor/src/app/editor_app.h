#pragma once

#include <string>
#include <vector>
#include <memory>
#include <SDL3/SDL.h>

#include "../project/project.h"
#include "../undo/command_stack.h"
#include "../graphics/camera.h"
#include "../graphics/viewport_renderer.h"
#include "../validator/schema_validator.h"
#include "../ui/ui_manager.h"

namespace eod::editor {

class EditorApp {
public:
    EditorApp();
    ~EditorApp();

    bool Initialize(int argc, char* argv[]);
    void Run();
    void Shutdown();

    bool IsRunning() const { return is_running_; }
    void RequestExit() { is_running_ = false; }

    EditorProject& GetProject() { return project_; }
    CommandStack& GetCommandStack() { return command_stack_; }
    EditorCamera& GetCamera() { return camera_; }
    ViewportRenderer& GetViewportRenderer() { return viewport_renderer_; }
    SchemaValidator& GetValidator() { return validator_; }
    UIManager& GetUIManager() { return ui_manager_; }

private:
    void PollEvents();
    void Update(float dt);
    void Render();
    void LaunchTestDrive();

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;

    EditorProject project_;
    CommandStack command_stack_;
    EditorCamera camera_;
    ViewportRenderer viewport_renderer_;
    SchemaValidator validator_;
    UIManager ui_manager_;

    bool is_running_ = false;
    bool is_headless_ = false;
    int max_frames_ = -1;
    int current_frame_ = 0;

    uint64_t last_time_ns_ = 0;
    float current_fps_ = 60.0f;
    float current_frame_time_ms_ = 16.6f;
};

} // namespace eod::editor
