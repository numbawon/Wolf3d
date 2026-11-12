// w3d game/actors.cpp — plane1 actor table (data only; see actors.hpp).
#include "game/actors.hpp"

#include <algorithm>

namespace w3d::game {
namespace {

// Regular-enemy bands: (base, type). Eight values per band: 0-3 standing
// (any difficulty), 4-7 patrolling. The reference spawns standing and
// patrolling identically (enemies are static; there is no AI in it).
// Note the reference's zombie quirk: the 216-223 band computes direction as
// (m1 - 116) % 4, not (m1 - 216) % 4 (game.js line ~771). Mirrored here so
// facing stays byte-identical to the reference.
const std::pair<std::uint16_t, EnemyType> kEnemyBands[] = {
    {108, EnemyType::kGuard},   // easy/any
    {116, EnemyType::kOfficer},
    {126, EnemyType::kSS},
    {134, EnemyType::kDog},
    {144, EnemyType::kGuard},   // medium
    {152, EnemyType::kOfficer},
    {162, EnemyType::kSS},
    {170, EnemyType::kDog},
    {216, EnemyType::kZombie},  // easy/any (reference dir quirk, see below)
    {234, EnemyType::kZombie},  // medium
};

// Blocking prop values (reference game.js BLOCKING list).
const std::uint16_t kBlockingProps[] = {
    24, 25, 26, 28, 30, 31, 33, 34, 35, 36, 39, 40, 41, 45, 58, 59, 60, 62,
    63, 68, 69};

// Collectible prop values (reference game.js collectible list).
const std::uint16_t kCollectibleProps[] = {
    29, 43, 44, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56};

struct BossDef {
    std::uint16_t m1;
    std::uint16_t sprite;
    EnemyType type;
    std::array<std::uint16_t, 8> death;
    std::uint8_t death_frames;
};

// Boss sprites (reference Enemy subclasses, game.js ~587-666).
const BossDef kBosses[] = {
    {160, 321, EnemyType::kFakeHitler, {328, 329, 330, 331, 332, 333}, 6},
    {178, 349, EnemyType::kHitler, {353, 354, 355, 356, 357, 358, 359, 352}, 8},
    {179, 400, EnemyType::kFettgesicht, {404, 405, 406, 407}, 4},
    {196, 312, EnemyType::kSchabbs, {313, 314, 315, 316}, 4},
    {197, 389, EnemyType::kGretel, {393, 394, 395, 392}, 4},
    {214, 300, EnemyType::kHans, {304, 305, 306, 303}, 4},
    {215, 364, EnemyType::kOtto, {366, 367, 368, 369}, 4},
};

ActorDef make_none() { return {}; }

ActorDef make_prop(std::uint16_t m1) {
    ActorDef d;
    d.kind = ActorKind::kProp;
    d.sprite = static_cast<std::uint16_t>(m1 - 21);
    d.blocking = std::any_of(std::begin(kBlockingProps), std::end(kBlockingProps),
                             [&](std::uint16_t b) { return b == m1; });
    d.collectible = std::any_of(std::begin(kCollectibleProps),
                                std::end(kCollectibleProps),
                                [&](std::uint16_t c) { return c == m1; });
    d.treasure = 52 <= m1 && m1 <= 56;  // m1 52-56 -> sprites 31-35
    return d;
}

ActorDef make_enemy(EnemyType type, std::uint16_t sprite,
                    std::array<std::uint16_t, 8> death, std::uint8_t n) {
    ActorDef d;
    d.kind = ActorKind::kEnemy;
    d.sprite = sprite;
    d.killable = true;
    d.orientable = true;
    d.enemy = type;
    d.death = death;
    d.death_frames = n;
    return d;
}

// The full table, one entry per plane1 value 0..511.
std::array<ActorDef, 512>& table() {
    static std::array<ActorDef, 512> t = [] {
        std::array<ActorDef, 512> tbl;
        for (auto& d : tbl)
            d = make_none();

        for (std::uint16_t m1 = 19; m1 <= 22; ++m1) {
            tbl[m1].kind = ActorKind::kStart;
        }
        for (std::uint16_t m1 = 23; m1 <= 70; ++m1) {
            tbl[m1] = make_prop(m1);
        }
        tbl[98].kind = ActorKind::kPushwall;
        tbl[124].kind = ActorKind::kDeadGuard;
        tbl[124].sprite = 95;  // last guard-death frame

        // Regular enemies (only the bands the reference spawns).
        for (const auto& [base, type] : kEnemyBands) {
            ActorDef d;
            switch (type) {
                case EnemyType::kGuard:
                    d = make_enemy(type, 50, {90, 91, 92, 93, 95}, 5);
                    break;
                case EnemyType::kOfficer:
                    d = make_enemy(type, 238, {279, 280, 281, 283, 284}, 5);
                    break;
                case EnemyType::kSS:
                    d = make_enemy(type, 138, {179, 180, 181, 183}, 4);
                    break;
                case EnemyType::kDog:
                    d = make_enemy(type, 99, {131, 132, 133, 134}, 4);
                    break;
                default:  // zombies: dir quirk handled in actor_direction
                    d = make_enemy(type, 187, {228, 229, 230, 232, 233}, 5);
                    break;
            }
            for (std::uint16_t v = base; v < base + 8; ++v)
                tbl[v] = d;
        }

        for (const auto& b : kBosses) {
            ActorDef d;
            d.kind = ActorKind::kBoss;
            d.sprite = b.sprite;
            d.killable = true;
            d.enemy = b.type;
            d.death = b.death;
            d.death_frames = b.death_frames;
            tbl[b.m1] = d;
        }

        for (std::uint16_t m1 = 224; m1 <= 227; ++m1) {
            ActorDef d;
            d.kind = ActorKind::kGhost;
            d.sprite = static_cast<std::uint16_t>(288 + 2 * (m1 - 224));
            d.sprite_frames = 2;
            d.ghost_loop = true;
            tbl[m1] = d;
        }

        // Values present in the shipped data but unhandled by the reference
        // (see the header comment): inert, kind kUnknown. The kNone guard
        // keeps already-classified values (bosses 196/197 sit inside the
        // 180-213 hard band).
        auto mark_unknown = [&tbl](int lo, int hi) {
            for (int v = lo; v <= hi; ++v)
                if (tbl[static_cast<std::size_t>(v)].kind == ActorKind::kNone)
                    tbl[static_cast<std::size_t>(v)].kind = ActorKind::kUnknown;
        };
        mark_unknown(71, 74);    // SoD static objects (XWolf)
        mark_unknown(90, 97);    // undocumented in every public spec
        mark_unknown(99, 99);    // "
        mark_unknown(106, 107);  // SoD enemies (no reference classes)
        mark_unknown(125, 125);  // SoD Trans
        mark_unknown(142, 143);  // SoD Uber/Will
        mark_unknown(161, 161);  // SoD Death
        mark_unknown(180, 213);  // hard-difficulty regulars (not spawned)
        mark_unknown(252, 259);  // hard-difficulty mutants (not spawned)
        return tbl;
    }();
    return t;
}

} // namespace

const ActorDef& classify_actor(std::uint16_t m1) {
    static const ActorDef none{};
    if (m1 >= table().size())
        return none;
    return table()[m1];
}

std::uint8_t actor_direction(std::uint16_t m1) {
    for (const auto& [base, type] : kEnemyBands) {
        if (base <= m1 && m1 < base + 8) {
            if (type == EnemyType::kZombie && base == 216)
                // reference quirk: (m1 - 116) % 4 for the 216-223 band
                return static_cast<std::uint8_t>((m1 - 116) % 4);
            return static_cast<std::uint8_t>((m1 - base) % 4);
        }
    }
    return 0;
}

bool actor_is_spawnable_enemy(std::uint16_t m1) {
    const auto& d = classify_actor(m1);
    return d.kind == ActorKind::kEnemy || d.kind == ActorKind::kBoss;
}

} // namespace w3d::game
