// Test pressure overrides through the same parser used by the game.
#include "platform/pad_script.h"
#include "iop/chord_guard.h"
#include <cstdio>
static uint64_t frame;
uint64_t kernel_vblank_count() { return frame; }
uint64_t kernel_menu_hold_frames() { return 0; }
static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
int main() {
    setenv("EOD_PAD_SCRIPT",
           "0:4000:128:128:0:13,10:0000:128:128:64:32,20:4000:128:128:255:0,"
           "25:4000:128:128:256:0,30:4000:128:128:-1:0,35:4000:128:128:12x:0,"
           "40:4000:128:128::0,45:4000:128:128:12:256,50:4000:128:128:12:-1,"
           "55:4000:128:128:12:1x,60:4000:128:128:12:,65:4000:128:128:12:2:3,"
           "70:0000:0:255:128,80:8000:1:254", 1);
    setenv("EOD_PAD_SCRIPT1", "0:4000,10:8000:32:224,20:0000", 1);
    HostPadState a, b;
    CHECK(pad_script_state(0, &a) && a.connected);
    CHECK(a.buttons == PAD_CROSS && a.pressure[6] == 0 && a.pressure[7] == 13);
    CHECK(a.lx == 128 && a.ly == 128);
    CHECK(pad_script_state(1, &b) && b.buttons == PAD_CROSS && b.pressure[6] == 255);
    frame = 10;
    CHECK(pad_script_state(0, &a) && a.buttons == 0 && a.pressure[6] == 64 && a.pressure[7] == 32);
    CHECK(pad_script_state(1, &b) && b.buttons == PAD_SQUARE && b.pressure[7] == 255);
    CHECK(b.lx == 32 && b.ly == 224);
    for (frame = 20; frame < 70; frame += 5) {
        CHECK(pad_script_state(0, &a));
        CHECK(a.buttons == PAD_CROSS && a.pressure[6] == 255 && a.pressure[7] == 0);
    }
    frame = 70;
    CHECK(pad_script_state(0, &a) && a.buttons == 0 && a.pressure[6] == 128 && a.pressure[7] == 0);
    CHECK(a.lx == 0 && a.ly == 255);
    frame = 80;
    CHECK(pad_script_state(0, &a) && a.buttons == PAD_SQUARE && a.pressure[6] == 0 && a.pressure[7] == 255);
    CHECK(a.lx == 1 && a.ly == 254);
    CHECK(pad_script_state(1, &b) && b.buttons == 0 && b.pressure[6] == 0 && b.pressure[7] == 0);
    CHECK(!pad_script_state(-1, &a) && !pad_script_state(2, &a));
    {   // Start+Select (the in-game menu chord) never reaches the game, in either order or on the same frame
        using chord_guard::Guard;
        const uint16_t ST = PAD_START, SE = PAD_SELECT, X = PAD_CROSS;
        Guard same;
        CHECK(same.filter(ST | SE | X) == X && same.filter(ST | SE) == 0 && same.filter(ST) == 0 && same.filter(0) == 0);
        CHECK(same.filter(ST) == 0);                      // a new lone press after the chord is held back again
        Guard start_first;
        CHECK(start_first.filter(ST) == 0 && start_first.filter(ST) == 0 && start_first.filter(ST | SE) == 0);
        CHECK(start_first.filter(SE) == 0 && start_first.filter(0) == 0);
        Guard select_first;
        CHECK(select_first.filter(SE | X) == X && select_first.filter(SE | ST | X) == X && select_first.filter(X) == X);
        Guard lone;                                       // a held Start arrives after the window
        int seen_at = -1;
        for (int i = 0; i < 12; ++i) if (lone.filter(ST) & ST) { seen_at = i; break; }
        CHECK(seen_at == chord_guard::kWindowFrames);
        Guard tap;                                        // a one-frame tap is still delivered, as a short press
        int tap_frames = 0;
        tap.filter(SE);
        for (int i = 0; i < 12; ++i) tap_frames += (tap.filter(0) & SE) != 0;
        CHECK(tap_frames == chord_guard::kTapFrames);
        Guard late;                                       // Start long held (delivered), then Select: both hidden
        for (int i = 0; i < 10; ++i) late.filter(ST);
        CHECK(late.filter(ST | SE) == 0 && late.filter(ST) == 0 && late.filter(0) == 0);
    }
    printf("%d scripted pressure checks passed, %d failed\n", checks - failures, failures);
    return failures ? 1 : 0;
}
