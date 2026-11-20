# EOD World Editor — UI Contract, FMEA Audit, Car Studio Parity Map & Halo Forge Spec

**Audit Scope:** `tools/world_editor/` (`main_window.py`, `explorer_panel.py`, `properties_panel.py`, `dialogs.py`, `home_hub.py`, `viewport_gl.py`, `pie_host.py`, `project_state.py`, `world_loader.py`), `src/` (`battle_driver.cpp`, `battle_bots.cpp`, `battle_events.cpp`, `battle_layout.h`, `session_start.cpp`), `tools/editor_macos/` (SwiftUI app), and `tools/` vehicle pipelines (`modding_backend.py`, `formats/valt_schema_rules.py`, `import_nascar_cars.py`, `nascar_roster.py`, `create_custom_car.py`, `vehicle_export.py`, `vehicle_import.py`).

**Status Summary (127 Audited Controls):**

| Classification | Count | Share | Meaning |
|---|---:|---:|---|
| **`REAL & VERIFIED`** | **85** | 66.9% | End-to-end wired; edits bake into `.TRK`/`.CAR`/`HOSTTEX`/`EOD_DIRECT_START`/`pie_live_control.ini` and are visibly/physically read by `eod.exe` (or pure viewport/UI utility working as labeled). |
| **`PARTIAL / DESYNC`** | **14** | 11.0% | Partially functional, but has a verified runtime protocol mismatch, execution-order overwrite, silent partial drop, missing undo, or visual-vs-physics desync. |
| **`JSON-ONLY / NOT READ BY GAME`** | **24** | 18.9% | Mutates Python state and writes `.eodworld.json` or `custom_race.json`, **never baked into PS2 binaries and never read by `eod.exe`**. |
| **`STUB / POTEMKIN`** | **4** | 3.1% | Dead widget, unconnected dropdown, broken live button with zero runtime effect, or static text pretending to be an interactive tool/importer. |
| **Total Distinct Controls** | **127** | **100%** | Every menu item, toolbar widget, context action, panel control, PIE bar button, and dialog control accounted for. |

---

## 1. Executive Bug Taxonomy & Anti-Potemkin Rule

Every defect uncovered in this audit falls into one of **5 deterministic engineering failure classes**:

### Class 1 — Potemkin / Stub UI (False Affordance)
- **Definition:** A visible menu item, button, dropdown, or dialog tab implies a real workflow, but has no connected signal, no execution path, or only displays static text/JSON metadata.
- **Code-Verified Examples:**
  1. `VehicleTunerDialog.cmb_car` (`dialogs.py:366-369`): `"Inspect Retail Car"` dropdown has **zero signal connected**; picking a car does nothing.
  2. `VehicleTunerDialog` `"NASCAR & Custom Cars"` tab (`dialogs.py:374-390`) & `HomeHubDialog.btn_vehicle_importer` (`home_hub.py:405-412`): Opens a read-only `QLabel` printing script existence (`available`), with zero ISO/folder picker, car selector, or execution button.
  3. All 7 `Markers` / `Cars` / `Breakers` placement actions (`main_window.py:360-377`), `Make Collision` (`project_state.py:211-239`), `Road` topology/lane buttons (`main_window.py:2188-2248`), `Environment` sun/fog/preset sliders (`properties_panel.py:639-713`), and `CustomRaceDialog` `Laps` / `AI Speed Scale` / `Event Name` (`dialogs.py:191-236`): all write to `.eodworld.json` / `custom_race.json`, **none are read by `eod.exe`**.

### Class 2 — Format-Semantics & Protocol Mismatch
- **Definition:** Editor Python code and native C++ (`eod.exe`) or PS2 binary parsers disagree on key names, execution order, data types, or file schemas, causing silent no-ops or corrupted invariants.
- **Code-Verified Examples:**
  1. **PIE Live Free Cam double failure:** `pie_host.py:600` writes `free_cam=1` to `pie_live_control.ini`, while `src/battle_driver.cpp:221` only parses `camera_view=<int>`. Worse, `camera_view=9` only sets a static retail preset (`0x3C411C` / `0x48CB90`), **not** a flyable free camera — live QA confirmed clicking **Free Cam** does nothing.
  2. **PIE AI Auto-Drive execution-order overwrite (`src/battle_driver.cpp:350-359`):** `drive_player_along_ilin()` writes steer/gas/brake (`car+0x1B6C..0x1B7C`) **after** `f_001A2D20_orig()` has already integrated physics for the tick! Unlike Battle AI bots (`NullDriver`), `player0_car` uses native `HumanDriver`, which overwrites `car+0x1B6C..0x1B7C` from the gamepad/keyboard on the very next frame *before* `f_001A2D20_orig()` consumes it — live QA confirmed **AI Auto-Drive** does nothing.
  3. **PIE Focus Controls / Win32 SDL2 input drop (`pie_host.py:875-884`):** Clicking `Focus Controls` puts Qt focus on the `QPushButton` itself, and `forward_key_event()` forwards keys to a `WS_CHILD`-reparented SDL2 window without complete Win32 scancode `lParam` / raw-input activation — live QA confirmed **Focus Controls** does nothing.
  4. **PIE Live Arcade Mode type mismatch:** `pie_host.py:611` writes string `arcade_mode=tag`, but `src/battle_driver.cpp:225` calls `std::atoi(v)` expecting `0..3` — live switching resets mode to `0` (`free_roam`). Furthermore, `cat_mouse` (`2`) has **no C++ gameplay code** in `apply_live_car_tuner_and_arcade()` (`src/battle_driver.cpp:302-344`).
  5. **Stacked Duplicate PIE Toolbars:** Main toolbar (`Free Roam`, `Play Event`, `Stop`) stays visible directly above `PieHostWidget.bar` (`Stop PIE (Shift+Esc)`, `Restart`, etc.), wasting vertical viewport space with duplicate controls.
  6. **Unapplied live tuner keys & AI clamp:** `src/battle_driver.cpp:223-224` parses `grip_boost` and `ride_height_delta` without applying them; `CustomRaceDialog.spn_ai` allows `0..15` (`dialogs.py:226`) while `session_start.cpp:498` rejects `> 12`.
  7. **ILIN `retail_index` drop & Hotkey collision:** `project_state.py:3229,3404` drops all `retail_index=None` road nodes and `lanes`/`two_way`; `Objects -> Snap Selected to Ground` steals `Ctrl+G` (`main_window.py:351`) from standard Grouping.

### Class 3 — Visual-vs-Physics Desync
- **Definition:** Visual mesh (OpenGL preview or PS2 `GRAF`/`MODL`) diverges from the collision/physics surface (`SCOL`, `BPPT`, `SOBL`, `obstacle.val`) or cross-event visibility (`c_*.va0`, `opt_terrain`).
- **Code-Verified Examples:**
  1. **`Make Collision` orphan:** `MakeCollisionCommand` draws a green wireframe box in OpenGL and sets `obj.has_custom_collision = True` (`project_state.py:211-239`), changing the button to `"Collision Ready"`, yet `export_mod_layer()` never appends `world.collision.custom_boxes` to `track.sol` (`SOBL`). Only moved/scaled retail statics with 0 SCOL hits trigger `auto_sobl_boxes` (`project_state.py:2150`).
  2. **Compound PS2 ramps split on edit:** Retail ramps like Afton's `ramp-in` + top cap + `BPPT` + `SOBL` are separate GRAF leaves/records; moving or deleting one sub-mesh leaves floating caps or ghost collision planes unless auto-grouped.
  3. **Indiscriminate `Flatten Track Barriers`:** `flatten_all_track_barriers()` (`main_window.py:2056-2074`) drops **every** vertical `SOBL` box to `-0.5m`, stripping both invisible sky-walls (`>=10m`) and legitimate waist-high concrete retaining walls alike.
  4. **Direct-color textures in HOSTTEX:** `_export_host_textures()` (`project_state.py:1211`) silently skips direct-color textures or textures missing raw `TEX` bytes, leaving viewport showing the replacement PNG while `eod.exe` renders retail pixels.

### Class 4 — Proxy-Approximation Picking
- **Definition:** Viewport ray-picking hits coarse AABBs or bounding spheres instead of actual geometry or logical compound assemblies, causing mis-clicks behind invisible boxes or accidental selection of sub-parts.
- **Code-Verified Examples:**
  1. `_pick_object()` (`viewport_gl.py:2434-2470`) properly runs AABB broadphase + exact Moller-Trumbore `ray_mesh_hit` on the top 48 candidates — **good**, except it picks individual GRAF sub-leaves rather than logical multi-part assemblies (ramps/fences/buildings).
  2. `_pick_sobl_barrier()` (`viewport_gl.py:2349-2365`) uses axis-aligned `ray_aabb_hit` even for rotated `SOBL` boxes (yawed walls pick wildly outside their visual wireframe).
  3. `_pick_marker()` (`viewport_gl.py:2417`) and `_pick_road_node()` (`viewport_gl.py:2399`) use fixed-radius spheres regardless of camera distance.

### Class 5 — Python-Loop Frame Budget & Main-Thread Stalls (`310.4 ms` Live Spike)
- **Definition:** Per-frame or per-edit Python loops over `~6,000+` `WorldObject` instances inside paint/picking/stats paths saturate the GIL and drop viewport FPS below 60 Hz.
- **Code-Verified Examples:**
  1. **Observed `310.4 ms` frame spike (`draws 125` in live QA screenshot):** `viewport_gl.py:1435` (`_rebuild_scene_batches`) loops over all `6,000+` `world.objects` in pure Python on the main GL thread, executing per-object matrix transforms (`verts @ rot3.T + t3`), list appends, `np.concatenate`, and full `glBufferData` re-uploads whenever `mark_scene_dirty()` fires (even for single-object drags or toggling `Sync All Events`!). **Target budget:** `< 16.6 ms` steady-state, `< 4 ms` incremental object transform via GPU model-matrix / instanced dynamic bucket separation (keep static `terrain` VBO immutable; only update the 1–50 dirty/selected objects in a dynamic VBO!).
  2. `_pick_object()` (`viewport_gl.py:2439`) loops over all `6,000+` `world.objects` in pure Python computing `obj.world_aabb(model)` per click (no vectorized NumPy AABB array or BVH).
  3. `_emit_stats()` (`viewport_gl.py:3066`) scans all `world.objects` every 120 ms just to count `not o.deleted`.
  4. Undo bypasses (`_on_prop_marker_meta_edited`, `_on_env_value_changed`) trigger immediate full `_refresh_after_edit()` / repaint cycles without coalescing.

### Strict Anti-Potemkin Engineering Rule (Binding Policy)
> **NO UI CONTROL MAY EXIST IN `tools/world_editor/` WITHOUT END-TO-END BEHAVIORAL VERIFICATION.**
>
> 1. **Binary or Runtime Proof Required:** Every authoring control that claims to affect gameplay must either (a) write validated bytes into a `.TRK`/`.CAR`/`COMMON.RES`/`HOSTTEX` file verified by a headless test (`test_editor.py`), or (b) send a verified live IPC/env parameter consumed by `src/*.cpp` in `eod.exe`.
> 2. **Explicit Scope Tagging:** Controls that are purely viewport-local (e.g., Wireframe, Lighting, Grid) must live strictly under `View` / `Camera` and never masquerade as baked track properties.
> 3. **No Silent Drops:** If a user edits a property (e.g., `Laps`, `Spawn`, `Custom Collision Box`, `Road Branch`), either implement the native baker/runtime hook OR **hide/remove the control** until the lane lands. Never write unconsumed JSON while displaying `"Saved!"`.
> 4. **CI Gate:** Every PR adding or modifying a UI action must add a corresponding assertion in `tools/world_editor/test_editor.py` proving the output artifact (`.TRK` CRC/payload, `pie_live_control.ini` key matched against C++, or `.CAR` VALT byte diff).

---

## 2. Complete UI Contract & FMEA Table (100% of Controls)

Every single menu item, toolbar button/widget, status/dock control, Explorer widget, viewport interaction, Properties Inspector control, PIE bar control, and dialog control in `tools/world_editor/` is audited below.

### 2.1 File Menu & Toolbar File Actions (`main_window.py:280-295, 445-448`)

| # | Control & Location | Current Code Reality | JTBD (User Intent) | Coupled PS2 Invariants | Forecasted Failure Modes (FMEA) | Verifiable DoD |
|---:|---|---|---|---|---|---|
| 1 | `File -> Home Hub...` | **`REAL & VERIFIED`** | Return to project launcher to switch tracks, recent projects, or tools. | None (reads `DATA/TRACKS/*.TRK` & `editor_prefs.json`). | Modal hub loses unsaved dirty edits if user switches tracks without prompt. | Prompts on dirty `QUndoStack` before discarding current track; `HomeHubDialog` returns valid action. |
| 2 | `File -> New World Project...` / Toolbar `New` (`Ctrl+N`) | **`REAL & VERIFIED`** | Start a clean mod project on a selected retail track. | Retail `.TRK` remains read-only; fresh overlay state. | Silently discards unsaved edits if no confirmation prompt on dirty stack. | Resets `objects`/`markers`/`sobl_edits`/`road_graph` to retail baseline; retail SHA-256 unchanged. |
| 3 | `File -> Open Project...` / Toolbar `Open` (`Ctrl+O`) | **`REAL & VERIFIED`** | Load a saved `.eodworld.json` overlay onto its retail base track. | Must match base `trk_rel` and `retail_instance_id` offsets. | Opening a project whose base `.TRK` is missing in `game_dir` fails silently or partially applies. | Round-trips all edits from `.eodworld.json`; displays clear error if base `.TRK` is absent. |
| 4 | `File -> Save Project` / Toolbar `Save` (`Ctrl+S`) | **`REAL & VERIFIED`** | Persist editor overlay (`.eodworld.json`) + viewport thumbnail and clear autosave. | Overlay JSON only; does not modify retail `.TRK`. | Thumbnail capture fails in headless/minimized window; partial JSON write on crash (mitigated by `atomic_write_json`). | Atomic write produces valid JSON reloadable with zero diff; clears pending autosave. |
| 5 | `File -> Save Project As...` | **`REAL & VERIFIED`** | Fork project under a new filename/slug. | Updates `project_path` and `project_name` slug for `mods/<slug>/`. | Slug collision overwrites another project's mod output directory. | Saves to chosen path, updates window title & recent projects list. |
| 6 | `File -> Write to Mod Layer (eod.exe)...` / Toolbar `Write Mod` (`Ctrl+Shift+S`) | **`PARTIAL / DESYNC`** | Compile all edits into a playable non-destructive `mods/<slug>/` overlay for `eod.exe`. | `0TSR v6` `.TRK` (2048B align + EOD CRC32), `GRAF`, `VIF`, `BPPT`, `SCOL`, `SOBL` (`track.sol`), `c_*.va0`, `obstacle.val`, `*.ild` (`ILIN`), `HOSTTEX` (`.eodhtx`), `PROFILE.TXT`. | **FMEA:** (1) Drops `custom_boxes`, `markers`, `Environment` sun/fog, new ILIN nodes/lanes, `custom_race` laps/speed; (2) Delta cache stale if user edits `custom_boxes` or `markers` once implemented; (3) Direct-color textures skipped in `HOSTTEX`. | Full + delta bake produce bit-identical `.TRK` CRC; retail `.TRK` SHA-256 unchanged; every supported edit type verified in output binary. |
| 7 | `File -> Set Game Data Directory...` | **`REAL & VERIFIED`** | Point editor at extracted EOD retail `DATA/` folder. | Expects `DATA/TRACKS/*.TRK`, `DATA/CAR/*.CAR`, `eod.exe`. | User picks `DATA/` subfolder instead of parent root, yielding 0 tracks found. | Auto-normalizes `.../DATA` to parent root; rediscovers tracks & cars. |
| 8 | `File -> Exit` (`Alt+F4`) | **`REAL & VERIFIED`** | Shut down editor cleanly and terminate any running PIE child `eod.exe`. | Must kill embedded `eod.exe` process & cancel `QThreadPool` workers. | Orphaned `eod.exe` left running in background if Win32 reparented HWND is open. | `closeEvent()` stops PIE, cancels tokens, flushes prefs; zero orphan processes. |

### 2.2 World Menu & Core PIE Launch (`main_window.py:297-313, 452-460`)

| # | Control & Location | Current Code Reality | JTBD (User Intent) | Coupled PS2 Invariants | Forecasted Failure Modes (FMEA) | Verifiable DoD |
|---:|---|---|---|---|---|---|
| 9 | `World -> Undo` (`Ctrl+Z`) | **`PARTIAL / DESYNC`** | Revert the last authoring operation cleanly. | Must restore `GRAF`, `SOBL`, `ILIN`, `HOSTTEX`, `markers` in lockstep. | **FMEA:** Marker name/kind (`_on_prop_marker_meta_edited`, `main_window.py:2286`) and Environment edits (`_on_env_value_changed`) mutate state directly without `QUndoCommand` -> cannot be undone! | 100% of mutating actions go through `QUndoStack`; Undo restores exact pre-edit state & marks viewport dirty. |
| 10 | `World -> Redo` (`Ctrl+Y`) | **`PARTIAL / DESYNC`** | Re-apply undone operation. | Same lockstep invariants as Undo. | Same `QUndoStack` bypasses as #9. | `Undo -> Redo` yields bit-identical `build_project_overlay_dict()`. |
| 11 | `World -> Environment / Skybox...` / Toolbar `Env/Sky` / Empty Inspector `Env / Sky` | **`PARTIAL / DESYNC`** | Adjust track skybox, time-of-day lighting, and fog for both preview and in-game rendering. | `HOSTTEX` `sky01A..16B` (`.eodhtx`), `native_renderer.cpp` / `track.grf` fog/lighting (not yet hooked). | **FMEA:** Only `Borrow Sky From Track` exports to `eod.exe`; Sky Presets, Sun, Ambient, and Fog are OpenGL-only illusions. | Either wire `EOD_ENV_FILE` (`env_override.ini`) into `native_renderer.cpp` + `gs_vulkan.cpp` OR badge non-baked controls clearly until wired. |
| 12 | `World -> Reload Track from Retail Disc` | **`REAL & VERIFIED`** | Discard in-memory edits and reload pristine retail `.TRK` (bypassing edits). | Retail `.TRK` + turbo binary cache. | Turbo cache could mask freshly modified retail disc files if mtime/size check fails. | Clears `undo_stack`, reloads track asynchronously, matches retail baseline. |
| 13 | `World -> Open DATA/TRACKS Folder` | **`REAL & VERIFIED`** | Inspect `.TRK` files directly in Windows Explorer. | `game_dir/DATA/TRACKS`. | Silent no-op on non-Windows or missing directory. | Opens folder in OS file manager or shows status warning if missing. |
| 14 | `World -> Play Free Roam (PIE)` / Toolbar `Free Roam` (`F5`) | **`REAL & VERIFIED`** | One-click compile + drive current track solo/battle inside embedded Vulkan viewport. | `EOD_MOD_DIR`, `EOD_DIRECT_START`, `EOD_OPT_TERRAIN_MASK`, `EOD_PIE_CONTROL_FILE`, `--windowed`. | Compile failure aborts PIE cleanly; if `eod.exe` missing, shows warning. **FMEA:** Win32 `SetParent` embedding can lose keyboard focus on Alt-Tab unless `AttachThreadInput` re-engages. | Compiles mod in background, spawns `eod.exe --windowed`, embeds HWND, restores 3D viewport on `Shift+Esc`. |
| 15 | `World -> Play Event / Race (PIE)` / Toolbar `Play Event` (`Shift+F5`) | **`REAL & VERIFIED`** | Compile + jump straight into the selected contest with chosen car & AI count. | `EOD_DIRECT_START="mode=action;track=...;event=...;car=...;ai=..."` (`src/session_start.cpp:492-500`). | Invalid event name falls back to native lobby (`session_start.cpp:809`). | Launches exact contest with requested car and `0..12` AI bots. |
| 16 | `World -> Stop PIE` / Toolbar `Stop` / PIE Bar `Stop PIE (Shift+Esc)` | **`REAL & VERIFIED`** | Terminate embedded `eod.exe` immediately and return to 3D editor. | Win32 `AttachThreadInput` detach + `proc.terminate()/kill()`. | Zombie `eod.exe` if process ignores `SIGTERM`; stuck `act_pie_stop` enabled. | Terminates process within 1s, detaches thread input, switches stack back to `viewport`. |

### 2.3 View & Camera Menus (`main_window.py:316-339`)

| # | Control & Location | Current Code Reality | JTBD (User Intent) | Coupled PS2 Invariants | Forecasted Failure Modes (FMEA) | Verifiable DoD |
|---:|---|---|---|---|---|---|
| 17 | `View -> Wireframe` / `Mesh -> Toggle Wireframe Overlay` | **`REAL & VERIFIED`** | Inspect triangle density and topology in OpenGL viewport. | Pure viewport display. | None (GL `GL_LINE` polygon mode). | Toggles wireframe cleanly; menu & mesh menu stay synced. |
| 18 | `View -> Collision Overlay (SCOL + SOBL)` / `Collision -> Toggle Collision Wireframe` | **`REAL & VERIFIED`** | Visualize height-coded `SOBL` walls, `SCOL` terrain, and `custom_boxes`. | Reads `track.sol` (`SOBL`), `track.col` (`SCOL`), `custom_boxes`. | Heavy line batches on large tracks; `custom_boxes` look identical to baked `SOBL` despite not exporting! | Syncs menu/toolbar toggles; visually distinguishes baked `SOBL` vs. unbaked/custom collision. |
| 19 | `View -> Markers & Start Grid` / `Markers -> Toggle Markers Visibility` | **`REAL & VERIFIED`** | Show/hide 3D marker cones and heading arrows. | Pure viewport display. | None. | Toggles marker rendering immediately. |
| 20 | `View -> Lighting` | **`REAL & VERIFIED`** | Toggle directional/ambient shading vs. unlit flat textures. | Pure viewport display. | None. | Updates GL shader lighting uniform. |
| 21 | `View -> Model Labels` | **`REAL & VERIFIED`** | Display 3D floating model names over nearby objects. | Pure viewport display. | Label clutter at overview zoom; CPU projection loop on 6k objects if uncapped. | Caps rendered labels to nearest N visible objects within distance threshold. |
| 22 | `View -> Racing Lines (ILIN)` / `Roads -> Show / Hide Racing Lines` | **`REAL & VERIFIED`** | Show/hide `.ild` AI racing spline ribbons & nodes. | Reads `*.ild` (`ILIN`). | None. | Toggles ILIN ribbon/node rendering. |
| 23 | `View -> Grid` | **`REAL & VERIFIED`** | Show/hide reference ground grid in viewport. | Pure viewport display. | None. | Toggles GL reference grid. |
| 24 | `View -> Performance Profiler / HUD` (`F3`) | **`REAL & VERIFIED`** | Monitor FPS, CPU/GPU frame time, draw calls, and bake timings. | `perf_profiler.py` + GL timer queries. | GL timer queries unsupported on software/old drivers (handled gracefully). | `F3` toggles HUD overlay with live ms breakdown. |
| 25 | `Camera -> Frame Selection` (`F`) | **`REAL & VERIFIED`** | Fly camera smoothly to frame selected object, marker, or road node. | Pure viewport camera. | No-op when nothing selected. | Centers camera at `2.2x` bounding radius of selection. |
| 26 | `Camera -> Reset Track Overview` | **`REAL & VERIFIED`** | Jump back to whole-track overview angle. | Uses `world.world_center` & `world_radius`. | None. | Resets camera pos/yaw/pitch to track overview. |
| 27 | `Camera -> Top-Down Map View` | **`REAL & VERIFIED`** | Bird's-eye orthographic-like view for layout & road planning. | Pure viewport camera. | Gimbal lock at `-90°` pitch (clamped to `-89°`). | Positions camera above `world_center` looking straight down. |
| 28 | `Camera -> Fly Speed: Slow (15)` | **`REAL & VERIFIED`** | Fine-detail camera movement for prop/ramp alignment. | Pure viewport camera. | None. | Sets `viewport.fly_speed = 15.0`. |
| 29 | `Camera -> Fly Speed: Normal (45)` | **`REAL & VERIFIED`** | Default track cruising speed. | Pure viewport camera. | None. | Sets `viewport.fly_speed = 45.0`. |
| 30 | `Camera -> Fly Speed: Fast (120)` | **`REAL & VERIFIED`** | Rapid cross-map traversal on large tracks (e.g., HomeBase/Welch). | Pure viewport camera. | None. | Sets `viewport.fly_speed = 120.0`. |

### 2.4 Objects Menu, Context Menu & Transform Toolbar (`main_window.py:341-356, 522-562, 1633-1650`)

| # | Control & Location | Current Code Reality | JTBD (User Intent) | Coupled PS2 Invariants | Forecasted Failure Modes (FMEA) | Verifiable DoD |
|---:|---|---|---|---|---|---|
| 31 | `Objects -> Move Tool` / Toolbar `Move` (`G`) | **`REAL & VERIFIED`** | Translate objects, markers, or road nodes along X/Y/Z gizmo axes. | Static/opt-terrain -> `GRAF` + `BPPT` + `SCOL` + `auto_sobl`; Dyno -> `GRAF` 4x3; Event-prop -> `c_*.va0` `unfloat_frame64`. | Moving only one sub-part of a compound ramp leaves top cap or `BPPT` split. | Gizmo drag pushes `TransformObjectCommand` / `MultiTransformObjectsCommand`; baked position matches in `eod.exe`. |
| 32 | `Objects -> Rotate Tool` / Toolbar `Rotate` (`R`) | **`REAL & VERIFIED`** | Rotate objects (H/P/R) or markers (heading). | Static/opt-terrain -> `VIF` vertex bake + `BPPT`/`SCOL` plane normal rotate; Dyno/Event-prop -> `GRAF`/`c_*.va0` matrix. | Steep pitch/roll on static ramps without `SCOL` triangle rebuild can misalign normal vs. `BPPT` planes. | Baked mesh & collision normals rotate consistently; verified in PIE. |
| 33 | `Objects -> Scale Tool` / Toolbar `Scale` | **`REAL & VERIFIED`** | Resize props, ramps, or cars uniformly or per-axis. | Static -> `VIF` bake + `BPPT`/`SCOL` + `auto_sobl`; Event-prop/Dyno -> hides retail at `Y=-5000`, injects scaled `.mod` + `obstacle.val` wall (since `unfloat_frame64` has no scale). | **FMEA:** No keyboard shortcut bound (unlike `G`/`R`/`P`/`B`); scaling a `dyno` car scales visual `.mod` + `obstacle.val` AABB (`dx/dy/dz`), not non-axis-aligned hulls. | Scaled prop renders & collides at scaled bounds in `eod.exe`. |
| 34 | `Objects -> Place Mode (Ghost Preview)` / Toolbar `Place` (`P`) | **`REAL & VERIFIED`** | Preview a translucent prop/car snapped to `SCOL` terrain and stamp copies with LMB. | Injects `.mod`/`.tex` + `obstacle.val` `[dyno]` + `c_*.va0` `$NNNN$object` via `0TSR` repack (`project_state.py:2260`). | **FMEA:** All placed props currently force `type = wall` in `obstacle.val` (no `Fixed`/`Phased`/`Normal` physics mode choice); placing >255 custom props may exhaust `c_*.va0` or `obstacle.val` limits. | Stamped object persists in project, compiles cleanly into `.TRK`, appears & collides in PIE. |
| 35 | `Objects -> Scatter / Paint Brush` / Toolbar `Scatter` (`B`) | **`REAL & VERIFIED`** | Paint randomized clusters of props/trees/wrecks across terrain in one stroke. | Same `0TSR` + `obstacle.val` + `c_*.va0` pipeline; single `ScatterPlaceCommand` undo. | Painting hundreds of props triggers heavy full `0TSR` repack and `obstacle.val` bloat; no per-stroke instance budget warning. | Single Undo removes entire stroke; compiled props render in `eod.exe`. |
| 36 | `Objects -> Replace Selection With...` / Context / Properties / `ReplaceModelDialog` | **`REAL & VERIFIED`** | Swap a retail prop/wreck for any other track model or one of the 37 retail `.CAR` models. | Hides retail original at `Y=-5000`, injects replacement `.mod`/`.car` into `.TRK`. | **FMEA:** Only replaces single `selected_obj`, not entire multi-selection; replacing a static terrain chunk with a `.car` prop converts its collision from `SCOL` to `obstacle.val` box. | Replaces selected object(s) undoably; replacement renders in viewport & PIE. |
| 37 | `Objects -> Select All Similar` / Context / Properties | **`REAL & VERIFIED`** | Batch-select every visible instance of the same fence, tree, cone, or texture family. | Matches `model_key` or `(primary_texture, category)` (`world_loader.py:1241`). | Texture-family fallback can over-select unrelated props that merely share a generic wood/metal texture. | Selects exact model matches (with optional texture-family toggle) and highlights all in viewport. |
| 38 | `Objects -> Delete All Similar` / Context / Properties | **`REAL & VERIFIED`** | Wipe all instances of an unwanted prop family + their LOD twins + overlapping SOBL barriers in one undoable step. | `MultiDeleteObjectsCommand` -> `Y=-5000` + `SOBL` flatten (`-0.5m`). | Over-broad similarity match can accidentally delete desired props sharing a texture. | Single Undo restores all deleted objects and all flattened `SOBL` barriers. |
| 39 | `Objects -> Snap Selected to Ground` / Context / Properties (`End` / `Ctrl+G`) | **`REAL & VERIFIED`** | Drop floating object/marker flush onto `SCOL` terrain surface. | Queries `collision.ground_height(x, z, below=y+5.0)`. | **FMEA:** (1) Ignores multi-selection (only snaps `selected_obj`); (2) `Ctrl+G` conflicts with universal Group shortcut; (3) Uses `SCOL` only, ignoring placed ramps underneath. | Snaps all selected objects cleanly; rebind to `End` only so `Ctrl+G` is freed for Grouping. |
| 40 | `Objects -> Duplicate Selected` / Context (`Ctrl+D`) | **`PARTIAL / DESYNC`** | Clone current selection (single, multi-select, compound group, or marker) with all properties. | Creates new `source="Placed by the editor"` object(s) compiled into `c_*.va0`. | **FMEA:** (`main_window.py:1868-1873`) Only duplicates single `viewport.selected_obj`! Ignores `selected_objects` multi-select, markers, and compound groups; hardcoded `(+4, 0, +4)` offset floats off slopes without ground snap. | Duplicates full multi-selection / group / marker in one `QUndoCommand` and selects the clones. |
| 41 | `Objects -> Edit Selected in Blender (OBJ)...` / `Mesh -> Export Selected Object to OBJ...` / Context / Properties (`F2`) | **`PARTIAL / DESYNC`** | Round-trip a track mesh or vehicle prop to Blender for geometry sculpting. | Exports RH Z-up `.obj` + `.mtl` + PNG textures (`project_state.py:3560`). | **FMEA:** One-way export only! No `"Re-import Edited OBJ..."` action exists in World Editor (despite `tools/vehicle_import.py` existing for cars). Modder gets stranded in Blender. | Exports valid `.obj/.mtl/png` AND provides paired `"Re-import OBJ..."` (same-topology VIF re-encode or standalone `.mod` inject). |
| 42 | `Objects -> Make Collision for Selected` / `Collision -> Make Collision Volume` / Context / Properties | **`JSON-ONLY / NOT READ BY GAME`** | Generate a solid physics box for a visual-only mesh so cars don't drive through it. | **SHOULD** append a `BOX` record to `track.sol` (`SOBL` v3) via `_append_sobl_boxes()`. | **CRITICAL FMEA:** `MakeCollisionCommand` (`project_state.py:211-239`) appends to `world.collision.custom_boxes` and sets `has_custom_collision=True` (button turns `"Collision Ready"`), **but `export_mod_layer()` NEVER passes `world.collision.custom_boxes` to `_append_sobl_boxes()`!** Cars drive straight through it in `eod.exe`! | `export_mod_layer()` compiles every `custom_boxes` entry into `track.sol` `SOBL` (invalidating delta cache when changed); verified by car collision in PIE + `test_editor.py`. |
| 43 | `Objects -> Delete Selected` / Context / Properties (`Del`) | **`REAL & VERIFIED`** | Remove selected object(s), LOD twins, overlapping SOBL walls, road node, or marker. | `GRAF` / `c_*.va0` moved to `Y=-5000` + `SOBL` flattened to `-0.5m`. | Deleting a road node only deletes in JSON (see #95); deleting a marker only deletes in JSON (see #102). | Objects & twins vanish both visually and physically in `eod.exe`; Undo restores completely. |

### 2.5 Markers, Cars & Breakers Menus (`main_window.py:359-379`)

| # | Control & Location | Current Code Reality | JTBD (User Intent) | Coupled PS2 Invariants | Forecasted Failure Modes (FMEA) | Verifiable DoD |
|---:|---|---|---|---|---|---|
| 44 | `Markers -> Place Marker at View Target` | **`JSON-ONLY / NOT READ BY GAME`** | Place a generic named world waypoint / reference point. | `GRAF` subtype `19` (`NamedPoint`) or editor-only annotation. | User assumes marker triggers game behavior; nothing reads it in `eod.exe`. | Either clearly labeled `"Editor Bookmark (No Gameplay Effect)"` OR wired to a specific `c_*.va0` / Forge gadget type. |
| 45 | `Markers -> Place Start Grid Slot` | **`JSON-ONLY / NOT READ BY GAME`** | Reposition race starting grid slots so races launch from a custom location/heading. | `*.ild` (`ILIN` node 0 origin/heading) for Races; `c_battle.va0` `[respawn]` sections (`0x44`-byte records at `battle_master+0x10`) for Battle/Free Roam. | **CRITICAL FMEA:** Only appends `WorldMarker(kind="Start Grid")` to `.eodworld.json` and `custom_race.json` `start_grid[]`. `eod.exe` **never reads `custom_race.json`** — cars still spawn at the untouched retail grid! | Bakes Battle/Free Roam spawns into `c_battle.va0` `[respawn]` AND/OR runtime `EOD_SPAWNS_FILE` hook in `f_00167C70` / `f_001D8F58` + Race grid hook; verified in PIE. |
| 46 | `Markers -> Place Spawn Point` | **`JSON-ONLY / NOT READ BY GAME`** | Set player spawn / Battle respawn point & facing direction. | Same as #45 (`c_battle.va0` `[respawn]` + `battle_master+0x10..+0x450`). | **CRITICAL FMEA:** Identical Potemkin drop — leaves `eod.exe` spawning at retail origin. | Player car spawns at exact marker `(X, Y, Z, heading)` in PIE Free Roam & Battle. |
| 47 | `Markers -> Place Chicken Marker` | **`JSON-ONLY / NOT READ BY GAME`** | Place a Battle weapon/chicken pickup or hazard spawn. | `c_battle.va0` weapon/dyno pickup entries (`kWeaponScenes` in `battle_bots.cpp:136`). | **CRITICAL FMEA:** Writes JSON marker only; zero chicken/weapon spawns appear in `eod.exe`. | Compiles into `c_battle.va0` weapon/chicken entry (or hides control until weapon table baker lands). |
| 48 | `Markers -> Place Event Trigger` | **`JSON-ONLY / NOT READ BY GAME`** | Define custom trigger volume (checkpoint, kill/safe zone, ringout, teleporter). | `c_<event>.va0` `bound`/`ringout`/`safe_bound`/`play_area` polygons or runtime volume table (`pie_forge_volumes.ini`). | **CRITICAL FMEA:** Writes JSON marker only; driving through it in `eod.exe` triggers nothing. | Compiles into `c_<event>.va0` polygon or runtime Forge Volume table (Section 4.4) evaluated in `battle_driver.cpp`. |
| 49 | `Cars -> Browse Cars & Wrecks Library (DATA/CAR)` | **`REAL & VERIFIED`** | Filter Explorer to browse all 37 retail `.CAR` chassis for prop/wreck placement. | Reads `DATA/CAR/*.CAR` (`MODL` + `TEX`). | Background car scan (`_start_background_car_scan`) may still be running when user clicks immediately after launch. | Switches Explorer to Models tab filtered to `car-wreck`; shows loading indicator if scan in progress. |
| 50 | `Cars -> Vehicle / NASCAR Tuner (Live PIE)...` / Toolbar `Car Tuner` | **`PARTIAL / DESYNC`** | Tune vehicle handling/physics permanently (`physics.val`) or live in PIE, inspect 3D cars, and import NASCAR vehicles. | `DATA/CAR/<CAR>.CAR#physics.val` (`VALT`), `pie_live_control.ini`, `CT.RES`, `COMMON.RES`, `import_nascar_cars.py`. | **FMEA:** Only live PIE Speed Boost works; `Inspect Retail Car` dropdown is dead (`STUB`) and NASCAR tab is static text (`STUB`). See Section 5 for full 3D Car Studio parity spec. | Replaced by full **3D Car Studio** (live + permanent `physics.val` editor, 3D car+chassis GL preview, click-to-retexture, real NASCAR `.ISO`/folder importer). |
| 51 | `Cars -> Place Car Spawn at View Target` | **`JSON-ONLY / NOT READ BY GAME`** | Place a player/free-roam car spawn point with heading & optional vehicle override. | `c_battle.va0` `[respawn]` + `EOD_SPAWNS_FILE` / `session_start.cpp` (`car=`). | **CRITICAL FMEA:** Places `WorldMarker(kind="Car")` in JSON only; does NOT spawn a car or move player spawn in `eod.exe`! (See Section 3 Deep Dive). | Spawns player/car at exact pose & facing in PIE and compiled mod layer. |
| 52 | `Cars -> Place AI Opponent Car Slot` | **`JSON-ONLY / NOT READ BY GAME`** | Place a specific AI opponent starting slot (pose, facing, optional vehicle/team). | `c_battle.va0` `[respawn]` slots `1..11` + `f_00167C70` (`src/battle_bots.cpp:197-219`). | **CRITICAL FMEA:** Places `WorldMarker(kind="Car", name="AI_Car_N")` in JSON only; AI cars still spawn at retail respawn table rotated by `index % records`! | AI slot `k` spawns at placed `AI_Car` marker pose & heading in PIE and mod layer. |
| 53 | `Breakers -> Place Destructible Breaker Marker` | **`JSON-ONLY / NOT READ BY GAME`** | Place a breakable/destructible prop (crate, fence, hay bale, barn wall). | `obstacle.val` (`type != wall` destructible dyno) + `c_*.va0` `$NNNN$object`. | **CRITICAL FMEA:** Places `WorldMarker(kind="Breaker")` which is **not even written to `custom_race.json`** and never compiles into `obstacle.val` or `c_*.va0`! | Replaced by real destructible prop placement (`obstacle.val` destructible dyno template) or removed until destructible dyno compilation lands. |
| 54 | `Breakers -> Filter Models: Dyno / Props` | **`REAL & VERIFIED`** | Filter Explorer catalog to dynamic/breakable track props. | `category == "dyno"`. | None. | Filters Explorer to `dyno` models. |

### 2.6 Roads, Terrain, Mesh, Races, Collision & Help Menus (`main_window.py:381-425`)

| # | Control & Location | Current Code Reality | JTBD (User Intent) | Coupled PS2 Invariants | Forecasted Failure Modes (FMEA) | Verifiable DoD |
|---:|---|---|---|---|---|---|
| 55 | `Roads -> Toggle Road Tool (ILIN)` / Toolbar `Road` / Properties `Draw roads` | **`REAL & VERIFIED`** | Activate ILIN racing line node editing mode. | `*.ild` (`ILIN` binary). | Allows users to click terrain and add new `retail_index=None` nodes that silently vanish on bake (see #96). | Clearly distinguishes movable retail nodes vs. full `.ild` rebuild support (once implemented). |
| 56 | `Roads -> End Current Road Chain` | **`REAL & VERIFIED`** | Stop chaining edges from the last road node. | Editor `road_chain_last_node` state. | None. | Clears `viewport.road_chain_last_node`. |
| 57 | `Terrain -> Toggle Static Terrain Layer` | **`REAL & VERIFIED`** | Hide/show static terrain meshes to inspect underlying collision or props. | Viewport `layer_visibility["static"]`. | None. | Toggles `static` visibility and syncs Explorer Layers checkbox. |
| 58 | `Terrain -> Toggle Optional Terrain Layer` | **`REAL & VERIFIED`** | Hide/show event-gated optional ramps/terrain (`opt_terrain`). | Viewport `layer_visibility["optional-terrain"]`. | None. | Toggles `optional-terrain` visibility and syncs Explorer checkbox. |
| 59 | `Terrain -> Sample Ground Height Under Camera` | **`REAL & VERIFIED`** | Read exact `SCOL` ground Y elevation directly beneath camera. | `world.collision.ground_height(x, z)`. | Returns `0.0` outside SCOL bounds without indicating out-of-bounds. | Prints exact SCOL `(X, Y, Z)` + surface type in status bar & Output Log. |
| 60 | `Mesh -> Show Track Mesh Statistics...` | **`REAL & VERIFIED`** | Audit model count, instance count, triangle budget, and texture count. | `world.models`, `world.objects`, `world.textures`. | None. | Dialog displays accurate live counts (excluding deleted instances). |
| 61 | `Races -> Custom Race & AI Pathfinding (ILIN) Editor...` / Toolbar `Race Setup` | **`PARTIAL / DESYNC`** | Configure event type, car, AI count, laps, AI speed, arcade mode, and launch PIE. | `EOD_DIRECT_START`, `EOD_ARCADE_MODE`, `EOD_OPT_TERRAIN_MASK`, `COMMON.RES#<track>_eves.val` (unpatched). | **FMEA:** Laps, AI Speed Scale, Event Name, and Start Grid are JSON-only (#120-122); `Linked ILIN` only changes editor view (#123). | Every visible field in `CustomRaceDialog` either patches `COMMON.RES` / runtime IPC or is removed. |
| 62 | `Races -> Sync All Events` / Toolbar `Sync All Events` / Dialog Checkbox | **`REAL & VERIFIED`** | Ensure modded ramps, props, and deletions appear across Free Roam and every race event on the track. | Sets `EOD_OPT_TERRAIN_MASK=0xFFFF`, patches `track.bsc` `table2[0].aux[1]=0xFE` (`battle_driver.cpp:242`), mirrors `c_*.va0` edits across peer events. | Mirror matching uses `(model_key, round(pos*2))` signature; if two events offset the same prop by >0.5m, peer isn't synced. | Toggling updates `world.sync_all_events`; verified in `track.bsc`, `c_*.va0`, and PIE. |
| 63 | `Races -> Jump Camera to Start / Finish Grid` | **`REAL & VERIFIED`** | Teleport editor camera to node 0 / Start Grid. | Reads `m_start_0` or `road_graph` node 0. | None. | Frames start grid cleanly. |
| 64 | `Races -> Show Track ILIN Racing Lines Summary...` | **`REAL & VERIFIED`** | Inspect all `.ild` lines embedded in the track archive. | `world.all_road_graphs`. | None. | Lists every `.ild` member, node count, and dirty status. |
| 65 | `Collision -> Remove / Flatten Track Barriers (SOBL)` / Properties `Flatten ALL` | **`PARTIAL / DESYNC`** | Remove invisible sky-walls so cars can jump over track walls and explore off-track. | Patches `track.sol` `SOBL` box `centre`, `ry`, `sphere_radius` (`project_state.py:2767`). | **FMEA:** Flattens **every** vertical `SOBL` box to `-0.5m` indiscriminately — destroys real physical concrete walls, bridges, and buildings along with 20m invisible sky-walls! Should be split into (a) **Clamp Invisible Sky-Walls to Visual Asset Height** and (b) **Walls Noclip (Keep Ground Solid)** (Section 4.3). | Provides two distinct undoable actions (`Clamp Sky-Walls to Asset Height` vs. `Flatten All Vertical Walls / Noclip`) with exact barrier count report. |
| 66 | `Collision -> Inspect Tallest Barriers...` | **`REAL & VERIFIED`** | Identify invisible sky-walls (`>=10m`) blocking jumps. | Reads `world.collision.sobl_records`. | Dialog is read-only text (cannot click a barrier row to jump camera to it). | Lists top 12 tallest barriers with index, surface, and height above ground. |
| 67 | `Collision -> Show Collision Statistics...` | **`REAL & VERIFIED`** | Inspect `SCOL` triangle count, `SOBL` record count, edited barriers, and custom boxes. | `world.collision`. | Reports `custom_boxes` count as if they were active in-game despite #42. | Accurately distinguishes baked vs. unbaked collision primitives. |
| 68 | `Help -> Controls & Keybindings...` | **`REAL & VERIFIED`** | Quick reference for editor & PIE hotkeys. | Static dialog text. | Lists `Ctrl+G` as Snap-to-Ground; does not document `Shift+Esc` vs `Esc` nuances. | Matches actual bound `QKeySequence`s 1:1. |
| 69 | `Help -> About EOD World Editor...` | **`REAL & VERIFIED`** | Display version and supported PS2 formats. | Static dialog text. | None. | Opens info dialog cleanly. |

### 2.7 Main Toolbar Unique Widgets, Status Bar & Output Log (`main_window.py:162-171, 463-653`)

| # | Control & Location | Current Code Reality | JTBD (User Intent) | Coupled PS2 Invariants | Forecasted Failure Modes (FMEA) | Verifiable DoD |
|---:|---|---|---|---|---|---|
| 70 | `Toolbar -> Event Template Picker (cmb_pie_event)` | **`REAL & VERIFIED`** | Switch viewport ramp/prop visibility, active `.ild` line, and PIE target contest simultaneously. | `world.event_templates`, `opt_terrain_mode`, `c_*.va0`, `EOD_DIRECT_START`. | Selecting `"All Templates (Combined)"` hides duplicate `c_*.va0` props using `c_battle.va0` preference, which can confuse modders editing a non-Battle event. | Live-filters viewport, switches `.ild`, routes exact contest to PIE. |
| 71 | `Toolbar -> Player Vehicle Picker (cmb_pie_car)` | **`REAL & VERIFIED`** | Choose retail or custom `.CAR` to drive in PIE. | `DATA/CAR/*.CAR` + `EOD_DIRECT_START` `car=<name>`. | Editable combo lets user type a typo'd car name that `session_start.cpp` rejects. | Auto-completes from discovered retail + mod `.CAR`s; validates before PIE launch. |
| 72 | `Toolbar -> AI Opponents SpinBox (spn_pie_ai)` | **`REAL & VERIFIED`** | Set `0..12` AI cars for Free Roam or Event PIE. | `EOD_DIRECT_START` `ai=0..12` (`src/battle_bots.cpp:84-106`). | When launching `Free Roam` on a specific non-Battle template, `pie_host.py:285` forces `ai=0` even if spinbox > 0 (surprise override!). | Displays tooltip/badge when `Free Roam` forces solo, or respects `spn_pie_ai` consistently. |
| 73 | `Toolbar -> Renderer Quality (PS2 Fast / Plume HQ)` | **`REAL & VERIFIED`** | Switch between authentic PS2 renderer and modern Plume Vulkan PBR renderer in both viewport and PIE. | `EOD_MODERN=1/0`, `EOD_NATIVE=0`. | Changing mode while PIE is running does not hot-reload `eod.exe` (requires PIE restart). | Updates viewport shader immediately and passes exact env vars to PIE. |
| 74 | `Toolbar -> Grid Snap (1.0 m)` | **`REAL & VERIFIED`** | Snap Move gizmo & Ghost placement to 1.0 m grid increments. | Editor transform precision. | Fixed at `1.0 m` (no configurable grid size like `0.25m / 0.5m / 1m / 2m / 5m`). | Snaps positions cleanly during drag and ghost stamp. |
| 75 | `Toolbar -> Angle Snap (15°)` | **`REAL & VERIFIED`** | Snap Rotate gizmo & Ghost rotation to 15° increments. | Editor transform precision. | Fixed at `15°` (no `5° / 15° / 45° / 90°` selector). | Snaps rotations cleanly during drag and ghost rotate. |
| 76 | `Autosave Banner -> Recover Autosave / Discard` | **`REAL & VERIFIED`** | Restore unsaved edits after a crash. | `~/.eod_world_editor/autosaves/*.eodworld.json`. | Only checks autosave for the currently opened track/project. | Recovers full overlay state and hides banner; Discard deletes snapshot. |
| 77 | `Status Bar -> Cancel Background Job` | **`REAL & VERIFIED`** | Abort a long track load or mod compile cleanly. | `CancelToken` cooperative cancellation. | Cannot interrupt mid-call inside `rst0.pack_archive()` until next checkpoint. | Sets token cancelled and resets progress bar without corrupting state. |
| 78 | `Status Bar / Dock -> Output Log Toggle & Verbose Output` | **`REAL & VERIFIED`** | Inspect compile timings, PIE command lines, and warnings. | Pure UI dock. | Large verbose logs over long sessions aren't capped in `QPlainTextEdit` (`maximumBlockCount` check). | Toggles dock cleanly; logs all bake/PIE events. |
| 79 | `Output Log Dock -> Dump cProfile Trace` | **`REAL & VERIFIED`** | Capture Python CPU profiling trace for performance debugging. | `cProfile` + `perf_profiler.py`. | Runs synchronous sample tick on UI thread briefly. | Writes `.prof`/summary file and prints path to Output Log. |
| 80 | `Output Log Dock -> Copy Log / Clear` | **`REAL & VERIFIED`** | Copy diagnostic log to clipboard or clear buffer. | Pure UI dock. | None. | Copies full text / empties log buffer. |

### 2.8 Left Panel: Explorer & Viewport Direct Manipulation (`explorer_panel.py`, `viewport_gl.py`)

| # | Control & Location | Current Code Reality | JTBD (User Intent) | Coupled PS2 Invariants | Forecasted Failure Modes (FMEA) | Verifiable DoD |
|---:|---|---|---|---|---|---|
| 81 | `Explorer -> Track Selector (track_combo)` | **`REAL & VERIFIED`** | Switch active retail `.TRK` archive quickly. | Loads `.TRK` via Turbo Binary Cache (`world_loader.py:2383`). | Switching track discards unsaved edits without dirty-check confirmation. | Loads chosen track async; prompts if current project is dirty. |
| 82 | `Explorer -> Scene Tab (Search + Hierarchy Tree)` | **`REAL & VERIFIED`** | Locate and select any placed object or marker by name/category. | `world.objects`, `world.markers`. | Clicking marker frames it, but does not indicate whether marker is game-active vs. JSON-only; 6k tree items rebuild on every edit. | Filters instantaneously; click selects & frames object/marker in viewport. |
| 83 | `Explorer -> Models Tab (Search, Category Filter, Cars Quick Filter, Only Placed)` | **`REAL & VERIFIED`** | Browse track models, standalone `.mod` props, and 37 retail `.CAR` wrecks with 3D thumbnails. | `world.models` + background `thumbnails.py`. | Thumbnail worker runs on startup; rapid filtering cancels/restarts queue safely. | Filters accurately; `Cars` button jumps straight to `car-wreck` category. |
| 84 | `Explorer -> Models Tab (Ghost / Scatter / Swap / Place & Double-Click)` | **`REAL & VERIFIED`** | Enter Ghost Place, Scatter Brush, Replace Selection, or spawn model in front of camera. | `PlaceObjectCommand`, `ReplaceObjectModelCommand`, `ScatterPlaceCommand`. | `Swap` does nothing silently if no object is currently selected in viewport. | Activates requested tool/action and provides status feedback if selection missing. |
| 85 | `Explorer -> Roads Tab (Active .ild Line Selector)` | **`REAL & VERIFIED`** | Switch which embedded `.ild` AI racing line is active for viewing/editing. | `world.all_road_graphs`. | Switching `.ild` in Explorer does not tell modder which contest uses that `.ild`. | Switches active `road_graph`, updates viewport spline & node list. |
| 86 | `Explorer -> Layers Tab (Category Visibility Toggles)` | **`REAL & VERIFIED`** | Isolate static terrain, optional terrain, dynos, event props, billboards, or cars. | `viewport.layer_visibility`. | None. | Toggles category visibility in viewport and stays synced with Terrain menu. |
| 87 | `Viewport -> 3D Picking (Objects, SOBL Barriers, Markers, Road Nodes)` | **`PARTIAL / DESYNC`** | Click any 3D asset, barrier, marker, or road node accurately without mis-clicks. | Exact mesh vs. proxy AABB/sphere; compound PS2 multi-leaf ramps. | **FMEA:** (1) Pure Python loop over 6k AABBs per click; (2) Rotated `SOBL` barriers picked via axis-aligned AABB; (3) Clicking a multi-part ramp selects only one leaf (e.g. `ramp-in`), leaving top cap behind! | Vectorized NumPy broadphase + OBB picking for `SOBL` + smart compound auto-grouping with double-click sub-part drill-in (Section 4.2). |
| 88 | `Viewport -> 3D Transform Gizmos (Move / Rotate / Scale, Single & Ctrl+Multi-Select)` | **`REAL & VERIFIED`** | Interactively drag/rotate/scale single or multi-selected objects directly in 3D. | Commits `TransformObjectCommand` / `MultiTransformObjectsCommand` on mouse release. | Dragging static terrain rebuilds GL batch on release; no local-vs-world gizmo space toggle. | Smooth 60 FPS gizmo drag; single Undo reverts entire multi-object transform. |
| 89 | `Viewport -> Fly Navigation (WASD/QE + Shift + RMB) & Ghost/Scatter Stamp` | **`REAL & VERIFIED`** | Unreal/Lumion-style free-fly camera + terrain-snapped ghost stamping. | `SCOL` ground raycast for ghost & scatter placement. | Holding `WASD` while focus is inside a `QLineEdit` in Properties doesn't hijack typing (good), but `Ctrl+A` Select All is unimplemented. | Smooth frame-rate-independent fly movement; Ghost `R` rotates by angle snap. |

### 2.9 Right Panel: Properties Inspector — All 6 Pages (`properties_panel.py`)

| # | Control & Location | Current Code Reality | JTBD (User Intent) | Coupled PS2 Invariants | Forecasted Failure Modes (FMEA) | Verifiable DoD |
|---:|---|---|---|---|---|---|
| 90 | `Properties (Object) -> Position / Rotation / Scale Numeric Fields` | **`REAL & VERIFIED`** | Enter exact coordinates, angles, or scale multipliers numerically. | Same bake path as #31-33. | Typing invalid text silently resets without tooltip. | Commits undoable `TransformObjectCommand` on `editingFinished`. |
| 91 | `Properties (Object) -> Include Overlapping LOD Twins & SOBL Barrier Checkbox` | **`REAL & VERIFIED`** | Ensure deleting a fence/prop also deletes its distance LOD twin (`lod-f`) and flattens its paired invisible `SOBL` wall. | `world.find_overlapping_lod_twins_and_barriers()`. | Only applies to **Delete**, NOT to **Move/Rotate/Scale**! Moving a fence leaves its `lod-f` twin and `SOBL` wall behind! | Extend twin-linking to Move/Rotate/Scale via Smart Compound Grouping (Section 4.2). |
| 92 | `Properties (Object) -> Texture Inspector (Thumbnail / Replace PNG... / Reset)` | **`REAL & VERIFIED`** | Inspect every texture on the selected mesh and swap any texture live with a PNG (baked to Vulkan `HOSTTEX`). | `HOSTTEX/replacements.tsv` + `.eodhtx` full mip chain (`project_state.py:1148`). | **FMEA:** Direct-color textures or textures missing raw PS2 `TEX` header skip `HOSTTEX` export with only a log warning (`project_state.py:1211`). | Swaps live in OpenGL + exports valid `.eodhtx` read by `eod.exe`; disables button with reason tooltip for non-CLUT/missing-TEX assets. |
| 93 | `Properties (Road) -> Retail Node Position (X, Y, Z)` | **`REAL & VERIFIED`** | Move an existing retail ILIN waypoint to reshape the AI racing line. | `*.ild` (`ILIN`) via `ilin.encode(..., recalculate=True)` (updates `dirX/dirZ`, `segLen`, `dist`, lap-length floats). | Moving node 0 shifts the visual Start Grid marker only after reload, not live in the same session. | Patches retail `.ild` node bytes & CRC; AI cars follow the reshaped line in PIE. |
| 94 | `Properties (Road) -> Pull a road out` | **`JSON-ONLY / NOT READ BY GAME`** | Create a new branch off the racing line. | Requires binary `ILIN` side-line (`branch_list` + node insertion + pointer fix-up table). | **CRITICAL FMEA:** Creates `RoadNode(retail_index=None)`; `project_state.py:3229,3404` skips all `retail_index is None` nodes during `.ild` export! | Either implement full `ILIN` binary rebuild/branch serializer in `tools/formats/ilin.py` OR remove button per Anti-Potemkin Rule. |
| 95 | `Properties (Road) -> Delete node` | **`JSON-ONLY / NOT READ BY GAME`** | Remove an unwanted AI waypoint from the `.ild` loop. | Requires resizing `ILIN` node array & rewriting pointer fix-up table. | **CRITICAL FMEA:** Removes node from `RoadGraph` in Python/JSON only; `ilin.encode()` leaves deleted node untouched in binary `.ild`! | Either rebuild `.ild` node table with updated counts/pointers OR disable delete on retail nodes until supported. |
| 96 | `Properties (Road) -> Draw from here / Split Segment / Join Nodes (new nodes)` | **`JSON-ONLY / NOT READ BY GAME`** | Extend or subdivide an AI racing line by clicking terrain. | Same binary `ILIN` node-count/pointer-table rebuild invariant. | **CRITICAL FMEA:** Every added/split node has `retail_index=None` and is **completely dropped** on `.TRK` bake! | Same as #94: full binary `ILIN` serializer or hide until implemented. |
| 97 | `Properties (Road) -> + Lane / - Lane` | **`JSON-ONLY / NOT READ BY GAME`** | Widen or narrow the AI racing corridor width. | `ILIN` node width/speed float (`tools/formats/ilin.py:275`, offset `+0x28`..`+0x34`). | **CRITICAL FMEA:** Only updates `node.lanes` & `node.width` in Python/JSON; `project_state.py` never passes width/lane edits into `ilin.encode()`! | Wire width float into `ilin.encode()` (if verified read by AI `f_00137A68`) OR remove lane buttons. |
| 98 | `Properties (Road) -> Split here: Exit` | **`JSON-ONLY / NOT READ BY GAME`** | Create a fork/exit ramp branch for AI traffic. | `ILIN` side-line `branch_list` (`ilin.py:290`). | **CRITICAL FMEA:** Creates `retail_index=None` node; ignored by `.ild` baker. | Same as #94. |
| 99 | `Properties (Road) -> Split here: Two directions` | **`JSON-ONLY / NOT READ BY GAME`** | Toggle oncoming two-way traffic on a road segment. | EOD `ILIN` has no verified two-way traffic flag. | **CRITICAL FMEA:** Pure City-Builder Potemkin control left over from UI prototype; never serialized to `.ild` and unsupported by `SLUS_209.10`. | **REMOVE** control (no PS2 engine equivalent). |
| 100 | `Properties (Marker) -> Name & Kind Dropdown` | **`JSON-ONLY / NOT READ BY GAME`** | Rename a marker or change its gameplay role (`Spawn`, `Start Grid`, `Car`, `Chicken`, `Trigger`, `Breaker`, `Marker`). | Must re-route to corresponding `c_*.va0` / `EOD_SPAWNS_FILE` / Forge gadget schema. | **CRITICAL FMEA:** Mutates JSON only AND bypasses `QUndoStack` (`main_window.py:2286`). | Wrap in `QUndoCommand` + wire to real Spawn/Gadget baker + Adaptive Scope Card (Section 3.5). |
| 101 | `Properties (Marker) -> Position (X, Y, Z) & Heading` | **`JSON-ONLY / NOT READ BY GAME`** | Precisely position and orient a spawn/grid/trigger marker. | `c_battle.va0` `[respawn]` `(pt, dir)` / `EOD_SPAWNS_FILE`. | **CRITICAL FMEA:** Never baked into `.TRK` or read by `eod.exe`. | Baked spawn pose & heading verified in PIE `eod.exe`. |
| 102 | `Properties (Marker) -> Snap to ground & Delete` | **`JSON-ONLY / NOT READ BY GAME`** | Ground or remove a marker. | Same as #101. | Deleting retail-derived `Start Grid` markers only deletes them in editor JSON; retail game still spawns at `ILIN` node 0 / `c_battle.va0`. | Deleting a spawn marker updates baked spawn table (preventing deletion of last valid spawn so `battle: must have at least 4 respawn points` never panics). |
| 103 | `Properties (Environment) -> Sky Preset Dropdown` | **`JSON-ONLY / NOT READ BY GAME`** | Change time-of-day sky gradient, sun, and fog in-game (Day/Sunset/Night/Storm). | Only `PROFILE.TXT` comment written (`project_state.py:3421`); `eod.exe` ignores presets. | **CRITICAL FMEA:** Modder picks "Night" or "Sunset", sees dramatic OpenGL preview, launches PIE, and gets bright retail noon! | Either write `env_override.ini` read by `native_renderer.cpp` + skybox tint `HOSTTEX`, or explicitly label as `"Viewport Lighting Preview Only"`. |
| 104 | `Properties (Environment) -> Borrow Sky From Track Dropdown` | **`REAL & VERIFIED`** | Transplant another retail track's skybox textures (`sky01A..16B`) into this track. | Exports `HOSTTEX/sky*.eodhtx` + `replacements.tsv` (`project_state.py:1225`). | Fails gracefully if target track lacks matching `sky01A..16B` names. | Borrowed skybox visibly replaces in-game sky in `eod.exe` Vulkan PIE. |
| 105 | `Properties (Environment) -> Sun Azimuth / Elevation / Ambient Intensity` | **`JSON-ONLY / NOT READ BY GAME`** | Adjust directional light angle & ambient fill in-game. | OpenGL preview + `.eodworld.json` only. | **CRITICAL FMEA:** Never sent to `native_renderer.cpp` / `gs_vulkan.cpp`; bypasses `QUndoStack`. | Wire `sun_dir`/`ambient` to `native_renderer.cpp` modern lighting uniform via `env_override.ini` OR mark viewport-only. |
| 106 | `Properties (Environment) -> Fog Start / Fog End / Fog Density / Pick Fog Color` | **`JSON-ONLY / NOT READ BY GAME`** | Adjust atmospheric fog distance and color in-game. | OpenGL preview + `.eodworld.json` only. | **CRITICAL FMEA:** Never sent to `eod.exe` / PS2 GS fog registers; bypasses `QUndoStack`. | Wire `fog_start/end/density/color` into `native_renderer.cpp` / `gs_vulkan.cpp` via `env_override.ini` OR mark viewport-only. |
| 107 | `Properties (Barrier) -> Target Height Spinbox & Apply Height to Barrier` | **`REAL & VERIFIED`** | Lower or raise a single selected `SOBL` invisible wall to exact height above ground. | `track.sol` (`SOBL` v3 `centre`, `ry`, `sphere_radius`). | Setting height `< -10m` may shift bounding sphere unnecessarily (clamped safely by `plan_box_edit`). | Patches exact `SOBL` record in `track.sol` (<1 ms delta); verified in `eod.exe`. |
| 108 | `Properties (Barrier) -> Flatten This Barrier (-0.5 m)` | **`REAL & VERIFIED`** | One-click remove a single selected invisible wall. | `track.sol` `SOBL` box lowered below terrain (`-0.5m`). | None. | Single Undo restores original barrier height; car drives cleanly through flattened wall in PIE. |

### 2.10 Embedded PIE Bar, Home Hub, Custom Race & Vehicle Tuner Dialogs (`pie_host.py`, `home_hub.py`, `dialogs.py`)

| # | Control & Location | Current Code Reality | JTBD (User Intent) | Coupled PS2 Invariants | Forecasted Failure Modes (FMEA) | Verifiable DoD |
|---:|---|---|---|---|---|---|
| 109 | `PIE Bar -> Free Cam Toggle` (`pie_host.py:512`) | **`STUB / POTEMKIN`** | Fly freely inside live PIE using World Editor controls (`F8`), then return to car or drop car at camera (`Ctrl+F8`). | `EOD_FREE_CAM` + `pie_live_control.ini` -> `src/battle_driver.cpp` + PS2 camera matrix hook. | **LIVE QA CONFIRMED DEAD (Class 1 & 2):** (1) `pie_host.py:600` writes `free_cam=1/0`, while `battle_driver.cpp:221` parses `camera_view=`; (2) `camera_view=9` is only a fixed retail preset, not a fly camera — clicking does nothing live! | Implement true `F8` Free Cam in `eod.exe` (editor `RMB + WASD/QE/Shift/Wheel` controls) + `F8` Return to Car + `Ctrl+F8` Drop Car Here (Section 4.5). |
| 110 | `PIE Bar -> AI Auto-Drive Toggle` (`pie_host.py:518`) | **`PARTIAL / DESYNC`** | Hand player car over to AI following the edited `.ild` racing line to verify pathfinding live. | `pie_live_control.ini` `ai_autodrive=1/0` -> `battle_driver.cpp:220, 350-359` (`drive_player_along_ilin`). | **LIVE QA CONFIRMED DEAD (Class 2):** Runs **after** `f_001A2D20_orig()`, and `HumanDriver` overwrites `car+0x1B6C..0x1B7C` from pad/keyboard on the next frame *before* physics reads it! | Execute `drive_player_along_ilin()` **before** `f_001A2D20_orig()` (bypassing `HumanDriver` pad overwrite while active); verified car drives edited `.ild` live in PIE. |
| 111 | `PIE Bar -> Arcade Mode Selector` (`pie_host.py:524`) | **`PARTIAL / DESYNC`** | Live-switch Halo-style custom game rules (`Free Roam`, `Tag / Infected`, `Cat & Mouse`, `Fat Kid / Juggernaut`). | `EOD_ARCADE_MODE` + `pie_live_control.ini` `arcade_mode=` -> `battle_driver.cpp:198-225, 307-343`. | **2 BUGS (Class 2 & 1):** (1) `pie_host.py:611` writes string (`tag`), while `battle_driver.cpp:225` reads `std::atoi(v)` -> live switch always resets mode to `0`! (2) `cat_mouse` (`2`) has **zero C++ gameplay implementation** in `battle_driver.cpp`! | Parse both string & int in `battle_driver.cpp:225`; implement real `cat_mouse` logic (mouse speed/size/target + hunter boost/HUD) or hide until implemented. |
| 112 | `PIE Bar -> Focus Controls` (`pie_host.py:532`) | **`PARTIAL / DESYNC`** | Restore keyboard & gamepad input focus to embedded `eod.exe` HWND. | Win32 `AttachThreadInput` + `SetFocus` + SDL2 scancode `lParam` forwarding. | **LIVE QA CONFIRMED BROKEN (Class 2):** Clicking the `QPushButton` steals Qt focus back onto the button itself; `PostMessage` key forwarding omits proper Win32 scancode `lParam` bits `16..23` required by SDL2! Also, PIE bar duplicates main toolbar `Free Roam`/`Play Event`/`Stop`. | Consolidate into ONE clean PIE toolbar strip; fix `focus_embedded_game()` (`setFocusPolicy(NoFocus)` on strip buttons + full `MapVirtualKeyW` scancode `lParam` + `SetForegroundWindow`/`SetFocus`); remove button once focus is automatic on viewport click. |
| 113 | `PIE Bar -> Restart` (`pie_host.py:537`) | **`REAL & VERIFIED`** | Re-compile latest changes and restart embedded PIE session. | Re-runs `start_pie(self._last_pie_mode)`. | None. | Cleanly stops & restarts PIE with fresh mod bake. |
| 114 | `PIE Bar -> Pop Out` (`pie_host.py:542`) | **`REAL & VERIFIED`** | Detach embedded `eod.exe` into a standalone top-level Win32 window. | Restores saved `GWL_STYLE` / `GWL_EXSTYLE` + `SetParent(0)`. | Disabled once popped out (cannot pop back in without restart). | Restores normal resizable window frame without crashing Vulkan swapchain. |
| 115 | `Home Hub -> Track Cards, Event Template, Project Name & Create World Project` (`home_hub.py`) | **`REAL & VERIFIED`** | Pick a retail track + starting event template and create a workspace project. | `discover_tracks()`, `editor_prefs.json`. | None. | Opens chosen track filtered to chosen event template. |
| 116 | `Home Hub -> Recent Project Cards & Open .eodworld.json...` (`home_hub.py`) | **`REAL & VERIFIED`** | Re-open recent projects with saved viewport thumbnails. | `recent_projects` in `editor_prefs.json`. | Stale path if project was moved/deleted on disk. | Validates path exists before opening; removes stale entries gracefully. |
| 117 | `Home Hub -> Quick Free Roam` (`home_hub.py:395`) | **`REAL & VERIFIED`** | Launch straight into Free Roam PIE on selected track without manual setup. | Loads track + triggers `start_pie("free_roam")`. | None. | Loads track and enters PIE Free Roam automatically. |
| 118 | `Home Hub -> Vehicle / NASCAR Importer Button` (`home_hub.py:405`) | **`STUB / POTEMKIN`** | Jump directly into a Vehicle & NASCAR Car Studio/Importer workflow. | Should open standalone **3D Car Studio** without even requiring a track load! | **CRITICAL FMEA:** Forces a full track load just to pop up `VehicleTunerDialog`, whose NASCAR tab is a static `QLabel` with zero buttons or file pickers! | Launches real **3D Car Studio & NASCAR Importer** immediately (Section 5). |
| 119 | `Home Hub -> Skip Home Hub on Startup Checkbox` (`home_hub.py:370`) | **`REAL & VERIFIED`** | Boot straight into last track on future launches. | Persists `skip_home_hub` in `editor_prefs.json`. | None. | Respected on next launch. |
| 120 | `Custom Race Dialog -> Event Name Field` (`dialogs.py:191`) | **`JSON-ONLY / NOT READ BY GAME`** | Give a custom race a display title shown in-game. | Would require patching `COMMON.RES#<track>_eves.val` contest name string. | **FMEA:** Overwritten immediately by `cmb_pie_event` text in `main_window.py:1278`; never reaches `eod.exe`. | Either patch contest display string in `COMMON.RES` / `battle_events.cpp` or remove field. |
| 121 | `Custom Race Dialog -> Laps SpinBox` (`dialogs.py:220`) | **`JSON-ONLY / NOT READ BY GAME`** | Change race lap count (e.g., 1 to 20 laps). | Requires patching contest `laps` in `COMMON.RES#<track>_eves.val` or adding `laps=` key to `session_start.cpp`. | **CRITICAL FMEA:** Saved only to `custom_race.json`; `eod.exe` runs the retail lap count every time! | Add `laps=<N>` support in `src/session_start.cpp` (or patch `*_eves.val` VALT) + verify in PIE HUD. |
| 122 | `Custom Race Dialog -> AI Speed Scale SpinBox` (`dialogs.py:230`) | **`JSON-ONLY / NOT READ BY GAME`** | Make AI opponents slower (chill roam) or faster (hardcore racing). | Would require live/launch multiplier in `src/battle_driver.cpp` (`desired` speed / throttle scale). | **CRITICAL FMEA:** Saved only to `custom_race.json`; `battle_driver.cpp` never reads it! | Pass `ai_speed_scale=` via `pie_live_control.ini` / `EOD_AI_SPEED_SCALE` and scale AI throttle/top-speed in `battle_driver.cpp`. |
| 123 | `Custom Race Dialog -> Linked ILIN AI Line Dropdown` (`dialogs.py:238`) | **`PARTIAL / DESYNC`** | Pick which `.ild` racing line the event uses. | Switches editor's active `road_graph` (`main_window.py:2104`), **not** which `.ild` `eod.exe` loads for the contest! | **FMEA:** User links `oval.ild` to `No-Rules Race`, expecting AI to drive the oval, but `eod.exe` still loads `No-Rules Race`'s retail `.ild`. | Either relabel `"Active ILIN Line to Edit"` (read-only binding info) or patch contest `.ild` reference in `COMMON.RES`. |
| 124 | `Custom Race Dialog -> Career Free-Roam Place Dropdown` (`dialogs.py:246`) | **`REAL & VERIFIED`** | Choose starting location (`home`, `dealer`, `garage`, `races`, `shop`, `paint`, `barn`) in Career Overworld (`HomeBase`). | `EOD_DIRECT_START="mode=career;place=<place>"` (`src/session_start.cpp:412-415, 499`). | Only applies when Track is `HomeBase` or Event is `Career Overworld (Free Roam)`. | Launches Career mode at exact chosen facility. |
| 125 | `Vehicle Tuner Dialog -> Live PIE Speed Boost Slider` (`dialogs.py:356`) | **`REAL & VERIFIED`** | Boost/nerf player car acceleration & top speed live in PIE (`0.50x..2.50x`). | `pie_live_control.ini` `speed_boost=` -> `src/battle_driver.cpp:222, 309-321`. | Slider only applies while `gas > 0.2` and only to `player0_car`; does not persist to `.CAR` file. | Smooth live acceleration boost in PIE; zero restart required. |
| 126 | `Vehicle Tuner Dialog -> Inspect Retail Car Dropdown (cmb_car)` (`dialogs.py:366`) | **`STUB / POTEMKIN`** | Select a `.CAR` archive to view/edit its 3D mesh, textures, and `physics.val`. | **NONE** — `cmb_car` has zero connected slots! | **CRITICAL FMEA:** Classic Potemkin widget; choosing any car produces zero UI or file change. | Wire to full **3D Car Studio** loader (Section 5) reading `physics.val`, 3D `MODL`+wheels, and `TEX` skins. |
| 127 | `Vehicle Tuner Dialog -> NASCAR & Custom Cars Tab` (`dialogs.py:374`) | **`STUB / POTEMKIN`** | Import vehicles from *NASCAR: Dirt to Daytona* (`SLUS-20441`) `.ISO`/folder or clone a custom EOD car. | **NONE** — static `QLabel` checking `.is_file()` only! | **CRITICAL FMEA:** Zero file picker, zero car roster selector, zero execute button, zero progress output. | Replace with real interactive **NASCAR `.ISO`/Folder Importer & Custom Car Cloner** tab (Section 5.3). |

---

## 3. Deep Dive: Car Spawns & Markers (Answering the 5 Modder Questions)

This section resolves the single largest conceptual ambiguity in the World Editor today: the `Markers` menu, `Cars -> Place Car Spawn / Place AI Opponent Car Slot`, and `Start Grid` cones.

### Q1: Why would a modder place a Car Spawn or Marker?
Modders place spawns and markers for **five distinct gameplay jobs**:
1. **Custom Starting Grid (Races & Derbies):** Relocate the race start to a custom bridge, barn, hilltop, or reversed track layout with custom facing direction.
2. **Free Roam / Stunt Spawn ("Drop Me Here"):** Spawn immediately in front of a newly built sky-ramp or stunt arena instead of driving 45 seconds from the retail start grid on every PIE iteration.
3. **Battle Arena Respawns:** Control where cars spawn and respawn in Battle / Tag / Fat Kid / Demolition Derby so they don't spawn inside custom walls or off-cliff.
4. **AI Opponent Slots & Vehicle Overrides:** Place specific AI cars (e.g., 3 Buses blocking a bridge, 1 Monster Truck Juggernaut at the top of a hill, or a 10-car NASCAR grid).
5. **Gameplay Gadgets & Triggers:** Place Battle weapon/chicken pickups, jump boost pads, teleporters, or out-of-bounds/ringout zones.

### Q2: What does a placed Spawn / Marker do by default RIGHT NOW if left alone?
**NOTHING IN `eod.exe`. Zero runtime effect.**

Code-verified trace:
1. Clicking `Cars -> Place Car Spawn at View Target` (`main_window.py:372`) calls `add_marker_at_camera("Car")` (`main_window.py:2253-2273`), which appends a `WorldMarker` to `world.markers` and draws a 3D colored cone + arrow in OpenGL.
2. On `Ctrl+Shift+S` (`export_mod_layer`, `project_state.py:3426-3436`), markers of kind `("Spawn", "Start Grid", "Car")` are serialized into `mods/<slug>/custom_race.json` under `"start_grid": [...]`. Markers of kind `"Chicken"`, `"Trigger"`, `"Breaker"`, and `"Marker"` are **not even included in `custom_race.json`** — they exist only in `.eodworld.json`.
3. `git grep custom_race` across `src/` and `runtime/` returns **0 matches**. Neither `eod.exe` nor `pie_host.py` ever reads `custom_race.json` or `.eodworld.json`.
4. **Result:** Leaving a placed Car Spawn alone changes **0 bytes** in the modded `.TRK`, changes **0 arguments** in `EOD_DIRECT_START`, and leaves `eod.exe` spawning every car at the untouched retail location.

### Q3: What does a modder EXPECT a Spawn / Marker to do?
When a modder drops a Car Spawn in an Unreal/Halo Forge-caliber editor, they expect clear, deterministic defaults with zero guesswork:
- **Player Spawn (`Player P1`):** Spawns the player car at that exact `(X, Y, Z)` and `Heading` in **Free Roam / Battle** immediately upon pressing `F5` (with an explicit scope selector: `All Modes`, `Free Roam / Battle Only`, or `Specific Event`).
- **Start Grid Slots (`Grid P1..P12`):** Defines the ordered starting grid for **Races / Derbies** (P1 = player or pole position, P2..P12 = AI/multiplayer slots), automatically oriented down-track along the heading arrow.
- **AI Opponent Car Slot (`AI Slot`):** Spawns an AI car at that pose with optional **Vehicle Override** (e.g., `Default`, `Bus`, `Copcar`, `Cupcar`), **Team/Role** (`Racer`, `Hunter / It`, `Runner`, `Juggernaut`, `Static Prop`), and **Respawn Behavior** (`Initial Only`, `Respawn Pool`, `On-Death Delay`).
- **Safety Invariant:** Automatically grounds to `SCOL` + `0.35 m` clearance (preventing wheels clipping through terrain) and warns if placed inside a solid `SOBL` box or `< 4` Battle respawns exist.

### Q4: How does `SLUS_209.10` / `eod.exe` ACTUALLY read and manage spawns today?
Verified directly from `src/battle_bots.cpp`, `src/battle_events.cpp`, `src/battle_layout.h`, `src/session_start.cpp`, and `docs/GAMEPLAY_MODS_RE.md`:

1. **Battle & Free Roam (`event=Battle`, `kEvent == 0x18000224`):**
   - `SLUS_209.10` loads `*<track>.trk/c_battle.va0` (`VALT` format) and parses `[respawn]` sections into an array of up to **16 records** (`0x44` bytes each) inside `battle_master` at `master + 0x10..+0x450`, with the valid record count stored at `master + 0x450` (`battle_layout::kRespawnCount`).
   - **Hard Engine Panic Invariant:** `0x00167B54` panics with `"battle: must have at least 4 respawn points"` if `num_respawns < 4`! (This is why `src/battle_events.cpp:15` synthesizes a `c_battle.va0` alternate for Welch, and why Demolition Derby `c_kill_derby.va0` has no `[respawn]` sections.)
   - **Initial Placement Hook (`f_00167C70`, `src/battle_bots.cpp:197-219`):** Places car `index` (`0..11`) on respawn record `(rotation + index) % num_respawns`, building the full 3D grounded pose matrix (`0x48` bytes) via native respawn builder `f_001D8F58(out, scratch_xz, scratch_dir)`. For `index >= num_respawns`, `battle_bots.cpp` offsets sideways by `±6.0 m` lanes and re-grounds via `f_001D8F58`.
2. **Races & Circuit Contests (`No-Rules Race`, `Jump Race`, `Figure-8`, `Gauntlet`, etc.):**
   - Starting grid is **NOT** stored as standalone `[respawn]` records in `c_*.va0`. Instead, the engine derives the staggered starting grid from the active **`ILIN` (`*.ild`) node 0** position and forward tangent (`dirX, dirZ`), projecting cars backward along the centerline and grounding onto `SCOL`.
   - Note: `world_loader.py:2664-2689` *synthesizes* 4 visual `Start Grid P1..P4` markers from `road_graph` node 0 for display only — moving those markers does NOT move `ILIN` node 0!
3. **Career Overworld (`HomeBase`):**
   - Spawns at fixed named facility indices (`home`, `dealer`, `garage`, `races`, `shop`, `paint`, `barn`) selected via `EOD_DIRECT_START="mode=career;place=<name>"` (`src/session_start.cpp:412-415`).
4. **Live Player Car State (`0x447458 + slot*4`):**
   - Each active car struct (`0x1C34` bytes) stores right vector at `+0x64..+0x6C`, forward vector at `+0x7C..+0x84`, world position `(X, Y, Z)` at `+0x88..+0x90`, and linear velocity at `+0x104..+0x10C` (`src/battle_driver.cpp:249-251`).

### Q5: How to Make Spawns 100% Real + Design the Adaptive Right-Hand "How It Works & Game-Mode Scope" Card

#### A. Concrete End-to-End Spawn Architecture (Zero Potemkin)
1. **Native Battle/Free Roam Baker (`c_battle.va0` + `pie_spawns.ini`):**
   - On `export_mod_layer()`, compile editor `Spawn` / `Car` / `AI_Car` markers into **both**:
     1. **`c_battle.va0` `[respawn]` records** inside the modded `.TRK` (so standalone `eod.exe` loads them natively without PIE, always padding/retaining at least 4 valid respawn records to satisfy `0x00167B54`).
     2. **`mods/<slug>/pie_spawns.ini`** (lightweight key-value spawn table: `slot=x,y,z,heading_deg,scope,role,vehicle_override`) read by a 35-line hook in **`src/battle_bots.cpp` (`f_00167C70`)** and **`src/battle_driver.cpp` (initial-frame Race/Free Roam pose override via `f_001D8F58`)**!
   - **Why both?** Patching `c_battle.va0` gives pure PS2-native Battle/Free Roam spawns; adding the `f_00167C70` / initial-frame `f_001D8F58` hook gives **exact slot ordering (`P1` always spawns at `Player Spawn`, `AI_Car_1` always spawns at slot 1)**, **all-mode support (Races, Derbies, Jump Races, Free Roam)**, and **per-slot vehicle/team roles** without corrupting `ILIN` lap-distance math!
2. **Start Grid <-> ILIN Node 0 Coupling:**
   - Moving a retail-derived `Start Grid P1` offers an explicit toggle: **`Anchor to Race Start Override (pie_spawns.ini)`** (default, preserves AI racing line) vs. **`Shift ILIN Node 0 Origin`** (bakes directly into `.ild`).

#### B. Adaptive Right-Hand "How It Works & Game-Mode Scope" Card (Properties Inspector)
Every selectable entity in `properties_panel.py` (Object, Spawn/Marker, Road Node, Barrier, Environment, Forge Gadget) must display a compact, collapsible **How It Works & Scope Card** at the top of the Right Inspector so **nothing in the editor is ever ambiguous**:

```text
┌──────────────────────────────────────────────────────────┐
│ ● BAKED TO GAME  |  Scope: Free Roam + Battle + Races    │
├──────────────────────────────────────────────────────────┤
│ WHAT THIS DOES IN EOD.EXE:                               │
│ • Spawns Player 1 (slot 0) here facing the blue arrow.   │
│ • Grounded automatically via PS2 f_001D8F58 (+0.35 m).   │
│ • Writes: c_battle.va0 [respawn] + pie_spawns.ini        │
│                                                          │
│ GAME-MODE SCOPE:  [ All Modes (Recommended)        ▼ ]   │
│ ROLE / SLOT:      [ Player 1 (Primary Spawn)       ▼ ]   │
│ VEHICLE OVERRIDE: [ Use Toolbar Car (Algar)        ▼ ]   │
│ RESPAWN POOL:     [x] Allow mid-match Battle respawns    │
└──────────────────────────────────────────────────────────┘
```

**Status Pill Color Contract (Anti-Potemkin UI Badge):**
- **Green (`● BAKED TO .TRK / LIVE PIE`):** Verified binary patch or C++ hook active in `eod.exe`.
- **Blue (`● LIVE PIE ONLY`):** Applied via `pie_live_control.ini` / `EOD_DIRECT_START` during PIE (not written to retail `.TRK`).
- **Amber (`● VIEWPORT ONLY`):** Editor visualization aid only; explicitly states *"Does not change `eod.exe`."*
- **Red (Forbidden in Shipped UI):** No control may ship with an unimplemented baker.

---

## 4. Halo Forge Mode Feature Blueprint for EOD World Editor

Transforming EOD World Editor from a raw PS2 asset inspector into an effortless **Halo: Reach / Halo Infinite Forge** sandbox requires four focused subsystems built directly atop existing PS2 primitives (`GRAF`, `SOBL`, `BPPT`, `obstacle.val`, `c_*.va0`, `battle_driver.cpp`):

### 4.1 Object Physics Modes (`Normal`, `Fixed`, `Phased`)
Every editor-placed or modified object gains a **`Physics Mode`** selector in the Properties Panel (and quick-cycle hotkey `Shift+P` when an object is selected), mapped cleanly to PS2 invariants:

| Forge Mode | Modder Intent | PS2 Binary Mapping | Placement & Rotation Rules |
|---|---|---|---|
| **`Normal`** (Dynamic / Breakable) | Crates, barrels, hay bales, traffic cones, wrecks that cars can smash, push, or launch. | `obstacle.val` dynamic/destructible `[dyno]` template + `c_*.va0` `$NNNN$object` (non-`wall` dyno type). | Ground-snapped by default; respects gravity in `eod.exe`; full crash physics. |
| **`Fixed`** (Solid World Geometry) | Ramps, bridges, buildings, concrete barriers, stadium props that stay rock-solid when hit at 120 mph. | `obstacle.val` `type = wall` (AABB/OBB) OR synthesized `track.sol` `SOBL` oriented box (`BOX` record) + `c_*.va0` visual `.mod`. | Can be placed anywhere (including mid-air sky bridges!); ignores gravity; solid collision; blocks overlaps during standard snap unless shifted. |
| **`Phased`** (Interpenetrating / Crazy-Angle Stunts) | **Sky-ramps, banked loops, sunken half-ramps, wall-rides, overlapping modular roads** at arbitrary pitch/roll (`±180°`). | Visual `.mod` baked with full 3D Euler/quaternion matrix (`VIF` vertex bake or rotated `c_*.va0` frame) + **oriented multi-slab `SOBL` deck boxes** (or `No Collision / Visual Only` sub-option for decorative clipping). | **Zero placement restrictions:** allowed to intersect terrain, other objects, and arbitrary sky heights; arbitrary pitch/roll/yaw; optional `Collision: Solid Deck (SOBL)` vs `Visual Only (Ghost)`. |

> **Key PS2 Unlock for Phased Sky-Ramps:** Retail `obstacle.val` `type = wall` uses an upright box (`dx/dy/dz`), which breaks on pitched/banked sky-ramps! For any `Phased` or `Fixed` object with `|pitch| > 5°` or `|roll| > 5°`, the baker automatically generates a **rotated `SOBL` oriented box** (`track.sol` `BOX` records natively carry a full 3×3 orientation matrix at `+0x00..+0x23`!) so cars smoothly drive up angled sky-ramps and corkscrew bridges!

### 4.2 Smart Compound Grouping & Industry-Standard Selection Conventions (UE / Blender / Lumion)
Retail PS2 tracks split logical structures across multiple GRAF leaves (`ramp-in` + `ramp-top` + `ramp-side`), distance LOD twins (`fence` + `lod-f`), and paired `SOBL` boxes. Today, clicking a ramp picks one tiny triangle patch and breaks it apart.

1. **Auto-Detected PS2 Compound Assemblies (Smart Groups):**
   - At track load (`world_loader.py`), run a fast spatial + naming cluster pass (`<15 ms`) that automatically tags logical compounds (`compound_id`):
     - **Multi-part Ramps:** Co-located `optional-terrain` or `static` leaves sharing ramp texture/prefix (`ramp*`, `jump*`, `stunt*`) within `1.5 m` AABB expansion -> grouped as `Ramp Assembly (N parts)`.
     - **Fences / Guardrails / Walls:** Visual fence leaf + co-located `lod-*` / billboard twin + overlapping vertical `SOBL` barrier indices -> grouped as `Barrier Assembly` so **Move, Rotate, Scale, Duplicate, AND Delete** all act on the entire unit!
2. **Industry-Standard Shortcuts (SketchUp / Blender / 3ds Max / Unreal Engine — Fixing `Ctrl+G` Conflict):**
   - **`Tab`** -> **Cycle Transform Tool** (`Move` -> `Rotate` -> `Scale` -> `Move`) rapidly without leaving the viewport.
   - **`Alt + Drag` on Gizmo** -> **Duplicate & Drag in One Motion** (UE / 3ds Max / Lumion gold standard: clones the active selection or compound group at drag start and slides the clone directly along the dragged axis/plane in a single undoable command!).
   - **`Ctrl+G`** -> **Group Selected** (wraps `selected_objects` into a `WorldGroup` with shared centroid pivot; undoable `GroupObjectsCommand`).
   - **`Ctrl+Shift+G`** -> **Ungroup Selected** (dissolves user or auto compound group).
   - **`End`** -> **Snap Selected to Ground** (remains on `End`, freeing `Ctrl+G` for Grouping — standard across Blender/UE/Figma/Photoshop).
   - **`Ctrl+A`** -> **Select All Visible** (respects active Event Template & Layer Visibility; ignores locked terrain unless Static layer is unlocked).
   - **`Ctrl+D`** -> **Duplicate Selected** (upgraded to duplicate full multi-selection, compound groups, AND markers in one `QUndoCommand`, preserving relative offsets).
3. **Double-Click Sub-Part Drill-In (UE / SketchUp / Blender Convention):**
   - **Single-Click** any piece of a compound group -> selects the **whole compound assembly** (cyan group bounding box + unified pivot gizmo).
   - **Double-Click** a selected group -> **drills into sub-part edit mode** (dimming non-group objects slightly; user can tweak or retexture a single `ramp-in` leaf).
   - **`Esc` or Click Empty Space** -> exits drill-in back to top-level selection.

### 4.3 Smart Barriers: Sky-Wall Clamp vs. Walls Noclip
Replace the destructive single button `Remove / Flatten Track Barriers (SOBL)` with **two distinct, surgical, undoable modes** under `Collision` and the Barrier Inspector:

1. **`Clamp Invisible Sky-Walls to Asset Height` (Recommended Default):**
   - **Algorithm:** For each vertical `SOBL` box (`above_ground >= 4.0 m`), find all overlapping visual `WorldObject` AABBs (fences, walls, hay bales, stands) within `2.0 m` XZ margin:
     - If overlapping visual assets exist: clamp `SOBL` top height to `max(visual_top_y - ground_y) + 0.25 m` (clamped to `[1.2 m, 4.5 m]`). **Result:** Real concrete walls and fences stay 100% solid when you hit them head-on, **but launching off a ramp sails cleanly OVER the wall instead of slamming into a 25-meter invisible ceiling!**
     - If **no** visual asset overlaps (pure invisible perimeter wall): flatten to `-0.5 m` (or keep at `0.0 m` configurable).
2. **`Walls Noclip (Keep Ground Solid)` (Total Sandbox Freedom):**
   - Flattens **all** vertical `SOBL` boxes (`vertical == True`) to `-0.5 m` while strictly preserving all horizontal/drivable `SCOL` terrain, `BPPT` ramps, and `SOBL` bridge decks (`vertical == False`).
   - Includes a one-click **`Restore Retail Barriers`** button (and full `Ctrl+Z` undo).

### 4.4 Game-Mode Labels & Forge Gadgets (Free Roam / Tag / Cat & Mouse / Fat Kid)
Extend `WorldMarker` into a typed **Forge Gadget** system backed by `mods/<slug>/pie_forge.ini` (parsed live by `src/battle_driver.cpp` + `src/battle_bots.cpp` alongside native `c_battle.va0` `[respawn]` baking):

| Forge Gadget | Properties | Runtime Behavior in `eod.exe` (`battle_driver.cpp` / `battle_bots.cpp`) |
|---|---|---|
| **Spawn Point (`Spawn`)** | `scope` (`All`, `FreeRoam`, `Battle`, `Race`, `Tag`, `CatMouse`, `FatKid`), `role` (`Player`, `AI`, `Hunter/It`, `Runner/Mouse`, `Juggernaut`), `slot_index` (`0..11`), `vehicle_override`, `respawn_delay_s` | `f_00167C70` & initial-frame hook place matching car slot at `(X, Y, Z, heading)` grounded via `f_001D8F58`. Role-tagged spawns put the Juggernaut/Bus at the top of the hill and Runners at the bottom! |
| **Teleporter Pair (`Teleporter`)** | `channel` (`Alpha`..`Golf` / `0..7`), `mode` (`Sender`, `Receiver`, `Two-Way`), `radius_m`, `preserve_velocity` (`bool`), `exit_boost_mps` | `battle_driver.cpp` checks `dist(car, sender) < radius_m` (with `1.5s` cooldown), teleports `car+0x88..+0x90` to paired `Receiver` + aligns forward matrix (`+0x64..+0x84`) + preserves/rotates velocity vector (`+0x104..+0x10C`)! Enables sky-island teleports & infinite jump loops. |
| **Forge Volume (`Volume`)** | `volume_kind` (`Boost Pad`, `Low-Gravity Jump`, `Kill / Reset Zone`, `Safe Zone`, `Finish / Checkpoint`), `half_extents (hx, hy, hz)`, `heading_deg`, `strength` | Oriented box test in `battle_driver.cpp`: `Boost Pad` adds forward impulse along volume heading; `Kill / Reset` triggers respawn at nearest valid spawn; `Safe Zone` protects Runner/Mouse from Tag/Fat Kid infection; `Checkpoint` tracks custom stunt/race lap progress. |
| **Custom Mode Rules (`ModeConfig`)** | `mode` (`free_roam`, `tag`, `cat_mouse`, `juggernaut`), `it_speed_mult`, `runner_speed_mult`, `tag_radius_m`, `juggernaut_car` (`Bus`/`Tow`), `infect_on_tag` (`bool`) | Unifies `ARCADE_MODES` across `pie_host.py`, `CustomRaceDialog`, and `src/battle_driver.cpp` (fixing both the string-vs-int live switch bug and implementing real **Cat & Mouse** + **Fat Kid / Juggernaut** role speed/vehicle/tag rules!). |

### 4.5 Consolidated PIE Strip, True Unreal `F8` Free Cam, Real Sunset/4K Skybox Bake & Bulk Texture Packs
Live QA screenshots uncovered four high-impact workflow gaps that belong directly in the Forge/Studio UX contract:

1. **Single Consolidated PIE Strip (Zero Duplicate Controls):**
   - Remove the redundant stacked double toolbar! While PIE is active, morph the top toolbar's PIE group into **one unified PIE control strip** (or hide the top PIE buttons while `PieHostWidget.bar` is mounted) containing only verified, working controls: `● PIE RUNNING`, `Mode / Car / AI`, `Free Cam (F8)`, `Drop Car Here (Ctrl+F8)`, `AI Auto-Drive`, `Arcade Mode`, `Restart`, `Pop Out`, `Stop PIE (Shift+Esc)`. Every button on the strip uses `Qt.FocusPolicy.NoFocus` so clicking a button never steals keyboard focus from `eod.exe`.
2. **Unreal Engine / Halo Forge `F8` Free Cam + `Ctrl+F8` Drop Car Here:**
   - **Enter Free Cam (`F8`):** Hooks the active PS2 camera view matrix in `eod.exe` (and/or `runtime/render/native_renderer.cpp` camera override) initialized from the live chase camera pose.
   - **Exact World Editor Controls:** Inherits 1:1 editor navigation — **`RMB + Mouse Look`**, **`WASD`** horizontal fly, **`Q / E`** down/up, **`Shift`** 2.8× speed boost, **`Mouse Wheel`** fly-speed adjust.
   - **Two Explicit Exit Actions:**
     - **`F8` (Return to Car):** Releases camera override and snaps view seamlessly back behind `player0_car`.
     - **`Ctrl+F8` (Drop Car Here):** Teleports `player0_car` (`0x447458`) to the camera's live `(x, y, z, yaw)` grounded safely via native `f_001D8F58` (zeroing velocity `car+0x104..+0x10C` so the car lands cleanly), releases Free Cam, and resumes driving immediately!
3. **Real In-Game Sunset / Environment Bake + 4K/8K HDR/PNG Equirectangular Skybox Importer:**
   - **Fixing Viewport-Only Sunset (`#103`, `#105`, `#106`):** Live QA confirmed selecting **Sunset** changed the editor OpenGL sky while `eod.exe` still loaded daytime Woodley! To make presets and custom skies 100% real in-game:
     - **Sky Presets (Sunset / Night / Dawn / Overcast / Custom Tint):** At `export_mod_layer()`, bake graded/tinted replacements for all `sky01A..16B` dome tiles into `HOSTTEX/*.eodhtx` + `replacements.tsv` (exactly like `Borrow Sky From Track` already does!), **AND** write `mods/<slug>/env_override.ini` (`sun_dir`, `sun_color`, `ambient_intensity`, `fog_color`, `fog_start`, `fog_end`, `fog_density`) consumed by `runtime/render/native_renderer.cpp` & `gs_vulkan.cpp` so sun lighting and fog match 1:1 in `eod.exe`!
     - **Custom 4K/8K HDR/PNG Equirectangular Skybox Importer (`Import Equirectangular Skybox...`):** Accepts any `.png` / `.jpg` / `.hdr` / `.exr` 360° equirectangular panorama, projects it cleanly onto the track's `sky01A..16B` UV dome patches (using the exact `sky` leaf UVs from `track.grf`), tone-maps HDR -> sRGB RGBA8, builds validated `EODHTX1` mip chains (`k=4..8` up to 4K/8K Vulkan `HOSTTEX`), and previews it immediately in both the OpenGL viewport and `eod.exe` Vulkan PIE!
4. **One-Click Bulk Texture Pack Workflow (`Mesh` / `Explorer -> Textures`):**
   - **`Export All Track Textures to Folder...`:** Dumps every decoded track texture (`<tex_name>.png`) plus a `manifest.json` (original dimensions, CLUT/direct-color flag, mip eligibility) into a chosen folder in one background `QThreadPool` job — ready for AI upscaling (ESRGAN/Real-ESRGAN) or batch Photoshop retexturing.
   - **`Import Texture Folder (HOSTTEX)...`:** Scans a folder for matching `<tex_name>.png` files, validates aspect ratio / scale compatibility, applies all matches in a single undoable `BatchReplaceTexturesCommand`, refreshes OpenGL textures live, and queues full `HOSTTEX/*.eodhtx` + `replacements.tsv` export on next `Ctrl+Shift+S` / `F5`!

---

## 5. Real 3D Car Studio & NASCAR: Dirt to Daytona Importer Spec (macOS SwiftUI Parity)

The existing Windows `VehicleTunerDialog` (`dialogs.py:320-403`) is a hollow stub compared to the macOS SwiftUI Creation Kit (`tools/editor_macos/`) and the mature Python backend (`tools/modding_backend.py`, `tools/formats/valt_schema_rules.py`, `tools/import_nascar_cars.py`, `tools/nascar_roster.py`, `tools/create_custom_car.py`).

### 5.1 Exact macOS SwiftUI Physics & Handling Parity Map
In `tools/editor_macos/Sources/EODModdingSuite/Views/MainView.swift:220-247` + `tools/modding_backend.py:32-39` + `tools/formats/valt_schema_rules.py:146-253`, every retail `.CAR` archive contains a binary **`physics.val`** (`VALT` v11) read natively by `SLUS_209.10` (note: legacy `SETU` `.gar` files are verified **unused** by EOD).

The Windows **3D Car Studio** must expose **both** the **Quick Tuners (macOS Featured 6 + Essential Handling)** AND the **Full Verified `physics.val` Sheet** (filtering out `used=0` dead source-engine fields by default, with an `"Show Unused Legacy Keys"` toggle):

| Section (`physics.val`) | Key | Plain-Language Label | Unit | Verified PS2 Reader (`SLUS_209.10`) | macOS Featured? |
|---|---|---|---|---|---|
| **`Chassis`** | `mass` | Car weight | `lb` | `0x185470` + Part Shop | **Yes** |
| **`Chassis`** | `grip` | Tyre grip multiplier | `x` | Tyre setup `0x1B...` | **Yes** |
| **`Chassis`** | `damage_tweak` | Collision damage multiplier (Armor) | `x` | Crash damage (lower = tougher armor) | **Yes** |
| **`Chassis`** | `spring_factor` | Suspension stiffness factor | `x` | Car setup `0x185470` | Extended |
| **`Chassis`** | `brake_factor` | Brake strength factor | `x` | Car setup `0x185470` | Extended |
| **`Chassis`** | `cm_height` | Centre of mass height | `in` | Car setup `0x185470` | Extended |
| **`Chassis`** | `wheelbase`, `ftrack`, `rtrack` | Wheelbase / Front & Rear Track Width | `in` | Car setup `0x185470` + 3D wheel placement | Extended |
| **`Chassis`** | `mx`, `my`, `mz` | Roll / Spin / Pitch inertia | `lb·in²` | Car setup `0x185470` | Extended |
| **`Chassis`** | `drag`, `air_drag` | Air drag coefficient | scalar | Aero top-speed limiter | Extended |
| **`Chassis`** | `slip`, `load_factor` | Tyre slip / load setting | scalar | Car setup `0x185470` | Extended |
| **`Chassis`** | `model_offset`, `chase_camera_dist`, `chase_camera_height` | 3D model offset & chase camera | `in` | Car renderer & camera | Extended |
| **`EngineNormal`** *(+ `EngineRestrictor`)* | `power_max` | Peak power | `hp` | `0x185470` + Part Shop | **Yes** |
| **`EngineNormal`** | `torque_max` | Peak torque | `lb-ft` | `0x185470` + Part Shop | **Yes** |
| **`EngineNormal`** | `redline` | Redline | `rpm` | `0x185470` | **Yes** |
| **`EngineNormal`** | `power_rpm`, `torque_rpm`, `idle_speed`, `drag` | Power/Torque RPM, Idle, Engine Braking | `rpm` / scalar | `0x185018` / `0x185470` | Extended |
| **`EngineNormal`** | `engine_sound` | Engine sound table (`.val` in `RACE`) | `file` | Audio loader | Extended |
| **`Transmission`** | `num_gears` | Number of forward gears | count | `0x186DC8` | Extended |
| **`Transmission`** | `g1`..`g6` (`g#`) | Individual gear ratios | `x` | `0x186DC8` | Extended |
| **`Transmission`** | `rear_end_ratio` | Final drive ratio (default `5.0`) | `x` | `0x186DC8` | Extended |
| **`Transmission`** | `torque_balance` | Drive torque split (`0`=FWD, `0.5`=AWD, `1`=RWD) | `0..1` | Drivetrain solver | Extended |
| **`Front` / `Rear`** *(+ `FrontDirt` / `RearDirt`)* | `tire_diameter` | Outer tyre diameter | `in` | `0x185470` + wheel preview | Extended |
| **`Front` / `Rear`** | `wheel_class` | Wheel model class (`com`, `mid`, `mus`, `bus`, `busrear`, `ncu`, `npc`) | class | Wheel loader | Extended |
| **`Front` / `Rear`** | `ride` | Suspension ride height range | `in` | Car preview & stance | Extended |
| **`Front` / `Rear`** | `magic_b`, `magic_c`, `magic_d`, `magic_e` | Pacejka Magic Formula tyre curve | scalar | Tyre solver | Advanced |
| **`Front` / `Rear`** | `long_stiff`, `long_frict`, `camber_stiff`, `camber_peak`, `camber_grip`, `ref_width`, `frict_load#`, `frict_k#`, `stiff_load#`, `stiff_c#` | Full longitudinal, camber & load grip curves | `lb`/`deg`/`mm` | Tyre solver | Advanced |

### 5.2 Windows 3D Car Studio Layout & Live Capabilities
Replace `VehicleTunerDialog` with a dedicated, non-blocking **`CarStudioDialog`** (accessible from `Home Hub -> Vehicle / NASCAR Studio`, `Cars -> 3D Car Studio...`, and Toolbar `Car Studio` — **works with or without a track loaded!**):

1. **Left Pane — Car Catalog & Cloner:**
   - Lists all 37 retail `.CAR` archives (`DATA/CAR/*.CAR`) + any modded/NASCAR cars in `mods/<Project>/DATA/CAR/`.
   - **`Clone as Custom Car...`** button wrapping `tools/create_custom_car.py` (name <= 8 chars for native ISO 8.3 lookup `f_00331708`, base chassis, class `compact`/`midsize`/`muscle`, car number) -> builds valid `.CAR` + `COMMON.RES` + `CT.RES` + `UICAR` entries into `mods/<Project>/`.
   - **`Export OBJ for Blender`** (`tools/vehicle_export.py`) & **`Re-import Edited OBJ`** (`tools/vehicle_import.py`).
2. **Center Pane — Interactive 3D Car + Chassis OpenGL Viewport:**
   - Reuses existing `viewport_gl.py` OpenGL 3.3 renderer + `vehicle_export.py` (`wheel_specs`) / `nascar_preview.py` (`wheel_centers`) to render:
     - Full body `MODL` (LOD 0..3 switcher) + **4 positioned wheels** (`wheelbase`, `ftrack`, `rtrack`, `tire_diameter`, `ride` update live in 3D as sliders move!).
     - Orbit camera + turntable spin + wireframe toggle + damage-bone overlay (`bone.mvl` inspection).
     - **Click-to-Retexture 3D Panels:** Clicking any body panel/window/wheel mesh highlights its `TEX`/`.tm0` texture in the Livery Strip and opens `Replace PNG...` (writing both native CLUT `TEX`/`.tm0` repack AND high-res Vulkan `HOSTTEX` `.eodhtx`, matching macOS `MainView.swift:149-171`).
3. **Right Pane — 3 Tabs:**
   - **Tab 1: `Physics & Handling (physics.val)`** — Top section shows the **6 macOS Featured Sliders** (`Weight`, `Peak Power`, `Peak Torque`, `Redline`, `Tyre Grip`, `Armor / Damage Tweak`) + `Suspension Stiffness`, `Brake Strength`, `Drive Split (FWD/AWD/RWD)`, `Final Drive`, and `Gear Ratios (g1..gN)` with original retail values shown underneath (`Original 3100 lb`), full Undo/Redo, **`Save to Mod .CAR`**, and **`Test Drive in PIE`**.
   - **Tab 2: `Live PIE Tuner`** — Real-time hot-tuning while PIE is running (`speed_boost` + newly wired `grip_boost` & `ride_height_delta` in `src/battle_driver.cpp`).
   - **Tab 3: `NASCAR: Dirt to Daytona Importer`** — Real one-click importer (detailed below).

### 5.3 Real NASCAR: Dirt to Daytona (`SLUS-20441`) `.ISO` / Folder Importer Pipeline
Today `tools/import_nascar_cars.py` and `tools/nascar_roster.py` only accept an extracted `--nascar-data <path/to/DATA>` directory via CLI. The Windows Car Studio must provide a turnkey UI:

1. **Source Picker (Supports BOTH `.ISO` and Extracted Folder):**
   - **Browse `.ISO` Image (`*.iso`)** OR **Browse Extracted `DATA/` Folder**.
   - **Built-in Pure-Python ISO9660 Extractor (Zero External Dependencies):** Add a small clean-room ISO9660 reader (`tools/iso9660_reader.py`, ~120 lines parsing PVD sector 16 + directory records — no `pycdlib`/`7z` install required!) that verifies `SLUS_204.41` on the disc, lists available archives (`DATA/CAR/STOCK.CAR`, `TRUCK.CAR`, `MODIFIED.CAR`, `HOBBY.CAR`, `PACECAR.CAR`, `DATA/RES/PAINT256.RES`, `TRUCKSET.RES`, `MODFYSET.RES`, `HOBBYSET.RES`), and extracts only the selected archives into a temp cache on demand.
2. **Interactive 5-Family Vehicle & Paint Selector (matching `tools/nascar_roster.py:28-42`):**
   - **Family Picker:**
     1. `Cup / Stock Car` (`STOCK.CAR`, `PAINT256.RES`, `cup.val`) — supports all 4 body masks (`Chevy 0x1867`, `Ford 0x1B07`, `Pontiac 0x1E07`, `Dodge 0x1999`) -> chassis `Cupcar`/`CupFord`/`CupPont`/`CupDodge`.
     2. `Craftsman Truck` (`TRUCK.CAR`, `TRUCKSET.RES`, `truck.val`) -> `NTruck`.
     3. `Featherlite Modified` (`MODIFIED.CAR`, `MODFYSET.RES`, `modified.val`) -> `NMod`.
     4. `Dirt / Hobby Stock` (`HOBBY.CAR`, `HOBBYSET.RES`, `dirt.val`) -> `NHobby`.
     5. `NASCAR Pace Car` (`PACECAR.CAR`, `Pacecar.tex`) -> `NPace`.
   - **Livery / Paint Browser:** Decodes `PAINT256.RES` / `*SET.RES` `.tex` entries into clickable thumbnails + live 3D body preview (`nascar_preview.py`) before importing!
   - **Target Settings:** Custom Chassis Name (`<=8 chars`), Car Number, Donor Base (`Algar`), EOD Class (`compact`/`midsize`/`muscle`), or **`Import Full 5-Family Roster`**.
3. **One-Click Convert & Auto-Register (Background `QThreadPool` Worker):**
   - Invokes `import_nascar_cars.py` / `nascar_roster.py` functions directly in a `FunctionWorker` with live progress bar & Output Log stream:
     1. VIF transcoding (NASCAR VU1 layout `MSCAL 0x2D` -> EOD VU1 layout),
     2. `TEX` v9 -> v10 conversion + livery `.tex`/`.tm0` + `CT.RES` / `UICAR` thumbnails,
     3. `bone.mvl` & shadow generation + `physics.val` merge + `nascar_wheels.py` (`ncu`/`npc`),
     4. Output written non-destructively into `mods/<Project>/DATA/CAR/` + `mods/<Project>/DATA/RES/`.
   - **Post-Import Hook:** Automatically refreshes Explorer's `Cars & Wrecks` catalog, refreshes Toolbar `cmb_pie_car`, selects the new NASCAR car in the 3D Car Studio viewport, and enables one-click **`Test Drive in PIE`**.

---

## 6. Prioritized Implementation Lanes (Ready for Parallel Short-Context Workers)

Three clean, file-disjoint work packets designed for parallel execution with zero merge conflicts:

### Lane A — Anti-Potemkin Runtime & Baker Closure (Spawns, Collision Boxes, PIE IPC, Custom Race & Smart Barriers)
- **Goal:** Eliminate all 24 `JSON-ONLY` drops and 7 runtime-desync bugs so every World Editor authoring control works end-to-end in `eod.exe`.
- **Owned Files:**
  - `src/battle_driver.cpp`
  - `src/battle_bots.cpp`
  - `src/session_start.cpp`
  - `runtime/render/native_renderer.cpp` *(env/fog/sun + Free Cam matrix hook)*
  - `tools/world_editor/project_state.py`
  - `tools/world_editor/pie_host.py`
  - `tools/world_editor/test_editor.py`
- **Concrete Deliverables:**
  1. **Fix All Live PIE Controls & Free Cam (`pie_host.py` + `battle_driver.cpp` + `native_renderer.cpp`):**
     - Implement true **`F8` Free Cam** (editor `RMB + WASD/QE/Shift/Wheel` controls) + **`F8` Return to Car** + **`Ctrl+F8` Drop Car Here** (teleports `player0_car` via `f_001D8F58` and zeros velocity).
     - Move `drive_player_along_ilin()` **before** `f_001A2D20_orig()` so `HumanDriver` stops overwriting AI Auto-Drive inputs!
     - Fix Win32/SDL2 `focus_embedded_game()` (`NoFocus` strip buttons + full scancode `lParam` forwarding) and collapse duplicate PIE toolbars into ONE clean strip.
     - Accept both string & int for `arcade_mode`; implement `cat_mouse`, `grip_boost`, `ride_height_delta`, and `ai_speed_scale` in `battle_driver.cpp`.
  2. **Fix `Make Collision` Drop (`project_state.py`):** Feed `world.collision.custom_boxes` (plus angled `Phased`/`Fixed` ramp `SOBL` OBBs) into `_append_sobl_boxes()` in `export_mod_layer()` and include `custom_boxes` in delta cache signature.
  3. **Real Sunset / Env Bake & 4K/8K Equirectangular Skybox + Bulk Texture Pack (`project_state.py` + `native_renderer.cpp`):** Bake tinted `sky01A..16B` `HOSTTEX` tiles + `env_override.ini` (sun/ambient/fog read by `native_renderer.cpp`); add equirectangular HDR/PNG -> `sky*.eodhtx` projector and `export_all_textures()` / `import_texture_folder()` batch HOSTTEX pipeline.
  4. **Make Spawns & Forge Gadgets Real (`project_state.py` + `battle_bots.cpp` + `battle_driver.cpp`):** Bake `Spawn`/`Car`/`AI_Car` markers into `c_battle.va0` `[respawn]` (keeping `>= 4` records) + write `pie_spawns.ini` & `pie_forge.ini` (Teleporters, Boost/Kill/Safe Volumes); hook `f_00167C70` and initial-frame placement via `f_001D8F58`.
  5. **Make Custom Race Laps Real (`session_start.cpp` + `pie_host.py`):** Support `laps=<N>` in `EOD_DIRECT_START` (and clamp `CustomRaceDialog.spn_ai` to `0..12`).
  6. **Smart Barriers & ILIN Honesty (`project_state.py` + `tools/formats/ilin.py`):** Add `plan_clamp_sky_walls_to_assets()` alongside `Walls Noclip (Keep Ground Solid)`; either implement binary ILIN node add/delete/width encoding OR guard unsupported ILIN topology edits.
  7. **Eliminate `310.4 ms` Viewport Frame Spike (shared perf audit handoff with Lane B):** Profile & split static vs. dirty dynamic VBOs in `viewport_gl.py` so object drags/bakes stay `< 16.6 ms`.
- **Verification Gate:** `pytest` / `test_editor.py` asserts binary `SOBL` box append, `c_battle.va0` respawn write + `>=4` invariant, `pie_spawns.ini` / `pie_forge.ini` / `env_override.ini` schema, bulk texture folder round-trip, and live `eod.exe` PIE boot check (`Free Cam`, `Drop Car Here`, `AI Auto-Drive`, `Sunset` sky visible in Vulkan).

### Lane B — Halo Forge UX, Smart Grouping, Adaptive Inspector & `<16.6 ms` Viewport Performance (`viewport_gl.py`, `main_window.py`, `properties_panel.py`, `world_loader.py`)
- **Goal:** Deliver the Halo Forge editing experience, eliminate `QUndoStack` bypasses, add `Tab` / `Alt+Drag` / `Ctrl+G` / `Ctrl+Shift+G` / `Ctrl+A` / `Ctrl+D`, consolidate duplicate PIE toolbars, add Bulk Texture UI & Adaptive Scope Card, and crush the `310.4 ms` viewport frame spike.
- **Owned Files:**
  - `tools/world_editor/world_loader.py`
  - `tools/world_editor/viewport_gl.py`
  - `tools/world_editor/properties_panel.py`
  - `tools/world_editor/main_window.py`
  - `tools/world_editor/explorer_panel.py`
- **Concrete Deliverables:**
  1. **Smart Compound Auto-Grouping & SketchUp/Blender/3ds Max/UE Shortcuts:** Auto-cluster multi-part ramps and `fence + lod-f + SOBL` on track load; add **`Tab`** (cycle Move/Rotate/Scale), **`Alt+Drag`** on gizmo (Duplicate & Drag in one motion), **`Ctrl+G`/`Ctrl+Shift+G`** (Group/Ungroup, freeing `Ctrl+G` by keeping Snap-to-Ground on `End`), **`Ctrl+A`** (Select All Visible), **`Ctrl+D`** (multi-select/group/marker duplicate), and double-click sub-part drill-in.
  2. **Object Physics Mode Selector (`Normal` / `Fixed` / `Phased`):** Add dropdown + `Shift+P` hotkey in Object Inspector; disable pitch/roll clamping in `Phased` mode.
  3. **Adaptive Right-Hand "How It Works & Game-Mode Scope" Card + Single PIE Strip:** Add color-coded status pill (`BAKED TO .TRK` / `LIVE PIE` / `VIEWPORT ONLY`) and scope/role/vehicle controls to Properties Inspector; wrap marker metadata & environment edits in `QUndoCommand`s; prune/hide any unbacked Potemkin road/breaker buttons; collapse stacked duplicate PIE toolbars into ONE clean strip.
  4. **Smart Barrier UI Split, Equirectangular Skybox & Bulk Texture Pack UI:** Expose `Clamp Sky-Walls to Asset Height` vs. `Walls Noclip (Keep Ground Solid)` in `Collision` menu; add `Import Equirectangular Skybox (4K/8K HDR/PNG)...` in Environment Inspector; add `Export All Track Textures to Folder...` and `Import Texture Folder (HOSTTEX)...` in `Mesh` / `World` menu.
  5. **Crush `310.4 ms` Frame Spike & Vectorize Picking (`viewport_gl.py`):** Separate immutable static terrain VBOs from a small dynamic/selected-object VBO (eliminating 6,000-object Python loops on `mark_scene_dirty()` -> `< 16.6 ms` frame budget!); vectorize `_pick_object()` AABB broadphase with NumPy arrays; upgrade `_pick_sobl_barrier()` to exact OBB ray intersection; maintain incremental `active_count` instead of per-120ms full list scans.
- **Verification Gate:** Headless Qt tests in `test_editor.py` verify compound group Move/Delete/Undo, double-click drill-in, `Tab`/`Alt+Drag`/`Ctrl+G`/`Ctrl+Shift+G`/`Ctrl+A`/`Ctrl+D`, OBB barrier picking, `< 16.6 ms` dirty-edit redraw timing, and zero `QUndoStack` bypasses.

### Lane C — Windows 3D Car Studio & NASCAR `.ISO`/Folder Importer (macOS SwiftUI Parity)
- **Goal:** Replace the stub `VehicleTunerDialog` and `HomeHub -> Vehicle / NASCAR Importer` with a real standalone 3D Car Studio achieving 100% parity with `tools/editor_macos/` plus native `.ISO` NASCAR importing.
- **Owned Files:**
  - `tools/iso9660_reader.py` *(new pure-Python ISO9660 reader)*
  - `tools/world_editor/car_studio_dialog.py` *(new 3D Car Studio widget)*
  - `tools/world_editor/dialogs.py` *(wire `VehicleTunerDialog` -> `CarStudioDialog` + clean up `CustomRaceDialog`)*
  - `tools/world_editor/home_hub.py` *(launch `CarStudioDialog` directly without forcing a track load)*
- **Concrete Deliverables:**
  1. **Pure-Python ISO9660 Reader (`tools/iso9660_reader.py`):** Reads `SLUS-20441` `.ISO` images directly without external tools; extracts requested `.CAR`/`.RES` archives on demand.
  2. **3D Car + Chassis Viewport (`car_studio_dialog.py`):** OpenGL preview of body `MODL` + 4 live-positioned wheels (`wheelbase`, `ftrack`, `rtrack`, `tire_diameter`, `ride`) + click-to-retexture panel picker writing native `TEX`/`.tm0` + `HOSTTEX` `.eodhtx`.
  3. **Full `physics.val` Editor (macOS Parity):** Exposes the 6 macOS Featured fields (`mass`, `power_max`, `torque_max`, `redline`, `grip`, `damage_tweak`) + `Chassis`, `EngineNormal`, `Transmission`, `Front`/`Rear` curves using `tools/formats/valt_schema_rules.py` labels/units/descriptions, with original values, Undo/Redo, and non-destructive `.CAR` repack (`mods/<Project>/DATA/CAR/`).
  4. **Turnkey NASCAR Importer & Custom Car Cloner Tabs:** `.ISO` or folder picker -> 5-family selector (`Stock`/`Truck`/`Modified`/`Hobby`/`Pace Car`) + `PAINT256` livery thumbnail picker -> background `QThreadPool` conversion via `import_nascar_cars.py` / `nascar_roster.py` / `create_custom_car.py` -> auto-refresh Editor car pickers & one-click `Test Drive in PIE`.
- **Verification Gate:** Unit tests verify ISO9660 reader on synthetic ISO image, `physics.val` round-trip write & CRC integrity on a retail `.CAR`, and headless `CarStudioDialog` launch/edit/save without requiring an open `.TRK`.
