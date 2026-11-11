// w3d/maps.cpp — implementation.
#include "w3d/maps.hpp"

#include <cstring>
#include <fstream>
#include <span>

#include "w3d/carmack.hpp"
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

constexpr std::array<std::byte, 10> kPlane2Blob{
    static_cast<std::byte>(0x08), static_cast<std::byte>(0x00),
    static_cast<std::byte>(0x00), static_cast<std::byte>(0x20),
    static_cast<std::byte>(0xCD), static_cast<std::byte>(0xAB),
    static_cast<std::byte>(0x00), static_cast<std::byte>(0x10),
    static_cast<std::byte>(0x00), static_cast<std::byte>(0x00)};

struct RawHeader {
    std::uint32_t p0o = 0, p1o = 0, p2o = 0;
    std::uint16_t p0s = 0, p1s = 0, p2s = 0;
    std::uint16_t w = 0, h = 0;
    std::string name;
};

} // namespace

Result<GameMaps> GameMaps::load(const std::filesystem::path& gamemaps_path,
                                const std::filesystem::path& maphead_path) {
    Error io_err{};
    auto gm = read_file(gamemaps_path, io_err);
    if (io_err.valid())
        return std::unexpected(io_err);
    io_err = {};
    auto mh = read_file(maphead_path, io_err);
    if (io_err.valid())
        return std::unexpected(io_err);

    const auto bytes = std::span(gm);
    const auto mbytes = std::span(mh);

    if (gm.size() < 11 || std::memcmp(bytes.data(), "TED5v1.0", 8) != 0)
        return std::unexpected(make_error(Error::Kind::Corrupt, "GAMEMAPS: bad magic"));
    if (mbytes.size() < 2 + 100 * 4)
        return std::unexpected(make_error(Error::Kind::Corrupt, "MAPHEAD: too small"));

    Reader mr{mbytes};
    std::uint16_t magic = 0;
    if (!mr.read_le(magic) || magic != 0xABCD)
        return std::unexpected(make_error(Error::Kind::Corrupt, "MAPHEAD: bad magic"));

    GameMaps out;
    for (int slot = 0; slot < 100; ++slot) {
        std::uint32_t off = 0;
        if (!mr.read_le(off))
            return std::unexpected(make_error(Error::Kind::Corrupt, "MAPHEAD: truncated"));
        if (off == 0)
            continue;
        if (off + 42 > gm.size())
            return std::unexpected(make_error(Error::Kind::Corrupt, "MAPHEAD: header offset out of range"));

        Reader hr{bytes.subspan(off, 42)};
        RawHeader rh;
        if (!hr.read_le(rh.p0o) || !hr.read_le(rh.p1o) || !hr.read_le(rh.p2o) ||
            !hr.read_le(rh.p0s) || !hr.read_le(rh.p1s) || !hr.read_le(rh.p2s) ||
            !hr.read_le(rh.w) || !hr.read_le(rh.h))
            return std::unexpected(make_error(Error::Kind::Corrupt, "map header: truncated"));
        // Name is a fixed 16-byte, NUL-padded field.
        std::array<std::byte, 16> namefield{};
        if (!hr.take(namefield))
            return std::unexpected(make_error(Error::Kind::Corrupt, "map header: truncated"));
        for (const auto b : namefield) {
            if (b == std::byte{0}) break;
            rh.name.push_back(static_cast<char>(b));
        }
        std::array<std::byte, 4> tag{};
        if (!hr.take(tag) || std::memcmp(tag.data(), "!ID!", 4) != 0)
            return std::unexpected(make_error(Error::Kind::Corrupt, "map header: bad '!ID!' tag"));
        if (rh.p2s != 10 || rh.p2o + 10 > gm.size())
            return std::unexpected(make_error(Error::Kind::Corrupt, "map: bad plane-2 descriptor"));
        if (std::memcmp(bytes.data() + rh.p2o, kPlane2Blob.data(), 10) != 0)
            return std::unexpected(make_error(Error::Kind::Corrupt, "map: plane-2 blob mismatch"));
        if (rh.w != kMapSide || rh.h != kMapSide)
            return std::unexpected(make_error(Error::Kind::OutOfRange, "map: unexpected dimensions"));

        Map map;
        map.name = std::move(rh.name);
        map.width = rh.w;
        map.height = rh.h;

        auto decode_plane = [&](std::uint32_t o, std::uint16_t s,
                                std::array<std::uint16_t, kMapPlaneCells>& dst) -> Result<void> {
            if (o + s > gm.size())
                return std::unexpected(make_error(Error::Kind::Corrupt, "map: plane offset out of range"));
            auto r = plane_decode(bytes.subspan(o, s), 2 * static_cast<std::uint16_t>(kMapPlaneCells));
            if (!r)
                return std::unexpected(r.error());
            for (std::size_t i = 0; i < kMapPlaneCells; ++i)
                dst[i] = static_cast<std::uint16_t>(
                    static_cast<std::uint8_t>((*r)[2 * i]) |
                    (static_cast<std::uint8_t>((*r)[2 * i + 1]) << 8));
            return {};
        };

        auto e0 = decode_plane(rh.p0o, rh.p0s, map.plane0);
        if (!e0)
            return std::unexpected(e0.error());
        auto e1 = decode_plane(rh.p1o, rh.p1s, map.plane1);
        if (!e1)
            return std::unexpected(e1.error());

        out.maps.push_back(std::move(map));
    }
    return out;
}

} // namespace w3d
