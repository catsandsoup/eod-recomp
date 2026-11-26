// frontend.cpp - the front end's model (see frontend.h).
#include "frontend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "../app/game_frontend.h"
#include "../platform/keyboard_input.h"
#include "../../src/mutator_presets.h"

namespace fs = std::filesystem;

namespace frontend {

const char *tab_name(Tab t) {
    switch (t) {
    case Tab::Play: return "PLAY";
    case Tab::Mods: return "MODS";
    case Tab::Graphics: return "GRAPHICS";
    case Tab::Inputs: return "INPUTS";
    case Tab::Settings: return "SETTINGS";
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
std::string upper(std::string s);
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
        if (name.empty() || name[0] == '.' || name == "integrated" || name == "music" || name == "maps" || name == "gametypes") continue;  // music/: Custom Music; maps/ & gametypes/: custom maps & modes
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
Menu::Menu(Config &cfg) : cfg_(cfg) {
    mods_ = builtin_mods();
    static const char *kDefaultCars[4] = {"Gladiator", "Outlaw", "Bulldog", "Bus"};
    for (int i = 0; i < 4; ++i) {
        split_seats_[i].active = (i < split_players_);
        split_seats_[i].pad_port = i;
        split_seats_[i].car_model = kDefaultCars[i];
        split_seats_[i].paint = i;
    }
    sync_draft_from_preset(mutator::PRESET_STANDARD);
}
Menu::~Menu() = default;

void Menu::set_session_kind(game_frontend::SessionKind k) {
    session_kind_ = k;
    sub_screen_ = SubScreen::None;
    sel_[(int)Tab::Play] = 0;
    scroll_[(int)Tab::Play] = 0;
}

void Menu::set_sub_screen(SubScreen s) {
    sub_screen_ = s;
    browser_sel_ = 0;
    if (s == SubScreen::RulesDrawer) {
        sel_[(int)Tab::Play] = 0;
        scroll_[(int)Tab::Play] = 0;
    }
}

void Menu::set_browser_category(int c) {
    const int max_c = sub_screen_ == SubScreen::CarBrowser ? 6 : sub_screen_ == SubScreen::MapBrowser ? 2 : 3;
    browser_cat_ = std::clamp(c, 0, max_c);
    browser_sel_ = 0;
}

std::vector<Menu::BrowserItem> Menu::browser_items() const {
    std::vector<BrowserItem> out;
    game_frontend::Snapshot snap;
    if (net_) snap = net_->snapshot();

    if (sub_screen_ == SubScreen::CarBrowser) {
        // 0=ALL, 1=COMPACT, 2=MIDSIZE, 3=MUSCLE, 4=SPECIAL, 5=TRAILERS, 6=MODDED
        static const char *kCatNames[7] = {"ALL", "COMPACT", "MIDSIZE", "MUSCLE", "SPECIAL", "TRAILERS", "MODDED"};
        std::vector<game_frontend::CarEntry> cars = snap.car_catalog;
        if (cars.empty()) {
            // Fallback list when unit-tested without EE RAM
            cars = {
                {"Algar", "COMPACT", 0, 1, 4, false, false},
                {"Gladiator", "MIDSIZE", 1, 10, 6, false, false},
                {"Outlaw", "MUSCLE", 2, 107, 5, false, false},
                {"Bus", "SPECIAL", 3, 505, 2, false, false},
                {"Trailer", "TRAILERS", 3, 400, 1, true, false},
                {"Phantom", "MODDED", 2, 775, 2, false, true},
                {"Cup Car", "MODDED", 4, 777, 3, false, true},
            };
        }
        for (const auto &c : cars) {
            if (browser_cat_ > 0 && c.category != kCatNames[browser_cat_]) continue;
            BrowserItem it;
            it.id = "car:" + c.model;
            it.title = upper(c.model);
            it.subtitle = c.category + "  •  " + std::to_string(c.paint_count) + (c.paint_count == 1 ? " PAINT" : " PAINTS");
            it.badge = c.is_modded ? "MODDED" : c.is_trailer ? "TRAILER" : c.category;
            it.car_num = c.vehicle_num;
            it.is_car = true;
            it.is_trailer = c.is_trailer;
            it.is_modded = c.is_modded;
            out.push_back(std::move(it));
        }
        return out;
    }
    if (sub_screen_ == SubScreen::MapBrowser) {
        // 0 = ALL, 1 = RETAIL TRACKS, 2 = CUSTOM MAPS (.eodmap)
        if (browser_cat_ == 0 || browser_cat_ == 1) {
            std::vector<std::string> tracks = snap.retail_tracks;
            if (tracks.empty()) {
                tracks = {"Anoka", "Bemidji", "Dennison", "Hastings", "HomeBase", "Mankato", "Springfield", "Welch", "Woodly"};
            }
            for (const auto &t : tracks) {
                BrowserItem it;
                it.id = "track:" + t;
                it.title = upper(t);
                it.subtitle = "RETAIL ARENA";
                it.badge = "RETAIL";
                it.track_art = t;
                out.push_back(std::move(it));
            }
        }
        if (browser_cat_ == 0 || browser_cat_ == 2) {
            for (const auto &cm : snap.custom_maps) {
                BrowserItem it;
                it.id = "cmap:" + cm.id;
                it.title = upper(cm.title.empty() ? cm.id : cm.title);
                it.subtitle = cm.author.empty() ? cm.id : ("BY " + upper(cm.author));
                it.badge = "CUSTOM MAP";
                it.track_art = cm.base_arena.empty() ? "Anoka" : cm.base_arena;
                it.is_custom_map = true;
                out.push_back(std::move(it));
            }
        }
        return out;
    }

    // ModeBrowser: 0=ALL, 1=RETAIL, 2=PARTY & ARENA, 3=MY CUSTOM MODES
    if (browser_cat_ == 0 || browser_cat_ == 2) {
        static const struct { uint8_t preset; const char *art; bool feat; } kParty[] = {
            {mutator::PRESET_TIPS_TAG,        "Anoka",       true},
            {mutator::PRESET_BATTLE_ROYALE,   "HomeBase",    true},
            {mutator::PRESET_DESTRUCTIONWARE, "Woodly",      true},
            {mutator::PRESET_INFECTION,       "Hastings",    false},
            {mutator::PRESET_FIESTA_ROULETTE, "Springfield", false},
            {mutator::PRESET_CHAIN_GANG,      "Mankato",     false},
        };
        for (const auto &p : kParty) {
            BrowserItem it;
            it.id = "preset:" + std::to_string(p.preset);
            it.title = mutator::preset_label(p.preset);
            it.subtitle = mutator::preset_description(p.preset);
            it.badge = p.feat ? "FEATURED" : "PARTY & ARENA";
            it.track_art = p.art;
            it.preset_id = p.preset;
            it.featured = p.feat;
            out.push_back(std::move(it));
        }
    }
    if (browser_cat_ == 0 || browser_cat_ == 1) {
        static const struct { const char *name; const char *art; const char *desc; } kRetail[] = {
            {"Battle (8 AI)",       "Anoka",       "Open-arena combat with 8 AI opponents."},
            {"Battle (4 AI)",       "Welch",       "Classic 4-AI Battle arena."},
            {"No-Rules Race",       "Woodly",      "Full-contact oval & dirt racing."},
            {"Figure-8 Jump Race",  "Hastings",    "High-speed intersection mayhem."},
            {"Demolition Derby",    "Dennison",    "Last car running in the pit."},
            {"School Bus",          "Springfield", "Heavyweight bus carnage."},
            {"Trailer Race",        "Mankato",     "Tow trailers through traffic."},
        };
        for (const auto &rt : kRetail) {
            BrowserItem it;
            it.id = std::string("retail:") + rt.name;
            it.title = upper(rt.name);
            it.subtitle = rt.desc;
            it.badge = "RETAIL";
            it.track_art = rt.art;
            it.preset_id = mutator::PRESET_STANDARD;
            out.push_back(std::move(it));
        }
    }
    if (browser_cat_ == 0 || browser_cat_ == 3) {
        for (const auto &m : snap.modes) {
            if (!m.is_custom_json) continue;
            BrowserItem it;
            it.id = "custom:" + m.id;
            it.title = upper(m.name);
            it.subtitle = m.description;
            it.badge = "CUSTOM JSON";
            it.track_art = m.default_track.empty() ? "Anoka" : m.default_track;
            it.is_custom_mode = true;
            out.push_back(std::move(it));
        }
        BrowserItem add;
        add.id = "save_new_mode";
        add.title = "+ SAVE CURRENT AS NEW MODE";
        add.subtitle = "Writes current ruleset to mods/gametypes/<id>.json";
        add.badge = "CREATE";
        add.track_art = offline_track_.empty() ? "Anoka" : offline_track_;
        add.is_save_new = true;
        out.push_back(std::move(add));
    }
    return out;
}

void Menu::activate_browser_item(int idx, Effects &fx) {
    const auto items = browser_items();
    if (idx < 0 || idx >= (int)items.size()) { fx.sound = Sound::Error; return; }
    const auto &it = items[idx];
    fx.sound = Sound::Select;
    if (it.is_save_new) {
        push_mutator_draft();
        if (net_) {
            game_frontend::Command sc{};
            sc.kind = game_frontend::Command::Kind::SaveCustomGametype;
            sc.draft = mutator_draft_;
            net_->submit(std::move(sc));
        }
        set_status("Saved custom mode to mods/gametypes/");
        sub_screen_ = SubScreen::None;
        return;
    }
    if (sub_screen_ == SubScreen::CarBrowser) {
        if (it.is_car && it.id.compare(0, 4, "car:") == 0) {
            const std::string model = it.id.substr(4);
            split_seats_[0].car_model = model;
            split_seats_[0].vehicle_num = it.car_num;
            split_seats_[0].paint = 0;
            pic_car_ = it.car_num;
            if (net_ && session_kind_ == game_frontend::SessionKind::Online) {
                game_frontend::Command opt{};
                opt.kind = game_frontend::Command::Kind::Option;
                opt.text = "car_model:" + model;
                net_->submit(std::move(opt));
            }
        }
        sub_screen_ = SubScreen::None;
        return;
    }
    if (sub_screen_ == SubScreen::MapBrowser) {
        if (it.is_custom_map) {
            offline_custom_map_ = it.id.substr(5);
            offline_track_ = it.track_art;
            if (net_ && session_kind_ == game_frontend::SessionKind::Online) {
                net_->submit({game_frontend::Command::Kind::SelectCustomMap, offline_custom_map_});
            }
        } else {
            offline_custom_map_.clear();
            offline_track_ = it.id.substr(6);
            if (net_ && session_kind_ == game_frontend::SessionKind::Online) {
                net_->submit({game_frontend::Command::Kind::SelectCustomMap, ""});
            }
        }
        sub_screen_ = SubScreen::None;
        return;
    }
    // ModeBrowser selection
    if (it.id.compare(0, 7, "preset:") == 0) {
        offline_preset_ = it.preset_id;
        offline_event_ = "Battle";
        sync_draft_from_preset(it.preset_id);
        push_mutator_draft();
    } else if (it.id.compare(0, 7, "retail:") == 0) {
        offline_preset_ = mutator::PRESET_STANDARD;
        offline_event_ = it.id.substr(7);
        sync_draft_from_preset(mutator::PRESET_STANDARD);
        push_mutator_draft();
    } else if (it.is_custom_mode) {
        offline_preset_ = mutator::PRESET_CUSTOM_JSON;
        mutator_draft_.id = it.id.substr(7);
        mutator_draft_.name = it.title;
        push_mutator_draft();
    }
    sub_screen_ = SubScreen::None;
}

void Menu::set_rules_tab(int t) {
    rules_tab_ = std::clamp(t, 0, 3);
    sel_[(int)Tab::Play] = 0;
    scroll_[(int)Tab::Play] = 0;
}

void Menu::set_quick_rail(bool on) {
    quick_rail_ = on;
    quick_scoreboard_ = false;
    save_prompt_ = false;
    sub_screen_ = SubScreen::None;
    if (on) {
        tab_ = Tab::Play;
        sel_[(int)Tab::Play] = 0;
        scroll_[(int)Tab::Play] = 0;
    }
}

int Menu::distinct_chassis_count() const {
    std::vector<std::string> seen;
    const int n = session_kind_ == game_frontend::SessionKind::SplitScreen ? split_players_ : 1;
    for (int i = 0; i < n && i < 4; ++i) {
        const std::string &m = split_seats_[i].car_model;
        if (!m.empty() && std::find(seen.begin(), seen.end(), m) == seen.end()) seen.push_back(m);
    }
    return (int)seen.size();
}

void Menu::sync_draft_from_preset(uint8_t preset_id) {
    mutator_draft_ = game_frontend::MutatorDraft{};
    mutator_draft_.preset_id = preset_id;
    mutator::GametypeConfig cfg;
    if (mutator::build_preset(preset_id, cfg)) {
        mutator_draft_.id = cfg.id;
        mutator_draft_.name = cfg.name;
        mutator_draft_.rule_flags = cfg.base_rule_flags;
        mutator_draft_.feat_bits = cfg.base_feats;
        mutator_draft_.tips_fuse_sec = cfg.tips_fuse_sec;
        mutator_draft_.fiesta_interval_sec = cfg.fiesta_interval_sec;
        mutator_draft_.shuffle_rounds = cfg.shuffle_rounds;
        mutator_draft_.rounds_to_win = cfg.rounds_to_win;
        mutator_draft_.gravity_scale = cfg.base_traits.gravity_scale;
        mutator_draft_.mass_scale = cfg.base_traits.mass_scale;
        mutator_draft_.grip_scale = cfg.base_traits.grip_scale;
        mutator_draft_.torque_scale = cfg.base_traits.torque_scale;
        mutator_draft_.top_speed_scale = cfg.base_traits.top_speed_scale;
        mutator_draft_.damage_taken_scale = cfg.base_traits.damage_taken_scale;
        mutator_draft_.chicken_force_scale = cfg.base_traits.chicken_force_scale;
        mutator_draft_.chicken_radius_scale = cfg.base_traits.chicken_radius_scale;
        mutator_draft_.chicken_reload_sec = cfg.base_traits.chicken_reload_sec;
        mutator_draft_.uses_weapons = mutator::config_uses_weapons(cfg);
        for (const auto &tr : cfg.base_triggers) {
            if (tr.kind == mutator::TriggerKind::RingOut) {
                mutator_draft_.storm_radius_m = tr.extents[0];
                mutator_draft_.storm_shrink_mps = tr.shrink_rate_mps;
                mutator_draft_.storm_dps = tr.dps_outside;
                break;
            }
        }
        mutator_draft_.custom_json = mutator::serialize_gametype_json(cfg);
    } else {
        mutator_draft_.id = "standard";
        mutator_draft_.name = "Standard (Vanilla)";
        mutator_draft_.uses_weapons = true;
    }
    mutator_draft_.summary_lines.clear();
    mutator_draft_.summary_lines.push_back(mutator::preset_description(preset_id));
    if (!mutator_draft_.uses_weapons) mutator_draft_.summary_lines.push_back("Weapons: OFF (Mode Enforced)");
    else if (mutator_draft_.feat_bits & (mutator::FB_CHICKEN_STRAIGHT | mutator::FB_CHICKEN_MORTAR))
        mutator_draft_.summary_lines.push_back("Weapons: Chickens ON");
    if (mutator_draft_.rule_flags & mutator::RF_TIPS_TAG) {
        char buf[64];
        snprintf(buf, sizeof buf, "Personal Fuse: %.0fs | Immunity: 3.5s", mutator_draft_.tips_fuse_sec);
        mutator_draft_.summary_lines.push_back(buf);
    }
}

void Menu::push_mutator_draft() {
    mutator::GametypeConfig cfg;
    if (!mutator::build_preset(mutator_draft_.preset_id, cfg)) {
        cfg.id = mutator_draft_.id.empty() ? "custom_rules" : mutator_draft_.id;
        cfg.name = mutator_draft_.name.empty() ? "Custom Rules" : mutator_draft_.name;
    }
    cfg.base_rule_flags = mutator_draft_.rule_flags;
    cfg.base_feats = mutator_draft_.feat_bits;
    cfg.tips_fuse_sec = mutator_draft_.tips_fuse_sec;
    cfg.fiesta_interval_sec = mutator_draft_.fiesta_interval_sec;
    cfg.shuffle_rounds = mutator_draft_.shuffle_rounds;
    cfg.rounds_to_win = mutator_draft_.rounds_to_win;
    cfg.base_traits.gravity_scale = mutator_draft_.gravity_scale;
    cfg.base_traits.mass_scale = mutator_draft_.mass_scale;
    cfg.base_traits.grip_scale = mutator_draft_.grip_scale;
    cfg.base_traits.torque_scale = mutator_draft_.torque_scale;
    cfg.base_traits.top_speed_scale = mutator_draft_.top_speed_scale;
    cfg.base_traits.damage_taken_scale = mutator_draft_.damage_taken_scale;
    cfg.base_traits.chicken_force_scale = mutator_draft_.chicken_force_scale;
    cfg.base_traits.chicken_radius_scale = mutator_draft_.chicken_radius_scale;
    cfg.base_traits.chicken_reload_sec = mutator_draft_.chicken_reload_sec;
    for (auto &tr : cfg.base_triggers) {
        if (tr.kind == mutator::TriggerKind::RingOut) {
            tr.extents[0] = tr.extents[2] = mutator_draft_.storm_radius_m;
            tr.shrink_rate_mps = mutator_draft_.storm_shrink_mps;
            tr.dps_outside = mutator_draft_.storm_dps;
        }
    }
    cfg.sanitize();
    mutator_draft_.uses_weapons = mutator::config_uses_weapons(cfg);
    mutator_draft_.custom_json = mutator::serialize_gametype_json(cfg);
    if (net_) {
        game_frontend::Command cmd{};
        cmd.kind = game_frontend::Command::Kind::UpdateMutatorDraft;
        cmd.draft = mutator_draft_;
        net_->submit(std::move(cmd));
    }
}

void Menu::launch_offline_match(bool split) {
    if (!net_) return;
    game_frontend::Command cmd{};
    cmd.kind = game_frontend::Command::Kind::LaunchTransition;
    cmd.transition.target = split ? 4 : 3; // SplitScreen = 4, Solo = 3
    cmd.transition.track = offline_track_.empty() ? "Anoka" : offline_track_;
    cmd.transition.event = offline_preset_ > 0 ? "Battle" : (offline_event_.empty() ? "Battle (8 AI)" : offline_event_);
    cmd.transition.custom_map_id = offline_custom_map_;
    cmd.transition.gametype_preset = offline_preset_;
    cmd.transition.mutator_json = offline_preset_ > 0 ? mutator_draft_.custom_json : "";
    cmd.transition.humans = split ? std::clamp(split_players_, 2, 4) : 1;
    cmd.transition.ai_bots = offline_ai_bots_;
    cmd.transition.laps = offline_laps_;
    for (int i = 0; i < 4; ++i) {
        cmd.transition.seat_cars[i] = split_seats_[i].car_model;
        cmd.transition.seat_paints[i] = split_seats_[i].paint;
        cmd.transition.seat_vehicles[i] = split_seats_[i].vehicle_num;
    }
    net_->submit(std::move(cmd));
}

void Menu::launch_career(bool classic_hub, bool new_career) {
    if (!net_) return;
    game_frontend::Command cmd{};
    cmd.kind = game_frontend::Command::Kind::LaunchTransition;
    cmd.transition.target = 2; // Career = 2
    cmd.transition.career_place = classic_hub ? -1 : career_warp_idx_;
    cmd.transition.career_eve = -1;
    cmd.transition.classic_career_hub = classic_hub;
    cmd.transition.new_career = new_career;
    net_->submit(std::move(cmd));
}

void Menu::set_network(std::shared_ptr<game_frontend::Network> n) {
    net_ = std::move(n);
    online_dialog_ = OnlineDialog::None;
    joined_lobby_ = false;
    join_attempt_.reset();
}

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
    close_map_picker();
    remove_player_.clear();
    details_ = false;
    music_open_ = false;
    if (music_) music_->rescan();
    status_.clear();
    rebind_ = -1;
    text_id_.clear();
    rescan_mods();
}

void Menu::set_tab(Tab t) {
    if (t != Tab::Play && online_modal_title()) return;
    if (t != Tab::Play) {
        close_map_picker();
        sub_screen_ = SubScreen::None;
    }
    tab_ = t;
    details_ = false;
    status_.clear();
}

void Menu::select(int row) {
    std::vector<Row> &r = rows_for(error_active() ? Tab::Count : tab_);
    if (row >= 0 && row < (int)r.size() && r[row].kind != RowKind::Header) sel_[error_active() ? (int)Tab::Count : (int)tab_] = row;
}

void Menu::show_error(const std::string &title, const std::string &message, bool offer_disc, bool fatal) {
    error_title_ = title;
    error_message_ = message;
    error_disc_ = offer_disc;
    error_fatal_ = fatal;
    browse_ = false;
    sel_[(int)Tab::Count] = 0;
}

std::vector<Row> &Menu::rows_for(Tab t) {
    rebuild(t);
    return rows_[(int)t];
}

void Menu::rebuild(Tab t) {
    std::vector<Row> &r = rows_[(int)t];
    // A roster update may insert rows above the selected control.
    const int old = sel_[(int)t];
    const std::string selected = t == Tab::Play && old >= 0 && old < (int)r.size() ? r[old].id : "";
    r.clear();
    switch (t) {
    case Tab::Play: {
        if (quick_rail_) rows_quick_rail(r);
        else if (sub_screen_ == SubScreen::RulesDrawer) rows_rules_drawer(r);
        else rows_play(r);
        if (!selected.empty()) {
            auto it = std::find_if(r.begin(), r.end(), [&](const Row &row) { return row.id == selected; });
            if (it != r.end()) sel_[(int)t] = (int)(it - r.begin());
            else sel_[(int)t] = 0;
        }
        break;
    }
    case Tab::Mods: rows_mods(r); break;
    case Tab::Graphics: rows_graphics(r); break;
    case Tab::Inputs: rows_inputs(r); break;
    case Tab::Settings: rows_settings(r); break;
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
                         "5X 3200x2240", "6X 3840x2688", "7X 4480x3136", "8X 5120x3584"};
static_assert(sizeof(kScales) / sizeof(kScales[0]) == kMaxRenderScale + 1);
const char *kAspects[] = {"display", "4:3", "16:9", "16:10", "stretch", "auto"};
const char *kAspectLabels[] = {"MATCH DISPLAY", "4:3 (ORIGINAL)", "16:9 WIDESCREEN", "16:10 WIDESCREEN", "STRETCH", "AUTO (GAME OPTION)"};
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
std::string turbo_label(const Config &cfg, const HostInfo &host) {
    const bool controller = !host.device[0].empty() && host.device[0] != "Keyboard";
#ifdef _WIN32
    const bool keyboard = true;  // Win32 always uses the racing layout on port 0.
    const bool racing = true;
    const bool modern = true;
#else
    const bool keyboard = host.device[0] == "Keyboard";
    const bool racing = cfg.keyboard_layout == "racing";
    const bool modern = cfg.controller_layout == "modern";
#endif
    std::vector<int> keys;
    std::string binding;
    auto append = [&](const std::string &input) {
        if (!binding.empty()) binding += " / ";
        binding += input;
    };
    if (keyboard) {
        auto append_key = [&](int key) {
            if (!key || key == 0x70 || (racing && keyboard_input::reserved(key)) ||
                std::find(keys.begin(), keys.end(), key) != keys.end()) return;
#ifndef _WIN32
            if (key == 0x6D) return;  // The SDL keyboard backend has no Subtract mapping.
#endif
            keys.push_back(key);
            append(upper(key_name(key)));
        };
        std::istringstream configured(cfg.keys[KEY_L2]);
        for (std::string name; std::getline(configured, name, ',');) append_key(key_code(name));
#ifdef _WIN32
        // The driving backend adds these even when native L2 is rebound.
        append_key('Q');
        append_key('F');
#endif
    }
    if (controller) append(modern ? "RIGHT STICK CLICK" : "LEFT TRIGGER");
    return "TURBO BOOST [" + (binding.empty() ? "NO P1 BINDING" : binding) + "]";
}
std::string percent(int v) { return std::to_string(v) + "%"; }
void incoming_voice_rows(std::vector<Row> &rows, const Config &cfg) {
    rows.push_back(choice("voicevolume", "INCOMING VOICE VOLUME", percent(cfg.voice_volume), "LIVE",
                          "Adjust other players from 0% (silent) to 200%."));
    rows.back().slider_fraction = cfg.voice_volume / 200.0f;
    rows.push_back(toggle("voicefocus", "LOWER GAME DURING SPEECH", cfg.lower_game_during_voice, "LIVE",
                          "Temporarily lower the game while another player speaks. Turn off for a fixed game level."));
}
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
    int sc = std::clamp(cfg_.render_scale, 0, kMaxRenderScale);
#ifdef _WIN32
    if (sc == 0) sc = 1;
#endif
    r.push_back(choice("scale", "RENDER SCALE", kScales[sc], scale_note,
                       "Internal resolution the 3D is drawn at. Higher is sharper and needs a faster GPU; it also "
                       "supersamples (the anti-aliasing of this renderer). AUTO matches the display's pixel height. "
                       "7X and 8X require a compatible Vulkan GPU."));
    r.push_back(choice("aspect", "ASPECT RATIO", kAspectLabels[index_of(kAspects, 6, cfg_.aspect)], "NEXT LAUNCH",
                       "Match Display adjusts the camera and HUD to the window or fullscreen display. "
                       "Movies retain their original 4:3 composition."));
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
    if (host_.plume_look != PlumeLook::Unavailable) {
        r.push_back(choice("plume_look", "PLUME LOOK", host_.plume_look == PlumeLook::Modern ? "MODERN" : "CLASSIC",
                           "LIVE", "Switches between Classic PS2 styling and Modern lighting. F9 also changes the look. "
                           "A paused race shows the new look when play resumes."));
        return;
    }
    if (!host_.legacy_modern_available) return;
    r.push_back(header("LEGACY GS RENDERER"));
    r.push_back(choice("modern", "GS LOOK", cfg_.modern_renderer ? "MODERN" : "ORIGINAL", "LIVE",
                       "MODERN redraws the race's world and cars with real-time lighting, soft sun shadows, "
                       "reflections and ambient occlusion. ORIGINAL uses the PS2 graphics."));
    auto pc = [&](const char *id, const char *label, int v, const char *help) {
        Row row = choice(id, label, v == 0 ? std::string("OFF") : percent(v), "LIVE", help);
        row.enabled = cfg_.modern_renderer;
        r.push_back(row);
    };
    pc("m_shadows", "SUN SHADOWS", cfg_.modern_shadows, "Strength of the sun's shadows (OFF also saves their drawing time).");
    Row ss = toggle("m_soft", "SOFT SHADOWS", cfg_.modern_soft_shadows, "LIVE",
                    "Shadows soften with distance from what casts them and stay sharp where they touch.");
    ss.enabled = cfg_.modern_renderer;
    r.push_back(ss);
    pc("m_refl", "REFLECTIONS", cfg_.modern_reflections, "Clear coat on car paint, sky reflections and highlights.");
    pc("m_ao", "AMBIENT OCCLUSION", cfg_.modern_ao, "Contact shading in creases, under cars and against walls.");
    pc("m_bloom", "BLOOM", cfg_.modern_bloom, "Glow around bright highlights.");
    pc("m_exposure", "EXPOSURE", cfg_.modern_exposure, "Overall brightness of the lit scene.");
    pc("m_haze", "DISTANCE HAZE", cfg_.modern_haze, "Atmospheric haze toward the horizon.");
    pc("m_grade", "COLOUR GRADE", cfg_.modern_grade, "Saturation and contrast of the filmic grade (OFF: neutral).");
    pc("m_vignette", "VIGNETTE", cfg_.modern_vignette, "Darkened screen corners.");
}

void Menu::rows_settings(std::vector<Row> &r) {
    r.push_back(header("AUDIO"));
    r.push_back(toggle("audio", "SOUND", cfg_.audio, "LIVE", "All game sound on or off."));
    r.push_back(choice("volume", "MASTER VOLUME", percent(cfg_.volume), "LIVE",
                       "Overall game and voice volume."));
    r.push_back(choice("musicvolume", "MUSIC VOLUME", percent(cfg_.music_volume), "LIVE",
                       "Turn down or mute your soundtrack independently of engines, effects and voice."));
    incoming_voice_rows(r, cfg_);
    r.push_back(header("FRONT END & BOOT"));
    r.push_back(toggle("unified_fe", "UNIFIED PLAY HUB", cfg_.unified_frontend, "LIVE",
                       "Boot into the unified PC/Console PLAY Hub. Turn OFF for Classic PS2 Menus."));
    r.push_back(toggle("pause", "PAUSE GAME IN THIS MENU", cfg_.pause_in_menu, "LIVE",
                       "Holds the game while this menu is open (never during online play)."));
    r.push_back(toggle("fast_boot", "FAST BOOT", cfg_.fast_boot, "NEXT LAUNCH",
                       "Skips startup intro movies, title screen, and artificial loading delays."));
    r.push_back(toggle("skip_memcard", "SKIP MEMCARD DELAYS", cfg_.skip_memcard, "NEXT LAUNCH",
                       "Skips PS2 memory card boot hold delays while still loading your save."));
    r.push_back(header("DRIVING & NETWORK"));
    bool online_connected = false;
    if (net_) {
        const auto p = net_->snapshot().phase;
        online_connected = (p != game_frontend::Snapshot::Phase::Offline &&
                            p != game_frontend::Snapshot::Phase::Disconnected &&
                            p != game_frontend::Snapshot::Phase::Error);
    }
    r.push_back(toggle("recovery", "MANUAL RECOVERY ONLY", cfg_.manual_recovery, online_connected ? "HOST RULE ONLINE" : "LIVE",
                       "No automatic wrong-way or upside-down countdown for humans; SELECT still recovers. AI unchanged."));
    r.push_back(toggle("durability", "PER-CAR DURABILITY", cfg_.car_durability, online_connected ? "HOST RULE ONLINE" : "LIVE",
                       "Each car's own damage scale from its files (the game's menus already rate cars by it)."));
    Row nick = action("nickname", "SCREEN NAME", text_id_ == "nickname" ? text_ + "_" :
                      (cfg_.nickname.empty() ? "RANDOM" : cfg_.nickname), "Leave blank for a random name.");
    nick.kind = RowKind::Text;
    r.push_back(nick);
    r.push_back(choice("snapshot_buffer", "REMOTE CAR SMOOTHING", std::to_string(cfg_.snapshot_buffer_ms) + " ms", "LIVE",
                       "Longer buffering smooths uneven packet arrivals. Shorter buffering shows recent movement sooner."));
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
#ifdef _WIN32
        const bool modern = false; // XInput maps the triggers to native L2/R2.
#else
        const bool modern = cfg_.controller_layout == "modern";
#endif
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
    bool online_connected = false;
    if (net_) {
        const auto p = net_->snapshot().phase;
        online_connected = (p != game_frontend::Snapshot::Phase::Offline &&
                            p != game_frontend::Snapshot::Phase::Disconnected &&
                            p != game_frontend::Snapshot::Phase::Error);
    }
    if (online_connected) {
        Row ol = info("online_mod_banner", "ONLINE SESSION", "CONTENT MATCHED (MUSIC STAYS LOCAL)",
                      "Gameplay mods are verified with the host when joining. Custom Music stays local to your PC.");
        ol.good = true;
        r.push_back(ol);
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
    // Career unlocks (docs/CAREER_UNLOCKS.md): all on out of the box, saved under [Cheats] in eod.ini
    r.push_back(header(online_connected ? "CAREER UNLOCKS & CHEATS (OFFLINE ONLY)" : "CAREER UNLOCKS & CHEATS"));
    r.push_back(toggle("unlock", "UNLOCK ALL", cfg_.unlock_all, "NEXT LAUNCH",
                       "All tracks, cars, dares and feat prizes unlocked out of the box."));
    r.push_back(toggle("career_cheats", "CHEATS IN CAREER", cfg_.career_cheats, "LIVE",
                       "Lets unlocked cheat feats work in Career too."));
    r.push_back(toggle("cu_money", "CAREER MONEY FLOOR", cfg_.career_money, "LIVE",
                       "Career money never drops below $10,000, so every car and part can be bought. Winnings still add."));
    r.push_back(toggle("cu_rank", "CAREER RANK ON TOP", cfg_.career_rank, "LIVE",
                       "Your Career rank is held at the top (shown 1st) without ending the Career."));
    r.push_back(toggle("cu_open", "ALL SHOPS AND EVES OPEN", cfg_.career_open, "LIVE",
                       "Upgrades, Time Challenges, Troy's Place, the dealer and every Eve are open from the start."));
    r.push_back(toggle("cu_cars", "DEALER SELLS EVERY CAR", cfg_.career_all_cars, "NEXT VISIT",
                       "The dealer shows a new page of cars on every visit and covers every car, Phantom, the Cup car and the special vehicles included."));
    r.push_back(toggle("cu_swap", "FREE CAR SWAPPING", cfg_.career_free_swap, "LIVE",
                       "Selling or trading a car refunds its full price, so you can try every car at no loss."));
    r.push_back(toggle("cu_kill", "KILLABLE CHALLENGERS", cfg_.career_killable, "LIVE",
                       "Highway and Malt Shop challengers in Career free roam can take full damage and be wrecked."));
}

const char *Menu::online_modal_title() const {
    if (tab_ != Tab::Play || error_active()) return nullptr;
    switch (online_dialog_) {
    case OnlineDialog::Connecting:
        return join_attempt_ && join_attempt_->kind == game_frontend::Command::Kind::Host
            ? "SETTING UP LOBBY" : "CONNECTING TO LOBBY";
    case OnlineDialog::Failure: return joined_lobby_ ? "SESSION ENDED" : "CONNECTION FAILED";
    case OnlineDialog::None: return nullptr;
    }
    return nullptr;
}

void Menu::remember_join(const game_frontend::Command &command) {
    joined_lobby_ = false;
    join_attempt_ = std::make_unique<game_frontend::Command>(command);
}

void Menu::submit_join(game_frontend::Command command) {
    remember_join(command);
    net_->submit(std::move(command));
}

void Menu::cancel_join() {
    joined_lobby_ = false;
    join_attempt_.reset();
    online_dialog_ = OnlineDialog::None;
    if (net_) net_->submit({game_frontend::Command::Kind::Cancel});
}

void Menu::close_map_picker() {
    if (map_return_row_ >= 0) {
        sel_[int(Tab::Play)] = map_return_row_;
        map_return_row_ = -1;
    }
}

void Menu::rows_play(std::vector<Row> &r) {
    using SK = game_frontend::SessionKind;
    using P = game_frontend::Snapshot::Phase;
    game_frontend::Snapshot s;
    if (net_) {
        net_->viewing_online();
        s = net_->snapshot();
    }
    const bool net_busy = s.phase != P::Offline && s.phase != P::Disconnected && s.phase != P::Error;
    const bool net_failed = !net_busy && !s.error.empty();
    if (net_busy || net_failed) {
        session_kind_ = SK::Online;
        rows_online(r);
        return;
    }
    static const char *kSessionLabels[4] = {"CAREER", "SOLO QUICK PLAY", "2P-4P SPLIT-SCREEN", "ONLINE MULTIPLAYER"};
    r.push_back(choice("play_session", "SESSION", kSessionLabels[(int)session_kind_], "LIVE",
                       "Switch seamlessly between Career, Solo Quick Play, 2-4P Local Split-Screen, and 16P Online Multiplayer."));
    if (session_kind_ == SK::Online) {
        rows_online(r);
        return;
    }
    online_dialog_ = OnlineDialog::None;
    pic_track_ = offline_track_;
    pic_car_ = split_seats_[0].vehicle_num;
    if (session_kind_ == SK::Career) {
        static const char *kPlaces[8] = {
            "HOME BASE", "MALT SHOP (TJ'S)", "CHOOSE NEXT EVE", "DEALER (SCRAPYARD)",
            "GARAGE & UPGRADES", "TIME GATE 1", "TIME GATE 2", "TROY'S PLACE"
        };
        r.push_back(header("CAREER COMMAND CENTER"));
        r.push_back(action("career_continue", "CONTINUE CAREER", "[ PLAY ]",
                           "Jump straight into Career Free Roam at your selected warp destination."));
        r.push_back(choice("career_warp", "WARP TARGET", kPlaces[std::clamp(career_warp_idx_, 0, 7)], "LIVE",
                           "Destination in Career Free Roam when continuing Career."));
        r.push_back(action("career_classic_hub", "CLASSIC PS2 HUB", "[ OPEN HUB ]",
                           "Enter the native PS2 Career Homebase menu directly."));
        r.push_back(action("career_new", "NEW CAREER", "[ START NEW ]",
                           "Begin a fresh Career from rank 100."));
        r.push_back(action("classic_ps2_toggle", "CLASSIC PS2 MENUS", "[ SWITCH ]",
                           "Return to the original PS2 main menu shell (press F1 anytime to come back)."));
        return;
    }
    const bool split = (session_kind_ == SK::SplitScreen);
    r.push_back(header(split ? "LOCAL SPLIT-SCREEN SETUP" : "SOLO MATCH SETUP"));
    if (split) {
        r.push_back(choice("split_players", "PLAYERS", std::to_string(split_players_) + "P", "NEXT MATCH",
                           "Number of local split-screen seats (2P, 3P, or 4P multitap)."));
    }
    const std::string mode_val = offline_preset_ > 0 ? mutator::preset_label(offline_preset_) : upper(offline_event_);
    r.push_back(choice("play_mode", "MODE", mode_val, "NEXT MATCH",
                       "Cycle retail events or Party & Arena mutator modes. Press X/Square for Visual Mode Browser."));
    r.push_back(choice("play_track", "MAP", offline_custom_map_.empty() ? upper(offline_track_) : upper(offline_custom_map_), "NEXT MATCH",
                       "Choose any of the 17 retail tracks or an installed .eodmap custom arena."));
    if (!split) {
        r.push_back(choice("seat0_car", "CAR", upper(split_seats_[0].car_model), "NEXT MATCH",
                           "Choose your vehicle model."));
        r.push_back(choice("seat0_paint", "PAINT", "PAINT " + std::to_string(split_seats_[0].paint + 1), "NEXT MATCH",
                           "Cycle paint schemes for your selected car."));
    } else {
        for (int i = 0; i < split_players_ && i < 4; ++i) {
            const std::string id = "seat" + std::to_string(i) + "_car";
            const std::string lbl = "P" + std::to_string(i + 1) + " CAR";
            r.push_back(choice(id.c_str(), lbl.c_str(),
                               upper(split_seats_[i].car_model) + " (#" + std::to_string(split_seats_[i].paint + 1) + ")",
                               "NEXT MATCH",
                               "Independent per-seat car selection (up to 6 distinct chassis across all 16 cars)."));
        }
    }
    r.push_back(choice("play_laps", "LAPS", std::to_string(offline_laps_) + " LAPS", "NEXT MATCH",
                       "Lap count override for lap-based races (1-50)."));
    const std::string ai_str = offline_ai_bots_ < 0 ? "FILL (AUTO)"
                             : offline_ai_bots_ == 0 ? "OFF (0 BOTS)"
                             : std::to_string(offline_ai_bots_) + " BOTS";
    r.push_back(choice("play_ai", "AI BOTS", ai_str, "NEXT MATCH",
                       "Number of AI opponents to spawn (up to 16 total cars)."));
    r.push_back(action("open_mode_browser", "BROWSE MODES", "[ VISUAL BROWSER ]",
                       "Open the Trackmania-style Visual Mode Browser (Hero Cards + Categorized Grid)."));
    r.push_back(action("open_map_browser", "BROWSE MAPS", "[ TRACKS & .EODMAP ]",
                       "Browse all 17 retail tracks and installed .eodmap custom maps."));
    r.push_back(action("open_car_browser", "BROWSE CARS", "[ BY CLASS & MODS ]",
                       "Visual Car Selector sorted by class (Compact, Midsize, Muscle, Special, Trailers & Modded)."));
    r.push_back(action("open_rules_drawer", "CUSTOMIZE RULES", "[ X / CONFIGURE ]",
                       "Tune mode rules, weapons/feats, physics traits, and save custom JSON gametypes."));
    Row launch = action(split ? "launch_split" : "launch_solo",
                        split ? "START SPLIT-SCREEN" : "LAUNCH SOLO EVENT",
                        "[ >>> LAUNCH <<< ]",
                        "Launch directly into the configured match.");
    launch.good = true;
    r.push_back(launch);
    r.push_back(action("classic_ps2_toggle", "CLASSIC PS2 MENUS", "[ SWITCH ]",
                       "Close the Unified PLAY Hub and use the native PS2 menus."));
}

void Menu::rows_rules_drawer(std::vector<Row> &r) {
    static const char *kTabs[4] = {"MODE RULES", "FEATS & WEAPONS", "PHYSICS & SCALE", "BOTS & ROLES"};
    game_frontend::Snapshot snap;
    if (net_) snap = net_->snapshot();
    using P = game_frontend::Snapshot::Phase;
    const bool online_connected = snap.phase != P::Offline && snap.phase != P::Disconnected && snap.phase != P::Error;
    const bool guest_ro = online_connected && !snap.is_host;
    const auto &draft = (guest_ro && !snap.mutator_draft.name.empty()) ? snap.mutator_draft : mutator_draft_;

    r.push_back(header((std::string(guest_ro ? "HOST RULES (READ-ONLY)  -  " : "CUSTOMIZE RULES  -  ") + kTabs[std::clamp(rules_tab_, 0, 3)]).c_str()));
    r.push_back(choice("rules_subtab", "SECTION", kTabs[std::clamp(rules_tab_, 0, 3)], "LIVE",
                       "Switch between Mode Rules, Feats & Weapons, Physics & Scale, and Bots & Roles."));
    auto fmt = [](float v, const char *u) { char b[32]; snprintf(b, sizeof b, "%.2f%s", v, u); return std::string(b); };
    auto ro_choice = [&](const char *id, const char *lbl, const std::string &val, const char *note, const char *help) {
        return guest_ro ? info(id, lbl, val, "Controlled by the host.") : choice(id, lbl, val, note, help);
    };
    auto ro_toggle = [&](const char *id, const char *lbl, bool on, const char *note, const char *help) {
        return guest_ro ? info(id, lbl, on ? "ON" : "OFF", "Controlled by the host.") : toggle(id, lbl, on, note, help);
    };
    if (rules_tab_ == 0) {
        r.push_back(ro_choice("draft_preset", "BASE PRESET", mutator::preset_label(draft.preset_id), "LIVE",
                              mutator::preset_description(draft.preset_id)));
        r.push_back(ro_choice("draft_fuse", "TIPS FUSE", fmt(draft.tips_fuse_sec, " S"), "LIVE",
                              "Personal Hot Potato fuse duration in Tips (Tag) mode."));
        r.push_back(ro_choice("draft_storm_r", "STORM START RADIUS", fmt(draft.storm_radius_m, " M"), "LIVE",
                              "Initial safe-zone storm radius in Battle Royale."));
        r.push_back(ro_choice("draft_storm_s", "STORM SHRINK RATE", fmt(draft.storm_shrink_mps, " M/S"), "LIVE",
                              "How fast the Battle Royale storm closes in."));
        r.push_back(ro_choice("draft_storm_dps", "STORM DAMAGE", fmt(draft.storm_dps * 100.0f, "%/S"), "LIVE",
                              "Damage per second outside the safe zone."));
        r.push_back(ro_choice("draft_fiesta", "FIESTA INTERVAL", fmt(draft.fiesta_interval_sec, " S"), "LIVE",
                              "Seconds between trait & weapon rerolls in Fiesta Roulette."));
        r.push_back(ro_toggle("draft_shuffle", "SHUFFLE MICRO-ROUNDS", draft.shuffle_rounds, "LIVE",
                              "Randomize DestructionWare micro-round order."));
    } else if (rules_tab_ == 1) {
        const bool wp_ok = draft.uses_weapons;
        r.push_back(ro_toggle("draft_turbo", "TURBO BOOST", (draft.feat_bits & mutator::FB_TURBO) != 0, "LIVE",
                              "Enable L2 / Right-Stick Turbo Boost."));
        Row ch = ro_toggle("draft_chickens", "CHICKEN FIGHT", (draft.feat_bits & mutator::FB_CHICKEN_STRAIGHT) != 0,
                           wp_ok ? "LIVE" : "NO WEAPONS MODE", "Enable straight-shot explosive chickens.");
        if (!guest_ro) ch.enabled = wp_ok;
        r.push_back(ch);
        Row cr = ro_choice("draft_chicken_reload", "CHICKEN RELOAD", fmt(draft.chicken_reload_sec, " S"),
                           wp_ok ? "LIVE" : "NO WEAPONS MODE", "Seconds between chicken shots.");
        if (!guest_ro) cr.enabled = wp_ok;
        r.push_back(cr);
        Row cf = ro_choice("draft_chicken_force", "CHICKEN BLAST FORCE", fmt(draft.chicken_force_scale, "X"),
                           wp_ok ? "LIVE" : "NO WEAPONS MODE", "Explosion knockback impulse multiplier.");
        if (!guest_ro) cf.enabled = wp_ok;
        r.push_back(cf);
        r.push_back(ro_toggle("draft_ohk", "ONE-HIT KILLS", (draft.feat_bits & mutator::FB_ONE_HIT_KILLS) != 0, "LIVE",
                              "Qualifying impacts instantly wreck opponents."));
        r.push_back(ro_toggle("draft_deadly", "DEADLY AMBITION", (draft.feat_bits & mutator::FB_DEADLY_AMBITION) != 0, "LIVE",
                              "1st-place car gradually loses health."));
        r.push_back(ro_toggle("draft_invincible", "INVINCIBLE CARS", (draft.feat_bits & mutator::FB_INVINCIBLE) != 0, "LIVE",
                              "Suppresses mechanical wreck damage."));
    } else if (rules_tab_ == 2) {
        r.push_back(ro_choice("draft_grav", "GRAVITY SCALE", fmt(draft.gravity_scale, "X"), "LIVE",
                              "Multiplies world gravity (0.25x Moon -> 2.50x Heavy)."));
        r.push_back(ro_choice("draft_mass", "CAR MASS SCALE", fmt(draft.mass_scale, "X"), "LIVE",
                              "Multiplies vehicle collision mass."));
        r.push_back(ro_choice("draft_grip", "TIRE GRIP SCALE", fmt(draft.grip_scale, "X"), "LIVE",
                              "Scales tire traction (low = ice drift, high = slot-car grip)."));
        r.push_back(ro_choice("draft_torque", "ENGINE TORQUE", fmt(draft.torque_scale, "X"), "LIVE",
                              "Scales drive acceleration and boost impulse."));
        r.push_back(ro_choice("draft_speed", "TOP SPEED CAP", fmt(draft.top_speed_scale, "X"), "LIVE",
                              "Scales maximum speed limit."));
        r.push_back(ro_choice("draft_dmg", "DAMAGE TAKEN", fmt(draft.damage_taken_scale, "X"), "LIVE",
                              "Scales collision damage received."));
    } else {
        const int shown_ai = online_connected ? snap.lobby_ai_count : offline_ai_bots_;
        const std::string ai_str = shown_ai < 0 || shown_ai >= 16 ? "FILL (AUTO)"
                                 : shown_ai == 0 ? "OFF (0 BOTS)"
                                 : std::to_string(shown_ai) + " BOTS";
        r.push_back(ro_choice("play_ai", "AI BOT FILL", ai_str, "NEXT MATCH",
                              "Control AI opponent fill count up to 16 total cars."));
        if (!guest_ro) {
            r.push_back(action("draft_save_json", "SAVE AS CUSTOM JSON", "[ SAVE TO MODS/GAMETYPES ]",
                               "Auto-slugs and saves this ruleset to mods/gametypes/<id>.json (no keyboard required)."));
        }
    }
    r.push_back(action("rules_back", "DONE / BACK TO LOBBY", "[ BACK ]", "Return to the PLAY lobby."));
}

void Menu::rows_quick_rail(std::vector<Row> &r) {
    game_frontend::Snapshot s;
    if (net_) s = net_->snapshot();
    const bool in_career = (s.ee_state == 5 || s.ee_state == 6 || s.ee_state == 7);
    const bool in_online = (s.phase == game_frontend::Snapshot::Phase::Racing ||
                            s.phase == game_frontend::Snapshot::Phase::Loading);
    if (save_prompt_) {
        r.push_back(header("LEAVE CAREER?"));
        r.push_back(action("save_jump_save", "SAVE & SWITCH", "[ SAVE FIRST ]",
                           "Save Career progress to Memory Card + Extended Garage before switching modes."));
        r.push_back(action("save_jump_discard", "SWITCH WITHOUT SAVING", "[ DISCARD ]",
                           "Exit Career cleanly without saving."));
        r.push_back(action("save_jump_cancel", "CANCEL", "[ STAY ]", "Return to the pause rail."));
        return;
    }
    r.push_back(header("QUICK PAUSE"));
    r.push_back(action("qr_resume", "RESUME DRIVING", "[ PLAY ]", "Close the quick rail and return to the action."));
    r.push_back(action("qr_scoreboard", "LIVE SCOREBOARD & RULES", quick_scoreboard_ ? "[ HIDE ]" : "[ SHOW ]",
                       "View live 16-slot roster, scores, roles, and active mutator rules."));
    if (in_career) {
        r.push_back(header("CAREER QUICK ACTIONS"));
        r.push_back(action("qr_career_save", "SAVE CAREER NOW", "[ SAVE ]",
                           "Save your Career progress and extended garage immediately."));
        r.push_back(action("qr_warp_home", "WARP: HOME BASE", "[ WARP ]", "Teleport to your Trailer."));
        r.push_back(action("qr_warp_dealer", "WARP: DEALER", "[ WARP ]", "Teleport to Sweeny's Scrapyard."));
        r.push_back(action("qr_warp_garage", "WARP: GARAGE", "[ WARP ]", "Teleport to Upgrades & Paint."));
        r.push_back(action("qr_warp_malt", "WARP: MALT SHOP", "[ WARP ]", "Teleport to TJ's Diner."));
        r.push_back(action("qr_warp_troy", "WARP: TROY'S PLACE", "[ WARP ]", "Teleport to Troy's Place."));
        r.push_back(action("qr_warp_eve", "WARP: NEXT EVE", "[ WARP ]", "Teleport to Choose Eve."));
    } else if (!in_online) {
        r.push_back(action("qr_rematch", "INSTANT REMATCH", "[ RESTART ]", "Relaunch this match immediately."));
    } else if (s.is_host) {
        r.push_back(action("qr_next_round", "NEXT ROUND", "[ ADVANCE ]", "Advance to the next micro-round."));
        r.push_back(action("end_event", "BACK TO ONLINE LOBBY", "[ END RACE ]", "End the event and keep the room open."));
    }
    r.push_back(header("SWITCH MODE / JUMP TO"));
    const std::string crew_badge = s.crew.empty() ? "" : (" (" + std::to_string(s.crew.size()) + " CREW)");
    r.push_back(action("qr_jump_online", ("ONLINE MULTIPLAYER" + crew_badge).c_str(), "[ JUMP ]",
                       "Jump straight to Online Multiplayer (auto-unwinds cleanly)."));
    if (!in_career) {
        r.push_back(action("qr_jump_career", "CAREER MODE", "[ JUMP ]", "Jump straight to Career Free Roam."));
    }
    r.push_back(action("qr_jump_hub", "RETURN TO PLAY HUB", "[ HUB ]",
                       "Exit current session cleanly and return to the Unified PLAY Hub."));
    r.push_back(action("qr_full_menu", "FULL MENU (MODS / GRAPHICS)", "[ OPEN ]",
                       "Open the full tabbed menu for Mods, Graphics, Inputs, and Settings."));
}

void Menu::rows_online(std::vector<Row> &r) {
    game_frontend::Snapshot s;
    if (net_) { net_->viewing_online(); s = net_->snapshot(); }
    using P = game_frontend::Snapshot::Phase;
    const bool can_select_map = s.phase == P::Lobby && s.is_host;
    if (!can_select_map) close_map_picker();
    if (map_return_row_ >= 0) {
        r.push_back(header("SELECT MAP"));
        auto add_map = [&](const std::string &id, const std::string &label, const std::string &help) {
            Row row = action(("map:" + id).c_str(), label.c_str(), "", help.c_str());
            row.kind = RowKind::Check;
            row.checked = id == s.custom_map_id;
            r.push_back(std::move(row));
        };
        add_map("", "RETAIL TRACK", "Use the track selected in the lobby.");
        for (const auto &map : s.custom_maps)
            add_map(map.id, map.title.empty() ? map.id : map.title, map.id);
        if (s.custom_maps.empty())
            r.push_back(info("empty_maps", "NO CUSTOM MAPS", "Place .eodmap files in mods/maps beside eod.ini before hosting."));
        r.push_back(action("map_back", "BACK", "", "Return to the lobby."));
        return;
    }
    const bool connecting = s.phase == P::Connecting || s.phase == P::CheckingCompatibility || s.phase == P::Hosting;
    const bool connected = s.phase != P::Offline && s.phase != P::Disconnected && s.phase != P::Error;
    const bool failed = !connected && !s.error.empty();
    online_dialog_ = connecting ? OnlineDialog::Connecting : failed ? OnlineDialog::Failure : OnlineDialog::None;
    if (connecting || failed) {
        pic_track_.clear();
        pic_car_ = 0;
        text_id_.clear();
        details_ = false;
        if (connecting) {
            r.push_back(info("status", "STATUS", upper(s.status.empty() ? "CONNECTING" : s.status)));
            if (s.can_cancel) r.push_back(action("leave", "CANCEL", "", "Stop connecting and go back to Online."));
        } else {
            Row error = info("error", joined_lobby_ ? "SESSION ENDED" : "COULD NOT CONNECT", s.error);
            error.warn = true;
            r.push_back(error);
            if (s.checking_installed_content) r.push_back(info("profile_search", "CHECKING INSTALLED CONTENT", ""));
            if (s.can_match_content) r.push_back(action("profile_match", "RESTART AND REJOIN", "",
                "Restart with matching content already installed. Your usual profile stays selected for future launches."));
            if (join_attempt_ && (!joined_lobby_ || s.retryable_error)) r.push_back(action("join_retry", joined_lobby_ ? "REJOIN" : "RETRY", "", "Try connecting to the same lobby again."));
            r.push_back(action("join_back", "BACK", "", "Go back to Online."));
        }
        return;
    }
    if (s.phase == P::Lobby || s.phase == P::Racing) joined_lobby_ = true;
    pic_track_ = s.track_pic;
    pic_car_ = s.car_pic;
    Row st = info("status", "CONNECTION", connected ? upper(s.status.empty() ? "CONNECTED" : s.status) : "NOT CONNECTED");
    st.warn = !connected;
    st.good = connected;
    r.push_back(st);
    if (!s.error.empty()) {
        Row e = info("error", "MESSAGE", s.error);
        e.warn = true;
        r.push_back(e);
    }
    r.push_back(header(s.is_host ? "HOST LOBBY" : connected ? "SESSION" : "PLAY ONLINE"));
    if (!s.code.empty()) {
        Row gc = info("gcode", "YOUR GARAGE CODE", s.code, "");
        gc.good = true;
        r.push_back(gc);
    }
    if (connected) {
        r.push_back(header(("PLAYERS (" + std::to_string(s.players.size()) + ")").c_str()));
        if (s.players.empty()) r.push_back(info("noplayers", "PLAYERS", "WAITING FOR LOBBY"));
        for (const auto &p : s.players) {
            std::string pstate = p.waiting ? "NEXT EVENT"
                               : s.phase == P::Racing ? "IN RACE"
                               : !p.map_ready ? "SYNCING MAP"
                               : p.ready ? "READY" : "NOT READY";
            Row pr = info("player", ("P" + std::to_string(p.slot + 1) + "  " + upper(p.name)).c_str(),
                          pstate + (p.speaking ? " - SPEAKING" : "") + (p.muted ? " - MUTED" : ""));
            pr.good = p.map_ready && (p.ready || p.speaking);
            pr.warn = !p.map_ready;
            r.push_back(pr);
        }
    }
    if (!s.route.empty()) r.push_back(info("route", "INTERNET LINK", upper(s.route), ""));
    if (!connected) {
        r.push_back(action("host", "HOST GAME", "[ HOST ]", ""));
        Row code = action("code", "JOIN BY CODE", text_id_ == "code" ? text_ + "_" : "[ ENTER CODE ]",
                          "5-character Garage Code.");
        code.kind = RowKind::Text;
        r.push_back(code);
    }
    Row ready = toggle("ready", "READY", s.ready, "", "");
    if (s.phase == P::Lobby && !s.is_host) {
        if (s.map_downloading) {
            ready.enabled = false;
            ready.value = "DOWNLOADING MAP (" + std::to_string(s.map_download_pct) + "%)";
            ready.help = "Downloading and verifying the host's custom map before you can ready up.";
        } else {
            ready.enabled = s.ready || s.car_pic != 0;
            if (!ready.enabled) ready.help = "Choose a drivable car before marking Ready.";
        }
        r.push_back(ready);
    }
    if (s.is_host && (s.phase == P::Racing || s.phase == P::Loading)) {
        r.push_back(action("end_event", "BACK TO LOBBY", s.phase == P::Loading ? "[ CANCEL START ]" : "[ END RACE ]",
                           "Ends the race for everyone. Keeps the lobby open."));
    }
    if (s.can_rejoin_lobby)
        r.push_back(action("withdraw", "RETURN TO LOBBY", "[ LEAVE RACE ]",
                           "Withdraw from this event and keep your place for the next one."));
    const bool guest_in_race = !s.is_host && s.phase == P::Racing;
    Row leave = action("leave", s.is_host ? "CLOSE LOBBY" : guest_in_race ? "LEAVE PARTY" : "LEAVE LOBBY",
                       "[ DISCONNECT ]",
                       guest_in_race ? "Disconnects you. Other players keep racing."
                                     : "");
    if (s.is_host) leave.help = "Disconnects everyone.";
    leave.enabled = connected || s.can_cancel;

    if (!connected && !s.crew.empty()) {
        r.push_back(header("FRIENDS HOSTING NOW"));
        for (const auto &g : s.crew)
            r.push_back(action(("crew:" + g.address).c_str(), upper(g.name).c_str(), "[ JOIN ]  " + std::to_string(g.players) + " PLAYING", ""));
    }
    if (!connected && !s.lan.empty()) {
        r.push_back(header("GAMES ON YOUR NETWORK"));
        for (const auto &g : s.lan)
            r.push_back(action(("lan:" + g.address).c_str(), upper(g.name).c_str(), "[ JOIN ]  " + std::to_string(g.players) + " PLAYING", ""));
    }
    const std::string map_name = s.custom_map_title.empty() ? s.custom_map_id : s.custom_map_title;
    if (can_select_map)
        r.push_back(action("custom_map", "MAP", map_name.empty() ? "RETAIL TRACK" : map_name,
                           "Choose an installed custom map or race on the retail track."));
    else if (connected && !map_name.empty())
        r.push_back(info("custom_map", "MAP", map_name, "Selected by the host."));
    for (const auto &o : s.options) {
        const std::string label = o.id == "rule_turbo" ? turbo_label(cfg_, host_) : o.label;
        if (s.phase == P::Lobby && o.editable) r.push_back(choice(("opt:" + o.id).c_str(), label.c_str(), upper(o.value), "", o.help.c_str()));
        else r.push_back(info(("opt:" + o.id).c_str(), label.c_str(), upper(o.value), o.help.c_str()));
    }
    if (s.phase == P::Lobby) {
        r.push_back(action("open_car_browser", "BROWSE CARS", "[ BY CLASS & MODS ]",
                           "Visual Car Selector sorted by class (Compact, Midsize, Muscle, Special, Trailers & Modded)."));
        r.push_back(action("open_rules_drawer", s.is_host ? "CUSTOMIZE RULES" : "VIEW HOST RULES",
                           s.is_host ? "[ X / CONFIGURE ]" : "[ READ-ONLY ]",
                           s.is_host ? "Tune mode rules, weapons/feats, and physics traits for this lobby."
                                     : "Inspect the host's active mode rules, weapons, and physics settings."));
    }
    if (s.is_host && s.phase == P::Lobby) {
        const bool alone = s.players.size() <= 1;
        Row start = action("start", alone && s.can_start ? "START EVENT ALONE" : "START EVENT", s.can_start ? "[ START ]" : "WAITING",
                           s.can_start ? "" : s.status.c_str());
        start.enabled = s.can_start;
        r.push_back(start);
    }
    if (!connected || s.is_host) {
        r.push_back(action("advanced", "ADVANCED CONNECTION OPTIONS", online_advanced_ ? "[ HIDE ]" : "[ SHOW ]",
                           "Show direct connection details and network options."));
        if (online_advanced_ && !connected) {
            Row addr = action("addr", "JOIN BY ADDRESS", text_id_ == "addr" ? text_ + "_" : "[ ENTER ADDRESS ]",
                              "Example: 192.168.1.20:27015.");
            addr.kind = RowKind::Text;
            r.push_back(addr);
            r.push_back(choice("snapshot_buffer", "REMOTE CAR SMOOTHING", std::to_string(cfg_.snapshot_buffer_ms) + " ms", "",
                               "Longer buffering smooths uneven packet arrivals. Shorter buffering shows recent movement sooner."));
        }
        if (online_advanced_ && !s.invite.empty()) r.push_back(info("invite", "YOUR ADDRESS", s.invite));
    }
    Row nick = action("nickname", "SCREEN NAME", text_id_ == "nickname" ? text_ + "_" :
                      (cfg_.nickname.empty() ? "RANDOM" : cfg_.nickname), "Leave blank for a random name.");
    nick.kind = RowKind::Text;
    r.push_back(nick);
    if (!connected) return;
    Row mods = info("modset", "GAME CONTENT", s.content_problem.empty() ? (s.players.size() <= 1 ? "WAITING FOR PLAYERS" : "MATCHED") : s.content_problem,
                    "Players need matching game content and gameplay mods. Music stays local.");
    mods.warn = !s.content_problem.empty();
    r.push_back(mods);

    r.push_back(header("VOICE"));
    incoming_voice_rows(r, cfg_);
    Row mic = toggle("mic", "MICROPHONE", s.microphone_enabled, "LIVE", "");
    mic.enabled = connected && s.voice;
    if (s.microphone_enabled && !s.microphone_capture) {
        mic.value = s.deafened ? "DEAFENED" : "NO INPUT";
        mic.help = s.deafened ? "Turn off Deafen to use your microphone."
                             : "Check microphone permission and the selected input device.";
    }
    r.push_back(mic);
    Row mute = toggle("mute", "MUTE OTHERS", s.muted, "LIVE", "");
    mute.enabled = connected && s.voice;
    r.push_back(mute);
    Row deaf = toggle("deafen", "DEAFEN", s.deafened, "LIVE", "Mutes your microphone and incoming voice.");
    deaf.enabled = connected && s.voice;
    r.push_back(deaf);
    if (s.is_host) {
        r.push_back(header("MANAGE PLAYERS"));
        for (const auto &player : s.players) if (player.slot) {
            const auto id = "kick:" + std::to_string(player.slot) + ":" + std::to_string(player.identity);
            r.push_back(action(id.c_str(), ("REMOVE " + upper(player.name)).c_str(),
                               remove_player_ == id ? "[ CONFIRM REMOVE ]" : "[ REMOVE ]",
                               "Disconnects this player."));
        }
    }
    if (connected || s.can_cancel) r.push_back(leave);
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
    if (error_fatal_) {  // a game panic: one plain choice (the id stays "quit"; the caller restarts the game)
        r.push_back(action("quit", "RETURN TO TITLE SCREEN", "", "Starts the game again from the title screen."));
        return;
    }
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
        if (host_.legacy_modern_available && host_.plume_look == PlumeLook::Unavailable) {
            cfg_.modern_shadows = d.modern_shadows; cfg_.modern_soft_shadows = d.modern_soft_shadows;
            cfg_.modern_reflections = d.modern_reflections; cfg_.modern_ao = d.modern_ao; cfg_.modern_bloom = d.modern_bloom;
            cfg_.modern_vignette = d.modern_vignette; cfg_.modern_exposure = d.modern_exposure;
            cfg_.modern_haze = d.modern_haze; cfg_.modern_grade = d.modern_grade;
        }
        break;
    case Tab::Settings:
        cfg_.audio = d.audio; cfg_.volume = d.volume; cfg_.music_volume = d.music_volume; cfg_.voice_volume = d.voice_volume; cfg_.lower_game_during_voice = d.lower_game_during_voice; cfg_.pause_in_menu = d.pause_in_menu; cfg_.unlock_all = d.unlock_all;
        cfg_.career_cheats = d.career_cheats; cfg_.battle_bots = d.battle_bots; cfg_.sp_battle_mode = d.sp_battle_mode;
        cfg_.manual_recovery = d.manual_recovery; cfg_.car_durability = d.car_durability; cfg_.cheat_turbo = d.cheat_turbo;
        cfg_.cheat_chickens = d.cheat_chickens; cfg_.cheat_invincible = d.cheat_invincible;
        cfg_.turbo_scale = d.turbo_scale; cfg_.chicken_reload = d.chicken_reload;
        cfg_.chicken_force_scale = d.chicken_force_scale; cfg_.chicken_radius_scale = d.chicken_radius_scale;
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
        cfg_.career_money = d.career_money; cfg_.career_rank = d.career_rank; cfg_.career_open = d.career_open;
        cfg_.career_all_cars = d.career_all_cars; cfg_.career_free_swap = d.career_free_swap;
        cfg_.career_killable = d.career_killable;
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
    if (id == "plume_look") {
        fx.toggle_plume_look = true;
        fx.sound = Sound::Move;
        return;
    }
    auto step = [&](int &v, int lo, int hi, int by) { v = std::clamp(v + dir * by, lo, hi); };
    // cheat tuning moves in quarter steps, the same units a lobby shares
    auto stepq = [&](float &v, float lo, float hi) { v = std::clamp(std::round(v * 4.0f + (dir >= 0 ? 1 : -1)) / 4.0f, lo, hi); };
    auto cycle = [&](const char *const *list, int n, std::string &v) { v = list[(index_of(list, n, v) + dir + n) % n]; };
    if (id == "scale") {
#ifdef _WIN32
        cfg_.render_scale = std::clamp(cfg_.render_scale + dir, 1, kMaxRenderScale);
#else
        const int count = kMaxRenderScale + 1;
        cfg_.render_scale = (cfg_.render_scale + dir + count) % count;
#endif
    } else if (id == "aspect") cycle(kAspects, 6, cfg_.aspect);
    else if (id == "fullscreen") { cfg_.fullscreen = !cfg_.fullscreen; fx.fullscreen = true; }
    else if (id == "vsync") cfg_.vsync = !cfg_.vsync;
    else if (id == "smooth") cfg_.smooth_display = !cfg_.smooth_display;
    else if (id == "frameskip") cfg_.frame_skip = !cfg_.frame_skip;
    else if (id == "detail") cycle(kDetails, 3, cfg_.detail);
    else if (id.compare(0, 4, "opt:") == 0 && net_) { game_frontend::Command c{}; c.kind = game_frontend::Command::Kind::Option; c.text = id.substr(4); c.value = (float)dir; net_->submit(c); fx.sound = Sound::Move; return; }
    else if (id == "ready" && net_) { game_frontend::Command c{}; c.kind = game_frontend::Command::Kind::Ready; c.enabled = !row.checked; net_->submit(c); fx.sound = Sound::Move; return; }
    else if (id == "mic" && net_) { game_frontend::Command c{}; c.kind = game_frontend::Command::Kind::MicrophoneEnabled; c.enabled = !row.checked; net_->submit(c); fx.sound = Sound::Move; return; }
    else if (id == "mute" && net_) { game_frontend::Command c{}; c.kind = game_frontend::Command::Kind::Muted; c.enabled = !row.checked; net_->submit(c); fx.sound = Sound::Move; return; }
    else if (id == "deafen" && net_) { game_frontend::Command c{}; c.kind = game_frontend::Command::Kind::Deafened; c.enabled = !row.checked; net_->submit(c); fx.sound = Sound::Move; return; }
    else if (id == "snapshot_buffer") step(cfg_.snapshot_buffer_ms, 60, 140, 20);
    else if (id == "fps") cfg_.show_fps = !cfg_.show_fps;
    else if (id == "modern") cfg_.modern_renderer = !cfg_.modern_renderer;
    else if (id == "m_shadows") step(cfg_.modern_shadows, 0, 150, 10);
    else if (id == "m_soft") cfg_.modern_soft_shadows = !cfg_.modern_soft_shadows;
    else if (id == "m_refl") step(cfg_.modern_reflections, 0, 200, 10);
    else if (id == "m_ao") step(cfg_.modern_ao, 0, 200, 10);
    else if (id == "m_bloom") step(cfg_.modern_bloom, 0, 200, 10);
    else if (id == "m_exposure") step(cfg_.modern_exposure, 50, 200, 5);
    else if (id == "m_haze") step(cfg_.modern_haze, 0, 200, 10);
    else if (id == "m_grade") step(cfg_.modern_grade, 0, 200, 10);
    else if (id == "m_vignette") step(cfg_.modern_vignette, 0, 200, 10);
    else if (id == "audio") { cfg_.audio = !cfg_.audio; fx.audio = true; }
    else if (id == "voicevolume") step(cfg_.voice_volume, 0, 200, 5);
    else if (id == "voicefocus") cfg_.lower_game_during_voice = !cfg_.lower_game_during_voice;
    else if (id == "musicvolume") { step(cfg_.music_volume, 0, 100, 5); fx.audio = true; }
    else if (id == "volume") { step(cfg_.volume, 0, 100, 5); fx.audio = true; }
    else if (id == "pause") cfg_.pause_in_menu = !cfg_.pause_in_menu;
    else if (id == "unified_fe") cfg_.unified_frontend = !cfg_.unified_frontend;
    else if (id == "fast_boot") cfg_.fast_boot = !cfg_.fast_boot;
    else if (id == "skip_memcard") cfg_.skip_memcard = !cfg_.skip_memcard;
    else if (id == "play_session") {
        const int next = ((int)session_kind_ + (dir >= 0 ? 1 : 3)) % 4;
        session_kind_ = (game_frontend::SessionKind)next;
        fx.sound = Sound::Move;
        return;
    } else if (id == "career_warp") {
        career_warp_idx_ = (career_warp_idx_ + (dir >= 0 ? 1 : 7)) % 8;
        fx.sound = Sound::Move;
        return;
    } else if (id == "split_players") {
        split_players_ = 2 + ((split_players_ - 2 + (dir >= 0 ? 1 : 2)) % 3);
        for (int i = 0; i < 4; ++i) split_seats_[i].active = (i < split_players_);
        fx.sound = Sound::Move;
        return;
    } else if (id == "play_mode") {
        const int n = (int)mutator::preset_count();
        offline_preset_ = (uint8_t)(((int)offline_preset_ + (dir >= 0 ? 1 : n - 1)) % n);
        sync_draft_from_preset(offline_preset_);
        fx.sound = Sound::Move;
        return;
    } else if (id == "play_track") {
        std::vector<std::string> tracks;
        if (net_) tracks = net_->snapshot().retail_tracks;
        if (tracks.empty()) tracks = {"Anoka", "Bemidji", "Dennison", "Hastings", "HomeBase", "Mankato", "Springfield", "Welch", "Woodly"};
        int idx = 0;
        for (size_t i = 0; i < tracks.size(); ++i) if (tracks[i] == offline_track_) { idx = (int)i; break; }
        offline_track_ = tracks[(idx + (dir >= 0 ? 1 : (int)tracks.size() - 1)) % tracks.size()];
        offline_custom_map_.clear();
        fx.sound = Sound::Move;
        return;
    } else if (id.compare(0, 4, "seat") == 0 && id.size() >= 9 && id.compare(5, 4, "_car") == 0) {
        const int seat = std::clamp(id[4] - '0', 0, 3);
        std::vector<std::string> cars;
        if (net_) cars = net_->snapshot().retail_cars;
        if (cars.empty()) cars = {"Gladiator", "Outlaw", "Bulldog", "Bus", "Phantom", "Algar", "Hearse", "Raven"};
        int idx = 0;
        for (size_t i = 0; i < cars.size(); ++i) if (cars[i] == split_seats_[seat].car_model) { idx = (int)i; break; }
        split_seats_[seat].car_model = cars[(idx + (dir >= 0 ? 1 : (int)cars.size() - 1)) % cars.size()];
        split_seats_[seat].vehicle_num = 0;
        fx.sound = Sound::Move;
        return;
    } else if (id == "seat0_paint") {
        split_seats_[0].paint = (split_seats_[0].paint + (dir >= 0 ? 1 : 5)) % 6;
        fx.sound = Sound::Move;
        return;
    } else if (id == "play_laps") {
        offline_laps_ = std::clamp(offline_laps_ + dir, 1, 50);
        fx.sound = Sound::Move;
        return;
    } else if (id == "play_ai") {
        offline_ai_bots_ = std::clamp(offline_ai_bots_ + dir, -1, 15);
        fx.sound = Sound::Move;
        return;
    } else if (id == "rules_subtab") {
        rules_tab_ = (rules_tab_ + (dir >= 0 ? 1 : 3)) % 4;
        fx.sound = Sound::Tab;
        return;
    } else if (id.compare(0, 6, "draft_") == 0) {
        if (net_) {
            const auto snap = net_->snapshot();
            using P = game_frontend::Snapshot::Phase;
            if (snap.phase != P::Offline && snap.phase != P::Disconnected && snap.phase != P::Error && !snap.is_host) {
                fx.sound = Sound::Error;
                return;
            }
        }
        if (id == "draft_preset") {
            const int n = (int)mutator::preset_count();
            const uint8_t next = (uint8_t)(((int)mutator_draft_.preset_id + (dir >= 0 ? 1 : n - 1)) % n);
            offline_preset_ = next;
            sync_draft_from_preset(next);
            push_mutator_draft();
            fx.sound = Sound::Move;
            return;
        } else if (id == "draft_fuse") { mutator_draft_.tips_fuse_sec = std::clamp(mutator_draft_.tips_fuse_sec + dir * 5.0f, 5.0f, 120.0f); push_mutator_draft(); fx.sound = Sound::Move; return; }
        else if (id == "draft_storm_r") { mutator_draft_.storm_radius_m = std::clamp(mutator_draft_.storm_radius_m + dir * 10.0f, 20.0f, 250.0f); push_mutator_draft(); fx.sound = Sound::Move; return; }
        else if (id == "draft_storm_s") { mutator_draft_.storm_shrink_mps = std::clamp(mutator_draft_.storm_shrink_mps + dir * 0.25f, 0.0f, 10.0f); push_mutator_draft(); fx.sound = Sound::Move; return; }
        else if (id == "draft_storm_dps") { mutator_draft_.storm_dps = std::clamp(mutator_draft_.storm_dps + dir * 0.02f, 0.0f, 1.0f); push_mutator_draft(); fx.sound = Sound::Move; return; }
        else if (id == "draft_fiesta") { mutator_draft_.fiesta_interval_sec = std::clamp(mutator_draft_.fiesta_interval_sec + dir * 5.0f, 5.0f, 120.0f); push_mutator_draft(); fx.sound = Sound::Move; return; }
        else if (id == "draft_shuffle") { mutator_draft_.shuffle_rounds = !mutator_draft_.shuffle_rounds; push_mutator_draft(); fx.sound = Sound::Move; return; }
        else if (id == "draft_turbo") { mutator_draft_.feat_bits ^= mutator::FB_TURBO; push_mutator_draft(); fx.sound = Sound::Move; return; }
        else if (id == "draft_chickens") { mutator_draft_.feat_bits ^= mutator::FB_CHICKEN_STRAIGHT; push_mutator_draft(); fx.sound = Sound::Move; return; }
        else if (id == "draft_chicken_reload") { mutator_draft_.chicken_reload_sec = std::clamp(mutator_draft_.chicken_reload_sec + dir * 0.25f, 0.25f, 10.0f); push_mutator_draft(); fx.sound = Sound::Move; return; }
        else if (id == "draft_chicken_force") { mutator_draft_.chicken_force_scale = std::clamp(mutator_draft_.chicken_force_scale + dir * 0.25f, 0.25f, 8.0f); push_mutator_draft(); fx.sound = Sound::Move; return; }
        else if (id == "draft_ohk") { mutator_draft_.feat_bits ^= mutator::FB_ONE_HIT_KILLS; push_mutator_draft(); fx.sound = Sound::Move; return; }
        else if (id == "draft_deadly") { mutator_draft_.feat_bits ^= mutator::FB_DEADLY_AMBITION; push_mutator_draft(); fx.sound = Sound::Move; return; }
        else if (id == "draft_invincible") { mutator_draft_.feat_bits ^= mutator::FB_INVINCIBLE; push_mutator_draft(); fx.sound = Sound::Move; return; }
        else if (id == "draft_grav") { mutator_draft_.gravity_scale = std::clamp(mutator_draft_.gravity_scale + dir * 0.10f, 0.10f, 4.0f); push_mutator_draft(); fx.sound = Sound::Move; return; }
        else if (id == "draft_mass") { mutator_draft_.mass_scale = std::clamp(mutator_draft_.mass_scale + dir * 0.15f, 0.25f, 5.0f); push_mutator_draft(); fx.sound = Sound::Move; return; }
        else if (id == "draft_grip") { mutator_draft_.grip_scale = std::clamp(mutator_draft_.grip_scale + dir * 0.10f, 0.20f, 3.0f); push_mutator_draft(); fx.sound = Sound::Move; return; }
        else if (id == "draft_torque") { mutator_draft_.torque_scale = std::clamp(mutator_draft_.torque_scale + dir * 0.10f, 0.25f, 3.0f); push_mutator_draft(); fx.sound = Sound::Move; return; }
        else if (id == "draft_speed") { mutator_draft_.top_speed_scale = std::clamp(mutator_draft_.top_speed_scale + dir * 0.10f, 0.40f, 3.0f); push_mutator_draft(); fx.sound = Sound::Move; return; }
        else if (id == "draft_dmg") { mutator_draft_.damage_taken_scale = std::clamp(mutator_draft_.damage_taken_scale + dir * 0.10f, 0.0f, 5.0f); push_mutator_draft(); fx.sound = Sound::Move; return; }
        return;
    }
    else if (id == "unlock") cfg_.unlock_all = !cfg_.unlock_all;
    else if (id == "career_cheats") cfg_.career_cheats = !cfg_.career_cheats;
    else if (id == "bots") cfg_.battle_bots = !cfg_.battle_bots;
    else if (id == "spbattle") cfg_.sp_battle_mode = !cfg_.sp_battle_mode;
    else if (id == "offline_gametype") {
        const int n = static_cast<int>(mutator::preset_count());
        cfg_.offline_gametype = (std::clamp(cfg_.offline_gametype, 0, n - 1) + (dir >= 0 ? 1 : n - 1)) % n;
    }
    else if (id == "offline_ai_bots") {
        cfg_.offline_ai_bots = (std::clamp(cfg_.offline_ai_bots, 0, 15) + (dir >= 0 ? 1 : 15)) % 16;
    }
    else if (id == "recovery") cfg_.manual_recovery = !cfg_.manual_recovery;
    else if (id == "durability") cfg_.car_durability = !cfg_.car_durability;
    else if (id == "turbo") cfg_.cheat_turbo = !cfg_.cheat_turbo;
    else if (id == "chickens") cfg_.cheat_chickens = !cfg_.cheat_chickens;
    else if (id == "turbo_scale") stepq(cfg_.turbo_scale, 1.0f, 4.0f);
    else if (id == "chicken_reload") stepq(cfg_.chicken_reload, 0.25f, 10.0f);
    else if (id == "chicken_force") stepq(cfg_.chicken_force_scale, 0.25f, 8.0f);
    else if (id == "chicken_size") stepq(cfg_.chicken_radius_scale, 0.25f, 4.0f);
    else if (id == "invincible") cfg_.cheat_invincible = !cfg_.cheat_invincible;
    else if (id == "vibration") cfg_.vibration = !cfg_.vibration;
    else if (id == "cu_money") cfg_.career_money = !cfg_.career_money;
    else if (id == "cu_rank") cfg_.career_rank = !cfg_.career_rank;
    else if (id == "cu_open") cfg_.career_open = !cfg_.career_open;
    else if (id == "cu_cars") cfg_.career_all_cars = !cfg_.career_all_cars;
    else if (id == "cu_swap") cfg_.career_free_swap = !cfg_.career_free_swap;
    else if (id == "cu_kill") cfg_.career_killable = !cfg_.career_killable;
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
    if (id == "custom_map" && row.kind == RowKind::Action) {
        map_return_row_ = selected();
        sel_[int(Tab::Play)] = 1;
        scroll_[int(Tab::Play)] = 0;
        return;
    }
    if (id == "open_mode_browser") { set_sub_screen(SubScreen::ModeBrowser); return; }
    if (id == "open_map_browser") { set_sub_screen(SubScreen::MapBrowser); return; }
    if (id == "open_car_browser") { set_sub_screen(SubScreen::CarBrowser); return; }
    if (id == "open_rules_drawer") { set_sub_screen(SubScreen::RulesDrawer); return; }
    if (id == "rules_back") { set_sub_screen(SubScreen::None); fx.sound = Sound::Back; return; }
    if (id == "draft_save_json" && net_) {
        push_mutator_draft();
        game_frontend::Command sc{};
        sc.kind = game_frontend::Command::Kind::SaveCustomGametype;
        sc.draft = mutator_draft_;
        net_->submit(std::move(sc));
        set_status("Saved custom gametype to mods/gametypes/");
        return;
    }
    if (id == "launch_solo") { launch_offline_match(false); fx.close = true; return; }
    if (id == "launch_split") { launch_offline_match(true); fx.close = true; return; }
    if (id == "career_continue") { launch_career(false, false); fx.close = true; return; }
    if (id == "career_classic_hub") { launch_career(true, false); fx.close = true; return; }
    if (id == "career_new") { launch_career(false, true); fx.close = true; return; }
    if (id == "classic_ps2_toggle") {
        cfg_.unified_frontend = false;
        dirty_ = true;
        fx.save = true;
        fx.close = true;
        return;
    }
    // Screen D Quick Rail actions
    if (id == "qr_resume") { quick_rail_ = false; fx.close = true; return; }
    if (id == "qr_scoreboard") { quick_scoreboard_ = !quick_scoreboard_; return; }
    if (id == "qr_full_menu") { quick_rail_ = false; tab_ = Tab::Mods; return; }
    if (id == "qr_career_save" && net_) {
        net_->submit({game_frontend::Command::Kind::CareerSaveNow});
        set_status("Career saved to Memory Card");
        return;
    }
    if (id.compare(0, 8, "qr_warp_") == 0 && net_) {
        int place = 0;
        if (id == "qr_warp_home") place = 0;
        else if (id == "qr_warp_malt") place = 1;
        else if (id == "qr_warp_eve") place = 2;
        else if (id == "qr_warp_dealer") place = 3;
        else if (id == "qr_warp_garage") place = 4;
        else if (id == "qr_warp_troy") place = 7;
        game_frontend::Command wc{};
        wc.kind = game_frontend::Command::Kind::CareerWarp;
        wc.value = (float)place;
        net_->submit(std::move(wc));
        quick_rail_ = false;
        fx.close = true;
        return;
    }
    if (id == "qr_rematch" && net_) {
        net_->submit({game_frontend::Command::Kind::InstantRematch});
        quick_rail_ = false;
        fx.close = true;
        return;
    }
    if (id == "qr_next_round" && net_) {
        net_->submit({game_frontend::Command::Kind::NextRound});
        quick_rail_ = false;
        fx.close = true;
        return;
    }
    if ((id == "qr_jump_online" || id == "qr_jump_career" || id == "qr_jump_hub") && net_) {
        const auto snap = net_->snapshot();
        const bool in_career = (snap.ee_state == 5 || snap.ee_state == 6 || snap.ee_state == 7);
        game_frontend::TransitionRequest req{};
        if (id == "qr_jump_career") { req.target = 2; req.career_place = career_warp_idx_; }
        else { req.target = 1; } // Hub (and set session kind to Online if qr_jump_online)
        if (id == "qr_jump_online") session_kind_ = game_frontend::SessionKind::Online;
        if (in_career) {
            pending_save_jump_ = req;
            save_prompt_ = true;
            sel_[(int)Tab::Play] = 0;
            return;
        }
        game_frontend::Command jc{};
        jc.kind = game_frontend::Command::Kind::LaunchTransition;
        jc.transition = req;
        net_->submit(std::move(jc));
        quick_rail_ = false;
        if (id == "qr_jump_career") fx.close = true;
        return;
    }
    if ((id == "save_jump_save" || id == "save_jump_discard") && net_) {
        pending_save_jump_.save_career_first = (id == "save_jump_save");
        game_frontend::Command jc{};
        jc.kind = game_frontend::Command::Kind::LaunchTransition;
        jc.transition = pending_save_jump_;
        net_->submit(std::move(jc));
        save_prompt_ = false;
        quick_rail_ = false;
        if (pending_save_jump_.target == 2) fx.close = true;
        return;
    }
    if (id == "save_jump_cancel") { save_prompt_ = false; fx.sound = Sound::Back; return; }
    if (id == "map_back") { close_map_picker(); fx.sound = Sound::Back; return; }
    if (id.compare(0, 4, "map:") == 0 && net_) {
        const auto s = net_->snapshot();
        const auto file = id.substr(4);
        const bool installed = file.empty() || std::any_of(s.custom_maps.begin(), s.custom_maps.end(),
            [&](const auto &map) { return map.id == file; });
        close_map_picker();
        if (!s.is_host || s.phase != game_frontend::Snapshot::Phase::Lobby || !installed) {
            fx.sound = Sound::Error;
            return;
        }
        net_->submit({game_frontend::Command::Kind::SelectCustomMap, file});
        return;
    }
    if (id == "start" && net_) {
        const auto s = net_->snapshot();
        if (!s.is_host || s.phase != game_frontend::Snapshot::Phase::Lobby || !s.can_start) {
            fx.sound = Sound::Error;
            return;
        }
        net_->submit({game_frontend::Command::Kind::Start});
        fx.close = true;
        return;
    }
    if (id == "advanced") { online_advanced_ = !online_advanced_; return; }
    if (id == "join_retry" && net_ && join_attempt_) { submit_join(*join_attempt_); return; }
    if (id == "join_back" || (id == "leave" && online_modal_title())) { cancel_join(); return; }
    if (id.compare(0, 4, "lan:") == 0 && net_) { submit_join({game_frontend::Command::Kind::JoinAddress, id.substr(4)}); return; }
    if (id.compare(0, 5, "crew:") == 0 && net_) { submit_join({game_frontend::Command::Kind::JoinCrew, id.substr(5)}); return; }
    if (id == "host" && net_) { submit_join({game_frontend::Command::Kind::Host}); return; }
    if (id == "code" || id == "addr" || id == "nickname") { text_id_ = id; text_ = id == "nickname" ? cfg_.nickname : ""; return; }
    if (id.compare(0,5,"kick:") == 0 && net_) {
        if (remove_player_ != id) { remove_player_=id; return; }
        c.kind=game_frontend::Command::Kind::Kick; c.text=id.substr(5); net_->submit(c);
        remove_player_.clear(); return;
    }
    if (id == "ready" && net_) { c.kind = game_frontend::Command::Kind::Ready; c.enabled = !row.checked; net_->submit(c); return; }
    if (id == "profile_match" && net_) { net_->submit({game_frontend::Command::Kind::MatchContent}); return; }
    if (id == "end_event" && net_) { c.kind = game_frontend::Command::Kind::EndEvent; net_->submit(c); return; }
    if (id == "withdraw" && net_ && net_->snapshot().can_rejoin_lobby) {
        net_->submit({game_frontend::Command::Kind::RejoinLobby});
        return;
    }
    if (id == "leave" && net_) { joined_lobby_ = false; join_attempt_.reset(); c.kind = game_frontend::Command::Kind::Disconnect; net_->submit(c); return; }
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
    if (online_modal_title()) {
        if (in.back) { cancel_join(); fx.sound = Sound::Back; return fx; }
        if (in.close || in.prev_tab || in.next_tab) return fx;
    }
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
        for (char pc : in.paste) if (pc >= ' ' && pc <= '~' && text_.size() < 64) text_.push_back(pc);
        if (in.erase && !text_.empty()) text_.pop_back();
        if (in.back) { text_id_.clear(); fx.sound = Sound::Back; return fx; }
        if (in.accept || in.close) {
            if (net_ && (!text_.empty() || text_id_ == "nickname")) {
                game_frontend::Command c{};
                c.kind = text_id_ == "nickname" ? game_frontend::Command::Kind::Nickname : text_id_ == "code" ? game_frontend::Command::Kind::JoinCode : game_frontend::Command::Kind::JoinAddress;
                c.text = text_;
                if (c.kind == game_frontend::Command::Kind::Nickname) {
                    dirty_ = true;
                    net_->submit(c);
                } else submit_join(std::move(c));
            }
            text_id_.clear();
            fx.sound = Sound::Select;
        }
        return fx;
    }
    // Screen B: Visual Mode, Map & Car Browser controller navigation
    if (!err && tab_ == Tab::Play && !quick_rail_ &&
        (sub_screen_ == SubScreen::ModeBrowser || sub_screen_ == SubScreen::MapBrowser || sub_screen_ == SubScreen::CarBrowser)) {
        if (in.back || in.close) { sub_screen_ = SubScreen::None; fx.sound = Sound::Back; return fx; }
        const int max_cat = sub_screen_ == SubScreen::CarBrowser ? 7 : sub_screen_ == SubScreen::MapBrowser ? 3 : 4;
        if (in.prev_tab || in.next_tab) {
            browser_cat_ = (browser_cat_ + (in.next_tab ? 1 : max_cat - 1)) % max_cat;
            browser_sel_ = 0;
            fx.sound = Sound::Tab;
            return fx;
        }
        const auto items = browser_items();
        const int n = (int)items.size();
        if (n > 0) {
            browser_sel_ = std::clamp(browser_sel_, 0, n - 1);
            if (in.dx) { browser_sel_ = (browser_sel_ + in.dx + n) % n; fx.sound = Sound::Move; }
            if (in.dy) { browser_sel_ = std::clamp(browser_sel_ + in.dy * 3, 0, n - 1); fx.sound = Sound::Move; }
            if (in.accept) { activate_browser_item(browser_sel_, fx); return fx; }
        }
        return fx;
    }
    // Screen C: Customize Rules Drawer L1/R1 switches sub-tabs (MODE RULES | FEATS & WEAPONS | PHYSICS & SCALE | BOTS & ROLES)
    if (!err && tab_ == Tab::Play && !quick_rail_ && sub_screen_ == SubScreen::RulesDrawer &&
        (in.prev_tab || in.next_tab)) {
        rules_tab_ = (rules_tab_ + (in.next_tab ? 1 : 3)) % 4;
        sel_[(int)Tab::Play] = 0;
        scroll_[(int)Tab::Play] = 0;
        fx.sound = Sound::Tab;
        return fx;
    }
    // mouse: hovering a row selects it (rows are hit-tested by the view and passed via select())
    if (in.close && !err) { fx.close = true; fx.sound = Sound::Back; return fx; }
    if (!err && (in.prev_tab || in.next_tab)) {
        int n = (int)Tab::Count;
        close_map_picker();
        tab_ = (Tab)(((int)tab_ + (in.next_tab ? 1 : -1) + n) % n);
        details_ = false;
        status_.clear();
        fx.sound = Sound::Tab;
        return fx;
    }
    if (in.back) {
        if (save_prompt_) { save_prompt_ = false; fx.sound = Sound::Back; return fx; }
        if (sub_screen_ != SubScreen::None) { sub_screen_ = SubScreen::None; fx.sound = Sound::Back; return fx; }
        if (map_return_row_ >= 0) { close_map_picker(); fx.sound = Sound::Back; return fx; }
        if (!remove_player_.empty()) { remove_player_.clear(); fx.sound=Sound::Back; return fx; }
        if (music_open_) { music_open_ = false; sel_[(int)Tab::Mods] = 0; scroll_[(int)Tab::Mods] = 0; fx.sound = Sound::Back; return fx; }
        if (details_) { details_ = false; fx.sound = Sound::Back; return fx; }
        if (err && browse_) { browse_ = false; sel_[(int)Tab::Count] = 0; fx.sound = Sound::Back; return fx; }
        if (!err) { quick_rail_ = false; fx.close = true; fx.sound = Sound::Back; }
        return fx;
    }
    if (in.dy) { move_selection(in.dy); fx.sound = Sound::Move; details_ = false; }
    if (rows.empty()) return fx;
    rows = rows_for(t);
    sel = std::clamp(sel, 0, (int)rows.size() - 1);
    const Row row = rows[sel];
    if (row.enabled && row.id == "voicevolume" && in.slider_fraction >= 0) {
        const int level = int(std::lround(std::clamp(in.slider_fraction, 0.0f, 1.0f) * 40)) * 5;
        if (cfg_.voice_volume != level) { cfg_.voice_volume = level; dirty_ = true; }
    }
    if (in.dx && row.enabled && (row.kind == RowKind::Choice || row.kind == RowKind::Toggle)) change(row, in.dx, fx);
    if (in.accept && !(in.dx && row.slider_fraction >= 0)) activate(row, fx);
    if (in.details && !err && !online_modal_title()) {
        if (tab_ == Tab::Play && !quick_rail_ && sub_screen_ == SubScreen::None) {
            set_sub_screen(SubScreen::RulesDrawer);
        } else {
            details_ = !details_;
        }
        fx.sound = Sound::Select;
    }
    if (in.reset && !err && tab_ != Tab::Play) {
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

std::string Menu::copy_text() {
    if (!net_ || tab_ != Tab::Play) return {};
    std::string code = net_->snapshot().code;
    if (!code.empty()) set_status("Garage Code " + code + " copied");
    return code;
}

}  // namespace frontend
