// w3d game/actors.hpp — plane1 actor/object definitions.
//
// Semantics derived from the reference engine (vpoupet/wolfenstein,
// js/game.js — a public clean-room JS reimplementation, NOT id source) and
// the public map-format documentation (Gaarabis/Wolf-wiki tables as reprinted
// in the reference repo's docs/files.md and the 1999 XWolf "Map Data Values"
// page). All ranges were empirically checked against the shipped maps with
// tools/analyze_maps.py; every behavior constant here is backed by a golden
// test in tests/test_sim.cpp.
//
// plane1 value classes (reference loadLevel, game.js setupLevel):
//   19-22    player spawn, facing N/E/S/W (19=N, 20=E, 21=S, 22=W)
//   23-70    props, sprite = value - 21; most walk-through, some blocking
//   98       pushwall (secret) — counts toward totalSecrets
//   108-141, 144-177, 216-223, 234-241
//            regular enemies; direction = (value - band base) % 4,
//            patrolling when (value - band base) >= 4 (standing/patrolling
//            are behaviorally identical in the reference: enemies are static)
//   124      dead guard — decorative sprite 95, inert
//   160, 178, 179, 196, 197, 214, 215   bosses (always face the player)
//   224-227  pac-man ghosts — 2-frame looping animation, not killable
//
// Values present in the shipped data but NOT handled by the reference
// engine are inert in the simulation (kUnknown: not spawned, no effect on
// movement, doors, LOS or score):
//   71-74    SoD static objects (XWolf docs); outside reference prop range
//   90-97, 99
//            undocumented in every public spec found; the reference ignores
//            them. Present in both games (1621 base / 548 SOD cells) in
//            small clusters on open cells. UNRESOLVED — see docs/formats.md;
//            may render in the original, to check in the Phase 3 screenshot
//            comparison.
//   106, 107, 125, 142, 143, 161
//            SoD enemies (XWolf: Spectre/Angel/Trans/Uber/Will/Death); the
//            reference has no such classes
//   180-213  hard-difficulty regular enemies (public spec); the reference
//            has no difficulty setting and does not spawn them
//   252-259  hard-difficulty mutants (public spec); not spawned either
#pragma once

#include <array>
#include <cstdint>

namespace w3d::game {

enum class ActorKind : std::uint8_t {
    kNone = 0,   // empty cell
    kStart,      // player spawn (19-22)
    kProp,       // decorative sprite (23-70)
    kCollectible,// pickup (29, 43, 44, 47-56)
    kDeadGuard,  // 124
    kPushwall,   // 98
    kEnemy,      // regular enemy (spawned by the reference)
    kBoss,       // 160, 178, 179, 196, 197, 214, 215
    kGhost,      // 224-227
    kUnknown,    // inert: present in data, unhandled by the reference
};

enum class EnemyType : std::uint8_t {
    kGuard = 0,
    kOfficer,
    kSS,
    kDog,
    kZombie,
    kFakeHitler,
    kHitler,
    kFettgesicht,
    kSchabbs,
    kGretel,
    kHans,
    kOtto,
};

// Definition of one plane1 value (sprite + behavior flags).
struct ActorDef {
    ActorKind kind = ActorKind::kNone;
    std::uint16_t sprite = 0;         // main sprite index (VSWAP)
    std::uint16_t sprite_frames = 1;  // ghosts animate 2 frames
    bool blocking = false;            // blocks player movement (plane2)
    bool collectible = false;
    bool treasure = false;            // counts toward totalTreasures
    bool killable = false;            // dies in one shot (reference)
    bool orientable = false;          // direction-dependent sprite
    EnemyType enemy = EnemyType::kGuard;
    std::array<std::uint16_t, 8> death{}; // dying animation sprites
    std::uint8_t death_frames = 0;
    bool ghost_loop = false;          // ghost animation loops
};

// Classify a plane1 cell value the way the reference loadLevel does.
// The returned pointer is stable for the program's lifetime.
[[nodiscard]] const ActorDef& classify_actor(std::uint16_t m1);

// Facing direction (0=N, 1=E, 2=S, 3=W) for an orientable enemy value,
// exactly as the reference computes it: (m1 - band base) % 4. 0 for values
// that are not orientable enemies.
[[nodiscard]] std::uint8_t actor_direction(std::uint16_t m1);

// True when the reference spawns a live enemy for this value (regular
// bands + bosses). Ghosts are Things (not counted in totalKills).
[[nodiscard]] bool actor_is_spawnable_enemy(std::uint16_t m1);

} // namespace w3d::game
