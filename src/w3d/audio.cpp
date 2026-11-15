// w3d/audio.cpp — AudioBank loader.
#include "w3d/audio.hpp"

#include <cstring>
#include <fstream>
#include <limits>
#include <span>

#include "w3d/bytes.hpp"

namespace w3d {
namespace {

std::vector<std::byte> read_file(const std::filesystem::path& path, Error& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = make_error(Error::Kind::Io, "cannot open " + path.string());
        return {};
    }
    f.seekg(0, std::ios::end);
    const auto sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<std::byte> buf(static_cast<std::size_t>(sz));
    if (sz > 0 && !f.read(reinterpret_cast<char*>(buf.data()), sz)) {
        err = make_error(Error::Kind::Io, "short read on " + path.string());
        return {};
    }
    return buf;
}

constexpr std::uint8_t U8(const std::byte* p) {
    return static_cast<std::uint8_t>(*p);
}

// Position of the 8-byte "!ID!!ID!" marker that opens the music region, or
// max when absent. Everything at or after it is a music track (or the empty
// slots that alias it), never an FM effect — so the effect classifier must not
// fire past this point.
constexpr std::size_t kNoMarker = std::numeric_limits<std::size_t>::max();
std::size_t find_music_marker(const std::span<const std::byte>& b) {
    static const std::uint8_t kMarker[8] = {'!', 'I', 'D', '!', '!', 'I', 'D', '!'};
    for (std::size_t i = 0; i + 8 <= b.size(); ++i) {
        bool match = true;
        for (int k = 0; k < 8; ++k)
            if (U8(b.data() + i + k) != kMarker[k]) {
                match = false;
                break;
            }
        if (match)
            return i;
    }
    return kNoMarker;
}

}  // namespace

Result<AudioBank> AudioBank::load(const std::filesystem::path& audiohed_path,
                                  const std::filesystem::path& audiot_path) {
    Error io_err{};
    auto ah = read_file(audiohed_path, io_err);
    if (io_err.valid())
        return std::unexpected(io_err);
    io_err = {};
    auto at = read_file(audiot_path, io_err);
    if (io_err.valid())
        return std::unexpected(io_err);

    const auto ahb = std::span(ah);
    const auto atb = std::span(at);
    if (ahb.size() < 4 || ahb.size() % 4 != 0)
        return std::unexpected(make_error(Error::Kind::Corrupt, "audiohed: bad size"));

    AudioBank out;
    const std::size_t n = ahb.size() / 4;
    std::vector<std::uint32_t> offs(n);
    for (std::size_t i = 0; i < n; ++i) {
        Reader r{ahb.subspan(4 * i, 4)};
        if (!r.read_le(offs[i]))
            return std::unexpected(make_error(Error::Kind::Corrupt, "audiohed: truncated"));
    }
    if (n > 0 && offs[0] != 0)
        return std::unexpected(make_error(Error::Kind::Corrupt, "audiohed: first offset != 0"));

    out.raw_chunks.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto s = offs[i];
        const auto e = (i + 1 < n) ? offs[i + 1] : static_cast<std::uint32_t>(at.size());
        if (s > e || e > at.size())
            return std::unexpected(
                make_error(Error::Kind::Corrupt, "audiohed: offsets not ascending"));
        out.raw_chunks.emplace_back(s, static_cast<std::size_t>(e - s));
    }

    out.fm.assign(n, std::nullopt);

    // The music region opens at the "!ID!!ID!" marker; FM effects all lie
    // before it. Cap the effect classifier there so music tracks (whose first
    // u32 is a large register-stream length) are not misread as effects.
    const std::size_t marker = find_music_marker(atb);
    const std::size_t fm_limit = (marker == kNoMarker) ? atb.size() : marker;

    // Classify and parse every chunk by shape.
    for (std::size_t i = 0; i < n; ++i) {
        const auto [s, len] = out.raw_chunks[i];
        if (len < 7)
            continue;  // empty slot
        const auto* c = atb.data() + s;
        const auto v0 = static_cast<std::uint32_t>(
            U8(c) | (U8(c + 1) << 8) | (U8(c + 2) << 16) | (U8(c + 3) << 24));

        // PCM SFX: [u32 ns][u16 vol 0..100][ns x u8][u8 flag]  =>  len == ns + 7.
        // Only the leading contiguous run (i == sfx.size()) is PCM; anything
        // later is an FM effect, the "unknown" table chunk, or padding.
        const bool isPcm =
            (i == out.sfx.size()) && (v0 + 7 == len) && (len >= 7) &&
            (static_cast<std::uint16_t>(U8(c + 4) | (U8(c + 5) << 8)) <= 100);
        if (isPcm) {
            Sfx sfx;
            sfx.nsamples = v0;
            sfx.volume = static_cast<std::uint16_t>(U8(c + 4) | (U8(c + 5) << 8));
            sfx.flag = U8(c + 6 + v0);
            sfx.samples.resize(v0);
            std::memcpy(sfx.samples.data(), c + 6, v0);
            out.sfx.push_back(std::move(sfx));
            out.sfx_count = i + 1;
            continue;
        }

        // FM effect: [u32 nl][u16 prio][16 inst][u8 block][nl notes][name]
        //             =>  nl > 0 and 23 + nl <= len, and before the music region.
        if (s < fm_limit && v0 > 0 && v0 < 0x10000 && len >= 24 && 23 + v0 <= len) {
            std::span<const std::byte> chunk(c, len);
            auto res = imf::parseFx(chunk);
            if (res) {
                out.fm[i] = std::move(*res);
                ++out.fm_count;
            }
            continue;
        }
        // Otherwise: the one-per-file "unknown" table chunk, or padding — skip.
    }

    if (out.sfx.empty())
        return std::unexpected(
            make_error(Error::Kind::Corrupt, "audiot: no PCM SFX-shaped entries"));

    // Music: walk the contiguous region after the "!ID!!ID!" marker.
    auto music = imf::parseMusicRegion(atb);
    if (!music)
        return std::unexpected(music.error());
    out.music = std::move(*music);

    return out;
}

}  // namespace w3d
