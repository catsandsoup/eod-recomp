// sdl_input.cpp - SDL3 game controllers on the two player ports (see sdl_input.h).
#include "sdl_input.h"

#include <cstdlib>
#include <cstring>

namespace sdl_input {

namespace {

int g_keyboard_port = 0;
bool g_modern_layout = false;
SDL_JoystickID g_port_pad[kPorts];
SDL_Gamepad *g_port_gamepad[kPorts];

// rumble: SDL effects end after a duration while a DualShock motor stays on
// until the game changes it, so a running effect is renewed (update_rumble)
struct Rumble {
    uint8_t small_motor, large_motor;
    uint64_t sent_ms;
};
Rumble g_rumble[kPorts];
constexpr Uint32 kRumbleMs = 1000;

const char *driver_name(SDL_GUID guid) {
    switch (guid.data[14]) {
    case 'h': return "HIDAPI";
    case 'v': return "virtual";
    case 'm': return "GameController.framework";
    case 'x': return "XInput";
    case 'w': return "Windows.Gaming.Input";
    case 'r': return "RawInput";
    case 'g': return "GameInput";
    case 0:
#ifdef __APPLE__
        return "IOKit HID";
#else
        return "platform";
#endif
    default: return "other";
    }
}

const char *bus_name(SDL_GUID guid) {
    switch (guid.data[0] | guid.data[1] << 8) {
    case 0x03: return "USB";
    case 0x05: return "Bluetooth";
    case 0xFF: return "virtual";
    default: return "unknown bus";
    }
}

const char *type_name(SDL_JoystickID id) {
    const char *t = SDL_GetGamepadStringForType(SDL_GetGamepadTypeForID(id));
    return t ? t : "unknown";
}

const char *pad_name(SDL_JoystickID id) {
    const char *n = SDL_GetGamepadNameForID(id);
    if (!n) n = SDL_GetJoystickNameForID(id);
    return n ? n : "controller";
}

int free_port() {
    for (int p = 0; p < kPorts; p++)
        if (p != g_keyboard_port && !g_port_pad[p]) return p;
    if (g_keyboard_port >= 0 && !g_port_pad[g_keyboard_port]) return g_keyboard_port;  // shared with the keyboard
    return -1;
}

void stop_rumble(int port) {
    if (g_port_gamepad[port] && (g_rumble[port].small_motor || g_rumble[port].large_motor))
        SDL_RumbleGamepad(g_port_gamepad[port], 0, 0, 0);
    g_rumble[port] = Rumble();
}

}  // namespace

void set_hints() {
    // Xbox 360 wired pads have no macOS driver: SDL's HIDAPI driver talks to
    // them over libusb (SDL3 must be built with it, EOD_BUNDLED_SDL).
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_HIDAPI_LIBUSB, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_PLAYER_LED, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
}

int keyboard_port_from_env() {
    const char *kp = getenv("EOD_KEYBOARD_PORT");
    if (!kp || !kp[0]) return 0;
    if (!strcmp(kp, "none") || !strcmp(kp, "off")) return -1;
    int n = atoi(kp);
    if (n == 0) return 0;  // older scripts used 0-based "0" for player 1
    return n <= kPorts ? n - 1 : -1;
}

void set_controller_layout(bool modern) { g_modern_layout = modern; }

void set_keyboard_port(int port) { g_keyboard_port = port >= 0 && port < kPorts ? port : -1; }
int keyboard_port() { return g_keyboard_port; }

SDL_Gamepad *port_gamepad(int port) { return port >= 0 && port < kPorts ? g_port_gamepad[port] : nullptr; }

int port_of(SDL_JoystickID id) {
    if (!id) return -1;
    for (int p = 0; p < kPorts; p++)
        if (g_port_pad[p] == id) return p;
    return -1;
}

void on_added(SDL_JoystickID id) {
    if (!id || port_of(id) >= 0) return;
    int p = free_port();
    if (p < 0) {
        fprintf(stderr, "[input] %s connected, but both player ports are taken\n", pad_name(id));
        return;
    }
    SDL_Gamepad *gp = SDL_OpenGamepad(id);
    if (!gp) {
        fprintf(stderr, "[input] cannot open %s: %s\n", pad_name(id), SDL_GetError());
        return;
    }
    g_port_pad[p] = id;
    g_port_gamepad[p] = gp;
    g_rumble[p] = Rumble();
    SDL_SetGamepadPlayerIndex(gp, p);  // Xbox 360 ring LED shows the player
    fprintf(stderr, "[input] player %d: %s (%04x:%04x, %s, %s)%s\n", p + 1, pad_name(id), SDL_GetGamepadVendor(gp),
            SDL_GetGamepadProduct(gp), type_name(id), driver_name(SDL_GetGamepadGUIDForID(id)),
            p == g_keyboard_port ? " with the keyboard" : "");
}

void on_removed(SDL_JoystickID id) {
    int p = port_of(id);
    if (p < 0) return;
    stop_rumble(p);
    SDL_CloseGamepad(g_port_gamepad[p]);
    g_port_pad[p] = 0;
    g_port_gamepad[p] = nullptr;
    fprintf(stderr, "[input] player %d: controller removed\n", p + 1);
    assign_waiting();
}

void assign_waiting() {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (!ids) return;
    for (int i = 0; i < n && free_port() >= 0; i++)
        if (port_of(ids[i]) < 0) on_added(ids[i]);
    SDL_free(ids);
}

void close_all() {
    for (int p = 0; p < kPorts; p++) {
        if (!g_port_gamepad[p]) continue;
        stop_rumble(p);
        SDL_CloseGamepad(g_port_gamepad[p]);
        g_port_gamepad[p] = nullptr;
        g_port_pad[p] = 0;
    }
}

uint8_t stick_byte(Sint16 v) {
    // dead zone of 1/8 of the travel around the centre (the Win32 backend's
    // 112..144), the rest rescaled so the stick leaves 128 smoothly
    constexpr int dz = 4096;
    int m = v < 0 ? -(int)v : v;
    if (m <= dz) return 128;
    int s = (m - dz) * 128 / (32768 - dz);  // 0..128
    int a = v < 0 ? 128 - s : 128 + s;
    return (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a);
}

uint8_t trigger_byte(Sint16 v) {
    int a = v >> 7;
    return (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a);
}

void read_gamepad(SDL_Gamepad *gp, HostPadState *out) {
    static const struct {
        SDL_GamepadButton b;
        uint16_t btn;
    } map[] = {
        // Xbox face buttons by position: A = Cross, B = Circle, X = Square, Y = Triangle
        {SDL_GAMEPAD_BUTTON_SOUTH, PAD_CROSS},        {SDL_GAMEPAD_BUTTON_EAST, PAD_CIRCLE},
        {SDL_GAMEPAD_BUTTON_WEST, PAD_SQUARE},        {SDL_GAMEPAD_BUTTON_NORTH, PAD_TRIANGLE},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_L1},   {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_R1},
        {SDL_GAMEPAD_BUTTON_BACK, PAD_SELECT},        {SDL_GAMEPAD_BUTTON_START, PAD_START},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, PAD_L3},      {SDL_GAMEPAD_BUTTON_RIGHT_STICK, PAD_R3},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_UP},         {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_LEFT},     {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_RIGHT},
    };
    out->connected = true;
    for (const auto &m : map)
        if (SDL_GetGamepadButton(gp, m.b)) out->buttons |= m.btn;
    // Original mirrors PS2 L2/R2. Modern feeds the game's classic analog
    // throttle/brake channels (Cross/Square) and leaves L2/R2 released so
    // pulling a trigger cannot also invoke its original shoulder action.
    uint8_t lt = trigger_byte(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    uint8_t rt = trigger_byte(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    int left_pressure = g_modern_layout ? 7 : 10;
    int right_pressure = g_modern_layout ? 6 : 11;
    uint16_t left_button = g_modern_layout ? PAD_SQUARE : PAD_L2;
    uint16_t right_button = g_modern_layout ? PAD_CROSS : PAD_R2;
    // Physical face buttons and a keyboard sharing this player are full
    // travel; a partial trigger must not reduce their existing pressure.
    if (g_modern_layout) {
        if (out->buttons & PAD_CROSS) out->pressure[6] = 255;
        if (out->buttons & PAD_SQUARE) out->pressure[7] = 255;
    }
    if (lt > kTriggerDigital) out->buttons |= left_button;
    if (rt > kTriggerDigital) out->buttons |= right_button;
    if (lt > out->pressure[left_pressure]) out->pressure[left_pressure] = lt;
    if (rt > out->pressure[right_pressure]) out->pressure[right_pressure] = rt;
    // SDL's Y axes point down like the DualShock's (0 = up); a centred stick
    // leaves the keyboard's steering alone when both share a port
    uint8_t lx = stick_byte(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX));
    uint8_t ly = stick_byte(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY));
    uint8_t rx = stick_byte(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX));
    uint8_t ry = stick_byte(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY));
    if (lx != 128) out->lx = lx;
    if (ly != 128) out->ly = ly;
    if (rx != 128) out->rx = rx;
    if (ry != 128) out->ry = ry;
}

bool rumble(int port, uint8_t small_motor, uint8_t large_motor) {
    if (port < 0 || port >= kPorts || !g_port_gamepad[port]) return false;
    Rumble &r = g_rumble[port];
    uint64_t now = SDL_GetTicks();
    bool changed = r.small_motor != small_motor || r.large_motor != large_motor;
    bool on = small_motor || large_motor;
    if (!changed && !(on && now - r.sent_ms >= kRumbleMs / 2)) return true;
    r.small_motor = small_motor;
    r.large_motor = large_motor;
    r.sent_ms = now;
    // large motor = low frequency (0-255); the small one is on/off on a DualShock 2
    return SDL_RumbleGamepad(g_port_gamepad[port], (Uint16)(large_motor * 257), small_motor ? 0xFFFF : 0,
                             on ? kRumbleMs : 0);
}

void update_rumble() {
    for (int p = 0; p < kPorts; p++)
        if (g_rumble[p].small_motor || g_rumble[p].large_motor) rumble(p, g_rumble[p].small_motor, g_rumble[p].large_motor);
}

// ------------------------------------------------------------- logging
bool log_enabled() {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("EOD_INPUT_LOG");
        on = e && e[0] && e[0] != '0';
    }
    return on;
}

void format_state(const HostPadState &s, char *buf, size_t size) {
    static const char *names[16] = {"Select", "L3", "R3",  "Start", "Up", "Right",    "Down",   "Left",
                                    "L2",     "R2", "L1",  "R1",    "Triangle", "Circle", "Cross", "Square"};
    char btn[160] = "";
    for (int i = 0; i < 16; i++)
        if (s.buttons & (1u << i)) {
            if (btn[0]) strncat(btn, "+", sizeof btn - strlen(btn) - 1);
            strncat(btn, names[i], sizeof btn - strlen(btn) - 1);
        }
    snprintf(buf, size, "%s Cross=%3d Square=%3d L2=%3d R2=%3d left=(%3d,%3d) right=(%3d,%3d)%s", btn[0] ? btn : "-", s.pressure[6], s.pressure[7], s.pressure[10],
             s.pressure[11], s.lx, s.ly, s.rx, s.ry, s.connected ? "" : " (disconnected)");
}

void log_state(int port, const HostPadState &s) {
    static HostPadState last[kPorts];
    static bool seen[kPorts];
    if (port < 0 || port >= kPorts) return;
    const HostPadState &o = last[port];
    auto moved = [](uint8_t a, uint8_t b, uint8_t rest) {
        int d = a > b ? a - b : b - a;
        return d >= 8 || (a != b && (a == rest || a == 0 || a == 255));
    };
    bool changed = !seen[port] || s.connected != o.connected || s.buttons != o.buttons ||
                   moved(s.lx, o.lx, 128) || moved(s.ly, o.ly, 128) || moved(s.rx, o.rx, 128) ||
                   moved(s.ry, o.ry, 128) || moved(s.pressure[10], o.pressure[10], 0) ||
                   moved(s.pressure[11], o.pressure[11], 0) || moved(s.pressure[6], o.pressure[6], 0) ||
                   moved(s.pressure[7], o.pressure[7], 0);
    if (!changed) return;
    seen[port] = true;
    last[port] = s;
    char buf[256];
    format_state(s, buf, sizeof buf);
    fprintf(stderr, "[input] P%d %s\n", port + 1, buf);
}

// ------------------------------------------------------------ --list-pads
int list_pads(int watch_seconds) {
    set_hints();
    if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    int v = SDL_GetVersion();
    printf("SDL %d.%d.%d (%s)\n", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v),
           SDL_GetRevision());
#ifdef EOD_SDL_LIBUSB
    printf("HIDAPI over libusb: built in (Xbox 360 wired pads supported)\n");
#else
    printf("HIDAPI over libusb: external SDL build (capability unknown)\n"
           "  (rebuild with -DEOD_BUNDLED_SDL=ON and brew install libusb)\n");
#endif
    // devices finish their HIDAPI handshake over the next few event pumps
    for (int i = 0; i < 50; i++) {
        SDL_PumpEvents();
        SDL_Delay(20);
    }

    int n = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&n);
    printf("\nControllers: %d\n", n);
    for (int i = 0; i < n; i++) {
        SDL_JoystickID id = ids[i];
        SDL_GUID guid = SDL_GetJoystickGUIDForID(id);
        const char *path = SDL_GetJoystickPathForID(id);
        bool is_gamepad = SDL_IsGamepad(id);
        printf("  #%d %s\n", i + 1, pad_name(id));
        printf("     VID:PID %04x:%04x  type %s  driver %s  %s\n", SDL_GetJoystickVendorForID(id),
               SDL_GetJoystickProductForID(id), type_name(id), driver_name(guid), bus_name(guid));
        if (path) printf("     path %s\n", path);
        if (is_gamepad) {
            SDL_Gamepad *gp = SDL_OpenGamepad(id);
            if (gp) {
                SDL_PropertiesID props = SDL_GetGamepadProperties(gp);
                bool rumble_cap = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
                printf("     opened as gamepad: yes  rumble: %s\n", rumble_cap ? "yes" : "no");
                SDL_CloseGamepad(gp);
            } else {
                printf("     opened as gamepad: FAILED (%s)\n", SDL_GetError());
            }
            char *mapping = SDL_GetGamepadMappingForID(id);
            printf("     mapping %s\n", mapping ? mapping : "(none)");
            SDL_free(mapping);
        } else {
            SDL_Joystick *j = SDL_OpenJoystick(id);
            printf("     opened as gamepad: no (no mapping) - joystick %s", j ? "opened" : "failed to open");
            if (j)
                printf(", %d axes, %d buttons, %d hats", SDL_GetNumJoystickAxes(j), SDL_GetNumJoystickButtons(j),
                       SDL_GetNumJoystickHats(j));
            printf("\n");
            if (j) SDL_CloseJoystick(j);
        }
    }
    SDL_free(ids);

    // what SDL's HIDAPI layer sees underneath (an Xbox 360 pad shows up here
    // through libusb even when no driver picked it up)
    SDL_hid_device_info *devs = SDL_hid_enumerate(0, 0);
    int nh = 0;
    for (SDL_hid_device_info *d = devs; d; d = d->next) nh++;
    printf("\nHID devices seen by SDL's HIDAPI: %d\n", nh);
    for (SDL_hid_device_info *d = devs; d; d = d->next) {
        bool x360 = d->interface_class == 0xFF && d->interface_subclass == 93 && d->interface_protocol == 1;
        printf("  %04x:%04x  %ls %ls  interface %d (class %02x/%02x/%02x)%s\n", d->vendor_id, d->product_id,
               d->manufacturer_string ? d->manufacturer_string : L"", d->product_string ? d->product_string : L"",
               d->interface_number, d->interface_class, d->interface_subclass, d->interface_protocol,
               x360 ? "  <- Xbox 360 wired pad (libusb)" : "");
    }
    SDL_hid_free_enumeration(devs);

    if (n == 0)
        printf("\nNo controllers. For an Xbox 360 wired pad (Rock Candy etc.): check it is listed by\n"
               "`system_profiler SPUSBDataType` (vendor 0x0e6f for PDP), try another cable/port\n"
               "(not a hub), and quit other programs that may hold it (Steam, emulators).\n");

    if (watch_seconds > 0) {
        printf("\nWatching for %d s: press buttons, move sticks, pull triggers, unplug/replug.\n"
               "Each controller rumbles briefly when it gets a player port.\n",
               watch_seconds);
        fflush(stdout);
        set_keyboard_port(keyboard_port_from_env());
        printf("Keyboard: %s (no window here, so keys are not read)\n",
               g_keyboard_port < 0 ? "no player" : g_keyboard_port == 0 ? "player 1" : "player 2");
        int nn = 0;
        SDL_JoystickID *gids = SDL_GetGamepads(&nn);
        for (int i = 0; i < nn; i++) on_added(gids[i]);
        SDL_free(gids);
        uint64_t end = SDL_GetTicks() + (uint64_t)watch_seconds * 1000;
        uint64_t rumble_until[kPorts] = {};
        bool had_pad[kPorts] = {};
        while (SDL_GetTicks() < end) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
                    on_added(e.gdevice.which);
                } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
                    on_removed(e.gdevice.which);
                }
            }
            for (int p = 0; p < kPorts; p++) {
                SDL_Gamepad *gp = g_port_gamepad[p];
                if (gp && !rumble_until[p]) {
                    rumble(p, 1, 160);
                    rumble_until[p] = SDL_GetTicks() + 300;
                } else if (!gp) {
                    rumble_until[p] = 0;
                } else if (rumble_until[p] > 1 && SDL_GetTicks() >= rumble_until[p]) {
                    rumble(p, 0, 0);
                    rumble_until[p] = 1;
                }
                HostPadState s;
                if (gp) read_gamepad(gp, &s);
                if (gp || had_pad[p]) log_state(p, s);
                had_pad[p] = gp != nullptr;
            }
            update_rumble();
            SDL_Delay(8);
        }
        close_all();
    }
    SDL_Quit();
    return 0;
}

}  // namespace sdl_input
