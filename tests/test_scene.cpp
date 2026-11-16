// tests/test_scene.cpp — Phase 5 WS2: scene state machine + menu model
// (headless; no SDL, no Vulkan, no game data).
//
// Covers the skeleton flow Title -> NewGame -> LevelIntro -> Game:
// keyboard navigation, mouse hover/click (against the same layout the
// render pass uses), transition table, quit semantics, and MenuDraw
// composition sanity (element counts, text bounds).
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app/menu.hpp"
#include "app/scene.hpp"

using namespace w3d::app;

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

void keydown(SceneState& s, const char* k) {
    InputEvent e;
    e.kind = InputEvent::KeyDown;
    e.key = k;
    s.on_input(e);
}
void mouse_move(SceneState& s, int x, int y) {
    InputEvent e;
    e.kind = InputEvent::MouseMove;
    e.mx = x;
    e.my = y;
    s.on_input(e);
}
void mouse_click(SceneState& s, int x, int y) {
    InputEvent e;
    e.kind = InputEvent::MouseClick;
    e.mx = x;
    e.my = y;
    s.on_input(e);
}

void test_title_flow() {
    SceneState s;
    CHECK(s.scene() == Scene::Title);
    keydown(s, "return");
    CHECK(s.scene() == Scene::NewGame);
    CHECK(s.selection() == 0);
    // Any key works ("press any key"), not just Enter.
    SceneState a;
    keydown(a, "z");
    CHECK(a.scene() == Scene::NewGame);
    // A click also starts.
    SceneState c;
    mouse_click(c, 10, 10);
    CHECK(c.scene() == Scene::NewGame);
    // Esc on the title quits the app (and leaves the scene).
    SceneState t;
    keydown(t, "escape");
    CHECK(t.quit_requested());
    CHECK(t.scene() == Scene::Title);
}

void test_episode_navigation() {
    SceneState s;
    keydown(s, "x");  // -> NewGame
    keydown(s, "down");
    CHECK(s.selection() == 1);
    keydown(s, "s");
    CHECK(s.selection() == 2);
    keydown(s, "down");
    keydown(s, "down");
    keydown(s, "down");
    CHECK(s.selection() == 1);  // wraps 4 -> 0 -> 1
    keydown(s, "up");
    CHECK(s.selection() == 0);
    keydown(s, "w");
    CHECK(s.selection() == 3);  // wraps the other way
    keydown(s, "return");
    CHECK(s.scene() == Scene::LevelIntro);
    CHECK(s.episode() == 3);
    CHECK(s.level() == SceneState::kEpisodes[3].first_level);
    keydown(s, "return");
    CHECK(s.scene() == Scene::Game);
    // Stub: Esc leaves the Game scene to the title (no quit).
    keydown(s, "escape");
    CHECK(s.scene() == Scene::Title);
    CHECK(!s.quit_requested());
}

void test_mouse_flow() {
    SceneState s;
    s.set_window(640, 400);
    keydown(s, "x");
    std::array<MenuLayout, SceneState::kEpisodeCount> lay;
    layout_episode_items(640, 400, lay);
    // Hover follows the mouse and drives the selection.
    mouse_move(s, lay[2].x + 5, lay[2].y + 5);
    CHECK(s.hover() == 2);
    CHECK(s.selection() == 2);
    mouse_move(s, 5, 5);
    CHECK(s.hover() == -1);
    // A click on an item confirms that episode.
    mouse_click(s, lay[1].x + 5, lay[1].y + 5);
    CHECK(s.scene() == Scene::LevelIntro);
    CHECK(s.episode() == 1);
    CHECK(s.level() == 16);
    // A click on empty space does nothing.
    SceneState s2;
    s2.set_window(640, 400);
    keydown(s2, "x");
    mouse_click(s2, 5, 5);
    CHECK(s2.scene() == Scene::NewGame);
    CHECK(s2.selection() == 0);
    // Esc backs out to the title.
    keydown(s2, "escape");
    CHECK(s2.scene() == Scene::Title);
}

void test_menu_layout() {
    std::array<MenuLayout, 4> lay;
    layout_episode_items(640, 400, lay);
    CHECK(lay[0].w == 420);
    CHECK(lay[0].h == 32);
    CHECK(lay[3].y > lay[2].y);
    CHECK(hit_episode_item(lay, lay[0].x, lay[0].y) == 0);
    CHECK(hit_episode_item(lay, 0, 0) == -1);
    CHECK(hit_episode_item(lay, 639, 399) == -1);

    // MenuDraw composition: element counts per scene + text bounds.
    SceneState s;
    s.set_window(640, 400);
    MenuDraw d;
    build_menu(s, 640, 400, d);
    CHECK(d.width == 640);
    CHECK(d.height == 400);
    CHECK(d.count == 3);  // title: logo + subtitle + press-any-key
    for (std::uint32_t i = 0; i < d.count; ++i) {
        CHECK(d.els[i].len <= kMenuTextLen - 1);
        CHECK(d.els[i].len == static_cast<float>(std::strlen(
                                  reinterpret_cast<const char*>(
                                      d.els[i].text))));
    }
    keydown(s, "x");
    build_menu(s, 640, 400, d);
    CHECK(d.count == 6);  // title + 4 episode items + hint
    // Exactly one selected item, and it carries the box flag.
    int selected = -1;
    for (std::uint32_t i = 0; i < d.count; ++i)
        if (d.els[i].flags & kMenuSelected)
            selected = static_cast<int>(i);
    CHECK(selected == 1);  // element 0 is the heading; items are 1..4
    keydown(s, "return");
    build_menu(s, 640, 400, d);
    CHECK(d.count == 3);  // level intro
}

void test_game_stub_and_hooks() {
    SceneState s = SceneState::from_name("game");
    CHECK(s.scene() == Scene::Game);
    CHECK(s.episode() == 0);
    CHECK(s.level() == 0);
    keydown(s, "escape");
    CHECK(s.scene() == Scene::Title);
    CHECK(SceneState::from_name("newgame").scene() == Scene::NewGame);
    CHECK(SceneState::from_name("levelintro").scene() == Scene::LevelIntro);
    CHECK(SceneState::from_name("bogus").scene() == Scene::Title);
    // LevelIntro: any key or click enters the Game scene.
    SceneState li = SceneState::from_name("levelintro");
    mouse_click(li, 0, 0);
    CHECK(li.scene() == Scene::Game);
}

}  // namespace

// ------------------------------------------------------------ registration
static Register r_title("title_flow", test_title_flow);
static Register r_nav("episode_navigation", test_episode_navigation);
static Register r_mouse("mouse_flow", test_mouse_flow);
static Register r_layout("menu_layout", test_menu_layout);
static Register r_stub("game_stub_and_hooks", test_game_stub_and_hooks);

int main() {
    for (const auto& c : cases()) {
        g_current = c.name;
        std::printf("[%s]\n", c.name);
        c.fn();
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
