// ui_menu_engine.h - Modernized UI & Menu Engine for Test Drive: Eve of Destruction (eod-recomp)
// Reverse engineered from SLUS_209.10:
//   0x00270C10 (MenuRun / UIDoMappedDialog)
//   0x003698E8 (HomebaseBuildMenu)
//   0x002A1298 (MainMenuUpdate)
//   0x002C0BB8 (ArcadeSingleOrMultiPromptScreen)
//   0x002CB228 (dd_chsr.lyt / Vehicle Select)
//   0x002C7AC4 (track1/23/4.lyt / Track Select)
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>

namespace ui_engine {

// =============================================================================
// 1. DECOMPILED PS2 MENU & WIDGET MEMORY STRUCTURES
// =============================================================================

// Binary layout of a native PS2 UI Widget in SLUS_209.10
#pragma pack(push, 1)
struct PS2Widget {
    uint32_t tag_ptr;         // +0x00: Pointer to widget identifier name (e.g., "opt1", "speed_bar")
    uint32_t flags;           // +0x04: State flags (0=normal, 1=disabled, 2=hidden, 3=interactive/bar, 4=focused)
    uint32_t label_ptr;       // +0x08: Display string pointer / localized text buffer
    int32_t  result_id;       // +0x0C: Command / action ID returned to MenuRun on confirmation
    uint32_t initial_state;   // +0x10: Toggle state, checkbox value, or sub-mode
    uint32_t pad14[4];        // +0x14..0x20: Screen bounds, alignment, or padding
    float    min_input;       // +0x24: Min input bound for progress/slider
    float    bar_value;       // +0x28: Bar/progress/slider current fill value (Speed, Handling, Durability)
    float    max_input;       // +0x2C: Max input bound
    float    min_screen_pos;  // +0x30: Screen render fill start
    float    max_screen_pos;  // +0x34: Screen render fill end
    uint8_t  pad38[0x248 - 0x38]; // +0x38..0x247: Internal widget state
    uint32_t vtable_ptr;      // +0x248 (offset 584): Pointer to C++ virtual function table
};

// Offsets in PS2Widget::vtable:
//   vtable[17] (+68): on_release(Widget*)
//   vtable[18] (+72): on_drag(Widget*, float dx, float dy)
//   vtable[19] (+76): on_confirm(Widget*) -> writes exit_flag=1, result=result_id
//   vtable[20] (+80): on_cancel(Widget*)  -> writes exit_flag=1, result=-1
//   vtable[21] (+84): on_focus(Widget*)
//   vtable[22] (+88): on_unfocus(Widget*)
//   vtable[25] (+100): on_navigate(Widget*, int dir)

struct PS2MenuItem {
    uint32_t widget_ptr;      // +0x00: Pointer to PS2Widget
    int32_t  action_id;       // +0x04: Command code
};

struct PS2DialogWindow {
    uint32_t window_id;       // +0x00: Layout resource handle
    uint8_t  pad04[0x30 - 0x04];
    uint32_t widgets[128];    // +0x30: Child widget pointers
    uint8_t  pad230[0x434 - 0x230];
    uint32_t update_callback; // +0x434 (1076): Per-frame update callback
    uint8_t  pad438[0x448 - 0x438];
    uint32_t focused_widget;  // +0x448 (1096): Currently focused widget
    uint32_t active_widget;   // +0x44C (1100): Currently active/dragged widget
    uint32_t default_widget;  // +0x450 (1104): Default initial widget
    int32_t  origin_x;        // +0x454 (1108): Dialog X screen offset
    int32_t  origin_y;        // +0x458 (1112): Dialog Y screen offset
    uint8_t  pad45C[0x4F4 - 0x45C];
    int32_t  menu_count;      // +0x4F4: Total selectable menu entries
    uint32_t selected_widget; // +0x4F8: Current selection widget pointer
    uint8_t  pad4FC[4];
    PS2MenuItem menu_items[32]; // +0x500: Array of 8-byte menu items
};

// MenuRun runtime state (fixed memory offsets in SLUS_209.10)
constexpr uint32_t kAddr_DialogDone        = 0x004A1AC0; // Non-zero terminates MenuRun
constexpr uint32_t kAddr_DialogResult      = 0x004A1AC4; // Selected item ID returned by MenuRun
constexpr uint32_t kAddr_DialogNesting     = 0x004A1AC8; // Dialog nesting counter
constexpr uint32_t kAddr_ActiveDialog      = 0x004A1AD4; // Pointer to active PS2DialogWindow
constexpr uint32_t kAddr_TransitionInDur   = 0x004A1B30; // Float seconds for transition in (native: 0.65f)
constexpr uint32_t kAddr_TransitionProgress= 0x004A1B38; // Current transition fraction (0.0f .. 1.0f)
constexpr uint32_t kAddr_TransitionSpeed   = 0x004A1B3C; // 1.0f / TransitionDuration
#pragma pack(pop)

// =============================================================================
// 2. VEHICLE & TRACK CLASSIFICATION
// =============================================================================

enum class VehicleClass : uint8_t {
    All = 0,
    Compact,   // Class 0: e.g., Ocelot, Desio, Digit, Roller, Mini
    Midsize,   // Class 1: e.g., Outlaw, Phantom, Cupcar, Condor, Taxi
    Muscle,    // Class 2: e.g., Raven, Stinger, Bullet, Thunder, Rocket
    Special,   // Class 3: e.g., Schoolbus, Hearse, Ambulance, Limo
    NASCAR,    // Class 4: e.g., Stock Cup cars (00..99)
    Count
};

enum class Drivetrain : uint8_t {
    All = 0,
    FWD,       // Front-Wheel Drive (typically Compacts)
    RWD,       // Rear-Wheel Drive (typically Muscle & Midsize)
    AWD,       // All-Wheel Drive (e.g. Special heavy vehicles, custom builds)
    Count
};

enum class TrackCategory : uint8_t {
    All = 0,
    Oval,
    Figure8,
    Derby,
    JumpCross,
    PointToPoint,
    Count
};

enum class SortMode : uint8_t {
    Default = 0,
    PriceAsc,
    PriceDesc,
    NameAsc,
    TopSpeed,
    Durability,
    Count
};

const char* vehicle_class_name(VehicleClass c);
const char* drivetrain_name(Drivetrain d);
const char* track_category_name(TrackCategory c);

struct VehicleItem {
    uint32_t     chassis_id;
    uint32_t     vehicle_id;
    std::string  name;
    uint32_t     price;
    VehicleClass vclass;
    Drivetrain   drivetrain;
    int          speed_stat;      // 0..100
    int          handling_stat;   // 0..100
    int          durability_stat; // 0..100
};

struct TrackItem {
    uint32_t      track_id;
    std::string   name;
    std::string   layout_file;
    TrackCategory category;
    int           max_players;
};

// =============================================================================
// 3. VIRTUAL LIST MODEL (PAGINATION, SMOOTH & INFINITE SCROLLING)
// =============================================================================

template<typename T>
class VirtualListModel {
public:
    explicit VirtualListModel(size_t visible_slots = 5)
        : m_visible_slots(visible_slots ? visible_slots : 1) {}

    void set_items(std::vector<T> items) {
        m_items = std::move(items);
        rebuild_filtered();
    }

    const std::vector<T>& all_items() const { return m_items; }

    void set_filter(std::function<bool(const T&)> predicate) {
        m_filter_predicate = std::move(predicate);
        rebuild_filtered();
    }

    void clear_filter() {
        m_filter_predicate = nullptr;
        rebuild_filtered();
    }

    void set_sorter(std::function<bool(const T&, const T&)> comparator) {
        m_sort_comparator = std::move(comparator);
        rebuild_filtered();
    }

    void clear_sorter() {
        m_sort_comparator = nullptr;
        rebuild_filtered();
    }

    // Navigation
    void move_cursor(int delta, bool wrap = true) {
        if (m_filtered_indices.empty()) return;
        const int count = static_cast<int>(m_filtered_indices.size());
        if (wrap) {
            m_cursor = ((m_cursor + delta) % count + count) % count;
        } else {
            m_cursor = std::clamp(m_cursor + delta, 0, count - 1);
        }
        adjust_scroll();
    }

    void page_flip(int page_delta, bool wrap = true) {
        if (m_filtered_indices.empty()) return;
        const int step = static_cast<int>(m_visible_slots);
        move_cursor(page_delta * step, wrap);
    }

    void select_index(size_t index) {
        if (index < m_filtered_indices.size()) {
            m_cursor = static_cast<int>(index);
            adjust_scroll();
        }
    }

    // Status queries
    size_t filtered_count() const { return m_filtered_indices.size(); }
    size_t total_count() const { return m_items.size(); }
    size_t visible_slots() const { return m_visible_slots; }
    int cursor() const { return m_cursor; }
    size_t scroll_offset() const { return m_scroll_offset; }
    size_t current_page() const { return m_cursor >= 0 ? static_cast<size_t>(m_cursor) / m_visible_slots : 0; }
    size_t total_pages() const {
        return m_filtered_indices.empty() ? 1 : (m_filtered_indices.size() + m_visible_slots - 1) / m_visible_slots;
    }

    const T* selected_item() const {
        if (m_cursor >= 0 && static_cast<size_t>(m_cursor) < m_filtered_indices.size()) {
            return &m_items[m_filtered_indices[m_cursor]];
        }
        return nullptr;
    }

    // Returns the active slice of items mapped to the visible UI slots (e.g. 5 dealer rows)
    std::vector<const T*> get_visible_slice() const {
        std::vector<const T*> slice;
        slice.reserve(m_visible_slots);
        for (size_t i = 0; i < m_visible_slots; ++i) {
            const size_t idx = m_scroll_offset + i;
            if (idx < m_filtered_indices.size()) {
                slice.push_back(&m_items[m_filtered_indices[idx]]);
            } else {
                slice.push_back(nullptr); // empty slot
            }
        }
        return slice;
    }

    // Relative slot index (0 .. visible_slots - 1) of the currently selected item
    int selected_slot() const {
        if (m_cursor >= 0 && static_cast<size_t>(m_cursor) >= m_scroll_offset) {
            const size_t slot = static_cast<size_t>(m_cursor) - m_scroll_offset;
            if (slot < m_visible_slots) return static_cast<int>(slot);
        }
        return -1;
    }

private:
    void rebuild_filtered() {
        m_filtered_indices.clear();
        m_filtered_indices.reserve(m_items.size());
        for (size_t i = 0; i < m_items.size(); ++i) {
            if (!m_filter_predicate || m_filter_predicate(m_items[i])) {
                m_filtered_indices.push_back(i);
            }
        }
        if (m_sort_comparator) {
            std::stable_sort(m_filtered_indices.begin(), m_filtered_indices.end(),
                [this](size_t a, size_t b) {
                    return m_sort_comparator(m_items[a], m_items[b]);
                });
        }
        if (m_filtered_indices.empty()) {
            m_cursor = -1;
            m_scroll_offset = 0;
        } else {
            m_cursor = std::clamp(m_cursor, 0, static_cast<int>(m_filtered_indices.size() - 1));
            adjust_scroll();
        }
    }

    void adjust_scroll() {
        if (m_cursor < 0 || m_filtered_indices.empty()) {
            m_scroll_offset = 0;
            return;
        }
        const size_t cur = static_cast<size_t>(m_cursor);
        // Page-aligned windowing (snapping to page boundaries)
        const size_t page = cur / m_visible_slots;
        m_scroll_offset = page * m_visible_slots;
    }

    std::vector<T> m_items;
    std::vector<size_t> m_filtered_indices;
    std::function<bool(const T&)> m_filter_predicate;
    std::function<bool(const T&, const T&)> m_sort_comparator;
    size_t m_visible_slots = 5;
    int m_cursor = 0;
    size_t m_scroll_offset = 0;
};

// =============================================================================
// 4. TURBO NAVIGATION & RESPONSIVE INPUT
// =============================================================================

struct TurboNavConfig {
    uint32_t initial_repeat_delay_ms = 180; // Delay before holding starts repeat (PS2 was 400ms)
    uint32_t repeat_interval_ms      = 50;  // Repeat rate: 20 ticks/sec (PS2 was 300ms)
    bool     fast_transitions        = true;// Accelerate sluggish 0.65s PS2 transitions
    float    transition_duration_s   = 0.05f; // Snappy sub-frame fade (50ms)
};

class TurboNavigator {
public:
    explicit TurboNavigator(TurboNavConfig cfg = TurboNavConfig{}) : m_cfg(cfg) {}

    // Processes pad state and determines navigation impulse
    // returns -1 for Up, +1 for Down, 0 for None
    int update_vertical(bool up_held, bool down_held, uint32_t delta_ms);

    // Filter tab cycling: -1 for Prev Tab, +1 for Next Tab
    int update_tab_cycle(bool l1_pressed, bool r1_pressed);

    // Fast page flip: -1 for Prev Page, +1 for Next Page
    int update_page_flip(bool l2_pressed, bool r2_pressed);

    const TurboNavConfig& config() const { return m_cfg; }
    void set_config(const TurboNavConfig &cfg) { m_cfg = cfg; }

private:
    TurboNavConfig m_cfg;
    int m_last_dir = 0;
    uint32_t m_hold_time_ms = 0;
    uint32_t m_repeat_accumulator_ms = 0;
};

// =============================================================================
// 5. HLE HOOKS & SYSTEM INTEGRATION
// =============================================================================

// Modernized Career Dealer catalog with class & drivetrain filtering
class DealerManager {
public:
    static DealerManager& instance();

    void initialize_catalog();
    VirtualListModel<VehicleItem>& list() { return m_list; }

    void cycle_class_filter(int dir);
    void cycle_drivetrain_filter(int dir);
    void cycle_sort_mode(int dir);

    VehicleClass current_class() const { return m_current_class; }
    Drivetrain current_drivetrain() const { return m_current_drivetrain; }
    SortMode current_sort() const { return m_current_sort; }

    // Synchronizes the native PS2 stack frame at buyscreen_sp with the visible slice
    void sync_to_native_stack(uint32_t stack_sp);

    // Formats a HUD banner showing: "DEALER: [COMPACT] (Page 1/3) - R1: Class | R2: Drivetrain"
    void format_status_banner(char *buf, size_t buf_sz) const;

private:
    DealerManager();
    void apply_filters();

    VirtualListModel<VehicleItem> m_list{5};
    VehicleClass m_current_class = VehicleClass::All;
    Drivetrain m_current_drivetrain = Drivetrain::All;
    SortMode m_current_sort = SortMode::PriceAsc;
};

// Modernized Arcade Vehicle Selector
class ArcadeVehicleManager {
public:
    static ArcadeVehicleManager& instance();

    void initialize_catalog();
    VirtualListModel<VehicleItem>& list() { return m_list; }

    void cycle_class_filter(int dir);
    void cycle_drivetrain_filter(int dir);

    // Synchronizes the 4 slots of dd_chsr.lyt
    void sync_to_native_slots(uint32_t candidate_array_addr, uint32_t count);

private:
    ArcadeVehicleManager();
    void apply_filters();

    VirtualListModel<VehicleItem> m_list{4}; // 4 slots in dd_chsr.lyt
    VehicleClass m_current_class = VehicleClass::All;
    Drivetrain m_current_drivetrain = Drivetrain::All;
};

// Modernized Arcade Track Selector
class TrackSelectManager {
public:
    static TrackSelectManager& instance();

    void initialize_catalog();
    VirtualListModel<TrackItem>& list() { return m_list; }

    void cycle_category_filter(int dir);

private:
    TrackSelectManager();
    void apply_filters();

    VirtualListModel<TrackItem> m_list{8};
    TrackCategory m_current_category = TrackCategory::All;
};

// Engine HLE Wrapper Hooks
void hle_menu_run_prologue();
void hle_homebase_build_menu_expanded(uint32_t out_buf_ptr);
void hle_autoshop_buy_menu_vblank(uint16_t pressed, uint32_t stack_sp);

} // namespace ui_engine
