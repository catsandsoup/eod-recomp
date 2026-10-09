// hw.h - EE hardware registers (0x10000000-0x1000FFFF) and DMA controller.
#pragma once
#include <cstdint>

void hw_init();

uint32_t hw_read32(uint32_t phys);
void hw_read128(uint32_t phys, uint32_t out[4]);
void hw_write8(uint32_t phys, uint8_t v);
void hw_write16(uint32_t phys, uint16_t v);
void hw_write32(uint32_t phys, uint32_t v);
void hw_write64(uint32_t phys, uint64_t v);
void hw_write128(uint32_t phys, const uint32_t v[4]);

// VU0/VU1 memories mapped at 0x11000000.
void *hw_vu_mem_ptr(uint32_t phys);

int hw_cpcond0();
bool hw_dmac_channel_irq_enabled(int ch);
int hw_dmac_set_channel_irq(int ch, bool on);

// GS privileged registers (0x12000000), implemented in gs/gs.cpp
uint64_t gs_priv_read64(uint32_t phys);
void gs_priv_write32(uint32_t phys, uint32_t v);
void gs_priv_write64(uint32_t phys, uint64_t v);

// DMA channels
enum DmaChannel {
    DMA_VIF0 = 0,
    DMA_VIF1 = 1,
    DMA_GIF = 2,
    DMA_IPU_FROM = 3,
    DMA_IPU_TO = 4,
    DMA_SIF0 = 5,
    DMA_SIF1 = 6,
    DMA_SIF2 = 7,
    DMA_SPR_FROM = 8,
    DMA_SPR_TO = 9,
};

// Marks channel finished: clears STR, sets D_STAT.CIS, raises the interrupt.
void dmac_channel_done(int ch);
