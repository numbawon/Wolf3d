// w3d app/scene.cpp — scene state machine (WS2 + WS4 settings flow).
#include "app/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "app/menu.hpp"

namespace w3d::app {

namespace {
bool same(const std::string& a, const char* b) { return a == b; }

// Title / Pause menu items (WS4: the title is a proper modern menu, per
// Decision 1 — the original "press any key" is retired).
constexpr int kTitleItems = 3;  // New Game / Options / Quit
constexpr int kPauseItems = 3;  // Resume / Options / Quit to Title
}  // namespace

void SceneState::set_window(int w, int h) {
    if (w > 0)
        win_w_ = w;
    if (h > 0)
        win_h_ = h;
}

void SceneState::move_selection(int delta) {
    // The item count depends on the scene (3 for Title/Pause, 4 episodes).
    const int n = scene_ == Scene::NewGame
                      ? kEpisodeCount
                      : (scene_ == Scene::Pause ? kPauseItems : kTitleItems);
    sel_ = ((sel_ + delta) % n + n) % n;
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

bool SceneState::take_config_dirty() {
    const bool d = cfg_dirty_;
    cfg_dirty_ = false;
    return d;
}

void SceneState::open_options_from(Scene from) {
    opt_return_ = from;
    opt_tab_ = 0;
    opt_sel_ = 0;
    opt_hover_ = -1;
    opt_tab_hover_ = -1;
    scene_ = Scene::Options;
}

void SceneState::switch_opt_tab(int t) {
    if (t < 0 || t > 3)
        return;
    if (t == opt_tab_)
        return;
    opt_tab_ = t;
    opt_sel_ = std::min(opt_sel_, kOptTabRows[t] - 1);
    opt_hover_ = -1;
    opt_tab_hover_ = -1;
}

// Cycle the value of the selected options row (wraps at the ends). The
// device row (Audio row 4) is not adjustable until the WS5 device list.
void SceneState::cycle_option(int dir) {
    auto adj = [this, dir](int& v, int lo, int hi, int step) {
        v += dir * step;
        if (v > hi)
            v = lo;
        if (v < lo)
            v = hi;
        cfg_dirty_ = true;
    };
    auto adjb = [this](bool& v) {
        v = !v;
        cfg_dirty_ = true;
    };
    switch (opt_tab_) {
        case 0:  // Video (all live)
            switch (opt_sel_) {
                case 0:
                    adj(cfg_.video.res_scale, kResScaleMin, kResScaleMax, 1);
                    break;
                case 1:
                    adj(cfg_.video.scale_filter, 0, 2, 1);
                    break;
                case 2:
                    adj(cfg_.video.fullscreen_mode, 0, 2, 1);
                    break;
                case 3:
                    adjb(cfg_.video.show_fps);
                    break;
            }
            break;
        case 1:  // Audio (stored-only until WS5)
            switch (opt_sel_) {
                case 0:
                    adj(cfg_.audio.master_volume, kVolumeMin, kVolumeMax, 10);
                    break;
                case 1:
                    adj(cfg_.audio.music_volume, kVolumeMin, kVolumeMax, 10);
                    break;
                case 2:
                    adj(cfg_.audio.sfx_volume, kVolumeMin, kVolumeMax, 10);
                    break;
                case 3:
                    adjb(cfg_.audio.music_on);
                    break;
                case 4:
                    break;  // OUTPUT DEVICE: single entry until WS5
            }
            break;
        case 2:  // Controls: sensitivity only (live; bindings are WS9)
            if (opt_sel_ == 0) {
                // 10 UI steps: 0.5 .. 5.0 (the config range itself is
                // 0.1 .. 10.0, so a hand-edited value keeps working).
                int idx = static_cast<int>(std::lround(
                    cfg_.controls.mouse_sensitivity / 0.5));
                idx = std::clamp(idx, 1, 10);
                idx += dir;
                if (idx > 10)
                    idx = 1;
                if (idx < 1)
                    idx = 10;
                cfg_.controls.mouse_sensitivity = 0.5 * idx;
                cfg_dirty_ = true;
            }
            break;
        case 3:  // Gameplay: difficulty pending (WS7), fog live
            switch (opt_sel_) {
                case 0:
                    adj(cfg_.gameplay.difficulty, kDifficultyMin,
                        kDifficultyMax, 1);
                    break;
                case 1:
                    adjb(cfg_.gameplay.fog);
                    break;
            }
            break;
    }
}

void SceneState::on_input(const InputEvent& ev) {
    switch (scene_) {
        case Scene::Title:
            // Modern title menu (WS4): New Game / Options / Quit.
            if (ev.kind == InputEvent::KeyDown) {
                if (same(ev.key, "escape")) {
                    quit_ = true;
                    return;
                }
                if (same(ev.key, "up") || same(ev.key, "w") ||
                    same(ev.key, "left") || same(ev.key, "a")) {
                    move_selection(-1);
                } else if (same(ev.key, "down") || same(ev.key, "s") ||
                           same(ev.key, "right") || same(ev.key, "d")) {
                    move_selection(1);
                } else if (same(ev.key, "return")) {
                    if (sel_ == 0) {
                        sel_ = 0;
                        hover_ = -1;
                        scene_ = Scene::NewGame;
                    } else if (sel_ == 1) {
                        open_options_from(Scene::Title);
                    } else {
                        quit_ = true;
                    }
                }
            } else if (ev.kind == InputEvent::MouseMove) {
                std::array<MenuLayout, 4> lay;
                layout_v_items(win_w_, win_h_, kTitleItems, 40, lay);
                const int hit = hit_v_items(lay, kTitleItems, ev.mx, ev.my);
                hover_ = hit;
                if (hit >= 0)
                    sel_ = hit;
            } else if (ev.kind == InputEvent::MouseClick) {
                std::array<MenuLayout, 4> lay;
                layout_v_items(win_w_, win_h_, kTitleItems, 40, lay);
                const int hit = hit_v_items(lay, kTitleItems, ev.mx, ev.my);
                if (hit >= 0) {
                    sel_ = hit;
                    if (hit == 0) {
                        hover_ = -1;
                        scene_ = Scene::NewGame;
                    } else if (hit == 1) {
                        open_options_from(Scene::Title);
                    } else {
                        quit_ = true;
                    }
                }
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
                    // A click confirms the item under the cursor, even if
                    // the keyboard selection pointed elsewhere.
                    sel_ = hit;
                    confirm_episode();
                }
            }
            break;

        case Scene::LevelIntro:
            // Any key or click starts the level (stub intro; WS8 makes it
            // the authentic screen).
            if (ev.kind == InputEvent::KeyDown ||
                ev.kind == InputEvent::MouseClick)
                scene_ = Scene::Game;
            break;

        case Scene::Game:
            // Esc pauses (WS4).
            if (ev.kind == InputEvent::KeyDown && same(ev.key, "escape")) {
                sel_ = 0;
                hover_ = -1;
                scene_ = Scene::Pause;
            }
            break;

        case Scene::Pause:
            if (ev.kind == InputEvent::KeyDown) {
                if (same(ev.key, "escape")) {
                    // Esc resumes (standard pause behavior).
                    scene_ = Scene::Game;
                } else if (same(ev.key, "up") || same(ev.key, "w")) {
                    move_selection(-1);
                } else if (same(ev.key, "down") || same(ev.key, "s")) {
                    move_selection(1);
                } else if (same(ev.key, "left") || same(ev.key, "a")) {
                    move_selection(-1);
                } else if (same(ev.key, "right") || same(ev.key, "d")) {
                    move_selection(1);
                } else if (same(ev.key, "return")) {
                    if (sel_ == 0) {
                        scene_ = Scene::Game;
                    } else if (sel_ == 1) {
                        open_options_from(Scene::Pause);
                    } else {
                        scene_ = Scene::Title;
                    }
                }
            } else if (ev.kind == InputEvent::MouseMove) {
                std::array<MenuLayout, 4> lay;
                layout_v_items(win_w_, win_h_, kPauseItems, 28, lay);
                const int hit = hit_v_items(lay, kPauseItems, ev.mx, ev.my);
                hover_ = hit;
                if (hit >= 0)
                    sel_ = hit;
            } else if (ev.kind == InputEvent::MouseClick) {
                std::array<MenuLayout, 4> lay;
                layout_v_items(win_w_, win_h_, kPauseItems, 28, lay);
                const int hit = hit_v_items(lay, kPauseItems, ev.mx, ev.my);
                if (hit >= 0) {
                    sel_ = hit;
                    if (hit == 0) {
                        scene_ = Scene::Game;
                    } else if (hit == 1) {
                        open_options_from(Scene::Pause);
                    } else {
                        scene_ = Scene::Title;
                    }
                }
            }
            break;

        case Scene::Options:
            if (ev.kind == InputEvent::KeyDown) {
                if (same(ev.key, "escape")) {
                    scene_ = opt_return_;
                    sel_ = 0;  // both return scenes restart at the top item
                    hover_ = -1;
                } else if (same(ev.key, "1")) {
                    switch_opt_tab(0);
                } else if (same(ev.key, "2")) {
                    switch_opt_tab(1);
                } else if (same(ev.key, "3")) {
                    switch_opt_tab(2);
                } else if (same(ev.key, "4")) {
                    switch_opt_tab(3);
                } else if (same(ev.key, "tab") || same(ev.key, "pageup") ||
                           same(ev.key, "pagedown")) {
                    switch_opt_tab((opt_tab_ + 1) % 4);
                } else if (same(ev.key, "up") || same(ev.key, "w")) {
                    const int n = kOptTabRows[opt_tab_];
                    opt_sel_ = (opt_sel_ - 1 + n) % n;
                } else if (same(ev.key, "down") || same(ev.key, "s")) {
                    const int n = kOptTabRows[opt_tab_];
                    opt_sel_ = (opt_sel_ + 1) % n;
                } else if (same(ev.key, "left") || same(ev.key, "a")) {
                    cycle_option(-1);
                } else if (same(ev.key, "right") || same(ev.key, "d")) {
                    cycle_option(1);
                }
            } else if (ev.kind == InputEvent::MouseMove) {
                OptionsLayout lay;
                layout_options(win_w_, win_h_, lay);
                opt_tab_hover_ = hit_options_tab(lay, ev.mx, ev.my);
                opt_hover_ = hit_options_row(lay, ev.mx, ev.my);
            } else if (ev.kind == InputEvent::MouseClick) {
                OptionsLayout lay;
                layout_options(win_w_, win_h_, lay);
                const int tab = hit_options_tab(lay, ev.mx, ev.my);
                if (tab >= 0) {
                    switch_opt_tab(tab);
                    break;
                }
                const int row = hit_options_row(lay, ev.mx, ev.my);
                if (row >= 0) {
                    if (row == opt_sel_)
                        cycle_option(1);  // click again to change the value
                    else
                        opt_sel_ = row;
                }
            }
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
    } else if (name == "pause") {
        s.scene_ = Scene::Pause;
    } else if (name.rfind("options", 0) == 0) {
        // Harness hook: "options" or "options:<tab 0-3>" (menu-shot
        // evidence per tab).
        s.scene_ = Scene::Options;
        s.opt_return_ = Scene::Title;
        if (name.size() >= 9 && name[7] == ':') {
            const char t = name[8];
            if (t >= '0' && t <= '3')
                s.opt_tab_ = t - '0';
        }
    }
    return s;
}

}  // namespace w3d::app
