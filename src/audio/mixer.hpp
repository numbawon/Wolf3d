// audio/mixer.hpp — the audio mixer: resampling + positional mixing.
//
// This is the "hi-res audio hook" from the plan: a resampling stage that
// accepts ANY input rate/width (8-bit or 16-bit PCM at any rate, or float32),
// resamples it to the mixer's output rate, and mixes it with a positional
// gain (pan by angle, volume by distance). The OPL2 music (native 49716 Hz)
// and the FM effects go through the same resampling path.
//
// The mixer is pure C++ (no device dependency) so it is fully testable; the
// miniaudio device that plays the mixed samples lives in the app. Output is
// stereo so positional panning works.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "w3d/audio.hpp"
#include "w3d/imf.hpp"
#include "w3d/opl2.hpp"

namespace w3d {
namespace audio {

// Decode raw interleaved-free mono PCM into float32 in [-1, 1].
// bits = 8 (unsigned, centre 128) or 16 (little-endian signed).
[[nodiscard]] std::vector<float> decodePcm(const std::uint8_t* data, std::size_t n,
                                           int bits);

// A resampling voice: a decoded mono buffer at `inRate`, advanced one output
// frame at a time with linear interpolation to the mixer's rate, and a
// positional gain applied.
class ResampleVoice {
public:
    void load(std::vector<float> src, double inRate, float pan, float gain,
              double outRate);
    // Advance one output frame (raw resampled sample, no gain). Returns 0 once
    // the buffer is exhausted.
    [[nodiscard]] float next();
    [[nodiscard]] bool done() const noexcept { return done_; }
    [[nodiscard]] float pan() const noexcept { return pan_; }
    [[nodiscard]] float gain() const noexcept { return gain_; }

private:
    std::vector<float> src_;
    double inRate_ = 44100.0;
    double outRate_ = 48000.0;
    double pos_ = 0.0;  // fractional source position
    float pan_ = 0.0;
    float gain_ = 1.0f;
    bool done_ = false;
};

// The mixer: OPL2 music + positional SFX voices (PCM or FM effect), resampled
// to a fixed output rate and mixed to stereo.
class Mixer {
public:
    explicit Mixer(int outRate = 48000) : outRate_(outRate) {}

    int outRate() const noexcept { return outRate_; }
    void setMusicGain(float g) noexcept { musicGain_ = g; }
    void setSfxGain(float g) noexcept { sfxGain_ = g; }

    // Start (or switch) the music track. Passing null stops the music. The
    // music chip is reset so a switch never leaves a note hanging.
    void setMusic(const imf::MusicTrack* track);
    bool hasMusic() const noexcept { return music_ != nullptr; }

    // Queue a decoded mono SFX voice (a PCM SFX or a rendered FM effect) with
    // its positional pan/gain.
    void playVoice(std::vector<float> src, double inRate, float pan, float gain);

    // Compute the positional pan (-1 left .. +1 right) and gain (0..1) for a
    // world sound at (wx, wy) heard by a player at (px, py) facing
    // `playerAngle` (radians). `maxDist` is the distance at which the gain
    // falls to zero.
    static void position(double wx, double wy, double px, double py,
                         double playerAngle, double maxDist, float& pan,
                         float& gain);

    // Mix n stereo frames. l and r must each have size >= n. Inactive voices
    // are dropped.
    void mix(std::span<float> l, std::span<float> r);

private:
    float musicNext();   // one resampled music frame (centre)
    float chipNative();  // one Opl2::kRate frame, advancing the IMF ticks
    void imfTick();      // one 700 Hz sequencer tick

    int outRate_;
    float musicGain_ = 0.6f;
    float sfxGain_ = 1.0f;

    Opl2 chip_;
    const imf::MusicTrack* music_ = nullptr;
    std::size_t mPos_ = 0;
    std::uint32_t mWait_ = 0;
    double mTickAcc_ = 0.0;
    double mFrac_ = 0.0;
    float mA_ = 0.0f, mB_ = 0.0f;
    bool mPrimed_ = false;

    std::vector<ResampleVoice> voices_;
};

}  // namespace audio
}  // namespace w3d
