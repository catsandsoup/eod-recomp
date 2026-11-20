#include "plume_scene.h"

namespace native::scene {
namespace {
#include "native_shaders_spv.inc"
#include "native_modern_spv.inc"
#include "bootstrap_quad_spv.inc"
} // namespace

plume::RenderDescriptorSetDesc texture_desc() {
    static const plume::RenderDescriptorRange ranges[2] = {
        plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 0, 1),
        plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 1, 1)};
    return plume::RenderDescriptorSetDesc(ranges, 2);
}

plume::RenderDescriptorSetDesc environment_desc() {
    static const plume::RenderDescriptorRange range(plume::RenderDescriptorRangeType::CONSTANT_BUFFER, 0, 1);
    return plume::RenderDescriptorSetDesc(&range, 1);
}

bool PipelineCache::initialize(plume::RenderDevice &dev, plume::RenderFormat format) {
    if (device_) return device_ == &dev && format_ == format;
    const plume::RenderDescriptorSetDesc sets[] = {texture_desc(), environment_desc()};
    const plume::RenderPushConstantRange push(0, 0, 0, sizeof(PushConstants),
        plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);
    layout_ = dev.createPipelineLayout(plume::RenderPipelineLayoutDesc(&push, 1, sets, 2, false, true));
    vs_ = dev.createShader(k_native_vs_main, sizeof k_native_vs_main, "vs_main", plume::RenderShaderFormat::SPIRV);
    ps_ = dev.createShader(k_native_ps_main, sizeof k_native_ps_main, "ps_main", plume::RenderShaderFormat::SPIRV);
    vs_modern_ = dev.createShader(k_native_vs_modern, sizeof k_native_vs_modern, "vs_modern", plume::RenderShaderFormat::SPIRV);
    ps_modern_ = dev.createShader(k_native_ps_modern, sizeof k_native_ps_modern, "ps_modern", plume::RenderShaderFormat::SPIRV);
    if (!layout_ || !vs_ || !ps_ || !vs_modern_ || !ps_modern_) return false;
    device_ = &dev;
    format_ = format;
    return true;
}

// stage R6: every pipeline is made with its Classic / Modern twin, so a look toggle compiles nothing
// (P5); a projected-shadow pipeline (B_SHADOW) has no Modern twin
plume::RenderPipeline *PipelineCache::pipeline(uint32_t key) {
    if (!device_) return nullptr;
    auto it = pipelines_.find(key);
    if (it != pipelines_.end() && it->second) return it->second.get();
    if ((key & 15) != B_SHADOW && !(key & K_BOOTSTRAP_QUAD)) make_pipeline((key ^ K_MODERN) & ~K_STATIC_QUEUE);
    return make_pipeline(key);
}
plume::RenderPipeline *PipelineCache::make_pipeline(uint32_t key) {
    auto &slot = pipelines_[key];
    if (slot) return slot.get();
    const uint32_t blend = key & 15, zfunc = (key >> 4) & 3, mask = (key >> 8) & 15;
    const uint32_t cull = (key >> 12) & 3;
    const bool zwrite = (key >> 6) & 1, depth = (key >> 7) & 1, blend_alpha = (key >> 14) & 1;
    plume::RenderInputSlot in[] = {
        plume::RenderInputSlot(0, sizeof(Vertex)),
        plume::RenderInputSlot(1, sizeof(StaticQueueProjection), plume::RenderInputSlotClassification::PER_INSTANCE_DATA)};
    const plume::RenderInputElement el[] = {
        plume::RenderInputElement("POSITION", 0, 0, plume::RenderFormat::R32G32B32_FLOAT, 0, 0),
        plume::RenderInputElement("TEXCOORD", 0, 1, plume::RenderFormat::R32G32_FLOAT, 0, 12),
        plume::RenderInputElement("NORMAL", 0, 2, plume::RenderFormat::R8G8B8A8_SINT, 0, 20),
        plume::RenderInputElement("COLOR", 0, 3, plume::RenderFormat::R8G8B8A8_UNORM, 0, 24),
        plume::RenderInputElement("SCREEN_X", 0, 4, plume::RenderFormat::R32G32B32A32_FLOAT, 1, 0),
        plume::RenderInputElement("SCREEN_Y", 0, 5, plume::RenderFormat::R32G32B32A32_FLOAT, 1, 16),
        plume::RenderInputElement("SCREEN_W", 0, 6, plume::RenderFormat::R32G32B32A32_FLOAT, 1, 32),
        plume::RenderInputElement("SCREEN_WINDOW", 0, 7, plume::RenderFormat::R32G32B32A32_FLOAT, 1, 48)};
    const bool static_queue = (key & K_STATIC_QUEUE) != 0;
    if (static_queue && !vs_static_queue_) {
        vs_static_queue_ = device_->createShader(k_native_vs_static_queue, sizeof k_native_vs_static_queue,
            "vs_static_queue", plume::RenderShaderFormat::SPIRV);
        if (!vs_static_queue_) return nullptr;
    }
    plume::RenderGraphicsPipelineDesc d;
    d.pipelineLayout = layout_.get();
    d.vertexShader = static_queue ? vs_static_queue_.get() : (key & K_MODERN) ? vs_modern_.get() : vs_.get();
    d.pixelShader = (key & K_MODERN) ? ps_modern_.get() : ps_.get();
    if (key & K_BOOTSTRAP_QUAD) {
        if (!vs_bootstrap_quad_) {
            vs_bootstrap_quad_ = device_->createShader(k_spv_vs_bootstrap_quad, sizeof k_spv_vs_bootstrap_quad,
                "vs_bootstrap_quad", plume::RenderShaderFormat::SPIRV);
            ps_bootstrap_quad_ = device_->createShader(k_spv_ps_bootstrap_quad, sizeof k_spv_ps_bootstrap_quad,
                "ps_bootstrap_quad", plume::RenderShaderFormat::SPIRV);
        }
        if (!vs_bootstrap_quad_ || !ps_bootstrap_quad_) return nullptr;
        d.vertexShader = vs_bootstrap_quad_.get();
        d.pixelShader = ps_bootstrap_quad_.get();
    }
    d.inputSlots = in;
    d.inputSlotsCount = static_queue ? 2 : 1;
    d.inputElements = el;
    d.inputElementsCount = static_queue ? 8 : 4;
    d.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    d.cullMode = cull == 1 ? plume::RenderCullMode::BACK : cull == 2 ? plume::RenderCullMode::FRONT : plume::RenderCullMode::NONE;
    d.depthClipEnabled = (key & K_NOCLIP) == 0;
    d.renderTargetCount = 1;
    d.renderTargetFormat[0] = format_;
    if (depth) {
        d.depthTargetFormat = plume::RenderFormat::D32_FLOAT;
        d.depthEnabled = true;
        d.depthWriteEnabled = zwrite;
        d.depthFunction = zfunc == 0   ? plume::RenderComparisonFunction::ALWAYS
                          : zfunc == 1 ? plume::RenderComparisonFunction::GREATER_EQUAL
                          : zfunc == 2 ? plume::RenderComparisonFunction::GREATER
                                       : plume::RenderComparisonFunction::NEVER;
    }
    using B = plume::RenderBlend;
    using O = plume::RenderBlendOperation;
    plume::RenderBlendDesc &b = d.renderTargetBlend[0];
    b.blendEnabled = blend != B_OPAQUE;
    b.blendOp = (blend == B_REVSUB || blend == B_SHADOW) ? O::REV_SUBTRACT : O::ADD;
    if (key & K_GENERIC) {
        // GS (A - B) C + D as src * F(src) op dst * F(dst) (blend_classes): F 0 zero, 1 one, 2 C, 3 1 - C;
        // C is the shader's second output (As or FIX), or the destination alpha when K_CDST
        const bool cd = (key & K_CDST) != 0;
        auto factor = [&](uint32_t f) {
            switch (f & 3) {
            case 0: return B::ZERO;
            case 1: return B::ONE;
            case 2: return cd ? B::DEST_ALPHA : B::SRC1_ALPHA;
            default: return cd ? B::INV_DEST_ALPHA : B::INV_SRC1_ALPHA;
            }
        };
        const uint32_t sf = (key >> 16) & 3, df = (key >> 18) & 3, op = (key >> 20) & 3;
        b.blendEnabled = !(sf == 1 && df == 0 && op == 0);
        b.srcBlend = factor(sf);
        b.dstBlend = factor(df);
        b.blendOp = op == 1 ? O::SUBTRACT : op == 2 ? O::REV_SUBTRACT : O::ADD;
    } else
    switch (blend) {
    case B_MIX: b.srcBlend = B::SRC1_ALPHA, b.dstBlend = B::INV_SRC1_ALPHA; break;
    case B_ADD: b.srcBlend = B::SRC1_ALPHA, b.dstBlend = B::ONE; break;
    case B_REVSUB: b.srcBlend = B::SRC1_ALPHA, b.dstBlend = B::ONE; break;
    case B_SCALE: b.srcBlend = B::SRC1_ALPHA, b.dstBlend = B::ZERO; break;
    case B_DST_ADD: b.srcBlend = B::ONE, b.dstBlend = B::SRC1_ALPHA; break;
    case B_INV_MIX: b.srcBlend = B::INV_SRC1_ALPHA, b.dstBlend = B::SRC1_ALPHA; break;
    case B_KEEP: b.srcBlend = B::ZERO, b.dstBlend = B::ONE; break;
    case B_SHADOW: b.srcBlend = B::ONE, b.dstBlend = B::INV_SRC1_ALPHA; break;  // dst (1 - As/128) - src
    default: b.srcBlend = B::ONE, b.dstBlend = B::ZERO; break;
    }
    // the GS writes the source alpha as it is; shadow accumulation blends alpha with the same op
    if (blend_alpha) {
        b.srcBlendAlpha = B::ONE;
        b.dstBlendAlpha = B::ONE;
        b.blendOpAlpha = b.blendOp;
    } else {
        b.srcBlendAlpha = B::ONE;
        b.dstBlendAlpha = B::ZERO;
        b.blendOpAlpha = O::ADD;
    }
    if (key & K_STRAIGHT_ALPHA) b = plume::RenderBlendDesc::AlphaBlend();
    b.renderTargetWriteMask = (uint8_t)mask;
    slot = device_->createGraphicsPipeline(d);
    return slot.get();
}

void bind_indexed_mesh(plume::RenderCommandList &list, const plume::RenderBuffer &buffer,
                       uint32_t vertex_bytes, uint64_t index_offset, uint32_t index_bytes) {
    const plume::RenderVertexBufferView vertices(buffer.at(0), vertex_bytes);
    const plume::RenderInputSlot slot(0, sizeof(Vertex));
    list.setVertexBuffers(0, &vertices, 1, &slot);
    const plume::RenderIndexBufferView indices(buffer.at(index_offset), index_bytes, plume::RenderFormat::R32_UINT);
    list.setIndexBuffer(&indices);
}

} // namespace native::scene
