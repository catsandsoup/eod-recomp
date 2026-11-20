# EOD World Editor — UI Platform, Dual-Renderer, Concurrency & Zero-Friction Architecture

> **Scope:** Deep technical architecture & research record answering the core engineering questions for the **Test Drive: Eve of Destruction** World Editor & Modding Suite:
> 1. Windows UI Platform (**WinUI 3** vs **Qt 6 C++** vs **Custom GPU-Accelerated UI** like Unreal Slate / Blender / Lumion)
> 2. Dual-Renderer Strategy (**PS2 Fast Performance** vs **Plume Vulkan High-Quality**) & Power-Sipping ("runs on a shitbox")
> 3. Multi-Threading, Process Isolation, MVVM, Asset DB, Queues/Retries, State Machines & Scripting API
> 4. **Standalone ISO-to-Mod-Distribution Fishbone Analysis** — eliminating every friction point from raw PS2 ISO to playable, shareable mods.

---

## 1. Windows UI Platform Research: WinUI 3 vs Qt 6 C++ vs Custom GPU UI

### Competitor Analysis (14 Reference Screenshots + 80.lv Game Dev Tools UX)

| Reference Tool | UI Engine | Key UX Strength to Adopt |
|---|---|---|
| **Unreal Engine 4/5** | **Slate** (custom C++ GPU-rendered retained-mode UI, zero Win32/WinUI controls) | Protect the viewport; Play-In-Editor (PIE) toolbar right above viewport; Outliner + contextual Details panel; scalability/quality selector; bottom Content Drawer. |
| **Blender 4.x** | **Custom OpenGL/Vulkan GPU UI + `wmWindow`** (non-overlapping subdivided areas) | Zero OS widget lag; instant hotkeys (`G`/`R`/`S`); low idle power (event-driven redraws); crisp vector icons & dark high-density panels. |
| **Lumion / Twinmotion** | **Custom Windowless GPU UI** rendered directly over the 3D swapchain | Bottom visual asset shelf with live 3D thumbnails; Ghost Placement preview snapped to terrain normal; Scatter / Paint Brush with 3D terrain ring; one-click Atmosphere / Sun / Weather sliders. |
| **NFSU2 World Editor** | Custom dark desktop chrome + live viewport | Object/Edit mode pill bar; direct Play-In-Editor driving inside the editor viewport; green selection highlight; road spline node handles (yellow/magenta diamonds, green=join, cyan=split, white=new). |
| **Minecraft Bedrock Editor** | Custom dark toolbars + gizmo + contextual right inspector | Clean left tool rail, top hotbar, right collapsible property drawer, "Test World" button in top-right. |

Per [80.lv — UI Design for Game Dev Tools](https://80.lv/articles/ui-design-for-game-dev-tools):
1. **Protect the Viewport:** 70%+ of the window must be unobstructed 3D canvas; tools live on dockable peripheries and floating HUD pills.
2. **Workflow & Muscle Memory > Minimalism:** High information density with consistent hotkeys, undoable commands, and zero modal popups during placement.
3. **Contextual Inspectors:** Selecting an object, road node, spawn marker, or SOBL barrier dynamically swaps the right-hand inspector to only relevant controls.

### Platform Comparison Matrix for Windows 11 (`eod-recomp`)

| Criterion | **WinUI 3 (Windows App SDK)** | **Custom GPU UI on Plume Vulkan** (Slate / Blender / Lumion style, e.g. RmlUi / custom retained-mode) | **Qt 6 C++ (`QVulkanWindow` / `QOpenGLWidget`)** | **Current: PySide6 (Qt 6) + OpenGL 3.3 Core** |
|---|---|---|---|---|
| **3D Vulkan Integration** | **Poor.** Requires `SwapChainPanel` DXGI/D3D11 interop or HWND airspace hacks; Vulkan swapchain embedding has composition & resize flicker. | **Best.** UI & 3D scene share the exact same Plume Vulkan command buffer and swapchain (`0` airspace issues, `<1 ms` overlays). | **Very Good.** Native `QVulkanWindow` or Win32 child HWND embedding; zero Python GIL. | **Good for MVP, GIL-bound.** PyOpenGL + Python GIL caps CPU throughput on heavy scenes. |
| **Memory & Power ("Shitbox" Budget)** | **Heavy.** WinRT + XAML + Windows App Runtime (~180–300 MB baseline RAM, slow cold start). | **Lowest.** ~25–50 MB RAM; event-driven dirty-rect redraws = **0% idle CPU/GPU**. | **Low-Medium.** ~60–110 MB RAM; native C++ widgets, 0% idle CPU/GPU. | **Medium.** ~140–220 MB RAM; now **0% idle CPU/GPU** after demand-driven timer fix. |
| **Engineering Cost (Docking, Menus, IME, Accessibility, DPI)** | Medium (XAML, but weak pro-tool docking/property grids). | **High** if built from scratch (text shaping, HarfBuzz, multi-monitor docking, clipboard, HiDPI, file dialogs). | **Low.** Battle-tested `QDockWidget`, `QTreeView`, `QUndoStack`, native Win11 DPI & file dialogs out of the box. | **Lowest** (already built & shipped in `tools/world_editor/`). |
| **Sharing `runtime/render/` (Plume + PS2 GS)** | Awkward (C++/WinRT ABI mismatch with `llvm-mingw` MinGW toolchain!). | **Native.** Compiles directly with existing `llvm-mingw` + CMake + Ninja + `runtime/render/plume_device.cpp`! | **Native** (with C++20 CMake build linking `runtime/render/`). | Process-isolated via Win32 `SetParent` PIE (`eod.exe`). |
| **Verdict** | **REJECT.** Wrong tool for a Vulkan C++ game editor; WinRT/MSVC friction with our `llvm-mingw` toolchain. | **TARGET (Phase 2 Viewport + Floating HUDs / or full Slate-style `eod-editor.exe`).** | **BEST NATIVE SHELL (Phase 2)** if keeping rich docking/tables while embedding Plume Vulkan viewport. | **SHIPPED PHASE 1** (now 13× faster: ~2.3 ms/frame, 0% idle). |

### Concrete UI Platform Decision

- **Do NOT use WinUI 3.** It fights Vulkan, fights our `llvm-mingw` CMake toolchain, bloats RAM, and lacks pro 3D docking/property-grid widgets.
- **Phase 1 (Shipped & Hardened Today):** Keep `tools/world_editor/` (Qt 6 + OpenGL 3.3 Core) with all performance, lighting, power-sipping, and PIE fixes applied so modders can edit tracks, cars, barriers, skyboxes, and Play-In-Editor **right now**.
- **Phase 2 (`eod-editor.exe` — Pure C++20 + Plume Vulkan):**
  - **Shell:** Native **C++20 + Qt 6 (Widgets/Docking)** *or* **Custom GPU Retained-Mode UI (RmlUi / Clay / custom Plume 2D batcher)** rendered directly on `plume::VulkanDevice` (`runtime/render/plume_device.cpp`) — compiled with the exact same `llvm-mingw` + CMake + Ninja toolchain as `eod.exe`.
  - **Viewport:** Direct **Plume Vulkan** surface sharing `runtime/render/vif_mesh.cpp` and `runtime/render/native_renderer.cpp` — zero Python, zero OpenGL translation, native PS2 GS Fast Mode and Plume HQ Mode inside the same process!

**Phase 2 progress.** The game and the `eod_editor_plume` viewport module share the `eod_plume_scene` pipelines and indexed mesh binding. In a Plume-enabled build, point `EOD_EDITOR_PLUME_LIBRARY` at the built `eod_editor_plume.dylib` or `eod_editor_plume.dll`, then select **Plume HQ** in the Qt editor. The module presents track geometry directly in a native child surface at the viewport's physical pixel size. It samples decoded RGBA textures at their mesh UVs with repeat addressing and base-level linear filtering. Track materials retain their opaque, cutout or translucent treatment, with authored fog. Replacing a texture from PNG and undoing the edit refresh the texture in the viewport. Mipmaps and native editor overlays are pending. Picking uses the editor's existing path. The default Qt preview and PIE remain available without loading the module.

---

## 2. Dual-Renderer Strategy & Power-Sipping ("Runs on a Shitbox")

A PS2 game from 2004 has ~50k–85k triangles per track. An editor for it should **sip power**, stay silent on laptops/integrated GPUs, and never peg CPU/GPU at 100%.

### Two Explicit Renderer Modes (Wired in Both Viewport & PIE)

| Mode | Viewport Behaviour | PIE (`eod.exe`) Env Var | Target Hardware / Use Case |
|---|---|---|---|
| **PS2 Fast (`ps2`)** *(Default)* | Lightweight half-Lambert + hemisphere PS2-era shader, linear filtering, cached static VBOs, demand-driven redraw (**~2.3 ms/frame**, **0% idle CPU/GPU**). | `EOD_NATIVE=0` (PS2 GS renderer) | Low-end PCs ("shitboxes"), laptops on battery, rapid level-design iteration. |
| **Plume HQ (`plume`)** | Enhanced sun, warm bounce, Fresnel/specular sheen and smoothstep aerial fog in the editor viewport. | `--renderer vulkan --require-plume` with `EOD_NATIVE=1`, `EOD_LOOK=modern` and `EOD_MODERN=0`. PIE waits for the native Vulkan renderer's readiness signal. | High-quality visual testing of native Plume materials and authored lighting. |

#### Authored environment scope

`env_override.ini` names the editor track by its `.TRK` archive stem and sets `enabled=1` when authored sun, ambient or fog values differ from Track Default. Plume Modern applies those values to material-lit native gameplay surfaces. Classic, Original, unlit materials, particles and UI use their existing lighting inputs. In-game Plume and the Vulkan GS backend both resolve the active overlay's validated HOSTTEX replacements. Native Plume Classic and Modern have rendered an installed Afton sky replacement at 8192×8192.

Inside the world-frame render at `0x00214B68`, the native feed uses the loaded GRAF pointer at `0x003D1C18` (`0x00214678`). `0x00130D90` stores the GRAF resource record at `GRAF-8`. Its resource-set owner comes from the `0x0043A0E0` and `0x0043A0E4` archive lists (`0x00130AD0`), using links at `+0x120`, record pointer at `+0x124`, count at `+0x140` and `0x44`-byte record strides. Recorded draws carry a scope bit to the GS thread. Outside the world-frame render or when the matching owner is absent, the existing native render inputs apply.

The authored shader uses the editor's two-sided wrap lighting and smoothstep fog over distance to the model-space camera already calculated by the Modern shader. Sun direction changes from the editor's Y-up coordinates to the native feed's Y-down coordinates. RGB is in the editor's `0..1` range. Fog density is the maximum blend factor. The shader clips the range width to at least one world unit, as in the editor.

### Power-Sipping Rules Enforced
1. **Event-Driven Dirty Redraw (No 60Hz Spin Loop):** The viewport only calls `paintGL()` when the camera moves, an edit occurs, or the window resizes. When idle, **frame rate = 0 Hz, CPU = 0%, GPU = 0%**.
2. **Zero Per-Frame Allocations / Lookups:** All GLSL uniform locations cached once in `initializeGL()`; roads & markers cached in static VBOs (`_roads_dirty`, `_markers_dirty`); object selection uses `GL_POLYGON_OFFSET_FILL` so clicking an object **never** rebuilds static world VBOs.
3. **PyOpenGL Error-Checking Disabled in Runtime:** Eliminates ~1,000 synchronous `glGetError()` pipeline stalls per frame (**13× speedup**: `30.9 ms` → `2.3 ms`).

---

## 3. Core Software Architecture: Concurrency, MVVM, Asset DB, Queues & Scripting

### 3.1 Multi-Threading & Process Isolation ("Degrees of Stability")

A crash or hang in an importer, thumbnailer, or running game must **never** freeze or crash the UI.

```
┌─────────────────────────────────────────────────────────────────────────┐
│  Process 1: EOD World Editor                                            │
│  ┌─────────────────────┐   Queued Signals / Lock-Free Ring Buffer       │
│  │ UI / Main Thread    │◄────────────────────────────────────────────┐  │
│  │ (Qt Event Loop,     │                                             │  │
│  │  Input, Inspector,  │   Immutable Snapshots / Command Dispatch    │  │
│  │  Undo/Redo Stack)   │─────────────────────────────────────────┐   │  │
│  └─────────┬───────────┘                                         ▼   │  │
│            │ (Phase 1: GL on UI thread, <2.5ms)   ┌──────────────────┴┐ │
│            │ (Phase 2: Dedicated Render Thread)   │ Priority Job Pool │ │
│            ▼                                      │ • P+10: PIE Build │ │
│  ┌─────────────────────┐                          │ • P+10: Track Load│ │
│  │ 3D Viewport Engine  │                          │ • P  0: Car Scan  │ │
│  │ (PS2 Fast / Plume)  │                          │ • P -5: Thumbs    │ │
│  └─────────────────────┘                          │ (Capped CPU-1,    │ │
│                                                   │  cooperative yield│ │
│                                                   │  + CancelToken)   │ │
│                                                   └───────────────────┘ │
└─────────────────────────────────┬───────────────────────────────────────┘
                                  │ Process Boundary (Crash-Isolated!)
                                  │ Win32 SetParent HWND Embed + Stdout Pipe
                                  ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  Process 2: eod.exe (Play-In-Editor Sandbox)                            │
│  EOD_MOD_DIR=... | EOD_DIRECT_START=... | EOD_NATIVE=0|1                │
│  If the mod crashes eod.exe, Editor catches exit code cleanly &         │
│  returns to the 3D viewport with zero lost edits!                       │
└─────────────────────────────────────────────────────────────────────────┘
```

**Why PIE previously hung on the loading bar (Fixed Today):**
1. **PySide6 GC Signal Drop:** `FunctionWorker` was submitted to `QThreadPool` without retaining a strong Python reference to its `WorkerSignals` QObject — Python GC could destroy the signal emitter before `export_mod_layer` finished, leaving the progress bar stuck forever. **Fix:** `_LIVE_WORKERS` registry in `workers.py` (`start_worker()`) retains every active worker until `finally:` completes.
2. **ThreadPool Starvation & GIL Contention:** `ThumbnailBatchWorker` flooded all threads with 1,883 thumbnail renders at equal priority. **Fix:** Priority queuing (`PIE = +10`, `Track = +10`, `Cars = 0`, `Thumbnails = -5`), cancelling thumbnail jobs on PIE launch, capping pool to `max(2, min(4, cpu_count - 1))`, and yielding every 4 thumbnails so a 2-core shitbox stays responsive.

### 3.2 MVVM + Command Pattern (Clean State Management)

- **Model (`world_loader.py` / Phase 2 C++ `WorldDocument`):** Pure data (GRAF placements, SCOL/SOBL collision, ILIN graphs, textures, `EnvironmentState`, `CustomRaceConfig`). Zero UI imports.
- **ViewModel (`project_state.py` + `QUndoStack`):** All mutations go through reversible `Command` objects (`TransformObjectCmd`, `ReplaceModelCmd`, `ScatterBatchCmd`, `BarrierHeightCmd`, `TextureSwapCmd`, `RoadEditCmd`). Emits granular dirty flags (`static_dirty`, `roads_dirty`, `markers_dirty`, `col_dirty`) so views only update what actually changed.
- **Views (`viewport_gl.py`, `explorer_panel.py`, `properties_panel.py`):** Stateless renderers/inspectors bound to ViewModel signals.

### 3.3 Explicit Editor State Machine

```
[BOOT / NO_ISO] ──(ISO Extracted / Game Dir Valid)──► [IDLE_EDITING]
                                                         │   ▲
           ┌─────────────────────────────────────────────┤   │
           │ (P: Place / B: Scatter / Road Tool)         │   │ (Esc / Stop)
           ▼                                             │   │
   [ACTIVE_TOOL_MODE]                                    │   │
           │ (Click / Stroke → Push Command)             │   │
           └─────────────────────────────────────────────┘   │
                                                             │
   [IDLE_EDITING] ──(F5 / Shift+F5)──► [PIE_COMPILING] ──────┤ (Cancel / Fail)
                                             │ (Success)     │
                                             ▼               │
                                       [PIE_LAUNCHING] ──────┤ (Timeout / Crash)
                                             │ (HWND Found)  │
                                             ▼               │
                                       [PIE_EMBEDDED] ───────┘ (Esc / Stop / Exit)
```

### 3.4 Local Asset Index DB, Caching & Retries

- **Asset Index Database (SQLite, `%LOCALAPPDATA%\EodEditor\asset_index.db`):**
  - Stores retail ISO SHA-256 verification, extracted `.TRK`/`.CAR`/`.RES` member catalog, pre-computed FNV-1a 64 `HOSTTEX` texture identities (`indexKey`, `paletteKey`), model bounding boxes, and thumbnail cache hashes.
  - **Benefit:** Cold track open drops from scanning archives repeatedly to instant SQLite lookup; thumbnails persist on disk (`~/.cache/eod_editor/thumbs/<sha1>.webp`) so they are **never re-rendered twice**.
- **Resilient I/O & Retries:**
  - Atomic file writes (`write .tmp` → `fsync` → `os.replace`) for `.eodworld.json` and `mods/<Project>/` so a crash or power loss never corrupts a user's project.
  - PIE HWND attachment uses bounded exponential/stepped polling (15s timeout with automatic fallback to "external window" mode instead of hanging).
  - Mod file locks (e.g. `eod.exe` still releasing a `.TRK` handle on Windows) retry 3× with 150ms backoff before surfacing an actionable error.

### 3.5 Scripting, Automation & Mod API (Phase 2)

- **Headless CLI / Batch API:** Every editor action (`place`, `replace`, `scatter`, `flatten-barriers`, `swap-tex`, `build-mod`, `pack-eodmod`) is callable from CLI/JSON recipes (`eod-editor --batch recipe.json`) for CI testing and procedural track generators.
- **Sandboxed Mod Scripting (Lua 5.4 or Python plugin hooks):** Event hooks (`on_track_load`, `on_pre_export`, `on_scatter_filter`) for community generators (e.g. procedural demolition arenas, randomized derby grids).

---

## 4. Standalone Zero-Friction ISO-to-Mod-Distribution Fishbone Analysis

**User Goal:** *"All the user needs is the game's PS2 ISO. The standalone app extracts it, manages mod installation, testing, and mod distribution with zero friction."*

### Fishbone Diagram (Ishikawa) — Every Possible Friction Point

```
  [1. ONBOARDING & ISO]              [2. PERFORMANCE & STABILITY]        [3. MOD AUTHORING]
   ├── User only has .ISO / .BIN      ├── Integrated GPU / 2-core CPU     ├── Broken lighting / dark map
   ├── Wrong region (PAL vs NTSC-U)   ├── PyOpenGL / GIL UI freezes       ├── Corrupted retail .TRK files
   ├── Corrupted / bad ISO dump       ├── PIE loading bar hangs           ├── Missing Blender / PNG sizes
   └── Missing Python / VC++ / DLLs   └── Crashed mod crashes editor      └── Lost edits on crash
                 \                                 |                                /
                  \                                |                               /
                   ▼                               ▼                              ▼
  ══════════════════════════════════════════════════════════════════════════════════════► [ZERO-FRICTION]
                   ▲                               ▲                              ▲       [STANDALONE APP]
                  /                                |                               \
                 /                                 |                                \
  [4. PIE TESTING & RUNTIME]         [5. MOD PACKAGING & SHARING]        [6. WINDOWS OS & SECURITY]
   ├── eod.exe not found / wrong cwd  ├── Users manually copy loose files ├── Win11 SmartScreen / AV false +
   ├── Vulkan driver missing / old    ├── Two mods edit same AFTON.TRK    ├── Program Files write UAC block
   ├── Controller/keyboard focus trap ├── Huge downloads (full .TRK copy) ├── HiDPI 125%/150%/200% scaling
   └── Stale mod cache not reloaded   └── Version mismatch (old mod fmt)  └── Long paths / spaces in path
```

### Friction-Point Resolution Matrix (Every Branch Solved)

| Category | Friction Point | Concrete Engineering Resolution |
|---|---|---|
| **1. Onboarding & ISO** | User only has `Test Drive - Eve of Destruction (USA).iso` | **First-Run ISO Wizard:** User drags & drops `.iso` onto the window. Built-in pure-code ISO-9660 reader extracts `DATA/` into `%LOCALAPPDATA%\EodRecomp\game\` (never requires ImgBurn/7-Zip), verifies `SLUS_209.10` + SHA-256 checksums, and flags PAL/corrupt dumps with a clear message. |
| **1. Onboarding & ISO** | Missing Python / Qt / DLLs | **Standalone Portable Bundle:** Package Phase 1 via Nuitka/PyInstaller (or Phase 2 single native `eod-editor.exe` + `eod.exe` + bundled Vulkan loader) in one ZIP/installer — zero Python/pip/VS install required. |
| **2. Perf & Stability** | Laggy viewport / high battery drain on low-end PCs | **Implemented Today:** Demand-driven rendering (0% idle CPU/GPU), cached VBOs & uniforms (~2.3 ms/frame), `PS2 Fast` default mode (`EOD_NATIVE=0`), `QThreadPool` capped to `CPU-1` with cooperative GIL yields. |
| **2. Perf & Stability** | PIE loading bar hangs / crashes editor | **Implemented Today:** `_LIVE_WORKERS` lifetime registry + priority queue (`PIE = +10`, `Thumbs = -5`) + process-isolated `eod.exe` sandbox. |
| **3. Mod Authoring** | Accidental corruption of retail game files | **Immutable Retail Rule:** Editor **never** writes to `DATA/TRACKS/*.TRK`. All edits write exclusively to `mods/<ModName>/` overlay folders; retail SHA-256 is verified in automated tests. |
| **3. Mod Authoring** | Lost work on crash | **Autosave + Atomic Write:** Periodic background autosave of lightweight `.eodworld.json` overlay every 60s via atomic `tmp -> replace`, with crash-recovery prompt on next launch. |
| **4. PIE Testing** | Keyboard focus trapped / resize broken in embedded HWND | Win32 `SetParent` + DPI-scaled `MoveWindow` + dedicated top PIE control bar (`Restart`, `Pop Out Window`, `Stop PIE (Esc)`). |
| **4. PIE Testing** | Old GPU lacks Vulkan 1.2 for Plume | Automatic pre-flight Vulkan probe: if Vulkan device creation fails, cleanly falls back to `PS2 Fast` (`EOD_NATIVE=0`) with a non-blocking banner instead of silent crash. |
| **5. Mod Distribution** | Sharing loose folders or 40 MB copied `.TRK` files | **One-Click `.eodmod` Package Format:** A tiny ZIP containing `mod.json` (name, author, version, target track, dependencies, load order) + **binary delta patches / overlay JSON / compressed `HOSTTEX` `.eodhtx`**. Typical mod size: **50 KB – 2 MB** instead of 40 MB! |
| **5. Mod Distribution** | Installing someone else's mod & managing conflicts | **Built-In Mod Manager Tab:** Drag-and-drop `.eodmod` to install; enable/disable checkboxes; drag-to-reorder load priority; automatic conflict detector ("Mod A and Mod B both edit `AFTON.TRK` — merge placements automatically or pick winner"). |
| **6. Windows OS** | UAC write failures in `C:\Program Files\` | Store all writable state (extracted game data, `mods/`, SQLite cache, autosaves, logs) in `%LOCALAPPDATA%\EodRecomp\` (or portable `./userdata/` when `portable.txt` is present). |
| **6. Windows OS** | Paths with spaces / HiDPI scaling | Always pass subprocess args as argv lists (never shell string concat), quote batch paths, and scale Win32 `MoveWindow` by `devicePixelRatioF()`. |
