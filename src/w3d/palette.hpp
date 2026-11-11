// w3d/palette.hpp — the 256-entry game texture palette (VSWAP walls & sprites).
// Storage layout of each u32: 0xAABBGGRR (R = low byte).
#pragma once

#include <cstdint>

namespace w3d {

#include "w3d/gamepalette.inc"

struct Rgb {
    std::uint8_t r = 0, g = 0, b = 0;
    [[nodiscard]] bool operator==(const Rgb&) const noexcept = default;
};

[[nodiscard]] constexpr Rgb palette_rgb(std::uint8_t idx) noexcept {
    const auto v = kGamePalette[idx];
    return {static_cast<std::uint8_t>(v & 0xFF),
            static_cast<std::uint8_t>((v >> 8) & 0xFF),
            static_cast<std::uint8_t>((v >> 16) & 0xFF)};
}

// 0xAARRGGBB for upload to GPU samplers.
[[nodiscard]] constexpr std::uint32_t palette_rgba_packed(std::uint8_t idx) noexcept {
    const auto v = kGamePalette[idx];
    return 0xFFu << 24 | (v & 0xFFu) << 16 | ((v >> 8) & 0xFFu) << 8 | ((v >> 16) & 0xFFu);
}

} // namespace w3d
