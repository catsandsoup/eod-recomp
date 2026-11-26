// frontend.h - the in-game front end (recomp settings, mods, network-play setup).
//
// Platform-independent model: tabs, rows, controller-first navigation and the
// changes the rows make to the settings (Config). No ImGui, no OS calls: the
// overlay (overlay_core.cpp) feeds Input, applies Effects and draws the View
// (frontend_draw.cpp). Unit-tested by tests/frontend_menu_test.cpp.
//
// Tabs: GRAPHICS | MODS | SETTINGS | INPUTS | ONLINE (L1/R1). D-pad/stick moves,
// left/right changes "< value >" rows, Cross/A selects or toggles, Circle/B goes
// back (closes at the top level), Square/X opens details, Triangle/Y resets the
// tab to defaults, L2/R2 move a mod in the load order. The native game menus are
// untouched: this front end only holds recomp settings, mods and online setup.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../app/game_frontend.h"
#include "../core/config.h"

namespace frontend {

enum class Tab { Play, Mods, Graphics, Inputs, Settings, Count };
const char *tab_name(Tab t);

// Which prompts to draw: follows the last device that pressed something.
enum class Glyphs { PlayStation, Xbox, Keyboard };

// One frame of menu input (edges already debounced / auto-repeated).
struct Input {
    int dx = 0, dy = 0;           // -1/0/1 (repeat handled by Repeater)
    bool accept = false, back = false, details = false, reset = false;
    bool prev_tab = false, next_tab = false;
    int move = 0;                 // L2/R2: -1 up, +1 down (mod load order)
    bool close = false;           // Start / F1: close the menu
    int key = 0;                  // a keyboard key pressed (Windows VK code, for rebinding / text)
    char ch = 0;                  // printable character typed (text fields)
    std::string paste;            // text pasted with Cmd+V / Ctrl+V into a text field
    bool erase = false;           // backspace in a text field
    // mouse (window pixels; hit-testing uses the rectangles of the last View)
    bool mouse_moved = false, click = false;
    float mx = -1, my = -1, wheel = 0;
    float slider_fraction = -1;  // pointer position on an adjustable track
};

// Auto-repeat for held directions (350 ms, then 100 ms).
class Repeater {
public:
    // held: current direction (-1/0/1); returns the step to apply this frame.
    int step(int held, uint64_t now_ms);
private:
    int last_ = 0;
    uint64_t next_ = 0;
};

enum class Sound { None, Move, Select, Back, Tab, Error };

enum class RowKind { Header, Choice, Toggle, Action, Info, Check, Text };

struct Row {
    std::string id;
    RowKind kind = RowKind::Info;
    std::string label, value;
    std::string note;   // "LIVE", "NEXT LAUNCH", "NEXT EVENT", "BUILT-IN", ...
    std::string help;   // right-hand panel text
    bool enabled = true;
    bool warn = false;  // drawn red (conflicts, errors)
    bool good = false;  // drawn green (connected, intact)
    float slider_fraction = -1;  // 0..1 draws a track in the value area
    bool checked = false;  // Check rows
};

// Built-in and discovered mods (MOD_SYSTEM_SCOPE.md section 5; the loader is a later task).
struct ModEntry {
    std::string id, name, version, author, kind, description, conflict;
    bool enabled = true, builtin = false;
};
// Lists <root>/*/mod.json (name/version/author/description/kind read with a tolerant
// scan, no JSON library). Missing root: empty list. Invalid manifests are listed with
// a conflict reason so the menu shows them red.
std::vector<ModEntry> discover_mods(const std::string &root);
std::vector<ModEntry> builtin_mods();

// The custom music library as the MODS tab sees it (runtime/iop/custom_music.h; the host
// supplies an adapter, so this model stays free of audio and OS code).
struct MusicSong {
    std::string rel, name;
    enum class Status { Waiting, Decoding, Ready, Skipped, Protected } status = Status::Waiting;
    std::string reason;
    bool enabled = true;
};
class MusicSource {
public:
    virtual ~MusicSource() = default;
    virtual std::vector<MusicSong> songs() = 0;
    virtual void progress(int &done, int &total, bool &scanning) = 0;
    virtual void set_enabled(const std::string &rel, bool on) = 0;
    virtual std::string folder() = 0;
    virtual void rescan() = 0;  // the menu opened: look for new or removed files
};

// Things the host must do after an update (platform effects; the Config is already changed).
struct Effects {
    Sound sound = Sound::None;
    bool close = false;
    bool save = false;           // write eod.ini now (the menu also saves on close when dirty)
    bool fullscreen = false;     // apply cfg.fullscreen to the window
    bool toggle_plume_look = false;
    bool audio = false;          // volume / sound on-off changed
    bool keys = false;           // keyboard bindings changed
    bool input_layout = false;   // controller/keyboard layout, keyboard port, deadzones
    bool swap_ports = false;     // exchange P1 and P2 controllers
    bool quit = false;           // the error screen's Quit
    bool choose_disc = false;    // the error screen's disc chooser was confirmed: path in disc_path
    bool retry = false;          // the error screen's Retry
    std::string disc_path;
};

enum class PlumeLook { Unavailable, Classic, Modern };

// Live facts the host supplies each frame (device names, mods root, version...).
struct HostInfo {
    std::string version = "0.0.0";
    std::string device[4];          // "DualShock 4", "Keyboard", "None" (ports 0..3)
    std::string mods_root;          // where discover_mods looks
    bool pause_supported = true;
    PlumeLook plume_look = PlumeLook::Unavailable;
    bool legacy_modern_available = false;
};

class Menu {
public:
    explicit Menu(Config &cfg);
    ~Menu();
    Config &cfg() { return cfg_; }

    void set_host(const HostInfo &h) { host_ = h; }
    const HostInfo &host() const { return host_; }
    void set_network(std::shared_ptr<game_frontend::Network> n);
    void remember_join(const game_frontend::Command &command);
    void set_music(std::shared_ptr<MusicSource> m) { music_ = std::move(m); }
    bool music_open() const { return music_open_; }
    game_frontend::Network *network() { return net_.get(); }

    // Opens on the remembered tab; clears transient states (rebind, details, text).
    void open();
    Effects update(const Input &in);

    Tab tab() const { return tab_; }
    void set_tab(Tab t);
    int selected() const { return sel_[error_active() ? (int)Tab::Count : (int)tab_]; }
    void select(int row);
    const std::vector<Row> &rows() { return rows_for(error_active() ? Tab::Count : tab_); }
    std::vector<Row> &rows_for(Tab t);
    bool details_open() const { return details_; }
    int rebinding() const { return rebind_; }   // pad key index waiting for a keyboard key, -1 none
    bool editing_text() const { return !text_id_.empty(); }
    const std::string &text_value() const { return text_; }
    const std::string &pic_track() const { return pic_track_; }
    uint32_t pic_car() const { return pic_car_; }
    const char *online_modal_title() const;
    std::string copy_text();       // what Cmd+C copies on this screen (the hosted Garage Code), "" if nothing
    bool dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }

    // Mods tab state
    std::vector<ModEntry> &mods() { return mods_; }
    void rescan_mods();
    void store_mods();  // cfg().mods <- the discovered mods' toggles and order; marks the menu dirty

    // Error screen (missing/corrupt content, no disc): replaces the tabs until resolved.
    // fatal: the game cannot continue (a game panic); the screen offers only QUIT.
    void show_error(const std::string &title, const std::string &message, bool offer_disc, bool fatal = false);
    bool error_active() const { return !error_title_.empty(); }
    const std::string &error_title() const { return error_title_; }
    const std::string &error_message() const { return error_message_; }
    bool browsing() const { return browse_; }
    const std::string &browse_dir() const { return browse_dir_; }
    void set_status(const std::string &s) { status_ = s; }
    const std::string &status() const { return status_; }

    // Scroll offset of the selected tab's list (rows kept visible by the view).
    int scroll() const { return scroll_[error_active() ? (int)Tab::Count : (int)tab_]; }
    void set_scroll(int s) { scroll_[error_active() ? (int)Tab::Count : (int)tab_] = s; }

    // PLAY Hub state (Screen A: Career / Solo / 2-4P Split-Screen / 16P Online)
    game_frontend::SessionKind session_kind() const { return session_kind_; }
    void set_session_kind(game_frontend::SessionKind k);
    int split_players() const { return split_players_; }
    const std::array<game_frontend::SplitSeatConfig, 4> &split_seats() const { return split_seats_; }
    const game_frontend::MutatorDraft &mutator_draft() const { return mutator_draft_; }
    int career_warp_index() const { return career_warp_idx_; }
    int offline_ai_bots() const { return offline_ai_bots_; }
    int offline_laps() const { return offline_laps_; }
    const std::string &offline_track() const { return offline_track_; }
    const std::string &offline_event() const { return offline_event_; }
    const std::string &offline_custom_map() const { return offline_custom_map_; }
    uint8_t offline_preset() const { return offline_preset_; }
    int distinct_chassis_count() const;

    // Screen B (Visual Mode, Map & Car Browser) & Screen C (Customize Rules Drawer) state
    enum class SubScreen : uint8_t { None = 0, ModeBrowser, MapBrowser, CarBrowser, RulesDrawer };
    SubScreen sub_screen() const { return sub_screen_; }
    void set_sub_screen(SubScreen s);
    int browser_category() const { return browser_cat_; }
    void set_browser_category(int c);
    int browser_selected() const { return browser_sel_; }
    void set_browser_selected(int idx) { browser_sel_ = idx; }
    struct BrowserItem {
        std::string id;
        std::string title;
        std::string subtitle;
        std::string badge;
        std::string track_art; // UIBIG.RES track key for thumbnail
        uint32_t car_num = 0;  // UICAR.RES vehicle number for car portrait
        uint8_t preset_id = 0;
        bool is_custom_map = false;
        bool is_custom_mode = false;
        bool is_car = false;
        bool is_trailer = false;
        bool is_modded = false;
        bool is_save_new = false;
        bool featured = false;
    };
    std::vector<BrowserItem> browser_items() const;
    void activate_browser_item(int idx, Effects &fx);
    int rules_tab() const { return rules_tab_; } // 0=MODE RULES, 1=FEATS & WEAPONS, 2=PHYSICS & SCALE, 3=BOTS & ROLES
    void set_rules_tab(int t);

    // Screen D (In-Match Translucent Pause Rail) state
    bool quick_rail_open() const { return quick_rail_; }
    void set_quick_rail(bool on);
    bool quick_scoreboard_open() const { return quick_scoreboard_; }
    bool save_prompt_open() const { return save_prompt_; }
    bool classic_ps2_mode() const { return !cfg_.unified_frontend; }

    // The menu's own defaults per tab (Triangle / Y); exposed for tests.
    void reset_tab(Tab t);

private:
    std::string pic_track_;
    uint32_t pic_car_ = 0;
    void rebuild(Tab t);
    void rows_play(std::vector<Row> &r);
    void rows_graphics(std::vector<Row> &r);
    void rows_mods(std::vector<Row> &r);
    void rows_settings(std::vector<Row> &r);
    void rows_inputs(std::vector<Row> &r);
    void rows_online(std::vector<Row> &r);
    void rows_rules_drawer(std::vector<Row> &r);
    void rows_quick_rail(std::vector<Row> &r);
    void rows_error(std::vector<Row> &r);
    void submit_join(game_frontend::Command command);
    void cancel_join();
    void close_map_picker();
    void sync_draft_from_preset(uint8_t preset_id);
    void push_mutator_draft();
    void launch_offline_match(bool split);
    void launch_career(bool classic_hub, bool new_career);
    void change(const Row &row, int dir, Effects &fx);
    void activate(const Row &row, Effects &fx);
    void move_selection(int dy);
    void browse_into(const std::string &dir);

    Config &cfg_;
    HostInfo host_;
    std::shared_ptr<game_frontend::Network> net_;
    std::unique_ptr<game_frontend::Command> join_attempt_;
    enum class OnlineDialog { None, Connecting, Failure } online_dialog_ = OnlineDialog::None;
    bool online_advanced_ = false;
    bool joined_lobby_ = false;
    Tab tab_ = Tab::Play;
    game_frontend::SessionKind session_kind_ = game_frontend::SessionKind::Solo;
    int split_players_ = 2;
    std::array<game_frontend::SplitSeatConfig, 4> split_seats_{};
    std::string offline_track_ = "Anoka";
    std::string offline_event_ = "Battle (8 AI)";
    std::string offline_custom_map_;
    uint8_t offline_preset_ = 0;
    int offline_ai_bots_ = -1; // -1 = Fill/Auto, 0..15 explicit
    int offline_laps_ = 3;
    int career_warp_idx_ = 0;  // 0 = Home Base .. 7 = Troy's Place
    game_frontend::MutatorDraft mutator_draft_{};
    SubScreen sub_screen_ = SubScreen::None;
    int browser_cat_ = 0;
    int browser_sel_ = 0;
    int rules_tab_ = 0;
    bool quick_rail_ = false;
    bool quick_scoreboard_ = false;
    bool save_prompt_ = false;
    game_frontend::TransitionRequest pending_save_jump_{};
    int sel_[(int)Tab::Count + 1] = {};
    int scroll_[(int)Tab::Count + 1] = {};
    std::vector<Row> rows_[(int)Tab::Count + 1];  // [Count] = error screen
    std::vector<ModEntry> mods_;
    std::shared_ptr<MusicSource> music_;
    std::vector<MusicSong> music_songs_;  // as of the last rows_mods()
    bool music_open_ = false;
    int map_return_row_ = -1;
    std::string remove_player_;
    bool details_ = false;
    int rebind_ = -1;
    std::string text_id_, text_, text_before_;
    bool dirty_ = false;
    std::string status_;
    std::string error_title_, error_message_;
    bool error_disc_ = false, error_fatal_ = false, browse_ = false;
    std::string browse_dir_;
    std::vector<std::string> browse_entries_;  // full paths; "" = "use this folder"
};

// Steering response (input mapping only; the game's physics is untouched): applies an
// inner dead zone (percent of travel), an outer saturation point and a response curve
// (100 = linear, >100 = finer control near the centre) to a 0..255 DualShock axis.
uint8_t shape_axis(uint8_t v, int inner_pct, int outer_pct, int curve_pct);
// Trigger pressure (0..255, 0 = released) through a dead zone and response curve; full travel
// (and the 255 of a face button or key) stays 255.
uint8_t shape_trigger(uint8_t v, int deadzone_pct, int curve_pct);

}  // namespace frontend
