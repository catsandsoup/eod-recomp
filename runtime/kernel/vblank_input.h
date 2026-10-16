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
