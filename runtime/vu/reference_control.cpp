// COP2 control accesses for reference and development builds.
#include "ee.h"
#include "../hw/offload.h"
#include <cstring>

void vu_reference_control_access(int reg, bool write);
namespace {
// control register indices in vi[]
enum { REG_STATUS = 16, REG_MAC = 17, REG_CLIP = 18, REG_R = 20, REG_I = 21, REG_Q = 22, REG_P = 23,
       REG_TPC = 26, REG_CMSAR0 = 27, REG_FBRST = 28, REG_VPU_STAT = 29, REG_CMSAR1 = 31 };

uint32_t float_bits(float value) {
    uint32_t bits;
    std::memcpy(&bits,&value,sizeof bits);
    return bits;
}
}
extern "C" uint32_t vu0_cfc2(int reg) {
    vu_reference_control_access(reg, false);
    VUState *vu = &g_vu0;
    if (reg < 16) return vu->vi[reg] & 0xFFFF;
    switch (reg) {
    case REG_STATUS: return vu->status;
    case REG_MAC: return vu->mac;
    case REG_CLIP: return vu->clip;
    case REG_R: return vu->r;
    case REG_I: return float_bits(vu->i);
    case REG_Q: return float_bits(vu->q);
    case REG_P: return float_bits(vu->p);
    case REG_TPC: return vu->tpc / 8;
    case REG_CMSAR0: return vu->cmsar;
    case REG_VPU_STAT: return offload_busy() ? 0x100 : 0;  // VBS1: VU1 (on the worker) busy
    case REG_CMSAR1: offload_sync(); return g_vu1.cmsar;
    }
    return 0;
}

extern "C" void vu0_ctc2(int reg, uint32_t v) {
    vu_reference_control_access(reg, true);
    VUState *vu = &g_vu0;
    if (reg < 16) {
        if (reg) vu->vi[reg] = v & 0xffff;
        return;
    }
    switch (reg) {
    case REG_STATUS: vu->status = (vu->status & 0x3F) | (v & 0xFC0); break;
    case REG_MAC: break;
    case REG_CLIP: vu->clip = v & 0xFFFFFF; break;
    case REG_R: vu->r = 0x3F800000 | (v & 0x7FFFFF); break;
    case REG_I: memcpy(&vu->i, &v, 4); break;
    case REG_Q: memcpy(&vu->q, &v, 4); break;
    case REG_P: memcpy(&vu->p, &v, 4); break;
    case REG_CMSAR0: vu->cmsar = v & 0xFFFF; break;
    case REG_FBRST:
        if (v & 2) { /* reset VU0 */ }
        if (v & 0x200) { /* reset VU1 */ }
        break;
    case REG_CMSAR1: {
        const uint32_t ee_ra = g_ee.r[31].uw[0];
        offload_run([v, ee_ra] {
            g_vu1.cmsar = v & 0xFFFF;
            vu_execute(&g_vu1, (v & 0xFFFF) * 8, VuLaunch::Cmsar1, ee_ra);
        });
        break;
    }
    default:
        break;
    }
}
