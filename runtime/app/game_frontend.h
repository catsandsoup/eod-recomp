#pragma once
#include "disc_import.h"
#include "../platform/platform.h"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace game_frontend {

enum class SessionKind : uint8_t { Career = 0, Solo = 1, SplitScreen = 2, Online = 3 };

struct Player {
    unsigned slot = 0;
    std::string name;
    bool ready = false, muted = false, deafened = false, speaking = false;
    uint64_t identity = 0;
    bool waiting = false;
    bool map_ready = true;
};

struct Option {
    std::string id, label, value;
    bool editable = false;
    std::string help;
};

struct LanGame {
    std::string name, address;
    unsigned players = 0;
};

struct CustomMapOption {
    std::string id, title;
    std::string base_arena;
    std::string author;
};

struct CarEntry {
    std::string model;
    std::string category; // "COMPACT", "MIDSIZE", "MUSCLE", "SPECIAL", "TRAILERS", "MODDED"
    uint32_t car_class = 0;
    uint32_t vehicle_num = 0; // representative vehicle number for ui_preview::car
    int paint_count = 1;
    bool is_trailer = false;
    bool is_modded = false;
};

struct CareerSummary {
    bool loaded = false;
    int rank = 100;
    int money = 0;
    int rep = 0;
    int events_done = 0;
    int events_total = 40;
    int garage_count = 0;
    int garage_cap = 4;
    uint32_t active_car_num = 0;
    std::string active_car_name;
};

struct SplitSeatConfig {
    bool active = false;
    int pad_port = -1;
    std::string car_model;
    int paint = 0;
    uint32_t vehicle_num = 0;
    std::string device_label;
};

struct LobbySlot {
    uint8_t slot = 0;
    bool occupied = false;
    bool is_ai = false;
    bool is_host = false;
    bool ready = false;
    bool speaking = false;
    bool muted = false;
    bool waiting = false;
    bool map_ready = true;
    std::string name;
    std::string car_name;
    uint32_t vehicle_num = 0;
    uint64_t identity = 0;
};

struct GameModeEntry {
    std::string id;
    std::string name;
    std::string category;     // "RETAIL", "PARTY & ARENA", "MY CUSTOM MODES"
    std::string description;
    std::string default_track;
    uint8_t preset_id = 0;
    bool is_custom_json = false;
    bool uses_weapons = true;
    bool featured = false;
};

struct MutatorDraft {
    uint8_t preset_id = 0;
    std::string id;
    std::string name;
    uint32_t rule_flags = 0;
    uint32_t feat_bits = 0;
    float tips_fuse_sec = 25.0f;
    float storm_radius_m = 95.0f;
    float storm_shrink_mps = 1.15f;
    float storm_dps = 0.14f;
    float fiesta_interval_sec = 20.0f;
    bool shuffle_rounds = false;
    uint8_t rounds_to_win = 5;
    float gravity_scale = 1.0f;
    float mass_scale = 1.0f;
    float grip_scale = 1.0f;
    float torque_scale = 1.0f;
    float top_speed_scale = 1.0f;
    float damage_taken_scale = 1.0f;
    float chicken_force_scale = 1.0f;
    float chicken_radius_scale = 1.0f;
    float chicken_reload_sec = 3.0f;
    bool uses_weapons = true;
    std::string custom_json;
    std::vector<std::string> summary_lines;
};

struct ScoreboardRow {
    uint8_t slot = 0;
    std::string name;
    std::string role;
    int32_t score = 0;
    bool alive = true;
    bool is_tagger = false;
    float fuse_sec = 0.0f;
};

struct TransitionRequest {
    int target = 0; // Mirrors TransitionTarget enum
    std::string track;
    std::string event;
    std::string custom_map_id;
    uint8_t gametype_preset = 0;
    std::string mutator_json;
    int humans = 1;
    int ai_bots = -1;
    int laps = -1;
    std::array<uint32_t, 4> seat_vehicles{};
    std::array<int, 4> seat_paints{};
    std::array<std::string, 4> seat_cars{};
    int career_place = -1;
    int career_eve = -1;
    bool save_career_first = false;
    bool new_career = false;
    bool classic_career_hub = false;
    std::string online_arg;
};

struct Snapshot {
    enum class Phase {
        Offline, Hosting, Connecting, CheckingCompatibility, Lobby, Loading,
        WaitingForStart, Racing, Results, Disconnected, Error
    };
    Phase phase = Phase::Offline;
    bool direct_connect = false, internet_invites = false, native_race_sync = false, voice = false, microphone_capture = false;
    bool can_match_content = false, checking_installed_content = false;
    bool retryable_error = false;
    uint64_t generation = 0;
    enum class Microphone { Unavailable, NotRequested, Requesting, Granted, Denied, DeviceLost };
    Microphone microphone = Microphone::Unavailable;
    bool can_cancel = false, can_start = false, can_rematch = false, can_rejoin_lobby = false;
    bool is_host = false, ready = false, muted = false, deafened = false, microphone_enabled = false;
    std::string status, error, content_problem, invite, address;
    std::vector<Player> players;
    std::vector<Option> options;
    std::vector<CustomMapOption> custom_maps;     // installed host/local choices
    std::string custom_map_id, custom_map_title;  // empty selection uses the retail track
    bool map_downloading = false;
    int map_download_pct = 0;
    bool map_verified = true;
    int lobby_ai_count = 0;
    int lobby_capacity = 16;
    std::vector<LanGame> lan;
    std::string strip;          // short text shown over the game while players choose
    std::string track_pic;      // lobby: the track whose picture to show (display name)
    uint32_t car_pic = 0;       // lobby: the vehicle number whose portrait to show
    std::string code;           // host: the Garage Code to share (Cmd+C copies it)
    std::string route;          // plain-words state of the internet link
    std::vector<LanGame> crew;  // remembered friends who are hosting right now

    // Unified Front-End persistent catalog & live state (Phases 2-5)
    bool catalog_ready = false;
    int ee_state = 0; // Mirrors EEStateKind
    std::vector<std::string> retail_tracks;
    std::vector<std::string> retail_cars;
    std::vector<CarEntry> car_catalog;
    std::vector<GameModeEntry> modes;
    CareerSummary career;
    MutatorDraft mutator_draft;
    std::vector<LobbySlot> roster_16;
    std::vector<ScoreboardRow> scoreboard;
};

struct Command {
    enum class Kind {
        Host, JoinAddress, JoinCode, Ready, Start, EndEvent, Cancel, Disconnect, Rematch, RejoinLobby,
        MicrophoneEnabled, Muted, Deafened, VoiceVolume, Option, JoinCrew, Nickname, Kick, MatchContent,
        SelectCustomMap,
        // Unified Front-End commands (Phases 2-5)
        LaunchTransition, UpdateMutatorDraft, SaveCustomGametype,
        CareerWarp, CareerSaveNow, NextRound, InstantRematch, ClassicPS2Menus
    };
    Kind kind{};
    std::string text;
    uint16_t port = 0;
    bool enabled = false;
    float value = 0;
    TransitionRequest transition{};
    MutatorDraft draft{};
};

class Network {
public:
    virtual ~Network() = default;
    virtual Snapshot snapshot() const = 0; // Thread-safe copy, no blocking UI/network waits.
    virtual void viewing_online() {}
    virtual void submit(Command command) = 0;
};

void set_network(std::shared_ptr<Network> network);
void set_settings_applied(std::function<void()> callback);
bool prepare(std::string &data, const std::string &config, bool headless, const DiscId &id, PlatformConfig &display);
void show_menu();
void tick();
bool captures_input();
bool paused();
bool controller_first();

}  // namespace game_frontend
