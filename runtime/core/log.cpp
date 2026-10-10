#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../platform/platform.h"

bool g_log_enabled[LOG_CAT_COUNT];

static const char *k_names[LOG_CAT_COUNT] = {
    "core", "kernel", "thread", "hw", "dma", "gs", "vif", "vu",
    "sif", "iop", "file", "pad", "mc", "snd", "game", "gsxfer",
};

static FILE *g_log_file;

// EOD_LOG_FROM=<vblank>: the categories given with --log start at that vblank.
static bool g_log_saved[LOG_CAT_COUNT];
static long long g_log_from = -1;

void log_frame(uint64_t frame) {
    if (g_log_from < 0 || (long long)frame != g_log_from) return;
    for (int i = 0; i < LOG_CAT_COUNT; i++) g_log_enabled[i] = g_log_saved[i];
    g_log_from = -1;
}

void log_set_file(const char *path) {
    FILE *f = fopen(path, "w");
    if (f) g_log_file = f;
}

void log_init(const char *spec) {
    g_log_enabled[LOG_CORE] = true;
    g_log_enabled[LOG_GAME] = true;
    if (!g_log_file) g_log_file = stderr;
    if (!spec) return;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(nullptr, ",")) {
        bool on = true;
        if (*tok == '-') {
            on = false;
            tok++;
        }
        if (!strcmp(tok, "all")) {  // the verbose categories have to be named
            for (int i = 0; i < LOG_CAT_COUNT; i++)
                if (i != LOG_GSXFER) g_log_enabled[i] = on;
            continue;
        }
        for (int i = 0; i < LOG_CAT_COUNT; i++)
            if (!strcmp(tok, k_names[i])) g_log_enabled[i] = on;
    }
    if (const char *f = getenv("EOD_LOG_FROM")) {
        g_log_from = atoll(f);
        for (int i = 0; i < LOG_CAT_COUNT; i++) {
            g_log_saved[i] = g_log_enabled[i];
            if (i != LOG_CORE && i != LOG_GAME) g_log_enabled[i] = false;
        }
    }
}

void log_write(LogCat cat, const char *fmt, ...) {
    if (!g_log_file) g_log_file = stderr;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n')) buf[--n] = 0;
    fprintf(g_log_file, "[%s] %s\n", k_names[cat], buf);
    fflush(g_log_file);
}

void fatal(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "FATAL: %s\n", buf);
    fflush(stderr);
    platform_fatal_message(buf);
    abort();
}
