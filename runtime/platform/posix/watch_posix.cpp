// watch_posix.cpp - debugging aid (Linux/x86-64): EOD_WATCHW=<EE address>
// traps every host store to the 16 bytes at that address (page protection
// + single step) and reports the host instruction that first makes one of
// its floats huge.  Resolve the address with `nm -C eod`.
#if defined(__linux__) && defined(__x86_64__)
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../../core/memory.h"
#include "ee.h"

namespace {
uint8_t *g_watch;     // host address of the watched bytes
uint8_t *g_page;
uintptr_t g_last_rip;
bool g_reported;

bool huge() {
    for (int i = 0; i < 3; i++) {  // x, y, z (w is often padding)
        float f;
        memcpy(&f, g_watch + i * 4, 4);
        if (f == f && fabsf(f) > 1e30f) return true;
    }
    return false;
}

void on_segv(int, siginfo_t *si, void *ctx) {
    auto *uc = static_cast<ucontext_t *>(ctx);
    uint8_t *a = static_cast<uint8_t *>(si->si_addr);
    if (a < g_page || a >= g_page + 4096) {
        signal(SIGSEGV, SIG_DFL);
        return;
    }
    mprotect(g_page, 4096, PROT_READ | PROT_WRITE);
    if (a >= g_watch && a < g_watch + 16) g_last_rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    else g_last_rip = 0;
    uc->uc_mcontext.gregs[REG_EFL] |= 0x100;  // single step the store
}

void on_trap(int, siginfo_t *, void *ctx) {
    auto *uc = static_cast<ucontext_t *>(ctx);
    uc->uc_mcontext.gregs[REG_EFL] &= ~0x100;
    if (g_last_rip && !g_reported && huge()) {
        g_reported = true;
        fprintf(stderr, "WATCH: huge value written by host pc %lx; EE ra=%08x a0=%08x a1=%08x a2=%08x sp=%08x\n",
                (unsigned long)g_last_rip, g_ee.r[31].uw[0], g_ee.r[4].uw[0], g_ee.r[5].uw[0], g_ee.r[6].uw[0],
                g_ee.r[29].uw[0]);
    }
    mprotect(g_page, 4096, PROT_READ);
}
}  // namespace

void debug_watch_init() {
    const char *w = getenv("EOD_WATCHW");
    if (!w) return;
    uint32_t ee = (uint32_t)strtoul(w, nullptr, 16) & 0x1FFFFFF;
    g_watch = g_ram + ee;
    g_page = (uint8_t *)((uintptr_t)g_watch & ~(uintptr_t)4095);
    struct sigaction sa = {};
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = on_segv;
    sigaction(SIGSEGV, &sa, nullptr);
    sa.sa_sigaction = on_trap;
    sigaction(SIGTRAP, &sa, nullptr);
    mprotect(g_page, 4096, PROT_READ);
    fprintf(stderr, "WATCH: %08x (host %p) debug_watch_init at %p\n", ee, (void *)g_watch, (void *)&debug_watch_init);
}
#else
void debug_watch_init() {}
#endif
