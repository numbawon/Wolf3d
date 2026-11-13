# Phase 3 (Vulkan render) — status (2026-11-12, M3 DONE)

Branch: `phase-3-render`. Repo: C:\Users\NumbaWon\git\Wolf3d.
Build: from git-bash `cmd /c ".build\build_arm64.bat"` → expect `BUILD_OK`.
Run one frame: `cd .build/cmake && PATH="/c/Users/NumbaWon/git/Wolf3d/.build/deps/vulkan_loader:$PATH" ./w3d_app.exe --one-frame --screenshot <abs.bmp path>`
(Always the ARM64 loader on PATH — System32 vulkan-1.dll is x64 and forces Prism.
BMP format: 56-byte header, bottom-up RGB, stride 960, 320x200.)

## Milestones
- M1 toolchain (SDL3 ARM64 + Vulkan headers/loader ARM64 + glslang): DONE.
- M2 test pattern (compute fills image, blit, screenshot readback, pixel-verified): DONE (commit 5e5a728).
- M3 real raycast frame: DONE (2026-11-12). Walls/floor/ceiling + all 30 sprites +
  pistol, PIXEL-PERFECT: full-frame diff against an independent Python reference
  of the reference-engine semantics = 0/64000 (scratch/m3_final.bmp). No merge yet
  (standing rule: merge+push at phase end only).

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
  Layout: columns 0 (320x32B), sprites 16384 (30x40B: x0,y0,s,dist,scale,
  sprite as doubles+uint), palette 32768 (256x4B RGBA), count 33792.
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

## State of the files (M3 commit)
- src/render/shaders/walls.comp — driver workarounds + comment block
  (documented at top). No debug markers left.
- src/render/shaders/sprites.comp — inverse post-scan (largest covering
  column, largest covering posted row per column); both scan bounds
  explicit (`lo <= u < lo + scale`, `v >= lor && v < lor + scale`).
  No debug markers left.
- src/render/renderer.cpp — load_atlases stride fix (dereferences the
  actual per-tile pointer array); one-shot GPU properties print; GPU
  mirror buffer + dump (kept, gated to readback path).
- CMakeLists.txt — stamp-based shader copy (KEEP — real fix from the M2→M3
  stale-shader bug).
- src/main.cpp — /"base" data path (KEEP).
- clear.comp deleted (M2 shader, replaced).

## Tests
173 (w3d_tests) + 327 (w3d_sim_tests) = 500 checks, 0 failures (post-M3).

## Next
M4 per PLAN.md, then M5, then phase end: commit phase-3-render, merge
--no-ff into main ("Merge phase-3-...: ..."), push origin main + phase
branch, update docs/PLAN.md DONE note with the commit hash.
