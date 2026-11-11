// w3d/audio.cpp — implementation.
#include "w3d/audio.hpp"

#include <fstream>
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

} // namespace

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
            return std::unexpected(make_error(Error::Kind::Corrupt, "audiohed: offsets not ascending"));
        out.raw_chunks.emplace_back(s, static_cast<std::size_t>(e - s));
    }

    // Parse the SFX region: [u32 ns][u16 vol 0..100][ns x u8][u8 flag].
    for (std::size_t i = 0; i < n; ++i) {
        const auto [s, len] = out.raw_chunks[i];
        if (len < 7 || len > 100007)
            break;
        const auto* c = atb.data() + s;
        std::uint32_t ns = 0;
        std::uint16_t vol = 0;
        Reader cr{atb.subspan(s, len)};
        if (!cr.read_le(ns) || !cr.read_le(vol))
            break;
        if (vol > 100 || static_cast<std::size_t>(ns) + 7 != len)
            break;
        Sfx sfx;
        sfx.nsamples = ns;
        sfx.volume = vol;
        sfx.flag = static_cast<std::uint8_t>(c[6 + ns]);
        sfx.samples.resize(ns);
        for (std::uint32_t k = 0; k < ns; ++k)
            sfx.samples[k] = static_cast<std::uint8_t>(c[6 + k]);
        out.sfx.push_back(std::move(sfx));
        out.sfx_count = i + 1;
    }
    if (out.sfx_count == 0)
        return std::unexpected(make_error(Error::Kind::Corrupt, "audiot: no SFX-shaped entries"));
    return out;
}

} // namespace w3d
