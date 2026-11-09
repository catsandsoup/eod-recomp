// career_upgrades.cpp - Career Vehicle Upgrades, Garage, and Tuning System
// Decompiled and extended for Test Drive: Eve of Destruction (eod-recomp)
#include "career_upgrades.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/cpu.h"
#include "core/host_file.h"
#include "core/log.h"
#include "core/memory.h"
#include "funcs.h"
#include "platform/platform.h"

extern "C" {
void f_0012C898_orig(void);
void f_002E2708_orig(void);
void f_002E1870_orig(void);
void f_002E1960_orig(void);
void f_002E18E0_orig(void);
void f_002E1BC0_orig(void);
void f_002E1BB0_orig(void);
void f_002E1380_orig(void);
void f_002E4280_orig(void);
void f_002E3BC8_orig(void);
void f_002E3E70_orig(void);
void f_002DFDF8_orig(void);
void f_002DED18_orig(void);
void f_00370AA8_orig(void);
}

namespace eod {

// =============================================================================
// Memory Constants & Pointers
// =============================================================================
static const uint32_t kCareerSave       = 0x003D9384; // Pointer: CareerSaveInfo (0xE7C bytes)
static const uint32_t kCDBRes           = 0x003D9394; // Pointer: CDBResSaveInfo (0x194 bytes)
static const uint32_t kCDBupgr          = 0x003D9398; // Pointer: CDBupgr (0xC68 bytes)
static const uint32_t kASChassisList    = 0x003D93B8; // 8-byte chassis records
static const uint32_t kASChassisCount   = 0x003D93BC;
static const uint32_t kASCarList        = 0x003D93C0; // 0x14-byte car records
static const uint32_t kASCarCount       = 0x003D93C4;
static const uint32_t kCarStatsCount    = 0x003CBF40; // Total count of car stats entries
static const uint32_t kCarStatsTable    = 0x00439B20; // CarStatsEntry array (20 bytes per car)
static const uint32_t kCatalogPtr       = 0x003D9374; // Pointer to catalog.val parts list
static const uint32_t kCatalogClassMask = 0x004AAF88; // Class compatibility bitmask table
static const uint32_t kOwnedCarList     = 0x004AB0E0; // Array of owned car records (0x14 bytes each)
static const uint32_t kEventWord        = 0x003CF19C; // Active event word (0x8000226 in free roam)

static inline bool IsValidAddress(uint32_t addr, uint32_t span) {
    return addr != 0 && (addr % 4 == 0) && (addr + span <= EE_RAM_SIZE);
}

// Extended garage slots (slots 4..15, total capacity = 16 cars)
static CDBGarageSlot s_extended_slots[12];
static CDBPartEntry  s_extended_parts[60];
static uint32_t      s_extended_car_count = 0;
static uint32_t      s_extended_part_count = 0;
static int           s_garage_page = 0; // Active page in garage screen (0..3)

void CareerUpgrades_Init() {
    std::memset(s_extended_slots, 0, sizeof(s_extended_slots));
    std::memset(s_extended_parts, 0, sizeof(s_extended_parts));
    s_extended_car_count = 0;
    s_extended_part_count = 0;
    s_garage_page = 0;
}

// =============================================================================
// Decompiled f_0012C898 - CarStatsLookup
// Scans loaded CarStats table (0x00439B20) for entry matching chassis_name
// =============================================================================
const CarStatsEntry* CarStatsLookup(const char* chassis_name) {
    if (!chassis_name) return nullptr;
    const uint32_t count = ee_r32(kCarStatsCount);
    const uint32_t table_addr = ee_r32(kCarStatsTable);
    if (!IsValidAddress(table_addr, count * sizeof(CarStatsEntry)) || count == 0 || count > 256) {
        return nullptr;
    }

    for (uint32_t i = 0; i < count; i++) {
        const uint32_t entry_addr = table_addr + i * sizeof(CarStatsEntry);
        const uint32_t name_ptr = ee_r32(entry_addr);
        if (IsValidAddress(name_ptr, 1)) {
            const char* entry_name = (const char*)ee_ptr(name_ptr);
            if (entry_name && std::strcmp(entry_name, chassis_name) == 0) {
                return (const CarStatsEntry*)ee_ptr(entry_addr);
            }
        }
    }
    return nullptr;
}

// =============================================================================
// Decompiled f_002E2708 - AutoShopGetVehicleStats
// Looks up base ratings and applies +1 increment per installed upgrade category
// =============================================================================
void AutoShopGetVehicleStats(uint32_t car_idx, CarStatsEntry* out_stats) {
    if (!out_stats) return;
    std::memset(out_stats, 0, sizeof(CarStatsEntry));

    // Get owned car record pointer (f_002E4280)
    const uint32_t owned_rec = kOwnedCarList + 0x14 * car_idx;
    if (!IsValidAddress(owned_rec, 0x14)) return;

    const uint32_t chassis_name_ptr = ee_r32(owned_rec + 0x0C);
    const char* chassis_name = IsValidAddress(chassis_name_ptr, 1) ? (const char*)ee_ptr(chassis_name_ptr) : nullptr;
    if (!chassis_name) return;

    // Lookup base ratings in car_stats table
    const CarStatsEntry* base = CarStatsLookup(chassis_name);
    if (base) {
        *out_stats = *base;
    }

    // Category 0: Engine upgrade adds +1 Speed
    uint32_t part_key = 0;
    if (CDBupgr_GetCarPart(car_idx, 0, &part_key)) {
        out_stats->speed += 1;
    }

    // Category 1: Suspension upgrade adds +1 Handling
    if (CDBupgr_GetCarPart(car_idx, 1, &part_key)) {
        out_stats->handling += 1;
    }

    // Category 4: Exhaust upgrade adds +1 Durability
    if (CDBupgr_GetCarPart(car_idx, 4, &part_key)) {
        out_stats->durability += 1;
    }

    // Modern Multi-Stage extension:
    // Category 2: Aero/Weight Reduction adds +1 Speed and +1 Handling
    if (g_config.career_multistage_upgrades && CDBupgr_GetCarPart(car_idx, 2, &part_key)) {
        out_stats->speed += 1;
        out_stats->handling += 1;
    }

    // Category 3: High-Performance Brakes adds +1 Handling
    if (g_config.career_multistage_upgrades && CDBupgr_GetCarPart(car_idx, 3, &part_key)) {
        out_stats->handling += 1;
    }

    // Clamp ratings to max bar value (10)
    out_stats->speed = std::clamp(out_stats->speed, 1, 10);
    out_stats->handling = std::clamp(out_stats->handling, 1, 10);
    out_stats->durability = std::clamp(out_stats->durability, 1, 10);
}

// =============================================================================
// Decompiled f_00185018 - Vehicle Physics Power & Torque Curve Conversion
// Uses constant 5252.0f: HP = (Torque * RPM) / 5252.0f
// =============================================================================
CalculatedVehicleStats CalculateFullVehiclePerformance(uint32_t car_idx) {
    CalculatedVehicleStats stats{};
    CarStatsEntry ui_ratings{};
    AutoShopGetVehicleStats(car_idx, &ui_ratings);

    stats.speed_rating = ui_ratings.speed;
    stats.handling_rating = ui_ratings.handling;
    stats.durability_rating = ui_ratings.durability;
    stats.curb_weight_lbs = (float)ui_ratings.mass;
    if (stats.curb_weight_lbs <= 0.0f) stats.curb_weight_lbs = 3500.0f;

    // Default baseline stock vehicle parameters
    float base_hp = 180.0f + (float)ui_ratings.speed * 28.0f;
    float peak_torque_rpm = 3800.0f;
    float power_peak_rpm = 5400.0f;
    float redline = 6200.0f;

    // Fetch tuning block for vehicle
    CDBGarageSlot* slot = Garage_GetSlot(car_idx);
    float power_mult = 1.0f;
    float torque_mult = 1.0f;
    float torque_rpm_mult = 1.0f;

    if (slot && slot->tuning.active > 0.0f) {
        power_mult = slot->tuning.power_mult;
        torque_mult = slot->tuning.torque_mult;
        torque_rpm_mult = slot->tuning.torque_rpm_mult;
        if (slot->tuning.redline_mult > 0.5f) redline *= slot->tuning.redline_mult;
    }

    // Automotive horsepower / torque conversion spline
    // Standard physics relation: HP = (Torque * RPM) / 5252.0f
    float scaled_hp = base_hp * power_mult;
    float calc_torque = (scaled_hp * 5252.0f) / power_peak_rpm * torque_mult;

    stats.horsepower = scaled_hp;
    stats.peak_torque = calc_torque;
    stats.redline_rpm = redline;

    // Top speed calculation based on aerodynamic drag and horsepower
    // V_max ~= c * (HP / CdA)^(1/3)
    stats.top_speed_mph = 85.0f + std::cbrt(scaled_hp) * 16.5f;
    if (stats.top_speed_mph > 195.0f) stats.top_speed_mph = 195.0f;

    // 1/4 mile elapsed time estimation (approx. ET = 5.825 * (Mass / HP)^(1/3))
    stats.quarter_mile_sec = 5.825f * std::cbrt(stats.curb_weight_lbs / scaled_hp);

    return stats;
}

// =============================================================================
// CDBupgr Part Management Implementation
// =============================================================================
bool CDBupgr_HasPart(uint32_t car_idx, uint32_t category) {
    uint32_t key = 0;
    return CDBupgr_GetCarPart(car_idx, category, &key);
}

int32_t CDBupgr_GetCarPart(uint32_t car_idx, uint32_t category, uint32_t* out_part_key) {
    // Check extended slots first if index >= 4
    if (car_idx >= 4) {
        for (uint32_t i = 0; i < s_extended_part_count && i < 60; i++) {
            if (s_extended_parts[i].car_idx == (uint8_t)car_idx &&
                s_extended_parts[i].category == (uint8_t)category) {
                if (out_part_key) *out_part_key = s_extended_parts[i].part_id;
                return 1;
            }
        }
        return 0;
    }

    // Native CDBupgr lookup (*0x003D9398)
    const uint32_t cdb_addr = ee_r32(kCDBupgr);
    if (!IsValidAddress(cdb_addr, sizeof(CDBupgr))) return 0;

    const uint32_t part_count = ee_r32(cdb_addr + offsetof(CDBupgr, part_count));
    for (uint32_t i = 0; i < part_count && i < 20; i++) {
        const uint32_t entry_addr = cdb_addr + offsetof(CDBupgr, parts) + i * sizeof(CDBPartEntry);
        const uint8_t entry_car = ee_r8(entry_addr + 1);
        const uint8_t entry_cat = ee_r8(entry_addr + 3);
        if (entry_car == (uint8_t)car_idx && entry_cat == (uint8_t)category) {
            if (out_part_key) *out_part_key = ee_r8(entry_addr + 0);
            return 1;
        }
    }
    return 0;
}

bool CDBupgr_AddPart(uint32_t car_idx, uint32_t category, uint32_t part_key, uint8_t condition) {
    if (car_idx >= 4) {
        if (s_extended_part_count >= 60) return false;
        s_extended_parts[s_extended_part_count].part_id = (uint8_t)part_key;
        s_extended_parts[s_extended_part_count].car_idx = (uint8_t)car_idx;
        s_extended_parts[s_extended_part_count].condition = condition;
        s_extended_parts[s_extended_part_count].category = (uint8_t)category;
        s_extended_part_count++;
        return true;
    }

    const uint32_t cdb_addr = ee_r32(kCDBupgr);
    if (!IsValidAddress(cdb_addr, sizeof(CDBupgr))) return false;

    uint32_t count = ee_r32(cdb_addr + offsetof(CDBupgr, part_count));
    if (count >= 20) return false;

    const uint32_t entry_addr = cdb_addr + offsetof(CDBupgr, parts) + count * sizeof(CDBPartEntry);
    ee_w8(entry_addr + 0, (uint8_t)part_key);
    ee_w8(entry_addr + 1, (uint8_t)car_idx);
    ee_w8(entry_addr + 2, condition);
    ee_w8(entry_addr + 3, (uint8_t)category);
    ee_w32(cdb_addr + offsetof(CDBupgr, part_count), count + 1);
    return true;
}

bool CDBupgr_RemovePart(uint32_t car_idx, uint32_t category) {
    if (car_idx >= 4) {
        for (uint32_t i = 0; i < s_extended_part_count; i++) {
            if (s_extended_parts[i].car_idx == (uint8_t)car_idx &&
                s_extended_parts[i].category == (uint8_t)category) {
                for (uint32_t j = i; j + 1 < s_extended_part_count; j++) {
                    s_extended_parts[j] = s_extended_parts[j + 1];
                }
                s_extended_part_count--;
                return true;
            }
        }
        return false;
    }

    const uint32_t cdb_addr = ee_r32(kCDBupgr);
    if (!IsValidAddress(cdb_addr, sizeof(CDBupgr))) return false;

    uint32_t count = ee_r32(cdb_addr + offsetof(CDBupgr, part_count));
    for (uint32_t i = 0; i < count && i < 20; i++) {
        const uint32_t entry_addr = cdb_addr + offsetof(CDBupgr, parts) + i * sizeof(CDBPartEntry);
        if (ee_r8(entry_addr + 1) == (uint8_t)car_idx && ee_r8(entry_addr + 3) == (uint8_t)category) {
            // Shift remaining parts down
            for (uint32_t j = i; j + 1 < count && j + 1 < 20; j++) {
                const uint32_t src = cdb_addr + offsetof(CDBupgr, parts) + (j + 1) * sizeof(CDBPartEntry);
                const uint32_t dst = cdb_addr + offsetof(CDBupgr, parts) + j * sizeof(CDBPartEntry);
                ee_w32(dst, ee_r32(src));
            }
            ee_w32(cdb_addr + offsetof(CDBupgr, part_count), count - 1);
            return true;
        }
    }
    return false;
}

// =============================================================================
// Valuation & Depreciation Algorithms
// =============================================================================
uint32_t CareerCalcTradeInDepreciation(uint32_t car_price, uint32_t parts_value, float condition) {
    if (g_config.career_free_swap) return 0;
    condition = std::clamp(condition, 0.0f, 1.0f);
    const float total_val = (float)(car_price + parts_value);
    const float dep = total_val * (1.0f - condition);
    return (uint32_t)std::round(dep);
}

uint32_t CareerGetOwnedCarResaleValue(uint32_t car_price) {
    if (g_config.career_free_swap) return car_price;
    // Native resale is 90% of original price paid
    return (uint32_t)std::round((float)car_price * 0.90f);
}

uint32_t CareerGetCarInstalledPartsResaleValue(uint32_t car_idx) {
    uint32_t total_resale = 0;
    // Scan all 5 categories
    for (uint32_t cat = 0; cat < 5; cat++) {
        uint32_t part_key = 0;
        if (CDBupgr_GetCarPart(car_idx, cat, &part_key)) {
            // Part base price lookup from catalog
            uint32_t part_price = 100; // fallback
            const uint32_t cat_addr = ee_r32(kCatalogPtr);
            if (IsValidAddress(cat_addr, sizeof(CatalogPart) * 16)) {
                const uint32_t rec_addr = cat_addr + (part_key % 16) * sizeof(CatalogPart);
                part_price = ee_r32(rec_addr + offsetof(CatalogPart, price));
            }
            // Installed parts resale value is 75% of retail price
            const uint32_t part_resale = g_config.career_free_swap ? part_price : (uint32_t)((float)part_price * 0.75f);
            total_resale += part_resale;
        }
    }
    return total_resale;
}

uint32_t CareerGetTotalCarTradeInPayout(uint32_t car_idx) {
    const uint32_t owned_rec = kOwnedCarList + 0x14 * car_idx;
    const uint32_t price_paid = IsValidAddress(owned_rec, 0x14) ? ee_r32(owned_rec + 0x08) : 0;
    const uint32_t chassis_resale = CareerGetOwnedCarResaleValue(price_paid);
    const uint32_t parts_resale = CareerGetCarInstalledPartsResaleValue(car_idx);

    CDBGarageSlot* slot = Garage_GetSlot(car_idx);
    const float condition = slot ? slot->tuning.condition : 1.0f;
    const uint32_t dep = CareerCalcTradeInDepreciation(chassis_resale, parts_resale, condition);
    return (chassis_resale + parts_resale > dep) ? (chassis_resale + parts_resale - dep) : 0;
}

// =============================================================================
// Constraint Removal: Class Compatibility Unlock
// =============================================================================
bool AutoShopIsPartCompatible(uint32_t chassis_class, uint32_t part_key) {
    // If Universal Parts toggle is active, any part fits any chassis!
    if (g_config.career_universal_parts) {
        return true;
    }

    // Native class bitmask lookup (f_002DFDF8)
    const uint32_t mask_table = kCatalogClassMask;
    if (IsValidAddress(mask_table, 16)) {
        const uint32_t allowed_mask = ee_r32(mask_table + 4 * (chassis_class % 4));
        // Check if part belongs to this class's mask
        const uint32_t part_bit = (1u << (part_key % 32));
        return (allowed_mask & part_bit) != 0;
    }
    return true;
}

// =============================================================================
// Constraint Removal: Multi-Stage Upgrades & Automatic Swapping
// =============================================================================
int32_t AutoShopPurchasePartWithMultiStage(uint32_t car_idx, uint32_t part_key) {
    const uint32_t cdb_addr = ee_r32(kCDBupgr);
    if (!IsValidAddress(cdb_addr, sizeof(CDBupgr))) return 0; // Error

    const int32_t player_cash = (int32_t)ee_r32(cdb_addr + offsetof(CDBupgr, money));

    // Lookup part metadata
    const uint32_t cat_addr = ee_r32(kCatalogPtr);
    uint32_t category = 0;
    uint32_t part_price = 250;
    if (IsValidAddress(cat_addr, sizeof(CatalogPart) * 16)) {
        const uint32_t rec_addr = cat_addr + (part_key % 16) * sizeof(CatalogPart);
        category = ee_r32(rec_addr + offsetof(CatalogPart, category));
        part_price = ee_r32(rec_addr + offsetof(CatalogPart, price));
    }

    uint32_t existing_part_key = 0;
    const bool has_existing = CDBupgr_GetCarPart(car_idx, category, &existing_part_key);

    if (has_existing) {
        if (existing_part_key == part_key) {
            // Already owned identical part
            return 2; // Part already installed
        }

        if (!g_config.career_multistage_upgrades) {
            // Native lock: refuse second part in category
            LOG(LOG_CORE, "AutoShop: Blocked buying second part key %u for car %u in category %u",
                part_key, car_idx, category);
            return 2;
        }

        // Multi-Stage Upgrade: uninstall previous stage and credit resale value!
        uint32_t old_resale = 0;
        if (IsValidAddress(cat_addr, sizeof(CatalogPart) * 16)) {
            const uint32_t old_rec = cat_addr + (existing_part_key % 16) * sizeof(CatalogPart);
            const uint32_t old_price = ee_r32(old_rec + offsetof(CatalogPart, price));
            old_resale = g_config.career_free_swap ? old_price : (uint32_t)((float)old_price * 0.75f);
        }

        const int32_t net_cost = (int32_t)part_price - (int32_t)old_resale;
        if (player_cash < net_cost) {
            return 0; // Insufficient funds
        }

        // Apply swap
        CDBupgr_RemovePart(car_idx, category);
        CDBupgr_AddPart(car_idx, category, part_key, 100);

        // Deduct net price from money
        ee_w32(cdb_addr + offsetof(CDBupgr, money), (uint32_t)(player_cash - net_cost));

        LOG(LOG_CORE, "AutoShop: Swapped category %u part %u -> %u (net cost $%d, player cash $%d)",
            category, existing_part_key, part_key, net_cost, player_cash - net_cost);
        return 4; // Success
    }

    // First installation in category
    if (player_cash < (int32_t)part_price) {
        return 0; // Insufficient funds
    }

    CDBupgr_AddPart(car_idx, category, part_key, 100);
    ee_w32(cdb_addr + offsetof(CDBupgr, money), (uint32_t)(player_cash - (int32_t)part_price));

    LOG(LOG_CORE, "AutoShop: Installed new part %u in category %u for car %u ($%d)",
        part_key, category, car_idx, part_price);
    return 4; // Success
}

// =============================================================================
// Constraint Removal: Expanded 16-Car Garage Manager & UI Pagination
// =============================================================================
uint32_t Garage_GetCapacity() {
    return g_config.career_garage_expansion ? 16 : 4;
}

uint32_t Garage_GetCarCount() {
    const uint32_t cdb_addr = ee_r32(kCDBupgr);
    const uint32_t native_count = IsValidAddress(cdb_addr, sizeof(CDBupgr)) ? ee_r32(cdb_addr + offsetof(CDBupgr, car_count)) : 0;
    if (!g_config.career_garage_expansion) return native_count;
    return native_count + s_extended_car_count;
}

uint32_t Garage_GetActiveCarIndex() {
    const uint32_t cdb_addr = ee_r32(kCDBupgr);
    return IsValidAddress(cdb_addr, sizeof(CDBupgr)) ? ee_r32(cdb_addr + offsetof(CDBupgr, active_car_idx)) : 0;
}

void Garage_SetActiveCarIndex(uint32_t slot_idx) {
    const uint32_t cdb_addr = ee_r32(kCDBupgr);
    if (IsValidAddress(cdb_addr, sizeof(CDBupgr))) {
        ee_w32(cdb_addr + offsetof(CDBupgr, active_car_idx), slot_idx % 16);
    }
}

CDBGarageSlot* Garage_GetSlot(uint32_t slot_idx) {
    if (slot_idx < 4) {
        const uint32_t cdb_addr = ee_r32(kCDBupgr);
        if (!IsValidAddress(cdb_addr, sizeof(CDBupgr))) return nullptr;
        return (CDBGarageSlot*)ee_ptr(cdb_addr + offsetof(CDBupgr, cars) + slot_idx * sizeof(CDBGarageSlot));
    }
    if (slot_idx < 16 && g_config.career_garage_expansion) {
        return &s_extended_slots[slot_idx - 4];
    }
    return nullptr;
}

bool Garage_AddCar(uint32_t vehicle_id) {
    const uint32_t total_count = Garage_GetCarCount();
    if (total_count >= Garage_GetCapacity()) return false;

    if (total_count < 4) {
        // Native garage slot allocation
        const uint32_t cdb_addr = ee_r32(kCDBupgr);
        if (!IsValidAddress(cdb_addr, sizeof(CDBupgr))) return false;
        const uint32_t slot_addr = cdb_addr + offsetof(CDBupgr, cars) + total_count * sizeof(CDBGarageSlot);
        ee_w32(slot_addr + offsetof(CDBGarageSlot, vehicle_id), vehicle_id);
        ee_w32(slot_addr + offsetof(CDBGarageSlot, flags), 1); // Owned
        ee_w32(slot_addr + offsetof(CDBGarageSlot, tuning) + offsetof(CarTuningBlock, active), 0x3F800000); // 1.0f
        ee_w32(cdb_addr + offsetof(CDBupgr, car_count), total_count + 1);
        return true;
    }

    // Extended garage slot allocation (slots 4..15)
    const uint32_t ext_idx = total_count - 4;
    if (ext_idx >= 12) return false;

    std::memset(&s_extended_slots[ext_idx], 0, sizeof(CDBGarageSlot));
    s_extended_slots[ext_idx].vehicle_id = vehicle_id;
    s_extended_slots[ext_idx].flags = 1;
    s_extended_slots[ext_idx].tuning.active = 1.0f;
    s_extended_slots[ext_idx].tuning.power_mult = 1.0f;
    s_extended_slots[ext_idx].tuning.torque_mult = 1.0f;
    s_extended_slots[ext_idx].tuning.condition = 1.0f;
    s_extended_car_count++;

    LOG(LOG_CORE, "Garage: Added vehicle %u to extended slot %u (total cars %u)",
        vehicle_id, total_count, total_count + 1);
    return true;
}

bool Garage_RemoveCar(uint32_t slot_idx) {
    const uint32_t total = Garage_GetCarCount();
    if (slot_idx >= total) return false;

    if (slot_idx < 4 && s_extended_car_count == 0) {
        // Pure native shift
        const uint32_t cdb_addr = ee_r32(kCDBupgr);
        if (!IsValidAddress(cdb_addr, sizeof(CDBupgr))) return false;
        for (uint32_t i = slot_idx; i + 1 < total && i + 1 < 4; i++) {
            const uint32_t src = cdb_addr + offsetof(CDBupgr, cars) + (i + 1) * sizeof(CDBGarageSlot);
            const uint32_t dst = cdb_addr + offsetof(CDBupgr, cars) + i * sizeof(CDBGarageSlot);
            std::memcpy(ee_ptr(dst), ee_ptr(src), sizeof(CDBGarageSlot));
        }
        ee_w32(cdb_addr + offsetof(CDBupgr, car_count), total - 1);
        return true;
    }

    // Mixed/Extended garage shift: flatten all slots, erase, and restore
    std::vector<CDBGarageSlot> all_slots;
    for (uint32_t i = 0; i < total; i++) {
        CDBGarageSlot* s = Garage_GetSlot(i);
        if (s) all_slots.push_back(*s);
    }
    if (slot_idx < all_slots.size()) {
        all_slots.erase(all_slots.begin() + slot_idx);
    }

    // Re-populate native slots
    const uint32_t cdb_addr = ee_r32(kCDBupgr);
    const uint32_t native_new_count = std::min((uint32_t)all_slots.size(), 4u);
    for (uint32_t i = 0; i < native_new_count; i++) {
        const uint32_t dst = cdb_addr + offsetof(CDBupgr, cars) + i * sizeof(CDBGarageSlot);
        std::memcpy(ee_ptr(dst), &all_slots[i], sizeof(CDBGarageSlot));
    }
    ee_w32(cdb_addr + offsetof(CDBupgr, car_count), native_new_count);

    // Re-populate extended slots
    s_extended_car_count = (all_slots.size() > 4) ? (uint32_t)(all_slots.size() - 4) : 0;
    std::memset(s_extended_slots, 0, sizeof(s_extended_slots));
    for (uint32_t i = 0; i < s_extended_car_count && i < 12; i++) {
        s_extended_slots[i] = all_slots[4 + i];
    }
    return true;
}

int Garage_GetActivePage() {
    return s_garage_page;
}

int Garage_GetTotalPages() {
    const uint32_t total = Garage_GetCarCount();
    if (total == 0) return 1;
    return (int)((total + 3) / 4);
}

void Garage_CyclePage(int delta) {
    const int pages = Garage_GetTotalPages();
    if (pages <= 1) return;
    s_garage_page = ((s_garage_page + delta) % pages + pages) % pages;
    char banner[96];
    std::snprintf(banner, sizeof(banner), "GARAGE: PAGE %d OF %d (CARS %d-%d)",
                  s_garage_page + 1, pages, s_garage_page * 4 + 1,
                  std::min((s_garage_page + 1) * 4, (int)Garage_GetCarCount()));
    platform_show_hud_banner(banner, 2500);
}

uint32_t Garage_GetSlotOnCurrentPage(int ui_button_idx) {
    return (uint32_t)(s_garage_page * 4 + (ui_button_idx % 4));
}

// =============================================================================
// Extended Garage Companion File Persistence (eod_garage_ext.dat)
// =============================================================================
bool Garage_SaveExtendedSlots(const std::string& save_dir) {
    if (!g_config.career_garage_expansion || s_extended_car_count == 0) return true;
    const std::string path = save_dir + "/eod_garage_ext.dat";
    FILE* f = host_fopen(path.c_str(), "wb");
    if (!f) return false;

    CDBGarageExtSave ext{};
    ext.magic = 0x47415258; // "GARX"
    ext.version = 1;
    ext.extended_count = s_extended_car_count;
    ext.active_car_idx = Garage_GetActiveCarIndex();
    std::memcpy(ext.ext_cars, s_extended_slots, sizeof(ext.ext_cars));
    std::memcpy(ext.ext_parts, s_extended_parts, sizeof(ext.ext_parts));

    size_t written = std::fwrite(&ext, sizeof(ext), 1, f);
    std::fclose(f);
    LOG(LOG_CORE, "Garage: Saved %u extended cars to %s", s_extended_car_count, path.c_str());
    return written == 1;
}

bool Garage_LoadExtendedSlots(const std::string& save_dir) {
    if (!g_config.career_garage_expansion) return true;
    const std::string path = save_dir + "/eod_garage_ext.dat";
    FILE* f = host_fopen(path.c_str(), "rb");
    if (!f) {
        // No extended save file exists yet
        s_extended_car_count = 0;
        return true;
    }

    CDBGarageExtSave ext{};
    size_t read_bytes = std::fread(&ext, sizeof(ext), 1, f);
    std::fclose(f);

    if (read_bytes == 1 && ext.magic == 0x47415258 && ext.version == 1) {
        s_extended_car_count = std::min(ext.extended_count, 12u);
        std::memcpy(s_extended_slots, ext.ext_cars, sizeof(s_extended_slots));
        std::memcpy(s_extended_parts, ext.ext_parts, sizeof(s_extended_parts));
        LOG(LOG_CORE, "Garage: Loaded %u extended cars from %s", s_extended_car_count, path.c_str());
        return true;
    }
    return false;
}

} // namespace eod

// =============================================================================
// HLE C-Linkage Export Functions & Wrappers
// =============================================================================

// f_002DFDF8: Class Compatibility Bitmask Lookup
// Replaced/wrapped to allow universal part fitting across all car classes
extern "C" void f_002DFDF8(void) {
    if (g_config.career_universal_parts) {
        // Return universal bitmask (all parts compatible)
        g_ee.r[2].sd[0] = (int64_t)(int32_t)0xFFFFFFFF;
        return;
    }
    f_002DFDF8_orig();
}

// f_002DED18: AutoShop Part Purchase & Validation
// Intercepts purchase to enable multi-stage upgrades, category swapping, and refunds
extern "C" void f_002DED18(void) {
    if (g_config.career_multistage_upgrades) {
        const uint32_t part_key = g_ee.r[4].uw[0];
        const uint32_t car_idx  = g_ee.r[5].uw[0];
        const int32_t result = eod::AutoShopPurchasePartWithMultiStage(car_idx, part_key);
        g_ee.r[2].sd[0] = (int64_t)result;
        return;
    }
    f_002DED18_orig();
}

// f_002E1BB0: CareerGetGarageCarCount
// Intercepted to return extended garage count when expansion is enabled
extern "C" void f_002E1BB0(void) {
    if (g_config.career_garage_expansion) {
        g_ee.r[2].sd[0] = (int64_t)(int32_t)eod::Garage_GetCarCount();
        return;
    }
    f_002E1BB0_orig();
}

// f_002E1BC0: CareerAddCarToGarage
// Intercepted to route cars 4..15 into extended slots
extern "C" void f_002E1BC0(void) {
    if (g_config.career_garage_expansion) {
        const uint32_t vehicle_id = g_ee.r[4].uw[0];
        bool ok = eod::Garage_AddCar(vehicle_id);
        g_ee.r[2].sd[0] = ok ? 1 : 0;
        return;
    }
    f_002E1BC0_orig();
}

// f_002E1380: CareerGetCarTuningBlock
// Intercepted to retrieve tuning parameters for extended slots without panicking
extern "C" void f_002E1380(void) {
    const uint32_t vehicle_idx = g_ee.r[4].uw[0];
    const uint32_t dst_buf     = g_ee.r[5].uw[0];

    if (g_config.career_garage_expansion && vehicle_idx >= 4) {
        eod::CDBGarageSlot* slot = eod::Garage_GetSlot(vehicle_idx);
        if (slot && dst_buf < EE_RAM_SIZE - sizeof(eod::CarTuningBlock)) {
            std::memcpy(ee_ptr(dst_buf), &slot->tuning, sizeof(eod::CarTuningBlock));
            g_ee.r[2].sd[0] = 1;
            return;
        }
    }
    f_002E1380_orig();
}
