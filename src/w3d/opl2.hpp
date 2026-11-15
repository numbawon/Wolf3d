// w3d/opl2.hpp — a Yamaha YM3812 (OPL2) FM synthesiser: Wolf3D's music chip.
//
// AUDIOT holds no notes — it holds *register writes* for the sound card's
// OPL2 chip (the IMF format, see w3d/imf.hpp). To hear the music you have to
// be the chip. This class is that chip, modelled from the documented behaviour
// of the part (log-domain envelope generator, quarter-sine + exp2 tables), and
// cross-validated sample-for-sample against the clean-room reference in
// dibdot/uWolf (js/opl2.js) — see docs/formats.md "OPL2 register map".
//
// Structure: 9 channels x 2 operators (Wolf3D's packed register map exposes a
// modulator and a carrier per channel). Each operator is a phase accumulator
// feeding a quarter-sine lookup in the log domain; attenuation (envelope +
// total level + key scaling + tremolo) is added in that log domain and
// converted back with an exp table — exactly how the silicon avoids
// multipliers. Operator 1 modulates operator 2's phase (FM) or is added to it
// (AM), per the channel's connect bit.
//
// Native output rate is 49716 Hz (3579545 / 72), the chip's real sample rate.
// Callers resample (see audio/mixer.hpp).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace w3d {

class Opl2 {
public:
    static constexpr int kRate = 49716;  // 3579545 / 72 — the chip's own rate
    static constexpr int kChannels = 9;

    Opl2() {
        buildOpMap();
        reset();
    }

    // Clear all registers, envelopes and phase state.
    void reset();

    // Write one Wolf3D packed register. Unmapped addresses are ignored (the
    // packed map only exposes 2 operators per channel plus the channel/global
    // registers).
    void write(std::uint8_t reg, std::uint8_t val);

    // Produce one mono sample in [-1, 1], advancing clocks and envelopes.
    float sample();

    // Produce n mono samples into out (out.size() must be >= n).
    void generate(std::span<float> out) {
        for (std::size_t i = 0; i < out.size(); ++i)
            out[i] = sample();
    }

private:
    enum class State : int { Off = 0, Attack = 1, Decay = 2, Sustain = 3, Release = 4 };

    struct Op {
        bool am = false, vib = false, egt = false, ksr = false;
        std::uint8_t mult = 0;        // 0..15 (MULT2 indexed)
        std::uint8_t kslBits = 0;     // 0..3
        std::uint8_t tl = 0;          // 0..63
        std::uint8_t ar = 0, dr = 0;  // 0..15
        std::uint8_t sl = 0, rr = 0;  // 0..15
        std::uint8_t wave = 0;        // 0..3
        std::uint32_t phase = 0;      // 10 fractional bits below the 10-bit index
        std::uint32_t inc = 0;
        std::int32_t env = 511;       // attenuation: 0 = loud, 511 = silent
        State state = State::Off;
        std::int32_t ksl = 0;         // key-scaling attenuation (1/256 octave)
        std::int32_t out = 0, prev = 0;
    };

    struct Ch {
        std::uint32_t fnum = 0;   // 10 bits
        std::uint8_t block = 0;   // 0..7
        bool kon = false;
        std::uint8_t fb = 0;      // 0..7 feedback
        std::uint8_t cnt = 0;     // 0 = FM, 1 = additive
        Op ops[2];
    };

    void buildOpMap();
    void keyOn(Ch& ch);
    void keyOff(Ch& ch);
    void retune(Ch* ch);
    std::uint8_t effRate(const Ch& ch, const Op& op, std::uint8_t r) const;
    void advanceEnv(Ch& ch, Op& op);
    // Waveform lookup: returns the quarter-sine index and neg flag, or
    // ok=false when this part of the wave is removed (waves 1 and 3).
    bool waveSample(const Op& op, std::uint32_t phaseIdx, std::uint32_t& idx, bool& neg) const;
    std::int32_t opSample(Ch& ch, Op& op, std::int32_t mod);

    // offset (reg & 0x1F) -> channel / operator index, or -1 when unmapped.
    std::array<std::int8_t, 32> opCh_{};
    std::array<std::int8_t, 32> opIdx_{};

    Ch ch_[9]{};
    std::array<std::uint8_t, 256> reg_{};
    std::uint32_t egCounter_ = 0;
    std::uint32_t tremPos_ = 0;
    std::int32_t tremolo_ = 0;
    std::uint32_t vibPos_ = 0;
    std::uint8_t dam_ = 0;   // tremolo depth (0xBD bit 7)
    std::uint8_t dvb_ = 0;   // vibrato depth (0xBD bit 6)
    std::uint8_t waveSelect_ = 0;  // 0x01 bit 5 (stored; not gating, per reference)
};

}  // namespace w3d
