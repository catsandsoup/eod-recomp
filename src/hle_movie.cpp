// hle_movie.cpp - MoviePlay(name): plays data\video\<name>.pss with the
// ezmpegstr player (libmpeg + IPU).  With EOD_SKIP_MOVIES=1 movies are
// skipped the same way the game skips them when it is short of memory.
#include <cstdlib>

#include "core/cpu.h"
#include "core/log.h"
#include "core/memory.h"
#include "funcs.h"

extern "C" {

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
    f_003670C8_orig();
}

}
