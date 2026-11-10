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

    State ordinary(false);
    ordinary.race_submitted(); ordinary.software_composed();
    ordinary.hardware_presented(); ordinary.software_presented();
    CHECK(!ordinary.ready());
    CHECK(!ordinary.hide_window(false));

    State hardware(true);
    CHECK(hardware.hide_window(false));
    CHECK(!hardware.hide_window(true)); // headless never becomes a hidden window
    hardware.hardware_presented(); // loading presentations cannot reveal
    CHECK(!hardware.ready());
    std::thread renderer([&] {
        hardware.race_submitted();
        CHECK(!hardware.ready()); // drawing alone cannot reveal
        hardware.hardware_presented();
    });
    renderer.join();
    CHECK(hardware.ready());
    CHECK(!hardware.hide_window(false));

    State software(true);
    software.software_composed(); software.software_presented();
    CHECK(!software.ready());
    software.race_submitted();
    software.software_presented(); // stale previously published loading image
    CHECK(!software.ready());
    software.software_composed();
    CHECK(!software.ready());
    software.software_presented();
    CHECK(software.ready());
    software.race_submitted(); software.software_composed();
    CHECK(software.ready()); // one-way latch; later races do not hide the window
    std::puts("editor test startup contract passed");
}
