// frontend.cpp - the front end's model (see frontend.h).
#include "frontend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "../app/game_frontend.h"

namespace fs = std::filesystem;

namespace frontend {

const char *tab_name(Tab t) {
    switch (t) {
    case Tab::Graphics: return "GRAPHICS";
    case Tab::Mods: return "MODS";
    case Tab::Settings: return "SETTINGS";
    case Tab::Inputs: return "INPUTS";
    case Tab::Online: return "ONLINE";
    default: return "";
    }
}

int Repeater::step(int held, uint64_t now) {
    if (!held) { last_ = 0; next_ = 0; return 0; }
    if (held != last_) { last_ = held; next_ = now + 350; return held; }
    if (now >= next_) { next_ = now + 100; return held; }
    return 0;
}

uint8_t shape_axis(uint8_t v, int inner, int outer, int curve) {
    if (inner <= 0 && outer >= 100 && curve == 100) return v;  // the default: unchanged
    float f = ((int)v - 128) / (v < 128 ? 128.0f : 127.0f);
    float m = std::fabs(f);
    float lo = std::clamp(inner, 0, 90) / 100.0f, hi = std::clamp(outer / 100.0f, lo + 0.05f, 1.0f);
    if (m <= lo) return 128;
    m = std::min(1.0f, (m - lo) / (hi - lo));
    m = std::pow(m, std::clamp(curve, 50, 300) / 100.0f);
    int out = (int)std::lround(128 + (f < 0 ? -m * 128 : m * 127));
    return (uint8_t)std::clamp(out, 0, 255);
}

uint8_t shape_trigger(uint8_t v, int dz, int curve) {
    if (dz <= 0 && curve == 100) return v;
    const float lo = std::clamp(dz, 0, 90) / 100.0f;
    float m = v / 255.0f;
    if (m <= lo) return 0;
    m = std::pow((m - lo) / (1.0f - lo), std::clamp(curve, 50, 300) / 100.0f);
    return (uint8_t)std::clamp((int)std::lround(m * 255), 0, 255);
}

// ------------------------------------------------------------------ mods
std::vector<ModEntry> builtin_mods() {
    std::vector<ModEntry> m;
    auto add = [&](const char *id, const char *name, const char *kind, const char *desc) {
        ModEntry e;
        e.id = id; e.name = name; e.version = "built-in"; e.author = "TDEOD recomp"; e.kind = kind;
        e.description = desc; e.builtin = true; e.enabled = true;
        m.push_back(e);
    };
    add("phantom", "Phantom", "car",
        "The Phantom (vehicles 775/776) in the Action car list, with every native record preserved.");
    add("cupcar", "NASCAR Cup car", "car",
        "The NASCAR Cup car (vehicles 777-779) with its own chassis, wheels and engine audio.");
    add("battle", "Battle content", "events",
        "Battle 1v1 (multiplayer), Battle 4 AI and Battle 8 AI on every map in Action and Multiplayer, "
        "Career Battles, the Welch respawn table and the Battle bots.");
    return m;
}

namespace {
std::string json_field(const std::string &text, const char *key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t at = text.find(k);
    if (at == std::string::npos) return "";
    at = text.find(':', at + k.size());
    if (at == std::string::npos) return "";
    at = text.find('"', at);
    if (at == std::string::npos) return "";
    std::string out;
    for (size_t i = at + 1; i < text.size() && text[i] != '"'; ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) ++i;
        out.push_back(text[i]);
    }
    return out;
}
}  // namespace

std::vector<ModEntry> discover_mods(const std::string &root) {
    std::vector<ModEntry> out;
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec)) return out;
    std::vector<fs::path> dirs;
    for (const auto &e : fs::directory_iterator(root, ec)) {
        const std::string name = e.path().filename().string();
        if (name.empty() || name[0] == '.' || name == "integrated" || name == "music") continue;  // music/: the Custom Music folder
        if (e.is_directory(ec)) dirs.push_back(e.path());
    }
    std::sort(dirs.begin(), dirs.end());
    for (const auto &d : dirs) {
        ModEntry m;
        m.id = d.filename().string();
        m.name = m.id;
        m.enabled = true;  // on unless the user's saved choice (Config::mods) says otherwise
        std::ifstream f(d / "mod.json");
        if (!f) {
            m.conflict = "No mod.json: not a mod folder";
        } else {
            std::stringstream ss;
            ss << f.rdbuf();
            const std::string t = ss.str();
            if (t.find('{') == std::string::npos) m.conflict = "mod.json is not valid JSON";
            std::string v;
            if (!(v = json_field(t, "name")).empty()) m.name = v;
            m.version = json_field(t, "version");
            m.author = json_field(t, "author");
            m.kind = json_field(t, "kind");
            m.description = json_field(t, "description");
            if (m.conflict.empty() && m.kind.empty()) m.conflict = "mod.json has no \"kind\"";
        }
        out.push_back(m);
    }
    return out;
}

// ------------------------------------------------------------------ menu
Menu::Menu(Config &cfg) : cfg_(cfg) { mods_ = builtin_mods(); }
Menu::~Menu() = default;

void Menu::set_network(std::shared_ptr<game_frontend::Network> n) { net_ = std::move(n); }

void Menu::rescan_mods() {
    std::vector<ModEntry> found = discover_mods(host_.mods_root);
    std::vector<ModEntry> next = builtin_mods();
    // keep the user's order/toggles of entries still present
    bool kept = false;
    for (const ModEntry &old : mods_)
        if (!old.builtin)
            for (ModEntry &f : found)
                if (f.id == old.id && !f.id.empty()) { f.enabled = old.enabled; next.push_back(f); f.id.clear(); kept = true; }
    // otherwise the choice saved in eod.ini: its toggles and its load order (unlisted mods are on, after the listed ones)
    if (!kept) {
        for (const auto &saved : cfg_.mods)
            for (ModEntry &f : found)
                if (f.id == saved.first && !f.id.empty()) { f.enabled = saved.second; next.push_back(f); f.id.clear(); }
    } else {
        for (ModEntry &f : found)
            if (!f.id.empty())
                for (const auto &saved : cfg_.mods) if (saved.first == f.id) f.enabled = saved.second;
    }
    for (ModEntry &f : found)
        if (!f.id.empty()) next.push_back(f);
    mods_ = next;
}

void Menu::store_mods() {
    std::vector<std::pair<std::string, bool>> out;
    for (const ModEntry &m : mods_)
        if (!m.builtin) out.emplace_back(m.id, m.enabled);
    // a mod whose folder is missing right now keeps its remembered choice
    for (const auto &old : cfg_.mods) {
        bool present = false;
        for (const auto &o : out) present = present || o.first == old.first;
        if (!present) out.push_back(old);
    }
    cfg_.mods = out;
    dirty_ = true;
}

void Menu::open() {
    details_ = false;
    music_open_ = false;
    if (music_) music_->rescan();
    status_.clear();
    rebind_ = -1;
    text_id_.clear();
    rescan_mods();
}

void Menu::set_tab(Tab t) {
    tab_ = t;
    details_ = false;
    status_.clear();
}

void Menu::select(int row) {
    std::vector<Row> &r = rows_for(error_active() ? Tab::Count : tab_);
    if (row >= 0 && row < (int)r.size() && r[row].kind != RowKind::Header) sel_[error_active() ? (int)Tab::Count : (int)tab_] = row;
}

void Menu::show_error(const std::string &title, const std::string &message, bool offer_disc) {
    error_title_ = title;
    error_message_ = message;
    error_disc_ = offer_disc;
    browse_ = false;
    sel_[(int)Tab::Count] = 0;
}

std::vector<Row> &Menu::rows_for(Tab t) {
    rebuild(t);
    return rows_[(int)t];
}

void Menu::rebuild(Tab t) {
    std::vector<Row> &r = rows_[(int)t];
    r.clear();
    switch (t) {
    case Tab::Graphics: rows_graphics(r); break;
    case Tab::Mods: rows_mods(r); break;
    case Tab::Settings: rows_settings(r); break;
    case Tab::Inputs: rows_inputs(r); break;
    case Tab::Online: rows_online(r); break;
    default: rows_error(r); break;
    }
}

namespace {
Row choice(const char *id, const char *label, const std::string &value, const char *note, const char *help) {
    Row r;
    r.id = id; r.kind = RowKind::Choice; r.label = label; r.value = value; r.note = note; r.help = help;
    return r;
}
Row toggle(const char *id, const char *label, bool on, const char *note, const char *help) {
    Row r = choice(id, label, on ? "ON" : "OFF", note, help);
    r.kind = RowKind::Toggle;
    r.checked = on;
    return r;
}
Row header(const char *label) {
    Row r;
    r.kind = RowKind::Header;
    r.label = label;
    r.enabled = false;
    return r;
}
Row info(const char *id, const char *label, const std::string &value, const char *help = "") {
    Row r;
    r.id = id; r.kind = RowKind::Info; r.label = label; r.value = value; r.help = help;
    return r;
}
Row action(const char *id, const char *label, const std::string &value, const char *help) {
    Row r;
    r.id = id; r.kind = RowKind::Action; r.label = label; r.value = value; r.help = help;
    return r;
}

const char *kScales[] = {"AUTO (WINDOW)", "1X 640x448 (PS2)", "2X 1280x896", "3X 1920x1344", "4X 2560x1792",
                         "5X 3200x2240", "6X 3840x2688"};
const char *kAspects[] = {"auto", "4:3", "16:9", "16:10", "stretch"};
const char *kAspectLabels[] = {"AUTO (GAME OPTION)", "4:3 (ORIGINAL)", "16:9 WIDESCREEN", "16:10 WIDESCREEN", "STRETCH"};
const char *kDetails[] = {"original", "high", "maximum"};

int index_of(const char *const *list, int n, const std::string &v) {
    for (int i = 0; i < n; i++)
        if (v == list[i]) return i;
    return 0;
}
std::string upper(std::string s) {
    for (char &c : s) c = (char)toupper((unsigned char)c);
    return s;
}
std::string percent(int v) { return std::to_string(v) + "%"; }
std::string curve_label(int v) {
    char b[32];
    snprintf(b, sizeof b, v == 100 ? "LINEAR" : "%.2f", v / 100.0);
    return b;
}
}  // namespace

void Menu::rows_graphics(std::vector<Row> &r) {
#ifdef _WIN32
    const char *scale_note = "NEXT LAUNCH";
    const char *vsync_note = "LIVE";
#else
    const char *scale_note = "NEXT LAUNCH";
    const char *vsync_note = "NEXT LAUNCH";
#endif
    int sc = std::clamp(cfg_.render_scale, 0, 6);
#ifdef _WIN32
    if (sc == 0) sc = 1;
#endif
    r.push_back(choice("scale", "RENDER SCALE", kScales[sc], scale_note,
                       "Internal resolution the 3D is drawn at. Higher is sharper and needs a faster GPU; it also "
                       "supersamples (the anti-aliasing of this renderer). AUTO matches the display's pixel height."));
    r.push_back(choice("aspect", "ASPECT RATIO", kAspectLabels[index_of(kAspects, 5, cfg_.aspect)], "NEXT LAUNCH",
                       "Display shape. 16:9 and 16:10 switch the game's own widescreen camera and HUD; menus and "
                       "movies keep their 4:3 composition. Camera and composition change together on the next launch."));
    r.push_back(toggle("fullscreen", "FULLSCREEN", cfg_.fullscreen, "LIVE", "Borderless window over the whole screen."));
    r.push_back(toggle("vsync", "VSYNC", cfg_.vsync, vsync_note, "Waits for the display between frames: no tearing."));
    r.push_back(choice("smooth", "SCREEN FILTERING", cfg_.smooth_display ? "SMOOTH (BILINEAR)" : "SHARP (NEAREST)", "LIVE",
                       "How the finished image is scaled to the window."));
    r.push_back(toggle("frameskip", "FRAME SKIPPING", cfg_.frame_skip, "LIVE",
                       "When drawing falls behind, whole frames are skipped so the game keeps its native speed. "
                       "There is no frame-rate cap option: the game runs at its native 60 Hz."));
    r.push_back(choice("detail", "DETAIL PRESET", upper(kDetails[index_of(kDetails, 3, cfg_.detail)]), "NEXT LAUNCH",
                       "Optional car and scenery level-of-detail preset (docs/DETAIL_PRESETS.md)."));
    r.push_back(toggle("fps", "SHOW FRAME RATE", cfg_.show_fps, "LIVE", "Frame counter in the top right corner."));
}

void Menu::rows_settings(std::vector<Row> &r) {
    r.push_back(header("AUDIO"));
    r.push_back(toggle("audio", "SOUND", cfg_.audio, "LIVE", "All game sound on or off."));
    r.push_back(choice("volume", "MASTER VOLUME", percent(cfg_.volume), "LIVE",
                       "The game mixes music and effects itself; this is the one volume the port has."));
    r.push_back(header("FRONT END"));
    r.push_back(toggle("pause", "PAUSE GAME IN THIS MENU", cfg_.pause_in_menu, "LIVE",
                       "Holds the game while this menu is open (never during online play)."));
    r.push_back(header("GAMEPLAY OPTIONS"));
    r.push_back(toggle("unlock", "UNLOCK ALL", cfg_.unlock_all, "NEXT LAUNCH",
                       "All tracks, cars, dares and feat prizes unlocked out of the box."));
    r.push_back(toggle("career_cheats", "CHEATS IN CAREER", cfg_.career_cheats, "LIVE",
                       "Lets unlocked cheat feats work in Career too."));
    r.push_back(toggle("bots", "BATTLE BOTS", cfg_.battle_bots, "NEXT EVENT",
                       "Fills Battle to four cars with AI opponents using native weapons and respawns."));
    r.push_back(toggle("spbattle", "SINGLE PLAYER BATTLE", cfg_.sp_battle_mode, "NEXT MENU",
                       "Adds Battle to the solo Action tracks."));
    r.push_back(toggle("recovery", "MANUAL RECOVERY ONLY", cfg_.manual_recovery, "LIVE",
                       "No automatic wrong-way or upside-down countdown for humans; SELECT still recovers. AI unchanged."));
    r.push_back(toggle("durability", "PER-CAR DURABILITY", cfg_.car_durability, "LIVE",
                       "Each car's own damage scale from its files (the game's menus already rate cars by it)."));
    r.push_back(toggle("turbo", "TURBO BOOST (L2)", cfg_.cheat_turbo, "LIVE", "Optional cheat: L2 boosts."));
    r.push_back(toggle("chickens", "CHICKEN FIGHT (L2)", cfg_.cheat_chickens, "LIVE", "Optional cheat: L2 fires chickens."));
    r.push_back(toggle("invincible", "INVINCIBLE HUMANS", cfg_.cheat_invincible, "LIVE",
                       "Optional cheat: human players take no damage; AI stay vulnerable."));
}

void Menu::rows_inputs(std::vector<Row> &r) {
    r.push_back(header("PLAYERS"));
    for (int p = 0; p < 2; p++) {
        Row d = action(p ? "dev2" : "dev1", p ? "PLAYER 2 (P2)" : "PLAYER 1 (P1)",
                       host_.device[p].empty() ? "NONE" : upper(host_.device[p]),
                       "Controllers fill P1 then P2 as they are plugged in (hot-plug). Select to swap P1 and P2.");
        r.push_back(d);
    }
#ifndef _WIN32
    static const char *kb[] = {"AUTO", "OFF", "P1", "P2"};
    int k = std::clamp(cfg_.keyboard_port + 1, 0, 3);
    r.push_back(choice("kbport", "KEYBOARD & MOUSE", kb[k], "LIVE",
                       "AUTO: the keyboard is P1 alone, or P2 next to a P1 controller."));
    r.push_back(choice("padlayout", "CONTROLLER LAYOUT", cfg_.controller_layout == "modern" ? "TRIGGERS (DEFAULT)" : "ORIGINAL (PS2)",
                       "LIVE", "TRIGGERS: RT is analog gas and LT analog brake (proportional travel); the native L2/R2 "
                       "actions move to the stick clicks. ORIGINAL: the PS2 mapping, triggers are L2/R2."));
    r.push_back(choice("kblayout", "KEYBOARD LAYOUT", cfg_.keyboard_layout == "racing" ? "RACING (WASD/ARROWS)" : "ORIGINAL (KEYS BELOW)",
                       "LIVE", "Racing: context-aware WASD and arrows. Original: the PS2 keys below."));
#endif
    r.push_back(toggle("vibration", "VIBRATION", cfg_.vibration, "LIVE", "Controller rumble."));
    for (int p = 0; p < 2; p++) {
        r.push_back(header(p ? "PLAYER 2 STICK" : "PLAYER 1 STICK"));
        const char *help = "Input mapping only: the car's physics is untouched.";
        r.push_back(choice(p ? "dz2" : "dz1", "INNER DEADZONE", percent(cfg_.stick_deadzone[p]), "LIVE", help));
        r.push_back(choice(p ? "out2" : "out1", "OUTER DEADZONE", percent(cfg_.stick_outer[p]), "LIVE", help));
        r.push_back(choice(p ? "curve2" : "curve1", "STEERING RESPONSE", curve_label(cfg_.stick_curve[p]), "LIVE",
                           "1.00 is linear. Higher values give finer steering near the centre (input mapping only)."));
        r.push_back(choice(p ? "tdz2" : "tdz1", "TRIGGER DEADZONE", percent(cfg_.trigger_deadzone[p]), "LIVE",
                           "Analog gas (RT) and brake (LT) travel ignored before the pedal starts (input mapping only)."));
        r.push_back(choice(p ? "tcurve2" : "tcurve1", "TRIGGER RESPONSE", curve_label(cfg_.trigger_curve[p]), "LIVE",
                           "1.00 is linear. The game reads the trigger as proportional gas/brake pressure."));
    }
    r.push_back(header("CONTROLLER BINDINGS (READ-ONLY)"));
    {
        const bool modern = cfg_.controller_layout == "modern";
        struct B { const char *pad, *ps2, *use; };
        const B list[] = {
            {"A / CROSS", "CROSS", "accept / gas (digital)"},
            {"B / CIRCLE", "CIRCLE", "back / change view"},
            {"X / SQUARE", "SQUARE", "brake (digital)"},
            {"Y / TRIANGLE", "TRIANGLE", "reverse"},
            {"LB / L1", "L1", "L1 (native)"},
            {"RB / R1", "R1", "R1 (native)"},
            {modern ? "RT" : "RT / R2", modern ? "CROSS PRESSURE" : "R2", modern ? "analog gas" : "R2 (native)"},
            {modern ? "LT" : "LT / L2", modern ? "SQUARE PRESSURE" : "L2", modern ? "analog brake" : "L2: weapon (Battle) / look back"},
            {modern ? "RIGHT STICK CLICK" : "R3", modern ? "L2" : "R3", modern ? "weapon (Battle) / look back" : "R3"},
            {modern ? "LEFT STICK CLICK" : "L3", modern ? "R2" : "L3", modern ? "R2 (reverse)" : "L3"},
            {"BACK / SELECT", "SELECT", "recover car"},
            {"START", "START", "pause; START+SELECT opens this menu"},
        };
        for (const B &b : list) {
            Row row = info("bind", b.pad, upper(std::string(b.ps2)) + "  -  " + upper(b.use),
                           "Controllers use this fixed mapping; the game's own controller options choose what each "
                           "PS2 button does. The labels for L1/R1/L2/R2 actions are from the game's default scheme.");
            r.push_back(row);
        }
    }
    r.push_back(header("KEYBOARD BINDINGS (PS2 BUTTONS)"));
    for (int i = 0; i < KEY_COUNT; i++) {
        Row b = action("key", upper(kPadKeyLabels[i]).c_str(), rebind_ == i ? "PRESS A KEY..." : upper(cfg_.keys[i].empty() ? "-" : cfg_.keys[i]),
                       "Select, then press the new key (Esc cancels). Controllers use the fixed DualShock layout: "
                       "A/Cross, B/Circle, X/Square, Y/Triangle, bumpers L1/R1, triggers L2/R2.");
        b.id = "key" + std::to_string(i);
        r.push_back(b);
    }
}

void Menu::rows_mods(std::vector<Row> &r) {
    using St = MusicSong::Status;
    music_songs_.clear();
    int done = 0, total = 0;
    bool scanning = false;
    if (music_) {
        music_songs_ = music_->songs();
        music_->progress(done, total, scanning);
    }
    if (music_open_) {
        int on = 0, skipped = 0;
        for (const MusicSong &m : music_songs_) {
            on += m.status == St::Ready && m.enabled;
            skipped += m.status == St::Skipped || m.status == St::Protected;
        }
        r.push_back(header("CUSTOM MUSIC"));
        if (scanning) {
            Row p = info("musicscan", "SCANNING MUSIC", std::to_string(done) + " / " + std::to_string(total),
                         "Decoding your songs in the background. Finished songs join the game's music right away.");
            r.push_back(p);
        }
        for (size_t i = 0; i < music_songs_.size(); i++) {
            const MusicSong &m = music_songs_[i];
            Row row;
            row.id = "song" + std::to_string(i);
            row.label = m.name;
            switch (m.status) {
            case St::Ready:
                row.kind = RowKind::Check;
                row.checked = m.enabled;
                row.note = "LIVE";
                row.help = "Plays in the game's music rotation (menus and races) with the original songs. "
                           "Takes effect the next time the game picks a song.";
                break;
            case St::Skipped:
            case St::Protected:
                row.kind = RowKind::Action;  // selectable, so the reason shows in the details panel
                row.id = "songx" + std::to_string(i);
                row.value = m.status == St::Protected ? "PROTECTED" : "SKIPPED";
                row.warn = true;
                row.help = (m.status == St::Protected ? "Protected: " : "Skipped: ") + m.reason;
                break;
            default:
                row.kind = RowKind::Info;
                row.value = m.status == St::Decoding ? "DECODING" : "WAITING";
                row.help = "Not decoded yet.";
                break;
            }
            r.push_back(row);
        }
        if (music_songs_.empty() && !scanning)
            r.push_back(info("musicnone", "NO SONGS FOUND", "", ("Put music files (MP3, M4A, FLAC, WAV...) in " + (music_ ? music_->folder() : std::string()) +
                                                                  " (folders are fine). They are found when this menu opens.").c_str()));
        r.push_back(action("musicback", "BACK", on == 0 && skipped == 0 ? "" : std::to_string(on) + " ON", "Back to the mods."));
        return;
    }
    if (music_) {
        int ready = 0, skipped = 0;
        for (const MusicSong &m : music_songs_) {
            ready += m.status == St::Ready;
            skipped += m.status == St::Skipped || m.status == St::Protected;
        }
        std::string v = std::to_string(ready) + (ready == 1 ? " SONG" : " SONGS");
        if (skipped) v += ", " + std::to_string(skipped) + " SKIPPED";
        Row row = action("music", "CUSTOM MUSIC", v,
                         "Your own songs, played with the game's original music in menus and races. Pick which ones are on.");
        row.note = scanning ? "SCANNING " + std::to_string(done) + "/" + std::to_string(total) : "LIVE";
        r.push_back(row);
    }
    for (size_t i = 0; i < mods_.size(); i++) {
        const ModEntry &m = mods_[i];
        Row row;
        row.id = "mod" + std::to_string(i);
        row.kind = RowKind::Check;
        row.label = m.name;
        row.value = m.version.empty() || m.builtin ? "" : "(v" + m.version + ")";
        row.checked = m.enabled;
        row.note = m.builtin ? "BUILT-IN" : "LISTED ONLY";
        row.warn = !m.conflict.empty() && !m.builtin;
        row.help = m.description.empty() ? "No description." : m.description;
        r.push_back(row);
    }
    if (mods_.empty() && !music_) r.push_back(info("nomods", "NO MODS", ""));
}

void Menu::rows_online(std::vector<Row> &r) {
    game_frontend::Snapshot s;
    if (net_) s = net_->snapshot();
    using P = game_frontend::Snapshot::Phase;
    const bool connected = s.phase != P::Offline && s.phase != P::Disconnected && s.phase != P::Error;
    Row st = info("status", "STATUS", connected ? upper(s.status.empty() ? "CONNECTED" : s.status) : "NOT CONNECTED",
                  "Network play setup. The netplay transport is a later task: this screen talks to the stub backend.");
    st.warn = !connected;
    st.good = connected;
    r.push_back(st);
    if (!s.error.empty()) {
        Row e = info("error", "MESSAGE", upper(s.error));
        e.warn = true;
        r.push_back(e);
    }
    r.push_back(header("SESSION"));
    r.push_back(action("host", "HOST GAME", "[ HOST ]", "Hosts a lobby; the other player joins with the code shown here."));
    Row code = action("code", "JOIN BY CODE", text_id_ == "code" ? text_ + "_" : "[ ENTER CODE ]",
                      "Enter the host's join code, then Start/Enter to join.");
    code.kind = RowKind::Text;
    r.push_back(code);
    Row addr = action("addr", "JOIN BY ADDRESS", text_id_ == "addr" ? text_ + "_" : "[ ENTER ADDRESS ]",
                      "Enter host:port for a direct connection.");
    addr.kind = RowKind::Text;
    r.push_back(addr);
    Row ready = toggle("ready", "READY", s.ready, "", "Marks you ready in the lobby.");
    ready.enabled = connected;
    r.push_back(ready);
    Row leave = action("leave", "LEAVE / CANCEL", "[ DISCONNECT ]", "Leaves the lobby or cancels the connection.");
    leave.enabled = connected || s.can_cancel;
    r.push_back(leave);
    r.push_back(header("LOBBY"));
    if (s.players.empty()) r.push_back(info("noplayers", "PLAYERS", "NO LOBBY"));
    for (const auto &p : s.players) {
        Row pr = info("player", ("P" + std::to_string(p.slot + 1) + "  " + upper(p.name)).c_str(),
                      std::string(p.ready ? "READY" : "NOT READY") + (p.muted ? " - MUTED" : ""));
        pr.good = p.ready;
        r.push_back(pr);
    }
    Row mods = info("modset", "MOD SET / VERSION", s.content_problem.empty() ? (connected ? "MATCH" : "-") : upper(s.content_problem),
                    "Both players need the same game build and the same gameplay mods (MOD_SYSTEM_SCOPE.md 5.6).");
    mods.warn = !s.content_problem.empty();
    r.push_back(mods);
    r.push_back(info("ping", "PING", connected ? s.status : "-", "Round trip to the other player."));
    r.push_back(header("VOICE"));
    Row mic = toggle("mic", "MICROPHONE", s.microphone_enabled, "", "Sends your voice to the lobby.");
    mic.enabled = connected;
    r.push_back(mic);
    Row mute = toggle("mute", "MUTE OTHERS", s.muted, "", "Stops playing the other players' voices.");
    mute.enabled = connected;
    r.push_back(mute);
    Row deaf = toggle("deafen", "DEAFEN", s.deafened, "", "Mutes your microphone and all incoming voice.");
    deaf.enabled = connected;
    r.push_back(deaf);
}

void Menu::rows_error(std::vector<Row> &r) {
    if (browse_) {
        r.push_back(info("dir", "FOLDER", browse_dir_));
        for (const std::string &p : browse_entries_) {
            Row e;
            e.kind = RowKind::Action;
            if (p.empty()) {
                e.id = "use"; e.label = "USE THIS FOLDER"; e.value = "[ INSTALL ]";
                e.help = "The folder that holds SLUS_209.10, SYSTEM.CNF and DATA.";
            } else if (p == "..") {
                e.id = "up"; e.label = ".. (UP ONE FOLDER)";
            } else {
                fs::path fp(p);
                std::error_code ec;
                const bool dir = fs::is_directory(fp, ec);
                e.id = dir ? "dir:" + p : "file:" + p;
                e.label = fp.filename().string();
                e.value = dir ? ">" : "[ INSTALL ]";
                e.help = dir ? "Open this folder." : "Install the game from this disc image.";
            }
            r.push_back(e);
        }
        r.push_back(action("cancel", "BACK", "", ""));
        return;
    }
    if (error_disc_) r.push_back(action("browse", "CHOOSE YOUR DISC", "[ BROWSE ]",
                                        "Pick your North American v1.00 disc image (.iso, .bin/.cue) or the folder holding "
                                        "SLUS_209.10 and DATA. Your original files stay untouched."));
    r.push_back(action("retry", "TRY AGAIN", "", "Checks the content again."));
    r.push_back(action("quit", "QUIT", "", "Closes Eve of Destruction."));
}

void Menu::browse_into(const std::string &dir) {
    browse_ = true;
    browse_dir_ = dir;
    browse_entries_.clear();
    browse_entries_.push_back("");
    browse_entries_.push_back("..");
    std::error_code ec;
    std::vector<std::string> dirs, files;
    for (const auto &e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        const std::string name = e.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        std::error_code ec2;
        if (e.is_directory(ec2)) dirs.push_back(e.path().string());
        else {
            std::string ext = e.path().extension().string();
            for (char &c : ext) c = (char)tolower((unsigned char)c);
            if (ext == ".iso" || ext == ".bin" || ext == ".cue") files.push_back(e.path().string());
        }
    }
    std::sort(dirs.begin(), dirs.end());
    std::sort(files.begin(), files.end());
    for (auto &f : files) browse_entries_.push_back(f);
    for (auto &d : dirs) browse_entries_.push_back(d);
    sel_[(int)Tab::Count] = 1;
    scroll_[(int)Tab::Count] = 0;
}

void Menu::reset_tab(Tab t) {
    const Config d;
    switch (t) {
    case Tab::Graphics:
        cfg_.render_scale = d.render_scale; cfg_.aspect = d.aspect; cfg_.fullscreen = d.fullscreen; cfg_.vsync = d.vsync;
        cfg_.smooth_display = d.smooth_display; cfg_.frame_skip = d.frame_skip; cfg_.detail = d.detail; cfg_.show_fps = d.show_fps;
        break;
    case Tab::Settings:
        cfg_.audio = d.audio; cfg_.volume = d.volume; cfg_.pause_in_menu = d.pause_in_menu; cfg_.unlock_all = d.unlock_all;
        cfg_.career_cheats = d.career_cheats; cfg_.battle_bots = d.battle_bots; cfg_.sp_battle_mode = d.sp_battle_mode;
        cfg_.manual_recovery = d.manual_recovery; cfg_.car_durability = d.car_durability; cfg_.cheat_turbo = d.cheat_turbo;
        cfg_.cheat_chickens = d.cheat_chickens; cfg_.cheat_invincible = d.cheat_invincible;
        break;
    case Tab::Inputs:
        for (int i = 0; i < KEY_COUNT; i++) cfg_.keys[i] = d.keys[i];
        for (int p = 0; p < 2; p++) {
            cfg_.stick_deadzone[p] = d.stick_deadzone[p]; cfg_.stick_outer[p] = d.stick_outer[p]; cfg_.stick_curve[p] = d.stick_curve[p];
            cfg_.trigger_deadzone[p] = d.trigger_deadzone[p]; cfg_.trigger_curve[p] = d.trigger_curve[p];
        }
        cfg_.vibration = d.vibration;
#ifndef _WIN32
        cfg_.keyboard_port = d.keyboard_port; cfg_.controller_layout = d.controller_layout; cfg_.keyboard_layout = d.keyboard_layout;
#endif
        break;
    case Tab::Mods:
        for (ModEntry &m : mods_) m.enabled = true;
        store_mods();
        break;
    default: break;
    }
}

void Menu::move_selection(int dy) {
    std::vector<Row> &r = rows_for(error_active() ? Tab::Count : tab_);
    int &s = sel_[error_active() ? (int)Tab::Count : (int)tab_];
    if (r.empty()) return;
    const int n = (int)r.size();
    s = std::clamp(s, 0, n - 1);
    for (int i = 0; i < n; i++) {
        s = (s + dy + n) % n;
        if (r[s].kind != RowKind::Header && r[s].kind != RowKind::Info) return;
    }
}

void Menu::change(const Row &row, int dir, Effects &fx) {
    const std::string &id = row.id;
    auto step = [&](int &v, int lo, int hi, int by) { v = std::clamp(v + dir * by, lo, hi); };
    auto cycle = [&](const char *const *list, int n, std::string &v) { v = list[(index_of(list, n, v) + dir + n) % n]; };
    if (id == "scale") {
#ifdef _WIN32
        cfg_.render_scale = std::clamp(cfg_.render_scale + dir, 1, 6);
#else
        cfg_.render_scale = (cfg_.render_scale + dir + 7) % 7;
#endif
    } else if (id == "aspect") cycle(kAspects, 5, cfg_.aspect);
    else if (id == "fullscreen") { cfg_.fullscreen = !cfg_.fullscreen; fx.fullscreen = true; }
    else if (id == "vsync") cfg_.vsync = !cfg_.vsync;
    else if (id == "smooth") cfg_.smooth_display = !cfg_.smooth_display;
    else if (id == "frameskip") cfg_.frame_skip = !cfg_.frame_skip;
    else if (id == "detail") cycle(kDetails, 3, cfg_.detail);
    else if (id == "fps") cfg_.show_fps = !cfg_.show_fps;
    else if (id == "audio") { cfg_.audio = !cfg_.audio; fx.audio = true; }
    else if (id == "volume") { step(cfg_.volume, 0, 100, 5); fx.audio = true; }
    else if (id == "pause") cfg_.pause_in_menu = !cfg_.pause_in_menu;
    else if (id == "unlock") cfg_.unlock_all = !cfg_.unlock_all;
    else if (id == "career_cheats") cfg_.career_cheats = !cfg_.career_cheats;
    else if (id == "bots") cfg_.battle_bots = !cfg_.battle_bots;
    else if (id == "spbattle") cfg_.sp_battle_mode = !cfg_.sp_battle_mode;
    else if (id == "recovery") cfg_.manual_recovery = !cfg_.manual_recovery;
    else if (id == "durability") cfg_.car_durability = !cfg_.car_durability;
    else if (id == "turbo") cfg_.cheat_turbo = !cfg_.cheat_turbo;
    else if (id == "chickens") cfg_.cheat_chickens = !cfg_.cheat_chickens;
    else if (id == "invincible") cfg_.cheat_invincible = !cfg_.cheat_invincible;
    else if (id == "vibration") cfg_.vibration = !cfg_.vibration;
#ifndef _WIN32
    else if (id == "kbport") { cfg_.keyboard_port = (cfg_.keyboard_port + 1 + dir + 4) % 4 - 1; fx.input_layout = true; }
    else if (id == "padlayout") { cfg_.controller_layout = cfg_.controller_layout == "modern" ? "original" : "modern"; fx.input_layout = true; }
    else if (id == "kblayout") { cfg_.keyboard_layout = cfg_.keyboard_layout == "racing" ? "original" : "racing"; fx.input_layout = true; }
#endif
    else if (id == "dz1" || id == "dz2") { step(cfg_.stick_deadzone[id[2] - '1'], 0, 50, 1); fx.input_layout = true; }
    else if (id == "out1" || id == "out2") { step(cfg_.stick_outer[id[3] - '1'], 50, 100, 1); fx.input_layout = true; }
    else if (id == "curve1" || id == "curve2") { step(cfg_.stick_curve[id[5] - '1'], 50, 300, 10); fx.input_layout = true; }
    else if (id == "tdz1" || id == "tdz2") { step(cfg_.trigger_deadzone[id[3] - '1'], 0, 50, 1); fx.input_layout = true; }
    else if (id == "tcurve1" || id == "tcurve2") { step(cfg_.trigger_curve[id[6] - '1'], 50, 300, 10); fx.input_layout = true; }
    else return;
    dirty_ = true;
    fx.sound = Sound::Move;
}

void Menu::activate(const Row &row, Effects &fx) {
    const std::string &id = row.id;
    if (!row.enabled) { fx.sound = Sound::Error; return; }
    if (row.kind == RowKind::Toggle || row.kind == RowKind::Choice) { change(row, +1, fx); fx.sound = Sound::Select; return; }
    fx.sound = Sound::Select;
    if (id == "music") { music_open_ = true; sel_[(int)Tab::Mods] = 0; scroll_[(int)Tab::Mods] = 0; return; }
    if (id == "musicback") { music_open_ = false; sel_[(int)Tab::Mods] = 0; scroll_[(int)Tab::Mods] = 0; fx.sound = Sound::Back; return; }
    if (id.compare(0, 4, "song") == 0 && row.kind == RowKind::Check) {
        const size_t i = (size_t)std::stoi(id.substr(4));
        if (music_ && i < music_songs_.size()) music_->set_enabled(music_songs_[i].rel, !music_songs_[i].enabled);
        return;
    }
    if (id.compare(0, 3, "mod") == 0 && row.kind == RowKind::Check) {
        ModEntry &m = mods_[std::stoi(id.substr(3))];
        if (m.builtin) { set_status("Built-in content is always on (it is part of the integrated game)."); fx.sound = Sound::Error; return; }
        if (!m.conflict.empty() && !m.enabled) { set_status(m.conflict); }
        m.enabled = !m.enabled;
        store_mods();
        return;
    }
    if (id.compare(0, 3, "key") == 0 && id.size() > 3) { rebind_ = std::stoi(id.substr(3)); return; }
    if (id == "dev1" || id == "dev2") { fx.swap_ports = true; return; }
    game_frontend::Command c{};
    if (id == "host" && net_) { c.kind = game_frontend::Command::Kind::Host; net_->submit(c); return; }
    if (id == "code" || id == "addr") { text_id_ = id; text_.clear(); return; }
    if (id == "ready" && net_) { c.kind = game_frontend::Command::Kind::Ready; c.enabled = !row.checked; net_->submit(c); return; }
    if (id == "leave" && net_) { c.kind = game_frontend::Command::Kind::Disconnect; net_->submit(c); return; }
    if (id == "mic" && net_) { c.kind = game_frontend::Command::Kind::MicrophoneEnabled; c.enabled = !row.checked; net_->submit(c); return; }
    if (id == "mute" && net_) { c.kind = game_frontend::Command::Kind::Muted; c.enabled = !row.checked; net_->submit(c); return; }
    if (id == "deafen" && net_) { c.kind = game_frontend::Command::Kind::Deafened; c.enabled = !row.checked; net_->submit(c); return; }
    // error screen
    if (id == "quit") { fx.quit = true; return; }
    if (id == "retry") { fx.retry = true; return; }
    if (id == "browse") {
        const char *home = getenv("HOME");
#ifdef _WIN32
        if (!home) home = getenv("USERPROFILE");
#endif
        browse_into(home ? home : "/");
        return;
    }
    if (id == "cancel") { browse_ = false; sel_[(int)Tab::Count] = 0; fx.sound = Sound::Back; return; }
    if (id == "up") { browse_into(fs::path(browse_dir_).parent_path().string()); return; }
    if (id.compare(0, 4, "dir:") == 0) { browse_into(id.substr(4)); return; }
    if (id == "use") { fx.choose_disc = true; fx.disc_path = browse_dir_; return; }
    if (id.compare(0, 5, "file:") == 0) { fx.choose_disc = true; fx.disc_path = id.substr(5); return; }
}

Effects Menu::update(const Input &in) {
    Effects fx;
    const bool err = error_active();
    const Tab t = err ? Tab::Count : tab_;
    std::vector<Row> &rows = rows_for(t);
    int &sel = sel_[(int)t];
    if (!rows.empty()) {
        sel = std::clamp(sel, 0, (int)rows.size() - 1);
        if (rows[sel].kind == RowKind::Header || rows[sel].kind == RowKind::Info) {
            const int before = sel;
            move_selection(+1);
            if (rows[sel].kind == RowKind::Header || rows[sel].kind == RowKind::Info) sel = before;
        }
    }
    // keyboard rebinding waits for one key
    if (rebind_ >= 0) {
        if (in.back || in.key == 0x1B) { rebind_ = -1; fx.sound = Sound::Back; return fx; }
        if (in.key) {
            const std::string name = ::key_name(in.key);
            if (!name.empty()) { cfg_.keys[rebind_] = name; dirty_ = true; fx.keys = true; fx.sound = Sound::Select; }
            else fx.sound = Sound::Error;
            rebind_ = -1;
        }
        return fx;
    }
    // text entry (online codes/addresses): typed characters, Backspace, Enter/Start confirms, Esc/Circle cancels
    if (!text_id_.empty()) {
        if (in.ch >= ' ' && in.ch <= '~' && text_.size() < 64) text_.push_back(in.ch);
        if (in.erase && !text_.empty()) text_.pop_back();
        if (in.back) { text_id_.clear(); fx.sound = Sound::Back; return fx; }
        if (in.accept || in.close) {
            if (net_ && !text_.empty()) {
                game_frontend::Command c{};
                c.kind = text_id_ == "code" ? game_frontend::Command::Kind::JoinCode : game_frontend::Command::Kind::JoinAddress;
                c.text = text_;
                net_->submit(c);
            }
            text_id_.clear();
            fx.sound = Sound::Select;
        }
        return fx;
    }
    // mouse: hovering a row selects it (rows are hit-tested by the view and passed via select())
    if (in.close && !err) { fx.close = true; fx.sound = Sound::Back; return fx; }
    if (!err && (in.prev_tab || in.next_tab)) {
        int n = (int)Tab::Count;
        tab_ = (Tab)(((int)tab_ + (in.next_tab ? 1 : -1) + n) % n);
        details_ = false;
        status_.clear();
        fx.sound = Sound::Tab;
        return fx;
    }
    if (in.back) {
        if (music_open_) { music_open_ = false; sel_[(int)Tab::Mods] = 0; scroll_[(int)Tab::Mods] = 0; fx.sound = Sound::Back; return fx; }
        if (details_) { details_ = false; fx.sound = Sound::Back; return fx; }
        if (err && browse_) { browse_ = false; sel_[(int)Tab::Count] = 0; fx.sound = Sound::Back; return fx; }
        if (!err) { fx.close = true; fx.sound = Sound::Back; }
        return fx;
    }
    if (in.dy) { move_selection(in.dy); fx.sound = Sound::Move; details_ = false; }
    if (rows.empty()) return fx;
    rows = rows_for(t);
    sel = std::clamp(sel, 0, (int)rows.size() - 1);
    const Row row = rows[sel];
    if (in.dx && row.enabled && (row.kind == RowKind::Choice || row.kind == RowKind::Toggle)) change(row, in.dx, fx);
    if (in.accept) activate(row, fx);
    if (in.details && !err) { details_ = !details_; fx.sound = Sound::Select; }
    if (in.reset && !err) {
        reset_tab(tab_);
        dirty_ = true;
        fx.sound = Sound::Select;
        fx.audio = fx.keys = fx.input_layout = true;
        set_status(std::string(tab_name(tab_)) + " reset to defaults");
    }
    if (in.move && tab_ == Tab::Mods && !err && row.id.compare(0, 3, "mod") == 0) {
        const int i = std::stoi(row.id.substr(3)), j = i + in.move;
        if (j >= 0 && j < (int)mods_.size() && !mods_[i].builtin && !mods_[j].builtin) {
            std::swap(mods_[i], mods_[j]);
            store_mods();
            sel = j;
            fx.sound = Sound::Move;
        } else fx.sound = Sound::Error;
    }
    if (fx.sound == Sound::None && in.dx) fx.sound = Sound::None;
    return fx;
}

}  // namespace frontend
