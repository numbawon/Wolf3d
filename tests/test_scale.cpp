// tests/test_scale.cpp — WS3 scale-math unit tests.
//
// The internal render scale S (1..4) parameterizes the CPU frame build
// (frame.cpp): W = 320*S columns, wall height 160*S, pixel space
// (320*S, 200*S). These tests pin the cross-scale invariants that make
// the S=1 pixel-diff baseline (scratch w3d_ref workflow, 0/64000)
// meaningful:
//   - clamp: scale 0 -> 1, 99 -> 4 (kMaxScale);
//   - even-column identity: at S=2, internal column 2i casts exactly the
//     same ray as S=1 column i (shift = fov*((2i)<<1 - 640)/640 ==
//     fov*(i<<1 - 320)/320 in exact integer/double arithmetic), so t, tx,
//     tex are bit-identical and h is exactly doubled (wall_height scales
//     by S: fl(320/(2t)) == 2*fl(160/(2t)), IEEE scaling by 2 commutes
//     with round-to-nearest);
//   - sprite geometry: th, s, and the weapon's x0 double exactly (IEEE
//     scaling by 2 commutes with round-to-nearest); a thing's x0 lands on
//     {2*x0, 2*x0+1} and y0 on {2*y0, 2*y0+1} by the trunc parity, and
//     dist / sprite index are scale-independent;
//   - the weapon record (no truncation ambiguity) doubles exactly.
// S=1 itself is pinned by the full-frame pixel-diff harness.

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "game/level.hpp"
#include "game/sim.hpp"
#include "render/frame.hpp"

using namespace w3d;
using namespace w3d::game;
using w3d::render::Frame;
using w3d::render::Pose;

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
            std::printf("  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__,         \
                        g_current, #cond);                                     \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!((a) == (b))) {                                                   \
            ++g_failures;                                                      \
            std::printf("  FAIL %s:%d [%s] %s != %s\n", __FILE__, __LINE__,   \
                        g_current, #a, #b);                                    \
        }                                                                      \
    } while (0)

// A 17x17 room (x 4..20, y 4..20) in a wall box, player at (6.5, 12.5)
// facing east, with a guard 5.5 cells ahead and two ammo clips off-axis.
Simulation make_sim() {
    Map m;
    m.name = "scale-synthetic";
    for (auto& v : m.plane0)
        v = 1;
    for (int y = 4; y <= 20; ++y)
        for (int x = 4; x <= 20; ++x)
            m.plane0[static_cast<std::size_t>(y) * 64 + x] = 106;
    m.plane1[static_cast<std::size_t>(12) * 64 + 6] = 20;    // start, east
    m.plane1[static_cast<std::size_t>(12) * 64 + 12] = 108;  // guard (50)
    m.plane1[static_cast<std::size_t>(11) * 64 + 10] = 49;   // ammo (28)
    m.plane1[static_cast<std::size_t>(13) * 64 + 14] = 49;   // ammo (28)
    return Simulation(Level::from_map(m));
}

Frame build(const Simulation& sim, std::uint32_t scale) {
    Frame f{};
    const Pose pose{sim.x(), sim.y(), sim.dx(), sim.dy()};
    std::array<Frame::ActorSnap, w3d::render::kMaxSprites> snap{};
    w3d::render::build_frame(f, sim, pose, snap, 0, 0, 0, scale);
    return f;
}

void test_scale_constants() {
    g_current = "scale_constants";
    CHECK_EQ(w3d::render::kMaxScale, 4);
    CHECK_EQ(w3d::render::kColumns * w3d::render::kMaxScale, 1280);
    CHECK_EQ(w3d::render::kRows * w3d::render::kMaxScale, 800);
    CHECK(w3d::render::kWallHeight == 160.0);
}

void test_scale_clamp() {
    g_current = "scale_clamp";
    const Simulation sim = make_sim();
    const Frame f0 = build(sim, 0);
    CHECK_EQ(f0.scale, 1u);
    const Frame f99 = build(sim, 99);
    CHECK_EQ(f99.scale, 4u);
    const Frame f2 = build(sim, 2);
    CHECK_EQ(f2.scale, 2u);
    const Frame f4 = build(sim, 4);
    CHECK_EQ(f4.scale, 4u);
}

void test_columns_even_columns() {
    g_current = "columns_even_columns";
    const Simulation sim = make_sim();
    const Frame f1 = build(sim, 1);
    const Frame f2 = build(sim, 2);
    for (int i = 0; i < w3d::render::kColumns; ++i) {
        const auto& a = f1.columns[static_cast<std::size_t>(i)];
        const auto& b =
            f2.columns[static_cast<std::size_t>(2 * i)];
        CHECK(b.t == a.t);
        CHECK(b.h == 2.0 * a.h);  // wall_height scales by S (exact IEEE x2)
        CHECK(b.tx == a.tx);
        CHECK_EQ(b.tex, a.tex);
    }
}

void test_sprites_scale() {
    g_current = "sprites_scale";
    const Simulation sim = make_sim();
    const Frame f1 = build(sim, 1);
    const Frame f2 = build(sim, 2);
    CHECK_EQ(f2.sprite_count, f1.sprite_count);
    CHECK(f1.sprite_count >= 3);  // weapon + the two in-view clips
    for (int j = 0; j < f1.sprite_count; ++j) {
        const auto& a = f1.sprites[static_cast<std::size_t>(j)];
        const auto& b = f2.sprites[static_cast<std::size_t>(j)];
        CHECK(b.dist == a.dist);  // world distance: scale independent
        CHECK_EQ(b.sprite, a.sprite);
        CHECK(b.th == 2.0 * a.th);
        CHECK(b.s == 2.0 * a.s);
        if (j == 0) {
            CHECK(b.y0 == 2.0 * a.y0);  // weapon: exact (H - h) form
            CHECK(b.x0 == 2.0 * a.x0);  // weapon: exact (integers only)
        } else {
            // x0 = trunc(v), v2 == 2*v1 exactly (IEEE x2 commutes with
            // rounding): trunc(2v) is 2*trunc(v) or one more (trunc parity
            // of the fractional part).
            CHECK(b.x0 == 2.0 * a.x0 || b.x0 == 2.0 * a.x0 + 1.0);
            // trunc((H - h)/2) with (H, h) both doubled: the result is the
            // exact double or one row up (the +1 is the trunc parity).
            CHECK(b.y0 == 2.0 * a.y0 || b.y0 == 2.0 * a.y0 + 1.0);
        }
    }
}

Register r_constants("scale_constants", test_scale_constants);
Register r_clamp("scale_clamp", test_scale_clamp);
Register r_columns("columns_even_columns", test_columns_even_columns);
Register r_sprites("sprites_scale", test_sprites_scale);

}  // namespace

int main() {
    for (const auto& c : cases())
        c.fn();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
