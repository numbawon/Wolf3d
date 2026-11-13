// src/render/window.hpp — SDL3 window + Vulkan surface.
#pragma once

#include <SDL3/SDL.h>

#include <vulkan/vulkan.h>

namespace w3d::render {

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

private:
    SDL_Window* window_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkInstance instance_ = VK_NULL_HANDLE;
};

} // namespace w3d::render
