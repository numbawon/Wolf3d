# Phase 3 (Vulkan render) — status (2026-11-13, M4 DONE)

Branch: `phase-3-render`. Repo: C:\Users\NumbaWon\git\Wolf3d.
Build: from git-bash `cmd /c ".build\build_arm64.bat"` → expect `BUILD_OK`.
Run one frame: `cd .build/cmake && PATH="/c/Users/NumbaWon/git/Wolf3d/.build/deps/vulkan_loader:$PATH" ./w3d_app.exe --one-frame --screenshot <abs.bmp path>`
(`--no-fog` disables the fog; the ARM64 loader on PATH is mandatory —
System32 vulkan-1.dll is x64 and forces Prism.
BMP format: 56-byte header, bottom-up RGB, stride 960, 320x200.)

## Milestones
- M1 toolchain (SDL3 ARM64 + Vulkan headers/loader ARM64 + glslang): DONE.
- M2 test pattern (compute fills image, blit, screenshot readback, pixel-verified): DONE (commit 5e5a728).
- M3 real raycast frame: DONE (2026-11-12). Walls/floor/ceiling + all 30 sprites +
  pistol, PIXEL-PERFECT: full-frame diff against an independent Python reference
  of the reference-engine semantics = 0/64000 (scratch/m3_final.bmp).
- M4 per-pixel distance fog: DONE (2026-11-13). PLAN says "visually
  equivalent" to the original's 24-palette fog table (no original fog data
  is recoverable: not in VSWAP.wl6, not in the JS reference). Verified
  against a full-frame CPU model — see below.

## M4 fog (design + root causes)
Design (PLAN: "visually equivalent" to the original's 24-palette fog table):
- 24 levels, `L = min(23, floor(dist / 1.25))`, blend `u = L/23` toward a
  brown ramp (140,92,40) → (24,14,6). kFogStep = 1.25 cells/level → full
  fog at 30 cells.
- Walls/floor/ceiling (walls.comp): wall-union rows use the column's ray
  distance t; other rows use the plane distance d = (t/h) * |row - 100|
  (similar triangles — the wall at distance t spans h px around horizon
  row 100). Occluded columns (h == 0) are left un-fogged.
- Sprites (sprites.comp): fogged at the sprite's own forward distance;
  the weapon's distance is 0 → level 0 → unmodified, matching the
  original's un-fogged weapon.
Driver facts proven this phase (in-shader value probes; the fog switch
started life as a bool UBO and migrated twice):
1. UBO descriptor offsets must be 256-byte aligned on this driver —
   unaligned offsets are silently clamped DOWN: a fog UBO at 33796 read
   the 33792 count slot (probe saw step where on should be), 33808 read
   zeros, 34048 (256*133) read correctly.
2. `bool` UBO members are misplaced (probe read fog_on at offset 0 and
   fog_step at offset 4). The on-flag is now a 1.0/0.0 double tested
   `< 0.5`.
Consequence: the fog never uses a UBO. The whole fog input set is
computed on the CPU in render_frame (same formulas as the shader's DDA:
top = 100 - h, fbot = floor(top + 2h), slope = t/h guarded by h > 0) and
rides in the frame buffer's columns SSBO tail (invstep @10240, on
@10248, slope[320] @10256, wlo[320] @12816, whi[320] @14096 — ends at
15376 < 16384) and the sprites SSBO tail (@26624). invstep = 1/kFogStep
so the shader only multiplies. The Counts UBO (@33792, 256-aligned) is
the only UBO left and is unchanged from M3.
Verification (scratch m4_verify2.py, full-frame CPU model, 2026-11-13
final captures): 61777/64000 px match the column model within 1.5/channel
(the GPU blends in float32, the model in float64; 11078 are bit-exact).
All 2223 remaining px are sprite pixels, and every one matches the blend
at its OWN sprite's forward distance (dist * 1/1.25, same double
arithmetic as the shader) — 0 unexplained. The weapon (dist 0) is
unmodified by design, matching the original's un-fogged weapon.
`--no-fog` frame: byte-identical scene data between runs (fog slots
differ by design). NOTE: back-to-back one-frame app runs can hit a
transient GPU-teardown race (observed once: second run rendered a black
frame with an all-zero frame buffer); put a few seconds between captures.
ANALYSIS PITFALL (cost one full debugging cycle): the frame buffer's
column region is 320 x 32B STRIDED records (t@0, h@8, tx@16, tex@24) and
the sprite region 256 x 40B (x0@0, y0@8, s@16, dist@24, scale@32,
sprite@36). Parsing them as flat double arrays (np.frombuffer with a
count, or `arr[3::4]`) silently yields a phantom "column permutation"
and phantom fog-level errors — every column looked like it read another
column's data. Parse with the record stride (or struct unpack).

## M3 root causes (all fixed)
1. DRIVER BUG: this ARM64 Vulkan driver (NVIDIA RTX Spark N1X, Blackwell)
   miscompiles `v & 0xFF` into a nonzero-test — yields 0 or 255 per byte.
   Every byte extraction / low-byte index uses `% 256u` and `/ 256u` instead.
   Proven by differential renders (m3_dbg10..m3_dbg11).
2. DRIVER BUG: `uvec4` imageStore on R8G8B8A8_UNORM storage images is
   quantized per component to {0, 255}. The float `vec4` path with
   `(v + 0.5) / 255.0` is correct (m3_dbg12: orange painted as yellow).
   Both shaders now write float vec4; keep it. (M2's clear.comp already used
   float — that is why M2 verified clean.)
3. APP BUG: `Renderer::load_atlases` built the sprite atlas with pointer
   arithmetic at a 4096-byte stride, but `w3d::Sprite` is 4096 + 2*uint16 =
   4100 bytes — every sprite tile after slot 0 read the wrong bytes
   (garbage colors + the spurious black pixels; the wall atlas is a plain
   `vector<array<uint8_t,4096>>` and was unaffected). Fixed by passing
   `sprites.data()` / `walls.data()` and dereferencing `(*entries[t])[i]`.
   The wall pass itself was never the problem: it uses 200 uint32 locals
   (~250 KB > the GPU's 48 KB maxComputeSharedMemorySize, but the driver
   handles it fine — output verified pixel-perfect, no redesign needed).

## Verification machinery (kept in the code, used for M4 too)
- GPU mirror buffer: `dbg_buf_` copies `frame_buf_`→host-visible each frame
  (`VK_BUFFER_USAGE_TRANSFER_SRC_BIT`), dumped to
  scratch/dbg_framebuf.bin on readback. Proves upload correctness (M3).
  Layout: columns 0 (320x32B: t@0, h@8, tx@16, tex@24, pad@28), fog tail
  10240 (invstep@0, on@8, slope[320]@16, wlo[320]@2576, whi[320]@3856),
  sprites 16384 (256x40B: x0@0, y0@8, s@16, dist@24, scale@32, sprite@36),
  sprite fog tail 26624 (invstep, on), palette 32768 (256x4B RGBA),
  count 33792. STRIDED — never parse flat (see M4 pitfall).
- One-shot GPU info print in the renderer constructor (name + compute
  limits). GPU: RTX Spark N1X, sharedMemMax=48 KB, wgSizeMax=(1024,1024,64).
- Python pixel-perfect reference (scratch, rebuildable from VSWAP.WL6 +
  dbg_framebuf.bin + scratch/atlas_ref.bin): exact float64 reference
  accumulation for walls, reference drawThings/drawSprite semantics for
  sprites (far-to-near last-write per sprite, nearest sprite wins, per-column
  z test). atlas_ref.bin = concatenated 64x64 idx images of the 30 sprites
  (probe_atlas.cpp in scratch regenerates it).
- Reference semantics authority: reference JS engine (scratch ref_engine.js).
  Closed-form row mapping verified against the reference's float accumulation:
  200k random trials, 0 mismatches. GPU doubles are exact IEEE float64.

## State of the files (M4)
- src/render/shaders/walls.comp — driver-workaround comment block at top;
  fog via SSBO tail arrays (slope/wlo/whi) + fog_mixed(); CPU-computed
  fog inputs. No debug markers left.
- src/render/shaders/sprites.comp — fog at the sprite's own forward
  distance via the sprites SSBO tail (invstep, on doubles).
- src/render/renderer.cpp — render_frame computes the fog inputs on the
  CPU (wlo/whi/slope per column) and uploads them in the frame-buffer
  tail; FogUpload{double invstep; double on}; frame buffer now 34064 B.
- src/render/renderer.hpp — render_frame takes fog_on; buffer layout
  comment.
- src/render/frame.hpp — kFogStep (1.25 cells/level).
- src/main.cpp — --no-fog flag.
- CMakeLists.txt — stamp-based shader copy (KEEP — real fix from the
  M2→M3 stale-shader bug).
- src/main.cpp — /"base" data path (KEEP).
- clear.comp deleted (M2 shader, replaced).

## Tests
173 (w3d_tests) + 327 (w3d_sim_tests) = 500 checks, 0 failures (post-M4).

## Next
M5 per PLAN.md (mouse/keyboard input + live 60 fps), then phase end:
commit phase-3-render, merge --no-ff into main ("Merge phase-3-...: ..."),
push origin main + phase branch, update docs/PLAN.md DONE note with the
commit hash.
