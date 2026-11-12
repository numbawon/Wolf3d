// src/render/renderer.hpp — Vulkan device/swapchain/compute pipeline.
//
// M2: renders a test pattern (compute shader) into a fixed internal image
// (kInternalW x kInternalH) and blits it scaled into the swapchain.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

#include "window.hpp"

namespace w3d::render {

// Internal render resolution (the original game renders 320x200 and scales).
inline constexpr uint32_t kInternalW = 320;
inline constexpr uint32_t kInternalH = 200;

class Renderer {
public:
    // Creates the Vulkan instance (surface + win32 extensions). The caller
    // owns it and destroys it after the Renderer is gone.
    static VkInstance create_instance();

    Renderer(Window& window, VkInstance instance);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // One rendered frame:
    //   render_clear(t, readback)  — records work (test pattern + optional
    //                                copy to host staging)
    //   present()                  — submits, waits, presents
    // Call begin_frame() first; it recreates the swapchain on resize and
    // acquires an image (returns false if the window should close).
    bool begin_frame();
    void render_clear(float t, bool readback);
    void present();
    // Blocks until the last present() completes, then copies the internal
    // image into out (kInternalW*kInternalH*4 bytes).
    bool readback(std::vector<uint8_t>& out);
    [[nodiscard]] VkFormat swapchain_format() const { return swap_fmt_; }

private:
    void create_swapchain(uint32_t w, uint32_t h);
    void destroy_swapchain();

    Window& window_;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;

    // Swapchain state
    VkSwapchainKHR swap_ = VK_NULL_HANDLE;
    std::vector<VkImage> swap_images_;
    std::vector<VkImageView> swap_views_;
    VkFormat swap_fmt_ = VK_FORMAT_UNDEFINED;
    uint32_t swap_w_ = 0, swap_h_ = 0;

    // Command pool/buffer (single in-flight frame)
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;

    // Synchronization
    VkSemaphore present_sem_ = VK_NULL_HANDLE;
    VkSemaphore acquire_sem_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    uint32_t acquired_ = 0;
    bool has_work_ = false;

    // Internal image + readback staging
    VkImage img_ = VK_NULL_HANDLE;
    VkImageView img_view_ = VK_NULL_HANDLE;
    VkDeviceMemory img_mem_ = VK_NULL_HANDLE;
    VkBuffer staging_ = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem_ = VK_NULL_HANDLE;

    // Clear pipeline
    VkShaderModule clear_mod_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout clear_dsl_ = VK_NULL_HANDLE;
    VkDescriptorPool dpool_ = VK_NULL_HANDLE;
    VkDescriptorSet dset_ = VK_NULL_HANDLE;
    VkPipelineLayout clear_layout_ = VK_NULL_HANDLE;
    VkPipeline clear_pipe_ = VK_NULL_HANDLE;
};

} // namespace w3d::render
