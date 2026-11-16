// w3d render/frame.cpp — CPU-side scene build (reference DDA port).
//
// The column DDA below is a line-for-line port of the reference engine's
// drawWalls (js/engine.js) inner loop: same grid walk, same boundary
// comparisons, same double-precision operations in the same order, so the
// per-column records are bit-identical to the reference's. The sprite setup
// ports drawThings/drawSprite (engine.js) and Thing.update (game.js).
#include "render/frame.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

#include "game/actors.hpp"
#include "w3d/palette.hpp"

namespace w3d::render {
namespace {

using game::Simulation;

// JS ~~x (ToInt32) truncates toward zero; positions are >= 0 here.
inline double js_trunc(double x) { return std::trunc(x); }

// The reference reads out-of-range cells as `undefined`, which never blocks;
// the shipped maps are fully walled so this is unreachable. Terminate the
// ray on a synthetic wall (texture 1) instead of looping forever.
constexpr std::uint16_t kOobWall = 1;

struct WallTimerLookup {
    const Simulation::WallTimer* at(int x, int y) const {
        for (const auto& w : walls)
            if (w.x == x && w.y == y)
                return &w;
        return nullptr;
    }
    const std::vector<Simulation::WallTimer>& walls;
};

void build_columns(Frame& f, const Simulation& sim, std::uint32_t scale) {
    const WallTimerLookup wall_lookup{sim.pushwalls()};
    // WS3: the DDA runs one ray per internal column, over W = 320*scale
    // columns, with the reference wall height scaled by the same factor
    // (wallHeight = pixelWidth / (2*fov) = 160*scale). At scale 1 every
    // operand is exactly the reference 320x200 value (bit-identical).
    const int W = kColumns * static_cast<int>(scale);
    const double wall_height = kWallHeight * static_cast<double>(scale);

    for (int i = 0; i < W; ++i) {
        // current column position on the camera plane (reference: shift)
        const double shift = kFov * ((i << 1) - W) / W;
        // direction of the ray
        double dx = f.pdx - shift * f.pdy;
        double dy = f.pdy + shift * f.pdx;

        // direction in which the ray moves along each axis
        const int stepx = dx >= 0 ? 1 : -1;
        const int stepy = dy >= 0 ? 1 : -1;
        // take absolute values of ray direction
        dx = stepx * dx;
        dy = stepy * dy;
        // cell position of the ray on the map (starting from the player)
        int cx = static_cast<int>(js_trunc(f.px));
        int cy = static_cast<int>(js_trunc(f.py));
        // remaining fractional distance to the next cell (0 < rfx, rfy <= 1)
        const double fx = f.px - std::floor(f.px);
        const double fy = f.py - std::floor(f.py);
        double rfx = stepx > 0 ? 1 - fx : fx;
        if (rfx == 0) {
            rfx = 1;
            cx += stepx;
        }
        double rfy = stepy > 0 ? 1 - fy : fy;
        if (rfy == 0) {
            rfy = 1;
            cy += stepy;
        }

        // total time traveled by the ray
        double t = 0;

        // index of tile to display
        int texture_index = 0;
        // coordinate on the wall tile where the ray hit (0 <= tx < 1)
        double tx = 0;

        auto map0 = [&](int x, int y) -> std::uint16_t {
            if (x < 0 || y < 0 || x >= 64 || y >= 64)
                return kOobWall;
            return f.plane0[static_cast<std::size_t>(y) * 64 + x];
        };
        auto map1 = [&](int x, int y) -> std::uint16_t {
            if (x < 0 || y < 0 || x >= 64 || y >= 64)
                return 0;
            return f.plane1[static_cast<std::size_t>(y) * 64 + x];
        };

        while (true) {
            const std::uint16_t m0 = map0(cx, cy);
            if (m0 <= 63) {
                // hit a wall
                double wall_shift = 0;
                if (map1(cx, cy) == 98) {
                    // pushwall
                    const auto* timer = wall_lookup.at(cx, cy);
                    if (timer != nullptr) {
                        wall_shift = timer->t / 64.0;
                        if (timer->dx != 0) {
                            // wall moves horizontally
                            if (dx * rfy >= dy * wall_shift) {
                                // ray hits wall
                                const double dt = wall_shift / dx;
                                t += dt;
                                rfy -= dt * dy;
                                rfx -= wall_shift;
                            } else {
                                // ray moves to next cell
                                const double dt = rfy / dy;
                                t += dt;
                                rfy = 1;
                                cy += stepy;
                                rfx -= dt * dx;
                                continue;
                            }
                        } else {
                            // wall moves vertically
                            if (dy * rfx >= dx * wall_shift) {
                                // ray hits wall
                                const double dt = wall_shift / dy;
                                t += dt;
                                rfx -= dt * dx;
                                rfy -= wall_shift;
                            } else {
                                // ray moves to next cell
                                const double dt = rfx / dx;
                                t += dt;
                                rfx = 1;
                                cx += stepx;
                                rfy -= dt * dy;
                                continue;
                            }
                        }
                    }
                }
                if (rfx == 1 - wall_shift) {
                    // NS wall
                    texture_index = 2 * static_cast<int>(m0) - 1;
                    // fix texture orientation depending on ray direction
                    tx = stepx * stepy > 0 ? 1 - rfy : rfy;
                } else {
                    // EW wall
                    texture_index = 2 * static_cast<int>(m0) - 2;
                    // fix texture orientation depending on ray direction
                    tx = stepx * stepy < 0 ? 1 - rfx : rfx;
                }
                break;
            } else if (m0 <= 101) {
                // hit a door
                const double door_shift = f.door_shift[static_cast<std::size_t>(cy) * 64 + cx];
                if (m0 % 2 == 0) {
                    // NS door
                    if (rfx >= .5 && (rfx - .5) * dy < rfy * dx) {
                        // ray hits the central door line
                        const double dt = (rfx - .5) / dx;
                        t += dt;
                        rfy -= dt * dy;
                        rfx = .5;
                        tx = stepy > 0 ? 1 - rfy : rfy;
                        tx -= door_shift;
                        if (tx >= 0) {
                            // ray hits the door
                            switch (m0) {
                            case 90: texture_index = 99; break;
                            case 92: texture_index = 105; break;
                            case 94: texture_index = 105; break;
                            case 100: texture_index = 103; break;
                            default: texture_index = 0; break;  // unreachable
                            }
                            break;
                        }
                    }
                    if (rfx * dy >= rfy * dx) {
                        // hit the side wall
                        const double dt = rfy / dy;
                        t += dt;
                        rfx -= dt * dx;
                        rfy = 1;
                        cy += stepy;
                        texture_index = 100;
                        tx = stepx > 0 ? 1 - rfx : rfx;
                        break;
                    } else {
                        // pass through
                        const double dt = rfx / dx;
                        t += dt;
                        rfy -= dt * dy;
                        rfx = 1;
                        cx += stepx;
                    }
                } else {
                    // EW door
                    if (rfy >= .5 && (rfy - .5) * dx < rfx * dy) {
                        // ray hits the central door line
                        const double dt = (rfy - .5) / dy;
                        t += dt;
                        rfx -= dt * dx;
                        rfy = .5;
                        tx = stepx > 0 ? 1 - rfx : rfx;
                        tx -= door_shift;
                        if (tx >= 0) {
                            // ray hits the door
                            switch (m0) {
                            case 91: texture_index = 98; break;
                            case 93: texture_index = 104; break;
                            case 95: texture_index = 104; break;
                            case 101: texture_index = 102; break;
                            default: texture_index = 0; break;  // unreachable
                            }
                            break;
                        }
                    }
                    if (rfy * dx >= rfx * dy) {
                        // hit the side wall
                        const double dt = rfx / dx;
                        t += dt;
                        rfy -= dt * dy;
                        rfx = 1;
                        cx += stepx;
                        texture_index = 101;
                        tx = stepy > 0 ? 1 - rfy : rfy;
                        break;
                    } else {
                        // pass through
                        const double dt = rfy / dy;
                        t += dt;
                        rfx -= dt * dy;
                        rfy = 1;
                        cy += stepy;
                    }
                }
            }
            // move to the next cell
            if (rfx * dy <= rfy * dx) {
                // move to next cell horizontally
                const double dt = rfx / dx;
                t += dt;
                rfx = 1;
                cx += stepx;
                rfy -= dt * dy;
            } else {
                // move to next cell vertically
                const double dt = rfy / dy;
                t += dt;
                rfy = 1;
                cy += stepy;
                rfx -= dt * dx;
            }
        }

        ColumnRecord& rec = f.columns[static_cast<std::size_t>(i)];
        rec.t = t;
        rec.h = wall_height / (2 * t);
        rec.tx = tx;
        rec.tex = static_cast<std::uint32_t>(texture_index);
    }
}

// JS Math.round: round half up (floor(x + 0.5)).
inline double js_round(double x) { return std::floor(x + 0.5); }

void build_sprites(Frame& f, const Simulation& sim, std::uint32_t scale) {
    const auto& level = sim.level();
    // WS3: sprite screen geometry is authored in the internal pixel space
    // (W x H) with the scaled wall height — the reference drawThings /
    // drawSprite formulas with pixelWidth/height and wallHeight scaled by
    // the same factor. At scale 1 every value is the reference 320x200 one.
    const int W = kColumns * static_cast<int>(scale);
    const int H = kRows * static_cast<int>(scale);
    const double wall_height = kWallHeight * static_cast<double>(scale);

    // Collect renderable things (reference drawThings input), then order
    // them NEAREST-FIRST. The reference draws them farthest-first (painter's
    // algorithm), so the nearest covering sprite is the last one drawn and
    // wins; the compute shader tests records in array order and stops at the
    // first covering one, which is the same result when the array is
    // nearest-first.
    struct Thing {
        double rx = 0, ry = 0;
        const game::Actor* a;
    };
    std::vector<Thing> things;
    things.reserve(level.actors.size());
    for (const auto& a : level.actors) {
        if (a.removed)
            continue;
        // Thing.update: relative coordinates in the player's frame. The
        // reference runs this BEFORE player.update() moves the player, so
        // the pose is the tick's starting pose (f.sp_*), one tick stale
        // relative to the wall DDA when the player is in motion.
        const double rx0 = a.wx() - f.sp_px;
        const double ry0 = a.wy() - f.sp_py;
        const double rx = rx0 * f.sp_pdx + ry0 * f.sp_pdy;
        const double ry = -rx0 * f.sp_pdy + ry0 * f.sp_pdx;
        things.push_back({rx, ry, &a});
    }
    std::stable_sort(things.begin(), things.end(),
                     [](const Thing& p, const Thing& q) { return p.rx < q.rx; });

    // The weapon is drawn last by the reference (on top of everything, no
    // z test). In the nearest-first shader order it must therefore come
    // first.
    int out = 0;
    if (out < kMaxSprites) {
        SpriteRecord& w = f.sprites[out++];
        // reference drawWeapon at 320x200 (zoom 2): height 192, bottom center
        const double height = 192.0 * static_cast<double>(scale);
        w.x0 = W / 2.0 - height / 2.0;
        w.y0 = H - height;
        w.s = height / 64.0;
        w.scale = static_cast<std::uint32_t>(std::ceil(height / 64.0));
        w.sprite = f.weapon_sprite;
        w.dist = 0.0;  // always passes the z test (reference: no z test)
        w.th = height;
    }

    // Things, nearest first. The reference's early `return` on
    // rx < radius is equivalent here as a per-thing skip: those things are
    // behind the screen and the rest (farther) are still rendered.
    for (const auto& th : things) {
        if (th.rx < game::kRadius)
            continue;  // behind the screen
        if (std::abs(th.ry) > th.rx + 1)
            continue;  // out of field of view
        if (out >= kMaxSprites)
            break;

        const double h = wall_height / th.rx;
        SpriteRecord& r = f.sprites[out];
        // Reference drawThings: tx/ty use JS ~~ (truncation toward zero),
        // which differs from floor for negative non-integers (close sprites
        // can project partly off-screen).
        r.x0 = js_trunc((th.ry / th.rx + kFov) * wall_height - h / 2.0);
        r.y0 = js_trunc((H - h) / 2.0);
        r.s = h / 64.0;
        r.scale = static_cast<std::uint32_t>(std::ceil(h / 64.0));
        r.dist = th.rx;
        r.th = h;

        std::uint32_t idx = th.a->sprite;
        if (th.a->def != nullptr && th.a->def->orientable) {
            // 8-way facing variant (reference drawThings): computed from the
            // LIVE player (draw() runs after player.update()), not the stale
            // sprite pose. Math.round is applied to (angle steps - direction)
            // as a whole.
            const double ang = std::atan2(th.a->wx() - f.px, th.a->wy() - f.py);
            const double v = js_round(4.0 * ang / std::numbers::pi_v<double> -
                                      static_cast<double>(th.a->direction));
            idx += static_cast<std::uint32_t>((static_cast<long>(v) + 16) % 8);
        }
        r.sprite = idx;
        ++out;
    }
    f.sprite_count = out;
}

}  // namespace

std::uint32_t weapon_sprite(int cooldown_ticks, int prev_cooldown) {
    // The sim counter (set to 24 on the firing tick, decremented at the end
    // of that tick) reads 23,22,...,1,0 and then stays 0. The reference
    // weapon animation shows 421 for six ticks (counter 23..18), 422 for six
    // (17..12), 423 for six (11..6), 424 for six (5..0, the last one being
    // the counter's 1 -> 0 transition) and 421 afterwards. The 1 -> 0
    // transition is what separates the last 424 frame from the rest.
    if (cooldown_ticks <= 0)
        return prev_cooldown == 1 ? 424 : 421;
    if (cooldown_ticks <= 5)
        return 424;
    if (cooldown_ticks <= 11)
        return 423;
    if (cooldown_ticks <= 17)
        return 422;
    return 421;
}

void update_flash(Flash& fl, int event) {
    if (event == 1) {  // yellow: treasure / 1up
        fl = Flash{true, 0.5, 0.5, 0.0, 0};
    } else if (event == 2) {  // white: other collectible
        fl = Flash{true, 0.5, 0.5, 0.5, 0};
    } else if (event == 3) {  // red: shot hit
        fl = Flash{true, 0.5, 0.0, 0.0, 0};
    }
    if (fl.active) {
        fl.timer += 1;
        if (fl.timer > 6)
            fl.active = false;
    }
}

void effective_palette(const Flash& fl,
                       std::array<std::uint32_t, 256>& out) {
    double rf = 0.0, gf = 0.0, bf = 0.0;
    if (fl.active) {
        // Reference stages: timer 1-2 half, 3-4 full, 5-6 half.
        const double k = (fl.timer <= 2 || fl.timer >= 5) ? 0.5 : 1.0;
        rf = fl.red * k;
        gf = fl.green * k;
        bf = fl.blue * k;
    }
    for (std::size_t i = 0; i < 256; ++i) {
        const std::uint32_t v = kGamePalette[i];
        const std::uint32_t r0 = v & 0xFFu;
        const std::uint32_t g0 = (v >> 8) & 0xFFu;
        const std::uint32_t b0 = (v >> 16) & 0xFFu;
        std::uint32_t r = r0;
        std::uint32_t g = g0;
        std::uint32_t b = b0;
        if (rf)
            r += static_cast<std::uint32_t>(std::floor(rf * (255 - r0)));
        if (gf)
            g += static_cast<std::uint32_t>(std::floor(gf * (255 - g0)));
        if (bf)
            b += static_cast<std::uint32_t>(std::floor(bf * (255 - b0)));
        out[i] = 0xFF000000u | (b << 16) | (g << 8) | r;
    }
}

void build_frame(Frame& f, const Simulation& sim, const Pose& prev_pose,
                 const std::array<Frame::ActorSnap, kMaxSprites>& prev_snap,
                 int prev_snap_count, int prev_weapon_cooldown,
                 int weapon_shift, std::uint32_t scale) {
    scale = std::clamp<std::uint32_t>(scale, 1, kMaxScale);
    f.scale = scale;
    const auto& level = sim.level();

    f.px = sim.x();
    f.py = sim.y();
    f.pdx = sim.dx();
    f.pdy = sim.dy();
    f.sp_px = prev_pose.x;
    f.sp_py = prev_pose.y;
    f.sp_pdx = prev_pose.dx;
    f.sp_pdy = prev_pose.dy;
    const int ws = static_cast<int>(
                       weapon_sprite(sim.weapon_cooldown(), prev_weapon_cooldown)) +
                   weapon_shift;
    f.weapon_sprite = static_cast<std::uint32_t>(ws);

    for (std::size_t i = 0; i < 4096; ++i) {
        f.plane0[i] = level.plane0[i];
        f.plane1[i] = level.plane1[i];
        f.blocking[i] = level.blocking[i] ? 1 : 0;
        f.door_shift[i] = 0.0f;
        f.wall_shift[i] = 0.0f;
        f.wall_dx[i] = 0.0f;
        f.wall_dy[i] = 0.0f;
    }

    // Per-cell door openness (reference drawWalls doorShift): timer progress
    // while a timer runs, else the plane2 (blocking) flag. t/64 is exactly
    // representable in binary floats (power-of-two divisor).
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const std::uint16_t m0 = level.plane0[static_cast<std::size_t>(y) * 64 + x];
            if (m0 < 90 || m0 > 101)
                continue;
            double sh = 0.0;
            for (const auto& d : sim.doors()) {
                if (d.x == x && d.y == y) {
                    sh = d.was_blocking ? d.t / 64.0 : 1.0 - d.t / 64.0;
                    break;
                }
            }
            if (!level.blocking[static_cast<std::size_t>(y) * 64 + x])
                sh = 1.0;
            f.door_shift[static_cast<std::size_t>(y) * 64 + x] =
                static_cast<float>(sh);
        }
    }

    // Per-cell pushwall timer state.
    for (const auto& w : sim.pushwalls()) {
        const std::size_t i =
            static_cast<std::size_t>(w.y) * 64 + w.x;
        f.wall_shift[i] = static_cast<float>(w.t / 64.0);
        f.wall_dx[i] = static_cast<float>(w.dx);
        f.wall_dy[i] = static_cast<float>(w.dy);
    }

    build_columns(f, sim, scale);
    build_sprites(f, sim, scale);

    // Actor snapshot + flash event detection. The reference fires its
    // flashes from key/collect events; on the shipped maps the only events
    // that set a flash are pickups (player.collect) and shot kills
    // (player.shoot -> t.die()). A pickup wins over a same-frame kill
    // because the reference collects inside player.update(), after the
    // between-tick shoot event.
    f.flash_event = 0;
    const int n = static_cast<int>(level.actors.size());
    f.actor_snap_count = n < kMaxSprites ? n : kMaxSprites;
    int kill = 0;
    for (int i = 0; i < f.actor_snap_count; ++i) {
        const auto& a = level.actors[static_cast<std::size_t>(i)];
        auto& snap = f.actor_snap[static_cast<std::size_t>(i)];
        snap.removed = a.removed ? 1 : 0;
        snap.alive = a.alive ? 1 : 0;
        snap.sprite = a.sprite;
        if (i < prev_snap_count) {
            const auto& prev = prev_snap[static_cast<std::size_t>(i)];
            if (a.def != nullptr && !prev.removed && a.removed &&
                a.def->kind == game::ActorKind::kCollectible) {
                // Reference: treasure = sprite index 31..35 (all animation
                // frames of the treasure/1up sprites), else other pickup.
                f.flash_event = 31 <= a.sprite && a.sprite <= 35 ? 1 : 2;
            } else if (a.def != nullptr && a.def->killable && prev.alive &&
                       !a.alive) {
                kill = 1;
            }
        }
    }
    if (f.flash_event == 0 && kill)
        f.flash_event = 3;
}

}  // namespace w3d::render
