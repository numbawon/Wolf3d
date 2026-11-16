// w3d app/menu.cpp — full-res menu composition (WS2).
#include "app/menu.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "app/scene.hpp"

namespace w3d::app {

namespace {

void add_text(MenuDraw& d, float x, float y, float scale, const char* text,
              std::uint32_t flags) {
    if (d.count >= kMaxMenuElements)
        return;
    MenuElement& e = d.els[d.count++];
    std::memset(&e, 0, sizeof(e));
    const int len = static_cast<int>(std::strlen(text));
    // len = characters actually stored, never the full (possibly longer)
    // string: the shader walks text[0..len-1].
    const int n = std::min(len, kMenuTextLen - 1);
    e.x = e.tx = x;
    e.y = e.ty = y;
    e.scale = scale;
    e.len = static_cast<float>(n);
    e.flags = flags;
    std::memcpy(e.text, text, static_cast<std::size_t>(n));
}

void add_box_text(MenuDraw& d, int x, int y, int w, int h, const char* text,
                  std::uint32_t flags) {
    if (d.count >= kMaxMenuElements)
        return;
    MenuElement& e = d.els[d.count++];
    std::memset(&e, 0, sizeof(e));
    e.x = static_cast<float>(x);
    e.y = static_cast<float>(y);
    e.w = static_cast<float>(w);
    e.h = static_cast<float>(h);
    // Text vertically centered in the box, 12 px left padding.
    e.tx = static_cast<float>(x + 12);
    e.ty = static_cast<float>(y + (h - 16) / 2);
    e.scale = 1.0f;
    e.flags = flags | kMenuBox;
    const int n = std::min<int>(static_cast<int>(std::strlen(text)),
                                kMenuTextLen - 1);
    // Same stored-count rule as add_text.
    e.len = static_cast<float>(n);
    std::memcpy(e.text, text, static_cast<std::size_t>(n));
}

void set_palette(MenuDraw& d) {
    auto put = [&d](int slot, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = d.palette + 4 * slot;
        p[0] = r;
        p[1] = g;
        p[2] = b;
        p[3] = 255;
    };
    // Dark, modern; gold for selection (the game's accent).
    put(kPalBg, 12, 12, 20);
    put(kPalItem, 34, 34, 46);
    put(kPalSel, 122, 92, 34);
    put(kPalText, 216, 216, 216);
    put(kPalTextSel, 255, 240, 200);
    put(kPalAccent, 214, 178, 94);
}

int center_x(int w, int len, int scale) {
    return (w - len * 8 * scale) / 2;
}

// Centered text: the width is derived from the string itself, so a
// changed or longer string can never drift off-center (hard-coded widths
// did — e.g. the 25-char episode 1 name centered on a 36-char guess).
void add_centered(MenuDraw& d, int w, float y, float scale, const char* text,
                  std::uint32_t flags) {
    add_text(d,
             static_cast<float>(center_x(
                 w, static_cast<int>(std::strlen(text)),
                 static_cast<int>(scale))),
             y, scale, text, flags);
}

}  // namespace

// Shared vertical-list geometry (420x32 boxes, 12 px apart, centered).
// `count` items (1..4), top offset `top` below the vertical center.
void layout_v_items(int w, int h, int count, int top,
                    std::array<MenuLayout, 4>& out) {
    constexpr int kBoxW = 420;
    constexpr int kBoxH = 32;
    constexpr int kGap = 12;
    const int n = std::clamp(count, 1, 4);
    const int total = n * kBoxH + (n - 1) * kGap;
    const int y0 = (h - total) / 2 + top;
    const int x = (w - kBoxW) / 2;
    for (int i = 0; i < n; ++i)
        out[i] = {x, y0 + i * (kBoxH + kGap), kBoxW, kBoxH};
    for (int i = n; i < 4; ++i)
        out[i] = {0, 0, 0, 0};
}

void layout_episode_items(int w, int h, std::array<MenuLayout, 4>& out) {
    layout_v_items(w, h, 4, 28, out);
}

int hit_v_items(const std::array<MenuLayout, 4>& lay, int count, int mx,
                int my) {
    const int n = std::clamp(count, 1, 4);
    for (int i = 0; i < n; ++i) {
        if (mx >= lay[i].x && mx < lay[i].x + lay[i].w && my >= lay[i].y &&
            my < lay[i].y + lay[i].h)
            return i;
    }
    return -1;
}

int hit_episode_item(const std::array<MenuLayout, 4>& lay, int mx, int my) {
    return hit_v_items(lay, 4, mx, my);
}

// ---- Options screen layout (WS4) ----
void layout_options(int w, int h, OptionsLayout& out) {
    // Tab label row: four 120x24 boxes, 12 px apart, centered.
    constexpr int kTabW = 120, kTabH = 24, kTabGap = 12;
    const int tab_total = 4 * kTabW + 3 * kTabGap;
    const int tab_x0 = (w - tab_total) / 2;
    // Option rows: 420x28 boxes, 10 px apart; up to 5 (the Audio tab).
    constexpr int kRowW = 420, kRowH = 28, kRowGap = 10, kMaxRows = 5;
    constexpr int kTabRowGap = 24;  // tabs -> first row
    const int rows_total = kMaxRows * kRowH + (kMaxRows - 1) * kRowGap;
    // Vertical block: tabs + gap + rows, centered a little above middle.
    const int block = kTabH + kTabRowGap + rows_total;
    const int tab_y = (h - block) / 2 + 12;
    const int row_x = (w - kRowW) / 2;
    for (int i = 0; i < 4; ++i)
        out.tabs[i] = {tab_x0 + i * (kTabW + kTabGap), tab_y, kTabW, kTabH};
    for (int i = 0; i < kMaxRows; ++i)
        out.rows[i] = {row_x, tab_y + kTabH + kTabRowGap +
                           i * (kRowH + kRowGap),
                       kRowW, kRowH};
}

int hit_options_tab(const OptionsLayout& lay, int mx, int my) {
    for (int i = 0; i < 4; ++i) {
        if (mx >= lay.tabs[i].x && mx < lay.tabs[i].x + lay.tabs[i].w &&
            my >= lay.tabs[i].y && my < lay.tabs[i].y + lay.tabs[i].h)
            return i;
    }
    return -1;
}

int hit_options_row(const OptionsLayout& lay, int mx, int my) {
    for (int i = 0; i < 5; ++i) {
        if (mx >= lay.rows[i].x && mx < lay.rows[i].x + lay.rows[i].w &&
            my >= lay.rows[i].y && my < lay.rows[i].y + lay.rows[i].h)
            return i;
    }
    return -1;
}

// One options row as a single monospace string: the label padded to 20
// cells, a space, the value, and "  (PENDING)" for stored-only settings
// (WS4: the whole Audio section + difficulty; the device list is WS5 and
// the difficulty semantics are WS7).
std::string option_row_text(const SceneState& s, int tab, int row) {
    const Config& c = s.cfg();
    const char* label = "";
    std::string value;
    bool pending = false;
    switch (tab) {
        case 0:  // Video (all live)
            switch (row) {
                case 0:
                    label = "INTERNAL RESOLUTION";
                    value = std::to_string(c.video.res_scale) + "x";
                    break;
                case 1:
                    label = "UPSCALE FILTER";
                    value = cfgname::scale_filter(c.video.scale_filter);
                    break;
                case 2:
                    label = "DISPLAY MODE";
                    value = cfgname::fullscreen_mode(c.video.fullscreen_mode);
                    break;
                case 3:
                    label = "SHOW FPS";
                    value = c.video.show_fps ? "ON" : "OFF";
                    break;
            }
            break;
        case 1:  // Audio (stored-only until WS5)
            pending = true;
            switch (row) {
                case 0:
                    label = "MASTER VOLUME";
                    value = std::to_string(c.audio.master_volume);
                    break;
                case 1:
                    label = "MUSIC VOLUME";
                    value = std::to_string(c.audio.music_volume);
                    break;
                case 2:
                    label = "SFX VOLUME";
                    value = std::to_string(c.audio.sfx_volume);
                    break;
                case 3:
                    label = "MUSIC";
                    value = c.audio.music_on ? "ON" : "OFF";
                    break;
                case 4:
                    label = "OUTPUT DEVICE";
                    value = c.audio.device_id.empty() ? "DEFAULT"
                                                      : c.audio.device_id;
                    break;
            }
            break;
        case 2:  // Controls (sensitivity only; bindings are WS9)
            if (row == 0) {
                label = "MOUSE SENSITIVITY";
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%.1f",
                              c.controls.mouse_sensitivity);
                value = buf;
            }
            break;
        case 3:  // Gameplay
            switch (row) {
                case 0:
                    label = "DIFFICULTY";
                    value = cfgname::difficulty(c.gameplay.difficulty);
                    pending = true;  // applies in WS7
                    break;
                case 1:
                    label = "FOG";
                    value = c.gameplay.fog ? "ON" : "OFF";
                    break;
            }
            break;
    }
    std::string t = label;
    t += std::string(20 - t.size(), ' ');
    t += " ";
    t += value;
    if (pending)
        t += "  (PENDING)";
    return t;
}

void build_menu(const SceneState& s, int w, int h, MenuDraw& out) {
    std::memset(&out, 0, sizeof(out));
    out.width = static_cast<std::uint32_t>(w);
    out.height = static_cast<std::uint32_t>(h);
    set_palette(out);
    switch (s.scene()) {
    case Scene::Title: {
        // Modern title menu (WS4, Decision 1): logo + three items.
        add_centered(out, w, static_cast<float>(h / 2 - 120), 3.0f,
                     "WOLFENSTEIN 3D", kMenuAccent);
        add_centered(out, w, static_cast<float>(h / 2 - 56), 1.0f,
                     "A MODERN REMAKE", 0u);
        const char* items[3] = {"NEW GAME", "OPTIONS", "QUIT"};
        std::array<MenuLayout, 4> lay;
        layout_v_items(w, h, 3, 40, lay);
        for (int i = 0; i < 3; ++i) {
            std::uint32_t flags = 0;
            if (i == s.selection())
                flags |= kMenuSelected;
            if (i == s.hover())
                flags |= kMenuHovered;
            add_box_text(out, lay[i].x, lay[i].y, lay[i].w, lay[i].h,
                         items[i], flags);
        }
        add_centered(out, w, static_cast<float>(h - 40), 1.0f,
                     "ARROWS = MOVE   ENTER = OK", 0u);
        break;
    }
    case Scene::NewGame: {
        add_centered(out, w,
                     static_cast<float>((h - 4 * 32 - 3 * 12) / 2 - 52),
                     2.0f, "SELECT EPISODE", kMenuAccent);
        std::array<MenuLayout, 4> lay;
        layout_episode_items(w, h, lay);
        for (int i = 0; i < SceneState::kEpisodeCount; ++i) {
            std::uint32_t flags = 0;
            if (i == s.selection())
                flags |= kMenuSelected;
            if (i == s.hover())
                flags |= kMenuHovered;
            add_box_text(out, lay[i].x, lay[i].y, lay[i].w, lay[i].h,
                         SceneState::kEpisodes[i].name, flags);
        }
        add_centered(out, w, static_cast<float>(h - 40), 1.0f,
                     "ENTER = START   ESC = BACK", 0u);
        break;
    }
    case Scene::LevelIntro: {
        char title[32];
        std::snprintf(title, sizeof(title), "LEVEL %d", s.level() + 1);
        add_centered(out, w, static_cast<float>(h / 2 - 56), 3.0f, title,
                     kMenuAccent);
        add_centered(out, w, static_cast<float>(h / 2 + 8), 1.0f,
                     SceneState::kEpisodes[s.episode()].name, 0u);
        add_centered(out, w, static_cast<float>(h / 2 + 44), 1.0f,
                     "PRESS ANY KEY TO BEGIN", 0u);
        break;
    }
    case Scene::Pause: {
        add_centered(out, w,
                     static_cast<float>((h - (3 * 32 + 2 * 12)) / 2 - 52),
                     2.0f, "PAUSED", kMenuAccent);
        const char* items[3] = {"RESUME", "OPTIONS", "QUIT TO TITLE"};
        std::array<MenuLayout, 4> lay;
        layout_v_items(w, h, 3, 28, lay);
        for (int i = 0; i < 3; ++i) {
            std::uint32_t flags = 0;
            if (i == s.selection())
                flags |= kMenuSelected;
            if (i == s.hover())
                flags |= kMenuHovered;
            add_box_text(out, lay[i].x, lay[i].y, lay[i].w, lay[i].h,
                         items[i], flags);
        }
        add_centered(out, w, static_cast<float>(h - 40), 1.0f,
                     "ESC = RESUME", 0u);
        break;
    }
    case Scene::Options: {
        const char* tab_names[4] = {"VIDEO", "AUDIO", "CONTROLS",
                                    "GAMEPLAY"};
        OptionsLayout lay;
        layout_options(w, h, lay);
        // Heading above the tab row.
        add_centered(out, w,
                     static_cast<float>(lay.tabs[0].y - 40), 2.0f,
                     "OPTIONS", kMenuAccent);
        for (int i = 0; i < 4; ++i) {
            std::uint32_t flags = 0;
            if (i == s.opt_tab())
                flags |= kMenuSelected;
            if (i == s.opt_tab_hover())
                flags |= kMenuHovered;
            add_box_text(out, lay.tabs[i].x, lay.tabs[i].y, lay.tabs[i].w,
                         lay.tabs[i].h, tab_names[i], flags);
        }
        const int rows = SceneState::kOptTabRows[s.opt_tab()];
        for (int i = 0; i < rows; ++i) {
            std::uint32_t flags = 0;
            if (i == s.opt_sel())
                flags |= kMenuSelected;
            if (i == s.opt_hover())
                flags |= kMenuHovered;
            add_box_text(out, lay.rows[i].x, lay.rows[i].y, lay.rows[i].w,
                         lay.rows[i].h,
                         option_row_text(s, s.opt_tab(), i).c_str(), flags);
        }
        add_centered(out, w, static_cast<float>(h - 40), 1.0f,
                     s.opt_tab() == 1 || s.opt_tab() == 3
                         ? "PENDING = SAVED, APPLIES LATER"
                         : "1-4 = TABS   ESC = CLOSE",
                     0u);
        break;
    }
    case Scene::Game:
        // No 2D elements: the app renders the 3D frame.
        break;
    }
}

}  // namespace w3d::app
