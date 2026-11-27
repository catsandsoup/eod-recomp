// runtime/remaster/remaster_renderer.h
// Foundational Forward+ Linear HDR PBR Renderer (Halo 2 Anniversary / Saber3D architecture).
// ZERO dependency on runtime/gs/*, runtime/vu/*, or vif_mesh.h.
#pragma once

#include "scene_types.h"
#include <array>
#include <cstdint>

namespace remaster {

// 3-Cascade Directional CSM Configuration (pure 3D math, zero PS2 shadow coupling).
struct CsmCascadeSet {
    static constexpr uint32_t kCascadeCount = 3;
    static constexpr uint32_t kResolution = 2048;
    float splits[kCascadeCount] = {24.0f, 80.0f, 260.0f};
    float view_proj[kCascadeCount][16] = {};
};

// Pure CPU helpers (also unit-tested in tests/remaster_engine_test.cpp):
CsmCascadeSet compute_csm_cascades(const SceneCamera &cam, const SceneEnvironment &env,
                                   uint32_t resolution = CsmCascadeSet::kResolution);

// Official Khronos PBR Neutral tone mapper (CPU reference for verification).
std::array<float, 3> khronos_pbr_neutral_cpu(float r, float g, float b);

// Debug Buffer Inspection Mode (0..5), initialized from EOD_REMASTER_DEBUG env var.
DebugViewMode debug_view_mode();
void set_debug_view_mode(DebugViewMode mode);
void cycle_debug_view_mode();

// Initialize Plume pipelines, CSM targets, reflection cubemap, and linear HDR framebuffer.
bool initialize();
bool is_ready();

// Render a full 4-pass Forward+ Linear HDR frame directly from the pure EE SceneFrame:
//   Pass 1: 3-Cascade CSM Directional Shadow Maps (2048x2048 D32_FLOAT)
//   Pass 2: Dynamic 128x128 Environment Reflection Cubemap (amortized 2 faces/frame around player car)
//   Pass 3: Forward+ Linear HDR PBR Geometry Pass (R16G16B16A16_FLOAT + D32_FLOAT) + Debug Modes 0..5
//   Pass 4: Single Khronos PBR Neutral Linear-to-sRGB Resolve + Crisp 2D HUD Composite
bool render_frame(const OutputTarget &target);

void shutdown();

} // namespace remaster
