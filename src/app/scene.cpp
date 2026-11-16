// w3d app/scene.cpp — scene state machine (WS2).
#include "app/scene.hpp"

#include <array>

#include "app/menu.hpp"

namespace w3d::app {

namespace {
bool same(const std::string& a, const char* b) { return a == b; }
}  // namespace

void SceneState::set_window(int w, int h) {
    if (w > 0)
        win_w_ = w;
    if (h > 0)
        win_h_ = h;
}

void SceneState::move_selection(int delta) {
    sel_ = ((sel_ + delta) % kEpisodeCount + kEpisodeCount) % kEpisodeCount;
}

void SceneState::set_selection(int i) {
    if (i < 0 || i >= kEpisodeCount)
        return;
    sel_ = i;
}

void SceneState::confirm_episode() {
    episode_ = sel_;
    level_ = kEpisodes[sel_].first_level;
    scene_ = Scene::LevelIntro;
}

void SceneState::on_input(const InputEvent& ev) {
    switch (scene_) {
    case Scene::Title:
        // "Press any key" (original title behavior); Esc quits the app.
        if (ev.kind == InputEvent::KeyDown) {
            if (same(ev.key, "escape")) {
                quit_ = true;
                return;
            }
            sel_ = 0;
            hover_ = -1;
            scene_ = Scene::NewGame;
        } else if (ev.kind == InputEvent::MouseClick) {
            sel_ = 0;
            hover_ = -1;
            scene_ = Scene::NewGame;
        }
        break;

    case Scene::NewGame:
        if (ev.kind == InputEvent::KeyDown) {
            if (same(ev.key, "up") || same(ev.key, "w")) {
                move_selection(-1);
            } else if (same(ev.key, "down") || same(ev.key, "s")) {
                move_selection(1);
            } else if (same(ev.key, "left") || same(ev.key, "a")) {
                move_selection(-1);
            } else if (same(ev.key, "right") || same(ev.key, "d")) {
                move_selection(1);
            } else if (same(ev.key, "return")) {
                confirm_episode();
            } else if (same(ev.key, "escape")) {
                hover_ = -1;
                scene_ = Scene::Title;
            }
        } else if (ev.kind == InputEvent::MouseMove) {
            std::array<MenuLayout, kEpisodeCount> lay;
            layout_episode_items(win_w_, win_h_, lay);
            hover_ = hit_episode_item(lay, ev.mx, ev.my);
            if (hover_ >= 0)
                sel_ = hover_;
        } else if (ev.kind == InputEvent::MouseClick) {
            std::array<MenuLayout, kEpisodeCount> lay;
            layout_episode_items(win_w_, win_h_, lay);
            const int hit = hit_episode_item(lay, ev.mx, ev.my);
            if (hit >= 0) {
                // A click confirms the item under the cursor, even if the
                // keyboard selection pointed elsewhere.
                sel_ = hit;
                confirm_episode();
            }
        }
        break;

    case Scene::LevelIntro:
        // Any key or click starts the level (stub intro; WS8 makes it the
        // authentic screen).
        if (ev.kind == InputEvent::KeyDown || ev.kind == InputEvent::MouseClick)
            scene_ = Scene::Game;
        break;

    case Scene::Game:
        // Stub: Esc leaves to the title. (The pause menu is a later
        // workstream.)
        if (ev.kind == InputEvent::KeyDown && same(ev.key, "escape"))
            scene_ = Scene::Title;
        break;
    }
}

SceneState SceneState::from_name(const std::string& name) {
    SceneState s;
    if (name == "newgame") {
        s.scene_ = Scene::NewGame;
    } else if (name == "levelintro") {
        s.scene_ = Scene::LevelIntro;
        s.episode_ = 0;
        s.level_ = kEpisodes[0].first_level;
    } else if (name == "game") {
        s.scene_ = Scene::Game;
        s.episode_ = 0;
        s.level_ = kEpisodes[0].first_level;
    }
    return s;
}

}  // namespace w3d::app
