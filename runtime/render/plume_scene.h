// Shared Classic/Modern Plume scene pipeline. Callers own the camera, draw
// list, geometry, textures and presentation or offscreen target.
#pragma once
#include "native_scene_types.h"
#include "plume_render_interface.h"
#include <memory>
#include <unordered_map>

namespace native::scene {

// Blend equations (GS ALPHA a, b, d with C in the shader's second output).
enum Blend : uint32_t { B_OPAQUE, B_MIX, B_ADD, B_REVSUB, B_SCALE, B_DST_ADD, B_INV_MIX, B_KEEP, B_SHADOW, B_COUNT };
// Pipeline key: blend 0..3, depth compare 4..5, depth write 6, depth test 7,
// colour write mask 8..11, cull 12..13, blend alpha 14.
// Generic blend uses bits 16..17 src factor, 18..19 dst factor and 20..21 op.
enum : uint32_t { K_GENERIC = 1u << 15, K_CDST = 1u << 22, K_NOCLIP = 1u << 23, K_MODERN = 1u << 24,
                  K_STRAIGHT_ALPHA = 1u << 25, K_BOOTSTRAP_QUAD = 1u << 26, K_STATIC_QUEUE = 1u << 27,
                  K_SCENE_INPUTS = 1u << 28, K_VFX_BILLBOARD = 1u << 29 };

// Instance data for the unclipped Classic static queue vertex entry.
struct StaticQueueProjection {
    float x[4], y[4], w[4];
    float viewport[4];   // GS XYOFFSET in pixels, then 2 / surface dimensions
};
static_assert(sizeof(StaticQueueProjection) == 64, "static queue projection layout");

plume::RenderDescriptorSetDesc texture_desc();
plume::RenderDescriptorSetDesc environment_desc();
plume::RenderDescriptorSetDesc scene_input_desc();

class PipelineCache {
public:
    // The caller's device must outlive this cache. All calls are on one render thread.
    bool initialize(plume::RenderDevice &device,
                    plume::RenderFormat format = plume::RenderFormat::R8G8B8A8_UNORM);
    plume::RenderPipelineLayout *layout() const { return layout_.get(); }
    plume::RenderPipeline *pipeline(uint32_t key);
    size_t size() const { return pipelines_.size(); }

private:
    plume::RenderPipeline *make_pipeline(uint32_t key);

    plume::RenderDevice *device_ = nullptr;
    plume::RenderFormat format_ = plume::RenderFormat::UNKNOWN;
    std::unique_ptr<plume::RenderPipelineLayout> layout_;
    std::unique_ptr<plume::RenderShader> vs_, ps_, vs_modern_, ps_modern_, ps_modern_scene_, vs_static_queue_;
    std::unique_ptr<plume::RenderShader> vs_bootstrap_quad_, ps_bootstrap_quad_;
    std::unique_ptr<plume::RenderShader> vs_vfx_billboard_;
    std::unordered_map<uint32_t, std::unique_ptr<plume::RenderPipeline>> pipelines_;
};

// The buffer holds one contiguous vertex stream at byte zero and u32 indices
// at index_offset. The caller chooses the index range and instance count.
void bind_indexed_mesh(plume::RenderCommandList &list, const plume::RenderBuffer &buffer,
                       uint32_t vertex_bytes, uint64_t index_offset, uint32_t index_bytes);

} // namespace native::scene
