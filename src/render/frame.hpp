// w3d render/frame.hpp — CPU-side per-tick scene data for the Vulkan passes.
//
// The reference engine (vpoupet/wolfenstein, js/engine.js) renders per screen
// column: a grid DDA finds the first blocking surface (wall / door /
// pushwall), then a 64-texel vertical DDA fills the wall strip. This module
// ports the DDA and the sprite setup to the CPU in double precision (the
// reference's float64 arithmetic, same order of operations), producing:
//   - one record per column (ray distance t, wall half-height h, texture u,
//     texture index) that the wall compute pass expands to pixels;
//   - one record per renderable thing (screen rect, scale, resolved sprite
//     index, distance) that the sprite compute pass tests per pixel.
// Everything is deterministic: no time, no randomness, fixed order.
#pragma once

#include <array>
#include <cstdint>

#include "game/sim.hpp"

namespace w3d::render {

// Internal resolution (reference setZoom(2): 320x200).
inline constexpr int kColumns = 320;
inline constexpr int kRows = 200;
inline constexpr int kMaxSprites = 256;

// Reference camera: fov = 1 (camera plane magnitude per unit forward),
// wallHeight = pixelWidth / (2 * fov) = 160 at 320x200.
inline constexpr double kFov = 1.0;
inline constexpr double kWallHeight = kColumns / (2.0 * kFov);

// M4 fog: distance (in cells, the reference's zIndex units) per fog level.
// 24 levels -> full fog at 24 * kFogStep cells. Visually equivalent to the
// original's 24-palette fog table (PLAN); the brown ramp constants live in
// the shaders (walls.comp / sprites.comp, kept identical).
inline constexpr double kFogStep = 1.25;

// Per-column DDA result (reference drawWalls locals).
struct ColumnRecord {
    double t = 1.0;    // ray distance in cells (reference zIndex[i])
    double h = 0.0;    // wall half-height in px = kWallHeight / (2*t)
    double tx = 0.0;   // texture u on the wall tile, 0 <= tx < 1
    std::uint32_t tex = 0;  // wall/door texture index (VSWAP wall bank)
};

// Per-thing sprite setup (reference drawThings / drawSprite).
struct SpriteRecord {
    double x0 = 0.0;   // screen column of the sprite's left edge
    double y0 = 0.0;   // screen row of the sprite's top edge
    double s = 0.0;    // texel -> px factor = th / 64 (scan bound only)
    double dist = 0.0;  // forward distance (z test vs column t)
    double th = 0.0;    // sprite height in px (reference `height`): the
                        // reference places texel column c's block at
                        // x0 + ~~(c * height / 64) — left-to-right, so the
                        // shader must multiply by th and divide by 64
    std::uint32_t scale = 1;  // reference: ceil(th / 64)
    std::uint32_t sprite = 0; // resolved VSWAP sprite index
};

// Player pose at a point in the tick cycle.
struct Pose {
    double x = 0.5, y = 0.5, dx = 0.0, dy = -1.0;
};

// Per-tick frame state handed to the renderer.
struct Frame {
    // Grid (copied from the sim level; pushwalls rewrite plane0/plane1 and
    // the blocking flag, so these move between ticks).
    std::array<std::uint16_t, 4096> plane0{};
    std::array<std::uint16_t, 4096> plane1{};
    std::array<std::uint8_t, 4096> blocking{};
    // Per-cell door openness for door cells (90..101): 0 = closed, 1 = open.
    // Reference: timer progress while moving, else the plane2 flag.
    std::array<float, 4096> door_shift{};
    // Per-cell pushwall timer state (only m1 == 98 cells with an active
    // timer are nonzero): shift = t/64, axis = (dx, dy).
    std::array<float, 4096> wall_shift{};
    std::array<float, 4096> wall_dx{};
    std::array<float, 4096> wall_dy{};

    std::array<ColumnRecord, kColumns> columns{};
    std::array<SpriteRecord, kMaxSprites> sprites{};
    int sprite_count = 0;

    double px = 0.5, py = 0.5, pdx = 0.0, pdy = -1.0;
    // Pre-tick player pose (the pose the tick started from). The reference
    // computes each thing's relative coordinates (Thing.update) BEFORE
    // player.update() moves the player, so sprite screen geometry, culling
    // and the z-test distance use this pose, while the wall DDA uses the
    // post-tick pose above. (drawThings recomputes the orientable facing
    // angle from the live player, so that one uses px/py.)
    double sp_px = 0.5, sp_py = 0.5, sp_pdx = 0.0, sp_pdy = -1.0;
    std::uint32_t weapon_sprite = 421;  // 421 pistol, 422..425 firing

    // Flash event detected this frame by diffing actor state:
    // 0 = none, 1 = yellow (treasure), 2 = white (other pickup), 3 = red
    // (shot kill). The renderer owns the Flash state machine.
    int flash_event = 0;

    // Actor snapshot for the next frame's diff (index-keyed into the sim's
    // actor list, which is fixed per level).
    struct ActorSnap {
        std::uint8_t removed = 0;
        std::uint8_t alive = 1;
        std::uint16_t sprite = 0;
    };
    std::array<ActorSnap, kMaxSprites> actor_snap{};
    int actor_snap_count = 0;
};

// One reference-engine flash effect (game.js Flash + flashPalette).
struct Flash {
    bool active = false;
    double red = 0.0, green = 0.0, blue = 0.0;
    int timer = 0;
};

// Advance the flash one frame. `event` replaces an in-flight flash exactly
// like the reference does (a new Flash object resets the timer).
void update_flash(Flash& fl, int event);
// Effective palette for the current flash state (reference flashPalette,
// from the base gamePalette).
void effective_palette(const Flash& fl,
                       std::array<std::uint32_t, 256>& out);

// Fill `f` from one simulation state (call after each tick). `prev_pose` is
// the player pose BEFORE this tick's movement (the pose the tick started
// from; on the first frame the spawn pose): the reference computes thing
// relative coordinates before moving the player, so sprite screen geometry
// uses it while the wall DDA uses the post-tick pose. `prev_snap` is
// the previous frame's actor snapshot (all-present on the first frame); it
// drives the flash event detection. `prev_weapon_cooldown` is the previous
// frame's sim cooldown counter (resolves the last 424 frame of the firing
// animation, see weapon_sprite). `weapon_shift` is the VSWAP bank offset of
// the pistol frames (0 for the base bank; -15 for SOD, whose re-packed
// VSWAP holds the byte-identical pistol at 406..410).
void build_frame(Frame& f, const game::Simulation& sim, const Pose& prev_pose,
                 const std::array<Frame::ActorSnap, kMaxSprites>& prev_snap,
                 int prev_snap_count, int prev_weapon_cooldown, int weapon_shift);

// Weapon sprite index from the sim's cooldown counter and the previous
// frame's counter (reference: 421 at rest; while firing, 421 for six ticks,
// then 422/423/424 for six ticks each — the Animation over [422,423,424,425]
// terminates at the last index, so 425 is never shown).
[[nodiscard]] std::uint32_t weapon_sprite(int cooldown_ticks, int prev_cooldown);

}  // namespace w3d::render
