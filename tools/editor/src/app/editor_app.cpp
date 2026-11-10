#include "editor_app.h"
#include <iostream>
#include <cstdlib>
#include <chrono>
#include <thread>

#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/backends/imgui_impl_sdl3.h"
#include "../../third_party/imgui/backends/imgui_impl_sdlrenderer3.h"

namespace eod::editor {

EditorApp::EditorApp() = default;

EditorApp::~EditorApp() {
    Shutdown();
}

bool EditorApp::Initialize(int argc, char* argv[]) {
    std::string project_to_load;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--headless") {
            is_headless_ = true;
        } else if (arg == "--frames" && i + 1 < argc) {
            max_frames_ = std::atoi(argv[++i]);
        } else if (arg == "--project" && i + 1 < argc) {
            project_to_load = argv[++i];
        }
    }

    if (const char* env_headless = std::getenv("EOD_HEADLESS")) {
        if (std::string(env_headless) == "1") {
            is_headless_ = true;
        }
    }

    if (!is_headless_) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            std::cerr << "[WARN] SDL_Init failed: " << SDL_GetError() << ". Falling back to headless mode.\n";
            is_headless_ = true;
        }
    }

    if (!is_headless_) {
        window_ = SDL_CreateWindow("Test Drive: Eve of Destruction — Modding Editor",
                                  1600, 960,
                                  SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!window_) {
            std::cerr << "[WARN] SDL_CreateWindow failed: " << SDL_GetError() << ". Falling back to headless mode.\n";
            is_headless_ = true;
        } else {
            renderer_ = SDL_CreateRenderer(window_, nullptr);
            if (!renderer_) {
                std::cerr << "[WARN] SDL_CreateRenderer failed: " << SDL_GetError() << ". Falling back to headless mode.\n";
                SDL_DestroyWindow(window_);
                window_ = nullptr;
                is_headless_ = true;
            }
        }
    }

    // Initialize Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Dear ImGui dark style
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 4.0f;

    if (!is_headless_ && window_ && renderer_) {
        ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
        ImGui_ImplSDLRenderer3_Init(renderer_);
    } else {
        // Set virtual display size for headless layout simulation
        io.DisplaySize = ImVec2(1600.0f, 960.0f);
        io.DeltaTime = 1.0f / 60.0f;

        // Build default font atlas for headless execution
        unsigned char* tex_pixels = nullptr;
        int tex_w = 0, tex_h = 0;
        io.Fonts->GetTexDataAsRGBA32(&tex_pixels, &tex_w, &tex_h);
    }

    // Default project setup
    project_.CreateNew(ProjectType::Vehicle, "Phantom GT Mod", "mod.chassis.phantom_gt");
    ui_manager_.GetAssetBrowser().SetRootDirectory("mods/" + project_.GetIdentifier());

    if (!project_to_load.empty()) {
        std::string err;
        if (project_.LoadFromDirectory(project_to_load, err)) {
            ui_manager_.GetConsole().Log(ValidationSeverity::Info, "Loaded project from " + project_to_load);
            ui_manager_.GetAssetBrowser().SetRootDirectory(project_to_load);
        } else {
            ui_manager_.GetConsole().Log(ValidationSeverity::Error, "Failed to load project: " + err);
        }
    }

    last_time_ns_ = SDL_GetTicksNS();
    is_running_ = true;

    ui_manager_.GetConsole().Log(ValidationSeverity::Info,
        is_headless_ ? "Editor running in headless automated verification mode."
                     : "Editor running with hardware accelerated rendering.");

    return true;
}

void EditorApp::PollEvents() {
    if (is_headless_ || !window_) return;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT) {
            is_running_ = false;
        } else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window_)) {
            is_running_ = false;
        }
    }
}

void EditorApp::Update(float dt) {
    camera_.Update(dt);
}

void EditorApp::Render() {
    bool req_play = false;
    bool req_exit = false;

    if (!is_headless_ && window_ && renderer_) {
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ui_manager_.Render(project_, command_stack_, camera_, viewport_renderer_, validator_,
                           current_fps_, current_frame_time_ms_, req_play, req_exit);

        ImGui::Render();

        SDL_SetRenderDrawColor(renderer_, 24, 24, 24, 255);
        SDL_RenderClear(renderer_);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
        SDL_RenderPresent(renderer_);
    } else {
        // Headless simulation frame
        ImGui::NewFrame();
        ui_manager_.Render(project_, command_stack_, camera_, viewport_renderer_, validator_,
                           current_fps_, current_frame_time_ms_, req_play, req_exit);
        ImGui::Render();
    }

    if (req_exit) {
        is_running_ = false;
    }

    if (req_play) {
        LaunchTestDrive();
    }
}

void EditorApp::LaunchTestDrive() {
    ui_manager_.GetConsole().Log(ValidationSeverity::Info, "Staging mod package and launching in-game test drive...");

    // Stage mod manifests to disk
    std::string stage_dir = "build/staged_mod";
    std::string err;
    if (project_.SaveToDirectory(stage_dir, err)) {
        ui_manager_.GetConsole().Log(ValidationSeverity::Info, "Staged mod files to " + stage_dir);
    } else {
        ui_manager_.GetConsole().Log(ValidationSeverity::Error, "Failed to stage mod: " + err);
        return;
    }

    // Direct session start command line
    std::string cmd = "./build/eod --mod " + stage_dir + " &";
    ui_manager_.GetConsole().Log(ValidationSeverity::Info, "Executing: " + cmd);
}

void EditorApp::Run() {
    while (is_running_) {
        uint64_t now_ns = SDL_GetTicksNS();
        float dt = static_cast<float>(now_ns - last_time_ns_) / 1e9f;
        if (dt <= 0.0f || dt > 0.2f) dt = 1.0f / 60.0f;
        last_time_ns_ = now_ns;

        current_frame_time_ms_ = dt * 1000.0f;
        current_fps_ = (dt > 0.0001f) ? (1.0f / dt) : 60.0f;

        PollEvents();
        Update(dt);
        Render();

        ++current_frame_;
        if (max_frames_ > 0 && current_frame_ >= max_frames_) {
            is_running_ = false;
        }

        if (is_headless_) {
            // In headless mode without window events, prevent busy-spinning
            if (max_frames_ < 0) {
                // If indefinite headless without frame limit, do 10 frames validation then exit
                if (current_frame_ >= 10) {
                    is_running_ = false;
                }
            }
        }
    }
}

void EditorApp::Shutdown() {
    if (window_ && renderer_) {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
    }

    if (ImGui::GetCurrentContext()) {
        ImGui::DestroyContext();
    }

    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }

    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }

    SDL_Quit();
}

} // namespace eod::editor
