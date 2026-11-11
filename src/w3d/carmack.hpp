// w3d/carmack.hpp — Carmack far/near-pointer decoder + RLEW word decoder.
//
// Carmack stream: [u16 total][pairs...]
//   (x, y) with y == 0xA7:  near pointer  [u8 z]  → copy x times the 2-byte word
//                            found (2*z) bytes back from the end of output
//   (x, y) with y == 0xA8:  far pointer   [u16 w] → copy x times the word at
//                            output offset 2*w, incrementing w by 1 per copy
//   (x, y) with x == 0, y in {0xA7,0xA8}: [u8 z]  → literal copy of (z, y)
//   otherwise: literal pair (x, y)
//
// RLEW stream (the Carmack output for map planes): [u16 total][u16 words...]
//   word == 0xABCD: [u16 count][u16 value] → count copies of the value
//   otherwise: the word verbatim.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "w3d/error.hpp"

namespace w3d {

[[nodiscard]] Result<std::vector<std::byte>> carmack_decode(std::span<const std::byte> in);
[[nodiscard]] Result<std::vector<std::byte>> rlew_decode(std::span<const std::byte> in,
                                                         std::uint16_t tag = 0xABCD);
[[nodiscard]] Result<std::vector<std::byte>> plane_decode(std::span<const std::byte> compressed,
                                                          std::uint16_t expect_bytes);

} // namespace w3d
