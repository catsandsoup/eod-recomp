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

/* a MAC/status or clip flag result on its way through the FMAC pipeline */
struct VUFlagUpd {
    uint64_t ready; /* cycle it becomes visible */
    uint32_t val;   /* MAC flag, or the 6 new clip bits */
    uint32_t kind;  /* 0 MAC (and status), 1 clip */
};

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
    /* pipeline emulation (cycle accurate enough for software-pipelined
       microcode): FMAC results are ready 4 cycles after issue, Q/P when
       their divider/EFU finishes; reading a pending register stalls. */
    uint64_t cycle;
    float q_pending, p_pending;
    int q_busy, p_busy;
    uint64_t q_ready, p_ready;
    uint64_t vf_ready[32][4];
    /* micro mode: flag results become visible to the flag instructions
       (FMAND, FCAND, ...) 4 cycles after issue, like register results, but
       without interlocks; microcode relies on the delay (e.g. a back-face
       sign test 4 instructions before the FMAND reading it) */
    int flag_pipe;
    int fq_head, fq_n;
    struct VUFlagUpd fq[8];
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
