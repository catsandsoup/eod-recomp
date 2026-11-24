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

/* Run a single macro-mode (COP2) instruction on VU0. */
#ifdef EOD_R7_RELEASE
void eod_unretired_vu0_macro(uint32_t instr);
#define vu0_macro(word) eod_unretired_vu0_macro(word)
#else
void vu0_macro(uint32_t instr);
#endif

#ifdef __cplusplus
}

enum class VuLaunch { Direct, Mscal, Mscalf, Mscnt, Vcallms, Vcallmsr, Cmsar1 };
#ifdef EOD_R7_RELEASE
void vu_reject_launch(const VUState& vu, uint32_t address, VuLaunch launch);
#else
/* Execute a micro program starting at tpc until the E bit retires. */
void vu_execute(VUState *vu, uint32_t start_pc, VuLaunch launch = VuLaunch::Direct,
                uint32_t ee_ra = 0);
#endif
/* Retire the verified VU1 MSCAL E-bit NOP and delay pair at 0x24D8. */
bool vu_retire_mscal_nop(VUState *vu, uint32_t start_pc);
/* Retire the verified VU1 A+D XGKICK-from-TOP program at 0x2538. */
namespace gs { struct MaterialState; }
bool vu_retire_xgkick_top(VUState *vu, uint32_t start_pc, uint32_t top, const gs::MaterialState *material);
/* Retire the verified VU1 GIFtag template setup program at 0x25F0. */
bool vu_retire_giftag_setup(VUState *vu, uint32_t start_pc, uint32_t top);
/* Retire the verified VU1 matrix/lighting setup program at 0x0000 (0 XGKICKs, bypass-only). */
bool vu_retire_setup_0000(VUState *vu, uint32_t start_pc);
/* Report producer-only PATH1 transport counts after the VIF worker drains. */
void vu_report_world_strip_transport(void);
void vu_report_census();
#endif
#endif
