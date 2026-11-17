// tests/test_scene.cpp — Phase 5 WS2+WS4: scene state machine + menu model
// (headless; no SDL, no Vulkan, no game data).
//
// Covers the full flow Title (menu) -> NewGame -> LevelIntro -> Game ->
// Pause -> Options: keyboard navigation, mouse hover/click (against the
// same layout the render pass uses), transition table, quit semantics,
// option editing (value cycling, tabs, dirty flag, config diffing), and
// MenuDraw composition sanity (element counts, text bounds, centering).
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app/config.hpp"
#include "app/menu.hpp"
#include "app/repeat.hpp"
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
    // The title is a menu (WS4, Decision 1): NEW GAME / OPTIONS / QUIT.
    SceneState s;
    CHECK(s.scene() == Scene::Title);
    keydown(s, "return");  // item 0 = NEW GAME
    CHECK(s.scene() == Scene::NewGame);
    CHECK(s.selection() == 0);

    // Options item (1).
    SceneState o;
    keydown(o, "down");
    CHECK(o.selection() == 1);
    keydown(o, "return");
    CHECK(o.scene() == Scene::Options);
    CHECK(o.opt_return() == Scene::Title);
    CHECK(o.opt_tab() == 0);

    // Quit item (2) requests the quit; Esc on the title quits as well.
    SceneState q;
    keydown(q, "down");
    keydown(q, "down");
    CHECK(q.selection() == 2);
    keydown(q, "return");
    CHECK(q.quit_requested());
    SceneState e;
    keydown(e, "escape");
    CHECK(e.quit_requested());
    CHECK(e.scene() == Scene::Title);

    // Wrap: up from item 0 lands on QUIT.
    SceneState w;
    keydown(w, "up");
    CHECK(w.selection() == 2);

    // Mouse: click item 1 -> Options; item 0 -> NewGame; empty space
    // does nothing.
    SceneState c;
    c.set_window(640, 400);
    std::array<MenuLayout, 4> lay;
    layout_v_items(640, 400, 3, 40, lay);
    mouse_click(c, lay[1].x + 5, lay[1].y + 5);
    CHECK(c.scene() == Scene::Options);
    SceneState c2;
    c2.set_window(640, 400);
    mouse_click(c2, lay[0].x + 5, lay[0].y + 5);
    CHECK(c2.scene() == Scene::NewGame);
    SceneState c3;
    c3.set_window(640, 400);
    mouse_click(c3, 5, 5);
    CHECK(c3.scene() == Scene::Title);
    CHECK(!c3.quit_requested());
}

void test_episode_navigation() {
    SceneState s = SceneState::from_name("newgame");
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
    // WS4: Esc leaves the Game scene to the PAUSE scene (not the title).
    keydown(s, "escape");
    CHECK(s.scene() == Scene::Pause);
    CHECK(!s.quit_requested());
    CHECK(s.selection() == 0);
    // Esc on the pause resumes.
    keydown(s, "escape");
    CHECK(s.scene() == Scene::Game);
    // Pause -> Quit to Title.
    keydown(s, "escape");
    keydown(s, "down");
    keydown(s, "down");
    keydown(s, "return");
    CHECK(s.scene() == Scene::Title);
}

void test_mouse_flow() {
    SceneState s = SceneState::from_name("newgame");
    s.set_window(640, 400);
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
    SceneState s2 = SceneState::from_name("newgame");
    s2.set_window(640, 400);
    mouse_click(s2, 5, 5);
    CHECK(s2.scene() == Scene::NewGame);
    CHECK(s2.selection() == 0);
    // Esc backs out to the title.
    keydown(s2, "escape");
    CHECK(s2.scene() == Scene::Title);
}

// Every non-box text run sits exactly at (w - len*8*scale) / 2 — guards
// the hard-coded-width drift class (see add_centered in menu.cpp).
void check_centered(const MenuDraw& d, int w) {
    for (std::uint32_t i = 0; i < d.count; ++i) {
        if (d.els[i].flags & kMenuBox)
            continue;  // box text is left-padded, not centered
        const int len = static_cast<int>(d.els[i].len);
        const int sc = static_cast<int>(d.els[i].scale);
        CHECK(d.els[i].tx == (w - len * 8 * sc) / 2);
    }
}

// Text-run invariants for any scene: stored len == strlen, fits the POD.
void check_text_runs(const MenuDraw& d) {
    for (std::uint32_t i = 0; i < d.count; ++i) {
        CHECK(d.els[i].len <= kMenuTextLen - 1);
        CHECK(d.els[i].len == static_cast<float>(std::strlen(
                                  reinterpret_cast<const char*>(
                                      d.els[i].text))));
    }
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
    // layout_v_items is the shared geometry: 4 items must be identical to
    // the episode layout (the NewGame flow relies on that).
    std::array<MenuLayout, 4> v4;
    layout_v_items(640, 400, 4, 28, v4);
    CHECK(v4 == lay);

    // MenuDraw composition: element counts per scene + text bounds.
    SceneState s;
    s.set_window(640, 400);
    MenuDraw d;
    build_menu(s, 640, 400, d);
    CHECK(d.width == 640);
    CHECK(d.height == 400);
    CHECK(d.count == 6);  // title: logo + subtitle + 3 items + footer
    check_text_runs(d);
    check_centered(d, 640);

    s = SceneState::from_name("newgame");
    build_menu(s, 640, 400, d);
    CHECK(d.count == 6);  // title + 4 episode items + hint
    // Exactly one selected item, and it carries the box flag.
    int selected = -1;
    for (std::uint32_t i = 0; i < d.count; ++i)
        if (d.els[i].flags & kMenuSelected)
            selected = static_cast<int>(i);
    CHECK(selected == 1);  // element 0 is the heading; items are 1..4

    s = SceneState::from_name("levelintro");
    build_menu(s, 640, 400, d);
    CHECK(d.count == 3);  // level intro
    check_centered(d, 640);

    s = SceneState::from_name("pause");
    build_menu(s, 640, 400, d);
    CHECK(d.count == 5);  // PAUSED + 3 items + footer
    check_text_runs(d);
    check_centered(d, 640);
}

void test_options_flow() {
    // Open from the title.
    SceneState s;
    keydown(s, "down");
    keydown(s, "return");
    CHECK(s.scene() == Scene::Options);
    CHECK(s.opt_tab() == 0);
    CHECK(s.opt_sel() == 0);
    CHECK(s.take_config_dirty() == false);  // entering is not an edit

    // Video tab: res scale 1 -> 2 -> 3 -> 4 -> 1 (wraps).
    keydown(s, "right");
    CHECK(s.cfg().video.res_scale == 2);
    CHECK(s.take_config_dirty() == true);
    CHECK(s.take_config_dirty() == false);  // consumed
    keydown(s, "right");
    keydown(s, "right");
    CHECK(s.cfg().video.res_scale == 4);
    keydown(s, "right");
    CHECK(s.cfg().video.res_scale == 1);  // wraps

    // Filter: linear -> nearest -> integer -> linear (wraps).
    keydown(s, "down");
    CHECK(s.opt_sel() == 1);
    keydown(s, "right");
    CHECK(s.cfg().video.scale_filter == 1);
    keydown(s, "right");
    CHECK(s.cfg().video.scale_filter == 2);
    keydown(s, "right");
    CHECK(s.cfg().video.scale_filter == 0);

    // Display mode + show fps.
    keydown(s, "down");
    keydown(s, "right");
    CHECK(s.cfg().video.fullscreen_mode == 1);
    keydown(s, "down");
    keydown(s, "left");
    CHECK(s.cfg().video.show_fps == false);

    // Up wraps within the tab (4 rows).
    keydown(s, "up");
    keydown(s, "up");
    keydown(s, "up");
    CHECK(s.opt_sel() == 0);
    keydown(s, "up");
    CHECK(s.opt_sel() == 3);  // wraps

    // Audio tab: volumes step by 10 with wrap; device row is inert.
    keydown(s, "2");
    CHECK(s.opt_tab() == 1);
    CHECK(s.opt_sel() == 3);  // row index preserved (clamped) on tab switch
    keydown(s, "up");
    keydown(s, "up");
    keydown(s, "up");
    CHECK(s.opt_sel() == 0);
    keydown(s, "right");  // 100 wraps to 0
    CHECK(s.cfg().audio.master_volume == 0);
    keydown(s, "left");   // 0 wraps to 100
    CHECK(s.cfg().audio.master_volume == 100);
    keydown(s, "down");
    keydown(s, "right");
    CHECK(s.cfg().audio.music_volume == 0);  // 100 wraps to 0
    keydown(s, "down");
    keydown(s, "down");
    CHECK(s.opt_sel() == 3);
    keydown(s, "right");
    CHECK(s.cfg().audio.music_on == false);
    keydown(s, "down");
    CHECK(s.opt_sel() == 4);  // OUTPUT DEVICE
    const bool dirty_before = s.take_config_dirty();
    (void)dirty_before;
    keydown(s, "right");
    CHECK(s.take_config_dirty() == false);  // the device row edited nothing
    CHECK(s.cfg().audio.device_id.empty());

    // Controls tab: selection clamps; sensitivity cycles in 0.5 steps.
    keydown(s, "3");
    CHECK(s.opt_tab() == 2);
    CHECK(s.opt_sel() == 0);  // clamped from row 4
    keydown(s, "right");
    CHECK(s.cfg().controls.mouse_sensitivity == 1.5);
    keydown(s, "left");
    keydown(s, "left");
    CHECK(s.cfg().controls.mouse_sensitivity == 0.5);
    keydown(s, "left");
    CHECK(s.cfg().controls.mouse_sensitivity == 5.0);  // wraps

    // Gameplay tab: difficulty wraps 0..2; fog toggles.
    keydown(s, "4");
    CHECK(s.opt_tab() == 3);
    keydown(s, "right");
    CHECK(s.cfg().gameplay.difficulty == 2);  // regular -> hardcore
    keydown(s, "right");
    CHECK(s.cfg().gameplay.difficulty == 0);  // wraps to recruit
    keydown(s, "down");
    keydown(s, "right");
    CHECK(s.cfg().gameplay.fog == false);

    // "tab" wraps the tab row 3 -> 0; "1" jumps back.
    keydown(s, "tab");
    CHECK(s.opt_tab() == 0);
    keydown(s, "1");
    CHECK(s.opt_tab() == 0);

    // Esc returns to the scene it was opened from.
    keydown(s, "escape");
    CHECK(s.scene() == Scene::Title);
}

void test_options_via_pause() {
    SceneState s = SceneState::from_name("game");
    keydown(s, "escape");
    CHECK(s.scene() == Scene::Pause);
    // Navigate to the OPTIONS item (index 1) and confirm.
    keydown(s, "down");
    keydown(s, "return");
    CHECK(s.scene() == Scene::Options);
    CHECK(s.opt_return() == Scene::Pause);
    keydown(s, "escape");
    CHECK(s.scene() == Scene::Pause);  // back to the pause, not the title
    keydown(s, "return");  // RESUME (item 0)
    CHECK(s.scene() == Scene::Game);
}

void test_options_mouse() {
    SceneState s;
    s.set_window(640, 400);
    keydown(s, "down");
    keydown(s, "return");
    OptionsLayout lay;
    layout_options(640, 400, lay);
    // Hit-testing sanity: each tab and row picks itself, empty space -1.
    for (int i = 0; i < 4; ++i)
        CHECK(hit_options_tab(lay, lay.tabs[i].x + 1, lay.tabs[i].y + 1) == i);
    for (int i = 0; i < 5; ++i)
        CHECK(hit_options_row(lay, lay.rows[i].x + 1, lay.rows[i].y + 1) == i);
    CHECK(hit_options_tab(lay, 5, 5) == -1);
    CHECK(hit_options_row(lay, 5, 5) == -1);
    CHECK(hit_options_row(lay, lay.rows[4].x + 1, lay.rows[4].y + 1) == 4);

    // Hovering a tab highlights it but does not switch; clicking does.
    mouse_move(s, lay.tabs[2].x + 5, lay.tabs[2].y + 5);
    CHECK(s.opt_tab_hover() == 2);
    CHECK(s.opt_tab() == 0);
    mouse_click(s, lay.tabs[2].x + 5, lay.tabs[2].y + 5);
    CHECK(s.opt_tab() == 2);
    CHECK(s.opt_tab_hover() == -1);  // resets on switch

    // The controls tab has one row, selected after the switch: clicking
    // the already-selected row cycles its value.
    mouse_click(s, lay.rows[0].x + 5, lay.rows[0].y + 5);
    CHECK(s.cfg().controls.mouse_sensitivity == 1.5);
    CHECK(s.take_config_dirty() == true);

    // Back on video: a click on a DIFFERENT row selects it (no change);
    // a second click on the selected row cycles the value.
    mouse_click(s, lay.tabs[0].x + 5, lay.tabs[0].y + 5);
    mouse_click(s, lay.rows[1].x + 5, lay.rows[1].y + 5);
    CHECK(s.opt_sel() == 1);
    CHECK(s.cfg().video.scale_filter == 0);  // untouched by the select
    CHECK(s.take_config_dirty() == false);
    mouse_click(s, lay.rows[1].x + 5, lay.rows[1].y + 5);
    CHECK(s.cfg().video.scale_filter == 1);
    CHECK(s.take_config_dirty() == true);

    // Row hover is reported.
    mouse_move(s, lay.rows[0].x + 5, lay.rows[0].y + 5);
    CHECK(s.opt_hover() == 0);
    mouse_move(s, 5, 5);
    CHECK(s.opt_hover() == -1);

    // Menu composition: the video tab (4 rows) draws 10 elements:
    // heading + 4 tabs + 4 rows + footer. The audio tab has 5 rows.
    MenuDraw d;
    build_menu(s, 640, 400, d);
    CHECK(d.count == 1 + 4 + 4 + 1);
    SceneState a = SceneState::from_name("options:1");
    a.set_window(640, 400);
    build_menu(a, 640, 400, d);
    CHECK(d.count == 1 + 4 + 5 + 1);
    check_text_runs(d);
    check_centered(d, 640);
    // WS5: the audio rows are live (volumes + music apply at once); only
    // OUTPUT DEVICE stays stored-only (no hot device switch) and carries
    // the single "(PENDING)" row marker on the tab (the footer also
    // contains the word "PENDING" — match the parenthesized marker).
    int pending_rows = 0;
    bool pending_on_device = false;
    for (std::uint32_t i = 0; i < d.count; ++i) {
        const char* t = reinterpret_cast<const char*>(d.els[i].text);
        if (std::strstr(t, "(PENDING)")) {
            ++pending_rows;
            if (std::strstr(t, "OUTPUT DEVICE"))
                pending_on_device = true;
        }
    }
    CHECK(pending_rows == 1);
    CHECK(pending_on_device);
    // The video tab has no stored-only rows at all.
    SceneState vid = SceneState::from_name("options");
    vid.set_window(640, 400);
    build_menu(vid, 640, 400, d);
    int vid_pending = 0;
    for (std::uint32_t i = 0; i < d.count; ++i)
        if (std::strstr(reinterpret_cast<const char*>(d.els[i].text),
                        "(PENDING)"))
            ++vid_pending;
    CHECK(vid_pending == 0);
    // The video tab rows carry live values.
    SceneState v2 = SceneState::from_name("options");
    v2.set_window(640, 400);
    v2.cfg().video.res_scale = 3;
    build_menu(v2, 640, 400, d);
    bool saw_3x = false;
    for (std::uint32_t i = 0; i < d.count; ++i) {
        const char* t = reinterpret_cast<const char*>(d.els[i].text);
        if (std::strstr(t, "INTERNAL RESOLUTION  3x"))
            saw_3x = true;
    }
    CHECK(saw_3x);
}

void test_game_stub_and_hooks() {
    SceneState s = SceneState::from_name("game");
    CHECK(s.scene() == Scene::Game);
    CHECK(s.episode() == 0);
    CHECK(s.level() == 0);
    keydown(s, "escape");
    CHECK(s.scene() == Scene::Pause);  // WS4: pause, not title
    CHECK(SceneState::from_name("newgame").scene() == Scene::NewGame);
    CHECK(SceneState::from_name("levelintro").scene() == Scene::LevelIntro);
    CHECK(SceneState::from_name("bogus").scene() == Scene::Title);
    CHECK(SceneState::from_name("pause").scene() == Scene::Pause);
    CHECK(SceneState::from_name("options").scene() == Scene::Options);
    CHECK(SceneState::from_name("options").opt_return() == Scene::Title);
    CHECK(SceneState::from_name("options:2").opt_tab() == 2);
    CHECK(SceneState::from_name("options:9").opt_tab() == 0);
    CHECK(SceneState::from_name("optionssss").scene() == Scene::Options);
    // LevelIntro: any key or click enters the Game scene.
    SceneState li = SceneState::from_name("levelintro");
    mouse_click(li, 0, 0);
    CHECK(li.scene() == Scene::Game);
    // A loaded config is what the options screen edits.
    SceneState cs;
    Config in;
    in.video.res_scale = 3;
    in.gameplay.fog = false;
    cs.set_config(in);
    CHECK(cs.cfg().video.res_scale == 3);
    CHECK(cs.cfg().gameplay.fog == false);
    keydown(cs, "down");  // Title -> options
    keydown(cs, "return");
    CHECK(cs.scene() == Scene::Options);
    keydown(cs, "4");  // gameplay tab
    keydown(cs, "down");  // row 1 = FOG
    CHECK(cs.opt_sel() == 1);
    keydown(cs, "right");  // fog was false -> true
    CHECK(cs.cfg().gameplay.fog == true);
    CHECK(cs.take_config_dirty() == true);
}

}  // namespace

// ------------------------------------------------------------ registration
static void test_menu_repeat() {
    g_current = "menu_repeat";
    MenuRepeat r;
    CHECK(r.tick(0) == 0);
    // Press: fires immediately, repeats at frame 16, then every 8 frames.
    int fired[40] = {0};
    for (int i = 0; i < 40; ++i)
        fired[i] = r.tick(kDirUp);
    CHECK(fired[0] == kDirUp);
    for (int i = 1; i < 16; ++i)
        CHECK(fired[i] == kDirNone);
    CHECK(fired[16] == kDirUp);
    CHECK(fired[24] == kDirUp);
    CHECK(fired[32] == kDirUp);
    CHECK(fired[39] == kDirNone);
    // Release fires nothing; a re-press fires immediately again.
    CHECK(r.tick(kDirNone) == kDirNone);
    CHECK(r.tick(kDirDown) == kDirDown);
    // reset drops the held state.
    r.reset();
    CHECK(r.tick(kDirNone) == kDirNone);
    CHECK(r.tick(kDirLeft) == kDirLeft);
    // A mid-hold direction change fires immediately.
    MenuRepeat r2;
    CHECK(r2.tick(kDirUp) == kDirUp);
    for (int i = 0; i < 5; ++i)
        r2.tick(kDirUp);
    CHECK(r2.tick(kDirDown) == kDirDown);
    // The synthetic keys the repeat emits navigate a real menu.
    SceneState t = SceneState::from_name("title");
    t.set_window(640, 400);
    MenuRepeat mr;
    auto feed = [&t](int d) {
        if (d == kDirNone)
            return;
        InputEvent ie;
        ie.kind = InputEvent::KeyDown;
        ie.key = d == kDirDown ? "down" : "up";
        t.on_input(ie);
    };
    for (int i = 0; i < 16; ++i)
        feed(mr.tick(kDirDown));  // frames 0..15: only the press fires
    CHECK(t.selection() == 1);
    for (int i = 0; i < 8; ++i)
        feed(mr.tick(kDirDown));  // frame 16: the first repeat
    CHECK(t.selection() == 2);
    feed(mr.tick(kDirNone));
    CHECK(t.selection() == 2);
}

static Register r_title("title_flow", test_title_flow);
static Register r_mrep("menu_repeat", test_menu_repeat);
static Register r_nav("episode_navigation", test_episode_navigation);
static Register r_mouse("mouse_flow", test_mouse_flow);
static Register r_layout("menu_layout", test_menu_layout);
static Register r_opt("options_flow", test_options_flow);
static Register r_opa("options_via_pause", test_options_via_pause);
static Register r_omp("options_mouse", test_options_mouse);
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
