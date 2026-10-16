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
#include <mutex>
#include <vector>

#include "../../core/config.h"
#ifdef __APPLE__
#include "../../app/macos_app.h"
#endif
#include "../pad_script.h"
#include "../platform.h"
#include "sdl_input.h"

namespace {

PlatformConfig g_cfg;
bool g_headless;
SDL_Window *g_window;
SDL_Renderer *g_renderer;
SDL_Texture *g_texture;
int g_tex_w, g_tex_h;
SDL_AudioStream *g_audio;
float g_volume = 1.0f;

std::vector<uint32_t> g_last_frame;
int g_last_w, g_last_h;

// ---------------------------------------------------------------- input
using sdl_input::kPorts;

struct KeyBind {
    SDL_Scancode sc;
    uint16_t btn;
};
std::vector<KeyBind> g_key_map;

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

void build_key_map() {
    g_key_map.clear();
    for (int k = 0; k < KEY_COUNT; k++) {
        const std::string &list = g_config.keys[k];
        size_t pos = 0;
        while (pos <= list.size()) {
            size_t comma = list.find(',', pos);
            std::string name = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            SDL_Scancode sc = vk_to_scancode(key_code(name));
            if (sc != SDL_SCANCODE_UNKNOWN) g_key_map.push_back({sc, pad_key_button(k)});
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
}

void read_keyboard(HostPadState *out) {
    out->connected = true;
    if (g_headless) return;
    const bool *keys = SDL_GetKeyboardState(nullptr);
    for (const KeyBind &m : g_key_map)
        if (keys[m.sc]) out->buttons |= m.btn;
    // the direction keys also move the left stick (steering)
    if (out->buttons & PAD_LEFT) out->lx = 0;
    if (out->buttons & PAD_RIGHT) out->lx = 255;
    if (out->buttons & PAD_UP) out->ly = 0;
    if (out->buttons & PAD_DOWN) out->ly = 255;
}

}  // namespace

bool platform_init(const PlatformConfig &cfg) {
    g_cfg = cfg;
    const char *h = getenv("EOD_HEADLESS");
    g_headless = cfg.headless || (h && h[0] == '1');
    const char *keyboard_override = getenv("EOD_KEYBOARD_PORT");
    sdl_input::set_keyboard_port(keyboard_override && keyboard_override[0]
        ? sdl_input::keyboard_port_from_env() : g_config.keyboard_port - 1);
    sdl_input::set_hints();
    SDL_InitFlags flags = SDL_INIT_GAMEPAD | SDL_INIT_EVENTS;
    if (!g_headless) flags |= SDL_INIT_VIDEO;
    if (!SDL_Init(flags)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    build_key_map();
    g_volume = (float)g_config.volume / 100.0f;

    if (!g_headless) {
        SDL_WindowFlags wf = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (cfg.fullscreen) wf |= SDL_WINDOW_FULLSCREEN;
        g_window = SDL_CreateWindow(cfg.title, cfg.width, cfg.height, wf);
        if (!g_window) {
            fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
            return false;
        }
        g_renderer = SDL_CreateRenderer(g_window, nullptr);
        if (!g_renderer) {
            fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
            return false;
        }
        SDL_SetRenderVSync(g_renderer, g_config.vsync ? 1 : 0);
        fprintf(stderr, "[video] SDL renderer: %s\n", SDL_GetRendererName(g_renderer));
    }

    int kb = sdl_input::keyboard_port();
    fprintf(stderr, "[input] keyboard: %s\n", kb < 0 ? "off" : kb == 0 ? "player 1" : "player 2");
    sdl_input::assign_waiting();
    return true;
}

void platform_shutdown() {
    if (g_audio) SDL_DestroyAudioStream(g_audio);
    sdl_input::close_all();
    if (g_texture) SDL_DestroyTexture(g_texture);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();
}

void platform_fatal_message(const char *msg) {
    if (!g_headless) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Eve of Destruction", msg, g_window);
}

bool platform_pump() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            return false;
        case SDL_EVENT_GAMEPAD_ADDED:
            sdl_input::on_added(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            sdl_input::on_removed(e.gdevice.which);
            break;
        case SDL_EVENT_KEY_DOWN:
#ifdef __APPLE__
            if (!g_headless && !e.key.repeat && (e.key.scancode == SDL_SCANCODE_F1 ||
                (e.key.scancode == SDL_SCANCODE_COMMA && (e.key.mod & SDL_KMOD_GUI)))) {
                if (mac_app_settings()) {
                    build_key_map();
                    g_volume = g_config.audio ? (float)g_config.volume / 100.0f : 0;
                    if (g_audio) SDL_SetAudioStreamGain(g_audio, g_volume);
                    g_cfg.aspect = g_config.aspect == "16:9" ? 1 : g_config.aspect == "stretch" ? 2 : 0;
                    g_cfg.smooth = g_config.smooth_display;
                    if (g_texture) SDL_SetTextureScaleMode(g_texture, g_cfg.smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
                    SDL_SetWindowSize(g_window, g_config.width, g_config.height);
                    SDL_SetWindowFullscreen(g_window, g_config.fullscreen);
                }
                break;
            }
#endif
            // Cmd+F / F11: toggle fullscreen
            if (g_window && (e.key.scancode == SDL_SCANCODE_F11 ||
                             (e.key.scancode == SDL_SCANCODE_F && (e.key.mod & SDL_KMOD_GUI)))) {
                bool fs = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;
                SDL_SetWindowFullscreen(g_window, !fs);
            }
            break;
        default:
            break;
        }
    }
    sdl_input::update_rumble();
    return true;
}

void platform_display_rect(int cw, int ch, int *x, int *y, int *w, int *h) {
    int vw = cw, vh = ch;
    if (g_cfg.aspect != 2) {  // 4:3 or 16:9, letterboxed
        int an = g_cfg.aspect == 1 ? 16 : 4, ad = g_cfg.aspect == 1 ? 9 : 3;
        vh = cw * ad / an;
        if (vh > ch) {
            vh = ch;
            vw = ch * an / ad;
        }
    }
    *x = (cw - vw) / 2;
    *y = (ch - vh) / 2;
    *w = vw;
    *h = vh;
}

void platform_present(const uint32_t *pixels, int width, int height, int stride) {
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
    SDL_RenderPresent(g_renderer);
}

bool platform_last_frame(std::vector<uint32_t> &pix, int &w, int &h) {
    if (g_last_frame.empty()) return false;
    pix = g_last_frame;
    w = g_last_w;
    h = g_last_h;
    return true;
}

void *platform_native_window() { return nullptr; }  // no D3D11; the GS renderer is software here

void platform_set_status(const char *text) {
    if (!g_window) return;
    char buf[256];
    snprintf(buf, sizeof buf, "%s - %s", g_cfg.title, text);
    SDL_SetWindowTitle(g_window, buf);
}

void platform_get_pad(int port, HostPadState *out) {
    *out = HostPadState();
    if (port < 0 || port >= kPorts) return;
    if (port == sdl_input::keyboard_port()) read_keyboard(out);
    if (SDL_Gamepad *gp = sdl_input::port_gamepad(port)) sdl_input::read_gamepad(gp, out);
    // EOD_PAD_SCRIPT / EOD_PAD_SCRIPT1 (automated tests) replace the port
    if (!pad_script_state(port, out)) {
        static const uint16_t order[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                           PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};
        for (int i = 0; i < 12; i++)
            if ((out->buttons & order[i]) && out->pressure[i] == 0) out->pressure[i] = 255;
    }
    if (sdl_input::log_enabled()) sdl_input::log_state(port, *out);
}

void platform_set_vibration(int port, uint8_t small_motor, uint8_t large_motor) {
    if (!g_config.vibration) small_motor = large_motor = 0;
    sdl_input::rumble(port, small_motor, large_motor);
}

int platform_list_pads(int watch_seconds) { return sdl_input::list_pads(watch_seconds); }

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
    SDL_SetAudioStreamGain(g_audio, g_volume);
    SDL_ResumeAudioStreamDevice(g_audio);
    return true;
}

void platform_audio_submit(const int16_t *samples, int frames) {
    if (g_audio && frames > 0) SDL_PutAudioStreamData(g_audio, samples, frames * 4);
}

int platform_audio_queued_frames() { return g_audio ? SDL_GetAudioStreamQueued(g_audio) / 4 : 0; }
