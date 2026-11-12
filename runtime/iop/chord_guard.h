// chord_guard.h - keeps the menu chord (Start+Select) away from the game (header-only, unit-tested).
//
// The in-game menu opens when Start and Select are held together. Fingers rarely land on the same
// frame, so without this the first button reaches the game: Start opens the game's own pause menu
// under ours, Select recovers the car. A lone Start or Select press is held back for a few frames;
// if the other button joins, neither reaches the game until both are released. Otherwise the press
// is delivered (a tap shorter than the window is still delivered as a short press).
#pragma once
#include <cstdint>

namespace chord_guard {

constexpr uint16_t kStart = 0x0008, kSelect = 0x0001, kChord = kStart | kSelect;
constexpr int kWindowFrames = 5;   // 83 ms at 60 Hz
constexpr int kTapFrames = 2;

class Guard {
public:
    // raw: the buttons held this frame; returns the buttons the game should see.
    uint16_t filter(uint16_t raw) {
        const uint16_t pressed = raw & ~prev_ & kChord;
        prev_ = raw;
        if (swallow_) {
            if (!(raw & kChord)) swallow_ = false;
            return raw & ~kChord;
        }
        if ((raw & kChord) == kChord && (pending_ || pressed)) {
            swallow_ = true; pending_ = 0; tap_ = 0; tap_frames_ = 0;
            return raw & ~kChord;
        }
        if (pressed) { pending_ |= pressed; age_ = 0; }
        if (pending_) {
            const uint16_t released = pending_ & ~raw;
            if (released) { tap_ |= released; tap_frames_ = kTapFrames; pending_ &= ~released; }
            if (++age_ > kWindowFrames) pending_ = 0;
        }
        uint16_t out = raw & ~pending_;
        if (tap_frames_ > 0) { out |= tap_; if (--tap_frames_ == 0) tap_ = 0; }
        return out;
    }

private:
    uint16_t prev_ = 0, pending_ = 0, tap_ = 0;
    int age_ = 0, tap_frames_ = 0;
    bool swallow_ = false;
};

}  // namespace chord_guard
