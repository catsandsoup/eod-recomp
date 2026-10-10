// hle_stream.cpp - diagnostics around the stream file system.
#include "core/cpu.h"
#include "core/log.h"
#include "core/memory.h"
#include "funcs.h"

extern "C" {

// StrmFindFile(path): returns the TOC index or -1.
void f_0033D4F8(void) {
    char name[256];
    ee_read_string(arg_u32(0), name, sizeof name);
    f_0033D4F8_orig();
    if (g_ee.r[2].sw[0] < 0) WARN("disc file not found: %s (is the game data complete?)", name);
}

}
