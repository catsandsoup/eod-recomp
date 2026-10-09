/*
 * vu.h - Vector Unit state shared by VU0 (macro + micro mode) and VU1.
 */
#ifndef VU_H
#define VU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union VUReg {
    float f[4];
    uint32_t u[4];
    int32_t s[4];
} VUReg;

struct VUState {
    VUReg vf[32];
    uint32_t vi[32];        /* 0-15 integer regs; 16+ control registers */
    VUReg acc;
    float q, p;
    float i;
    uint32_t r;             /* random register (23-bit mantissa) */
    uint32_t status;        /* status flag */
    uint32_t mac;           /* MAC flag */
    uint32_t clip;          /* clipping flag */
    uint32_t tpc;           /* micro program counter (in instructions) */
    uint32_t cmsar;         /* start address for vcallmsr */
    int running;            /* micro program executing */
    int index;              /* 0 or 1 */
    uint8_t *mem;           /* data memory */
    uint32_t mem_mask;      /* 0xFFF (VU0) or 0x3FFF (VU1) */
    uint8_t *micro;         /* micro memory */
    uint32_t micro_mask;    /* 0xFFF (VU0) or 0x3FFF (VU1) */
    /* pipeline emulation for q/p */
    float q_pending, p_pending;
    int q_delay, p_delay;
    int ebit_delay;
};

typedef struct VUState VUState;
extern VUState g_vu0;
extern VUState g_vu1;

/* Execute a micro program starting at tpc until the E bit retires. */
void vu_execute(VUState *vu, uint32_t start_pc);
/* Run a single macro-mode (COP2) instruction on VU0. */
void vu0_macro(uint32_t instr);

#ifdef __cplusplus
}
#endif
#endif
