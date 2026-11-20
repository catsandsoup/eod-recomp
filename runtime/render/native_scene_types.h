// Geometry and object data shared by the native game and standalone renderers.
#pragma once
#include <cstdint>

namespace native::scene {

struct Vertex {
    float p[3];
    float uv[2];
    int8_t n[4];   // byte - 128 of the normal; [3] = 1 when present
    uint8_t c[4];  // vertex colour bytes (0x80 = 1.0)
};
static_assert(sizeof(Vertex) == 28, "vertex layout");

// Untessellated retail ParticleAdd / SpriteAdd input. The Modern vertex shader
// expands it in the current view. Classic's indexed quad remains available.
struct EffectBillboard {
    float center[3], radius;
    float uv_rect[4];
    float angle, shade;
    uint8_t rgba[4];
};
static_assert(sizeof(EffectBillboard) == 44, "effect instance layout");
inline constexpr uint32_t kMaxEffectBillboardsPerStream = 512;
inline constexpr uint32_t kMaxEffectBillboardsPerVblank = 4096;

struct PushConstants {
    float cx[4], cy[4], cz[4], cw[4];
    float light[4], diffuse[4], ambient[4];
    uint32_t mode[4];
};
static_assert(sizeof(PushConstants) == 128, "push constants");

} // namespace native::scene
