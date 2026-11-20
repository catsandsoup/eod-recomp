# Modernized UI & Menu Engine Architecture (eod-recomp)

This document details the reverse engineering, decompilation, binary structures, and modernization of the UI and Menu Engine for *Test Drive: Eve of Destruction* (`SLUS_209.10`), eliminating legacy PlayStation 2 hardware constraints and delivering high-performance, modern racing QOL capabilities.

---

## 1. Disassembled Core Menu Functions & Dispatch Mechanisms

### 1.1 `0x00270C10`: `MenuRun` (`UIDoMappedDialog`)
`MenuRun` is the central modal dialog runner for all non-HUD menus in the game.

- **Guest Signature:**
  ```c
  int32_t MenuRun(const DialogDescriptor *desc, int32_t x_pos, int32_t y_pos, int32_t flags);
  ```
- **Arguments:**
  - `a0` (`desc`): Pointer to dialog definition struct (20 bytes). Offset `+0x0` contains the layout file name (e.g. `"s_and_m.lyt"`, `"buy_car.lyt"`, `"track1.lyt"`), and offset `+0xC` receives the instantiated window pointer (`PS2DialogWindow*`).
  - `a1` (`x_pos`): Horizontal screen position. When set to `-999` (`0xFFFFFC19`), the engine centers the dialog horizontally.
  - `a2` (`y_pos`): Vertical screen position. When set to `-999` (`0xFFFFFC19`), the engine centers the dialog vertically.
  - `a3` (`flags`): Modal presentation flags (typically `1` or `3`).
- **Execution Flow:**
  1. `f_0012EA98()` marks heap watermark for memory integrity validation.
  2. `f_00270430` parses the layout descriptor, invokes `f_00270F10` to deserialize the `.lyt` asset, compiles widget tree, and attaches perspective cameras (`f_0034E530`).
  3. `f_00278F98` sets the tick dispatcher (`0x0026AA70`).
  4. `f_002789B8` -> `f_0027C508`: **Main Event Dispatch Loop**:
     - Polls pad state via `f_0029E688`.
     - Invokes dialog-specific update callback at `dialog->m_update_cb` (offset `+0x434` / `1076`).
     - Dispatches widget input events via `f_0027BB68`:
       - Jump table at `0x004111B0` routes directional events:
         - `0x0027F630`: Navigate Up
         - `0x0027F518`: Navigate Down
         - `0x0027F680`: Navigate Left
         - `0x0027F568`: Navigate Right
         - `0x0027F438`: Update focus / selection
         - `0x0027F408`: Confirm (Cross / Enter) -> invokes `widget->vtable[19]` (`on_confirm`), setting `0x004A1AC0 = 1` and `0x004A1AC4 = widget->result_id`.
         - `0x0027F3D8`: Cancel (Triangle / Escape) -> invokes `widget->vtable[20]` (`on_cancel`), setting `0x004A1AC0 = 1` and `0x004A1AC4 = -1`.
     - Draws UI primitives and synchronizes VBlank via `f_002718F8`.
     - Checks exit flag at `0x004A1AC0`: when non-zero, executes transition-out fade/slide and unwinds.
  5. `f_0027B8F8` and `f_0026D568` deallocate transient widgets and texture handles.
  6. Returns the selected item action ID stored at `0x004A1AC4`.

### 1.2 `0x003698E8`: `HomebaseBuildMenu`
Builds the action lists and display text for the Career Homebase hub, AutoShop, Garage, and Challenger screens.
- **Buffer Layout (`a0`):**
  - `+0x00`: Active menu item count.
  - `+0x04`: Array of integer action IDs (1 = Garage, 2 = AutoShop, 3 = Upgrades, 4 = Jump To / Out of Town, 5 = Challengers).
  - `+0x7C`: Total labels count.
  - `+0x80`: Array of label string pointers resolved via string table (`f_00128AF0`).
- **Legacy Constraint:** The function features hardcoded loop bounds that truncate the menu at 4 or 5 items, preventing dynamic mod items or additional hub facilities without binary patching.

### 1.3 `0x002A1298`: `MainMenuUpdate`
Top-level state machine driving the main menu.
- State variable at `*0x003D746C`:
  - `0`: Attract mode / splash video playback.
  - `1`: Interactive main menu (`arcade`, `career`, `load`, `options`, `multiplayer`).
  - `2`: Career mode dispatcher (`0x002D58E8`).
  - `3`: Action / Arcade single-player/multi-player mode (`0x002C0BB8`).
  - `4`: Multiplayer splitscreen lobby.
  - `5`: Options & Audio settings.

### 1.4 `0x002C0BB8`: `ArcadeSingleOrMultiPromptScreen`
Handles the initial entry into Action/Arcade mode.
- Presents `s_and_m.lyt` via `MenuRun`.
- Return codes: `1` = Single Player, `4` = Multiplayer, `-1` = Cancel / Return to Main Menu.
- From `arcade.lyt`, the race flow enters the native car and track menus described below.

### 1.5 Arcade vehicle selection
- Single-player uses `carchoose.lyt` at `0x002A4FC8`. Head-to-head uses a separate CarChooseWidget implementation with the same label layout.
- The Champion challenge vehicle prompt separately calls MenuRun at `0x002CB228` with `dd_chsr.lyt` and options `opt1` through `opt4`.

### 1.6 `0x002C7340`: Track Selection (`track1.lyt`, `track23.lyt`, `track4.lyt`)
- Dynamically allocates two linear arrays: `ai_vals` and `ai_labels` via `f_0012E7D0`.
- All tracks are rendered in a single un-categorized vertical list, requiring excessive scrolling to locate specific event formats (Figure 8, Derby, Jump Cross).

#### Arcade mod badges

In SLUS_209.10, `0x002C7AC4` is the call to MenuRun inside the track screen at
`0x002C7340`. The same Eve roster builder `0x002C71B0` serves this screen and
the `eves.lyt` and `mpeves.lyt` screens. It creates a separate 80-byte display
buffer for each Eve, with parallel arrays of display pointers and Eve indices.

| MenuRun return address | Labels in caller frame | Eve indices | Row count |
|---|---|---|---|
| `0x002C7ACC` | `sp+0x24` | `sp+0xC8` | `sp+0x16C` |
| `0x002C8304` | `sp+0x10` | `sp+0xC0` | `sp+0x374` |
| `0x002C8A20` | `sp+0x20` | `sp+0xD0` | `sp+0x264` |

Eve records start at `0x003C4D08`, stride `0x84`. The load key is the internal
track at `+0x08` (for example, Afton for Woodly). The display name is at
`+0x28`. The badge goes on the temporary display buffer while MenuRun executes,
after the retail default-row name lookup. Return values and Eve records stay
available to the game by their original identity.

The single-player vehicle chooser is `carchoose.lyt` (`0x002A4FC8`), and
multiplayer has its own copy of CarChooseWidget. Both call the label setter
`0x002A8EF0`. At paint-label call sites, return addresses `0x002A7FF4` and
`0x002B9EAC`, the widget in `s3` has the chassis key at `+0x49` and a
display-only paint label at `+0x6A`. The chosen paint at `+0x24` indexes
eight-byte paint records at `s4+4`, whose first field is the vehicle number.
The badge follows the paint label, leaving the chassis string available for
stats and asset lookup.

The process reads the selected `EOD_MOD_DIR` against retail. The badge checks
effective .TRK and .CAR member contents, the track's own `<track>_eves.val`,
the selected paint's numbered CT.RES and UICAR.RES entries, and the matching
asset IDs from the validated HOSTTEX index. Repacking an archive with identical
members does not badge it. HOSTTEX files are validated from their headers and
sizes without loading mip data for attribution.

---

## 2. Menu Widget & Dialog Data Structures

### 2.1 `PS2Widget` Layout (Binary Exact)

| Offset | Type | Field Name | Description |
|---|---|---|---|
| `+0x00` | `uint32_t` | `tag_ptr` | Pointer to widget identifier string (e.g. `"opt1"`, `"speed_bar"`, `"tablist"`) |
| `+0x04` | `uint32_t` | `flags` | State flags: `0`=normal, `1`=disabled, `2`=hidden, `3`=interactive/bar, `4`=focused |
| `+0x08` | `uint32_t` | `label_ptr` | Display string pointer (or in-place buffer) |
| `+0x0C` | `int32_t` | `result_id` | Command ID returned to `MenuRun` on confirmation |
| `+0x10` | `uint32_t` | `initial_state` | Toggle/checkbox state, sub-index, or default value |
| `+0x14..+0x20` | `uint32_t[4]` | `pad14` | Geometry coordinates and alignment bounds |
| `+0x24` | `float` | `min_input` | Minimum input range for progress/slider widgets |
| `+0x28` | `float` | `bar_value` | Current fill value / stat level (Speed, Handling, Durability) |
| `+0x2C` | `float` | `max_input` | Maximum input range |
| `+0x30` | `float` | `min_screen_pos` | Screen render fill start coordinate (pixels) |
| `+0x34` | `float` | `max_screen_pos` | Screen render fill end coordinate (pixels) |
| `+0x248` | `uint32_t` | `vtable_ptr` | Virtual method table pointer (offset 584) |

#### Widget Virtual Method Table Offsets:
- `vtable + 68` (`+0x44`): `on_release(PS2Widget*)`
- `vtable + 72` (`+0x48`): `on_drag(PS2Widget*, float dx, float dy)`
- `vtable + 76` (`+0x4C`): `on_confirm(PS2Widget*)` -> Writes `*(0x004A1AC0) = 1` and `*(0x004A1AC4) = widget->result_id`.
- `vtable + 80` (`+0x50`): `on_cancel(PS2Widget*)` -> Writes `*(0x004A1AC0) = 1` and `*(0x004A1AC4) = -1`.
- `vtable + 84` (`+0x54`): `on_focus(PS2Widget*)`
- `vtable + 88` (`+0x58`): `on_unfocus(PS2Widget*)`
- `vtable + 100` (`+0x64`): `on_navigate(PS2Widget*, int direction)`

### 2.2 `PS2DialogWindow` Layout

| Offset | Type | Field Name | Description |
|---|---|---|---|
| `+0x00` | `uint32_t` | `window_id` | Resource ID / handle |
| `+0x30` | `uint32_t[128]`| `widgets` | Array of child widget pointers in layout |
| `+0x434`| `uint32_t` | `update_callback` | Per-frame callback function pointer (offset 1076) |
| `+0x448`| `uint32_t` | `focused_widget` | Currently focused interactive widget (offset 1096) |
| `+0x44C`| `uint32_t` | `active_widget` | Currently dragged/active widget (offset 1100) |
| `+0x450`| `uint32_t` | `default_widget` | Default initial widget (offset 1104) |
| `+0x454`| `int32_t` | `origin_x` | Screen X origin offset (offset 1108) |
| `+0x458`| `int32_t` | `origin_y` | Screen Y origin offset (offset 1112) |
| `+0x4F4`| `int32_t` | `menu_count` | Total selectable menu items |
| `+0x4F8`| `uint32_t` | `selected_widget` | Pointer to currently selected widget |
| `+0x500`| `PS2MenuItem[32]` | `menu_items` | Array of 8-byte items: `+0` widget pointer, `+4` action ID |

- Selection Getter (`0x0027EE08`, `UIGetMenuIndex`): Compares `selected_widget` against `menu_items[i].widget`.
- Selection Setter (`0x0027D590`, `UISetMenuIndex`): Sets `selected_widget = menu_items[index].widget`.

---

## 3. Removal of PS2 Constraints & Modern QOL Architecture

### 3.1 Artificial Constraints Eliminated

1. **Fixed 5-Slot Dealer Rows & Static 4-Item Menus:**
   - *Legacy:* The PS2 dealer (`kShopSlots = 0x004AB130`) only offered 5 cars at a time. The arcade car select screen was locked to 4 cards.
   - *Modern Solution:* Implemented `ui_engine::VirtualListModel<T>`, which decouples arbitrary dataset sizes (28+ dealer chassis, 130+ vehicle variants, 40+ tracks) from the visible hardware slot count. Slices are projected live into the target memory buffers.
2. **Sluggish 0.65-Second Transition Delays:**
   - *Legacy:* `f_0027C508` hardcoded `0x3F266666` (`0.65f` seconds, or ~40 VBlanks) for both transition-in and transition-out animations. Flipping pages or opening menus incurred ~1.5 seconds of locked input.
   - *Modern Solution:* `hle_menu_run_prologue()` adjusts `kAddr_TransitionInDur` to `0.05f` (`0x3D4CCCCD`) or instant (`0.0f`), providing instantaneous sub-frame responsiveness.
3. **Absence of Vehicle Class & Drivetrain Filtering:**
   - *Legacy:* The game engine supported 4 distinct classes (`Compact`, `Midsize`, `Muscle`, `Special`) and multiple drivetrains (`FWD`, `RWD`, `AWD`), but offered no menu UI to filter them.
   - *Modern Solution:* `ui_engine::DealerManager` and `ui_engine::ArcadeVehicleManager` provide instant multi-criteria filtering and sorting (Price, Speed, Durability, Name). Filter tabs cycle with shoulder buttons (`PAD_L1` / `PAD_R1` or Tab).
4. **Single-Column Uncategorized Track List:**
   - *Legacy:* All tracks were presented in one flat vertical list.
   - *Modern Solution:* `ui_engine::TrackSelectManager` categorizes tracks into `Oval`, `Figure 8`, `Demolition Derby`, `Jump Cross`, and `Point-to-Point`.

### 3.2 Turbo Navigation & Repeat Timing

The modern `TurboNavigator` engine eliminates sluggish PS2 pad read delays:
- **Initial Press:** Instant zero-latency response on frame 0.
- **Initial Repeat Delay:** 180 ms (reduced from PS2's 400 ms).
- **Repeat Rate:** 50 ms (20 items/sec, up from PS2's 300 ms).
- **Shoulder Buttons / Triggers:**
  - `L1` / `R1` (or Tab / Shift-Tab): Cycle Vehicle Class tabs.
  - `L2` / `R2` (or Q / E): Jump whole pages or cycle Drivetrain filter.

---

## 4. Decompiled Models & HLE Integration Architecture

The implementation is packaged into clean, standalone C++ modules ready for direct linking:
- **Header:** [`src/ui_menu_engine.h`](file:///C:$HOME/Documents/GitHub/eod-recomp/src/ui_menu_engine.h)
- **Source:** [`src/ui_menu_engine.cpp`](file:///C:$HOME/Documents/GitHub/eod-recomp/src/ui_menu_engine.cpp)
- **Unit Test:** [`tests/ui_menu_engine_test.cpp`](file:///C:$HOME/Documents/GitHub/eod-recomp/tests/ui_menu_engine_test.cpp)

### 4.1 Verification & Test Evidence
All unit tests compile and run cleanly with CTest:
```bash
ctest --test-dir build -R "UIMenuEngine" -V
```
**Test Results:**
- `test_struct_offsets`: Verified exact PS2 binary offsets for `PS2Widget` (tags, flags, labels, result IDs, float ranges, vtable at +0x248) and `PS2DialogWindow`.
- `test_virtual_list_pagination`: Verified smooth sliding window, page boundaries, page flipping, empty slot padding, and bidirectional wrap-around.
- `test_vehicle_filtering_and_sorting`: Verified filtering across Compact, Midsize, Muscle, Special, NASCAR, and drivetrains (FWD, RWD, AWD), as well as multi-criteria sorting.
- `test_turbo_navigation`: Verified initial press latency, 180ms hold gate, 50ms turbo repeat, and shoulder button tab cycling.
- Full project test suite passed: **51 of 51 tests passing (100%)**.
