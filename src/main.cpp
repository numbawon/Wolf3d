// src/main.cpp — Phase 3 application entry point.
//
// M2: opens the window, runs the Vulkan renderer's test pattern, and (with
// --screenshot PATH --frames N) saves the last frame as a BMP. Without
// --screenshot the window stays open until closed. --one-frame renders a
// single hidden-window frame (smoke test; requires --screenshot).
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "render/renderer.hpp"
#include "render/window.hpp"
#include "util/bmp.hpp"

namespace {

struct Args {
    std::string screenshot;
    uint32_t frames = 0; // 0 = run until closed
    bool one_frame = false;
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            a.screenshot = argv[++i];
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            a.frames = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (std::strcmp(argv[i], "--one-frame") == 0) {
            a.one_frame = true;
        } else {
            std::fprintf(stderr,
                         "usage: wolf3d [--screenshot PATH] [--frames N] "
                         "[--one-frame]\n");
            std::abort();
        }
    }
    if (a.one_frame && a.screenshot.empty()) {
        std::fprintf(stderr, "w3d: --one-frame requires --screenshot\n");
        std::abort();
    }
    if (a.one_frame)
        a.frames = 1;
    return a;
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "w3d: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    int rc = 0;
    VkInstance instance = w3d::render::Renderer::create_instance();
    {
        SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
        if (args.one_frame)
            flags |= SDL_WINDOW_HIDDEN;
        w3d::render::Window window(640, 400, "wolf3d (phase 3)", instance, flags);
        w3d::render::Renderer renderer(window, instance);

        uint32_t frame = 0;
        double t = 0.0;
        bool quit = false;
        while (!quit) {
            if (!window.poll())
                quit = true;
            if (args.frames != 0 && frame >= args.frames)
                quit = true;
            if (renderer.begin_frame()) {
                const bool do_shot =
                    !args.screenshot.empty() && args.frames != 0 &&
                    frame == args.frames - 1;
                renderer.render_clear(static_cast<float>(t), do_shot);
                renderer.present();
                if (do_shot) {
                    std::vector<uint8_t> px;
                    if (renderer.readback(px) &&
                        w3d::util::write_bmp(
                            args.screenshot, w3d::render::kInternalW,
                            w3d::render::kInternalH, px.data())) {
                        std::printf("w3d: saved %s\n",
                                    args.screenshot.c_str());
                    } else {
                        std::fprintf(stderr, "w3d: screenshot failed\n");
                        rc = 1;
                    }
                }
            }
            frame++;
            t += 1.0 / 60.0;
            SDL_Delay(16);
        }
    }
    vkDestroyInstance(instance, nullptr);
    SDL_Quit();
    return rc;
}
