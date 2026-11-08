#pragma once
// Direct session start (developer hook for the future in-game front end).
//
// EOD_DIRECT_START="mode=action;track=Anoka;event=Battle (8 AI);car=Phantom;paint=0;humans=1"
// starts a native Action session (GameDoSingle, 0x2A2C98) as soon as the game first
// reaches its front-end loop (0x2A1298), answering only the two native menu screens
// that GameDoSingle opens first (car select 0x2A75E0, track select 0x2C7340) and
// pre-selecting the event in the native Eve tracker (object at 0x3C4BF8, via 0x120BE8).
// Everything else (human count, pad map, session description, roster, world load,
// results, return to the native menus) is the unmodified native code.
// See docs RACE_SETUP_MAP.md.
#include <cstdint>
#include <string>
#include <vector>

struct SessionRequest {
    std::string mode = "action";   // only "action" is implemented
    std::string track;             // Eve track: internal id ("Anoka") or display name
    std::string event;             // contest name ("No-Rules Race", "Battle (8 AI)") or "#<position>"
    std::string car;               // vehicle model name ("Phantom") or vehicle number
    int paint = 0;                 // 0-based paint index among the model's vehicle records
    int humans = 1;                // only 1 is implemented
    int ai = -1;                   // Battle variant helper: event=Battle + ai=8 -> "Battle (8 AI)"
};

// Parses "k=v;k=v". Returns false and sets err for unknown keys / bad numbers.
bool session_parse(const char *text, SessionRequest &out, std::string &err);
// Canonical contest name for the request ("Battle (8 AI)" for event=Battle;ai=8).
std::string session_event_name(const SessionRequest &r);

// Catalog of what a direct start can pick, read from the guest's tables (Eves 0x3C4D08, vehicle
// records 0x3CBEE4). refresh() calls guest code, so it must run in guest context (the pad-read hook
// while the main menu is up). Used by the online lobby's TRACK / MODE / CAR / PAINT rows.
namespace session_catalog {
struct Track { int eve = 0; std::string name; std::vector<std::string> events; };
struct Vehicle { uint32_t number = 0; std::string name; uint32_t record = 0; };
bool refresh();                                   // false while the tables are not loaded yet
bool ready();
const std::vector<Track> &tracks();
const std::vector<Vehicle> &vehicles();
std::vector<std::string> models();                // distinct vehicle names in table order
std::vector<uint32_t> paints(const std::string &model);   // vehicle numbers of a model
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
