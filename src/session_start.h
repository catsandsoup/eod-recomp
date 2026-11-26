#pragma once
// Direct session start (developer hook for the future in-game front end).
//
// EOD_DIRECT_START="mode=action;track=Anoka;event=Battle (8 AI);car=Phantom;paint=0;humans=1"
// starts a native Action session (GameDoSingle, 0x2A2C98) as soon as the game first
// reaches its front-end loop (0x2A1298), answering only the two native menu screens
// that GameDoSingle opens first (car select 0x2A75E0, track select 0x2C7340) and
// pre-selecting the event in the native Eve tracker (object at 0x3C4BF8, via 0x120BE8).
// Add ;laps=N to override the lap count of the first selected single-player race.
// Everything else (human count, pad map, session description, roster, world load,
// results, return to the native menus) is the unmodified native code.
// With ;humans=N (2..4), GameDoHeadToHead registers the connected local seats.
// The first player gets the requested paint and the other seats use distinct
// paint records of the same model. The head-to-head tracker resolves the event.
// See docs RACE_SETUP_MAP.md.
//
// EOD_DIRECT_START="mode=career;place=dealer" (or ;eve=3) starts a fresh Career straight from boot and
// puts the car at a free-roam place with the game's own Jump To teleport, or runs a Career Eve. The main
// menu is answered with its Career result (15), the Career driver (0x112298) runs natively, its tutorial
// notes are dismissed and its hub answers Drive Around; then src/career_warp.cpp teleports once the
// free-roam Home Driver is running. See docs/CAREER_WARP.md.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

bool session_test_native_lap(); // Headless native-driver integration fixture.

struct SessionRequest {
    std::string mode = "action";   // "action" or "career"
    std::string track;             // Eve track: internal id ("Anoka") or display name
    std::string event;             // contest name ("No-Rules Race", "Battle (8 AI)") or "#<position>"
    std::string car;               // vehicle model name ("Phantom") or vehicle number
    int paint = 0;                 // 0-based paint index among the model's vehicle records
    int humans = 1;                // 1..4: head-to-head split screen when > 1 (GameDoHeadToHead)
    int ai = -1;                   // Battle/Action AI count override (0..15), or -1 for native default
    int laps = -1;                 // 1..50: race lap override, or -1 for native rules
    std::string place;             // career: free-roam place (career_place_index); default home
    int eve = -1;                  // career: run this Career Eve (index) as soon as free roam starts
    // Per-seat split-screen overrides (seats 0..3):
    std::array<uint32_t, 4> seat_vehicles{}; // explicit vehicle numbers (0 = resolve from seat_cars/car)
    std::array<int, 4> seat_paints{};        // 0-based paint index per seat
    std::array<std::string, 4> seat_cars{};  // model name per seat
    uint16_t local_human_mask = 1u;          // bitmask of local human seats
};

// Launch-scoped offline match configuration (cleared on return to MainMenu 0x002A1298).
// Replaces sticky global config fields (offline_gametype / offline_ai_bots).
struct PendingOfflineMatch {
    bool active = false;
    std::string track;
    std::string event;
    std::string custom_map_id;
    std::string custom_gametype_id;
    uint8_t gametype_preset = 0;    // 0 = PRESET_STANDARD
    bool has_custom_mutator = false;
    std::string mutator_json;       // Serialized mutator::GametypeConfig
    int humans = 1;                 // 1..4
    int ai_bots = -1;               // -1 = native default, 0..15 = explicit count
    int laps = -1;                  // -1 = native default, 1..50
    std::array<uint32_t, 4> seat_vehicles{};
    std::array<int, 4> seat_paints{};
    std::array<std::string, 4> seat_cars{};
    uint16_t local_human_mask = 1u;
};

// Resettable Career Free-Roam / Eve warp state (reusable from Hub and Pause Rail).
struct PendingCareerWarp {
    bool active = false;
    int target_place = -1;          // 0..7 (kCareerPlaces), -1 = none
    int target_eve = -1;            // >= 0 to enter Career Eve, -1 = none
    int delay_frames = 30;
    int frames_elapsed = 0;
};

// Cross-mode transition targets for the Any-State Unwind & Transition Orchestrator.
enum class TransitionTarget : uint8_t {
    None = 0,
    Hub,
    Career,
    Solo,
    SplitScreen,
    OnlineHost,
    OnlineJoinCode,
    OnlineJoinAddress,
    OnlineJoinCrew,
};

struct SessionTransition {
    TransitionTarget target = TransitionTarget::None;
    PendingOfflineMatch offline{};
    PendingCareerWarp career_warp{};
    bool save_career_first = false;
    bool new_career = false;
    bool classic_career_hub = false;
    std::string online_arg;
};

enum class EEStateKind : uint8_t {
    Boot = 0,
    MainMenu,
    SoloAction,
    SplitScreen,
    OnlineMatch,
    CareerHub,
    CareerRoam,
    CareerEve,
};

// Career free-roam places, in the game's Jump To order (teleport.lyt items, area index for 0x16F488).
// Accepts the game's area ids and plain names ("dealer", "malt shop", "pete's"). -1 when unknown.
int career_place_index(const std::string &name);
const char *career_place_name(int index);     // plain name for logs ("dealer (junkyard)")
constexpr int kCareerPlaces = 8;

// The career request of this run (mode=career), or nullptr. Read by src/career_warp.cpp.
const SessionRequest *career_warp_request();
// Resettable Career warp API.
void career_warp_arm(int place, int eve = -1, int delay_frames = 30);
void career_warp_clear();
PendingCareerWarp &career_warp_state();

// Launch-scoped offline match API.
void session_set_pending_offline(const PendingOfflineMatch &m);
const PendingOfflineMatch &session_pending_offline();
void session_clear_pending_offline();

// Any-State Unwind & Transition Orchestrator API.
void session_request_transition(const SessionTransition &t);
void session_cancel_transition();
bool session_transition_pending();
bool session_unwind_active();
EEStateKind session_ee_state();
void session_set_online_transition_handler(void (*fn)(const SessionTransition &));
void session_set_hub_arrival_handler(void (*fn)());

// Parses "k=v;k=v". Returns false and sets err for unknown keys / bad numbers.
bool session_parse(const char *text, SessionRequest &out, std::string &err);
// Canonical contest name for the request ("Battle (8 AI)" for event=Battle;ai=8).
std::string session_event_name(const SessionRequest &r);
// Explicit AI count override from EOD_DIRECT_START (0..12), or -1 if unspecified.
int session_ai_override();

// Catalog of what a direct start can pick, read from the guest's tables (Eves 0x3C4D08, vehicle
// records 0x3CBEE4). refresh() calls guest code, so it must run in guest context (the pad-read hook
// while the main menu is up). Used by the online lobby's TRACK / MODE / CAR / PAINT rows.
namespace session_catalog {
inline constexpr char kHumanBattle[] = "Battle (1v1, humans only)";
struct Track {
    int eve = 0;
    std::string name;
    std::string human_battle_contest;  // native Battle template for the online duel
    std::vector<std::string> events;
    std::vector<uint32_t> event_types;
};
struct Vehicle {
    uint32_t number = 0;
    std::string name;
    uint32_t record = 0;
    bool is_trailer = false;
    uint32_t car_class = 0; // 0=Compact, 1=Midsize, 2=Muscle, 3=Special, 4=Stock Car (NASCAR)
    bool is_modded = false;
};
bool refresh();                                   // false while the tables are not loaded yet
bool ready();
const std::vector<Track> &tracks();
const Track *track_for_arena(const std::string &arena);
const std::vector<Vehicle> &vehicles();
std::vector<std::string> models();                // distinct vehicle names in table order
std::vector<uint32_t> paints(const std::string &model);   // vehicle numbers of a model
bool is_online_driver_choice(const Vehicle &vehicle);
bool online_vehicle_is_drivable(uint32_t number);
void dump();                                      // developer: EOD_CATALOG_DUMP=1
}

extern "C" void f_002BEC08(void);   // title screen: skipped when a direct start is requested
extern "C" void f_002A1298(void);   // main menu: the session starts when it is first reached
extern "C" void f_002A75E0(void);   // GameDoSingle car-select screen
extern "C" void f_002C7340(void);   // GameDoSingle track-select screen
extern "C" void f_00120650(void);   // per-event setup: pre-select the event
extern "C" void f_00120FB8(void);   // EveHasMoreEvents: returns 0 after an online event completes

// Aborts the active race/contest loop cleanly on the EE thread so GameDoSingle unwinds to 0x2A1298.
void session_abort_event();
