// tests/test_audio.cpp — Phase 4 audio tests.
//
// Verifies the audio pipeline against the real game data:
//   * the AudioBank decodes the PCM SFX, FM effects, and music tracks of both
//     base and Spear of Destiny with the right counts and names;
//   * the OPL2 chip renders a real music track and an FM effect to
//     non-trivial (non-silent, non-DC, non-clipping) audio;
//   * the resampling hook accepts a 16-bit / 44.1 kHz drop-in and resamples it
//     to the output rate with the frequency preserved;
//   * positional mixing pans by angle and attenuates by distance.
// Game data: W3D_GAME_DIR env var, falling back to the baked-in Steam path.

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

struct Case {
    const char* name;
    void (*fn)();
};
std::vector<Case>& cases() {
    static std::vector<Case> c;
    return c;
}
struct Register {
    Register(const char* name, void (*fn)()) { cases().push_back({name, fn}); }
};

int g_checks = 0;
int g_failures = 0;
const char* g_current = "?";

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__,          \
                        g_current, #cond);                                     \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!((a) == (b))) {                                                   \
            ++g_failures;                                                      \
            std::printf("  FAIL %s:%d [%s] %s != %s\n", __FILE__, __LINE__,    \
                        g_current, #a, #b);                                    \
        }                                                                      \
    } while (0)

std::filesystem::path game_dir() {
    if (const char* env = std::getenv("W3D_GAME_DIR"))
        return env;
#ifdef W3D_GAME_DIR_DEFAULT
    return W3D_GAME_DIR_DEFAULT;
#else
    return "C:/Program Files (x86)/Steam/steamapps/common/Wolfenstein 3D";
#endif
}

// Render `seconds` of a music track at the native Opl2 rate; return the RMS
// and peak of the result.
struct RenderStats {
    double rms = 0.0;
    double peak = 0.0;
    double dc = 0.0;
    bool ok = false;
};
RenderStats renderMusicStats(const w3d::imf::MusicTrack& track, double seconds) {
    const auto n = static_cast<std::size_t>(seconds * w3d::Opl2::kRate);
    std::vector<float> buf(n);
    w3d::Opl2 chip;
    w3d::imf::renderTrack(chip, track, std::span<float>(buf));
    RenderStats s;
    double sum = 0.0, sumsq = 0.0, peak = 0.0;
    for (auto v : buf) {
        sum += v;
        sumsq += static_cast<double>(v) * v;
        peak = std::max(peak, static_cast<double>(std::abs(v)));
    }
    s.rms = std::sqrt(sumsq / n);
    s.peak = peak;
    s.dc = std::abs(sum / n);
    s.ok = true;
    return s;
}

}  // namespace

// ------------------------------------------------------------- audio bank

static void test_bank_base() {
    g_current = "bank_base";
    const auto g = game_dir();
    auto res = w3d::AudioBank::load(g / "base" / "AUDIOHED.WL6",
                                    g / "base" / "AUDIOT.WL6");
    CHECK(res.has_value());
    if (!res) {
        std::printf("  (load error: %s)\n", res.error().message.c_str());
        return;
    }
    auto& a = *res;
    CHECK_EQ(a.sfx.size(), 86u);
    CHECK_EQ(a.sfx_count, 86u);
    CHECK_EQ(a.fm_count, 87u);
    CHECK_EQ(a.music.size(), 27u);
    CHECK_EQ(a.music[0].name, std::string("CORNER"));
    CHECK_EQ(a.music[26].name, std::string("PACMAN"));
    // Every music track's register stream is a whole number of 4-byte units.
    for (const auto& t : a.music)
        CHECK(t.data.size() % 4 == 0);
    // A PCM SFX keeps its decoded shape.
    CHECK_EQ(a.sfx[0].nsamples, 8u);
    CHECK_EQ(a.sfx[0].volume, 1u);
    // An FM effect that we know by name: SHOT2 is a base effect.
    bool foundShot = false;
    for (const auto& f : a.fm)
        if (f && f->name == "SHOT2") foundShot = true;
    CHECK(foundShot);
}

static Register r_bank_base("bank_base", test_bank_base);

static void test_bank_sod() {
    g_current = "bank_sod";
    const auto g = game_dir();
    auto res = w3d::AudioBank::load(g / "base/m1" / "AUDIOHED.SOD",
                                    g / "base/m1" / "AUDIOT.SOD");
    CHECK(res.has_value());
    if (!res) {
        std::printf("  (load error: %s)\n", res.error().message.c_str());
        return;
    }
    auto& a = *res;
    CHECK_EQ(a.sfx.size(), 80u);
    CHECK_EQ(a.sfx_count, 80u);
    CHECK_EQ(a.fm_count, 81u);
    CHECK_EQ(a.music.size(), 24u);
    CHECK_EQ(a.music[0].name, std::string("XFUNKIE"));
    for (const auto& t : a.music)
        CHECK(t.data.size() % 4 == 0);
}

static Register r_bank_sod("bank_sod", test_bank_sod);

// ------------------------------------------------------------------ OPL2

static void test_opl2_track() {
    g_current = "opl2_track";
    const auto g = game_dir();
    auto res = w3d::AudioBank::load(g / "base" / "AUDIOHED.WL6",
                                    g / "base" / "AUDIOT.WL6");
    CHECK(res.has_value());
    if (!res)
        return;
    auto& a = *res;
    if (a.music.empty())
        return;
    // CORNER: a real floor track. It must produce real audio: non-silent,
    // centred (small DC), and not clipping.
    auto s = renderMusicStats(a.music[0], 2.0);
    CHECK(s.ok);
    CHECK(s.rms > 0.01);     // definitely not silent
    CHECK(s.rms < 0.95);     // not pegged
    CHECK(s.peak <= 1.0 + 1e-6);
    CHECK(s.dc < 0.05);      // no large DC offset
    // A second, different track should also be audible.
    if (a.music.size() > 1) {
        auto s2 = renderMusicStats(a.music[1], 2.0);
        CHECK(s2.ok);
        CHECK(s2.rms > 0.01);
    }
}

static Register r_opl2_track("opl2_track", test_opl2_track);

static void test_opl2_fx() {
    g_current = "opl2_fx";
    const auto g = game_dir();
    auto res = w3d::AudioBank::load(g / "base" / "AUDIOHED.WL6",
                                    g / "base" / "AUDIOT.WL6");
    CHECK(res.has_value());
    if (!res)
        return;
    auto& a = *res;
    // Render every FM effect; at least some must be non-trivial.
    int rendered = 0, nontrivial = 0;
    for (std::size_t i = 0; i < a.fm.size(); ++i) {
        if (!a.fm[i] || a.fm[i]->badInstrument())
            continue;
        auto fx = w3d::imf::renderFx(*a.fm[i]);
        if (fx.empty())
            continue;
        ++rendered;
        double peak = 0.0;
        for (auto v : fx)
            peak = std::max(peak, static_cast<double>(std::abs(v)));
        if (peak > 0.001)
            ++nontrivial;
    }
    CHECK(rendered > 10);
    CHECK(nontrivial > 5);
}

static Register r_opl2_fx("opl2_fx", test_opl2_fx);

// ---------------------------------------------------------- resampling

static void test_resample_dropin() {
    g_current = "resample_dropin";
    // A 16-bit / 44.1 kHz A4 (440 Hz) sine, resampled to 48 kHz. The
    // frequency must survive the resampling (within a couple of hertz).
    const double inRate = 44100.0;
    const double hz = 440.0;
    const int outRate = 48000;
    const std::size_t inFrames = static_cast<std::size_t>(2 * inRate);
    std::vector<std::uint8_t> pcm16(inFrames * 2);
    for (std::size_t i = 0; i < inFrames; ++i) {
        const double ph = 2.0 * 3.14159265358979323846 * hz * i / inRate;
        const std::int16_t v = static_cast<std::int16_t>(std::sin(ph) * 20000.0);
        pcm16[i * 2] = static_cast<std::uint8_t>(v & 0xFF);
        pcm16[i * 2 + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    }
    auto dec = w3d::audio::decodePcm(pcm16.data(), pcm16.size(), 16);
    CHECK_EQ(dec.size(), inFrames);
    // Decoded amplitude should be ~20000/32768.
    double decPeak = 0.0;
    for (auto v : dec)
        decPeak = std::max(decPeak, static_cast<double>(std::abs(v)));
    CHECK(decPeak > 0.5 && decPeak < 0.7);

    w3d::audio::Mixer m(outRate);
    m.playVoice(std::move(dec), inRate, 0.0f, 1.0f);
    const std::size_t outFrames = static_cast<std::size_t>(inFrames * outRate / inRate) + 8;
    std::vector<float> l(outFrames), r(outFrames);
    m.mix(l, r);
    // Trim the silent tail.
    std::size_t end = outFrames;
    while (end > 0 && std::abs(l[end - 1]) < 1e-4f) --end;
    CHECK(end > inFrames * outRate / inRate - 16);  // full length survived
    // Measure the frequency by positive-going zero crossings.
    std::size_t zc = 0;
    for (std::size_t i = 1; i < end; ++i)
        if (l[i - 1] < 0.0f && l[i] >= 0.0f) ++zc;
    const double seconds = static_cast<double>(end) / outRate;
    const double meas = seconds > 0 ? static_cast<double>(zc) / seconds : 0.0;
    CHECK(std::abs(meas - hz) < 2.0);  // ~440 Hz, within 2 Hz
    // Left and right must be identical (centre, no pan).
    double diff = 0.0;
    for (std::size_t i = 0; i < end; ++i)
        diff = std::max(diff, static_cast<double>(std::abs(l[i] - r[i])));
    CHECK(diff < 1e-5);
}

static Register r_resample("resample_dropin", test_resample_dropin);

// ----------------------------------------------------------- positional

static void test_positional() {
    g_current = "positional";
    using w3d::audio::Mixer;
    float pan = 0.0f, gain = 0.0f;
    // Player at origin facing +x (angle 0).
    // A sound directly ahead: centred, full gain (distance ~0).
    Mixer::position(1.0, 0.0, 0.0, 0.0, 0.0, 1000.0, pan, gain);
    CHECK(std::abs(pan) < 0.01);
    CHECK(gain > 0.99);
    // A sound to the player's right (+y): pans right (pan > 0).
    Mixer::position(0.0, 100.0, 0.0, 0.0, 0.0, 1000.0, pan, gain);
    CHECK(pan > 0.9);
    // A sound to the left (-y): pans left (pan < 0).
    Mixer::position(0.0, -100.0, 0.0, 0.0, 0.0, 1000.0, pan, gain);
    CHECK(pan < -0.9);
    // A sound behind: centred.
    Mixer::position(-100.0, 0.0, 0.0, 0.0, 0.0, 1000.0, pan, gain);
    CHECK(std::abs(pan) < 0.01);
    // A distant sound is quieter than a near one.
    float gNear = 0.0f, gFar = 0.0f;
    Mixer::position(100.0, 0.0, 0.0, 0.0, 0.0, 1000.0, pan, gNear);
    Mixer::position(900.0, 0.0, 0.0, 0.0, 0.0, 1000.0, pan, gFar);
    CHECK(gNear > gFar);
    CHECK(gFar < 0.2);
    // Beyond maxDist: silent.
    Mixer::position(5000.0, 0.0, 0.0, 0.0, 0.0, 1000.0, pan, gFar);
    CHECK_EQ(gFar, 0.0f);
}

static Register r_positional("positional", test_positional);

// -------------------------------------------------------- mixer music

static void test_mixer_music() {
    g_current = "mixer_music";
    const auto g = game_dir();
    auto res = w3d::AudioBank::load(g / "base" / "AUDIOHED.WL6",
                                    g / "base" / "AUDIOT.WL6");
    CHECK(res.has_value());
    if (!res)
        return;
    auto& a = *res;
    if (a.music.empty())
        return;
    // Run the music through the mixer (resampled to 48 kHz) and confirm the
    // output is audible and centred.
    const int outRate = 48000;
    w3d::audio::Mixer m(outRate);
    m.setMusic(&a.music[0]);
    m.setMusicGain(1.0f);
    const std::size_t frames = static_cast<std::size_t>(1.0 * outRate);
    std::vector<float> l(frames), r(frames);
    m.mix(l, r);
    double rms = 0.0, diff = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        rms += static_cast<double>(l[i]) * l[i];
        diff = std::max(diff, static_cast<double>(std::abs(l[i] - r[i])));
    }
    rms = std::sqrt(rms / frames);
    CHECK(rms > 0.01);     // audible through the mixer
    CHECK(diff < 1e-5);    // centred (mono music)
    // Switching to null silences the music.
    m.setMusic(nullptr);
    CHECK(!m.hasMusic());
    std::vector<float> l2(frames), r2(frames);
    m.mix(l2, r2);
    double peak = 0.0;
    for (auto v : l2)
        peak = std::max(peak, static_cast<double>(std::abs(v)));
    CHECK(peak < 1e-6);
}

static Register r_mixer_music("mixer_music", test_mixer_music);

// ------------------------------------------------------- mixer sfx voice

static void test_mixer_voice() {
    g_current = "mixer_voice";
    // A short 8-bit mono tone at 11025 Hz, played with the mixer at 48 kHz,
    // panned fully right: the left channel should be (near) silent.
    const int outRate = 48000;
    const double inRate = 11025.0;
    const double hz = 220.0;
    const std::size_t n = static_cast<std::size_t>(0.2 * inRate);
    std::vector<std::uint8_t> pcm8(n);
    for (std::size_t i = 0; i < n; ++i)
        pcm8[i] = static_cast<std::uint8_t>(128 +
                    std::sin(2.0 * 3.14159265358979323846 * hz * i / inRate) * 100.0);
    auto dec = w3d::audio::decodePcm(pcm8.data(), pcm8.size(), 8);
    CHECK_EQ(dec.size(), n);
    w3d::audio::Mixer m(outRate);
    m.playVoice(std::move(dec), inRate, 1.0f /*full right*/, 1.0f);
    const std::size_t frames = static_cast<std::size_t>(n * outRate / inRate) + 8;
    std::vector<float> l(frames), r(frames);
    m.mix(l, r);
    double lrms = 0.0, rrms = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        lrms += static_cast<double>(l[i]) * l[i];
        rrms += static_cast<double>(r[i]) * r[i];
    }
    lrms = std::sqrt(lrms / frames);
    rrms = std::sqrt(rrms / frames);
    CHECK(rrms > 0.05);   // audible in the right channel
    CHECK(lrms < rrms * 0.1);  // far quieter on the left (panned right)
}

static Register r_mixer_voice("mixer_voice", test_mixer_voice);

// ------------------------------------------------------------- error path

static void test_error_paths() {
    g_current = "error_paths";
    // A file that is not an audio bank: no PCM SFX, no music marker.
    std::vector<std::byte> junk(64, std::byte{0x11});
    auto hp = std::filesystem::temp_directory_path() / "w3d_test_ahed.tmp";
    auto ap = std::filesystem::temp_directory_path() / "w3d_test_audiot.tmp";
    {
        std::ofstream h(hp, std::ios::binary);
        // A 2-entry AUDIOHED pointing into the 64-byte AUDIOT.
        const std::uint32_t offs[2] = {0u, 64u};
        h.write(reinterpret_cast<const char*>(offs), sizeof(offs));
        std::ofstream a(ap, std::ios::binary);
        a.write(reinterpret_cast<const char*>(junk.data()), junk.size());
    }
    auto bad = w3d::AudioBank::load(hp, ap);
    // It should fail (no PCM SFX / no music region) rather than crash.
    CHECK(!bad.has_value());
    std::filesystem::remove(hp);
    std::filesystem::remove(ap);
}

static Register r_errors("error_paths", test_error_paths);

int main() {
    std::printf("w3d Phase 4 audio tests — game dir: %s\n",
                game_dir().string().c_str());
    for (const auto& c : cases()) {
        const int checks_before = g_checks;
        const int failures_before = g_failures;
        c.fn();
        std::printf("[%s] %s (%d checks)\n",
                    failures_before == g_failures ? "PASS" : "FAIL",
                    c.name, g_checks - checks_before);
    }
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
