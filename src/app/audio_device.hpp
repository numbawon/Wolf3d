// w3d app/audio_device.hpp — miniaudio output device + mixer pull callback (WS5).
// The mixer (audio/mixer.cpp) is the pure-C++ audio producer: Mixer::mix()
// fills stereo float32 frames at 48 kHz. This class owns a miniaudio playback
// device whose data callback draws from the mixer. miniaudio (v0.11.x single
// header) is compiled into audio_device.cpp only; this header does not expose
// it, so including it does not drag in platform headers.
//
// Threading: miniaudio's data callback runs on miniaudio's internal thread.
// The mixer is accessed from that thread (onPull) and from the app thread
// (playSfx, setMusic, set*Volume). A mutex serializes mixer access. SFX
// decoding happens on the app thread before the lock is taken, so the audio
// thread is never blocked by decode work.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio/mixer.hpp"
#include "w3d/audio.hpp"
#include "w3d/imf.hpp"

namespace w3d::app {

// One SFX to play: bank index + whether it's an FM effect, plus a
// pre-computed pan/gain.
struct SfxPlay {
    int bankIndex = -1;   // index into AudioBank.sfx (PCM) or AudioBank.fm chunk (FM)
    bool isFm = false;    // true = FM effect at fm[chunk], false = PCM SFX at sfx[idx]
    float pan = 0.0f;     // -1 left .. +1 right
    float gain = 1.0f;    // 0..1
};

class AudioDevice {
public:
    // Create and start a 48 kHz float32 stereo playback device.
    // deviceId == "" -> system default. Otherwise the first device whose name
    // contains deviceId is selected (falling back to the default if none
    // matches). On failure the object is still usable; it is just silent.
    explicit AudioDevice(const std::string& deviceId = "");
    ~AudioDevice();

    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // List available output device names.
    static std::vector<std::string> listDevices();

    // True when the output device opened and started.
    [[nodiscard]] bool ok() const noexcept { return initialized_; }

    // Output diagnostics: frames delivered to the device so far and the peak
    // absolute sample level (post master gain) seen in them.
    struct Stats {
        std::uint64_t frames = 0;
        float peak = 0.0f;
    };
    [[nodiscard]] Stats stats() const;

    // The bank playSfx() indices refer to. Must outlive this object (or be
    // reset to nullptr first). Until a bank is set, playSfx() is a no-op.
    void setBank(const w3d::AudioBank* bank);

    // Play a positional SFX. pan/gain are computed by the caller from the
    // sound's world position relative to the player (Mixer::position).
    void playSfx(const SfxPlay& sfx);

    // Set the music track (nullptr = stop). The mixer loops the track. The
    // track must outlive its use by the device.
    void setMusic(const w3d::imf::MusicTrack* track);
    [[nodiscard]] bool hasMusic() const noexcept;

    // Volume controls (0..1), applied to the mixer gains / the final output.
    void setMasterVolume(float g) noexcept;
    void setMusicVolume(float g) noexcept;
    void setSfxVolume(float g) noexcept;
    void setMusicOn(bool on) noexcept;

private:
    // Holds the miniaudio context + device; defined in audio_device.cpp.
    struct Impl;

    // Fill `frameCount` interleaved stereo float32 frames (audio thread).
    void onPull(float* out, std::uint32_t frameCount);

    std::unique_ptr<Impl> impl_;
    bool initialized_ = false;

    mutable std::mutex mixerMutex_;
    const w3d::AudioBank* bank_ = nullptr;
    const w3d::imf::MusicTrack* musicTrack_ = nullptr;
    w3d::audio::Mixer mixer_{48000};
    std::vector<float> mixL_, mixR_;

    float masterGain_ = 1.0f;
    bool musicOn_ = true;
    Stats stats_;
};

}  // namespace w3d::app
