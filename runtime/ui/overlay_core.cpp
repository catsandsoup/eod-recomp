// overlay_core.cpp - the platform-independent half of the in-game front end
// (see overlay.h): ImGui context, fonts, textures, controller/keyboard/mouse
// input for the menu, menu sounds, publishing the draw lists for the renderer.
#include "overlay.h"
#include "../iop/custom_music.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#include "../../third_party/imgui/imgui.h"
#include "../app/game_frontend.h"
#include "../core/config.h"
#include "../platform/platform.h"
#include "frontend.h"
#include "frontend_draw.h"
#include "menu_input.h"
#include "ui_art.h"
#include "../../src/netplay_session.h"

[[noreturn]] void app_exit(int code);
uint64_t gs_drawn_frames();
void gs_overlay_redraw();
extern std::string g_game_root;

#ifndef EOD_RECOMP_VERSION
#define EOD_RECOMP_VERSION "0.1.0"
#endif

namespace {

std::atomic<bool> g_available{false};
std::atomic<bool> g_open{false};
bool g_context;
bool g_published_any;
uint64_t g_last_time;
std::unique_ptr<frontend::Menu> g_menu;
std::shared_ptr<game_frontend::Network> g_net;
ImFont *g_font_main;
menu_input::PadEdges g_pads[2];
frontend::Repeater g_rep_x, g_rep_y;
frontend::Glyphs g_glyphs = frontend::Glyphs::PlayStation;
bool g_release_wait;  // the game stays neutral until the closing buttons are released
std::vector<frontend_draw::Hit> g_hits;

// queued keyboard / mouse input (EE thread only: platform pump and overlay_frame share it)
struct KeyEvent { int vk; bool down; };
std::vector<KeyEvent> g_keys;
std::vector<unsigned> g_chars;
std::string g_paste;
float g_mx = -1, g_my = -1, g_wheel;
bool g_mouse_moved, g_click;
bool g_shift;

// error screen
OverlayDiscHandler g_disc_handler;
std::atomic<int> g_error_result{0};
bool g_fatal_screen = false;  // overlay_fatal_screen is running
std::thread g_import;
std::atomic<int> g_import_state{0};  // 0 idle, 1 running, 2 done ok, 3 failed
std::string g_import_error;          // written by the import thread before state 3

// published draw lists (EE thread -> GS thread)
std::mutex g_pub_mtx;
OverlayDraw g_pub;
bool g_pub_new;
std::atomic<bool> g_changed{false};
uint64_t g_pub_hash;

// textures: id 1 = font, 2.. = UI art
struct Tex { int w, h; std::vector<uint32_t> px; uint32_t gen; };
std::mutex g_tex_mtx;
std::vector<Tex> g_tex;  // index = id - 1
std::atomic<uint32_t> g_font_gen{0};

uint32_t add_texture(int w, int h, const uint32_t *rgba) {
    std::lock_guard<std::mutex> lk(g_tex_mtx);
    Tex t{w, h, std::vector<uint32_t>(rgba, rgba + (size_t)w * h), 1};
    g_tex.push_back(std::move(t));
    return (uint32_t)g_tex.size();
}

// ------------------------------------------------------------------ sounds
std::mutex g_snd_mtx;
std::vector<int16_t> g_snd;   // current sound, mono at g_snd_rate
int g_snd_rate = 22050;
double g_snd_pos;

void play(frontend::Sound s) {
    using frontend::Sound;
    if (s == Sound::None || !g_config.audio) return;
    ui_art::Sfx x = s == Sound::Select ? ui_art::Sfx::Select : s == Sound::Back ? ui_art::Sfx::Back
                    : s == Sound::Error ? ui_art::Sfx::Error : ui_art::Sfx::Move;
    int rate = 0;
    const std::vector<int16_t> &pcm = ui_art::sound(x, rate);
    if (pcm.empty()) return;
    std::lock_guard<std::mutex> lk(g_snd_mtx);
    g_snd = pcm;
    g_snd_rate = rate;
    g_snd_pos = 0;
}


// The MODS tab's Custom Music list, backed by the music library (runtime/iop/custom_music.h).
class MusicAdapter : public frontend::MusicSource {
public:
    std::vector<frontend::MusicSong> songs() override {
        using S = custom_music::State;
        std::vector<frontend::MusicSong> out;
        for (const custom_music::Song &s : custom_music::songs()) {
            frontend::MusicSong m;
            m.rel = s.rel;
            m.name = s.name;
            m.reason = s.reason;
            m.enabled = s.enabled;
            switch (s.state) {
            case S::Pending: m.status = frontend::MusicSong::Status::Waiting; break;
            case S::Scanning: m.status = frontend::MusicSong::Status::Decoding; break;
            case S::Ready: m.status = frontend::MusicSong::Status::Ready; break;
            case S::Skipped: m.status = frontend::MusicSong::Status::Skipped; break;
            case S::Protected: m.status = frontend::MusicSong::Status::Protected; break;
            }
            out.push_back(std::move(m));
        }
        return out;
    }
    void progress(int &done, int &total, bool &scanning) override {
        const custom_music::Progress p = custom_music::progress();
        done = p.done; total = p.total; scanning = p.scanning;
    }
    void set_enabled(const std::string &rel, bool on) override { custom_music::set_enabled(rel, on); }
    std::string folder() override { return custom_music::root_dir(); }
    void rescan() override { custom_music::rescan(); }
};

void refresh_host();

void open_menu(bool open) {
    if (g_open == open) return;
    g_open = open;
    if (open) {
        if (g_menu) {
            refresh_host();  // the mods root and device names before the rescan
            g_menu->open();
        }
        g_release_wait = false;
        for (int p = 0; p < 2; p++) {
            HostPadState s;
            overlay_platform_pad(p, &s);
            g_pads[p].reset(s.connected ? s.buttons : 0);  // the chord's buttons are not menu presses
        }
    } else {
        g_release_wait = true;
        if (g_menu && g_menu->dirty()) {
            config_save(g_config_path);
            g_menu->clear_dirty();
        }
    }
}

bool load_font(ImGuiIO &io, float px) {
    static const char *paths[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\bahnschrift.ttf", "C:\\Windows\\Fonts\\arialnb.ttf", "C:\\Windows\\Fonts\\segoeuib.ttf",
#else
        "/System/Library/Fonts/Supplemental/DIN Condensed Bold.ttf",
        "/System/Library/Fonts/Supplemental/Arial Narrow Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf",
#endif
    };
    for (const char *p : paths) {
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        fclose(f);
        ImFontConfig fc;
        fc.OversampleH = 2;
        fc.OversampleV = 2;
        static const ImWchar ranges[] = {0x20, 0xFF, 0x2022, 0x2026, 0x2190, 0x2193, 0};
        g_font_main = io.Fonts->AddFontFromFileTTF(p, px, &fc, ranges);
        if (g_font_main) { fprintf(stderr, "[frontend] font %s\n", p); return true; }
    }
    ImFontConfig fc;
    fc.SizePixels = px * 0.5f;
    g_font_main = io.Fonts->AddFontDefault(&fc);
    fprintf(stderr, "[frontend] no condensed system font found: ImGui default font\n");
    return false;
}

void ensure_context() {
    if (g_context) return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendPlatformName = "eod";
    io.BackendRendererName = "eod_overlay";
    load_font(io, 56.0f);  // drawn at any size from one atlas (linear sampling)
    unsigned char *pixels;
    int fw, fh;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
    {
        std::lock_guard<std::mutex> lk(g_tex_mtx);
        Tex t{fw, fh, std::vector<uint32_t>(reinterpret_cast<uint32_t *>(pixels), reinterpret_cast<uint32_t *>(pixels) + fw * fh), 1};
        if (g_tex.empty()) g_tex.push_back(std::move(t));
        else g_tex[0] = std::move(t);
    }
    io.Fonts->SetTexID((ImTextureID)1);
    g_font_gen++;
    ui_art::load(g_game_root, add_texture);
    g_menu = std::make_unique<frontend::Menu>(g_config);
    g_menu->set_music(std::make_shared<MusicAdapter>());
    g_net = std::shared_ptr<game_frontend::Network>(&netplay::session(), [](game_frontend::Network *) {});
    g_menu->set_network(g_net);
    g_context = true;
}

void display_size(int &w, int &h) {
    if (overlay_platform_display(&w, &h) && w > 0 && h > 0) return;
    const char *sw = getenv("EOD_SCREENSHOT_WIDTH"), *sh = getenv("EOD_SCREENSHOT_HEIGHT");
    w = sw ? atoi(sw) : 0;
    h = sh ? atoi(sh) : 0;
    if (w <= 0 || h <= 0) { w = 1280; h = 720; }
}

void refresh_host() {
    frontend::HostInfo hi;
    hi.version = EOD_RECOMP_VERSION;
    for (int p = 0; p < 2; p++) {
        bool xbox = false;
        hi.device[p] = overlay_platform_device(p, &xbox);
    }
    hi.mods_root = overlay_platform_mods_root();
    g_menu->set_host(hi);
}

// Controller + keyboard + mouse -> one frame of menu input.
frontend::Input gather_input(bool &toggled) {
    frontend::Input in;
    const uint64_t now = platform_time_us() / 1000;
    int dx = 0, dy = 0;
    toggled = false;
    for (int p = 0; p < 2; p++) {
        HostPadState s;
        overlay_platform_pad(p, &s);
        menu_input::PadFrame f = g_pads[p].update(s);
        if (f.toggle) { toggled = true; continue; }
        if (!g_open) continue;
        const uint16_t pr = f.pressed;
        if (pr || f.dx || f.dy) {
            bool xbox = false;
            overlay_platform_device(p, &xbox);
            g_glyphs = xbox ? frontend::Glyphs::Xbox : frontend::Glyphs::PlayStation;
        }
        in.accept |= (pr & PAD_CROSS) != 0;
        in.back |= (pr & PAD_CIRCLE) != 0;
        in.details |= (pr & PAD_SQUARE) != 0;
        in.reset |= (pr & PAD_TRIANGLE) != 0;
        in.prev_tab |= (pr & PAD_L1) != 0;
        in.next_tab |= (pr & PAD_R1) != 0;
        if (pr & PAD_L2) in.move = -1;
        if (pr & PAD_R2) in.move = 1;
        if ((pr & PAD_START) && !(f.held & PAD_SELECT)) in.close = true;
        if (!dx) dx = f.dx;
        if (!dy) dy = f.dy;
    }
    in.dx = g_rep_x.step(dx, now);
    in.dy = g_rep_y.step(dy, now);
    const bool rebinding = g_menu && g_menu->rebinding() >= 0, typing = g_menu && g_menu->editing_text();
    for (const KeyEvent &k : g_keys) {
        if (!k.down) continue;
        g_glyphs = frontend::Glyphs::Keyboard;
        if (rebinding) { in.key = k.vk; continue; }
        switch (k.vk) {
        case 0x26: in.dy = -1; break;  // arrows
        case 0x28: in.dy = 1; break;
        case 0x25: in.dx = -1; break;
        case 0x27: in.dx = 1; break;
        case 0x0D: in.accept = true; break;  // Enter
        case 0x1B: in.back = true; break;    // Esc
        case 0x08: if (typing) in.erase = true; else in.back = true; break;
        case 0x20: if (!typing) in.details = true; break;  // Space
        case 0x09: if (g_shift) in.prev_tab = true; else in.next_tab = true; break;
        case 'Q': if (!typing) in.prev_tab = true; break;
        case 'E': if (!typing) in.next_tab = true; break;
        case 'R': if (!typing) in.reset = true; break;
        case 0x21: in.move = -1; break;  // PageUp
        case 0x22: in.move = 1; break;
        default: break;
        }
        in.key = k.vk;
    }
    if (typing) {
        for (unsigned c : g_chars)
            if (c >= 32 && c < 127) in.paste.push_back((char)c);
        in.paste += g_paste;
    }
    g_keys.clear();
    g_chars.clear();
    g_paste.clear();
    if (g_mouse_moved || g_click || g_wheel != 0) {
        in.mouse_moved = g_mouse_moved;
        in.mx = g_mx;
        in.my = g_my;
        in.wheel = g_wheel;
        // hit-test the last drawn frame
        for (const frontend_draw::Hit &h : g_hits) {
            if (g_mx < h.x0 || g_mx >= h.x1 || g_my < h.y0 || g_my >= h.y1) continue;
            if (h.kind == frontend_draw::HitRow && g_menu) {
                if (g_mouse_moved || g_click) g_menu->select(h.index);
                if (g_click) in.accept = true;
            } else if (h.kind == frontend_draw::HitTab && g_click && g_menu) {
                g_menu->set_tab((frontend::Tab)h.index);
            } else if (h.kind == frontend_draw::HitLeft && g_click) {
                if (g_menu) g_menu->select(h.index);
                in.dx = -1;
            } else if (h.kind == frontend_draw::HitRight && g_click) {
                if (g_menu) g_menu->select(h.index);
                in.dx = 1;
            }
        }
        if (g_wheel != 0 && g_menu) in.dy = g_wheel > 0 ? -1 : 1;
        g_mouse_moved = g_click = false;
        g_wheel = 0;
    }
    return in;
}

void apply(const frontend::Effects &fx) {
    play(fx.sound);
    if (fx.fullscreen) platform_set_fullscreen(g_config.fullscreen);
    if (fx.audio || fx.keys || fx.input_layout) overlay_platform_apply();
    if (fx.swap_ports) overlay_platform_swap_ports();
    if (fx.save) config_save(g_config_path);
    if (fx.close) open_menu(false);
    if (fx.quit) {
        g_error_result = 2;
        if (g_fatal_screen) return;  // the panic screen's one choice: the caller restarts the game
        app_exit(0);
    }
    if (fx.retry) g_error_result = 1;
    if (fx.choose_disc && g_disc_handler && g_import_state != 1) {
        if (g_import.joinable()) g_import.join();
        g_import_state = 1;
        const std::string path = fx.disc_path;
        g_import = std::thread([path] {
            g_import_error = g_disc_handler(path);
            g_import_state = g_import_error.empty() ? 2 : 3;
        });
    }
}

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
            o.texture = (uint32_t)c.GetTexID();
            if (!o.texture) o.texture = 1;
            d.cmds.push_back(o);
        }
    }
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void *p, size_t n) {
        const uint8_t *b = static_cast<const uint8_t *>(p);
        for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    };
    mix(d.verts.data(), d.verts.size() * sizeof(OverlayVertex));
    mix(d.indices.data(), d.indices.size() * 2);
    for (const OverlayCmd &c : d.cmds) mix(&c.texture, 4);
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

void fps_text(char *text, size_t n) {
    static uint64_t t0, f0;
    static char last[32] = "";
    uint64_t now = platform_time_us(), f = gs_drawn_frames();
    if (!t0) t0 = now, f0 = f;
    if (now - t0 >= 500000) {
        snprintf(last, sizeof last, "%.0f FPS", (double)(f - f0) * 1e6 / (double)(now - t0));
        t0 = now;
        f0 = f;
    }
    snprintf(text, n, "%s", last);
}

// While the game is held (pause), the SPU does not run: feed the menu sounds directly.
void pump_paused_audio() {
    {
        std::lock_guard<std::mutex> lk(g_snd_mtx);
        if (g_snd.empty()) return;
    }
    if (platform_audio_queued_frames() > 48000 / 20) return;
    static int16_t buf[1600 * 2];
    memset(buf, 0, sizeof buf);
    overlay_mix_audio(buf, 1600);
    platform_audio_submit(buf, 1600);
}

}  // namespace

// --------------------------------------------------------------------------- API
void overlay_enable() { g_available = true; }
bool overlay_menu_open() { return g_open; }
bool overlay_captures_input() { return g_open || g_release_wait; }
bool overlay_paused() { return g_open && g_config.pause_in_menu && !(g_menu && g_menu->error_active()); }
void overlay_toggle() { open_menu(!g_open); }
bool overlay_in_screenshots() {
    const char *e = getenv("EOD_SCREENSHOT_OVERLAY");
    return !(e && e[0] == '0');
}

bool overlay_key(int vk, bool down) {
    if (vk == 0x10) g_shift = down;
    if (!g_available) return false;
    // F1 (and Cmd+, on the Mac) opens/closes it. Esc is the game's own pause/back key, so it never
    // opens the menu (it only backs out while the menu is open).
    if (down && vk == 0x70) {
        if (!g_context) ensure_context();
        if (g_menu && g_menu->error_active()) return true;
        if (vk == 0x70 && g_open && g_menu && g_menu->rebinding() < 0) { open_menu(false); play(frontend::Sound::Back); return true; }
        if (!g_open) { open_menu(true); play(frontend::Sound::Select); return true; }
    }
    if (!g_open) return false;
    g_keys.push_back({vk, down});
    return true;
}
void overlay_char(unsigned c) { if (g_open) g_chars.push_back(c); }
void overlay_paste(const char *utf8) { if (g_open && utf8) g_paste.append(utf8, strnlen(utf8, 256)); }
std::string overlay_copy_text() { return g_open && g_menu ? g_menu->copy_text() : std::string(); }
void overlay_mouse_move(float x, float y) {
    if (x == g_mx && y == g_my) return;
    g_mx = x;
    g_my = y;
    g_mouse_moved = true;
}
void overlay_mouse_button(bool down) {
    if (down && g_open) {
        g_click = true;
        g_glyphs = frontend::Glyphs::Keyboard;
    }
}
void overlay_mouse_wheel(float dy) { if (g_open) g_wheel += dy; }

void overlay_show_error(const std::string &title, const std::string &message, bool offer_disc, OverlayDiscHandler handler) {
    g_available = true;
    ensure_context();
    g_disc_handler = handler;
    g_error_result = 0;
    g_menu->show_error(title, message, offer_disc);
    g_open = true;
}
bool overlay_fatal_screen(const std::string &title, const std::string &message) {
    g_fatal_screen = true;
    g_available = true;
    ensure_context();
    g_disc_handler = nullptr;
    g_error_result = 0;
    g_menu->show_error(title, message, false, true);
    g_open = true;
    // The game is stopped for good: this is the same hold as the in-game menu's (kernel.cpp).
    while (g_error_result == 0) {
        gs_overlay_redraw();
        platform_sleep_us(16000);
        if (!platform_pump()) app_exit(0);
        overlay_frame();
    }
    return true;
}
int overlay_error_result() { return g_error_result; }

void overlay_show_online() {
    if (!g_available) return;
    ensure_context();
    if (g_menu) g_menu->set_tab(frontend::Tab::Online);
    if (!g_open) { open_menu(true); play(frontend::Sound::Select); }
}

void overlay_frame() {
    netplay::session().poll();   // network pump: every vblank, also with the menu closed or headless
    if (!g_available) return;
    if (!g_context) {
        // the chord before the first use (no context yet: nothing else to do)
        bool toggled = false;
        for (int p = 0; p < 2; p++) {
            HostPadState s;
            overlay_platform_pad(p, &s);
            if (g_pads[p].update(s).toggle) toggled = true;
        }
        if (toggled || g_open) { ensure_context(); g_open = false; open_menu(true); play(frontend::Sound::Select); }
        if (!g_open && !g_config.show_fps && netplay::session().strip().empty()) return;
    }
    ensure_context();
    refresh_host();
    // import thread finished?
    if (g_import_state == 2 || g_import_state == 3) {
        if (g_import.joinable()) g_import.join();
        if (g_import_state == 2) g_error_result = 1;
        else g_menu->show_error(g_menu->error_title(), g_import_error, true);
        g_import_state = 0;
    }
    bool toggled = false;
    frontend::Input in = gather_input(toggled);
    if (toggled && !g_menu->error_active()) {
        open_menu(!g_open);
        play(g_open ? frontend::Sound::Select : frontend::Sound::Back);
    } else if (g_open && g_import_state != 1) {
        apply(g_menu->update(in));
    }
    if (g_release_wait) {
        bool any = false;
        for (int p = 0; p < 2; p++) {
            HostPadState s;
            overlay_platform_pad(p, &s);
            any |= s.connected && s.buttons;
        }
        any |= overlay_platform_keys_held();  // e.g. the Esc/Enter that closed the menu: not a game press
        if (!any) g_release_wait = false;
    }
    const std::string strip = netplay::session().strip();
    if (!g_open && !g_config.show_fps && strip.empty()) {
        if (g_published_any) {
            g_published_any = false;
            std::lock_guard<std::mutex> lk(g_pub_mtx);
            g_pub = OverlayDraw();
            g_pub_new = true;
            g_pub_hash = 0;
            g_changed = true;
        }
        return;
    }
    ImGuiIO &io = ImGui::GetIO();
    int w, h;
    display_size(w, h);
    io.DisplaySize = ImVec2((float)w, (float)h);
    uint64_t now = platform_time_us();
    io.DeltaTime = g_last_time && now > g_last_time ? (float)(now - g_last_time) / 1e6f : 1.0f / 60.0f;
    if (io.DeltaTime > 0.25f || io.DeltaTime <= 0) io.DeltaTime = 1.0f / 60.0f;
    g_last_time = now;
    ImGui::NewFrame();
    frontend_draw::Ctx c;
    c.font = g_font_main;
    c.w = (float)w;
    c.h = (float)h;
    c.glyphs = g_glyphs;
    c.version = EOD_RECOMP_VERSION;
    if (g_import_state == 1) c.busy = "INSTALLING YOUR DISC...";
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    if (g_open) frontend_draw::draw_menu(dl, *g_menu, c, g_hits);
    else g_hits.clear();
    if (!strip.empty()) {
        dl->AddRectFilled(ImVec2(0, 0), ImVec2((float)w, 38), IM_COL32(24, 14, 8, 215));
        dl->AddRectFilled(ImVec2(0, 36), ImVec2((float)w, 38), IM_COL32(230, 120, 30, 255));
        dl->AddText(g_font_main, 22.f, ImVec2(18, 7), IM_COL32(255, 160, 50, 255), strip.c_str());
    }
    if (g_config.show_fps) {
        char t[32];
        fps_text(t, sizeof t);
        frontend_draw::draw_fps(dl, c, t);
    }
    ImGui::Render();
    publish(ImGui::GetDrawData());
    if (overlay_paused()) pump_paused_audio();
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

bool overlay_texture(uint32_t id, OverlayTexture &out) {
    std::lock_guard<std::mutex> lk(g_tex_mtx);
    if (id == 0 || id > g_tex.size()) return false;
    const Tex &t = g_tex[id - 1];
    out.w = t.w;
    out.h = t.h;
    out.rgba = t.px.data();  // stable: textures are never replaced after creation (font: generation)
    out.generation = id == 1 ? g_font_gen.load() : t.gen;
    return true;
}

const uint32_t *overlay_font(int &w, int &h, uint32_t &generation) {
    OverlayTexture t;
    if (!overlay_texture(1, t)) return nullptr;
    w = t.w;
    h = t.h;
    generation = t.generation;
    return t.rgba;
}

void overlay_mix_audio(int16_t *stereo, int frames) {
    std::lock_guard<std::mutex> lk(g_snd_mtx);
    if (g_snd.empty()) return;
    const double step = (double)g_snd_rate / 48000.0;
    const int vol = std::clamp(g_config.volume, 0, 100) * 180 / 100;  // menu sounds a little under full scale
    for (int i = 0; i < frames; i++) {
        const size_t k = (size_t)g_snd_pos;
        if (k + 1 >= g_snd.size()) { g_snd.clear(); break; }
        const double t = g_snd_pos - (double)k;
        const int s = (int)((g_snd[k] * (1 - t) + g_snd[k + 1] * t) * vol / 256);
        for (int c = 0; c < 2; c++) stereo[i * 2 + c] = (int16_t)std::clamp(stereo[i * 2 + c] + s, -32768, 32767);
        g_snd_pos += step;
    }
}

// Defaults for platforms without their own hooks (headless POSIX builds).
#if !defined(_WIN32)
__attribute__((weak)) void overlay_platform_pad(int port, HostPadState *out) {
    *out = HostPadState();
    (void)port;
}
__attribute__((weak)) std::string overlay_platform_device(int, bool *xbox) {
    if (xbox) *xbox = false;
    return "";
}
__attribute__((weak)) void overlay_platform_apply() {}
__attribute__((weak)) void overlay_platform_swap_ports() {}
__attribute__((weak)) bool overlay_platform_display(int *, int *) { return false; }
__attribute__((weak)) std::string overlay_platform_mods_root() { return g_game_root + "/mods"; }
#endif
