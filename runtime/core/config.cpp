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

std::string g_config_path = "eod.ini";

bool config_load(const std::string &path, Config &c) {
    if (&c == &g_config) g_config_path = path;
    FILE *f = fopen(path.c_str(), "r");
    const bool file_loaded = f != nullptr;
    int input_defaults = 0;  // [Input] InputDefaults: 2 = written after triggers became the default
    char line[1024];
    std::string section;
    while (f && fgets(line, sizeof line, f)) {
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
#ifdef _WIN32
            if (ieq(k, "Renderer")) c.renderer = ieq(v, "software") ? "software" : ieq(v, "warp") ? "warp" : "d3d11";
#else
            if (ieq(k, "Renderer")) c.renderer = ieq(v, "software") ? "software" : ieq(v, "vulkan") ? "vulkan" : ieq(v, "warp") ? "warp" : "d3d11";
#endif
            else if (ieq(k, "Detail") && (ieq(v,"original") || ieq(v,"high") || ieq(v,"maximum")))
                c.detail = ieq(v,"maximum") ? "maximum" : ieq(v,"high") ? "high" : "original";
            else if (ieq(k, "Adapter")) c.adapter = num(0, 15);
            else if (ieq(k, "Fullscreen")) c.fullscreen = to_bool(v);
            else if (ieq(k, "Width")) c.width = num(320, 16384);
            else if (ieq(k, "Height")) c.height = num(240, 16384);
            else if (ieq(k, "VSync")) c.vsync = to_bool(v);
            else if (ieq(k, "AspectRatio")) c.aspect = ieq(v, "16:9") ? "16:9" : ieq(v, "16:10") ? "16:10" : ieq(v, "stretch") ? "stretch" : ieq(v, "4:3") ? "4:3" : "auto";
#ifdef __APPLE__
            else if (ieq(k, "RenderScale")) c.render_scale = num(0, 6);  // 0 = auto (Vulkan)
#else
            else if (ieq(k, "RenderScale")) c.render_scale = num(1, 6);
#endif
            else if (ieq(k, "SmoothDisplay")) c.smooth_display = to_bool(v);
            else if (ieq(k, "FrameSkip")) c.frame_skip = to_bool(v);
            else if (ieq(k, "ShowFps")) c.show_fps = to_bool(v);
        } else if (ieq(section, "Audio")) {
            if (ieq(k, "Enabled")) c.audio = to_bool(v);
            else if (ieq(k, "Volume")) c.volume = num(0, 100);
        } else if (ieq(section, "Input")) {
            if (ieq(k, "Vibration")) c.vibration = to_bool(v);
            else if (ieq(k, "P1Deadzone")) c.stick_deadzone[0] = num(0, 50);
            else if (ieq(k, "P2Deadzone")) c.stick_deadzone[1] = num(0, 50);
            else if (ieq(k, "P1OuterDeadzone")) c.stick_outer[0] = num(50, 100);
            else if (ieq(k, "P2OuterDeadzone")) c.stick_outer[1] = num(50, 100);
            else if (ieq(k, "P1SteeringCurve")) c.stick_curve[0] = num(50, 300);
            else if (ieq(k, "P2SteeringCurve")) c.stick_curve[1] = num(50, 300);
            else if (ieq(k, "P1TriggerDeadzone")) c.trigger_deadzone[0] = num(0, 50);
            else if (ieq(k, "P2TriggerDeadzone")) c.trigger_deadzone[1] = num(0, 50);
            else if (ieq(k, "P1TriggerCurve")) c.trigger_curve[0] = num(50, 300);
            else if (ieq(k, "P2TriggerCurve")) c.trigger_curve[1] = num(50, 300);
#ifndef _WIN32  // SDL3 input options
            else if (ieq(k, "KeyboardPort")) c.keyboard_port = num(-1, 2);
            else if (ieq(k, "InputDefaults")) input_defaults = atoi(v.c_str());
            else if (ieq(k, "ControllerLayout") && (ieq(v, "original") || ieq(v, "modern")))
                c.controller_layout = ieq(v, "modern") ? "modern" : "original";
            else if (ieq(k, "KeyboardLayout") && (ieq(v, "original") || ieq(v, "racing")))
                c.keyboard_layout = ieq(v, "racing") ? "racing" : "original";
#endif
            for (int i = 0; i < KEY_COUNT; i++)
                if (ieq(k, kPadKeyNames[i])) c.keys[i] = v;
        } else if (ieq(section, "Gameplay") || ieq(section, "Cheats")) {
            if (ieq(k, "CarDurability")) c.car_durability = to_bool(v);
            else if (ieq(k, "ManualRecovery")) c.manual_recovery = to_bool(v);
            else if (ieq(k, "UnlockAll")) c.unlock_all = to_bool(v);
            else if (ieq(k, "CareerCheats")) c.career_cheats = to_bool(v);
            else if (ieq(k, "TurboBoost") || ieq(k, "Turbo")) c.cheat_turbo = to_bool(v);
            else if (ieq(k, "ChickenFight") || ieq(k, "Chickens")) c.cheat_chickens = to_bool(v);
            else if (ieq(k, "TurboScale")) c.turbo_scale = (float)atof(v.c_str());
            else if (ieq(k, "ChickenForceScale")) c.chicken_force_scale = (float)atof(v.c_str());
            else if (ieq(k, "ChickenRadiusScale")) c.chicken_radius_scale = (float)atof(v.c_str());
            else if (ieq(k, "BattleBots")) c.battle_bots = to_bool(v);
            else if (ieq(k, "SinglePlayerBattle") || ieq(k, "SPBattle")) c.sp_battle_mode = to_bool(v);
            else if (ieq(k, "Invincible") || ieq(k, "GodMode")) c.cheat_invincible = to_bool(v);
            else if (ieq(k, "CareerMoney")) c.career_money = to_bool(v);
            else if (ieq(k, "CareerRank")) c.career_rank = to_bool(v);
            else if (ieq(k, "OpenCareer")) c.career_open = to_bool(v);
            else if (ieq(k, "AllCars")) c.career_all_cars = to_bool(v);
            else if (ieq(k, "FreeSwap")) c.career_free_swap = to_bool(v);
            else if (ieq(k, "KillableChallengers")) c.career_killable = to_bool(v);
        } else if (ieq(section, "Mods")) {
            if (!k.empty()) {
                bool found = false;
                for (auto &m : c.mods) if (m.first == k) { m.second = to_bool(v); found = true; }
                if (!found) c.mods.emplace_back(k, to_bool(v));
            }
        } else if (ieq(section, "Career")) {
            if (ieq(k, "CarLooks")) c.car_looks = v;
        } else if (ieq(section, "System")) {
            if (ieq(k, "MemoryCard")) c.memcard_dir = v;
            else if (ieq(k, "DataPath")) c.data_dir = v;
            else if (ieq(k, "PauseInMenu")) c.pause_in_menu = to_bool(v);
        }
    }
    if (f) fclose(f);
#ifndef _WIN32
    // Older files stored ControllerLayout=original only because it was the default then; analog
    // trigger gas/brake is the default now (an explicit choice is kept once the file is rewritten).
    if (file_loaded && input_defaults < 2) c.controller_layout = "modern";
#endif

    // Environment variable overrides apply even on first launch without an ini.
    if (const char *e = getenv("EOD_UNLOCK_ALL")) c.unlock_all = to_bool(e);
    if (const char *e = getenv("EOD_CAREER_CHEATS")) c.career_cheats = to_bool(e);
    if (const char *e = getenv("EOD_TURBO")) c.cheat_turbo = to_bool(e);
    if (const char *e = getenv("EOD_CHICKENS")) c.cheat_chickens = to_bool(e);
    if (const char *e = getenv("EOD_INVINCIBLE")) c.cheat_invincible = to_bool(e);
    if (const char *e = getenv("EOD_GODMODE")) c.cheat_invincible = to_bool(e);
    if (const char *e = getenv("EOD_TURBO_SCALE")) c.turbo_scale = (float)atof(e);
    if (const char *e = getenv("EOD_CHICKEN_FORCE")) c.chicken_force_scale = (float)atof(e);
    if (const char *e = getenv("EOD_CHICKEN_RADIUS")) c.chicken_radius_scale = (float)atof(e);
    if (const char *e = getenv("EOD_BATTLE_BOTS")) c.battle_bots = to_bool(e);
    if (const char *e = getenv("EOD_SP_BATTLE")) c.sp_battle_mode = to_bool(e);
    if (const char *e = getenv("EOD_CAREER_MONEY")) c.career_money = to_bool(e);
    if (const char *e = getenv("EOD_CAREER_RANK")) c.career_rank = to_bool(e);
    if (const char *e = getenv("EOD_OPEN_CAREER")) c.career_open = to_bool(e);
    if (const char *e = getenv("EOD_ALL_CARS")) c.career_all_cars = to_bool(e);
    if (const char *e = getenv("EOD_FREE_SWAP")) c.career_free_swap = to_bool(e);
    if (const char *e = getenv("EOD_KILLABLE_CHALLENGERS")) c.career_killable = to_bool(e);

    return file_loaded;
}

bool config_save(const std::string &path, const Config &c) {
    FILE *f = fopen(path.c_str(), "w");
    if (!f) return false;
    fprintf(f, "; Test Drive: Eve of Destruction - PC port settings (written by eodsetup and the in-game menu)\n\n");
    fprintf(f, "[Video]\n");
    fprintf(f, "Detail=%s\n", c.detail.c_str());
#ifdef _WIN32
    fprintf(f, "Renderer=%s        ; d3d11, software or warp (Direct3D 11 without the graphics driver)\n", c.renderer.c_str());
#else
    fprintf(f, "Renderer=%s        ; vulkan or software\n", c.renderer.c_str());
#endif
    fprintf(f, "Adapter=%d\n", c.adapter);
    fprintf(f, "Fullscreen=%d\n", c.fullscreen ? 1 : 0);
    fprintf(f, "Width=%d\n", c.width);
    fprintf(f, "Height=%d\n", c.height);
    fprintf(f, "VSync=%d\n", c.vsync ? 1 : 0);
    fprintf(f, "AspectRatio=%s      ; auto (the game's Widescreen option), 4:3, 16:9 (sets the game's option) or stretch\n",
            c.aspect.c_str());
#ifdef __APPLE__
    fprintf(f, "RenderScale=%d        ; internal resolution: 0 = auto, 1 = 640x448 (PS2), 2 = 1280x896, ...\n", c.render_scale);
#else
    fprintf(f, "RenderScale=%d        ; internal resolution: 1 = 640x448 (PS2), 2 = 1280x896, ...\n", c.render_scale);
#endif
    fprintf(f, "SmoothDisplay=%d\n", c.smooth_display ? 1 : 0);
    fprintf(f, "FrameSkip=%d\n", c.frame_skip ? 1 : 0);
    fprintf(f, "ShowFps=%d\n", c.show_fps ? 1 : 0);
    fprintf(f, "\n[Audio]\n");
    fprintf(f, "Enabled=%d\n", c.audio ? 1 : 0);
    fprintf(f, "Volume=%d\n", c.volume);
    fprintf(f, "\n[Input]\n; keyboard keys for each PS2 button, separated by commas\n");
    for (int i = 0; i < KEY_COUNT; i++) fprintf(f, "%s=%s\n", kPadKeyNames[i], c.keys[i].c_str());
#ifndef _WIN32
    fprintf(f, "KeyboardPort=%d\n", c.keyboard_port);
#endif
    fprintf(f, "Vibration=%d\n", c.vibration ? 1 : 0);
    for (int p = 0; p < 2; p++) {
        fprintf(f, "P%dDeadzone=%d        ; left stick inner dead zone, percent (input mapping only)\n", p + 1, c.stick_deadzone[p]);
        fprintf(f, "P%dOuterDeadzone=%d  ; stick travel that already counts as full lock, percent\n", p + 1, c.stick_outer[p]);
        fprintf(f, "P%dSteeringCurve=%d  ; 100 = linear, higher = finer near the centre\n", p + 1, c.stick_curve[p]);
        fprintf(f, "P%dTriggerDeadzone=%d ; analog gas/brake trigger dead zone, percent\n", p + 1, c.trigger_deadzone[p]);
        fprintf(f, "P%dTriggerCurve=%d   ; 100 = linear trigger travel\n", p + 1, c.trigger_curve[p]);
    }
#ifndef _WIN32
    fprintf(f, "ControllerLayout=%s     ; modern: RT/LT analog gas/brake, stick clicks = L2/R2; original: PS2 L2/R2\n", c.controller_layout.c_str());
    fprintf(f, "InputDefaults=2\n");
    fprintf(f, "KeyboardLayout=%s\n", c.keyboard_layout.c_str());
#endif
    fprintf(f, "\n[Gameplay]\n");
    fprintf(f, "CarDurability=%d      ; 1: each car's durability from its files (damage_scale) on top of its weight\n",
            c.car_durability ? 1 : 0);
    fprintf(f, "ManualRecovery=%d     ; 1: human countdown recovery disabled; SELECT retained\n", c.manual_recovery ? 1 : 0);
    fprintf(f, "\n[Cheats]\n");
    fprintf(f, "UnlockAll=%d          ; 1: unlock all career tracks, vehicles, dares, and feat prizes\n", c.unlock_all ? 1 : 0);
    fprintf(f, "CareerCheats=%d       ; 1: allow cheat feats to function in career mode\n", c.career_cheats ? 1 : 0);
    fprintf(f, "TurboBoost=%d         ; 1: enable turbo boost on L2\n", c.cheat_turbo ? 1 : 0);
    fprintf(f, "ChickenFight=%d       ; 1: enable explosive chicken shooting on L2\n", c.cheat_chickens ? 1 : 0);
    fprintf(f, "Invincible=%d        ; 1: suppress damage/deformation for human players only; AI remain vulnerable\n", c.cheat_invincible ? 1 : 0);
    fprintf(f, "TurboScale=%.2f       ; multiplier for turbo thrust and top speed cap (default 1.0)\n", c.turbo_scale);
    fprintf(f, "ChickenForceScale=%.2f; multiplier for chicken explosion blast impulse (default 1.0)\n", c.chicken_force_scale);
    fprintf(f, "ChickenRadiusScale=%.2f; multiplier for chicken explosion blast radius (default 1.0)\n", c.chicken_radius_scale);
    fprintf(f, "BattleBots=%d         ; 1: fill Battle to four participants; restart event to apply\n", c.battle_bots ? 1 : 0);
    fprintf(f, "SinglePlayerBattle=%d ; 1: inject Battle Mode into single player action menus with AI\n", c.sp_battle_mode ? 1 : 0);
    fprintf(f, "CareerMoney=%d        ; 1: Career money never drops below $10,000 (winnings still add)\n", c.career_money ? 1 : 0);
    fprintf(f, "CareerRank=%d         ; 1: Career rank held at the top (shown 1st) without ending the Career\n", c.career_rank ? 1 : 0);
    fprintf(f, "OpenCareer=%d         ; 1: every Career shop, upgrade, time challenge, Troy's Place and Eve open from the start\n", c.career_open ? 1 : 0);
    fprintf(f, "AllCars=%d            ; 1: the Career dealer pages through every car, Phantom and the Cup car included\n", c.career_all_cars ? 1 : 0);
    fprintf(f, "FreeSwap=%d           ; 1: selling or trading a Career car refunds its full price\n", c.career_free_swap ? 1 : 0);
    fprintf(f, "KillableChallengers=%d; 1: Highway and Malt Shop challengers in Career free roam can be wrecked\n", c.career_killable ? 1 : 0);
    fprintf(f, "\n[Mods]\n; MODS tab: <mod folder>=1 (on) or 0 (off), in load order; a mod not listed is on\n");
    for (const auto &m : c.mods) {
        // ids the ini syntax cannot carry are simply not remembered
        if (m.first.empty() || m.first.find_first_of("=;[\r\n") != std::string::npos) continue;
        fprintf(f, "%s=%d\n", m.first.c_str(), m.second ? 1 : 0);
    }
    fprintf(f, "\n[Career]\n; career cars shown as another car or paint job (career vehicle:vehicle, set by the save editor)\n");
    fprintf(f, "CarLooks=%s\n", c.car_looks.c_str());
    fprintf(f, "\n[System]\n");
    fprintf(f, "MemoryCard=%s\n", c.memcard_dir.c_str());
    fprintf(f, "DataPath=%s\n", c.data_dir.c_str());
    fprintf(f, "PauseInMenu=%d        ; the game stops while the in-game menu (F1) is open\n", c.pause_in_menu ? 1 : 0);
    fclose(f);
    return true;
}
