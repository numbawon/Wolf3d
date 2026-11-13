// w3d game/input.hpp — live input state, shared by the app and the tests (M5).
//
// Reference keymap (pinned in Phase 3 against the reference engine):
// move/turn: ArrowUp or w/z (forward), ArrowDown or s (backward),
// ArrowLeft or a, ArrowRight or e; strafe: q (left), d (right).
// One-shot (key-down events only): f or Control = shoot, space = activate.
// Relative mouse: accumulated x motion turns the player (sign per tick).
#pragma once

#include <cstdint>

#include "game/sim.hpp"

namespace w3d::game {

// Bitmask of held movement keys (one bit per SimInput movement field).
inline constexpr std::uint8_t kFwd = 1, kBwd = 2, kLeft = 4, kRight = 8,
                              kStrafeL = 16, kStrafeR = 32;

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
    void on_mouse(double xrel);
    [[nodiscard]] SimInput take();

  private:
    std::uint8_t held_ = 0;
    bool shoot_ = false;
    bool activate_ = false;
    double dx_ = 0.0;
};

}  // namespace w3d::game
