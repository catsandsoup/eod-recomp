#pragma once
// Function-pointer registry the native hooks call into, so hook files (and their unit
// tests) need no link dependency on the netplay session. netplay_session.cpp fills it in.
#include <cstdint>
#include <string>
namespace netplay_hooks {
struct StartInfo { std::string event, track; uint32_t event_id = 0, seed = 0, car = 0; bool host = false; };   // track/car set: direct start
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
