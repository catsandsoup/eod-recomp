// cpu.cpp - glue between recompiled code and the runtime: indirect calls,
// coprocessor 0, FPU control registers and fault reporting.
#include <cstdio>
#include <cstdlib>

#include "cpu.h"
#include "ee.h"
#include "log.h"
#include "../kernel/kernel.h"
#include "../hw/hw.h"
#include "../platform/platform.h"

EEState g_ee;
#ifdef EE_TRACE_CALLS
uint32_t g_ee_trace[256];
uint32_t g_ee_trace_pos;
#endif

// ------------------------------------------------------------ lookup
namespace {
struct CacheEnt {
    uint32_t addr;
    EEFunc fn;
};
constexpr unsigned kCacheSize = 8192;
CacheEnt g_cache[kCacheSize];

EEFunc g_override_fn[64];
uint32_t g_override_addr[64];
int g_override_count;
}  // namespace

void cpu_register_override(uint32_t addr, EEFunc fn) {
    if (g_override_count == 64) fatal("too many overrides");
    g_override_addr[g_override_count] = addr;
    g_override_fn[g_override_count++] = fn;
}

extern "C" EEFunc ee_lookup_func(uint32_t addr) {
    CacheEnt &c = g_cache[(addr >> 2) & (kCacheSize - 1)];
    if (c.addr == addr && c.fn) return c.fn;
    for (int i = 0; i < g_override_count; i++)
        if (g_override_addr[i] == addr) return c.addr = addr, c.fn = g_override_fn[i];
    unsigned lo = 0, hi = ee_func_table_size;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (ee_func_table[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < ee_func_table_size && ee_func_table[lo].addr == addr) {
        c.addr = addr;
        c.fn = ee_func_table[lo].fn;
        return c.fn;
    }
    return nullptr;
}

static void dump_and_die(const char *what, uint32_t addr) {
    cpu_dump_state();
    fatal("%s to %08x: no recompiled function at that address", what, addr);
}

extern "C" void ee_call(uint32_t addr) {
    if (EEFunc f = ee_lookup_func(addr)) {
        f();
        return;
    }
    if (kernel_call_trampoline(addr)) return;
    dump_and_die("indirect call", addr);
}

extern "C" void ee_jump(uint32_t addr) {
    // Tail jump to a computed address: treat as a call whose return is ours.
    if (EEFunc f = ee_lookup_func(addr)) {
        f();
        return;
    }
    if (kernel_call_trampoline(addr)) return;
    dump_and_die("indirect jump", addr);
}

extern "C" void ee_call_unknown(uint32_t addr) { ee_call(addr); }
extern "C" void ee_jump_unknown(uint32_t addr) { ee_jump(addr); }

extern "C" void ee_bad_instruction(uint32_t pc, uint32_t instr) {
    cpu_dump_state();
    fatal("invalid/unsupported instruction %08x at %08x", instr, pc);
}

extern "C" void ee_break(uint32_t pc, uint32_t instr) {
    cpu_dump_state();
    fatal("BREAK %d executed at %08x (game assertion?)", (instr >> 6) & 0xFFFFF, pc);
}

void cpu_dump_state() {
    static const char *names[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                    "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                    "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    char line[256];
    for (int i = 0; i < 32; i += 4) {
        int n = 0;
        for (int j = i; j < i + 4; j++)
            n += snprintf(line + n, sizeof line - n, "%4s=%016llx ", names[j], (unsigned long long)g_ee.r[j].ud[0]);
        log_write(LOG_CORE, "%s", line);
    }
    kernel_dump_threads();
#ifdef EE_TRACE_CALLS
    char buf[1024];
    int n = 0;
    for (int i = 32; i >= 1; i--) {
        n += snprintf(buf + n, sizeof buf - n, "%08x ", g_ee_trace[(g_ee_trace_pos - i) & 255]);
        if (i % 8 == 1) {
            log_write(LOG_CORE, "recent calls: %s", buf);
            n = 0;
        }
    }
#endif
}

// ------------------------------------------------------------- COP0
static uint32_t g_cop0[32];

void cpu_init() {
    g_cop0[12] = 0x70030C13;  // Status: COP0/1 usable, EIE, user mode (EXL set until ei)
    g_cop0[12] = 0x70030C11;
    g_cop0[15] = 0x2E20;      // PRId: R5900 rev 2.0
    g_cop0[16] = 0x440;       // Config
}

uint32_t cpu_cycles() {
    // 294.912 MHz derived from host time
    return (uint32_t)(platform_time_us() * 294912ull / 1000ull);
}

extern "C" uint32_t ee_mfc0(int reg) {
    switch (reg) {
    case 9:
        return cpu_cycles();
    case 12: {
        uint32_t s = g_cop0[12] & ~0x10001u;
        if (kernel_interrupts_enabled()) s |= 0x10001;
        return s;
    }
    default:
        return g_cop0[reg];
    }
}

extern "C" void ee_mtc0(int reg, uint32_t v) {
    LOG(LOG_HW, "mtc0 $%d = %08x", reg, v);
    if (reg == 12) {
        kernel_set_interrupts_enabled((v & 0x10001) == 0x10001 && !(v & 6));
    }
    g_cop0[reg] = v;
}

extern "C" void ee_ei(void) { kernel_set_interrupts_enabled(true); }
extern "C" void ee_di(void) { kernel_set_interrupts_enabled(false); }
extern "C" void ee_eret(void) { kernel_set_interrupts_enabled(true); }

static uint32_t g_pccr, g_pcr0, g_pcr1;
extern "C" uint32_t ee_mfpc(int sel) {
    // sel: 0 = PCCR, 1 = PCR0, 3 = PCR1
    if (sel == 0) return g_pccr;
    uint32_t c = cpu_cycles();
    return sel == 1 ? c - g_pcr0 : c - g_pcr1;
}
extern "C" void ee_mtpc(int sel, uint32_t v) {
    if (sel == 0) g_pccr = v;
    else if (sel == 1) g_pcr0 = cpu_cycles() - v;
    else g_pcr1 = cpu_cycles() - v;
}

extern "C" int ee_cpcond0(void) { return hw_cpcond0(); }
extern "C" void ee_cache(int, uint32_t) {}

// -------------------------------------------------------------- FPU
extern "C" uint32_t ee_cfc1(int reg) {
    if (reg == 0) return 0x2E30;  // FCR0: implementation/revision
    if (reg == 31) return (g_ee.fcr31 & ~0x00800000u) | (g_ee.fcc ? 0x00800000u : 0);
    return 0;
}

extern "C" void ee_ctc1(int reg, uint32_t v) {
    if (reg == 31) {
        g_ee.fcr31 = v;
        g_ee.fcc = (v >> 23) & 1;
    }
}
