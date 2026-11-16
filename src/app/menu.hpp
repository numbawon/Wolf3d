// w3d app/menu.hpp — full-res 2D menu model for the modern menus (WS2).
//
// Decision 1 of the phase plan: title/pause/options menus render
// full-resolution with a clean modern layout, so this is a plain 2D draw
// description (boxes + text runs) that the renderer's menu compute pass
// (menu.comp) executes at window resolution — independent of the 320x200
// internal game frame.
//
// MenuDraw is a POD: its exact byte layout is consumed by the GLSL shader
// as a std430 SSBO. Do not reorder members or change sizes without
// updating menu.comp (and the static_asserts).
//
// Text: embedded 8x16 bitmap font (font8x16.inc) — a compact public-domain-
//style bitmap so the menu pass has no font dependency (per the phase plan;
// the VGAGRAPH fonts are 320x200-scale pixel art and stay on the in-game
// HUD).
#pragma once

#include <array>
#include <cstdint>

namespace w3d::app {

class SceneState;  // scene.hpp

inline constexpr int kMaxMenuElements = 16;
inline constexpr int kMenuTextLen = 40;  // longest episode name is 38 chars

enum MenuFlags : std::uint32_t {
    kMenuBox = 1,        // draw the item box
    kMenuSelected = 2,   // selected: box + text use the selected palette
    kMenuHovered = 4,    // mouse hover: box brightened
    kMenuAccent = 8,     // text uses the accent (gold) color
};

// One menu element (80 bytes; std430 layout in menu.comp).
struct MenuElement {
    float x, y, w, h;    // item box, logical px (drawn only with kMenuBox)
    float tx, ty;        // text origin, logical px
    float scale;         // text scale 1..4 (glyph cell = 8*scale x 16*scale)
    float len;           // text length in characters
    std::uint32_t flags;
    std::uint32_t pad;
    std::uint8_t text[kMenuTextLen];  // ASCII, NUL-padded
};
static_assert(sizeof(MenuElement) == 80);

// Palette slot indexes inside MenuDraw::palette (RGBA bytes each).
enum MenuPalette : int {
    kPalBg = 0,
    kPalItem = 1,
    kPalSel = 2,
    kPalText = 3,
    kPalTextSel = 4,
    kPalAccent = 5,
};

// One full menu frame. sizeof == 1320 (std430 SSBO size in menu.comp).
struct MenuDraw {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t count;
    std::uint32_t pad[1];
    MenuElement els[kMaxMenuElements];
    std::uint8_t palette[24];  // 6 x RGBA, 0..255
};
static_assert(sizeof(MenuDraw) == 1320);

// Geometry of one menu item, logical px.
struct MenuLayout {
    int x, y, w, h;
};

// Episode-item layout for a window of (w, h): four 420x32 boxes, 12 px
// apart, centered. Shared by the render pass and the mouse hit-test so
// what the cursor sees is what the cursor picks.
void layout_episode_items(int w, int h,
                          std::array<MenuLayout, 4>& out);
// -1 if (mx, my) is outside every box.
[[nodiscard]] int hit_episode_item(const std::array<MenuLayout, 4>& lay,
                                   int mx, int my);

// Compose the menu frame for the current scene (count = 0 for Game — the
// app renders the 3D frame instead).
void build_menu(const SceneState& s, int w, int h, MenuDraw& out);

}  // namespace w3d::app
