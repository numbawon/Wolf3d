// src/util/bmp.hpp — minimal 24-bit BMP writer (no dependencies).
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace w3d::util {

// Writes an RGBA8 image as a 24-bit BMP (top-to-bottom source rows).
[[nodiscard]] bool write_bmp(const std::filesystem::path& path, uint32_t w, uint32_t h,
                             const uint8_t* rgba);

} // namespace w3d::util
