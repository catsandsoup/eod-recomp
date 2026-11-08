#pragma once
// Function-pointer registry the native hooks call into, so hook files (and their unit
// tests) need no link dependency on the netplay session. netplay_session.cpp fills it in.
#include <cstdint>
#include <string>
namespace netplay_hooks {
enum RaceRule { Turbo = 1, Chickens = 2, Invincible = 4, Durability = 8, ManualRecovery = 16 };
inline int (*race_rules)() = nullptr; // -1 uses personal offline settings
inline bool rule(RaceRule flag, bool offline) {
    const int rules = race_rules ? race_rules() : -1;
    return rules < 0 ? offline : (rules & flag) != 0;
}
inline float race_scale(float personal, float shared) {
    return race_rules && race_rules() >= 0 ? shared : personal;
}
// Normalize the native cheat word online after applying the shared lobby rules.
// Offline combo remnants (rain, vengeance/hardcore, master) cannot survive room
// entry. Battle's native weapon remains available without the optional mod.
inline uint32_t online_cheats(uint32_t native, bool battle, bool turbo, bool chickens) {
    native &= ~0xA7u; // master, turbo, hardcore, AI vengeance, raining chickens
    if (!battle) native &= ~0x08u;
    if (turbo) native |= 0x03u;
    if (chickens) native = (native | 0x09u) & ~0x90u;
    if (battle && (native & 0x08u)) native |= 0x01u;
    return native;
}
struct StartInfo { std::string event, track; uint32_t event_id = 0, seed = 0, car = 0; bool host = false; };   // track/car set: direct start
inline bool title_screen = false;
inline void (*menu_enter)() = nullptr;                        // the native main menu loop starts / ends
inline void (*menu_leave)() = nullptr;
inline void (*arm)(const StartInfo &) = nullptr;              // session_start: the next GameDoSingle is an online event
inline bool (*take_start)(StartInfo &) = nullptr;
inline void (*car_chosen)(uint32_t) = nullptr;
inline void (*track_chosen)(const std::string &) = nullptr;
inline bool (*wait_track)(std::string &) = nullptr;
inline void (*event_ended)() = nullptr;
inline bool (*event_active)() = nullptr;
inline bool (*active)() = nullptr;
inline bool (*is_host)() = nullptr;
inline uint32_t (*remote_humans)() = nullptr;
inline void (*roster_pre)(uint32_t) = nullptr;
inline void (*roster_post)(uint32_t) = nullptr;
}  // namespace netplay_hooks
