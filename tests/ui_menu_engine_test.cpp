// tests/ui_menu_engine_test.cpp - Test suite for Modernized UI & Menu Engine (eod-recomp)
#include "ui_menu_engine.h"

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <vector>
#include <string>

static int g_failures = 0;
#define CHECK(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        g_failures++; \
    } \
} while (0)

using namespace ui_engine;

// Test mock ee memory functions for test environment
extern "C" {
uint32_t ee_r32(uint32_t) { return 0; }
void ee_w32(uint32_t, uint32_t) {}
uint8_t ee_r8(uint32_t) { return 0; }
void* ee_ptr(uint32_t) { return nullptr; }
}

void test_struct_offsets() {
    // Binary memory layout verification for PS2 RE compatibility
    CHECK(offsetof(PS2Widget, tag_ptr) == 0x00);
    CHECK(offsetof(PS2Widget, flags) == 0x04);
    CHECK(offsetof(PS2Widget, label_ptr) == 0x08);
    CHECK(offsetof(PS2Widget, result_id) == 0x0C);
    CHECK(offsetof(PS2Widget, initial_state) == 0x10);
    CHECK(offsetof(PS2Widget, min_input) == 0x24);
    CHECK(offsetof(PS2Widget, bar_value) == 0x28);
    CHECK(offsetof(PS2Widget, max_input) == 0x2C);
    CHECK(offsetof(PS2Widget, min_screen_pos) == 0x30);
    CHECK(offsetof(PS2Widget, max_screen_pos) == 0x34);
    CHECK(offsetof(PS2Widget, vtable_ptr) == 0x248); // 584 bytes

    CHECK(offsetof(PS2DialogWindow, widgets) == 0x30);
    CHECK(offsetof(PS2DialogWindow, update_callback) == 0x434); // 1076
    CHECK(offsetof(PS2DialogWindow, focused_widget) == 0x448);  // 1096
    CHECK(offsetof(PS2DialogWindow, active_widget) == 0x44C);   // 1100
    CHECK(offsetof(PS2DialogWindow, default_widget) == 0x450);  // 1104
    CHECK(offsetof(PS2DialogWindow, origin_x) == 0x454);        // 1108
    CHECK(offsetof(PS2DialogWindow, origin_y) == 0x458);        // 1112
    CHECK(offsetof(PS2DialogWindow, menu_count) == 0x4F4);
    CHECK(offsetof(PS2DialogWindow, selected_widget) == 0x4F8);
    CHECK(offsetof(PS2DialogWindow, menu_items) == 0x500);
}

void test_virtual_list_pagination() {
    VirtualListModel<int> model(5); // 5 visible slots
    std::vector<int> numbers;
    for (int i = 0; i < 17; ++i) numbers.push_back(i * 10);
    model.set_items(numbers);

    CHECK(model.total_count() == 17);
    CHECK(model.filtered_count() == 17);
    CHECK(model.total_pages() == 4); // ceil(17 / 5) = 4
    CHECK(model.current_page() == 0);
    CHECK(model.cursor() == 0);
    CHECK(model.selected_slot() == 0);

    // Initial slice (slots 0..4)
    auto slice = model.get_visible_slice();
    CHECK(slice.size() == 5);
    CHECK(*slice[0] == 0);
    CHECK(*slice[4] == 40);

    // Advance cursor across page boundaries
    model.move_cursor(5);
    CHECK(model.cursor() == 5);
    CHECK(model.current_page() == 1);
    CHECK(model.scroll_offset() == 5);
    CHECK(model.selected_slot() == 0);
    slice = model.get_visible_slice();
    CHECK(*slice[0] == 50);

    // Page flip
    model.page_flip(1); // Page 2 (items 10..14)
    CHECK(model.current_page() == 2);
    CHECK(model.cursor() == 10);
    slice = model.get_visible_slice();
    CHECK(*slice[0] == 100);

    // Jump to last page (items 15, 16, and empty slots)
    model.page_flip(1); // Page 3
    CHECK(model.current_page() == 3);
    CHECK(model.cursor() == 15);
    slice = model.get_visible_slice();
    CHECK(*slice[0] == 150);
    CHECK(*slice[1] == 160);
    CHECK(slice[2] == nullptr); // empty slot
    CHECK(slice[3] == nullptr);
    CHECK(slice[4] == nullptr);

    // Wrap around
    model.move_cursor(2); // From 15 + 2 = 17 -> wraps to 0
    CHECK(model.cursor() == 0);
    CHECK(model.current_page() == 0);

    // Reverse wrap
    model.move_cursor(-1); // Wraps to 16
    CHECK(model.cursor() == 16);
    CHECK(model.current_page() == 3);
}

void test_vehicle_filtering_and_sorting() {
    auto &dealer = DealerManager::instance();
    dealer.initialize_catalog();

    // Default: All classes, price ascending
    CHECK(dealer.list().total_count() >= 20);
    CHECK(dealer.current_class() == VehicleClass::All);
    CHECK(dealer.current_drivetrain() == Drivetrain::All);

    // Filter by Compact
    dealer.cycle_class_filter(1); // Next: Compact
    CHECK(dealer.current_class() == VehicleClass::Compact);
    CHECK(dealer.list().filtered_count() == 4); // Ocelot, Desio, Digit, Mini
    for (size_t i = 0; i < dealer.list().filtered_count(); ++i) {
        dealer.list().select_index(i);
        const auto *v = dealer.list().selected_item();
        CHECK(v != nullptr);
        CHECK(v->vclass == VehicleClass::Compact);
        CHECK(v->drivetrain == Drivetrain::FWD);
    }

    // Filter by Muscle
    dealer.cycle_class_filter(2); // Midsize, Muscle
    CHECK(dealer.current_class() == VehicleClass::Muscle);
    CHECK(dealer.list().filtered_count() == 5); // Raven, Stinger, Bullet, Thunder, Rocket
    for (size_t i = 0; i < dealer.list().filtered_count(); ++i) {
        dealer.list().select_index(i);
        const auto *v = dealer.list().selected_item();
        CHECK(v != nullptr);
        CHECK(v->vclass == VehicleClass::Muscle);
        CHECK(v->drivetrain == Drivetrain::RWD);
    }

    // Filter by Special
    dealer.cycle_class_filter(1); // Special
    CHECK(dealer.current_class() == VehicleClass::Special);
    CHECK(dealer.list().filtered_count() == 4); // Schoolbus, Funeral, Medic, Limo

    // Filter by Drivetrain AWD within Special
    dealer.cycle_drivetrain_filter(3); // All -> FWD -> RWD -> AWD
    CHECK(dealer.current_drivetrain() == Drivetrain::AWD);
    CHECK(dealer.list().filtered_count() == 2); // Medic, Limo

    // Reset filters
    dealer.cycle_class_filter(2); // Reset to All
    dealer.cycle_drivetrain_filter(1); // Reset to All
    CHECK(dealer.current_class() == VehicleClass::All);
    CHECK(dealer.current_drivetrain() == Drivetrain::All);

    // Sorting: Price Descending
    dealer.cycle_sort_mode(1); // PriceDesc
    dealer.list().select_index(0);
    const auto *first = dealer.list().selected_item();
    dealer.list().select_index(dealer.list().filtered_count() - 1);
    const auto *last = dealer.list().selected_item();
    CHECK(first->price >= last->price);

    // Status banner formatting
    char banner[128];
    dealer.format_status_banner(banner, sizeof(banner));
    CHECK(banner[0] == 'D' && banner[1] == 'E' && banner[2] == 'A');
}

void test_turbo_navigation() {
    TurboNavConfig cfg;
    cfg.initial_repeat_delay_ms = 180;
    cfg.repeat_interval_ms = 50;
    TurboNavigator nav(cfg);

    // Frame 0: Press Down
    int dir = nav.update_vertical(false, true, 16);
    CHECK(dir == 1); // Instant response on first frame

    // Frame 1..10 (160ms total elapsed): Holding Down
    for (int i = 0; i < 10; ++i) {
        dir = nav.update_vertical(false, true, 16);
        CHECK(dir == 0); // Within initial hold delay, no repeat yet
    }

    // Frame 12 (192ms total elapsed): Reached 180ms delay threshold
    dir = nav.update_vertical(false, true, 16);
    // Accumulator starts accumulating intervals

    // Simulate 50ms interval passing
    dir = nav.update_vertical(false, true, 50);
    CHECK(dir == 1); // Fast repeat fired!

    // Release button
    dir = nav.update_vertical(false, false, 16);
    CHECK(dir == 0);

    // Press Up
    dir = nav.update_vertical(true, false, 16);
    CHECK(dir == -1); // Instant response Up

    // Tab cycling with shoulder buttons
    CHECK(nav.update_tab_cycle(false, true) == 1);   // R1 -> Next Tab
    CHECK(nav.update_tab_cycle(true, false) == -1);  // L1 -> Prev Tab
    CHECK(nav.update_tab_cycle(false, false) == 0);

    // Page flip with triggers
    CHECK(nav.update_page_flip(false, true) == 1);   // R2 -> Next Page
    CHECK(nav.update_page_flip(true, false) == -1);  // L2 -> Prev Page
}

int main() {
    std::printf("[UI_MENU_ENGINE_TEST] Running menu engine unit tests...\n");
    test_struct_offsets();
    test_virtual_list_pagination();
    test_vehicle_filtering_and_sorting();
    test_turbo_navigation();

    if (g_failures == 0) {
        std::printf("[UI_MENU_ENGINE_TEST] ALL TESTS PASSED SUCCESSFULLY!\n");
        return 0;
    } else {
        std::fprintf(stderr, "[UI_MENU_ENGINE_TEST] %d TESTS FAILED!\n", g_failures);
        return 1;
    }
}
