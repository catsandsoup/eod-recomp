// gs.h - Graphics Synthesizer: GIF packet parsing, GS registers, local
// memory and rasterisation.
#pragma once
#include <cstdint>

enum { GIF_PATH1 = 1, GIF_PATH2 = 2, GIF_PATH3 = 3 };

void gs_init();
// EE-thread shutdown: stop producers, drain GS/raster work, join all workers.
void gs_shutdown();
// Feeds qwc quadwords of GIF data on a path (packets may span calls).
void gif_transfer(int path, const uint8_t *data, uint32_t qwc);
uint32_t gif_reg_read(uint32_t off);
void gif_reg_write(uint32_t off, uint32_t v);

// PATH3 arbitration: GIF DMA data is queued and released packet by packet
// while PATH3 is not masked (VIF1 MSKPATH3 / GIF_MODE.M3R).
void gif_path3_enqueue(const uint8_t *data, uint32_t qwc);
// Runs queued PATH3 packets (max_packets < 0: all) if unmasked.  Returns
// true when the queue is empty.
bool gif_path3_run(int max_packets);
bool gif_path3_pending();
void gif_set_path3_mask(bool masked);

void gs_set_crt(int interlace, int mode, int ffmd);
uint32_t gs_get_imr();
void gs_put_imr(uint32_t v);
// Called at vblank start: presents the displayed framebuffer.
void gs_vblank();
// Game frames drawn so far (frame rate counter).
uint64_t gs_drawn_frames();
// Presents the last image again (with the in-game menu over it) while the game is paused.
void gs_overlay_redraw();
// Writes a debug screenshot of the current display to a PPM file.
bool gs_screenshot(const char *path);
// replays a GS capture (EOD_GS_DUMP) standalone; returns the exit code
int gs_replay(const char *path, int repeat);
