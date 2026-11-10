#include "../../runtime/platform/editor_test_startup.h"
#include <cstdio>
#include <cstdlib>
#include <thread>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "failed: %s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

int main() {
    using namespace editor_test_startup;
    CHECK(!requested(nullptr, "track=Anoka"));
    CHECK(!requested("0", "track=Anoka"));
    CHECK(!requested("1", nullptr));
    CHECK(!requested("1", ""));
    CHECK(!requested("10", "track=Anoka"));
    CHECK(requested("1", "track=Anoka"));

    NativeFrameGate native;
    native.begin_draw(true); // generic session draw while loading
    native.end_draw();
    CHECK(!native.frame_published(true));
    native.gameplay_drawn(true, 0); // outside the race-loop draw scope
    CHECK(!native.frame_published(true));
    native.begin_draw(true);
    native.gameplay_drawn(false, 0); // no actual viewport
    native.end_draw();
    CHECK(!native.frame_published(true));
    native.begin_draw(true);
    native.gameplay_drawn(true, 255); // native loading transition still fully opaque
    native.end_draw();
    CHECK(!native.frame_published(true));
    native.begin_draw(true);
    native.gameplay_drawn(true, 1); // native transition still covers the race
    native.end_draw();
    CHECK(!native.frame_published(true));
    native.begin_draw(true);
    native.gameplay_drawn(true, 0); // native world and per-player HUD completed
    CHECK(!native.frame_published(true)); // draw has not completed
    native.end_draw();
    CHECK(!native.frame_published(false)); // unrelated menu/load frame wait
    CHECK(native.frame_published(true)); // the race-loop frame wait completed
    CHECK(!native.frame_published(true)); // one-time startup marker

    State ordinary(false);
    ordinary.race_submitted(64); ordinary.software_composed(65);
    ordinary.hardware_display_changed(65); ordinary.hardware_composed();
    ordinary.hardware_presented(); ordinary.software_presented();
    CHECK(!ordinary.ready());
    CHECK(!ordinary.hide_window(false));

    State hardware(true);
    CHECK(hardware.hide_window(false));
    CHECK(!hardware.hide_window(true)); // headless never becomes a hidden window
    hardware.hardware_display_changed(65); hardware.hardware_composed();
    hardware.hardware_presented(); // loading presentations cannot reveal
    CHECK(!hardware.ready());
    std::thread renderer([&] {
        hardware.race_submitted(64);
        CHECK(!hardware.ready()); // drawing alone cannot reveal
        hardware.hardware_presented(); // redraw of a loading image after marker
        CHECK(!hardware.ready());
        hardware.hardware_composed(); // overlay-only present, unchanged scanout
        hardware.hardware_presented();
        CHECK(!hardware.ready());
        hardware.hardware_display_changed(64); // register change without a new displayed draw
        hardware.hardware_composed();
        hardware.hardware_presented();
        CHECK(!hardware.ready());
        hardware.hardware_display_changed(65);
        hardware.hardware_presented(); // changed display is not yet composed
        CHECK(!hardware.ready());
        hardware.hardware_composed();
        CHECK(!hardware.ready());
        hardware.hardware_presented();
    });
    renderer.join();
    CHECK(hardware.ready());
    CHECK(!hardware.hide_window(false));

    State software(true);
    software.software_composed(65); software.software_presented();
    CHECK(!software.ready());
    software.race_submitted(64);
    software.software_presented(); // stale previously published loading image
    CHECK(!software.ready());
    software.software_composed(65);
    CHECK(!software.ready());
    software.software_presented();
    CHECK(software.ready());
    software.race_submitted(64); software.software_composed(65);
    CHECK(software.ready()); // one-way latch; later races do not hide the window
    State offscreen(true);
    CHECK(!offscreen.claim_preview(true));
    offscreen.race_submitted(64);
    CHECK(!offscreen.claim_preview(true));
    offscreen.hardware_display_changed(65);
    CHECK(!offscreen.claim_preview(false)); // visible path needs composition
    CHECK(!offscreen.claim_preview(true)); // no valid display circuit composed yet
    offscreen.hardware_composed();
    CHECK(offscreen.claim_preview(true)); // ordered offscreen scanout may be captured
    CHECK(!offscreen.claim_preview(true)); // bounded to one attempt, including failure
    CHECK(!offscreen.ready()); // snapshot is never evidence of window presentation
    CHECK(!ordinary.claim_preview(true));
    std::puts("editor test startup contract passed");
}
