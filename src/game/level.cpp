// w3d game/level.cpp — build the level model (reference setupLevel port).
#include "game/level.hpp"

namespace w3d::game {

Level Level::from_map(const w3d::Map& m) {
    Level lv;
    lv.name = m.name;
    lv.plane0 = m.plane0;
    lv.plane1 = m.plane1;
    lv.actors.reserve(512);

    for (std::uint16_t y = 0; y < w3d::kMapSide; ++y) {
        for (std::uint16_t x = 0; x < w3d::kMapSide; ++x) {
            const auto idx = static_cast<std::size_t>(y) * w3d::kMapSide + x;
            // structural (reference: m0 <= 63 wall, 90..101 door, both block)
            const std::uint16_t m0 = m.cell0(x, y);
            if (m0 <= 63 || (90 <= m0 && m0 <= 101))
                lv.blocking[idx] = true;

            // entities
            const std::uint16_t m1 = m.cell1(x, y);
            if (19 <= m1 && m1 <= 22) {
                // player start (19=N, 20=E, 21=S, 22=W)
                lv.has_start = true;
                lv.px = x + 0.5;
                lv.py = y + 0.5;
                switch (m1) {
                    case 19: lv.pdx = 0.0; lv.pdy = -1.0; break;
                    case 20: lv.pdx = 1.0; lv.pdy = 0.0; break;
                    case 21: lv.pdx = 0.0; lv.pdy = 1.0; break;
                    default: lv.pdx = -1.0; lv.pdy = 0.0; break;
                }
            } else if (23 <= m1 && m1 <= 70) {
                // props (collectibles included); blocking ones set plane2
                const ActorDef& d = classify_actor(m1);
                Actor a;
                a.x = x;
                a.y = y;
                a.def = &d;
                a.sprite = d.sprite;
                lv.actors.push_back(a);
                if (d.blocking)
                    lv.blocking[idx] = true;
                if (d.treasure)
                    lv.total_treasures += 1;
            } else if (m1 == 98) {
                // pushwall (reference keeps no Thing for it)
                lv.total_secrets += 1;
            } else if (m1 == 124) {
                // dead guard (decorative)
                const ActorDef& d = classify_actor(m1);
                Actor a;
                a.x = x;
                a.y = y;
                a.def = &d;
                a.sprite = d.sprite;
                lv.actors.push_back(a);
            } else if (m1 >= 108) {
                const ActorDef& d = classify_actor(m1);
                if (d.kind == ActorKind::kEnemy || d.kind == ActorKind::kBoss) {
                    Actor a;
                    a.x = x;
                    a.y = y;
                    a.def = &d;
                    a.direction = actor_direction(m1);
                    a.alive = true;
                    a.sprite = d.sprite;
                    lv.actors.push_back(a);
                    lv.total_kills += 1;
                } else if (d.kind == ActorKind::kGhost) {
                    // animated Thing, 2-frame loop [s, s+1], not killable
                    Actor a;
                    a.x = x;
                    a.y = y;
                    a.def = &d;
                    a.alive = false;
                    a.sprite = d.sprite;
                    a.animating = true;
                    a.anim_sprites[0] = d.sprite;
                    a.anim_sprites[1] = static_cast<std::uint16_t>(d.sprite + 1);
                    a.anim_frames = 2;
                    a.anim_loop = true;
                    lv.actors.push_back(a);
                }
                // other m1 >= 108 values (hard tier, SoD extras): inert
            }
            // m1 0-18, 71-97, 99: no reference behavior; inert
        }
    }
    return lv;
}

} // namespace w3d::game
