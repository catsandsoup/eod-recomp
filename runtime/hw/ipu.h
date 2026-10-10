// ipu.h - Image Processing Unit (MPEG decoding for FMV).
#pragma once
#include <cstdint>

void ipu_init();
uint32_t ipu_read32(uint32_t off);
void ipu_write32(uint32_t off, uint32_t v);
void ipu_in_fifo(const uint8_t *data, uint32_t qwc);
// Returns the number of quadwords produced.
uint32_t ipu_out_fifo(uint8_t *dst, uint32_t qwc);
// A DMA channel of the IPU (3 or 4) started: move data and resume a waiting command.
void ipu_dma_kick();
