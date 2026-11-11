// w3d/bytes.hpp — bounds-checked little-endian reader over a raw buffer.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace w3d {

// A view over a file's bytes with sequential, bounds-checked reads.
// Reads are little-endian; the source files are all LE.
class Reader {
public:
    explicit Reader(std::span<const std::byte> data) noexcept
        : p_(data.data()), begin_(data.data()), end_(data.data() + data.size()) {}

    [[nodiscard]] std::size_t remaining() const noexcept { return static_cast<std::size_t>(end_ - p_); }
    [[nodiscard]] std::size_t offset() const noexcept { return static_cast<std::size_t>(p_ - begin_); }
    [[nodiscard]] bool eof() const noexcept { return p_ == end_; }

    // N bytes raw; fails when remaining < N.
    [[nodiscard]] bool take(std::span<std::byte> out) {
        if (remaining() < out.size()) return false;
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = p_[i];
        p_ += out.size();
        return true;
    }
    [[nodiscard]] bool skip(std::size_t n) { return n <= remaining() ? (p_ += n, true) : false; }

    template <std::size_t N>
    [[nodiscard]] bool take(std::array<std::byte, N>& out) { return take(std::span<std::byte>{out}); }

    template <typename T>
    [[nodiscard]] bool read_le(T& out) {
        static_assert(std::is_integral_v<T>);
        std::array<std::byte, sizeof(T)> tmp{};
        if (!take(tmp)) return false;
        T v{};
        for (std::size_t i = 0; i < sizeof(T); ++i)
            v |= static_cast<T>(static_cast<std::uint8_t>(tmp[i])) << (8 * i);
        out = v;
        return true;
    }

    [[nodiscard]] bool read_cstr(std::string& out, std::size_t max) {
        out.clear();
        while (!eof() && out.size() < max) {
            const auto c = static_cast<std::uint8_t>(*p_);
            if (c == 0) { ++p_; return true; }
            out.push_back(static_cast<char>(c));
            ++p_;
        }
        return false;
    }

    // Peek without consuming: the next n bytes as a span (empty on failure).
    [[nodiscard]] std::span<const std::byte> peek(std::size_t n) const {
        const auto m = n < remaining() ? n : remaining();
        return {p_, m};
    }

private:
    const std::byte* p_;
    const std::byte* begin_;
    const std::byte* end_;
};

} // namespace w3d
