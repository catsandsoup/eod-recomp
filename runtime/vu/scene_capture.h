// scene_capture.h - EOD_SCENE_CAPTURE: records the VU1 inputs and GS packets of
// a few frames so an external renderer can rebuild the 3D scene
// (docs/SCENE_CAPTURE.md). Off unless the variable is set; recording never
// changes what the game draws.
//
//   EOD_SCENE_CAPTURE=file  EOD_SCENE_CAPTURE_AT=vblank  [EOD_SCENE_CAPTURE_N=3]
//
// records from vblank AT-N to vblank AT+1 (the frame numbers of
// --screenshot-every). `file` gets the producer stream (VU1 runs with their
// registers and data memory, every GIF packet in order, vblank markers);
// `file.tex` gets the texture each GS draw used, keyed like host textures.
#pragma once
#include <cstdint>

struct VUState;

namespace scene_capture {

extern bool g_on;  // EOD_SCENE_CAPTURE is set (the rest are no-ops otherwise)

// Producer side (the thread that runs VU1 and queues GS commands).
void vblank(uint64_t frame);  // before the vblank command is queued
void vu1_start(const VUState *vu, uint32_t pc);
void gif(int path, const uint8_t *data, uint32_t qwc);

// GS side (in command order).
void gs_vblank(uint64_t frame);
void gs_packet(int path);
bool gs_active();
// One textured draw state: TEX0 fields and the host-texture identity keys
// (0 for direct-colour formats).
void gs_texture(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th, uint32_t cpsm, uint32_t csa,
                uint64_t index_key, uint64_t palette_key);

}  // namespace scene_capture
