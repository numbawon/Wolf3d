// tests/test_formats.cpp — Phase 1 golden tests for the w3d data layer.
//
// Golden values were computed by an independent Python reference implementation
// (see docs/formats.md) from the same game files and an external reference
// sprite render. Game data location: W3D_GAME_DIR environment variable,
// falling back to the Steam install path baked in at configure time.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "w3d/audio.hpp"
#include "w3d/carmack.hpp"
#include "w3d/maps.hpp"
#include "w3d/palette.hpp"
#include "w3d/vswap.hpp"

namespace {

struct Case {
    const char* name;
    void (*fn)();
};
std::vector<Case>& cases() {
    static std::vector<Case> c;
    return c;
}
struct Register {
    Register(const char* name, void (*fn)()) { cases().push_back({name, fn}); }
};

int g_checks = 0;
int g_failures = 0;
const char* g_current = "?";

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__,          \
                        g_current, #cond);                                     \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!((a) == (b))) {                                                   \
            ++g_failures;                                                      \
            std::printf("  FAIL %s:%d [%s] %s != %s\n", __FILE__, __LINE__,    \
                        g_current, #a, #b);                                    \
        }                                                                      \
    } while (0)

std::filesystem::path game_dir() {
    if (const char* env = std::getenv("W3D_GAME_DIR"))
        return env;
#ifdef W3D_GAME_DIR_DEFAULT
    return W3D_GAME_DIR_DEFAULT;
#else
    return "C:/Program Files (x86)/Steam/steamapps/common/Wolfenstein 3D";
#endif
}

std::vector<std::byte> to_bytes(const std::vector<int>& v) {
    std::vector<std::byte> b(v.size());
    for (std::size_t i = 0; i < v.size(); ++i)
        b[i] = static_cast<std::byte>(v[i]);
    return b;
}

} // namespace

// ---------------------------------------------------------------- carmack

static void test_carmack_rlew_synthetic() {
    g_current = "carmack_rlew_synthetic";
    // Build an RLEW payload: words [1,2] then run(3 x 0x0005) then 0x0009.
    // -> bytes: 0c 00 01 00 02 00 cd ab 03 00 05 00 09 00 ; total = 6 words = 12 bytes
    std::vector<int> rlew{0x0001, 0x0002, 0xABCD, 3, 0x0005, 0x0009};
    std::vector<std::byte> rraw;
    rraw.push_back(static_cast<std::byte>(12 & 0xFF));
    rraw.push_back(static_cast<std::byte>(12 >> 8));
    for (int w : rlew) {
        rraw.push_back(static_cast<std::byte>(w & 0xFF));
        rraw.push_back(static_cast<std::byte>(w >> 8));
    }
    auto r = w3d::rlew_decode(std::span(rraw));
    CHECK(r.has_value());
    if (!r)
        return;
    // decoded words: 1,2,5,5,5,9
    const int expect[] = {1, 2, 5, 5, 5, 9};
    for (int i = 0; i < 6; ++i)
        CHECK_EQ(static_cast<int>((*r)[2 * i]) | (static_cast<int>((*r)[2 * i + 1]) << 8),
                 expect[i]);

    // Carmack: total=10; literal pair (0x11,0x22); near pointer x=2,z=1 (copy
    // the first word twice); literal (0x33,0x44); escaped pointer (0,0xA7,0x99).
    // (The total word itself occupies two bytes: 0x0a 0x00.)
    std::vector<int> carraw{0x0a, 0x00, 0x11, 0x22, 0x02, 0xA7, 0x01, 0x33, 0x44, 0x00, 0xA7, 0x99};
    std::vector<std::byte> craw;
    for (int w : carraw)
        craw.push_back(static_cast<std::byte>(w & 0xFF));
    auto c = w3d::carmack_decode(std::span(craw));
    CHECK(c.has_value());
    if (!c)
        return;
    CHECK_EQ(c->size(), 10u);
    CHECK_EQ(static_cast<int>((*c)[0]), 0x11);
    CHECK_EQ(static_cast<int>((*c)[1]), 0x22);
    CHECK_EQ(static_cast<int>((*c)[2]), 0x11); // near pointer copy 1
    CHECK_EQ(static_cast<int>((*c)[3]), 0x22);
    CHECK_EQ(static_cast<int>((*c)[4]), 0x11); // near pointer copy 2
    CHECK_EQ(static_cast<int>((*c)[5]), 0x22);
    CHECK_EQ(static_cast<int>((*c)[6]), 0x33);
    CHECK_EQ(static_cast<int>((*c)[7]), 0x44);
    CHECK_EQ(static_cast<int>((*c)[8]), 0x99); // escaped pointer z
    CHECK_EQ(static_cast<int>((*c)[9]), 0xA7);
}

static Register r_carmack("carmack_rlew_synthetic", test_carmack_rlew_synthetic);

// ------------------------------------------------------------------- maps

static void test_maps_main() {
    g_current = "maps_main";
    const auto g = game_dir();
    auto res = w3d::GameMaps::load(g / "base" / "GAMEMAPS.WL6",
                                   g / "base" / "MAPHEAD.WL6");
    CHECK(res.has_value());
    if (!res) {
        std::printf("  (load error: %s)\n", res.error().message.c_str());
        return;
    }
    auto& m = *res;
    CHECK_EQ(m.maps.size(), 60u);
    auto& first = m.maps[0];
    CHECK_EQ(first.name, std::string("Wolf1 Map1"));
    CHECK_EQ(first.plane0[0], 1u);
    CHECK_EQ(first.plane0[500], 1u);
    CHECK_EQ(first.plane0[1000], 12u);
    CHECK_EQ(first.plane0[1500], 110u);
    CHECK_EQ(first.plane0[2500], 2u);
    CHECK_EQ(first.plane0[3000], 118u);
    CHECK_EQ(first.plane0[3500], 1u);
    CHECK_EQ(first.plane1[1500], 49u);
    unsigned mx0 = 0, mx1 = 0;
    for (auto& mp : m.maps) {
        for (auto v : mp.plane0)
            mx0 = std::max<unsigned>(mx0, v);
        for (auto v : mp.plane1)
            mx1 = std::max<unsigned>(mx1, v);
    }
    CHECK_EQ(mx0, 143u);  // documented value range
    CHECK_EQ(mx1, 259u);  // includes 256..259 markers
    CHECK_EQ(m.maps[1].name, std::string("Wolf1 Map2"));
}

static void test_maps_sod() {
    g_current = "maps_sod";
    const auto g = game_dir();
    auto res = w3d::GameMaps::load(g / "base/m1" / "GAMEMAPS.SOD",
                                   g / "base/m1" / "MAPHEAD.SOD");
    CHECK(res.has_value());
    if (!res) {
        std::printf("  (load error: %s)\n", res.error().message.c_str());
        return;
    }
    auto& m = *res;
    CHECK_EQ(m.maps.size(), 21u);
    CHECK_EQ(m.maps[0].name, std::string("Tunnels 1"));
}

static Register r_maps_main("maps_main", test_maps_main);
static Register r_maps_sod("maps_sod", test_maps_sod);

// ------------------------------------------------------------------- vswap

static void test_vswap() {
    g_current = "vswap";
    const auto g = game_dir();
    auto res = w3d::Vswap::load(g / "base" / "VSWAP.WL6");
    CHECK(res.has_value());
    if (!res) {
        std::printf("  (load error: %s)\n", res.error().message.c_str());
        return;
    }
    auto& v = *res;
    CHECK_EQ(v.walls.size(), 106u);
    CHECK_EQ(v.sprites.size(), 436u);
    CHECK_EQ(v.sounds.size(), 121u);

    // Wall 0: golden is the first 16 bytes in file order = column 0, rows
    // 0..15. Our storage is row-major (idx[r*64+c]), so column 0 row i is
    // walls[0][i*64].
    const int expect[] = {30, 8, 8, 23, 18, 7, 20, 20, 22, 23, 25, 27, 31, 30, 29, 8};
    for (int i = 0; i < 16; ++i)
        CHECK_EQ(static_cast<int>(v.walls[0][static_cast<std::size_t>(i) * 64]), expect[i]);
    unsigned mx = 0;
    for (auto b : v.walls[0])
        mx = std::max<unsigned>(mx, b);
    CHECK_EQ(mx, 31u);

    // Sprite 110 (global chunk) = index 110-106 = 4 in the sprites vector.
    const auto& sp = v.sprites[110 - 106];
    CHECK_EQ(sp.first_col, 7u);
    CHECK_EQ(sp.last_col, 61u);
    unsigned content = 0;
    for (auto b : sp.idx)
        content += (b != 255);
    CHECK_EQ(content, 956u); // golden: pool fully consumed
}

static Register r_vswap("vswap", test_vswap);

// ------------------------------------------------------------------- audio

static void test_audio() {
    g_current = "audio";
    const auto g = game_dir();
    auto res = w3d::AudioBank::load(g / "base" / "AUDIOHED.WL6",
                                    g / "base" / "AUDIOT.WL6");
    CHECK(res.has_value());
    if (!res) {
        std::printf("  (load error: %s)\n", res.error().message.c_str());
        return;
    }
    auto& a = *res;
    CHECK_EQ(a.raw_chunks.size(), 289u);
    CHECK_EQ(a.sfx_count, 86u);
    CHECK_EQ(a.sfx.size(), 86u);
    CHECK_EQ(a.raw_chunks[0].second, 15u); // first chunk length
    CHECK_EQ(a.sfx[0].nsamples, 8u);
    CHECK_EQ(a.sfx[0].volume, 1u);
    CHECK_EQ(a.sfx[0].samples.size(), 8u);
    CHECK_EQ(a.sfx[1].nsamples, 6u);
    CHECK_EQ(a.sfx[1].volume, 50u);
    CHECK_EQ(a.sfx[2].nsamples, 9u);
    CHECK_EQ(a.sfx[3].nsamples, 51u);
    CHECK_EQ(a.sfx[3].volume, 10u);
    for (const auto& s : a.sfx)
        CHECK(s.volume <= 100);
}

static Register r_audio("audio", test_audio);

// ----------------------------------------------------------------- palette

static void test_palette() {
    g_current = "palette";
    auto black = w3d::palette_rgb(0);
    CHECK_EQ(black.r, 0u);
    CHECK_EQ(black.g, 0u);
    CHECK_EQ(black.b, 0u);
    // Entry 1 = 0xffa80000 → R=0x00,G=0x00,B=0xA8 (classic VGA dark blue).
    auto one = w3d::palette_rgb(1);
    CHECK_EQ(one.r, 0u);
    CHECK_EQ(one.g, 0u);
    CHECK_EQ(one.b, 0xA8u);
    auto four = w3d::palette_rgb(4); // 0xff0000a8 → dark red
    CHECK_EQ(four.r, 0xA8u);
    CHECK_EQ(four.g, 0u);
    CHECK_EQ(four.b, 0u);
    CHECK_EQ(w3d::palette_rgba_packed(1), 0xFF0000A8u);
    CHECK_EQ(w3d::palette_rgba_packed(4), 0xFFA80000u);
}

static Register r_palette("palette", test_palette);

// ------------------------------------------------------------- error paths

static void test_error_paths() {
    g_current = "error_paths";
    // Missing file.
    auto res = w3d::GameMaps::load(game_dir() / "nope.WL6", game_dir() / "nope2.WL6");
    CHECK(!res.has_value());
    if (!res)
        CHECK(res.error().kind == w3d::Error::Kind::Io);

    // Truncated vswap.
    auto junk = to_bytes(std::vector<int>{1, 2, 3});
    auto path = std::filesystem::temp_directory_path() / "w3d_test_vswap.tmp";
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(junk.data()), junk.size());
    }
    auto bad = w3d::Vswap::load(path);
    CHECK(!bad.has_value());
    std::filesystem::remove(path);
}

static Register r_errors("error_paths", test_error_paths);

int main() {
    std::printf("w3d Phase 1 golden tests — game dir: %s\n",
                game_dir().string().c_str());
    for (const auto& c : cases()) {
        const int checks_before = g_checks;
        const int failures_before = g_failures;
        c.fn();
        std::printf("[%s] %s (%d checks)\n",
                    failures_before == g_failures ? "PASS" : "FAIL",
                    c.name, g_checks - checks_before);
    }
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
