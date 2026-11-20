// The one Vulkan instance and device, owned by plume (docs/NATIVE_RENDERER.md, stage R0).
// The Vulkan GS (runtime/gs/vulkan/vk_compat.h) borrows these handles instead of creating
// its own, so the native renderer and the GS share one device. Windows only for now.
#pragma once
#ifdef EOD_PLUME
#include <volk.h>
#include <mutex>
#include <vector>

namespace plume {
struct RenderDevice;
}

namespace plume_device {
#ifdef __APPLE__
// Use SDL's loader for a window, or open the bundle/system loader for headless.
bool initialize_loader(PFN_vkGetInstanceProcAddr getProc);
#endif
// plume's instance (created on the first call, API 1.3, surface extensions enabled), or
// VK_NULL_HANDLE when plume cannot create one.
VkInstance instance();

// Creates plume's device on `physical` with every extension in `extensions` plus dynamic
// rendering (core 1.3 when `core13`, else VK_KHR_dynamic_rendering), and returns queue 0 of
// `family` with the mutex plume uses for that queue: every vkQueueSubmit, vkQueuePresentKHR,
// vkQueueWaitIdle and vkDeviceWaitIdle must hold it. False (logged) when plume picks another
// GPU, has no queue in `family`, or device creation fails; the caller then makes its own.
bool create_device(VkPhysicalDevice physical, uint32_t family, const std::vector<const char *> &extensions,
                   bool core13, VkDevice &device, VkQueue &queue, std::mutex *&queueMutex);

// plume's device once create_device succeeded (the native renderer draws with it), else null.
plume::RenderDevice *render_device();
} // namespace plume_device
#endif
