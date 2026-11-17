// w3d app/repeat.hpp — menu key-repeat for gamepad navigation (WS6).
//
// Pure logic (no SDL): the app feeds the current digitalized direction
// once per frame, and the class returns which direction should fire an
// InputEvent this frame. Standard menu repeat: the press fires
// immediately, then — after kDelay frames of continuous hold — one
// event every kPeriod frames. Direction codes: 0 = none, 1 = up,
// 2 = down, 3 = left, 4 = right (must match w3d::game::gamepad::kDir*).
#pragma once

namespace w3d::app {

// Direction codes (mirror w3d::game::gamepad::kDirNone..kDirRight).
constexpr int kDirNone = 0, kDirUp = 1, kDirDown = 2, kDirLeft = 3,
              kDirRight = 4;

class MenuRepeat {
  public:
    static constexpr int kDelay = 16;  // held frames before the first repeat
    static constexpr int kPeriod = 8;  // frames between repeats

    // Feed the current direction; returns the direction to act on this
    // frame (kDirNone = nothing).
    [[nodiscard]] int tick(int dir);
    // Call on device removal: drop the held state.
    void reset();

  private:
    int dir_ = kDirNone;
    int count_ = 0;
};

inline int MenuRepeat::tick(int dir) {
    if (dir != dir_) {
        dir_ = dir;
        count_ = kDelay;
        return dir;  // immediate (0 on release)
    }
    if (dir != kDirNone && --count_ <= 0) {
        count_ = kPeriod;
        return dir;
    }
    return kDirNone;
}

inline void MenuRepeat::reset() {
    dir_ = kDirNone;
    count_ = 0;
}

}  // namespace w3d::app
