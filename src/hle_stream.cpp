// hle_stream.cpp - diagnostics and NVMe turbo-loading hooks around the stream file system.
#include <cstdlib>
#include "core/cpu.h"
#include "core/log.h"
#include "core/memory.h"
#include "platform/platform.h"
#include "funcs.h"

namespace {
bool turbo_loading_enabled() {
    // Frame-scripted tests (EOD_PAD_SCRIPT) rely on exact PS2 vblank counts.
    // All interactive play and online direct-start run at native NVMe speed.
    static int cached = -1;
    if (cached < 0) {
        const char *slow = std::getenv("EOD_SLOW_LOAD");
        const char *pad_script = std::getenv("EOD_PAD_SCRIPT");
        cached = (!(slow && slow[0] == '1') && !(pad_script && pad_script[0])) ? 1 : 0;
    }
    return cached != 0;
}
}  // namespace

extern "C" {

// StrmFindFile(path): returns the TOC index or -1.
void f_0033D4F8(void) {
    char name[256];
    ee_read_string(arg_u32(0), name, sizeof name);
    f_0033D4F8_orig();
    if (g_ee.r[2].sw[0] < 0) WARN("disc file not found: %s (is the game data complete?)", name);
}

// Event/race archive loading progress callback (0x002C1A20):
// Native PS2 code renders and waits 3 vblanks (50 ms) after every single stream chunk.
// Rate-limit the visual progress redraw to at most once every 50 ms of wall time so
// NVMe reads finish in <0.1s instead of sleeping 2.5-4s in FrameWaitVblank.
void f_002C1A20(void) {
    if (turbo_loading_enabled()) {
        static uint64_t last_draw_us = 0;
        const uint64_t now = platform_time_us();
        if (now - last_draw_us < 50000) return;
        last_draw_us = now;
    }
    f_002C1A20_orig();
}

// Front-end/menu archive loading progress callback (0x002C2188):
void f_002C2188(void) {
    if (turbo_loading_enabled()) {
        static uint64_t last_draw_us = 0;
        const uint64_t now = platform_time_us();
        if (now - last_draw_us < 50000) return;
        last_draw_us = now;
    }
    f_002C2188_orig();
}

// TimerGetMs (0x00337718):
// Skip artificial PS2 minimum-duration wait loops after data is already in RAM:
// - 0x00369FA8: 2,000 ms post-load spin in Career/Homebase loading screen (f_00369DB8)
// - 0x002C6170: 4,000 ms boot memory card notice wait (f_002C6160)
// - 0x002CBCF0: memory card save/load banner minimum hold timer (f_002CBCD8)
void f_00337718(void) {
    const uint32_t caller = g_ee.r[31].uw[0];
    f_00337718_orig();
    if (turbo_loading_enabled()) {
        if (caller == 0x00369FA8 || caller == 0x002C6170 || caller == 0x002CBCF0) {
            g_ee.r[2].ud[0] = (uint64_t)(int64_t)(int32_t)(g_ee.r[2].uw[0] + 5000u);
        }
    }
}

}
