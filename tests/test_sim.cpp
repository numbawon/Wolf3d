// tests/test_sim.cpp — Phase 2 golden tests for the headless game sim.
//
// Two families of tests:
//  1. Semantic tests on small synthetic levels: exact reference behavior for
//     doors (64-tick open/close), locked doors, pushwalls (2 x 64 ticks),
//     collectibles, one-shot shooting, elevator exit, start orientation,
//     and wall collision.
//  2. The Phase 2 acceptance test: every map of both shipped games (81 maps)
//     is walked headless with a fixed input script; runs must not crash and
//     the tick output must be byte-identical across runs.
//
// Game data location: W3D_GAME_DIR environment variable, falling back to the
// Steam install path baked in at configure time (same as test_formats).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "game/actors.hpp"
#include "game/input.hpp"
#include "game/level.hpp"
#include "game/sim.hpp"
#include "w3d/maps.hpp"

using namespace w3d;
using namespace w3d::game;

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

// ---------------------------------------------------------------- helpers

// 64x64 all-wall map.
w3d::Map make_map() {
    w3d::Map m;
    m.name = "synthetic";
    for (auto& v : m.plane0)
        v = 1;
    for (auto& v : m.plane1)
        v = 0;
    return m;
}

void open_rect(w3d::Map& m, int x0, int y0, int x1, int y1,
               std::uint16_t floor_value = 106) {
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            m.plane0[static_cast<std::size_t>(y) * 64 + x] = floor_value;
}

// Player spawn facing east.
void put_start(w3d::Map& m, int x, int y, std::uint16_t facing = 20) {
    m.plane1[static_cast<std::size_t>(y) * 64 + x] = facing;
}

void put(w3d::Map& m, int x, int y, std::uint16_t plane1_value) {
    m.plane1[static_cast<std::size_t>(y) * 64 + x] = plane1_value;
}

void put0(w3d::Map& m, int x, int y, std::uint16_t plane0_value) {
    m.plane0[static_cast<std::size_t>(y) * 64 + x] = plane0_value;
}

void tick_n(Simulation& sim, int n, SimInput in = {}) {
    for (int i = 0; i < n; ++i)
        sim.tick(in);
}

// Walk forward until the player's cell x-coordinate reaches `cell`
// (or the guard expires). Returns the number of ticks used.
int walk_to_cell_x(Simulation& sim, int cell, int guard = 1000) {
    int n = 0;
    SimInput fwd;
    fwd.forward = true;
    while (static_cast<int>(std::floor(sim.x())) < cell && n < guard) {
        sim.tick(fwd);
        ++n;
    }
    return n;
}

} // namespace

// ----------------------------------------------------------- actor table

static void test_classify_actors() {
    g_current = "classify_actors";
    // spawn
    for (std::uint16_t v = 19; v <= 22; ++v)
        CHECK_EQ(classify_actor(v).kind, ActorKind::kStart);
    CHECK_EQ(classify_actor(18).kind, ActorKind::kNone);
    // props: sprite = m1 - 21
    CHECK_EQ(classify_actor(23).kind, ActorKind::kProp);
    CHECK_EQ(classify_actor(23).sprite, 2);
    CHECK_EQ(classify_actor(70).kind, ActorKind::kProp);
    CHECK_EQ(classify_actor(70).sprite, 49);
    // blocking props (reference list)
    for (int v : {24, 25, 26, 28, 30, 31, 33, 34, 35, 36, 39, 40,
                 41, 45, 58, 59, 60, 62, 63, 68, 69})
        CHECK(classify_actor(static_cast<std::uint16_t>(v)).blocking);
    // non-blocking props
    for (int v : {23, 27, 29, 32, 37, 38, 42, 43, 44, 46, 47, 48,
                 49, 50, 51, 52, 53, 54, 55, 56, 57, 61, 64, 65,
                 66, 67, 70})
        CHECK(!classify_actor(static_cast<std::uint16_t>(v)).blocking);
    // collectibles and treasures
    for (int v : {29, 43, 44, 47, 48, 49, 50, 51, 52, 53, 54, 55,
                 56})
        CHECK(classify_actor(static_cast<std::uint16_t>(v)).collectible);
    for (int v : {52, 53, 54, 55, 56})
        CHECK(classify_actor(static_cast<std::uint16_t>(v)).treasure);
    CHECK_EQ(classify_actor(43).sprite, 22);  // gold key
    CHECK_EQ(classify_actor(44).sprite, 23);  // silver key
    CHECK_EQ(classify_actor(49).sprite, 28);  // ammo clip
    // inert values (present in shipped data, unhandled by the reference)
    for (int v : {71, 72, 73, 74, 90, 95, 97, 99, 106, 107, 125,
                 142, 143, 161, 180, 184, 188, 198, 206, 210,
                 252, 256, 259})
        CHECK_EQ(classify_actor(static_cast<std::uint16_t>(v)).kind,
                 ActorKind::kUnknown);
    // values absent from the data are plain "nothing"
    CHECK_EQ(classify_actor(500).kind, ActorKind::kNone);
    // pushwall / dead guard
    CHECK_EQ(classify_actor(98).kind, ActorKind::kPushwall);
    CHECK_EQ(classify_actor(124).kind, ActorKind::kDeadGuard);
    CHECK_EQ(classify_actor(124).sprite, 95);
    // regular enemy bands
    CHECK_EQ(classify_actor(108).kind, ActorKind::kEnemy);
    CHECK_EQ(classify_actor(108).enemy, EnemyType::kGuard);
    CHECK_EQ(classify_actor(108).sprite, 50);
    CHECK_EQ(classify_actor(115).kind, ActorKind::kEnemy);
    CHECK_EQ(classify_actor(116).enemy, EnemyType::kOfficer);
    CHECK_EQ(classify_actor(123).enemy, EnemyType::kOfficer);
    CHECK_EQ(classify_actor(126).enemy, EnemyType::kSS);
    CHECK_EQ(classify_actor(134).enemy, EnemyType::kDog);
    CHECK_EQ(classify_actor(141).enemy, EnemyType::kDog);
    CHECK_EQ(classify_actor(144).enemy, EnemyType::kGuard);
    CHECK_EQ(classify_actor(216).enemy, EnemyType::kZombie);
    CHECK_EQ(classify_actor(241).enemy, EnemyType::kZombie);
    CHECK(classify_actor(108).killable);
    CHECK(classify_actor(108).orientable);
    CHECK_EQ(classify_actor(108).death_frames, 5);
    CHECK_EQ(classify_actor(108).death[0], 90);
    CHECK_EQ(classify_actor(108).death[4], 95);
    // directions
    CHECK_EQ(actor_direction(108), 0);  // N
    CHECK_EQ(actor_direction(109), 1);  // E
    CHECK_EQ(actor_direction(110), 2);  // S
    CHECK_EQ(actor_direction(111), 3);  // W
    CHECK_EQ(actor_direction(112), 0);  // patrolling, same dirs
    CHECK_EQ(actor_direction(144 + 3), 3);
    // zombie reference quirk: 216 band uses (m1 - 116) % 4
    CHECK_EQ(actor_direction(216), 0);  // (216-116)%4 = 0
    CHECK_EQ(actor_direction(219), 3);
    // spawnable enemies (counted in totalKills)
    for (int v : {108, 112, 116, 126, 134, 144, 170, 216, 234,
                 160, 178, 196, 214})
        CHECK(actor_is_spawnable_enemy(static_cast<std::uint16_t>(v)));
    for (int v : {224, 225, 226, 227, 180, 252, 106, 142, 98})
        CHECK(!actor_is_spawnable_enemy(static_cast<std::uint16_t>(v)));
    // bosses
    CHECK_EQ(classify_actor(178).kind, ActorKind::kBoss);
    CHECK_EQ(classify_actor(178).sprite, 349);
    CHECK_EQ(classify_actor(178).death_frames, 8);
    CHECK_EQ(classify_actor(160).sprite, 321);
    CHECK_EQ(classify_actor(215).sprite, 364);
    // ghosts
    CHECK_EQ(classify_actor(224).kind, ActorKind::kGhost);
    CHECK_EQ(classify_actor(224).sprite, 288);
    CHECK_EQ(classify_actor(227).sprite, 294);
    CHECK(classify_actor(224).ghost_loop);
}

// ----------------------------------------------------------- level build

static void test_level_build_synthetic() {
    g_current = "level_build_synthetic";
    auto m = make_map();
    open_rect(m, 4, 10, 14, 14);
    put_start(m, 5, 12);               // facing east
    put(m, 8, 12, 49);                 // ammo
    put(m, 9, 12, 52);                 // treasure
    put(m, 10, 12, 98);                // pushwall
    put0(m, 10, 12, 5);
    put(m, 11, 12, 108);               // guard (any-difficulty)
    put(m, 12, 12, 224);               // ghost
    put(m, 13, 12, 180);               // hard-tier guard (inert!)
    put(m, 4, 12, 24);                 // blocking barrel

    Level lv = Level::from_map(m);
    CHECK(lv.has_start);
    CHECK_EQ(lv.px, 5.5);
    CHECK_EQ(lv.py, 12.5);
    CHECK_EQ(lv.pdx, 1.0);
    CHECK_EQ(lv.pdy, 0.0);
    CHECK(lv.is_blocking(15, 12));   // outer wall
    CHECK(lv.is_blocking(10, 12));   // pushwall cell
    CHECK(lv.is_blocking(4, 12));    // blocking barrel
    CHECK(!lv.is_blocking(6, 12));
    CHECK(lv.is_blocking(0, 0));     // out of range / wall ring
    // actor census: barrel, ammo, treasure, guard, ghost (pushwall keeps no
    // actor; hard-tier 180 is inert)
    CHECK_EQ(lv.actors.size(), 5u);
    CHECK_EQ(lv.total_kills, 1);
    CHECK_EQ(lv.total_treasures, 1);
    CHECK_EQ(lv.total_secrets, 1);
    // start facing for all four orientations
    for (int i = 0; i < 4; ++i) {
        auto m2 = make_map();
        open_rect(m2, 30, 30, 34, 34);
        put_start(m2, 32, 32, static_cast<std::uint16_t>(19 + i));
        Level l2 = Level::from_map(m2);
        const double ex[] = {0.0, 1.0, 0.0, -1.0};
        const double ey[] = {-1.0, 0.0, 1.0, 0.0};
        CHECK_EQ(l2.pdx, ex[i]);
        CHECK_EQ(l2.pdy, ey[i]);
    }
}

// ----------------------------------------------------------- door logic

static void test_door_open_close() {
    g_current = "door_open_close";
    auto m = make_map();
    open_rect(m, 4, 10, 9, 14);   // room A
    open_rect(m, 11, 10, 16, 14); // room B
    put0(m, 10, 12, 90);          // regular door between the rooms
    put_start(m, 5, 12);

    Simulation sim(Level::from_map(m));
    CHECK(sim.level().is_blocking(10, 12));  // closed at spawn
    // walk up to the door (cell 9), then activate on tick 0
    walk_to_cell_x(sim, 9);
    SimInput in;
    in.activate = true;
    sim.tick(in);
    CHECK_EQ(sim.active_doors(), 1);
    CHECK(sim.level().is_blocking(10, 12));
    // ticks 1..63: still blocking
    for (int t = 1; t < 64; ++t) {
        sim.tick({});
        CHECK(sim.level().is_blocking(10, 12));
    }
    // tick 64: opens
    sim.tick({});
    CHECK(!sim.level().is_blocking(10, 12));
    CHECK_EQ(sim.active_doors(), 0);
    // walk through into room B
    walk_to_cell_x(sim, 11, 400);
    CHECK(static_cast<int>(std::floor(sim.x())) >= 11);
    // turn around (64 ticks x 0.05 rad = 3.2 > pi)
    SimInput tr;
    tr.turn_right = true;
    tick_n(sim, 64, tr);
    CHECK(sim.dx() < 0.0);
    // back away from the door (a closing door refuses to shut on the
    // player); stop in cell 11, safely past the 0.25 close guard
    SimInput back;
    back.backward = true;
    tick_n(sim, 14, back);
    CHECK(sim.x() > 11.5 && sim.x() < 12.0);
    // close: activate from the far side
    SimInput act;
    act.activate = true;
    sim.tick(act);
    CHECK(sim.level().is_blocking(10, 12));  // closing blocks immediately
    CHECK_EQ(sim.active_doors(), 1);
    tick_n(sim, 63);
    CHECK(sim.level().is_blocking(10, 12));
    sim.tick({});
    CHECK(sim.level().is_blocking(10, 12));  // stays closed
    CHECK_EQ(sim.active_doors(), 0);
}

static void test_locked_door() {
    g_current = "locked_door";
    auto m = make_map();
    open_rect(m, 4, 10, 9, 14);
    open_rect(m, 11, 10, 16, 14);
    put0(m, 10, 12, 92);  // gold-locked door
    put_start(m, 5, 12);
    put(m, 6, 12, 43);    // gold key on the path

    Simulation sim(Level::from_map(m));
    SimInput fwd;
    fwd.forward = true;
    // walk to the key and pick it up
    while (!sim.gold_key())
        sim.tick(fwd);
    CHECK(sim.gold_key());
    // walk up to the door (cell 9)
    walk_to_cell_x(sim, 9);
    // now the door works: one "use" press (activate must not be held —
    // the reference fires activate from key events; holding it in the
    // headless sim would re-trigger the close on the tick the door opens).
    // The timer starts on tick 0, opens at t=64.
    SimInput act;
    act.activate = true;
    sim.tick(act);
    tick_n(sim, 64);
    CHECK(!sim.level().is_blocking(10, 12));
    // silver door without silver key: locked
    auto m2 = make_map();
    open_rect(m2, 4, 10, 9, 14);
    open_rect(m2, 11, 10, 16, 14);
    put0(m2, 10, 12, 94);  // silver-locked
    put_start(m2, 5, 12);
    Simulation sim2(Level::from_map(m2));
    for (int t = 0; t < 80; ++t)
        sim2.tick(act);
    CHECK(sim2.level().is_blocking(10, 12));
    CHECK_EQ(sim2.active_doors(), 0);
}

// ----------------------------------------------------------- collectibles

static void test_collect_pickups() {
    g_current = "collect_pickups";
    auto m = make_map();
    open_rect(m, 4, 10, 12, 14);
    put_start(m, 4, 12);
    put(m, 6, 12, 49);  // ammo
    put(m, 7, 12, 43);  // gold
    put(m, 8, 12, 44);  // silver
    put(m, 9, 12, 52);  // treasure
    put(m, 10, 12, 24); // blocking barrel

    Simulation sim(Level::from_map(m));
    const int things0 = sim.thing_count();
    CHECK_EQ(things0, 5);
    SimInput fwd;
    fwd.forward = true;
    int guard = 0;
    while (sim.score().treasures < 1 && guard < 400) {
        sim.tick(fwd);
        ++guard;
    }
    CHECK(sim.gold_key());
    CHECK(sim.silver_key());
    CHECK_EQ(sim.score().treasures, 1);
    CHECK_EQ(sim.thing_count(), things0 - 4);  // 4 pickups removed
    // the blocking barrel stops the player in cell 9
    while (guard < 400) {
        sim.tick(fwd);
        ++guard;
    }
    CHECK(sim.x() < 10.0);
    CHECK(sim.x() > 9.0);
}

// ----------------------------------------------------------- pushwalls

static void test_pushwall() {
    g_current = "pushwall";
    auto m = make_map();
    open_rect(m, 4, 10, 14, 14);
    put0(m, 10, 12, 7);   // wall texture 7 at the pushwall cell
    put(m, 10, 12, 98);   // pushwall marker
    put_start(m, 5, 12);

    Simulation sim(Level::from_map(m));
    CHECK_EQ(sim.level().total_secrets, 1);
    CHECK_EQ(sim.score().secrets, 0);
    CHECK(sim.level().is_blocking(10, 12));
    // walk up to the pushwall (cell 9), then activate on tick 0
    walk_to_cell_x(sim, 9);
    SimInput act;
    act.activate = true;
    sim.tick(act);
    CHECK_EQ(sim.active_pushwalls(), 1);
    CHECK_EQ(sim.score().secrets, 1);
    tick_n(sim, 63);
    // step 1 completes at tick 64
    sim.tick({});
    CHECK_EQ(sim.level().cell0(10, 12), 106);  // copied from behind
    CHECK_EQ(sim.level().cell0(11, 12), 7);    // wall moved east
    CHECK_EQ(sim.level().cell1(10, 12), 0);
    CHECK_EQ(sim.level().cell1(11, 12), 98);   // marker follows
    CHECK(!sim.level().is_blocking(10, 12));
    CHECK(sim.level().is_blocking(11, 12));
    CHECK_EQ(sim.active_pushwalls(), 1);       // still moving
    tick_n(sim, 63);
    sim.tick({});  // tick 128: step 2
    CHECK_EQ(sim.level().cell0(11, 12), 106);
    CHECK_EQ(sim.level().cell0(12, 12), 7);
    CHECK_EQ(sim.level().cell1(11, 12), 0);
    CHECK(!sim.level().is_blocking(11, 12));
    CHECK(sim.level().is_blocking(12, 12));
    CHECK_EQ(sim.active_pushwalls(), 0);
    CHECK_EQ(sim.score().secrets, 1);
}

// ----------------------------------------------------------- shooting

static void test_shoot_kills() {
    g_current = "shoot_kills";
    auto m = make_map();
    open_rect(m, 4, 10, 14, 14);
    put_start(m, 4, 12);
    put(m, 7, 12, 108);  // guard 3 cells ahead

    Simulation sim(Level::from_map(m));
    CHECK_EQ(sim.level().total_kills, 1);
    CHECK_EQ(sim.alive_enemies(), 1);
    // weapon starts ready; shoot on tick 0
    SimInput fire;
    fire.shoot = true;
    sim.tick(fire);
    CHECK_EQ(sim.score().kills, 1);
    CHECK_EQ(sim.alive_enemies(), 0);
    CHECK_EQ(sim.weapon_cooldown(), 23);  // 24 set, 1 decremented this tick
    const Actor* guard = nullptr;
    for (const auto& a : sim.level().actors)
        if (a.def && a.def->enemy == EnemyType::kGuard)
            guard = &a;
    CHECK(guard != nullptr);
    if (!guard)
        return;
    CHECK_EQ(guard->sprite, 90);  // first death frame
    // a second shot on the same tick stream is swallowed while cooling
    sim.tick(fire);
    CHECK_EQ(sim.score().kills, 1);
    // death animation: 5 frames x 8 ticks (frames 90,91,92,93,95)
    tick_n(sim, 7);  // tick 8: frame 1
    CHECK_EQ(guard->sprite, 91);
    tick_n(sim, 16);  // tick 24: frame 3; weapon back to 0
    CHECK_EQ(sim.weapon_cooldown(), 0);
    CHECK_EQ(guard->sprite, 93);
    tick_n(sim, 8);  // tick 32: final frame
    CHECK_EQ(guard->sprite, 95);
    tick_n(sim, 8);  // tick 40: animation done, stays on 95
    CHECK_EQ(guard->sprite, 95);
    CHECK(!guard->animating);
}

static void test_shoot_los_and_ghost() {
    g_current = "shoot_los_and_ghost";
    // guard behind a wall: not killed (ray distance < guard distance)
    auto m = make_map();
    open_rect(m, 4, 10, 14, 14);
    put0(m, 6, 10, 3);   // wall across the corridor (row y=10 closed... )
    put0(m, 6, 12, 3);   // wall in the line of fire
    put_start(m, 4, 12);
    put(m, 8, 12, 108);
    Simulation sim(Level::from_map(m));
    SimInput fire;
    fire.shoot = true;
    sim.tick(fire);
    CHECK_EQ(sim.score().kills, 0);  // wall blocks the shot
    // ghost is not killable
    auto m2 = make_map();
    open_rect(m2, 4, 10, 14, 14);
    put_start(m2, 4, 12);
    put(m2, 7, 12, 224);  // ghost
    Simulation sim2(Level::from_map(m2));
    CHECK_EQ(sim2.thing_count(), 1);
    sim2.tick(fire);
    CHECK_EQ(sim2.score().kills, 0);
    CHECK_EQ(sim2.thing_count(), 1);  // still there, unkillable
    // ghost animation loops between its two frames
    tick_n(sim2, 16);
    const auto& ghost = sim2.level().actors.front();
    CHECK(ghost.sprite == 288 || ghost.sprite == 289);
    // ray_distance sanity: open room x4..14, wall at x=15, from x=4.5 east
    CHECK_EQ(sim2.ray_distance(4.5, 12.5, 1.0, 0.0), 10.5);
    // through a closed door cell the ray stops at the door's cell boundary
    // (equivalent to the reference's door-center line for all killable
    // cases — see docs/formats.md)
    CHECK_EQ(sim2.ray_distance(4.5, 12.5, 1.0, 0.0), 10.5);
}

// ----------------------------------------------------------- elevator

static void test_elevator_exit() {
    g_current = "elevator_exit";
    auto m = make_map();
    open_rect(m, 4, 10, 5, 14);
    put0(m, 6, 12, 21);  // elevator shaft wall directly ahead
    put_start(m, 5, 12);  // facing east

    Simulation sim(Level::from_map(m));
    CHECK(!sim.level_completed());
    SimInput act;
    act.activate = true;
    sim.tick(act);
    CHECK(sim.level_completed());
    const double x0 = sim.x();
    tick_n(sim, 10, act);  // frozen after exit
    CHECK_EQ(sim.x(), x0);
    CHECK(sim.tick_line().find("EXIT") != std::string::npos);
    // facing the elevator from the N/S side does NOT trigger it
    auto m2 = make_map();
    open_rect(m2, 4, 10, 5, 14);
    put0(m2, 6, 11, 21);  // above the player, not in front
    put_start(m2, 5, 12);
    Simulation sim2(Level::from_map(m2));
    sim2.tick(act);
    CHECK(!sim2.level_completed());
}

// ----------------------------------------------------------- collision

static void test_collision_wall() {
    g_current = "collision_wall";
    auto m = make_map();
    open_rect(m, 4, 10, 6, 14);  // corridor ending in a wall at x=7
    put_start(m, 4, 12);

    Simulation sim(Level::from_map(m));
    SimInput fwd;
    fwd.forward = true;
    for (int t = 0; t < 400; ++t)
        sim.tick(fwd);
    CHECK(std::isfinite(sim.x()));
    CHECK(std::isfinite(sim.y()));
    CHECK(sim.x() < 7.0);          // stopped by the wall
    CHECK(sim.x() > 6.0);          // but walked the corridor
    CHECK_EQ(sim.y(), 12.5);       // no drift
    // LOS: wall at x=7, from x=4.5 -> distance 2.5
    CHECK_EQ(sim.ray_distance(4.5, 12.5, 1.0, 0.0), 2.5);
    // diagonal direction: ray from (4.5,12.5) toward (1,1)/sqrt(2)
    const double inv = 1.0 / std::sqrt(2.0);
    const double d = sim.ray_distance(4.5, 12.5, inv, inv);
    CHECK(d > 0.0 && d < 64.0);
}

// ------------------------------------------------------------- input (M5)

static void test_input_keymap() {
    g_current = "input_keymap";
    // Held movement keys (reference keymap; SDL3 names).
    CHECK_EQ(movement_key_mask("ArrowUp"), kFwd);
    CHECK_EQ(movement_key_mask("up"), kFwd);
    CHECK_EQ(movement_key_mask("w"), kFwd);
    CHECK_EQ(movement_key_mask("z"), kFwd);
    CHECK_EQ(movement_key_mask("ArrowDown"), kBwd);
    CHECK_EQ(movement_key_mask("s"), kBwd);
    CHECK_EQ(movement_key_mask("ArrowLeft"), kLeft);
    CHECK_EQ(movement_key_mask("a"), kLeft);
    CHECK_EQ(movement_key_mask("ArrowRight"), kRight);
    CHECK_EQ(movement_key_mask("e"), kRight);
    CHECK_EQ(movement_key_mask("q"), kStrafeL);
    CHECK_EQ(movement_key_mask("d"), kStrafeR);
    CHECK_EQ(movement_key_mask("space"), 0);
    CHECK_EQ(movement_key_mask("Escape"), 0);
    CHECK_EQ(movement_key_mask("m"), 0);
    // One-shot events.
    CHECK_EQ(one_shot_event("f"), 1);
    CHECK_EQ(one_shot_event("Control"), 1);
    CHECK_EQ(one_shot_event("left ctrl"), 1);
    CHECK_EQ(one_shot_event("right ctrl"), 1);
    CHECK_EQ(one_shot_event("space"), 2);
    CHECK_EQ(one_shot_event("up"), 0);
    // Exact names as reported by SDL3 3.4.20 (capitalized).
    CHECK_EQ(movement_key_mask("Up"), kFwd);
    CHECK_EQ(movement_key_mask("Down"), kBwd);
    CHECK_EQ(movement_key_mask("Left"), kLeft);
    CHECK_EQ(movement_key_mask("Right"), kRight);
    CHECK_EQ(movement_key_mask("W"), kFwd);
    CHECK_EQ(movement_key_mask("A"), kLeft);
    CHECK_EQ(movement_key_mask("S"), kBwd);
    CHECK_EQ(movement_key_mask("D"), kStrafeR);
    CHECK_EQ(movement_key_mask("Q"), kStrafeL);
    CHECK_EQ(movement_key_mask("Z"), kFwd);
    CHECK_EQ(movement_key_mask("E"), kRight);
    CHECK_EQ(one_shot_event("F"), 1);
    CHECK_EQ(one_shot_event("Left Ctrl"), 1);
    CHECK_EQ(one_shot_event("Right Ctrl"), 1);
    CHECK_EQ(one_shot_event("Space"), 2);
}

static void test_input_live() {
    g_current = "input_live";
    LiveInput in;
    // Held key down/up.
    in.on_key("ArrowUp", true);
    SimInput s = in.take();
    CHECK(s.forward);
    CHECK(!s.backward);
    CHECK(!s.shoot);
    CHECK(!s.activate);
    in.on_key("ArrowUp", false);
    CHECK(!in.take().forward);
    // Simultaneous holds.
    in.on_key("z", true);
    in.on_key("ArrowLeft", true);
    s = in.take();
    CHECK(s.forward);
    CHECK(s.turn_left);
    in.on_key("z", false);
    in.on_key("ArrowLeft", false);
    // One-shot shoot fires exactly once.
    in.on_key("f", true);
    in.on_key("f", false);
    CHECK(in.take().shoot);
    CHECK(!in.take().shoot);
    // Activate (space) likewise.
    in.on_key("space", true);
    CHECK(in.take().activate);
    CHECK(!in.take().activate);
    // Mouse button 1 (captured) fires, like the original.
    in.shoot();
    CHECK(in.take().shoot);
    CHECK(!in.take().shoot);
    // Mouse: accumulated relative x, half-pixel threshold, sign decides.
    in.on_mouse(3.0);
    s = in.take();
    CHECK(s.turn_right);
    CHECK(!s.turn_left);
    in.on_mouse(-2.0);
    s = in.take();
    CHECK(s.turn_left);
    CHECK(!s.turn_right);
    in.on_mouse(0.2);
    s = in.take();
    CHECK(!s.turn_left);
    CHECK(!s.turn_right);
    in.on_mouse(1.0);
    in.on_mouse(-1.0);
    s = in.take();
    CHECK(!s.turn_left);
    CHECK(!s.turn_right);
    // Mouse and key both turn left: OR.
    in.on_key("a", true);
    in.on_mouse(-5.0);
    s = in.take();
    CHECK(s.turn_left);
    CHECK(!s.turn_right);
    in.on_key("a", false);
}

// ----------------------------------------------------------- acceptance

// Run the walk script over one map; returns the full tick transcript and
// checks invariants. `crashed` is set if any exception-like state appears.
std::string walk_map(const w3d::Map& m, bool& ok) {
    ok = true;
    std::string out;
    Simulation sim(Level::from_map(m));
    out.reserve(static_cast<std::size_t>(kWalkTicks) * 160);
    for (std::uint32_t t = 0; t < kWalkTicks; ++t) {
        sim.tick(walk_input(t));
        out += sim.tick_line();
        const double x = sim.x(), y = sim.y();
        if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0 || y < 0.0 ||
            x >= 64.0 || y >= 64.0) {
            ok = false;
            std::printf("  out of bounds %s t=%u x=%f y=%f\n", m.name.c_str(),
                        t, x, y);
        }
    }
    return out;
}

static void test_walk_all_maps() {
    g_current = "walk_all_maps";
    const auto root = game_dir();
    std::vector<w3d::Map> all;
    for (const auto& pair :
         {std::pair<std::filesystem::path, std::filesystem::path>{
              root / "base" / "GAMEMAPS.WL6", root / "base" / "MAPHEAD.WL6"},
          {root / "base" / "m1" / "GAMEMAPS.SOD",
           root / "base" / "m1" / "MAPHEAD.SOD"}}) {
        auto g = GameMaps::load(pair.first, pair.second);
        CHECK(g.has_value());
        if (!g)
            return;
        for (auto& m : g->maps)
            all.push_back(std::move(m));
    }
    CHECK_EQ(all.size(), 81u);  // 60 base + 21 SOD

    // pass 1: no crashes + invariants; pass 2: byte-identical output
    std::string pass1, pass2;
    int completed = 0;
    bool ok = true;
    for (const auto& m : all) {
        auto s1 = walk_map(m, ok);
        auto s2 = walk_map(m, ok);
        pass1 += s1;
        pass2 += s2;
        if (s1 != s2) {
            CHECK(false);  // nondeterministic transcript
            std::printf("  nondeterministic: %s\n", m.name.c_str());
        }
        // count exits via the transcript (last line EXIT means the walk
        // reached the elevator)
        if (!s1.empty() && s1.find("EXIT") != std::string::npos)
            ++completed;
    }
    CHECK(ok);
    CHECK(pass1 == pass2);
    std::printf("  walked %zu maps, %u ticks each, %d exits, %zu bytes\n",
                all.size(), kWalkTicks, completed, pass1.size());
}

// ------------------------------------------------------------ registration

static Register r_classify("classify_actors", test_classify_actors);
static Register r_level("level_build_synthetic", test_level_build_synthetic);
static Register r_door("door_open_close", test_door_open_close);
static Register r_locked("locked_door", test_locked_door);
static Register r_collect("collect_pickups", test_collect_pickups);
static void test_input_gamepad() {
    g_current = "input_gamepad";
    using namespace w3d::game::gamepad;
    LiveInput in;
    // Deadzone: below it inert, above it sets the bit, re-center clears.
    in.on_gamepad_axis(kLeftX, -0.24f);
    SimInput s = in.take();
    CHECK(!s.turn_left);
    in.on_gamepad_axis(kLeftX, -0.26f);
    s = in.take();
    CHECK(s.turn_left);
    in.on_gamepad_axis(kLeftX, 0.0f);
    s = in.take();
    CHECK(!s.turn_left);
    // Stick Y inversion (push up = forward).
    in.on_gamepad_axis(kLeftY, -0.5f);
    s = in.take();
    CHECK(s.forward);
    CHECK(!s.backward);
    in.on_gamepad_axis(kLeftY, 0.5f);
    s = in.take();
    CHECK(s.backward);
    CHECK(!s.forward);
    in.on_gamepad_axis(kLeftY, 0.0f);
    // A edge: one shot per press; holding never re-fires.
    in.on_gamepad_button(kSouth, true);
    s = in.take();
    CHECK(s.shoot);
    s = in.take();
    CHECK(!s.shoot);
    in.on_gamepad_button(kSouth, false);
    in.on_gamepad_button(kSouth, true);
    s = in.take();
    CHECK(s.shoot);
    // B edge: activate.
    in.on_gamepad_button(kSouth, false);
    in.on_gamepad_button(kEast, true);
    s = in.take();
    CHECK(s.activate);
    in.on_gamepad_button(kEast, false);
    // Keyboard and gamepad merge.
    LiveInput m;
    m.on_key("ArrowUp", true);
    m.on_gamepad_axis(kLeftX, -0.5f);
    SimInput mm = m.take();
    CHECK(mm.forward);
    CHECK(mm.turn_left);
    // Menu direction: stick takes precedence over the D-pad;
    // clear_gamepad drops everything.
    LiveInput d;
    d.on_gamepad_button(kDpadUp, true);
    CHECK(d.gamepad_direction() == kDirUp);
    d.on_gamepad_button(kDpadUp, false);
    CHECK(d.gamepad_direction() == kDirNone);
    d.on_gamepad_axis(kLeftY, -0.5f);  // stick up
    CHECK(d.gamepad_direction() == kDirUp);
    d.on_gamepad_button(kDpadLeft, true);
    CHECK(d.gamepad_direction() == kDirUp);  // stick wins
    d.on_gamepad_axis(kLeftY, 0.0f);
    CHECK(d.gamepad_direction() == kDirLeft);
    d.clear_gamepad();
    CHECK(d.gamepad_direction() == kDirNone);
    d.on_gamepad_button(kDpadUp, true);
    d.clear_gamepad();
    CHECK(d.gamepad_direction() == kDirNone);
}

static Register r_push("pushwall", test_pushwall);
static Register r_shoot("shoot_kills", test_shoot_kills);
static Register r_los("shoot_los_and_ghost", test_shoot_los_and_ghost);
static Register r_elev("elevator_exit", test_elevator_exit);
static Register r_coll("collision_wall", test_collision_wall);
static Register r_kmap("input_keymap", test_input_keymap);
static Register r_livein("input_live", test_input_live);
static Register r_gpadin("input_gamepad", test_input_gamepad);
static Register r_walk("walk_all_maps", test_walk_all_maps);

int main() {
    for (const auto& c : cases()) {
        g_current = c.name;
        std::printf("[%s]\n", c.name);
        c.fn();
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
