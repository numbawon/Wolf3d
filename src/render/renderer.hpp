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

#include "app/menu.hpp"
#include "frame.hpp"
#include "window.hpp"

namespace w3d::render {

// Internal render resolution (the original game renders 320x200 and scales).
// WS3: the internal target is (kInternalW*S, kInternalH*S) for S = res_scale
// in 1..4 (see kMaxScale in frame.hpp); S=1 is the pixel-perfect baseline.
inline constexpr uint32_t kInternalW = 320;
inline constexpr uint32_t kInternalH = 200;

// Window blit filters for the internal -> swapchain blit (WS3 / Decision 2).
enum class BlitFilter {
    Linear,   // VK_FILTER_LINEAR (default)
    Nearest,  // VK_FILTER_NEAREST
    Integer,  // nearest at an integer window scale (falls back to the max
              // non-integer fit when the window is smaller than the frame)
};

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
    // window_shot additionally copies the PRESENTED window (letterbox bars
    // and all) for a --window-shot capture.
    void render_frame(const Frame& f,
                      const std::array<std::uint32_t, 256>& palette,
                      bool readback, bool fog_on, bool window_shot = false);
    // Acquires the next swapchain image (recreating the swapchain on
    // resize). Returns false if the window should close.
    bool begin_frame();
    // Submits the recorded work and presents.
    void present();

    // ---- Internal resolution scale (WS3) ----
    // Sets the internal render scale S in 1..4 (out-of-range is clamped):
    // the raycaster re-renders at (320*S, 200*S). Rebuilds the offscreen
    // target immediately (device idle first); the window is untouched.
    void set_res_scale(uint32_t s);
    [[nodiscard]] uint32_t res_scale() const { return scale_; }
    // Current internal (offscreen) size in pixels.
    [[nodiscard]] void internal_size(uint32_t& w, uint32_t& h) const;
    // Window blit filter for the internal -> swapchain blit.
    void set_blit_filter(BlitFilter f);

    // ---- Full-res menu pass (WS2) ----
    // Draws one menu frame (app::MenuDraw) into the window-sized menu
    // image and blits it 1:1 (nearest) into the swapchain. Pairs with
    // begin_frame() / present() exactly like render_frame().
    void render_menu(const app::MenuDraw& d);
    // Blocks until the last present() completes, then copies the menu
    // image into out (w*h*4 bytes, top-down); w/h take the size.
    bool readback_menu(std::vector<uint8_t>& out, uint32_t& w, uint32_t& h);
    [[nodiscard]] void swapchain_size(uint32_t& w, uint32_t& h) const;

    // Blocks until the last present() completes, then copies the internal
    // image into out ((320*S, 200*S)*4 bytes, top-down).
    bool readback(std::vector<uint8_t>& out);
    // WS3: same, but for the presented WINDOW (swap_w_ x swap_h_,
    // letterbox bars included). Pairs with render_frame(window_shot=true).
    bool readback_window(std::vector<uint8_t>& out);
    // WS3 debug: dump the host-visible GPU buffers (frame buffer, wall and
    // sprite atlases) to <prefix>.{frame,wall,sprite}.bin after a frame.
    void dump_atlases(const std::string& prefix);
    [[nodiscard]] VkFormat swapchain_format() const { return swap_fmt_; }
    [[nodiscard]] bool window_should_close() const { return window_gone_; }

private:
    void create_swapchain(uint32_t w, uint32_t h);
    void destroy_swapchain();
    // (Re)creates the internal (320*S, 200*S) storage image + its readback
    // staging buffer. The descriptor set's image binding is refreshed in a
    // second step (it may not exist yet in the constructor).
    void create_internal(uint32_t w, uint32_t h);
    void update_internal_descriptor();
    // (Re)creates the window-sized menu image + staging buffer. Called on
    // swapchain resize; the descriptor set's image binding is refreshed.
    void create_menu_target(uint32_t w, uint32_t h);
    uint32_t alloc_mem(const VkMemoryRequirements& mr, uint32_t want_bits,
                       VkDeviceMemory& out);

    Window& window_;

    // Internal render scale (WS3) + the window blit filter.
    uint32_t scale_ = 1;
    BlitFilter blit_filter_ = BlitFilter::Linear;

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
    // WS3: host-visible staging for the presented window (--window-shot).
    // Sized to the swapchain; recreated with it.
    VkBuffer swap_staging_ = VK_NULL_HANDLE;
    VkDeviceMemory swap_staging_mem_ = VK_NULL_HANDLE;
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

    // WS3 letterbox bars: the ARM64 N1X driver segfaults inside
    // vkCmdClearAttachments on the swapchain image (bisected with
    // frame-1 checkpoints, 2026-11-16), so the bars are painted with a
    // full-window NEAREST blit of this 1x1 black image instead.
    VkImage black_img_ = VK_NULL_HANDLE;
    VkDeviceMemory black_mem_ = VK_NULL_HANDLE;

    // Shared descriptor layout: 0 columns SSBO, 1 wall atlas SSBO,
    // 2 sprite atlas SSBO, 3 palette SSBO, 4 sprite records SSBO,
    // 5 count UBO {spriteCount, scale}, 6 storage image,
    // 7 wall scratch SSBO (WS3 two-pass walls).
    VkShaderModule walls1_mod_ = VK_NULL_HANDLE;
    VkShaderModule walls2_mod_ = VK_NULL_HANDLE;
    VkShaderModule sprites_mod_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_ = VK_NULL_HANDLE;
    VkDescriptorPool dpool_ = VK_NULL_HANDLE;
    VkDescriptorSet dset_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline walls1_pipe_ = VK_NULL_HANDLE;
    VkPipeline walls2_pipe_ = VK_NULL_HANDLE;
    VkPipeline sprites_pipe_ = VK_NULL_HANDLE;

    // Static atlases (u32 per texel, index in the low byte)
    VkBuffer wall_atlas_ = VK_NULL_HANDLE;
    VkDeviceMemory wall_atlas_mem_ = VK_NULL_HANDLE;
    VkBuffer sprite_atlas_ = VK_NULL_HANDLE;
    VkDeviceMemory sprite_atlas_mem_ = VK_NULL_HANDLE;
    uint32_t wall_atlas_texels_ = 0;
    uint32_t sprite_atlas_texels_ = 0;

    // Per-frame dynamic buffer (host visible, permanently mapped). WS3: the
    // layout is scale-INDEPENDENT — the scale-dependent regions (columns,
    // walls fog tail) are reserved at the S=4 capacity, and every descriptor
    // offset is 256-byte aligned (the ARM64 driver clamps unaligned UBO
    // descriptor offsets DOWN — proven in M4; keep the padding).
    //   columns:      1280 x 32B @0          (.. 40960)
    //   walls fog:    invstep @40960, on @40968      (16B)
    //   rowdist:      800 x 8B @40976        (.. 47376)
    //   wlo:          1280 x 4B @47376       (.. 52496)
    //   whi:          1280 x 4B @52496       (.. 57616)
    //   sprites:      256 x 48B @57856       (.. 70144)
    //   sprite fog:   invstep @70144, on @70152 (16B, in the sprites range)
    //   palette:      256 x 4B @70400        (.. 71424)
    //   count UBO:    {spriteCount, scale} @71424 (8B)
    //   wall scratch: 1280 x 800 u32 @71680  (WS3 two-pass walls: one u32
    //                                        per internal pixel, column-major
    //                                        scratch[x*H + row]; S=4 capacity)
    // The dynamic buffer is 4167680 B (256 * 16280) in total.
    VkBuffer frame_buf_ = VK_NULL_HANDLE;
    VkDeviceMemory frame_mem_ = VK_NULL_HANDLE;
    void* frame_map_ = nullptr;

    // Full-res menu pass (WS2): window-sized storage image + staging,
    // per-frame MenuDraw SSBO (host visible, mapped), static font SSBO,
    // dedicated pipeline/descriptors.
    VkImage menu_img_ = VK_NULL_HANDLE;
    VkDeviceMemory menu_img_mem_ = VK_NULL_HANDLE;
    VkImageView menu_img_view_ = VK_NULL_HANDLE;
    VkBuffer menu_staging_ = VK_NULL_HANDLE;
    VkDeviceMemory menu_staging_mem_ = VK_NULL_HANDLE;
    VkBuffer menu_data_ = VK_NULL_HANDLE;
    VkDeviceMemory menu_data_mem_ = VK_NULL_HANDLE;
    void* menu_data_map_ = nullptr;
    VkBuffer menu_font_ = VK_NULL_HANDLE;
    VkDeviceMemory menu_font_mem_ = VK_NULL_HANDLE;
    VkShaderModule menu_mod_ = VK_NULL_HANDLE;
    VkPipeline menu_pipe_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout menu_dsl_ = VK_NULL_HANDLE;
    VkPipelineLayout menu_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool menu_dpool_ = VK_NULL_HANDLE;
    VkDescriptorSet menu_dset_ = VK_NULL_HANDLE;
    uint32_t menu_w_ = 0, menu_h_ = 0;
};

} // namespace w3d::render
