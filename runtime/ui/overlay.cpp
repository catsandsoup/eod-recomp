// overlay.cpp - Win32 plumbing of the in-game front end (see overlay.h): window
// messages to the shared menu (overlay_core.cpp) and the platform hooks it calls.
// macOS feeds the same menu from SDL events in platform_sdl3.cpp.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "overlay.h"

#include <windows.h>

#include <algorithm>
#include <string>

#include "../core/config.h"
#include "../platform/platform.h"

void build_key_map();
extern std::string g_game_root;

// Window messages (EE thread): true when the overlay used the message.
bool overlay_wndproc(void *hwnd, unsigned msg, uintptr_t wp, intptr_t lp) {
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        // Alt+Enter and F11 still switch fullscreen
        if (wp == VK_F11 || (msg == WM_SYSKEYDOWN && wp == VK_RETURN)) return false;
        if (lp & (1 << 30) && (wp == VK_F1 || wp == VK_ESCAPE) && !overlay_menu_open()) return true;  // auto-repeat
        return overlay_key((int)wp, true);
    case WM_KEYUP:
    case WM_SYSKEYUP:
        overlay_key((int)wp, false);
        return false;  // (the game's key state still sees the release)
    case WM_CHAR:
        if (!overlay_menu_open()) return false;
        if (wp >= 32 && wp < 127) overlay_char((unsigned)wp);
        return true;
    case WM_MOUSEMOVE:
        overlay_mouse_move((float)(short)LOWORD(lp), (float)(short)HIWORD(lp));
        return overlay_menu_open();
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (!overlay_menu_open()) return false;
        overlay_mouse_button(true);
        SetCapture((HWND)hwnd);
        return true;
    case WM_LBUTTONUP:
        if (!overlay_menu_open()) return false;
        overlay_mouse_button(false);
        ReleaseCapture();
        return true;
    case WM_MOUSEWHEEL:
        if (!overlay_menu_open()) return false;
        overlay_mouse_wheel((float)(short)HIWORD(wp) / (float)WHEEL_DELTA);
        return true;
    }
    return false;
}

void overlay_platform_pad(int port, HostPadState *out) { platform_get_controller(port, out); }

std::string overlay_platform_device(int port, bool *xbox) {
    HostPadState s;
    platform_get_controller(port, &s);
    if (xbox) *xbox = s.connected;  // XInput controllers
    if (s.connected) return "XInput controller";
    return port == 0 ? "Keyboard" : "";
}

void overlay_platform_apply() { build_key_map(); }
void overlay_platform_swap_ports() {}

bool overlay_platform_display(int *w, int *h) {
    RECT rc = {0, 0, 0, 0};
    HWND hw = (HWND)platform_native_window();
    if (!hw || !GetClientRect(hw, &rc)) return false;
    *w = std::max(1L, rc.right - rc.left);
    *h = std::max(1L, rc.bottom - rc.top);
    return true;
}

std::string overlay_platform_mods_root() { return g_game_root + "\\mods"; }
#endif  // _WIN32
