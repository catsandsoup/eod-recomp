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
#include <mutex>
#include <vector>

#include "../../core/config.h"
#include "audio_gain.h"
#ifdef __APPLE__
#include "../../app/macos_app.h"
#endif
#include "../../gs/gs_hw.h"

#include "../../core/memory.h"
#include "../keyboard_input.h"
#include "../pad_script.h"
#include "../platform.h"
#include "sdl_input.h"

namespace {

PlatformConfig g_cfg;
bool g_headless;
bool g_vulkan_window;
SDL_Window *g_window;
SDL_Renderer *g_renderer;
SDL_Texture *g_texture;
int g_tex_w, g_tex_h;
SDL_AudioStream *g_audio;
float g_volume = 1.0f;

void update_audio_gain() {
    g_volume = sdl_output_gain(g_config);
    if (g_audio) SDL_SetAudioStreamGain(g_audio, g_volume);
}

std::vector<uint32_t> g_last_frame;
int g_last_w, g_last_h;

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
    *out = g_keyboard.read(keys, ctx, g_config.keyboard_layout == "racing", g_key_map);
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
    const char *h = getenv("EOD_HEADLESS");
    g_headless = cfg.headless || (h && h[0] == '1');
    const char *keyboard_override = getenv("EOD_KEYBOARD_PORT");
    sdl_input::set_keyboard_port(keyboard_override && keyboard_override[0]
        ? sdl_input::keyboard_port_from_env() : g_config.keyboard_port - 1);
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
        case SDL_EVENT_KEY_UP:
            time_key_event(e.key);
            break;
        case SDL_EVENT_KEY_DOWN:
            time_key_event(e.key);
#ifdef __APPLE__
            if (!g_headless && !e.key.repeat && (e.key.scancode == SDL_SCANCODE_F1 ||
                (e.key.scancode == SDL_SCANCODE_COMMA && (e.key.mod & SDL_KMOD_GUI)))) {
                if (mac_app_settings()) {
                    build_key_map();
                    update_audio_gain();
                    // Aspect and camera projection both change on the next launch.
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
    char buf[256];
    snprintf(buf, sizeof buf, "%s - %s", g_cfg.title, text);
    SDL_SetWindowTitle(g_window, buf);
}

void platform_get_pad(int port, HostPadState *out) {
    *out = HostPadState();
    if (port < 0 || port >= kPorts) return;
    auto ctx = keyboard_context(); // also observed in headless/scripted tests
    if (port == sdl_input::keyboard_port()) read_keyboard(out, ctx);
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
    update_audio_gain();
    SDL_ResumeAudioStreamDevice(g_audio);
    return true;
}

void platform_audio_submit(const int16_t *samples, int frames) {
    if (g_audio && frames > 0) SDL_PutAudioStreamData(g_audio, samples, frames * 4);
}

int platform_audio_queued_frames() { return g_audio ? SDL_GetAudioStreamQueued(g_audio) / 4 : 0; }
