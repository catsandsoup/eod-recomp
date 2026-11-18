// limit_tracker.cpp - see limit_tracker.h.
#include "limit_tracker.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace {

struct Limit {
    uint64_t capacity = 0, ps2_cap = 0;
    uint64_t scene_high = 0, run_high = 0, logged_high = 0;
    bool approached = false, exceeded = false, full = false;
};

std::map<std::string, Limit> &limits() {
    static std::map<std::string, Limit> m;
    return m;
}
std::string g_scene = "boot";
bool g_hooked = false;

void summary(const char *why) {
    for (auto &[name, l] : limits()) {
        if (!l.scene_high) continue;
        std::fprintf(stderr, "[limit] %s '%s': %s high-water %llu (capacity %llu, PS2 cap %llu, run high-water %llu)\n",
                     why, g_scene.c_str(), name.c_str(), (unsigned long long)l.scene_high,
                     (unsigned long long)l.capacity, (unsigned long long)l.ps2_cap, (unsigned long long)l.run_high);
    }
}

void at_exit() { summary("scene end"); }

}  // namespace

void limit_note(const char *name, uint64_t used, uint64_t capacity, uint64_t ps2_cap) {
    if (!g_hooked) g_hooked = true, std::atexit(at_exit);
    Limit &l = limits()[name];
    l.capacity = capacity;
    l.ps2_cap = ps2_cap;
    if (used > l.run_high) l.run_high = used;
    if (ps2_cap && !l.approached && used * 4 >= ps2_cap * 3) {
        l.approached = true;
        std::fprintf(stderr, "[limit] %s: %llu reaches 75%% of the PS2 cap %llu (scene '%s')\n", name,
                     (unsigned long long)used, (unsigned long long)ps2_cap, g_scene.c_str());
    }
    if (ps2_cap && !l.exceeded && used > ps2_cap) {
        l.exceeded = true;
        std::fprintf(stderr, "[limit] %s: %llu is past the PS2 cap %llu (scene '%s')\n", name,
                     (unsigned long long)used, (unsigned long long)ps2_cap, g_scene.c_str());
    }
    if (capacity && !l.full && used >= capacity) {
        l.full = true;
        std::fprintf(stderr, "[limit] %s: %llu fills the current capacity %llu (scene '%s')\n", name,
                     (unsigned long long)used, (unsigned long long)capacity, g_scene.c_str());
    }
    if (used > l.scene_high) {
        l.scene_high = used;
        if (used >= l.logged_high + l.logged_high / 8 + 1) {
            l.logged_high = used;
            std::fprintf(stderr, "[limit] %s: high-water %llu (capacity %llu, PS2 cap %llu, scene '%s')\n", name,
                         (unsigned long long)used, (unsigned long long)capacity, (unsigned long long)ps2_cap,
                         g_scene.c_str());
        }
    }
}

void limit_scene(const char *label) {
    summary("scene end");
    g_scene = label;
    for (auto &[name, l] : limits()) l.scene_high = l.logged_high = 0;
}
