// w3d game/input.hpp — live input state, shared by the app and the tests (M5).
//
// Reference keymap (pinned in Phase 3 against the reference engine):
// move/turn: ArrowUp or w/z (forward), ArrowDown or s (backward),
// ArrowLeft or a, ArrowRight or e; strafe: q (left), d (right).
// One-shot (key-down events only): f or Control = shoot, space = activate.
// Relative mouse: accumulated x motion turns the player (sign per tick);
// while captured, mouse button 1 fires (original behavior).
// Key names are matched case-insensitively: SDL3 3.4.x reports them
// capitalized ("Up", "W", "Left Ctrl"); SDL2/X11 report "up"/"ArrowUp".
#pragma once

#include <cstdint>

#include "game/sim.hpp"

namespace w3d::game {

// Bitmask of held movement keys (one bit per SimInput movement field).
inline constexpr std::uint8_t kFwd = 1, kBwd = 2, kLeft = 4, kRight = 8,
                              kStrafeL = 16, kStrafeR = 32;

// WS6: gamepad (standard mapping, digitalized). The app translates
// SDL3 gamepad events into the calls below; the button/axis codes
// mirror the SDL3 SDL_GamepadButton / SDL_GamepadAxis enum values
// (stable ABI), so the wiring is one-for-one.
namespace gamepad {
    constexpr int kSouth = 0;    // A: shoot
    constexpr int kEast = 1;     // B: activate
    constexpr int kStart = 6;    // Esc equivalent (app-level)
    constexpr int kDpadUp = 11, kDpadDown = 12,
                 kDpadLeft = 13, kDpadRight = 14;
    constexpr int kLeftX = 0, kLeftY = 1;
    constexpr float kDeadzone = 0.25f;
    // Menu direction codes (also used by app/repeat.hpp).
    constexpr int kDirNone = 0, kDirUp = 1, kDirDown = 2,
                 kDirLeft = 3, kDirRight = 4;
}

// SDL3 key name -> held movement mask (0 = not a movement key).
[[nodiscard]] std::uint8_t movement_key_mask(const char* key);
// SDL3 key name on a key-DOWN event -> one-shot: 0 = none, 1 = shoot,
// 2 = activate.
[[nodiscard]] int one_shot_event(const char* key);

// Live input state: held keys + pending one-shots + accumulated relative
// mouse x (pixels). take() consumes the one-shots and this tick's mouse
// turn; call it once per simulation tick.
class LiveInput {
  public:
    void on_key(const char* key, bool down);
    // Multiplier on relative mouse x (WS4: the controls.mouse_sensitivity
    // setting, live-applied; 1.0 = the original per-pixel behavior).
    void set_sensitivity(double s);
    [[nodiscard]] double sensitivity() const { return sensitivity_; }
    void on_mouse(double xrel);
    // Mouse button 1 pressed while the mouse is captured (original: fire).
    void shoot();
    // WS6: gamepad (standard mapping, digitalized). on_gamepad_button
    // has EDGE semantics: the one-shots (A -> shoot, B -> activate)
    // fire on the down edge only; holding never re-fires (matches the
    // keyboard one-shots). on_gamepad_axis digitalizes at the deadzone
    // (|v| > 0.25): left stick X = turn, left stick Y = movement (push
    // up = forward); re-centering clears the bits. clear_gamepad drops
    // all gamepad state (device removal).
    void on_gamepad_button(int button, bool down);
    void on_gamepad_axis(int axis, float value);
    void clear_gamepad();
    // Current digitalized menu direction (stick, else D-pad):
    // gamepad::kDirNone/Up/Down/Left/Right.
    [[nodiscard]] int gamepad_direction() const;
    [[nodiscard]] SimInput take();

  private:
    std::uint8_t held_ = 0;
    bool shoot_ = false;
    bool activate_ = false;
    double dx_ = 0.0;
    double sensitivity_ = 1.0;
    // WS6: gamepad state (held-button bitmask + digitalized stick bits).
    std::uint16_t gpad_btn_ = 0;
    std::uint8_t gpad_mov_ = 0;
};

}  // namespace w3d::game
