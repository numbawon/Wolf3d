// tests/test_window.cpp — WS3 display-mode tests (F11 cycle).
//
// Exercises Window::fullscreen_mode() / cycle_fullscreen() against the
// real SDL3 + display. The F11 cycle must be one of:
//   windowed -> borderless -> exclusive -> windowed -> borderless (the
//   driver supports exclusive), or
//   windowed -> borderless -> windowed -> borderless (degraded: exclusive
//   probe failed, the latch degrades the cycle, logged to stderr).
// Borderless entry must always succeed (desktop mode == current mode).
// While borderless, the window size must match the desktop display mode.
// If no video display is available (headless CI) the test skips.
//
// Note: a successful exclusive cycle briefly switches the real display
// mode and restores it; that is the documented F11 behavior.

#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstdio>

#include "render/window.hpp"

using w3d::render::FullscreenMode;
using w3d::render::Window;

namespace {

const char* name(FullscreenMode m) {
    switch (m) {
        case FullscreenMode::Windowed:
            return "windowed";
        case FullscreenMode::Borderless:
            return "borderless";
        case FullscreenMode::Exclusive:
            return "exclusive";
    }
    return "?";
}

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL %s\n", what);
    }
}

}  // namespace

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SKIP: no video display (%s)\n", SDL_GetError());
        return 0;
    }
    VkInstance instance = VK_NULL_HANDLE;
    {
        // SDL3 has no SDL_Vulkan_CreateInstance: the app (renderer.cpp)
        // builds the instance itself with the surface extensions.
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "w3d_window_test";
        app.apiVersion = VK_MAKE_API_VERSION(0, 1, 4, 0);
        const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                              VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = 2;
        ci.ppEnabledExtensionNames = exts;
        if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS) {
            std::printf("SKIP: vkCreateInstance failed\n");
            SDL_Quit();
            return 0;
        }
    }

    {
        Window win(640, 400, "w3d window test", instance);

        const auto m0 = win.fullscreen_mode();
        std::printf("initial: %s\n", name(m0));
        check(m0 == FullscreenMode::Windowed, "starts windowed");

        const auto m1 = win.cycle_fullscreen();
        std::printf("cycle 1: %s\n", name(m1));
        check(m1 == FullscreenMode::Borderless,
              "borderless entry must always succeed");
        if (m1 == FullscreenMode::Borderless) {
            // Borderless takes the desktop extent.
            int iw = 0, ih = 0;
            SDL_GetWindowSize(win.window(), &iw, &ih);
            const SDL_DisplayID disp = SDL_GetDisplayForWindow(win.window());
            const SDL_DisplayMode* desktop =
                disp ? SDL_GetDesktopDisplayMode(disp) : nullptr;
            std::printf("borderless size: %dx%d (desktop %dx%d)\n", iw, ih,
                        desktop ? desktop->w : -1, desktop ? desktop->h : -1);
            check(desktop && iw == desktop->w && ih == desktop->h,
                  "borderless size matches desktop mode");
        }

        const auto m2 = win.cycle_fullscreen();
        std::printf("cycle 2: %s\n", name(m2));
        check(m2 == FullscreenMode::Exclusive || m2 == FullscreenMode::Windowed,
              "borderless next is exclusive (or degraded windowed)");

        const auto m3 = win.cycle_fullscreen();
        std::printf("cycle 3: %s\n", name(m3));
        if (m2 == FullscreenMode::Exclusive)
            check(m3 == FullscreenMode::Windowed, "exclusive -> windowed");
        else
            check(m3 == FullscreenMode::Borderless,
                  "degraded cycle returns to borderless");

        const auto m4 = win.cycle_fullscreen();
        std::printf("cycle 4: %s\n", name(m4));
        if (m2 == FullscreenMode::Exclusive)
            check(m4 == FullscreenMode::Borderless, "windowed -> borderless");
        else
            check(m4 == FullscreenMode::Windowed, "borderless -> windowed");

        // Leave the display in windowed mode.
        int guard = 0;
        while (win.fullscreen_mode() != FullscreenMode::Windowed && guard < 3) {
            (void)win.cycle_fullscreen();
            ++guard;
        }
        check(win.fullscreen_mode() == FullscreenMode::Windowed,
              "restored to windowed at test end");
    }

    if (instance != VK_NULL_HANDLE)
        vkDestroyInstance(instance, nullptr);
    SDL_Quit();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
