// Private, deliberately small adapter for the GS surface algorithm inherited
// from gs_d3d11.cpp. These are CPU state descriptions, not a Direct3D runtime:
// all allocation, synchronization and drawing below use Vulkan exclusively.
#pragma once
#include "present_timing.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <functional>
#include <stdexcept>
#include <string>
#include <vulkan/vulkan.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/qos.h>
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
    // Command-buffer serials: `serial` is the buffer being (or next) recorded;
    // every serial <= `completed` has finished on the GPU. A fence signal
    // covers all earlier submissions on this queue, so one wait suffices.
    uint64_t serial = 1, completed = 0;
    // Render-pass ends by cause: submit, upload, copy, clear, map, target change, layout.
    uint64_t pass_ends[7] = {}, passes = 0;
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
    // All graphics pipelines share one layout and make these states dynamic.
    // Like Granite's flush_render_state, emit only changed command state.
    // Cache Vulkan handles, not wrapper addresses (which allocators may reuse).
    bool cacheState = true;
    VkPipeline boundPipeline{};
    bool dynamicValid = false, imagesValid = false;
    VkViewport boundViewport{};
    VkRect2D boundScissor{};
    float boundBlend[4]{};
    VkDescriptorImageInfo boundImages[6]{};
    // Push constants persist across binds of pipelines sharing one layout.
    bool constantsValid = false;
    uint32_t boundConstantBytes = 0;
    uint8_t boundConstants[128]{};
    // Vertex buffer binding and the batch window uploaded once for its draws.
    bool vertexValid = false;
    VkBuffer boundVertexBuffer{};
    VkDeviceSize boundVertexOffset{};
    Buffer *windowSource{};
    UINT windowFirst = 0, windowCount = 0;
    bool windowUploaded = false;
    VkBuffer windowBuffer{};
    VkDeviceSize windowOffset{};
    uint32_t fragmentSpecialization = 0; // ps_draw constant_id 0 (alpha-test half)
    VkDescriptorSetLayout descriptors{};
    VkPipelineLayout layout{};
    PFN_vkCmdPushDescriptorSetKHR push{};
    std::vector<HostBuffer> chunks;
    size_t chunkIndex = 0;
    std::vector<std::function<void()>> garbage;
    // Submitted command buffers still owned by the GPU, oldest first, and
    // recycled ones. Each owns its pool, upload chunks and deferred destruction
    // until its fence signals. Queue order keeps every image dependency.
    struct Flight {
        VkCommandPool pool{};
        VkCommandBuffer cmd{};
        VkFence fence{};
        std::vector<HostBuffer> chunks;
        std::vector<std::function<void()>> garbage;
        uint64_t serial = 0;
    };
    std::deque<Flight> inflight;
    std::vector<Flight> idle;
    VkSemaphore spareAcquired{};
    // Serial of the last frame-end submission: the next frame end waits for it.
    uint64_t frameSerial = 0;
    // Mid-frame submissions (see Draw): after this many draws a new render pass
    // first submits the commands recorded so far, so MoltenVK encodes them and
    // the GPU runs them while the GS thread records the rest of the frame.
    uint32_t chunkDraws = 0, drawsSinceSubmit = 0;
    uint64_t chunk_submits = 0;
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
    struct PipelineKey {
        int32_t v[16]; // ..., [13] alpha-test half, [14] ps_draw flags, [15] flags specialized
        bool operator==(const PipelineKey &o) const { return !memcmp(v, o.v, sizeof v); }
    };
    struct PipelineKeyHash {
        size_t operator()(const PipelineKey &k) const {
            uint64_t h = 1469598103934665603ull;
            for (int32_t x : k.v)
                h = (h ^ (uint32_t)x) * 1099511628211ull;
            return (size_t)h;
        }
    };
    std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipelines;
    // Two-pass alpha draws alternate between two pipelines: check recent keys first.
    PipelineKey recentKeys[4]{};
    // Specialized ps_draw pipelines (EOD_VK_SPECIALIZE: 0 off, 1 background, 2 synchronous).
    static constexpr size_t kDrawFlagsOffset = 52; // DrawCB::flags
    int specializeMode = 1;
    Shader *shaderTable[16]{};
    struct Specializer {
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<PipelineKey> queue;
        std::vector<std::pair<PipelineKey, VkPipeline>> done;
        std::atomic<bool> ready{false};
        uint64_t compiled = 0, compile_us = 0; // under mutex
    } *specializer = nullptr;
    VkPipeline recentPipelines[4]{};
    unsigned recentNext = 0;
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
        // A reset/recycled command buffer inherits no graphics state, including
        // when the two in-flight command pools exchange ownership.
        boundPipeline = VK_NULL_HANDLE;
        dynamicValid = imagesValid = constantsValid = vertexValid = false;
        windowUploaded = false;
    }
    // Diagnostic (EOD_VK_PASS_LOG=1): one stderr line per render pass.
    int passLog = -1;
    struct PassDiag { uint32_t w, h, rtf, ds, draws, verts, blend, spec, pass_ps, dwrite, sx0, sy0, sx1, sy1; double area; } pd{};
    void end_render(int cause = 0) {
        if (rendering) {
            if (passLog > 0)
                fprintf(stderr, "PASS %llu c%d %ux%u rt%u ds%u draws %u verts %u blend %u atest %u passps %u zw %u sc %u,%u-%u,%u area %.0f\n",
                        (unsigned long long)passes, cause, pd.w, pd.h, pd.rtf, pd.ds, pd.draws, pd.verts, pd.blend,
                        pd.spec, pd.pass_ps, pd.dwrite, pd.sx0, pd.sy0, pd.sx1, pd.sy1, pd.area);
            ++pass_ends[cause];
            vkCmdEndRendering(cmd);
            rendering = false;
        }
    }
    void barrier(int cause = 0) {
        begin();
        end_render(cause);
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0,
                             nullptr, 0, nullptr);
    }
    // Recycles a completed flight: its commands, uploads and garbage are done.
    void recycle(Flight &f) {
        completed = std::max(completed, f.serial);
        for (auto &g : f.garbage)
            g();
        f.garbage.clear();
        for (auto &c : f.chunks)
            c.used = 0;
        vkResetFences(device, 1, &f.fence);
        vkResetCommandPool(device, f.pool, 0);
        idle.push_back(std::move(f));
    }
    // Retires submitted flights in order: every one with serial <= `upTo` is
    // waited for, later ones only if already complete.
    bool retire(uint64_t upTo) {
        bool waited = false;
        while (!inflight.empty()) {
            Flight &f = inflight.front();
            if (vkGetFenceStatus(device, f.fence) != VK_SUCCESS) {
                if (f.serial > upTo)
                    break;
                uint64_t wait_start = now_us();
                require(vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX), "retire GPU work");
                wait_us += now_us() - wait_start;
                waited = true;
            }
            Flight done = std::move(inflight.front());
            inflight.pop_front();
            recycle(done);
        }
        return waited;
    }
    Flight new_flight() {
        Flight f;
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cp.queueFamilyIndex = family;
        require(vkCreateCommandPool(device, &cp, nullptr, &f.pool), "command pool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = f.pool;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        require(vkAllocateCommandBuffers(device, &ca, &f.cmd), "command buffer");
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        require(vkCreateFence(device, &fc, nullptr, &f.fence), "fence");
        return f;
    }
    // synchronous: submit and wait for all GPU work. Otherwise `frameEnd`
    // submits and then waits for the previous frame end's submission (one frame
    // in flight, as with two alternating command buffers); a mid-frame kick
    // only submits. The recorded commands move to a flight the GPU owns.
    void submit(bool synchronous = true, bool frameEnd = true) {
        if (!recording) {
            if (synchronous)
                retire(UINT64_MAX);
            else if (frameEnd)
                retire(frameSerial);
            return;
        }
        end_render();
        if (passLog > 0)
            fprintf(stderr, "SUBMIT %s\n", synchronous ? "sync" : frameEnd ? "frame" : "kick");
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
        windowUploaded = false;
        ++submit_count;
        drawsSinceSubmit = 0;
        const bool presented = waitAcquire || signalPresent;
        waitAcquire = signalPresent = false;
        const uint64_t submitted = serial++;
        recording = false;
        Flight done;
        done.pool = pool;
        done.cmd = cmd;
        done.fence = fence;
        done.chunks.swap(chunks);
        done.garbage.swap(garbage);
        done.serial = submitted;
        inflight.push_back(std::move(done));
        if (synchronous)
            retire(UINT64_MAX);
        else if (frameEnd) {
            // The acquire semaphore alternates by frame: the previous frame's
            // present submission has completed before its semaphore is reused.
            if (presented)
                std::swap(acquired, spareAcquired);
            const uint64_t previous = frameSerial;
            frameSerial = submitted;
            retire(previous);
        } else
            retire(0);
        // Bound the GPU-owned command buffers and their upload memory.
        while (inflight.size() > 32)
            retire(inflight.front().serial);
        Flight next;
        if (!idle.empty()) {
            next = std::move(idle.back());
            idle.pop_back();
        } else
            next = new_flight();
        pool = next.pool;
        cmd = next.cmd;
        fence = next.fence;
        chunks.swap(next.chunks);
        chunkIndex = 0;
    }
    // Makes earlier transfer writes visible to host reads once this command
    // buffer completes, without submitting it (see wait_serial).
    void host_read_barrier() {
        begin();
        end_render(4);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr,
                             0, nullptr);
    }
    // Waits until command buffer `s` (recorded or already submitted) completes.
    // Returns true when the CPU actually had to submit or block.
    bool wait_serial(uint64_t s) {
        if (s <= completed)
            return false;
        if (s >= serial) {
            if (!recording)
                return false;
            submit(true);
            return true;
        }
        // A fence covers its submission and every earlier one on this queue.
        return retire(s);
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
        end_render(6);
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
        barrier(1);
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
        barrier(2);
        auto aspect = s->desc.Format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        VkExtent3D sz{b->right - b->left, b->bottom - b->top, 1};
        if (d->host.buffer) {
            VkBufferImageCopy c{};
            c.bufferOffset = ((VkDeviceSize)y * d->desc.Width + x) * 4; // destination texel (x, y)
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
        end_render(4);
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
        barrier(3);
        VkClearColorValue v;
        memcpy(v.float32, c, 16);
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    }
    void ClearDepthStencilView(Texture *t, int, float depth, UINT) {
        barrier(3);
        VkClearDepthStencilValue v{depth, 0};
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    }
    void OMSetRenderTargets(UINT n, Texture *const *t, Texture *z) {
        auto *color = n ? *t : nullptr;
        if (rt != color || ds != z)
            end_render(5);
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
    // Vertices [first, first + count) of b are final until the next window: their
    // draws share one upload and binding (vkCmdDraw firstVertex selects a range).
    void SetVertexWindow(Buffer *b, UINT first, UINT count) {
        windowSource = b;
        windowFirst = first;
        windowCount = count;
        windowUploaded = false;
    }
    void PSSetSpecialization(uint32_t value) { fragmentSpecialization = value; }
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
    VkPipeline create_pipeline(const PipelineKey &key);
    VkPipeline find_pipeline(const PipelineKey &key);
    void remember(const PipelineKey &key, VkPipeline pipeline);
    VkPipeline specialized(const PipelineKey &key);
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
inline VkPipeline Context::create_pipeline(const PipelineKey &k) {
    // Everything comes from the key (and the shader modules of its ids), so
    // the asynchronous specializer can build the same pipeline off-thread.
    Shader *vs = shaderTable[k.v[0]], *ps = shaderTable[k.v[1]];
    const int topology = k.v[2];
    const bool hasRt = k.v[3] != VK_FORMAT_UNDEFINED, hasDs = k.v[4] != VK_FORMAT_UNDEFINED;
    D3D11_RENDER_TARGET_BLEND_DESC b;
    b.BlendEnable = k.v[5];
    b.SrcBlend = k.v[6];
    b.DestBlend = k.v[7];
    b.BlendOp = k.v[8];
    b.RenderTargetWriteMask = (uint8_t)k.v[9];
    D3D11_DEPTH_STENCIL_DESC z;
    z.DepthEnable = k.v[10];
    z.DepthWriteMask = k.v[11];
    z.DepthFunc = k.v[12];
    const uint32_t specialization = (uint32_t)k.v[13];
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (int i = 0; i < 2; i++) {
        auto *s = i ? ps : vs;
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        stages[i].module = s->module;
        stages[i].pName = s->entry;
    }
    const uint32_t specData[3] = {specialization, (uint32_t)k.v[14], (uint32_t)k.v[15]};
    VkSpecializationMapEntry specEntry[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo specInfo{3, specEntry, sizeof specData, specData};
    if (ps->id == 1)
        stages[1].pSpecializationInfo = &specInfo;
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
    blendInfo.attachmentCount = hasRt ? 1 : 0;
    blendInfo.pAttachments = &cb;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                            VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 3;
    dynamic.pDynamicStates = dyn;
    VkFormat color = (VkFormat)k.v[3];
    VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    renderingInfo.colorAttachmentCount = hasRt ? 1 : 0;
    renderingInfo.pColorAttachmentFormats = &color;
    renderingInfo.depthAttachmentFormat = hasDs ? (VkFormat)k.v[4] : VK_FORMAT_UNDEFINED;
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
    return pipeline;
}
inline VkPipeline Context::find_pipeline(const PipelineKey &key) {
    for (unsigned i = 0; i < 4; i++)
        if (recentPipelines[i] && recentKeys[i] == key)
            return recentPipelines[i];
    auto found = pipelines.find(key);
    if (found == pipelines.end() || !found->second)
        return VK_NULL_HANDLE;
    remember(key, found->second);
    return found->second;
}
inline void Context::remember(const PipelineKey &key, VkPipeline pipeline) {
    recentKeys[recentNext] = key;
    recentPipelines[recentNext] = pipeline;
    recentNext = (recentNext + 1) % 4;
}
// Returns the specialized pipeline if it can be had now: synchronously in mode
// 2 (deterministic, for comparisons), else queued once for a background thread
// (VK_NULL_HANDLE until it is ready).
inline VkPipeline Context::specialized(const PipelineKey &key) {
    if (specializeMode == 2) {
        uint64_t pipeline_start = now_us();
        VkPipeline pipeline = create_pipeline(key);
        pipeline_us += now_us() - pipeline_start;
        pipelines.emplace(key, pipeline);
        remember(key, pipeline);
        return pipeline;
    }
    if (specializer->ready.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(specializer->mutex);
        for (auto &done : specializer->done)
            pipelines[done.first] = done.second;
        specializer->done.clear();
        specializer->ready.store(false, std::memory_order_relaxed);
        auto found = pipelines.find(key);
        if (found != pipelines.end() && found->second) {
            remember(key, found->second);
            return found->second;
        }
    }
    if (pipelines.emplace(key, VK_NULL_HANDLE).second) { // first request
        std::lock_guard<std::mutex> lock(specializer->mutex);
        specializer->queue.push_back(key);
        specializer->wake.notify_one();
    }
    return VK_NULL_HANDLE;
}
inline void Context::Draw(UINT count, UINT first) {
    if (!rt && !ds)
        return;
    if (!rendering && chunkDraws && recording && drawsSinceSubmit >= chunkDraws && !waitAcquire && !signalPresent) {
        // Between render passes: hand the commands so far to MoltenVK/the GPU.
        ++chunk_submits;
        submit(false, false);
    }
    ++drawsSinceSubmit;
    if (!rendering)
        barrier(); // Makes uploads, previous attachment writes and feedback copies
                   // visible.
    const auto &b = blend->desc.RenderTarget[0];
    const auto &z = depth->desc;
    const uint32_t specialization = ps->id == 1 ? fragmentSpecialization : 0;
    PipelineKey key{{vs->id, ps->id, topology, (int32_t)(rt ? rt->desc.Format : VK_FORMAT_UNDEFINED),
                     (int32_t)(ds ? ds->desc.Format : VK_FORMAT_UNDEFINED), b.BlendEnable, b.SrcBlend, b.DestBlend,
                     b.BlendOp, b.RenderTargetWriteMask, z.DepthEnable, z.DepthWriteMask, z.DepthFunc,
                     (int32_t)specialization, 0, 0}};
    if (shaderTable[vs->id] != vs) // set once: the specializer thread reads it
        shaderTable[vs->id] = vs;
    if (shaderTable[ps->id] != ps)
        shaderTable[ps->id] = ps;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (specializeMode && ps->id == 1) {
        // ps_draw specialized for this draw's flags (identical results, fewer
        // instructions/registers). Until it exists, the generic pipeline draws.
        PipelineKey special = key;
        memcpy(&special.v[14], constants[0]->data.data() + kDrawFlagsOffset, 4);
        special.v[15] = 1;
        pipeline = find_pipeline(special);
        if (!pipeline)
            pipeline = specialized(special);
    }
    if (!pipeline)
        pipeline = find_pipeline(key);
    if (!pipeline) {
        uint64_t pipeline_start = now_us();
        pipeline = create_pipeline(key);
        pipeline_us += now_us() - pipeline_start;
        pipelines.emplace(key, pipeline);
        remember(key, pipeline);
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
        ++passes;
        if (passLog < 0) { const char *e = getenv("EOD_VK_PASS_LOG"); passLog = e && *e == '1'; }
        if (passLog > 0) { pd = {}; pd.w = target->desc.Width; pd.h = target->desc.Height; pd.rtf = rt ? rt->desc.Format : 0; pd.ds = ds != nullptr; pd.sx0 = pd.sy0 = ~0u; }
    }
    if (!cacheState || boundPipeline != pipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        boundPipeline = pipeline;
    }
    VkViewport vp{viewport.TopLeftX, viewport.Height, -0.0f + viewport.Width, -viewport.Height, 0, 1};
    if (!cacheState || !dynamicValid || memcmp(&boundViewport, &vp, sizeof vp)) {
        vkCmdSetViewport(cmd, 0, 1, &vp);
        boundViewport = vp;
    }
    VkRect2D rect{{0, 0}, {target->desc.Width, target->desc.Height}};
    if (raster && raster->desc.ScissorEnable) {
        rect.offset = {std::max(0, scissor.left), std::max(0, scissor.top)};
        rect.extent = {(uint32_t)std::max(0, scissor.right - rect.offset.x),
                       (uint32_t)std::max(0, scissor.bottom - rect.offset.y)};
    }
    if (!cacheState || !dynamicValid || memcmp(&boundScissor, &rect, sizeof rect)) {
        vkCmdSetScissor(cmd, 0, 1, &rect);
        boundScissor = rect;
    }
    if (!cacheState || !dynamicValid || memcmp(boundBlend, blendFactor, sizeof boundBlend)) {
        vkCmdSetBlendConstants(cmd, blendFactor);
        memcpy(boundBlend, blendFactor, sizeof boundBlend);
    }
    dynamicValid = true;
    if (passLog > 0) {
        pd.draws++; pd.verts += count; pd.blend += b.BlendEnable; pd.spec += specialization != 0; pd.pass_ps += vs->id != 0;
        pd.dwrite += z.DepthEnable && z.DepthWriteMask;
        if (vs->id == 0 && topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            for (UINT t = 0; t + 2 < count; t += 3) {
                const float *a = (const float *)(vertices->data.data() + (size_t)(first + t) * 32);
                const float *b = (const float *)(vertices->data.data() + (size_t)(first + t + 1) * 32);
                const float *c = (const float *)(vertices->data.data() + (size_t)(first + t + 2) * 32);
                pd.area += std::fabs((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1])) * 0.5;
            }
        pd.sx0 = std::min<uint32_t>(pd.sx0, rect.offset.x); pd.sy0 = std::min<uint32_t>(pd.sy0, rect.offset.y);
        pd.sx1 = std::max<uint32_t>(pd.sx1, rect.offset.x + rect.extent.width); pd.sy1 = std::max<uint32_t>(pd.sy1, rect.offset.y + rect.extent.height);
    }
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
    if (!cacheState || !imagesValid || memcmp(boundImages, images, sizeof images)) {
        push(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 6, writes);
        memcpy(boundImages, images, sizeof images);
        imagesValid = true;
    }
    Buffer *cb = constants[vs->id == 0 ? 0 : 1];
    const uint32_t constantBytes = (uint32_t)cb->data.size();
    if (!cacheState || !constantsValid || constantBytes != boundConstantBytes ||
        memcmp(boundConstants, cb->data.data(), constantBytes)) {
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, constantBytes,
                           cb->data.data());
        memcpy(boundConstants, cb->data.data(), constantBytes);
        boundConstantBytes = constantBytes;
        constantsValid = true;
    }
    if (vs->id == 0) {
        VkBuffer buffer;
        VkDeviceSize offset;
        UINT start = 0;
        if (cacheState && windowSource == vertices && first >= windowFirst &&
            (uint64_t)first + count <= (uint64_t)windowFirst + windowCount) {
            if (!windowUploaded) {
                auto u = upload(vertices->data.data() + (size_t)windowFirst * 32, (size_t)windowCount * 32);
                windowBuffer = u.first->buffer;
                windowOffset = u.second;
                windowUploaded = true;
            }
            buffer = windowBuffer;
            offset = windowOffset;
            start = first - windowFirst;
        } else {
            auto u = upload(vertices->data.data() + (size_t)first * 32, (size_t)count * 32);
            buffer = u.first->buffer;
            offset = u.second;
        }
        if (!cacheState || !vertexValid || boundVertexBuffer != buffer || boundVertexOffset != offset) {
            vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &offset);
            boundVertexBuffer = buffer;
            boundVertexOffset = offset;
            vertexValid = true;
        }
        vkCmdDraw(cmd, count, 1, start, 0);
    } else
        vkCmdDraw(cmd, count, 1, first, 0);
}
inline bool Context::init(bool withWindow) {
    wantVsync = g_config.vsync;
    const char *stateCache = getenv("EOD_VK_STATE_CACHE");
    cacheState = !stateCache || stateCache[0] != '0';
#ifdef __APPLE__
    // MoltenVK encodes the whole Metal command buffer inside vkQueueSubmit. Its
    // asynchronous submission mode runs that encoding on MoltenVK's own queue
    // thread, overlapping the GS thread's recording of the next commands.
    // Queue order, fences and presentation semantics are unchanged; a user
    // setting of the variable still wins.
    setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "0", 0);
#endif
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
    if (const char *v = getenv("EOD_VK_CHUNK_DRAWS"))
        chunkDraws = (uint32_t)std::max(0, atoi(v));
    else
        chunkDraws = 1024;
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
    if (const char *v = getenv("EOD_VK_SPECIALIZE"))
        specializeMode = std::clamp(atoi(v), 0, 2);
    if (specializeMode == 1) {
        specializer = new Specializer; // lives for the process (no join at exit)
        std::thread([this, sp = specializer] {
#ifdef __APPLE__
            // Below the EE/GS threads: the compile (and Metal's compiler
            // service, which inherits the QoS) uses otherwise idle cores.
            pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
            for (;;) {
                PipelineKey key;
                {
                    std::unique_lock<std::mutex> lock(sp->mutex);
                    sp->wake.wait(lock, [sp] { return !sp->queue.empty(); });
                    key = sp->queue.front();
                    sp->queue.pop_front();
                }
                const uint64_t start = now_us();
                VkPipeline pipeline = create_pipeline(key);
                const uint64_t took = now_us() - start;
                std::lock_guard<std::mutex> lock(sp->mutex);
                sp->compiled++;
                sp->compile_us += took;
                sp->done.emplace_back(key, pipeline);
                sp->ready.store(true, std::memory_order_release);
            }
        }).detach();
    }
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
        require(vkCreateSemaphore(device, &sc, nullptr, &spareAcquired), "second acquire semaphore");
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
