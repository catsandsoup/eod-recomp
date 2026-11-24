// Register and memory effects of the seven unrolled groups in 0x1DE7B0.
#include "physics_transfer.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
using Block = std::array<uint8_t, 4096>;
Block ram, local, scratch;
unsigned generic_calls;

void interlock() {
    if (VU0.q_busy) {
        VU0.cycle = std::max(VU0.cycle, VU0.q_ready);
        VU0.q = VU0.q_pending;
        VU0.q_busy = 0;
    }
    if (VU0.p_busy) {
        VU0.cycle = std::max(VU0.cycle, VU0.p_ready);
        VU0.p = VU0.p_pending;
        VU0.p_busy = 0;
    }
}
}

VUState g_vu0, g_vu1;
uint8_t *g_ram = ram.data();

extern "C" void ee_read128_slow(uint32_t address, R128 *out) {
    memcpy(out, scratch.data() + (address & 4095u), 16);
}

extern "C" void ee_write128_slow(uint32_t address, const R128 *value) {
    memcpy(scratch.data() + (address & 4095u), value, 16);
}

extern "C" void vu0_macro(uint32_t word) {
    ++generic_calls;
    interlock();
    if (word == 0x4A0002FF) return;
    auto *row = local.data() + (VU0.vi[1] & 255u) * 16;
    if (VU0.vi[1] & 0x400) {
        unsigned r = VU0.vi[1] & 63;
        row = r < 32 ? reinterpret_cast<uint8_t*>(&g_vu1.vf[r]) :
                       reinterpret_cast<uint8_t*>(&g_vu1.vi[r-32]);
    }
    switch (word) {
        case 0x4BE10B7D: memcpy(row, &VU0.vf[1], 16); break;
        case 0x4BE1137D: memcpy(row, &VU0.vf[2], 16); break;
        case 0x4BE10B7C: memcpy(&VU0.vf[1], row, 16); break;
        case 0x4BE20B7C: memcpy(&VU0.vf[2], row, 16); break;
        default: std::abort();
    }
    VU0.vi[1] = (VU0.vi[1] + 1u) & 65535u;
}

namespace {
bool check(uint32_t address, unsigned first, unsigned count, bool alternating, bool upload, unsigned busy) {
    for (unsigned i = 0; i < local.size(); ++i) {
        ram[i] = uint8_t(i * 13 + i / 16);
        local[i] = uint8_t(i * 17 + i / 8);
        scratch[i] = uint8_t(i * 11 + i / 4);
    }
    memset(&VU0, 0xa5, sizeof VU0);
    memset(&g_vu1, 0x5a, sizeof g_vu1);
    VU0.mem = local.data();
    VU0.mem_mask = 4095;
    VU0.micro = nullptr;
    VU0.vi[1] = first;
    VU0.p_busy = busy & 1;
    VU0.q_busy = busy >> 1;
    VU0.cycle = 2;
    VU0.q_ready = 9;
    VU0.p_ready = 7;
    VU0.q_pending = -0.5f;
    VU0.p_pending = 0.125f;

    const VUState before = VU0, vu1_before = g_vu1;
    const auto ram_before = ram, local_before = local, scratch_before = scratch;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned vf = alternating && (i & 1) ? 2 : 1;
        auto *reg = reinterpret_cast<R128 *>(&VU0.vf[vf]);
        if (upload) {
            MEM_LQ(address + i * 16, reg);
            vu0_macro(vf == 1 ? 0x4BE10B7D : 0x4BE1137D);
        } else {
            vu0_macro(vf == 1 ? 0x4BE10B7C : 0x4BE20B7C);
            MEM_SQ(address + i * 16, reg);
        }
    }
    const VUState expected = VU0, vu1_after = g_vu1;
    const auto ee_after = ram, vu_after = local, scratch_after = scratch;
    VU0 = before;
    g_vu1 = vu1_before;
    ram = ram_before;
    local = local_before;
    scratch = scratch_before;
    generic_calls = 0;
    if (upload) eod_physics_stage(address, count, alternating);
    else eod_physics_collect(address, count, alternating);
    return !generic_calls && !memcmp(&g_vu1, &vu1_after, sizeof g_vu1) &&
           !memcmp(&expected, &VU0, sizeof VU0) && ee_after == ram &&
           vu_after == local && scratch_after == scratch;
}
}

int main() {
    for (unsigned count : {3u, 4u, 11u, 12u})
        for (unsigned address : {32u, 47u, 0x70000020u})
            for (unsigned first : {0u, 11u, 42u, 256u - count, 255u, 0x41fu, 0x43eu, 0xffffu})
                for (unsigned busy = 0; busy < 4; ++busy)
                    for (bool upload : {false, true})
                        if (!check(address, first, count, count > 4, upload, busy)) {
                            std::fprintf(stderr, "failed qwords=%u address=%08x first=%u busy=%u upload=%d\n",
                                         count, address, first, busy, int(upload));
                            return 1;
                        }
    return 0;
}
