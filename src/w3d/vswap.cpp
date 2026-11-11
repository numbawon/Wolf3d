// w3d/vswap.cpp — implementation.
#include "w3d/vswap.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

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

std::uint16_t le16(const std::byte* p) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint8_t>(p[0]) | (static_cast<std::uint8_t>(p[1]) << 8));
}

Result<Sprite> decode_sprite(std::span<const std::byte> spr) {
    const std::size_t l = spr.size();
    if (l < 4)
        return std::unexpected(make_error(Error::Kind::Corrupt, "sprite: too small"));
    Sprite sp;
    const auto fc = le16(spr.data());
    const auto lc = le16(spr.data() + 2);
    if (lc < fc || fc > 63)
        return std::unexpected(make_error(Error::Kind::Corrupt, "sprite: bad column range"));
    const std::size_t ncols = static_cast<std::size_t>(lc - fc) + 1;
    const std::size_t table_end = 4 + 2 * ncols;
    if (table_end > l)
        return std::unexpected(make_error(Error::Kind::Corrupt, "sprite: column table past end"));
    std::vector<std::uint16_t> coloffs(ncols);
    for (std::size_t i = 0; i < ncols; ++i)
        coloffs[i] = le16(spr.data() + 4 + 2 * i);
    const std::size_t posts_start = coloffs[0];
    const std::size_t pool_start = table_end;
    if (posts_start < pool_start || posts_start > l)
        return std::unexpected(make_error(Error::Kind::Corrupt, "sprite: bad posts offset"));
    const std::byte* pool = spr.data() + pool_start;
    const std::size_t pool_len = posts_start - pool_start;

    std::fill(sp.idx.begin(), sp.idx.end(), 255u);
    sp.first_col = fc;
    sp.last_col = lc;

    std::size_t pix_i = 0;
    std::size_t pi = posts_start;
    for (std::uint16_t c = fc; c <= lc; ++c) {
        while (true) {
            if (pi + 2 > l)
                return std::unexpected(make_error(Error::Kind::Corrupt, "sprite: post table truncated"));
            const auto e1 = le16(spr.data() + pi);
            pi += 2;
            if (e1 == 0)
                break;  // column terminator
            if (pi + 4 > l)
                return std::unexpected(make_error(Error::Kind::Corrupt, "sprite: post entry truncated"));
            (void)le16(spr.data() + pi);  // m1: not used for counting
            const auto s1 = le16(spr.data() + pi + 2);
            pi += 4;
            const std::size_t rstart = s1 / 2;
            const std::size_t rend = e1 / 2;
            for (std::size_t r = rstart; r < rend; ++r) {
                if (pix_i >= pool_len || r >= 64)
                    return std::unexpected(make_error(Error::Kind::Corrupt, "sprite: pool/row overflow"));
                sp.idx[r * 64 + c] = static_cast<std::uint8_t>(pool[pix_i++]);
            }
        }
    }
    // The pool is padded with one 0x00 byte when the pixel count is odd
    // (2-byte alignment); the reference engine never reads the pad byte.
    if (pool_len < pix_i || pool_len - pix_i > 1 ||
        (pool_len - pix_i == 1 && pool[pool_len - 1] != std::byte{0}))
        return std::unexpected(make_error(Error::Kind::Corrupt, "sprite: bad pixel pool size"));
    if (pi != l)
        return std::unexpected(make_error(Error::Kind::Corrupt, "sprite: post table does not end at chunk end"));
    return sp;
}

} // namespace

Result<Vswap> Vswap::load(const std::filesystem::path& path) {
    Error io_err{};
    auto buf = read_file(path, io_err);
    if (io_err.valid())
        return std::unexpected(io_err);
    const auto data = std::span(buf);
    if (data.size() < 6)
        return std::unexpected(make_error(Error::Kind::Corrupt, "vswap: too small"));

    const auto n = le16(data.data());
    const auto first_sprite = le16(data.data() + 2);
    const auto first_sound = le16(data.data() + 4);
    if (static_cast<std::size_t>(n) * 6 + 6 > data.size())
        return std::unexpected(make_error(Error::Kind::Corrupt, "vswap: chunk table too large"));
    if (first_sprite > n || first_sound > n || first_sound < first_sprite)
        return std::unexpected(make_error(Error::Kind::Corrupt, "vswap: bad chunk ranges"));

    Vswap out;
    out.walls.reserve(first_sprite);
    out.sprites.reserve(first_sound - first_sprite);
    out.sounds.reserve(n - first_sound);

    for (std::size_t i = 0; i < n; ++i) {
        std::uint32_t off = 0;
        std::uint16_t len = 0;
        {
            Reader r{data.subspan(6 + 4 * i, 4)};
            if (!r.read_le(off))
                return std::unexpected(make_error(Error::Kind::Corrupt, "vswap: truncated offset"));
        }
        {
            Reader r{data.subspan(6 + 4 * n + 2 * i, 2)};
            if (!r.read_le(len))
                return std::unexpected(make_error(Error::Kind::Corrupt, "vswap: truncated length"));
        }
        if (static_cast<std::size_t>(off) + len > data.size())
            return std::unexpected(make_error(Error::Kind::Corrupt, "vswap: chunk out of range"));
        const auto chunk = data.subspan(off, len);

        if (i < first_sprite) {
            if (len != 4096)
                return std::unexpected(make_error(Error::Kind::Corrupt, "vswap: wall chunk not 4096 bytes"));
            std::array<std::uint8_t, 4096> rowmajor{};
            for (std::uint16_t c = 0; c < 64; ++c)
                for (std::uint16_t r = 0; r < 64; ++r)
                    rowmajor[r * 64 + c] = static_cast<std::uint8_t>(chunk[c * 64 + r]);
            out.walls.push_back(rowmajor);
        } else if (i < first_sound) {
            auto sp = decode_sprite(chunk);
            if (!sp)
                return std::unexpected(sp.error());
            out.sprites.push_back(std::move(*sp));
        } else {
            out.sounds.emplace_back(off, len);
        }
    }
    if (out.walls.size() != first_sprite ||
        out.sprites.size() != static_cast<std::size_t>(first_sound - first_sprite) ||
        out.sounds.size() != n - first_sound)
        return std::unexpected(make_error(Error::Kind::Corrupt, "vswap: chunk count mismatch"));
    return out;
}

} // namespace w3d
