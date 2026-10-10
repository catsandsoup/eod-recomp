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
    std::string renderer = "d3d11";  // d3d11 | software
    int adapter = 0;                 // DXGI adapter index
    bool fullscreen = false;         // borderless window covering the monitor
    int width = 1280, height = 960;  // window client size (windowed mode)
    bool vsync = true;
    std::string aspect = "4:3";      // 4:3 | 16:9 | stretch
    int render_scale = 2;            // internal resolution multiplier (d3d11 renderer)
    bool smooth_display = true;      // bilinear filtering when scaling the image to the window
    bool frame_skip = true;          // drop frames when drawing can't keep up (the game keeps its speed)
    // [Audio]
    bool audio = true;
    int volume = 100;  // percent
    // [Input] keyboard bindings: key names separated by commas (see key_code)
    std::string keys[KEY_COUNT];
    bool vibration = true;
    // [System]
    std::string memcard_dir;  // empty: <game folder>/memcard
    std::string data_dir;     // empty: the executable's folder

    Config();
};

extern Config g_config;

// Reads path into g_config (missing file or keys keep the defaults).
bool config_load(const std::string &path, Config &c = g_config);
bool config_save(const std::string &path, const Config &c = g_config);

extern const char *const kPadKeyNames[KEY_COUNT];   // ini keys ("Up", "Cross", ...)
extern const char *const kPadKeyLabels[KEY_COUNT];  // for the setup UI

// Keyboard key names <-> Windows virtual-key codes (0 if unknown).
int key_code(const std::string &name);
std::string key_name(int code);
