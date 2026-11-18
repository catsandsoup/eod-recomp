// plume owns the Vulkan instance and device; see plume_device.h.
#ifdef EOD_PLUME
#include "plume_device.h"
#include "plume_vulkan.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace plume {
// Defined in plume_vulkan.cpp; no plume header declares it.
std::unique_ptr<RenderInterface> CreateVulkanInterface();
} // namespace plume

namespace plume_device {
namespace {
// Never destroyed: the GS keeps using the device until the process exits, as it did with
// its own device (which it never destroyed either).
std::unique_ptr<plume::RenderInterface> g_interface;
plume::RenderDevice *g_device = nullptr;
bool g_tried = false;

struct DeviceRequest {
    const std::vector<const char *> *extensions;
    bool core13;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR fdr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
};

// Runs inside plume's VulkanDevice constructor, just before vkCreateDevice.
void add_gs_requirements(VkPhysicalDevice, std::vector<const char *> &extensions, void *&chain,
                         VkPhysicalDeviceFeatures &features, void *userData) {
    auto *req = static_cast<DeviceRequest *>(userData);
    auto enabled = [&](const char *name) {
        for (const char *e : extensions)
            if (!strcmp(e, name))
                return true;
        return false;
    };
    for (const char *e : *req->extensions)
        if (!enabled(e))
            extensions.push_back(e);
    if (req->core13) {
        req->f13.dynamicRendering = VK_TRUE;
        req->f13.pNext = chain;
        chain = &req->f13;
    } else {
        if (!enabled(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME))
            extensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
        req->fdr.dynamicRendering = VK_TRUE;
        req->fdr.pNext = chain;
        chain = &req->fdr;
    }
    // plume enables every supported core feature; the GS's device had robust buffer access
    // off. Keep it off (and robustness2's buffer/image parts, which depend on it) so the GS
    // runs as it did. robustness2's nullDescriptor, which plume uses, stays on.
    features.robustBufferAccess = VK_FALSE;
    for (auto *s = static_cast<VkBaseOutStructure *>(chain); s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) {
            auto *r = reinterpret_cast<VkPhysicalDeviceRobustness2FeaturesEXT *>(s);
            r->robustBufferAccess2 = VK_FALSE;
            r->robustImageAccess2 = VK_FALSE;
        }
}
} // namespace

VkInstance instance() {
    if (!g_tried) {
        g_tried = true;
        // EOD_PLUME=0: the GS creates its own instance and device (diagnostic A/B).
        if (const char *v = getenv("EOD_PLUME"); v && v[0] == '0') {
            fprintf(stderr, "[vulkan] EOD_PLUME=0: the GS creates its own instance and device\n");
            return VK_NULL_HANDLE;
        }
        plume::vulkanCreateOptions.applicationName = "TDEODRecomp";
        plume::vulkanCreateOptions.apiVersion = VK_API_VERSION_1_3;
        g_interface = plume::CreateVulkanInterface();
        if (!g_interface)
            fprintf(stderr, "[vulkan] plume could not create an instance: the GS creates its own\n");
    }
    return g_interface ? static_cast<plume::VulkanInterface *>(g_interface.get())->instance : VK_NULL_HANDLE;
}

bool create_device(VkPhysicalDevice physical, uint32_t family, const std::vector<const char *> &extensions,
                   bool core13, VkDevice &device, VkQueue &queue, std::mutex *&queueMutex) {
    if (!instance())
        return false;
    VkPhysicalDeviceProperties prop;
    vkGetPhysicalDeviceProperties(physical, &prop);
    DeviceRequest req{&extensions, core13};
    plume::vulkanCreateOptions.deviceCreateHook = add_gs_requirements;
    plume::vulkanCreateOptions.userData = &req;
    std::unique_ptr<plume::RenderDevice> created = g_interface->createDevice(prop.deviceName);
    plume::vulkanCreateOptions.deviceCreateHook = nullptr;
    plume::vulkanCreateOptions.userData = nullptr;
    if (!created) {
        fprintf(stderr, "[vulkan] plume could not create a device: the GS creates its own\n");
        return false;
    }
    auto *dev = static_cast<plume::VulkanDevice *>(created.get());
    if (dev->physicalDevice != physical) {
        fprintf(stderr, "[vulkan] plume chose another GPU (%s): the GS creates its own device\n",
                dev->description.name.c_str());
        return false;
    }
    if (family >= dev->queueFamilies.size() || dev->queueFamilies[family].queues.empty()) {
        fprintf(stderr, "[vulkan] plume made no queue in family %u: the GS creates its own device\n", family);
        return false;
    }
    plume::VulkanQueue &q = dev->queueFamilies[family].queues[0];
    // plume creates this lazily for its first command queue on q; creating it now, before any
    // plume queue exists, gives both users the same lock.
    if (!q.mutex)
        q.mutex = std::make_unique<std::mutex>();
    device = dev->vk;
    queue = q.vk;
    queueMutex = q.mutex.get();
    fprintf(stderr, "[vulkan] device owned by plume (queue family %u, %zu queues)\n", family,
            dev->queueFamilies[family].queues.size());
    g_device = created.release(); // lives for the process, like the interface
    return true;
}

plume::RenderDevice *render_device() { return g_device; }
} // namespace plume_device
#endif
