// memory.cpp - EE address space.
#include <cstdlib>
#include <cstring>

#include "ee.h"
#include "log.h"
#include "memory.h"
#include "../hw/hw.h"

uint8_t *g_ram;
uint8_t g_scratchpad[0x4000];
uint8_t *g_iop_ram;

void mem_init() {
    // 32MB RAM + a guard so that overrunning helpers never fault on host.
    g_ram = static_cast<uint8_t *>(calloc(EE_RAM_SIZE + 0x10000, 1));
    g_iop_ram = static_cast<uint8_t *>(calloc(IOP_RAM_SIZE, 1));
    if (!g_ram || !g_iop_ram) fatal("out of memory");
}

enum Region { REG_RAM, REG_SPR, REG_HW, REG_VU, REG_GS, REG_ROM, REG_NONE };

static inline Region classify(uint32_t a, uint32_t &phys) {
    switch (a >> 28) {
    case 0x0:
    case 0x1:
        phys = a;
        break;
    case 0x2:
    case 0x3:
        phys = a & 0x0FFFFFFF;
        if (phys >= EE_RAM_SIZE) return REG_NONE;
        return REG_RAM;
    case 0x7:
        if (a < 0x70004000) {
            phys = a & 0x3FFF;
            return REG_SPR;
        }
        return REG_NONE;
    case 0x8:
    case 0x9:
    case 0xA:
    case 0xB:
        phys = a & 0x1FFFFFFF;
        break;
    default:
        return REG_NONE;
    }
    if (phys < EE_RAM_SIZE) return REG_RAM;
    if (phys >= 0x10000000 && phys < 0x10010000) return REG_HW;
    if (phys >= 0x11000000 && phys < 0x11010000) return REG_VU;
    if (phys >= 0x12000000 && phys < 0x12002000) return REG_GS;
    if (phys >= 0x1FC00000 && phys < 0x20000000) return REG_ROM;
    return REG_NONE;
}

void *ee_ptr(uint32_t a) {
    uint32_t phys;
    switch (classify(a, phys)) {
    case REG_RAM:
        return g_ram + phys;
    case REG_SPR:
        return g_scratchpad + phys;
    case REG_VU:
        return hw_vu_mem_ptr(phys);
    default:
        return nullptr;
    }
}

static uint8_t *direct(uint32_t a, uint32_t &phys, Region &r) {
    r = classify(a, phys);
    if (r == REG_RAM) return g_ram + phys;
    if (r == REG_SPR) return g_scratchpad + phys;
    if (r == REG_VU) return static_cast<uint8_t *>(hw_vu_mem_ptr(phys));
    return nullptr;
}

extern uint32_t g_vu_micro_gen[2];  // vu.cpp

// Writes into VU micro memory invalidate compiled micro programs.
static uint8_t *direct_w(uint32_t a, uint32_t &phys, Region &r) {
    uint8_t *d = direct(a, phys, r);
    if (r == REG_VU) {
        uint32_t off = phys - 0x11000000;
        if (off < 0x1000) g_vu_micro_gen[0]++;
        else if (off >= 0x8000 && off < 0xC000) g_vu_micro_gen[1]++;
    }
    return d;
}

static void bad_access(const char *what, uint32_t a) {
    static int count;
    if (count++ < 64)
        WARN("unmapped %s at %08x (last function %08x)", what, a, g_ee.pc);
}

extern "C" {

uint8_t ee_read8_slow(uint32_t a) {
    uint32_t p;
    Region r;
    if (uint8_t *d = direct(a, p, r)) return *d;
    if (r == REG_HW) return (uint8_t)hw_read32(p & ~3u) >> ((p & 3) * 8);
    if (r == REG_ROM) return 0;
    bad_access("read8", a);
    return 0;
}

uint16_t ee_read16_slow(uint32_t a) {
    uint32_t p;
    Region r;
    if (uint8_t *d = direct(a, p, r)) {
        uint16_t v;
        memcpy(&v, d, 2);
        return v;
    }
    if (r == REG_HW) return (uint16_t)(hw_read32(p & ~3u) >> ((p & 2) * 8));
    if (r == REG_ROM) return 0;
    bad_access("read16", a);
    return 0;
}

uint32_t ee_read32_slow(uint32_t a) {
    uint32_t p;
    Region r;
    if (uint8_t *d = direct(a, p, r)) {
        uint32_t v;
        memcpy(&v, d, 4);
        return v;
    }
    if (r == REG_HW) return hw_read32(p);
    if (r == REG_GS) return (uint32_t)gs_priv_read64(p & ~7u) >> ((p & 4) * 8);
    if (r == REG_ROM) return 0;
    bad_access("read32", a);
    return 0;
}

uint64_t ee_read64_slow(uint32_t a) {
    uint32_t p;
    Region r;
    if (uint8_t *d = direct(a, p, r)) {
        uint64_t v;
        memcpy(&v, d, 8);
        return v;
    }
    if (r == REG_HW) return hw_read32(p) | ((uint64_t)hw_read32(p + 4) << 32);
    if (r == REG_GS) return gs_priv_read64(p);
    if (r == REG_ROM) return 0;
    bad_access("read64", a);
    return 0;
}

void ee_read128_slow(uint32_t a, R128 *out) {
    uint32_t p;
    Region r;
    if (uint8_t *d = direct(a, p, r)) {
        memcpy(out, d, 16);
        return;
    }
    if (r == REG_HW) {
        hw_read128(p, out->uw);
        return;
    }
    memset(out, 0, 16);
    if (r != REG_ROM) bad_access("read128", a);
}

void ee_write8_slow(uint32_t a, uint8_t v) {
    uint32_t p;
    Region r;
    if (uint8_t *d = direct_w(a, p, r)) {
        *d = v;
        return;
    }
    if (r == REG_HW) {
        hw_write8(p, v);
        return;
    }
    bad_access("write8", a);
}

void ee_write16_slow(uint32_t a, uint16_t v) {
    uint32_t p;
    Region r;
    if (uint8_t *d = direct_w(a, p, r)) {
        memcpy(d, &v, 2);
        return;
    }
    if (r == REG_HW) {
        hw_write16(p, v);
        return;
    }
    bad_access("write16", a);
}

void ee_write32_slow(uint32_t a, uint32_t v) {
    uint32_t p;
    Region r;
    if (uint8_t *d = direct_w(a, p, r)) {
        memcpy(d, &v, 4);
        return;
    }
    if (r == REG_HW) {
        hw_write32(p, v);
        return;
    }
    if (r == REG_GS) {
        gs_priv_write32(p, v);
        return;
    }
    bad_access("write32", a);
}

void ee_write64_slow(uint32_t a, uint64_t v) {
    uint32_t p;
    Region r;
    if (uint8_t *d = direct_w(a, p, r)) {
        memcpy(d, &v, 8);
        return;
    }
    if (r == REG_HW) {
        hw_write64(p, v);
        return;
    }
    if (r == REG_GS) {
        gs_priv_write64(p, v);
        return;
    }
    bad_access("write64", a);
}

void ee_write128_slow(uint32_t a, const R128 *v) {
    uint32_t p;
    Region r;
    if (uint8_t *d = direct_w(a, p, r)) {
        memcpy(d, v, 16);
        return;
    }
    if (r == REG_HW) {
        hw_write128(p, v->uw);
        return;
    }
    if (r == REG_GS) {
        gs_priv_write64(p, v->ud[0]);
        return;
    }
    bad_access("write128", a);
}

}  // extern "C"
