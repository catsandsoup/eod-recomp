// Shared vblank host-input publication step, also exercised with real pad DMA
// in the hardware-free input test.
#pragma once
#include "../platform/platform.h"
void iop_pad_vblank();
inline bool vblank_poll_input() {
    if (!platform_pump()) return false;
    iop_pad_vblank();
    return true;
}

// Wait for rendering before sampling: events that arrive during GS backpressure
// must be visible to the EE consumer on this same vblank.
template<class FinishGraphics>
inline bool vblank_finish_and_poll_input(FinishGraphics finish_graphics) {
    finish_graphics();
    return vblank_poll_input();
}
