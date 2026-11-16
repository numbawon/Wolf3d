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

FullscreenMode Window::fullscreen_mode() const {
    const auto flags = SDL_GetWindowFlags(window_);
    if (!(flags & SDL_WINDOW_FULLSCREEN))
        return FullscreenMode::Windowed;
    // SDL3: the plain flag means "any fullscreen"; the MODE decides the
    // kind — NULL = borderless (desktop), non-NULL = exclusive.
    return SDL_GetWindowFullscreenMode(window_) ? FullscreenMode::Exclusive
                                                : FullscreenMode::Borderless;
}

FullscreenMode Window::cycle_fullscreen() {
    // SDL3 semantics verified on this platform (probe: scratch
    // w3d_ref/probe_fs.cpp, Nov 2026):
    //   - SDL_SetWindowFullscreen(win, true)  enters fullscreen using the
    //     window's *stored* fullscreen mode (NULL = borderless/desktop);
    //   - SDL_SetWindowFullscreen(win, false) exits to windowed (original
    //     size/position restored);
    //   - SDL_SetWindowFullscreenMode(win, mode) only sets the stored mode;
    //     called while already fullscreen it switches the mode
    //     asynchronously (borderless <-> exclusive);
    //   - SDL_SetWindowFullscreenMode(win, nullptr) alone is a silent
    //     no-op (returns success, never changes state) — do not rely on
    //     it to enter borderless.
    // If exclusive is not available on this driver the cycle degrades to
    // windowed -> borderless -> windowed (the documented fallback, plan
    // section 7), latched so the probe is logged once.
    const auto cur = fullscreen_mode();
    switch (cur) {
        case FullscreenMode::Windowed:
            // Clear any stored exclusive mode (it persists after leaving
            // fullscreen) so SetWindowFullscreen(true) enters borderless.
            SDL_SetWindowFullscreenMode(window_, nullptr);
            if (!SDL_SetWindowFullscreen(window_, true)) {
                std::fprintf(stderr,
                             "w3d: borderless fullscreen failed (%s), "
                             "staying windowed\n",
                             SDL_GetError());
                return FullscreenMode::Windowed;
            }
            SDL_SyncWindow(window_);
            return FullscreenMode::Borderless;
        case FullscreenMode::Borderless: {
            if (exclusive_unavailable_) {
                // Exclusive was unavailable; the cycle degrades to
                // windowed<->borderless (logged once at the failed probe).
                SDL_SetWindowFullscreen(window_, false);
                SDL_SyncWindow(window_);
                return FullscreenMode::Windowed;
            }
            const SDL_DisplayID disp = SDL_GetDisplayForWindow(window_);
            bool ok = false;
            SDL_DisplayMode** modes = nullptr;
            int n = 0;
            if (disp)
                modes = SDL_GetFullscreenDisplayModes(disp, &n);
            if (modes && n > 0) {
                // The list is sorted largest-first; prefer a mode matching
                // the desktop size (no visible change for the player),
                // else the largest.
                const SDL_DisplayMode* want = modes[0];
                const SDL_DisplayMode* desktop =
                    SDL_GetDesktopDisplayMode(disp);
                for (int i = 0; i < n; ++i) {
                    if (desktop && modes[i]->w == desktop->w &&
                        modes[i]->h == desktop->h) {
                        want = modes[i];
                        break;
                    }
                }
                ok = SDL_SetWindowFullscreenMode(window_, want);
            }
            if (modes)
                SDL_free(modes);
            if (ok)
                SDL_SyncWindow(window_);  // the switch is async; block on it
            if (ok && fullscreen_mode() == FullscreenMode::Exclusive) {
                exclusive_unavailable_ = false;  // it works; drop the latch
                return FullscreenMode::Exclusive;
            }
            // Exclusive not available on this driver: latch it and degrade
            // the cycle to windowed<->borderless.
            exclusive_unavailable_ = true;
            std::fprintf(stderr,
                         "w3d: exclusive fullscreen not available (%s); "
                         "F11 now cycles windowed<->borderless\n",
                         SDL_GetError());
            SDL_SetWindowFullscreen(window_, false);
            SDL_SyncWindow(window_);
            return FullscreenMode::Windowed;
        }
        case FullscreenMode::Exclusive:
            SDL_SetWindowFullscreen(window_, false);
            SDL_SyncWindow(window_);
            return FullscreenMode::Windowed;
    }
    return cur;
}

} // namespace w3d::render
