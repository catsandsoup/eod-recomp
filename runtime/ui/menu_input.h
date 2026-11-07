// menu_input.h - controller edges for the front end (header-only, unit-tested).
//
// Start+Select (pressed together, in either order) toggles the menu. (L3+R3 is
// left to the game: the recomp's live-cheat chord uses it.) Directions come from the d-pad or the left
// stick. Only digital buttons drive the menu: analog triggers never select
// anything (the Modern layout's RT/LT pressure is not read here).
#pragma once
#include <cstdint>

#include "../platform/platform.h"

namespace menu_input {

struct PadFrame {
    uint16_t held = 0, pressed = 0;
    int dx = 0, dy = 0;     // held direction (d-pad or stick)
    bool toggle = false;    // Start+Select / L3+R3 chord completed this frame
};

class PadEdges {
public:
    PadFrame update(const HostPadState &p) {
        PadFrame f;
        const uint16_t b = p.connected ? p.buttons : 0;
        f.held = b;
        f.pressed = b & ~prev_;
        auto chord = [&](uint16_t c) { return (b & c) == c && (f.pressed & c); };
        f.toggle = chord(PAD_START | PAD_SELECT);
        if (f.toggle) chord_held_ = true;
        if (!(b & (PAD_START | PAD_SELECT))) chord_held_ = false;
        const uint8_t lx = p.connected ? p.lx : 128, ly = p.connected ? p.ly : 128;
        f.dx = ((b & PAD_RIGHT) || lx > 192) - ((b & PAD_LEFT) || lx < 64);
        f.dy = ((b & PAD_DOWN) || ly > 192) - ((b & PAD_UP) || ly < 64);
        prev_ = b;
        return f;
    }
    // true while the buttons of a chord that toggled the menu are still held
    bool chord_held() const { return chord_held_; }
    void reset(uint16_t held) { prev_ = held; }

private:
    uint16_t prev_ = 0;
    bool chord_held_ = false;
};

}  // namespace menu_input
