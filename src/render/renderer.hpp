// src/render/renderer.hpp — Vulkan device/swapchain/compute pipelines.
//
// M3: renders one simulation frame. The CPU (frame.cpp) has already run the
// reference DDA and sprite setup; this class uploads the per-frame records
// plus the static texture atlases, dispatches the wall pass (per column)
// and the sprite pass (per pixel) into the 320x200 internal image, and
// blits it scaled into the swapchain.
#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <vector>

#include "frame.hpp"
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

    // Upload the static texture atlases once per level. walls/sprites are
    // row-major 64x64 palette-index textures (one per VSWAP chunk).
    void load_atlases(
        const std::vector<const std::array<std::uint8_t, 4096>*>& walls,
        const std::vector<const std::array<std::uint8_t, 4096>*>& sprites);

    // One rendered frame: records the wall + sprite dispatches (and an
    // optional copy to host staging) into the command buffer.
    void render_frame(const Frame& f,
                      const std::array<std::uint32_t, 256>& palette,
                      bool readback, bool fog_on);
    // Acquires the next swapchain image (recreating the swapchain on
    // resize). Returns false if the window should close.
    bool begin_frame();
    // Submits the recorded work and presents.
    void present();

    // Blocks until the last present() completes, then copies the internal
    // image into out (kInternalW*kInternalH*4 bytes).
    bool readback(std::vector<uint8_t>& out);
    [[nodiscard]] VkFormat swapchain_format() const { return swap_fmt_; }
    [[nodiscard]] bool window_should_close() const { return window_gone_; }

private:
    void create_swapchain(uint32_t w, uint32_t h);
    void destroy_swapchain();
    uint32_t alloc_mem(const VkMemoryRequirements& mr, uint32_t want_bits,
                       VkDeviceMemory& out);

    Window& window_;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint64_t buf_align_ = 64;

    // Swapchain state
    VkSwapchainKHR swap_ = VK_NULL_HANDLE;
    std::vector<VkImage> swap_images_;
    std::vector<VkImageView> swap_views_;
    VkFormat swap_fmt_ = VK_FORMAT_UNDEFINED;
    uint32_t swap_w_ = 0, swap_h_ = 0;
    bool window_gone_ = false;

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
    VkDeviceMemory img_mem_ = VK_NULL_HANDLE;
    VkImageView img_view_ = VK_NULL_HANDLE;
    VkBuffer staging_ = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem_ = VK_NULL_HANDLE;
    // TEMP DEBUG mirror of the frame buffer (GPU -> host copy).
    VkBuffer dbg_buf_ = VK_NULL_HANDLE;
    VkDeviceMemory dbg_mem_ = VK_NULL_HANDLE;

    // Shared descriptor layout: 0 columns SSBO, 1 wall atlas SSBO,
    // 2 sprite atlas SSBO, 3 palette SSBO, 4 sprite records SSBO,
    // 5 sprite count UBO, 6 storage image.
    VkShaderModule walls_mod_ = VK_NULL_HANDLE;
    VkShaderModule sprites_mod_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_ = VK_NULL_HANDLE;
    VkDescriptorPool dpool_ = VK_NULL_HANDLE;
    VkDescriptorSet dset_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline walls_pipe_ = VK_NULL_HANDLE;
    VkPipeline sprites_pipe_ = VK_NULL_HANDLE;

    // Static atlases (u32 per texel, index in the low byte)
    VkBuffer wall_atlas_ = VK_NULL_HANDLE;
    VkDeviceMemory wall_atlas_mem_ = VK_NULL_HANDLE;
    VkBuffer sprite_atlas_ = VK_NULL_HANDLE;
    VkDeviceMemory sprite_atlas_mem_ = VK_NULL_HANDLE;
    uint32_t wall_atlas_texels_ = 0;
    uint32_t sprite_atlas_texels_ = 0;

    // Per-frame dynamic buffer (host visible, permanently mapped) with
    // sub-regions: columns @0 (320x32B), fog @10240 (two doubles, walls
    // pass), sprites @16384 (256x40B), fog @26624 (two doubles, sprite
    // pass), palette @32768 (256x4B), count @33792 (4B).
    VkBuffer frame_buf_ = VK_NULL_HANDLE;
    VkDeviceMemory frame_mem_ = VK_NULL_HANDLE;
    void* frame_map_ = nullptr;
};

} // namespace w3d::render
