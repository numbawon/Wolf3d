// w3d game/sim.cpp — headless simulation (reference update loop port).
#include "game/sim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace w3d::game {

Simulation::Simulation(Level level)
    : level_(std::move(level)),
      px_(level_.px),
      py_(level_.py),
      pdx_(level_.pdx),
      pdy_(level_.pdy) {}

// ------------------------------------------------------------- actors

void Simulation::update_actors() {
    for (auto& a : level_.actors) {
        if (a.removed)
            continue;
        if (!a.animating)
            continue;
        a.anim_timer += 1;
        if (a.anim_timer < kAnimTicks)
            continue;
        a.anim_timer = 0;
        if (a.anim_frame >= a.anim_frames - 1) {
            if (a.anim_loop) {
                a.anim_frame = 0;
            } else {
                a.animating = false;  // stays on the final frame
            }
        } else {
            a.anim_frame += 1;
        }
        a.sprite = a.anim_sprites[a.anim_frame];
    }
}

// ------------------------------------------------------------- doors

void Simulation::update_door_timers() {
    for (std::size_t i = 0; i < door_timers_.size(); ++i) {
        DoorTimer& tm = door_timers_[i];
        tm.t += 1;
        if (tm.t < kDoorTicks)
            continue;
        if (tm.was_blocking)  // reference: opening -> unblocks at t >= 64
            level_.blocking[static_cast<std::size_t>(tm.y) * w3d::kMapSide + tm.x] = false;
        door_timers_.erase(door_timers_.begin() + static_cast<long>(i));
        --i;
    }
}

void Simulation::update_wall_timers() {
    for (std::size_t i = 0; i < wall_timers_.size(); ++i) {
        WallTimer& tm = wall_timers_[i];
        tm.t += 1;
        if (tm.t != kDoorTicks)  // reference: exactly t === 64
            continue;
        const int x = tm.x, y = tm.y;
        const int dx = tm.dx, dy = tm.dy;
        const std::uint16_t wall_value = level_.cell0(x, y);
        level_.set_cell0(x, y, level_.cell0(x - dx, y - dy));  // copy from behind
        level_.set_cell0(x + dx, y + dy, wall_value);
        level_.set_cell1(x, y, 0);
        level_.blocking[static_cast<std::size_t>(y) * w3d::kMapSide + x] = false;
        level_.blocking[static_cast<std::size_t>(y + dy) * w3d::kMapSide + (x + dx)] = true;
        tm.steps -= 1;
        if (tm.steps > 0 && !level_.is_blocking(x + 2 * dx, y + 2 * dy)) {
            level_.set_cell1(x + dx, y + dy, 98);  // marker keeps moving
            tm.t = 0;
            tm.x = static_cast<std::uint16_t>(x + dx);
            tm.y = static_cast<std::uint16_t>(y + dy);
        } else {
            wall_timers_.erase(wall_timers_.begin() + static_cast<long>(i));
            --i;
        }
    }
}

// ------------------------------------------------------------- player

void Simulation::turn(double alpha) {
    const double ndx = pdx_ * std::cos(alpha) - pdy_ * std::sin(alpha);
    pdy_ = pdx_ * std::sin(alpha) + pdy_ * std::cos(alpha);
    pdx_ = ndx;
}

bool Simulation::can_move_to(double x, double y) const {
    const double r = kRadius;
    const double fx = std::fmod(x, 1.0);
    const int ix = static_cast<int>(std::floor(x));
    const double fy = std::fmod(y, 1.0);
    const int iy = static_cast<int>(std::floor(y));

    if (level_.is_blocking(ix, iy))
        return false;
    if (fx < r) {
        if (level_.is_blocking(ix - 1, iy))
            return false;
        if (fy < r && level_.is_blocking(ix - 1, iy - 1))
            return false;
        if (fy > 1 - r && level_.is_blocking(ix - 1, iy + 1))
            return false;
    }
    if (fx > 1 - r) {
        if (level_.is_blocking(ix + 1, iy))
            return false;
        if (fy < r && level_.is_blocking(ix + 1, iy - 1))
            return false;
        if (fy > 1 - r && level_.is_blocking(ix + 1, iy + 1))
            return false;
    }
    if (fy < r && level_.is_blocking(ix, iy - 1))
        return false;
    if (fy > 1 - r && level_.is_blocking(ix, iy + 1))
        return false;
    return true;
}

void Simulation::move(double length, double sideways) {
    const int oldx = static_cast<int>(std::floor(px_));
    const int oldy = static_cast<int>(std::floor(py_));
    const double nx = px_ + pdx_ * length - pdy_ * sideways;
    const double ny = py_ + pdy_ * length + pdx_ * sideways;
    // axis-separated, exactly like the reference
    if (can_move_to(nx, py_))
        px_ = nx;
    if (can_move_to(px_, ny))
        py_ = ny;
    const int nxx = static_cast<int>(std::floor(px_));
    const int nyy = static_cast<int>(std::floor(py_));
    if (nxx != oldx || nyy != oldy)
        collect_cell(nxx, nyy);
}

// ------------------------------------------------------------- things

void Simulation::collect_cell(int cx, int cy) {
    if (cx < 0 || cy < 0 || cx >= w3d::kMapSide || cy >= w3d::kMapSide)
        return;
    for (auto& a : level_.actors) {
        if (a.removed || a.x != cx || a.y != cy)
            continue;
        const ActorDef* d = a.def;
        if (!d || !d->collectible)
            continue;
        if (d->treasure) {
            score_.treasures += 1;  // reference: sprites 31-35 (m1 52-56)
        } else if (d->sprite == 22) {
            gold_ = true;           // reference: gold key
        } else if (d->sprite == 23) {
            silver_ = true;         // reference: silver key
        }
        a.removed = true;
    }
}

// ------------------------------------------------------------- activate

void Simulation::do_activate() {
    int dx = 0, dy = 0;
    int cx = static_cast<int>(std::floor(px_));
    int cy = static_cast<int>(std::floor(py_));
    if (std::abs(pdx_) >= std::abs(pdy_)) {
        dx = pdx_ >= 0 ? 1 : -1;
        cx += dx;
    } else {
        dy = pdy_ >= 0 ? 1 : -1;
        cy += dy;
    }
    if (cx < 0 || cy < 0 || cx >= w3d::kMapSide || cy >= w3d::kMapSide)
        return;
    const std::uint16_t m0 = level_.cell0(cx, cy);
    const std::uint16_t m1 = level_.cell1(cx, cy);

    if (m0 == 21 && dx != 0) {
        // elevator shaft wall (textures 40/41): exit the level
        completed_ = true;
        return;
    }
    if (90 <= m0 && m0 <= 101) {
        // door
        if ((m0 == 92 || m0 == 93) && !gold_)
            return;
        if ((m0 == 94 || m0 == 95) && !silver_)
            return;
        const bool has_timer = std::any_of(door_timers_.begin(), door_timers_.end(),
                                           [&](const DoorTimer& t) {
                                               return t.x == cx && t.y == cy;
                                           });
        if (has_timer)
            return;
        const auto idx = static_cast<std::size_t>(cy) * w3d::kMapSide + cx;
        const bool was_blocking = level_.blocking[idx];
        if (!was_blocking) {
            // currently open: the door cannot close onto the player
            if ((dx > 0 && cx - px_ <= kRadius) ||
                (dx < 0 && px_ - cx - 1 <= kRadius) ||
                (dy > 0 && cy - py_ <= kRadius) ||
                (dy < 0 && py_ - cy - 1 <= kRadius))
                return;
            level_.blocking[idx] = true;  // closing blocks immediately
        }
        door_timers_.push_back({static_cast<std::uint16_t>(cx),
                                static_cast<std::uint16_t>(cy), 0, was_blocking});
    } else if (m1 == 98) {
        // pushwall: must be able to move into the cell behind it
        const bool has_timer = std::any_of(wall_timers_.begin(), wall_timers_.end(),
                                           [&](const WallTimer& t) {
                                               return t.x == cx && t.y == cy;
                                           });
        const std::uint16_t behind =
            (cx + dx >= 0 && cy + dy >= 0 && cx + dx < w3d::kMapSide &&
             cy + dy < w3d::kMapSide)
                ? level_.cell0(cx + dx, cy + dy)
                : 0;  // bounds act as walls (reference: undefined comparison)
        if (!has_timer && behind >= 106) {
            wall_timers_.push_back({static_cast<std::uint16_t>(cx),
                                    static_cast<std::uint16_t>(cy), 0,
                                    static_cast<std::int16_t>(dx),
                                    static_cast<std::int16_t>(dy), 2});
            score_.secrets += 1;
        }
    }
}

// ------------------------------------------------------------- shoot

void Simulation::do_shoot() {
    if (weapon_ticks_ > 0)
        return;  // reference: only when the weapon animation is idle
    weapon_ticks_ = kWeaponCooldown;
    const double d = ray_distance(px_, py_, pdx_, pdy_);

    // reference: stable-sort things by rx descending, iterate in reverse
    struct Rx {
        Actor* a;
        double rx;
    };
    std::vector<Rx> sorted;
    sorted.reserve(level_.actors.size());
    for (auto& a : level_.actors) {
        if (a.removed)
            continue;
        const double rx0 = a.wx() - px_;
        const double ry0 = a.wy() - py_;
        sorted.push_back({&a, rx0 * pdx_ + ry0 * pdy_});
    }
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Rx& p, const Rx& q) { return p.rx > q.rx; });
    for (std::size_t i = sorted.size(); i > 0; --i) {
        const Rx& e = sorted[i - 1];
        if (e.rx < 0)
            continue;
        if (e.rx >= d)
            break;
        Actor* a = e.a;
        const double ry0 = a->wy() - py_;
        const double rx0 = a->wx() - px_;
        const double ry = -rx0 * pdy_ + ry0 * pdx_;
        if (std::abs(ry) <= kShotSpread && a->alive &&
            a->def && a->def->killable) {
            a->alive = false;
            score_.kills += 1;
            // start the dying animation (reference die())
            a->animating = true;
            a->anim_loop = false;
            a->anim_frame = 0;
            a->anim_timer = 0;
            a->anim_frames = a->def->death_frames;
            for (int f = 0; f < a->def->death_frames; ++f)
                a->anim_sprites[f] = a->def->death[f];
            a->sprite = a->def->death[0];
            return;
        }
    }
}

// ------------------------------------------------------------- LOS

double Simulation::ray_distance(double x, double y, double dx, double dy) const {
    // grid DDA; t in cell units; blocks on plane2 cells and the map bounds
    const double inf = 1e30;
    int cx = static_cast<int>(std::floor(x));
    int cy = static_cast<int>(std::floor(y));
    double t = 0.0;
    double tdx = dx > 0 ? (cx + 1.0 - x) / dx : (dx < 0 ? (x - cx) / -dx : inf);
    double tdy = dy > 0 ? (cy + 1.0 - y) / dy : (dy < 0 ? (y - cy) / -dy : inf);
    const int sx = dx > 0 ? 1 : -1;
    const int sy = dy > 0 ? 1 : -1;
    const double ddx = dx != 0 ? 1.0 / std::abs(dx) : inf;
    const double ddy = dy != 0 ? 1.0 / std::abs(dy) : inf;
    for (int i = 0; i < 512; ++i) {
        if (tdx < tdy) {
            t = tdx;
            tdx += ddx;
            cx += sx;
        } else {
            t = tdy;
            tdy += ddy;
            cy += sy;
        }
        if (cx < 0 || cy < 0 || cx >= w3d::kMapSide || cy >= w3d::kMapSide)
            return t;
        if (level_.blocking[static_cast<std::size_t>(cy) * w3d::kMapSide + cx])
            return t;
    }
    return t;
}

// ------------------------------------------------------------- tick

void Simulation::tick(const SimInput& in) {
    if (completed_)
        return;  // reference: loadNextLevel(); headless: freeze
    ++tick_count_;

    update_actors();
    update_door_timers();
    update_wall_timers();

    // turn
    int turn_amount = 0;
    if (in.turn_right)
        turn_amount += 1;
    if (in.turn_left)
        turn_amount -= 1;
    if (turn_amount != 0)
        turn(turn_amount * kTurnSpeed);

    // move (reference: diagonal = both / sqrt(2))
    double forward = 0.0, sideways = 0.0;
    if (in.forward)
        forward += kSpeed;
    if (in.backward)
        forward -= kSpeed;
    if (in.strafe_left)
        sideways -= kSpeed;
    if (in.strafe_right)
        sideways += kSpeed;
    if (forward != 0.0) {
        if (sideways != 0.0)
            move(forward / std::sqrt(2.0), sideways / std::sqrt(2.0));
        else
            move(forward);
    } else if (sideways != 0.0) {
        move(0.0, sideways);
    }

    if (in.activate)
        do_activate();
    if (in.shoot)
        do_shoot();

    if (weapon_ticks_ > 0)
        weapon_ticks_ -= 1;
}

// ------------------------------------------------------------- output

int Simulation::alive_enemies() const {
    int n = 0;
    for (const auto& a : level_.actors)
        if (!a.removed && a.alive && a.def && a.def->killable)
            ++n;
    return n;
}

int Simulation::thing_count() const {
    int n = 0;
    for (const auto& a : level_.actors)
        if (!a.removed)
            ++n;
    return n;
}

std::string Simulation::tick_line() const {
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "t=%07llu x=%.9f y=%.9f fx=%.9f fy=%.9f%s%s kills=%02d/%02d "
                  "trs=%02d/%02d sec=%02d/%02d ent=%03d drs=%02d pus=%02d "
                  "wpn=%02d %s\n",
                  static_cast<unsigned long long>(tick_count_), px_, py_, pdx_,
                  pdy_, gold_ ? " g" : "  ", silver_ ? "s" : " ",
                  score_.kills, level_.total_kills, score_.treasures,
                  level_.total_treasures, score_.secrets, level_.total_secrets,
                  alive_enemies(), active_doors(), active_pushwalls(),
                  weapon_ticks_, completed_ ? "EXIT" : "PLAY");
    return buf;
}

// ------------------------------------------------------------- walk script

SimInput walk_input(std::uint32_t tick) {
    SimInput in;
    in.forward = true;
    in.backward = (tick % 512) >= 480;
    in.strafe_right = (tick % 64) >= 56;
    in.turn_right = (tick % 128) < 32;
    in.turn_left = (tick % 128) >= 96;
    in.activate = (tick % 40) == 0;
    in.shoot = (tick % 25) == 0;
    return in;
}

} // namespace w3d::game
