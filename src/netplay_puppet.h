#pragma once
// Registry of puppet cars (remote players' cars in this game). Header-only so
// native hooks and their unit tests need no netplay link dependency.
#include <cstdint>
namespace netplay_car {
inline uint32_t g_puppets[16] = {};
inline bool is_puppet(uint32_t car) { for (auto p : g_puppets) if (p && p == car) return true; return false; }
inline void set_puppet(uint32_t car, bool on) {
    for (auto &p : g_puppets) if (p == car) { if (!on) p = 0; return; }
    if (on) for (auto &p : g_puppets) if (!p) { p = car; return; }
}
}  // namespace netplay_car
