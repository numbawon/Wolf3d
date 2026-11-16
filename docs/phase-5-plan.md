# Phase 5 Plan — UI/HUD, Menus, Settings, Input

Status: IN PROGRESS on branch phase-5-ui-hud.
- WS1 (VGAGRAPH decode) DONE fba657c (docs de2582c, 2026-11-15) — see
  docs/formats.md: Huffman 255 nodes / 4-plane 4bpp / kVgaGamepal, golden
  tests; GOG deviations pinned (TILE8 2240 B, extern screens).
- WS2 (scene skeleton + menus) DONE 13d1bc5 + audit a93a1e2 (2026-11-15) —
  src/app/scene.* + menu.* + font8x16, full-res menu pass (window-sized
  image, 1:1 blit, per Decision 1), w3d_scene_tests.
- WS3 (windowing + internal resolution) DONE f508bdd (2026-11-16) — res_scale
  1..4 (--res-scale, clamped), 8:5 letterbox blit with linear/nearest/
  integer filters, F11 windowed->borderless->exclusive cycle (exclusive
  verified working on the N1X; latched degradation if a driver lacks it),
  walls split into two compute passes (walls1/walls2), resize-safe
  swapchain. S=1 stays 0/64000 in the pixel-diff harness; 6 suites green
  (2224 checks) incl. new w3d_scale_tests (1314) + w3d_window_tests (7).
  Bench (N1X, scripted 4000 frames, last second): S=1 ~1600 fps,
  S=2 ~735, S=3 ~517, S=4 ~323 — 4x is ~5x the cost of 1x, sublinear in
  pixel count; all well above 60.
- WS4 (config + options/pause scenes) DONE 2026-11-16 (this workstream's
  commit) — src/app/config.* (pure C++ v1 JSON schema, strict parser +
  tolerant mapping, deterministic writer, load/save, CLI overrides),
  Scene::Pause + Scene::Options (4 tabs; live-apply of video/sensitivity,
  stored-only audio + difficulty with PENDING markers), save-on-change +
  clean-exit save for interactive runs only. 7 suites green (2464 checks)
  incl. new w3d_config_tests (97) + rewritten w3d_scene_tests (201).
  Evidence: scratch/w3d_ref ws4_title/ws4_pause/ws4_opt0..3.bmp (by eye);
  first-boot interactive run wrote the default config; harness runs write
  nothing.
- NEXT: WS5 — audio device (miniaudio): pull callback, music loop, SFX
  voices, wire the stored audio settings (volumes/music_on/device).
Decisions confirmed by user 2026-11-14: 1, 4, 5 as
recommended; 2 REVISED (configurable internal resolution 1x-4x instead of
320x200-strict); 3 REVISED (keyboard rebind UI in P5 + standard gamepad
mapping with Steam Deck support).

Scope in one line: everything between "rendered 3D frame" and "audible,
playable, configurable game" — HUD, messages, score/death/level-intro
screens, title/pause/options menus, persistent settings (incl. key
rebinds), audio output device, gamepad, internal resolution 1x-4x, modern
windowing.

## 1. Decisions (confirmed 2026-11-14)

1. UI style: HYBRID. In-game surfaces (HUD bar, message line, level-intro,
   score/death/episode-end screens) are authentic: original layout and
   decoded original font, drawn in the frame's internal resolution.
   Title/pause/options menus render full-resolution with a clean modern
   layout. All menus navigable by keyboard, mouse, and gamepad.
2. Resolution/windowing: internal render scale is a video setting: 1x
   (320x200, DEFAULT — the authentic look and the pixel-perfect acceptance
   baseline), 2x (640x400), 3x (960x600), 4x (1280x800). The raycaster is
   per-pixel compute, so internal upscaling costs ALU only, no algorithm
   change; HUD/2D art nearest-upscales with the frame so the pixel-art
   look is preserved at every scale. The texture atlas is already
   resolution-agnostic (mipmapped RGBA) — this is the enabler for future
   hi-res texture packs (packs themselves are P6). Window: freely
   resizable, 8:5 letterbox, selectable blit filter (linear default /
   nearest / integer), F11 cycles windowed + borderless + fullscreen,
   HiDPI-aware (logical pixels).
3. Controller + rebinds: SDL3 Gamepad API with the built-in standard
   mapping — Steam Deck works out of the box (its controller is in SDL's
   gamepad database; Steam Input routes through it); rumble on
   fire/hit/damage/door/pickup. Keyboard rebind UI IN P5: unified action
   list with the built-in default keymap (the reference keymap),
   click-to-rebind with key capture. Gamepad stays on standard mapping in
   P5 (per-button gamepad rebinds only if cheap, else P6). Keyboard +
   mouse + gamepad coexist.
4. Settings persistence: versioned JSON config in
   %APPDATA%\Wolf3DRemake\config.json, written on change (and on exit),
   loaded at startup; missing/invalid file -> defaults + rewrite. P6 keeps
   save games; P5 owns settings.
5. Audio settings: master + music + SFX volumes (0-100), music on/off,
   output-device picker (miniaudio device enumeration). Device defaults to
   system default.

Out of scope (stays P6): save games, hi-res texture packs (P5's internal
upscaling is the enabler), widescreen-native rendering, per-button
gamepad rebinds, exclusive-fullscreen fallbacks beyond what works.

## 2. Technical baseline (main @ 657e239, post-Phase 4)

- Render: internal 320x200 Vulkan compute raycaster (walls/floors/sprites/
  fog, pixel-perfect vs reference JS engine), linear-upscaled into a
  640x400 resizable SDL3 window (src/render/). No fullscreen, no scale-mode
  option, no DPI handling, no letterbox logic (window is 8:5 today by
  coincidence only).
- Input: src/game/input.hpp LiveInput — keyboard (reference keymap: WASD +
  arrows, a/e turn, q/d strafe, f/Ctrl shoot, space activate) + relative
  mouse X turn, mouse button 1 fire while captured. No gamepad, no
  rebinds, no sensitivity setting.
- Audio: full software chain, NO output device yet (game is silent):
  src/w3d/opl2.cpp (YM3812, sample-exact vs uWolf), imf.cpp (700 Hz
  sequencer + FM effects), audio.cpp (AudioBank: 86 PCM + 87 FM + 27 music
  base / 80+81+24 SOD), src/audio/mixer.cpp (float32 stereo, cosine pan,
  distance attenuation, resampling any rate/width -> 48 kHz). miniaudio is
  the planned device layer (PLAN.md stack) and is not in .build/deps yet.
- Sim: src/game (sim/actors/level) — movement, doors, shooting, level exit;
  weapon sprite drawn at distance 0. No messages, scoring, or event
  output.
- Data: w3d::GameMaps, GamePalette, AudioBank load from
  C:\Program Files (x86)\Steam\steamapps\common\Wolfenstein 3D\base (+ m1
  for SOD). VGAGRAPH/VGAHEAD/VGADICT NOT decoded — the P5 format work.
  formats.md open item #1: VGAGRAPH = [u32 528][12 zero bytes][275770
  high-entropy payload]; VGAHEAD = 450 bytes = 75 x (u16,u16,u16), third
  field monotonically increasing (likely a tile index table).
- Tests: 659 checks green (173 format, 386 sim, 100 audio).
- Reference engine (scratch ref_engine.js) is RENDER-ONLY: no HUD/menu/
  score logic to borrow. HUD semantics/layout must come from the
  original's documented look (screenshots, public docs) — verified by eye
  via the --screenshot harness, not pixel-diffed against the reference.

## 3. UI inventory (original surfaces the play loop needs)

In-game (authentic, drawn in the internal-resolution frame; layout
authored in 320x200 space, HUD = bottom 50 rows; 3D view = rows 0..149):
- Player face: 10 faces x 10 health stages (100 tiles in VGAGRAPH),
  selected by current health; face blinks on damage.
- Counters: health %, ammo, gold, keys carried (silver/gold icons).
- Weapon in hand (already on screen as 3D sprite at dist 0 — keep).
- Level name (top-left corner).
- Message line (bottom band): pickups, locked door, out of ammo, level
  start/end notes. Timed display, new message replaces old.

Screens (authentic style, internal resolution):
- Per-level intro: "LEVEL N" + one story line (text lives in the original
  EXE, not in data files — transcribe from public screenshots/docs).
- Level-end score: time, gold, secrets, score.
- Death screen ("you are dead" + score).
- Episode-end screen (total score).
- High-score table: name entry + persisted list (original table size to
  confirm from public docs; assume 50).

Menus (modern full-res per Decision 1):
- Title: logo/name, "press any key", main menu.
- Main menu: new game (episode 1-4 select with episode names), high
  scores, options, quit.
- Pause (Esc, also on gamepad Start): resume, options, abort level.
- Options: sections Video / Audio / Controls / Gameplay (see config
  schema below).

## 4. Design by area

### 4.1 Windowing and resolution (Decision 2)
- Offscreen render target: (320*S, 200*S) where S = res_scale in
  {1,2,3,4}; compute dispatch matches. Changing S rebuilds the offscreen
  target — no window change.
- Blit pass: offscreen -> window with chosen filter: linear =
  VK_FILTER_LINEAR, nearest = VK_FILTER_NEAREST, integer = nearest at
  integer window scale. One blit pass (2D blit compute or small blit
  shader; no new dependencies).
- HUD/2D art: authored in 320x200 coordinates; the HUD layer multiplies
  by S (one constant). Tiles nearest-neighbor upscale by S — pixel-art
  look preserved at all scales.
- Window: borderless = SDL_SetWindowFullscreen(win, true) with a NULL
  stored fullscreen mode; exclusive = SDL_SetWindowFullscreenMode(win,
  mode) while already fullscreen (async; SyncWindow after). Note the
  SDL3 trap verified on this platform: SDL_SetWindowFullscreenMode(win,
  nullptr) alone is a silent no-op. Exclusive works on the N1X (verified);
  if a driver lacks it the cycle degrades to windowed + borderless (a
  latch, logged once — see workstream 3). Aspect always 8:5, centered,
  black bars. F11 cycles windowed -> borderless -> (exclusive) -> windowed.
- HiDPI: treat SDL window size as logical pixels.
- Performance: 4x internal = 16x the pixel count of 1x. Benchmarked at
  workstream 3: ~5x the time of 1x on the N1X (~323 fps), sublinear in
  pixel count — the option is kept, no cost caveat needed.

### 4.2 Audio device (Decision 5)
- Add miniaudio (single header) to .build/deps; new target w3d_audio_dev
  (or fold into w3d_audio): creates a 48 kHz float32 stereo device with a
  ring-buffer pull callback.
- Music: IMF sequencer advances on the 700 Hz tick; render OPL2 into the
  buffer in the pull thread (OPL2 is fast enough single-threaded at 48k —
  verify; else a small ring of pre-rendered OPL2 frames). Music loops.
- SFX: sim event queue -> Mixer voices (PCM or FM effects), positional
  pan from actor angle relative to player.
- Settings: master/music/sfx gain applied in the mixer; music on/off;
  device picker lists miniaudio devices (id + name), change = recreate
  device.
- Level->music-track mapping: original mapping lives in the EXE;
  transcribe from public docs (open item, verify a couple of levels by
  ear).

### 4.3 Gamepad (Decision 3)
SDL3 standard mapping (SDL_AddGamepad / SDL_EVENT_GAMEPAD_*,
SDL_GamePadGetAxisForType etc.), deadzone 0.15 fixed:
- Left stick: Y = forward/back, X = strafe (twin-stick modern layout).
- Right stick X: turn.
- D-pad: alt movement (up/down move, left/right turn) — for keyboard
  purists and low-stick-preference.
- A = fire, B = activate/use.
- Left bumper = menu previous/back, right bumper = next/forward (in
  menus); in-game bumpers = no action (no weapon switching in original).
- Start = open/close pause menu. Back = back in menus.
- Rumble: fire (small blip), enemy hit (small), player damage (strong),
  door open/close (small), key/item pickup (blip).
- Steam Deck: standard mapping covers it (Xbox-compatible controller, in
  SDL's gamepad DB; Steam Input passes through). Rumble via SDL. No
  Deck-specific code expected — verify on hardware if available. Touch
  controls: out of scope.
- Gamepad + keyboard + mouse coexist; menus navigate with any of the
  three. Joystick disconnect handled (fall back to kb/mouse; no
  crash, no stuck keys).
- Gamepad rebinds: NOT in P5 (standard mapping); keyboard rebinds are —
  see 4.4.

### 4.4 Settings, config, rebinds (Decisions 3+4)
File: %APPDATA%\Wolf3DRemake\config.json. Hand-rolled minimal JSON
writer + tolerant parser (no new dependency); version field; unknown keys
preserved on round-trip if trivially possible, else dropped (document).
Schema draft (v1):
  {
    "version": 1,
    "video":    { "res_scale": 1, "scale_filter": "linear",
                  "fullscreen_mode": "windowed", "show_fps": false },
    "audio":    { "master": 80, "music": 80, "sfx": 80,
                  "music_on": true, "device_id": "default" },
    "controls": { "mouse_sensitivity": 1.0,
                  "bindings": {
                    "forward": "Up", "back": "Down",
                    "turn_left": "Left", "turn_right": "Right",
                    "strafe_left": "Q", "strafe_right": "D",
                    "fire": "F", "activate": "Space",
                    "pause": "Escape", "screenshot": "F12",
                    "fullscreen": "F11" } },
    "gameplay": { "difficulty": "normal", "fog": true }
  }
- res_scale: 1|2|3|4 (internal resolution multiplier; default 1 =
  authentic 320x200). Changing applies at the next frame (offscreen
  rebuild).
- scale_filter: linear | nearest | integer (window blit).
- fullscreen_mode: windowed | borderless | exclusive
- difficulty: easy | normal | hard (original semantics: difficulty scales
  enemy damage; pin exact multipliers from public docs)
- bindings: optional section. Absent = built-in defaults (the reference
  keymap — the exact table in src/game/input.hpp). Written out in full
  (effective table) when the user changes anything.
- Written on change (debounced) + on clean exit. Corrupt file -> defaults,
  log warning, rewrite.
- Data dir for high scores: same folder (highscores.json, separate file).

Rebind UI (Controls section of options, full-res modern):
- Action list (the 11 actions above) showing current key; select an
  action -> "press a key" capture state -> next key-down binds it.
- Conflicts: two actions on one key -> reject with a notice (keep it
  simple); Escape always cancels capture / closes menus; modifier-only
  presses (Shift/Ctrl/Alt alone) rejected.
- "Reset to defaults" button restores the built-in table.
- Persisted to config immediately on bind.
- Default-table semantics: the built-in reference keymap has multiple
  keys per action (W and Up both move forward; A and Left both turn, etc).
  Rebinds layer ON TOP of the built-in table: a binding sets the action's
  primary key (overriding it), built-in secondary keys stay active unless
  the binding is cleared. Document the behavior in the UI as "default +
  overrides". Keep it simple; do not attempt a full multi-binding editor
  in P5.

### 4.5 Menus (Decision 1)
- Scene/state machine in a new src/app/ (matching PLAN.md repo layout):
    Title -> NewGame (episode select) -> LevelIntro -> Game <-> Pause
    Game -> LevelExit -> Score -> (next level | EpisodeEnd)
    Game -> Death -> DeathScore -> (retry | menu)
- Full-res menu rendering: 2D draw pass over the window (rects, text with
  a small built-in font — decision: embed a compact public-domain bitmap
  font for menus, e.g. 8x16, so no font dependency; do NOT use the
  VGAGRAPH font at window res since it is 320x200-scale pixel art).
  Menu background: dimmed last frame or flat dark; keep it simple.
- Keyboard (up/down/left/right + Enter/Esc), mouse (hover + click),
  gamepad (sticks/D-pad + A/B) all drive the same menu state.
- Options screen is one screen, sections as tabs (Video/Audio/Controls/
  Gameplay), live-apply on change (volume changes audible immediately,
  res_scale/scale filter apply immediately, device change recreates
  device, rebinds persist immediately).
- Show-FPS toggle (video section) + F12 = PNG screenshot to
  %APPDATA%\Wolf3DRemake\screenshots\ (SDL3 has no PNG exporter; use the
  stb_image_write single header — build-time only, acceptable under the
  dependency rule; flag at kickoff if read strictly).

### 4.6 HUD rendering (authentic, internal resolution)
- HUD assets decoded from VGAGRAPH: font glyphs (2 sizes if present),
  10x10 face grid, key icons, HUD frame/border art. Decode step must be
  verified against known original screenshots (this is the acceptance
  evidence for the format work).
- HUD draw = a 2D pass on the frame after the 3D passes (index into
  decoded tiles, blit with palette or pre-expanded RGBA tiles; original
  font is 4-bit? verify — if 4-bit, expand with the 16-color tile palette
  like walls).
- Layout coordinates pinned against screenshots (face box bottom-right,
  counters bottom-left, level name top-left, message line bottom band);
  authored in 320x200 space, scaled by res_scale (see 4.1).
- Message line: font render, timed (replace on new message; clear after a
  fixed display time in the original's behavior).

### 4.7 Sim events (new output of the sim)
- Sim emits a per-tick event list (owned by the app): enum/struct events:
  Sfx(name, x, z), Message(text), Rumble(strength, ms), LevelExited,
  PlayerDied, ScoreTick updates (time, gold, secrets counted by existing
  sim state).
- SFX name -> AudioBank index: PCM SFX are index-based in the data (names
  live in the original EXE); build the mapping table in code from the
  publicly documented SFX order and verify a handful by ear against the
  Phase 4 WAV dumps (evidence files exist in scratch/w3d_audio_out). FM
  effects carry inline names in the data.
- Score: time (original counts down/up? pin from docs), gold collected,
  secrets (secret sectors — sim may need a secret-sector flag; check
  plane values), score formula from public docs.

## 5. Workstreams (implementation order)

1. VGAGRAPH/VGAHEAD decode (format work, critical path): Python probes in
   scratch (pattern: 8x8/9x16 font bitmaps, 20x20 face tiles, icon
   bitmaps; VGAHEAD = 75 index triples cross-referenced to payload).
   Verify with tile-sheet PNG dumps against original screenshots. Update
   formats.md; golden tests in tests/test_formats.cpp.
   DONE fba657c (2026-11-15) — formats.md is authoritative; the face
   grid / icon tile-sheets for the WS7 HUD are decoded via the same
   decoder (VgaGraph::load), verify them against screenshots when the
   HUD lands.
2. Scene/state machine skeleton (src/app/): Title -> NewGame -> LevelIntro
   -> Game stub -> back; keyboard + mouse navigation; full-res menu
   render pass with embedded font.
   DONE 13d1bc5 + audit a93a1e2 (2026-11-15) — gamepad navigation still
   owed to WS6; Pause/LevelExit/Score/Death scenes owed to WS7/WS8.
3. Windowing + internal resolution: offscreen target at (320*S, 200*S),
   compute dispatch change, letterbox blit, blit filters, res_scale live
   rebuild, F11 fullscreen modes, resize-safe swapchain handling.
   BENCHMARK 4x on target hardware early (16x pixel count).
   DONE f508bdd (2026-11-16). Implementation summary:
   - frame.cpp/hpp: build_frame takes a scale (1..4, clamped; kMaxScale=4);
     W = 320*S columns, wall height 160*S, pixel space (320*S, 200*S).
     Even-column identity (S=2 col 2i == S=1 col i bit-exact) and sprite
     scaling invariants pinned by tests/test_scale.cpp (w3d_scale_tests,
     1314 checks) — this is why the S=1 pixel-diff baseline stays
     meaningful at every scale.
   - renderer.cpp/hpp: internal image sized (320*S, 200*S) RGBA8; GPU
     buffer layout is scale-INDEPENDENT (variable regions reserved at
     S=4 capacity at fixed 256-byte-aligned offsets — the ARM64 driver
     clamps unaligned UBO offsets down). Walls dispatch = S workgroups of
     320, split into two passes (shaders/walls1.comp + walls2.comp;
     walls.comp deleted) to keep shared memory bounded at 4x. Blit stage:
     8:5 letterbox (black bars via vkCmdClearAttachments + blit), centered,
     filter selectable (linear default / nearest / integer = nearest at
     integer window scale only). Resize recreates the swapchain + staging.
   - window.cpp/hpp: FullscreenMode {Windowed, Borderless, Exclusive};
     F11 = cycle_fullscreen(). VERIFIED SDL3 semantics on this platform
     (probe: scratch w3d_ref/probe_fs.cpp): SDL_SetWindowFullscreenMode
     (win, nullptr) is a silent no-op; the reliable paths are
     SDL_SetWindowFullscreen(win, true) [enters fullscreen with the
     stored mode; NULL = borderless] and SDL_SetWindowFullscreenMode(win,
     mode) called WHILE already fullscreen [switches to exclusive,
     async — SyncWindow after]. The stored mode persists after leaving
     fullscreen, so the cycle clears it before entering borderless. If a
     driver lacks exclusive the cycle latches (exclusive_unavailable_)
     and degrades to windowed<->borderless, logged once.
   - main.cpp: --res-scale S (clamped 1..4), --blit-filter
     linear|nearest|integer, F11 in both event loops (legacy + scene)
     with a console line on mode change.
   - Tests: w3d_window_tests (tests/test_window.cpp) drives
     cycle_fullscreen() against the real display; skips headless. On the
     N1X the full cycle passes: windowed -> borderless -> exclusive ->
     windowed -> borderless (7 checks).
   - Evidence (scratch/w3d_ref): ws3_s2_internal.bmp (640x400 frame),
     ws3_win_s2_linear.bmp / ws3_win_s2_nearest.bmp (800x400 window,
     80px side bars), ws3_win_s4_linear.bmp (960x640 window, 20px top/bot
     bars, 0.75x downscale). All verified by eye + programmatic bar
     measurement.
   - Bench (N1X, --sod level 1, scripted 4000 frames, last 1s window,
     640x400 window, unthrottled): S=1 ~1600 fps (64k px), S=2 ~735
     (256k), S=3 ~517 (576k), S=4 ~323 (1024k). 4x = 16x pixels but only
     ~5x the time of 1x (bandwidth/compute mix); every scale clears 60
     fps by a wide margin, so 4x is free to offer.
   - NOTE: injected keyboard input (SendInput / PostMessage WM_KEYDOWN)
     does NOT produce SDL key events on this box (WM_CLOSE via
     PostMessage does reach SDL — the pump works). F11 e2e verified by
     w3d_window_tests + the 3-line handler; manual keypress confirmed by
     the user 2026-11-16 (also exercises the live resize/swapchain
     rebuild, since borderless resizes the window).
4. Config: JSON writer/parser, load/save, live-apply hooks; options
   screen sections.
   DONE 2026-11-16 (all kickoff decisions as confirmed; see notes below).
   Implementation summary:
   - src/app/config.{hpp,cpp}: pure C++ (no SDL/Vulkan — headless
     testable). v1 schema (video/audio/controls/gameplay; key order
     fixed). Hand-rolled JSON: STRICT parser (RFC 8259 shape; malformed
     -> error + note, never a partial value) + TOLERANT mapping
     (missing key/wrong type -> field default; out-of-range -> clamped;
     enum by name (case-insensitive) or number; unknown keys dropped on
     round-trip). Deterministic writer (2-space indent). load_config /
     save_config take a CALLER-SUPPLIED path (save = <path>.tmp +
     rename, no torn files); default_config_path() =
     %APPDATA%\Wolf3DRemake\config.json. CliOverrides applies --res-scale
     / --blit-filter / --no-fog for the run only and never touches the
     file (harness determinism).
   - scene.{hpp,cpp}: Scene::Pause + Scene::Options. Title is now a
     3-item menu (NEW GAME / OPTIONS / QUIT; the original any-key is
     retired per Decision 1's modern menu). Game: Esc -> Pause. Pause:
     RESUME / OPTIONS / QUIT TO TITLE; Esc resumes. Options: 4 tabs
     (1-4 or tab keys; mouse hover highlights, click selects, click the
     selected row again to cycle), rows cycle with wrap; audio rows +
     difficulty carry "(PENDING)" (stored-only: device = WS5, difficulty
     = WS7). Esc returns to the scene that opened Options (the menu
     restarts at its top item). from_name("options:<tab>") harness hook
     for per-tab menu shots.
   - Live-apply (main.cpp scene loop): on take_config_dirty() diff
     against the applied state — res_scale, scale_filter,
     fullscreen_mode (window.set_fullscreen; driver degradation is
     written back so the screen follows reality), show_fps,
     mouse_sensitivity (LiveInput multiplier, clamped 0.1..10). Audio +
     difficulty apply nothing yet. F11 stays above the machine and
     syncs cfg without triggering a save.
   - window.{hpp,cpp}: public set_fullscreen() = the WS3 cycle logic as
     enter_borderless()/enter_exclusive() (latched exclusive
     degradation). input.{hpp,cpp}: LiveInput::set_sensitivity().
   - Save policy: interactive runs only (no --frames / --scene /
     --menu-shot / --script). Save on every user change + on clean exit
     (first boot rewrites the effective defaults). CLI flags override
     the loaded config for the run, never persisted.
   - Tests: w3d_config_tests (tests/test_config.cpp, 97 checks):
     parser (escapes, surrogates, malformed battery), round-trip,
     tolerant mapping (clamps, name/number enums, wrong types,
     unknown-key drop, version), file I/O (missing/empty/garbage/
     partial/overwrite), CLI precedence. w3d_scene_tests rewritten
     (201 checks): full Title/NewGame/LevelIntro/Game/Pause/Options
     flow, mouse against the shared layout, options value cycling /
     tabs / dirty flag / from_name hooks, MenuDraw counts + centering.
     All 7 suites green: 2464 checks, 0 failures, 0 warnings.
   - Evidence (scratch/w3d_ref): ws4_title.bmp, ws4_pause.bmp,
     ws4_opt0..3.bmp (each tab, by eye: correct values, PENDING on the
     stored-only rows, centering intact). First-boot interactive run:
     created %APPDATA%\Wolf3DRemake\config.json with the defaults on
     clean exit (verified by content). Harness runs (--menu-shot /
     --scene): no config dir created (verified).
   - Invariants held: scripted harness unchanged (legacy loop reads the
     loaded cfg; CLI overrides fold in; render path identical); S=1
     pixel-diff baseline untouched by this workstream; no new runtime
     dependencies; nothing written outside %APPDATA%\Wolf3DRemake.
   KICKOFF NOTES (WS4, superseded by the DONE summary above — kept for
   the decision record):
   - Current state: zero persistence — all video options are CLI flags
     (main.cpp Args: --res-scale, --blit-filter, --no-fog, --win, ...).
     Scene machine (src/app/scene.*) has Title/NewGame/LevelIntro/Game +
     the full-res menu pass (menu.comp; MenuDraw POD static_assert'd
     std430, element 80 B / frame 1320 B). Live-apply plumbing for video
     already exists: renderer.set_res_scale(), set_blit_filter(),
     window.cycle_fullscreen()/fullscreen_mode(). No Options scene, no
     config file, no %APPDATA% writes anywhere yet.
   - Change: new src/app/config.* — pure C++, no SDL (testable headless),
     minimal hand-rolled JSON writer + tolerant parser (no new
     dependency, per plan 4.4). File: %APPDATA%\Wolf3DRemake\config.json
     (env APPDATA; create dir on first write). version field = 1;
     missing/corrupt file -> defaults + one log line + rewrite; unknown
     keys dropped on round-trip (document). Save on change + on clean
     exit (explicit flush at quit, not atexit — console app). CLI flags
     override the config for the current run and NEVER write the file
     (the scripted harness must stay deterministic; bench flags included).
   - Schema v1 (plan 4.4): video {res_scale 1-4, scale_filter
     linear|nearest|integer, fullscreen_mode windowed|borderless|
     exclusive, show_fps bool}; audio {master/music/sfx 0-100, music_on,
     device_id} (STORED ONLY until the WS5 device lands); controls
     {mouse_sensitivity, bindings} (the bindings EDITOR is WS9);
     gameplay {difficulty — applies in WS7 with events/score; fog —
     live, reuses the --no-fog plumbing}.
   - Options scene: Scene::Options in the scene machine (reachable from
     Title + Pause), sections as tabs Video/Audio/Controls/Gameplay,
     live-apply on change: video -> renderer setters + window mode; fog
     -> renderer flag; everything else stored-only with a "pending"
     visual. MenuDraw POD grows — re-run the std430 static_assert and the
     scene centering test after any layout change (audit a93a1e2 trap:
     centered widths derive from strlen).
   - Tests: new w3d_config_tests suite: parse/serialize round-trip,
     corrupt/missing/malformed tolerance, unknown-key drop, default
     rewrite, version field, CLI-override precedence. All existing 2224
     checks stay green.
   - Invariants: scripted harness output unchanged (config must not
     alter the render path when CLI flags are given); S=1 stays 0/64000;
     no new runtime dependencies; nothing written outside
     %APPDATA%\Wolf3DRemake.
   - Traps to carry (skill): menu element len = stored char count;
     SDL3 key names CAPITALIZED; one-shot command buffer for
     constructor-time GPU work; keep the menu pass on SSBOs (UBO
     256-byte alignment if anything new touches a UBO).
   - WS4 kickoff decisions — CONFIRMED by the user 2026-11-16 (all as
     recommended): (a) audio + gameplay sections stored-only in WS4 (the
     device is WS5, difficulty applies WS7); (b) show_fps wired in WS4
     (the per-second fps line exists in both event loops; it now follows
     video.show_fps) and dropped from WS10 polish; (c) Controls tab in
     WS4 = mouse sensitivity only (LIVE, wired into LiveInput), bindings
     omitted until WS9 ships the editor.
5. Audio device: miniaudio in .build/deps; pull callback; music loop;
   SFX voices; volume/device settings wired.
6. Gamepad: mapping table, deadzone, rumble, disconnect handling; menu
   navigation via gamepad (verify the Steam Deck path if hardware
   available).
7. HUD: face/counters/keys/level name/message line drawn on-frame (scaled
   by res_scale); sim event queue feeding messages + SFX.
8. Screens: level intro, score, death, episode end, high scores (persist).
9. Rebind UI: action list, capture state, conflict rules, reset, persist.
10. Polish within scope: F12 screenshot (stb_image_write), difficulty
    semantics, episode select names. (show-FPS moved to WS4 per the
    2026-11-16 kickoff decision.)
11. Tests + acceptance (below). Keep all 659 existing checks green.

Suggested branch: phase-5-ui-hud (naming convention: no acronyms —
"ui-hud" matches the PLAN.md phase name).

## 6. Acceptance criteria (Phase 5)

- Full play loop: title -> new game (episode select) -> level intro ->
  play with correct HUD (face follows health, ammo/health/gold/keys
  correct, messages appear) -> shoot/doors/pickups audible with
  positional SFX and per-level music -> level exit -> score -> next
  level; death -> death screen -> restart works; episode-end screen.
- Menus navigable by keyboard, mouse, AND gamepad; pause menu via Esc
  and Start.
- Internal resolution 1x/2x/3x/4x selectable from options; 1x default and
  pixel-identical to the Phase 3 baseline (pixel-diff harness unchanged);
  2x-4x render cleanly (by eye, plus a screenshot at 2x as evidence).
- Rebind UI: change a binding, use it in-game, survives a restart; fresh
  config = built-in reference keymap; reset-to-defaults works.
- F11 cycles window modes; blit filter (linear/nearest/integer) applied
  from options; window resize keeps 8:5 letterbox.
- Settings survive a restart (config round-trip test); corrupt config ->
  defaults without crash; high scores persist.
- Audio settings audible: volumes, music on/off, device change.
- Tests: new unit suites (scene transitions, config round-trip, scale
  math, gamepad mapping, event queue, rebind conflict rules) + existing
  659 stay green.
- Visual evidence: tile-sheet PNG dumps of decoded VGAGRAPH art matching
  original screenshots; in-game HUD/screenshot comparison by eye (no
  reference engine exists for HUD).

## 7. Risks / open items

- VGAGRAPH layout unknown until probed (formats.md open item #1);
  VGADICT (open item #2) only if 4-bit tiles need the dictionary.
- Original text content (level names/intros, SFX index->name table,
  level->music mapping, score formula, difficulty multipliers, high-score
  table size) lives in the original EXE — transcribe from public
  screenshots/docs; every transcription is a verify-by-play item.
- 4x internal (1280x800) = 16x the pixel count of 1x on ARM64 — RESOLVED
  (WS3 bench): ~323 fps on the N1X, sublinear in pixel count; option
  kept, no cost caveat needed.
- Exclusive fullscreen on Windows ARM64 — RESOLVED (WS3): works on the
  N1X (verified by w3d_window_tests); the latched windowed<->borderless
  degradation covers drivers without it.
- miniaudio on ARM64 Windows: single header, expected to compile clean;
  OPL2 render cost per 48k pull callback — benchmark, add ring-buffer
  pre-render if >~5% CPU.
- Menu font choice: embedded public-domain bitmap font (keep dependency
  story clean); stb_image_write for F12 PNG (SDL3 has no PNG exporter —
  one build-time header; flag at kickoff if the dependency rule is read
  strictly).
- Rebind edge cases: OS-reserved keys, sticky modifiers — rule: any key
  SDL3 reports may bind; modifier-only rejected; document in UI.
- Steam Deck verification only if hardware is available; standard mapping
  is the documented path regardless.
- HiDPI verification only possible if a HiDPI display is attached.

## 8. References

- docs/PLAN.md (phase list, stack, repo layout, rules)
- docs/formats.md (VGAGRAPH/VGAHEAD/VGADICT entries, open items)
- docs/phase-3-status.md (pixel-perfect pipeline, screenshot harness)
- src/render/window.* (SDL3 + Vulkan surface), src/game/input.* (keymap,
  LiveInput — the built-in default binding table), src/audio/mixer.*
  (float32 stereo API), src/w3d/* (OPL2, IMF, AudioBank, maps, palette)
- tools/w3d_audio_dump.cpp + scratch WAV evidence (SFX verification by
  ear)
- Game data: C:\Program Files (x86)\Steam\steamapps\common\Wolfenstein
  3D\base\{VGAGRAPH,WL6;VGAHEAD,WL6;VGADICT,WL6;AUDIOT,WL6;AUDIOHED,WL6}
  (+ base/m1 for SOD)
- wolf3d-remake skill (build: cd .build && cmd /c build_arm64.bat;
  test suites; conventions)
- miniaudio: https://miniaud.io (single header, no deps)
- SDL3 Game API: SDL_AddGamepad / SDL_EVENT_GAMEPAD_* (SDL3 3.4.x);
  SDL's gamepad database includes the Steam Deck controller
