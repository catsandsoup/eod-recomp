// sif.h - SIF (EE<->IOP interface) and the HLE IOP behind it.
#pragma once
#include <cstdint>

void sif_init();
uint32_t sif_set_dma(uint32_t list, int count);
int sif_dma_stat(int id);
void sif_set_dchain();
void sif_set_reg(uint32_t reg, uint32_t v);
uint32_t sif_get_reg(uint32_t reg);
uint32_t sif_read_reg(uint32_t off);
void sif_write_reg(uint32_t off, uint32_t v);
// Called regularly (vblank) to let IOP-side HLE make progress.
void sif_update();
// Called by the kernel after the EE's SIF0 DMA handlers ran.
void sif_after_sif0_irq();
