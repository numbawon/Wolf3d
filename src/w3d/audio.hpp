// w3d/audio.hpp — SFX bank (AUDIOHED + AUDIOT, SFX region only).
//
// AUDIOHED: 289 x u32 offsets into AUDIOT (offsets[0] == 0).
// SFX chunk: [u32 nsamples][u16 volume 0..100][nsamples x u8 PCM][u8 flag].
// The entries after the SFX region point into an OPL FM music section
// (undocumented — Phase 4); they are kept as raw (offset, length) pairs.
#pragma once

#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

#include "w3d/error.hpp"

namespace w3d {

struct Sfx {
    std::uint32_t nsamples = 0;
    std::uint16_t volume = 0;  // 0..100
    std::uint8_t flag = 0;
    std::vector<std::uint8_t> samples;  // unsigned 8-bit mono PCM
};

struct AudioBank {
    std::vector<Sfx> sfx;                              // the 86 PCM SFX
    std::vector<std::pair<std::size_t, std::size_t>> raw_chunks;  // all entries
    std::size_t sfx_count = 0;  // index of the first non-SFX entry

    static Result<AudioBank> load(const std::filesystem::path& audiohed_path,
                                  const std::filesystem::path& audiot_path);
};

} // namespace w3d
