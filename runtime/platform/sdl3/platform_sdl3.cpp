#include "../display_rect.h"
#include "../presentation.h"
// platform_sdl3.cpp - window, audio and controllers through SDL3 (macOS;
// also builds on Linux). Time, fibers, files and the ticker stay in
// posix/platform_posix.cpp.
//
// The window shows the software GS output through an SDL renderer (Metal on
// macOS). Controllers and the two player ports are in sdl_input.cpp: the
// keyboard sits on EOD_KEYBOARD_PORT (default player 1, "none" to disable)
// and game controllers fill the ports in the order they are plugged in,
// hot-plug included, so "keyboard + pad" and "two pads" both give two
// independent players. Xbox 360 wired pads (Rock Candy and other third-party
// ones) have no driver on macOS; SDL reaches them through its HIDAPI driver
// over libusb, which needs the SDL3 built by EOD_BUNDLED_SDL.
//
// EOD_HEADLESS=1 (or --headless): no window and no audio device; frames are
// kept for screenshots. EOD_MUTE=1: no audio device.
#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../core/config.h"
#include "audio_gain.h"
#ifdef __APPLE__
#include "../../app/macos_app.h"
#ifndef EOD_DISPLAY_BACKEND_TEST
#include "../../app/microphone_permission.h"
#endif
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <spawn.h>
extern char **environ;
#endif
#include "../../gs/gs_hw.h"

#include "../../core/memory.h"
#include "../keyboard_input.h"
#include "../pad_script.h"
#include "../platform.h"
#include "sdl_input.h"
#ifndef EOD_DISPLAY_BACKEND_TEST
#include "../../ui/frontend.h"
#include "../../ui/overlay.h"
#else  // the display composition test links this file without the front end
inline bool overlay_key(int, bool) { return false; }
inline void overlay_char(unsigned) {}
inline void overlay_paste(const char *) {}
inline std::string overlay_copy_text() { return {}; }
inline void overlay_mouse_move(float, float) {}
inline void overlay_mouse_button(bool) {}
inline void overlay_mouse_wheel(float) {}
inline bool overlay_menu_open() { return false; }
inline bool overlay_captures_input() { return false; }
namespace frontend { inline uint8_t shape_axis(uint8_t v, int, int, int) { return v; }
inline uint8_t shape_trigger(uint8_t v, int, int) { return v; } }
#endif

namespace {

PlatformConfig g_cfg;
bool g_headless;
bool g_vulkan_window;
SDL_Window *g_window;
std::thread::id g_main_thread;  // the thread that ran platform_init (the EE runs on it)
SDL_Renderer *g_renderer;
SDL_Texture *g_texture;
int g_tex_w, g_tex_h;
SDL_AudioStream *g_audio;
SDL_AudioStream *g_mic;
float g_volume = 1.0f;

void update_audio_gain() {
    g_volume = sdl_output_gain(g_config);
    if (g_audio) SDL_SetAudioStreamGain(g_audio, g_volume);
}

std::vector<uint32_t> g_last_frame;
int g_last_w, g_last_h;
std::string g_hud_banner_text;
uint64_t g_hud_banner_until = 0;
bool g_mod_overlay_visible = false;
uint64_t g_haptic_pulse_until = 0;

// ---------------------------------------------------------------- input
using sdl_input::kPorts;

std::vector<keyboard_input::Binding> g_key_map;
keyboard_input::Mapper g_keyboard;
std::vector<std::pair<int, SDL_Scancode>> g_keyboard_scancodes;
uint64_t g_key_event_ns;
unsigned g_key_timing_lines;
bool g_key_timing_pending;
bool input_timing() { static const bool enabled = getenv("EOD_INPUT_TIMING") != nullptr; return enabled; }
void time_key_event(const SDL_KeyboardEvent &event) {
    if (!input_timing() || event.repeat) return;
    g_key_event_ns = event.timestamp;
    g_key_timing_pending = true;
    if (g_key_timing_lines < 256) {
        uint64_t now = SDL_GetTicksNS();
        fprintf(stderr, "[input-timing] key=%d %s event_age_us=%llu\n", int(event.scancode), event.down ? "down" : "up",
                (unsigned long long)(now >= event.timestamp ? (now - event.timestamp) / 1000 : 0));
        ++g_key_timing_lines;
    }
}

uint16_t pad_key_button(int k) {
    static const uint16_t b[KEY_COUNT] = {PAD_UP,  PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_CROSS,    PAD_CIRCLE,
                                          PAD_SQUARE, PAD_TRIANGLE, PAD_L1, PAD_R1, PAD_L2, PAD_R2,
                                          PAD_L3,  PAD_R3,   PAD_START, PAD_SELECT};
    return b[k];
}

// Windows virtual-key codes (what eod.ini's key names map to, config.cpp) to
// SDL scancodes, so the same eod.ini works on every platform.
SDL_Scancode vk_to_scancode(int vk) {
    if (vk >= 'A' && vk <= 'Z') return (SDL_Scancode)(SDL_SCANCODE_A + (vk - 'A'));
    if (vk >= '1' && vk <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + (vk - '1'));
    if (vk == '0') return SDL_SCANCODE_0;
    if (vk >= 0x70 && vk <= 0x7B) return (SDL_Scancode)(SDL_SCANCODE_F1 + (vk - 0x70));
    if (vk >= 0x61 && vk <= 0x69) return (SDL_Scancode)(SDL_SCANCODE_KP_1 + (vk - 0x61));
    switch (vk) {
    case 0x08: return SDL_SCANCODE_BACKSPACE;
    case 0x09: return SDL_SCANCODE_TAB;
    case 0x0D: return SDL_SCANCODE_RETURN;
    case 0x10: return SDL_SCANCODE_LSHIFT;
    case 0x11: return SDL_SCANCODE_LCTRL;
    case 0x12: return SDL_SCANCODE_LALT;
    case 0x13: return SDL_SCANCODE_PAUSE;
    case 0x14: return SDL_SCANCODE_CAPSLOCK;
    case 0x1B: return SDL_SCANCODE_ESCAPE;
    case 0x20: return SDL_SCANCODE_SPACE;
    case 0x21: return SDL_SCANCODE_PAGEUP;
    case 0x22: return SDL_SCANCODE_PAGEDOWN;
    case 0x23: return SDL_SCANCODE_END;
    case 0x24: return SDL_SCANCODE_HOME;
    case 0x25: return SDL_SCANCODE_LEFT;
    case 0x26: return SDL_SCANCODE_UP;
    case 0x27: return SDL_SCANCODE_RIGHT;
    case 0x28: return SDL_SCANCODE_DOWN;
    case 0x2D: return SDL_SCANCODE_INSERT;
    case 0x2E: return SDL_SCANCODE_DELETE;
    case 0x60: return SDL_SCANCODE_KP_0;
    case 0x6A: return SDL_SCANCODE_KP_MULTIPLY;
    case 0x6B: return SDL_SCANCODE_KP_PLUS;
    case 0x6D: return SDL_SCANCODE_KP_MINUS;
    case 0x6E: return SDL_SCANCODE_KP_PERIOD;
    case 0x6F: return SDL_SCANCODE_KP_DIVIDE;
    case 0xBA: return SDL_SCANCODE_SEMICOLON;
    case 0xBB: return SDL_SCANCODE_EQUALS;
    case 0xBC: return SDL_SCANCODE_COMMA;
    case 0xBD: return SDL_SCANCODE_MINUS;
    case 0xBE: return SDL_SCANCODE_PERIOD;
    case 0xBF: return SDL_SCANCODE_SLASH;
    case 0xC0: return SDL_SCANCODE_GRAVE;
    case 0xDB: return SDL_SCANCODE_LEFTBRACKET;
    case 0xDC: return SDL_SCANCODE_BACKSLASH;
    case 0xDD: return SDL_SCANCODE_RIGHTBRACKET;
    case 0xDE: return SDL_SCANCODE_APOSTROPHE;
    }
    return SDL_SCANCODE_UNKNOWN;
}

int scancode_to_vk(SDL_Scancode sc) {
    for (int vk = 1; vk < 256; ++vk)
        if (vk_to_scancode(vk) == sc) return vk;
    if (sc == SDL_SCANCODE_RSHIFT) return 0x10;
    if (sc == SDL_SCANCODE_KP_ENTER) return 0x0D;
    return 0;
}

// Printable character of a key for the front end's text fields (codes, addresses).
char key_char(int vk, bool shift) {
    if (vk >= 'A' && vk <= 'Z') return shift ? (char)vk : (char)(vk - 'A' + 'a');
    if (vk >= '0' && vk <= '9') return vk == '0' || !shift ? (char)vk : 0;
    switch (vk) {
    case 0xBE: return '.';
    case 0xBD: return shift ? '_' : '-';
    case 0xBA: return shift ? ':' : ';';
    case 0xBF: return '/';
    case 0x20: return ' ';
    default: return 0;
    }
}

void build_key_map() {
    g_key_map.clear();
    g_keyboard = {};
    g_keyboard_scancodes.clear();
    for (int vk = 1; vk < 256; ++vk) {
        auto sc = vk_to_scancode(vk);
        if (sc != SDL_SCANCODE_UNKNOWN) g_keyboard_scancodes.push_back({vk, sc});
    }
    for (int k = 0; k < KEY_COUNT; k++) {
        const std::string &list = g_config.keys[k];
        size_t pos = 0;
        while (pos <= list.size()) {
            size_t comma = list.find(',', pos);
            std::string name = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            int vk = key_code(name);
            if (vk > 0 && vk < 256 && vk_to_scancode(vk) != SDL_SCANCODE_UNKNOWN)
                g_key_map.push_back({vk, pad_key_button(k)});
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
}

keyboard_input::Context keyboard_context() {
    auto word = [](uint32_t address) { uint32_t v = 0; if (g_ram) memcpy(&v, g_ram + address, sizeof v); return v; };
    uint32_t title = word(0x003D7BB8), alternate = word(0x003D7BBC);
    uint32_t race = word(0x00437BB8), modal = word(0x004A1AD4);
    auto ctx = keyboard_input::context(title, alternate, race, modal);
    static int last = -1;
    if (sdl_input::log_enabled() && int(ctx) != last) {
        fprintf(stderr, "[input] keyboard context=%s title=%08x alt=%08x game_loop=%u modal=%08x\n",
                ctx == keyboard_input::Context::Title ? "title" : ctx == keyboard_input::Context::Driving ? "driving" : "menu",
                title, alternate, race, modal);
        last = int(ctx);
    }
    return ctx;
}

void read_keyboard(HostPadState *out, keyboard_input::Context ctx) {
    out->connected = true;
    if (g_headless) return;
    const bool *physical = SDL_GetKeyboardState(nullptr);
    keyboard_input::Keys keys{};
    for (const auto &m : g_keyboard_scancodes) keys[m.first] = physical[m.second];
    *out = g_keyboard.read(keys, ctx, g_config.keyboard_layout == "racing", g_key_map,
        g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS));
    if (g_key_timing_pending && g_key_timing_lines < 256) {
        uint64_t now = SDL_GetTicksNS();
        fprintf(stderr, "[input-timing] keyboard_sample buttons=%04x event_age_us=%llu\n", out->buttons,
                (unsigned long long)(now >= g_key_event_ns ? (now - g_key_event_ns) / 1000 : 0));
        ++g_key_timing_lines;
    }
    g_key_timing_pending = false;
}

}  // namespace

void platform_configure_display(const PlatformConfig &cfg) {
    g_cfg=cfg;
    const char *headless=getenv("EOD_HEADLESS");
    g_headless=cfg.headless || (headless && headless[0]=='1');
}

bool platform_init(const PlatformConfig &cfg) {
    g_cfg = cfg;
    g_main_thread = std::this_thread::get_id();
    const char *h = getenv("EOD_HEADLESS");
    g_headless = cfg.headless || (h && h[0] == '1');
    const char *keyboard_override = getenv("EOD_KEYBOARD_PORT");
    const bool explicit_keyboard_port = keyboard_override && keyboard_override[0];
    // Saved KeyboardPort never takes a port away from the first/second controller.
    sdl_input::set_keyboard_port(explicit_keyboard_port
        ? sdl_input::keyboard_port_from_env() : g_config.keyboard_port - 1, explicit_keyboard_port);
    sdl_input::set_controller_layout(g_config.controller_layout == "modern");
    sdl_input::set_hints();
    SDL_InitFlags flags = SDL_INIT_GAMEPAD | SDL_INIT_EVENTS;
    if (!g_headless) flags |= SDL_INIT_VIDEO;
    if (!SDL_Init(flags)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    build_key_map();
    update_audio_gain();

    if (!g_headless) {
        SDL_WindowFlags wf = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifdef EOD_VULKAN
        if (g_config.renderer == "vulkan") { wf |= SDL_WINDOW_VULKAN; g_vulkan_window=true; }
#endif
        if (cfg.fullscreen) wf |= SDL_WINDOW_FULLSCREEN;
        g_window = SDL_CreateWindow(cfg.title, cfg.width, cfg.height, wf);
        if (!g_window) {
            fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
            return false;
        }
        if (!g_vulkan_window) {
        g_renderer = SDL_CreateRenderer(g_window, nullptr);
        if (!g_renderer) {
            fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
            return false;
        }
        SDL_SetRenderVSync(g_renderer, g_config.vsync ? 1 : 0);
        fprintf(stderr, "[video] SDL renderer: %s\n", SDL_GetRendererName(g_renderer));
        }
    }

    int kb = sdl_input::keyboard_port();
    fprintf(stderr, "[input] keyboard: %s\n", kb < 0 ? "off" : kb == 0 ? "player 1" : "player 2");
    fprintf(stderr, "[input] keyboard layout: %s\n", g_config.keyboard_layout.c_str());
    sdl_input::assign_waiting();
    return true;
}

void platform_shutdown() {
    if (g_mic) { SDL_DestroyAudioStream(g_mic); g_mic = nullptr; }
    if (g_audio) SDL_DestroyAudioStream(g_audio);
    sdl_input::close_all();
    if (g_texture) SDL_DestroyTexture(g_texture);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();
}

// A fatal error never opens an OS dialog (no NSAlert / SDL message box). The message goes to
// crash.log next to the settings, and, in a windowed run, onto the in-game error screen, which
// holds the stopped game on screen until the player chooses QUIT.
static std::string fatal_log_path() {
#ifdef __APPLE__
    return mac_app_support() + "/crash.log";
#else
    return "crash.log";
#endif
}

#ifndef EOD_DISPLAY_BACKEND_TEST
[[noreturn]] void app_exit(int code);
// Starts a fresh copy of the game (its title screen) and lets this one go. The panicked
// process cannot be reused: its guest memory is corrupt. From an app bundle it is a new
// instance of the bundle; otherwise the same executable with the same arguments.
static void relaunch_game() {
#ifdef __APPLE__
    char path[4096];
    uint32_t size = sizeof path;
    if (_NSGetExecutablePath(path, &size) != 0) return;
    const std::string exe = path;
    const size_t at = exe.find(".app/Contents/MacOS/");
    pid_t pid = 0;
    if (at != std::string::npos) {
        const std::string bundle = exe.substr(0, at + 4);
        char *args[] = {const_cast<char *>("open"), const_cast<char *>("-n"), const_cast<char *>(bundle.c_str()), nullptr};
        posix_spawn(&pid, "/usr/bin/open", nullptr, nullptr, args, environ);
    } else {
        posix_spawn(&pid, exe.c_str(), nullptr, nullptr, *_NSGetArgv(), environ);
    }
#endif
}
#endif

void platform_fatal_message(const char *msg) {
    if (FILE *f = fopen(fatal_log_path().c_str(), "a")) {
        const time_t now = time(nullptr);
        char when[32] = "";
        strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(f, "[%s] %s\n", when, msg);
        fclose(f);
    }
    if (g_headless || !g_window) return;
    // The screen runs the window's event loop, which only the thread that owns the window (the
    // EE thread) may do; a fatal error from any other thread just logs and aborts.
    if (std::this_thread::get_id() != g_main_thread) return;
    if (g_audio) SDL_PauseAudioStreamDevice(g_audio);  // the stopped game would otherwise loop its last buffer
#ifndef EOD_DISPLAY_BACKEND_TEST
    // Plain words only; the technical message is in crash.log.
    overlay_fatal_screen("SOMETHING WENT WRONG",
                         "The game ran into a problem and has to start over. "
                         "Choose RETURN TO TITLE SCREEN to start the game again.");
    relaunch_game();
    app_exit(0);  // no relaunch possible: close cleanly (the details are in crash.log)
#endif
}

bool platform_pump() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            for (int p = 0; p < kPorts; ++p) sdl_input::rumble(p, 0, 0);
            break;
        case SDL_EVENT_QUIT:
            return false;
        case SDL_EVENT_GAMEPAD_ADDED:
            sdl_input::on_added(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            sdl_input::on_removed(e.gdevice.which);
            break;
        case SDL_EVENT_MOUSE_MOTION: {
            const float d = g_window ? SDL_GetWindowPixelDensity(g_window) : 1.0f;
            overlay_mouse_move(e.motion.x * d, e.motion.y * d);
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button == SDL_BUTTON_LEFT) overlay_mouse_button(e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            overlay_mouse_wheel(e.wheel.y);
            break;
        case SDL_EVENT_KEY_UP:
            time_key_event(e.key);
            overlay_key(scancode_to_vk(e.key.scancode), false);
            break;
        case SDL_EVENT_KEY_DOWN:
            time_key_event(e.key);
            if (!g_headless && !(e.key.scancode == SDL_SCANCODE_F11 || (e.key.scancode == SDL_SCANCODE_F && (e.key.mod & SDL_KMOD_GUI)))) {
                // the in-game front end: F1 / Cmd+, open it (Esc stays the game's); while open it takes every key
                const int vk = (e.key.scancode == SDL_SCANCODE_COMMA && (e.key.mod & SDL_KMOD_GUI)) ? 0x70 : scancode_to_vk(e.key.scancode);
                const bool open = overlay_menu_open();
                // clipboard in the menu: Cmd+V / Ctrl+V pastes into a text field, Cmd+C / Ctrl+C copies the Garage Code
                if (open && (e.key.mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) && !e.key.repeat) {
                    if (e.key.scancode == SDL_SCANCODE_V) { if (char *clip = SDL_GetClipboardText()) { overlay_paste(clip); SDL_free(clip); } break; }
                    if (e.key.scancode == SDL_SCANCODE_C) { const std::string text = overlay_copy_text(); if (!text.empty()) SDL_SetClipboardText(text.c_str()); break; }
                }
                if (vk && (!e.key.repeat || open) && overlay_key(vk, true)) {
                    if (open) if (char c = key_char(vk, (e.key.mod & SDL_KMOD_SHIFT) != 0)) overlay_char((unsigned char)c);
                    break;
                }
            }
            if (!g_headless && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F2) {
                const std::string help = sdl_input::help_text(g_config.keyboard_layout == "racing", g_config.keys[KEY_CROSS]);
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Controls and player 2", help.c_str(), g_window);
                break;
            }
            // Cmd+F / F11: toggle fullscreen
            if (g_window && (e.key.scancode == SDL_SCANCODE_F11 ||
                             (e.key.scancode == SDL_SCANCODE_F && (e.key.mod & SDL_KMOD_GUI)))) {
                bool fs = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;
                SDL_SetWindowFullscreen(g_window, !fs);
            }
            if (!g_headless && !e.key.repeat) {
                if (e.key.scancode == SDL_SCANCODE_F5) {
                    g_config.cheat_chickens = !g_config.cheat_chickens;
                    platform_show_hud_banner(g_config.cheat_chickens ? "★ CHICKEN FIGHT: ON (Press L2 to Fire) ★" : "★ CHICKEN FIGHT: OFF ★");
                    platform_haptic_pulse(0, 150, 1, 180);
                    platform_audio_feedback(g_config.cheat_chickens);
                } else if (e.key.scancode == SDL_SCANCODE_F6) {
                    g_config.cheat_turbo = !g_config.cheat_turbo;
                    if (g_config.cheat_turbo && g_config.turbo_scale <= 1.0f) g_config.turbo_scale = 1.75f;
                    platform_show_hud_banner(g_config.cheat_turbo ? "★ TURBO BOOST: ON (Hold L2) ★" : "★ TURBO BOOST: OFF ★");
                    platform_haptic_pulse(0, 150, 1, 180);
                    platform_audio_feedback(g_config.cheat_turbo);
                } else if (e.key.scancode == SDL_SCANCODE_F7) {
                    uint32_t c = ee_r32(0x003C4190);
                    bool on = !(c & 0x80);
                    if (on) {
                        ee_w32(0x003C4190, (c | 0x89) & ~0x10);
                        g_config.cheat_chickens = false;
                    } else {
                        ee_w32(0x003C4190, c & ~0x80);
                    }
                    platform_show_hud_banner(on ? "★ RAINING CHICKENS: ON ★" : "★ RAINING CHICKENS: OFF ★");
                    platform_haptic_pulse(0, 150, 1, 180);
                    platform_audio_feedback(on);
                } else if (e.key.scancode == SDL_SCANCODE_F8) {
                    bool on = !(g_config.cheat_chickens && g_config.cheat_turbo);
                    g_config.cheat_chickens = on;
                    g_config.cheat_turbo = on;
                    if (on && g_config.turbo_scale <= 1.0f) g_config.turbo_scale = 1.75f;
                    platform_show_hud_banner(on ? "★ CHAOS MODE: ON (Chickens + Turbo) ★" : "★ CHAOS MODE: OFF ★");
                    platform_haptic_pulse(0, 150, 1, 200);
                    platform_audio_feedback(on);
                }
            }
            break;
        default:
            break;
        }
    }
    uint64_t now_us = platform_time_us();
    if (g_haptic_pulse_until && now_us >= g_haptic_pulse_until) {
        g_haptic_pulse_until = 0;
        for (int p = 0; p < kPorts; ++p) sdl_input::rumble(p, 0, 0);
    }
    sdl_input::update_rumble();
    return true;
}

void platform_display_rect(int cw, int ch, int *x, int *y, int *w, int *h) {
    const DisplayRect rect=display_rect(presentation::current(g_cfg.aspect),cw,ch);
    *x=rect.x;*y=rect.y;*w=rect.width;*h=rect.height;
}

void platform_present(const uint32_t *pixels, int width, int height, int stride) {
    // Hardware initialization can fail after SDL created its Vulkan window.
    // Recreate that window for the software presenter on this main thread.
    if (!g_headless && !g_renderer && g_vulkan_window && !gs::hw::g_on) {
        SDL_DestroyWindow(g_window);
        SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (g_cfg.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
        g_window = SDL_CreateWindow(g_cfg.title, g_cfg.width, g_cfg.height, flags);
        g_vulkan_window = false;
        g_renderer = g_window ? SDL_CreateRenderer(g_window, nullptr) : nullptr;
        if (!g_renderer) {
            fprintf(stderr, "[video] software fallback display failed: %s\n", SDL_GetError());
            return;
        }
        SDL_SetRenderVSync(g_renderer, g_config.vsync ? 1 : 0);
        fprintf(stderr, "[video] software fallback: %s\n", SDL_GetRendererName(g_renderer));
    }
    if (g_headless || !g_renderer) {
        g_last_frame.resize((size_t)width * height);
        for (int y = 0; y < height; y++)
            memcpy(&g_last_frame[(size_t)y * width], pixels + (size_t)y * stride, (size_t)width * 4);
        g_last_w = width;
        g_last_h = height;
        return;
    }
    if (!g_texture || g_tex_w != width || g_tex_h != height) {
        if (g_texture) SDL_DestroyTexture(g_texture);
        // bytes R,G,B,A in memory
        g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, width, height);
        if (!g_texture) return;
        SDL_SetTextureScaleMode(g_texture, g_cfg.smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        g_tex_w = width;
        g_tex_h = height;
    }
    SDL_UpdateTexture(g_texture, nullptr, pixels, stride * 4);
    int ow = 0, oh = 0;
    SDL_GetCurrentRenderOutputSize(g_renderer, &ow, &oh);
    int x, y, w, h;
    platform_display_rect(ow, oh, &x, &y, &w, &h);
    SDL_FRect dst = {(float)x, (float)y, (float)w, (float)h};
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderTexture(g_renderer, g_texture, nullptr, &dst);

    uint64_t now_us = platform_time_us();
    if (g_mod_overlay_visible) {
        SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
        float box_w = std::min(500.0f, (float)ow - 40.0f);
        float box_h = 185.0f;
        float box_x = ((float)ow - box_w) * 0.5f;
        float box_y = ((float)oh - box_h) * 0.5f;

        SDL_FRect bg = {box_x, box_y, box_w, box_h};
        SDL_SetRenderDrawColor(g_renderer, 15, 15, 20, 230);
        SDL_RenderFillRect(g_renderer, &bg);

        SDL_SetRenderDrawColor(g_renderer, 240, 180, 20, 255);
        SDL_RenderRect(g_renderer, &bg);

        float text_scale = ow >= 1280 ? 1.5f : 1.0f;
        SDL_SetRenderScale(g_renderer, text_scale, text_scale);
        float sx = (box_x + 16.0f) / text_scale;
        float sy = (box_y + 14.0f) / text_scale;
        float line_h = 18.0f;

        SDL_SetRenderDrawColor(g_renderer, 255, 215, 0, 255);
        SDL_RenderDebugText(g_renderer, sx, sy, "[ LIVE MOD CHEATS - L1+R1 HELD ]");

        SDL_SetRenderDrawColor(g_renderer, 255, 255, 255, 255);
        SDL_RenderDebugTextFormat(g_renderer, sx, sy + line_h * 1.5f, "D-PAD UP   : Chicken Fight   [%s]", g_config.cheat_chickens ? "ON" : "OFF");
        SDL_RenderDebugTextFormat(g_renderer, sx, sy + line_h * 2.5f, "D-PAD RIGHT: Turbo Boost     [%s]", g_config.cheat_turbo ? "ON" : "OFF");
        SDL_RenderDebugTextFormat(g_renderer, sx, sy + line_h * 3.5f, "D-PAD DOWN : Rain Chickens   [%s]", (ee_r32(0x003C4190) & 0x80) ? "ON" : "OFF");
        SDL_RenderDebugTextFormat(g_renderer, sx, sy + line_h * 4.5f, "D-PAD LEFT : AI Vengeance    [%s]", (ee_r32(0x003C4190) & 0x20) ? "ON" : "OFF");
        SDL_RenderDebugText(g_renderer, sx, sy + line_h * 5.5f, "TRIANGLE   : Chaos Mode      (Chickens + Turbo)");
        SDL_RenderDebugTextFormat(g_renderer, sx, sy + line_h * 6.5f, "SELECT / L3+R3: Quick Chickens  [%s]", g_config.cheat_chickens ? "ON" : "OFF");

        SDL_SetRenderScale(g_renderer, 1.0f, 1.0f);
    } else if (g_hud_banner_until > now_us && !g_hud_banner_text.empty()) {
        SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
        float banner_w = std::min(540.0f, (float)ow - 40.0f);
        float banner_h = 36.0f;
        float banner_x = ((float)ow - banner_w) * 0.5f;
        float banner_y = 24.0f;

        SDL_FRect bg = {banner_x, banner_y, banner_w, banner_h};
        SDL_SetRenderDrawColor(g_renderer, 15, 15, 22, 235);
        SDL_RenderFillRect(g_renderer, &bg);

        SDL_SetRenderDrawColor(g_renderer, 240, 180, 20, 255);
        SDL_RenderRect(g_renderer, &bg);

        float text_scale = ow >= 1280 ? 1.5f : 1.0f;
        SDL_SetRenderScale(g_renderer, text_scale, text_scale);
        float sx = (banner_x + 14.0f) / text_scale;
        float sy = (banner_y + 10.0f) / text_scale;
        SDL_SetRenderDrawColor(g_renderer, 255, 255, 255, 255);
        SDL_RenderDebugText(g_renderer, sx, sy, g_hud_banner_text.c_str());
        SDL_SetRenderScale(g_renderer, 1.0f, 1.0f);
    }

    SDL_RenderPresent(g_renderer);
}

bool platform_last_frame(std::vector<uint32_t> &pix, int &w, int &h) {
    if (g_last_frame.empty()) return false;
    pix = g_last_frame;
    w = g_last_w;
    h = g_last_h;
    return true;
}

void *platform_native_window() { return g_vulkan_window ? g_window : nullptr; }

void platform_set_status(const char *text) {
    if (!g_window) return;
    char buf[512];
    const char *help = keyboard_context() == keyboard_input::Context::Driving ? "" : " - F2: Controls / player 2";
    snprintf(buf, sizeof buf, "%s - %s%s", g_cfg.title, text, help);
    SDL_SetWindowTitle(g_window, buf);
}

void platform_show_hud_banner(const char *text, int duration_ms) {
    if (!text || !text[0]) {
        g_hud_banner_until = 0;
        return;
    }
    g_hud_banner_text = text;
    g_hud_banner_until = platform_time_us() + (uint64_t)duration_ms * 1000;
    platform_set_status(text);
}

void platform_set_mod_overlay(bool visible) {
    g_mod_overlay_visible = visible;
}

#ifndef EOD_DISPLAY_BACKEND_TEST
void overlay_platform_pad(int port, HostPadState *out) {
    // menus read buttons only (Original mapping): a Modern trigger's gas/brake pressure never selects
    *out = HostPadState();
    if (port < 0 || port >= kPorts) return;
    if (pad_script_state(port, out)) return;
    if (SDL_Gamepad *gp = sdl_input::port_gamepad(port))
        sdl_input::read_menu_gamepad(gp, out, g_headless || (g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS)));
}
bool overlay_platform_keys_held() {
    if (g_headless) return false;
    int n = 0;
    const bool *keys = SDL_GetKeyboardState(&n);
    for (int i = 0; keys && i < n; i++)
        if (keys[i]) return true;
    return false;
}
std::string overlay_platform_device(int port, bool *xbox) { return sdl_input::port_description(port, xbox); }
void overlay_platform_apply() {
    build_key_map();
    update_audio_gain();
    sdl_input::set_controller_layout(g_config.controller_layout == "modern");
    const char *keyboard_override = getenv("EOD_KEYBOARD_PORT");
    if (!(keyboard_override && keyboard_override[0])) sdl_input::set_keyboard_port(g_config.keyboard_port - 1, false);
}
void overlay_platform_swap_ports() { sdl_input::swap_ports(); }
bool overlay_platform_display(int *w, int *h) {
    if (!g_window) return false;
    return SDL_GetWindowSizeInPixels(g_window, w, h) && *w > 0 && *h > 0;
}
std::string overlay_platform_mods_root() {
#ifdef __APPLE__
    return mac_app_support() + "/mods";
#else
    return "mods";
#endif
}
#endif

void platform_get_pad(int port, HostPadState *out) {
    *out = HostPadState();
    if (port < 0 || port >= kPorts) return;
    if (overlay_captures_input()) {  // the front end has the input: the game sees a neutral pad
        out->connected = true;
        return;
    }
    auto ctx = keyboard_context(); // also observed in headless/scripted tests
    if (port == sdl_input::keyboard_port()) read_keyboard(out, ctx);
    if (SDL_Gamepad *gp = sdl_input::port_gamepad(port)) {
        sdl_input::read_gamepad(gp, out, g_headless || (g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS)));
        // per-player stick shaping and trigger response (front end INPUTS tab): input mapping only
        const int p = port & 1;
        out->lx = frontend::shape_axis(out->lx, g_config.stick_deadzone[p], g_config.stick_outer[p], g_config.stick_curve[p]);
        out->ly = frontend::shape_axis(out->ly, g_config.stick_deadzone[p], g_config.stick_outer[p], g_config.stick_curve[p]);
        for (int i : {6, 7, 10, 11})  // Modern gas/brake (Cross/Square) or Original L2/R2 pressure
            out->pressure[i] = frontend::shape_trigger(out->pressure[i], g_config.trigger_deadzone[p], g_config.trigger_curve[p]);
    }
    // EOD_PAD_SCRIPT / EOD_PAD_SCRIPT1 (automated tests) replace the port
    if (!pad_script_state(port, out)) {
        static const uint16_t order[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                           PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};
        for (int i = 0; i < 12; i++)
            if ((out->buttons & order[i]) && out->pressure[i] == 0) out->pressure[i] = 255;
    }
    if (sdl_input::log_enabled()) sdl_input::log_state(port, *out);
}

void platform_haptic_pulse(int port, int duration_ms, uint8_t small_motor, uint8_t large_motor) {
    if (!g_config.vibration || g_headless) return;
    g_haptic_pulse_until = platform_time_us() + (uint64_t)duration_ms * 1000;
    sdl_input::rumble(port, small_motor, large_motor);
}

void platform_set_vibration(int port, uint8_t small_motor, uint8_t large_motor) {
    uint64_t now_us = platform_time_us();
    if (now_us < g_haptic_pulse_until) return; // protect timed haptic pulse
    if (!g_config.vibration || (!g_headless &&
        (!g_window || !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS)))) small_motor = large_motor = 0;
    sdl_input::rumble(port, small_motor, large_motor);
}

void platform_audio_feedback(bool enabled) {
    if (g_headless || !g_config.audio) return;
    const int sample_rate = 48000;
    const int duration_ms = 80;
    const int total_samples = (sample_rate * duration_ms) / 1000;
    std::vector<int16_t> pcm(total_samples * 2);
    float freq1 = 880.0f;
    float freq2 = enabled ? 1320.0f : 440.0f;
    int half = total_samples / 2;
    for (int i = 0; i < total_samples; i++) {
        float f = (i < half) ? freq1 : freq2;
        float t = (float)i / (float)sample_rate;
        float env = 1.0f;
        if (i < 200) env = (float)i / 200.0f;
        else if (total_samples - i < 200) env = (float)(total_samples - i) / 200.0f;
        int16_t val = (int16_t)(sinf(2.0f * 3.14159265f * f * t) * 5000.0f * env);
        pcm[i * 2 + 0] = val;
        pcm[i * 2 + 1] = val;
    }
    platform_audio_submit(pcm.data(), total_samples);
}

int platform_list_pads(int watch_seconds) { return sdl_input::list_pads(watch_seconds); }

// The game controller alone, without the keyboard (upstream's in-game menu API).
void platform_get_controller(int port, HostPadState *out) {
    *out = HostPadState();
    if (port < 0 || port >= kPorts) return;
    if (SDL_Gamepad *gp = sdl_input::port_gamepad(port)) sdl_input::read_gamepad(gp, out, g_headless ||
        (g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS)));
}

// The game's display mode (src/game_patches.cpp, EE thread). The GS command
// stream carries it per frame (eod_presentation_aspect, presentation.h), so the
// composed image switches aspect in step with the frames that use it.
void platform_set_aspect(int aspect) { presentation::requested.store(aspect, std::memory_order_relaxed); }

void platform_set_fullscreen(bool on) {
    if (g_window) SDL_SetWindowFullscreen(g_window, on);
}
bool platform_fullscreen() { return g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0; }

// ------------------------------------------------------------------ audio
bool platform_audio_open(int sample_rate) {
    const char *m = getenv("EOD_MUTE");
    if (g_headless || (m && m[0] == '1')) return false;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL audio: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec spec = {SDL_AUDIO_S16LE, 2, sample_rate};
    g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!g_audio) {
        fprintf(stderr, "SDL audio stream: %s\n", SDL_GetError());
        return false;
    }
    update_audio_gain();
    if (!SDL_ResumeAudioStreamDevice(g_audio)) {
        fprintf(stderr, "SDL audio resume: %s\n", SDL_GetError());
        SDL_DestroyAudioStream(g_audio);
        g_audio = nullptr;
        return false;
    }
    fprintf(stderr, "SDL audio: playback device=%s rate=%d\n",
            SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(g_audio)), sample_rate);
    return true;
}

void platform_audio_submit(const int16_t *samples, int frames) {
    if (g_audio && frames > 0) SDL_PutAudioStreamData(g_audio, samples, frames * 4);
}

int platform_audio_queued_frames() { return g_audio ? SDL_GetAudioStreamQueued(g_audio) / 4 : 0; }

bool platform_mic_open(int sample_rate) {
    const char *m = getenv("EOD_MUTE");
    if (g_headless || (m && m[0] == '1')) return false;
    if (g_mic) return true;
#if defined(__APPLE__) && !defined(EOD_DISPLAY_BACKEND_TEST)
    // This function is reached only after the player opts into microphone use.
    // Ask through AVFoundation before opening SDL's CoreAudio recording device.
    const auto permission = microphone_permission::state();
    static int previous_permission = -1;
    if (previous_permission != int(permission)) {
        previous_permission = int(permission);
        fprintf(stderr, "SDL mic: macOS permission state=%d (0=not requested, 1=granted, 2=denied, 3=restricted)\n", previous_permission);
    }
    if (permission != microphone_permission::State::Granted) {
        static std::atomic<bool> requesting{false};
        if (permission == microphone_permission::State::NotRequested && !requesting.exchange(true)) {
            microphone_permission::request([](microphone_permission::State result) {
                fprintf(stderr, "SDL mic: macOS permission request completed state=%d\n", int(result));
                requesting.store(false);
            });
        }
        return false;
    }
#endif
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL audio: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec spec = {SDL_AUDIO_S16LE, 1, sample_rate};
    g_mic = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, nullptr, nullptr);
    if (!g_mic) {
        fprintf(stderr, "SDL mic stream: %s\n", SDL_GetError());
        return false;
    }
    if (!SDL_ResumeAudioStreamDevice(g_mic)) {
        fprintf(stderr, "SDL mic resume: %s\n", SDL_GetError());
        SDL_DestroyAudioStream(g_mic);
        g_mic = nullptr;
        return false;
    }
    fprintf(stderr, "SDL mic: recording device=%s rate=%d\n",
            SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(g_mic)), sample_rate);
    return true;
}

void platform_mic_close() {
    if (g_mic) {
        SDL_DestroyAudioStream(g_mic);
        g_mic = nullptr;
    }
}

int platform_mic_read(int16_t *samples, int max_samples) {
    if (!g_mic || max_samples <= 0) return 0;
    const int available_bytes = SDL_GetAudioStreamAvailable(g_mic);
    if (available_bytes < 0) {
        fprintf(stderr, "SDL mic read: %s\n", SDL_GetError());
        platform_mic_close();
        return -1;
    }
    const int avail = available_bytes / 2;
    if (avail < max_samples) return 0;
    if (avail > max_samples * 8) {
        std::vector<int16_t> discard((size_t)(avail - max_samples * 2));
        if (SDL_GetAudioStreamData(g_mic, discard.data(), (int)discard.size() * 2) < 0) {
            fprintf(stderr, "SDL mic backlog: %s\n", SDL_GetError());
            platform_mic_close();
            return -1;
        }
    }
    const int got = SDL_GetAudioStreamData(g_mic, samples, max_samples * 2);
    if (got < 0) {
        fprintf(stderr, "SDL mic read: %s\n", SDL_GetError());
        platform_mic_close();
        return -1;
    }
    return got > 0 ? got / 2 : 0;
}
