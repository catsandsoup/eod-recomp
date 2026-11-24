#pragma once

#include "ee.h"
#include "vehicle_dynamics.h"

/* 0x1DE7B0 transfers body, contact and parameter records into qwords [0,66).
 * Enabled bodies copy [0,42) back. The generated EE loop still handles each
 * back edge so interrupt handlers see the same progress and live registers. */
EE_INLINE void eod_physics_interlock(void) {
    if (EE_UNLIKELY(VU0.q_busy)) {
        if (VU0.cycle < VU0.q_ready) VU0.cycle = VU0.q_ready;
        VU0.q = VU0.q_pending;
        VU0.q_busy = 0;
    }
    if (EE_UNLIKELY(VU0.p_busy)) {
        if (VU0.cycle < VU0.p_ready) VU0.cycle = VU0.p_ready;
        VU0.p = VU0.p_pending;
        VU0.p_busy = 0;
    }
}

EE_INLINE void eod_physics_transfer(uint32_t address, unsigned count, int alternating, int upload) {
    address &= ~15u;
    const uint32_t first = VU0.vi[1];
    const unsigned bytes = count * 16;
    /* The generator only passes groups of 3, 4, 11 or 12 qwords. Ordinary
     * EE RAM and the VU0 data store are separate, contiguous allocations. */
    if (EE_LIKELY(address <= EE_RAM_SIZE - bytes && first <= 256u - count)) {
        uint8_t *ee = g_ram + address;
        uint8_t *local = VU0.mem + first * 16;
        eod_physics_interlock();
        if (upload) memcpy(local, ee, bytes);
        else memcpy(ee, local, bytes);
        const unsigned last = count - 1;
        /* An alternating group leaves its last two rows in VF1/VF2. */
        if (alternating) memcpy(&VU0.vf[1 + ((last - 1) & 1)], local + (last - 1) * 16, 16);
        memcpy(&VU0.vf[1 + (alternating && (last & 1))], local + last * 16, 16);
        VU0.vi[1] = first + count;
        return;
    }
    /* Preserve ordering at EE MMIO, address wrap and the VU1 register window. */
    for (unsigned row = 0; row < count; ++row) {
        const unsigned vf = 1 + (alternating && (row & 1));
        if (upload) MEM_LQ(address + row * 16, (R128 *)&VU0.vf[vf]);
        eod_physics_interlock();
        const unsigned index = VU0.vi[1];
        uint8_t *local = VU0.mem + ((index * 16) & VU0.mem_mask);
        if (index & 0x400) {
            const unsigned reg = index & 63;
            local = reg < 32 ? (uint8_t *)&g_vu1.vf[reg] : (uint8_t *)&g_vu1.vi[reg - 32];
        }
        if (upload) memcpy(local, &VU0.vf[vf], 16);
        else memcpy(&VU0.vf[vf], local, 16);
        VU0.vi[1] = (index + 1) & 0xffffu;
        if (!upload) MEM_SQ(address + row * 16, (const R128 *)&VU0.vf[vf]);
    }
}

EE_INLINE void eod_physics_stage(uint32_t address, unsigned count, int alternating) {
    eod_physics_transfer(address, count, alternating, 1);
}

EE_INLINE void eod_physics_collect(uint32_t address, unsigned count, int alternating) {
    eod_physics_transfer(address, count, alternating, 0);
}
