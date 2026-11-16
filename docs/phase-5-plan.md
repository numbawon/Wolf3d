# Phase 5 Plan — UI/HUD, Menus, Settings, Input

Status: IN PROGRESS on branch phase-5-ui-hud.
- WS1 (VGAGRAPH decode) DONE fba657c (docs de2582c, 2026-11-15) — see
  docs/formats.md: Huffman 255 nodes / 4-plane 4bpp / kVgaGamepal, golden
  tests; GOG deviations pinned (TILE8 2240 B, extern screens).
- WS2 (scene skeleton + menus) DONE 13d1bc5 + audit a93a1e2 (2026-11-15) —
  src/app/scene.* + menu.* + font8x16, full-res menu pass (window-sized
  image, 1:1 blit, per Decision 1), w3d_scene_tests.
- NEXT: WS3 — windowing + internal resolution (section 4.1 / workstream 3).
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
- Window: SDL_WINDOW_FULLSCREEN_DESKTOP for borderless; exclusive
  SDL_WINDOW_FULLSCREEN only if it works cleanly on Windows ARM64, else
  windowed + borderless only (risk, see 7). Aspect always 8:5, centered,
  black bars. F11 cycles windowed -> borderless -> (exclusive) -> windowed.
- HiDPI: treat SDL window size as logical pixels.
- Performance: 4x internal = 16x the pixel count of 1x. Benchmark at
  workstream 3; keep the option regardless, note the cost if it matters.

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
   KICKOFF NOTES (WS3, for a fresh context window):
   - Current state: the game pass renders a FIXED 320x200 (kInternalW/H)
     R8G8B8A8 storage image and blits it VK_FILTER_LINEAR into the
     640x400 swapchain (render_frame in src/render/renderer.cpp). The
     menu pass (WS2) is already window-sized and 1:1 — leave it alone.
   - Change: parameterize the game image to (320*S, 200*S), S in 1..4,
     rebuild offscreen target on change (no window change); blit stage
     gains a filter (linear/nearest/integer) + 8:5 letterbox (black
     bars, centered); F11 cycles windowed -> borderless -> (exclusive if
     it works on ARM64, else stop at borderless).
   - INVARIANT: at S=1 the internal frame must stay pixel-identical to
     the Phase 3/4 baseline — the --script/--one-frame screenshot
     harness (scratch w3d_ref workflow) must keep diffing 0/64000; 1x is
     also the acceptance default.
   - Bench FIRST: 4x = 16x the pixel count of 1x on the RTX Spark N1X —
     time a 4x internal render before building the whole options path.
   - Known driver traps still apply (skill): UBO 256-byte alignment,
     % 256u byte extraction, float imageStore, no bool UBO members;
     the fog table and per-column tails are already resolution-agnostic
     (pure per-pixel math).
   - The window today is 8:5 by coincidence — WS3 makes it a rule
     (resizable, letterboxed, HiDPI logical px).
4. Config: JSON writer/parser, load/save, live-apply hooks; options
   screen sections.
5. Audio device: miniaudio in .build/deps; pull callback; music loop;
   SFX voices; volume/device settings wired.
6. Gamepad: mapping table, deadzone, rumble, disconnect handling; menu
   navigation via gamepad (verify the Steam Deck path if hardware
   available).
7. HUD: face/counters/keys/level name/message line drawn on-frame (scaled
   by res_scale); sim event queue feeding messages + SFX.
8. Screens: level intro, score, death, episode end, high scores (persist).
9. Rebind UI: action list, capture state, conflict rules, reset, persist.
10. Polish within scope: F12 screenshot (stb_image_write), show-FPS,
    difficulty semantics, episode select names.
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
- 4x internal (1280x800) = 16x the compute of 1x on ARM64 — benchmark at
  workstream 3; option stays either way, note cost if it matters.
- Exclusive fullscreen on Windows ARM64 may be flaky; fallback =
  windowed + borderless only (document if so).
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
