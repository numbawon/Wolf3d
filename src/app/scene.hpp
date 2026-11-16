// w3d app/scene.hpp — scene state machine for the Phase 5 UI flow (WS2).
//
// Pure logic (no SDL / no Vulkan): the app translates SDL events into
// InputEvent and feeds them in, then asks which scene to render. The sim
// and the renderer live in the app; this machine only tracks where in the
// Title -> NewGame -> LevelIntro -> Game flow the player is.
//
// WS2 skeleton flow (the full screen set — pause, score, death, episode
// end, high scores — lands in later workstreams):
//   Title -> NewGame (episode select) -> LevelIntro -> Game (stub)
//   Game: Esc -> Title.  Title: Esc -> quit (app checks quit_requested()).
//
// Menu navigation: keyboard (arrows / W-S-A-D move, Enter confirms, Esc
// backs out) and mouse (hover selects, left click confirms) drive the
// same state. Key names arrive lower-cased from the app (SDL3 3.4.x
// reports them capitalized; the scene machine is case-locked to lower).
#pragma once

#include <cstdint>
#include <string>

namespace w3d::app {

enum class Scene { Title, NewGame, LevelIntro, Game };

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

    Scene scene_ = Scene::Title;
    int sel_ = 0;       // selected episode (NewGame)
    int hover_ = -1;    // hovered episode under the mouse, -1 = none
    int episode_ = 0;   // confirmed episode
    int level_ = 0;     // level for LevelIntro / Game
    bool quit_ = false;
    int win_w_ = 640, win_h_ = 400;
};

}  // namespace w3d::app
