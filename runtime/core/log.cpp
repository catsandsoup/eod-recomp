#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../platform/platform.h"

bool g_log_enabled[LOG_CAT_COUNT];

static const char *k_names[LOG_CAT_COUNT] = {
    "core", "kernel", "thread", "hw", "dma", "gs", "vif", "vu",
    "sif", "iop", "file", "pad", "mc", "snd", "game",
};

static FILE *g_log_file;

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
        if (!strcmp(tok, "all")) {
            for (bool &b : g_log_enabled) b = on;
            continue;
        }
        for (int i = 0; i < LOG_CAT_COUNT; i++)
            if (!strcmp(tok, k_names[i])) g_log_enabled[i] = on;
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
