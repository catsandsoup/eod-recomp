// hle_log.cpp - hooks that mirror the game's debug output to the log.
#include "core/cpu.h"
#include "core/ee_format.h"
#include "core/log.h"
#include "funcs.h"

extern "C" {

// printf(fmt, ...)
void f_003A60E0(void) {
    std::string s = ee_format_regs(arg_u32(0), 4);
    LOG(LOG_GAME, "%s", s.c_str());
    ret_s32((int32_t)s.size());
}

// The developers' debug printf (it formats into a buffer and discards it in
// the release build): logged with --log game.
void f_00331E48(void) {
    if (!g_log_enabled[LOG_GAME]) return;
    std::string s = ee_format_regs(arg_u32(0), 4);
    LOG(LOG_GAME, "dbg: %s", s.c_str());
}

// panic(fmt, ...): the game's fatal error handler
void f_00331DF0(void) {
    std::string s = ee_format_regs(arg_u32(0), 4);
    cpu_dump_state();
    fatal("game panic: %s", s.c_str());
}

}
