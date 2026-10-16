// Display aspect for the Mac renderer's per-frame composition, and an opt-in
// projection trace.
//
// Widescreen follows upstream 1.0 (src/game_patches.cpp): the game's own
// display mode, forced by AspectRatio=16:9 / 4:3 or left to the game (auto);
// movies stay 4:3. This fork's earlier race-only wide camera
// (docs/NATIVE_CAMERA_ASPECT.md) is retired. The aspect the GS command stream
// carries per frame is the one game_patches_vblank reports through
// platform_set_aspect.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "core/config.h"
#include "core/cpu.h"
#include "core/memory.h"
#include "platform/display_rect.h"
#include "platform/presentation.h"

extern "C" void f_0034E530_orig();

int eod_presentation_aspect() {
    const int requested = presentation::requested.load(std::memory_order_relaxed);
    return requested >= 0 ? requested : display_aspect(g_config.aspect);
}

// Diagnostic only (EOD_CAMERA_TRACE): the native projection calculation is unchanged.
extern "C" void f_0034E530() {
    static const bool trace=std::getenv("EOD_CAMERA_TRACE") != nullptr;
    static unsigned count;
    if (trace && count < 16) {
        ++count;
        float xscale,base;uint32_t xb=ee_r32(0x3dd658),bb=ee_r32(0x3dd65c);
        std::memcpy(&xscale,&xb,sizeof xscale);std::memcpy(&base,&bb,sizeof base);
        std::fprintf(stderr,"[camera] projection ra=%08x near=%.9g far=%.9g fov=%.9g xscale=%.9g base=%.9g mode=%u wide=%u letterbox=%u\n",
            g_ee.r[31].uw[0],double(g_ee.fpr[12].f),double(g_ee.fpr[13].f),double(g_ee.fpr[14].f),
            double(xscale),double(base),ee_r32(0x3dd660),ee_r32(0x3dd64c),ee_r32(0x3dd650));
    }
    f_0034E530_orig();
}
