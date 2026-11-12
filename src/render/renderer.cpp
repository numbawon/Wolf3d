// src/render/renderer.cpp — Vulkan device/swapchain/compute pipeline (M2).
#include "renderer.hpp"

#include <vulkan/vulkan_win32.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

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

} // namespace

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
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ii, nullptr, &img_) != VK_SUCCESS)
        vk_fail("vkCreateImage(internal)", VK_ERROR_INITIALIZATION_FAILED);
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(device_, img_, &mr);
    VkPhysicalDeviceMemoryProperties mprops{};
    vkGetPhysicalDeviceMemoryProperties(physical_, &mprops);
    uint32_t type = 0;
    for (; type < mprops.memoryTypeCount; ++type)
        if (mr.memoryTypeBits & (1u << type))
            if (mprops.memoryTypes[type].propertyFlags &
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                break;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(device_, &ai, nullptr, &img_mem_) != VK_SUCCESS)
        vk_fail("vkAllocateMemory(internal)", VK_ERROR_OUT_OF_HOST_MEMORY);
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
    type = 0;
    for (; type < mprops.memoryTypeCount; ++type)
        if (mr.memoryTypeBits & (1u << type))
            if (mprops.memoryTypes[type].propertyFlags &
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
                break;
    ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(device_, &ai, nullptr, &staging_mem_) != VK_SUCCESS)
        vk_fail("vkAllocateMemory(staging)", VK_ERROR_OUT_OF_HOST_MEMORY);
    vkBindBufferMemory(device_, staging_, staging_mem_, 0);

    // ---- Clear compute pipeline
    const auto spv = load_file(exe_dir() / "shaders" / "clear.spv");
    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = spv.size();
    smci.pCode = reinterpret_cast<const uint32_t*>(spv.data());
    if (vkCreateShaderModule(device_, &smci, nullptr, &clear_mod_) != VK_SUCCESS)
        vk_fail("vkCreateShaderModule(clear)", VK_ERROR_INITIALIZATION_FAILED);
    VkDescriptorSetLayoutBinding bind{};
    bind.binding = 0;
    bind.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bind.descriptorCount = 1;
    bind.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 1;
    dslci.pBindings = &bind;
    if (vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &clear_dsl_) !=
        VK_SUCCESS)
        vk_fail("vkCreateDescriptorSetLayout(clear)",
                VK_ERROR_INITIALIZATION_FAILED);
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 64};
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &clear_dsl_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(device_, &plci, nullptr, &clear_layout_) !=
        VK_SUCCESS)
        vk_fail("vkCreatePipelineLayout(clear)", VK_ERROR_INITIALIZATION_FAILED);
    VkComputePipelineCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = clear_mod_;
    stage.pName = "main";
    cpci.stage = stage;
    cpci.layout = clear_layout_;
    if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpci, nullptr,
                                 &clear_pipe_) != VK_SUCCESS)
        vk_fail("vkCreateComputePipelines(clear)", VK_ERROR_INITIALIZATION_FAILED);
    // Descriptor set pointing at the internal image.
    VkDescriptorPoolSize dps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &dps;
    if (vkCreateDescriptorPool(device_, &dpci, nullptr, &dpool_) != VK_SUCCESS)
        vk_fail("vkCreateDescriptorPool(clear)", VK_ERROR_INITIALIZATION_FAILED);
    VkDescriptorSetLayout dsls[1] = {clear_dsl_};
    VkDescriptorSetAllocateInfo dsa{};
    dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsa.descriptorPool = dpool_;
    dsa.descriptorSetCount = 1;
    dsa.pSetLayouts = dsls;
    if (vkAllocateDescriptorSets(device_, &dsa, &dset_) != VK_SUCCESS)
        vk_fail("vkAllocateDescriptorSets(clear)", VK_ERROR_OUT_OF_HOST_MEMORY);
    VkDescriptorImageInfo dii{};
    dii.imageView = img_view_;
    dii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet wds{};
    wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds.dstSet = dset_;
    wds.dstBinding = 0;
    wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wds.pImageInfo = &dii;
    vkUpdateDescriptorSets(device_, 1, &wds, 0, nullptr);

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
    vkDestroyPipeline(device_, clear_pipe_, nullptr);
    vkDestroyPipelineLayout(device_, clear_layout_, nullptr);
    vkDestroyDescriptorPool(device_, dpool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, clear_dsl_, nullptr);
    vkDestroyShaderModule(device_, clear_mod_, nullptr);
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

void Renderer::render_clear(float t, bool readback) {
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

    // Push constants: uint w, uint h, float t (16 bytes).
    uint32_t pc[4] = {kInternalW, kInternalH, 0, 0};
    std::memcpy(&pc[2], &t, sizeof(float));
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 64};
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, clear_pipe_);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, clear_layout_,
                            0, 1, &dset_, 0, nullptr);
    vkCmdPushConstants(cmd_, clear_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, 64,
                       pc);
    vkCmdDispatch(cmd_, (kInternalW + 7) / 8, (kInternalH + 7) / 8, 1);

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
    return true;
}

} // namespace w3d::render
