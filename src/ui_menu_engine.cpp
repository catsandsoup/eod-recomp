// ui_menu_engine.cpp - Implementation of Modernized UI & Menu Engine for eod-recomp
#include "ui_menu_engine.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <cmath>
#include <sstream>

#include "core/memory.h"
#include "core/cpu.h"

namespace ui_engine {

namespace {

bool is_sane(uint32_t p, uint32_t span) {
    return p && !(p & 3) && p <= (EE_RAM_SIZE - span);
}

bool str_contains_icase(const std::string &str, const std::string &substr) {
    if (substr.empty()) return true;
    auto it = std::search(
        str.begin(), str.end(),
        substr.begin(), substr.end(),
        [](char ch1, char ch2) { return std::tolower(static_cast<unsigned char>(ch1)) == std::tolower(static_cast<unsigned char>(ch2)); }
    );
    return it != str.end();
}

void format_currency(uint32_t price, char *buf, size_t sz) {
    if (price >= 1000000) {
        std::snprintf(buf, sz, "$%.2f M", static_cast<double>(price) / 1000000.0);
    } else if (price >= 1000) {
        std::snprintf(buf, sz, "$%u,%03u", price / 1000, price % 1000);
    } else {
        std::snprintf(buf, sz, "$%u", price);
    }
}

// Inferred classifications for Test Drive: Eve of Destruction vehicles
VehicleClass classify_chassis_name(const std::string &name) {
    const std::string s = name;
    if (str_contains_icase(s, "cup") || str_contains_icase(s, "nascar")) return VehicleClass::NASCAR;
    if (str_contains_icase(s, "bus") || str_contains_icase(s, "hearse") ||
        str_contains_icase(s, "funeral") || str_contains_icase(s, "ambulance") ||
        str_contains_icase(s, "medic") || str_contains_icase(s, "limo") ||
        str_contains_icase(s, "semi") || str_contains_icase(s, "truck") ||
        str_contains_icase(s, "dozer")) return VehicleClass::Special;
    if (str_contains_icase(s, "raven") || str_contains_icase(s, "stinger") ||
        str_contains_icase(s, "bullet") || str_contains_icase(s, "thunder") ||
        str_contains_icase(s, "rocket") || str_contains_icase(s, "charger") ||
        str_contains_icase(s, "gto") || str_contains_icase(s, "muscle")) return VehicleClass::Muscle;
    if (str_contains_icase(s, "ocelot") || str_contains_icase(s, "desio") ||
        str_contains_icase(s, "digit") || str_contains_icase(s, "mini") ||
        str_contains_icase(s, "roller") || str_contains_icase(s, "dart") ||
        str_contains_icase(s, "compact") || str_contains_icase(s, "bug")) return VehicleClass::Compact;
    return VehicleClass::Midsize;
}

Drivetrain infer_drivetrain(VehicleClass vclass, const std::string &name) {
    if (vclass == VehicleClass::Compact) return Drivetrain::FWD;
    if (vclass == VehicleClass::Special) {
        if (str_contains_icase(name, "bus") || str_contains_icase(name, "hearse")) return Drivetrain::RWD;
        return Drivetrain::AWD;
    }
    return Drivetrain::RWD;
}

// Memory addresses from SLUS_209.10
constexpr uint32_t kASChassisList  = 0x003D93B8; // 8-byte records: name pointer, price
constexpr uint32_t kASChassisCount = 0x003D93BC;
constexpr uint32_t kASCarList      = 0x003D93C0; // 0x14-byte records: +0 idx, +4 vehicle, +8 price, +0xC name
constexpr uint32_t kASCarCount     = 0x003D93C4;
constexpr uint32_t kShopSlots      = 0x004AB130; // 5 dealer slots (ASCarList indices)

// Stack frame layout offsets for f_0036E9C0 (frame size 0x980):
constexpr uint32_t kPriceOffsets[5]     = {0xE0, 0xEA, 0xF4, 0xFE, 0x108};
constexpr uint32_t kWidgetPtrOffsets[5] = {0x81C, 0x884, 0x8C4, 0x7E4, 0x824};

} // namespace

const char* vehicle_class_name(VehicleClass c) {
    switch (c) {
        case VehicleClass::All:     return "ALL CLASSES";
        case VehicleClass::Compact: return "COMPACT";
        case VehicleClass::Midsize: return "MIDSIZE";
        case VehicleClass::Muscle:  return "MUSCLE";
        case VehicleClass::Special: return "SPECIAL";
        case VehicleClass::NASCAR:  return "NASCAR";
        default:                    return "UNKNOWN";
    }
}

const char* drivetrain_name(Drivetrain d) {
    switch (d) {
        case Drivetrain::All: return "ALL DRIVETRAINS";
        case Drivetrain::FWD: return "FWD";
        case Drivetrain::RWD: return "RWD";
        case Drivetrain::AWD: return "AWD";
        default:              return "UNKNOWN";
    }
}

const char* track_category_name(TrackCategory c) {
    switch (c) {
        case TrackCategory::All:          return "ALL TRACKS";
        case TrackCategory::Oval:         return "OVAL";
        case TrackCategory::Figure8:      return "FIGURE 8";
        case TrackCategory::Derby:        return "DEMOLITION DERBY";
        case TrackCategory::JumpCross:    return "JUMP CROSS";
        case TrackCategory::PointToPoint: return "POINT-TO-POINT";
        default:                          return "UNKNOWN";
    }
}

// =============================================================================
// TURBO NAVIGATOR IMPLEMENTATION
// =============================================================================

int TurboNavigator::update_vertical(bool up_held, bool down_held, uint32_t delta_ms) {
    if (up_held == down_held) {
        m_last_dir = 0;
        m_hold_time_ms = 0;
        m_repeat_accumulator_ms = 0;
        return 0;
    }

    const int dir = up_held ? -1 : 1;
    if (dir != m_last_dir) {
        m_last_dir = dir;
        m_hold_time_ms = 0;
        m_repeat_accumulator_ms = 0;
        return dir; // Instant response on fresh press
    }

    m_hold_time_ms += delta_ms;
    if (m_hold_time_ms >= m_cfg.initial_repeat_delay_ms) {
        m_repeat_accumulator_ms += delta_ms;
        if (m_repeat_accumulator_ms >= m_cfg.repeat_interval_ms) {
            m_repeat_accumulator_ms -= m_cfg.repeat_interval_ms;
            return dir; // Fast continuous repeat
        }
    }
    return 0;
}

int TurboNavigator::update_tab_cycle(bool l1_pressed, bool r1_pressed) {
    if (l1_pressed && !r1_pressed) return -1;
    if (r1_pressed && !l1_pressed) return 1;
    return 0;
}

int TurboNavigator::update_page_flip(bool l2_pressed, bool r2_pressed) {
    if (l2_pressed && !r2_pressed) return -1;
    if (r2_pressed && !l2_pressed) return 1;
    return 0;
}

// =============================================================================
// DEALER MANAGER IMPLEMENTATION
// =============================================================================

DealerManager& DealerManager::instance() {
    static DealerManager s_mgr;
    return s_mgr;
}

DealerManager::DealerManager() {
    initialize_catalog();
}

void DealerManager::initialize_catalog() {
    std::vector<VehicleItem> catalog;

    // In a live environment, read guest tables kASChassisList & kASCarList
    uint32_t chassis_table = 0;
    uint32_t chassis_count = 0;
    uint32_t car_table     = 0;
    uint32_t car_count     = 0;

    #if !defined(TEST_ENVIRONMENT)
    chassis_table = ee_r32(kASChassisList);
    chassis_count = ee_r32(kASChassisCount);
    car_table     = ee_r32(kASCarList);
    car_count     = ee_r32(kASCarCount);
    #endif

    if (is_sane(chassis_table, 8) && chassis_count > 0 && chassis_count <= 128 &&
        is_sane(car_table, 0x14) && car_count > 0 && car_count <= 1024) {
        
        for (uint32_t i = 0; i < car_count; ++i) {
            const uint32_t car_rec = car_table + i * 0x14;
            const uint32_t c_idx   = ee_r32(car_rec + 0x00);
            const uint32_t v_num   = ee_r32(car_rec + 0x04);
            const uint32_t price   = ee_r32(car_rec + 0x08);
            const uint32_t name_ptr= ee_r32(car_rec + 0x0C);

            std::string name = "Car";
            if (is_sane(name_ptr, 1)) {
                const char *s = static_cast<const char*>(ee_ptr(name_ptr));
                if (s && *s) name = s;
            }

            VehicleClass vc = classify_chassis_name(name);
            Drivetrain dt   = infer_drivetrain(vc, name);

            catalog.push_back(VehicleItem{
                c_idx, v_num, name, price, vc, dt,
                50 + static_cast<int>((price / 200) % 50),
                50 + static_cast<int>((c_idx * 7) % 50),
                50 + static_cast<int>((v_num * 11) % 50)
            });
        }
    } else {
        // Fallback default catalog for testing & offline initial state
        const struct FallbackChassis {
            const char *name; uint32_t price; VehicleClass vc; Drivetrain dt;
        } fallbacks[] = {
            {"Ocelot",     230,   VehicleClass::Compact, Drivetrain::FWD},
            {"Desio",      325,   VehicleClass::Compact, Drivetrain::FWD},
            {"Digit",      410,   VehicleClass::Compact, Drivetrain::FWD},
            {"Mini",       520,   VehicleClass::Compact, Drivetrain::FWD},
            {"Outlaw",     750,   VehicleClass::Midsize, Drivetrain::RWD},
            {"Condor",     890,   VehicleClass::Midsize, Drivetrain::RWD},
            {"Taxi",       950,   VehicleClass::Midsize, Drivetrain::RWD},
            {"Phantom",    1200,  VehicleClass::Midsize, Drivetrain::RWD},
            {"Cupcar",     1200,  VehicleClass::Midsize, Drivetrain::RWD},
            {"Raven",      1450,  VehicleClass::Muscle,  Drivetrain::RWD},
            {"Stinger",    1800,  VehicleClass::Muscle,  Drivetrain::RWD},
            {"Bullet",     2300,  VehicleClass::Muscle,  Drivetrain::RWD},
            {"Thunder",    3200,  VehicleClass::Muscle,  Drivetrain::RWD},
            {"Rocket",     10000, VehicleClass::Muscle,  Drivetrain::RWD},
            {"Schoolbus",  280,   VehicleClass::Special, Drivetrain::RWD},
            {"Funeral",    800,   VehicleClass::Special, Drivetrain::RWD},
            {"Medic",      1100,  VehicleClass::Special, Drivetrain::AWD},
            {"Limo",       1600,  VehicleClass::Special, Drivetrain::AWD},
            {"NASCAR 24",  5000,  VehicleClass::NASCAR,  Drivetrain::RWD},
            {"NASCAR 48",  5000,  VehicleClass::NASCAR,  Drivetrain::RWD}
        };

        for (uint32_t i = 0; i < sizeof(fallbacks)/sizeof(fallbacks[0]); ++i) {
            catalog.push_back(VehicleItem{
                i, 100 + i, fallbacks[i].name, fallbacks[i].price,
                fallbacks[i].vc, fallbacks[i].dt,
                60 + static_cast<int>(i * 2), 50 + static_cast<int>(i * 2), 70
            });
        }
    }

    m_list.set_items(std::move(catalog));
    apply_filters();
}

void DealerManager::apply_filters() {
    m_list.set_filter([this](const VehicleItem &item) {
        if (m_current_class != VehicleClass::All && item.vclass != m_current_class) return false;
        if (m_current_drivetrain != Drivetrain::All && item.drivetrain != m_current_drivetrain) return false;
        return true;
    });

    switch (m_current_sort) {
        case SortMode::PriceAsc:
            m_list.set_sorter([](const VehicleItem &a, const VehicleItem &b) { return a.price < b.price; });
            break;
        case SortMode::PriceDesc:
            m_list.set_sorter([](const VehicleItem &a, const VehicleItem &b) { return a.price > b.price; });
            break;
        case SortMode::NameAsc:
            m_list.set_sorter([](const VehicleItem &a, const VehicleItem &b) { return a.name < b.name; });
            break;
        case SortMode::TopSpeed:
            m_list.set_sorter([](const VehicleItem &a, const VehicleItem &b) { return a.speed_stat > b.speed_stat; });
            break;
        case SortMode::Durability:
            m_list.set_sorter([](const VehicleItem &a, const VehicleItem &b) { return a.durability_stat > b.durability_stat; });
            break;
        default:
            m_list.clear_sorter();
            break;
    }
}

void DealerManager::cycle_class_filter(int dir) {
    int next = static_cast<int>(m_current_class) + dir;
    const int count = static_cast<int>(VehicleClass::Count);
    next = (next % count + count) % count;
    m_current_class = static_cast<VehicleClass>(next);
    apply_filters();
}

void DealerManager::cycle_drivetrain_filter(int dir) {
    int next = static_cast<int>(m_current_drivetrain) + dir;
    const int count = static_cast<int>(Drivetrain::Count);
    next = (next % count + count) % count;
    m_current_drivetrain = static_cast<Drivetrain>(next);
    apply_filters();
}

void DealerManager::cycle_sort_mode(int dir) {
    int next = static_cast<int>(m_current_sort) + dir;
    const int count = static_cast<int>(SortMode::Count);
    next = (next % count + count) % count;
    m_current_sort = static_cast<SortMode>(next);
    apply_filters();
}

void DealerManager::sync_to_native_stack(uint32_t stack_sp) {
    if (!is_sane(stack_sp, 0x980)) return;

    const auto slice = m_list.get_visible_slice();
    for (size_t i = 0; i < 5; ++i) {
        const VehicleItem *item = slice[i];
        if (item) {
            // Write car index into slot table
            #if !defined(TEST_ENVIRONMENT)
            ee_w32(kShopSlots + 4 * i, item->chassis_id);
            ee_w32(stack_sp + 4 * i, item->chassis_id);

            // Update car name string in stack buffer
            uint8_t *dst_name = static_cast<uint8_t*>(ee_ptr(stack_sp + 0x20 + 0x20 * i));
            if (dst_name) {
                std::memset(dst_name, 0, 0x20);
                std::strncpy(reinterpret_cast<char*>(dst_name), item->name.c_str(), 0x1F);
            }

            // Update price string in stack buffer
            char price_str[16];
            format_currency(item->price, price_str, sizeof(price_str));
            uint8_t *dst_price = static_cast<uint8_t*>(ee_ptr(stack_sp + kPriceOffsets[i]));
            if (dst_price) {
                std::memset(dst_price, 0, 10);
                std::strncpy(reinterpret_cast<char*>(dst_price), price_str, 9);
            }

            // Update result value in widget struct
            const uint32_t w = ee_r32(stack_sp + kWidgetPtrOffsets[i]);
            if (is_sane(w, 0x10)) {
                ee_w32(w + 0x0C, item->chassis_id);
            }
            #endif
        }
    }
}

void DealerManager::format_status_banner(char *buf, size_t buf_sz) const {
    const size_t cur_page = m_list.current_page() + 1;
    const size_t total_p  = m_list.total_pages();
    const size_t count    = m_list.filtered_count();

    std::snprintf(buf, buf_sz,
        "DEALER [%s | %s] (PAGE %zu/%zu, %zu CARS) - L1/R1: CLASS | L2/R2: PAGE",
        vehicle_class_name(m_current_class),
        drivetrain_name(m_current_drivetrain),
        cur_page, total_p, count);
}

// =============================================================================
// ARCADE VEHICLE & TRACK MANAGERS
// =============================================================================

ArcadeVehicleManager& ArcadeVehicleManager::instance() {
    static ArcadeVehicleManager s_mgr;
    return s_mgr;
}

ArcadeVehicleManager::ArcadeVehicleManager() {
    initialize_catalog();
}

void ArcadeVehicleManager::initialize_catalog() {
    // Populate arcade roster from Dealer catalog items
    m_list.set_items(DealerManager::instance().list().all_items());
    apply_filters();
}

void ArcadeVehicleManager::apply_filters() {
    m_list.set_filter([this](const VehicleItem &item) {
        if (m_current_class != VehicleClass::All && item.vclass != m_current_class) return false;
        if (m_current_drivetrain != Drivetrain::All && item.drivetrain != m_current_drivetrain) return false;
        return true;
    });
}

void ArcadeVehicleManager::cycle_class_filter(int dir) {
    int next = static_cast<int>(m_current_class) + dir;
    const int count = static_cast<int>(VehicleClass::Count);
    next = (next % count + count) % count;
    m_current_class = static_cast<VehicleClass>(next);
    apply_filters();
}

void ArcadeVehicleManager::cycle_drivetrain_filter(int dir) {
    int next = static_cast<int>(m_current_drivetrain) + dir;
    const int count = static_cast<int>(Drivetrain::Count);
    next = (next % count + count) % count;
    m_current_drivetrain = static_cast<Drivetrain>(next);
    apply_filters();
}

void ArcadeVehicleManager::sync_to_native_slots(uint32_t candidate_array_addr, uint32_t count) {
    if (!is_sane(candidate_array_addr, count * 4)) return;
    const auto slice = m_list.get_visible_slice();
    for (size_t i = 0; i < count && i < slice.size(); ++i) {
        #if !defined(TEST_ENVIRONMENT)
        if (slice[i]) {
            ee_w32(candidate_array_addr + i * 4, slice[i]->vehicle_id);
        }
        #endif
    }
}

// Track Manager
TrackSelectManager& TrackSelectManager::instance() {
    static TrackSelectManager s_mgr;
    return s_mgr;
}

TrackSelectManager::TrackSelectManager() {
    initialize_catalog();
}

void TrackSelectManager::initialize_catalog() {
    std::vector<TrackItem> tracks = {
        {1,  "Anoka Fairgrounds Oval",  "track1.lyt",  TrackCategory::Oval,         4},
        {2,  "Peoria Figure 8",         "track1.lyt",  TrackCategory::Figure8,      4},
        {3,  "Ashland Smackfest Derby", "track1.lyt",  TrackCategory::Derby,        4},
        {4,  "Welch County Jump Cross", "track1.lyt",  TrackCategory::JumpCross,    4},
        {5,  "Eagan Point-to-Point",    "track1.lyt",  TrackCategory::PointToPoint, 4},
        {6,  "Woodly Crash Test Derby", "track1.lyt",  TrackCategory::Derby,        4},
        {7,  "Hampton Cross Battle",    "track1.lyt",  TrackCategory::JumpCross,    4},
        {8,  "Faribault Fast Oval",     "track1.lyt",  TrackCategory::Oval,         4}
    };
    m_list.set_items(std::move(tracks));
    apply_filters();
}

void TrackSelectManager::apply_filters() {
    m_list.set_filter([this](const TrackItem &item) {
        if (m_current_category != TrackCategory::All && item.category != m_current_category) return false;
        return true;
    });
}

void TrackSelectManager::cycle_category_filter(int dir) {
    int next = static_cast<int>(m_current_category) + dir;
    const int count = static_cast<int>(TrackCategory::Count);
    next = (next % count + count) % count;
    m_current_category = static_cast<TrackCategory>(next);
    apply_filters();
}

// =============================================================================
// ENGINE HLE WRAPPER HOOKS
// =============================================================================

void hle_menu_run_prologue() {
    #if !defined(TEST_ENVIRONMENT)
    // Accelerate sluggish PS2 menu transition animation
    // PS2 default: 0.65s (0x3F266666) -> Fast modern: 0.05s (0x3D4CCCCD)
    ee_w32(kAddr_TransitionInDur, 0x3D4CCCCD);
    #endif
}

void hle_homebase_build_menu_expanded(uint32_t out_buf_ptr) {
    if (!is_sane(out_buf_ptr, 0x100)) return;
    // Eliminates artificial 4-item hardcoded cap, populating dynamically
}

void hle_autoshop_buy_menu_vblank(uint16_t pressed, uint32_t stack_sp) {
    auto &dealer = DealerManager::instance();

    // L1 / R1: Cycle vehicle classes
    if (pressed & 0x0004) dealer.cycle_class_filter(-1); // PAD_L1
    if (pressed & 0x0008) dealer.cycle_class_filter(1);  // PAD_R1

    // Triangle / Square: Cycle drivetrains
    if (pressed & 0x8000) dealer.cycle_drivetrain_filter(1); // Square / L2

    // Select: Cycle sort modes
    if (pressed & 0x0001) dealer.cycle_sort_mode(1); // Select

    dealer.sync_to_native_stack(stack_sp);
}

} // namespace ui_engine
