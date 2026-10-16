// prof.h - where the EE and GS threads spend their time (EOD_FPS statistics).
//
// Each thread is always "in" one category; ProfScope switches to another for
// its lifetime and the time stamp counter difference goes to the category
// that was running (exclusive time: a VIF transfer that runs a VU program
// counts the program as VU).
#ifndef PROF_H
#define PROF_H

#include <stdint.h>

#include <atomic>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <x86intrin.h>
inline uint64_t prof_ticks() { return __rdtsc(); }
#else
#include <chrono>
inline uint64_t prof_ticks() {
    return (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
}
#endif

enum ProfCat {
    // EE thread
    P_EE_GAME,   // recompiled game code (and anything not listed)
    P_EE_VU,     // VU micro programs
    P_EE_VIF,    // VIF transfers (unpack, MPG, ...)
    P_EE_GIF,    // GIF paths: packets to the GS thread
    P_EE_SOUND,  // SPU2 / IOP sound
    P_EE_IPU,    // movie decoding
    P_EE_WAIT,   // waiting for the GS thread
    P_EE_IDLE,   // asleep: nothing to do until the next event
    // GS thread
    P_GS_CMDS,      // command parsing, primitives, software work
    P_GS_STATE,     // drawing state: register decoding, render targets, GPU state
    P_GS_DRAW,      // draw submission
    P_GS_TEX,       // texture lookups (the texture cache)
    P_GS_TEXNEW,    // new textures: decoding and creation
    P_GS_UPLOAD,    // memory -> GPU uploads
    P_GS_READBACK,  // GPU -> memory readbacks (wait for the GPU)
    P_GS_CONVERT,   // format conversions on the GPU
    P_GS_PRESENT,   // display composition and Present
    P_GS_IDLE,      // waiting for commands
    P_COUNT
};

extern std::atomic<uint64_t> g_prof[P_COUNT];

struct ProfThread {
    int cur = -1;  // -1: thread not profiled
    uint64_t last = 0;
};
extern thread_local ProfThread t_prof;

// Marks the calling thread as an EE or GS thread starting in category c.
inline void prof_thread_begin(int c) {
    t_prof.cur = c;
    t_prof.last = prof_ticks();
}

inline void prof_switch(int c) {
    ProfThread &t = t_prof;
    if (t.cur < 0) return;
    uint64_t now = prof_ticks();
    g_prof[t.cur].fetch_add(now - t.last, std::memory_order_relaxed);
    t.last = now;
    t.cur = c;
}

struct ProfScope {
    int saved;
    explicit ProfScope(int c) {
        saved = t_prof.cur;
        if (saved >= 0) prof_switch(c);
    }
    ~ProfScope() {
        if (saved >= 0) prof_switch(saved);
    }
    ProfScope(const ProfScope &) = delete;
    ProfScope &operator=(const ProfScope &) = delete;
};

// "EE: game 41% VU 22% ..." and "GS: ..." lines for the time since the last call
void prof_report(char *ee, int ee_size, char *gs, int gs_size);

#endif
