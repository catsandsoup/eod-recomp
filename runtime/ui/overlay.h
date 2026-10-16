// overlay.h - the in-game menu (F1) and frame rate counter, drawn with Dear
// ImGui over the game by the Direct3D 11 renderer.
//
// The menu runs on the EE thread, which also owns the window (its messages
// are handled there) and the settings; each vblank it builds the menu and
// publishes the draw lists, which the GS thread draws over the image when it
// presents.  Without the Direct3D 11 renderer (or outside Windows) there is
// no overlay and the functions below do nothing.
#pragma once
#include <cstdint>
#include <vector>

struct OverlayVertex {  // as ImDrawVert
    float x, y, u, v;
    uint32_t rgba;
};

struct OverlayCmd {
    float clip[4];  // x0, y0, x1, y1 in window pixels
    uint32_t first_index, index_count, base_vertex;
};

// One frame of the overlay (window pixels of a w x h client area).
struct OverlayDraw {
    int w = 0, h = 0;
    std::vector<OverlayVertex> verts;
    std::vector<uint16_t> indices;
    std::vector<OverlayCmd> cmds;
};

#ifdef _WIN32
// The renderer can draw the overlay (set by the Direct3D 11 renderer at start).
void overlay_enable();
// Window messages (EE thread): true when the overlay used the message
// (input for the open menu, F1).
bool overlay_wndproc(void *hwnd, unsigned msg, uintptr_t wp, intptr_t lp);
// Builds the menu / counter for this vblank (EE thread).
void overlay_frame();
// The menu is open: the game gets no keyboard or controller input.
bool overlay_menu_open();
// The game is held while the menu is open.
bool overlay_paused();

// GS thread: the latest published overlay (false: nothing to draw).
bool overlay_latest(OverlayDraw &out);
// Something new to draw since the last call (present even when the game's image is unchanged).
bool overlay_changed();
// The font atlas (RGBA), and a number that changes when it does.
const uint32_t *overlay_font(int &w, int &h, uint32_t &generation);
#else
inline void overlay_enable() {}
inline void overlay_frame() {}
inline bool overlay_menu_open() { return false; }
inline bool overlay_paused() { return false; }
inline bool overlay_changed() { return false; }
#endif
