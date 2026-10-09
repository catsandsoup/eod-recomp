// Temporary VIF/IPU stubs.
#include "vif.h"
#include "../hw/ipu.h"
#include "../core/log.h"
void vif_init() {}
void vif_write(int, const uint32_t *, uint32_t) {}
uint32_t vif_reg_read(int, uint32_t) { return 0; }
void vif_reg_write(int, uint32_t, uint32_t) {}
void vif1_read_fifo(uint32_t out[4]) { out[0] = out[1] = out[2] = out[3] = 0; }
void ipu_init() {}
uint32_t ipu_read32(uint32_t) { return 0; }
void ipu_write32(uint32_t, uint32_t) {}
void ipu_in_fifo(const uint8_t *, uint32_t) {}
uint32_t ipu_out_fifo(uint8_t *, uint32_t) { return 0; }
