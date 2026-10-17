// config.h - user settings (eod.ini next to the executable), shared by the
// game and the setup program (eodsetup).
#pragma once
#include <string>

enum PadKey {
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_CROSS, KEY_CIRCLE, KEY_SQUARE, KEY_TRIANGLE,
    KEY_L1, KEY_R1, KEY_L2, KEY_R2, KEY_L3, KEY_R3,
    KEY_START, KEY_SELECT,
    KEY_COUNT
};

struct Config {
    // [Video]
#if defined(__APPLE__) && defined(EOD_VULKAN)
    std::string renderer = "vulkan";  // vulkan | software (macOS)
#elif defined(__APPLE__)
    std::string renderer = "software";
#else
    std::string renderer = "d3d11";  // d3d11 | software | warp (Direct3D 11 in software)
#endif
    int adapter = 0;                 // DXGI adapter index
    bool fullscreen = false;         // borderless window covering the monitor
    int width = 1280, height = 960;  // window client size (windowed mode; SDL logical points on macOS)
    bool vsync = true;
    std::string aspect = "auto";     // auto (the game's own 4:3 / 16:9 option) | 4:3 | 16:9 | stretch
#ifdef __APPLE__
    int render_scale = 0;  // 0 = auto (Vulkan: enough GS samples to cover the window pixel height)
#else
    int render_scale = 2;            // internal resolution multiplier (d3d11 renderer)
#endif
    bool smooth_display = true;      // bilinear filtering when scaling the image to the window
    bool frame_skip = true;          // drop frames when drawing can't keep up (the game keeps its speed)
    bool show_fps = false;           // frame rate in a corner (d3d11 renderer)
    // [Audio]
    bool audio = true;
    int volume = 100;  // percent
    // [Input] keyboard bindings: key names separated by commas (see key_code)
    std::string keys[KEY_COUNT];
    bool vibration = true;
#ifndef _WIN32  // SDL3 input options
    int keyboard_port = -1;          // -1: automatic, 0: disabled, 1 or 2: player port
    std::string keyboard_layout = "racing"; // racing (context-aware WASD/arrows) | original (Key_* bindings)
    std::string controller_layout = "original"; // original | modern (RT Cross, LT Square pressure)
#endif
    // [System]
    std::string memcard_dir;  // empty: <game folder>/memcard
    std::string data_dir;     // empty: the executable's folder
    bool pause_in_menu = true;  // the game stops while the in-game menu (F1) is open
    // [Gameplay] each car's damage_scale (from its file; the game ignores it,
    // its menu durability ratings follow it) multiplies the damage it takes
    bool car_durability = false;
    // [Career] career cars drawn as another vehicle: "845:790,859:68" (career
    // vehicle : the vehicle it looks like; set by the save editor)
    std::string car_looks;

    Config();
};

extern Config g_config;
extern std::string g_config_path;  // the file config_load read (where the in-game menu saves)

// Reads path into g_config (missing file or keys keep the defaults).
bool config_load(const std::string &path, Config &c = g_config);
bool config_save(const std::string &path, const Config &c = g_config);

extern const char *const kPadKeyNames[KEY_COUNT];   // ini keys ("Up", "Cross", ...)
extern const char *const kPadKeyLabels[KEY_COUNT];  // for the setup UI

// Keyboard key names <-> Windows virtual-key codes (0 if unknown).
int key_code(const std::string &name);
std::string key_name(int code);
