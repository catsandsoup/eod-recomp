// log.h - tiny logging facility for the runtime.
#pragma once
#include <cstdarg>
#include <cstdint>

enum LogCat {
    LOG_CORE,
    LOG_KERNEL,
    LOG_THREAD,
    LOG_HW,
    LOG_DMA,
    LOG_GS,
    LOG_VIF,
    LOG_VU,
    LOG_SIF,
    LOG_IOP,
    LOG_FILE,
    LOG_PAD,
    LOG_MC,
    LOG_SND,
    LOG_GAME,
    LOG_CAT_COUNT
};

extern bool g_log_enabled[LOG_CAT_COUNT];

void log_init(const char *spec);  // e.g. "kernel,sif,file" or "all"
void log_set_file(const char *path);
void log_write(LogCat cat, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
[[noreturn]] void fatal(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

#define LOG(cat, ...)                                   \
    do {                                                \
        if (g_log_enabled[cat]) log_write(cat, __VA_ARGS__); \
    } while (0)

#define WARN(...) log_write(LOG_CORE, __VA_ARGS__)
