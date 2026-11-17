// w3d app/audio_device.cpp — miniaudio output device + mixer pull callback (WS5).
//
// This is the one TU that compiles the miniaudio implementation. Only device
// I/O is needed, so the decoder / encoder / engine layers are compiled out.
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#ifndef NOMINMAX
#define NOMINMAX
#endif
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <miniaudio/miniaudio.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "app/audio_device.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <utility>

namespace w3d::app {

namespace {
constexpr ma_uint32 kSampleRate = 48000;
constexpr ma_uint32 kChannels = 2;
constexpr double kPcmSfxRate = 11025.0;  // PCM SFX assumed rate
}  // namespace

struct AudioDevice::Impl {
    ma_context context{};
    ma_device device{};
    bool contextOk = false;
    bool deviceOk = false;

    // miniaudio data callback: runs on miniaudio's internal thread.
    static void dataCallback(ma_device* pDevice, void* pOutput,
                             const void* /*pInput*/, ma_uint32 frameCount) {
        auto* self = static_cast<AudioDevice*>(pDevice->pUserData);
        self->onPull(static_cast<float*>(pOutput), frameCount);
    }
};

AudioDevice::AudioDevice(const std::string& deviceId)
    : impl_(std::make_unique<Impl>()) {
    if (ma_context_init(nullptr, 0, nullptr, &impl_->context) != MA_SUCCESS) {
        std::fprintf(stderr, "w3d: miniaudio context init failed\n");
        return;
    }
    impl_->contextOk = true;

    // Select the requested device (substring match on the device name).
    ma_device_id selected{};
    bool haveSelected = false;
    if (!deviceId.empty()) {
        ma_device_info* playback = nullptr;
        ma_uint32 playbackCount = 0;
        if (ma_context_get_devices(&impl_->context, &playback, &playbackCount,
                                   nullptr, nullptr) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < playbackCount; ++i) {
                if (std::strstr(playback[i].name, deviceId.c_str())) {
                    selected = playback[i].id;
                    haveSelected = true;
                    break;
                }
            }
        }
        if (!haveSelected) {
            std::fprintf(stderr,
                         "w3d: audio device '%s' not found; using default\n",
                         deviceId.c_str());
        }
    }

    // 48 kHz, float32, stereo. miniaudio converts to the device's native
    // format if it differs.
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.pDeviceID = haveSelected ? &selected : nullptr;
    config.playback.format = ma_format_f32;
    config.playback.channels = kChannels;
    config.sampleRate = kSampleRate;
    config.dataCallback = &Impl::dataCallback;
    config.pUserData = this;

    if (ma_device_init(&impl_->context, &config, &impl_->device) != MA_SUCCESS) {
        std::fprintf(stderr, "w3d: miniaudio device init failed\n");
        return;
    }
    impl_->deviceOk = true;

    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        std::fprintf(stderr, "w3d: miniaudio device start failed\n");
        return;
    }
    initialized_ = true;
}

AudioDevice::~AudioDevice() {
    // Uninit the device first: it stops the callback thread, so onPull can no
    // longer run by the time the mixer members are destroyed.
    if (impl_->deviceOk)
        ma_device_uninit(&impl_->device);
    if (impl_->contextOk)
        ma_context_uninit(&impl_->context);
}

std::vector<std::string> AudioDevice::listDevices() {
    std::vector<std::string> names;
    ma_context context;
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS)
        return names;
    ma_device_info* playback = nullptr;
    ma_uint32 playbackCount = 0;
    if (ma_context_get_devices(&context, &playback, &playbackCount, nullptr,
                               nullptr) == MA_SUCCESS) {
        names.reserve(playbackCount);
        for (ma_uint32 i = 0; i < playbackCount; ++i)
            names.emplace_back(playback[i].name);
    }
    ma_context_uninit(&context);
    return names;
}

AudioDevice::Stats AudioDevice::stats() const {
    std::lock_guard<std::mutex> lock(mixerMutex_);
    return stats_;
}

void AudioDevice::setBank(const w3d::AudioBank* bank) {
    std::lock_guard<std::mutex> lock(mixerMutex_);
    bank_ = bank;
}

void AudioDevice::playSfx(const SfxPlay& sfx) {
    const w3d::AudioBank* bank = nullptr;
    {
        std::lock_guard<std::mutex> lock(mixerMutex_);
        bank = bank_;
    }
    if (sfx.bankIndex < 0 || bank == nullptr)
        return;
    const auto idx = static_cast<std::size_t>(sfx.bankIndex);

    // Decode/render the SFX on the app thread, before the lock.
    std::vector<float> src;
    double inRate = kPcmSfxRate;
    if (sfx.isFm) {
        if (idx < bank->fm.size() && bank->fm[idx]) {
            src = w3d::imf::renderFx(*bank->fm[idx]);
            inRate = static_cast<double>(w3d::Opl2::kRate);
        }
    } else {
        if (idx < bank->sfx.size()) {
            const auto& s = bank->sfx[idx];
            src = w3d::audio::decodePcm(s.samples.data(), s.samples.size(), 8);
        }
    }
    if (src.empty())
        return;

    std::lock_guard<std::mutex> lock(mixerMutex_);
    mixer_.playVoice(std::move(src), inRate, sfx.pan, sfx.gain);
}

void AudioDevice::setMusic(const w3d::imf::MusicTrack* track) {
    std::lock_guard<std::mutex> lock(mixerMutex_);
    musicTrack_ = track;
    mixer_.setMusic(musicOn_ ? track : nullptr);
}

bool AudioDevice::hasMusic() const noexcept {
    std::lock_guard<std::mutex> lock(mixerMutex_);
    return mixer_.hasMusic();
}

void AudioDevice::setMasterVolume(float g) noexcept {
    std::lock_guard<std::mutex> lock(mixerMutex_);
    masterGain_ = std::clamp(g, 0.0f, 1.0f);
}

void AudioDevice::setMusicVolume(float g) noexcept {
    std::lock_guard<std::mutex> lock(mixerMutex_);
    mixer_.setMusicGain(std::clamp(g, 0.0f, 1.0f));
}

void AudioDevice::setSfxVolume(float g) noexcept {
    std::lock_guard<std::mutex> lock(mixerMutex_);
    mixer_.setSfxGain(std::clamp(g, 0.0f, 1.0f));
}

void AudioDevice::setMusicOn(bool on) noexcept {
    std::lock_guard<std::mutex> lock(mixerMutex_);
    if (on == musicOn_)
        return;
    musicOn_ = on;
    mixer_.setMusic(on ? musicTrack_ : nullptr);
}

void AudioDevice::onPull(float* out, std::uint32_t frameCount) {
    std::lock_guard<std::mutex> lock(mixerMutex_);
    // resize() only allocates when the callback size grows (normally once).
    mixL_.resize(frameCount);
    mixR_.resize(frameCount);
    mixer_.mix(std::span<float>(mixL_), std::span<float>(mixR_));
    // Interleave stereo, applying the master gain.
    for (std::uint32_t i = 0; i < frameCount; ++i) {
        out[i * 2]     = mixL_[i] * masterGain_;
        out[i * 2 + 1] = mixR_[i] * masterGain_;
        stats_.peak = std::max({stats_.peak, std::abs(out[i * 2]),
                                std::abs(out[i * 2 + 1])});
    }
    stats_.frames += frameCount;
}

}  // namespace w3d::app
