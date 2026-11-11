// w3d/carmack.cpp — implementation (see carmack.hpp for the format notes).
#include "w3d/carmack.hpp"

#include <algorithm>

namespace w3d {
namespace {

std::uint16_t le16(const std::byte* p) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint8_t>(p[0]) | (static_cast<std::uint8_t>(p[1]) << 8));
}

void push16(std::vector<std::byte>& out, std::uint16_t v) {
    out.push_back(static_cast<std::byte>(v & 0xFF));
    out.push_back(static_cast<std::byte>(v >> 8));
}

} // namespace

Result<std::vector<std::byte>> carmack_decode(std::span<const std::byte> in) {
    if (in.size() < 2)
        return std::unexpected(make_error(Error::Kind::Corrupt, "carmack: stream shorter than total word"));
    const auto* p = in.data();
    const std::size_t n = in.size();
    const std::size_t total = le16(p);
    std::vector<std::byte> out;
    out.reserve(total);
    std::size_t i = 2;
    while (i + 2 <= n && out.size() < total) {
        const auto x = static_cast<std::uint8_t>(p[i]);
        const auto y = static_cast<std::uint8_t>(p[i + 1]);
        i += 2;
        if (y == 0xA7 || y == 0xA8) {
            if (x == 0) {
                // Pointer bytes stored verbatim (escaped).
                if (i >= n)
                    return std::unexpected(make_error(Error::Kind::Corrupt, "carmack: truncated escaped pointer"));
                out.push_back(p[i]);
                out.push_back(static_cast<std::byte>(y));
                ++i;
                continue;
            }
            if (y == 0xA7) {
                // Near pointer: z in WORD units behind the end of the output.
                if (i >= n)
                    return std::unexpected(make_error(Error::Kind::Corrupt, "carmack: truncated near pointer"));
                const auto z = static_cast<std::size_t>(static_cast<std::uint8_t>(p[i]));
                ++i;
                if (out.size() < 2 * z)
                    return std::unexpected(make_error(Error::Kind::Corrupt, "carmack: near pointer out of range"));
                auto src = out.size() - 2 * z;
                for (std::uint8_t k = 0; k < x; ++k) {
                    out.push_back(out[src]);
                    out.push_back(out[src + 1]);
                    src += 2;
                }
            } else {
                // Far pointer: u16 w, word at 2*w, w increments per copy.
                if (i + 2 > n)
                    return std::unexpected(make_error(Error::Kind::Corrupt, "carmack: truncated far pointer"));
                auto w = static_cast<std::size_t>(le16(p + i));
                i += 2;
                if (2 * (w + x) > out.size())
                    return std::unexpected(make_error(Error::Kind::Corrupt, "carmack: far pointer out of range"));
                for (std::uint8_t k = 0; k < x; ++k) {
                    out.push_back(out[2 * w]);
                    out.push_back(out[2 * w + 1]);
                    ++w;
                }
            }
        } else {
            out.push_back(static_cast<std::byte>(x));
            out.push_back(static_cast<std::byte>(y));
        }
    }
    if (out.size() < total)
        return std::unexpected(make_error(Error::Kind::Corrupt, "carmack: stream ended before declared total"));
    out.resize(total);
    return out;
}

Result<std::vector<std::byte>> rlew_decode(std::span<const std::byte> in, std::uint16_t tag) {
    if (in.size() < 2)
        return std::unexpected(make_error(Error::Kind::Corrupt, "rlew: stream shorter than total word"));
    const auto* p = in.data();
    const std::size_t n = in.size();
    const std::size_t total = le16(p);
    if (total % 2 != 0)
        return std::unexpected(make_error(Error::Kind::Corrupt, "rlew: declared total is not word-aligned"));
    std::vector<std::byte> out;
    out.reserve(total);
    std::size_t i = 2;
    while (i + 2 <= n && out.size() < total) {
        const auto w = le16(p + i);
        i += 2;
        if (w == tag) {
            if (i + 4 > n)
                return std::unexpected(make_error(Error::Kind::Corrupt, "rlew: truncated run"));
            const auto count = le16(p + i);
            const auto value = le16(p + i + 2);
            i += 4;
            for (std::uint16_t k = 0; k < count; ++k)
                push16(out, value);
        } else {
            push16(out, w);
        }
    }
    if (out.size() < total)
        return std::unexpected(make_error(Error::Kind::Corrupt, "rlew: stream ended before declared total"));
    out.resize(total);
    return out;
}

Result<std::vector<std::byte>> plane_decode(std::span<const std::byte> compressed,
                                            std::uint16_t expect_bytes) {
    auto c = carmack_decode(compressed);
    if (!c)
        return std::unexpected(c.error());
    auto r = rlew_decode(*c);
    if (!r)
        return std::unexpected(r.error());
    if (expect_bytes != 0 && r->size() != expect_bytes) {
        return std::unexpected(make_error(Error::Kind::Corrupt, "plane: decoded size mismatch"));
    }
    return std::move(*r);
}

} // namespace w3d
