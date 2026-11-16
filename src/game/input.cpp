// w3d game/input.cpp — reference keymap implementation (M5).
#include "game/input.hpp"

#include <string>

namespace w3d::game {

namespace {
// SDL key names are matched case-insensitively: SDL3 3.4.x reports
// "Up"/"W"/"Left Ctrl" (capitalized), SDL2/X11 report "up"/"ArrowUp".
std::string lower(const char* s) {
    std::string r(s);
    for (char& c : r)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return r;
}
}  // namespace

std::uint8_t movement_key_mask(const char* key) {
    const std::string k = lower(key);
    if (k == "arrowup" || k == "up" || k == "w" || k == "z")
        return kFwd;
    if (k == "arrowdown" || k == "down" || k == "s")
        return kBwd;
    if (k == "arrowleft" || k == "left" || k == "a")
        return kLeft;
    if (k == "arrowright" || k == "right" || k == "e")
        return kRight;
    if (k == "q")
        return kStrafeL;
    if (k == "d")
        return kStrafeR;
    return 0;
}

int one_shot_event(const char* key) {
    const std::string k = lower(key);
    if (k == "f" || k == "control" || k == "left ctrl" || k == "right ctrl")
        return 1;
    if (k == "space" || k == " ")
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

void LiveInput::set_sensitivity(double s) {
    sensitivity_ = s > 0.0 ? s : 1.0;
}

void LiveInput::on_mouse(double xrel) { dx_ += xrel * sensitivity_; }

void LiveInput::shoot() { shoot_ = true; }

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
