/*
 * ee.h - Emotion Engine (R5900) CPU state and instruction semantics used by
 * the recompiled code in build/gen/rec_NNN.c (see tools/recomp.py).
 *
 * The whole program shares one CPU state, g_ee, exactly like the hardware:
 * the thread scheduler (runtime/kernel) saves and restores it on switches.
 */
#ifndef EE_H
#define EE_H

#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define EE_LIKELY(x) __builtin_expect(!!(x), 1)
#define EE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define EE_INLINE static inline __attribute__((always_inline))
#define EE_NORETURN __attribute__((noreturn))
#else
#define EE_LIKELY(x) (x)
#define EE_UNLIKELY(x) (x)
#define EE_INLINE static __forceinline
#define EE_NORETURN __declspec(noreturn)
#endif

typedef union R128 {
    uint64_t ud[2];
    int64_t sd[2];
    uint32_t uw[4];
    int32_t sw[4];
    uint16_t uh[8];
    int16_t sh[8];
    uint8_t ub[16];
    int8_t sb[16];
    float f[4];
} R128;

typedef union FReg {
    float f;
    uint32_t u;
    int32_t s;
} FReg;

typedef struct EEState {
    R128 r[32];
    R128 hi, lo;          /* hi.ud[1]/lo.ud[1] are HI1/LO1 */
    uint32_t sa;          /* shift amount register (bits) */
    FReg fpr[32];
    float facc;           /* FPU accumulator */
    uint32_t fcc;         /* FPU condition (FCR31.C) */
    uint32_t fcr31;
    uint32_t pc;          /* only meaningful for diagnostics */
} EEState;

extern EEState g_ee;

#define R g_ee.r
#define FPR g_ee.fpr
#define FACC g_ee.facc
#define FCC g_ee.fcc

/* ------------------------------------------------------------- memory */
#define EE_RAM_SIZE 0x02000000u

extern uint8_t *g_ram;   /* 32MB main memory (+ guard) */

uint8_t ee_read8_slow(uint32_t a);
uint16_t ee_read16_slow(uint32_t a);
uint32_t ee_read32_slow(uint32_t a);
uint64_t ee_read64_slow(uint32_t a);
void ee_read128_slow(uint32_t a, R128 *out);
void ee_write8_slow(uint32_t a, uint8_t v);
void ee_write16_slow(uint32_t a, uint16_t v);
void ee_write32_slow(uint32_t a, uint32_t v);
void ee_write64_slow(uint32_t a, uint64_t v);
void ee_write128_slow(uint32_t a, const R128 *v);

EE_INLINE uint32_t MEM_LB(uint32_t a) {
    if (EE_LIKELY(a < EE_RAM_SIZE)) return g_ram[a];
    return ee_read8_slow(a);
}
EE_INLINE uint32_t MEM_LH(uint32_t a) {
    if (EE_LIKELY(a < EE_RAM_SIZE)) { uint16_t v; memcpy(&v, g_ram + a, 2); return v; }
    return ee_read16_slow(a);
}
EE_INLINE uint32_t MEM_LW(uint32_t a) {
    if (EE_LIKELY(a < EE_RAM_SIZE)) { uint32_t v; memcpy(&v, g_ram + a, 4); return v; }
    return ee_read32_slow(a);
}
EE_INLINE uint64_t MEM_LD(uint32_t a) {
    if (EE_LIKELY(a < EE_RAM_SIZE)) { uint64_t v; memcpy(&v, g_ram + a, 8); return v; }
    return ee_read64_slow(a);
}
EE_INLINE void MEM_LQ(uint32_t a, R128 *out) {
    a &= ~15u;
    if (EE_LIKELY(a < EE_RAM_SIZE)) { memcpy(out, g_ram + a, 16); return; }
    ee_read128_slow(a, out);
}
EE_INLINE void MEM_SB(uint32_t a, uint8_t v) {
    if (EE_LIKELY(a < EE_RAM_SIZE)) { g_ram[a] = v; return; }
    ee_write8_slow(a, v);
}
EE_INLINE void MEM_SH(uint32_t a, uint16_t v) {
    if (EE_LIKELY(a < EE_RAM_SIZE)) { memcpy(g_ram + a, &v, 2); return; }
    ee_write16_slow(a, v);
}
EE_INLINE void MEM_SW(uint32_t a, uint32_t v) {
    if (EE_LIKELY(a < EE_RAM_SIZE)) { memcpy(g_ram + a, &v, 4); return; }
    ee_write32_slow(a, v);
}
EE_INLINE void MEM_SD(uint32_t a, uint64_t v) {
    if (EE_LIKELY(a < EE_RAM_SIZE)) { memcpy(g_ram + a, &v, 8); return; }
    ee_write64_slow(a, v);
}
EE_INLINE void MEM_SQ(uint32_t a, const R128 *v) {
    a &= ~15u;
    if (EE_LIKELY(a < EE_RAM_SIZE)) { memcpy(g_ram + a, v, 16); return; }
    ee_write128_slow(a, v);
}

/* Host pointer for an EE address range that is known to be in RAM or
 * scratchpad (used by HLE code).  Returns NULL for I/O space. */
void *ee_ptr(uint32_t a);

/* Unaligned load/store pairs (little endian semantics). */
EE_INLINE void ee_lwl(int rt, uint32_t a) {
    uint32_t sh = (a & 3) * 8;
    uint32_t mem = MEM_LW(a & ~3u);
    uint32_t mask = 0x00FFFFFFu >> sh;
    uint32_t v = (R[rt].uw[0] & mask) | (mem << (24 - sh));
    R[rt].sd[0] = (int32_t)v;
}
EE_INLINE void ee_lwr(int rt, uint32_t a) {
    uint32_t sh = (a & 3) * 8;
    uint32_t mem = MEM_LW(a & ~3u);
    if (sh == 0) {
        R[rt].sd[0] = (int32_t)mem;
    } else {
        uint32_t mask = 0xFFFFFF00u << (24 - sh);
        R[rt].uw[0] = (R[rt].uw[0] & mask) | (mem >> sh);
    }
}
EE_INLINE void ee_swl(int rt, uint32_t a) {
    uint32_t sh = (a & 3) * 8;
    uint32_t mem = MEM_LW(a & ~3u);
    uint32_t mask = 0xFFFFFF00u << sh;
    MEM_SW(a & ~3u, (R[rt].uw[0] >> (24 - sh)) | (mem & mask));
}
EE_INLINE void ee_swr(int rt, uint32_t a) {
    uint32_t sh = (a & 3) * 8;
    uint32_t mem = MEM_LW(a & ~3u);
    uint32_t mask = 0x00FFFFFFu >> (24 - sh);
    MEM_SW(a & ~3u, (R[rt].uw[0] << sh) | (mem & mask));
}
EE_INLINE void ee_ldl(int rt, uint32_t a) {
    uint32_t sh = (a & 7) * 8;
    uint64_t mem = MEM_LD(a & ~7u);
    uint64_t mask = 0x00FFFFFFFFFFFFFFull >> sh;
    R[rt].ud[0] = (R[rt].ud[0] & mask) | (mem << (56 - sh));
}
EE_INLINE void ee_ldr(int rt, uint32_t a) {
    uint32_t sh = (a & 7) * 8;
    uint64_t mem = MEM_LD(a & ~7u);
    if (sh == 0) {
        R[rt].ud[0] = mem;
    } else {
        uint64_t mask = 0xFFFFFFFFFFFFFF00ull << (56 - sh);
        R[rt].ud[0] = (R[rt].ud[0] & mask) | (mem >> sh);
    }
}
EE_INLINE void ee_sdl(int rt, uint32_t a) {
    uint32_t sh = (a & 7) * 8;
    uint64_t mem = MEM_LD(a & ~7u);
    uint64_t mask = sh == 56 ? 0 : (0xFFFFFFFFFFFFFF00ull << sh);
    MEM_SD(a & ~7u, (R[rt].ud[0] >> (56 - sh)) | (mem & mask));
}
EE_INLINE void ee_sdr(int rt, uint32_t a) {
    uint32_t sh = (a & 7) * 8;
    uint64_t mem = MEM_LD(a & ~7u);
    uint64_t mask = sh == 0 ? 0 : (0x00FFFFFFFFFFFFFFull >> (56 - sh));
    MEM_SD(a & ~7u, (R[rt].ud[0] << sh) | (mem & mask));
}

/* ------------------------------------------------------ mult / divide */
EE_INLINE void ee_set_hilo(int pipe, int rd, int64_t res) {
    g_ee.lo.sd[pipe] = (int32_t)(uint32_t)res;
    g_ee.hi.sd[pipe] = (int32_t)(uint32_t)((uint64_t)res >> 32);
    if (rd) R[rd].sd[0] = g_ee.lo.sd[pipe];
}
EE_INLINE void ee_mult(int pipe, int rd, int rs, int rt) {
    ee_set_hilo(pipe, rd, (int64_t)R[rs].sw[0] * (int64_t)R[rt].sw[0]);
}
EE_INLINE void ee_multu(int pipe, int rd, int rs, int rt) {
    ee_set_hilo(pipe, rd, (int64_t)((uint64_t)R[rs].uw[0] * (uint64_t)R[rt].uw[0]));
}
EE_INLINE void ee_madd(int pipe, int rd, int rs, int rt) {
    int64_t acc = (int64_t)(((uint64_t)g_ee.hi.uw[pipe * 2] << 32) | g_ee.lo.uw[pipe * 2]);
    ee_set_hilo(pipe, rd, acc + (int64_t)R[rs].sw[0] * (int64_t)R[rt].sw[0]);
}
EE_INLINE void ee_maddu(int pipe, int rd, int rs, int rt) {
    uint64_t acc = ((uint64_t)g_ee.hi.uw[pipe * 2] << 32) | g_ee.lo.uw[pipe * 2];
    ee_set_hilo(pipe, rd, (int64_t)(acc + (uint64_t)R[rs].uw[0] * (uint64_t)R[rt].uw[0]));
}
EE_INLINE void ee_div(int pipe, int rs, int rt) {
    int32_t n = R[rs].sw[0], d = R[rt].sw[0];
    if (d == 0) {
        g_ee.lo.sd[pipe] = n < 0 ? 1 : -1;
        g_ee.hi.sd[pipe] = n;
    } else if (n == (int32_t)0x80000000 && d == -1) {
        g_ee.lo.sd[pipe] = (int32_t)0x80000000;
        g_ee.hi.sd[pipe] = 0;
    } else {
        g_ee.lo.sd[pipe] = n / d;
        g_ee.hi.sd[pipe] = n % d;
    }
}
EE_INLINE void ee_divu(int pipe, int rs, int rt) {
    uint32_t n = R[rs].uw[0], d = R[rt].uw[0];
    if (d == 0) {
        g_ee.lo.sd[pipe] = -1;
        g_ee.hi.sd[pipe] = (int32_t)n;
    } else {
        g_ee.lo.sd[pipe] = (int32_t)(n / d);
        g_ee.hi.sd[pipe] = (int32_t)(n % d);
    }
}

/* ------------------------------------------------------------- FPU
 * The EE FPU is not IEEE 754: there are no infinities/NaNs and denormals
 * are flushed to zero (FTZ/DAZ are enabled in MXCSR at startup).  Results
 * that would overflow saturate to +-FLT_MAX.  Every arithmetic result is
 * clamped: an IEEE infinity would otherwise propagate where the game relies
 * on saturation (e.g. the track follower walks segments forever).
 * EE_FPU_NO_CLAMP disables it (division/sqrt/conversion stay emulated).
 */
EE_INLINE float ee_fclamp(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    if (EE_UNLIKELY((u & 0x7F800000u) == 0x7F800000u)) {
        u = (u & 0x80000000u) | 0x7F7FFFFFu;
        memcpy(&f, &u, 4);
    }
    return f;
}
EE_INLINE int ee_fspecial(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return (u & 0x7F800000u) == 0x7F800000u;
}
/* A special result means an overflow or an operand that is Inf/NaN in IEEE
 * terms but a (huge) ordinary number on the EE (e.g. 0xFFFFFFFF padding in
 * a vector multiplied by 0): redo it with saturated operands. */
#ifndef EE_FPU_NO_CLAMP
#define EE_FIX2(r, a, b, OP) \
    if (EE_UNLIKELY(ee_fspecial(r))) r = ee_fclamp(ee_fclamp(a) OP ee_fclamp(b))
#else
#define EE_FIX2(r, a, b, OP) (void)0
#endif
EE_INLINE float fadd_ps2(float a, float b) {
    float r = a + b;
    EE_FIX2(r, a, b, +);
    return r;
}
EE_INLINE float fsub_ps2(float a, float b) {
    float r = a - b;
    EE_FIX2(r, a, b, -);
    return r;
}
EE_INLINE float fmul_ps2(float a, float b) {
    float r = a * b;
    EE_FIX2(r, a, b, *);
    return r;
}
EE_INLINE float fdiv_ps2(float a, float b) {
    if (EE_UNLIKELY(b == 0.0f)) {
        uint32_t ua, ub, r;
        float f;
        memcpy(&ua, &a, 4);
        memcpy(&ub, &b, 4);
        r = ((ua ^ ub) & 0x80000000u) | 0x7F7FFFFFu;
        memcpy(&f, &r, 4);
        return f;
    }
    float r = a / b;
    if (EE_UNLIKELY(ee_fspecial(r))) r = ee_fclamp(ee_fclamp(a) / ee_fclamp(b));
    return r;
}
EE_INLINE float fsqrt_ps2(float a) { return sqrtf(fabsf(ee_fclamp(a))); }
EE_INLINE float frsqrt_ps2(float a, float b) {
    float s = sqrtf(fabsf(ee_fclamp(b)));
    return fdiv_ps2(a, s);
}
EE_INLINE float fmax_ps2(float a, float b) {
    a = ee_fclamp(a);
    b = ee_fclamp(b);
    return a > b ? a : b;
}
EE_INLINE float fmin_ps2(float a, float b) {
    a = ee_fclamp(a);
    b = ee_fclamp(b);
    return a < b ? a : b;
}
/* comparisons see Inf/NaN bit patterns as the huge numbers they are */
EE_INLINE int fcmp_eq(float a, float b) { return ee_fclamp(a) == ee_fclamp(b); }
EE_INLINE int fcmp_lt(float a, float b) { return ee_fclamp(a) < ee_fclamp(b); }
EE_INLINE int fcmp_le(float a, float b) { return ee_fclamp(a) <= ee_fclamp(b); }
EE_INLINE int32_t fcvtw_ps2(float f) {
    f = ee_fclamp(f);
    if (f >= 2147483648.0f) return 0x7FFFFFFF;
    if (f <= -2147483648.0f) return (int32_t)0x80000000;
    return (int32_t)f;
}
uint32_t ee_cfc1(int reg);
void ee_ctc1(int reg, uint32_t v);

/* ------------------------------------------------------------- VU0 */
#ifdef __cplusplus
}
#endif
#include "vu.h"
#ifdef __cplusplus
extern "C" {
#endif
#define VU0 g_vu0
void vu0_sync(void);
uint32_t vu0_cfc2(int reg);
void vu0_ctc2(int reg, uint32_t v);

/* ------------------------------------------------------------- MMI */
#define MMI_DECL(n) void mmi_##n(int rd, int rs, int rt, int sa);
MMI_DECL(paddb) MMI_DECL(paddh) MMI_DECL(paddw) MMI_DECL(padduw) MMI_DECL(paddsw) MMI_DECL(paddub)
MMI_DECL(psubb) MMI_DECL(psubh) MMI_DECL(psubw) MMI_DECL(psubsw) MMI_DECL(psubub)
MMI_DECL(pand) MMI_DECL(por) MMI_DECL(pxor) MMI_DECL(pnor)
MMI_DECL(pcgtb) MMI_DECL(pcgth) MMI_DECL(pcgtw) MMI_DECL(pceqb) MMI_DECL(pceqh) MMI_DECL(pceqw)
MMI_DECL(pmaxh) MMI_DECL(pminh) MMI_DECL(pmaxw) MMI_DECL(pminw) MMI_DECL(pabsh) MMI_DECL(pabsw)
MMI_DECL(pcpyh) MMI_DECL(pcpyld) MMI_DECL(pcpyud)
MMI_DECL(pextlb) MMI_DECL(pextlh) MMI_DECL(pextlw) MMI_DECL(pextub) MMI_DECL(pextuh) MMI_DECL(pextuw)
MMI_DECL(pinth) MMI_DECL(pinteh) MMI_DECL(ppacb) MMI_DECL(ppach) MMI_DECL(ppacw)
MMI_DECL(pexeh) MMI_DECL(pexew) MMI_DECL(prevh) MMI_DECL(pexch) MMI_DECL(pexcw) MMI_DECL(prot3w)
MMI_DECL(psllh) MMI_DECL(psrlh) MMI_DECL(psrah) MMI_DECL(psllw) MMI_DECL(psrlw) MMI_DECL(psraw)
MMI_DECL(psllvw) MMI_DECL(psrlvw) MMI_DECL(psravw)
MMI_DECL(phmadh) MMI_DECL(phmsbh) MMI_DECL(pmaddh) MMI_DECL(pmulth) MMI_DECL(pmultw) MMI_DECL(pmultuw)
MMI_DECL(pmaddw) MMI_DECL(pmadduw) MMI_DECL(pdivw) MMI_DECL(pdivuw) MMI_DECL(pdivbw)
MMI_DECL(pmfhi) MMI_DECL(pmflo) MMI_DECL(pmthi) MMI_DECL(pmtlo)
MMI_DECL(pmfhl_lw) MMI_DECL(pmfhl_uw) MMI_DECL(pmfhl_slw) MMI_DECL(pmfhl_lh) MMI_DECL(pmfhl_sh)
MMI_DECL(pmthl_lw) MMI_DECL(qfsrv) MMI_DECL(plzcw) MMI_DECL(pext5) MMI_DECL(ppac5) MMI_DECL(padsbh)
#undef MMI_DECL

/* ------------------------------------------------- system / control */
void ee_syscall(uint32_t pc);
void ee_break(uint32_t pc, uint32_t instr);
void ee_ei(void);
void ee_di(void);
uint32_t ee_mfc0(int reg);
void ee_mtc0(int reg, uint32_t v);
uint32_t ee_mfpc(int sel);
void ee_mtpc(int sel, uint32_t v);
int ee_cpcond0(void);
void ee_cache(int op, uint32_t addr);
void ee_bad_instruction(uint32_t pc, uint32_t instr);
void ee_eret(void);

/* indirect control flow */
void ee_call(uint32_t addr);           /* jalr */
void ee_jump(uint32_t addr);           /* jr to an unknown target (tail) */
void ee_call_unknown(uint32_t addr);   /* jal to a non-function */
void ee_jump_unknown(uint32_t addr);

/* Interrupt delivery points (back-edges of loops). */
extern volatile int g_ee_irq_pending;
void ee_service_interrupts(void);
#define IRQ_CHECK() do { if (EE_UNLIKELY(g_ee_irq_pending)) ee_service_interrupts(); } while (0)

/* Debug tracing of function entries (cmake -DEOD_TRACE_CALLS=ON). */
#ifdef EE_TRACE_CALLS
extern uint32_t g_ee_trace[256];
extern uint32_t g_ee_trace_pos;
#define EE_FUNC_ENTRY(a) (g_ee_trace[(g_ee_trace_pos++) & 255] = (a))
#else
#define EE_FUNC_ENTRY(a) ((void)0)
#endif

typedef void (*EEFunc)(void);
typedef struct EEFuncEntry {
    uint32_t addr;
    EEFunc fn;
} EEFuncEntry;
extern const EEFuncEntry ee_func_table[];
extern const unsigned ee_func_table_size;
extern const uint32_t ee_elf_crc32;
extern const uint32_t ee_elf_size;
EEFunc ee_lookup_func(uint32_t addr);

#ifdef __cplusplus
}
#endif
#endif
