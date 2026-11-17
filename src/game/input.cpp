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

void LiveInput::on_gamepad_button(int button, bool down) {
    if (button < 0 || button > 15)
        return;
    const std::uint16_t bit = static_cast<std::uint16_t>(1u << button);
    if (down) {
        if (gpad_btn_ & bit)
            return;  // already held: no re-fire
        gpad_btn_ |= bit;
        if (button == gamepad::kSouth)
            shoot_ = true;
        else if (button == gamepad::kEast)
            activate_ = true;
    } else {
        gpad_btn_ &= ~bit;
    }
}

void LiveInput::on_gamepad_axis(int axis, float value) {
    const float dz = gamepad::kDeadzone;
    if (axis == gamepad::kLeftX) {
        std::uint8_t bits = 0;
        if (value < -dz)
            bits = kLeft;
        else if (value > dz)
            bits = kRight;
        gpad_mov_ =
            static_cast<std::uint8_t>((gpad_mov_ & ~(kLeft | kRight)) | bits);
    } else if (axis == gamepad::kLeftY) {
        std::uint8_t bits = 0;
        if (value < -dz)
            bits = kFwd;  // pushed up
        else if (value > dz)
            bits = kBwd;  // pushed down
        gpad_mov_ =
            static_cast<std::uint8_t>((gpad_mov_ & ~(kFwd | kBwd)) | bits);
    }
}

void LiveInput::clear_gamepad() {
    gpad_btn_ = 0;
    gpad_mov_ = 0;
}

int LiveInput::gamepad_direction() const {
    // Stick takes precedence over the D-pad (finer control).
    if (gpad_mov_ & kFwd)
        return gamepad::kDirUp;
    if (gpad_mov_ & kBwd)
        return gamepad::kDirDown;
    if (gpad_mov_ & kLeft)
        return gamepad::kDirLeft;
    if (gpad_mov_ & kRight)
        return gamepad::kDirRight;
    if (gpad_btn_ & (1u << gamepad::kDpadUp))
        return gamepad::kDirUp;
    if (gpad_btn_ & (1u << gamepad::kDpadDown))
        return gamepad::kDirDown;
    if (gpad_btn_ & (1u << gamepad::kDpadLeft))
        return gamepad::kDirLeft;
    if (gpad_btn_ & (1u << gamepad::kDpadRight))
        return gamepad::kDirRight;
    return gamepad::kDirNone;
}

SimInput LiveInput::take() {
    SimInput in;
    // Keyboard and gamepad (WS6) share the movement mask.
    const std::uint8_t held =
        static_cast<std::uint8_t>(held_ | gpad_mov_);
    in.forward = held & kFwd;
    in.backward = held & kBwd;
    in.turn_left = held & kLeft;
    in.turn_right = held & kRight;
    in.strafe_left = held & kStrafeL;
    in.strafe_right = held & kStrafeR;
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
