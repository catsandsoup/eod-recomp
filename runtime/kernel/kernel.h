// kernel.h - high level emulation of the EE kernel (threads, semaphores,
// interrupt handlers, alarms) on top of host fibers.
#pragma once
#include <cstdint>

void kernel_init(int argc, const char *const *argv);
// Runs the program: enters the ELF entry point on the main thread.
void kernel_run(uint32_t entry);

bool kernel_interrupts_enabled();
void kernel_set_interrupts_enabled(bool on);
bool kernel_in_interrupt();

// Hardware raises interrupts through these (delivered at the next safe point).
void intc_raise(int cause);
void dmac_raise(int channel);
uint32_t intc_stat();
uint32_t intc_mask();
void intc_write_stat(uint32_t v);
void intc_write_mask(uint32_t v);

bool kernel_call_trampoline(uint32_t addr);
void kernel_dump_threads();

// Blocks the calling EE thread until something wakes it (used by HLE code
// waiting on an event, e.g. synchronous IOP calls).
void kernel_yield();

// Current vblank counter.
uint64_t kernel_vblank_count();

// INTC causes
enum {
    INTC_GS = 0,
    INTC_SBUS = 1,
    INTC_VBLANK_S = 2,
    INTC_VBLANK_E = 3,
    INTC_VIF0 = 4,
    INTC_VIF1 = 5,
    INTC_VU0 = 6,
    INTC_VU1 = 7,
    INTC_IPU = 8,
    INTC_TIM0 = 9,
    INTC_TIM1 = 10,
    INTC_TIM2 = 11,
    INTC_TIM3 = 12,
    INTC_SFIFO = 13,
    INTC_VU0WD = 14,
};
