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
//     - "START shoot" / "START activate": one-shot event on tick START;
//     - lines starting with '#' are comments.
//
// Without a script the window accepts live keyboard + mouse input: left-
// click captures the mouse for turning (while captured, left click fires,
// as in the original), Esc or closing the window quits; a per-second fps
// line goes to stdout.
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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "app/menu.hpp"
#include "app/repeat.hpp"
#include "app/scene.hpp"
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
#include "app/audio_device.hpp"
#include "audio/mixer.hpp"
#include "w3d/audio.hpp"
#include "w3d/imf.hpp"

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

// WS3: F11 mode changes are logged to the console (user feedback).
const char* fullscreen_mode_name(w3d::render::FullscreenMode m) {
    switch (m) {
        case w3d::render::FullscreenMode::Windowed:
            return "windowed";
        case w3d::render::FullscreenMode::Borderless:
            return "borderless";
        case w3d::render::FullscreenMode::Exclusive:
            return "exclusive";
    }
    return "?";
}

// ---- WS5: SFX mapping (sim event -> bank index) ----
// PCM SFX are indexed 0..sfx_count-1 in file order. FM effects are indexed
// by chunk number; search by name. Indices below are best-guess values from
// the original game data and MUST be verified by ear (probe_audio_bank.txt
// captured the real names/indices in a previous session).
struct SfxEntry {
    const char* fm_name;  // non-null: search fm vector by this name
    int pcm_index;        // used when fm_name is null (-1 = not a PCM SFX)
};
static const SfxEntry kSfxMapBase[] = {
    /* SfxWeapon */ {"SHOT2", -1},   // FM effect
    /* SfxDoor */   {nullptr,  9},   // PCM SFX
    /* SfxPickup */ {nullptr,  3},   // PCM SFX
    /* SfxEnemyDie */{nullptr, 17},  // PCM SFX
    /* SfxLevelExit */{nullptr, 50}, // PCM SFX
};
static const SfxEntry kSfxMapSod[] = {
    /* SfxWeapon */ {"SHOT2", -1},   // FM effect (SOD reuses the name)
    /* SfxDoor */   {nullptr,  9},
    /* SfxPickup */ {nullptr,  3},
    /* SfxEnemyDie */{nullptr, 17},
    /* SfxLevelExit */{nullptr, 50},
};

inline int find_fm_chunk(const w3d::AudioBank& bank, const char* name) {
    for (std::size_t i = 0; i < bank.fm.size(); ++i)
        if (bank.fm[i] && bank.fm[i]->name == name)
            return static_cast<int>(i);
    return -1;
}

// ---- WS5: level -> music track index ----
// Base game: 27 tracks; SOD: 24 tracks. Mapping is level -> track index,
// clamped to the track count. Values from the original game's music table;
// verify by ear.
static const int kMusicMapBase[] = {
    0,1,2,2,3,4,5,6,7,8,9,10,11,2,3,4,  // E1M1..E1M16
    5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20, // E2M1..E2M16
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15, // E3M1..E3M16
    0,1,2,3,4,5,6,7,8,9,10,11, // E4M1..E4M12
};
static const int kMusicMapSod[] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20, // SOD levels
};

inline int music_for_level(int level, bool sod, int track_count) {
    const auto& map = sod ? kMusicMapSod : kMusicMapBase;
    const std::size_t n = sizeof(map) / sizeof(map[0]);
    if (level < 0 || level >= static_cast<int>(n))
        return level % track_count;
    return map[level] % track_count;
}

// ---- WS5: sim events -> mixer voices ----
// Maps the events of the most recent sim tick to SFX voices, panned and
// attenuated relative to the player.
inline void play_sim_events(w3d::app::AudioDevice& dev,
                            const w3d::AudioBank& bank,
                            const w3d::game::Simulation& sim, bool sod,
                            SDL_Gamepad* pad = nullptr) {
    using Event = w3d::game::Simulation::Event;
    constexpr double kMaxDist = 1000.0;  // cells at which the gain reaches 0
    constexpr double kNearDist = 0.5;    // closer than this: no panning
    const auto& sfx_map = sod ? kSfxMapSod : kSfxMapBase;
    const std::size_t map_size = sizeof(kSfxMapBase) / sizeof(kSfxMapBase[0]);
    const double facing = std::atan2(sim.dy(), sim.dx());
    for (const Event& sev : sim.events()) {
        // WS6: rumble on the connected pad (damage only).
        if (sev.type == Event::Rumble) {
            if (pad)
                SDL_RumbleGamepad(
                    pad,
                    static_cast<std::uint16_t>(
                        0x8000u *
                        static_cast<unsigned>(sev.rumbleStrength)),
                    0xC000u,
                    sev.rumbleMs > 0
                        ? static_cast<std::uint32_t>(sev.rumbleMs)
                        : 100u);
            continue;
        }
        if (sev.type == Event::None || sev.sfxIndex < 0 ||
            static_cast<std::size_t>(sev.sfxIndex) >= map_size)
            continue;
        const auto& entry = sfx_map[static_cast<std::size_t>(sev.sfxIndex)];
        w3d::app::SfxPlay sp;
        w3d::audio::Mixer::position(sev.wx, sev.wy, sim.x(), sim.y(), facing,
                                    kMaxDist, sp.pan, sp.gain);
        // A sound at the player's own position has no direction.
        if (std::hypot(sev.wx - sim.x(), sev.wy - sim.y()) < kNearDist)
            sp.pan = 0.0f;
        if (entry.fm_name) {
            sp.bankIndex = find_fm_chunk(bank, entry.fm_name);
            sp.isFm = true;
        } else {
            sp.bankIndex = entry.pcm_index;
            sp.isFm = false;
        }
        if (sp.bankIndex >= 0)
            dev.playSfx(sp);
    }
}

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
    bool pose = false;
    // Scene mode (WS2): start in a given scene and capture the first menu
    // frame (hidden window, one frame) — test hooks for the menu pass.
    std::string scene_start;
    std::string menu_shot;
    uint32_t res_scale = 1;  // internal render scale 1..4 (WS3)
    bool res_scale_given = false;  // WS4: CLI override flag tracking
    std::string dump_atlas;  // WS3 debug: dump GPU buffers after the shot
    std::string win_size;    // WS3: initial window "WxH" (default 640x400)
    std::string blit_filter; // WS3: linear | nearest | integer (window blit)
    std::string window_shot; // WS3: save the presented window (bars incl.)
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
        } else if (std::strcmp(argv[i], "--pose") == 0) {
            a.pose = true;
        } else if (std::strcmp(argv[i], "--scene") == 0 && i + 1 < argc) {
            a.scene_start = argv[++i];
        } else if (std::strcmp(argv[i], "--menu-shot") == 0 && i + 1 < argc) {
            a.menu_shot = argv[++i];
        } else if (std::strcmp(argv[i], "--res-scale") == 0 && i + 1 < argc) {
            a.res_scale = static_cast<uint32_t>(std::stoul(argv[++i]));
            a.res_scale_given = true;
        } else if (std::strcmp(argv[i], "--dump-atlas") == 0 && i + 1 < argc) {
            a.dump_atlas = argv[++i];
        } else if (std::strcmp(argv[i], "--win") == 0 && i + 1 < argc) {
            a.win_size = argv[++i];
        } else if (std::strcmp(argv[i], "--blit-filter") == 0 && i + 1 < argc) {
            a.blit_filter = argv[++i];
        } else if (std::strcmp(argv[i], "--window-shot") == 0 && i + 1 < argc) {
            a.window_shot = argv[++i];
        } else {
            std::fprintf(stderr,
                         "usage: w3d_app [--level N] [--sod] "
                         "[--data-dir PATH] [--script FILE] "
                         "[--screenshot PATH] [--frames N] [--one-frame] "
                         "[--scene NAME] [--menu-shot PATH] "
                         "[--res-scale N] [--win WxH] "
                         "[--blit-filter linear|nearest|integer] "
                         "[--window-shot PATH] [--dump-atlas PREFIX]\n");
            std::abort();
        }
    }
    if (a.one_frame && a.screenshot.empty()) {
        std::fprintf(stderr, "w3d: --one-frame requires --screenshot\n");
        std::abort();
    }
    if (!a.blit_filter.empty() && a.blit_filter != "linear" &&
        a.blit_filter != "nearest" && a.blit_filter != "integer") {
        std::fprintf(stderr,
                     "w3d: --blit-filter must be linear|nearest|integer\n");
        std::abort();
    }
    if (a.one_frame)
        a.frames = 1;
    if (!a.menu_shot.empty())
        a.frames = 1;
    return a;
}

// "960x400" -> (960, 400). Empty string -> (640, 400) default.
void parse_win_size(const std::string& s, uint32_t& w, uint32_t& h) {
    w = 640;
    h = 400;
    if (s.empty())
        return;
    const auto x = s.find('x');
    if (x == std::string::npos) {
        std::fprintf(stderr, "w3d: --win expects WxH, got '%s'\n", s.c_str());
        std::abort();
    }
    w = static_cast<uint32_t>(std::stoul(s.substr(0, x)));
    h = static_cast<uint32_t>(std::stoul(s.substr(x + 1)));
    if (w < 1 || h < 1) {
        std::fprintf(stderr, "w3d: --win dimensions must be >= 1\n");
        std::abort();
    }
}

// SDL3 3.4.x reports key names capitalized ("Up", "W"); the scene machine
// matches lower-cased names.
std::string lower_key(const char* k) {
    std::string s = k ? k : "";
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
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
            if (!line.empty() && line[0] == '#')
                continue;  // comment
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

    // Legacy (deterministic harness) mode: fixed level, direct sim — the
    // Phase 3/4 pipeline, untouched. Scene mode (WS2 default): the scene
    // machine picks the level; the sim is created on Game-scene entry.
    const bool legacy = !args.script.empty() || !args.screenshot.empty();
    std::optional<w3d::game::Level> opt_level;
    if (legacy) {
        auto level = w3d::game::Level::from_map(
            maps.maps[static_cast<std::size_t>(args.level)]);
        if (!level.has_start) {
            std::fprintf(stderr, "w3d: level %d has no player start\n",
                         args.level);
            return 1;
        }
        opt_level.emplace(std::move(level));
    }

    // ---- Config (WS4): load once, CLI flags override for this run.
    // Flags NEVER write the file (the scripted harness stays
    // deterministic); the scene loop saves on change / on clean exit.
    const auto cfg_path = w3d::app::default_config_path();
    const auto load_res = w3d::app::load_config(cfg_path);
    w3d::app::Config cfg = load_res.cfg;
    if (!load_res.file_valid)
        std::fprintf(stderr, "w3d: %s\n", load_res.note.c_str());
    {
        w3d::app::CliOverrides cli;
        if (args.res_scale_given) {
            cli.has_res_scale = true;
            cli.res_scale = static_cast<int>(args.res_scale);
        }
        if (!args.blit_filter.empty()) {
            cli.has_scale_filter = true;
            cli.scale_filter = w3d::app::cfgname::scale_filter(
                args.blit_filter);
        }
        if (args.no_fog) {
            cli.has_fog = true;
            cli.fog = false;
        }
        w3d::app::apply_cli_overrides(cfg, cli);
    }
    // Interactive = the user drives the menus (not a harness run). Only
    // interactive scene-mode runs ever write the config file.
    const bool interactive =
        args.frames == 0 && args.scene_start.empty() &&
        args.menu_shot.empty() && args.script.empty();

    // ---- WS5: audio bank + output device ----
    const auto audiohed_path = args.sod
        ? base_dir / "AUDIOHED.SOD"
        : base_dir / "AUDIOHED.WL6";
    const auto audiot_path = args.sod
        ? base_dir / "AUDIOT.SOD"
        : base_dir / "AUDIOT.WL6";
    auto bank_res = w3d::AudioBank::load(audiohed_path, audiot_path);
    if (!bank_res) {
        std::fprintf(stderr, "w3d: audio bank load failed: %s\n",
                     bank_res.error().message.c_str());
        return 1;
    }
    const auto& bank = *bank_res;
    std::unique_ptr<w3d::app::AudioDevice> audio_dev =
        std::make_unique<w3d::app::AudioDevice>(cfg.audio.device_id);
    audio_dev->setBank(&bank);
    audio_dev->setMasterVolume(
        static_cast<float>(cfg.audio.master_volume) / 100.0f);
    audio_dev->setMusicVolume(
        static_cast<float>(cfg.audio.music_volume) / 100.0f);
    audio_dev->setSfxVolume(
        static_cast<float>(cfg.audio.sfx_volume) / 100.0f);
    audio_dev->setMusicOn(cfg.audio.music_on);

    // ---- Window + renderer
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "w3d: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    int rc = 0;
    VkInstance instance = Renderer::create_instance();
    {
        SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
        if (args.one_frame || !args.menu_shot.empty())
            flags |= SDL_WINDOW_HIDDEN;
        uint32_t win_w = 0, win_h = 0;
        parse_win_size(args.win_size, win_w, win_h);
        Window window(win_w, win_h, "wolf3d (phase 5)", instance, flags);
        Renderer renderer(window, instance);
        // Video settings: config (with CLI overrides) is the source of
        // truth for this run.
        renderer.set_res_scale(static_cast<uint32_t>(cfg.video.res_scale));
        if (cfg.video.scale_filter == 1)
            renderer.set_blit_filter(w3d::render::BlitFilter::Nearest);
        else if (cfg.video.scale_filter == 2)
            renderer.set_blit_filter(w3d::render::BlitFilter::Integer);
        else
            renderer.set_blit_filter(w3d::render::BlitFilter::Linear);
        if (cfg.video.fullscreen_mode != 0) {
            const auto m = window.set_fullscreen(
                static_cast<w3d::render::FullscreenMode>(
                    cfg.video.fullscreen_mode));
            std::fprintf(stderr, "w3d: display mode: %s\n",
                         fullscreen_mode_name(m));
            // A driver that can't do exclusive reports the degraded mode;
            // the run follows reality.
            cfg.video.fullscreen_mode = static_cast<int>(m);
        }

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
        // The SOD VSWAP is re-packed: its pistol frames (byte-identical to
        // the base bank's 421..425) live at 406..410.
        const int weapon_shift = args.sod ? -15 : 0;
        std::array<std::uint32_t, 256> palette{};
        w3d::render::effective_palette(flash, palette);

        Frame frame{};
        LiveInput live;
        uint32_t frame_idx = 0;
        std::uint64_t fps_last = SDL_GetTicks();
        uint32_t fps_frames = 0;
        bool quit = false;
        // WS4: the per-second fps line follows video.show_fps; fog follows
        // gameplay.fog (CLI --no-fog already folded into cfg above). The
        // scene loop may update both (live-apply); the legacy loop reads
        // them.
        bool show_fps = cfg.video.show_fps;
        bool fog_on = cfg.gameplay.fog;
        if (legacy) {
            Simulation sim(std::move(*opt_level));
        while (!quit) {
            // SDL events (window close, Esc, live keys, relative mouse).
            bool window_open = true;
            SDL_Event ev{};
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_QUIT) {
                    window_open = false;
                } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                    // F11: fullscreen cycle (WS3) — works in every mode.
                    if (ev.key.key == SDLK_F11) {
                        const auto m = window.cycle_fullscreen();
                        std::fprintf(stderr, "w3d: display mode: %s\n",
                                     fullscreen_mode_name(m));
                        break;
                    }
                    if (!scripted) {
                        if (ev.key.key == SDLK_ESCAPE)
                            window_open = false;
                        live.on_key(SDL_GetKeyName(ev.key.key), true);
                    }
                } else if (!scripted && ev.type == SDL_EVENT_KEY_UP) {
                    live.on_key(SDL_GetKeyName(ev.key.key), false);
                } else if (!scripted && ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    // Left click: first click captures the mouse for turning;
                    // while captured, button 1 fires (original behavior).
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        if (SDL_GetWindowRelativeMouseMode(window.window()))
                            live.shoot();
                        else
                            SDL_SetWindowRelativeMouseMode(window.window(), true);
                    }
                } else if (!scripted && ev.type == SDL_EVENT_MOUSE_MOTION) {
                    live.on_mouse(ev.motion.xrel);
                }
            }
            if (!window_open)
                quit = true;
            if (args.frames != 0 && frame_idx >= args.frames)
                quit = true;

            // One simulation tick + one rendered frame. live.take() already
            // folds in this tick's relative-mouse turn. The pre-tick pose is
            // snapshotted for the renderer: the reference computes thing
            // relative coordinates before the player moves (see frame.hpp).
            const w3d::render::Pose pre_pose{sim.x(), sim.y(), sim.dx(),
                                             sim.dy()};
            const SimInput in = scripted ? script.input_for(frame_idx)
                                         : live.take();
            sim.tick(in);
            // WS5: consume sim events and map them to mixer voices.
            if (audio_dev)
                play_sim_events(*audio_dev, bank, sim, args.sod);
            w3d::render::build_frame(frame, sim, pre_pose, prev_snap,
                                     prev_snap_count, prev_weapon_cooldown,
                                     weapon_shift, args.res_scale);
            prev_weapon_cooldown = sim.weapon_cooldown();
            prev_snap = frame.actor_snap;
            prev_snap_count = frame.actor_snap_count;
            w3d::render::update_flash(flash, frame.flash_event);
            w3d::render::effective_palette(flash, palette);

            const bool do_shot = !args.screenshot.empty() &&
                                 args.frames != 0 &&
                                 frame_idx == args.frames - 1;
            // WS3: capture the PRESENTED window (letterbox bars included).
            const bool do_win_shot = !args.window_shot.empty() &&
                                     args.frames != 0 &&
                                     frame_idx == args.frames - 1;
            if (renderer.begin_frame()) {
                renderer.render_frame(frame, palette, do_shot, fog_on,
                                      do_win_shot);
                renderer.present();
                if (do_shot) {
                    std::vector<uint8_t> px;
                    uint32_t iw = 0, ih = 0;
                    renderer.internal_size(iw, ih);
                    if (renderer.readback(px) &&
                        w3d::util::write_bmp(args.screenshot, iw, ih,
                                             px.data())) {
                        std::printf("w3d: saved %s\n",
                                    args.screenshot.c_str());
                    } else {
                        std::fprintf(stderr, "w3d: screenshot failed\n");
                        rc = 1;
                    }
                    if (!args.dump_atlas.empty()) {
                        renderer.dump_atlases(args.dump_atlas);
                        std::printf("w3d: dumped GPU buffers %s.*\n",
                                    args.dump_atlas.c_str());
                    }
                }
                if (do_win_shot) {
                    std::vector<uint8_t> wp;
                    uint32_t sw = 0, sh = 0;
                    renderer.swapchain_size(sw, sh);
                    if (renderer.readback_window(wp) &&
                        w3d::util::write_bmp(args.window_shot, sw, sh,
                                             wp.data())) {
                        std::printf("w3d: saved window %s\n",
                                    args.window_shot.c_str());
                    } else {
                        std::fprintf(stderr, "w3d: window shot failed\n");
                        rc = 1;
                    }
                }
            } else {
                rc = 1;
                quit = true;
            }
            frame_idx++;
            if (show_fps) {
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
        if (args.pose) {
            std::printf("pose x=%.17g y=%.17g dx=%.17g dy=%.17g\n", sim.x(),
                        sim.y(), sim.dx(), sim.dy());
        }
        } else {
            // ---- Scene mode (WS2): scene machine + full-res menus ----
            w3d::app::SceneState scene = args.scene_start.empty()
                ? w3d::app::SceneState()
                : w3d::app::SceneState::from_name(args.scene_start);
            // WS4: the machine owns the settings the options screen edits.
            scene.set_config(cfg);
            live.set_sensitivity(cfg.controls.mouse_sensitivity);
            std::unique_ptr<Simulation> scene_sim;
            int scene_sim_level = -1;
            auto ensure_sim = [&]() {
                if (scene_sim && scene_sim_level == scene.level())
                    return;
                const int lvl =
                    std::min(scene.level(),
                             static_cast<int>(maps.maps.size()) - 1);
                scene_sim = std::make_unique<Simulation>(
                    w3d::game::Level::from_map(maps.maps[static_cast<std::size_t>(lvl)]));
                scene_sim_level = lvl;
                flash = Flash{};
                w3d::render::effective_palette(flash, palette);
                prev_snap = {};
                prev_snap_count = 0;
                prev_weapon_cooldown = 0;
                // WS5: set the music track for the level (silenced if music_off).
                const int track =
                    music_for_level(lvl, args.sod,
                                    static_cast<int>(bank.music.size()));
                if (track >= 0 && track < static_cast<int>(bank.music.size()))
                    audio_dev->setMusic(&bank.music[track]);
            };
            bool was_in_game = false;
            // WS6: gamepad (standard mapping, digitalized). `pad` tracks
            // the connected device; `menu_rep` applies repeat to the
            // per-frame menu direction.
            SDL_Gamepad* pad = nullptr;
            w3d::app::MenuRepeat menu_rep;
            // WS4 live-apply state: what the renderer/window actually run
            // on. After the machine reports a change we diff against this
            // and touch only what moved.
            w3d::app::Config applied = cfg;
            bool cfg_touched = false;
            while (!quit) {
                // SDL events: routed to the scene machine, or to live
                // input while in the Game scene.
                bool window_open = true;
                SDL_Event ev{};
                while (SDL_PollEvent(&ev)) {
                    if (ev.type == SDL_EVENT_QUIT ||
                        ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                        window_open = false;
                    } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                        // F11: fullscreen cycle (WS3) — before routing to
                        // the scene machine / live input.
                        if (ev.key.key == SDLK_F11) {
                            const auto m = window.cycle_fullscreen();
                            std::fprintf(stderr, "w3d: display mode: %s\n",
                                         fullscreen_mode_name(m));
                            // Keep the settings (and the options screen)
                            // in step with reality; not a user edit, so
                            // no save is triggered.
                            scene.cfg().video.fullscreen_mode =
                                static_cast<int>(m);
                            applied.video.fullscreen_mode =
                                static_cast<int>(m);
                            continue;
                        }
                        const char* raw = SDL_GetKeyName(ev.key.key);
                        if (scene.scene() == w3d::app::Scene::Game) {
                            // Esc routes to the scene machine (Esc ->
                            // Pause); everything else feeds live input.
                            if (ev.key.key == SDLK_ESCAPE) {
                                w3d::app::InputEvent ie;
                                ie.kind = w3d::app::InputEvent::KeyDown;
                                ie.key = "escape";
                                scene.on_input(ie);
                            } else {
                                live.on_key(raw, true);
                            }
                        } else {
                            w3d::app::InputEvent ie;
                            ie.kind = w3d::app::InputEvent::KeyDown;
                            ie.key = lower_key(raw);
                            scene.on_input(ie);
                        }
                    } else if (ev.type == SDL_EVENT_KEY_UP) {
                        if (scene.scene() == w3d::app::Scene::Game)
                            live.on_key(SDL_GetKeyName(ev.key.key), false);
                    } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                        if (ev.button.button == SDL_BUTTON_LEFT) {
                            if (scene.scene() == w3d::app::Scene::Game) {
                                // Left click: first click captures the
                                // mouse for turning; while captured, button
                                // 1 fires (original behavior).
                                if (SDL_GetWindowRelativeMouseMode(
                                        window.window()))
                                    live.shoot();
                                else
                                    SDL_SetWindowRelativeMouseMode(
                                        window.window(), true);
                            } else {
                                w3d::app::InputEvent ie;
                                ie.kind = w3d::app::InputEvent::MouseClick;
                                ie.mx = static_cast<int>(ev.button.x);
                                ie.my = static_cast<int>(ev.button.y);
                                scene.on_input(ie);
                            }
                        }
                    } else if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                        if (scene.scene() == w3d::app::Scene::Game) {
                            live.on_mouse(ev.motion.xrel);
                        } else {
                            w3d::app::InputEvent ie;
                            ie.kind = w3d::app::InputEvent::MouseMove;
                            ie.mx = static_cast<int>(ev.motion.x);
                            ie.my = static_cast<int>(ev.motion.y);
                            scene.on_input(ie);
                        }
                    } else if (ev.type == SDL_EVENT_GAMEPAD_ADDED) {
                        pad = SDL_OpenGamepad(ev.gdevice.which);
                        if (pad)
                            std::fprintf(stderr, "w3d: gamepad: %s\n",
                                         SDL_GetGamepadName(pad));
                        else
                            std::fprintf(stderr,
                                         "w3d: gamepad: open failed: %s\n",
                                         SDL_GetError());
                    } else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
                        if (pad) {
                            SDL_CloseGamepad(pad);
                            pad = nullptr;
                        }
                        live.clear_gamepad();
                        menu_rep.reset();
                        std::fprintf(stderr, "w3d: gamepad: removed\n");
                    } else if (ev.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
                        // Tracked in both contexts: in the Game scene the
                        // stick folds into the sim input (take()); out of
                        // it the same state feeds the menu direction.
                        // SDL3.4 axis values are Sint16 +-32767.
                        live.on_gamepad_axis(
                            ev.gaxis.axis,
                            static_cast<float>(ev.gaxis.value) / 32768.0f);
                    } else if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                        const int b = ev.gbutton.button;
                        if (scene.scene() == w3d::app::Scene::Game) {
                            if (b == w3d::game::gamepad::kStart) {
                                // Start = the Esc equivalent: pause.
                                w3d::app::InputEvent ie;
                                ie.kind = w3d::app::InputEvent::KeyDown;
                                ie.key = "escape";
                                scene.on_input(ie);
                            } else {
                                live.on_gamepad_button(b, true);
                            }
                        } else if (b == w3d::game::gamepad::kSouth) {
                            // Menus: A = confirm (Enter).
                            w3d::app::InputEvent ie;
                            ie.kind = w3d::app::InputEvent::KeyDown;
                            ie.key = "return";
                            scene.on_input(ie);
                        } else if (b == w3d::game::gamepad::kEast ||
                                   b == w3d::game::gamepad::kStart) {
                            // Menus: B / Start = back (Esc).
                            w3d::app::InputEvent ie;
                            ie.kind = w3d::app::InputEvent::KeyDown;
                            ie.key = "escape";
                            scene.on_input(ie);
                        } else if (b >= w3d::game::gamepad::kDpadUp &&
                                   b <= w3d::game::gamepad::kDpadRight) {
                            // D-pad: tracked for the per-frame menu
                            // direction (live gamepad state).
                            live.on_gamepad_button(b, true);
                        }
                    } else if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
                        live.on_gamepad_button(ev.gbutton.button, false);
                    }
                }
                if (!window_open || scene.quit_requested())
                    quit = true;
                if (args.frames != 0 && frame_idx >= args.frames)
                    quit = true;

                const bool in_game = scene.scene() == w3d::app::Scene::Game;
                if (in_game && !was_in_game)
                    ensure_sim();
                if (!in_game && was_in_game)
                    // Leaving the Game scene: release the captured mouse.
                    SDL_SetWindowRelativeMouseMode(window.window(), false);
                was_in_game = in_game;

                // WS6: gamepad menu navigation (out of the Game scene):
                // the digitalized direction with repeat becomes synthetic
                // key events for the scene machine.
                if (!in_game) {
                    const int dir = menu_rep.tick(live.gamepad_direction());
                    if (dir != w3d::game::gamepad::kDirNone) {
                        w3d::app::InputEvent ie;
                        ie.kind = w3d::app::InputEvent::KeyDown;
                        ie.key = dir == w3d::game::gamepad::kDirUp
                                     ? "up"
                                     : dir == w3d::game::gamepad::kDirDown
                                           ? "down"
                                           : dir ==
                                                 w3d::game::gamepad::kDirLeft
                                                 ? "left"
                                                 : "right";
                        scene.on_input(ie);
                    }
                }

                // WS4 live-apply: the options screen changed a value.
                if (scene.take_config_dirty()) {
                    cfg_touched = true;
                    const auto& nc = scene.cfg();
                    if (nc.video.res_scale != applied.video.res_scale)
                        renderer.set_res_scale(nc.video.res_scale);
                    if (nc.video.scale_filter !=
                        applied.video.scale_filter) {
                        if (nc.video.scale_filter == 1)
                            renderer.set_blit_filter(
                                w3d::render::BlitFilter::Nearest);
                        else if (nc.video.scale_filter == 2)
                            renderer.set_blit_filter(
                                w3d::render::BlitFilter::Integer);
                        else
                            renderer.set_blit_filter(
                                w3d::render::BlitFilter::Linear);
                    }
                    if (nc.video.fullscreen_mode !=
                        applied.video.fullscreen_mode) {
                        const auto m = window.set_fullscreen(
                            static_cast<w3d::render::FullscreenMode>(
                                nc.video.fullscreen_mode));
                        std::fprintf(stderr, "w3d: display mode: %s\n",
                                     fullscreen_mode_name(m));
                        if (m != static_cast<w3d::render::FullscreenMode>(
                                     nc.video.fullscreen_mode)) {
                            // The driver degraded the request (e.g. no
                            // exclusive): the screen follows reality; the
                            // correction rides the next save.
                            scene.cfg().video.fullscreen_mode =
                                static_cast<int>(m);
                        }
                    }
                    if (nc.video.show_fps != applied.video.show_fps)
                        show_fps = nc.video.show_fps;
                    if (nc.controls.mouse_sensitivity !=
                        applied.controls.mouse_sensitivity)
                        live.set_sensitivity(nc.controls.mouse_sensitivity);
                    if (nc.gameplay.fog != applied.gameplay.fog)
                        fog_on = nc.gameplay.fog;
                    // WS5: audio settings apply live (the device setters are
                    // thread-safe under the mixer mutex). device_id has no
                    // hot-switch; it applies on the next launch.
                    if (nc.audio.master_volume != applied.audio.master_volume)
                        audio_dev->setMasterVolume(
                            static_cast<float>(nc.audio.master_volume) / 100.0f);
                    if (nc.audio.music_volume != applied.audio.music_volume)
                        audio_dev->setMusicVolume(
                            static_cast<float>(nc.audio.music_volume) / 100.0f);
                    if (nc.audio.sfx_volume != applied.audio.sfx_volume)
                        audio_dev->setSfxVolume(
                            static_cast<float>(nc.audio.sfx_volume) / 100.0f);
                    if (nc.audio.music_on != applied.audio.music_on)
                        audio_dev->setMusicOn(nc.audio.music_on);
                    // difficulty is stored-only (WS7) — nothing to apply live.
                    if (interactive) {
                        std::string serr;
                        if (!w3d::app::save_config(cfg_path, scene.cfg(),
                                                   serr))
                            std::fprintf(stderr,
                                         "w3d: config save failed: %s\n",
                                         serr.c_str());
                    }
                    applied = scene.cfg();
                }

                if (renderer.begin_frame()) {
                    if (in_game) {
                        // The Phase 3 pipeline, driven by the scene's sim.
                        const w3d::render::Pose pre_pose{
                            scene_sim->x(), scene_sim->y(),
                            scene_sim->dx(), scene_sim->dy()};
                        const SimInput in = live.take();
                        scene_sim->tick(in);
                        // WS5: consume sim events and map them to audio.
                        // WS6: Rumble events also reach the gamepad.
                        if (audio_dev)
                            play_sim_events(*audio_dev, bank, *scene_sim,
                                            args.sod, pad);
                        w3d::render::build_frame(
                            frame, *scene_sim, pre_pose, prev_snap,
                            prev_snap_count, prev_weapon_cooldown,
                            weapon_shift, args.res_scale);
                        prev_weapon_cooldown = scene_sim->weapon_cooldown();
                        prev_snap = frame.actor_snap;
                        prev_snap_count = frame.actor_snap_count;
                        w3d::render::update_flash(flash, frame.flash_event);
                        w3d::render::effective_palette(flash, palette);
                        renderer.render_frame(frame, palette, false, fog_on);
                    } else {
                        uint32_t mw = 0, mh = 0;
                        renderer.swapchain_size(mw, mh);
                        scene.set_window(static_cast<int>(mw),
                                         static_cast<int>(mh));
                        w3d::app::MenuDraw md;
                        w3d::app::build_menu(scene, static_cast<int>(mw),
                                             static_cast<int>(mh), md);
                        renderer.render_menu(md);
                    }
                    renderer.present();
                    if (!args.menu_shot.empty() && frame_idx == 0) {
                        std::vector<uint8_t> px;
                        uint32_t mw = 0, mh = 0;
                        if (renderer.readback_menu(px, mw, mh) &&
                            w3d::util::write_bmp(args.menu_shot, mw, mh,
                                                 px.data())) {
                            std::printf("w3d: saved %s\n",
                                        args.menu_shot.c_str());
                        } else {
                            std::fprintf(stderr,
                                         "w3d: menu screenshot failed\n");
                            rc = 1;
                        }
                    }
                } else {
                    rc = 1;
                    quit = true;
                }
                frame_idx++;
                if (show_fps) {
                    const std::uint64_t now = SDL_GetTicks();
                    if (now - fps_last >= 1000) {
                        std::printf("w3d: %d fps\n",
                                    static_cast<int>(
                                        (frame_idx - fps_frames) * 1000 /
                                        (now - fps_last)));
                        fps_last = now;
                        fps_frames = frame_idx;
                    }
                }
                SDL_Delay(16);  // ~60 Hz
            }
            // WS4: clean-exit save — covers the first run (missing or
            // corrupt file: rewrite the effective defaults) and any change
            // the per-change save could not flush. Harness runs never
            // write the file.
            if (interactive && (cfg_touched || !load_res.file_valid)) {
                std::string serr;
                if (!w3d::app::save_config(cfg_path, scene.cfg(), serr))
                    std::fprintf(stderr, "w3d: config save failed: %s\n",
                                 serr.c_str());
            }
        }
    }
    if (audio_dev) {
        const auto st = audio_dev->stats();
        std::fprintf(stderr,
                     "w3d: audio: device %s, %llu frames pulled, peak %.3f\n",
                     audio_dev->ok() ? "ok" : "FAILED",
                     static_cast<unsigned long long>(st.frames),
                     static_cast<double>(st.peak));
    }
    vkDestroyInstance(instance, nullptr);
    SDL_Quit();
    return rc;
}
