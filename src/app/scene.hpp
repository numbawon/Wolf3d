// w3d app/scene.hpp — scene state machine for the Phase 5 UI flow (WS2).
//
// Pure logic (no SDL / no Vulkan): the app translates SDL events into
// InputEvent and feeds them in, then asks which scene to render. The sim
// and the renderer live in the app; this machine only tracks where in the
// Title -> NewGame -> LevelIntro -> Game flow the player is.
//
// WS2 skeleton flow (the full screen set — score, death, episode end, high
// scores — lands in later workstreams); WS4 adds the settings flow:
//   Title (menu: New Game / Options / Quit)
//   Title -> NewGame (episode select) -> LevelIntro -> Game
//   Game: Esc -> Pause.  Pause: Esc/Enter -> Game (resume),
//     item Options -> Options, item Quit to Title -> Title.
//   Options (tabs Video/Audio/Controls/Gameplay, live-apply on change;
//     owned settings are persisted by the app when it sees cfg_dirty):
//     Esc -> back to the scene it was opened from (Title or Pause).
//   Title: Esc or the Quit item -> quit (app checks quit_requested()).
//
// Menu navigation: keyboard (arrows / W-S-A-D move, Enter confirms, Esc
// backs out) and mouse (hover selects, left click confirms) drive the
// same state. Key names arrive lower-cased from the app (SDL3 3.4.x
// reports them capitalized; the scene machine is case-locked to lower).
#pragma once

#include <cstdint>
#include <string>

#include "app/config.hpp"

namespace w3d::app {

enum class Scene { Title, NewGame, LevelIntro, Game, Pause, Options };

// One discrete input event.
struct InputEvent {
    enum Kind { None, KeyDown, MouseMove, MouseClick } kind = None;
    std::string key{};  // KeyDown: lower-cased key name
    int mx = 0, my = 0;  // MouseMove / MouseClick: logical window px
};

class SceneState {
  public:
    // The four original episodes (60 levels in the base game). Names are
    // the publicly documented ones; WS10 re-verifies them against the
    // original (transcription per the phase plan).
    struct Episode {
        const char* name;
        int first_level;  // 0-based level index
        int count;
    };
    static constexpr int kEpisodeCount = 4;
    static constexpr Episode kEpisodes[kEpisodeCount] = {
        {"EPISODE 1 - THE EARLY DAYS", 0, 16},
        {"EPISODE 2 - THE FORTRESS OF WOLFENSTEIN", 16, 16},
        {"EPISODE 3 - THE SECRET ARCHIVE", 32, 16},
        {"EPISODE 4 - THE SECRET MISSIONS", 48, 12},
    };

    [[nodiscard]] Scene scene() const { return scene_; }
    [[nodiscard]] int selection() const { return sel_; }
    [[nodiscard]] int hover() const { return hover_; }
    [[nodiscard]] int episode() const { return episode_; }
    [[nodiscard]] int level() const { return level_; }  // 0-based map index
    [[nodiscard]] bool quit_requested() const { return quit_; }

    // ---- Settings (WS4): the machine owns the Config the options screen
    // edits; the app loads it in once, diffs after take_config_dirty(),
    // live-applies the changed fields, and persists the file.
    [[nodiscard]] const Config& cfg() const { return cfg_; }
    [[nodiscard]] Config& cfg() { return cfg_; }
    void set_config(const Config& c) { cfg_ = c; }
    // True once after every user value change; reading it clears the flag.
    [[nodiscard]] bool take_config_dirty();
    // Options screen state (tab 0=Video 1=Audio 2=Controls 3=Gameplay).
    [[nodiscard]] int opt_tab() const { return opt_tab_; }
    [[nodiscard]] int opt_sel() const { return opt_sel_; }
    [[nodiscard]] int opt_hover() const { return opt_hover_; }
    [[nodiscard]] int opt_tab_hover() const { return opt_tab_hover_; }
    [[nodiscard]] Scene opt_return() const { return opt_return_; }
    // Rows per options tab (must match menu.cpp's row composition).
    static constexpr int kOptTabRows[4] = {4, 5, 1, 2};

    // Navigate the episode list (wraps). `i` is clamped to range.
    void move_selection(int delta);
    void set_selection(int i);

    // Feed one input event; applies the menu semantics of the current
    // scene (see the file header for the transition table).
    void on_input(const InputEvent& ev);
    // The window's logical size (used for mouse hit-testing against the
    // same layout the menu render pass uses — see menu.hpp).
    void set_window(int w, int h);

    // Test/harness hook: a SceneState already sitting in `name`
    // ("title" / "newgame" / "levelintro" / "game"; anything else =
    // title). levelintro/game start from episode 1, level 1.
    [[nodiscard]] static SceneState from_name(const std::string& name);

  private:
    void confirm_episode();
    void switch_opt_tab(int t);
    void cycle_option(int dir);
    void open_options_from(Scene from);

    Scene scene_ = Scene::Title;
    int sel_ = 0;       // selected item (Title / NewGame / Pause menus)
    int hover_ = -1;    // hovered item under the mouse, -1 = none
    int episode_ = 0;   // confirmed episode
    int level_ = 0;     // level for LevelIntro / Game
    bool quit_ = false;
    int win_w_ = 640, win_h_ = 400;

    // WS4 settings state.
    Config cfg_;
    bool cfg_dirty_ = false;
    int opt_tab_ = 0;
    int opt_sel_ = 0;
    int opt_hover_ = -1;
    int opt_tab_hover_ = -1;
    Scene opt_return_ = Scene::Title;
};

}  // namespace w3d::app
