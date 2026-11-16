// w3d app/menu.cpp — full-res menu composition (WS2).
#include "app/menu.hpp"

#include <algorithm>
#include <cstring>

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

void layout_episode_items(int w, int h, std::array<MenuLayout, 4>& out) {
    constexpr int kBoxW = 420;
    constexpr int kBoxH = 32;
    constexpr int kGap = 12;
    const int total = 4 * kBoxH + 3 * kGap;
    const int top = (h - total) / 2 + 28;  // below the title
    const int x = (w - kBoxW) / 2;
    for (int i = 0; i < 4; ++i)
        out[i] = {x, top + i * (kBoxH + kGap), kBoxW, kBoxH};
}

int hit_episode_item(const std::array<MenuLayout, 4>& lay, int mx, int my) {
    for (int i = 0; i < 4; ++i) {
        if (mx >= lay[i].x && mx < lay[i].x + lay[i].w && my >= lay[i].y &&
            my < lay[i].y + lay[i].h)
            return i;
    }
    return -1;
}

void build_menu(const SceneState& s, int w, int h, MenuDraw& out) {
    std::memset(&out, 0, sizeof(out));
    out.width = static_cast<std::uint32_t>(w);
    out.height = static_cast<std::uint32_t>(h);
    set_palette(out);
    switch (s.scene()) {
    case Scene::Title: {
        const char* title = "WOLFENSTEIN 3D";
        add_centered(out, w, static_cast<float>(h / 2 - 64), 3.0f, title,
                     kMenuAccent);
        add_centered(out, w, static_cast<float>(h / 2 - 8), 1.0f,
                     "A MODERN REMAKE", 0u);
        add_centered(out, w, static_cast<float>(h / 2 + 32), 1.0f,
                     "PRESS ANY KEY", 0u);
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
    case Scene::Game:
        // No 2D elements: the app renders the 3D frame.
        break;
    }
}

}  // namespace w3d::app
