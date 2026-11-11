// w3d/maps.hpp — map geometry (GAMEMAPS + MAPHEAD).
//
// File layout (docs/formats.md):
//   [8B "TED5v1.0"][3B unknown]
//   per map: [plane0][plane1][10B const blob][42B header]
//   planes: Carmack -> RLEW -> 64x64 u16
// MAPHEAD: [u16 0xABCD][100 x u32 header offsets, 0 = empty]
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "w3d/error.hpp"

namespace w3d {

constexpr std::uint16_t kMapSide = 64;
inline constexpr std::size_t kMapPlaneCells = kMapSide * kMapSide;

struct Map {
    std::string name;
    std::uint16_t width = kMapSide;
    std::uint16_t height = kMapSide;
    // plane 0: walls / walkable ("rooms"); plane 1: objects / actors.
    std::array<std::uint16_t, kMapPlaneCells> plane0{};
    std::array<std::uint16_t, kMapPlaneCells> plane1{};

    [[nodiscard]] std::uint16_t cell0(std::uint16_t x, std::uint16_t y) const {
        return plane0[static_cast<std::size_t>(y) * kMapSide + x];
    }
    [[nodiscard]] std::uint16_t cell1(std::uint16_t x, std::uint16_t y) const {
        return plane1[static_cast<std::size_t>(y) * kMapSide + x];
    }
};

struct GameMaps {
    std::vector<Map> maps;

    static Result<GameMaps> load(const std::filesystem::path& gamemaps_path,
                                 const std::filesystem::path& maphead_path);
};

} // namespace w3d
