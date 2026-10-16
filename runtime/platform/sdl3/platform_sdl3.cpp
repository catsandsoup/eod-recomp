// platform_sdl3.cpp - SDL3 backend for Linux and macOS (CMake option
// EOD_SDL3): window, keyboard, game controllers and audio. Time, fibers, the
// ticker and files stay in posix/platform_posix.cpp, whose headless window,
// input and audio functions are left out of this build.
//
// The software GS output is shown through an SDL renderer (Metal on macOS).
// Input follows the Windows backend: the keyboard ([Input] bindings in
// eod.ini) is on player 1, and game controllers take the player ports in the
// order they are connected, so the first controller shares player 1 with the
// keyboard and the second one is player 2. A controller unplugged frees its
// port for the next one.
//
// --headless / EOD_HEADLESS=1: no window and no audio device. EOD_MUTE=1: no
// audio device.
#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../../core/config.h"
#include "../pad_script.h"
#include "../platform.h"

namespace {

PlatformConfig g_cfg;
std::atomic<int> g_aspect{0};  // display aspect (platform_set_aspect)
bool g_headless;
SDL_Window *g_window;
SDL_Renderer *g_renderer;
SDL_Texture *g_texture;
int g_tex_w, g_tex_h;
SDL_AudioStream *g_audio;

// ------------------------------------------------------------- keyboard
// eod.ini names keys by their Windows virtual-key codes (config.cpp's
// key_code); these are the SDL scancodes at the same places.
void vk_scancodes(int vk, std::vector<SDL_Scancode> &out) {
    if (vk >= 'A' && vk <= 'Z') out.push_back((SDL_Scancode)(SDL_SCANCODE_A + (vk - 'A')));
    else if (vk >= '1' && vk <= '9') out.push_back((SDL_Scancode)(SDL_SCANCODE_1 + (vk - '1')));
    else if (vk == '0') out.push_back(SDL_SCANCODE_0);
    else if (vk >= 0x70 && vk <= 0x7B) out.push_back((SDL_Scancode)(SDL_SCANCODE_F1 + (vk - 0x70)));
    else if (vk >= 0x61 && vk <= 0x69) out.push_back((SDL_Scancode)(SDL_SCANCODE_KP_1 + (vk - 0x61)));
    else switch (vk) {
    case 0x08: out.push_back(SDL_SCANCODE_BACKSPACE); break;
    case 0x09: out.push_back(SDL_SCANCODE_TAB); break;
    case 0x0D: out.push_back(SDL_SCANCODE_RETURN); out.push_back(SDL_SCANCODE_KP_ENTER); break;
    case 0x10: out.push_back(SDL_SCANCODE_LSHIFT); out.push_back(SDL_SCANCODE_RSHIFT); break;
    case 0x11: out.push_back(SDL_SCANCODE_LCTRL); out.push_back(SDL_SCANCODE_RCTRL); break;
    case 0x12: out.push_back(SDL_SCANCODE_LALT); out.push_back(SDL_SCANCODE_RALT); break;
    case 0x13: out.push_back(SDL_SCANCODE_PAUSE); break;
    case 0x14: out.push_back(SDL_SCANCODE_CAPSLOCK); break;
    case 0x1B: out.push_back(SDL_SCANCODE_ESCAPE); break;
    case 0x20: out.push_back(SDL_SCANCODE_SPACE); break;
    case 0x21: out.push_back(SDL_SCANCODE_PAGEUP); break;
    case 0x22: out.push_back(SDL_SCANCODE_PAGEDOWN); break;
    case 0x23: out.push_back(SDL_SCANCODE_END); break;
    case 0x24: out.push_back(SDL_SCANCODE_HOME); break;
    case 0x25: out.push_back(SDL_SCANCODE_LEFT); break;
    case 0x26: out.push_back(SDL_SCANCODE_UP); break;
    case 0x27: out.push_back(SDL_SCANCODE_RIGHT); break;
    case 0x28: out.push_back(SDL_SCANCODE_DOWN); break;
    case 0x2D: out.push_back(SDL_SCANCODE_INSERT); break;
    case 0x2E: out.push_back(SDL_SCANCODE_DELETE); break;
    case 0x60: out.push_back(SDL_SCANCODE_KP_0); break;
    case 0x6A: out.push_back(SDL_SCANCODE_KP_MULTIPLY); break;
    case 0x6B: out.push_back(SDL_SCANCODE_KP_PLUS); break;
    case 0x6D: out.push_back(SDL_SCANCODE_KP_MINUS); break;
    case 0x6E: out.push_back(SDL_SCANCODE_KP_PERIOD); break;
    case 0x6F: out.push_back(SDL_SCANCODE_KP_DIVIDE); break;
    case 0xBA: out.push_back(SDL_SCANCODE_SEMICOLON); break;
    case 0xBB: out.push_back(SDL_SCANCODE_EQUALS); break;
    case 0xBC: out.push_back(SDL_SCANCODE_COMMA); break;
    case 0xBD: out.push_back(SDL_SCANCODE_MINUS); break;
    case 0xBE: out.push_back(SDL_SCANCODE_PERIOD); break;
    case 0xBF: out.push_back(SDL_SCANCODE_SLASH); break;
    case 0xC0: out.push_back(SDL_SCANCODE_GRAVE); break;
    case 0xDB: out.push_back(SDL_SCANCODE_LEFTBRACKET); break;
    case 0xDC: out.push_back(SDL_SCANCODE_BACKSLASH); break;
    case 0xDD: out.push_back(SDL_SCANCODE_RIGHTBRACKET); break;
    case 0xDE: out.push_back(SDL_SCANCODE_APOSTROPHE); break;
    }
}

struct KeyBind {
    SDL_Scancode sc;
    uint16_t btn;
};
std::vector<KeyBind> g_key_map;
const uint16_t kPadBits[KEY_COUNT] = {PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_CROSS, PAD_CIRCLE, PAD_SQUARE,
                                      PAD_TRIANGLE, PAD_L1, PAD_R1, PAD_L2, PAD_R2, PAD_L3, PAD_R3, PAD_START,
                                      PAD_SELECT};

void build_key_map() {
    g_key_map.clear();
    std::vector<SDL_Scancode> sc;
    for (int i = 0; i < KEY_COUNT; i++) {
        const std::string &list = g_config.keys[i];
        size_t p = 0;
        while (p <= list.size()) {
            size_t c = list.find(',', p);
            if (c == std::string::npos) c = list.size();
            sc.clear();
            vk_scancodes(key_code(list.substr(p, c - p)), sc);
            for (SDL_Scancode s : sc) g_key_map.push_back({s, kPadBits[i]});
            p = c + 1;
        }
    }
}

// ---------------------------------------------------------- controllers
// Two player ports; a controller takes the lowest free one when it is
// connected (or at start-up).
constexpr int kPorts = 2;
SDL_JoystickID g_port_id[kPorts];
SDL_Gamepad *g_port_pad[kPorts];

// A DualShock motor runs until the game changes it, an SDL rumble effect
// ends after its duration: running effects are renewed every frame.
struct Rumble {
    uint8_t small_motor, large_motor;
    uint64_t sent_ms;
};
Rumble g_rumble[kPorts];
constexpr Uint32 kRumbleMs = 1000;

int port_of(SDL_JoystickID id) {
    for (int p = 0; p < kPorts; p++)
        if (id && g_port_id[p] == id) return p;
    return -1;
}

void pad_added(SDL_JoystickID id) {
    if (port_of(id) >= 0) return;
    int p = 0;
    while (p < kPorts && g_port_pad[p]) p++;
    if (p == kPorts) return;
    SDL_Gamepad *gp = SDL_OpenGamepad(id);
    if (!gp) {
        fprintf(stderr, "[input] can't open controller: %s\n", SDL_GetError());
        return;
    }
    g_port_id[p] = id;
    g_port_pad[p] = gp;
    g_rumble[p] = Rumble();
    SDL_SetGamepadPlayerIndex(gp, p);
    const char *name = SDL_GetGamepadName(gp);
    fprintf(stderr, "[input] player %d: %s\n", p + 1, name ? name : "controller");
}

void pad_removed(SDL_JoystickID id) {
    int p = port_of(id);
    if (p < 0) return;
    SDL_CloseGamepad(g_port_pad[p]);
    g_port_pad[p] = nullptr;
    g_port_id[p] = 0;
    g_rumble[p] = Rumble();
    fprintf(stderr, "[input] player %d: controller removed\n", p + 1);
    // a controller that found no free port takes this one
    int n = 0;
    if (SDL_JoystickID *ids = SDL_GetGamepads(&n)) {
        for (int i = 0; i < n; i++) pad_added(ids[i]);
        SDL_free(ids);
    }
}

void rumble(int port, uint8_t small_motor, uint8_t large_motor) {
    if (port < 0 || port >= kPorts || !g_port_pad[port]) return;
    Rumble &r = g_rumble[port];
    uint64_t now = SDL_GetTicks();
    bool on = small_motor || large_motor;
    if (r.small_motor == small_motor && r.large_motor == large_motor && !(on && now - r.sent_ms >= kRumbleMs / 2))
        return;
    r = {small_motor, large_motor, now};
    // as XInput: the large motor is the low-frequency one, the small one on/off
    SDL_RumbleGamepad(g_port_pad[port], (Uint16)(large_motor * 257), small_motor ? 0xFFFF : 0, on ? kRumbleMs : 0);
}

// -32768..32767 -> 0..255 with the Windows backend's dead zone (112..144)
uint8_t stick(Sint16 v) {
    int a = (v + 32768) >> 8;
    if (a >= 112 && a <= 144) return 128;
    return (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a);
}

void read_gamepad(SDL_Gamepad *gp, HostPadState *out) {
    static const struct {
        SDL_GamepadButton b;
        uint16_t btn;
    } map[] = {
        // face buttons by position: south = Cross, east = Circle, west = Square, north = Triangle
        {SDL_GAMEPAD_BUTTON_SOUTH, PAD_CROSS},      {SDL_GAMEPAD_BUTTON_EAST, PAD_CIRCLE},
        {SDL_GAMEPAD_BUTTON_WEST, PAD_SQUARE},      {SDL_GAMEPAD_BUTTON_NORTH, PAD_TRIANGLE},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_R1},
        {SDL_GAMEPAD_BUTTON_BACK, PAD_SELECT},      {SDL_GAMEPAD_BUTTON_START, PAD_START},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, PAD_L3},    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, PAD_R3},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_UP},       {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_LEFT},   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_RIGHT},
    };
    out->connected = true;
    for (const auto &m : map)
        if (SDL_GetGamepadButton(gp, m.b)) out->buttons |= m.btn;
    auto trigger = [&](SDL_GamepadAxis a) {
        int v = SDL_GetGamepadAxis(gp, a) >> 7;  // 0..32767 -> 0..255
        return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    };
    uint8_t lt = trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), rt = trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    if (lt > 30) out->buttons |= PAD_L2;
    if (rt > 30) out->buttons |= PAD_R2;
    out->pressure[10] = lt;
    out->pressure[11] = rt;
    // SDL's Y axes point down like the DualShock's
    out->lx = stick(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX));
    out->ly = stick(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY));
    out->rx = stick(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX));
    out->ry = stick(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY));
}

void toggle_fullscreen() {
    if (g_window) SDL_SetWindowFullscreen(g_window, !platform_fullscreen());
}

}  // namespace

// ================================================================= API
bool platform_init(const PlatformConfig &cfg) {
    g_cfg = cfg;
    g_aspect = cfg.aspect;
    const char *h = getenv("EOD_HEADLESS");
    g_headless = cfg.headless || (h && h[0] == '1');
    // controllers keep working while the window is in the background (as XInput)
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_InitFlags flags = SDL_INIT_GAMEPAD | SDL_INIT_EVENTS;
    if (!g_headless) flags |= SDL_INIT_VIDEO;
    if (!SDL_Init(flags)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    build_key_map();
    int n = 0;
    if (SDL_JoystickID *ids = SDL_GetGamepads(&n)) {
        for (int i = 0; i < n; i++) pad_added(ids[i]);
        SDL_free(ids);
    }
    if (g_headless) return true;
    SDL_WindowFlags wf = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (cfg.fullscreen) wf |= SDL_WINDOW_FULLSCREEN;
    g_window = SDL_CreateWindow(cfg.title, cfg.width > 0 ? cfg.width : 1280, cfg.height > 0 ? cfg.height : 960, wf);
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
    return true;
}

void platform_shutdown() {
    if (g_audio) SDL_DestroyAudioStream(g_audio);
    g_audio = nullptr;
    for (int p = 0; p < kPorts; p++)
        if (g_port_pad[p]) SDL_CloseGamepad(g_port_pad[p]);
    if (g_texture) SDL_DestroyTexture(g_texture);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();
}

void platform_fatal_message(const char *msg) {
    if (!g_headless) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Eve of Destruction - fatal error", msg, g_window);
}

bool platform_pump() {
    SDL_Event e;
    bool quit = false;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            quit = true;
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            pad_added(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            pad_removed(e.gdevice.which);
            break;
        case SDL_EVENT_KEY_DOWN:
            // F11 or Alt+Enter: fullscreen (as on Windows)
            if (!e.key.repeat && (e.key.scancode == SDL_SCANCODE_F11 ||
                                  (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT))))
                toggle_fullscreen();
            break;
        default:
            break;
        }
    }
    for (int p = 0; p < kPorts; p++)
        if (g_rumble[p].small_motor || g_rumble[p].large_motor) rumble(p, g_rumble[p].small_motor, g_rumble[p].large_motor);
    return !quit;
}

void platform_present(const uint32_t *pixels, int width, int height, int stride) {
    if (!g_renderer) return;
    if (!g_texture || g_tex_w != width || g_tex_h != height) {
        if (g_texture) SDL_DestroyTexture(g_texture);
        // bytes R, G, B, A in memory
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

void platform_set_aspect(int aspect) { g_aspect = aspect; }

void platform_display_rect(int cw, int ch, int *x, int *y, int *w, int *h) {
    int vw = cw, vh = ch, aspect = g_aspect;
    if (aspect != 2) {  // 4:3 or 16:9, letterboxed
        int an = aspect == 1 ? 16 : 4, ad = aspect == 1 ? 9 : 3;
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

void *platform_native_window() { return nullptr; }

void platform_set_fullscreen(bool on) {
    if (g_window) SDL_SetWindowFullscreen(g_window, on);
}

bool platform_fullscreen() { return g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0; }

void platform_set_status(const char *text) {
    if (!g_window) return;
    char buf[256];
    snprintf(buf, sizeof buf, "%s - %s", g_cfg.title, text);
    SDL_SetWindowTitle(g_window, buf);
}

// ------------------------------------------------------------------ input
void platform_get_pad(int port, HostPadState *out) {
    *out = HostPadState();
    if (port == 0) {
        // keyboard (always on port 0; SDL clears it when the window loses focus)
        out->connected = true;
        if (g_window) {
            const bool *keys = SDL_GetKeyboardState(nullptr);
            for (const KeyBind &m : g_key_map)
                if (keys[m.sc]) out->buttons |= m.btn;
        }
        // the direction keys also move the left stick (steering)
        if (out->buttons & PAD_LEFT) out->lx = 0;
        if (out->buttons & PAD_RIGHT) out->lx = 255;
        if (out->buttons & PAD_UP) out->ly = 0;
        if (out->buttons & PAD_DOWN) out->ly = 255;
    }
    HostPadState c;
    platform_get_controller(port, &c);
    if (c.connected) {
        out->connected = true;
        out->buttons |= c.buttons;
        if (c.lx != 128) out->lx = c.lx;  // (the stick outside its dead zone)
        if (c.ly != 128) out->ly = c.ly;
        out->rx = c.rx;
        out->ry = c.ry;
        out->pressure[10] = c.pressure[10];
        out->pressure[11] = c.pressure[11];
    }
    // EOD_PAD_SCRIPT (automated testing) replaces port 0's input
    uint16_t sb;
    if (port == 0 && pad_script_buttons(sb)) {
        *out = HostPadState();
        out->connected = true;
        out->buttons = sb;
    }
    // pressure for digital buttons
    static const uint16_t order[12] = {PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
                                       PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2};
    for (int i = 0; i < 12; i++)
        if ((out->buttons & order[i]) && out->pressure[i] == 0) out->pressure[i] = 255;
}

void platform_get_controller(int port, HostPadState *out) {
    *out = HostPadState();
    if (port >= 0 && port < kPorts && g_port_pad[port]) read_gamepad(g_port_pad[port], out);
}

void platform_set_vibration(int port, uint8_t small_motor, uint8_t large_motor) {
    if (!g_config.vibration) small_motor = large_motor = 0;
    rumble(port, small_motor, large_motor);
}

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
    SDL_ResumeAudioStreamDevice(g_audio);
    return true;
}

void platform_audio_submit(const int16_t *samples, int frames) {
    if (!g_audio || frames <= 0) return;
    SDL_PutAudioStreamData(g_audio, samples, frames * 4);
}

int platform_audio_queued_frames() { return g_audio ? SDL_GetAudioStreamQueued(g_audio) / 4 : 0; }
