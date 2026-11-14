// scene_emit.h - EOD_SCENE_EMIT: the decoded-batch emitter (docs/SCENE_CAPTURE.md,
// "Decoded-batch emitter"). Each frame it emits the game's 3D scene as a modern
// renderer needs it: per VU1 geometry batch the model-space mesh (positions,
// normals or vertex colours, UVs, strip flags; sent once and then referenced by
// content id), the model matrix, the program identity and the game's light
// direction; per frame the camera; and on the GS side the texture identity of
// each draw. Off unless the variable is set; it never changes what the game draws.
//
//   EOD_SCENE_EMIT=file  [EOD_SCENE_EMIT_FROM=vblank] [EOD_SCENE_EMIT_N=frames]
//                        [EOD_SCENE_EMIT_VERIFY=vblank]   (also the raw kicks, that vblank and the next 3)
//                        [EOD_SCENE_EMIT_NOWRITE=1]       (decode only: cost measurement)
//
// `file` gets the producer stream, `file.tex` the GS-side texture rows.
// tools/scene_view.py reads both.
#pragma once
#include <cstdint>

struct VUState;

namespace scene_emit {

extern bool g_on;

// Producer side (the thread that runs VU1 and queues GS commands).
// true: the modern renderer redraws this run's batch from cached results, so the program is not
// run and its GS packet not built (EOD_MODERN_SKIPVU=0 disables)
bool vu1_start(const VUState *vu, uint32_t pc);
void gif(int path, const uint8_t *data, uint32_t qwc);
void vblank(uint64_t frame);

// GS side, in command order.
void gs_vblank(uint64_t frame, uint64_t fogcol);
void gs_packet(int path);
bool gs_wants_texture();  // a texture row is wanted for the current draw state
void gs_texture(uint32_t tbp, uint32_t psm, uint32_t tw, uint32_t th, uint64_t index_key, uint64_t palette_key,
                uint64_t xyoffset, uint32_t fbp);
// Index-key cache by raw VRAM content (the key is expensive, the content hash is not).
bool gs_key_lookup(uint64_t raw, uint64_t &index_key);
void gs_key_store(uint64_t raw, uint64_t index_key);
// Time spent by the GS-side hook of the current draw (added to the GS cost).
void gs_cost_ns(uint64_t ns);

}  // namespace scene_emit
