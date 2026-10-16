// Private, deliberately small adapter for the GS surface algorithm inherited
// from gs_d3d11.cpp. These are CPU state descriptions, not a Direct3D runtime:
// all allocation, synchronization and drawing below use Vulkan exclusively.
#pragma once
#include "present_timing.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <array>
#include <functional>
#include <stdexcept>
#include <string>
#include <vulkan/vulkan.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

using UINT = uint32_t;
using UINT8 = uint8_t;
using HRESULT = int;
using BOOL = bool;
#ifndef TRUE
constexpr bool TRUE = true;
#endif
#ifndef FALSE
constexpr bool FALSE = false;
#endif
constexpr int S_OK = 0;
#define SUCCEEDED(x) ((x) == 0)
#define FAILED(x) ((x) != 0)
using D3D11_BLEND = int;
using D3D11_BLEND_OP = int;
using D3D11_COMPARISON_FUNC = int;
using D3D11_MAP = int;
constexpr int D3D11_BLEND_ZERO = VK_BLEND_FACTOR_ZERO, D3D11_BLEND_ONE = VK_BLEND_FACTOR_ONE,
              D3D11_BLEND_DEST_ALPHA = VK_BLEND_FACTOR_DST_ALPHA,
              D3D11_BLEND_INV_DEST_ALPHA = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
              D3D11_BLEND_SRC1_ALPHA = VK_BLEND_FACTOR_SRC1_ALPHA, D3D11_BLEND_SRC1_COLOR = VK_BLEND_FACTOR_SRC1_COLOR,
              D3D11_BLEND_BLEND_FACTOR = VK_BLEND_FACTOR_CONSTANT_COLOR,
              D3D11_BLEND_INV_BLEND_FACTOR = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
              D3D11_BLEND_OP_ADD = VK_BLEND_OP_ADD, D3D11_BLEND_OP_SUBTRACT = VK_BLEND_OP_SUBTRACT,
              D3D11_BLEND_OP_REV_SUBTRACT = VK_BLEND_OP_REVERSE_SUBTRACT, D3D11_COMPARISON_NEVER = VK_COMPARE_OP_NEVER,
              D3D11_COMPARISON_ALWAYS = VK_COMPARE_OP_ALWAYS, D3D11_COMPARISON_GREATER = VK_COMPARE_OP_GREATER,
              D3D11_COMPARISON_GREATER_EQUAL = VK_COMPARE_OP_GREATER_OR_EQUAL, D3D11_DEPTH_WRITE_MASK_ALL = 1,
              D3D11_DEPTH_WRITE_MASK_ZERO = 0, D3D11_MAP_READ = 0, D3D11_MAP_WRITE_DISCARD = 1,
              D3D11_MAP_WRITE_NO_OVERWRITE = 2, D3D11_USAGE_DEFAULT = 0, D3D11_USAGE_STAGING = 1,
              D3D11_USAGE_IMMUTABLE = 2, D3D11_USAGE_DYNAMIC = 3, D3D11_BIND_SHADER_RESOURCE = 1,
              D3D11_BIND_RENDER_TARGET = 2, D3D11_BIND_DEPTH_STENCIL = 4, D3D11_BIND_VERTEX_BUFFER = 8,
              D3D11_BIND_CONSTANT_BUFFER = 16, D3D11_CPU_ACCESS_READ = 1, D3D11_CPU_ACCESS_WRITE = 2,
              D3D11_TEXTURE_ADDRESS_CLAMP = 1, D3D11_TEXTURE_ADDRESS_WRAP = 0, D3D11_FILTER_MIN_MAG_MIP_POINT = 0,
              D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT = 1,
              D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
              D3D11_PRIMITIVE_TOPOLOGY_LINELIST = VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
              D3D11_PRIMITIVE_TOPOLOGY_POINTLIST = VK_PRIMITIVE_TOPOLOGY_POINT_LIST, D3D11_DSV_DIMENSION_TEXTURE2D = 0,
              D3D11_SRV_DIMENSION_TEXTURE2D = 0, D3D11_CLEAR_DEPTH = 1, D3D11_COLOR_WRITE_ENABLE_ALL = 15,
              D3D11_FILL_SOLID = 0, D3D11_CULL_NONE = 0;
constexpr float D3D11_FLOAT32_MAX = 1000;
constexpr VkFormat DXGI_FORMAT_R8G8B8A8_UNORM = VK_FORMAT_R8G8B8A8_UNORM,
                   DXGI_FORMAT_R32_TYPELESS = VK_FORMAT_D32_SFLOAT, DXGI_FORMAT_D32_FLOAT = VK_FORMAT_D32_SFLOAT,
                   DXGI_FORMAT_R32_FLOAT = VK_FORMAT_R32_SFLOAT, DXGI_FORMAT_R32_UINT = VK_FORMAT_R32_UINT,
                   DXGI_FORMAT_R16G16_UINT = VK_FORMAT_R16G16_UINT;
struct D3D11_TEXTURE2D_DESC {
    UINT Width = 0, Height = 0, MipLevels = 1, ArraySize = 1;
    VkFormat Format = VK_FORMAT_UNDEFINED;
    struct {
        UINT Count = 1;
    } SampleDesc;
    int Usage = 0, BindFlags = 0, CPUAccessFlags = 0;
};
struct D3D11_SUBRESOURCE_DATA {
    const void *pSysMem;
    UINT SysMemPitch, SysMemSlicePitch;
};
struct D3D11_BUFFER_DESC {
    UINT ByteWidth = 0;
    int Usage = 0, BindFlags = 0, CPUAccessFlags = 0;
};
struct D3D11_MAPPED_SUBRESOURCE {
    void *pData;
    UINT RowPitch;
};
struct D3D11_BOX {
    UINT left, top, front, right, bottom, back;
};
struct D3D11_VIEWPORT {
    float TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth;
};
struct D3D11_RECT {
    int left, top, right, bottom;
};
struct D3D11_RENDER_TARGET_BLEND_DESC {
    bool BlendEnable = false;
    int SrcBlend = 0, DestBlend = 0, BlendOp = 0, SrcBlendAlpha = 1, DestBlendAlpha = 0, BlendOpAlpha = 0;
    uint8_t RenderTargetWriteMask = 15;
};
struct D3D11_BLEND_DESC {
    D3D11_RENDER_TARGET_BLEND_DESC RenderTarget[1];
};
struct D3D11_DEPTH_STENCIL_DESC {
    bool DepthEnable = false;
    int DepthFunc = VK_COMPARE_OP_ALWAYS, DepthWriteMask = 0;
};
struct D3D11_RASTERIZER_DESC {
    int FillMode = 0, CullMode = 0;
    bool DepthClipEnable = false, ScissorEnable = false;
};
struct D3D11_SAMPLER_DESC {
    int Filter = 0, AddressU = 1, AddressV = 1, AddressW = 1, ComparisonFunc = 0;
    float MaxLOD = 1000;
};
struct D3D11_DEPTH_STENCIL_VIEW_DESC {
    VkFormat Format;
    int ViewDimension;
};
struct D3D11_SHADER_RESOURCE_VIEW_DESC {
    VkFormat Format;
    int ViewDimension;
    struct {
        UINT MipLevels;
    } Texture2D;
};

namespace vkgs {
inline void require(VkResult r, const char *what) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string(what) + " VkResult=" + std::to_string(r));
}
struct HostBuffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void *map{};
    VkDeviceSize size{}, used{};
};
struct Context;
inline Context *ctx;
struct Ref {
    int refs = 1;
    virtual ~Ref() = default;
    void AddRef() { ++refs; }
    void Release() {
        if (!--refs)
            delete this;
    }
};
struct Texture : Ref {
    D3D11_TEXTURE2D_DESC desc;
    VkImage image{};
    VkImageView view{};
    VkDeviceMemory memory{};
    HostBuffer host;
    bool external = false;
    ~Texture();
};
struct Buffer : Ref {
    std::vector<uint8_t> data;
    explicit Buffer(size_t n) : data(n) {}
};
struct Shader : Ref {
    VkShaderModule module{};
    const char *entry{};
    int id{};
};
struct Blend : Ref {
    D3D11_BLEND_DESC desc;
};
struct Depth : Ref {
    D3D11_DEPTH_STENCIL_DESC desc;
};
struct Raster : Ref {
    D3D11_RASTERIZER_DESC desc;
};
struct Sampler : Ref {
    VkSampler sampler{};
};
struct Context {
    uint64_t submit_count = 0, wait_us = 0, pipeline_us = 0;
    static uint64_t now_us() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family{};
    VkCommandPool pool{};
    VkCommandBuffer cmd{};
    VkFence fence{};
    bool recording = false, rendering = false;
    VkDescriptorSetLayout descriptors{};
    VkPipelineLayout layout{};
    PFN_vkCmdPushDescriptorSetKHR push{};
    std::vector<HostBuffer> chunks;
    size_t chunkIndex = 0;
    std::vector<std::function<void()>> garbage;
    struct Flight {
        VkCommandPool pool{};
        VkCommandBuffer cmd{};
        VkFence fence{};
        VkSemaphore acquired{};
        std::vector<HostBuffer> chunks;
        std::vector<std::function<void()>> garbage;
        bool submitted = false;
    } spare;
    std::vector<VkSemaphore> presentDone;
    bool wantVsync = true; // snapshotted once from the launch configuration
    bool displayTiming = false;
    PFN_vkGetRefreshCycleDurationGOOGLE refreshDuration{};
    PFN_vkGetPastPresentationTimingGOOGLE pastPresentation{};
    PresentCadence cadence;
    uint32_t presentSequence = 0, refreshQueryCountdown = 0;
    static uint64_t display_clock_ns() {
#ifdef __APPLE__
        // MoltenVK passes desiredPresentTime to Metal's presentAtTime. Apple's
        // CACurrentMediaTime uses this mach_absolute_time epoch, in seconds.
        static const mach_timebase_info_data_t scale = [] {
            mach_timebase_info_data_t value;
            mach_timebase_info(&value);
            return value;
        }();
        const uint64_t ticks = mach_absolute_time();
        return (ticks / scale.denom) * scale.numer + (ticks % scale.denom) * scale.numer / scale.denom;
#else
        timespec time{};
        clock_gettime(CLOCK_MONOTONIC, &time);
        return uint64_t(time.tv_sec) * 1000000000 + time.tv_nsec;
#endif
    }
    uint64_t presentation_deadline() {
        if (!displayTiming)
            return 0;
        if (!refreshQueryCountdown) {
            VkRefreshCycleDurationGOOGLE duration{};
            cadence.refresh(refreshDuration(device, swap, &duration) == VK_SUCCESS ? duration.refreshDuration : 0);
            refreshQueryCountdown = 119;
        } else
            --refreshQueryCountdown;
        // Nonblocking feedback: the extension returns only completed presents.
        uint32_t count = 0;
        if (pastPresentation(device, swap, &count, nullptr) == VK_SUCCESS && count) {
            std::vector<VkPastPresentationTimingGOOGLE> times(std::min(count, 256u));
            count = (uint32_t)times.size();
            auto result = pastPresentation(device, swap, &count, times.data());
            if (result == VK_SUCCESS || result == VK_INCOMPLETE)
                for (uint32_t i = 0; i < count; ++i)
                    cadence.observe(times[i].actualPresentTime);
        }
        return cadence.schedule(display_clock_ns(), wantVsync);
    }
    VkPhysicalDeviceMemoryProperties memories{};
    std::unordered_map<std::string, VkPipeline> pipelines;
    Texture *dummy[4]{}, *rt{}, *ds{}, *resources[4]{};
    Sampler *samplers[2]{};
    Shader *vs{}, *ps{};
    Buffer *constants[2]{}, *vertices{};
    Blend *blend{};
    Depth *depth{};
    Raster *raster{};
    int topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    D3D11_VIEWPORT viewport{};
    D3D11_RECT scissor{};
    float blendFactor[4]{};
    VkSurfaceKHR surface{};
    VkSwapchainKHR swap{};
    SDL_Window *window{};
    VkExtent2D extent{};
    VkFormat swapFormat{};
    std::vector<Texture *> back;
    VkSemaphore acquired{}, complete{};
    uint32_t backIndex{};
    bool waitAcquire = false, signalPresent = false;
    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
        for (uint32_t i = 0; i < memories.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (memories.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        throw std::runtime_error("Vulkan memory type unavailable");
    }
    HostBuffer host_buffer(VkDeviceSize bytes) {
        HostBuffer b;
        b.size = bytes;
        VkBufferCreateInfo i{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        i.size = bytes;
        i.usage =
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        require(vkCreateBuffer(device, &i, nullptr, &b.buffer), "buffer");
        VkMemoryRequirements r;
        vkGetBufferMemoryRequirements(device, b.buffer, &r);
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        a.allocationSize = r.size;
        a.memoryTypeIndex =
            memory_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        require(vkAllocateMemory(device, &a, nullptr, &b.memory), "host memory");
        require(vkBindBufferMemory(device, b.buffer, b.memory, 0), "bind buffer");
        require(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.map), "map buffer");
        return b;
    }
    void begin() {
        if (recording)
            return;
        VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        require(vkBeginCommandBuffer(cmd, &b), "begin commands");
        recording = true;
    }
    void end_render() {
        if (rendering) {
            vkCmdEndRendering(cmd);
            rendering = false;
        }
    }
    void barrier() {
        begin();
        end_render();
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0,
                             nullptr, 0, nullptr);
    }
    void submit(bool synchronous = true) {
        if (!recording) {
            if (synchronous && spare.submitted) {
                uint64_t wait_start = now_us();
                require(vkWaitForFences(device, 1, &spare.fence, VK_TRUE, UINT64_MAX), "finish GPU");
                wait_us += now_us() - wait_start;
            }
            return;
        }
        end_render();
        require(vkEndCommandBuffer(cmd), "end commands");
        VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        s.commandBufferCount = 1;
        s.pCommandBuffers = &cmd;
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        if (waitAcquire) {
            s.waitSemaphoreCount = 1;
            s.pWaitSemaphores = &acquired;
            s.pWaitDstStageMask = &stage;
        }
        if (signalPresent) {
            s.signalSemaphoreCount = 1;
            s.pSignalSemaphores = &complete;
        }
        require(vkQueueSubmit(queue, 1, &s, fence), "submit");
        ++submit_count;
        waitAcquire = signalPresent = false;
        bool wait = synchronous;
        if (!synchronous) {
            // The other frame owns its command pool, mapped upload chunks and
            // deferred destruction until its fence completes. Queue order retains
            // image dependencies while CPU encoding overlaps the previous frame.
            std::swap(pool, spare.pool);
            std::swap(cmd, spare.cmd);
            std::swap(fence, spare.fence);
            std::swap(acquired, spare.acquired);
            chunks.swap(spare.chunks);
            garbage.swap(spare.garbage);
            wait = spare.submitted;
            spare.submitted = true;
        }
        if (wait) {
            uint64_t wait_start = now_us();
            require(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "recycle GPU frame");
            wait_us += now_us() - wait_start;
        }
        vkResetFences(device, 1, &fence);
        vkResetCommandPool(device, pool, 0);
        recording = false;
        for (auto &f : garbage)
            f();
        garbage.clear();
        for (auto &c : chunks)
            c.used = 0;
        chunkIndex = 0;
    }
    std::pair<HostBuffer *, VkDeviceSize> upload(const void *p, size_t bytes) {
        begin();
        while (chunkIndex < chunks.size() && chunks[chunkIndex].used + bytes > chunks[chunkIndex].size)
            ++chunkIndex;
        if (chunkIndex == chunks.size())
            chunks.push_back(host_buffer(std::max<size_t>(8 << 20, bytes)));
        auto &c = chunks[chunkIndex];
        auto off = c.used;
        c.used = (off + bytes + 255) & ~VkDeviceSize(255);
        memcpy((uint8_t *)c.map + off, p, bytes);
        return {&c, off};
    }
    void transition(Texture *t, VkImageLayout old, VkImageLayout next) {
        begin();
        end_render();
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = old == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = old;
        b.newLayout = next;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t->image;
        b.subresourceRange = {t->desc.Format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                     : VK_IMAGE_ASPECT_COLOR_BIT,
                              0, t->desc.MipLevels, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
    }
    int CreateTexture2D(const D3D11_TEXTURE2D_DESC *d, const D3D11_SUBRESOURCE_DATA *initial, Texture **out) {
        auto *t = new Texture;
        t->desc = *d;
        if (d->Usage == D3D11_USAGE_STAGING) {
            t->host = host_buffer((size_t)d->Width * d->Height * 4);
            *out = t;
            return 0;
        }
        VkImageCreateInfo i{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        i.imageType = VK_IMAGE_TYPE_2D;
        i.format = d->Format;
        i.extent = {d->Width, d->Height, 1};
        i.mipLevels = d->MipLevels;
        i.arrayLayers = 1;
        i.samples = VK_SAMPLE_COUNT_1_BIT;
        i.tiling = VK_IMAGE_TILING_OPTIMAL;
        i.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (d->BindFlags & D3D11_BIND_DEPTH_STENCIL)
            i.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (d->BindFlags & D3D11_BIND_RENDER_TARGET)
            i.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        require(vkCreateImage(device, &i, nullptr, &t->image), "image");
        VkMemoryRequirements r;
        vkGetImageMemoryRequirements(device, t->image, &r);
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        a.allocationSize = r.size;
        a.memoryTypeIndex = memory_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        require(vkAllocateMemory(device, &a, nullptr, &t->memory), "image memory");
        require(vkBindImageMemory(device, t->image, t->memory, 0), "bind image");
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v.image = t->image;
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.format = d->Format;
        v.subresourceRange = {d->Format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                              0, d->MipLevels, 0, 1};
        require(vkCreateImageView(device, &v, nullptr, &t->view), "image view");
        transition(t, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        if (initial) {
            for (UINT l = 0; l < d->MipLevels; l++) {
                D3D11_BOX box{0, 0, 0, std::max(1u, d->Width >> l), std::max(1u, d->Height >> l), 1};
                UpdateSubresource(t, l, &box, initial[l].pSysMem, initial[l].SysMemPitch, 0);
            }
        }
        *out = t;
        return 0;
    }
    int CreateRenderTargetView(Texture *t, const void *, Texture **out) {
        t->AddRef();
        *out = t;
        return 0;
    }
    int CreateDepthStencilView(Texture *t, const void *, Texture **out) {
        t->AddRef();
        *out = t;
        return 0;
    }
    int CreateShaderResourceView(Texture *t, const void *, Texture **out) {
        t->AddRef();
        *out = t;
        return 0;
    }
    int CreateBuffer(const D3D11_BUFFER_DESC *d, const void *, Buffer **o) {
        *o = new Buffer(d->ByteWidth);
        return 0;
    }
    int CreateBlendState(const D3D11_BLEND_DESC *d, Blend **o) {
        *o = new Blend;
        (*o)->desc = *d;
        return 0;
    }
    int CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC *d, Depth **o) {
        *o = new Depth;
        (*o)->desc = *d;
        return 0;
    }
    int CreateRasterizerState(const D3D11_RASTERIZER_DESC *d, Raster **o) {
        *o = new Raster;
        (*o)->desc = *d;
        return 0;
    }
    int CreateSamplerState(const D3D11_SAMPLER_DESC *d, Sampler **o) {
        *o = new Sampler;
        VkSamplerCreateInfo i{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        i.magFilter = i.minFilter = d->Filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        i.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        i.addressModeU = d->AddressU ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        i.addressModeV = d->AddressV ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        i.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        i.maxLod = d->MaxLOD;
        require(vkCreateSampler(device, &i, nullptr, &(*o)->sampler), "sampler");
        return 0;
    }
    void UpdateSubresource(Texture *t, UINT level, const D3D11_BOX *b, const void *p, UINT pitch, UINT) {
        barrier();
        UINT w = b->right - b->left, h = b->bottom - b->top;
        std::vector<uint8_t> tight((size_t)w * h * 4);
        for (UINT y = 0; y < h; y++)
            memcpy(tight.data() + (size_t)y * w * 4, (const uint8_t *)p + (size_t)y * pitch, (size_t)w * 4);
        auto u = upload(tight.data(), tight.size());
        VkBufferImageCopy c{};
        c.bufferOffset = u.second;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        c.imageOffset = {(int)b->left, (int)b->top, 0};
        c.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cmd, u.first->buffer, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    void CopySubresourceRegion(Texture *d, UINT, UINT x, UINT y, UINT, Texture *s, UINT, const D3D11_BOX *b) {
        barrier();
        auto aspect = s->desc.Format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        VkExtent3D sz{b->right - b->left, b->bottom - b->top, 1};
        if (d->host.buffer) {
            VkBufferImageCopy c{};
            c.bufferRowLength = d->desc.Width;
            c.imageSubresource = {aspect, 0, 0, 1};
            c.imageOffset = {(int)b->left, (int)b->top, 0};
            c.imageExtent = sz;
            vkCmdCopyImageToBuffer(cmd, s->image, VK_IMAGE_LAYOUT_GENERAL, d->host.buffer, 1, &c);
        } else {
            VkImageCopy c{};
            c.srcSubresource = {aspect, 0, 0, 1};
            c.dstSubresource = c.srcSubresource;
            c.srcOffset = {(int)b->left, (int)b->top, 0};
            c.dstOffset = {(int)x, (int)y, 0};
            c.extent = sz;
            vkCmdCopyImage(cmd, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        }
    }
    void CopyResource(Texture *d, Texture *s) {
        D3D11_BOX b{0, 0, 0, s->desc.Width, s->desc.Height, 1};
        CopySubresourceRegion(d, 0, 0, 0, 0, s, 0, &b);
    }
    int Map(Buffer *b, UINT, int, UINT, D3D11_MAPPED_SUBRESOURCE *m) {
        m->pData = b->data.data();
        return 0;
    }
    int Map(Texture *t, UINT, int, UINT, D3D11_MAPPED_SUBRESOURCE *m) {
        begin();
        end_render();
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr,
                             0, nullptr);
        submit();
        m->pData = t->host.map;
        m->RowPitch = t->desc.Width * 4;
        return 0;
    }
    template <class T> void Unmap(T *, UINT) {}
    void ClearRenderTargetView(Texture *t, const float *c) {
        barrier();
        VkClearColorValue v;
        memcpy(v.float32, c, 16);
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    }
    void ClearDepthStencilView(Texture *t, int, float depth, UINT) {
        barrier();
        VkClearDepthStencilValue v{depth, 0};
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    }
    void OMSetRenderTargets(UINT n, Texture *const *t, Texture *z) {
        auto *color = n ? *t : nullptr;
        if (rt != color || ds != z)
            end_render();
        rt = color;
        ds = z;
    }
    void OMSetBlendState(Blend *b, const float *f, UINT) {
        blend = b;
        memset(blendFactor, 0, 16);
        if (f)
            memcpy(blendFactor, f, 16);
    }
    void OMSetDepthStencilState(Depth *d, UINT) { depth = d; }
    void RSSetViewports(UINT, const D3D11_VIEWPORT *v) { viewport = *v; }
    void RSSetScissorRects(UINT, const D3D11_RECT *r) { scissor = *r; }
    void RSSetState(Raster *r) { raster = r; }
    void IASetPrimitiveTopology(int t) { topology = t; }
    void IASetInputLayout(void *) {}
    void IASetVertexBuffers(UINT, UINT, Buffer *const *b, const UINT *, const UINT *) { vertices = *b; }
    void VSSetShader(Shader *s, void *, UINT) { vs = s; }
    void PSSetShader(Shader *s, void *, UINT) { ps = s; }
    void VSSetConstantBuffers(UINT n, UINT, Buffer *const *b) { constants[n] = *b; }
    void PSSetConstantBuffers(UINT n, UINT, Buffer *const *b) { constants[n] = *b; }
    void PSSetShaderResources(UINT n, UINT count, Texture *const *t) {
        for (UINT i = 0; i < count; i++)
            resources[n + i] = t[i];
    }
    void PSSetSamplers(UINT n, UINT count, Sampler *const *s) {
        for (UINT i = 0; i < count; i++)
            samplers[n + i] = s[i];
    }
    void Draw(UINT count, UINT first);
    bool init(bool withWindow);
    bool resize();
    Texture *acquire(bool vsync);
    void present();
};
inline Texture::~Texture() {
    auto d = ctx->device;
    auto i = image;
    auto v = view;
    auto m = memory;
    auto b = host;
    bool ext = external;
    ctx->garbage.push_back([=] {
        if (v)
            vkDestroyImageView(d, v, nullptr);
        if (i && !ext)
            vkDestroyImage(d, i, nullptr);
        if (m)
            vkFreeMemory(d, m, nullptr);
        if (b.buffer) {
            vkUnmapMemory(d, b.memory);
            vkDestroyBuffer(d, b.buffer, nullptr);
            vkFreeMemory(d, b.memory, nullptr);
        }
    });
}
inline void Context::Draw(UINT count, UINT first) {
    if (!rt && !ds)
        return;
    if (!rendering)
        barrier(); // Makes uploads, previous attachment writes and feedback copies
                   // visible.
    std::string key;
    auto add = [&](auto v) { key.append((const char *)&v, sizeof v); };
    add(vs->id);
    add(ps->id);
    add(topology);
    add(rt ? rt->desc.Format : VK_FORMAT_UNDEFINED);
    add(ds ? ds->desc.Format : VK_FORMAT_UNDEFINED);
    const auto &b = blend->desc.RenderTarget[0];
    const auto &z = depth->desc;
    add(b.BlendEnable);
    add(b.SrcBlend);
    add(b.DestBlend);
    add(b.BlendOp);
    add(b.RenderTargetWriteMask);
    add(z.DepthEnable);
    add(z.DepthWriteMask);
    add(z.DepthFunc);
    VkPipeline pipeline;
    auto found = pipelines.find(key);
    if (found != pipelines.end())
        pipeline = found->second;
    else {
        uint64_t pipeline_start = now_us();
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (int i = 0; i < 2; i++) {
            auto *s = i ? ps : vs;
            stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            stages[i].module = s->module;
            stages[i].pName = s->entry;
        }
        VkVertexInputBindingDescription binding{0, 32, VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attrs[] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
                                                     {1, 0, VK_FORMAT_R32_UINT, 8},
                                                     {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 12},
                                                     {3, 0, VK_FORMAT_R32G32B32_SFLOAT, 16},
                                                     {4, 0, VK_FORMAT_R32_SFLOAT, 28}};
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        if (vs->id == 0) {
            input.vertexBindingDescriptionCount = 1;
            input.pVertexBindingDescriptions = &binding;
            input.vertexAttributeDescriptionCount = 5;
            input.pVertexAttributeDescriptions = attrs;
        }
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = (VkPrimitiveTopology)topology;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.depthClampEnable = VK_TRUE;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo dz{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        dz.depthTestEnable = z.DepthEnable;
        dz.depthWriteEnable = z.DepthWriteMask;
        dz.depthCompareOp = (VkCompareOp)z.DepthFunc;
        VkPipelineColorBlendAttachmentState cb{};
        cb.blendEnable = b.BlendEnable;
        cb.srcColorBlendFactor = b.BlendEnable ? (VkBlendFactor)b.SrcBlend : VK_BLEND_FACTOR_ONE;
        cb.dstColorBlendFactor = b.BlendEnable ? (VkBlendFactor)b.DestBlend : VK_BLEND_FACTOR_ZERO;
        cb.colorBlendOp = (VkBlendOp)b.BlendOp;
        cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        cb.alphaBlendOp = VK_BLEND_OP_ADD;
        cb.colorWriteMask = b.RenderTargetWriteMask;
        VkPipelineColorBlendStateCreateInfo blendInfo{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blendInfo.attachmentCount = rt ? 1 : 0;
        blendInfo.pAttachments = &cb;
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 3;
        dynamic.pDynamicStates = dyn;
        VkFormat color = rt ? rt->desc.Format : VK_FORMAT_UNDEFINED;
        VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        renderingInfo.colorAttachmentCount = rt ? 1 : 0;
        renderingInfo.pColorAttachmentFormats = &color;
        renderingInfo.depthAttachmentFormat = ds ? ds->desc.Format : VK_FORMAT_UNDEFINED;
        VkGraphicsPipelineCreateInfo p{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        p.pNext = &renderingInfo;
        p.stageCount = 2;
        p.pStages = stages;
        p.pVertexInputState = &input;
        p.pInputAssemblyState = &ia;
        p.pViewportState = &vp;
        p.pRasterizationState = &rs;
        p.pMultisampleState = &ms;
        p.pDepthStencilState = &dz;
        p.pColorBlendState = &blendInfo;
        p.pDynamicState = &dynamic;
        p.layout = layout;
        require(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &p, nullptr, &pipeline), "graphics pipeline");
        pipeline_us += now_us() - pipeline_start;
        pipelines.emplace(key, pipeline);
    }
    auto *target = rt ? rt : ds;
    if (!rendering) {
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},
            dep{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = rt ? rt->view : VK_NULL_HANDLE;
        color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        dep = color;
        dep.imageView = ds ? ds->view : VK_NULL_HANDLE;
        VkRenderingInfo render{VK_STRUCTURE_TYPE_RENDERING_INFO};
        render.renderArea.extent = {target->desc.Width, target->desc.Height};
        render.layerCount = 1;
        render.colorAttachmentCount = rt ? 1 : 0;
        render.pColorAttachments = &color;
        render.pDepthAttachment = ds ? &dep : nullptr;
        vkCmdBeginRendering(cmd, &render);
        rendering = true;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkViewport vp{viewport.TopLeftX, viewport.Height, -0.0f + viewport.Width, -viewport.Height, 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D rect{{0, 0}, {target->desc.Width, target->desc.Height}};
    if (raster && raster->desc.ScissorEnable) {
        rect.offset = {std::max(0, scissor.left), std::max(0, scissor.top)};
        rect.extent = {(uint32_t)std::max(0, scissor.right - rect.offset.x),
                       (uint32_t)std::max(0, scissor.bottom - rect.offset.y)};
    }
    vkCmdSetScissor(cmd, 0, 1, &rect);
    vkCmdSetBlendConstants(cmd, blendFactor);
    VkDescriptorImageInfo images[6]{};
    VkWriteDescriptorSet writes[6]{};
    for (int i = 0; i < 6; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstBinding = i < 4 ? i : i + 4;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[i].pImageInfo = &images[i];
        if (i < 4) {
            auto *t = resources[i] ? resources[i] : dummy[i];
            images[i].imageView = t->view;
            images[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        } else
            images[i].sampler = samplers[i - 4]->sampler;
    }
    push(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 6, writes);
    Buffer *cb = constants[vs->id == 0 ? 0 : 1];
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       (uint32_t)cb->data.size(), cb->data.data());
    if (vs->id == 0) {
        size_t bytes = (size_t)count * 32;
        auto u = upload(vertices->data.data() + (size_t)first * 32, bytes);
        vkCmdBindVertexBuffers(cmd, 0, 1, &u.first->buffer, &u.second);
        vkCmdDraw(cmd, count, 1, 0, 0);
    } else
        vkCmdDraw(cmd, count, 1, first, 0);
}
inline bool Context::init(bool withWindow) {
    wantVsync = g_config.vsync;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "TDEODRecomp";
    app.apiVersion = VK_API_VERSION_1_3;
    std::vector<const char *> exts;
    uint32_t n = 0;
    if (withWindow) {
        window = (SDL_Window *)platform_native_window();
        auto names = SDL_Vulkan_GetInstanceExtensions(&n);
        if (!names)
            throw std::runtime_error(SDL_GetError());
        exts.assign(names, names + n);
    }
    exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    VkInstanceCreateInfo i{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    i.pApplicationInfo = &app;
    i.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    i.enabledExtensionCount = (uint32_t)exts.size();
    i.ppEnabledExtensionNames = exts.data();
    require(vkCreateInstance(&i, nullptr, &instance), "instance");
    if (withWindow && !SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface))
        throw std::runtime_error(SDL_GetError());
    require(vkEnumeratePhysicalDevices(instance, &n, nullptr), "enumerate GPUs");
    std::vector<VkPhysicalDevice> devices(n);
    require(vkEnumeratePhysicalDevices(instance, &n, devices.data()), "GPUs");
    if (!n)
        throw std::runtime_error("No Vulkan GPU");
    physical = devices[std::min<size_t>(std::max(0, g_config.adapter), n - 1)];
    VkPhysicalDeviceProperties prop;
    vkGetPhysicalDeviceProperties(physical, &prop);
    fprintf(stderr, "[vulkan] %s API %u.%u.%u\n", prop.deviceName, VK_API_VERSION_MAJOR(prop.apiVersion),
            VK_API_VERSION_MINOR(prop.apiVersion), VK_API_VERSION_PATCH(prop.apiVersion));
    vkGetPhysicalDeviceMemoryProperties(physical, &memories);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, nullptr);
    std::vector<VkQueueFamilyProperties> queues(n);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, queues.data());
    bool got = false;
    for (uint32_t f = 0; f < n; f++) {
        VkBool32 present = VK_TRUE;
        if (surface)
            vkGetPhysicalDeviceSurfaceSupportKHR(physical, f, surface, &present);
        if ((queues[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            family = f;
            got = true;
            break;
        }
    }
    if (!got)
        throw std::runtime_error("No graphics/present queue");
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(physical, &features);
    if (!features.dualSrcBlend || !features.depthClamp)
        throw std::runtime_error("Vulkan GS requires dual-source blending and depth clamp");
    VkPhysicalDeviceFeatures enabled{};
    enabled.dualSrcBlend = VK_TRUE;
    enabled.depthClamp = VK_TRUE;
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &ne, nullptr);
    std::vector<VkExtensionProperties> available(ne);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &ne, available.data());
    auto has = [&](const char *s) {
        return std::any_of(available.begin(), available.end(), [&](auto &e) { return !strcmp(e.extensionName, s); });
    };
    std::vector<const char *> dext{VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    if (has("VK_KHR_portability_subset"))
        dext.push_back("VK_KHR_portability_subset");
    if (surface)
        dext.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    const char *timing = getenv("EOD_PRESENT_TIMING");
    // Fidelity bring-up uses ordinary presentation; high-refresh scheduling is opt-in.
    displayTiming = surface && has(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME) && timing && *timing == '1';
    if (displayTiming)
        dext.push_back(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
    float priority = 1;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = family;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    VkDeviceCreateInfo d{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    d.pNext = &f13;
    d.queueCreateInfoCount = 1;
    d.pQueueCreateInfos = &q;
    d.pEnabledFeatures = &enabled;
    d.enabledExtensionCount = (uint32_t)dext.size();
    d.ppEnabledExtensionNames = dext.data();
    require(vkCreateDevice(physical, &d, nullptr, &device), "device");
    vkGetDeviceQueue(device, family, 0, &queue);
    if (displayTiming) {
        refreshDuration = reinterpret_cast<PFN_vkGetRefreshCycleDurationGOOGLE>(
            vkGetDeviceProcAddr(device, "vkGetRefreshCycleDurationGOOGLE"));
        pastPresentation = reinterpret_cast<PFN_vkGetPastPresentationTimingGOOGLE>(
            vkGetDeviceProcAddr(device, "vkGetPastPresentationTimingGOOGLE"));
        displayTiming = refreshDuration && pastPresentation;
        if (displayTiming)
            fprintf(stderr, "[vulkan] refresh-grid presentation timing enabled\n");
    }
    push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR");
    if (!push)
        throw std::runtime_error("push descriptors unavailable");
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.queueFamilyIndex = family;
    require(vkCreateCommandPool(device, &cp, nullptr, &pool), "command pool");
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = pool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    require(vkAllocateCommandBuffers(device, &ca, &cmd), "command buffer");
    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    require(vkCreateFence(device, &fc, nullptr, &fence), "fence");
    // Two independent submissions allow CPU encoding and GPU rendering to overlap.
    require(vkCreateCommandPool(device, &cp, nullptr, &spare.pool), "second command pool");
    ca.commandPool = spare.pool;
    require(vkAllocateCommandBuffers(device, &ca, &spare.cmd), "second command buffer");
    require(vkCreateFence(device, &fc, nullptr, &spare.fence), "second fence");
    VkDescriptorSetLayoutBinding bindings[6]{};
    for (int j = 0; j < 6; j++) {
        bindings[j].binding = j < 4 ? j : j + 4;
        bindings[j].descriptorCount = 1;
        bindings[j].descriptorType = j < 4 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
        bindings[j].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dl.bindingCount = 6;
    dl.pBindings = bindings;
    require(vkCreateDescriptorSetLayout(device, &dl, nullptr, &descriptors), "descriptor layout");
    VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &descriptors;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    require(vkCreatePipelineLayout(device, &pl, nullptr, &layout), "pipeline layout");
    VkFormat formats[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R16G16_UINT};
    uint32_t zero = 0;
    D3D11_SUBRESOURCE_DATA init{&zero, 4, 0};
    for (int j = 0; j < 4; j++) {
        D3D11_TEXTURE2D_DESC td;
        td.Width = td.Height = 1;
        td.Format = formats[j];
        CreateTexture2D(&td, &init, &dummy[j]);
    }
    if (surface) {
        VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        require(vkCreateSemaphore(device, &sc, nullptr, &acquired), "acquire semaphore");
        require(vkCreateSemaphore(device, &sc, nullptr, &spare.acquired), "second acquire semaphore");
        resize();
    }
    return true;
}
inline bool Context::resize() {
    cadence = {};
    refreshQueryCountdown = 0;
    submit();
    vkQueueWaitIdle(queue);
    for (auto sem : presentDone)
        vkDestroySemaphore(device, sem, nullptr);
    presentDone.clear();
    for (auto *t : back)
        t->Release();
    back.clear();
    for (auto &f : garbage)
        f();
    garbage.clear();
    if (swap)
        vkDestroySwapchainKHR(device, swap, nullptr);
    swap = VK_NULL_HANDLE;
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0 || h <= 0)
        return false;
    VkSurfaceCapabilitiesKHR caps;
    require(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps), "surface caps");
    uint32_t n;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, formats.data());
    auto format = formats[0];
    for (auto f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            format = f;
            break;
        }
    swapFormat = format.format;
    extent = caps.currentExtent;
    if (extent.width == UINT32_MAX)
        extent = {(uint32_t)std::clamp(w, (int)caps.minImageExtent.width, (int)caps.maxImageExtent.width),
                  (uint32_t)std::clamp(h, (int)caps.minImageExtent.height, (int)caps.maxImageExtent.height)};
    VkSwapchainCreateInfoKHR s{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    s.surface = surface;
    s.minImageCount = std::max(2u, caps.minImageCount);
    if (caps.maxImageCount)
        s.minImageCount = std::min(s.minImageCount, caps.maxImageCount);
    s.imageFormat = format.format;
    s.imageColorSpace = format.colorSpace;
    s.imageExtent = extent;
    s.imageArrayLayers = 1;
    s.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    s.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    s.preTransform = caps.currentTransform;
    s.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    s.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!wantVsync) {
        uint32_t count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, nullptr);
        std::vector<VkPresentModeKHR> modes(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, modes.data());
        for (auto mode : modes)
            if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR || mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                s.presentMode = mode;
                break;
            }
    }
    s.clipped = VK_TRUE;
    require(vkCreateSwapchainKHR(device, &s, nullptr, &swap), "swapchain");
    vkGetSwapchainImagesKHR(device, swap, &n, nullptr);
    std::vector<VkImage> images(n);
    vkGetSwapchainImagesKHR(device, swap, &n, images.data());
    for (auto image : images) {
        auto *t = new Texture;
        t->external = true;
        t->image = image;
        t->desc.Width = extent.width;
        t->desc.Height = extent.height;
        t->desc.Format = swapFormat;
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v.image = image;
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.format = swapFormat;
        v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        require(vkCreateImageView(device, &v, nullptr, &t->view), "swap view");
        back.push_back(t);
        VkSemaphore sem;
        VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        require(vkCreateSemaphore(device, &sc, nullptr, &sem), "per-image present semaphore");
        presentDone.push_back(sem);
    }
    return true;
}
inline Texture *Context::acquire(bool /*frameVsync*/) {
    // gs.cpp may disable frame pacing transiently while catching up. A Vulkan
    // present-mode change requires swapchain replacement and a queue drain:
    // rebuilding on every late/on-time oscillation is more costly than the
    // missed frame. Honor the user's launch VSync setting for this swapchain;
    // frame skipping and the game's clock remain under the existing GS policy.
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0 || h <= 0)
        return nullptr;
    if (!swap || (uint32_t)w != extent.width || (uint32_t)h != extent.height)
        if (!resize())
            return nullptr;
    auto r = vkAcquireNextImageKHR(device, swap, UINT64_MAX, acquired, VK_NULL_HANDLE, &backIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        resize();
        return nullptr;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        require(r, "acquire swap image");
    waitAcquire = true;
    complete = presentDone[backIndex];
    auto *t = back[backIndex];
    transition(t, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    return t;
}
inline void Context::present() {
    auto *t = back[backIndex];
    transition(t, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    signalPresent = true;
    submit(false);
    VkPresentInfoKHR p{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    p.waitSemaphoreCount = 1;
    p.pWaitSemaphores = &complete;
    p.swapchainCount = 1;
    p.pSwapchains = &swap;
    p.pImageIndices = &backIndex;
    VkPresentTimeGOOGLE time{++presentSequence, presentation_deadline()};
    VkPresentTimesInfoGOOGLE timing{VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE};
    if (displayTiming) {
        timing.swapchainCount = 1;
        timing.pTimes = &time;
        p.pNext = &timing;
    }
    auto r = vkQueuePresentKHR(queue, &p);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        resize();
    else
        require(r, "present");
}
} // namespace vkgs
using ID3D11Texture2D = vkgs::Texture;
using ID3D11RenderTargetView = vkgs::Texture;
using ID3D11DepthStencilView = vkgs::Texture;
using ID3D11ShaderResourceView = vkgs::Texture;
using ID3D11Device = vkgs::Context;
using ID3D11DeviceContext = vkgs::Context;
using ID3D11Buffer = vkgs::Buffer;
using ID3D11VertexShader = vkgs::Shader;
using ID3D11PixelShader = vkgs::Shader;
using ID3D11InputLayout = void;
using ID3D11SamplerState = vkgs::Sampler;
using ID3D11BlendState = vkgs::Blend;
using ID3D11DepthStencilState = vkgs::Depth;
using ID3D11RasterizerState = vkgs::Raster;
