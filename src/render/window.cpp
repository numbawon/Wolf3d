// src/render/window.cpp — SDL3 window + Vulkan surface.
#include "window.hpp"

#include <SDL3/SDL_vulkan.h>

#include <cstdio>
#include <cstdlib>

namespace w3d::render {

Window::Window(uint32_t w, uint32_t h, const char* title, VkInstance instance,
               SDL_WindowFlags extra_flags)
    : instance_(instance) {
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | extra_flags;
    window_ = SDL_CreateWindow(title, static_cast<int>(w), static_cast<int>(h), flags);
    if (!window_) {
        std::fprintf(stderr, "w3d: SDL_CreateWindow failed: %s\n", SDL_GetError());
        std::abort();
    }
    if (!SDL_Vulkan_CreateSurface(window_, instance, nullptr, &surface_)) {
        std::fprintf(stderr, "w3d: SDL_Vulkan_CreateSurface failed: %s\n",
                     SDL_GetError());
        std::abort();
    }
}

Window::~Window() {
    if (surface_ != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (window_)
        SDL_DestroyWindow(window_);
}

void Window::size(uint32_t& w, uint32_t& h) const {
    int iw = 0, ih = 0;
    SDL_GetWindowSize(window_, &iw, &ih);
    w = static_cast<uint32_t>(iw > 0 ? iw : 1);
    h = static_cast<uint32_t>(ih > 0 ? ih : 1);
}

bool Window::poll() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT)
            return false;
        if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            return false;
    }
    return true;
}

} // namespace w3d::render
