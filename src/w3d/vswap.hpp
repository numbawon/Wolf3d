// w3d/vswap.hpp — walls, sprites and sound chunks (VSWAP).
//
// Layout (docs/formats.md):
//   [u16 n][u16 first_sprite][u16 first_sound][n x u32 offsets][n x u16 lengths]
//   chunks: 0..first_sprite-1 walls (4096B, 8-bit palette idx, column-major)
//           first_sprite..first_sound-1 sprites
//           first_sound..n-1 sounds (PCM audio)
// Sprite chunk: [u16 first_col][u16 last_col][ncols x u16 coloffs]
//               [pixel pool][posts]  (posts end exactly at chunk end)
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <utility>
#include <vector>

#include "w3d/error.hpp"

namespace w3d {

struct Sprite {
    // Row-major 64x64 palette indices; 255 = transparent (no post coverage).
    std::array<std::uint8_t, 64 * 64> idx{};
    std::uint16_t first_col = 0;
    std::uint16_t last_col = 63;

    [[nodiscard]] std::uint8_t at(std::uint16_t x, std::uint16_t y) const {
        return idx[static_cast<std::size_t>(y) * 64 + x];
    }
};

struct Vswap {
    // Row-major 64x64 (converted from the file's column-major order).
    std::vector<std::array<std::uint8_t, 64 * 64>> walls;
    std::vector<Sprite> sprites;
    // (offset, length) of the raw sound chunks.
    std::vector<std::pair<std::size_t, std::size_t>> sounds;

    static Result<Vswap> load(const std::filesystem::path& path);
};

} // namespace w3d
