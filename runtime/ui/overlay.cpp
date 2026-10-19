// overlay.cpp - the in-game menu (F1) and frame rate counter (see overlay.h).
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
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "../../third_party/imgui/imgui.h"
#include "../core/config.h"
#include "../platform/platform.h"

[[noreturn]] void app_exit(int code);
void build_key_map();
uint64_t gs_drawn_frames();

namespace {

std::atomic<bool> g_available{false};
std::atomic<bool> g_open{false};
bool g_context;           // ImGui context created (EE thread)
bool g_published_any;    // the last publication had something to draw
int g_rebind = -1;        // pad button waiting for a key (Controls tab)
uint64_t g_last_time;
uint16_t g_pad_prev;      // controller buttons last vblank (menu chord)
std::string g_status;     // message under the buttons
uint64_t g_status_until;
int g_started_scale;      // the internal resolution in use

// published draw lists (EE thread -> GS thread)
std::mutex g_pub_mtx;
OverlayDraw g_pub;
bool g_pub_new;
std::atomic<bool> g_changed{false};
uint64_t g_pub_hash;

// font atlas
std::vector<uint32_t> g_font;
int g_font_w, g_font_h;
std::atomic<uint32_t> g_font_gen{0};

ImGuiKey vk_to_key(WPARAM vk) {
    if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
    if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
    switch (vk) {
    case VK_TAB: return ImGuiKey_Tab;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_INSERT: return ImGuiKey_Insert;
    case VK_DELETE: return ImGuiKey_Delete;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_SHIFT: return ImGuiKey_LeftShift;
    case VK_CONTROL: return ImGuiKey_LeftCtrl;
    case VK_MENU: return ImGuiKey_LeftAlt;
    default: return ImGuiKey_None;
    }
}

void set_status(const std::string &s) {
    g_status = s;
    g_status_until = platform_time_us() + 4000000;
}

void open_menu(bool open) {
    g_open = open;
    g_rebind = -1;
    if (g_context) ImGui::GetIO().ClearInputKeys();
}

void ensure_context() {
    if (g_context) return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    g_started_scale = g_config.render_scale;
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;  // nothing written next to the game
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "eod";
    io.BackendRendererName = "eod_d3d11";
    // the font: Segoe UI (Windows 7 and later) sized for the monitor, else ImGui's own
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    float px = (float)std::max(15, std::min(32, screen_h / 48));
    char path[MAX_PATH];
    UINT n = GetWindowsDirectoryA(path, MAX_PATH);
    ImFont *font = nullptr;
    if (n && n < MAX_PATH - 32) {
        strcat(path, "\\Fonts\\segoeui.ttf");
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) font = io.Fonts->AddFontFromFileTTF(path, px);
    }
    if (!font) {
        ImFontConfig fc;
        fc.SizePixels = std::max(13.0f, px * 0.8f);
        io.Fonts->AddFontDefault(&fc);
    }
    unsigned char *pixels;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &g_font_w, &g_font_h);
    g_font.assign(reinterpret_cast<uint32_t *>(pixels), reinterpret_cast<uint32_t *>(pixels) + g_font_w * g_font_h);
    io.Fonts->SetTexID((ImTextureID)1);
    g_font_gen++;
    // look
    ImGui::StyleColorsDark();
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 6;
    s.FrameRounding = 4;
    s.GrabRounding = 4;
    s.TabRounding = 4;
    s.WindowPadding = ImVec2(px * 0.8f, px * 0.6f);
    s.FramePadding = ImVec2(px * 0.4f, px * 0.25f);
    s.ItemSpacing = ImVec2(px * 0.5f, px * 0.35f);
    s.WindowBorderSize = 0;
    ImVec4 *c = s.Colors;
    ImVec4 accent(0.86f, 0.42f, 0.10f, 1.0f), accent_hi(0.96f, 0.52f, 0.18f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.09f, 0.94f);
    c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.12f, 0.13f, 1.0f);
    c[ImGuiCol_CheckMark] = accent_hi;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_hi;
    c[ImGuiCol_Button] = ImVec4(0.22f, 0.22f, 0.24f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.55f, 0.28f, 0.08f, 1.0f);
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.17f, 0.19f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.22f, 0.20f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.34f, 0.24f, 0.16f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.45f, 0.24f, 0.08f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.60f, 0.31f, 0.10f, 1.0f);
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Tab] = ImVec4(0.17f, 0.17f, 0.19f, 1.0f);
    c[ImGuiCol_TabHovered] = ImVec4(0.60f, 0.31f, 0.10f, 1.0f);
    c[ImGuiCol_TabSelected] = accent;
    c[ImGuiCol_NavCursor] = accent_hi;
    g_context = true;
}

// Controller input for the menu (and the L3 + R3 chord that opens it).
void controller_input() {
    HostPadState p;
    platform_get_controller(0, &p);
    uint16_t b = p.connected ? p.buttons : 0;
    uint16_t pressed = b & ~g_pad_prev;
    g_pad_prev = b;
    const uint16_t chord = PAD_L3 | PAD_R3 | PAD_START;
    if ((b & chord) == chord && (pressed & chord)) open_menu(!g_open);
    if (!g_open) return;
    ImGuiIO &io = ImGui::GetIO();
    struct Map {
        uint16_t bit;
        ImGuiKey key;
    } map[] = {
        {PAD_UP, ImGuiKey_GamepadDpadUp},       {PAD_DOWN, ImGuiKey_GamepadDpadDown},
        {PAD_LEFT, ImGuiKey_GamepadDpadLeft},   {PAD_RIGHT, ImGuiKey_GamepadDpadRight},
        {PAD_CROSS, ImGuiKey_GamepadFaceDown},  {PAD_CIRCLE, ImGuiKey_GamepadFaceRight},
        {PAD_SQUARE, ImGuiKey_GamepadFaceLeft}, {PAD_TRIANGLE, ImGuiKey_GamepadFaceUp},
        {PAD_L1, ImGuiKey_GamepadL1},           {PAD_R1, ImGuiKey_GamepadR1},
    };
    for (const Map &m : map) io.AddKeyEvent(m.key, (b & m.bit) != 0);
    auto stick = [&](ImGuiKey neg, ImGuiKey pos, uint8_t v) {
        float f = ((int)v - 128) / 127.0f;
        io.AddKeyAnalogEvent(neg, f < -0.4f, f < -0.4f ? -f : 0.0f);
        io.AddKeyAnalogEvent(pos, f > 0.4f, f > 0.4f ? f : 0.0f);
    };
    stick(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, p.connected ? p.lx : 128);
    stick(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, p.connected ? p.ly : 128);
}

void draw_fps() {
    static uint64_t t0, f0;
    static char text[32] = "";
    uint64_t now = platform_time_us(), f = gs_drawn_frames();
    if (!t0) t0 = now, f0 = f;
    if (now - t0 >= 500000) {
        snprintf(text, sizeof text, "%.0f fps", (double)(f - f0) * 1e6 / (double)(now - t0));
        t0 = now;
        f0 = f;
    }
    if (!text[0]) return;
    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImVec2 size = ImGui::CalcTextSize(text);
    float pad = ImGui::GetFontSize() * 0.3f;
    ImVec2 p(io.DisplaySize.x - size.x - pad * 3, pad);
    dl->AddRectFilled(ImVec2(p.x - pad, p.y - pad * 0.5f), ImVec2(p.x + size.x + pad, p.y + size.y + pad * 0.5f),
                      IM_COL32(0, 0, 0, 150), pad);
    dl->AddText(p, IM_COL32(255, 255, 255, 230), text);
}

void help(const char *text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void tab_display() {
    bool fs = platform_fullscreen();
    if (ImGui::Checkbox("Fullscreen", &fs)) {
        platform_set_fullscreen(fs);
        g_config.fullscreen = fs;
    }
    help("Borderless window over the whole screen (also F11 or Alt+Enter).");
    static const char *aspects[] = {"auto", "4:3", "16:9", "stretch"};
    static const char *aspect_labels[] = {"Auto (game option)", "4:3", "16:9 widescreen", "Stretch to window"};
    int a = 0;
    for (int i = 0; i < 4; i++)
        if (g_config.aspect == aspects[i]) a = i;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 13);
    if (ImGui::Combo("Aspect ratio", &a, aspect_labels, 4)) g_config.aspect = aspects[a];
    help("16:9 switches the game to its own widescreen mode (wider view, HUD placed for it). Movies always "
         "play in 4:3.");
    ImGui::Checkbox("VSync", &g_config.vsync);
    help("Waits for the monitor between frames: no tearing.");
    ImGui::Checkbox("Smooth scaling", &g_config.smooth_display);
    help("Filters the image when it is scaled to the window.");
    ImGui::Checkbox("Frame skipping", &g_config.frame_skip);
    help("When drawing falls behind, whole frames are skipped so the game keeps its speed.");
    static const char *scales[] = {"1x (640 x 448, PS2)", "2x (1280 x 896)", "3x (1920 x 1344)",
                                   "4x (2560 x 1792)",   "5x (3200 x 2240)", "6x (3840 x 2688)"};
    int sc = std::max(1, std::min(6, g_config.render_scale)) - 1;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 13);
    if (ImGui::Combo("Internal resolution", &sc, scales, 6)) g_config.render_scale = sc + 1;
    if (g_config.render_scale != g_started_scale) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "after a restart");
    } else {
        help("Resolution the 3D is drawn at. Higher is sharper and needs a faster graphics card. Takes effect "
             "the next time the game starts.");
    }
    ImGui::Checkbox("Show frame rate", &g_config.show_fps);
}

void tab_audio() {
    ImGui::Checkbox("Sound", &g_config.audio);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::SliderInt("Volume", &g_config.volume, 0, 100, "%d%%");
}

void tab_controls() {
    ImGui::Checkbox("Controller vibration", &g_config.vibration);
    ImGui::Spacing();
    ImGui::TextDisabled("Keyboard (controllers use the DualShock layout):");
    if (ImGui::BeginTable("keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (int i = 0; i < KEY_COUNT; i++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kPadKeyLabels[i]);
            ImGui::TableNextColumn();
            if (g_rebind == i)
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "press a key (Esc: cancel)");
            else
                ImGui::TextUnformatted(g_config.keys[i].empty() ? "-" : g_config.keys[i].c_str());
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            if (ImGui::SmallButton("Change")) g_rebind = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void tab_gameplay() {
    ImGui::Checkbox("Manual recovery only", &g_config.manual_recovery);
    help("Explore without automatic wrong-way, upside-down or off-track countdown resets. "
         "SELECT still recovers your car. Applies immediately; AI recovery is unchanged.");
    ImGui::Separator();
    ImGui::Text("Live Cheats & Mods (Controller combos: L1 + R1 + D-Pad):");
    ImGui::Checkbox("Chicken Fight (L2 fires explosive chickens)", &g_config.cheat_chickens);
    help("L1 + R1 + D-Pad Up toggles live in-game without opening menus.");
    ImGui::Checkbox("Turbo Boost (L2 boosts beyond top speed)", &g_config.cheat_turbo);
    help("L1 + R1 + D-Pad Right toggles live in-game without opening menus.");
    ImGui::SliderFloat("Turbo Scale", &g_config.turbo_scale, 1.0f, 3.0f, "%.2fx");
    ImGui::SliderFloat("Chicken Blast Force", &g_config.chicken_force_scale, 1.0f, 4.0f, "%.2fx");
    ImGui::SliderFloat("Chicken Blast Radius", &g_config.chicken_radius_scale, 1.0f, 4.0f, "%.2fx");
    ImGui::Checkbox("Single Player Battle Mode", &g_config.sp_battle_mode);
    help("Adds Battle to supported solo tracks where menu capacity permits. Reload solo mode to apply.");
    ImGui::Checkbox("Battle bots (fill to four cars)", &g_config.battle_bots);
    help("Experimental opponents with native weapons and respawns. Navigation can stall at obstacles. Applies on the next event.");
    ImGui::Checkbox("Unlock All Out of the Box", &g_config.unlock_all);
    help("All tracks, cars, dares, and feat cheat prizes unlocked out of the box.");
}

void tab_system() {
    ImGui::Checkbox("Pause the game while this menu is open", &g_config.pause_in_menu);
    ImGui::Spacing();
    ImGui::TextDisabled("F1 or L3 + R3 (both sticks pressed) opens and closes this menu.");
}

void draw_menu() {
    ImGuiIO &io = ImGui::GetIO();
    if (g_config.pause_in_menu)
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    bool open = true;
    const char *title = g_config.pause_in_menu ? "Settings (game paused)###menu" : "Settings###menu";
    if (ImGui::Begin(title, &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoMove)) {
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Display")) {
                tab_display();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Audio")) {
                tab_audio();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Controls")) {
                tab_controls();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Gameplay")) {
                tab_gameplay();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("System")) {
                tab_system();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        ImGui::TextDisabled("Changes apply right away; Save keeps them for next time.");
        if (ImGui::Button("Resume")) open = false;
        ImGui::SameLine();
        if (ImGui::Button("Save settings")) {
            std::string shown = g_config_path;
            std::replace(shown.begin(), shown.end(), '/', '\\');
            set_status(config_save(g_config_path) ? "Saved to " + shown : "Couldn't write " + shown);
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit game")) ImGui::OpenPopup("quit");
        if (ImGui::BeginPopupModal("quit", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
            ImGui::TextUnformatted("Quit the game? Unsaved progress is lost.");
            if (ImGui::Button("Quit")) app_exit(0);
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (!g_status.empty() && platform_time_us() < g_status_until) ImGui::TextDisabled("%s", g_status.c_str());
    }
    ImGui::End();
    if (!open) open_menu(false);
}

// Copies the draw data for the GS thread.
void publish(ImDrawData *dd) {
    OverlayDraw d;
    d.w = (int)dd->DisplaySize.x;
    d.h = (int)dd->DisplaySize.y;
    d.verts.reserve((size_t)dd->TotalVtxCount);
    d.indices.reserve((size_t)dd->TotalIdxCount);
    for (const ImDrawList *l : dd->CmdLists) {
        uint32_t vbase = (uint32_t)d.verts.size(), ibase = (uint32_t)d.indices.size();
        for (const ImDrawVert &v : l->VtxBuffer) d.verts.push_back({v.pos.x, v.pos.y, v.uv.x, v.uv.y, v.col});
        d.indices.insert(d.indices.end(), l->IdxBuffer.begin(), l->IdxBuffer.end());
        for (const ImDrawCmd &c : l->CmdBuffer) {
            if (c.UserCallback || !c.ElemCount) continue;
            OverlayCmd o;
            o.clip[0] = std::max(0.0f, c.ClipRect.x);
            o.clip[1] = std::max(0.0f, c.ClipRect.y);
            o.clip[2] = std::min(dd->DisplaySize.x, c.ClipRect.z);
            o.clip[3] = std::min(dd->DisplaySize.y, c.ClipRect.w);
            if (o.clip[2] <= o.clip[0] || o.clip[3] <= o.clip[1]) continue;
            o.first_index = ibase + c.IdxOffset;
            o.index_count = c.ElemCount;
            o.base_vertex = vbase + c.VtxOffset;
            d.cmds.push_back(o);
        }
    }
    // only a change needs a new present (a still menu or counter doesn't)
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void *p, size_t n) {
        const uint8_t *b = static_cast<const uint8_t *>(p);
        for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    };
    mix(d.verts.data(), d.verts.size() * sizeof(OverlayVertex));
    mix(d.indices.data(), d.indices.size() * 2);
    mix(&d.w, sizeof d.w);
    mix(&d.h, sizeof d.h);
    g_published_any = !d.cmds.empty();
    std::lock_guard<std::mutex> lk(g_pub_mtx);
    g_pub = std::move(d);
    g_pub_new = true;
    if (h != g_pub_hash) {
        g_pub_hash = h;
        g_changed = true;
    }
}

}  // namespace

void overlay_enable() { g_available = true; }

bool overlay_menu_open() { return g_open; }

bool overlay_paused() { return g_open && g_config.pause_in_menu; }

bool overlay_wndproc(void *hwnd, unsigned msg, uintptr_t wp, intptr_t lp) {
    (void)hwnd;
    if (!g_available) return false;
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wp == VK_F1) {
        if (!(lp & (1 << 30))) open_menu(!g_open);  // not auto-repeat
        return true;
    }
    if (!g_open || !g_context) return false;
    ImGuiIO &io = ImGui::GetIO();
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g_rebind >= 0) {
            if (wp != VK_ESCAPE) {
                std::string name = key_name((int)wp);
                if (!name.empty()) {
                    g_config.keys[g_rebind] = name;
                    build_key_map();
                }
            }
            g_rebind = -1;
            return true;
        }
        if (wp == VK_ESCAPE && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
            open_menu(false);
            return true;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (GetKeyState(VK_MENU) & 0x8000) != 0);
        io.AddKeyEvent(vk_to_key(wp), true);
        // Alt+Enter and F11 still switch fullscreen
        return !(wp == VK_F11 || (msg == WM_SYSKEYDOWN && wp == VK_RETURN));
    case WM_KEYUP:
    case WM_SYSKEYUP:
        io.AddKeyEvent(vk_to_key(wp), false);
        io.AddKeyEvent(ImGuiMod_Ctrl, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (GetKeyState(VK_MENU) & 0x8000) != 0);
        return false;  // (the game's key state still sees the release)
    case WM_CHAR:
        if (wp > 0 && wp < 0x10000) io.AddInputCharacterUTF16((unsigned short)wp);
        return true;
    case WM_MOUSEMOVE:
        io.AddMousePosEvent((float)(short)LOWORD(lp), (float)(short)HIWORD(lp));
        return true;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        io.AddMouseButtonEvent(msg == WM_RBUTTONDOWN ? 1 : msg == WM_MBUTTONDOWN ? 2 : 0, true);
        SetCapture((HWND)hwnd);
        return true;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        io.AddMouseButtonEvent(msg == WM_RBUTTONUP ? 1 : msg == WM_MBUTTONUP ? 2 : 0, false);
        ReleaseCapture();
        return true;
    case WM_MOUSEWHEEL:
        io.AddMouseWheelEvent(0.0f, (float)(short)HIWORD(wp) / (float)WHEEL_DELTA);
        return true;
    case WM_KILLFOCUS:
        io.AddFocusEvent(false);
        return false;
    case WM_SETFOCUS:
        io.AddFocusEvent(true);
        return false;
    }
    return false;
}

void overlay_frame() {
    if (!g_available) return;
    if (g_context) controller_input();
    else if (g_open) ensure_context();
    if (!g_open && !g_config.show_fps) {
        if (g_published_any) {  // clear what was shown
            g_published_any = false;
            std::lock_guard<std::mutex> lk(g_pub_mtx);
            g_pub = OverlayDraw();
            g_pub_new = true;
            g_pub_hash = 0;
            g_changed = true;
        }
        if (!g_context) {  // the controller chord before the first use
            HostPadState p;
            platform_get_controller(0, &p);
            uint16_t b = p.connected ? p.buttons : 0, pressed = b & ~g_pad_prev;
            g_pad_prev = b;
            if ((b & (PAD_L3 | PAD_R3)) == (PAD_L3 | PAD_R3) && (pressed & (PAD_L3 | PAD_R3))) open_menu(true);
        }
        return;
    }
    ensure_context();
    ImGuiIO &io = ImGui::GetIO();
    RECT rc = {0, 0, 640, 480};
    if (HWND h = (HWND)platform_native_window()) GetClientRect(h, &rc);
    io.DisplaySize = ImVec2((float)std::max(1L, rc.right - rc.left), (float)std::max(1L, rc.bottom - rc.top));
    uint64_t now = platform_time_us();
    io.DeltaTime = g_last_time && now > g_last_time ? (float)(now - g_last_time) / 1e6f : 1.0f / 60.0f;
    if (io.DeltaTime > 0.25f) io.DeltaTime = 0.25f;
    g_last_time = now;
    ImGui::NewFrame();
    if (g_config.show_fps) draw_fps();
    if (g_open) draw_menu();
    ImGui::Render();
    publish(ImGui::GetDrawData());
}

bool overlay_latest(OverlayDraw &out) {
    {
        std::lock_guard<std::mutex> lk(g_pub_mtx);
        if (g_pub_new) {
            out = g_pub;
            g_pub_new = false;
        }
    }
    return !out.cmds.empty();
}

bool overlay_changed() { return g_changed.exchange(false); }

const uint32_t *overlay_font(int &w, int &h, uint32_t &generation) {
    w = g_font_w;
    h = g_font_h;
    generation = g_font_gen;
    return g_font.empty() ? nullptr : g_font.data();
}
#endif  // _WIN32
