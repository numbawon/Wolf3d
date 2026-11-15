// audio/mixer.cpp — resampling + positional mixing.
#include "audio/mixer.hpp"

#include <algorithm>
#include <cmath>

namespace w3d {
namespace audio {

std::vector<float> decodePcm(const std::uint8_t* data, std::size_t n, int bits) {
    std::vector<float> out;
    if (bits == 8) {
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            out.push_back((static_cast<float>(data[i]) - 128.0f) / 128.0f);
    } else if (bits == 16) {
        out.reserve(n / 2);
        for (std::size_t i = 0; i + 1 < n; i += 2) {
            const std::int16_t v =
                static_cast<std::int16_t>(static_cast<std::uint16_t>(data[i]) |
                                          (static_cast<std::uint16_t>(data[i + 1]) << 8));
            out.push_back(static_cast<float>(v) / 32768.0f);
        }
    } else {
        // Unknown width: leave empty (the caller checks the result).
    }
    return out;
}

void ResampleVoice::load(std::vector<float> src, double inRate, float pan, float gain,
                         double outRate) {
    src_ = std::move(src);
    inRate_ = inRate;
    outRate_ = outRate;
    pos_ = 0.0;
    pan_ = pan;
    gain_ = gain;
    done_ = src_.empty();
}

float ResampleVoice::next() {
    if (done_)
        return 0.0f;
    const double p = pos_;
    const auto i0 = static_cast<std::size_t>(p);
    if (i0 >= src_.size()) {
        done_ = true;
        return 0.0f;
    }
    const auto i1 = (i0 + 1 < src_.size()) ? i0 + 1 : i0;
    const float v = src_[i0] + (src_[i1] - src_[i0]) * static_cast<float>(p - i0);
    pos_ += inRate_ / outRate_;
    if (static_cast<std::size_t>(pos_) >= src_.size())
        done_ = true;
    return v;
}

void Mixer::setMusic(const imf::MusicTrack* track) {
    chip_.reset();
    music_ = track;
    mPos_ = 0;
    mWait_ = 0;
    mTickAcc_ = 0.0;
    mFrac_ = 0.0;
    mA_ = mB_ = 0.0f;
    mPrimed_ = false;
}

void Mixer::playVoice(std::vector<float> src, double inRate, float pan, float gain) {
    ResampleVoice v;
    v.load(std::move(src), inRate, pan, gain, static_cast<double>(outRate_));
    voices_.push_back(std::move(v));
}

void Mixer::position(double wx, double wy, double px, double py,
                     double playerAngle, double maxDist, float& pan, float& gain) {
    const double dx = wx - px;
    const double dy = wy - py;
    const double dist = std::hypot(dx, dy);
    // Pan: the component of the direction to the sound perpendicular to the
    // player's facing. sin() of the relative angle is -1 (fully left) .. +1
    // (fully right).
    const double rel = std::atan2(dy, dx) - playerAngle;
    pan = static_cast<float>(std::sin(rel));
    // Volume falls linearly to zero at maxDist.
    double g = 1.0 - dist / maxDist;
    if (g < 0.0) g = 0.0;
    if (g > 1.0) g = 1.0;
    gain = static_cast<float>(g);
}

float Mixer::chipNative() {
    mTickAcc_ += 1.0;
    const double samplesPerTick =
        static_cast<double>(Opl2::kRate) / imf::kImfRate;
    if (mTickAcc_ >= samplesPerTick) {
        mTickAcc_ -= samplesPerTick;
        imfTick();
    }
    return chip_.sample();
}

void Mixer::imfTick() {
    if (!music_ || music_->data.empty())
        return;
    if (mWait_ > 0) {
        --mWait_;
        return;
    }
    const auto& d = music_->data;
    while (mPos_ + 3 < d.size()) {
        const auto reg = d[mPos_];
        const auto val = d[mPos_ + 1];
        const auto delay =
            static_cast<std::uint16_t>(d[mPos_ + 2] | (d[mPos_ + 3] << 8));
        mPos_ += 4;
        chip_.write(reg, val);
        if (delay) {
            mWait_ = delay - 1;
            return;
        }
    }
    if (mPos_ + 3 >= d.size()) {
        mPos_ = 0;
        mWait_ = 0;  // loop
    }
}

float Mixer::musicNext() {
    if (!mPrimed_) {
        mA_ = chipNative();
        mB_ = chipNative();
        mPrimed_ = true;
    }
    const float v = mA_ + (mB_ - mA_) * static_cast<float>(mFrac_);
    mFrac_ += static_cast<double>(Opl2::kRate) / outRate_;
    while (mFrac_ >= 1.0) {
        mFrac_ -= 1.0;
        mA_ = mB_;
        mB_ = chipNative();
    }
    return v;
}

void Mixer::mix(std::span<float> l, std::span<float> r) {
    const std::size_t n = std::min(l.size(), r.size());
    for (std::size_t i = 0; i < n; ++i) {
        float lv = 0.0f, rv = 0.0f;
        if (music_) {
            const float m = musicNext() * musicGain_;
            lv += m;  // music is mono (centre)
            rv += m;
        }
        for (auto& v : voices_) {
            if (v.done())
                continue;
            const float s = v.next() * v.gain() * sfxGain_;
            // Energy-preserving pan law: p in [-1,1] (left..right).
            const float p = v.pan();
            const float lp = std::cos((p + 1.0f) * 0.25f * 3.14159265358979323846f);
            const float rp = std::sin((p + 1.0f) * 0.25f * 3.14159265358979323846f);
            lv += s * lp;
            rv += s * rp;
        }
        l[i] = lv;
        r[i] = rv;
    }
    // Drop exhausted voices.
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                 [](const ResampleVoice& v) { return v.done(); }),
                  voices_.end());
}

}  // namespace audio
}  // namespace w3d
