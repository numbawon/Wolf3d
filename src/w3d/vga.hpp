// w3d/vga.hpp — VGAGRAPH HUD/menu graphics (Huffman + 4-plane 4-bit deplane).
//
// Layout (docs/formats.md):
//   VGAHEAD: (N+1) x u24 (3-byte LE) byte offsets into VGAGRAPH; the last
//            entry is the EOF sentinel (= file size), 0xFFFFFF = sparse chunk.
//   VGADICT: 255 huffman nodes x 4 bytes ([bit0 u16, bit1 u16] LE);
//            value < 256 emits that byte, >= 256 descends to node value-256;
//            root = node 254; stream bits are read LSB-first per byte.
//   chunk:   [u32 expanded_size][huffman stream]  (the single TILE8 chunk has
//            no u32; its size is implicit — the GOG files carry 2240 bytes)
//   chunk 0: pictable, n x (w u16, h u16); entry i describes pic chunk i+3
//   chunks 1,2: fonts [u16 height][256 x u16 loc][256 x u8 width]
//               [glyph pool: width x height bytes per glyph, 0 = transparent]
//   pics:    16-color 4-bit, plane-major: 4 blocks of h x (w/4) bytes;
//            byte (plane p, row y, byte cx), bit k = plane k of pixel
//            (4cx+k, y); color = kVgaGamepal[b0 + 16*b1 + 32*b2 + 64*b3].
//
// kVgaGamepal is the id 256-entry DAC palette (6-bit RGB scaled to 8-bit),
// distinct from the VSWAP texture palette in palette.hpp.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <utility>
#include <vector>

#include "w3d/error.hpp"
#include "w3d/vga_palette.inc"

namespace w3d {

// 0xAABBGGRR storage, like kGamePalette.
[[nodiscard]] constexpr std::uint32_t vga_gamepal_packed(std::uint8_t idx) noexcept {
    return kVgaGamepal[idx];
}

struct VgaGlyph {
    std::uint8_t width = 0;   // 0 = empty glyph
    std::vector<std::uint8_t> px;  // row-major, width x font height, 0 = transparent
};

struct VgaFont {
    std::uint8_t height = 0;
    std::array<VgaGlyph, 256> glyphs{};
    [[nodiscard]] const VgaGlyph& glyph(std::uint8_t ascii) const {
        return glyphs[ascii];
    }
};

struct VgaPic {
    std::uint16_t w = 0;
    std::uint16_t h = 0;
    std::vector<std::uint8_t> rgba;  // w*h x RGBA, A = 255
    [[nodiscard]] const std::uint8_t* at(std::size_t x, std::size_t y) const {
        return &rgba[(static_cast<std::size_t>(y) * w + x) * 4];
    }
};

struct VgaGraph {
    // Indexed by pic index i (IGRAB chunk = i + 3): base 0..131, SOD 0..146.
    std::vector<VgaPic> pics;
    VgaFont smallFont;  // chunk 1, height 10 (message line)
    VgaFont bigFont;    // chunk 2, height 13 (menus/options)
    std::vector<std::uint8_t> tiles;  // TILE8 expanded bytes (GOG: 2240 = 35 x 8x8)
    std::size_t numChunks = 0;        // chunk count excluding the EOF sentinel

    // Loads and decodes everything (pic total: ~1.4 MB base / ~4.4 MB SOD).
    static Result<VgaGraph> load(const std::filesystem::path& vgahead,
                                 const std::filesystem::path& vgagraph,
                                 const std::filesystem::path& vgadict);
};

} // namespace w3d
