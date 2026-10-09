// gs.h - Graphics Synthesizer: GIF packet parsing, GS registers, local
// memory and rasterisation.
#pragma once
#include <cstdint>

enum { GIF_PATH1 = 1, GIF_PATH2 = 2, GIF_PATH3 = 3 };

void gs_init();
// Feeds qwc quadwords of GIF data on a path (packets may span calls).
void gif_transfer(int path, const uint8_t *data, uint32_t qwc);
uint32_t gif_reg_read(uint32_t off);
void gif_reg_write(uint32_t off, uint32_t v);

void gs_set_crt(int interlace, int mode, int ffmd);
uint32_t gs_get_imr();
void gs_put_imr(uint32_t v);
// Called at vblank start: presents the displayed framebuffer.
void gs_vblank();
// Writes a debug screenshot of the current display to a PPM file.
bool gs_screenshot(const char *path);
