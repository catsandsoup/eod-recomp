// spu2.h - SPU2 sound processor (2 cores x 24 voices, 2MB sound RAM),
// programmed at the level of the IOP's libsd (sceSd*) API.
#pragma once
#include <cstdint>

void spu2_init();

// libsd parameter entries: (param << 8 | 0x80 for core params) | voice << 1 | core
void spu2_set_param(uint16_t entry, uint16_t value);
uint16_t spu2_get_param(uint16_t entry);
void spu2_set_switch(uint16_t entry, uint32_t value);
uint32_t spu2_get_switch(uint16_t entry);
void spu2_set_addr(uint16_t entry, uint32_t value);
uint32_t spu2_get_addr(uint16_t entry);
void spu2_set_core_attr(uint16_t entry, uint16_t value);
uint16_t spu2_get_core_attr(uint16_t entry);

// Voice transfer: IOP memory <-> sound RAM (completes immediately).
int spu2_voice_trans(int channel, int mode, uint32_t iop_addr, uint32_t spu_addr, uint32_t size);
// Block transfer: AutoDMA PCM input from IOP memory (played in real time).
int spu2_block_trans(int channel, int mode, uint32_t iop_addr, uint32_t size, uint32_t start);
uint32_t spu2_block_trans_status(int channel);

// Interrupt callbacks: transfer completion (per channel) and SPU2 IRQ (per core).
void spu2_set_callbacks(void (*trans_done)(int channel), void (*irq)(int core));

// Produces the audio due since the last call (wall clock driven).
void spu2_update();
