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

#include "app/font8x16.inc"  // kFont8x16[95][16], the menu font bitmap
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

// Per-frame sub-region offsets inside the dynamic buffer (WS3): the layout
// is scale-INDEPENDENT — the scale-dependent regions (columns, walls fog
// tail) are reserved at the S=4 capacity (kMaxScale in frame.hpp) and every
// DESCRIPTOR offset is 256-byte aligned. The ARM64 driver clamps unaligned
// UBO descriptor offsets DOWN (33796 read the 33792 slot, proven in M4), so
// the padding is load-bearing; do not "compact" this table.
//
//   columns:      1280 x 32B @0          (t@0, h@8, tx@16, tex@24)
//   walls fog:    invstep @40960, on @40968      (16B)
//   rowdist:      800 x 8B @40976        (floor/ceiling plane distance per
//                 internal row; horizon row = 99.0 sentinel = full fog)
//   wlo/whi:      1280 x 4B @47376/@52496 (wall-union row bounds per
//                 internal column)
//   The five regions form ONE SSBO (the shader's Columns block); its base
//   is 0 and the block ends at 57616.
constexpr uint64_t kColumnsOff = 0;
constexpr uint64_t kColumnsBlockEnd = 57616;  // descriptor range for b0
constexpr uint64_t kWallsFogOff = 40960;      // in-block: after 1280 x 32B
constexpr uint64_t kRowDistOff = 40976;       // in-block: 8-aligned
constexpr uint64_t kWloOff = 47376;           // in-block
constexpr uint64_t kWhiOff = 52496;           // in-block
constexpr uint64_t kSpritesOff = 57856;       // 256-aligned descriptor base
constexpr uint64_t kSpritesFogOff = kSpritesOff + 256u * 48;  // 70144
constexpr uint64_t kPaletteOff = 70400;       // 256-aligned descriptor base
constexpr uint64_t kCountOff = 71424;         // UBO {spriteCount, scale}, 8B
constexpr uint64_t kScratchOff = 71680;       // WS3 wall scratch: one u32 per
                                              // internal pixel (S=4 capacity:
                                              // 1280 x 800), column-major
constexpr uint64_t kScratchSize = 1280u * 800u * 4u;  // 4096000
constexpr uint64_t kFrameBufSize = kScratchOff + kScratchSize;  // 4167680
static_assert(kColumnsOff % 256 == 0);
static_assert(kSpritesOff % 256 == 0);
static_assert(kPaletteOff % 256 == 0);
static_assert(kCountOff % 256 == 0);
static_assert(kScratchOff % 256 == 0);
static_assert(kColumnsBlockEnd <= kSpritesOff);
static_assert(kSpritesFogOff + 16 <= kPaletteOff);
static_assert(kPaletteOff + 1024 <= kCountOff);
static_assert(kCountOff + 8 <= kScratchOff);
static_assert(kFrameBufSize % 256 == 0);

// CPU-side record layouts (must match the GLSL struct layouts exactly).
struct ColumnUpload {
    double t, h, tx;
    std::uint32_t tex;
    std::uint32_t pad;
};
struct SpriteUpload {
    double x0, y0, s, dist, th;
    std::uint32_t scale;
    std::uint32_t sprite;
};
// The walls-pass fog params ride as two trailing doubles in the columns
// SSBO (same UBO-corruption workaround as before). The scale is NOT here —
// it rides in the count UBO ({spriteCount, scale}) so BOTH passes see the
// same value through one descriptor.
struct FogUpload {
    double invstep;
    double on;
};
struct CountUpload {
    std::uint32_t count;
    std::uint32_t scale;
};
static_assert(sizeof(ColumnUpload) == 32);
static_assert(offsetof(ColumnUpload, tex) == 24);
static_assert(sizeof(SpriteUpload) == 48);
static_assert(offsetof(SpriteUpload, scale) == 40);
static_assert(sizeof(FogUpload) == 16);
static_assert(offsetof(FogUpload, on) == 8);
static_assert(sizeof(CountUpload) == 8);

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

    // ---- Internal image (320*scale x 200*scale, RGBA8, storage + blit src)
    // and readback staging. create_internal() owns both (WS3: rebuilt on
    // res_scale changes); the descriptor set's image binding below points
    // at the current view.
    create_internal(kInternalW, kInternalH);

    // ---- Per-frame dynamic buffer (host visible, permanently mapped)
    VkBufferCreateInfo bi{};
    VkMemoryRequirements mr{};
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
    VkDescriptorSetLayoutBinding binds[8]{};
    for (uint32_t i = 0; i < 8; ++i) {
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
    dslci.bindingCount = 8;
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

    auto make_pipe = [&](const char* spv_name, VkPipeline* out,
                         VkPipelineLayout pipe_layout) {
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
        cpci.layout = pipe_layout;
        if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpci, nullptr,
                                     out) != VK_SUCCESS)
            vk_fail("vkCreateComputePipelines", VK_ERROR_INITIALIZATION_FAILED);
        return mod;
    };
    walls1_mod_ = make_pipe("walls1.spv", &walls1_pipe_, layout_);
    walls2_mod_ = make_pipe("walls2.spv", &walls2_pipe_, layout_);
    sprites_mod_ = make_pipe("sprites.spv", &sprites_pipe_, layout_);

    // ---- Descriptor set (buffer entries filled by load_atlases)
    VkDescriptorPoolSize dps[8]{};
    for (uint32_t i = 0; i < 8; ++i)
        dps[i] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    dps[5] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    dps[6] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 8;
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
        VkDescriptorBufferInfo binfo[8]{};
        binfo[0].buffer = frame_buf_;  // columns + walls fog tail
        binfo[0].offset = kColumnsOff;
        binfo[0].range = kColumnsBlockEnd;
        binfo[1].buffer = VK_NULL_HANDLE;  // wall atlas (filled later)
        binfo[2].buffer = VK_NULL_HANDLE;  // sprite atlas (filled later)
        binfo[3].buffer = frame_buf_;      // palette
        binfo[3].offset = kPaletteOff;
        binfo[3].range = 1024;
        binfo[4].buffer = frame_buf_;      // sprite records
        binfo[4].offset = kSpritesOff;
        binfo[4].range = 256u * 48 + 16;   // + trailing fog doubles
        binfo[5].buffer = frame_buf_;      // count UBO {spriteCount, scale}
        binfo[5].offset = kCountOff;
        binfo[5].range = 8;
        binfo[6].buffer = VK_NULL_HANDLE;  // (image binding, below)
        binfo[7].buffer = frame_buf_;      // wall scratch (WS3)
        binfo[7].offset = kScratchOff;
        binfo[7].range = kScratchSize;
        VkWriteDescriptorSet wds[8]{};
        for (uint32_t i = 0; i < 8; ++i) {
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
        vkUpdateDescriptorSets(device_, 8, wds, 0, nullptr);
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

    // ---- Letterbox black source (WS3): 1x1, filled once at startup ----
    {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {1, 1, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device_, &ii, nullptr, &black_img_) != VK_SUCCESS)
            vk_fail("vkCreateImage(black)", VK_ERROR_INITIALIZATION_FAILED);
        VkMemoryRequirements imr{};
        vkGetImageMemoryRequirements(device_, black_img_, &imr);
        alloc_mem(imr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, black_mem_);
        vkBindImageMemory(device_, black_img_, black_mem_, 0);
        // Fill it with a one-shot command buffer. Must NOT reuse cmd_
        // (the per-frame buffer): submitting cmd_ here leaves it in the
        // executed state and this driver then mis-records the first game
        // frame from it (all-black internal image, observed 2026-11-16).
        VkCommandBuffer temp = VK_NULL_HANDLE;
        {
            VkCommandBufferAllocateInfo ta{};
            ta.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            ta.commandPool = pool_;
            ta.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ta.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(device_, &ta, &temp) != VK_SUCCESS)
                vk_fail("vkAllocateCommandBuffers(black)",
                        VK_ERROR_OUT_OF_HOST_MEMORY);
        }
        {
            VkCommandBufferBeginInfo bgi{};
            bgi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            if (vkBeginCommandBuffer(temp, &bgi) != VK_SUCCESS)
                vk_fail("vkBeginCommandBuffer(black)",
                        VK_ERROR_INITIALIZATION_FAILED);
            VkImageMemoryBarrier bar{};
            bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            bar.srcAccessMask = 0;
            bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bar.image = black_img_;
            bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vkCmdPipelineBarrier(temp, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &bar);
            VkClearColorValue cv{};
            cv.float32[0] = 0.0f;
            cv.float32[1] = 0.0f;
            cv.float32[2] = 0.0f;
            cv.float32[3] = 1.0f;
            vkCmdClearColorImage(
                temp, black_img_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, &cv,
                1, &bar.subresourceRange);
            vkEndCommandBuffer(temp);
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &temp;
            if (vkQueueSubmit(queue_, 1, &si, fence_) != VK_SUCCESS)
                vk_fail("vkQueueSubmit(black)", VK_ERROR_INITIALIZATION_FAILED);
            if (vkWaitForFences(device_, 1, &fence_, VK_TRUE, 1000000000ULL) !=
                VK_SUCCESS)
                vk_fail("vkWaitForFences(black)", VK_ERROR_DEVICE_LOST);
            vkResetFences(device_, 1, &fence_);
            vkFreeCommandBuffers(device_, pool_, 1, &temp);
        }
    }

    // ---- Full-res menu pass (WS2): pipeline, buffers, descriptors ----
    VkDescriptorSetLayoutBinding mb[3]{};
    mb[0].binding = 0;
    mb[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    mb[0].descriptorCount = 1;
    mb[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;  // MenuDraw
    mb[1].binding = 1;
    mb[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    mb[1].descriptorCount = 1;
    mb[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;  // font
    mb[2].binding = 2;
    mb[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    mb[2].descriptorCount = 1;
    mb[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    VkDescriptorSetLayoutCreateInfo mdsl{};
    mdsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    mdsl.bindingCount = 3;
    mdsl.pBindings = mb;
    if (vkCreateDescriptorSetLayout(device_, &mdsl, nullptr, &menu_dsl_) !=
        VK_SUCCESS)
        vk_fail("vkCreateDescriptorSetLayout(menu)",
                VK_ERROR_INITIALIZATION_FAILED);
    VkPipelineLayoutCreateInfo mpl{};
    mpl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    mpl.setLayoutCount = 1;
    mpl.pSetLayouts = &menu_dsl_;
    if (vkCreatePipelineLayout(device_, &mpl, nullptr, &menu_layout_) !=
        VK_SUCCESS)
        vk_fail("vkCreatePipelineLayout(menu)", VK_ERROR_INITIALIZATION_FAILED);
    // The menu pipeline is built against its OWN 3-binding layout: the
    // game's 7-set layout would silently mismatch the bound set (binding
    // 2 = storage image here vs storage buffer there — undefined
    // behaviour, and this driver renders it as an all-zero image).
    menu_mod_ = make_pipe("menu.spv", &menu_pipe_, menu_layout_);
    VkBufferCreateInfo mbi{};
    mbi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    mbi.size = sizeof(app::MenuDraw);
    mbi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (vkCreateBuffer(device_, &mbi, nullptr, &menu_data_) != VK_SUCCESS)
        vk_fail("vkCreateBuffer(menu data)", VK_ERROR_INITIALIZATION_FAILED);
    VkMemoryRequirements mmr{};
    vkGetBufferMemoryRequirements(device_, menu_data_, &mmr);
    alloc_mem(mmr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, menu_data_mem_);
    vkBindBufferMemory(device_, menu_data_, menu_data_mem_, 0);
    if (vkMapMemory(device_, menu_data_mem_, 0, VK_WHOLE_SIZE, 0,
                    &menu_data_map_) != VK_SUCCESS)
        vk_fail("vkMapMemory(menu data)", VK_ERROR_MEMORY_MAP_FAILED);
    mbi.size = 95u * 16;
    if (vkCreateBuffer(device_, &mbi, nullptr, &menu_font_) != VK_SUCCESS)
        vk_fail("vkCreateBuffer(menu font)", VK_ERROR_INITIALIZATION_FAILED);
    vkGetBufferMemoryRequirements(device_, menu_font_, &mmr);
    alloc_mem(mmr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, menu_font_mem_);
    vkBindBufferMemory(device_, menu_font_, menu_font_mem_, 0);
    {
        void* fmap = nullptr;
        if (vkMapMemory(device_, menu_font_mem_, 0, VK_WHOLE_SIZE, 0,
                        &fmap) != VK_SUCCESS)
            vk_fail("vkMapMemory(menu font)", VK_ERROR_MEMORY_MAP_FAILED);
        std::memcpy(fmap, kFont8x16, sizeof(kFont8x16));
        VkMappedMemoryRange mfr{};
        mfr.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        mfr.memory = menu_font_mem_;
        mfr.offset = 0;
        mfr.size = VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(device_, 1, &mfr);
        vkUnmapMemory(device_, menu_font_mem_);
    }
    {
        VkDescriptorPoolSize mps[2]{};
        mps[0] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
        mps[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
        VkDescriptorPoolCreateInfo mdpci{};
        mdpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        mdpci.maxSets = 1;
        mdpci.poolSizeCount = 2;
        mdpci.pPoolSizes = mps;
        if (vkCreateDescriptorPool(device_, &mdpci, nullptr, &menu_dpool_) !=
            VK_SUCCESS)
            vk_fail("vkCreateDescriptorPool(menu)",
                    VK_ERROR_INITIALIZATION_FAILED);
        VkDescriptorSetAllocateInfo mdsa{};
        mdsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        mdsa.descriptorPool = menu_dpool_;
        mdsa.descriptorSetCount = 1;
        mdsa.pSetLayouts = &menu_dsl_;
        if (vkAllocateDescriptorSets(device_, &mdsa, &menu_dset_) !=
            VK_SUCCESS)
            vk_fail("vkAllocateDescriptorSets(menu)",
                    VK_ERROR_OUT_OF_HOST_MEMORY);
        VkDescriptorBufferInfo mbf[2]{};
        mbf[0].buffer = menu_data_;
        mbf[0].offset = 0;
        mbf[0].range = sizeof(app::MenuDraw);
        mbf[1].buffer = menu_font_;
        mbf[1].offset = 0;
        mbf[1].range = 95u * 16;
        VkWriteDescriptorSet mwds[2]{};
        for (int i = 0; i < 2; ++i) {
            mwds[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            mwds[i].dstSet = menu_dset_;
            mwds[i].dstBinding = i;
            mwds[i].descriptorCount = 1;
            mwds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            mwds[i].pBufferInfo = &mbf[i];
        }
        vkUpdateDescriptorSets(device_, 2, mwds, 0, nullptr);
    }
    {
        uint32_t mw = 0, mh = 0;
        window_.size(mw, mh);
        create_menu_target(mw, mh);
    }
}

Renderer::~Renderer() {
    if (!device_)
        return;
    vkDeviceWaitIdle(device_);
    destroy_swapchain();
    if (menu_img_view_)
        vkDestroyImageView(device_, menu_img_view_, nullptr);
    if (menu_img_)
        vkDestroyImage(device_, menu_img_, nullptr);
    if (menu_img_mem_)
        vkFreeMemory(device_, menu_img_mem_, nullptr);
    if (menu_staging_mem_)
        vkFreeMemory(device_, menu_staging_mem_, nullptr);
    if (menu_staging_)
        vkDestroyBuffer(device_, menu_staging_, nullptr);
    if (menu_data_map_)
        vkUnmapMemory(device_, menu_data_mem_);
    if (menu_data_mem_)
        vkFreeMemory(device_, menu_data_mem_, nullptr);
    if (menu_data_)
        vkDestroyBuffer(device_, menu_data_, nullptr);
    if (menu_font_mem_)
        vkFreeMemory(device_, menu_font_mem_, nullptr);
    if (menu_font_)
        vkDestroyBuffer(device_, menu_font_, nullptr);
    if (menu_pipe_)
        vkDestroyPipeline(device_, menu_pipe_, nullptr);
    if (menu_mod_)
        vkDestroyShaderModule(device_, menu_mod_, nullptr);
    if (menu_dpool_)
        vkDestroyDescriptorPool(device_, menu_dpool_, nullptr);
    if (menu_dsl_)
        vkDestroyDescriptorSetLayout(device_, menu_dsl_, nullptr);
    if (menu_layout_)
        vkDestroyPipelineLayout(device_, menu_layout_, nullptr);
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
    vkDestroyPipeline(device_, walls1_pipe_, nullptr);
    vkDestroyPipeline(device_, walls2_pipe_, nullptr);
    vkDestroyPipeline(device_, sprites_pipe_, nullptr);
    vkDestroyPipelineLayout(device_, layout_, nullptr);
    vkDestroyDescriptorPool(device_, dpool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, dsl_, nullptr);
    vkDestroyShaderModule(device_, walls1_mod_, nullptr);
    vkDestroyShaderModule(device_, walls2_mod_, nullptr);
    vkDestroyShaderModule(device_, sprites_mod_, nullptr);
    vkDestroyImageView(device_, img_view_, nullptr);
    vkDestroyImage(device_, img_, nullptr);
    vkFreeMemory(device_, img_mem_, nullptr);
    if (black_img_)
        vkDestroyImage(device_, black_img_, nullptr);
    if (black_mem_)
        vkFreeMemory(device_, black_mem_, nullptr);
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
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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
    // --window-shot staging (WS3): host-visible, swapchain-sized.
    {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = static_cast<VkDeviceSize>(sw) * sh * 4;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(device_, &bi, nullptr, &swap_staging_) !=
            VK_SUCCESS)
            vk_fail("vkCreateBuffer(swap staging)",
                    VK_ERROR_INITIALIZATION_FAILED);
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(device_, swap_staging_, &mr);
        alloc_mem(mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                  swap_staging_mem_);
        vkBindBufferMemory(device_, swap_staging_, swap_staging_mem_, 0);
    }
    if (old != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(device_, old, nullptr);
}

void Renderer::destroy_swapchain() {
    for (auto v : swap_views_)
        vkDestroyImageView(device_, v, nullptr);
    swap_views_.clear();
    swap_images_.clear();
    if (swap_staging_mem_)
        vkFreeMemory(device_, swap_staging_mem_, nullptr);
    if (swap_staging_)
        vkDestroyBuffer(device_, swap_staging_, nullptr);
    swap_staging_ = VK_NULL_HANDLE;
    swap_staging_mem_ = VK_NULL_HANDLE;
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
                            bool readback, bool fog_on, bool window_shot) {
    // WS3: internal frame is (320*S, 200*S); S rides in the count UBO so
    // both compute passes see it.
    const uint32_t S = scale_;
    const uint32_t W = kInternalW * S;
    const uint32_t H = kInternalH * S;
    const double mid = 100.0 * static_cast<double>(S);  // H/2
    // Scale-mismatch guard: the frame's column/sprite records are only
    // valid for f.scale. If the app built them at a different scale, the
    // records beyond 320*f.scale are unwritten defaults (h = 0), which
    // the wall pass draws as flat ceiling/floor — most of the frame goes
    // blank. Warn once; the run stays up and the next launch after the
    // fix is clean. (WS7: a stale CLI scale vs the config's live scale
    // did exactly this.)
    static bool scale_mismatch_warned = false;
    if (f.scale != S && !scale_mismatch_warned) {
        scale_mismatch_warned = true;
        std::fprintf(stderr, "w3d: frame built at scale %u but the renderer "
                             "is at scale %u; the frame will render corrupt "
                             "(scale mismatch)\n",
                     f.scale, S);
    }

    // ---- Upload the per-frame data (host visible, permanently mapped)
    auto* base = static_cast<uint8_t*>(frame_map_);
    for (uint32_t i = 0; i < W; ++i) {
        ColumnUpload c{f.columns[static_cast<std::size_t>(i)].t,
                       f.columns[static_cast<std::size_t>(i)].h,
                       f.columns[static_cast<std::size_t>(i)].tx,
                       f.columns[static_cast<std::size_t>(i)].tex, 0};
        std::memcpy(base + kColumnsOff + static_cast<uint64_t>(i) * 32, &c,
                    sizeof(c));
        // M4 fog inputs (CPU side — see the offset notes): the wall union
        // bounds for this column. The reference writes rows
        // [max(0, ~~(100-h)), yi_final) where yi_final comes from the same
        // 64-step yf accumulation the shader runs (NOT floor(100-h+2h),
        // which is off by one row when 100-h is near an integer). At scale
        // S the mid row and the row bounds scale by S; the per-column h is
        // already in internal (scaled) pixels.
        const double top = mid - f.columns[static_cast<std::size_t>(i)].h;
        const double ytrunc = (top >= 0.0) ? std::floor(top) : std::ceil(top);
        const double s = f.columns[static_cast<std::size_t>(i)].h / 32.0;
        const double stepi = std::floor(s);
        double yf = top - ytrunc;
        int yi = static_cast<int>(ytrunc);
        for (int j = 0; j < 64; ++j) {
            yf += s - stepi;
            int n = static_cast<int>(stepi);
            if (yf >= 1.0) {
                ++n;
                yf -= 1.0;
            }
            yi += n;
        }
        const int wlo =
            (ytrunc < 0.0) ? 0 : (ytrunc > static_cast<double>(H) ? static_cast<int>(H)
                                                             : static_cast<int>(ytrunc));
        const int whi = (yi > static_cast<int>(H)) ? static_cast<int>(H)
                                                   : (yi < 0 ? 0 : yi);
        const uint64_t ii = static_cast<uint64_t>(i);
        std::memcpy(base + kWloOff + ii * 4, &wlo, 4);
        std::memcpy(base + kWhiOff + ii * 4, &whi, 4);
    }
    // M5.1: floor/ceiling fog distance per screen row (static table). A
    // floor point at distance D projects to row mid + (wallHeight/2)/D
    // (eye at mid-wall height, floor 0.5 cells below the eye), i.e.
    // D(row) = (wallHeight/2) / |row - mid| — column independent, nearest
    // floor at the screen edge, full fog at the horizon row (99.0 sentinel,
    // level 23). (Replaces M4's per-column slope t/h * |row-100|, which
    // fogged the nearest floor rows the hardest — user-visible as a bright
    // wall line with dark banded floor beyond it.)
    {
        static double rowdist[kRows * kMaxScale];
        for (uint32_t r = 0; r < H; ++r) {
            const double ar = std::abs(static_cast<double>(r) - mid);
            rowdist[r] =
                (ar == 0.0) ? 99.0
                            : (kWallHeight * static_cast<double>(S) / 2.0) / ar;
        }
        std::memcpy(base + kRowDistOff, rowdist,
                    static_cast<size_t>(H) * sizeof(double));
    }
    for (uint32_t i = 0; i < kMaxSprites && i < static_cast<uint32_t>(f.sprite_count);
         ++i) {
        const auto& s = f.sprites[i];
        SpriteUpload up{s.x0, s.y0, s.s, s.dist, s.th, s.scale, s.sprite};
        std::memcpy(base + kSpritesOff + static_cast<uint64_t>(i) * 48, &up,
                    sizeof(up));
    }
    std::memcpy(base + kPaletteOff, palette.data(), 256 * 4);
    // Count UBO: {spriteCount, scale} — the scale is the single source of
    // truth both compute passes read (one descriptor, no drift).
    CountUpload cnt{static_cast<std::uint32_t>(f.sprite_count), S};
    std::memcpy(base + kCountOff, &cnt, sizeof(cnt));
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
    // srcAccess includes HOST_WRITE: the per-frame upload (host-visible
    // frame buffer, flushed above) must be visible to the compute passes.
    bar.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = img_;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &bar);

    // Wall pass 1 (WS3): S workgroups of 320 invocations (one per internal
    // column; the shader uses gl_GlobalInvocationID.x). Resolves each
    // column's ceiling/floor/wall rows into the scratch SSBO — no private
    // rows[] array (that would be H uints per invocation, too much local
    // memory at S=4 for 320 concurrent invocations on the N1X).
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, walls1_pipe_);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0,
                            1, &dset_, 0, nullptr);
    vkCmdDispatch(cmd_, S, 1, 1);

    // Wall pass 2: fog + image store. NOT an implicit barrier: two
    // dispatches in one command buffer may run concurrently on the GPU
    // until an explicit memory barrier orders them — without it, walls2
    // read last frame's scratch on moving scenes (stale texels; invisible
    // on static scenes, which masked it).
    VkMemoryBarrier scratch_bar{};
    scratch_bar.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    scratch_bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    scratch_bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &scratch_bar, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, walls2_pipe_);
    vkCmdDispatch(cmd_, S, 1, 1);

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

    // Sprite pass: 8x8 workgroups over the (320*S, 200*S) image.
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, sprites_pipe_);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0,
                            1, &dset_, 0, nullptr);
    vkCmdDispatch(cmd_, (W + 7) / 8, (H + 7) / 8, 1);

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

    // ---- Window presentation (WS3): paint the letterbox bars black,
    // then blit the (320*S, 200*S) frame centered. The frame is always
    // 8:5; the window is freely resizable, so the black bars carry
    // whatever aspect the window has (Decision 2). The bars are a
    // full-window NEAREST blit of a 1x1 black image: this ARM64 driver
    // segfaults inside vkCmdClearAttachments on the swapchain image
    // (bisected with frame-1 checkpoints, 2026-11-16).
    {
        VkImageMemoryBarrier sbar{};
        sbar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        sbar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        sbar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        sbar.srcAccessMask = 0;
        sbar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sbar.image = swap_images_[acquired_];
        sbar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        sbar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sbar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &sbar);
        VkImageBlit fill{};
        fill.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        fill.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        fill.srcOffsets[0] = {0, 0, 0};
        fill.srcOffsets[1] = {1, 1, 1};
        fill.dstOffsets[0] = {0, 0, 0};
        fill.dstOffsets[1] = {static_cast<int32_t>(swap_w_),
                              static_cast<int32_t>(swap_h_), 1};
        vkCmdBlitImage(cmd_, black_img_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       swap_images_[acquired_],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &fill,
                       VK_FILTER_NEAREST);
    }

    // Letterbox rect: the largest 8:5 rectangle fitting the window. The
    // Integer filter snaps it to the largest INTEGER window scale k >= 1
    // (k * (320*S, 200*S)); if the window is smaller than one frame, the
    // filter falls back to the max fit (per the BlitFilter contract).
    uint32_t fw = std::min(swap_w_, (swap_h_ * 8) / 5);
    uint32_t fh = (fw * 5) / 8;
    if (blit_filter_ == BlitFilter::Integer) {
        const uint32_t k = std::min(swap_w_ / W, swap_h_ / H);
        if (k >= 1) {
            fw = W * k;
            fh = H * k;
        }
    }
    const uint32_t ox = (swap_w_ - fw) / 2;
    const uint32_t oy = (swap_h_ - fh) / 2;
    const VkFilter filter =
        (blit_filter_ == BlitFilter::Linear) ? VK_FILTER_LINEAR
                                             : VK_FILTER_NEAREST;

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {static_cast<int32_t>(W), static_cast<int32_t>(H), 1};
    blit.dstOffsets[0] = {static_cast<int32_t>(ox), static_cast<int32_t>(oy),
                          0};
    blit.dstOffsets[1] = {static_cast<int32_t>(ox + fw),
                          static_cast<int32_t>(oy + fh), 1};
    vkCmdBlitImage(cmd_, img_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   swap_images_[acquired_], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, filter);

    if (window_shot) {
        // --window-shot evidence: grab the presented window (bars and all).
        VkImageMemoryBarrier cbar{};
        cbar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        cbar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        cbar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        cbar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        cbar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        cbar.image = swap_images_[acquired_];
        cbar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        cbar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        cbar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &cbar);
        VkBufferImageCopy bic2{};
        bic2.bufferOffset = 0;
        bic2.bufferRowLength = 0;
        bic2.bufferImageHeight = 0;
        bic2.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bic2.imageOffset = {0, 0, 0};
        bic2.imageExtent = {swap_w_, swap_h_, 1};
        vkCmdCopyImageToBuffer(cmd_, swap_images_[acquired_],
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               swap_staging_, 1, &bic2);
    }
    {
        VkImageMemoryBarrier pbar{};
        pbar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        pbar.oldLayout = window_shot ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                     : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        pbar.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        pbar.srcAccessMask = window_shot ? VK_ACCESS_TRANSFER_READ_BIT
                                         : VK_ACCESS_TRANSFER_WRITE_BIT;
        pbar.dstAccessMask = 0;
        pbar.image = swap_images_[acquired_];
        pbar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        pbar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        pbar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &pbar);
    }

    if (readback) {
        VkBufferImageCopy bic{};
        bic.bufferOffset = 0;
        bic.bufferRowLength = 0;
        bic.bufferImageHeight = 0;
        bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bic.imageOffset = {0, 0, 0};
        bic.imageExtent = {W, H, 1};
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
        create_menu_target(w, h);
    }
    VkResult r = vkAcquireNextImageKHR(device_, swap_, 1000000000ull,
                                       acquire_sem_, VK_NULL_HANDLE, &acquired_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        destroy_swapchain();
        create_swapchain(w, h);
        create_menu_target(w, h);
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
    const uint32_t W = kInternalW * scale_;
    const uint32_t H = kInternalH * scale_;
    const size_t bytes = static_cast<size_t>(W) * H * 4;
    out.resize(bytes);
    std::memcpy(out.data(), data, bytes);
    vkUnmapMemory(device_, staging_mem_);
    return true;
}

bool Renderer::readback_window(std::vector<uint8_t>& out) {
    const VkResult wr = vkWaitForFences(device_, 1, &fence_, VK_TRUE,
                                        1000000000ull);
    if (wr != VK_SUCCESS)
        return false;
    void* data = nullptr;
    if (vkMapMemory(device_, swap_staging_mem_, 0, VK_WHOLE_SIZE, 0, &data) !=
        VK_SUCCESS)
        return false;
    const size_t bytes = static_cast<size_t>(swap_w_) * swap_h_ * 4;
    out.resize(bytes);
    std::memcpy(out.data(), data, bytes);
    // The swapchain surface is B8G8R8A8: the raw readback bytes are BGRA,
    // but write_bmp expects RGBA. Normalize here so window-shot captures
    // match the displayed window (and the internal readback) channel for
    // channel — unswapped, every R/B comparison against a screenshot or
    // the internal capture silently mismatches.
    if (swap_fmt_ == VK_FORMAT_B8G8R8A8_UNORM) {
        for (size_t i = 0; i < bytes; i += 4) {
            const uint8_t r = out[static_cast<std::size_t>(i) + 2];
            out[static_cast<std::size_t>(i) + 2] = out[static_cast<std::size_t>(i)];
            out[static_cast<std::size_t>(i)] = r;
        }
    }
    vkUnmapMemory(device_, swap_staging_mem_);
    return true;
}

void Renderer::dump_atlases(const std::string& prefix) {
    // All three buffers are HOST_VISIBLE, so remapping shows exactly the
    // bytes the GPU last saw. Wait for the queue to drain first.
    vkDeviceWaitIdle(device_);
    auto dump = [&](VkBuffer buf, VkDeviceMemory mem, uint64_t bytes,
                    const char* suffix) {
        if (!buf || !mem || !bytes)
            return;
        void* m = nullptr;
        if (vkMapMemory(device_, mem, 0, VK_WHOLE_SIZE, 0, &m) !=
            VK_SUCCESS)
            return;
        std::ofstream f(prefix + suffix, std::ios::binary);
        if (f) {
            const uint32_t n = static_cast<uint32_t>(bytes);
            f.write(reinterpret_cast<const char*>(&n), 4);
            f.write(static_cast<const char*>(m),
                    static_cast<std::streamsize>(bytes));
        }
        vkUnmapMemory(device_, mem);
    };
    dump(wall_atlas_, wall_atlas_mem_,
         static_cast<uint64_t>(wall_atlas_texels_) * 4, ".wall.bin");
    dump(sprite_atlas_, sprite_atlas_mem_,
         static_cast<uint64_t>(sprite_atlas_texels_) * 4, ".sprite.bin");
    if (frame_map_) {
        std::ofstream f(prefix + ".frame.bin", std::ios::binary);
        if (f)
            f.write(static_cast<const char*>(frame_map_),
                    static_cast<std::streamsize>(kFrameBufSize));
    }
}

// Rebuilds the internal image + readback staging at (w x h). Called from
// the constructor (320x200) and from set_res_scale(). Safe to call any
// time; destroys the previous pair first. The descriptor set's image
// binding is refreshed in a second step (update_internal_descriptor)
// because dset_ may not exist yet in the constructor.
void Renderer::create_internal(uint32_t w, uint32_t h) {
    if (img_view_ != VK_NULL_HANDLE)
        vkDestroyImageView(device_, img_view_, nullptr);
    if (img_ != VK_NULL_HANDLE)
        vkDestroyImage(device_, img_, nullptr);
    if (img_mem_ != VK_NULL_HANDLE)
        vkFreeMemory(device_, img_mem_, nullptr);
    if (staging_ != VK_NULL_HANDLE)
        vkDestroyBuffer(device_, staging_, nullptr);
    if (staging_mem_ != VK_NULL_HANDLE)
        vkFreeMemory(device_, staging_mem_, nullptr);
    img_ = VK_NULL_HANDLE;
    img_view_ = VK_NULL_HANDLE;
    img_mem_ = VK_NULL_HANDLE;
    staging_ = VK_NULL_HANDLE;
    staging_mem_ = VK_NULL_HANDLE;

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {w, h, 1};
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

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bi, nullptr, &staging_) != VK_SUCCESS)
        vk_fail("vkCreateBuffer(staging)", VK_ERROR_INITIALIZATION_FAILED);
    vkGetBufferMemoryRequirements(device_, staging_, &mr);
    alloc_mem(mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, staging_mem_);
    vkBindBufferMemory(device_, staging_, staging_mem_, 0);
}

// Refreshes the descriptor set's internal-image binding to the current
// view (used after set_res_scale rebuilt the image).
void Renderer::update_internal_descriptor() {
    if (dset_ == VK_NULL_HANDLE)
        return;  // constructor path: the initial set init points at it
    VkDescriptorImageInfo dii{};
    dii.imageView = img_view_;
    dii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet wds{};
    wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds.dstSet = dset_;
    wds.dstBinding = 6;
    wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wds.pImageInfo = &dii;
    vkUpdateDescriptorSets(device_, 1, &wds, 0, nullptr);
}

void Renderer::set_res_scale(uint32_t s) {
    s = (s < 1) ? 1 : ((s > kMaxScale) ? kMaxScale : s);
    if (s == scale_)
        return;
    scale_ = s;
    create_internal(kInternalW * s, kInternalH * s);
    update_internal_descriptor();
}

void Renderer::set_blit_filter(BlitFilter f) {
    blit_filter_ = f;
}

void Renderer::internal_size(uint32_t& w, uint32_t& h) const {
    w = kInternalW * scale_;
    h = kInternalH * scale_;
}

void Renderer::create_menu_target(uint32_t w, uint32_t h) {
    // Menus render at window resolution (the MenuDraw coordinates are in
    // window px) and are blitted 1:1 into the swapchain.
    menu_w_ = w;
    menu_h_ = h;
    if (menu_img_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, menu_img_view_, nullptr);
        vkDestroyImage(device_, menu_img_, nullptr);
        vkFreeMemory(device_, menu_img_mem_, nullptr);
        menu_img_ = VK_NULL_HANDLE;
        menu_img_view_ = VK_NULL_HANDLE;
        menu_img_mem_ = VK_NULL_HANDLE;
    }
    VkImageCreateInfo mi{};
    mi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    mi.imageType = VK_IMAGE_TYPE_2D;
    mi.format = VK_FORMAT_R8G8B8A8_UNORM;
    mi.extent = {w, h, 1};
    mi.mipLevels = 1;
    mi.arrayLayers = 1;
    mi.samples = VK_SAMPLE_COUNT_1_BIT;
    mi.tiling = VK_IMAGE_TILING_OPTIMAL;
    mi.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    mi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &mi, nullptr, &menu_img_) != VK_SUCCESS)
        vk_fail("vkCreateImage(menu)", VK_ERROR_INITIALIZATION_FAILED);
    VkMemoryRequirements imr{};
    vkGetImageMemoryRequirements(device_, menu_img_, &imr);
    alloc_mem(imr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, menu_img_mem_);
    vkBindImageMemory(device_, menu_img_, menu_img_mem_, 0);
    VkImageViewCreateInfo ivi{};
    ivi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivi.image = menu_img_;
    ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivi.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &ivi, nullptr, &menu_img_view_) !=
        VK_SUCCESS)
        vk_fail("vkCreateImageView(menu)", VK_ERROR_INITIALIZATION_FAILED);
    if (menu_staging_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, menu_staging_, nullptr);
        vkFreeMemory(device_, menu_staging_mem_, nullptr);
        menu_staging_ = VK_NULL_HANDLE;
        menu_staging_mem_ = VK_NULL_HANDLE;
    }
    VkMemoryRequirements mr{};
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = static_cast<VkDeviceSize>(menu_w_) * menu_h_ * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bi, nullptr, &menu_staging_) != VK_SUCCESS)
        vk_fail("vkCreateBuffer(menu staging)", VK_ERROR_INITIALIZATION_FAILED);
    vkGetBufferMemoryRequirements(device_, menu_staging_, &mr);
    alloc_mem(mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, menu_staging_mem_);
    vkBindBufferMemory(device_, menu_staging_, menu_staging_mem_, 0);
    // Refresh the descriptor set's image binding (the set is reused).
    VkDescriptorImageInfo dii{};
    dii.imageView = menu_img_view_;
    dii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet wds{};
    wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds.dstSet = menu_dset_;
    wds.dstBinding = 2;
    wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wds.pImageInfo = &dii;
    vkUpdateDescriptorSets(device_, 1, &wds, 0, nullptr);
}

void Renderer::render_menu(const app::MenuDraw& d) {
    // Upload the per-frame menu description (host visible, mapped).
    std::memcpy(menu_data_map_, &d, sizeof(app::MenuDraw));
    VkMappedMemoryRange r{};
    r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    r.memory = menu_data_mem_;
    r.offset = 0;
    r.size = VK_WHOLE_SIZE;
    vkFlushMappedMemoryRanges(device_, 1, &r);

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
    bar.image = menu_img_;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &bar);

    // Full-window dispatch (8x8 workgroups).
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, menu_pipe_);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE,
                            menu_layout_, 0, 1, &menu_dset_, 0, nullptr);
    vkCmdDispatch(cmd_, (menu_w_ + 7) / 8, (menu_h_ + 7) / 8, 1);

    bar = {};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bar.image = menu_img_;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &bar);

    // 1:1 NEAREST blit into the swapchain (the menu image is already at
    // window resolution — no scaling).
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {static_cast<int32_t>(menu_w_),
                          static_cast<int32_t>(menu_h_), 1};
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {static_cast<int32_t>(swap_w_),
                          static_cast<int32_t>(swap_h_), 1};
    vkCmdBlitImage(cmd_, menu_img_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   swap_images_[acquired_], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                   1, &blit, VK_FILTER_NEAREST);

    // Copy to staging so readback_menu can grab it (always recorded; the
    // menu is only shown in UI mode and the copy is cheap).
    VkBufferImageCopy bic{};
    bic.bufferOffset = 0;
    bic.bufferRowLength = 0;
    bic.bufferImageHeight = 0;
    bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bic.imageOffset = {0, 0, 0};
    bic.imageExtent = {menu_w_, menu_h_, 1};
    vkCmdCopyImageToBuffer(cmd_, menu_img_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           menu_staging_, 1, &bic);

    if (vkEndCommandBuffer(cmd_) != VK_SUCCESS)
        vk_fail("vkEndCommandBuffer", VK_ERROR_INITIALIZATION_FAILED);
    has_work_ = true;
}

bool Renderer::readback_menu(std::vector<uint8_t>& out, uint32_t& w,
                             uint32_t& h) {
    const VkResult wr = vkWaitForFences(device_, 1, &fence_, VK_TRUE,
                                        1000000000ull);
    if (wr != VK_SUCCESS)
        return false;
    void* data = nullptr;
    if (vkMapMemory(device_, menu_staging_mem_, 0, VK_WHOLE_SIZE, 0,
                    &data) != VK_SUCCESS)
        return false;
    const size_t bytes = static_cast<size_t>(menu_w_) * menu_h_ * 4;
    out.resize(bytes);
    std::memcpy(out.data(), data, bytes);
    vkUnmapMemory(device_, menu_staging_mem_);
    w = menu_w_;
    h = menu_h_;
    return true;
}

void Renderer::swapchain_size(uint32_t& w, uint32_t& h) const {
    w = swap_w_;
    h = swap_h_;
}

} // namespace w3d::render
