/*
 * mmi.c - R5900 multimedia (128-bit parallel) instructions.
 * Signature: mmi_xxx(rd, rs, rt, sa).  Operands are read into temporaries
 * first, so rd may alias rs/rt.
 */
#include "ee.h"

#define LOAD R128 s = R[rs], t = R[rt], d; (void)s; (void)t; (void)sa; (void)d
#define STORE if (rd) R[rd] = d

static inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
static inline int32_t sat32(int64_t v) {
    return v > 2147483647LL ? 2147483647 : v < -2147483648LL ? (int32_t)0x80000000 : (int32_t)v;
}

void mmi_paddb(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 16; i++) d.ub[i] = s.ub[i] + t.ub[i]; STORE; }
void mmi_paddh(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 8; i++) d.uh[i] = s.uh[i] + t.uh[i]; STORE; }
void mmi_paddw(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 4; i++) d.uw[i] = s.uw[i] + t.uw[i]; STORE; }
void mmi_padduw(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int i = 0; i < 4; i++) { uint64_t v = (uint64_t)s.uw[i] + t.uw[i]; d.uw[i] = v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v; }
    STORE;
}
void mmi_paddsw(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 4; i++) d.sw[i] = sat32((int64_t)s.sw[i] + t.sw[i]); STORE; }
void mmi_paddub(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int i = 0; i < 16; i++) { int v = s.ub[i] + t.ub[i]; d.ub[i] = v > 255 ? 255 : v; }
    STORE;
}
void mmi_psubb(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 16; i++) d.ub[i] = s.ub[i] - t.ub[i]; STORE; }
void mmi_psubh(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 8; i++) d.uh[i] = s.uh[i] - t.uh[i]; STORE; }
void mmi_psubw(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 4; i++) d.uw[i] = s.uw[i] - t.uw[i]; STORE; }
void mmi_psubsw(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 4; i++) d.sw[i] = sat32((int64_t)s.sw[i] - t.sw[i]); STORE; }
void mmi_psubub(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int i = 0; i < 16; i++) { int v = s.ub[i] - t.ub[i]; d.ub[i] = v < 0 ? 0 : v; }
    STORE;
}
void mmi_padsbh(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int i = 0; i < 4; i++) d.uh[i] = s.uh[i] - t.uh[i];
    for (int i = 4; i < 8; i++) d.uh[i] = s.uh[i] + t.uh[i];
    STORE;
}

void mmi_pand(int rd, int rs, int rt, int sa) { LOAD; d.ud[0] = s.ud[0] & t.ud[0]; d.ud[1] = s.ud[1] & t.ud[1]; STORE; }
void mmi_por(int rd, int rs, int rt, int sa) { LOAD; d.ud[0] = s.ud[0] | t.ud[0]; d.ud[1] = s.ud[1] | t.ud[1]; STORE; }
void mmi_pxor(int rd, int rs, int rt, int sa) { LOAD; d.ud[0] = s.ud[0] ^ t.ud[0]; d.ud[1] = s.ud[1] ^ t.ud[1]; STORE; }
void mmi_pnor(int rd, int rs, int rt, int sa) { LOAD; d.ud[0] = ~(s.ud[0] | t.ud[0]); d.ud[1] = ~(s.ud[1] | t.ud[1]); STORE; }

void mmi_pcgtb(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 16; i++) d.ub[i] = s.sb[i] > t.sb[i] ? 0xFF : 0; STORE; }
void mmi_pcgth(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 8; i++) d.uh[i] = s.sh[i] > t.sh[i] ? 0xFFFF : 0; STORE; }
void mmi_pcgtw(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 4; i++) d.uw[i] = s.sw[i] > t.sw[i] ? 0xFFFFFFFFu : 0; STORE; }
void mmi_pceqb(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 16; i++) d.ub[i] = s.ub[i] == t.ub[i] ? 0xFF : 0; STORE; }
void mmi_pceqh(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 8; i++) d.uh[i] = s.uh[i] == t.uh[i] ? 0xFFFF : 0; STORE; }
void mmi_pceqw(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 4; i++) d.uw[i] = s.uw[i] == t.uw[i] ? 0xFFFFFFFFu : 0; STORE; }

void mmi_pmaxh(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 8; i++) d.sh[i] = s.sh[i] > t.sh[i] ? s.sh[i] : t.sh[i]; STORE; }
void mmi_pminh(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 8; i++) d.sh[i] = s.sh[i] < t.sh[i] ? s.sh[i] : t.sh[i]; STORE; }
void mmi_pmaxw(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 4; i++) d.sw[i] = s.sw[i] > t.sw[i] ? s.sw[i] : t.sw[i]; STORE; }
void mmi_pminw(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 4; i++) d.sw[i] = s.sw[i] < t.sw[i] ? s.sw[i] : t.sw[i]; STORE; }
void mmi_pabsh(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int i = 0; i < 8; i++) d.sh[i] = t.sh[i] == -32768 ? 32767 : (t.sh[i] < 0 ? -t.sh[i] : t.sh[i]);
    STORE;
}
void mmi_pabsw(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int i = 0; i < 4; i++) d.sw[i] = t.sw[i] == (int32_t)0x80000000 ? 0x7FFFFFFF : (t.sw[i] < 0 ? -t.sw[i] : t.sw[i]);
    STORE;
}

void mmi_pcpyh(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int i = 0; i < 4; i++) { d.uh[i] = t.uh[0]; d.uh[4 + i] = t.uh[4]; }
    STORE;
}
void mmi_pcpyld(int rd, int rs, int rt, int sa) { LOAD; d.ud[0] = t.ud[0]; d.ud[1] = s.ud[0]; STORE; }
void mmi_pcpyud(int rd, int rs, int rt, int sa) { LOAD; d.ud[0] = s.ud[1]; d.ud[1] = t.ud[1]; STORE; }

void mmi_pextlb(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int i = 0; i < 8; i++) { d.ub[2 * i] = t.ub[i]; d.ub[2 * i + 1] = s.ub[i]; }
    STORE;
}
void mmi_pextub(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int i = 0; i < 8; i++) { d.ub[2 * i] = t.ub[8 + i]; d.ub[2 * i + 1] = s.ub[8 + i]; }
    STORE;
}
void mmi_pextlh(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int i = 0; i < 4; i++) { d.uh[2 * i] = t.uh[i]; d.uh[2 * i + 1] = s.uh[i]; }
    STORE;
}
void mmi_pextuh(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int i = 0; i < 4; i++) { d.uh[2 * i] = t.uh[4 + i]; d.uh[2 * i + 1] = s.uh[4 + i]; }
    STORE;
}
void mmi_pextlw(int rd, int rs, int rt, int sa) {
    LOAD;
    d.uw[0] = t.uw[0]; d.uw[1] = s.uw[0]; d.uw[2] = t.uw[1]; d.uw[3] = s.uw[1];
    STORE;
}
void mmi_pextuw(int rd, int rs, int rt, int sa) {
    LOAD;
    d.uw[0] = t.uw[2]; d.uw[1] = s.uw[2]; d.uw[2] = t.uw[3]; d.uw[3] = s.uw[3];
    STORE;
}
void mmi_pinth(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int i = 0; i < 4; i++) { d.uh[2 * i] = t.uh[i]; d.uh[2 * i + 1] = s.uh[4 + i]; }
    STORE;
}
void mmi_pinteh(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int i = 0; i < 4; i++) { d.uh[2 * i] = t.uh[2 * i]; d.uh[2 * i + 1] = s.uh[2 * i]; }
    STORE;
}
void mmi_ppacb(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int i = 0; i < 8; i++) { d.ub[i] = t.ub[2 * i]; d.ub[8 + i] = s.ub[2 * i]; }
    STORE;
}
void mmi_ppach(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int i = 0; i < 4; i++) { d.uh[i] = t.uh[2 * i]; d.uh[4 + i] = s.uh[2 * i]; }
    STORE;
}
void mmi_ppacw(int rd, int rs, int rt, int sa) {
    LOAD;
    d.uw[0] = t.uw[0]; d.uw[1] = t.uw[2]; d.uw[2] = s.uw[0]; d.uw[3] = s.uw[2];
    STORE;
}
void mmi_pexeh(int rd, int rs, int rt, int sa) {
    LOAD;
    d.uh[0] = t.uh[2]; d.uh[1] = t.uh[1]; d.uh[2] = t.uh[0]; d.uh[3] = t.uh[3];
    d.uh[4] = t.uh[6]; d.uh[5] = t.uh[5]; d.uh[6] = t.uh[4]; d.uh[7] = t.uh[7];
    STORE;
}
void mmi_pexew(int rd, int rs, int rt, int sa) {
    LOAD;
    d.uw[0] = t.uw[2]; d.uw[1] = t.uw[1]; d.uw[2] = t.uw[0]; d.uw[3] = t.uw[3];
    STORE;
}
void mmi_prevh(int rd, int rs, int rt, int sa) {
    LOAD;
    d.uh[0] = t.uh[3]; d.uh[1] = t.uh[2]; d.uh[2] = t.uh[1]; d.uh[3] = t.uh[0];
    d.uh[4] = t.uh[7]; d.uh[5] = t.uh[6]; d.uh[6] = t.uh[5]; d.uh[7] = t.uh[4];
    STORE;
}
void mmi_pexch(int rd, int rs, int rt, int sa) {
    LOAD;
    d.uh[0] = t.uh[0]; d.uh[1] = t.uh[2]; d.uh[2] = t.uh[1]; d.uh[3] = t.uh[3];
    d.uh[4] = t.uh[4]; d.uh[5] = t.uh[6]; d.uh[6] = t.uh[5]; d.uh[7] = t.uh[7];
    STORE;
}
void mmi_pexcw(int rd, int rs, int rt, int sa) {
    LOAD;
    d.uw[0] = t.uw[0]; d.uw[1] = t.uw[2]; d.uw[2] = t.uw[1]; d.uw[3] = t.uw[3];
    STORE;
}
void mmi_prot3w(int rd, int rs, int rt, int sa) {
    LOAD;
    d.uw[0] = t.uw[1]; d.uw[1] = t.uw[2]; d.uw[2] = t.uw[0]; d.uw[3] = t.uw[3];
    STORE;
}

void mmi_psllh(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 8; i++) d.uh[i] = (uint16_t)(t.uh[i] << (sa & 15)); STORE; }
void mmi_psrlh(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 8; i++) d.uh[i] = t.uh[i] >> (sa & 15); STORE; }
void mmi_psrah(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 8; i++) d.sh[i] = t.sh[i] >> (sa & 15); STORE; }
void mmi_psllw(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 4; i++) d.uw[i] = t.uw[i] << (sa & 31); STORE; }
void mmi_psrlw(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 4; i++) d.uw[i] = t.uw[i] >> (sa & 31); STORE; }
void mmi_psraw(int rd, int rs, int rt, int sa) { LOAD; for (int i = 0; i < 4; i++) d.sw[i] = t.sw[i] >> (sa & 31); STORE; }
void mmi_psllvw(int rd, int rs, int rt, int sa) {
    LOAD;
    d.sd[0] = (int32_t)(t.uw[0] << (s.uw[0] & 31));
    d.sd[1] = (int32_t)(t.uw[2] << (s.uw[2] & 31));
    STORE;
}
void mmi_psrlvw(int rd, int rs, int rt, int sa) {
    LOAD;
    d.sd[0] = (int32_t)(t.uw[0] >> (s.uw[0] & 31));
    d.sd[1] = (int32_t)(t.uw[2] >> (s.uw[2] & 31));
    STORE;
}
void mmi_psravw(int rd, int rs, int rt, int sa) {
    LOAD;
    d.sd[0] = t.sw[0] >> (s.uw[0] & 31);
    d.sd[1] = t.sw[2] >> (s.uw[2] & 31);
    STORE;
}

/* HI/LO based operations */
void mmi_phmadh(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int k = 0; k < 2; k++) {
        int32_t a = s.sh[4 * k + 1] * t.sh[4 * k + 1] + s.sh[4 * k] * t.sh[4 * k];
        int32_t b = s.sh[4 * k + 3] * t.sh[4 * k + 3] + s.sh[4 * k + 2] * t.sh[4 * k + 2];
        g_ee.lo.sw[2 * k] = a;
        g_ee.lo.sw[2 * k + 1] = s.sh[4 * k + 1] * t.sh[4 * k + 1];
        g_ee.hi.sw[2 * k] = b;
        g_ee.hi.sw[2 * k + 1] = s.sh[4 * k + 3] * t.sh[4 * k + 3];
        d.sw[2 * k] = a;
        d.sw[2 * k + 1] = b;
    }
    STORE;
}
void mmi_phmsbh(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int k = 0; k < 2; k++) {
        int32_t a = s.sh[4 * k + 1] * t.sh[4 * k + 1] - s.sh[4 * k] * t.sh[4 * k];
        int32_t b = s.sh[4 * k + 3] * t.sh[4 * k + 3] - s.sh[4 * k + 2] * t.sh[4 * k + 2];
        g_ee.lo.sw[2 * k] = a;
        g_ee.lo.sw[2 * k + 1] = ~(s.sh[4 * k + 1] * t.sh[4 * k + 1]);
        g_ee.hi.sw[2 * k] = b;
        g_ee.hi.sw[2 * k + 1] = ~(s.sh[4 * k + 3] * t.sh[4 * k + 3]);
        d.sw[2 * k] = a;
        d.sw[2 * k + 1] = b;
    }
    STORE;
}
void mmi_pmulth(int rd, int rs, int rt, int sa) {
    LOAD;
    int32_t p[8];
    for (int i = 0; i < 8; i++) p[i] = s.sh[i] * t.sh[i];
    g_ee.lo.sw[0] = p[0]; g_ee.lo.sw[1] = p[1]; g_ee.hi.sw[0] = p[2]; g_ee.hi.sw[1] = p[3];
    g_ee.lo.sw[2] = p[4]; g_ee.lo.sw[3] = p[5]; g_ee.hi.sw[2] = p[6]; g_ee.hi.sw[3] = p[7];
    d.sw[0] = p[0]; d.sw[1] = p[2]; d.sw[2] = p[4]; d.sw[3] = p[6];
    STORE;
}
void mmi_pmaddh(int rd, int rs, int rt, int sa) {
    LOAD;
    int32_t p[8];
    p[0] = g_ee.lo.sw[0] + s.sh[0] * t.sh[0]; p[1] = g_ee.lo.sw[1] + s.sh[1] * t.sh[1];
    p[2] = g_ee.hi.sw[0] + s.sh[2] * t.sh[2]; p[3] = g_ee.hi.sw[1] + s.sh[3] * t.sh[3];
    p[4] = g_ee.lo.sw[2] + s.sh[4] * t.sh[4]; p[5] = g_ee.lo.sw[3] + s.sh[5] * t.sh[5];
    p[6] = g_ee.hi.sw[2] + s.sh[6] * t.sh[6]; p[7] = g_ee.hi.sw[3] + s.sh[7] * t.sh[7];
    g_ee.lo.sw[0] = p[0]; g_ee.lo.sw[1] = p[1]; g_ee.hi.sw[0] = p[2]; g_ee.hi.sw[1] = p[3];
    g_ee.lo.sw[2] = p[4]; g_ee.lo.sw[3] = p[5]; g_ee.hi.sw[2] = p[6]; g_ee.hi.sw[3] = p[7];
    d.sw[0] = p[0]; d.sw[1] = p[2]; d.sw[2] = p[4]; d.sw[3] = p[6];
    STORE;
}
void mmi_pmultw(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int k = 0; k < 2; k++) {
        int64_t p = (int64_t)s.sw[2 * k] * t.sw[2 * k];
        g_ee.lo.sd[k] = (int32_t)p;
        g_ee.hi.sd[k] = (int32_t)(p >> 32);
        d.sd[k] = p;
    }
    STORE;
}
void mmi_pmultuw(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int k = 0; k < 2; k++) {
        uint64_t p = (uint64_t)s.uw[2 * k] * t.uw[2 * k];
        g_ee.lo.sd[k] = (int32_t)p;
        g_ee.hi.sd[k] = (int32_t)(p >> 32);
        d.ud[k] = p;
    }
    STORE;
}
void mmi_pmaddw(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int k = 0; k < 2; k++) {
        int64_t acc = (int64_t)(((uint64_t)g_ee.hi.uw[2 * k] << 32) | g_ee.lo.uw[2 * k]);
        int64_t p = acc + (int64_t)s.sw[2 * k] * t.sw[2 * k];
        g_ee.lo.sd[k] = (int32_t)p;
        g_ee.hi.sd[k] = (int32_t)(p >> 32);
        d.sd[k] = p;
    }
    STORE;
}
void mmi_pmadduw(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int k = 0; k < 2; k++) {
        uint64_t acc = ((uint64_t)g_ee.hi.uw[2 * k] << 32) | g_ee.lo.uw[2 * k];
        uint64_t p = acc + (uint64_t)s.uw[2 * k] * t.uw[2 * k];
        g_ee.lo.sd[k] = (int32_t)p;
        g_ee.hi.sd[k] = (int32_t)(p >> 32);
        d.ud[k] = p;
    }
    STORE;
}
void mmi_pdivw(int rd, int rs, int rt, int sa) {
    LOAD;
    (void)rd;
    for (int k = 0; k < 2; k++) {
        int32_t n = s.sw[2 * k], dd = t.sw[2 * k];
        if (dd == 0) { g_ee.lo.sd[k] = n < 0 ? 1 : -1; g_ee.hi.sd[k] = n; }
        else if (n == (int32_t)0x80000000 && dd == -1) { g_ee.lo.sd[k] = n; g_ee.hi.sd[k] = 0; }
        else { g_ee.lo.sd[k] = n / dd; g_ee.hi.sd[k] = n % dd; }
    }
}
void mmi_pdivuw(int rd, int rs, int rt, int sa) {
    LOAD;
    (void)rd;
    for (int k = 0; k < 2; k++) {
        uint32_t n = s.uw[2 * k], dd = t.uw[2 * k];
        if (dd == 0) { g_ee.lo.sd[k] = -1; g_ee.hi.sd[k] = (int32_t)n; }
        else { g_ee.lo.sd[k] = (int32_t)(n / dd); g_ee.hi.sd[k] = (int32_t)(n % dd); }
    }
}
void mmi_pdivbw(int rd, int rs, int rt, int sa) {
    LOAD;
    (void)rd;
    int16_t dd = t.sh[0];
    for (int k = 0; k < 4; k++) {
        int32_t n = s.sw[k];
        if (dd == 0) { g_ee.lo.sw[k] = n < 0 ? 1 : -1; g_ee.hi.sw[k] = n; }
        else if (n == (int32_t)0x80000000 && dd == -1) { g_ee.lo.sw[k] = n; g_ee.hi.sw[k] = 0; }
        else { g_ee.lo.sw[k] = n / dd; g_ee.hi.sw[k] = (int16_t)(n % dd); }
    }
}
void mmi_pmfhi(int rd, int rs, int rt, int sa) { (void)rs; (void)rt; (void)sa; if (rd) R[rd] = g_ee.hi; }
void mmi_pmflo(int rd, int rs, int rt, int sa) { (void)rs; (void)rt; (void)sa; if (rd) R[rd] = g_ee.lo; }
void mmi_pmthi(int rd, int rs, int rt, int sa) { (void)rd; (void)rt; (void)sa; g_ee.hi = R[rs]; }
void mmi_pmtlo(int rd, int rs, int rt, int sa) { (void)rd; (void)rt; (void)sa; g_ee.lo = R[rs]; }
void mmi_pmfhl_lw(int rd, int rs, int rt, int sa) {
    R128 d;
    (void)rs; (void)rt; (void)sa;
    d.uw[0] = g_ee.lo.uw[0]; d.uw[1] = g_ee.hi.uw[0]; d.uw[2] = g_ee.lo.uw[2]; d.uw[3] = g_ee.hi.uw[2];
    STORE;
}
void mmi_pmfhl_uw(int rd, int rs, int rt, int sa) {
    R128 d;
    (void)rs; (void)rt; (void)sa;
    d.uw[0] = g_ee.lo.uw[1]; d.uw[1] = g_ee.hi.uw[1]; d.uw[2] = g_ee.lo.uw[3]; d.uw[3] = g_ee.hi.uw[3];
    STORE;
}
void mmi_pmfhl_slw(int rd, int rs, int rt, int sa) {
    R128 d;
    (void)rs; (void)rt; (void)sa;
    for (int k = 0; k < 2; k++) {
        int64_t v = (int64_t)(((uint64_t)g_ee.hi.uw[2 * k] << 32) | g_ee.lo.uw[2 * k]);
        d.sd[k] = sat32(v);
    }
    STORE;
}
void mmi_pmfhl_lh(int rd, int rs, int rt, int sa) {
    R128 d;
    (void)rs; (void)rt; (void)sa;
    d.uh[0] = g_ee.lo.uh[0]; d.uh[1] = g_ee.lo.uh[2]; d.uh[2] = g_ee.hi.uh[0]; d.uh[3] = g_ee.hi.uh[2];
    d.uh[4] = g_ee.lo.uh[4]; d.uh[5] = g_ee.lo.uh[6]; d.uh[6] = g_ee.hi.uh[4]; d.uh[7] = g_ee.hi.uh[6];
    STORE;
}
void mmi_pmfhl_sh(int rd, int rs, int rt, int sa) {
    R128 d;
    (void)rs; (void)rt; (void)sa;
    d.sh[0] = sat16(g_ee.lo.sw[0]); d.sh[1] = sat16(g_ee.lo.sw[1]); d.sh[2] = sat16(g_ee.hi.sw[0]); d.sh[3] = sat16(g_ee.hi.sw[1]);
    d.sh[4] = sat16(g_ee.lo.sw[2]); d.sh[5] = sat16(g_ee.lo.sw[3]); d.sh[6] = sat16(g_ee.hi.sw[2]); d.sh[7] = sat16(g_ee.hi.sw[3]);
    STORE;
}
void mmi_pmthl_lw(int rd, int rs, int rt, int sa) {
    R128 s = R[rs];
    (void)rd; (void)rt; (void)sa;
    g_ee.lo.uw[0] = s.uw[0]; g_ee.hi.uw[0] = s.uw[1]; g_ee.lo.uw[2] = s.uw[2]; g_ee.hi.uw[2] = s.uw[3];
}

void mmi_qfsrv(int rd, int rs, int rt, int sa) {
    LOAD;
    /* 256-bit {rs:rt} >> SA, low 128 bits */
    uint32_t sh = g_ee.sa & 127;
    uint64_t w[4] = {t.ud[0], t.ud[1], s.ud[0], s.ud[1]};
    uint32_t q = sh / 64, b = sh % 64;
    for (int i = 0; i < 2; i++) {
        uint64_t lo = w[i + q];
        uint64_t hi = (i + q + 1 < 4) ? w[i + q + 1] : 0;
        d.ud[i] = b ? (lo >> b) | (hi << (64 - b)) : lo;
    }
    STORE;
}

void mmi_plzcw(int rd, int rs, int rt, int sa) {
    R128 s = R[rs];
    (void)rt; (void)sa;
    for (int k = 0; k < 2; k++) {
        uint32_t v = s.uw[k];
        if (v & 0x80000000u) v = ~v;
        int n = 0;
        while (n < 32 && !(v & (0x80000000u >> n))) n++;
        if (rd) R[rd].uw[k] = n - 1;
    }
}

void mmi_pext5(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int i = 0; i < 4; i++) {
        uint32_t v = t.uw[i];
        d.uw[i] = ((v & 0x1F) << 3) | ((v & 0x3E0) << 6) | ((v & 0x7C00) << 9) | ((v & 0x8000) << 16);
    }
    STORE;
}
void mmi_ppac5(int rd, int rs, int rt, int sa) {
    LOAD;
    for (int i = 0; i < 4; i++) {
        uint32_t v = t.uw[i];
        d.uw[i] = ((v >> 3) & 0x1F) | ((v >> 6) & 0x3E0) | ((v >> 9) & 0x7C00) | ((v >> 16) & 0x8000);
    }
    STORE;
}
