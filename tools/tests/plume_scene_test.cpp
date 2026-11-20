#include "plume_scene.h"
#include "plume_vulkan.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#ifdef __APPLE__
#include <dlfcn.h>
#endif

namespace plume {
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

namespace {
using namespace plume;
using namespace native::scene;

constexpr uint32_t width = 64, height = 64;
constexpr RenderFormat format = RenderFormat::R8G8B8A8_UNORM;

bool matches(const uint8_t *pixel, int r, int g, int b) {
    return std::abs(pixel[0] - r) <= 1 && std::abs(pixel[1] - g) <= 1 && std::abs(pixel[2] - b) <= 1;
}

enum class Draw { Classic, ModernFog, ModernTexture };

bool render(RenderDevice &dev, PipelineCache &scene, Draw draw) {
    const bool modern = draw != Draw::Classic;
    auto queue = dev.createCommandQueue(RenderCommandListType::DIRECT);
    if (!queue) return false;
    auto list = queue->createCommandList();
    auto fence = dev.createCommandFence();
    auto color = dev.createTexture(RenderTextureDesc::ColorTarget(width, height, format));
    auto texture = dev.createTexture(RenderTextureDesc::Texture2D(2, 2, 1, format));
    RenderSamplerDesc filter;
    filter.minFilter = filter.magFilter = RenderFilter::NEAREST;
    auto sampler = dev.createSampler(filter);
    auto texture_set = dev.createDescriptorSet(texture_desc());
    auto environment_set = dev.createDescriptorSet(environment_desc());
    auto staging = dev.createBuffer(RenderBufferDesc::UploadBuffer(512));
    auto readback = dev.createBuffer(RenderBufferDesc::ReadbackBuffer(width * height * 4));
    static const Vertex vertices[] = {
        {{-.8f, -.7f, .5f}, {0, 0}, {}, {180, 85, 45, 128}},
        {{ .8f, -.7f, .5f}, {1, 0}, {}, {180, 85, 45, 128}},
        {{  0.f,  .8f, .5f}, {.5f, 1}, {}, {180, 85, 45, 128}},
    };
    static const uint32_t indices[] = {0, 1, 2};
    auto mesh = dev.createBuffer(RenderBufferDesc::UploadBuffer(
        sizeof vertices + sizeof indices, RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX));
    auto environment = dev.createBuffer(RenderBufferDesc::UploadBuffer(8 * sizeof(float), RenderBufferFlag::CONSTANT));
    auto *pipeline = scene.pipeline((0xFu << 8) | (modern ? K_MODERN : 0u));
    if (!list || !fence || !color || !texture || !sampler || !texture_set || !environment_set ||
        !staging || !readback || !mesh || !environment || !pipeline) return false;
    const RenderTexture *attachments[] = {color.get()};
    auto framebuffer = dev.createFramebuffer(RenderFramebufferDesc(attachments, 1, nullptr));
    if (!framebuffer) return false;

    auto *mesh_bytes = static_cast<uint8_t *>(mesh->map());
    if (!mesh_bytes) return false;
    std::memcpy(mesh_bytes, vertices, sizeof vertices);
    std::memcpy(mesh_bytes + sizeof vertices, indices, sizeof indices);
    mesh->unmap();
    auto *pixels = static_cast<uint8_t *>(staging->map());
    if (!pixels) return false;
    const uint8_t top[] = {255, 0, 0, 255, 0, 255, 0, 255};
    const uint8_t bottom[] = {0, 0, 255, 255, 255, 255, 0, 255};
    std::memcpy(pixels, top, sizeof top);
    std::memcpy(pixels + 256, bottom, sizeof bottom);
    staging->unmap();
    void *params = environment->map();
    if (!params) return false;
    const float fog[] = {0, 1, 0, 0, 0, 1, 1, 1};
    std::memcpy(params, fog, sizeof fog);
    environment->unmap();
    texture_set->setTexture(0, texture.get(), RenderTextureLayout::SHADER_READ);
    texture_set->setSampler(1, sampler.get());
    environment_set->setBuffer(0, environment.get(), 8 * sizeof(float));

    list->begin();
    list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(texture.get(), RenderTextureLayout::COPY_DEST));
    list->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.get()),
                            RenderTextureCopyLocation::PlacedFootprint(staging.get(), format, 2, 2, 1, 64));
    const RenderTextureBarrier draw_barriers[] = {
        {texture.get(), RenderTextureLayout::SHADER_READ}, {color.get(), RenderTextureLayout::COLOR_WRITE}};
    list->barriers(RenderBarrierStage::GRAPHICS, draw_barriers, 2);
    list->setFramebuffer(framebuffer.get());
    list->setViewports(RenderViewport(0, 0, width, height));
    list->setScissors(RenderRect(0, 0, width, height));
    list->clearColor(0, RenderColor(0, 0, 0, 1));
    list->setGraphicsPipelineLayout(scene.layout());
    list->setPipeline(pipeline);
    list->setGraphicsDescriptorSet(texture_set.get(), 0);
    if (modern) list->setGraphicsDescriptorSet(environment_set.get(), 1);
    PushConstants push{};
    push.cx[0] = push.cy[1] = push.cz[2] = push.cw[3] = 1;
    push.cw[2] = 1;  // model-space eye z = -1, fog saturates at a distance of 1
    push.ambient[3] = 128;
    if (draw == Draw::ModernFog) push.mode[2] = 1u << 31;  // Modern authored fog
    if (draw == Draw::ModernTexture) {
        push.mode[0] = 1u | (1u << 2);  // texture decal
        push.mode[2] = 1u << 27;        // unlit
    }
    list->setGraphicsPushConstants(0, &push, 0, sizeof push);
    bind_indexed_mesh(*list, *mesh, sizeof vertices, sizeof vertices, sizeof indices);
    list->drawIndexedInstanced(3, 1, 0, 0, 0);
    list->setFramebuffer(nullptr);
    list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(color.get(), RenderTextureLayout::COPY_SOURCE));
    list->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readback.get(), format, width, height, 1, width),
                            RenderTextureCopyLocation::Subresource(color.get()));
    list->end();
    queue->executeCommandLists(list.get(), fence.get());
    queue->waitForCommandFence(fence.get());

    const auto *bytes = static_cast<const uint8_t *>(readback->map());
    if (!bytes) return false;
    const auto *middle = bytes + (height / 2 * width + width / 2) * 4;
    std::printf("%s center (%u, %u, %u), corner (%u, %u, %u)\n", modern ? "Modern" : "Classic",
                middle[0], middle[1], middle[2], bytes[0], bytes[1], bytes[2]);
    bool drawn;
    if (draw == Draw::ModernTexture) {
        auto pixel = [&](uint32_t x, uint32_t y) { return bytes + (y * width + x) * 4; };
        drawn = matches(pixel(24, 24), 255, 0, 0) && matches(pixel(40, 24), 0, 255, 0) &&
                matches(pixel(29, 38), 0, 0, 255) && matches(pixel(34, 38), 255, 255, 0);
        if (!drawn) std::fprintf(stderr, "Modern decal did not sample the four UV quadrants\n");
    } else {
        drawn = modern ? matches(middle, 0, 255, 0) : matches(middle, 180, 85, 45);
    }
    const bool valid = drawn && matches(bytes, 0, 0, 0);
    readback->unmap();
    return valid;
}
}

int main() {
#ifdef __APPLE__
    void *loader = dlopen(EOD_VULKAN_LOADER, RTLD_NOW | RTLD_LOCAL | RTLD_FIRST);
    auto get_proc = loader ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr")) : nullptr;
    if (!get_proc) {
        std::fprintf(stderr, "Plume scene test requires a Vulkan loader\n");
        return 77;
    }
    volkInitializeCustom(get_proc);
#endif
    auto api = plume::CreateVulkanInterface();
    auto dev = api ? api->createDevice("") : nullptr;
    if (!dev) {
        std::fprintf(stderr, "Plume scene test requires a Vulkan device\n");
        return 77;
    }
    native::scene::PipelineCache scene;
    if (!scene.initialize(*dev) || !render(*dev, scene, Draw::Classic) ||
        !render(*dev, scene, Draw::ModernFog) || !render(*dev, scene, Draw::ModernTexture)) {
        std::fprintf(stderr, "Shared Plume scene draw failed\n");
        return 1;
    }
    return 0;
}
