// src/render/window.hpp — SDL3 window + Vulkan surface.
#pragma once

#include <SDL3/SDL.h>

#include <vulkan/vulkan.h>

namespace w3d::render {

// Window display mode (WS3, Decision 2). F11 cycles
// windowed -> borderless -> (exclusive, if the driver supports it)
// -> windowed. SDL3 mapping (verified on this platform): borderless =
// SDL_SetWindowFullscreen(win, true) with a NULL stored fullscreen mode;
// exclusive = SDL_SetWindowFullscreenMode(win, mode) while already
// fullscreen.
enum class FullscreenMode {
    Windowed,    // plain resizable window
    Borderless,  // fullscreen at the desktop extent, no frame
    Exclusive,   // real display-mode switch; probed on ARM64
};

// Owns the SDL window and the KHR surface bound to it.
class Window {
public:
    // Creates the window and its Vulkan surface (instance must already exist).
    Window(uint32_t w, uint32_t h, const char* title, VkInstance instance,
           SDL_WindowFlags extra_flags = 0);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    [[nodiscard]] VkSurfaceKHR surface() const { return surface_; }
    [[nodiscard]] SDL_Window* window() const { return window_; }
    // Logical drawable size (matches the swapchain extent).
    [[nodiscard]] void size(uint32_t& w, uint32_t& h) const;
    // Processes pending SDL events; returns false on close/quit.
    bool poll();

    // WS3: current display mode, and the F11 cycle (windowed -> borderless
    // -> exclusive -> windowed). If exclusive fails on this driver, the
    // cycle degrades to windowed -> borderless -> windowed (logged).
    [[nodiscard]] FullscreenMode fullscreen_mode() const;
    [[nodiscard]] FullscreenMode cycle_fullscreen();
    // WS4: jump straight to a display mode (options screen, live-apply).
    // Returns the mode ACTUALLY in effect — if exclusive is unavailable
    // on this driver (latched, logged), borderless or windowed instead.
    [[nodiscard]] FullscreenMode set_fullscreen(FullscreenMode want);

private:
    SDL_Window* window_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkInstance instance_ = VK_NULL_HANDLE;
    // Latched when an exclusive-fullscreen probe fails, so the F11 cycle
    // degrades to windowed<->borderless instead of re-attempting (and
    // re-failing) exclusive on every press. Cleared if a later attempt
    // succeeds.
    bool exclusive_unavailable_ = false;
    // Enters borderless (desktop) fullscreen from whatever state; returns
    // the mode actually in effect.
    FullscreenMode enter_borderless();
    // Tries the exclusive switch (probes display modes, latches failure).
    // Returns true when exclusive is in effect afterwards.
    bool enter_exclusive();
};

} // namespace w3d::render
