// w3d/audio.hpp — the full audio bank: AUDIOHED + AUDIOT.
//
// AUDIOT holds three kinds of sound (docs/formats.md "AUDIOHED / AUDIOT"):
//   * PCM SFX      — 8-bit mono digitised effects (the first chunks).
//   * FM effects   — OPL2 sound effects (pickup, door, key …) as an instrument
//                    + a note stream, one note per 140 Hz tick.
//   * music tracks — OPL2 IMF register streams (27 base / 24 SOD), looping.
//
// The GOG files rearranged the original DOS layout, so the regions are found by
// chunk *shape*, not by trusting the table indices: the PCM region is the
// leading run of PCM-shaped chunks; the FM effects are the FM-shaped chunks
// after it; the music is the contiguous region after the "!ID!!ID!" marker.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "w3d/error.hpp"
#include "w3d/imf.hpp"

namespace w3d {

// A digitised PCM sound effect.
struct Sfx {
    std::uint32_t nsamples = 0;
    std::uint16_t volume = 0;  // 0..100 — the effect's base gain
    std::uint8_t flag = 0;
    std::vector<std::uint8_t> samples;  // unsigned 8-bit mono PCM
};

struct AudioBank {
    // PCM SFX, in file order. sfx[i] is chunk i for i < sfx_count.
    std::vector<Sfx> sfx;
    // FM effects, indexed by *chunk number*: fm[chunk] is the effect for that
    // chunk, or std::nullopt when the slot is not an FM effect. This lets the
    // game address an effect by its original chunk id.
    std::vector<std::optional<imf::FmEffect>> fm;
    // Music tracks, in file order (base 27 / SOD 24).
    std::vector<imf::MusicTrack> music;

    std::vector<std::pair<std::size_t, std::size_t>> raw_chunks;  // (start,len) x N
    std::size_t sfx_count = 0;  // index of the first non-PCM chunk
    std::size_t fm_count = 0;   // number of valid FM effects

    static Result<AudioBank> load(const std::filesystem::path& audiohed_path,
                                  const std::filesystem::path& audiot_path);
};

}  // namespace w3d
