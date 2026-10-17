// input_test.cpp - controller checks without hardware (ctest: input).
//
// SDL virtual joysticks pose as Xbox 360 wired pads (Microsoft 045e:028e and
// PDP Rock Candy 0e6f:011f) and go through the same code the game uses
// (runtime/platform/sdl3/sdl_input.cpp): player port assignment with the
// keyboard, every button, trigger -> L2/R2 pressure, sticks with dead zone,
// rumble, and hot-plug. Real controller drivers are switched off so a pad
// plugged into the machine does not disturb the result.
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "platform/sdl3/sdl_input.h"

namespace {

int g_fail, g_pass;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (cond) {                                       \
            g_pass++;                                     \
        } else {                                          \
            g_fail++;                                     \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

struct Pad {
    SDL_JoystickID id = 0;
    SDL_Joystick *joy = nullptr;  // for setting the virtual inputs
    Uint16 rumble_low = 0, rumble_high = 0;
    int rumble_calls = 0;
};

bool SDLCALL on_rumble(void *userdata, Uint16 low, Uint16 high) {
    Pad *p = (Pad *)userdata;
    p->rumble_low = low;
    p->rumble_high = high;
    p->rumble_calls++;
    return true;
}

// what platform_pump does with controller events
void pump() {
    SDL_UpdateJoysticks();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) sdl_input::on_added(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) sdl_input::on_removed(e.gdevice.which);
    }
}

void attach(Pad &p, Uint16 vid, Uint16 pid, const char *name) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.vendor_id = vid;
    d.product_id = pid;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;          // LX LY RX RY LT RT, as an Xbox 360 pad
    d.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;  // A B X Y Back Guide Start L3 R3 LB RB d-pad
    d.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    d.button_mask = (1u << (SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1)) - 1;
    d.name = name;
    d.userdata = &p;
    d.Rumble = on_rumble;
    p = Pad();
    d.userdata = &p;
    p.id = SDL_AttachVirtualJoystick(&d);
    if (!p.id) {
        printf("SDL_AttachVirtualJoystick: %s\n", SDL_GetError());
        exit(2);
    }
    p.joy = SDL_OpenJoystick(p.id);
    pump();
}

void detach(Pad &p) {
    SDL_CloseJoystick(p.joy);
    SDL_DetachVirtualJoystick(p.id);
    p.joy = nullptr;
    pump();
}

HostPadState read_port(int port) {
    HostPadState s;
    if (SDL_Gamepad *gp = sdl_input::port_gamepad(port)) sdl_input::read_gamepad(gp, &s);
    return s;
}

void set_button(Pad &p, SDL_GamepadButton b, bool down) {
    SDL_SetJoystickVirtualButton(p.joy, b, down);
    SDL_UpdateJoysticks();
}

void set_axis(Pad &p, SDL_GamepadAxis a, Sint16 v) {
    SDL_SetJoystickVirtualAxis(p.joy, a, v);
    SDL_UpdateJoysticks();
}

// trigger at rest: SDL maps the virtual trigger axis' full range to 0..32767
constexpr Sint16 kTriggerRest = SDL_JOYSTICK_AXIS_MIN;

void test_conversions() {
    CHECK(sdl_input::stick_byte(0) == 128, "centre");
    CHECK(sdl_input::stick_byte(3000) == 128 && sdl_input::stick_byte(-3000) == 128, "inside the dead zone");
    CHECK(sdl_input::stick_byte(-32768) == 0, "full left/up = %d", sdl_input::stick_byte(-32768));
    CHECK(sdl_input::stick_byte(32767) == 255, "full right/down = %d", sdl_input::stick_byte(32767));
    int prev = 0;
    bool mono = true;
    for (int v = -32768; v <= 32767; v += 64) {
        int b = sdl_input::stick_byte((Sint16)v);
        if (b < prev) mono = false;
        prev = b;
    }
    CHECK(mono, "stick mapping is monotonic");
    // just outside the dead zone the stick moves by one step, not a jump
    CHECK(sdl_input::stick_byte(4400) <= 130 && sdl_input::stick_byte(4400) > 128, "%d",
          sdl_input::stick_byte(4400));
    CHECK(sdl_input::trigger_byte(0) == 0 && sdl_input::trigger_byte(32767) == 255, "trigger range");

    const char *old = getenv("EOD_KEYBOARD_PORT");
    (void)old;
    unsetenv("EOD_KEYBOARD_PORT");
    CHECK(sdl_input::keyboard_port_from_env() == -2, "keyboard defaults to automatic");
    setenv("EOD_KEYBOARD_PORT", "2", 1);
    CHECK(sdl_input::keyboard_port_from_env() == 1, "EOD_KEYBOARD_PORT=2");
    setenv("EOD_KEYBOARD_PORT", "none", 1);
    CHECK(sdl_input::keyboard_port_from_env() == -1, "EOD_KEYBOARD_PORT=none");
    unsetenv("EOD_KEYBOARD_PORT");
}

void test_rock_candy_with_keyboard() {
    sdl_input::set_keyboard_port(0);
    Pad rc;
    attach(rc, 0x0E6F, 0x011F, "PDP Xbox 360 Rock Candy");
    CHECK(SDL_IsGamepad(rc.id), "virtual 360 pad is a gamepad");
    CHECK(SDL_GetGamepadTypeForID(rc.id) == SDL_GAMEPAD_TYPE_XBOX360, "PDP 0e6f:011f is an Xbox 360 pad (type %d)",
          (int)SDL_GetGamepadTypeForID(rc.id));
    CHECK(sdl_input::port_of(rc.id) == 1, "keyboard + pad: pad is player 2 (port %d)", sdl_input::port_of(rc.id));
    CHECK(!sdl_input::port_gamepad(0), "keyboard port has no pad");
    auto help = sdl_input::help_text(true, "X,Space");
    CHECK(help.find("Player 1: Keyboard - Enter") != std::string::npos, "help identifies the keyboard's actual port");
    CHECK(help.find("Player 2: PDP Xbox 360 Rock Candy - bottom face button (A)") != std::string::npos,
          "help identifies the assigned P2 controller and physical Accept button");

    // at rest
    set_axis(rc, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, kTriggerRest);
    set_axis(rc, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, kTriggerRest);
    HostPadState s = read_port(1);
    CHECK(s.connected && s.buttons == 0, "rest buttons %04x", s.buttons);
    CHECK(s.lx == 128 && s.ly == 128 && s.rx == 128 && s.ry == 128, "rest sticks");
    CHECK(s.pressure[10] == 0 && s.pressure[11] == 0, "rest triggers %d %d", s.pressure[10], s.pressure[11]);

    // every button, alone
    static const struct {
        SDL_GamepadButton b;
        uint16_t want;
        const char *name;
    } buttons[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, PAD_CROSS, "A = Cross"},
        {SDL_GAMEPAD_BUTTON_EAST, PAD_CIRCLE, "B = Circle"},
        {SDL_GAMEPAD_BUTTON_WEST, PAD_SQUARE, "X = Square"},
        {SDL_GAMEPAD_BUTTON_NORTH, PAD_TRIANGLE, "Y = Triangle"},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_L1, "LB = L1"},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_R1, "RB = R1"},
        {SDL_GAMEPAD_BUTTON_BACK, PAD_SELECT, "Back = Select"},
        {SDL_GAMEPAD_BUTTON_START, PAD_START, "Start = Start"},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, PAD_L3, "left stick click = L3"},
        {SDL_GAMEPAD_BUTTON_RIGHT_STICK, PAD_R3, "right stick click = R3"},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_UP, "d-pad up"},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_DOWN, "d-pad down"},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_LEFT, "d-pad left"},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_RIGHT, "d-pad right"},
    };
    for (const auto &b : buttons) {
        set_button(rc, b.b, true);
        s = read_port(1);
        CHECK(s.buttons == b.want, "%s: got %04x want %04x", b.name, s.buttons, b.want);
        set_button(rc, b.b, false);
    }
    set_button(rc, SDL_GAMEPAD_BUTTON_GUIDE, true);
    CHECK(read_port(1).buttons == 0, "Guide is not mapped");
    set_button(rc, SDL_GAMEPAD_BUTTON_GUIDE, false);

    // triggers: analog throttle/brake -> L2/R2 pressure + digital bit
    set_axis(rc, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    s = read_port(1);
    CHECK(s.pressure[11] == 255 && (s.buttons & PAD_R2), "RT full: R2 pressure %d buttons %04x", s.pressure[11],
          s.buttons);
    CHECK(s.pressure[10] == 0 && !(s.buttons & PAD_L2), "LT untouched");
    set_axis(rc, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);  // half travel
    s = read_port(1);
    CHECK(s.pressure[11] > 100 && s.pressure[11] < 160 && (s.buttons & PAD_R2), "RT half: pressure %d",
          s.pressure[11]);
    set_axis(rc, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -30000);  // a touch
    s = read_port(1);
    CHECK(s.pressure[11] > 0 && s.pressure[11] <= sdl_input::kTriggerDigital && !(s.buttons & PAD_R2),
          "RT light touch: pressure %d, no digital bit", s.pressure[11]);
    set_axis(rc, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, kTriggerRest);
    set_axis(rc, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    s = read_port(1);
    CHECK(s.pressure[10] == 255 && (s.buttons & PAD_L2) && s.pressure[11] == 0, "LT full: L2 pressure %d",
          s.pressure[10]);
    set_axis(rc, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, kTriggerRest);

    // sticks: 0..255, up = 0 like the DualShock, dead zone around the centre
    set_axis(rc, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MIN);
    set_axis(rc, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MIN);
    s = read_port(1);
    CHECK(s.lx == 0 && s.ly == 0, "left stick up-left: %d,%d", s.lx, s.ly);
    set_axis(rc, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX);
    set_axis(rc, SDL_GAMEPAD_AXIS_LEFTY, SDL_JOYSTICK_AXIS_MAX);
    s = read_port(1);
    CHECK(s.lx == 255 && s.ly == 255, "left stick down-right: %d,%d", s.lx, s.ly);
    set_axis(rc, SDL_GAMEPAD_AXIS_LEFTX, 2500);
    set_axis(rc, SDL_GAMEPAD_AXIS_LEFTY, -2500);
    s = read_port(1);
    CHECK(s.lx == 128 && s.ly == 128, "left stick drift inside dead zone: %d,%d", s.lx, s.ly);
    set_axis(rc, SDL_GAMEPAD_AXIS_LEFTX, 0);
    set_axis(rc, SDL_GAMEPAD_AXIS_LEFTY, 0);
    set_axis(rc, SDL_GAMEPAD_AXIS_RIGHTX, SDL_JOYSTICK_AXIS_MAX);
    set_axis(rc, SDL_GAMEPAD_AXIS_RIGHTY, SDL_JOYSTICK_AXIS_MIN);
    s = read_port(1);
    CHECK(s.rx == 255 && s.ry == 0 && s.lx == 128, "right stick: %d,%d", s.rx, s.ry);
    set_axis(rc, SDL_GAMEPAD_AXIS_RIGHTX, 0);
    set_axis(rc, SDL_GAMEPAD_AXIS_RIGHTY, 0);

    // rumble: large motor = low frequency, small motor (on/off) = high frequency
    CHECK(sdl_input::rumble(1, 1, 128), "rumble on a pad");
    CHECK(rc.rumble_low == 128 * 257 && rc.rumble_high == 0xFFFF, "rumble %04x %04x", rc.rumble_low,
          rc.rumble_high);
    int calls = rc.rumble_calls;
    sdl_input::rumble(1, 1, 128);  // unchanged: not resent
    CHECK(rc.rumble_calls == calls, "unchanged rumble not resent");
    sdl_input::rumble(1, 0, 0);
    CHECK(rc.rumble_low == 0 && rc.rumble_high == 0, "rumble off");
    CHECK(!sdl_input::rumble(0, 1, 255), "no rumble on the keyboard port");

    set_button(rc, SDL_GAMEPAD_BUTTON_SOUTH, true);
    HostPadState unfocused;
    sdl_input::read_gamepad(sdl_input::port_gamepad(1), &unfocused, false);
    CHECK(unfocused.connected && unfocused.buttons==0, "focus loss neutralizes held gamepad input");
    CHECK(read_port(1).buttons==PAD_CROSS, "focus return restores held gamepad input");
    set_button(rc, SDL_GAMEPAD_BUTTON_SOUTH, false);

    // a second pad: both ports now have a pad (the keyboard shares player 1)
    Pad ms;
    attach(ms, 0x045E, 0x028E, "Xbox 360 Controller");
    CHECK(sdl_input::port_of(ms.id) == 0, "second pad joins the keyboard on player 1 (port %d)",
          sdl_input::port_of(ms.id));
    // players are independent
    set_button(ms, SDL_GAMEPAD_BUTTON_SOUTH, true);
    set_axis(rc, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    HostPadState p1 = read_port(0), p2 = read_port(1);
    CHECK(p1.buttons == PAD_CROSS && p1.pressure[11] == 0, "P1 only Cross: %04x", p1.buttons);
    CHECK(p2.buttons == PAD_R2 && p2.pressure[11] == 255, "P2 only R2: %04x", p2.buttons);
    set_button(ms, SDL_GAMEPAD_BUTTON_SOUTH, false);
    set_axis(rc, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, kTriggerRest);

    // a third pad waits for a free port
    Pad extra;
    attach(extra, 0x0E6F, 0x021F, "PDP Xbox 360 Rock Candy");
    CHECK(sdl_input::port_of(extra.id) == -1, "third pad has no port");

    // hot-plug: unplugging player 2 frees the port; the waiting pad takes it
    sdl_input::rumble(1, 1, 200);
    detach(rc);
    CHECK(sdl_input::port_of(rc.id) == -1, "unplugged pad left its port");
    CHECK(sdl_input::port_of(extra.id) == 1, "waiting pad took player 2 (port %d)", sdl_input::port_of(extra.id));
    CHECK(sdl_input::port_of(ms.id) == 0, "player 1 unaffected");
    detach(extra);
    CHECK(!sdl_input::port_gamepad(1), "player 2 empty after unplug");
    HostPadState gone = read_port(1);
    CHECK(!gone.connected, "empty port reads disconnected (keyboard is on player 1)");
    help = sdl_input::help_text(true, "X,Space");
    CHECK(help.find("Player 2: No device") != std::string::npos && help.find("SINGLE") != std::string::npos,
          "unplugged P2 has actionable single-player guidance");
    CHECK(help.find("Enter belongs to player 1") != std::string::npos,
          "P1 Enter cannot join the now empty P2 port");
    // re-plugging gets player 2 back
    attach(rc, 0x0E6F, 0x011F, "PDP Xbox 360 Rock Candy");
    CHECK(sdl_input::port_of(rc.id) == 1, "re-plugged pad is player 2 again (port %d)", sdl_input::port_of(rc.id));
    set_button(rc, SDL_GAMEPAD_BUTTON_START, true);
    CHECK(read_port(1).buttons == PAD_START, "re-plugged pad reads");
    // unplugging player 1's pad leaves the keyboard there
    detach(ms);
    CHECK(!sdl_input::port_gamepad(0) && sdl_input::port_of(rc.id) == 1, "player 1 back to keyboard only");
    detach(rc);
    sdl_input::close_all();
}

void test_auto_ports() {
    sdl_input::set_keyboard_port(-2); // automatic keyboard fallback
    CHECK(sdl_input::keyboard_port()==0, "keyboard alone controls P1");
    Pad a,b;
    attach(a, 0x0E6F, 0x011F, "PDP Xbox 360 Rock Candy");
    CHECK(sdl_input::port_of(a.id)==0, "lone pad controls P1");
    CHECK(sdl_input::keyboard_port()==1, "keyboard joins P2 with lone pad");
    attach(b, 0x045E, 0x028E, "Xbox 360 Controller");
    CHECK(sdl_input::port_of(b.id)==1, "second pad controls P2");
    detach(a);
    CHECK(sdl_input::keyboard_port()==1, "keyboard does not swap players when P1 disconnects");
    detach(b);
    attach(b, 0x045E, 0x028E, "Xbox 360 Controller");
    CHECK(sdl_input::port_of(b.id)==1, "reconnect restores P2 even with P1 absent");
    attach(a, 0x0E6F, 0x011F, "PDP Xbox 360 Rock Candy");
    CHECK(sdl_input::port_of(a.id)==0, "reconnect restores P1");
    detach(a); detach(b); sdl_input::close_all();
}

void test_identical_pads_keep_live_slots() {
    sdl_input::set_keyboard_port(-2);
    Pad a,b;
    attach(a, 0x0E6F, 0x011F, "PDP Xbox 360 Rock Candy");
    attach(b, 0x0E6F, 0x011F, "PDP Xbox 360 Rock Candy");
    CHECK(sdl_input::port_of(a.id)==0 && sdl_input::port_of(b.id)==1, "identical pads occupy separate slots");
    detach(a);
    CHECK(sdl_input::port_of(b.id)==1, "disconnect does not swap identical live P2");
    attach(a, 0x0E6F, 0x011F, "PDP Xbox 360 Rock Candy");
    CHECK(sdl_input::port_of(a.id)==0 && sdl_input::port_of(b.id)==1, "reconnect cannot steal live P2");
    detach(a); detach(b); sdl_input::close_all();
}

void test_two_pads_no_keyboard() {
    sdl_input::set_keyboard_port(-1);
    Pad a, b;
    attach(a, 0x0E6F, 0x011F, "PDP Xbox 360 Rock Candy");
    attach(b, 0x045E, 0x028E, "Xbox 360 Controller");
    CHECK(sdl_input::port_of(a.id) == 0 && sdl_input::port_of(b.id) == 1, "no keyboard: pads are P1, P2 in plug order");
    auto help = sdl_input::help_text(true, "X,Space");
    CHECK(help.find("Player 1: PDP Xbox 360 Rock Candy") != std::string::npos &&
          help.find("Player 2: Xbox 360 Controller") != std::string::npos &&
          help.find("Keyboard - Enter") == std::string::npos, "two-pad help follows plug order with keyboard off");
    detach(a);
    CHECK(sdl_input::port_of(b.id) == 1, "P2 stays P2 when P1 unplugs");
    attach(a, 0x0E6F, 0x011F, "PDP Xbox 360 Rock Candy");
    CHECK(sdl_input::port_of(a.id) == 0, "re-plugged pad is P1 again");
    detach(a);
    detach(b);
    sdl_input::close_all();
}

void test_keyboard_on_player_2() {
    sdl_input::set_keyboard_port(1);
    Pad a;
    attach(a, 0x0E6F, 0x011F, "PDP Xbox 360 Rock Candy");
    CHECK(sdl_input::port_of(a.id) == 0, "EOD_KEYBOARD_PORT=2: pad is player 1");
    CHECK(sdl_input::help_text(true, "X,Space").find("Player 2: Keyboard - Enter") != std::string::npos,
          "P2 keyboard joins with Enter");
    auto original = sdl_input::help_text(false, "Z,C");
    CHECK(original.find("Player 2: Keyboard - Z,C") != std::string::npos &&
          original.find("Keyboard - Enter") == std::string::npos,
          "Original preset help respects custom Cross bindings");
    detach(a);
    sdl_input::close_all();
}

void test_modern_triggers() {
    sdl_input::set_controller_layout(true);
    sdl_input::set_keyboard_port(0);
    Pad a, b;
    attach(a, 0x0E6F, 0x011F, "PDP Xbox 360 Rock Candy");
    attach(b, 0x045E, 0x028E, "Xbox 360 Controller");
    for (Pad *p : {&a, &b}) {
        set_axis(*p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, kTriggerRest);
        set_axis(*p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, kTriggerRest);
    }
    set_axis(a, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0); // half travel, P2
    HostPadState s = read_port(1);
    CHECK(s.buttons == PAD_CROSS && s.pressure[6] > 100 && s.pressure[6] < 160, "RT half is analog Cross");
    CHECK(s.pressure[10] == 0 && s.pressure[11] == 0, "modern has no original L2/R2 pressure");
    CHECK(read_port(0).buttons == 0 && read_port(0).pressure[6] == 0, "P1 unaffected by P2 trigger");
    set_axis(b, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    HostPadState other = read_port(0);
    CHECK(other.buttons == PAD_SQUARE && other.pressure[7] == 255 && other.pressure[6] == 0, "P1 LT brakes independently");
    set_axis(a, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -30000);
    s = read_port(1);
    CHECK(s.buttons == 0 && s.pressure[6] > 0 && s.pressure[6] <= sdl_input::kTriggerDigital, "light RT pressure without digital Cross");
    set_button(a, SDL_GAMEPAD_BUTTON_SOUTH, true);
    CHECK(read_port(1).pressure[6] == 255, "physical A stays full pressure with light RT");
    set_button(a, SDL_GAMEPAD_BUTTON_SOUTH, false);
    set_axis(a, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    set_button(a, SDL_GAMEPAD_BUTTON_WEST, true);
    CHECK(read_port(1).pressure[7] == 255, "physical X stays full pressure with partial LT");
    set_button(a, SDL_GAMEPAD_BUTTON_WEST, false);
    HostPadState keyboard;
    keyboard.connected = true;
    keyboard.buttons = PAD_CROSS | PAD_SQUARE;
    sdl_input::read_gamepad(sdl_input::port_gamepad(1), &keyboard);
    CHECK(keyboard.pressure[6] == 255 && keyboard.pressure[7] == 255, "shared keyboard stays at full pressure");
    s = read_port(1);
    CHECK(s.pressure[6] > 0 && s.pressure[6] < 31 && s.pressure[7] > 100 && s.pressure[7] < 160,
          "released face/keyboard input returns to partial triggers");
    set_axis(a, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, kTriggerRest);
    set_axis(a, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, kTriggerRest);
    s = read_port(1);
    CHECK(s.buttons == 0 && s.pressure[6] == 0 && s.pressure[7] == 0, "released triggers return to neutral");
    sdl_input::set_controller_layout(false);
    set_axis(a, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    s = read_port(1);
    CHECK(s.buttons == PAD_R2 && s.pressure[11] == 255 && s.pressure[6] == 0, "switching back restores Original PS2 mapping");
    detach(a); detach(b); sdl_input::close_all();
}

}  // namespace

int main() {
    // virtual joysticks only: no real controller can change the outcome
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_MFI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_IOKIT, "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        printf("SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    test_conversions();
    test_rock_candy_with_keyboard();
    test_auto_ports();
    test_identical_pads_keep_live_slots();
    test_two_pads_no_keyboard();
    test_keyboard_on_player_2();
    test_modern_triggers();
    SDL_Quit();
    printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
