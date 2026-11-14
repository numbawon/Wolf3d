// src/render/vk_common.hpp — small Vulkan helpers: result checks, string utils.
#pragma once

#include <windows.h>

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace w3d::render {

[[noreturn]] void vk_fail(const char* what, VkResult r) {
    std::fprintf(stderr, "w3d: %s failed: VkResult=%d\n", what, static_cast<int>(r));
    std::abort();
}

// Load a file fully into memory (shader SPIR-V, etc.).
[[nodiscard]] inline std::vector<uint8_t> load_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "w3d: cannot open %s\n", p.string().c_str());
        std::abort();
    }
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Directory of the running executable (for locating bundled shaders).
[[nodiscard]] inline std::filesystem::path exe_dir() {
    char buf[4096];
    // GetModuleFileNameA takes an LPSTR; on Windows the wide form is preferred,
    // but the ANSI form is fine for ASCII paths.
    auto n = ::GetModuleFileNameA(nullptr, buf, sizeof(buf) - 1);
    buf[n] = '\0';
    std::filesystem::path p(buf);
    return p.parent_path();
}

} // namespace w3d::render
