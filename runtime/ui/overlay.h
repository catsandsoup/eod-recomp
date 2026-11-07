// overlay.h - the in-game front end (recomp settings, mods, online setup) and the
// frame-rate counter, drawn with Dear ImGui draw lists over the game.
//
// The menu runs on the EE thread (which also pumps the window's events and owns
// the settings): each vblank it builds the menu and publishes the draw lists,
// which the GS thread draws over the composed image when it presents (Direct3D
// 11 on Windows, Vulkan on macOS). The menu itself (overlay_core.cpp,
// frontend.cpp, frontend_draw.cpp) is shared by both; only the event plumbing is
// per platform (overlay.cpp: Win32 messages; platform_sdl3.cpp: SDL events).
//
// Open/close: Start+Select on any controller, F1 or Esc on the
// keyboard. While it is open the game gets neutral input.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct OverlayVertex {  // as ImDrawVert
    float x, y, u, v;
    uint32_t rgba;
};

struct OverlayCmd {
    float clip[4];  // x0, y0, x1, y1 in overlay pixels
    uint32_t first_index, index_count, base_vertex;
    uint32_t texture;  // overlay texture id (1 = font atlas; see overlay_texture)
};

// One frame of the overlay (overlay pixels of a w x h area, scaled to the target).
struct OverlayDraw {
    int w = 0, h = 0;
    std::vector<OverlayVertex> verts;
    std::vector<uint16_t> indices;
    std::vector<OverlayCmd> cmds;
};

// An RGBA texture the overlay draws with (font atlas, decoded game UI art).
struct OverlayTexture {
    int w = 0, h = 0;
    const uint32_t *rgba = nullptr;
    uint32_t generation = 0;  // changes when the pixels do
};

// The renderer can draw the overlay (set by the hardware renderer at start).
void overlay_enable();
// Builds the menu / counter for this vblank (EE thread).
void overlay_frame();
// The menu is open: the game gets no keyboard or controller input.
bool overlay_menu_open();
// The game's input is neutral: the menu is open, or the buttons that closed it are still held.
bool overlay_captures_input();
// The game is held while the menu is open (Settings > Pause game in this menu).
bool overlay_paused();
void overlay_toggle();

// GS thread: the latest published overlay (false: nothing to draw).
bool overlay_latest(OverlayDraw &out);
// Something new to draw since the last call (present even when the game's image is unchanged).
bool overlay_changed();
// Texture by id (GS thread); false when unknown.
bool overlay_texture(uint32_t id, OverlayTexture &out);
// The font atlas (RGBA), and a number that changes when it does (texture id 1).
const uint32_t *overlay_font(int &w, int &h, uint32_t &generation);
// The overlay is drawn into screenshots too (EOD_SCREENSHOT_OVERLAY=0 turns it off).
bool overlay_in_screenshots();

#ifdef _WIN32
// Window messages (EE thread): true when the overlay used the message (overlay.cpp).
bool overlay_wndproc(void *hwnd, unsigned msg, uintptr_t wp, intptr_t lp);
#endif

// ---- input from the platform layer (EE thread) -------------------------------------
// Windows virtual-key codes (what eod.ini key names use); true = consumed by the menu.
bool overlay_key(int vk, bool down);
void overlay_char(unsigned c);
void overlay_mouse_move(float x, float y);
void overlay_mouse_button(bool down);
void overlay_mouse_wheel(float dy);
// UI menu sounds mixed into the game's 48 kHz stereo output (SPU thread / hold loop).
void overlay_mix_audio(int16_t *stereo, int frames);

// ---- in-game errors and first-run disc setup ----------------------------------------
// Shows the error screen (replaces the tabs) and opens the menu. offer_disc adds the
// controller disc browser; the handler installs the chosen image/folder and returns ""
// or a message for the player.
using OverlayDiscHandler = std::string (*)(const std::string &path);
void overlay_show_error(const std::string &title, const std::string &message, bool offer_disc,
                        OverlayDiscHandler handler = nullptr);
// The error screen's result: 0 still open, 1 retry (or a disc was installed), 2 quit.
int overlay_error_result();

// ---- platform hooks the shared menu calls (per platform) -----------------------------
struct HostPadState;
// The controller states for the menu (keyboard excluded), before the menu's input gate.
void overlay_platform_pad(int port, HostPadState *out);
// Short device name on a player port ("" = none), and whether it has Xbox-style glyphs.
std::string overlay_platform_device(int port, bool *xbox);
// Settings changed: audio gain, key map, controller/keyboard layout, stick shaping.
void overlay_platform_apply();
void overlay_platform_swap_ports();
// Window client size in pixels (false: no window; the overlay then uses the screenshot size
// EOD_SCREENSHOT_WIDTH/HEIGHT, else 1280x720).
bool overlay_platform_display(int *w, int *h);
// Where mods live ("<app support>/mods" on macOS, "<game>/mods" on Windows).
std::string overlay_platform_mods_root();
