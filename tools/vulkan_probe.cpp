// tools/vulkan_probe.cpp — Phase 3, milestone 1: toolchain/driver probe.
//
// Creates a surfaceless Vulkan instance, enumerates the physical devices,
// and picks the best compute-capable one. Exit code 0 means the renderer
// prerequisites hold on this machine (loader, driver, compute queue);
// anything else prints the failure and exits 1.

#include <vulkan/vulkan.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

[[nodiscard]] const char* result_name(VkResult r) {
    switch (r) {
        case VK_SUCCESS:
            return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_HOST_MEMORY:
            return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:
            return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_EXTENSION_NOT_PRESENT:
            return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_DEVICE_LOST:
            return "VK_ERROR_DEVICE_LOST";
        default:
            return "VK_ERROR_*";
    }
}

struct Device {
    VkPhysicalDevice handle;
    std::string name;
    VkPhysicalDeviceType type;
    uint32_t api;
    bool has_compute = false;
};

} // namespace

int main() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "wolf3d";
    app.apiVersion = VK_MAKE_API_VERSION(0, 1, 3, 0);
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    const VkResult ri = vkCreateInstance(&ci, nullptr, &inst);
    if (ri != VK_SUCCESS) {
        std::printf("vkCreateInstance failed: %s\n", result_name(ri));
        return 1;
    }

    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(inst, &count, nullptr) != VK_SUCCESS ||
        count == 0) {
        std::printf("no physical devices\n");
        vkDestroyInstance(inst, nullptr);
        return 1;
    }
    std::vector<VkPhysicalDevice> handles(count);
    vkEnumeratePhysicalDevices(inst, &count, handles.data());

    std::vector<Device> devices;
    int best = -1;
    uint32_t best_api = 0;
    for (uint32_t i = 0; i < count; ++i) {
        Device d;
        d.handle = handles[i];
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d.handle, &p);
        d.name = p.deviceName;
        d.type = p.deviceType;
        d.api = p.apiVersion;

        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d.handle, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        if (qn)
            vkGetPhysicalDeviceQueueFamilyProperties(d.handle, &qn, qf.data());
        for (const auto& q : qf)
            if (q.queueFlags & VK_QUEUE_COMPUTE_BIT)
                d.has_compute = true;

        devices.push_back(d);
        const char* tname =
            d.type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete"
            : d.type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated"
            : d.type == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? "virtual"
            : d.type == VK_PHYSICAL_DEVICE_TYPE_CPU ? "cpu"
                                                    : "other";
        std::printf("GPU %u: %s (%s) api=%u.%u.%u compute=%d\n", i, d.name.c_str(),
                    tname, VK_API_VERSION_MAJOR(d.api), VK_API_VERSION_MINOR(d.api),
                    VK_API_VERSION_PATCH(d.api), d.has_compute ? 1 : 0);
        if (d.has_compute && (best < 0 || d.api >= best_api)) {
            best = static_cast<int>(i);
            best_api = d.api;
        }
    }

    if (best < 0) {
        std::printf("no compute-capable device\n");
        vkDestroyInstance(inst, nullptr);
        return 1;
    }
    const Device& b = devices[static_cast<std::size_t>(best)];
    VkPhysicalDeviceProperties bp{};
    vkGetPhysicalDeviceProperties(b.handle, &bp);
    const VkPhysicalDeviceLimits& lim = bp.limits;
    std::printf("selected GPU %d (%s)\n", best, b.name.c_str());
    std::printf("  maxComputeWorkGroupCount = [%u,%u,%u]\n",
                lim.maxComputeWorkGroupCount[0], lim.maxComputeWorkGroupCount[1],
                lim.maxComputeWorkGroupCount[2]);
    std::printf("  maxComputeWorkGroupInvocations = %u\n",
                lim.maxComputeWorkGroupInvocations);
    std::printf("  maxUBOSize = %u\n", lim.maxUniformBufferRange);
    std::printf("VULKAN_PROBE_OK\n");
    vkDestroyInstance(inst, nullptr);
    return 0;
}
