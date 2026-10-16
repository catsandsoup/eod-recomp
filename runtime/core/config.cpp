// config.cpp - eod.ini reading and writing.
#include "config.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

Config g_config;

const char *const kPadKeyNames[KEY_COUNT] = {"Up", "Down", "Left", "Right", "Cross", "Circle", "Square", "Triangle",
                                             "L1", "R1", "L2", "R2", "L3", "R3", "Start", "Select"};
const char *const kPadKeyLabels[KEY_COUNT] = {"Up / steer", "Down", "Left / steer left", "Right / steer right",
                                              "Cross (X)", "Circle", "Square", "Triangle",
                                              "L1", "R1", "L2", "R2", "L3", "R3", "Start", "Select"};

Config::Config() {
    static const char *const defaults[KEY_COUNT] = {"Up", "Down", "Left", "Right", "X,Space", "C", "Z", "V",
                                                    "Q", "E", "A,1", "D,3", "", "", "Enter", "Backspace"};
    for (int i = 0; i < KEY_COUNT; i++) keys[i] = defaults[i];
}

namespace {

struct KeyName {
    const char *name;
    int code;
};
const KeyName kKeys[] = {
    {"Backspace", 0x08}, {"Tab", 0x09}, {"Enter", 0x0D}, {"Shift", 0x10}, {"Ctrl", 0x11}, {"Alt", 0x12},
    {"Pause", 0x13}, {"CapsLock", 0x14}, {"Escape", 0x1B}, {"Space", 0x20}, {"PageUp", 0x21}, {"PageDown", 0x22},
    {"End", 0x23}, {"Home", 0x24}, {"Left", 0x25}, {"Up", 0x26}, {"Right", 0x27}, {"Down", 0x28},
    {"Insert", 0x2D}, {"Delete", 0x2E}, {"Numpad0", 0x60}, {"Numpad1", 0x61}, {"Numpad2", 0x62},
    {"Numpad3", 0x63}, {"Numpad4", 0x64}, {"Numpad5", 0x65}, {"Numpad6", 0x66}, {"Numpad7", 0x67},
    {"Numpad8", 0x68}, {"Numpad9", 0x69}, {"Multiply", 0x6A}, {"Add", 0x6B}, {"Subtract", 0x6D},
    {"Decimal", 0x6E}, {"Divide", 0x6F}, {"F1", 0x70}, {"F2", 0x71}, {"F3", 0x72}, {"F4", 0x73}, {"F5", 0x74},
    {"F6", 0x75}, {"F7", 0x76}, {"F8", 0x77}, {"F9", 0x78}, {"F10", 0x79}, {"F12", 0x7B},
    {"Semicolon", 0xBA}, {"Equals", 0xBB}, {"Comma", 0xBC}, {"Minus", 0xBD}, {"Period", 0xBE}, {"Slash", 0xBF},
    {"Backquote", 0xC0}, {"LeftBracket", 0xDB}, {"Backslash", 0xDC}, {"RightBracket", 0xDD}, {"Quote", 0xDE},
};

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool ieq(const std::string &a, const char *b) {
    size_t n = strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

bool to_bool(const std::string &v) { return ieq(v, "1") || ieq(v, "true") || ieq(v, "yes") || ieq(v, "on"); }

}  // namespace

int key_code(const std::string &name0) {
    std::string name = trim(name0);
    if (name.size() == 1) {
        char c = (char)toupper((unsigned char)name[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    for (const KeyName &k : kKeys)
        if (ieq(name, k.name)) return k.code;
    return 0;
}

std::string key_name(int code) {
    if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) return std::string(1, (char)code);
    for (const KeyName &k : kKeys)
        if (k.code == code) return k.name;
    return "";
}

bool config_load(const std::string &path, Config &c) {
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return false;
    char line[1024];
    std::string section;
    while (fgets(line, sizeof line, f)) {
        std::string l = line;
        size_t sc = l.find(';');
        if (sc != std::string::npos) l = l.substr(0, sc);
        l = trim(l);
        if (l.empty()) continue;
        if (l[0] == '[') {
            section = trim(l.substr(1, l.find(']') == std::string::npos ? std::string::npos : l.find(']') - 1));
            continue;
        }
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
        auto num = [&](int lo, int hi) {
            int n = atoi(v.c_str());
            return n < lo ? lo : n > hi ? hi : n;
        };
        if (ieq(section, "Video")) {
            if (ieq(k, "Renderer")) c.renderer = ieq(v, "software") ? "software" : ieq(v, "vulkan") ? "vulkan" : ieq(v, "warp") ? "warp" : "d3d11";
            else if (ieq(k, "Adapter")) c.adapter = num(0, 15);
            else if (ieq(k, "Fullscreen")) c.fullscreen = to_bool(v);
            else if (ieq(k, "Width")) c.width = num(320, 16384);
            else if (ieq(k, "Height")) c.height = num(240, 16384);
            else if (ieq(k, "VSync")) c.vsync = to_bool(v);
            else if (ieq(k, "AspectRatio")) c.aspect = ieq(v, "16:9") ? "16:9" : ieq(v, "stretch") ? "stretch" : "4:3";
            else if (ieq(k, "RenderScale")) c.render_scale = num(0, 6);
            else if (ieq(k, "SmoothDisplay")) c.smooth_display = to_bool(v);
            else if (ieq(k, "FrameSkip")) c.frame_skip = to_bool(v);
        } else if (ieq(section, "Audio")) {
            if (ieq(k, "Enabled")) c.audio = to_bool(v);
            else if (ieq(k, "Volume")) c.volume = num(0, 100);
        } else if (ieq(section, "Input")) {
            if (ieq(k, "KeyboardPort")) c.keyboard_port = num(0, 2);
            else if (ieq(k, "Vibration")) c.vibration = to_bool(v);
            for (int i = 0; i < KEY_COUNT; i++)
                if (ieq(k, kPadKeyNames[i])) c.keys[i] = v;
        } else if (ieq(section, "System")) {
            if (ieq(k, "MemoryCard")) c.memcard_dir = v;
            else if (ieq(k, "DataPath")) c.data_dir = v;
        }
    }
    fclose(f);
    return true;
}

bool config_save(const std::string &path, const Config &c) {
    FILE *f = fopen(path.c_str(), "w");
    if (!f) return false;
    fprintf(f, "; Test Drive: Eve of Destruction - PC port settings (written by eodsetup)\n\n");
    fprintf(f, "[Video]\n");
    fprintf(f, "Renderer=%s        ; vulkan, d3d11, software or warp (Direct3D 11 without the graphics driver)\n", c.renderer.c_str());
    fprintf(f, "Adapter=%d\n", c.adapter);
    fprintf(f, "Fullscreen=%d\n", c.fullscreen ? 1 : 0);
    fprintf(f, "Width=%d\n", c.width);
    fprintf(f, "Height=%d\n", c.height);
    fprintf(f, "VSync=%d\n", c.vsync ? 1 : 0);
    fprintf(f, "AspectRatio=%s      ; 4:3, 16:9 or stretch\n", c.aspect.c_str());
    fprintf(f, "RenderScale=%d        ; internal resolution: 0 = auto, 1 = 640x448 (PS2), 2 = 1280x896, ...\n", c.render_scale);
    fprintf(f, "SmoothDisplay=%d\n", c.smooth_display ? 1 : 0);
    fprintf(f, "FrameSkip=%d\n", c.frame_skip ? 1 : 0);
    fprintf(f, "\n[Audio]\n");
    fprintf(f, "Enabled=%d\n", c.audio ? 1 : 0);
    fprintf(f, "Volume=%d\n", c.volume);
    fprintf(f, "\n[Input]\n; keyboard keys for each PS2 button, separated by commas\n");
    for (int i = 0; i < KEY_COUNT; i++) fprintf(f, "%s=%s\n", kPadKeyNames[i], c.keys[i].c_str());
    fprintf(f, "KeyboardPort=%d\n", c.keyboard_port);
    fprintf(f, "Vibration=%d\n", c.vibration ? 1 : 0);
    fprintf(f, "\n[System]\n");
    fprintf(f, "MemoryCard=%s\n", c.memcard_dir.c_str());
    fprintf(f, "DataPath=%s\n", c.data_dir.c_str());
    fclose(f);
    return true;
}
