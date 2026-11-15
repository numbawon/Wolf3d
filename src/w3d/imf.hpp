// w3d/imf.hpp — IMF (Wolf3D OPL register stream) model, parsers, and sequencer.
//
// A music "track" is not a list of notes — it is a stream of register writes
// for the OPL2 chip, in the IMF format: four bytes per packet — register,
// value, and a 16-bit delay measured in ticks of a 700 Hz clock. The
// sequencer walks that list and hands the writes to an Opl2 (w3d/opl2.hpp),
// which is the chip. Tracks loop (the original never stops a floor's music).
//
// An FM *sound effect* (pickup, locked door, key …) is a different, smaller
// format: an instrument (two operators' parameters), an octave, and a list of
// note bytes played one per 140 Hz tick (a zero byte means key-off). It borrows
// voice 0 of the music chip, exactly as the original did.
//
// Layouts are documented and verified in docs/formats.md ("AUDIOHED / AUDIOT").
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "w3d/error.hpp"
#include "w3d/opl2.hpp"

namespace w3d {
namespace imf {

constexpr int kImfRate = 700;   // music sequencer tick clock (Hz)
constexpr int kSfxRate = 140;   // FM effect note-step clock (Hz)
constexpr int kTagSize = 88;    // Muse tag after each music track
constexpr double kFxTail = 0.12;  // seconds of release after the last note

// A music track: the register stream plus its (optional) name.
struct MusicTrack {
    std::string name;
    std::vector<std::uint8_t> data;  // [reg][val][u16 delay] * (len % 4 == 0)
};

// An FM sound effect.
struct FmEffect {
    std::string name;
    // Instrument (modulator = m*, carrier = c*).
    std::uint8_t mChar = 0, cChar = 0, mScale = 0, cScale = 0;
    std::uint8_t mAttack = 0, cAttack = 0, mSus = 0, cSus = 0;
    std::uint8_t mWave = 0, cWave = 0;
    std::uint8_t block = 0;          // octave
    std::vector<std::uint8_t> notes;  // one byte per 140 Hz tick
    // The original treats an effect with no sustain on either operator as a
    // bad instrument and stays silent.
    [[nodiscard]] bool badInstrument() const { return (mSus | cSus) == 0; }
};

// Parse a music track chunk: [u16 dlen][dlen x IMF bytes][88-byte tag].
// The tag's 16-byte title (bytes 2..18) becomes the name.
[[nodiscard]] Result<MusicTrack> parseTrack(std::span<const std::byte> chunk);

// Parse an FM effect chunk:
// [u32 note_len][u16 priority][16 x u8 instrument][u8 block][notes][name].
[[nodiscard]] Result<FmEffect> parseFx(std::span<const std::byte> chunk);

// Render out.size() native-rate (Opl2::kRate Hz) mono samples of `track`,
// driving `chip` (which is reset first). The track loops.
void renderTrack(Opl2& chip, const MusicTrack& track, std::span<float> out);

// Render the whole FM effect at native rate: the note stream (140 Hz) plus a
// release tail, silent tail trimmed. Empty when the instrument is bad or there
// are no notes.
[[nodiscard]] std::vector<float> renderFx(const FmEffect& fx);

// Locate the first "!ID!!ID!" (8-byte) marker in the raw AUDIOT file and walk
// the contiguous track region after it, returning every track. Returns an
// error when the marker is absent or the region does not close at EOF.
[[nodiscard]] Result<std::vector<MusicTrack>> parseMusicRegion(
    std::span<const std::byte> audiot);

}  // namespace imf
}  // namespace w3d
