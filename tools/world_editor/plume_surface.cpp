// Vulkan presentation for the Qt World Editor. Qt owns the native child and
// invokes this ABI on the window thread. No frame data crosses back into Python.
#include "plume_scene.h"
#include "plume_vulkan.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#ifdef __APPLE__
#include <dlfcn.h>
#endif

#ifdef _WIN32
#define EOD_VIEWPORT_API extern "C" __declspec(dllexport)
#else
#define EOD_VIEWPORT_API extern "C" __attribute__((visibility("default")))
#endif

namespace plume {
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

namespace editor_plume {
#ifdef __APPLE__
plume::RenderWindow attach_surface(void *native_view);
void resize_surface(plume::RenderWindow window, void *native_view, uint32_t width, uint32_t height);
void detach_surface(plume::RenderWindow window);
#endif

namespace {
using namespace plume;
using namespace native::scene;
constexpr RenderFormat scene_format = RenderFormat::B8G8R8A8_UNORM;
constexpr uint32_t opaque_depth = (0xFu << 8) | (1u << 7) | (1u << 6) | (1u << 4);
constexpr uint32_t blend_depth = (opaque_depth & ~(1u << 6)) | K_STRAIGHT_ALPHA;
constexpr uint32_t tex_decal = 1u | 1u << 2; // TME, TFX=decal
constexpr uint32_t tex_alpha = 1u << 1; // TCC
constexpr uint32_t alpha_cutout = 1u << 4 | 5u << 5 | 1u << 8; // ATE, GEQUAL, discard
constexpr uint32_t alpha_reference = 115; // matches the editor's 0.45 cutout threshold after RGBA8 rounding

enum Alpha : uint32_t { Opaque, Cutout, BlendAlpha };

struct Batch {
    uint32_t first, count, slot;
    float center[3];
};
static_assert(sizeof(Batch) == 24, "Qt Plume batch format");

struct MipUpload {
    uint32_t width, height, row;
    uint64_t offset;
};

struct Texture {
    std::unique_ptr<RenderTexture> image;
    std::unique_ptr<RenderDescriptorSet> set;
    std::unique_ptr<RenderBuffer> staging;
    std::vector<MipUpload> mips;
    Alpha alpha = Opaque;
    bool pending = false;
};

struct Surface {
    void *native_view = nullptr;
    RenderWindow window{};
    std::unique_ptr<RenderInterface> api;
    std::unique_ptr<RenderDevice> device;
    PipelineCache scene;
    std::unique_ptr<RenderCommandQueue> queue;
    std::unique_ptr<RenderCommandList> list;
    std::unique_ptr<RenderCommandFence> fence;
    std::unique_ptr<RenderCommandSemaphore> acquired;
    std::unique_ptr<RenderSwapChain> swapchain;
    std::unique_ptr<RenderTexture> depth;
    std::unique_ptr<RenderSampler> sampler;
    std::unique_ptr<RenderDescriptorSet> environment_set;
    std::unique_ptr<RenderBuffer> mesh, environment;
    Texture white;
    std::unordered_map<uint32_t, Texture> textures;
    std::vector<Batch> batches;
    std::vector<std::unique_ptr<RenderFramebuffer>> targets;
    std::vector<std::unique_ptr<RenderCommandSemaphore>> ready;
    RenderPipeline *opaque_pipeline = nullptr, *blend_pipeline = nullptr;
    uint32_t vertex_bytes = 0, index_bytes = 0;
    bool needs_resize = true, submitted = false;

    void finish_frame() {
        if (!submitted) return;
        queue->waitForCommandFence(fence.get());
        if (!white.pending) white.staging.reset();
        for (auto &entry : textures)
            if (!entry.second.pending) entry.second.staging.reset();
        submitted = false;
    }

    void wait_idle() const {
        if (!queue) return;
        auto *vk_queue = static_cast<VulkanCommandQueue *>(queue.get())->queue;
        const std::scoped_lock lock(*vk_queue->mutex);
        vkQueueWaitIdle(vk_queue->vk);
    }

    ~Surface() {
        wait_idle();
        targets.clear();
        ready.clear();
        swapchain.reset();
#ifdef __APPLE__
        if (window.view) detach_surface(window);
#endif
    }

    bool initialize(void *view) {
        native_view = view;
#ifdef __APPLE__
        const char *path = std::getenv("SDL_VULKAN_LIBRARY");
        static void *loader = dlopen(path && *path ? path : EOD_VULKAN_LOADER, RTLD_NOW | RTLD_LOCAL | RTLD_FIRST);
        const auto proc = loader ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr")) : nullptr;
        if (!proc) return false;
        volkInitializeCustom(proc);
        window = attach_surface(view);
        if (!window.view) return false;
#else
        window = static_cast<HWND>(view);
#endif
        api = CreateVulkanInterface();
        if (!api) return false;
        device = api->createDevice();
        if (!device || !scene.initialize(*device, scene_format)) return false;
        opaque_pipeline = scene.pipeline(opaque_depth | K_MODERN);
        blend_pipeline = scene.pipeline(blend_depth | K_MODERN);
        queue = device->createCommandQueue(RenderCommandListType::DIRECT);
        if (!queue || !opaque_pipeline || !blend_pipeline) return false;
        list = queue->createCommandList();
        fence = device->createCommandFence();
        acquired = device->createCommandSemaphore();
        swapchain = queue->createSwapChain(RenderSwapChainDesc(window, scene_format, 2));
        if (!swapchain || static_cast<VulkanSwapChain *>(swapchain.get())->pickedSurfaceFormat.format == VK_FORMAT_UNDEFINED)
            return false;
        RenderSamplerDesc filter;
        filter.minFilter = filter.magFilter = RenderFilter::LINEAR;
        filter.addressU = filter.addressV = RenderTextureAddressMode::WRAP;
        filter.mipmapMode = RenderMipmapMode::LINEAR;
        sampler = device->createSampler(filter);
        environment_set = device->createDescriptorSet(environment_desc());
        environment = device->createBuffer(RenderBufferDesc::UploadBuffer(8 * sizeof(float), RenderBufferFlag::CONSTANT));
        if (!list || !fence || !acquired || !sampler || !environment_set || !environment) return false;
        const uint8_t pixel[] = {255, 255, 255, 255};
        if (!create_texture(white, 1, 1, 1, Opaque, pixel)) return false;
        environment_set->setBuffer(0, environment.get(), 8 * sizeof(float));
        return true;
    }

    bool create_texture(Texture &out, uint32_t width, uint32_t height, uint32_t levels, Alpha alpha,
                        const uint8_t *pixels) {
        constexpr RenderFormat rgba = RenderFormat::R8G8B8A8_UNORM;
        Texture texture;
        texture.alpha = alpha;
        uint64_t staging_size = 0;
        for (uint32_t level = 0; level < levels; ++level) {
            const uint32_t w = std::max(1u, width >> level), h = std::max(1u, height >> level);
            const uint32_t row = (w * 4 + 255) & ~255u;
            texture.mips.push_back({w, h, row, staging_size});
            staging_size = (staging_size + uint64_t(row) * h + 511) & ~511ull;
        }
        texture.image = device->createTexture(RenderTextureDesc::Texture2D(width, height, levels, rgba));
        texture.set = device->createDescriptorSet(texture_desc());
        texture.staging = device->createBuffer(RenderBufferDesc::UploadBuffer(staging_size));
        if (!texture.image || !texture.set || !texture.staging) return false;
        auto *bytes = static_cast<uint8_t *>(texture.staging->map());
        if (!bytes) return false;
        for (const auto &mip : texture.mips) {
            for (uint32_t y = 0; y < mip.height; ++y) {
                std::memcpy(bytes + mip.offset + uint64_t(y) * mip.row, pixels, mip.width * 4);
                pixels += mip.width * 4;
            }
        }
        texture.staging->unmap();
        texture.set->setTexture(0, texture.image.get(), RenderTextureLayout::SHADER_READ);
        texture.set->setSampler(1, sampler.get());
        texture.pending = true;
        out = std::move(texture);
        return true;
    }

    bool replace_texture(uint32_t slot, uint32_t width, uint32_t height, uint32_t alpha, const uint8_t *pixels,
                         uint64_t bytes) {
        if (!slot || !pixels || !width || !height || width > 8192 || height > 8192 ||
            alpha > BlendAlpha) return false;
        uint32_t levels = 1;
        uint64_t expected_size = uint64_t(width) * height * 4;
        // The ABI accepts a base image from older callers or a complete packed mip chain.
        if (bytes != expected_size) {
            for (uint32_t w = width, h = height; w > 1 || h > 1; ++levels) {
                w = std::max(1u, w >> 1);
                h = std::max(1u, h >> 1);
                expected_size += uint64_t(w) * h * 4;
            }
            if (bytes != expected_size) return false;
        }
        Texture texture;
        if (!create_texture(texture, width, height, levels, Alpha(alpha), pixels)) return false;
        finish_frame();
        textures.insert_or_assign(slot, std::move(texture));
        return true;
    }

    void clear_textures() {
        finish_frame();
        textures.clear();
        batches.clear();
    }

    void record_upload(Texture &texture) {
        if (!texture.pending) return;
        list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(texture.image.get(), RenderTextureLayout::COPY_DEST));
        for (uint32_t level = 0; level < texture.mips.size(); ++level) {
            const auto &mip = texture.mips[level];
            list->copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.image.get(), level),
                RenderTextureCopyLocation::PlacedFootprint(texture.staging.get(), RenderFormat::R8G8B8A8_UNORM,
                                                           mip.width, mip.height, 1, mip.row / 4, mip.offset));
        }
        list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(texture.image.get(), RenderTextureLayout::SHADER_READ));
        texture.pending = false;
    }

    bool resize(uint32_t width, uint32_t height) {
        if (!needs_resize && width == swapchain->getWidth() && height == swapchain->getHeight()) return true;
        wait_idle();
        needs_resize = true;
        targets.clear();
        ready.clear();
#ifdef __APPLE__
        resize_surface(window, native_view, width, height);
#endif
        if (!swapchain->resize()) return false;
        const uint32_t w = swapchain->getWidth(), h = swapchain->getHeight();
        if (w != width || h != height) {
            std::fprintf(stderr, "[editor plume] viewport extent %ux%u differs from requested %ux%u\n", w, h, width, height);
            return false;
        }
        depth = device->createTexture(RenderTextureDesc::DepthTarget(w, h, RenderFormat::D32_FLOAT));
        if (!depth) return false;
        for (uint32_t i = 0; i < swapchain->getTextureCount(); ++i) {
            const RenderTexture *colors[] = {swapchain->getTexture(i)};
            targets.push_back(device->createFramebuffer(RenderFramebufferDesc(colors, 1, depth.get())));
            ready.push_back(device->createCommandSemaphore());
            if (!targets.back() || !ready.back()) return false;
        }
        needs_resize = false;
        return true;
    }

    bool replace_mesh(const Vertex *vertices, uint32_t vertex_count, const uint32_t *indices, uint32_t count,
                      const Batch *incoming, uint32_t batch_count) {
        finish_frame();
        if (count == 0) {
            mesh.reset();
            batches.clear();
            vertex_bytes = index_bytes = 0;
            return true;
        }
        if (!vertices || !vertex_count || !indices || !incoming || !batch_count ||
            count % 3 || vertex_count > UINT32_MAX / sizeof(Vertex) ||
            count > UINT32_MAX / sizeof(uint32_t)) return false;
        uint32_t covered = 0;
        for (uint32_t i = 0; i < batch_count; ++i) {
            const auto &batch = incoming[i];
            if (batch.first != covered || !batch.count || batch.count % 3 || batch.count > count - covered ||
                (batch.slot && !textures.count(batch.slot))) return false;
            covered += batch.count;
        }
        if (covered != count || std::any_of(indices, indices + count, [vertex_count](uint32_t i) { return i >= vertex_count; }))
            return false;
        const uint32_t vb = vertex_count * sizeof(Vertex), ib = count * sizeof(uint32_t);
        auto next = device->createBuffer(RenderBufferDesc::UploadBuffer(
            uint64_t(vb) + ib, RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX));
        if (!next) return false;
        auto *bytes = static_cast<uint8_t *>(next->map());
        if (!bytes) return false;
        std::memcpy(bytes, vertices, vb);
        std::memcpy(bytes + vb, indices, ib);
        next->unmap();
        mesh = std::move(next);
        batches.assign(incoming, incoming + batch_count);
        vertex_bytes = vb;
        index_bytes = ib;
        return true;
    }

    void draw_batches(const float *matrix) {
        PushConstants push{};
        std::memcpy(&push, matrix, 16 * sizeof(float));
        push.ambient[3] = 255;
        push.mode[2] = (1u << 31) | (1u << 27); // authored fog and unlit asset colours
        list->setGraphicsPipelineLayout(scene.layout());
        list->setGraphicsDescriptorSet(environment_set.get(), 1);
        bind_indexed_mesh(*list, *mesh, vertex_bytes, vertex_bytes, index_bytes);
        auto draw_batch = [&](const Batch &batch, const Texture &texture) {
            push.mode[0] = batch.slot ? tex_decal : 0;
            if (texture.alpha != Opaque) push.mode[0] |= tex_alpha;
            if (texture.alpha == Cutout) push.mode[0] |= alpha_cutout;
            push.mode[1] = alpha_reference;
            list->setGraphicsDescriptorSet(texture.set.get(), 0);
            list->setGraphicsPushConstants(0, &push, 0, sizeof push);
            list->drawIndexedInstanced(batch.count, 1, batch.first, 0, 0);
        };
        list->setPipeline(opaque_pipeline);
        std::vector<std::pair<float, size_t>> sorted_blend;
        for (size_t i = 0; i < batches.size(); ++i) {
            const auto &batch = batches[i];
            const Texture &texture = batch.slot ? textures.at(batch.slot) : white;
            if (texture.alpha != BlendAlpha) {
                draw_batch(batch, texture);
                continue;
            }
            const float z = push.cw[0] * batch.center[0] + push.cw[1] * batch.center[1] + push.cw[2] * batch.center[2];
            sorted_blend.emplace_back(z, i);
        }
        if (!sorted_blend.empty()) {
            std::sort(sorted_blend.rbegin(), sorted_blend.rend());
            list->setPipeline(blend_pipeline);
            for (const auto &item : sorted_blend) {
                const auto &batch = batches[item.second];
                draw_batch(batch, textures.at(batch.slot));
            }
        }
    }

    int draw(uint32_t width, uint32_t height, const float *mat, const float *fog, const float *clear) {
        if (!width || !height) return 0;
        finish_frame();
        if (!resize(width, height)) return -1;
        auto *params = environment->map();
        if (!params) return -1;
        std::memcpy(params, fog, 8 * sizeof(float));
        environment->unmap();
        uint32_t image = 0;
        if (!swapchain->acquireTexture(acquired.get(), &image)) {
            needs_resize = true;
            return 0;
        }
        RenderTexture *color = swapchain->getTexture(image);
        list->begin();
        record_upload(white);
        for (auto &entry : textures) record_upload(entry.second);
        const RenderTextureBarrier barriers[] = {
            {color, RenderTextureLayout::COLOR_WRITE}, {depth.get(), RenderTextureLayout::DEPTH_WRITE}};
        list->barriers(RenderBarrierStage::GRAPHICS, barriers, 2);
        list->setFramebuffer(targets[image].get());
        list->setViewports(RenderViewport(0, 0, width, height));
        list->setScissors(RenderRect(0, 0, int32_t(width), int32_t(height)));
        list->clearColor(0, RenderColor(clear[0], clear[1], clear[2], 1));
        list->clearDepth(true, 0);
        if (mesh && !batches.empty()) draw_batches(mat);
        list->setFramebuffer(nullptr);
        list->barriers(RenderBarrierStage::NONE, RenderTextureBarrier(color, RenderTextureLayout::PRESENT));
        list->end();
        const RenderCommandList *recorded = list.get();
        RenderCommandSemaphore *wait = acquired.get(), *signal = ready[image].get();
        queue->executeCommandLists(&recorded, 1, &wait, 1, &signal, 1, fence.get());
        submitted = true;
        const bool presented = swapchain->present(image, &signal, 1);
        needs_resize = !presented;
        return presented ? 1 : 0;
    }
};
} // namespace
} // namespace editor_plume

using editor_plume::Surface;
using editor_plume::Batch;

EOD_VIEWPORT_API Surface *eod_plume_view_create(void *view) {
    try {
        auto surface = std::make_unique<Surface>();
        return view && surface->initialize(view) ? surface.release() : nullptr;
    } catch (...) {
        return nullptr;
    }
}

EOD_VIEWPORT_API void eod_plume_view_destroy(Surface *surface) {
    delete surface;
}

EOD_VIEWPORT_API bool eod_plume_view_mesh(Surface *surface, const native::scene::Vertex *vertices, uint32_t vertex_count,
                                        const uint32_t *indices, uint32_t index_count, const Batch *batches,
                                        uint32_t batch_count) {
    try {
        return surface && surface->replace_mesh(vertices, vertex_count, indices, index_count, batches, batch_count);
    } catch (...) {
        return false;
    }
}

EOD_VIEWPORT_API bool eod_plume_view_texture(Surface *surface, uint32_t slot, uint32_t width, uint32_t height,
                                           uint32_t alpha, const uint8_t *pixels, uint64_t bytes) {
    try {
        return surface && surface->replace_texture(slot, width, height, alpha, pixels, bytes);
    } catch (...) {
        return false;
    }
}

EOD_VIEWPORT_API void eod_plume_view_clear_textures(Surface *surface) {
    if (surface) surface->clear_textures();
}

EOD_VIEWPORT_API int eod_plume_view_draw(Surface *surface, uint32_t width, uint32_t height, const float *matrix,
                                       const float *fog, const float *clear) {
    try {
        return surface && matrix && fog && clear ? surface->draw(width, height, matrix, fog, clear) : -1;
    } catch (...) {
        return -1;
    }
}
