// career_upgrades.h - Career Vehicle Upgrades, Garage, and Tuning System
// Decompiled and extended for Test Drive: Eve of Destruction (eod-recomp)
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eod {

#pragma pack(push, 1)

// =============================================================================
// Part & Inventory Structures
// =============================================================================

// Installed part entry inside CDBupgr (4 bytes each, array of 20)
struct CDBPartEntry {
    uint8_t part_id;    // Catalog part identifier / key
    uint8_t car_idx;    // Index of the car slot this part is fitted to (0..3 native, 0..15 extended)
    uint8_t condition;  // Part condition / wear percentage (100 = new, 0 = broken)
    uint8_t category;   // 0 = Engine, 1 = Suspension, 2 = Aero/Weight, 3 = Brakes, 4 = Exhaust
};
static_assert(sizeof(CDBPartEntry) == 4, "CDBPartEntry must be exactly 4 bytes");

// Car tuning physics adjustment parameters (0x4C = 76 bytes, 19 floats)
// Initialized by f_002DD1C8; read and passed into physics setup routines.
struct CarTuningBlock {
    float active;           // +0x00: 1.0f if record initialized / vehicle active
    float power_mult;       // +0x04: Engine peak power multiplier (f_00185018 $f20)
    float torque_mult;      // +0x08: Engine peak torque multiplier (f_00185018 $f21)
    float redline_mult;     // +0x0C: Engine max RPM / redline multiplier
    float torque_rpm_mult;  // +0x10: Torque curve peak RPM multiplier (f_00185018 $f22)
    float rpm_mult;         // +0x14: Idle / baseline RPM scaling
    float power_rpm_mult;   // +0x18: Power peak RPM multiplier
    float f_grip_mult;      // +0x1C: Front tire longitudinal & lateral grip scale
    float r_grip_mult;      // +0x20: Rear tire grip scale
    float f_downforce;      // +0x24: Front aerodynamic downforce coefficient
    float r_downforce;      // +0x28: Rear aerodynamic downforce coefficient
    float f_compliance;     // +0x2C: Front spring / damper suspension compliance
    float r_compliance;     // +0x30: Rear spring / damper suspension compliance
    float brake_mult;       // +0x34: Braking torque coefficient multiplier
    float damage_mult;      // +0x38: Chassis rigidity / structural damage mitigation
    float unk_3C;           // +0x3C: Tuning offset parameter (default 1.0f)
    float unk_40;           // +0x40: Tuning offset parameter (default 1.0f)
    float unk_44;           // +0x44: Tuning offset parameter (default 1.0f)
    float condition;        // +0x48: Overall vehicle mechanical health (1.0f = pristine)
};
static_assert(sizeof(CarTuningBlock) == 0x4C, "CarTuningBlock must be exactly 0x4C (76) bytes");

// Body panel and mesh vertex deformation state (0x2AC = 684 bytes)
// Initialized by f_002DD448; stores mesh vertex displacements and panel detachment bits.
struct CarDamagePanelsState {
    uint8_t vertex_deformation[0x290]; // 656 deformation bytes (neutral reference bias 0x80)
    uint32_t detached_panels_mask[7];  // Bitmasks for detachable body panels (doors, hood, trunk, bumpers)
};
static_assert(sizeof(CarDamagePanelsState) == 0x2AC, "CarDamagePanelsState must be exactly 0x2AC (684) bytes");

// Garage vehicle slot in Career save memory (0x300 = 768 bytes)
struct CDBGarageSlot {
    uint32_t vehicle_id;            // +0x00: Vehicle identifier (e.g., 65 = Hornet starter)
    uint32_t flags;                 // +0x04: Ownership / lock flags (bit 0 = owned)
    CarTuningBlock tuning;          // +0x08: Tuning and performance multipliers (0x4C bytes)
    CarDamagePanelsState damage;    // +0x54: Mesh deformation and panel loss state (0x2AC bytes)
};
static_assert(sizeof(CDBGarageSlot) == 0x300, "CDBGarageSlot must be exactly 0x300 (768) bytes");
static_assert(offsetof(CDBGarageSlot, tuning) == 0x08, "CDBGarageSlot::tuning offset mismatch");
static_assert(offsetof(CDBGarageSlot, damage) == 0x54, "CDBGarageSlot::damage offset mismatch");

// =============================================================================
// CDBupgr Career Upgrade State (*0x003D9398, size 0xC68 = 3176 bytes)
// =============================================================================
struct CDBupgr {
    uint32_t version;               // +0x00: Structure format version (3)
    int32_t  money;                 // +0x04: Player bank balance ($75 default starter cash)
    uint32_t total_earned;          // +0x08: Cumulative career winnings
    uint32_t car_count;             // +0x0C: Number of cars currently owned (0..4 native)
    uint32_t part_count;            // +0x10: Total installed parts count (0..20 native)
    uint32_t active_car_idx;        // +0x14: Currently selected / driven garage slot (0..3)
    CDBPartEntry parts[20];         // +0x18: Installed parts inventory (20 * 4 = 80 bytes)
    CDBGarageSlot cars[4];          // +0x68: Native garage slots (4 * 0x300 = 3072 bytes)
};
static_assert(sizeof(CDBupgr) == 0xC68, "CDBupgr must be exactly 0xC68 (3176) bytes");
static_assert(offsetof(CDBupgr, parts) == 0x18, "CDBupgr::parts offset mismatch");
static_assert(offsetof(CDBupgr, cars) == 0x68, "CDBupgr::cars offset mismatch");

// Companion Extended Garage Slot Storage (slots 4..15 for up to 16-car garage)
// Preserves native 4-slot save backwards compatibility by writing extended slots to eod_garage_ext.dat
struct CDBGarageExtSave {
    uint32_t magic;                 // 0x47415258 "GARX"
    uint32_t version;               // 1
    uint32_t extended_count;        // Count of cars in slots 4..15 (0..12)
    uint32_t active_car_idx;        // Global active car index (0..15)
    CDBPartEntry ext_parts[60];     // Extra part slots for cars 4..15
    CDBGarageSlot ext_cars[12];     // Extended garage slots 4..15 (12 * 0x300 = 3600 bytes)
};

// =============================================================================
// CareerSaveInfo Root State (*0x003D9384, size 0xE7C = 3708 bytes)
// =============================================================================
struct CDBResSaveInfo {
    int32_t  rank;                  // +0x000: Rank (0 = champion, 1 = 1st, 99 = 100th)
    uint32_t open_eve_count;        // +0x004: Number of active open Eves
    uint32_t open_eves[4];          // +0x008: Eve IDs currently displayed in UI
    uint8_t  padding[0x100];        // +0x018..0x117 (256 bytes)
    uint8_t  eve_results[40];       // +0x118: Per-Eve completion status (0xFF = unplayed)
    uint8_t  reserved[0x54];        // +0x140..0x193 (84 bytes)
};
static_assert(sizeof(CDBResSaveInfo) == 0x194, "CDBResSaveInfo must be 0x194 (404) bytes");

struct CareerSaveInfo {
    uint32_t magic;                 // +0x000: 0x43726572 "Crer"
    uint8_t  driver_meta[0x7C];     // +0x004: Driver name, face ID, trophies, dates
    CDBupgr  upgrades;              // +0x080: CDBupgr state (0xC68 bytes)
    CDBResSaveInfo results;         // +0xCE8: Event & championship progression (0x194 bytes)
};
static_assert(sizeof(CareerSaveInfo) == 0xE7C, "CareerSaveInfo must be exactly 0xE7C (3708) bytes");
static_assert(offsetof(CareerSaveInfo, upgrades) == 0x80, "CareerSaveInfo::upgrades offset mismatch");
static_assert(offsetof(CareerSaveInfo, results) == 0xCE8, "CareerSaveInfo::results offset mismatch");

// =============================================================================
// Catalog & Performance Models
// =============================================================================

// In-memory catalog part record (from catalog.val, stride 0x78 = 120 bytes)
struct CatalogPart {
    uint32_t part_key;          // +0x00: Unique part key (0x01..0xFC)
    uint32_t name_ptr;          // +0x04: String pointer to part display name
    uint32_t price;             // +0x08: Retail purchase price in dollars
    uint32_t car_class_mask;    // +0x0C: Bitmask of eligible vehicle classes (bit 0=Compact, 1=Midsize, 2=Muscle, 3=Special)
    uint32_t stage;             // +0x10: Upgrade stage level (1, 2, 3, 4, 5)
    uint32_t condition_rating;  // +0x14: Base durability rating
    uint32_t tier;              // +0x18: Wealth / unlock tier
    uint32_t category;          // +0x1C: Category index (0=Engine, 1=Suspension, 2=Aero, 3=Brakes, 4=Exhaust)
    float    mass_delta;        // +0x20: Weight change in lbs (negative for weight reduction)
    float    power_mult;        // +0x24: Horsepower multiplier
    float    redline_mult;      // +0x28: Redline RPM multiplier
    float    torque_rpm_mult;   // +0x2C: Peak torque RPM multiplier
    float    rpm_mult;          // +0x30: Idle RPM multiplier
    float    power_rpm_mult;    // +0x34: Peak power RPM multiplier
    float    f_grip_mult;       // +0x38: Front tire grip multiplier
    float    r_grip_mult;       // +0x3C: Rear tire grip multiplier
    float    f_downforce;       // +0x40: Front downforce delta
    float    r_downforce;       // +0x44: Rear downforce delta
    float    f_compliance;      // +0x48: Front suspension compliance delta
    float    r_compliance;      // +0x4C: Rear suspension compliance delta
    float    brake_mult;        // +0x50: Braking torque multiplier
    float    damage_mult;       // +0x54: Chassis damage resistance multiplier
    float    breakdown_chance;  // +0x58: Reliability / failure rate under heavy load
    uint8_t  reserved[0x1C];    // +0x5C: Reserved / alignment
};
static_assert(sizeof(CatalogPart) == 0x78, "CatalogPart must be exactly 0x78 (120) bytes");

// Car ratings record looked up by f_0012C898 (20 bytes)
struct CarStatsEntry {
    uint32_t chassis_name_ptr;  // +0x00: Pointer to chassis name string (e.g., "Hornet")
    int32_t  speed;             // +0x04: Speed rating bar value (1..10)
    int32_t  handling;          // +0x08: Handling rating bar value (1..10)
    int32_t  durability;        // +0x0C: Durability rating bar value (1..10)
    int32_t  mass;              // +0x10: Curb weight in lbs
};
static_assert(sizeof(CarStatsEntry) == 0x14, "CarStatsEntry must be exactly 0x14 (20) bytes");

#pragma pack(pop)

// =============================================================================
// Decompiled Career Upgrades & Performance API
// =============================================================================

// High-level stat calculation result
struct CalculatedVehicleStats {
    int speed_rating;       // Display rating 1..10
    int handling_rating;    // Display rating 1..10
    int durability_rating;  // Display rating 1..10
    float horsepower;       // Calculated brake horsepower (BHP)
    float peak_torque;      // Peak torque in ft-lbs
    float redline_rpm;      // Max engine RPM
    float top_speed_mph;    // Theoretical top speed in MPH
    float curb_weight_lbs;  // Total vehicle mass in pounds
    float quarter_mile_sec; // Estimated 1/4 mile elapsed time
};

// Initializes upgrade system and extended slots
void CareerUpgrades_Init();

// Lookup vehicle baseline stats by chassis name (Decompiled f_0012C898)
const CarStatsEntry* CarStatsLookup(const char* chassis_name);

// Computes current display stats with installed parts (Decompiled f_002E2708)
void AutoShopGetVehicleStats(uint32_t car_idx, CarStatsEntry* out_stats);

// Computes full physics performance curve including HP and Torque spline (Decompiled f_00185018)
CalculatedVehicleStats CalculateFullVehiclePerformance(uint32_t car_idx);

// Part inspection, query, installation, and removal
bool CDBupgr_HasPart(uint32_t car_idx, uint32_t category);
int32_t CDBupgr_GetCarPart(uint32_t car_idx, uint32_t category, uint32_t* out_part_key);
bool CDBupgr_AddPart(uint32_t car_idx, uint32_t category, uint32_t part_key, uint8_t condition);
bool CDBupgr_RemovePart(uint32_t car_idx, uint32_t category);

// Valuation & depreciation calculations
uint32_t CareerCalcTradeInDepreciation(uint32_t car_price, uint32_t parts_value, float condition);
uint32_t CareerGetOwnedCarResaleValue(uint32_t car_price);
uint32_t CareerGetCarInstalledPartsResaleValue(uint32_t car_idx);
uint32_t CareerGetTotalCarTradeInPayout(uint32_t car_idx);

// =============================================================================
// Constraint Removal & Deep Modding Hooks
// =============================================================================

// Class Unlock: returns true if part is compatible with chassis (removes class locks when enabled)
bool AutoShopIsPartCompatible(uint32_t chassis_class, uint32_t part_key);

// Multi-Stage Part Purchase & Swap: supports stages 1..5, nitrous, supercharger, weight reduction
// Automatically refunds resale value of lower-stage part when upgrading!
int32_t AutoShopPurchasePartWithMultiStage(uint32_t car_idx, uint32_t part_key);

// Expanded Garage Manager: expands garage from 4 to 16 slots with UI pagination
uint32_t Garage_GetCapacity();
uint32_t Garage_GetCarCount();
uint32_t Garage_GetActiveCarIndex();
void Garage_SetActiveCarIndex(uint32_t slot_idx);
CDBGarageSlot* Garage_GetSlot(uint32_t slot_idx);
bool Garage_AddCar(uint32_t vehicle_id);
bool Garage_RemoveCar(uint32_t slot_idx);

// Garage UI Pagination (4 slots per page, up to 4 pages = 16 cars)
int Garage_GetActivePage();
int Garage_GetTotalPages();
void Garage_CyclePage(int delta);
uint32_t Garage_GetSlotOnCurrentPage(int ui_button_idx);

// Persistence: Saves slots 0..3 to native save and slots 4..15 to eod_garage_ext.dat
bool Garage_SaveExtendedSlots(const std::string& save_dir);
bool Garage_LoadExtendedSlots(const std::string& save_dir);

} // namespace eod
