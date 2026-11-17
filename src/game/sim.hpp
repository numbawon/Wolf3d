// w3d game/sim.hpp — headless deterministic game simulation (Phase 2).
//
// One simulation tick = one reference-engine frame (its 60fps update loop).
// The tick order mirrors game.js update():
//   1. actor animations
//   2. door timers (64 ticks per open/close cycle)
//   3. pushwall timers (64 ticks per cell step, 2 steps)
//   4. player: turn, move (axis-separated, radius collision), collect
//   5. input activate (door / pushwall / elevator)
//   6. input shoot (one-shot kill through the center ray)
//   7. weapon animation cooldown (24 ticks)
// The reference fires activate/shoot from key events; the headless sim
// processes them at a fixed point (after movement) so runs are reproducible.
//
// Determinism: pure double arithmetic in fixed order, no randomness, no
// time, no unordered containers. tick_line() is the golden-fixture output.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game/level.hpp"

namespace w3d::game {

// Reference constants (game.js Player / Animation / door logic).
inline constexpr double kSpeed = 0.065;    // cells per tick
inline constexpr double kTurnSpeed = 0.05; // radians per tick
inline constexpr double kRadius = 0.25;    // collision radius (cells)
inline constexpr int kDoorTicks = 64;      // door open/close duration
inline constexpr int kAnimTicks = 8;       // sprite frames per animation step
inline constexpr int kWeaponCooldown = 24; // 4 weapon frames x 6 ticks
inline constexpr double kShotSpread = 0.3; // |ry| limit for a hit

// Per-tick player input (reference keymap states).
struct SimInput {
    bool forward = false;
    bool backward = false;
    bool strafe_left = false;
    bool strafe_right = false;
    bool turn_left = false;
    bool turn_right = false;
    bool activate = false;  // use: open/close door, push wall, exit elevator
    bool shoot = false;     // fire
};

struct Score {
    int kills = 0;
    int treasures = 0;
    int secrets = 0;
};

class Simulation {
public:
    explicit Simulation(Level level);

    // Advance one tick. No-ops (player frozen) once level_completed().
    void tick(const SimInput& in);

    [[nodiscard]] double x() const { return px_; }
    [[nodiscard]] double y() const { return py_; }
    [[nodiscard]] double dx() const { return pdx_; }
    [[nodiscard]] double dy() const { return pdy_; }
    [[nodiscard]] bool gold_key() const { return gold_; }
    [[nodiscard]] bool silver_key() const { return silver_; }
    [[nodiscard]] const Level& level() const { return level_; }
    [[nodiscard]] const Score& score() const { return score_; }
    [[nodiscard]] bool level_completed() const { return completed_; }
    [[nodiscard]] int alive_enemies() const;
    [[nodiscard]] int active_doors() const { return static_cast<int>(door_timers_.size()); }
    [[nodiscard]] int active_pushwalls() const {
        return static_cast<int>(wall_timers_.size());
    }
    [[nodiscard]] int weapon_cooldown() const { return weapon_ticks_; }
    [[nodiscard]] int thing_count() const;  // not-yet-removed actors

    // ---- Audio/sim events (WS5) ----
    // One discrete event emitted by the sim during a tick. The app reads
    // these each frame and maps them to mixer voices / music / messages.
    struct Event {
        enum Type { None, SfxWeapon, SfxDoor, SfxPickup, SfxEnemyDie,
                    SfxPlayerDie, SfxPushwall, SfxLevelExit, Message, Rumble };
        Type type = None;
        // Sfx: index into the app's SFX mapping table (bank index + isFm
        //     resolved by the app). Position (world coords) for pan.
        int sfxIndex = -1;
        double wx = 0.0, wy = 0.0;
        // Message text.
        std::string text;
        // Rumble: strength (0..1) and duration (ms).
        float rumbleStrength = 0.0f;
        int rumbleMs = 0;
        // Level exit: next level index.
        int nextLevel = -1;
    };

    // Events emitted during the most recent tick. Call clearEvents() after
    // consuming them.
    [[nodiscard]] const std::vector<Event>& events() const { return events_; }
    void clearEvents() { events_.clear(); }

    // Door / pushwall timers (reference doorTimers / wallTimers), exposed
    // read-only for the renderer's per-cell door/pushwall shift state.
    struct DoorTimer {
        std::uint16_t x, y;
        int t;
        bool was_blocking;  // reference "opening"
    };
    struct WallTimer {
        std::uint16_t x, y;
        int t;
        std::int16_t dx, dy;
        int steps;
    };
    [[nodiscard]] const std::vector<DoorTimer>& doors() const {
        return door_timers_;
    }
    [[nodiscard]] const std::vector<WallTimer>& pushwalls() const {
        return wall_timers_;
    }

    // Fixed-format deterministic tick output (golden fixture line).
    [[nodiscard]] std::string tick_line() const;

    // LOS raycast: distance in cells to the first blocking surface from
    // (x, y) in unit direction (dx, dy). Blocks on plane2 cells and map
    // bounds. (Reference zIndex[center] equivalent for all killable
    // cases — see docs/formats.md, "simulation simplifications".)
    [[nodiscard]] double ray_distance(double x, double y, double dx, double dy) const;

private:
    Level level_;
    double px_ = 0.5, py_ = 0.5, pdx_ = 0.0, pdy_ = -1.0;
    bool gold_ = false, silver_ = false;
    Score score_;
    bool completed_ = false;
    int weapon_ticks_ = 0;  // 0 = ready to fire

    std::vector<DoorTimer> door_timers_;
    std::vector<WallTimer> wall_timers_;
    std::uint64_t tick_count_ = 0;

    // WS5: events emitted during the most recent tick.
    std::vector<Event> events_;

    void emit(Event::Type type, int sfxIndex, double wx, double wy);
    void turn(double alpha);
    bool can_move_to(double x, double y) const;
    void move(double length, double sideways = 0.0);
    void collect_cell(int cx, int cy);
    void do_activate();
    void do_shoot();
    void update_actors();
    void update_door_timers();
    void update_wall_timers();
};

// Deterministic "walk each map" input script used by the Phase 2 acceptance
// test (movement, collision, doors, pushwalls, shooting, elevator exit).
[[nodiscard]] SimInput walk_input(std::uint32_t tick);
inline constexpr std::uint32_t kWalkTicks = 2000;

} // namespace w3d::game
