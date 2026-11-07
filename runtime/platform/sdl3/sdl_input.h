// sdl_input.h - SDL3 game controllers on the two DualShock 2 player ports.
//
// Split out of platform_sdl3.cpp so tools/input_test can drive it with SDL
// virtual joysticks (no hardware) and so `eod --list-pads` runs without game
// data. The keyboard stays in platform_sdl3.cpp; this module only needs to
// know which port it sits on.
//
// Ports: controllers always fill P1 then P2, even when a saved KeyboardPort
// names P1 (the keyboard then shares that port). Automatic keyboard: alone it
// is P1, with a P1 controller it is P2. The stored setting is never rewritten.
// Reconnect restores an available remembered slot by serial/path/GUID. Never
// move a connected controller to make room for a different controller.
#pragma once
#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "../platform.h"

namespace sdl_input {

constexpr int kPorts = 2;

// SDL hints for HIDAPI (Xbox 360 pads over libusb on macOS); call before SDL_Init.
void set_hints();

// Explicit persisted app setting: false = Original PS2, true = Modern RT/LT gas/brake.
void set_controller_layout(bool modern);

// -2 = automatic, 0 or 1 = explicit, -1 = none. reserve_port keeps controllers off the
// keyboard's port (legacy); leave false for saved settings so controllers stay P1/P2.
void set_keyboard_port(int port, bool reserve_port = false);
// EOD_KEYBOARD_PORT: "auto" (default), "1" or "2", "none"
int keyboard_port_from_env();
int keyboard_port();

// Current live assignments and menu Accept controls, for the in-game help.
std::string help_text(bool racing_keyboard, const std::string &cross_keys);

void on_added(SDL_JoystickID id);    // SDL_EVENT_GAMEPAD_ADDED
void on_removed(SDL_JoystickID id);  // SDL_EVENT_GAMEPAD_REMOVED
void assign_waiting();               // gives free ports to connected, unassigned gamepads
void close_all();
// Exchanges the P1 and P2 controllers (front end INPUTS tab).
void swap_ports();
// Name of the device on a port ("" = none, "Keyboard"), and whether its prompts are Xbox style
// (false for PlayStation pads).
std::string port_description(int port, bool *xbox_glyphs);

SDL_Gamepad *port_gamepad(int port);
int port_of(SDL_JoystickID id);  // -1 when not on a port

// SDL gamepad -> DualShock 2: buttons, 0..255 sticks (dead zone), triggers as
// L2/R2 pressure plus the digital bit (Original), or Cross/Square pressure
// for analog throttle/brake (Modern).
void read_gamepad(SDL_Gamepad *gp, HostPadState *out, bool focused = true);
// Menu input never converts RT/LT driving axes into confirm/delete buttons.
void read_menu_gamepad(SDL_Gamepad *gp, HostPadState *out, bool focused = true);
uint8_t stick_byte(Sint16 v);    // -32768..32767 -> 0..255, centre 128, dead zone
uint8_t trigger_byte(Sint16 v);  // 0..32767 -> 0..255
constexpr uint8_t kTriggerDigital = 30;  // L2/R2 bit above this travel (as XInput)

// DualShock motors -> SDL rumble on the port's controller (false: none there).
// Running effects are renewed by update_rumble() (call once per frame), since
// SDL effects time out and a DualShock motor runs until the game changes it.
bool rumble(int port, uint8_t small_motor, uint8_t large_motor);
void update_rumble();

// EOD_INPUT_LOG=1: prints a port's state when buttons change or an axis moves.
bool log_enabled();
void log_state(int port, const HostPadState &s);
void format_state(const HostPadState &s, char *buf, size_t size);

// `eod --list-pads`: every joystick/gamepad and HID device SDL sees. With
// watch_seconds > 0 it then prints each player's DualShock state as it
// changes (and rumbles a pad when it gets a port) until the time runs out.
int list_pads(int watch_seconds);

}  // namespace sdl_input
