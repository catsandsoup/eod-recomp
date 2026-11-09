# Career Vehicle Upgrades, Garage, and Tuning System Decompilation

## Executive Summary
This document details the complete reverse-engineering and decompilation of the Career vehicle upgrade, garage, and tuning systems for *Test Drive: Eve of Destruction* (`eod-recomp`). It documents native MIPS EE data structures (`CDBupgr`, `CDBGarageSlot`, `CarTuningBlock`, `CarDamagePanelsState`, `CatalogPart`, `CarStatsEntry`), exact valuation and depreciation algorithms, vehicle physics stat conversion curves, identification of artificial game constraints, and modern C++ implementations that remove those limits to empower deep automotive modding.

---

## 1. Disassembly & Architecture Analysis

### 1.1 Memory Map & Global State Pointers
All addresses correspond to EE RAM in `SLUS_209.10`:

| Address | Size / Stride | Symbol / Structure | Description |
|---|---|---|---|
| `*(0x003D9384)` | `0xE7C` (3708 B) | `CareerSaveInfo` | Root Career save state in RAM (`0x43726572` `'Crer'`) |
| `*(0x003D9394)` | `0x194` (404 B) | `CDBResSaveInfo` | Career event rank (`+0x00`), active Eves (`+0x08`), results table (`+0x118`) |
| `*(0x003D9398)` | `0xC68` (3176 B) | `CDBupgr` | Career vehicle upgrades, player money (`+0x04`), 4 garage slots (`+0x68`) |
| `*(0x003D9374)` | `0x78` (120 B) | `CatalogPart[]` | In-memory catalog of all upgrade parts (`catalog.val`) |
| `0x004AAF88` | 4 bytes / class | `kCatalogClassMask[]` | Bitmask table of compatible parts for each chassis class |
| `*(0x00439B20)` | `0x14` (20 B) | `CarStatsEntry[]` | Base vehicle rating table (Speed, Handling, Durability, Mass) |
| `*(0x003CBF40)` | 4 bytes | `kCarStatsCount` | Number of entries in `CarStatsEntry` table |
| `0x004AB0E0` | `0x14` (20 B) | `OwnedCarRecord[]` | List of owned car records (`+0x00` idx, `+0x08` price paid, `+0x0C` chassis name) |
| `0x0041EFF8` | 12 bytes | `kRankGates[]` | Rank gate table `{79, 49, 0}` for shops and challenges |

---

### 1.2 Core Upgrade & AutoShop Functions (0x002E0000 - 0x002E6FFF)

#### `f_002E14A0` - `CDBUpgrades_InitDefaults`
- Allocates or resets `0xC68` bytes for `CDBupgr`.
- Sets version `3` at offset `+0x00`.
- Initializes starter player cash `$75` at offset `+0x04`.
- Clears car count (`+0x0C` = 0) and part count (`+0x10` = 0).
- Sets active vehicle slot (`+0x14` = 0).

#### `f_002E1870` - `CDBupgr_GetCarPart`
- Signature: `int32_t CDBupgr_GetCarPart(uint32_t car_idx, uint32_t category, uint32_t* out_part_key)`
- Scans `CDBupgr->parts[20]` (offset `+0x18`) for entry matching `entry->car_idx == car_idx` and `entry->category == category`.
- If found: writes `entry->part_id` to `*out_part_key` and returns 1.
- If not found: returns 0.

#### `f_002E1960` - `CDBupgr_AddPart`
- Signature: `bool CDBupgr_AddPart(uint32_t car_idx, uint32_t category, uint32_t part_key, uint8_t condition)`
- Reads `CDBupgr->part_count` (`+0x10`). If `part_count >= 20`, rejects.
- Writes 4-byte `CDBPartEntry` at `parts[part_count]`:
  - `byte[0] = part_key`
  - `byte[1] = car_idx`
  - `byte[2] = condition`
  - `byte[3] = category`
- Increments `CDBupgr->part_count`.

#### `f_002E18E0` - `CDBupgr_RemovePart`
- Signature: `bool CDBupgr_RemovePart(uint32_t car_idx, uint32_t category)`
- Scans `parts[20]`. When matching entry `i` is found, shifts all elements `[i+1 .. count-1]` down by 1.
- Decrements `part_count`.

#### `f_002E1BC0` - `CDBupgr_AddCar`
- Signature: `bool CDBupgr_AddCar(uint32_t vehicle_id)`
- Multiplies `car_count * 0x300` and adds base offset `+0x68`.
- Writes `vehicle_id` at `slot + 0x00`, flags `1` at `slot + 0x04`.
- Calls `f_002DD1C8(slot + 0x08)` to initialize `CarTuningBlock` (sets all multipliers to `1.0f`).
- Calls `f_002DD448(slot + 0x54)` to initialize `CarDamagePanelsState` (sets mesh neutral bias `0x80`).
- Writes `1.0f` to `slot + 0x70` (tuning active flag).
- Increments `CDBupgr->car_count`.

#### `f_002E1380` - `CDBupgr_GetCarTuningBlock`
- Signature: `void CDBupgr_GetCarTuningBlock(uint32_t car_idx, void* out_tuning_block)`
- Validates `car_idx < CDBupgr->car_count`.
- Copies `0x4C` bytes from `slot[car_idx] + 0x08` to destination buffer.
- Panics if car index is not found: `"CDBUpgr: Error in algorithm/assertions. couldn't find any car %d"`.

#### `f_0012C898` - `CarStatsLookup`
- Scans `CarStatsEntry` array at `*(0x00439B20)` of count `*(0x003CBF40)`.
- Compares `entry->chassis_name` with input string using `f_003A72D8` (`strcmp`).
- Returns pointer to `CarStatsEntry` (20 bytes: name, speed, handling, durability, mass).
- Panics if missing: `"CarStatsLookup: no such car as %s, AFAIK"`.

#### `f_002E2708` - `AutoShopGetVehicleStats`
- Queries `CarStatsLookup` for chassis base ratings.
- Inspects installed parts via `f_002E1870`:
  - Category 0 (Engine) installed: `speed += 1`
  - Category 1 (Suspension) installed: `handling += 1`
  - Category 4 (Exhaust) installed: `durability += 1`

---

## 2. Reverse Engineered Data Structures

### 2.1 Bit-Exact C++ Struct Definitions

```cpp
#pragma pack(push, 1)

// Installed part entry inside CDBupgr (4 bytes each)
struct CDBPartEntry {
    uint8_t part_id;    // Catalog part identifier / key
    uint8_t car_idx;    // Garage slot index (0..3 native, 0..15 extended)
    uint8_t condition;  // Part wear percentage (100 = brand new, 0 = broken)
    uint8_t category;   // 0 = Engine, 1 = Suspension, 2 = Aero, 3 = Brakes, 4 = Exhaust
};
static_assert(sizeof(CDBPartEntry) == 4, "CDBPartEntry must be exactly 4 bytes");

// Car tuning physics adjustment parameters (0x4C = 76 bytes, 19 floats)
struct CarTuningBlock {
    float active;           // +0x00: 1.0f if initialized / active
    float power_mult;       // +0x04: Engine peak power multiplier (f_00185018 $f20)
    float torque_mult;      // +0x08: Engine peak torque multiplier (f_00185018 $f21)
    float redline_mult;     // +0x0C: Engine max RPM / redline multiplier
    float torque_rpm_mult;  // +0x10: Peak torque RPM multiplier (f_00185018 $f22)
    float rpm_mult;         // +0x14: Idle / baseline RPM scale
    float power_rpm_mult;   // +0x18: Peak power RPM multiplier
    float f_grip_mult;      // +0x1C: Front tire grip multiplier
    float r_grip_mult;      // +0x20: Rear tire grip multiplier
    float f_downforce;      // +0x24: Front aerodynamic downforce delta
    float r_downforce;      // +0x28: Rear aerodynamic downforce delta
    float f_compliance;     // +0x2C: Front suspension compliance delta
    float r_compliance;     // +0x30: Rear suspension compliance delta
    float brake_mult;       // +0x34: Braking torque multiplier
    float damage_mult;      // +0x38: Chassis rigidity / damage resistance
    float unk_3C;           // +0x3C: Tuning offset parameter (default 1.0f)
    float unk_40;           // +0x40: Tuning offset parameter (default 1.0f)
    float unk_44;           // +0x44: Tuning offset parameter (default 1.0f)
    float condition;        // +0x48: Overall vehicle health (1.0f = pristine)
};
static_assert(sizeof(CarTuningBlock) == 0x4C, "CarTuningBlock must be exactly 0x4C bytes");

// Mesh deformation and detached body panels state (0x2AC = 684 bytes)
struct CarDamagePanelsState {
    uint8_t vertex_deformation[0x290]; // 656 displacement bytes (neutral bias 0x80)
    uint32_t detached_panels_mask[7];  // Bitmasks for detached doors, hood, bumpers
};
static_assert(sizeof(CarDamagePanelsState) == 0x2AC, "CarDamagePanelsState must be 0x2AC bytes");

// Garage vehicle slot in Career save memory (0x300 = 768 bytes)
struct CDBGarageSlot {
    uint32_t vehicle_id;            // +0x00: Vehicle identifier (e.g. 65 = Hornet)
    uint32_t flags;                 // +0x04: Bit 0 = owned
    CarTuningBlock tuning;          // +0x08: Tuning multipliers (0x4C bytes)
    CarDamagePanelsState damage;    // +0x54: Deformation state (0x2AC bytes)
};
static_assert(sizeof(CDBGarageSlot) == 0x300, "CDBGarageSlot must be exactly 0x300 bytes");

// CDBupgr Structure (*0x003D9398, size 0xC68 = 3176 bytes)
struct CDBupgr {
    uint32_t version;               // +0x00: Version (3)
    int32_t  money;                 // +0x04: Player bank balance ($75 starter)
    uint32_t total_earned;          // +0x08: Cumulative career earnings
    uint32_t car_count;             // +0x0C: Number of cars owned (0..4 native)
    uint32_t part_count;            // +0x10: Installed parts count (0..20 native)
    uint32_t active_car_idx;        // +0x14: Selected garage slot (0..3 native)
    CDBPartEntry parts[20];         // +0x18: Installed parts (80 bytes)
    CDBGarageSlot cars[4];          // +0x68: Native garage slots (4 * 0x300 = 3072 bytes)
};
static_assert(sizeof(CDBupgr) == 0xC68, "CDBupgr must be exactly 0xC68 bytes");

// CareerSaveInfo Root State (*0x003D9384, size 0xE7C = 3708 bytes)
struct CDBResSaveInfo {
    int32_t  rank;                  // +0x000: Rank (0 = champion, 1 = 1st, 99 = 100th)
    uint32_t open_eve_count;        // +0x004: Number of active open Eves
    uint32_t open_eves[4];          // +0x008: Open Eve IDs shown in UI
    uint8_t  padding[0x100];        // +0x018..0x117 (256 bytes)
    uint8_t  eve_results[40];       // +0x118: Per-Eve status (0xFF = unplayed)
    uint8_t  reserved[0x54];        // +0x140..0x193 (84 bytes)
};
static_assert(sizeof(CDBResSaveInfo) == 0x194, "CDBResSaveInfo must be 0x194 bytes");

struct CareerSaveInfo {
    uint32_t magic;                 // +0x000: 0x43726572 "Crer"
    uint8_t  driver_meta[0x7C];     // +0x004: Driver metadata
    CDBupgr  upgrades;              // +0x080: CDBupgr (0xC68 bytes)
    CDBResSaveInfo results;         // +0xCE8: Career progression (0x194 bytes)
};
static_assert(sizeof(CareerSaveInfo) == 0xE7C, "CareerSaveInfo must be exactly 0xE7C bytes");

#pragma pack(pop)
```

---

## 3. Valuation & Economic Models

Native disassembly of `f_002E3BC8`, `f_002E3D20`, `f_002E3E70`, and `f_002E3F20` reveals the exact financial formula used when trading in or selling vehicles:

1. **Chassis Resale (`f_002E3E70`)**:
   $$\text{ChassisResale} = \lfloor 0.90 \times \text{PricePaid} \rfloor$$
2. **Installed Parts Resale (`f_002E3D20`)**:
   $$\text{PartsResale} = \sum_{\text{installed parts}} \lfloor 0.75 \times \text{PartRetailPrice} \rfloor$$
3. **Trade-In Wear Depreciation (`f_002E3BC8`)**:
   $$\text{Depreciation} = \text{round}\big((\text{ChassisResale} + \text{PartsResale}) \times (1.0 - \text{Condition})\big)$$
4. **Total Payout (`f_002E3F20`)**:
   $$\text{TotalPayout} = (\text{ChassisResale} + \text{PartsResale}) - \text{Depreciation}$$

Under `FreeSwap`, depreciation is clamped to `0` and chassis resale pays 100% of purchase price.

---

## 4. Vehicle Physics & Stat Calculation

In `f_00185018`, baseline chassis physics parameters from `physics.val` are multiplied by `CarTuningBlock` parameters and converted into the engine torque-RPM polynomial spline.

The automotive conversion constant is hardcoded at `0x00185100`:
$$\text{lui } \$at, 0x45A4; \quad \text{ori } \$at, \$at, 0x2000 \implies 0x45A42000 = 5252.0f$$
$$\text{Horsepower} = \frac{\text{Torque} \times \text{RPM}}{5252.0}$$

The resulting engine power curves feed into:
- Aerodynamic top speed limit: $V_{\max} \approx c \cdot \sqrt[3]{\frac{\text{HP}}{C_d A}}$
- Quarter-mile elapsed time: $\text{ET} \approx 5.825 \cdot \sqrt[3]{\frac{\text{Mass}}{\text{HP}}}$

---

## 5. Constraint Removal & Deep Modding Architecture

### 5.1 Part Locks by Car Class (Universal Parts)
- **Constraint**: `f_002DFDF8` indexes `kCatalogClassMask` (0x004AAF88) by chassis class, restricting engines and suspensions to specific classes.
- **Solution**: Intercept `f_002DFDF8`. When `g_config.career_universal_parts` is active, return universal bitmask `0xFFFFFFFF`. Any engine (e.g. Muscle V8 or Stage 3 Turbo) or suspension part can now be installed on any vehicle (Compact, Postal Van, School Bus, Hearse).

### 5.2 Single-Stage Parts (Multi-Stage Tuning & Swapping)
- **Constraint**: `f_002DED18` rejects purchasing a second part in a category with error code 2 (`"Catalog: Buying second part in category... already has key!"`).
- **Solution**: Intercept `f_002DED18`. When `g_config.career_multistage_upgrades` is active:
  - If a part in that category is already installed, compute trade-in refund credit ($0.75 \times \text{OldPrice}$).
  - Automatically uninstall old part via `CDBupgr_RemovePart`.
  - Install new higher-stage part via `CDBupgr_AddPart`.
  - Deduct only the net price from player money.
  - Activates hidden categories 2 (Aero/Weight Reduction) and 3 (Brakes).

### 5.3 Garage Capacity Expansion (4 to 16 Slots)
- **Constraint**: The 4-car garage limit is hardcoded in three places:
  1. Dealer buy check: `0x00370AA8: slti $v0, $s0, 0x4` -> `"dealer: You can't own more than 4 cars"`
  2. Garage slot buttons: `0x00375410: slti $v0, $s0, 0x4`
  3. Garage model display: `0x003759FC: sltiu $v0, $s3, 0x4`
- **Solution**:
  1. Expand capacity to 16 slots with `s_extended_slots[12]`.
  2. Implement Garage UI pagination (4 pages of 4 slots) using L1/R1, L2/R2, Q/E, or Left/Right, mirroring the dealer paging mechanism.
  3. **Preserve Native Save Backwards Compatibility**: Slots 0..3 are serialized to native `CareerSaveInfo` at offset `+0x80`, while extended slots 4..15 are persisted to companion file `eod_garage_ext.dat`. A native PS2 emulator or unmodded build can still load the save without crashing!
