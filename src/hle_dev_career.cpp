// hle_dev_career.cpp - developer shortcut: EOD_CAREER_EVE=<Eve index> runs that Eve of the
// loaded Career (CareerDoEve, 0x2DDC08) right where the Career driver (0x112298) would start the
// free-roam drive (0x2D58E8), as if the player had just driven into the Eve. Once per run; when
// the Eve ends the game carries on with the normal free roam. Off unless the variable is set.
#include "core/cpu.h"
#include "core/memory.h"
#include "funcs.h"

#include <cstdio>
#include <cstdlib>

extern "C" {
void f_002D58E8_orig(void);
}

extern "C" void f_002D58E8(void) {
    static bool done = false;
    const char *text = std::getenv("EOD_CAREER_EVE");
    if (!done && text && *text) {
        done = true;
        const uint32_t eve = static_cast<uint32_t>(std::strtoul(text, nullptr, 0));
        ee_w32(0x004AAFC8, eve);  // chosen Eve (read by 0x2E2F28)
        ee_w32(0x004AAFCC, 1);    // its "chosen" flag (set by 0x2E2F38)
        std::fprintf(stderr, "[CAREER_EVE] entering Eve %u\n", eve);
        const EEState saved = g_ee;
        g_ee.r[4].ud[0] = eve;
        f_002DDC08();
        g_ee = saved;
        std::fprintf(stderr, "[CAREER_EVE] Eve %u returned to the Career driver\n", eve);
    }
    f_002D58E8_orig();
}

// EOD_BATTLE_END_AFTER=<updates>: after that many Battle updates (0x167D58, once per frame) the second participant
// is given the winning score (a lead of 5, which 0x167D58 turns into the finished state), so a headless run
// reaches the end-of-event path without playing a match. Developer only.
extern "C" {
void f_00167D58_orig(void);
}
extern "C" void f_00167D58(void) {
    static long updates = 0;
    static const char *text = std::getenv("EOD_BATTLE_END_AFTER");
    if (text && *text && ee_r32(arg_u32(0) + 4) == 1 && ++updates == std::atol(text)) {
        const uint32_t master = arg_u32(0);
        ee_w32(master + 0x460 + 0x50 * 1, ee_r32(master + 0x468 + 0x50 * 1) + 5);
        std::fprintf(stderr, "[BATTLE_END] gave participant 1 a winning lead after %ld updates\n", updates);
    }
    f_00167D58_orig();
}
