// Keyboard-only PC racing preset. Gamepad mappings are independent.
#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include "platform.h"

namespace keyboard_input {
enum class Context { Menu, Title, Driving };
// SLUS_209.10 producer globals, not elapsed-frame or image heuristics.
inline Context context(uint32_t title, uint32_t alternate_title, uint32_t game_loop, uint32_t modal) {
    if (title || alternate_title) return Context::Title;
    return game_loop && !modal ? Context::Driving : Context::Menu;
}
using Keys = std::array<bool, 256>; // Windows VK codes, shared with config.cpp
struct Binding { int key; uint16_t button; };
inline bool reserved(int k) {
    return k == 'W' || k == 'A' || k == 'S' || k == 'D' || k == 0x0d || k == 0x1b || k == 0x20 ||
           (k >= 0x25 && k <= 0x28);
}
class Mapper {
    Keys previous_{}, blocked_{};
    Context previous_context_ = Context::Menu;
    bool initialized_ = false, previous_racing_ = false;
public:
    HostPadState read(const Keys &physical, Context ctx, bool racing, const std::vector<Binding> &bindings) {
        if (initialized_ && ((racing && ctx != previous_context_) || racing != previous_racing_))
            for (size_t k = 0; k < physical.size(); ++k) blocked_[k] = physical[k] && previous_[k];
        Keys keys{};
        for (size_t k = 0; k < keys.size(); ++k) {
            if (!physical[k]) blocked_[k] = false;
            keys[k] = physical[k] && !blocked_[k];
        }
        previous_ = physical;
        previous_context_ = ctx;
        previous_racing_ = racing;
        initialized_ = true;
        HostPadState out;
        out.connected = true;
        for (const Binding &b : bindings)
            if (b.key > 0 && b.key < 256 && keys[b.key] && (!racing || !reserved(b.key))) out.buttons |= b.button;
        if (racing) {
            bool up = keys['W'] || keys[0x26], down = keys['S'] || keys[0x28];
            bool left = keys['A'] || keys[0x25], right = keys['D'] || keys[0x27];
            if (left != right) out.buttons |= left ? PAD_LEFT : PAD_RIGHT;
            if (ctx == Context::Driving) {
                if (up) out.buttons |= PAD_CROSS;
                if (down || keys[0x20]) out.buttons |= PAD_SQUARE;
                if (keys[0x1b]) out.buttons |= PAD_START;
            } else {
                if (up != down) out.buttons |= up ? PAD_UP : PAD_DOWN;
                if (keys[0x0d]) out.buttons |= ctx == Context::Title ? PAD_START : PAD_CROSS;
                if (keys[0x1b]) out.buttons |= PAD_CIRCLE;
            }
        }
        const bool left = out.buttons & PAD_LEFT, right = out.buttons & PAD_RIGHT;
        const bool up = out.buttons & PAD_UP, down = out.buttons & PAD_DOWN;
        if (left && right) out.buttons &= ~(PAD_LEFT | PAD_RIGHT);
        if (up && down) out.buttons &= ~(PAD_UP | PAD_DOWN);
        out.lx = left == right ? 128 : left ? 0 : 255;
        out.ly = up == down ? 128 : up ? 0 : 255;
        return out;
    }
};
} // namespace keyboard_input
