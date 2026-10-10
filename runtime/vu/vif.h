// vif.h - VIF0/VIF1 command processors.
#pragma once
#include <cstdint>

void vif_init();
// Feeds `count` 32-bit words to VIF n (commands may span calls).
void vif_write(int n, const uint32_t *words, uint32_t count);
uint32_t vif_reg_read(int n, uint32_t off);
void vif_reg_write(int n, uint32_t off, uint32_t v);
void vif1_read_fifo(uint32_t out[4]);
