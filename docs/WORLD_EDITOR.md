# EOD World Editor (Qt 6 + OpenGL 3.3 Core + Embedded PIE)

Native Windows 11 desktop World Editor for **Test Drive: Eve of Destruction**, built with **Qt 6 (`PySide6` 6.12)**, hardware-accelerated **OpenGL 3.3 Core (`QOpenGLWidget`)**, **Lumion / Twinmotion / Unreal Engine** placement & atmosphere workflows, **Unreal-style Play-In-Editor (PIE)** embedded Vulkan viewport, non-blocking `QThreadPool` background workers, and native Windows 11 dark titlebar chrome (`DwmSetWindowAttribute`).

For the full JTBD modding blueprint, complete 24-format asset matrix, and on-demand decompile/recompile architecture, see [WORLD_EDITOR_ROADMAP.md](WORLD_EDITOR_ROADMAP.md).

---

## Quick Launch (Windows 11)

- **Double-click (no console window):** `tools\world_editor\launch_editor.bat`
- **Debug console launcher:** `tools\world_editor\launch_editor_console.bat`
- **Command-line options (optional):**
  ```bat
  tools\world_editor\.venv\Scripts\python.exe tools\world_editor\app.py --track AFTON
  tools\world_editor\.venv\Scripts\python.exe tools\world_editor\app.py --track DUNDAS --game-dir "D:\Test Drive - Eve of Destruction (USA)"
  ```

By default the editor auto-discovers all 15 retail `.TRK` archives in `D:\Test Drive - Eve of Destruction (USA)\DATA\TRACKS\` and all 37 retail vehicles in `DATA\CAR\`.

---

## Architecture (`tools/world_editor/`)

| Module | Responsibility |
|---|---|
| `app.py` | Configures OpenGL 3.3 Core `QSurfaceFormat`, applies dark Qt 6 Fusion theme, parses CLI flags, launches `EodWorldEditorWindow`. |
| `theme.py` | Native Win11 dark titlebar (`DwmSetWindowAttribute`), commercial dark Fusion/QSS stylesheet, SVG icon factory, R/G/B `AxisSpinBox`/`Vec3Editor`. |
| `workers.py` | Non-blocking `QThreadPool` / `QRunnable` workers (`FunctionWorker`, `ThumbnailBatchWorker`, `CancelToken`) so track loads, 37-car scanning, 3D thumbnails, and PIE mod compilation never freeze the UI or 60 FPS viewport. |
| `world_loader.py` | Clean-room `.TRK` & `.CAR` loader reusing `world_graf.py`, `vif_mesh.py`, `world_export.py`, `texture_preview.py`, `texture_identity.py`, `world_collision.py`, `vehicle_export.py`, `formats/ilin.py`, and `modding_backend.py`. Loads GRAF v18 worlds, all 37 retail **Cars & Wrecks**, SCOL terrain + surface normals, height-coded editable SOBL barriers, ILIN lines, Start Grid markers, `EnvironmentState`, and `CustomRaceConfig`. |
| `viewport_gl.py` | OpenGL 3.3 Core `QOpenGLWidget` with live sky/sun/fog shaders, live single-texture PNG upload, **Ghost Placement Preview** (`P`) snapped to SCOL height+normal, **Scatter / Paint Brush** (`B`) with floating HUD & 3D brush ring, Grid/Angle Snap, 3D Move/Rotate/Scale gizmos for both Objects and Markers, height-coded SOBL barrier picking, Road Tool (ILIN), and RMB context menu. |
| `pie_host.py` | **Unreal-style Play-In-Editor (PIE)**: launches `eod.exe` (`EOD_MOD_DIR`, `EOD_DIRECT_START`, `EOD_SKIP_MOVIES=1`) and embeds its live Vulkan HWND directly inside the Qt 6 central viewport via Win32 `SetParent` + borderless `WS_CHILD` + DPI-synced `MoveWindow`, with `Stop PIE (Esc)`, `Restart`, and `Pop Out Window`. |
| `explorer_panel.py` | Left sidebar with Track selector, `Tree` / `Models` / `Mlists` / `Layers` tabs, dedicated **Cars & Wrecks (37)** category + quick filter button, Ghost/Scatter/Swap buttons, and off-thread `QThreadPool` 3D thumbnail rendering. |
| `properties_panel.py` | 6-page context inspector: Empty, **Object** (with **Replace With…** + clickable **Texture/Material Inspector** & live **Replace PNG…**), **Road Node (ILIN)**, **Marker / Start Grid**, **Environment / Skybox** (presets, Borrow Sky From Track, Sun/Fog), and **SOBL Collision Barrier** (per-barrier height & Flatten All). |
| `dialogs.py` | `ReplaceModelDialog` (searchable 3D thumbnail picker with Cars & Wrecks filter) and `CustomRaceDialog` (event type, laps, AI count, vehicle, AI speed, linked ILIN line, Play PIE). |
| `project_state.py` | Full `QUndoStack` command suite, `.eodworld.json` v2 overlay save/load, Blender OBJ export, and real in-game Mod Layer writer (`Ctrl+Shift+S`: patched `track.grf`, `c_*.va0`, `track.sol`, `*.ild`, Vulkan `HOSTTEX/replacements.tsv` + `.eodhtx`, `PROFILE.TXT`, `custom_race.json`). |
| `main_window.py` | 14-menu `QMenuBar`, UE PIE + Lumion/Twinmotion toolbar, central `QStackedWidget` (`ViewportGL` / `PieHostWidget`), status-bar progress + cancel, and full signal wiring. |
| `test_editor.py` | Automated verification suite covering retail tracks, all 37 retail Cars & Wrecks, Replace With, Ghost & Scatter, Environment & Texture PNG swap, SOBL barrier flatten, PIE launch spec, real `.TRK` + `HOSTTEX` write-back with retail SHA-256 verification, background `QThreadPool` workers, and live OpenGL 3.3 Core (`glError == 0`). |

---

## Controls & Shortcuts

- **Trackpad:** Two-finger scroll pans. Shift + two-finger scroll orbits around the pointer. Pinch zooms toward the pointer. On macOS, twisting two fingers orbits horizontally where the system delivers a rotation gesture.
- **Mouse and keyboard:** Middle-drag pans. Right-drag looks around and Alt + right-drag orbits. Command + scroll on macOS or Ctrl + scroll on Windows zooms. Hold right mouse and scroll to change fly speed. Use `W/A/S/D` to fly, `Q/E` for down/up, `Shift` for 2.8× boost, and `F` to frame the selection.
- **Lumion / Twinmotion / UE Editing:** `G` Move, `R` Rotate (or +15° in Place Mode), `P` Ghost Place Mode, `B` Scatter Brush, `End` / `Ctrl+G` Drop to SCOL ground, Right-click selected object for quick menu (`Replace Selection With...`), `Ctrl+D` Duplicate, `F2` Edit in Blender (OBJ), `Del` Delete, `Ctrl+Z` / `Ctrl+Y` Undo / Redo.
- **Mod Write-Back & PIE:** `Ctrl+S` Save overlay, `Ctrl+Shift+S` Write real Mod Layer to `mods\<Project>\`, `F5` **Play Free Roam (PIE)**, `Shift+F5` **Play Event / Race (PIE)**, `Esc` Stop PIE / Exit tool.

---

## Vehicle handling and stock-car imports

Open **Car Tuner** for the handling sheet or use **Vehicle / NASCAR Importer**
from the Home Hub to open the import tab without loading a track. The handling
sheet edits verified `physics.val` fields and saves an isolated archive.
Live speed boost remains a session setting.

In **NASCAR Import**, choose your *Dirt to Daytona* ISO or extracted folder,
enter a new chassis name and vehicle number, and import a stock car. The EOD
template provides the archive structure and required defaults for the new
chassis. The importer converts the selected NASCAR model and livery and
builds the EOD registration alongside it. Choose the registered entry and
click **Test Drive in PIE**. This uses the exact numbered car and your current
track, or asks for a track if the editor has none open.

Local imports persist in the editor's registered car list. This test-drive
workflow uses a private overlay. Installing add-on cars into shared profiles,
packaging them for distribution and acquiring missing content online remain
separate workflows under development.

## Running Tests

```bat
tools\world_editor\.venv\Scripts\python.exe tools\world_editor\test_editor.py
```
