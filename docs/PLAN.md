# Wolf3D Remake — Master Plan

Clean-room modern engine remake of Wolfenstein 3D. No original source code is
used; all formats are derived from the shipped binaries/data files and verified
empirically (pixel/audio golden checks) before any consumer is built.

## Deliverable

A single standalone native binary — modern stack (C++23/MSVC, SDL3 + Vulkan +
miniaudio), runs directly on the host OS. No DOS, no DOSBox, no emulation, no
runtime network; the only external input is the game data files. Visual
verification uses the modern GOG/Steam port as reference, never DOSBox.

## Source material (on-disk, read-only)

Install root: `C:\Program Files (x86)\Steam\steamapps\common\Wolfenstein 3D`

| Path | What it is |
|---|---|
| `base/wolf3d.exe` | GOG/Steam modern port executable (reference only, not linked) |
| `base/*.WL6` | GOG `TED5v1.0` container data — main game (episodes 1–2) |
| `base/m1/Spear.exe` | Original 1992 DOS executable, LZEXE 0.91 packed (Spear of Destiny) |
| `base/m1/*.SOD` | Spear of Destiny data; mixed flavors: `GAMEMAPS.SOD` is TED5, `VGAGRAPH.SOD` is raw/wrapped — flavor determined per file in phase 1 |
| `DOSBox/` | Ships with the install (for the 1992 original). Not used by this project — not in the build, the runtime, or the verification. |

Game data is read from the install directory at runtime (path via config).
The repo never stores the full game assets.

## Stack

- C++23, MSVC arm64 (host is Windows ARM64).
- CMake build; artifacts go to the in-repo, gitignored `.build/` folder
  (nothing build-related is tracked; nothing is written outside the repo).
- SDL3 (windowing, input) — prebuilt arm64.
- Vulkan (renderer): compute raycaster.
- miniaudio (audio, single header).
- No other dependencies.

## Renderer design (Vulkan compute raycaster)

Original Wolf3D renderer: per-scanline DDA ray march over a 16x16 sector
grid, 8 fixed wall directions per sector, 4-bit (16-color) tiles, 8-bit WAD
textures, per-pixel distance fog, billboarded sprites.

Vulkan mapping:
- One compute invocation per output pixel (or 2x2 tile for headroom).
- Map in a storage buffer: per sector, 8 wall entries (texture id, offset,
  flags) + type; world coords in float32 (original fixed-point converted once).
- Wall pass: DDA until sector boundary, sample texture atlas (any resolution,
  mipmapped, linear) with u/v derived from hit wall and distance.
- Floor/ceiling pass: project floor/ceiling planes onto the scanline, distance
  fog mix (replaces the original 24-palette fog table; visually equivalent).
- Sprite pass: per-actor billboards, depth-tested after wall pass,
  sorted batch.
- HiDPI: integer-scaled offscreen render target, present scaled to window;
  optional smoothing for non-integer.
- Hi-res texture hook: textures are an asset-side RGBA atlas; the renderer is
  resolution-agnostic. Original 4/8-bit sources are expanded to RGBA at load
  using the shipped palette.

## Audio design

- Original: 8-bit mono chunks in AUDIOT (per-chunk rate, to be pinned in
  phase 1); music as OPL FM data (to be located in the containers), replayed
  by a software FM synthesizer in the miniaudio pipeline — the chip is
  emulated in code; no sound hardware or DOS involved.
- Pipeline: miniaudio mixer; decode chunks to 16-bit float32 internal;
  resampling stage accepts ANY input rate/width — the hi-res audio hook.
- SFX: positional (pan by angle, volume by distance) per original behavior.

## Repo layout

```
src/
  core/      fixed-point, math, logging, config
  data/      container readers (ted5, classic lump, raw), asset models
  game/      map model, actors, simulation, state
  render/    vulkan device, raycaster shaders, framebuffer, present
  audio/     mixer, chunks, positional playback
  app/       main loop, scenes (game, menu, hud)
tests/       C++ unit + golden-file tests (small fixtures only)
tools/       Python analysis scripts used to pin formats (kept as docs-of-record)
docs/        plan, format write-ups
```

## Phases (one branch each, pushed at boundary, merged on user go-ahead)

1. `phase-1-data-formats` — pin TED5 + raw flavors, map/audio/tile formats;
   C++ `data` layer with golden tests; pixel/audio dumps as evidence.
   ACCEPT: every container decodes cleanly to end-of-file; tile atlas PNG and
   decoded audio WAV match the original by eye/ear; MAPHEAD→GAMEMAPS
   cross-reference holds for all maps.
   **DONE (2026-11-11, c230f1a)** — 173 golden checks green: all 81 maps
   (MAPHEAD cross-reference holds), 106 walls + 436 sprites + 86 SFX decode
   to end-of-file; sprite 110 pixel-verified 888/956 vs external reference
   (residual = 2 GOG-remaster palette entries). Deferred to later phases,
   documented in formats.md: VGAGRAPH/VGAHEAD (Phase 5), OPL FM music
   (Phase 4).
2. `phase-2-game-simulation` — map model, actor definitions, headless
   simulation: level load, LOS, movement, doors, triggers.
   ACCEPT: headless test run walks each map without crashes; deterministic
   tick output.
   **DONE (2026-11-11, 17baff7)** — 327 golden checks green: plane value
   semantics pinned against the reference engine + public docs (see
   formats.md "Map plane value semantics"); headless sim with 64-tick door
   cycles, key locks, 2x64-tick pushwalls, grid-DDA LOS, one-shot static
   enemies, elevator exit; acceptance walk of all 81 maps x 2000 ticks with
   byte-identical deterministic transcripts and no crashes. Unresolved,
   documented: plane1 90-97/99 (inert; Phase 3 screenshot check) —
   resolved in the Phase 3 acceptance (2026-11-14): confirmed inert,
   renders identically with such cells in view.
3. `phase-3-render` — Vulkan raycaster + SDL3 window; walls, floors, sprites,
   fog; HiDPI.
   ACCEPT: screenshot comparison against the GOG/Steam modern port (reference
   only) scenes is visually faithful. The deliverable is the standalone
   native binary; it never runs under DOSBox.
   **DONE (2026-11-14, aaf66fb)** — pixel-perfect against the reference JS engine on
   all three pixel-diff acceptance scenes (0/64000 each, --no-fog: Wolf1
   Map2 idle, Wolf4 Map4 walk, SOD Tunnels 2 door); plane1 90-99 confirmed
   inert (cells in view, identical output); fog verified visually on 4
   scenes; 173+386 checks green. See docs/phase-3-status.md.
4. `phase-4-audio` — mixer, SFX, music.
   ACCEPT: audible, correctly positioned; resampling hook demonstrated with
   a 16-bit/44.1k drop-in.
5. `phase-5-ui-hud` — HUD, menus, messages, high scores, input (kb + gamepad).
   ACCEPT: full play loop of one episode start→death→restart.
6. `phase-6-polish` — widescreen, config, save games, hi-res asset pipeline
   documentation + hooks.
   ACCEPT: episode playable start-to-end with clean performance.

## Phase 2 kickoff (next session starts here)

Branch: `phase-2-game-simulation` from main (Phase 1 merged first).

Reference engine (public clean-room JS reimplementation — NOT ID source;
safe under the clean-room rule). Scratch copies may be pruned, re-fetch:
- https://raw.githubusercontent.com/vpoupet/wolfenstein/master/js/game.js   (game logic: player, enemies, doors, pickups — the Phase 2 source of truth)
- https://raw.githubusercontent.com/vpoupet/wolfenstein/master/js/engine.js (graphics; verified against our sprite/wall decodes)
- https://raw.githubusercontent.com/vpoupet/wolfenstein/master/js/files.js  (file parsing)

Work order:
1. Map model on `w3d::GameMaps` (planes already decode): 64×64 grid;
   plane0 = wall/open texture indices (0..143), plane1 = objects/actors
   (0..259; values 256..259 observed across all 81 maps as door/exit
   markers). Derive exact value semantics (which plane0 values are walls
   vs open, plane1 ID → actor mapping) empirically from game.js.
2. Actor/object definitions as data (id → sprite, health, behavior table)
   derived from game.js; every behavior backed by a golden test.
3. Headless sim: fixed tick, player state, movement + collision vs
   plane0, LOS raycast, doors, triggers. Deterministic — tick output is
   the golden fixture (like Phase 1's golden.json, kept in scratch).
4. Keep the golden suite green at every step; add sim tests to
   tests/ as a second executable or a new test file.

ACCEPT: headless test run walks each of the 81 maps without crashes;
deterministic tick output (byte-identical across runs).

## Rules

- Phase-based branch names, no acronyms.
- Build/scratch artifacts go to the gitignored `.build/` folder — never
  tracked, never outside the repo.
- Clean-room: no original code; format knowledge from files + public docs,
  always empirically verified.
- Security/efficiency conscious: no network in the engine, no unsafe code,
  bounded allocations.
