// w3d game/level.hpp — level model on the Phase 1 map layer.
//
// Mirrors the reference engine's setupLevel (game.js): a 64x64 grid plus a
// "plane2" blocking array, a thing list, the player start, and the score
// totals. plane0/plane1 are copied (mutable: pushwalls rewrite them, exactly
// like the reference's setMap0/setMap1).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "game/actors.hpp"
#include "w3d/maps.hpp"

namespace w3d::game {

// One map thing (reference Thing/Enemy).
struct Actor {
    std::uint16_t x = 0;  // cell
    std::uint16_t y = 0;
    const ActorDef* def = nullptr;
    std::uint8_t direction = 0;  // N/E/S/W (orientable enemies)
    // state
    bool alive = true;    // enemies: false once shot
    bool removed = false; // collected things leave the list
    // animation (reference Thing.update: 8 ticks per frame)
    bool animating = false;
    std::uint16_t anim_sprites[8]{};
    std::uint8_t anim_frames = 0;
    std::uint8_t anim_frame = 0;
    std::uint8_t anim_timer = 0;
    bool anim_loop = false;
    // current rendered sprite (main sprite or animation frame)
    std::uint16_t sprite = 0;

    [[nodiscard]] double wx() const { return x + 0.5; }  // reference centers
    [[nodiscard]] double wy() const { return y + 0.5; }  // things in cells
};

struct Level {
    std::string name;
    std::array<std::uint16_t, w3d::kMapPlaneCells> plane0{};
    std::array<std::uint16_t, w3d::kMapPlaneCells> plane1{};
    std::array<bool, w3d::kMapPlaneCells> blocking{};  // reference plane2
    std::vector<Actor> actors;
    // player start (cell center + facing)
    bool has_start = false;
    double px = 0.5, py = 0.5, pdx = 0.0, pdy = -1.0;
    // score totals, filled while building (reference setupLevel)
    int total_kills = 0;
    int total_treasures = 0;
    int total_secrets = 0;

    [[nodiscard]] std::uint16_t cell0(int x, int y) const {
        if (x < 0 || y < 0 || x >= w3d::kMapSide || y >= w3d::kMapSide)
            return 0;  // out of range reads as wall (reference: undefined)
        return plane0[static_cast<std::size_t>(y) * w3d::kMapSide + x];
    }
    [[nodiscard]] std::uint16_t cell1(int x, int y) const {
        if (x < 0 || y < 0 || x >= w3d::kMapSide || y >= w3d::kMapSide)
            return 0;
        return plane1[static_cast<std::size_t>(y) * w3d::kMapSide + x];
    }
    [[nodiscard]] bool is_blocking(int x, int y) const {
        if (x < 0 || y < 0 || x >= w3d::kMapSide || y >= w3d::kMapSide)
            return true;  // map bounds act as walls
        return blocking[static_cast<std::size_t>(y) * w3d::kMapSide + x];
    }
    // Out-of-range writes are silently dropped (the reference writes into a
    // JS array, where out-of-range stores are harmless no-ops).
    void set_cell0(int x, int y, std::uint16_t v) {
        if (x < 0 || y < 0 || x >= w3d::kMapSide || y >= w3d::kMapSide)
            return;
        plane0[static_cast<std::size_t>(y) * w3d::kMapSide + x] = v;
    }
    void set_cell1(int x, int y, std::uint16_t v) {
        if (x < 0 || y < 0 || x >= w3d::kMapSide || y >= w3d::kMapSide)
            return;
        plane1[static_cast<std::size_t>(y) * w3d::kMapSide + x] = v;
    }

    // Build from a decoded map (reference setupLevel loop, cell order y,x).
    static Level from_map(const w3d::Map& m);
};

} // namespace w3d::game
