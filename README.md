# Wolf3D Remake

A clean-room, modern-engine remake of id Software's **Wolfenstein 3D**
(1992), including the *Spear of Destiny* expansion.

No original source code is used. Every file format is derived from the
shipped game data and verified empirically (pixel and audio golden checks)
before any consumer is built. The deliverable is a single standalone native
binary — modern C++23, SDL3 + Vulkan, software FM audio. No DOS, no DOSBox,
no emulation, no runtime network. The only external input is the game data
itself.

## Status

| Phase | Scope | State |
|---|---|---|
| 1 | Data formats (TED5, Carpack, VSWAP, VGAGRAPH, maps, audio) + golden tests | done |
| 2 | Headless game simulation (maps, actors, doors, LOS, movement) | done |
| 3 | Vulkan compute raycaster — **pixel-perfect** vs an independent reference engine | done |
| 4 | Audio: OPL2 FM chip + IMF sequencer + resampling/positional mixer, bit-exact vs a JS chip reference | done |
| 5 | UI/HUD, menus, settings, input (kb + gamepad + rebinds), audio device, modern windowing | in progress on `phase-5-ui-hud` (WS1–WS5 done: VGAGRAPH decode, scene/menu skeleton, internal resolution 1x–4x + F11 fullscreen, persistent config + options/pause screens, miniaudio audio device with level music + positional SFX) |

The renderer has been verified **pixel-perfect** (0/64000 mismatching pixels
per frame) against a clean-room JS reimplementation of the original render
semantics, driven on the same real game data with the same input scripts.
The OPL2 chip port is bit-exact against uWolf's `opl2.js` (max sample diff
0.0 over 20 000 samples of a real track).

## Requirements

- **Windows** — the pinned dependency set targets **Windows ARM64**
  (development/verification machine: a Nitro N1X, MSVC 14.51, Vulkan on
  RTX Spark). x64 is not verified yet.
- CMake ≥ 3.29 and Ninja on `PATH`.
- Python 3 (dependency fetching, tools).
- A legitimate copy of *Wolfenstein 3D* on **Steam or GOG** (the modern
  port's data: `base/`, plus `base/m1/` for *Spear of Destiny*). The repo
  never ships game assets.
- A Vulkan-capable GPU.

## Build

```bat
git clone https://github.com/numbawon/Wolf3d
cd Wolf3d
.build\build_arm64.bat
```

The script is self-contained and idempotent: it sets up the MSVC ARM64
environment, fetches the **hash-pinned** dependencies into the gitignored
`.build/deps` (SDL3 3.4.20 prebuilt arm64, Vulkan SDK 1.4.357 headers plus an
ARM64 loader built from source, glslang 16.6.0 for build-time shader
compilation), generates the SDL3 import library from the DLL's export table,
configures and builds. All artifacts land in the gitignored `.build/` folder
— nothing build-related is tracked, nothing is written outside the repo.

The game app and its runtime DLLs end up in `.build\cmake\`.

## Run

```bat
.build\cmake\w3d_app.exe
```

The game data is located via `--data-dir PATH` or the `W3D_GAME_DIR`
environment variable (default: the standard Steam install path). Add `--sod`
to play *Spear of Destiny* (reads the `m1/` subdirectory).

**Controls** (original reference keymap; case-insensitive):

| Action | Keys |
|---|---|
| Move forward / back | `Up`/`W`/`Z` , `Down`/`S` |
| Turn left / right | `Left`/`A` , `Right`/`E` |
| Strafe left / right | `Q` / `D` |
| Shoot | `F`, `Ctrl`, or left mouse button |
| Activate (doors, switches, …) | `Space` |
| Pause (scene mode) / quit (harness mode) | `Esc` |
| Cycle windowed → borderless → exclusive fullscreen | `F11` |

**Gamepad** (standard mapping, Phase 5 — Steam Deck works out of the box
via SDL's gamepad database):

| Action | Pad |
|---|---|
| Move / turn | Left stick (digitalized, deadzone 0.25) |
| Shoot | `A` |
| Activate (doors, switches, …) | `B` |
| Pause | `Start` |
| Menu navigation | D-pad or stick; `A` = confirm, `B` / `Start` = back |

Mouse turn sensitivity (scene mode, Phase 5) is configurable in the options
screen and persisted to the config file.

### Application flags

```
w3d_app [--level N] [--sod] [--data-dir PATH] [--script FILE]
        [--screenshot PATH] [--frames N] [--one-frame] [--no-fog] [--pose]
        [--scene NAME] [--menu-shot PATH] [--win WxH]
        [--res-scale N] [--blit-filter linear|nearest|integer]
        [--window-shot PATH] [--dump-atlas PREFIX]
```

Two modes:

- **Play mode** — a level with live keyboard/mouse input. From Phase 5 on,
  the app boots into the scene flow: title menu → episode select → level
  intro → play, with a pause menu and an options screen (video / audio /
  controls / gameplay).
- **Harness mode** — deterministic scripted runs for verification:
  `--script FILE` (lines of `START END KEY` / `START shoot` / `START
  activate`), `--frames N` ticks, `--screenshot` captures the last internal
  frame, `--window-shot` the presented window, `--pose` dumps the final
  player state at full precision. This is the mode used for the
  pixel-perfect verification; it is fully deterministic (same input → same
  frame, every run).

Phase 5 adds a persistent config file at
`%APPDATA%\Wolf3DRemake\config.json` (versioned JSON). Interactive runs save
it on change and on clean exit; harness runs never write it. CLI flags
override the config for a single run and are never persisted.

## Tests

Seven suites, run directly or via `ctest` from `.build\cmake\`:

| Suite | What it pins |
|---|---|
| `w3d_tests` | all data formats decode to end-of-file with golden hashes (maps, walls, sprites, fonts, tiles, audio) |
| `w3d_sim_tests` | simulation semantics: movement, doors, LOS/DDA, triggers, weapons (golden transcript) |
| `w3d_audio_tests` | OPL2 chip + IMF sequencer + resampler/positional mixer |
| `w3d_scene_tests` | UI scene state machine + menu model (title → episode → intro → game → pause → options) |
| `w3d_config_tests` | config JSON: strict parse, tolerant mapping, round-trip, file I/O, CLI precedence |
| `w3d_scale_tests` | internal-resolution scale math (even-column identity, sprite scaling, every column written at each scale) |
| `w3d_window_tests` | F11 fullscreen cycle against the real display (skips when headless) |

At the time of writing (Phase 5, WS6 + pre-WS7 bugfix) the suites total
**5 716 checks**, all green. The suites grow as phases land; every phase
keeps the previous suites green.

## Tools

- `w3d_tool_audio_dump [gamedir] [outdir] [out_rate]` — renders the
  game's music (OPL2), FM effects, a PCM SFX, plus positional-pan and
  resampling demo tones to stereo WAV files at any output rate (default
  48 kHz) — the Phase 4 audible evidence. `--xcheck ref.json N` compares
  our OPL2 render of the first track against a reference render,
  sample-for-sample.
- `w3d_tool_vulkan_probe` — driver/toolchain probe (device, limits,
  extensions) for diagnosing a machine.
- `tools/analyze_maps.py [gamedir]` — quick Python inspection of the real
  maps (start cells, doors, histograms).

## Architecture

```
src/
  w3d/     data layer: TED5 containers, Carpack, VSWAP, VGAGRAPH (fonts/pics),
           map models, audio bank, OPL2 chip, IMF sequencer
  game/    headless simulation: levels, actors, LOS, doors, live input
  audio/   audio mixer: resampling (any rate/width) + positional pan (pure C++)
  app/     UI (Phase 5): scene state machine, menu model, config (JSON),
           miniaudio audio device (music + positional SFX pull)
  render/  SDL3 window + Vulkan: compute raycaster, blit/letterbox, menu pass
  util/    BMP writer and small helpers
tests/     the seven suites above
tools/     evidence/analysis tools
cmake/     dependency fetching + arm64 loader build helpers
docs/      master plan, format write-ups, phase plans
.build/    gitignored: deps, cmake build tree, everything build-related
```

**Renderer** — one compute invocation per output pixel. The internal frame
is 320×200 scaled 1x–4x (an option, Phase 5) into a (320·S)×(200·S) target;
walls are two compute passes (S workgroups of 320, split to bound shared
memory at 4x), sprites a sorted-batch billboard pass, all sampling 16-color
tiles expanded to RGBA with the shipped palette. The internal frame is
letterboxed (8:5) onto the swapchain with linear, nearest, or integer
filtering. Distance fog is per-pixel and CPU-table-driven.

**Audio** — the YM3812 (OPL2) chip is emulated in code and driven by the
game's IMF data (music + FM sound effects); digitized SFX and the music are
mixed positionally by a pure-C++ mixer (resample → pan → sum). The audio
output device lands with Phase 5.

**Design rules** — deterministic by construction: no randomness, no time
dependence, fixed tick order (which is what makes the pixel harness
possible). Pure logic lives in SDL/Vulkan-free libraries so every rule is
testable headless.

## How it's verified

- **Pixel-perfect render harness**: a Node driver pumps the reference JS
  engine on the real Steam data; `w3d_app --script … --screenshot` renders
  the same ticks; a Python differ compares the full 320×200 frame. Result:
  0 mismatching pixels per frame.
- **Bit-exact audio**: a real track rendered by both the C++ OPL2 and
  uWolf's `opl2.js`, compared sample-for-sample (max diff 0.0).
- **Golden format tests**: every container decodes to end-of-file with
  pinned hashes; anchor pixels are checked against external references.

## References

- [vpoupet/wolfenstein](https://github.com/vpoupet/wolfenstein) — the
  clean-room JS engine used as the **render-semantics reference** (it is
  render-only; UI/HUD semantics come from the original's documented look).
- The original game data (Steam/GOG install) — format authority.
- `docs/PLAN.md` — master plan and phase-by-phase decisions.
- `docs/formats.md` — the pinned data formats (authoritative).
- `docs/phase-5-plan.md` — Phase 5 plan, workstreams, and acceptance
  criteria.

## Game data & licensing

This repository contains **no game assets** and is not a re-release of the
game: it reads `*.WL6` / `*.SOD` data files from your own legitimate
Steam/GOG installation at runtime. A license file for the code has not yet
been added to the repo.
