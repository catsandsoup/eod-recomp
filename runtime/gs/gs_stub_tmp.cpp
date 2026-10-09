// Temporary GS stub.
#include "gs.h"
#include "../hw/hw.h"
#include "../core/log.h"
void gs_init() {}
void gif_transfer(int, const uint8_t *, uint32_t) {}
uint32_t gif_reg_read(uint32_t) { return 0; }
void gif_reg_write(uint32_t, uint32_t) {}
void gs_set_crt(int a, int b, int c) { LOG(LOG_GS, "SetGsCrt %d %d %d", a, b, c); }
static uint32_t imr = 0xFF00;
uint32_t gs_get_imr() { return imr; }
void gs_put_imr(uint32_t v) { imr = v; }
void gs_vblank() {}
bool gs_screenshot(const char *) { return false; }
static uint64_t csr = 0;
uint64_t gs_priv_read64(uint32_t p) { if ((p & 0xFFFF) == 0x1000) { csr ^= 0x2000; return csr | 0x8; } return 0; }
void gs_priv_write32(uint32_t, uint32_t) {}
void gs_priv_write64(uint32_t, uint64_t) {}
