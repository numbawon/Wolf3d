// w3d/imf.cpp — IMF model, parsers, and sequencer.
#include "w3d/imf.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "w3d/bytes.hpp"

namespace w3d {
namespace imf {
namespace {

constexpr std::uint8_t U8(const std::byte* p) {
    return static_cast<std::uint8_t>(*p);
}

// Advance the IMF sequence by one 700 Hz tick, performing every write that is
// due. Mirrors the original: a packet with a non-zero delay consumes the tick.
struct Seq {
    Opl2* chip = nullptr;
    const MusicTrack* t = nullptr;
    std::size_t pos = 0;
    std::uint32_t wait = 0;

    void tick() {
        if (wait > 0) {
            --wait;
            return;
        }
        const auto& d = t->data;
        while (pos + 3 < d.size()) {
            const auto reg = d[pos];
            const auto val = d[pos + 1];
            const auto delay =
                static_cast<std::uint16_t>(d[pos + 2] | (d[pos + 3] << 8));
            pos += 4;
            chip->write(reg, val);
            if (delay) {
                wait = delay - 1;
                return;  // this tick is used up
            }
        }
        if (pos + 3 >= d.size()) {
            pos = 0;
            wait = 0;  // loop
        }
    }
};

}  // namespace

Result<MusicTrack> parseTrack(std::span<const std::byte> chunk) {
    if (chunk.size() < 2)
        return std::unexpected(
            make_error(Error::Kind::Corrupt, "imf: track chunk too short"));
    const auto* c = chunk.data();
    const auto dlen =
        static_cast<std::uint16_t>(U8(c) | (U8(c + 1) << 8));
    MusicTrack t;
    if (dlen == 0) {
        // Quirk (SD_StartMusic): a leading 0 means "no header — the chunk is
        // the data". Kept for fidelity; real game tracks always carry a dlen.
        t.data.resize(chunk.size());
        for (std::size_t i = 0; i < chunk.size(); ++i)
            t.data[i] = U8(c + i);
        return t;
    }
    const auto dataStart = std::size_t{2};
    if (dataStart + dlen > chunk.size())
        return std::unexpected(
            make_error(Error::Kind::Corrupt, "imf: dlen overruns chunk"));
    t.data.resize(dlen);
    for (std::uint16_t i = 0; i < dlen; ++i)
        t.data[i] = U8(c + dataStart + i);
    // The 88-byte Muse tag carries the song title (bytes 2..18).
    const auto tagStart = dataStart + dlen;
    if (tagStart + kTagSize <= chunk.size()) {
        const auto* tag = c + tagStart;
        std::string name;
        for (std::size_t i = 2; i < 2 + 16; ++i) {
            const auto ch = U8(tag + i);
            if (ch == 0)
                break;
            name.push_back(static_cast<char>(ch));
        }
        t.name = name;
    }
    return t;
}

Result<FmEffect> parseFx(std::span<const std::byte> chunk) {
    if (chunk.size() < 24)
        return std::unexpected(
            make_error(Error::Kind::Corrupt, "fx: chunk too short"));
    const auto* c = chunk.data();
    const auto nl =
        static_cast<std::uint32_t>(U8(c) | (U8(c + 1) << 8) | (U8(c + 2) << 16) |
                                  (U8(c + 3) << 24));
    FmEffect fx;
    fx.mChar = U8(c + 6);
    fx.cChar = U8(c + 7);
    fx.mScale = U8(c + 8);
    fx.cScale = U8(c + 9);
    fx.mAttack = U8(c + 10);
    fx.cAttack = U8(c + 11);
    fx.mSus = U8(c + 12);
    fx.cSus = U8(c + 13);
    fx.mWave = U8(c + 14);
    fx.cWave = U8(c + 15);
    fx.block = U8(c + 22);
    auto noteCount = nl;
    if (23 + noteCount > chunk.size())
        noteCount = static_cast<std::uint32_t>(chunk.size() - 23);
    fx.notes.resize(noteCount);
    for (std::size_t i = 0; i < noteCount; ++i)
        fx.notes[i] = U8(c + 23 + i);
    // Name: first run of >= 3 printable bytes in the trailer after the notes.
    const auto* trail = c + 23 + noteCount;
    const auto trailLen = chunk.size() - (23 + noteCount);
    for (std::size_t i = 0; i < trailLen;) {
        const std::uint8_t b = U8(trail + i);
        if (b >= 0x20 && b < 0x7f) {
            std::string name;
            std::size_t j = i;
            while (j < trailLen) {
                const std::uint8_t bj = U8(trail + j);
                if (bj < 0x20 || bj >= 0x7f)
                    break;
                name.push_back(static_cast<char>(bj));
                ++j;
            }
            if (name.size() >= 3) {
                fx.name = name;
                break;
            }
            i = j;
        } else {
            ++i;
        }
    }
    return fx;
}

void renderTrack(Opl2& chip, const MusicTrack& track, std::span<float> out) {
    chip.reset();
    Seq seq;
    seq.chip = &chip;
    seq.t = &track;
    double tickAcc = 0.0;
    const double samplesPerTick =
        static_cast<double>(Opl2::kRate) / kImfRate;
    for (auto& s : out) {
        tickAcc += 1.0;
        if (tickAcc >= samplesPerTick) {
            tickAcc -= samplesPerTick;
            seq.tick();
        }
        s = chip.sample();
    }
}

std::vector<float> renderFx(const FmEffect& fx) {
    if (fx.badInstrument() || fx.notes.empty())
        return {};
    Opl2 chip;
    chip.reset();
    // Program the instrument onto channel 0: modulator = operator offset 0,
    // carrier = operator offset 3; no feedback, FM connect.
    chip.write(0x20 + 0, fx.mChar);
    chip.write(0x40 + 0, fx.mScale);
    chip.write(0x60 + 0, fx.mAttack);
    chip.write(0x80 + 0, fx.mSus);
    chip.write(0xE0 + 0, fx.mWave);
    chip.write(0x20 + 3, fx.cChar);
    chip.write(0x40 + 3, fx.cScale);
    chip.write(0x60 + 3, fx.cAttack);
    chip.write(0x80 + 3, fx.cSus);
    chip.write(0xE0 + 3, fx.cWave);
    chip.write(0xC0, 0);

    const auto blockReg = static_cast<std::uint8_t>(((fx.block & 7) << 2) | 0x20);
    std::size_t notePos = 0;
    bool sfxDone = false;
    double sfxAcc = 0.0;
    const double samplesPerSfxTick =
        static_cast<double>(Opl2::kRate) / kSfxRate;
    const auto n = static_cast<std::size_t>(std::ceil(
        (fx.notes.size() + 1) * Opl2::kRate / static_cast<double>(kSfxRate) +
        kFxTail * Opl2::kRate));
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        sfxAcc += 1.0;
        if (sfxAcc >= samplesPerSfxTick) {
            sfxAcc -= samplesPerSfxTick;
            if (!sfxDone) {
                if (notePos >= fx.notes.size()) {
                    chip.write(0xB0, 0);  // key off, then the release tail
                    sfxDone = true;
                } else {
                    const auto note = fx.notes[notePos++];
                    if (note) {
                        chip.write(0xA0, note);
                        chip.write(0xB0, blockReg);
                    } else {
                        chip.write(0xB0, 0);
                    }
                }
            }
        }
        out[i] = chip.sample();
    }
    // Trim the silent tail.
    while (!out.empty() && std::abs(out.back()) < 1e-4f)
        out.pop_back();
    return out;
}

Result<std::vector<MusicTrack>> parseMusicRegion(
    std::span<const std::byte> audiot) {
    static const std::uint8_t kMarker[8] = {'!', 'I', 'D', '!', '!', 'I', 'D', '!'};
    std::size_t start = std::string_view::npos;
    for (std::size_t i = 0; i + 8 <= audiot.size(); ++i) {
        bool match = true;
        for (int k = 0; k < 8; ++k) {
            if (U8(audiot.data() + i + k) != kMarker[k]) {
                match = false;
                break;
            }
        }
        if (match) {
            start = i;
            break;
        }
    }
    if (start == std::string_view::npos)
        return std::unexpected(
            make_error(Error::Kind::Corrupt, "music: '!ID!!ID!' marker not found"));

    std::vector<MusicTrack> tracks;
    std::size_t pos = start + 8;
    while (pos + 2 <= audiot.size()) {
        const auto dlen = static_cast<std::uint16_t>(
            U8(audiot.data() + pos) | (U8(audiot.data() + pos + 1) << 8));
        const std::size_t chunkEnd = pos + 2 + dlen + kTagSize;
        if (chunkEnd > audiot.size())
            break;  // region does not close cleanly
        std::span<const std::byte> chunk(audiot.data() + pos, 2 + dlen + kTagSize);
        auto res = parseTrack(chunk);
        if (!res)
            return std::unexpected(res.error());
        tracks.push_back(std::move(*res));
        pos = chunkEnd;
    }
    if (tracks.empty() || pos != audiot.size())
        return std::unexpected(
            make_error(Error::Kind::Corrupt,
                       "music: region does not walk to end of file"));
    return tracks;
}

}  // namespace imf
}  // namespace w3d
