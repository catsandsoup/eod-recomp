#include "movie_bits.h"
#include "core/cpu.h"
#include <cstring>

#ifdef EOD_MOVIE_BITS_TEST
extern "C" void movie_bits_test_safe_point();
#endif

namespace {
uint64_t signed_word(uint32_t word) { return (uint64_t)(int64_t)(int32_t)word; }

// Resume at the original refill-loop entry after an interrupt. Re-read guest
// state and use normal memory accessors: an interrupt may change RAM/addresses.
void refill_after_interrupt() {
    for (;;) {
        R[5].ud[0] = signed_word(MEM_LW(R[6].uw[0] + 12));
        R[2].ud[0] = 56;
        R[7].ud[0] = signed_word(MEM_LW(R[6].uw[0] + 16));
        R[3].ud[0] = MEM_LB(R[5].uw[0]);
        R[2].ud[0] = signed_word(R[2].uw[0] - R[7].uw[0]);
        R[4].ud[0] = MEM_LD(R[6].uw[0]);
        R[3].ud[0] <<= R[2].uw[0] & 63;
        R[5].ud[0] = signed_word(R[5].uw[0] + 1);
        R[4].ud[0] |= R[3].ud[0];
        R[2].ud[0] = R[5].ud[0] < R[8].ud[0];
        MEM_SD(R[6].uw[0], R[4].ud[0]);
        MEM_SW(R[6].uw[0] + 12, R[5].uw[0]);
        if (!R[2].ud[0]) {
            R[2].ud[0] = signed_word(MEM_LW(R[6].uw[0] + 32));
            MEM_SW(R[6].uw[0] + 12, R[2].uw[0]);
        }
        R[2].ud[0] = signed_word(R[7].uw[0] + 8);
        R[3].ud[0] = R[2].ud[0] < 57;
        MEM_SW(R[6].uw[0] + 16, R[2].uw[0]);
        if (!R[3].ud[0]) break;
#ifdef EOD_MOVIE_BITS_TEST
        movie_bits_test_safe_point();
#endif
        IRQ_CHECK();
    }
    R[2].ud[0] = R[10].ud[0] + R[9].ud[0];
    MEM_SD(R[6].uw[0] + 24, R[2].ud[0]);
}
}

bool movie_bits_advance_fast() {
    const uint64_t address_register = R[4].ud[0], request_register = R[5].ud[0];
    const uint32_t address = R[4].uw[0], request = R[5].uw[0];
    // Cached host locals are safe only for the ordinary RAM-backed bit reader.
    // Leave aliases, MMIO, overlapping input/state and invalid counts to the
    // original function, before changing any memory or register.
    if (address > EE_RAM_SIZE - 48 || request > 64) return false;
    uint8_t *state = g_ram + address;
    uint32_t count;
    std::memcpy(&count, state + 16, 4);
    if (count > 64 || request > count) return false;
    count -= request;
    uint32_t cursor = 0, begin = 0, end = 0;
    const bool refill = count < 57;
    if (refill) {
        std::memcpy(&cursor, state + 12, 4);
        std::memcpy(&begin, state + 32, 4);
        std::memcpy(&end, state + 36, 4);
        if (end == UINT32_MAX) {
            // The movie demuxer also uses an unbounded linear-reader sentinel.
            // Its sign-extended end is UINT64_MAX, so positive RAM cursors do
            // not wrap. Prove only the bytes this call actually fetches safe.
            const uint32_t bytes = (64 - count) / 8;
            if (cursor > EE_RAM_SIZE - bytes ||
                (cursor < address + 48 && cursor + bytes > address)) return false;
        } else if (begin >= end || end > EE_RAM_SIZE || cursor < begin || cursor >= end ||
                   (begin < address + 48 && end > address)) return false;
    }
    EE_FUNC_ENTRY(0x0039AFB0);
    uint64_t bits, consumed;
    std::memcpy(&bits, state, 8);
    std::memcpy(&consumed, state + 24, 8);
    bits <<= request & 63; // R5900 DSLLV masks the shift, including request=64.
    std::memcpy(state, &bits, 8);
    std::memcpy(state + 16, &count, 4);

    // Write only the low 64 bits of changed GPRs; their upper halves survive.
    R[6].ud[0] = address_register;
    R[8].ud[0] = refill ? signed_word(end) : R[8].ud[0];
    R[9].ud[0] = request_register;
    R[10].ud[0] = consumed;
    if (!refill) {
        R[3].ud[0] = signed_word(count);
        R[4].ud[0] = 0;
    } else {
        uint32_t previous_count = 0, incremented_cursor = 0;
        do {
            previous_count = count;
            bits |= (uint64_t)g_ram[cursor] << ((56 - count) & 63);
            incremented_cursor = cursor + 1;
            cursor = incremented_cursor < end ? incremented_cursor : begin;
            count += 8;
            std::memcpy(state, &bits, 8);
            std::memcpy(state + 12, &cursor, 4);
            std::memcpy(state + 16, &count, 4);
#ifdef EOD_MOVIE_BITS_TEST
            if (count < 57) movie_bits_test_safe_point();
#endif
            if (count < 57 && g_ee_irq_pending) {
                // Exact original safe-point state, including temporary GPRs,
                // must be visible to interrupt handlers and the fiber scheduler.
                R[2].ud[0] = signed_word(count);
                R[3].ud[0] = 1;
                R[4].ud[0] = bits;
                R[5].ud[0] = signed_word(incremented_cursor);
                R[7].ud[0] = signed_word(previous_count);
                ee_service_interrupts();
                refill_after_interrupt();
                return true;
            }
        } while (count < 57);
        R[3].ud[0] = 0;
        R[4].ud[0] = bits;
        R[5].ud[0] = signed_word(incremented_cursor);
        R[7].ud[0] = signed_word(previous_count);
    }
    R[2].ud[0] = consumed + request_register;
    std::memcpy(state + 24, &R[2].ud[0], 8);
    return true;
}
