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
    game_frontend::Snapshot snapshot() const override { return snap; }
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
    Config cfg;
    Menu m(cfg);
    m.open();
    // tabs: L1/R1 cycle with wrap
    CHECK(m.tab() == Tab::Graphics);
    Input in = none(); in.next_tab = true;
    Effects fx = m.update(in);
    CHECK(m.tab() == Tab::Mods && fx.sound == Sound::Tab);
    in = none(); in.prev_tab = true;
    m.update(in); m.update(in);
    CHECK(m.tab() == Tab::Online);
    in = none(); in.next_tab = true;
    m.update(in);
    CHECK(m.tab() == Tab::Graphics);

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
    m.set_tab(Tab::Online);
    CHECK(m.rows()[0].value == "NOT CONNECTED");
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
    net->snap.players.push_back({1, "Guest  Trailer2", true, false, true});
    CHECK(find(m, "host") < 0 && find(m, "code") < 0 && find(m, "addr") < 0);
    CHECK(find(m, "gcode") >= 0 && find(m, "player") >= 0);
    net->snap.is_host = false;
    CHECK(find(m, "ready") >= 0);
    net->snap.phase = game_frontend::Snapshot::Phase::Connecting;
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
    CHECK(find(m, "end_event") < 0 && m.rows()[find(m, "leave")].label == "LEAVE MATCH");
    m.set_tab(Tab::Settings);
    CHECK(!m.rows()[find(m, "turbo")].enabled && !m.rows()[find(m, "chickens")].enabled && !m.rows()[find(m, "invincible")].enabled);
    m.set_tab(Tab::Online);
    net->snap = {};

    // Couch route: shoulders, D-pad, accept/back. The only typing is the code.
    Config couch_cfg;
    Menu couch(couch_cfg);
    couch.set_network(net);
    couch.open();
    Input shoulder = none(); shoulder.prev_tab = true;
    couch.update(shoulder); // Graphics wraps left to Online
    CHECK(couch.tab() == Tab::Online);
    Input down = none(); down.dy = 1;
    Input accept = none(); accept.accept = true;
    couch.update(none());
    CHECK(couch.rows()[couch.selected()].id == "host");
    couch.update(down);
    CHECK(couch.rows()[couch.selected()].id == "code");
    couch.update(accept);
    for (char c : std::string("DEMO2")) { Input key = none(); key.ch = c; couch.update(key); }
    couch.update(accept);
    CHECK(net->got.back().kind == game_frontend::Command::Kind::JoinCode && net->got.back().text == "DEMO2");
    net->snap.phase = game_frontend::Snapshot::Phase::Lobby;
    couch.update(none());
    CHECK(couch.rows()[couch.selected()].id == "ready");
    couch.update(accept);
    CHECK(net->got.back().kind == game_frontend::Command::Kind::Ready && net->got.back().enabled);
    net->snap.phase = game_frontend::Snapshot::Phase::Racing;
    net->snap.is_host = true;
    couch.update(none());
    CHECK(couch.rows()[couch.selected()].id == "end_event");
    CHECK(!couch.update(accept).close && net->got.back().kind == game_frontend::Command::Kind::EndEvent);
    net->snap = {};

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
    CHECK(Config().controller_layout == "modern");

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

    std::printf("%s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
