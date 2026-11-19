// diff_harness.h - proves a readable C++ replacement equivalent to the recompiled original on
// live game state (docs/DECOMPILATION.md).
//
// A replaced function is registered in config/hle.txt as `wrap` and defined as
//     extern "C" void f_XXXXXXXX(void) { diff_run(0xXXXXXXXX, replacement, f_XXXXXXXX_orig); }
// Without EOD_DIFF the replacement simply runs. With EOD_DIFF=<hex addr,...> or EOD_DIFF=all,
// sampled calls (EOD_DIFF_EVERY=N, default 1) run the original and the replacement from the same
// state and compare what a caller can observe:
//   - $v0 (and $v1 / $f0 with kDiffRet64 / kDiffFloatRet), callee-saved $s0-$s7, $gp, $sp,
//     $fp, $ra and $f20-$f31
//   - guest RAM (the low EOD_DIFF_RAM_MB, default 64, and the stack region above the caller's
//     $sp), the scratchpad and VU0 registers and memory
// Dead stack below the caller's $sp and caller-saved registers are not compared (callers never
// read them). The original's results are kept, so a mismatch never changes the game.
// EOD_DIFF_REPLAY=<hex addr>:<n>,... checks each checked call of those functions n more times from
// the same recorded entry state (deterministic replay; counted as "replayed").
// Interrupt delivery is held during a checked call (the recompiled code delivers pending
// interrupts at loop back-edges; a replacement moves those points, see docs/DECOMPILATION.md).
// Limits: the function must not touch hardware (DMA kicks, GS/VIF registers, IOP), and host-side
// state in other wrapped functions it calls is advanced twice.
#pragma once
#include <cstdint>

enum : unsigned { kDiffRet64 = 1, kDiffFloatRet = 2 };
void diff_run(uint32_t addr, void (*replacement)(void), void (*original)(void), unsigned flags = 0);
// True while the original side of a comparison runs (its guest results are kept, but the
// replacement side repeats every nested call): host-side bookkeeping done there is undone.
bool diff_in_original_side();
// Prints the [diff] totals (called at exit: app_exit leaves through _exit, so atexit never runs).
void diff_report();
// Another exit report to print with diff_report (test counters that would otherwise need atexit).
void diff_add_exit_report(void (*fn)());
