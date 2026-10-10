// memory.h - EE/IOP memory helpers for HLE code.
#pragma once
#include <cstdint>
#include <cstring>

#include "ee.h"

constexpr uint32_t IOP_RAM_SIZE = 0x200000;

extern uint8_t g_scratchpad[0x4000];
extern uint8_t *g_iop_ram;

void mem_init();

// Convenience accessors for HLE code (go through the full address decode).
inline uint32_t ee_r32(uint32_t a) { return MEM_LW(a); }
inline void ee_w32(uint32_t a, uint32_t v) { MEM_SW(a, v); }
inline uint16_t ee_r16(uint32_t a) { return (uint16_t)MEM_LH(a); }
inline uint8_t ee_r8(uint32_t a) { return (uint8_t)MEM_LB(a); }
inline void ee_w8(uint32_t a, uint8_t v) { MEM_SB(a, v); }
inline void ee_w16(uint32_t a, uint16_t v) { MEM_SH(a, v); }

// Copy between host and EE memory (handles RAM, scratchpad and mirrors).
inline void ee_read_block(uint32_t a, void *dst, uint32_t n) {
    uint8_t *d = static_cast<uint8_t *>(dst);
    while (n) {
        uint8_t *p = static_cast<uint8_t *>(ee_ptr(a));
        if (!p) {
            *d++ = (uint8_t)MEM_LB(a++);
            n--;
            continue;
        }
        // stay within the current 4KB page so mirrors/regions can't be crossed
        uint32_t chunk = 0x1000 - (a & 0xFFF);
        if (chunk > n) chunk = n;
        memcpy(d, p, chunk);
        d += chunk;
        a += chunk;
        n -= chunk;
    }
}

inline void ee_write_block(uint32_t a, const void *src, uint32_t n) {
    const uint8_t *s = static_cast<const uint8_t *>(src);
    while (n) {
        uint8_t *p = static_cast<uint8_t *>(ee_ptr(a));
        if (!p) {
            MEM_SB(a++, *s++);
            n--;
            continue;
        }
        uint32_t chunk = 0x1000 - (a & 0xFFF);
        if (chunk > n) chunk = n;
        memcpy(p, s, chunk);
        s += chunk;
        a += chunk;
        n -= chunk;
    }
}

// Reads a NUL terminated string from EE memory.
inline void ee_read_string(uint32_t a, char *out, size_t max) {
    size_t i = 0;
    for (; i + 1 < max; i++) {
        char c = (char)MEM_LB(a + (uint32_t)i);
        out[i] = c;
        if (!c) return;
    }
    out[i] = 0;
}
