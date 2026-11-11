// w3d/error.hpp — non-throwing error reporting (std::expected-based).
#pragma once

#include <expected>
#include <string>

namespace w3d {

struct Error {
    enum class Kind {
        Io,          // file read failure
        Corrupt,     // magic/size/consistency check failed
        OutOfRange,  // value outside a documented range
        NotImplemented,
    } kind;
    std::string message;
    [[nodiscard]] bool valid() const noexcept { return !message.empty(); }
};

template <typename T>
using Result = std::expected<T, Error>;

[[nodiscard]] inline Error make_error(Error::Kind k, std::string msg) {
    return {k, std::move(msg)};
}

} // namespace w3d
