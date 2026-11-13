// career_warp.cpp - the free-roam half of EOD_DIRECT_START="mode=career;place=...;eve=..." (src/session_start.h).
//
// Once the Career hub has answered Drive Around, the Home Driver (vtable 0x3F8398, per-frame update 0x16FE48,
// home object at 0x3CDA04) runs every frame on the game thread. A few frames after it starts (so the world,
// the player's car and the area records are in place) the warp calls the game's own Jump To teleport
// 0x16F488(area) -- the call the pause menu's teleport list (teleport.lyt, 0x386DE8) makes -- once.
// The "where am I" word 0x3CDA00 (9 = no area) is logged for a second afterwards, so a run shows where the
// car arrived. eve= is handled by src/hle_dev_career.cpp (CareerDoEve at the start of free roam).
#include "core/cpu.h"
#include "core/memory.h"
#include "funcs.h"
#include "session_start.h"

#include <cstdio>
#include <cstring>

extern "C" {
void f_0016FE48_orig(void);
}

namespace {
constexpr uint32_t kHome = 0x003CDA04;      // Home Driver object while free roam runs
constexpr uint32_t kArea = 0x003CDA00;      // area the player's car is in (9 = none)
constexpr int kSettleFrames = 30;           // Home Driver updates before the teleport

uint32_t call(void (*fn)(void), uint32_t a0) {
    const EEState saved = g_ee;
    g_ee.r[4].ud[0] = a0;
    fn();
    const uint32_t v0 = g_ee.r[2].uw[0];
    g_ee = saved;
    return v0;
}
float rf(uint32_t a) { const uint32_t v = ee_r32(a); float f; std::memcpy(&f, &v, 4); return f; }
}  // namespace

// HomeDriver::Update(home)
extern "C" void f_0016FE48(void) {
    f_0016FE48_orig();
    const SessionRequest *req = career_warp_request();
    if (!req) return;
    static int frames = 0, after = -1;
    static bool done = false;
    ++frames;
    if (!done && frames >= kSettleFrames && ee_r32(kHome)) {
        done = true;
        const int place = career_place_index(req->place);
        std::fprintf(stderr, "[CAREER_WARP] teleport to %s (area %d), from area %u\n", career_place_name(place), place, ee_r32(kArea));
        call(f_0016F488, (uint32_t)place);
        after = 0;
    }
    if (after >= 0 && after <= 600 && after % 30 == 0) {
        const uint32_t home = ee_r32(kHome);
        const uint32_t car = call(f_00179A08, call(f_00179920, 0));
        std::fprintf(stderr, "[CAREER_WARP] +%d frames: area %u (home zone %d, pending %d, driving %d), car at %.1f %.1f %.1f\n", after,
                     ee_r32(kArea), (int)ee_r32(home + 3056), (int)ee_r32(home + 3060), (int)ee_r32(home + 3064),
                     car ? rf(car + 0x88) : 0.f, car ? rf(car + 0x8C) : 0.f, car ? rf(car + 0x90) : 0.f);
    }
    if (after >= 0) ++after;
}
