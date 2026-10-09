// cpu.h - runtime side of the EE CPU.
#pragma once
#include <cstdint>

#include "ee.h"

void cpu_init();
void cpu_dump_state();
uint32_t cpu_cycles();
// Replace the target of indirect calls to addr (direct jal calls are
// redirected at recompile time via config/hle.txt).
void cpu_register_override(uint32_t addr, EEFunc fn);

// Helpers for HLE functions implemented in C++.
inline uint32_t arg_u32(int n) { return g_ee.r[4 + n].uw[0]; }
inline int32_t arg_s32(int n) { return g_ee.r[4 + n].sw[0]; }
inline void ret_s32(int32_t v) { g_ee.r[2].sd[0] = v; }
inline void ret_u64(uint64_t v) { g_ee.r[2].ud[0] = v; }
