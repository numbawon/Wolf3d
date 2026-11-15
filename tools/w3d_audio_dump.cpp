// tools/w3d_audio_dump.cpp — Phase 4 audible evidence.
//
// Renders the game's audio to WAV files at the target output rate (default
// 48 kHz stereo), so a human can listen and the pipeline can be verified by
// ear:
//   * a music track (OPL2, resampled from the native 49716 Hz),
//   * a few FM effects (OPL2),
//   * a PCM SFX (resampled from the original 11025 Hz),
//   * a 16-bit / 44.1 kHz "drop-in" tone, resampled to the output rate — the
//     demonstration that the resampling stage accepts any input rate/width.
//
// Usage: w3d_tool_audio_dump [gamedir] [outdir] [out_rate]
//   gamedir  root of the Steam/GOG install (default baked in or $W3D_GAME_DIR)
//   outdir   directory to write the .wav files (default ".")
//   out_rate mixer output sample rate (default 48000)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "audio/mixer.hpp"
#include "w3d/audio.hpp"
#include "w3d/imf.hpp"
#include "w3d/opl2.hpp"

namespace {

std::filesystem::path game_dir() {
    if (const char* env = std::getenv("W3D_GAME_DIR"))
        return env;
#ifdef W3D_GAME_DIR_DEFAULT
    return W3D_GAME_DIR_DEFAULT;
#else
    return "C:/Program Files (x86)/Steam/steamapps/common/Wolfenstein 3D";
#endif
}

// Write a 16-bit little-endian interleaved-stereo WAV.
bool write_wav(const std::filesystem::path& path, const float* l, const float* r,
               std::size_t frames, std::uint32_t rate) {
    auto clamp16 = [](float v) -> std::int16_t {
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        return static_cast<std::int16_t>(v * 32767.0f);
    };
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(frames * 4);
    std::vector<std::uint8_t> hdr(44);
    auto put32 = [&](std::size_t off, std::uint32_t v) {
        hdr[off] = v & 0xFF; hdr[off + 1] = (v >> 8) & 0xFF;
        hdr[off + 2] = (v >> 16) & 0xFF; hdr[off + 3] = (v >> 24) & 0xFF;
    };
    auto put16 = [&](std::size_t off, std::uint16_t v) {
        hdr[off] = v & 0xFF; hdr[off + 1] = (v >> 8) & 0xFF;
    };
    std::memcpy(hdr.data(), "RIFF", 4);
    put32(4, 36 + dataBytes);
    std::memcpy(hdr.data() + 8, "WAVEfmt ", 8);
    put32(16, 16);          // fmt chunk size
    put16(20, 1);           // PCM
    put16(22, 2);           // channels
    put32(24, rate);
    put32(28, rate * 4);    // byte rate
    put16(32, 4);           // block align
    put16(34, 16);          // bits
    std::memcpy(hdr.data() + 36, "data", 4);
    put32(40, dataBytes);

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(hdr.data()), hdr.size());
    for (std::size_t i = 0; i < frames; ++i) {
        const std::int16_t a = clamp16(l[i]);
        const std::int16_t b = clamp16(r[i]);
        char buf[4] = {static_cast<char>(a & 0xFF), static_cast<char>((a >> 8) & 0xFF),
                       static_cast<char>(b & 0xFF), static_cast<char>((b >> 8) & 0xFF)};
        f.write(buf, 4);
    }
    return true;
}

// Count positive-going zero crossings to measure a tone's frequency.
double measure_hz(const std::vector<float>& x, std::uint32_t rate) {
    std::size_t zc = 0;
    for (std::size_t i = 1; i < x.size(); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) ++zc;
    const double seconds = static_cast<double>(x.size()) / rate;
    return seconds > 0 ? static_cast<double>(zc) / seconds : 0.0;
}

}  // namespace

int main(int argc, char** argv) {
    // Cross-validation mode: compare our C++ Opl2 render of track 0 against a
    // reference render produced by the uWolf clean-room OPL2 (see
    // scratch/opl2_ref.js).
    if (argc > 1 && std::string(argv[1]) == "--xcheck") {
        const auto refjson = (argc > 2) ? std::filesystem::path(argv[2])
                                        : std::filesystem::path("opl2_ref.json");
        const int N = (argc > 3) ? std::atoi(argv[3]) : 4000;
        const auto gdir = game_dir();
        auto bank = w3d::AudioBank::load(gdir / "base" / "AUDIOHED.WL6",
                                         gdir / "base" / "AUDIOT.WL6");
        if (!bank) {
            std::printf("xcheck: load failed: %s\n", bank.error().message.c_str());
            return 1;
        }
        if (bank->music.empty()) {
            std::printf("xcheck: no music\n");
            return 1;
        }
        std::vector<float> mine(N);
        w3d::Opl2 chip;
        w3d::imf::renderTrack(chip, bank->music[0], std::span<float>(mine));
        std::ifstream f(refjson);
        if (!f) {
            std::printf("xcheck: cannot open %s\n", refjson.string().c_str());
            return 1;
        }
        std::string s((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
        std::vector<float> ref;
        std::size_t i = 0;
        while (i < s.size()) {
            const char c = s[i];
            if (c == '-' || c == '+' || std::isdigit((unsigned char)c) || c == '.') {
                std::size_t j = i;
                while (j < s.size() &&
                       (std::isdigit((unsigned char)s[j]) || s[j] == '-' || s[j] == '+' ||
                        s[j] == '.' || s[j] == 'e' || s[j] == 'E'))
                    ++j;
                ref.push_back(std::stof(s.substr(i, j - i)));
                i = j;
            } else {
                ++i;
            }
        }
        if (ref.size() < (std::size_t)N) {
            std::printf("xcheck: ref has %zu samples (< %d)\n", ref.size(), N);
            return 1;
        }
        double maxdiff = 0.0;
        std::size_t argmax = 0;
        for (int k = 0; k < N; ++k) {
            const double d = std::abs(mine[k] - ref[k]);
            if (d > maxdiff) {
                maxdiff = d;
                argmax = (std::size_t)k;
            }
        }
        std::printf("xcheck N=%d maxdiff=%.8g at i=%zu\n", N, maxdiff, argmax);
        auto peak = [](const std::vector<float>& v) {
            double p = 0;
            for (auto x : v)
                p = std::max(p, std::abs(static_cast<double>(x)));
            return p;
        };
        auto rms = [](const std::vector<float>& v) {
            double s = 0;
            for (auto x : v)
                s += static_cast<double>(x) * x;
            return v.empty() ? 0.0 : std::sqrt(s / static_cast<double>(v.size()));
        };
        std::printf("  mine peak=%.6g rms=%.6g\n", peak(mine), rms(mine));
        std::printf("  ref  peak=%.6g rms=%.6g\n", peak(ref), rms(ref));
        std::printf("  mine[0..7]: ");
        for (int k = 0; k < 8; ++k) std::printf("%.6g ", mine[k]);
        std::printf("\n  ref [0..7]: ");
        for (int k = 0; k < 8; ++k) std::printf("%.6g ", ref[k]);
        std::printf("\n");
        return maxdiff < 0.02 ? 0 : 2;
    }

    const auto gdir = (argc > 1) ? std::filesystem::path(argv[1]) : game_dir();
    const auto outdir = (argc > 2) ? std::filesystem::path(argv[2]) : std::filesystem::path(".");
    const int outRate = (argc > 3) ? std::atoi(argv[3]) : 48000;

    std::error_code ec;
    std::filesystem::create_directories(outdir, ec);

    auto bank = w3d::AudioBank::load(gdir / "base" / "AUDIOHED.WL6",
                                     gdir / "base" / "AUDIOT.WL6");
    if (!bank) {
        std::printf("audio_dump: load failed: %s\n", bank.error().message.c_str());
        return 1;
    }
    auto& a = *bank;
    std::printf("loaded base: %zu PCM SFX, %zu FM effects, %zu music tracks\n",
                a.sfx.size(), a.fm_count, a.music.size());

    // --- Music: render a few seconds of track 0 (CORNER) at the output rate.
    if (!a.music.empty()) {
        const auto& track = a.music[0];
        const std::size_t frames = static_cast<std::size_t>(8 * outRate);
        std::vector<float> l(frames), r(frames);
        w3d::audio::Mixer m(outRate);
        m.setMusic(&track);
        m.mix(l, r);
        const auto p = outdir / ("music_" + track.name + ".wav");
        if (write_wav(p, l.data(), r.data(), frames, static_cast<std::uint32_t>(outRate)))
            std::printf("  wrote %s (%.2f s, OPL2 @49716 -> %d Hz)\n",
                        p.string().c_str(), 8.0, outRate);
    }

    // --- FM effects: render the first few named ones.
    std::size_t written = 0;
    for (std::size_t i = 0; i < a.fm.size() && written < 4; ++i) {
        if (!a.fm[i] || a.fm[i]->name.empty())
            continue;
        auto fx = w3d::imf::renderFx(*a.fm[i]);
        if (fx.empty())
            continue;
        // Resample the native-rate effect buffer to the output rate.
        const std::size_t outFrames =
            static_cast<std::size_t>(static_cast<double>(fx.size()) * outRate /
                                     w3d::Opl2::kRate) + 8;
        w3d::audio::Mixer m(outRate);
        m.playVoice(std::move(fx), static_cast<double>(w3d::Opl2::kRate), 0.0f, 1.0f);
        std::vector<float> l(outFrames), r(outFrames);
        m.mix(l, r);
        // Trim the silent tail.
        std::size_t end = outFrames;
        while (end > 0 && (std::abs(l[end - 1]) < 1e-4f && std::abs(r[end - 1]) < 1e-4f)) --end;
        const auto p = outdir / ("fx_" + a.fm[i]->name + ".wav");
        if (write_wav(p, l.data(), r.data(), end, static_cast<std::uint32_t>(outRate))) {
            std::printf("  wrote %s (%.2f s, FM effect '%s')\n", p.string().c_str(),
                        static_cast<double>(end) / outRate, a.fm[i]->name.c_str());
            ++written;
        }
    }

    // --- PCM SFX: resample a real digitised effect (original 11025 Hz).
    //     Wolf3D's digitised SFX are short (the long ones are FM); render the
    //     longest one so the output is audible.
    const double pcmRate = 11025.0;
    if (!a.sfx.empty()) {
        std::size_t best = 0;
        for (std::size_t i = 1; i < a.sfx.size(); ++i)
            if (a.sfx[i].nsamples > a.sfx[best].nsamples)
                best = i;
        const auto& sfx = a.sfx[best];
        std::vector<std::uint8_t> raw(sfx.samples.begin(), sfx.samples.end());
        auto dec = w3d::audio::decodePcm(raw.data(), raw.size(), 8);
        if (!dec.empty()) {
            w3d::audio::Mixer m(outRate);
            m.playVoice(std::move(dec), pcmRate, 0.0f, 1.0f);
            const std::size_t outFrames =
                static_cast<std::size_t>(static_cast<double>(sfx.nsamples) * outRate / pcmRate) + 8;
            std::vector<float> l(outFrames), r(outFrames);
            m.mix(l, r);
            std::size_t end = outFrames;
            while (end > 0 && (std::abs(l[end - 1]) < 1e-4f && std::abs(r[end - 1]) < 1e-4f)) --end;
            const auto p = outdir / ("pcm_" + std::to_string(best) + ".wav");
            if (write_wav(p, l.data(), r.data(), end, static_cast<std::uint32_t>(outRate)))
                std::printf("  wrote %s (%.2f s, PCM SFX #%zu [%u samples] @%d -> %d Hz)\n",
                            p.string().c_str(), static_cast<double>(end) / outRate, best,
                            sfx.nsamples, static_cast<int>(pcmRate), outRate);
        }
    }

    // --- Positional demo: a short tone played at three angles (left, centre,
    //     right), concatenated into one stereo file so it sweeps the field.
    //     Demonstrates the mixer's angle-based panning audibly.
    {
        const double hz = 660.0;
        const std::size_t seg = static_cast<std::size_t>(0.3 * outRate);
        std::vector<float> tone(seg);
        const double fade = 0.01;
        const double segSec = static_cast<double>(seg) / outRate;
        for (std::size_t i = 0; i < seg; ++i) {
            const double t = static_cast<double>(i) / outRate;
            double env = 1.0;
            if (t < fade) env = t / fade;
            if (segSec - t < fade) env = (segSec - t) / fade;
            tone[i] = static_cast<float>(
                std::sin(2.0 * 3.14159265358979323846 * hz * t) * 0.5 * env);
        }
        const float pans[3] = {-1.0f, 0.0f, 1.0f};
        std::vector<float> allL, allR;
        allL.reserve(seg * 3);
        allR.reserve(seg * 3);
        for (int k = 0; k < 3; ++k) {
            w3d::audio::Mixer m(outRate);
            m.playVoice(std::vector<float>(tone), static_cast<double>(outRate), pans[k], 1.0f);
            std::vector<float> l(seg), r(seg);
            m.mix(l, r);
            allL.insert(allL.end(), l.begin(), l.end());
            allR.insert(allR.end(), r.begin(), r.end());
        }
        const auto p = outdir / "positional_demo.wav";
        if (write_wav(p, allL.data(), allR.data(), allL.size(),
                      static_cast<std::uint32_t>(outRate)))
            std::printf("  wrote %s (%.2f s, 660 Hz tone panned left -> centre -> right)\n",
                        p.string().c_str(), static_cast<double>(allL.size()) / outRate);
    }

    // --- Resampling-hook demo: a 16-bit / 44.1 kHz drop-in tone, resampled to
    //     the output rate. Verify the frequency survives the resampling.
    {
        const double inRate = 44100.0;
        const double hz = 440.0;  // A4
        const std::size_t inFrames = static_cast<std::size_t>(2 * inRate);
        std::vector<std::uint8_t> pcm16(inFrames * 2);
        for (std::size_t i = 0; i < inFrames; ++i) {
            const double ph = 2.0 * 3.14159265358979323846 * hz * i / inRate;
            const std::int16_t v = static_cast<std::int16_t>(std::sin(ph) * 20000.0);
            pcm16[i * 2] = static_cast<std::uint8_t>(v & 0xFF);
            pcm16[i * 2 + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
        }
        auto dec = w3d::audio::decodePcm(pcm16.data(), pcm16.size(), 16);
        w3d::audio::Mixer m(outRate);
        m.playVoice(std::move(dec), inRate, 0.0f, 1.0f);
        const std::size_t outFrames =
            static_cast<std::size_t>(inFrames * outRate / inRate) + 8;
        std::vector<float> l(outFrames), r(outFrames);
        m.mix(l, r);
        std::size_t end = outFrames;
        while (end > 0 && (std::abs(l[end - 1]) < 1e-4f && std::abs(r[end - 1]) < 1e-4f)) --end;
        l.resize(end);
        r.resize(end);
        const auto p = outdir / ("dropin_44k_to_" + std::to_string(outRate) + ".wav");
        if (write_wav(p, l.data(), r.data(), end, static_cast<std::uint32_t>(outRate))) {
            const double meas = measure_hz(l, static_cast<std::uint32_t>(outRate));
            std::printf("  wrote %s (%.2f s, 16-bit/44100 drop-in -> %d Hz; "
                        "measured %.2f Hz, expect ~%.0f)\n",
                        p.string().c_str(), static_cast<double>(end) / outRate, outRate,
                        meas, hz);
        }
    }

    std::printf("done.\n");
    return 0;
}
