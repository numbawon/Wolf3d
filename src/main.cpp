// src/main.cpp — Phase 3 application entry point (M3).
//
// Runs the headless simulation (Phase 2) at 60 ticks/s and renders each
// frame with the Vulkan raycaster (frame.cpp + the compute passes). Input
// comes from the reference keymap (arrows = move/turn, q/d = strafe, held;
// f/Control shoot, space activate) plus relative mouse turning, or, for
// deterministic runs, from an input script:
//
//   --script FILE   text lines:
//     - "START END KEY ...": movement keys (ArrowUp ArrowDown ArrowLeft
//       ArrowRight z s q d) held on ticks START..END-1;
//     - "START shoot" / "START activate": one-shot event on tick START.
//
// Without a script the window accepts live keyboard + mouse input: left-
// click captures the mouse for turning, Esc or closing the window quits;
// a per-second fps line goes to stdout.
//
// Data:
//   --data-dir PATH  default: the Steam Wolfenstein 3D base directory
//   --sod            use the Shattered Oaths files (m1 subdirectory)
//   --level N        level index (base 0..59, SoD 0..20)
//   --frames N       render N frames then exit
//   --screenshot PATH  save the last frame (of N) as a BMP
//   --one-frame      hidden window, one frame (requires --screenshot)
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "game/input.hpp"
#include "game/level.hpp"
#include "game/sim.hpp"
#include "render/frame.hpp"
#include "render/renderer.hpp"
#include "render/window.hpp"
#include "util/bmp.hpp"
#include "w3d/error.hpp"
#include "w3d/maps.hpp"
#include "w3d/vswap.hpp"

namespace {

using w3d::game::kBwd;
using w3d::game::kFwd;
using w3d::game::kLeft;
using w3d::game::kRight;
using w3d::game::kStrafeL;
using w3d::game::kStrafeR;
using w3d::game::LiveInput;
using w3d::game::SimInput;
using w3d::game::Simulation;
using w3d::render::Frame;
using w3d::render::Flash;
using w3d::render::Renderer;
using w3d::render::Window;

struct Args {
    std::filesystem::path data_dir =
        std::filesystem::path("C:/Program Files (x86)/Steam/steamapps/common/") /
        "Wolfenstein 3D" / "base";
    bool sod = false;
    int level = 0;
    std::string screenshot;
    std::string script;
    uint32_t frames = 0;  // 0 = run until closed
    bool one_frame = false;
    bool no_fog = false;
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            a.screenshot = argv[++i];
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            a.frames = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (std::strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
            a.level = std::stoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--sod") == 0) {
            a.sod = true;
        } else if (std::strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            a.data_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
            a.script = argv[++i];
        } else if (std::strcmp(argv[i], "--one-frame") == 0) {
            a.one_frame = true;
        } else if (std::strcmp(argv[i], "--no-fog") == 0) {
            a.no_fog = true;
        } else {
            std::fprintf(stderr,
                         "usage: w3d_app [--level N] [--sod] "
                         "[--data-dir PATH] [--script FILE] "
                         "[--screenshot PATH] [--frames N] [--one-frame]\n");
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

// Deterministic input script (see the file header). Movement keys are held
// ranges; "shoot"/"activate" are one-shot events.
struct Script {
    std::vector<bool> shoot{};
    std::vector<bool> activate{};
    std::vector<std::uint8_t> held{};  // bitmask of held movement keys
    std::size_t ticks = 0;

    // Reference keymap: arrows + z/s move/turn; q/d strafe.
    static std::uint8_t key_mask(const std::string& k) {
        if (k == "ArrowUp" || k == "z")
            return kFwd;
        if (k == "ArrowDown" || k == "s")
            return kBwd;
        if (k == "ArrowLeft")
            return kLeft;
        if (k == "ArrowRight")
            return kRight;
        if (k == "q")
            return kStrafeL;
        if (k == "d")
            return kStrafeR;
        return 0;
    }

    SimInput input_for(std::size_t tick) {
        SimInput in;
        if (tick >= ticks)
            return in;
        const auto h = held[tick];
        in.forward = h & kFwd;
        in.backward = h & kBwd;
        in.turn_left = h & kLeft;
        in.turn_right = h & kRight;
        in.strafe_left = h & kStrafeL;
        in.strafe_right = h & kStrafeR;
        in.shoot = shoot[tick];
        in.activate = activate[tick];
        return in;
    }

    bool parse(const std::string& path, std::size_t ticks_) {
        std::ifstream f(path);
        if (!f) {
            std::fprintf(stderr, "w3d: cannot open script %s\n",
                         path.c_str());
            return false;
        }
        ticks = ticks_;
        shoot.assign(ticks, false);
        activate.assign(ticks, false);
        held.assign(ticks, 0);
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream is(line);
            std::string tok;
            if (!(is >> tok))
                continue;
            const long start = std::stol(tok);
            if (start < 0 || start >= static_cast<long>(ticks)) {
                std::fprintf(stderr, "w3d: script tick out of range: %ld\n",
                             start);
                return false;
            }
            // Second token: event name or range end.
            std::string second;
            is >> second;
            if (second == "shoot") {
                shoot[static_cast<std::size_t>(start)] = true;
                continue;
            }
            if (second == "activate") {
                activate[static_cast<std::size_t>(start)] = true;
                continue;
            }
            // START END KEY...: movement keys held on START..END-1.
            long end = start + 1;
            bool numeric = !second.empty();
            for (char c : second)
                if (c < '0' || c > '9')
                    numeric = false;
            if (numeric)
                end = std::stol(second);
            if (end <= start)
                continue;
            if (end > static_cast<long>(ticks))
                end = static_cast<long>(ticks);
            std::uint8_t mask = 0;
            std::string key;
            while (is >> key)
                mask |= key_mask(key);
            if (!mask)
                continue;
            for (long t = start; t < end; ++t)
                held[static_cast<std::size_t>(t)] |= mask;
        }
        return true;
    }
};

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    // ---- Game data
    const auto base_dir = args.sod ? args.data_dir / "m1" : args.data_dir;
    const auto maps_res = w3d::GameMaps::load(
        base_dir / (args.sod ? "GAMEMAPS.SOD" : "GAMEMAPS.WL6"),
        base_dir / (args.sod ? "MAPHEAD.SOD" : "MAPHEAD.WL6"));
    if (!maps_res.has_value()) {
        std::fprintf(stderr, "w3d: %s\n", maps_res.error().message.c_str());
        return 1;
    }
    const auto& maps = maps_res.value();
    if (args.level < 0 || args.level >= static_cast<int>(maps.maps.size())) {
        std::fprintf(stderr, "w3d: level %d out of range (0..%zu)\n",
                     args.level, maps.maps.size() - 1);
        return 1;
    }
    const auto vswap_res =
        w3d::Vswap::load(base_dir / (args.sod ? "VSWAP.SOD" : "VSWAP.WL6"));
    if (!vswap_res.has_value()) {
        std::fprintf(stderr, "w3d: %s\n",
                     vswap_res.error().message.c_str());
        return 1;
    }
    const auto& vswap = vswap_res.value();

    // Deterministic run?
    Script script;
    const bool scripted = !args.script.empty();
    if (scripted) {
        const std::size_t ticks =
            args.frames ? args.frames : static_cast<std::size_t>(1);
        if (!script.parse(args.script, ticks))
            return 1;
    }

    // ---- Simulation
    auto level = w3d::game::Level::from_map(
        maps.maps[static_cast<std::size_t>(args.level)]);
    Simulation sim(std::move(level));
    if (!sim.level().has_start) {
        std::fprintf(stderr, "w3d: level %d has no player start\n", args.level);
        return 1;
    }

    // ---- Window + renderer
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "w3d: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    int rc = 0;
    VkInstance instance = Renderer::create_instance();
    {
        SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
        if (args.one_frame)
            flags |= SDL_WINDOW_HIDDEN;
        Window window(640, 400, "wolf3d (phase 3)", instance, flags);
        Renderer renderer(window, instance);

        // Static atlases from the VSWAP banks.
        {
            std::vector<const std::array<std::uint8_t, 4096>*> walls;
            walls.reserve(vswap.walls.size());
            for (auto& w : vswap.walls)
                walls.push_back(&w);
            std::vector<const std::array<std::uint8_t, 4096>*> sprites;
            sprites.reserve(vswap.sprites.size());
            for (auto& s : vswap.sprites)
                sprites.push_back(&s.idx);
            renderer.load_atlases(walls, sprites);
        }

        Flash flash{};
        std::array<Frame::ActorSnap, w3d::render::kMaxSprites> prev_snap{};
        int prev_snap_count = 0;
        int prev_weapon_cooldown = 0;
        std::array<std::uint32_t, 256> palette{};
        w3d::render::effective_palette(flash, palette);

        Frame frame{};
        LiveInput live;
        uint32_t frame_idx = 0;
        std::uint64_t fps_last = SDL_GetTicks();
        uint32_t fps_frames = 0;
        bool quit = false;
        while (!quit) {
            // SDL events (window close, Esc, live keys, relative mouse).
            bool window_open = true;
            SDL_Event ev{};
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT) {
                    window_open = false;
                } else if (!scripted && ev.type == SDL_EVENT_KEY_DOWN) {
                    if (ev.key.key == SDLK_ESCAPE)
                        window_open = false;
                    live.on_key(SDL_GetKeyName(ev.key.key), true);
                } else if (!scripted && ev.type == SDL_EVENT_KEY_UP) {
                    live.on_key(SDL_GetKeyName(ev.key.key), false);
                } else if (!scripted && ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    // First left click captures the mouse for turning.
                    if (ev.button.button == SDL_BUTTON_LEFT)
                        SDL_SetWindowRelativeMouseMode(window.window(), true);
                } else if (!scripted && ev.type == SDL_EVENT_MOUSE_MOTION) {
                    live.on_mouse(ev.motion.xrel);
                }
            }
            if (!window_open)
                quit = true;
            if (args.frames != 0 && frame_idx >= args.frames)
                quit = true;

            // One simulation tick + one rendered frame. live.take() already
            // folds in this tick's relative-mouse turn.
            const SimInput in = scripted ? script.input_for(frame_idx)
                                         : live.take();
            sim.tick(in);
            w3d::render::build_frame(frame, sim, prev_snap, prev_snap_count,
                                     prev_weapon_cooldown);
            prev_weapon_cooldown = sim.weapon_cooldown();
            prev_snap = frame.actor_snap;
            prev_snap_count = frame.actor_snap_count;
            w3d::render::update_flash(flash, frame.flash_event);
            w3d::render::effective_palette(flash, palette);

            const bool do_shot = !args.screenshot.empty() &&
                                 args.frames != 0 &&
                                 frame_idx == args.frames - 1;
            if (renderer.begin_frame()) {
                renderer.render_frame(frame, palette, do_shot, !args.no_fog);
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
            } else {
                rc = 1;
                quit = true;
            }
            frame_idx++;
            {
                const std::uint64_t now = SDL_GetTicks();
                if (now - fps_last >= 1000) {
                    std::printf("w3d: %d fps\n",
                                static_cast<int>((frame_idx - fps_frames) *
                                                 1000 / (now - fps_last)));
                    fps_last = now;
                    fps_frames = frame_idx;
                }
            }
            if (!scripted)
                SDL_Delay(16);  // ~60 Hz
        }
    }
    vkDestroyInstance(instance, nullptr);
    SDL_Quit();
    return rc;
}
