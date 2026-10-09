// ipu.cpp - Image Processing Unit. The game uses it for FMV (MPEG-2 .pss
// files); not emulated yet: commands complete immediately with no output.
#include "ipu.h"

#include "../core/log.h"

void ipu_init() {}
uint32_t ipu_read32(uint32_t off) {
    LOG(LOG_HW, "IPU read %04x", off);
    return 0;
}
void ipu_write32(uint32_t off, uint32_t v) { LOG(LOG_HW, "IPU write %04x = %08x", off, v); }
void ipu_in_fifo(const uint8_t *, uint32_t) {}
uint32_t ipu_out_fifo(uint8_t *, uint32_t) { return 0; }
