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
#include "../gs/modern_scene.h"
#include "../platform/platform.h"
#include "../platform/presentation.h"
#include "../render/native_renderer.h"
#include "frontend.h"
#include "frontend_draw.h"
#include "menu_input.h"
#include "ui_art.h"
#include "ui_preview.h"
#include "../../src/netplay_session.h"
#include "../../src/mutator_hooks.h"
#include "../../src/session_start.h"
#include "../../src/catalog_cache.h"
extern uint64_t kernel_vblank_count();
extern uint64_t kernel_menu_hold_frames();

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
// Keep the notice and its expiry together when input and overlay rendering run on different threads.
std::atomic<uint64_t> g_renderer_notice{0};
constexpr unsigned kRendererLookBits = 2;
constexpr uint64_t kRendererLookMask = (1u << kRendererLookBits) - 1;
std::unique_ptr<frontend::Menu> g_menu;
std::unique_ptr<game_frontend::Command> g_resumed_invite;
std::shared_ptr<game_frontend::Network> g_net;
ImFont *g_font_main;
menu_input::PadEdges g_pads[4];
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
bool g_mouse_moved, g_click, g_mouse_down, g_dragging_slider;
frontend_draw::Hit g_slider_track{};
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

void load_menu_art() {
    static bool loaded;
    if (loaded) return;
    ui_art::load(g_game_root, add_texture);
    ui_preview::init(g_game_root, add_texture);
    loaded = true;
}

void open_menu(bool open) {
    if (g_open == open) return;
    g_open = open;
    g_dragging_slider = false;
    if (open) {
        load_menu_art();
        if (g_menu) {
            refresh_host();  // the mods root and device names before the rescan
            g_menu->open();
        }
        g_release_wait = false;
        for (int p = 0; p < 4; p++) {
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
    for (int p = 0; p < 4; p++) {
        bool xbox = false;
        hi.device[p] = overlay_platform_device(p, &xbox);
    }
    hi.mods_root = overlay_platform_mods_root();
    if (native::look_available())
        hi.plume_look = native::look_modern() ? frontend::PlumeLook::Modern : frontend::PlumeLook::Classic;
    else
        hi.legacy_modern_available = modern::available();
    g_menu->set_host(hi);
}

// Controller + keyboard + mouse -> one frame of menu input.
frontend::Input gather_input(bool &toggled) {
    frontend::Input in;
    const uint64_t now = platform_time_us() / 1000;
    int dx = 0, dy = 0;
    toggled = false;
    for (int p = 0; p < 4; p++) {
        HostPadState s;
        overlay_platform_pad(p, &s);
        menu_input::PadFrame f = g_pads[p].update(s);
        if (f.toggle) { toggled = true; continue; }
        const bool in_gameplay = netplay::session().event_active() ||
            (g_config.unified_frontend && g_ram && (ee_r32(0x00437BB8) != 0 || ee_r32(0x003CDA04) != 0));
        if (!g_open && in_gameplay && (f.pressed & PAD_START) && !(f.held & PAD_SELECT)) {
            if (g_menu) g_menu->set_quick_rail(true);
            toggled = true;
            continue;
        }
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
        // A held thumb follows the pointer beyond the ends of its track.
        if (g_dragging_slider && g_menu) {
            in.slider_fraction = std::clamp((g_mx - g_slider_track.x0) / (g_slider_track.x1 - g_slider_track.x0), 0.0f, 1.0f);
            if (!g_mouse_down) g_dragging_slider = false;
            g_mouse_moved = g_click = false;
            g_wheel = 0;
            return in;
        }
        // hit-test the last drawn frame
        for (const frontend_draw::Hit &h : g_hits) {
            if (g_mx < h.x0 || g_mx >= h.x1 || g_my < h.y0 || g_my >= h.y1) continue;
            if (h.kind == frontend_draw::HitRow && g_menu) {
                if (g_mouse_moved || g_click) g_menu->select(h.index);
                if (g_click) in.accept = true;
            } else if (h.kind == frontend_draw::HitTab && g_click && g_menu) {
                g_menu->set_tab((frontend::Tab)h.index);
            } else if (h.kind == frontend_draw::HitSlider && g_click && g_menu) {
                g_menu->select(h.index);
                in.accept = false;
                in.slider_fraction = (g_mx - h.x0) / (h.x1 - h.x0);
                g_slider_track = h;
                g_dragging_slider = g_mouse_down;
            } else if (h.kind == frontend_draw::HitLeft && g_click) {
                if (g_menu) g_menu->select(h.index);
                in.dx = -1;
            } else if (h.kind == frontend_draw::HitRight && g_click) {
                if (g_menu) g_menu->select(h.index);
                in.dx = 1;
            } else if (h.kind == frontend_draw::HitBrowserTile && g_menu) {
                if (g_mouse_moved || g_click) g_menu->set_browser_selected(h.index);
                if (g_click) in.accept = true;
            } else if (h.kind == frontend_draw::HitBrowserCat && g_click && g_menu) {
                g_menu->set_browser_category(h.index);
            } else if (h.kind == frontend_draw::HitSubTab && g_click && g_menu) {
                g_menu->set_rules_tab(h.index);
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
    if (fx.toggle_plume_look && native::look_available()) {
        modern::toggle_from_input("settings menu");
        refresh_host();
    }
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
    bool has_menu_snd = false;
    {
        std::lock_guard<std::mutex> lk(g_snd_mtx);
        has_menu_snd = !g_snd.empty();
    }
    if (!has_menu_snd && !netplay::session().active()) return;
    if (platform_audio_queued_frames() > 48000 / 20) return;
    static int16_t buf[1600 * 2];
    memset(buf, 0, sizeof buf);
    overlay_mix_audio(buf, 1600);
    platform_audio_submit(buf, 1600);
}

}  // namespace

// --------------------------------------------------------------------------- API
void overlay_enable() {
    g_available = true;
    catalog_cache::global().refresh(overlay_platform_mods_root());
    session_set_hub_arrival_handler([] {
        if (!g_available || !g_config.unified_frontend) return;
        const char *headless = std::getenv("EOD_HEADLESS");
        if (headless && headless[0] == '1') return;
        const char *direct = std::getenv("EOD_DIRECT_START");
        if (direct && direct[0]) return;
        const char *pad_script = std::getenv("EOD_PAD_SCRIPT");
        if (pad_script && pad_script[0]) return;
        const char *net_auto = std::getenv("EOD_NETPLAY_AUTO");
        if (net_auto && net_auto[0]) return;
        ensure_context();
        if (g_menu) {
            g_menu->set_quick_rail(false);
            g_menu->set_tab(frontend::Tab::Play);
        }
        if (!g_open) open_menu(true);
    });
}
const char *overlay_renderer_look_label(RendererLook look) {
    switch (look) {
    case RendererLook::Classic: return "RENDERER: CLASSIC";
    case RendererLook::Modern: return "RENDERER: REMASTER";
    case RendererLook::Original: return "RENDERER: ORIGINAL";
    }
    return "";
}
void overlay_show_renderer_look(RendererLook look) {
    if (!g_available) return;
    const uint64_t until = platform_time_us() + 2500000;
    g_renderer_notice.store((until << kRendererLookBits) | uint64_t(look), std::memory_order_relaxed);
}
bool overlay_menu_open() { return g_open; }
bool overlay_captures_input() { return g_open || g_release_wait; }
// Never hold the game when an online session is active (Hosting, Connecting, Lobby, or Racing): holding the EE
// thread freezes the local simulation, which desyncs multiplayer. Which tab is on screen never matters, so
// switching tabs never resumes a held game or its music; the network pump keeps running while the game is
// held (hold_for_menu draws the menu through overlay_frame, which polls the session).
bool overlay_paused() {
    return g_open && g_config.pause_in_menu && !netplay::session().active() && !(g_menu && g_menu->error_active());
}
void overlay_toggle() { open_menu(!g_open); }
bool overlay_in_screenshots() {
    const char *e = getenv("EOD_SCREENSHOT_OVERLAY");
    return !(e && e[0] == '0');
}

bool overlay_key(int vk, bool down) {
    if (vk == 0x10) g_shift = down;
    if (!g_available) return false;
    // F1 (and Cmd+, on the Mac) opens/closes the full menu. During gameplay (online or Unified Front-End
    // Career/Solo/Split-Screen), Esc opens the Screen D translucent Quick Pause Rail.
    const bool in_gameplay = netplay::session().event_active() ||
        (g_config.unified_frontend && g_ram && (ee_r32(0x00437BB8) != 0 || ee_r32(0x003CDA04) != 0));
    if (!g_open && down && vk == 0x1B && in_gameplay) {
        if (!g_context) ensure_context();
        if (g_menu) g_menu->set_quick_rail(true);
        open_menu(true);
        play(frontend::Sound::Select);
        return true;
    }
    if (down && vk == 0x70) {
        if (!g_context) ensure_context();
        if (g_menu && (g_menu->error_active() || g_menu->online_modal_title())) return true;
        if (vk == 0x70 && g_open && g_menu && g_menu->rebinding() < 0) { open_menu(false); play(frontend::Sound::Back); return true; }
        if (!g_open) { open_menu(true); play(frontend::Sound::Select); return true; }
    }
    if (!g_open) return false;
    // Let the platform's focused F9 shortcut through during ordinary menu navigation.
    if (down && vk == 0x78 && g_menu && g_menu->rebinding() < 0 &&
        !g_menu->editing_text() && !g_menu->error_active()) return false;
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
    g_mouse_down = down;
    if (!down && g_dragging_slider) g_mouse_moved = true;
    if (down && g_open) {
        g_click = true;
        g_glyphs = frontend::Glyphs::Keyboard;
    }
}
void overlay_mouse_wheel(float dy) { if (g_open) g_wheel += dy; }

void overlay_show_error(const std::string &title, const std::string &message, bool offer_disc, OverlayDiscHandler handler) {
    g_available = true;
    ensure_context();
    load_menu_art();
    g_disc_handler = handler;
    g_error_result = 0;
    g_menu->show_error(title, message, offer_disc);
    g_open = true;
}
bool overlay_fatal_screen(const std::string &title, const std::string &message) {
    if (!g_available) return false;  // Renderer startup failed before the overlay could draw.
    g_fatal_screen = true;
    ensure_context();
    load_menu_art();
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
    if (g_menu) {
        g_menu->set_quick_rail(false);
        g_menu->set_tab(frontend::Tab::Play);
        g_menu->set_session_kind(game_frontend::SessionKind::Online);
    }
    if (!g_open) { open_menu(true); play(frontend::Sound::Select); }
}

void overlay_resume_join(const game_frontend::Command &command) {
    g_resumed_invite = std::make_unique<game_frontend::Command>(command);
    netplay::session().submit(command);
}

// Developer test hook: EOD_MENU_SCRIPT="FRAME:ROW[:left|right],..." presses a menu row by its id at a presented
// frame (game vblanks plus frames drawn while the menu holds the game), through the menu's own input path.
static void menu_script(frontend::Input &in) {
    struct Step { uint64_t frame; std::string id; int dx; bool done; };
    static std::vector<Step> steps;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char *e = getenv("EOD_MENU_SCRIPT")) {
            std::string all = e;
            for (size_t pos = 0; pos < all.size();) {
                size_t end = all.find(',', pos);
                if (end == std::string::npos) end = all.size();
                const std::string item = all.substr(pos, end - pos);
                pos = end + 1;
                const size_t c1 = item.find(':');
                if (c1 == std::string::npos) continue;
                const size_t c2 = item.rfind(':');
                Step st{strtoull(item.c_str(), nullptr, 10), item.substr(c1 + 1), 0, false};
                if (c2 != c1) {
                    const std::string tail = item.substr(c2 + 1);
                    if (tail == "left" || tail == "right") { st.dx = tail == "left" ? -1 : 1; st.id = item.substr(c1 + 1, c2 - c1 - 1); }
                }
                steps.push_back(st);
            }
        }
    }
    if (steps.empty() || !g_open || !g_menu) return;
    const uint64_t frame = kernel_vblank_count() + kernel_menu_hold_frames();
    for (Step &st : steps) {
        if (st.done || frame < st.frame) continue;
        st.done = true;
        bool found = false;
        for (int t = 0; t < (int)frontend::Tab::Count && !found; t++) {
            const auto &rows = g_menu->rows_for((frontend::Tab)t);
            for (size_t i = 0; i < rows.size(); i++) {
                if (rows[i].id != st.id) continue;
                g_menu->set_tab((frontend::Tab)t);
                g_menu->select((int)i);
                if (st.dx) in.dx = st.dx; else in.accept = true;
                found = true;
                break;
            }
        }
        fprintf(stderr, "[menu] script frame=%llu row=%s %s\n", (unsigned long long)frame, st.id.c_str(), found ? "pressed" : "NOT FOUND");
        return;   // one step per frame
    }
}

void overlay_frame() {
    static const bool menu_trace = getenv("EOD_MENU_TRACE") != nullptr;
    const uint64_t poll_t0 = menu_trace ? platform_time_us() : 0;
    struct FrameClock {  // developer trace: a menu frame that holds the game thread (the app's main thread)
        uint64_t t0; bool on;
        ~FrameClock() { if (on && platform_time_us() - t0 > 50000) fprintf(stderr, "[menu] STALL: menu frame took %llu ms\n", (unsigned long long)((platform_time_us() - t0) / 1000)); }
    } frame_clock{poll_t0, menu_trace};
    netplay::session().poll();   // network pump: every vblank, also with the menu closed or headless
    if (menu_trace && platform_time_us() - poll_t0 > 50000)
        fprintf(stderr, "[menu] STALL: network pump held the game thread %llu ms\n", (unsigned long long)((platform_time_us() - poll_t0) / 1000));
    static const bool trace_names = std::getenv("EOD_NETPLAY_TEST_NAMES") != nullptr;
    if (!g_available && !trace_names) return;
    if (g_available && g_resumed_invite) {
        overlay_show_online();
        g_menu->remember_join(*g_resumed_invite);
        g_resumed_invite.reset();
    }
    const auto tags = (!g_open || trace_names) ? netplay::session().driver_tags() : std::vector<netplay::names::Tag>{};
    if (!g_available) return;
    const uint64_t renderer_notice = g_renderer_notice.load(std::memory_order_relaxed);
    const bool show_renderer_look = renderer_notice && (renderer_notice >> kRendererLookBits) > platform_time_us();
    const bool mutator_hud_visible = mutator_hooks::hud.visible &&
        (netplay::session().event_active() || (g_ram && ee_r32(0x0048522C) != 0 && ee_r32(0x003CF194) != 0));
    if (!g_context) {
        // the chord before the first use (no context yet: nothing else to do)
        bool toggled = false;
        for (int p = 0; p < 4; p++) {
            HostPadState s;
            overlay_platform_pad(p, &s);
            if (g_pads[p].update(s).toggle) toggled = true;
        }
        if (toggled || g_open) { ensure_context(); g_open = false; open_menu(true); play(frontend::Sound::Select); }
        if (!g_open && !g_config.show_fps && !show_renderer_look && !mutator_hud_visible && netplay::session().strip().empty() && tags.empty()) return;
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
    menu_script(in);
    if (toggled && !g_menu->error_active() && !g_menu->online_modal_title()) {
        open_menu(!g_open);
        play(g_open ? frontend::Sound::Select : frontend::Sound::Back);
    } else if (g_open && g_import_state != 1) {
        apply(g_menu->update(in));
    }
    if (menu_trace) {  // developer trace: menu tab and whether the game is held, per change
        static int last = -1;
        const int now = g_open ? (int)g_menu->tab() * 2 + (overlay_paused() ? 1 : 0) : -1;
        if (now != last) {
            last = now;
            fprintf(stderr, "[menu] open=%d tab=%d paused=%d vblank=%llu\n", (int)g_open, g_open ? (int)g_menu->tab() : -1,
                    (int)overlay_paused(), (unsigned long long)kernel_vblank_count());
        }
    }
    if (g_release_wait) {
        bool any = false;
        for (int p = 0; p < 4; p++) {
            HostPadState s;
            overlay_platform_pad(p, &s);
            any |= s.connected && s.buttons;
        }
        any |= overlay_platform_keys_held();  // e.g. the Esc/Enter that closed the menu: not a game press
        if (!any) g_release_wait = false;
    }
    const std::string strip = netplay::session().strip();
    const auto *event_results = netplay::session().event_results();
    const std::vector<std::string> speakers = netplay::session().speakers();
    if (!g_open && !g_config.show_fps && !show_renderer_look && !mutator_hud_visible && strip.empty() && speakers.empty() && !event_results && tags.empty()) {
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
    if (!g_open && !tags.empty()) {
        const int requested = presentation::requested.load(std::memory_order_relaxed);
        const auto rect = display_rect(requested >= 0 ? requested : display_aspect(g_config.aspect, presentation::active_drawable()), w, h);
        const float scale = rect.height / 720.f;
        for (const auto &tag : tags) {
            const float size = scale * (15.f + 12.f / (1.f + tag.distance / 18.f));
            const ImVec2 text = g_font_main->CalcTextSizeA(size, FLT_MAX, 0.f, tag.name.c_str());
            const float x = rect.x + tag.x * rect.width - text.x * 0.5f;
            const float y = rect.y + tag.y * rect.height - text.y - 3.f * scale;
            dl->PushClipRect(ImVec2(rect.x, rect.y), ImVec2(rect.x + rect.width, rect.y + rect.height), true);
            dl->AddRectFilled(ImVec2(x - 4.f * scale, y - scale), ImVec2(x + text.x + 4.f * scale, y + text.y + scale),
                              IM_COL32(14, 12, 10, 180), 3.f * scale);
            dl->AddText(g_font_main, size, ImVec2(x, y), IM_COL32(255, 250, 238, 255), tag.name.c_str());
            dl->PopClipRect();
        }
    }
    if (g_open) frontend_draw::draw_menu(dl, *g_menu, c, g_hits);
    else g_hits.clear();
    if (!g_open && mutator_hud_visible) {
        const auto &mh = mutator_hooks::hud;
        const bool tips_beacon = ((mh.active_rule_flags & mutator::RF_TIPS_TAG) != 0) &&
            mh.tips_in_slot < mutator::kMaxSlots;
        // 3D-projected trigger zones, shrinking Battle Royale storm ring, CTF flags, chain tethers & Tips "IN!" beacon
        if (mutator_hooks::world_camera.valid &&
            (!mutator_hooks::active_triggers.empty() || !mutator_hooks::active_chains.empty() || tips_beacon)) {
            const int requested = presentation::requested.load(std::memory_order_relaxed);
            const auto rect = display_rect(requested >= 0 ? requested : display_aspect(g_config.aspect, presentation::active_drawable()), w, h);
            const auto &wc = mutator_hooks::world_camera;
            netplay::names::Camera cam{};
            for (int i = 0; i < 3; ++i) {
                cam.position[i] = wc.position[i];
                for (int j = 0; j < 3; ++j) cam.axes[i][j] = wc.axes[i][j];
            }
            cam.horizontal_scale = wc.horizontal_scale;
            cam.vertical_scale = wc.vertical_scale;
            cam.width = wc.width;
            cam.height = wc.height;

            auto proj_screen = [&](float wx, float wy, float wz, ImVec2 &out_pt) -> bool {
                netplay::names::Tag tg;
                if (!netplay::names::project(cam, {wx, wy, wz}, tg)) return false;
                out_pt = ImVec2(rect.x + tg.x * rect.width, rect.y + tg.y * rect.height);
                return true;
            };
            auto rgba_to_u32 = [](uint32_t rgba, uint8_t alpha_override = 0) -> ImU32 {
                const uint8_t r = (uint8_t)((rgba >> 16) & 0xFF);
                const uint8_t g = (uint8_t)((rgba >> 8) & 0xFF);
                const uint8_t b = (uint8_t)(rgba & 0xFF);
                const uint8_t a = alpha_override ? alpha_override : (uint8_t)((rgba >> 24) & 0xFF);
                return IM_COL32(r, g, b, a);
            };

            dl->PushClipRect(ImVec2(rect.x, rect.y), ImVec2(rect.x + rect.width, rect.y + rect.height), true);
            const float line_scale = std::max(1.0f, rect.height / 720.f);

            for (const auto &tr : mutator_hooks::active_triggers) {
                if (!tr.active) continue;
                const ImU32 col = rgba_to_u32(tr.color_rgba, 220);
                const ImU32 col_hi = rgba_to_u32(tr.color_rgba, 110);
                if (tr.kind == mutator::TriggerKind::RingOut || tr.kind == mutator::TriggerKind::Cylinder ||
                    tr.kind == mutator::TriggerKind::Sphere) {
                    constexpr int kSegs = 48;
                    const float wall_h = (tr.kind == mutator::TriggerKind::RingOut) ? 14.0f : std::min(tr.extents[1], 10.0f);
                    for (int i = 0; i < kSegs; ++i) {
                        const float a0 = (2.0f * 3.14159265f * i) / kSegs;
                        const float a1 = (2.0f * 3.14159265f * (i + 1)) / kSegs;
                        const float x0 = tr.center[0] + std::cos(a0) * tr.radius;
                        const float z0 = tr.center[2] + std::sin(a0) * tr.radius;
                        const float x1 = tr.center[0] + std::cos(a1) * tr.radius;
                        const float z1 = tr.center[2] + std::sin(a1) * tr.radius;
                        ImVec2 p0, p1, q0, q1;
                        if (proj_screen(x0, tr.center[1] + 0.5f, z0, p0) && proj_screen(x1, tr.center[1] + 0.5f, z1, p1)) {
                            dl->AddLine(p0, p1, col, 2.5f * line_scale);
                        }
                        if (proj_screen(x0, tr.center[1] + wall_h, z0, q0) && proj_screen(x1, tr.center[1] + wall_h, z1, q1)) {
                            dl->AddLine(q0, q1, col_hi, 1.5f * line_scale);
                        }
                        if ((i % 4) == 0 && proj_screen(x0, tr.center[1] + 0.5f, z0, p0) && proj_screen(x0, tr.center[1] + wall_h, z0, q0)) {
                            dl->AddLine(p0, q0, col_hi, 1.5f * line_scale);
                        }
                    }
                } else if (tr.kind == mutator::TriggerKind::OBB) {
                    const float c = std::cos(tr.yaw_rad), s = std::sin(tr.yaw_rad);
                    const float hx = tr.extents[0], hz = tr.extents[2];
                    const float sx[4] = {-hx, hx, hx, -hx}, sz[4] = {-hz, -hz, hz, hz};
                    ImVec2 lo[4], hi[4];
                    bool lo_ok[4], hi_ok[4];
                    for (int k = 0; k < 4; ++k) {
                        const float wx = tr.center[0] + sx[k] * c - sz[k] * s;
                        const float wz = tr.center[2] + sx[k] * s + sz[k] * c;
                        lo_ok[k] = proj_screen(wx, tr.center[1] + 0.4f, wz, lo[k]);
                        hi_ok[k] = proj_screen(wx, tr.center[1] + std::min(tr.extents[1], 8.0f), wz, hi[k]);
                    }
                    for (int k = 0; k < 4; ++k) {
                        const int nk = (k + 1) & 3;
                        if (lo_ok[k] && lo_ok[nk]) dl->AddLine(lo[k], lo[nk], col, 2.5f * line_scale);
                        if (hi_ok[k] && hi_ok[nk]) dl->AddLine(hi[k], hi[nk], col_hi, 1.5f * line_scale);
                        if (lo_ok[k] && hi_ok[k]) dl->AddLine(lo[k], hi[k], col_hi, 1.5f * line_scale);
                    }
                } else if (tr.kind == mutator::TriggerKind::Flag) {
                    ImVec2 base, top;
                    if (proj_screen(tr.center[0], tr.center[1], tr.center[2], base) &&
                        proj_screen(tr.center[0], tr.center[1] + 4.5f, tr.center[2], top)) {
                        dl->AddLine(base, top, IM_COL32(255, 230, 80, 240), 2.5f * line_scale);
                        const float d = 9.0f * line_scale;
                        ImVec2 dia[4] = {ImVec2(top.x, top.y - d), ImVec2(top.x + d, top.y),
                                         ImVec2(top.x, top.y + d), ImVec2(top.x - d, top.y)};
                        dl->AddConvexPolyFilled(dia, 4, col);
                    }
                }
            }

            // Glowing spring-damper chain tethers between linked cars
            for (const auto &ch : mutator_hooks::active_chains) {
                const uint32_t ca = netplay_car::car_of_slot(ch.slot_a);
                const uint32_t cb = netplay_car::car_of_slot(ch.slot_b);
                if (!ca || !cb || ca > EE_RAM_SIZE - 0x94u || cb > EE_RAM_SIZE - 0x94u) continue;
                auto rflt = [](uint32_t a) { uint32_t u = ee_r32(a); float f; std::memcpy(&f, &u, 4); return f; };
                const float ax = rflt(ca + 0x88u), ay = rflt(ca + 0x8Cu) + 0.8f, az = rflt(ca + 0x90u);
                const float bx = rflt(cb + 0x88u), by = rflt(cb + 0x8Cu) + 0.8f, bz = rflt(cb + 0x90u);
                const float dist = std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay) + (bz - az) * (bz - az));
                const bool taut = dist > ch.rest_len_m;
                const ImU32 chain_col = taut ? IM_COL32(255, 96, 48, 245) : IM_COL32(245, 200, 90, 215);
                ImVec2 pa, pb;
                if (proj_screen(ax, ay, az, pa) && proj_screen(bx, by, bz, pb)) {
                    dl->AddLine(pa, pb, chain_col, (taut ? 4.0f : 2.5f) * line_scale);
                }
            }

            // Pulsing 3D red vertical beacon beam + diamond + "IN!" label above current Tips tagger
            if (tips_beacon && g_ram) {
                const uint32_t in_car = netplay_car::car_of_slot(mh.tips_in_slot);
                if (in_car && in_car <= EE_RAM_SIZE - 0x94u) {
                    auto rflt = [](uint32_t a) { uint32_t u = ee_r32(a); float f; std::memcpy(&f, &u, 4); return f; };
                    const float cx = rflt(in_car + 0x88u), cy = rflt(in_car + 0x8Cu), cz = rflt(in_car + 0x90u);
                    const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(static_cast<double>(now / 1000) * 0.012));
                    ImVec2 p_base, p_top;
                    if (proj_screen(cx, cy + 1.2f, cz, p_base) &&
                        proj_screen(cx, cy + 8.5f + 1.5f * pulse, cz, p_top)) {
                        const uint8_t glow_a = static_cast<uint8_t>(110 + static_cast<int>(120.f * pulse));
                        dl->AddLine(p_base, p_top, IM_COL32(255, 40, 40, glow_a), (7.5f + 3.0f * pulse) * line_scale);
                        dl->AddLine(p_base, p_top, IM_COL32(255, 225, 190, 240), 2.5f * line_scale);
                        const float d = (10.f + 4.f * pulse) * line_scale;
                        ImVec2 dia[4] = {ImVec2(p_top.x, p_top.y - d), ImVec2(p_top.x + d, p_top.y),
                                         ImVec2(p_top.x, p_top.y + d), ImVec2(p_top.x - d, p_top.y)};
                        dl->AddConvexPolyFilled(dia, 4, IM_COL32(255, 48, 48, 245));
                        dl->AddPolyline(dia, 4, IM_COL32(255, 245, 220, 255), ImDrawFlags_Closed, 2.0f * line_scale);
                        const float lbl_px = 18.f * line_scale;
                        const char *lbl = "IN!";
                        const ImVec2 lsz = g_font_main->CalcTextSizeA(lbl_px, FLT_MAX, 0.f, lbl);
                        const float lx = p_top.x - lsz.x * 0.5f;
                        const float ly = p_top.y - d - lsz.y - 6.f * line_scale;
                        dl->AddRectFilled(ImVec2(lx - 6.f * line_scale, ly - 2.f * line_scale),
                                          ImVec2(lx + lsz.x + 6.f * line_scale, ly + lsz.y + 2.f * line_scale),
                                          IM_COL32(180, 18, 18, 230), 4.f * line_scale);
                        dl->AddText(g_font_main, lbl_px, ImVec2(lx, ly), IM_COL32(255, 255, 255, 255), lbl);
                    }
                }
            }
            dl->PopClipRect();
        }

        const float scale = (float)h / 720.f;
        const float title_px = 20.f * scale;
        const float sub_px = 14.f * scale;
        const float pad_x = 16.f * scale, pad_y = 8.f * scale;

        char header[160];
        if (mh.active_rule_flags & mutator::RF_TIPS_TAG) {
            if (mh.local_role == mutator::RoleId::Tagger && mh.tips_in_slot < mutator::kMaxSlots) {
                std::snprintf(header, sizeof(header), "YOU'RE IN!  |  FUSE %.1fs", mh.tips_fuse_sec);
            } else if (mh.tips_in_slot < mutator::kMaxSlots) {
                std::snprintf(header, sizeof(header), "RUN!  |  ALIVE: %u  |  FUSE %.1fs",
                              (unsigned)mh.tips_alive_count, mh.tips_fuse_sec);
            } else {
                std::snprintf(header, sizeof(header), "TIPS (TAG)  |  ALIVE: %u", (unsigned)mh.tips_alive_count);
            }
        } else if (mh.total_rounds > 0 && mh.remaining_sec > 0.0f) {
            const int secs = (int)std::ceil(mh.remaining_sec);
            std::snprintf(header, sizeof(header), "ROUND %u/%u  |  %s  |  %d:%02d",
                          mh.round_number, mh.total_rounds, mh.round_title.c_str(), secs / 60, secs % 60);
        } else if (mh.remaining_sec > 0.0f) {
            const int secs = (int)std::ceil(mh.remaining_sec);
            std::snprintf(header, sizeof(header), "%s  |  %d:%02d",
                          mh.round_title.c_str(), secs / 60, secs % 60);
        } else {
            std::snprintf(header, sizeof(header), "%s", mh.round_title.c_str());
        }

        std::string badges;
        auto add_badge = [&](const char *b) {
            if (!badges.empty()) badges += "  ";
            badges += b;
        };
        switch (mh.local_role) {
        case mutator::RoleId::Survivor:   add_badge("[SURVIVOR]"); break;
        case mutator::RoleId::Infected:   add_badge("[INFECTED]"); break;
        case mutator::RoleId::Juggernaut: add_badge("[JUGGERNAUT]"); break;
        case mutator::RoleId::VIP:        add_badge("[VIP]"); break;
        case mutator::RoleId::Bus:        add_badge("[BUS]"); break;
        case mutator::RoleId::Tagger:     add_badge("[IN! - TAG SOMEONE]"); break;
        default: break;
        }
        if (mh.active_rule_flags & mutator::RF_INFECTION_TAG) {
            char ib[48];
            std::snprintf(ib, sizeof(ib), "[SURV:%u  INF:%u]", (unsigned)mh.survivors_alive, (unsigned)mh.infected_count);
            add_badge(ib);
        }
        if (mh.storm_radius_m > 0.5f) {
            char sb[32];
            std::snprintf(sb, sizeof(sb), "[STORM %.0fm]", mh.storm_radius_m);
            add_badge(sb);
        }
        if (mh.active_rule_flags & mutator::RF_CHAIN_TETHER) add_badge("[CHAINED]");
        if (mh.local_direction_sign < 0) add_badge("[REVERSED]");
        if (std::fabs(mh.local_gravity_scale - 1.0f) > 0.05f) {
            char gb[24];
            std::snprintf(gb, sizeof(gb), "[GRAV %.2fx]", mh.local_gravity_scale);
            add_badge(gb);
        }
        if (mh.local_feat_bits & mutator::FB_INVINCIBLE) add_badge("[INVINCIBLE]");
        if (mh.local_feat_bits & mutator::FB_ONE_HIT_KILLS) add_badge("[1-HIT]");
        if (mh.local_feat_bits & mutator::FB_DEADLY_AMBITION) add_badge("[DEADLY AMBITION]");
        if (mh.local_feat_bits & mutator::FB_RAIN_CHICKENS) add_badge("[CHICKEN RAIN]");
        else if (mh.local_feat_bits & (mutator::FB_CHICKEN_STRAIGHT | mutator::FB_CHICKEN_MORTAR)) add_badge("[CHICKENS]");
        if (mh.local_feat_bits & mutator::FB_TURBO) add_badge("[TURBO]");

        const ImVec2 h_sz = g_font_main->CalcTextSizeA(title_px, (float)w * 0.9f, 0.f, header);
        const ImVec2 b_sz = badges.empty() ? ImVec2(0, 0) : g_font_main->CalcTextSizeA(sub_px, (float)w * 0.9f, 0.f, badges.c_str());
        const float box_w = std::max(h_sz.x, b_sz.x) + 2.f * pad_x;
        const float box_h = h_sz.y + (badges.empty() ? 0.f : b_sz.y + 4.f * scale) + 2.f * pad_y;
        const float box_x = ((float)w - box_w) * 0.5f;
        const float box_y = 14.f * scale;
        const bool tips_critical = (mh.active_rule_flags & mutator::RF_TIPS_TAG) &&
            mh.tips_in_slot < mutator::kMaxSlots && mh.tips_fuse_sec < 5.0f;
        const ImU32 border = (tips_critical || (mh.remaining_sec > 0.0f && mh.remaining_sec <= 5.0f))
            ? IM_COL32(255, 72, 64, 240)
            : (mh.remaining_sec > 0.0f && mh.remaining_sec <= 10.0f)
                ? IM_COL32(255, 196, 48, 230)
                : IM_COL32(240, 140, 36, 200);
        dl->AddRectFilled(ImVec2(box_x, box_y), ImVec2(box_x + box_w, box_y + box_h), IM_COL32(14, 14, 18, 196), 8.f * scale);
        dl->AddRect(ImVec2(box_x, box_y), ImVec2(box_x + box_w, box_y + box_h), border, 8.f * scale, 0, 2.f * scale);
        dl->AddText(g_font_main, title_px, ImVec2(box_x + (box_w - h_sz.x) * 0.5f, box_y + pad_y),
                    IM_COL32(255, 250, 238, 255), header);
        if (!badges.empty()) {
            dl->AddText(g_font_main, sub_px, ImVec2(box_x + (box_w - b_sz.x) * 0.5f, box_y + pad_y + h_sz.y + 4.f * scale),
                        IM_COL32(255, 210, 110, 255), badges.c_str());
        }
        if (!mh.zone_warning.empty()) {
            const float warn_px = 18.f * scale;
            const ImVec2 w_sz = g_font_main->CalcTextSizeA(warn_px, (float)w * 0.85f, 0.f, mh.zone_warning.c_str());
            const float wx = ((float)w - w_sz.x) * 0.5f - 14.f * scale;
            const float wy = box_y + box_h + 8.f * scale;
            const bool danger = mh.zone_warning.find("OUTSIDE") != std::string::npos;
            dl->AddRectFilled(ImVec2(wx, wy), ImVec2(wx + w_sz.x + 28.f * scale, wy + w_sz.y + 10.f * scale),
                              danger ? IM_COL32(160, 22, 22, 220) : IM_COL32(24, 96, 48, 220), 6.f * scale);
            dl->AddText(g_font_main, warn_px, ImVec2(wx + 14.f * scale, wy + 5.f * scale),
                        IM_COL32(255, 255, 255, 255), mh.zone_warning.c_str());
        }
    }
    if (show_renderer_look && (!g_open || g_menu->tab() != frontend::Tab::Graphics)) {
        const auto look = static_cast<RendererLook>(renderer_notice & kRendererLookMask);
        const char *label = overlay_renderer_look_label(look);
        const float scale = h / 720.f, px = 24.f * scale, pad = 12.f * scale;
        const float width = g_font_main->CalcTextSizeA(px, (float)w, 0.f, label).x + 2.f * pad;
        const ImVec2 a((w - width) * 0.5f, strip.empty() ? pad : 38.f + pad);
        dl->AddRectFilled(a, ImVec2(a.x + width, a.y + px + pad),
                          IM_COL32(24, 14, 8, 230), 4.f * scale);
        dl->AddText(g_font_main, px, ImVec2(a.x + pad, a.y + pad * 0.5f),
                    IM_COL32(255, 180, 80, 255), label);
    }
    if (!strip.empty()) {
        dl->AddRectFilled(ImVec2(0, 0), ImVec2((float)w, 38), IM_COL32(24, 14, 8, 215));
        dl->AddRectFilled(ImVec2(0, 36), ImVec2((float)w, 38), IM_COL32(230, 120, 30, 255));
        dl->AddText(g_font_main, 22.f, ImVec2(18, 7), IM_COL32(255, 160, 50, 255), strip.c_str());
    }
    if (event_results) {
        using netplay::EventFinishState;
        const float scale = std::min(w / 720.f, h / 640.f);
        const float width = 490.f * scale;
        const float height = (93.f + 27.f * event_results->size()) * scale;
        const float x = (w - width) * 0.5f, y = (h - height) * 0.5f;
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height), IM_COL32(24, 14, 8, 245), 8.f * scale);
        dl->AddText(g_font_main, 31.f * scale, ImVec2(x + 24.f * scale, y + 14.f * scale),
                    IM_COL32(255, 180, 80, 255), "Event results");
        float row_y = y + 59.f * scale;
        for (const auto &row : *event_results) {
            std::string status;
            switch (row.state) {
            case EventFinishState::Finished: status = "Finished"; break;
            case EventFinishState::Totalled: status = "Totalled"; break;
            case EventFinishState::Completed: status = "Event complete"; break;
            case EventFinishState::BattleScore: status = std::to_string(row.score) + (row.score == 1 ? " point" : " points"); break;
            case EventFinishState::Withdrawn: status = "Withdrew"; break;
            case EventFinishState::Disconnected: status = "Disconnected"; break;
            case EventFinishState::TimedOut: status = "Time expired"; break;
            case EventFinishState::Survivor: status = "Winner"; break;
            case EventFinishState::Active: break;
            }
            const std::string rank = row.rank ? std::to_string(row.rank) : "-";
            const auto color = IM_COL32(250, 240, 225, 255);
            dl->AddText(g_font_main, 22.f * scale, ImVec2(x + 24.f * scale, row_y), color, rank.c_str());
            dl->AddText(g_font_main, 22.f * scale, ImVec2(x + 70.f * scale, row_y), color, row.name.c_str());
            dl->AddText(g_font_main, 22.f * scale, ImVec2(x + 325.f * scale, row_y), color, status.c_str());
            row_y += 27.f * scale;
        }
        dl->AddText(g_font_main, 20.f * scale, ImVec2(x + 24.f * scale, row_y + 3.f * scale),
                    IM_COL32(255, 180, 80, 255), "Returning to the lobby...");
    }
    if (!speakers.empty()) {
        float y = strip.empty() ? 18.f : 48.f;
        for (const std::string &spk : speakers) {
            const float x = 18.f, bh = 32.f, bw = 210.f;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + bw, y + bh), IM_COL32(18, 24, 18, 220), 4.f);
            dl->AddRect(ImVec2(x, y), ImVec2(x + bw, y + bh), IM_COL32(110, 235, 120, 240), 4.f, 0, 1.5f);
            // Halo-style speaker cone + radiating voice waves
            const float cx = x + 14.f, cy = y + bh * 0.5f;
            dl->AddRectFilled(ImVec2(cx - 5.f, cy - 3.5f), ImVec2(cx - 1.f, cy + 3.5f), IM_COL32(130, 255, 140, 255));
            ImVec2 cone[4] = {ImVec2(cx - 1.f, cy - 3.5f), ImVec2(cx + 4.f, cy - 8.f), ImVec2(cx + 4.f, cy + 8.f), ImVec2(cx - 1.f, cy + 3.5f)};
            dl->AddConvexPolyFilled(cone, 4, IM_COL32(130, 255, 140, 255));
            dl->AddBezierQuadratic(ImVec2(cx + 7.f, cy - 5.f), ImVec2(cx + 11.f, cy), ImVec2(cx + 7.f, cy + 5.f), IM_COL32(130, 255, 140, 255), 1.8f);
            dl->AddBezierQuadratic(ImVec2(cx + 10.f, cy - 8.f), ImVec2(cx + 16.f, cy), ImVec2(cx + 10.f, cy + 8.f), IM_COL32(130, 255, 140, 200), 1.8f);
            dl->AddText(g_font_main, 19.f, ImVec2(x + 36.f, y + 6.f), IM_COL32(235, 255, 235, 255), spk.c_str());
            y += bh + 6.f;
        }
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
    netplay::session().mix_voice(stereo, frames);
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
