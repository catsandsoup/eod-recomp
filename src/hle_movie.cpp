// hle_movie.cpp - MoviePlay(name): plays data\video\<name>.pss with the
// ezmpegstr player (libmpeg + IPU).  With EOD_SKIP_MOVIES=1 movies are
// skipped the same way the game skips them when it is short of memory.
#include <atomic>
#include <cstdlib>

#include "core/cpu.h"
#include "core/log.h"
#include "core/memory.h"
#include "platform/platform.h"
#include "funcs.h"
#include "movie_bits.h"

// set while a movie plays (shown in 4:3 whatever the game's aspect setting)
std::atomic<bool> g_movie_playing{false};

extern "C" {

void f_0039AFB0_orig(void);
void f_0039AFB0(void) {
    if (!movie_bits_advance_fast()) f_0039AFB0_orig();
}

void f_003670C8(void) {
    static bool skip = getenv("EOD_SKIP_MOVIES") && atoi(getenv("EOD_SKIP_MOVIES"));
    char name[256];
    ee_read_string(arg_u32(0), name, sizeof name);
    if (skip) {
        LOG(LOG_CORE, "skipping movie %s", name);
        g_ee.r[2].sd[0] = 0;
        return;
    }
    LOG(LOG_CORE, "playing movie %s", name);
    g_movie_playing = true;
    uint64_t t0 = platform_time_us();
    f_003670C8_orig();
    g_movie_playing = false;
    LOG(LOG_CORE, "movie %s ended after %.1f s", name, (platform_time_us() - t0) / 1e6);
}

}
