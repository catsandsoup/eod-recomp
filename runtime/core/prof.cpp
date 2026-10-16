// prof.cpp - EE / GS thread time by category (see prof.h).
#include "prof.h"

#include <chrono>
#include <cstdio>

std::atomic<uint64_t> g_prof[P_COUNT];
thread_local ProfThread t_prof;

void prof_report(char *ee, int ee_size, char *gs, int gs_size) {
    static const char *names[P_COUNT] = {"game code", "VU", "VIF", "GIF", "sound", "movies", "waiting for GS", "idle",
                                         "commands", "state", "draws", "textures", "new textures", "uploads", "readbacks", "conversions",
                                         "present", "idle"};
    uint64_t v[P_COUNT];
    for (int i = 0; i < P_COUNT; i++) v[i] = g_prof[i].exchange(0, std::memory_order_relaxed);
    auto line = [&](char *out, int size, const char *who, int first, int last) {
        uint64_t total = 0;
        for (int i = first; i <= last; i++) total += v[i];
        int n = snprintf(out, (size_t)size, "%s:", who);
        for (int i = first; i <= last && n < size; i++)
            n += snprintf(out + n, (size_t)(size - n), " %s %.0f%%", names[i], total ? 100.0 * v[i] / total : 0.0);
    };
    line(ee, ee_size, "  EE time", P_EE_GAME, P_EE_IDLE);
    line(gs, gs_size, "  GS time", P_GS_CMDS, P_GS_IDLE);
}
