// hle_stream.cpp - diagnostics and NVMe turbo-loading hooks around the stream file system.
#include <cstdlib>
#include "core/config.h"
#include "core/cpu.h"
#include "core/eod_tracy.h"
#include "core/log.h"
#include "core/memory.h"
#include "platform/platform.h"
#include "funcs.h"

namespace {
bool env_allows_fast_boot() {
    // Frame-scripted tests (EOD_PAD_SCRIPT) rely on exact PS2 vblank counts.
    static int cached = -1;
    if (cached < 0) {
        const char *slow = std::getenv("EOD_SLOW_LOAD");
        const char *pad_script = std::getenv("EOD_PAD_SCRIPT");
        cached = (!(slow && slow[0] == '1') && !(pad_script && pad_script[0])) ? 1 : 0;
    }
    return cached != 0;
}

bool turbo_loading_enabled() {
    return env_allows_fast_boot() && g_config.fast_boot;
}

bool skip_memcard_delays() {
    return env_allows_fast_boot() && g_config.fast_boot && g_config.skip_memcard;
}

// Rate-limits the 3-vblank per-chunk loading progress redraws. Short NVMe bursts
// (<150 ms) finish without stalling on FrameWaitVblank at all; longer loads still
// redraw every 50 ms so the progress bar stays responsive.
bool should_skip_progress_draw(uint64_t &burst_start_us, uint64_t &last_call_us, uint64_t &last_draw_us) {
    const uint64_t now = platform_time_us();
    if (!last_call_us || now - last_call_us > 250000) {
        burst_start_us = now;
        last_draw_us = now;
    }
    last_call_us = now;
    if (now - burst_start_us < 150000) return true;
    if (now - last_draw_us < 50000) return true;
    last_draw_us = now;
    return false;
}
}  // namespace

extern "C" {

// StrmFindFile(path): returns the TOC index or -1.
void f_0033D4F8(void) {
    EOD_ZONE_SCOPED_N("stream.find_file");
    char name[256];
    ee_read_string(arg_u32(0), name, sizeof name);
    f_0033D4F8_orig();
    if (g_ee.r[2].sw[0] < 0) WARN("disc file not found: %s (is the game data complete?)", name);
}

// Event/race archive loading progress callback (0x002C1A20):
// Native PS2 code renders and waits 3 vblanks (50 ms) after every single stream chunk.
void f_002C1A20(void) {
    if (turbo_loading_enabled()) {
        static uint64_t burst_start_us = 0, last_call_us = 0, last_draw_us = 0;
        if (should_skip_progress_draw(burst_start_us, last_call_us, last_draw_us)) return;
    }
    f_002C1A20_orig();
}

// Front-end/menu archive loading progress callback (0x002C2188):
void f_002C2188(void) {
    if (turbo_loading_enabled()) {
        static uint64_t burst_start_us = 0, last_call_us = 0, last_draw_us = 0;
        if (should_skip_progress_draw(burst_start_us, last_call_us, last_draw_us)) return;
    }
    f_002C2188_orig();
}

// TimerGetMs (0x00337718):
// Skip artificial PS2 minimum-duration wait loops after data is already in RAM:
// - 0x00339DA8: 3,400 ms boot/load memory card progress-bar spin (f_00339D78 -> f_00338850)
// - 0x003359D8 (when SleepMs caller at sp+48 is 0x0033961C): 500-1,000 ms memcard status banner hold (f_003395B0)
// - 0x002C6170: 4,000 ms boot memory card notice wait (f_002C6160)
// - 0x002CBCF0: memory card save/load banner minimum hold timer (f_002CBCD8)
// - 0x00369FA8: 2,000 ms post-load spin in Career/Homebase loading screen (f_00369DB8)
void f_00337718(void) {
    const uint32_t caller = g_ee.r[31].uw[0];
    f_00337718_orig();
    if (caller == 0x00339DA8 || caller == 0x002C6170 || caller == 0x002CBCF0) {
        if (skip_memcard_delays()) {
            g_ee.r[2].ud[0] = (uint64_t)(int64_t)(int32_t)(g_ee.r[2].uw[0] + 10000u);
        }
    } else if (caller == 0x003359D8) {
        const uint32_t sp = g_ee.r[29].uw[0];
        if (skip_memcard_delays() && sp <= EE_RAM_SIZE - 52 && ee_r32(sp + 48) == 0x0033961Cu) {
            g_ee.r[2].ud[0] = (uint64_t)(int64_t)(int32_t)(g_ee.r[2].uw[0] + 5000u);
        }
    } else if (caller == 0x00369FA8) {
        if (turbo_loading_enabled()) {
            g_ee.r[2].ud[0] = (uint64_t)(int64_t)(int32_t)(g_ee.r[2].uw[0] + 5000u);
        }
    }
}

}
