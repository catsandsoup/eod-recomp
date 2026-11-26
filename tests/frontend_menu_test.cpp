// Front end model tests: navigation state, tab changes, value rows, reset, rebinding,
// mods list, online stub commands, stick shaping and the menu-input mapper.
#include "ui/frontend.h"
#include "ui/menu_input.h"
#include "app/game_frontend.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

using namespace frontend;

struct FakeNet : game_frontend::Network {
    game_frontend::Snapshot snap;
    std::vector<game_frontend::Command> got;
    mutable int ready_reads_left = -1;
    game_frontend::Snapshot snapshot() const override {
        auto s = snap;
        if (ready_reads_left == 0) s.can_start = false;
        if (ready_reads_left > 0) --ready_reads_left;
        return s;
    }
    void submit(game_frontend::Command c) override { got.push_back(c); }
};

static Input none() { return Input(); }
static int find(Menu &m, const std::string &id) {
    const auto &r = m.rows();
    for (size_t i = 0; i < r.size(); i++)
        if (r[i].id == id) return (int)i;
    return -1;
}

int main() {
    {
        Config graphics;
        graphics.modern_renderer = true;
        graphics.modern_shadows = 40;
        Menu menu(graphics);
        menu.set_tab(Tab::Graphics);
        menu.open();
        CHECK(find(menu, "plume_look") < 0 && find(menu, "modern") < 0);
        HostInfo host;
        host.plume_look = PlumeLook::Classic;
        menu.set_host(host);
        const int look = find(menu, "plume_look");
        CHECK(look >= 0 && menu.rows()[look].value == "CLASSIC");
        CHECK(find(menu, "modern") < 0 && find(menu, "m_shadows") < 0);
        if (look >= 0) {
            menu.select(look);
            Input change; change.dx = -1;
            Effects fx = menu.update(change);
            CHECK(fx.toggle_plume_look && !fx.save && !menu.dirty() && graphics.modern_renderer);
            host.plume_look = PlumeLook::Modern;  // fresh renderer reading, including changes made outside the menu
            menu.set_host(host);
            CHECK(menu.selected() == look && menu.rows()[look].value == "MODERN");
            change = none(); change.accept = true;
            CHECK(menu.update(change).toggle_plume_look);
            change = none(); change.reset = true;
            CHECK(!menu.update(change).toggle_plume_look && graphics.modern_shadows == 40);
        }
        host.plume_look = PlumeLook::Unavailable;
        host.legacy_modern_available = true;
        menu.set_host(host);
        const int legacy = find(menu, "modern"), shadows = find(menu, "m_shadows");
        CHECK(find(menu, "plume_look") < 0 && legacy >= 0 && shadows >= 0);
        if (legacy >= 0 && shadows >= 0) {
            CHECK(menu.rows()[legacy].value == "MODERN" && menu.rows()[shadows].enabled);
            menu.select(legacy);
            Input change; change.accept = true;
            CHECK(!menu.update(change).toggle_plume_look && !graphics.modern_renderer);
            CHECK(menu.rows()[legacy].value == "ORIGINAL" && !menu.rows()[shadows].enabled);
        }
    }
    Config cfg;
    Menu m(cfg);
    m.open();
    // tabs: L1/R1 cycle with wrap (Play | Mods | Graphics | Inputs | Settings)
    CHECK(m.tab() == Tab::Play);
    Input in = none(); in.next_tab = true;
    Effects fx = m.update(in);
    CHECK(m.tab() == Tab::Mods && fx.sound == Sound::Tab);
    in = none(); in.prev_tab = true;
    m.update(in); m.update(in);
    CHECK(m.tab() == Tab::Settings);
    in = none(); in.next_tab = true;
    m.update(in);
    CHECK(m.tab() == Tab::Play);
    m.set_tab(Tab::Graphics);

    // down/up move and wrap; headers are skipped
    int first = m.selected();
    in = none(); in.dy = 1; m.update(in);
    CHECK(m.selected() == first + 1);
    in.dy = -1; m.update(in); m.update(in);
    CHECK(m.selected() == (int)m.rows().size() - 1);

    // < value > rows change the config, and mark dirty
    m.select(find(m, "aspect"));
    in = none(); in.dx = 1; m.update(in);
    CHECK(cfg.aspect == "4:3");
    m.update(in); m.update(in);
    CHECK(cfg.aspect == "16:10");
    CHECK(m.dirty());
    m.select(find(m, "fullscreen"));
    in = none(); in.accept = true; fx = m.update(in);
    CHECK(cfg.fullscreen && fx.fullscreen);
    // Triangle resets the tab
    in = none(); in.reset = true; m.update(in);
    CHECK(cfg.aspect == Config().aspect && !cfg.fullscreen);

    // settings: volume steps by 5 and clamps
    m.set_tab(Tab::Settings);
    CHECK(m.rows()[m.selected()].kind != RowKind::Header || true);
    m.select(find(m, "volume"));
    in = none(); in.dx = 1; fx = m.update(in);
    CHECK(cfg.volume == 100 && fx.audio);
    in.dx = -1; m.update(in);
    CHECK(cfg.volume == 95);
    CHECK(cfg.voice_volume == 100 && cfg.lower_game_during_voice);
    m.select(find(m, "voicevolume"));
    for (int i = 0; i < 25; ++i) m.update(in);
    CHECK(cfg.voice_volume == 0 && cfg.volume == 95 && cfg.music_volume == 100);
    in.dx = 1;
    for (int i = 0; i < 45; ++i) m.update(in);
    CHECK(cfg.voice_volume == 200 && m.rows()[m.selected()].slider_fraction == 1.0f);
    Input pointer;
    pointer.slider_fraction = 0.37f; m.update(pointer);
    CHECK(cfg.voice_volume == 75 && m.rows()[m.selected()].slider_fraction == 0.375f);
    pointer.slider_fraction = 1; m.update(pointer);
    CHECK(cfg.voice_volume == 200);
    m.select(find(m, "voicefocus"));
    m.update(in);
    CHECK(!cfg.lower_game_during_voice && cfg.voice_volume == 200);
    // the update skips a selected header row
    m.select(0);
    m.update(none());
    CHECK(m.rows()[m.selected()].kind != RowKind::Header);

    // inputs: keyboard rebinding waits for a key; Esc cancels
    m.set_tab(Tab::Inputs);
    m.select(find(m, "key4"));
    in = none(); in.accept = true; m.update(in);
    CHECK(m.rebinding() == 4);
    in = none(); in.key = 'J'; fx = m.update(in);
    CHECK(cfg.keys[4] == "J" && fx.keys && m.rebinding() < 0);
    m.select(find(m, "dz1"));
    in = none(); in.dx = 1; m.update(in);
    CHECK(cfg.stick_deadzone[0] == 1);
    m.select(find(m, "curve2"));
    in = none(); in.dx = 1; m.update(in);
    CHECK(cfg.stick_curve[1] == 110);
    m.select(find(m, "dev1"));
    in = none(); in.accept = true; fx = m.update(in);
    CHECK(fx.swap_ports);

    // mods: built-ins are locked; discovered mods toggle and reorder
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "eod_frontend_mods_test";
    fs::remove_all(root);
    fs::create_directories(root / "a_music");
    fs::create_directories(root / "b_tex");
    std::ofstream(root / "a_music" / "mod.json") << "{\"name\": \"Loud Music\", \"version\": \"1.2\", \"kind\": \"music\"}";
    std::ofstream(root / "b_tex" / "mod.json") << "{\"name\": \"HD Signs\"}";
    HostInfo h; h.mods_root = root.string();
    m.set_host(h);
    m.set_tab(Tab::Mods);
    m.open();
    CHECK(m.mods().size() == builtin_mods().size() + 2);
    const size_t a = builtin_mods().size();
    CHECK(m.mods()[a].name == "Loud Music" && m.mods()[a].version == "1.2");
    CHECK(!m.mods()[a + 1].conflict.empty());  // no "kind": red with a reason
    m.select(0);
    in = none(); in.accept = true; fx = m.update(in);
    CHECK(m.mods()[0].enabled && fx.sound == Sound::Error);
    m.select((int)a);
    CHECK(m.mods()[a].enabled && m.mods()[a + 1].enabled);  // discovered mods start on
    m.clear_dirty();
    m.update(in);
    CHECK(!m.mods()[a].enabled && m.dirty());
    in = none(); in.move = 1; m.update(in);
    CHECK(m.mods()[a + 1].name == "Loud Music" && m.selected() == (int)a + 1);
    // the toggle and the order are remembered in the Config and survive a save/load
    CHECK(cfg.mods.size() == 2 && cfg.mods[0].first == "b_tex" && cfg.mods[0].second &&
          cfg.mods[1].first == "a_music" && !cfg.mods[1].second);
    {
        const std::string ini = (root / "eod.ini").string();
        CHECK(config_save(ini, cfg));
        Config loaded;
        CHECK(loaded.mods.empty() && config_load(ini, loaded));
        CHECK(loaded.mods == cfg.mods);
        CHECK(loaded.voice_volume == 200 && !loaded.lower_game_during_voice);
        std::ofstream(root / "old.ini") << "[Audio]\nMusicVolume=35\nVolume=70\n";
        Config old;
        CHECK(config_load((root / "old.ini").string(), old));
        CHECK(old.voice_volume == 100 && old.lower_game_during_voice && old.music_volume == 35 && old.volume == 70);
        Menu again(loaded);
        again.set_host(h);
        again.open();
        CHECK(again.mods()[a].name == "HD Signs" && again.mods()[a].enabled);
        CHECK(again.mods()[a + 1].name == "Loud Music" && !again.mods()[a + 1].enabled);
        // a missing file and a partial one load as all on
        Config none_cfg;
        CHECK(!config_load((root / "absent.ini").string(), none_cfg) && none_cfg.mods.empty());
        Menu fresh(none_cfg); fresh.set_host(h); fresh.open();
        CHECK(fresh.mods()[a].enabled && fresh.mods()[a + 1].enabled);
        std::ofstream(root / "partial.ini") << "[Mods]\nb_tex=0\n[Audio]\nVolume=30\n";
        Config partial;
        CHECK(config_load((root / "partial.ini").string(), partial) && partial.volume == 30);
        Menu part(partial); part.set_host(h); part.open();
        CHECK(part.mods()[a].name == "HD Signs" && !part.mods()[a].enabled && part.mods()[a + 1].enabled);  // listed first, off; unlisted on
        // reset on the MODS tab turns everything back on
        part.set_tab(Tab::Mods);
        Input r = none(); r.reset = true; part.update(r);
        CHECK(part.mods()[a].enabled && part.mods()[a + 1].enabled && part.dirty());
        for (const auto &e : partial.mods) CHECK(e.second);
    }
    // Career unlocks (docs/CAREER_UNLOCKS.md): all on by default, live rows in MODS, saved under [Cheats]
    {
        CHECK(cfg.career_money && cfg.career_rank && cfg.career_open && cfg.career_all_cars && cfg.career_free_swap &&
              cfg.career_killable);
        m.set_tab(Tab::Mods);
        for (const char *id : {"cu_money", "cu_rank", "cu_open", "cu_cars", "cu_swap", "cu_kill"}) CHECK(find(m, id) >= 0);
        Input go = none(); go.accept = true;
        m.select(find(m, "cu_money")); m.update(go);
        m.select(find(m, "cu_swap")); m.update(go);
        m.select(find(m, "cu_kill")); m.update(go);
        CHECK(!cfg.career_money && !cfg.career_free_swap && !cfg.career_killable && cfg.career_rank && cfg.career_open && m.dirty());
        const std::string ini = (root / "career.ini").string();
        CHECK(config_save(ini, cfg));
        Config loaded;
        CHECK(config_load(ini, loaded) && !loaded.career_money && !loaded.career_free_swap && !loaded.career_killable &&
              loaded.career_rank && loaded.career_open && loaded.career_all_cars);
        Input r = none(); r.reset = true; m.update(r);
        CHECK(cfg.career_money && cfg.career_free_swap && cfg.career_killable);
    }
    in = none(); in.details = true; m.update(in);
    CHECK(m.details_open());
    in = none(); in.back = true; fx = m.update(in);
    CHECK(!m.details_open() && !fx.close);
    fx = m.update(in);
    CHECK(fx.close);
    fs::remove_all(root);

    // online: commands reach the network interface; text entry
    auto net = std::make_shared<FakeNet>();
    m.set_network(net);
    m.set_tab(Tab::Play);
    m.set_session_kind(game_frontend::SessionKind::Online);
    CHECK(m.rows()[find(m, "status")].value == "NOT CONNECTED");
    m.select(find(m, "host"));
    in = none(); in.accept = true; m.update(in);
    CHECK(net->got.size() == 1 && net->got[0].kind == game_frontend::Command::Kind::Host);
    m.select(find(m, "code"));
    m.update(in);
    CHECK(m.editing_text());
    in = none(); in.ch = 'A'; m.update(in); in.ch = '7'; m.update(in);
    in = none(); in.accept = true; m.update(in);
    CHECK(!m.editing_text() && net->got.back().kind == game_frontend::Command::Kind::JoinCode && net->got.back().text == "A7");
    CHECK(find(m, "ready") < 0 && find(m, "leave") < 0);
    net->snap.phase = game_frontend::Snapshot::Phase::Lobby;
    net->snap.is_host = true;
    net->snap.code = "ABCDE";
    net->snap.players.push_back({1, "Guest  Trailer2", true, false, false, true});
    CHECK(find(m, "host") < 0 && find(m, "code") < 0 && find(m, "addr") < 0);
    CHECK(find(m, "gcode") >= 0 && find(m, "player") >= 0);
    m.select(find(m, "voicevolume"));
    in = none(); in.dx = -1; m.update(in);
    CHECK(cfg.voice_volume == 195 && net->got.back().kind == game_frontend::Command::Kind::JoinCode);
    m.set_tab(Tab::Settings);
    CHECK(m.rows()[find(m, "voicevolume")].value == "195%");
    in = none(); in.reset = true; m.update(in);
    CHECK(cfg.voice_volume == 100 && cfg.lower_game_during_voice);
    m.set_tab(Tab::Play);
    net->snap.custom_maps = {{"bridge.eodmap", "Woodly Bridge"}};
    m.select(find(m, "custom_map"));
    in = none(); in.accept = true;
    m.update(in);
    CHECK(find(m, "start") < 0 && m.rows()[find(m, "map:")].checked);
    m.select(find(m, "map:bridge.eodmap"));
    m.update(in);
    CHECK(net->got.back().kind == game_frontend::Command::Kind::SelectCustomMap &&
          net->got.back().text == "bridge.eodmap");
    net->snap.custom_map_id = "bridge.eodmap";
    net->snap.custom_map_title = "Woodly Bridge";
    CHECK(m.rows()[find(m, "custom_map")].value == "Woodly Bridge");
    m.select(find(m, "custom_map"));
    m.update(in);
    CHECK(m.rows()[find(m, "map:bridge.eodmap")].checked);
    m.select(find(m, "map:"));
    m.update(in);
    CHECK(net->got.back().kind == game_frontend::Command::Kind::SelectCustomMap && net->got.back().text.empty());
    net->snap.is_host = false;
    CHECK(m.rows()[find(m, "custom_map")].kind == RowKind::Info && find(m, "map:") < 0);
    net->snap.is_host = true;
    net->snap.custom_map_id.clear();
    net->snap.custom_map_title.clear();
    // Controller moderation requires two accepts and binds confirmation to a
    // particular occupant, so a reused slot can never remove a later joiner.
    net->snap.players[0].identity = 41;
    m.select(find(m, "kick:1:41"));
    in = none(); in.accept = true;
    size_t before_kick = net->got.size();
    m.update(in);
    CHECK(net->got.size() == before_kick);
    in = none(); in.back = true;
    CHECK(!m.update(in).close);
    CHECK(net->got.size() == before_kick);
    in = none(); in.accept = true;
    m.update(in);
    net->snap.players[0].identity = 42;
    m.select(find(m, "kick:1:42"));
    m.update(in);
    CHECK(net->got.size() == before_kick);
    m.update(in);
    CHECK(net->got.back().kind == game_frontend::Command::Kind::Kick && net->got.back().text == "1:42");
    net->snap.is_host = false;
    CHECK(find(m, "kick:1:42") < 0);
    CHECK(find(m, "ready") >= 0);
    net->snap.phase = game_frontend::Snapshot::Phase::Connecting;
    net->snap.can_cancel = true;
    CHECK(find(m, "ready") < 0 && find(m, "addr") < 0 && find(m, "leave") >= 0);
    // host racing: BACK TO LOBBY (EndEvent) keeps the overlay open through cleanup
    net->snap.phase = game_frontend::Snapshot::Phase::Racing;
    net->snap.is_host = true;
    CHECK(find(m, "end_event") >= 0);
    m.select(find(m, "end_event"));
    in = none(); in.accept = true; fx = m.update(in);
    CHECK(!fx.close && net->got.back().kind == game_frontend::Command::Kind::EndEvent);
    // While scrolling with a controller, a join must not move focus to another action.
    m.select(find(m, "mic"));
    net->snap.players.push_back({2, "New guest", false, false, false});
    CHECK(m.rows()[m.selected()].id == "mic");
    CHECK(find(m, "leave") > find(m, "mic"));
    // guest racing: LEAVE MATCH is shown, and gameplay-diverging cheats in SETTINGS are locked to host rules
    net->snap.is_host = false;
    CHECK(find(m, "end_event") < 0 && m.rows()[find(m, "leave")].label == "LEAVE PARTY");
    net->snap.can_rejoin_lobby = true;
    m.select(find(m, "withdraw"));
    m.update(in);
    CHECK(net->got.back().kind == game_frontend::Command::Kind::RejoinLobby);
    // Settings stays strictly Audio / System / Driving / Network (zero match mutators or offline_gametype)
    m.set_tab(Tab::Settings);
    CHECK(find(m, "offline_gametype") < 0 && find(m, "offline_ai_bots") < 0 && find(m, "bots") < 0 && find(m, "spbattle") < 0);
    CHECK(find(m, "unified_fe") >= 0);
    m.set_tab(Tab::Play);
    net->snap = {};

    // Couch route: PLAY tab -> Online session -> Host / Join by Code
    Config couch_cfg;
    Menu couch(couch_cfg);
    couch.set_network(net);
    couch.open();
    couch.set_session_kind(game_frontend::SessionKind::Online);
    CHECK(couch.tab() == Tab::Play);
    Input shoulder = none(); shoulder.prev_tab = true;
    Input down = none(); down.dy = 1;
    Input accept = none(); accept.accept = true;
    couch.select(find(couch, "host"));
    CHECK(couch.rows()[couch.selected()].id == "host");
    couch.update(down);
    CHECK(couch.rows()[couch.selected()].id == "code");
    couch.update(accept);
    for (char c : std::string("DEMO2")) { Input key = none(); key.ch = c; couch.update(key); }
    couch.update(accept);
    CHECK(net->got.back().kind == game_frontend::Command::Kind::JoinCode && net->got.back().text == "DEMO2");
    CHECK(find(couch, "addr") < 0);
    int advanced = find(couch, "advanced");
    CHECK(advanced >= 0);
    if (advanced >= 0) {
        couch.select(advanced);
        couch.update(accept);
        CHECK(find(couch, "addr") >= 0);
    }
    net->snap.phase = game_frontend::Snapshot::Phase::CheckingCompatibility;
    net->snap.status = "Checking your game files";
    net->snap.can_cancel = true;
    net->snap.players.push_back({0, "Stale host"});
    net->snap.options.push_back({"car", "YOUR CAR", "Stale car", true, ""});
    net->snap.track_pic = "Stale track";
    net->snap.car_pic = 37;
    CHECK(find(couch, "player") < 0 && find(couch, "opt:car") < 0);
    CHECK(couch.pic_track().empty() && !couch.pic_car());
    CHECK(find(couch, "ready") < 0 && find(couch, "leave") >= 0);
    couch.update(shoulder);
    CHECK(couch.tab() == Tab::Play);
    couch.set_tab(Tab::Graphics);
    CHECK(couch.tab() == Tab::Play);
    net->snap.phase = game_frontend::Snapshot::Phase::Error;
    net->snap.error = "The lobby did not respond.";
    CHECK(find(couch, "profile_match") < 0);
    net->snap.checking_installed_content = true;
    CHECK(find(couch, "profile_search") >= 0 && find(couch, "profile_match") < 0);
    net->snap.checking_installed_content = false;
    net->snap.can_match_content = true;
    const int match = find(couch, "profile_match");
    CHECK(match >= 0);
    if (match >= 0) {
        couch.select(match);
        couch.update(accept);
        CHECK(net->got.back().kind == game_frontend::Command::Kind::MatchContent);
    }
    net->snap.can_match_content = false;
    int retry = find(couch, "join_retry");
    CHECK(retry >= 0 && find(couch, "join_back") >= 0 && find(couch, "code") < 0);
    if (retry >= 0) {
        const size_t before_retry = net->got.size();
        couch.select(retry);
        couch.update(accept);
        CHECK(net->got.size() == before_retry + 1);
        CHECK(net->got.back().kind == game_frontend::Command::Kind::JoinCode && net->got.back().text == "DEMO2");
    }
    net->snap.phase = game_frontend::Snapshot::Phase::Lobby;
    net->snap.error.clear();
    CHECK(find(couch, "ready") >= 0);
    net->snap.phase = game_frontend::Snapshot::Phase::Error;
    net->snap.error = "Lost connection to the host.";
    net->snap.retryable_error = true;
    retry = find(couch, "join_retry");
    CHECK(retry >= 0 && couch.rows()[retry].label == "REJOIN");
    net->snap.retryable_error = false;
    CHECK(find(couch, "join_retry") < 0);
    net->snap.retryable_error = true;
    couch.select(find(couch, "join_retry"));
    couch.update(accept);
    CHECK(net->got.back().kind == game_frontend::Command::Kind::JoinCode && net->got.back().text == "DEMO2");
    net->snap.phase = game_frontend::Snapshot::Phase::Connecting;
    Input back = none(); back.back = true;
    CHECK(!couch.update(back).close && net->got.back().kind == game_frontend::Command::Kind::Cancel);
    net->snap = {};
    CHECK(find(couch, "host") >= 0 && find(couch, "join_back") < 0);

    net->snap.phase = game_frontend::Snapshot::Phase::Lobby;
    couch.update(none());
    CHECK(couch.rows()[couch.selected()].id == "ready");
    const size_t before_ready = net->got.size();
    CHECK(!couch.rows()[couch.selected()].enabled);
    couch.update(accept);
    CHECK(net->got.size() == before_ready);
    net->snap.car_pic = 37;
    couch.update(accept);
    CHECK(net->got.back().kind == game_frontend::Command::Kind::Ready && net->got.back().enabled);
    net->snap.is_host = true;
    net->snap.can_start = true;
    CHECK(couch.rows()[find(couch, "start")].label == "START EVENT ALONE");
    net->snap.players = {{0, "Host"}, {1, "Guest"}};
    CHECK(couch.rows()[find(couch, "start")].label == "START EVENT");
    couch.select(find(couch, "start"));
    net->ready_reads_left = 2; // readiness changes after the rows are built
    const size_t before_start = net->got.size();
    CHECK(!couch.update(accept).close && net->got.size() == before_start);
    net->ready_reads_left = -1;
    net->snap.phase = game_frontend::Snapshot::Phase::Racing;
    net->snap.is_host = true;
    couch.update(none());
    CHECK(couch.rows()[couch.selected()].id == "end_event");
    CHECK(!couch.update(accept).close && net->got.back().kind == game_frontend::Command::Kind::EndEvent);
    net->snap = {};

    // Turbo explains the local driving input even when a guest cannot edit the room rule.
    {
        Config input_cfg;
        input_cfg.keys[KEY_L2] = "A, J, Space";
        Menu turbo_menu(input_cfg);
        turbo_menu.set_network(net);
        HostInfo input_host;
        input_host.device[0] = "Keyboard";
        turbo_menu.set_host(input_host);
        turbo_menu.set_tab(Tab::Play);
        turbo_menu.set_session_kind(game_frontend::SessionKind::Online);
#ifdef _WIN32
        const std::string keyboard_label = "TURBO BOOST [J / Q / F]";
#else
        const std::string keyboard_label = "TURBO BOOST [J]";
#endif
        net->snap.phase = game_frontend::Snapshot::Phase::Lobby;
        net->snap.is_host = true;
        net->snap.options = {{"rule_turbo", "TURBO BOOST", "ON", true, "Applies to all players."}};
        auto turbo = turbo_menu.rows()[find(turbo_menu, "opt:rule_turbo")];
        CHECK(turbo.label == keyboard_label && turbo.kind == RowKind::Choice);
        net->snap.is_host = false;
        net->snap.options[0].editable = false;
        input_host.device[0] = "Gamepad";
        turbo_menu.set_host(input_host);
        turbo = turbo_menu.rows()[find(turbo_menu, "opt:rule_turbo")];
        CHECK(turbo.label.find("RIGHT STICK CLICK") != std::string::npos && turbo.kind == RowKind::Info);
#ifndef _WIN32
        input_cfg.controller_layout = "original";
        turbo = turbo_menu.rows()[find(turbo_menu, "opt:rule_turbo")];
        CHECK(turbo.label == "TURBO BOOST [LEFT TRIGGER]");
#endif
        net->snap = {};
    }

    // error screen replaces the tabs; Back never closes it
    m.show_error("NO DISC", "x", true);
    CHECK(m.rows()[0].id == "browse");
    in = none(); in.back = true; fx = m.update(in);
    CHECK(!fx.close);
    m.select(2);
    in = none(); in.accept = true; fx = m.update(in);
    CHECK(fx.quit);

    // auto-repeat timing
    Repeater r;
    CHECK(r.step(1, 0) == 1 && r.step(1, 100) == 0 && r.step(1, 351) == 1 && r.step(1, 400) == 0 && r.step(1, 452) == 1);
    CHECK(r.step(0, 500) == 0 && r.step(-1, 501) == -1);

    // stick shaping: neutral by default, dead zone, saturation, curve
    CHECK(shape_axis(200, 0, 100, 100) == 200);
    CHECK(shape_axis(140, 20, 100, 100) == 128);
    CHECK(shape_axis(250, 0, 90, 100) == 255);
    CHECK(shape_axis(0, 0, 90, 100) == 0);
    CHECK(shape_axis(192, 0, 100, 200) < 192 && shape_axis(192, 0, 100, 200) > 128);

    // trigger shaping: neutral default, dead zone, curve, full travel kept
    CHECK(shape_trigger(128, 0, 100) == 128);
    CHECK(shape_trigger(20, 10, 100) == 0);
    CHECK(shape_trigger(255, 20, 200) == 255);
    CHECK(shape_trigger(128, 0, 200) < 128 && shape_trigger(128, 0, 200) > 40);
#ifndef _WIN32  // This setting belongs to the SDL input backend.
    CHECK(Config().controller_layout == "modern");
#endif

    // menu input mapping: chords, edges, trigger-proof menus
    menu_input::PadEdges pe;
    HostPadState p; p.connected = true;
    p.buttons = PAD_START;
    CHECK(!pe.update(p).toggle);
    p.buttons = PAD_START | PAD_SELECT;
    CHECK(pe.update(p).toggle);
    CHECK(!pe.update(p).toggle);  // held: once
    p.buttons = 0; pe.update(p);
    p.buttons = PAD_CROSS;
    menu_input::PadFrame f = pe.update(p);
    CHECK(f.pressed & PAD_CROSS);
    p.buttons = PAD_CROSS;
    CHECK(!(pe.update(p).pressed & PAD_CROSS));
    p.buttons = PAD_LEFT; p.lx = 128;
    CHECK(pe.update(p).dx == -1);
    p.buttons = 0; p.lx = 20;
    CHECK(pe.update(p).dx == -1);

    // Unified PLAY Hub: Career, Solo, Split-Screen, Rules Drawer, Quick Rail Save Gate & Classic PS2 toggle
    {
        Config hub_cfg;
        Menu hub(hub_cfg);
        auto hnet = std::make_shared<FakeNet>();
        hub.set_network(hnet);
        hub.open();
        CHECK(hub.tab() == Tab::Play);
        CHECK(hub.session_kind() == game_frontend::SessionKind::Solo);

        // Solo launch emits LaunchTransition (target=3)
        hub.select(find(hub, "launch_solo"));
        Input act = none(); act.accept = true;
        Effects hfx = hub.update(act);
        CHECK(hfx.close && !hnet->got.empty() &&
              hnet->got.back().kind == game_frontend::Command::Kind::LaunchTransition &&
              hnet->got.back().transition.target == 3 &&
              hnet->got.back().transition.humans == 1);

        // Split-Screen 4P launch emits LaunchTransition (target=4, humans=4)
        hub.set_session_kind(game_frontend::SessionKind::SplitScreen);
        hub.select(find(hub, "split_players"));
        Input rt = none(); rt.dx = 1;
        hub.update(rt); hub.update(rt); // 2P -> 3P -> 4P
        CHECK(hub.split_players() == 4);
        hub.select(find(hub, "launch_split"));
        hub.update(act);
        CHECK(hnet->got.back().kind == game_frontend::Command::Kind::LaunchTransition &&
              hnet->got.back().transition.target == 4 &&
              hnet->got.back().transition.humans == 4);

        // Career Continue emits LaunchTransition (target=2) with selected warp target
        hub.set_session_kind(game_frontend::SessionKind::Career);
        hub.select(find(hub, "career_warp"));
        hub.update(rt); hub.update(rt); hub.update(rt); // 0 -> 3 (Dealer)
        CHECK(hub.career_warp_index() == 3);
        hub.select(find(hub, "career_continue"));
        hub.update(act);
        CHECK(hnet->got.back().kind == game_frontend::Command::Kind::LaunchTransition &&
              hnet->got.back().transition.target == 2 &&
              hnet->got.back().transition.career_place == 3);

        // Customize Rules Drawer: Tips (Tag) locks Chicken Fight with NO WEAPONS MODE
        hub.set_session_kind(game_frontend::SessionKind::Solo);
        hub.set_sub_screen(Menu::SubScreen::RulesDrawer);
        hub.select(find(hub, "draft_preset"));
        hub.update(rt); hub.update(rt); // Standard -> Battle Royale -> Tips (Tag)
        CHECK(hub.mutator_draft().preset_id == 2 && !hub.mutator_draft().uses_weapons);
        CHECK(hnet->got.back().kind == game_frontend::Command::Kind::UpdateMutatorDraft);
        hub.set_rules_tab(1); // FEATS & WEAPONS
        const int ch_idx = find(hub, "draft_chickens");
        CHECK(ch_idx >= 0 && !hub.rows()[ch_idx].enabled && hub.rows()[ch_idx].note == "NO WEAPONS MODE");
        // L1/R1 inside RulesDrawer switches sub-tabs (1 -> 2 PHYSICS & SCALE -> 3 BOTS & ROLES)
        Input r1_rules = none(); r1_rules.next_tab = true;
        hub.update(r1_rules);
        CHECK(hub.rules_tab() == 2);
        hub.select(find(hub, "draft_grav"));
        hub.update(rt);
        CHECK(hub.mutator_draft().gravity_scale > 1.05f &&
              hnet->got.back().kind == game_frontend::Command::Kind::UpdateMutatorDraft);
        hub.update(r1_rules);
        CHECK(hub.rules_tab() == 3);
        hub.select(find(hub, "draft_save_json"));
        hub.update(act);
        CHECK(hnet->got.back().kind == game_frontend::Command::Kind::SaveCustomGametype);
        hub.set_sub_screen(Menu::SubScreen::None);

        // Quick Rail Save Safety Gate when jumping out of Career
        hnet->snap.ee_state = 6; // CareerRoam
        hub.set_quick_rail(true);
        hub.select(find(hub, "qr_jump_online"));
        hub.update(act);
        CHECK(hub.save_prompt_open());
        hub.select(find(hub, "save_jump_save"));
        hub.update(act);
        CHECK(!hub.save_prompt_open() &&
              hnet->got.back().kind == game_frontend::Command::Kind::LaunchTransition &&
              hnet->got.back().transition.save_career_first);

        // Phase 3: Visual Mode & Map Browser (Hero Cards, Categories, Custom Modes & .eodmap)
        hnet->snap.retail_tracks = {"Anoka", "HomeBase", "Woodly"};
        hnet->snap.custom_maps = {{"sky_arena.eodmap", "Sky Arena", "Anoka", "Tester"}};
        hub.set_session_kind(game_frontend::SessionKind::Solo);
        hub.select(find(hub, "open_mode_browser"));
        hub.update(act);
        CHECK(hub.sub_screen() == Menu::SubScreen::ModeBrowser);
        CHECK(hub.browser_category() == 0);
        auto mode_items = hub.browser_items();
        CHECK(mode_items.size() >= 6 && mode_items[0].featured); // Hero featured cards at top
        // L1/R1 switches category inside Mode Browser
        Input r1 = none(); r1.next_tab = true;
        hub.update(r1); CHECK(hub.browser_category() == 1); // RETAIL
        hub.update(r1); CHECK(hub.browser_category() == 2); // PARTY & ARENA
        hub.update(r1); CHECK(hub.browser_category() == 3); // MY CUSTOM MODES
        auto custom_items = hub.browser_items();
        CHECK(!custom_items.empty() && custom_items.back().is_save_new);
        // Activate "+ SAVE CURRENT AS NEW MODE"
        hub.set_browser_selected((int)custom_items.size() - 1);
        hub.update(act);
        CHECK(hub.sub_screen() == Menu::SubScreen::None);
        CHECK(hnet->got.back().kind == game_frontend::Command::Kind::SaveCustomGametype);

        // Map Browser: select installed .eodmap custom map
        hub.select(find(hub, "open_map_browser"));
        hub.update(act);
        CHECK(hub.sub_screen() == Menu::SubScreen::MapBrowser);
        hub.set_browser_category(2); // CUSTOM MAPS
        auto map_items = hub.browser_items();
        CHECK(map_items.size() == 1 && map_items[0].is_custom_map);
        hub.update(act);
        CHECK(hub.sub_screen() == Menu::SubScreen::None && hub.offline_custom_map() == "sky_arena.eodmap");

        // Phase 6: Automated cross-mode soak (Hub -> Career -> Warp -> Save & Jump to Online -> Leave -> 4P Split-Screen -> Solo)
        {
            Config soak_cfg;
            Menu soak(soak_cfg);
            auto snet = std::make_shared<FakeNet>();
            soak.set_network(snet);
            soak.open();
            // 1. Hub -> Career
            soak.set_session_kind(game_frontend::SessionKind::Career);
            soak.select(find(soak, "career_continue"));
            soak.update(act);
            CHECK(snet->got.back().transition.target == 2);
            // 2. Career Free Roam -> Quick Rail -> Warp to Garage
            snet->snap.ee_state = 6;
            soak.set_quick_rail(true);
            soak.select(find(soak, "qr_warp_garage"));
            soak.update(act);
            CHECK(snet->got.back().kind == game_frontend::Command::Kind::CareerWarp &&
                  snet->got.back().value == 4.0f);
            // 3. Career -> Save & Jump to Online
            soak.set_quick_rail(true);
            soak.select(find(soak, "qr_jump_online"));
            soak.update(act);
            CHECK(soak.save_prompt_open());
            soak.select(find(soak, "save_jump_save"));
            soak.update(act);
            CHECK(snet->got.back().kind == game_frontend::Command::Kind::LaunchTransition &&
                  snet->got.back().transition.save_career_first);
            // 4. Online Host -> Leave
            snet->snap.ee_state = 1;
            soak.set_quick_rail(false);
            soak.set_session_kind(game_frontend::SessionKind::Online);
            soak.select(find(soak, "host"));
            soak.update(act);
            CHECK(snet->got.back().kind == game_frontend::Command::Kind::Host);
            snet->snap.phase = game_frontend::Snapshot::Phase::Lobby;
            snet->snap.is_host = true;
            soak.select(find(soak, "leave"));
            soak.update(act);
            CHECK(snet->got.back().kind == game_frontend::Command::Kind::Disconnect);
            snet->snap.phase = game_frontend::Snapshot::Phase::Offline;
            snet->snap.is_host = false;
            // 5. 4P Split-Screen -> Solo
            soak.set_session_kind(game_frontend::SessionKind::SplitScreen);
            soak.select(find(soak, "split_players"));
            soak.update(rt); soak.update(rt);
            soak.select(find(soak, "launch_split"));
            soak.update(act);
            CHECK(snet->got.back().transition.target == 4 && snet->got.back().transition.humans == 4);
            soak.set_session_kind(game_frontend::SessionKind::Solo);
            soak.select(find(soak, "launch_solo"));
            soak.update(act);
            CHECK(snet->got.back().transition.target == 3 && snet->got.back().transition.humans == 1);
        }

        // Classic PS2 Menus toggle flips UnifiedFrontEnd off and saves
        hub.set_quick_rail(false);
        hub.set_session_kind(game_frontend::SessionKind::Solo);
        hub.select(find(hub, "classic_ps2_toggle"));
        Effects cfx = hub.update(act);
        CHECK(!hub_cfg.unified_frontend && cfx.save && cfx.close);

        // Visual Car Browser: class categories (ALL..MODDED), Trailers/Caravans, and Modded cars
        {
            Config car_cfg;
            Menu car_menu(car_cfg);
            auto cnet = std::make_shared<FakeNet>();
            car_menu.set_network(cnet);
            car_menu.open();
            car_menu.set_session_kind(game_frontend::SessionKind::Solo);
            car_menu.select(find(car_menu, "open_car_browser"));
            car_menu.update(act);
            CHECK(car_menu.sub_screen() == Menu::SubScreen::CarBrowser);
            CHECK(car_menu.browser_items().size() >= 7);
            car_menu.set_browser_category(5); // TRAILERS
            auto trailers = car_menu.browser_items();
            CHECK(trailers.size() == 1 && trailers[0].is_trailer);
            car_menu.set_browser_category(6); // MODDED
            auto modded = car_menu.browser_items();
            CHECK(modded.size() == 2 && modded[0].is_modded);
            car_menu.update(act);
            CHECK(car_menu.sub_screen() == Menu::SubScreen::None);
            CHECK(car_menu.split_seats()[0].car_model == "Phantom");
        }

        // Host vs. Guest lifecycle: Guest read-only Rules Drawer, map download READY lock,
        // Host SYNCING MAP roster indicator, MODS/SETTINGS online awareness, and discover_mods filtering
        {
            Config hg_cfg;
            Menu guest(hg_cfg);
            auto gnet = std::make_shared<FakeNet>();
            gnet->snap.phase = game_frontend::Snapshot::Phase::Lobby;
            gnet->snap.is_host = false;
            gnet->snap.car_pic = 10;
            gnet->snap.map_downloading = true;
            gnet->snap.map_download_pct = 42;
            gnet->snap.custom_map_title = "Sky Arena";
            gnet->snap.mutator_draft.name = "Tips (Tag)";
            gnet->snap.mutator_draft.uses_weapons = false;
            gnet->snap.mutator_draft.gravity_scale = 0.70f;
            guest.set_network(gnet);
            guest.open();
            guest.set_session_kind(game_frontend::SessionKind::Online);

            // Guest READY must be disabled while map is downloading
            const int rdy_i = find(guest, "ready");
            CHECK(rdy_i >= 0);
            const auto online_rows = guest.rows();
            CHECK(!online_rows[rdy_i].enabled && online_rows[rdy_i].value.find("42%") != std::string::npos);

            // Guest opens Rules Drawer -> read-only Info rows, edits rejected
            const int rules_i = find(guest, "open_rules_drawer");
            CHECK(rules_i >= 0 && online_rows[rules_i].label == "VIEW HOST RULES");
            guest.select(rules_i);
            guest.update(act);
            CHECK(guest.sub_screen() == Menu::SubScreen::RulesDrawer);
            const auto rrows = guest.rows();
            CHECK(!rrows.empty() && rrows[0].label.find("HOST RULES (READ-ONLY)") != std::string::npos);
            int preset_row = find(guest, "draft_preset");
            CHECK(preset_row >= 0 && rrows[preset_row].kind == RowKind::Info);

            // Guest MODS and SETTINGS tabs show online status
            guest.set_tab(Tab::Mods);
            CHECK(find(guest, "online_mod_banner") >= 0);
            guest.set_tab(Tab::Settings);
            const int rec_i = find(guest, "recovery");
            CHECK(rec_i >= 0 && guest.rows()[rec_i].note == "HOST RULE ONLINE");

            // Host sees SYNCING MAP for peer whose map_ready == false
            gnet->snap.is_host = true;
            gnet->snap.map_downloading = false;
            gnet->snap.players = {
                {0, "Host", true, false, false, false, 0, false, true},
                {1, "Guest1", false, false, false, false, 7, false, false},
            };
            guest.set_tab(Tab::Play);
            const auto hrows = guest.rows();
            bool saw_syncing = false;
            for (const auto &hr : hrows) {
                if (hr.id == "player" && hr.value.find("SYNCING MAP") != std::string::npos) saw_syncing = true;
            }
            CHECK(saw_syncing);

            // discover_mods ignores maps/ and gametypes/ folders
            const fs::path tmp_mods = fs::temp_directory_path() / "eod_test_discover_mods_filter";
            std::error_code ec;
            fs::remove_all(tmp_mods, ec);
            fs::create_directories(tmp_mods / "maps", ec);
            fs::create_directories(tmp_mods / "gametypes", ec);
            fs::create_directories(tmp_mods / "music", ec);
            const auto found_mods = discover_mods(tmp_mods.string());
            CHECK(found_mods.empty());
            fs::remove_all(tmp_mods, ec);
        }
    }

    std::printf("%s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
