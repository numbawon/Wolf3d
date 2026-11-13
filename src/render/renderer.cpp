// src/render/renderer.cpp — Vulkan device/swapchain/compute pipelines (M3).
#include "renderer.hpp"

#include <vulkan/vulkan_win32.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <utility>

#include "vk_common.hpp"

namespace w3d::render {

VkInstance Renderer::create_instance() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "wolf3d";
    app.apiVersion = VK_MAKE_API_VERSION(0, 1, 4, 0);
    const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                          VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = exts;
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, nullptr, &inst) != VK_SUCCESS)
        vk_fail("vkCreateInstance", VK_ERROR_INITIALIZATION_FAILED);
    return inst;
}

namespace {

void pick_physical(VkInstance inst, VkSurfaceKHR surface,
                   VkPhysicalDevice* out_device, uint32_t* out_family) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst, &n, devs.data());
    for (VkPhysicalDevice d : devs) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qf.data());
        for (uint32_t i = 0; i < qn; ++i) {
            if ((qf[i].queueFlags &
                 (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
                VkBool32 supported = VK_FALSE;
                if (vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface,
                                                         &supported) ==
                        VK_SUCCESS &&
                    supported) {
                    *out_device = d;
                    *out_family = i;
                    return;
                }
            }
        }
    }
    // Fall back to any compute-capable device (probe already verified one).
    for (VkPhysicalDevice d : devs) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qf.data());
        for (uint32_t i = 0; i < qn; ++i)
            if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                *out_device = d;
                *out_family = i;
                return;
            }
    }
    std::fprintf(stderr, "w3d: no suitable GPU\n");
    std::abort();
}

struct PickFormatResult {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
};

PickFormatResult pick_swapchain_props(VkPhysicalDevice d, VkSurfaceKHR surface) {
    PickFormatResult r;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(d, surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(d, surface, &n, fmts.data());
    if (n > 0) {
        if (fmts[0].format == VK_FORMAT_UNDEFINED) {
            // "any format" — pick a concrete one.
            r.format = VK_FORMAT_B8G8R8A8_UNORM;
            r.color = fmts[0].colorSpace;
        } else {
            r.format = fmts[0].format;
            r.color = fmts[0].colorSpace;
        }
    }
    n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(d, surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(d, surface, &n, modes.data());
    for (auto m : modes)
        if (m == VK_PRESENT_MODE_MAILBOX_KHR)
            r.mode = m;
    return r;
}

VkSurfaceCapabilitiesKHR pick_caps(VkPhysicalDevice d, VkSurfaceKHR surface) {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d, surface, &caps);
    if (caps.currentExtent.width == 0) {
        // Variable extent: the caller sizes the swapchain from the window.
        caps.currentExtent = {1, 1};
    }
    return caps;
}

// Per-frame sub-region offsets inside the dynamic buffer.
constexpr uint64_t kColumnsOff = 0;
constexpr uint64_t kSpritesOff = 16384;
constexpr uint64_t kPaletteOff = 32768;
constexpr uint64_t kCountOff = 33792;
// UBO offsets must be a multiple of minUniformBufferOffsetAlignment. This
// ARM64 driver requires 256 and silently clamps unaligned offsets DOWN:
// 33796 -> 33792 (fog read the count slot), proven by in-shader probes.
// Fog params: this driver clamps unaligned UBO descriptor offsets down
// (256-byte alignment required) and misplaces `bool` UBO members (both
// proven by value probes this phase), so the fog never uses a UBO. The
// whole fog input set is computed here on the CPU and rides in the
// trailing region of the columns SSBO (walls pass) / sprites SSBO
// (sprite pass) — plain storage reads, cross-checkable against the CPU
// model:
//   @10240 fog_invstep (double), @10248 fog_on (double),
//   @10256 rowdist[320] (double; first 200 = floor/ceiling plane distance
//           per screen row, 80/|row-100| — M5.1 fix replacing the per-column
//           slope t/h, which fogged the nearest floor the hardest),
//   @12816 wlo_arr[320] (int), @14096 whi_arr[320] (int) — ends at
//   15376 < 16384. (rowdist keeps the old 320x8B slot so every downstream
//   offset is unchanged.)
constexpr uint64_t kWallsFogOff = kColumnsOff + 320u * 32;      // 10240
constexpr uint64_t kRowDistOff = kWallsFogOff + 16;             // 10256
constexpr uint64_t kWloOff = kRowDistOff + 320u * 8;            // 12816
constexpr uint64_t kWhiOff = kWloOff + 320u * 4;                // 14096
constexpr uint64_t kSpritesFogOff = kSpritesOff + 256u * 40;    // 26624
constexpr uint64_t kFrameBufSize = 34064;

// CPU-side record layouts (must match the GLSL struct layouts exactly).
struct ColumnUpload {
    double t, h, tx;
    std::uint32_t tex;
    std::uint32_t pad;
};
struct SpriteUpload {
    double x0, y0, s, dist;
    std::uint32_t scale;
    std::uint32_t sprite;
};
// GLSL trailing fog doubles (`double fog_invstep; double fog_on;` appended
// to the columns / sprites SSBO blocks). Plain doubles — UBOs are off the
// table for fog on this driver (see the offset/bool notes above); the
// on-flag is 1.0/0.0 tested with `< 0.5`. invstep is 1/kFogStep so the
// shader only multiplies.
struct FogUpload {
    double invstep;
    double on;
};
static_assert(sizeof(ColumnUpload) == 32);
static_assert(offsetof(ColumnUpload, tex) == 24);
static_assert(sizeof(SpriteUpload) == 40);
static_assert(offsetof(SpriteUpload, scale) == 32);
static_assert(sizeof(FogUpload) == 16);
static_assert(offsetof(FogUpload, on) == 8);

} // namespace

uint32_t Renderer::alloc_mem(const VkMemoryRequirements& mr, uint32_t want_bits,
                             VkDeviceMemory& out) {
    VkPhysicalDeviceMemoryProperties mprops{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &mprops);
    uint32_t type = 0;
    for (; type < mprops.memoryTypeCount; ++type)
        if (mr.memoryTypeBits & (1u << type))
            if (mprops.memoryTypes[type].propertyFlags & want_bits)
                break;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(device_, &ai, nullptr, &out) != VK_SUCCESS)
        vk_fail("vkAllocateMemory", VK_ERROR_OUT_OF_HOST_MEMORY);
    return type;
}

Renderer::Renderer(Window& window, VkInstance instance)
    : window_(window), instance_(instance) {
    surface_ = window.surface();

    // ---- Device
    pick_physical(instance_, surface_, &physical_, &queue_family_);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pQueueCreateInfos = &qci;
    dci.queueCreateInfoCount = 1;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dexts;
    if (vkCreateDevice(physical_, &dci, nullptr, &device_) != VK_SUCCESS)
        vk_fail("vkCreateDevice", VK_ERROR_INITIALIZATION_FAILED);
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    VkPhysicalDeviceProperties pprops{};
    vkGetPhysicalDeviceProperties(physical_, &pprops);
    std::fprintf(stderr,
                 "GPU: %s  sharedMemMax=%llu invocationsMax=%llu wgMax=(%llu,%llu,%llu) wgSizeMax=(%llu,%llu,%llu)\n",
                 pprops.deviceName,
                 (unsigned long long)pprops.limits.maxComputeSharedMemorySize,
                 (unsigned long long)pprops.limits.maxComputeWorkGroupInvocations,
                 (unsigned long long)pprops.limits.maxComputeWorkGroupCount[0],
                 (unsigned long long)pprops.limits.maxComputeWorkGroupCount[1],
                 (unsigned long long)pprops.limits.maxComputeWorkGroupCount[2],
                 (unsigned long long)pprops.limits.maxComputeWorkGroupSize[0],
                 (unsigned long long)pprops.limits.maxComputeWorkGroupSize[1],
                 (unsigned long long)pprops.limits.maxComputeWorkGroupSize[2]);
    buf_align_ = std::max<uint64_t>(
        64, std::max(pprops.limits.minStorageBufferOffsetAlignment,
                     pprops.limits.minUniformBufferOffsetAlignment));

    // ---- Internal image (kInternalW x kInternalH, RGBA8, storage + blit src)
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {kInternalW, kInternalH, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ii, nullptr, &img_) != VK_SUCCESS)
        vk_fail("vkCreateImage(internal)", VK_ERROR_INITIALIZATION_FAILED);
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(device_, img_, &mr);
    alloc_mem(mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img_mem_);
    vkBindImageMemory(device_, img_, img_mem_, 0);
    VkImageViewCreateInfo ivi{};
    ivi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivi.image = img_;
    ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &ivi, nullptr, &img_view_) != VK_SUCCESS)
        vk_fail("vkCreateImageView(internal)", VK_ERROR_INITIALIZATION_FAILED);

    // ---- Readback staging buffer (host visible)
    const VkDeviceSize kBytes =
        static_cast<VkDeviceSize>(kInternalW) * kInternalH * 4;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = kBytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bi, nullptr, &staging_) != VK_SUCCESS)
        vk_fail("vkCreateBuffer(staging)", VK_ERROR_INITIALIZATION_FAILED);
    vkGetBufferMemoryRequirements(device_, staging_, &mr);
    alloc_mem(mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, staging_mem_);
    vkBindBufferMemory(device_, staging_, staging_mem_, 0);

    // TEMP DEBUG: host visible mirror of the frame buffer, filled by a GPU
    // copy, so we can see what the GPU actually reads.
    bi.size = kFrameBufSize;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(device_, &bi, nullptr, &dbg_buf_) != VK_SUCCESS)
        vk_fail("vkCreateBuffer(dbg)", VK_ERROR_INITIALIZATION_FAILED);
    vkGetBufferMemoryRequirements(device_, dbg_buf_, &mr);
    alloc_mem(mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, dbg_mem_);
    vkBindBufferMemory(device_, dbg_buf_, dbg_mem_, 0);

    // ---- Per-frame dynamic buffer (host visible, permanently mapped)
    bi = {};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = kFrameBufSize;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(device_, &bi, nullptr, &frame_buf_) != VK_SUCCESS)
        vk_fail("vkCreateBuffer(frame)", VK_ERROR_INITIALIZATION_FAILED);
    vkGetBufferMemoryRequirements(device_, frame_buf_, &mr);
    alloc_mem(mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, frame_mem_);
    vkBindBufferMemory(device_, frame_buf_, frame_mem_, 0);
    if (vkMapMemory(device_, frame_mem_, 0, VK_WHOLE_SIZE, 0, &frame_map_) !=
        VK_SUCCESS)
        vk_fail("vkMapMemory(frame)", VK_ERROR_MEMORY_MAP_FAILED);

    // ---- Pipelines (shared descriptor layout)
    VkDescriptorSetLayoutBinding binds[7]{};
    for (uint32_t i = 0; i < 7; ++i) {
        binds[i].binding = i;
        binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        binds[i].descriptorCount = 1;
        binds[i].descriptorType =
            (i == 6) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                     : (i == 5) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 7;
    dslci.pBindings = binds;
    if (vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &dsl_) !=
        VK_SUCCESS)
        vk_fail("vkCreateDescriptorSetLayout", VK_ERROR_INITIALIZATION_FAILED);
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl_;
    if (vkCreatePipelineLayout(device_, &plci, nullptr, &layout_) !=
        VK_SUCCESS)
        vk_fail("vkCreatePipelineLayout", VK_ERROR_INITIALIZATION_FAILED);

    auto make_pipe = [&](const char* spv_name, VkPipeline* out) {
        const auto spv = load_file(exe_dir() / "shaders" / spv_name);
        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = spv.size();
        smci.pCode = reinterpret_cast<const uint32_t*>(spv.data());
        VkShaderModule mod = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_, &smci, nullptr, &mod) !=
            VK_SUCCESS)
            vk_fail("vkCreateShaderModule", VK_ERROR_INITIALIZATION_FAILED);
        VkComputePipelineCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = mod;
        stage.pName = "main";
        cpci.stage = stage;
        cpci.layout = layout_;
        if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpci, nullptr,
                                     out) != VK_SUCCESS)
            vk_fail("vkCreateComputePipelines", VK_ERROR_INITIALIZATION_FAILED);
        return mod;
    };
    walls_mod_ = make_pipe("walls.spv", &walls_pipe_);
    sprites_mod_ = make_pipe("sprites.spv", &sprites_pipe_);

    // ---- Descriptor set (buffer entries filled by load_atlases)
    VkDescriptorPoolSize dps[7]{};
    for (uint32_t i = 0; i < 7; ++i)
        dps[i] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    dps[5] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    dps[6] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 7;
    dpci.pPoolSizes = dps;
    if (vkCreateDescriptorPool(device_, &dpci, nullptr, &dpool_) !=
        VK_SUCCESS)
        vk_fail("vkCreateDescriptorPool", VK_ERROR_INITIALIZATION_FAILED);
    VkDescriptorSetAllocateInfo dsa{};
    dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsa.descriptorPool = dpool_;
    dsa.descriptorSetCount = 1;
    dsa.pSetLayouts = &dsl_;
    if (vkAllocateDescriptorSets(device_, &dsa, &dset_) != VK_SUCCESS)
        vk_fail("vkAllocateDescriptorSets", VK_ERROR_OUT_OF_HOST_MEMORY);
    {
        // Image binding + the per-frame buffer bindings (atlas buffers get
        // their ranges in load_atlases).
        VkDescriptorImageInfo dii{};
        dii.imageView = img_view_;
        dii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorBufferInfo binfo[7]{};
        binfo[0].buffer = frame_buf_;  // columns
        binfo[0].offset = kColumnsOff;
        binfo[0].range = kSpritesOff - kColumnsOff;
        binfo[1].buffer = VK_NULL_HANDLE;  // wall atlas (filled later)
        binfo[2].buffer = VK_NULL_HANDLE;  // sprite atlas (filled later)
        binfo[3].buffer = frame_buf_;      // palette
        binfo[3].offset = kPaletteOff;
        binfo[3].range = 1024;
        binfo[4].buffer = frame_buf_;      // sprite records
        binfo[4].offset = kSpritesOff;
        binfo[4].range = 256u * 40 + 16;   // + trailing fog doubles
        binfo[5].buffer = frame_buf_;      // count
        binfo[5].offset = kCountOff;
        binfo[5].range = 4;
        VkWriteDescriptorSet wds[7]{};
        for (uint32_t i = 0; i < 7; ++i) {
            wds[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wds[i].dstSet = dset_;
            wds[i].dstBinding = i;
            wds[i].descriptorCount = 1;
            wds[i].descriptorType = (i == 5)
                ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                : (i == 6) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                           : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            wds[i].pBufferInfo = &binfo[i];
        }
        wds[6].pBufferInfo = nullptr;  // image binding below
        wds[6].pImageInfo = &dii;
        vkUpdateDescriptorSets(device_, 7, wds, 0, nullptr);
    }

    // ---- Synchronization + command pool
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (vkCreateSemaphore(device_, &sci, nullptr, &present_sem_) != VK_SUCCESS)
        vk_fail("vkCreateSemaphore", VK_ERROR_INITIALIZATION_FAILED);
    if (vkCreateSemaphore(device_, &sci, nullptr, &acquire_sem_) != VK_SUCCESS)
        vk_fail("vkCreateSemaphore", VK_ERROR_INITIALIZATION_FAILED);
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT; // wait-able from the start
    if (vkCreateFence(device_, &fci, nullptr, &fence_) != VK_SUCCESS)
        vk_fail("vkCreateFence", VK_ERROR_INITIALIZATION_FAILED);
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family_;
    if (vkCreateCommandPool(device_, &pci, nullptr, &pool_) != VK_SUCCESS)
        vk_fail("vkCreateCommandPool", VK_ERROR_INITIALIZATION_FAILED);
    VkCommandBufferAllocateInfo abi{};
    abi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    abi.commandPool = pool_;
    abi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    abi.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &abi, &cmd_) != VK_SUCCESS)
        vk_fail("vkAllocateCommandBuffers", VK_ERROR_OUT_OF_HOST_MEMORY);

    // ---- Swapchain
    {
        uint32_t w = 0, h = 0;
        window_.size(w, h);
        create_swapchain(w, h);
    }
}

Renderer::~Renderer() {
    if (!device_)
        return;
    vkDeviceWaitIdle(device_);
    destroy_swapchain();
    if (frame_map_)
        vkUnmapMemory(device_, frame_mem_);
    if (frame_mem_)
        vkFreeMemory(device_, frame_mem_, nullptr);
    if (frame_buf_)
        vkDestroyBuffer(device_, frame_buf_, nullptr);
    if (sprite_atlas_mem_)
        vkFreeMemory(device_, sprite_atlas_mem_, nullptr);
    if (sprite_atlas_)
        vkDestroyBuffer(device_, sprite_atlas_, nullptr);
    if (wall_atlas_mem_)
        vkFreeMemory(device_, wall_atlas_mem_, nullptr);
    if (wall_atlas_)
        vkDestroyBuffer(device_, wall_atlas_, nullptr);
    vkDestroyPipeline(device_, walls_pipe_, nullptr);
    vkDestroyPipeline(device_, sprites_pipe_, nullptr);
    vkDestroyPipelineLayout(device_, layout_, nullptr);
    vkDestroyDescriptorPool(device_, dpool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, dsl_, nullptr);
    vkDestroyShaderModule(device_, walls_mod_, nullptr);
    vkDestroyShaderModule(device_, sprites_mod_, nullptr);
    vkDestroyImageView(device_, img_view_, nullptr);
    vkDestroyImage(device_, img_, nullptr);
    vkFreeMemory(device_, img_mem_, nullptr);
    vkFreeMemory(device_, staging_mem_, nullptr);
    vkDestroyBuffer(device_, staging_, nullptr);
    vkDestroySemaphore(device_, present_sem_, nullptr);
    vkDestroySemaphore(device_, acquire_sem_, nullptr);
    vkDestroyFence(device_, fence_, nullptr);
    vkFreeCommandBuffers(device_, pool_, 1, &cmd_);
    vkDestroyCommandPool(device_, pool_, nullptr);
    vkDestroyDevice(device_, nullptr);
    // Instance and surface are owned by the caller.
}

void Renderer::create_swapchain(uint32_t w, uint32_t h) {
    const auto props = pick_swapchain_props(physical_, surface_);
    const auto caps = pick_caps(physical_, surface_);
    uint32_t sw = w, sh = h;
    if (caps.minImageExtent.width != 0) {
        sw = std::max(caps.minImageExtent.width,
                      std::min(sw, caps.maxImageExtent.width));
        sh = std::max(caps.minImageExtent.height,
                      std::min(sh, caps.maxImageExtent.height));
    }
    swap_w_ = sw;
    swap_h_ = sh;
    swap_fmt_ = props.format;
    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = surface_;
    sci.minImageCount = caps.minImageCount + 1;
    sci.imageFormat = props.format;
    sci.imageColorSpace = props.color;
    sci.imageExtent = {sw, sh};
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = props.mode;
    sci.clipped = VK_TRUE;
    VkSwapchainKHR old = swap_;
    if (vkCreateSwapchainKHR(device_, &sci, nullptr, &swap_) != VK_SUCCESS)
        vk_fail("vkCreateSwapchainKHR", VK_ERROR_INITIALIZATION_FAILED);
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(device_, swap_, &n, nullptr);
    swap_images_.resize(n);
    vkGetSwapchainImagesKHR(device_, swap_, &n, swap_images_.data());
    swap_views_.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = swap_images_[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = props.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &vi, nullptr, &swap_views_[i]) !=
            VK_SUCCESS)
            vk_fail("vkCreateImageView(swap)", VK_ERROR_INITIALIZATION_FAILED);
    }
    if (old != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(device_, old, nullptr);
}

void Renderer::destroy_swapchain() {
    for (auto v : swap_views_)
        vkDestroyImageView(device_, v, nullptr);
    swap_views_.clear();
    swap_images_.clear();
    if (swap_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swap_, nullptr);
        swap_ = VK_NULL_HANDLE;
    }
}

void Renderer::load_atlases(
    const std::vector<const std::array<std::uint8_t, 4096>*>& walls,
    const std::vector<const std::array<std::uint8_t, 4096>*>& sprites) {
    // The GPU may still be reading the old atlases; wait before rebuilding.
    vkDeviceWaitIdle(device_);

    // entries is a pointer to the vector's data: an array of pointers, one
    // per tile, each pointing at that tile's 4096-byte index image. We must
    // dereference entries[t] (the real pointer) rather than do pointer
    // arithmetic (entries + t): the wall tiles are contiguous 4096-byte
    // blocks, but sprite tiles live in 4100-byte Sprite structs (4096 idx +
    // two uint16), so a 4096-stride walk lands 4t bytes early for tile t.
    auto make_atlas = [&](const auto* entries, uint32_t count,
                          VkBuffer* out_buf, VkDeviceMemory* out_mem,
                          uint32_t* out_texels) {
        if (*out_buf) {
            if (*out_mem)
                vkFreeMemory(device_, *out_mem, nullptr);
            vkDestroyBuffer(device_, *out_buf, nullptr);
        }
        *out_buf = VK_NULL_HANDLE;
        *out_texels = count * 4096;
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = (*out_texels ? static_cast<VkDeviceSize>(*out_texels) * 4
                               : 4);
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (vkCreateBuffer(device_, &bi, nullptr, out_buf) != VK_SUCCESS)
            vk_fail("vkCreateBuffer(atlas)", VK_ERROR_INITIALIZATION_FAILED);
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(device_, *out_buf, &mr);
        alloc_mem(mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, *out_mem);
        vkBindBufferMemory(device_, *out_buf, *out_mem, 0);
        void* map = nullptr;
        if (vkMapMemory(device_, *out_mem, 0, VK_WHOLE_SIZE, 0, &map) !=
            VK_SUCCESS)
            vk_fail("vkMapMemory(atlas)", VK_ERROR_MEMORY_MAP_FAILED);
        auto* dst = static_cast<std::uint32_t*>(map);
        for (uint32_t t = 0; t < count; ++t)
            for (uint32_t i = 0; i < 4096; ++i)
                dst[static_cast<std::size_t>(t) * 4096 + i] =
                    static_cast<std::uint32_t>((*entries[t])[i]);
        VkMappedMemoryRange r{};
        r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        r.memory = *out_mem;
        r.offset = 0;
        r.size = VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(device_, 1, &r);
        vkUnmapMemory(device_, *out_mem);
    };
    make_atlas(walls.empty() ? nullptr : walls.data(),
               static_cast<uint32_t>(walls.size()), &wall_atlas_,
               &wall_atlas_mem_, &wall_atlas_texels_);
    make_atlas(sprites.empty() ? nullptr : sprites.data(),
               static_cast<uint32_t>(sprites.size()), &sprite_atlas_,
               &sprite_atlas_mem_, &sprite_atlas_texels_);

    VkDescriptorBufferInfo binfo[2]{};
    binfo[0].buffer = wall_atlas_;
    binfo[0].offset = 0;
    binfo[0].range = static_cast<VkDeviceSize>(wall_atlas_texels_) * 4;
    binfo[1].buffer = sprite_atlas_;
    binfo[1].offset = 0;
    binfo[1].range = static_cast<VkDeviceSize>(sprite_atlas_texels_) * 4;
    VkWriteDescriptorSet wds[2]{};
    for (int i = 0; i < 2; ++i) {
        wds[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wds[i].dstSet = dset_;
        wds[i].dstBinding = 1 + i;
        wds[i].descriptorCount = 1;
        wds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        wds[i].pBufferInfo = &binfo[i];
    }
    vkUpdateDescriptorSets(device_, 2, wds, 0, nullptr);
}

void Renderer::render_frame(const Frame& f,
                            const std::array<std::uint32_t, 256>& palette,
                            bool readback, bool fog_on) {
    // ---- Upload the per-frame data (host visible, permanently mapped)
    auto* base = static_cast<uint8_t*>(frame_map_);
    for (uint32_t i = 0; i < kColumns; ++i) {
        ColumnUpload c{f.columns[i].t, f.columns[i].h, f.columns[i].tx,
                       f.columns[i].tex, 0};
        std::memcpy(base + kColumnsOff + static_cast<uint64_t>(i) * 32, &c,
                    sizeof(c));
        // M4 fog inputs (CPU side — see the offset notes): the wall union
        // bounds for this column. Same formulas the shader's DDA uses
        // (top = 100 - h, fbot = floor(top + 2h)).
        const double top = 100.0 - f.columns[i].h;
        const double fbot = std::floor(top + 2.0 * f.columns[i].h);
        const double tf = std::floor(top);
        const int wlo = (tf < 0.0) ? 0 : (tf > 200.0 ? 200 : static_cast<int>(tf));
        const int whi = (fbot > 200.0) ? 200 : (fbot < 0.0 ? 0 : static_cast<int>(fbot));
        const uint64_t ii = static_cast<uint64_t>(i);
        std::memcpy(base + kWloOff + ii * 4, &wlo, 4);
        std::memcpy(base + kWhiOff + ii * 4, &whi, 4);
    }
    // M5.1: floor/ceiling fog distance per screen row (static table). A
    // floor point at distance D projects to row 100 + (kWallHeight/2)/D
    // (eye at mid-wall height, floor 0.5 cells below the eye), i.e.
    // D(row) = (kWallHeight/2) / |row-100| — column independent, nearest
    // floor (D = 0.81) at the screen edge, full fog at the horizon row.
    // (Replaces M4's per-column slope t/h * |row-100|, which fogged the
    // nearest floor rows the hardest — user-visible as a bright wall line
    // with dark banded floor beyond it.)
    {
        static double rowdist[320];
        for (int r = 0; r < 320; ++r) {
            if (r >= kRows) {
                rowdist[r] = 99.0;  // unused tail (keeps the slot padded)
                continue;
            }
            const double ar = std::abs(r - 100.0);
            rowdist[r] = (ar == 0.0) ? 99.0 : (kWallHeight / 2.0) / ar;
        }
        std::memcpy(base + kRowDistOff, rowdist, sizeof(rowdist));
    }
    for (uint32_t i = 0; i < kMaxSprites && i < static_cast<uint32_t>(f.sprite_count);
         ++i) {
        const auto& s = f.sprites[i];
        SpriteUpload up{s.x0, s.y0, s.s, s.dist, s.scale, s.sprite};
        std::memcpy(base + kSpritesOff + static_cast<uint64_t>(i) * 40, &up,
                    sizeof(up));
    }
    std::memcpy(base + kPaletteOff, palette.data(), 256 * 4);
    std::uint32_t count = static_cast<std::uint32_t>(f.sprite_count);
    std::memcpy(base + kCountOff, &count, 4);
    // Fog params (invstep + on-flag) as two doubles, written to both the
    // columns-SSBO tail (walls pass) and the sprites-SSBO tail (sprite
    // pass) — the UBO slot is gone (see kWallsFogOff note).
    FogUpload fog{1.0 / kFogStep, fog_on ? 1.0 : 0.0};
    std::memcpy(base + kWallsFogOff, &fog, sizeof(fog));
    std::memcpy(base + kSpritesFogOff, &fog, sizeof(fog));
    VkMappedMemoryRange r{};
    r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    r.memory = frame_mem_;
    r.offset = 0;
    r.size = VK_WHOLE_SIZE;
    vkFlushMappedMemoryRanges(device_, 1, &r);

    // ---- Record the command buffer
    VkCommandBufferBeginInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(cmd_, &bci) != VK_SUCCESS)
        vk_fail("vkBeginCommandBuffer", VK_ERROR_INITIALIZATION_FAILED);

    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    bar.srcAccessMask = 0;
    bar.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = img_;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &bar);

    // Wall pass: one workgroup of 320 invocations (one per column).
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, walls_pipe_);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0,
                            1, &dset_, 0, nullptr);
    vkCmdDispatch(cmd_, 1, 1, 1);

    // Barrier between the two compute passes (both write the image).
    bar = {};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bar.image = img_;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &bar);

    // Sprite pass: 8x8 workgroups over the 320x200 image.
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, sprites_pipe_);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0,
                            1, &dset_, 0, nullptr);
    vkCmdDispatch(cmd_, (kInternalW + 7) / 8, (kInternalH + 7) / 8, 1);

    // TEMP DEBUG: GPU -> host mirror of the frame buffer.
    {
        VkBufferCopy bc{};
        bc.srcOffset = 0;
        bc.dstOffset = 0;
        bc.size = kFrameBufSize;
        vkCmdCopyBuffer(cmd_, frame_buf_, dbg_buf_, 1, &bc);
    }

    bar = {};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bar.image = img_;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &bar);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {static_cast<int32_t>(kInternalW),
                          static_cast<int32_t>(kInternalH), 1};
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {static_cast<int32_t>(swap_w_),
                          static_cast<int32_t>(swap_h_), 1};
    vkCmdBlitImage(cmd_, img_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   swap_images_[acquired_], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 1,
                   &blit, VK_FILTER_LINEAR);

    if (readback) {
        VkBufferImageCopy bic{};
        bic.bufferOffset = 0;
        bic.bufferRowLength = 0;
        bic.bufferImageHeight = 0;
        bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bic.imageOffset = {0, 0, 0};
        bic.imageExtent = {kInternalW, kInternalH, 1};
        vkCmdCopyImageToBuffer(cmd_, img_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging_, 1, &bic);
    }
    if (vkEndCommandBuffer(cmd_) != VK_SUCCESS)
        vk_fail("vkEndCommandBuffer", VK_ERROR_INITIALIZATION_FAILED);
    has_work_ = true;
}

bool Renderer::begin_frame() {
    // Wait for the previous frame to finish before reusing fence/semaphores.
    if (has_work_) {
        const VkResult wr =
            vkWaitForFences(device_, 1, &fence_, VK_TRUE, 1000000000ull);
        if (wr != VK_SUCCESS)
            return false;
        vkResetFences(device_, 1, &fence_);
    }
    // Recreate the swapchain if the window was resized.
    uint32_t w = 0, h = 0;
    window_.size(w, h);
    if (w != swap_w_ || h != swap_h_) {
        destroy_swapchain();
        create_swapchain(w, h);
    }
    VkResult r = vkAcquireNextImageKHR(device_, swap_, 1000000000ull,
                                       acquire_sem_, VK_NULL_HANDLE, &acquired_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        destroy_swapchain();
        create_swapchain(w, h);
        r = vkAcquireNextImageKHR(device_, swap_, 1000000000ull,
                                  acquire_sem_, VK_NULL_HANDLE, &acquired_);
    }
    if (r != VK_SUCCESS)
        return false;
    has_work_ = false;
    return true;
}

void Renderer::present() {
    if (!has_work_)
        return;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquire_sem_;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &present_sem_;
    if (vkQueueSubmit(queue_, 1, &si, fence_) != VK_SUCCESS)
        vk_fail("vkQueueSubmit", VK_ERROR_DEVICE_LOST);
    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &present_sem_;
    uint32_t idx = acquired_;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swap_;
    pi.pImageIndices = &idx;
    vkQueuePresentKHR(queue_, &pi);
}

bool Renderer::readback(std::vector<uint8_t>& out) {
    const VkResult wr = vkWaitForFences(device_, 1, &fence_, VK_TRUE,
                                        1000000000ull);
    if (wr != VK_SUCCESS)
        return false;
    void* data = nullptr;
    if (vkMapMemory(device_, staging_mem_, 0, VK_WHOLE_SIZE, 0, &data) !=
        VK_SUCCESS)
        return false;
    const size_t bytes = static_cast<size_t>(kInternalW) * kInternalH * 4;
    out.resize(bytes);
    std::memcpy(out.data(), data, bytes);
    vkUnmapMemory(device_, staging_mem_);

    // TEMP DEBUG: dump the GPU-side mirror of the frame buffer.
    if (dbg_mem_) {
        void* d2 = nullptr;
        if (vkMapMemory(device_, dbg_mem_, 0, VK_WHOLE_SIZE, 0, &d2) ==
            VK_SUCCESS) {
            std::ofstream f("C:/Users/NumbaWon/AppData/Local/hermes/cache/"
                            "scratch/dbg_framebuf.bin", std::ios::binary);
            if (f)
                f.write(static_cast<const char*>(d2),
                        static_cast<std::streamsize>(kFrameBufSize));
            vkUnmapMemory(device_, dbg_mem_);
        }
    }
    return true;
}

} // namespace w3d::render
