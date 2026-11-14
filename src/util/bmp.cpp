// src/util/bmp.cpp — minimal 24-bit BMP writer.
#include "bmp.hpp"

#include <cstdio>
#include <fstream>

namespace w3d::util {

namespace {
// Standard BMP layout: 'BM' @0, file size @2, reserved @6/8, data offset @10,
// 40-byte DIB @14 (total 54). Packed so the size field sits at byte 2 with
// no padding (a 4-aligned size after the 2-byte tag would shift the whole
// header and leave two uninitialized bytes in the file).
#pragma pack(push, 1)
struct BmpHeader {
    uint16_t tag = 0x4D42; // "BM"
    uint32_t size = 0;
    uint16_t reserved0 = 0;
    uint16_t reserved1 = 0;
    uint32_t offset = 0;
    uint32_t info_size = 40;
    int32_t width = 0;
    int32_t height = 0; // positive = bottom-up
    uint16_t planes = 1;
    uint16_t bpp = 24;
    uint32_t compression = 0;
    uint32_t image_size = 0;
    int32_t x_ppm = 0;
    int32_t y_ppm = 0;
    uint32_t colors_used = 0;
    uint32_t colors_important = 0;
};
#pragma pack(pop)
} // namespace

bool write_bmp(const std::filesystem::path& path, uint32_t w, uint32_t h,
               const uint8_t* rgba) {
    const uint32_t row_bytes = (w * 3 + 3) & ~3u;
    std::vector<uint8_t> pixels(static_cast<size_t>(row_bytes) * h);
    for (uint32_t y = 0; y < h; ++y) {
        // BMP rows are bottom-up.
        uint8_t* dst = pixels.data() + static_cast<size_t>(h - 1 - y) * row_bytes;
        const uint8_t* src = rgba + static_cast<size_t>(y) * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 2]; // B
            dst[x * 3 + 1] = src[x * 4 + 1]; // G
            dst[x * 3 + 2] = src[x * 4 + 0]; // R
        }
    }
    BmpHeader hdr;
    hdr.image_size = row_bytes * h;
    hdr.size = static_cast<uint32_t>(sizeof(BmpHeader) + hdr.image_size);
    hdr.offset = sizeof(BmpHeader);
    hdr.width = static_cast<int32_t>(w);
    hdr.height = static_cast<int32_t>(h);

    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    f.write(reinterpret_cast<const char*>(pixels.data()),
            static_cast<std::streamsize>(pixels.size()));
    return static_cast<bool>(f);
}

} // namespace w3d::util
