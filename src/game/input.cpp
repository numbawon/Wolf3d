// w3d game/input.cpp — reference keymap implementation (M5).
#include "game/input.hpp"

#include <cstring>

namespace w3d::game {

std::uint8_t movement_key_mask(const char* key) {
    if (std::strcmp(key, "ArrowUp") == 0 || std::strcmp(key, "up") == 0 ||
        std::strcmp(key, "w") == 0 || std::strcmp(key, "z") == 0)
        return kFwd;
    if (std::strcmp(key, "ArrowDown") == 0 || std::strcmp(key, "down") == 0 ||
        std::strcmp(key, "s") == 0)
        return kBwd;
    if (std::strcmp(key, "ArrowLeft") == 0 || std::strcmp(key, "left") == 0 ||
        std::strcmp(key, "a") == 0)
        return kLeft;
    if (std::strcmp(key, "ArrowRight") == 0 || std::strcmp(key, "right") == 0 ||
        std::strcmp(key, "e") == 0)
        return kRight;
    if (std::strcmp(key, "q") == 0)
        return kStrafeL;
    if (std::strcmp(key, "d") == 0)
        return kStrafeR;
    return 0;
}

int one_shot_event(const char* key) {
    if (std::strcmp(key, "f") == 0 || std::strcmp(key, "Control") == 0 ||
        std::strcmp(key, "left ctrl") == 0 ||
        std::strcmp(key, "right ctrl") == 0)
        return 1;
    if (std::strcmp(key, "space") == 0 || std::strcmp(key, " ") == 0)
        return 2;
    return 0;
}

void LiveInput::on_key(const char* key, bool down) {
    const std::uint8_t m = movement_key_mask(key);
    if (m) {
        if (down)
            held_ |= m;
        else
            held_ &= ~m;
        return;
    }
    if (down) {
        const int ev = one_shot_event(key);
        if (ev == 1)
            shoot_ = true;
        else if (ev == 2)
            activate_ = true;
    }
}

void LiveInput::on_mouse(double xrel) { dx_ += xrel; }

SimInput LiveInput::take() {
    SimInput in;
    in.forward = held_ & kFwd;
    in.backward = held_ & kBwd;
    in.turn_left = held_ & kLeft;
    in.turn_right = held_ & kRight;
    in.strafe_left = held_ & kStrafeL;
    in.strafe_right = held_ & kStrafeR;
    // Net mouse motion above half a pixel turns this tick.
    if (dx_ < -0.5)
        in.turn_left = true;
    else if (dx_ > 0.5)
        in.turn_right = true;
    dx_ = 0.0;
    in.shoot = shoot_;
    in.activate = activate_;
    shoot_ = false;
    activate_ = false;
    return in;
}

}  // namespace w3d::game
