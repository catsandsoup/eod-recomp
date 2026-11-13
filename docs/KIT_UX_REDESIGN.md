# Creation Kit UX redesign: jobs first

Status: design proposal, 2026-11-11. Nothing here is implemented. The clickable
prototype is [`docs/design/kit-prototype.html`](design/kit-prototype.html); its
car, track, event, dealer, physics, sky, collision and AI-line values were
extracted from the retail disc with the repository tools (section 7). Retail
renders and textures are not in the repository: the prototype shows them when
`work/kit-prototype-assets/` exists (ignored by Git; command in section 7).

Player success for this redesign: a modder who opens the Kit can, without
reading documentation, (1) say which car, track, event or save they are
editing and how they got there, (2) start any of the owner's named jobs from
the first screen in one click, (3) see before editing whether a change will
reach the game, and (4) keep browsing and previewing while an export, build or
test race runs. Evidence needed: screenshots of the visible window for each
screen in section 4, a timed hallway test of the jobs in section 2 (target:
first visible in-game change in under 10 minutes for a tinkerer), and a
responsiveness check with a long export running (section 6.5).

Contents: 1 heuristic evaluation · 2 job map and feasibility · 3 information
architecture · 4 screen specs · 5 copy · 6 implementation plan · 7 data and
sources.

---

## 1. Heuristic evaluation of Kit 0.4.1

Reference: the owner's screenshot of 0.4.1 with `BUS.CAR / physics.val` open.
Regions used below: **T** toolbar (Open Project…, Game Folder…, Export All
Textures…, Save & Build, Run Mod in Game), **N** tab strip (Asset Workspace /
Mods) with the archive path at the far right, **S1** "Game Sources" archive
column, **S2** asset list, **C** centre editor (physics.val rows "› Chassis · 82
values" … "› $0000$speedo_slider · 2 values"), **I** Inspector ("BUS · Vehicle
Physics", damage_scale / grip / mass …, Undo/Redo), **B** status bar
("Opened project.eodproject", Project Folder). Code references are to
`tools/editor_macos/Sources/EODModdingSuite`.

Severity scale (Nielsen): 0 not a problem, 1 cosmetic, 2 minor, 3 major, 4
catastrophe (blocks the job).

### 1.1 The owner's three questions

| Question | What 0.4.1 answers | Why it fails |
|---|---|---|
| What do I do on this page? | Nothing. The window opens on a list of 8.3 archive names (S1) and a file list (S2). The centre says "Game Asset Workspace · 72 archives · 7,329 assets" when nothing is selected. | The model is the disc's file system, not the modder's goal. Nothing on screen connects "make the bus faster" to `BUS.CAR → physics.val → Chassis`. There is no primary action anywhere in the window: five toolbar buttons have equal weight (T). |
| How did I get here? | Header "AnokaEnvironment / physics.val" (the project folder name, then a member name), and "DATA/CAR/BUS.CAR" in small grey at the far right of N. | No breadcrumb, no back/forward, no selection highlight that names the car. The header names the *project* (a track environment project) while the screen edits a *car*, so the two locations contradict each other. |
| Why is it open? | Undeclared. Selecting an archive auto-opens a member (`selectArchive` → `inspectSelection`); "Show Sky Textures" silently sets the search field to `sky` (`MainView.swift`, inspector). | Hidden mode changes and auto-selection put things on screen the modder did not ask for, with no visible reason. |

### 1.2 Problems by heuristic

| # | Problem | Heuristic | Sev | Where |
|---|---|---|---|---|
| 1 | Entry point is the archive file system (ALGAR.CAR, AMBULA_1.CAR, GLADIA_1.CAR …). The modder must recall that the ambulance is `AMBULA_1.CAR` and that handling lives in `physics.val`. | H2 match with the real world; H6 recognition over recall | 4 | S1, S2 |
| 2 | Section disclosure rows: the toggle is the disclosure triangle only (SwiftUI's default macOS `DisclosureGroup`: the label is not a button), about 13 × 13 pt. HIG minimum control size on macOS is 20 × 20 pt (28 × 28 default). All sections start collapsed, so the most common action needs eight small, precise clicks. | Fitts's law; H7 efficiency; HIG accessibility | 3 | C |
| 3 | Values the game never reads are shown as editable: `rate`, `bump`, `rebound`, `camber`, `toe`, `sway`, `brake_force`, `wheel_lock`, `rspoiler_angle`, `fuel_capacity` do not occur in the executable (docs/SETU_CAR_SETUP.md). Editing them saves, builds and does nothing in game. | H5 error prevention; H1 visibility | 4 | C (Front, Rear) |
| 4 | Raw keys and storage noise as labels: `damage_scale`, `grip`, `0.300000012` beside "Original 0.3", `$0000$speedo_slider`, "· 82 values". | H2; H8 minimalist design | 3 | C, I |
| 5 | Two editors for the same values: the centre table and the inspector edit the same `physics.val` keys; the inspector shows a fixed "featured" subset that does not follow what was clicked. HIG: an inspector shows the details of the current selection. | H4 consistency and standards | 3 | C, I |
| 6 | No location model: header names the project, the archive path is far right in grey, nothing names "Bus". | H1; wayfinding (NN/G "you are here") | 3 | header, N |
| 7 | Toolbar mixes setup (Game Folder…), a rare bulk task (Export All Textures…), persistence (Save & Build) and the loop action (Run Mod in Game) at equal weight; the loop action is disabled with no reason (`canTest` needs a saved archive or an active profile). | H8; H1; HIG toolbars (one primary action) | 3 | T |
| 8 | App-wide busy lock: `busy` and `gameRunning` disable both lists, the editor and the toolbar (`.disabled(state.busy …)` in `MainView.swift`). A test race or export freezes browsing. Violates the AGENTS.md concurrency rule. | H3 user control and freedom; H1 | 4 | S1, S2, C, T |
| 9 | Changes are invisible: "· Unsaved" in the subtitle is the only signal; there is no list of what changed, per field revert, or modified markers on cars/tracks. Undo/Redo sit inside the inspector rather than only in Edit. | H1; H3 | 3 | header, I |
| 10 | No consequence before acting: editing a value does not say which races, liveries or cars it affects; Replace with Image… explains the size consequence only after a file is chosen. Paint edits to most cars' own `.tex` are inert because the game draws the numbered livery from `CT.RES` (`modding_backend.save`, Bus-only rebuild). | H1; H5 | 3 | C |
| 11 | Track jobs are hidden behind archive selection: sky tint is three free-text RGB multiplier fields, "Show Sky Textures" filters the asset list, props/walls/AI lines have no surface. | H6; H2 | 3 | I (track) |
| 12 | Packaging is a separate "Mods" tab disconnected from the edit; the modder must know that Save & Build, Build Package, Install and Enable are four different states. | H2; H6 | 2 | N |
| 13 | Monospaced 8.3 names and byte counts in lists; no thumbnails for cars, textures or tracks. | H6 | 2 | S1, S2 |
| 14 | Empty centre state has statistics but no action. | NN/G empty states | 2 | C |
| 15 | Forced dark appearance (`.preferredColorScheme(.dark)` in `App.swift`) ignores the system setting. | H4; HIG | 1 | whole window |
| 16 | Errors appear as an orange banner above the status bar and logs open as a file in another app. | H9 | 2 | B |

Top ten by severity × frequency: 1, 8, 3, 2, 6, 7, 4, 5, 9, 10.

### 1.3 Disclosure-row target size, specifically

The triangle is the only hit area and it sits 4–6 pt from the row's label; the
row is ~36 pt tall but only ~4 % of its area responds. NN/G's accordion
guidance: make the heading and the icon clickable, and avoid accordions when
users need most of the content. Here the modder needs most of it (handling is
read as a whole), so the redesign removes the accordion as the main
interaction: sections become always-open grouped forms with a pinned section
index (Weight & grip · Engine · Gearbox · Tyres · Damage), and the only
remaining disclosure ("All values", for experts) is a full-width header button
at least 28 pt tall whose whole row toggles.

---

## 2. Jobs-to-be-done map

Personas (docs/MODDER_JOURNEYS.md): Tinkerer (largest), Texture artist,
Track/prop tweaker, Vehicle modder, Distributor, Researcher. Rank = frequency
(1–5) × value (1–5); frequency weights the Tinkerer and Texture artist, value
is how visible the change is in game. Status: **works** (Kit or tool does it
end to end on retail data), **partly** (a repository tool does it, the Kit has
no surface, or it is not proven in game), **missing**.

| Rank | Job story | Persona | F×V | Data that makes it happen | Status | Evidence |
|---|---|---|---|---|---|---|
| 1 | When a car looks dull, I want to repaint it, so I can race it in my colours. | Tinkerer, Vehicle | 5×5 | The game draws a car with a numbered livery `DATA/RES/CT.RES#<number>.tm0/.tex` (285 liveries); the number comes from `dirt.val [Driver]` (AI) or `buycar.val [vehicle]` (dealer, 800–929). The car's own `Bus1.tex`-style member is a template. | partly | Same-size texture replace works for any `TEX`; Kit rebuilds the livery automatically only for Bus (790/79a, `modding_backend.save`). Other cars need the livery chosen explicitly. |
| 2 | When I want a bigger, sharper world, I want to export every texture, upscale it and put it back, so I can play in 4K. | Texture artist | 4×5 | 4,923 `TEX` members; host replacement `k×` keyed by index/palette. | works (sky native-verified) | CREATION_KIT_ACCEPTANCE: batch export 4,923/4,923; 32-tile 4K sky package native-verified. Other roles not raced. |
| 3 | When a track's sky doesn't suit my mod, I want to swap it for my own panorama or tint it, so I can race at sunset. | Tinkerer, Texture artist | 4×5 | 32 sky tiles per track (16 horizon `skyNNA` 128 px + 16 cap `skyNNB` 64 px), geometry baked in `track.grf`. `sky_export.py reimport --from-panorama`. | partly | Anoka verified (native k=32 = 4096/2048 tiles). 13 other tracks decode, no native run; Welch topology fails (mirrored lower hemisphere). Kit offers tint + per-tile replace only; panorama import is CLI only. An 8,192-px panorama yields tiles sampled at 4× (512 px); 4,096-px tiles need a 65,536-px panorama, otherwise larger `k` only enlarges pixels. |
| 4 | When a car feels wrong, I want to change its weight, power, grip and gears in real units, so I can feel the difference in a race. | Vehicle, Tinkerer | 4×4 | `physics.val` per car. Read by car physics init: `mass grip slip load_factor brake_factor spring_factor tire_diameter rear_end_ratio redline num_gears g1…`; plus `power_max torque_max damage_scale` (curated). | works (data edit); effect not measured | Kit saves any car's `physics.val` (single-archive VALT edit). No timed-run comparison recorded. Ten setup range keys are not read by the game. |
| 5 | When I want to drive the school bus in my career, I want it on sale at the dealer, so I can buy, upgrade and race it. | Tinkerer | 3×5 | Bus is chassis `Bus`, class Special (`carclass.val`), not in `buycar.val [car]`; appears as AI (dirt.val 790, 791, 540, 620, 602) and in Kenyon "School Bus Race". Add a Career chassis copy (`Schoolbus`, Midsize, $280, vehicle 785) to `buycar.val [car]/[vehicle]`, `car_stats.val`, `carclass.val`, plus CT/UICAR art. | partly | `tools/build_career_specials.py` builds it into an integrated profile (offline). Alternative: eod.ini `[Career] CarLooks=<career car>:790` draws an owned car as the bus (`src/game_patches.cpp apply_car_looks`). No Kit surface; no native purchase recorded in acceptance docs. |
| 6 | When I'm stuck or want to try everything, I want to edit my save's money, cars and reputation, so I can play what I like. | Tinkerer | 3×4 | Memory card `BASLUS-20910DRIVEN` (298,255 B): `HSCM` v3 header, sections OPTs, Idnt, Chmp, Rep0, Feat, MVid, Crer (0xE7C: money `+0x84`, 4 garage slots `+0xE8 + 0x300·i` = vehicle − 800, standings `+0xCE8`), CCar, Pant. Checksum: add byte, rotate right 1, xor `0xFEEDFACE` over the payload. | partly | `runtime/core/eodsave.{h,cpp}` reads/writes money, reputation, garage vehicles, options, CarLooks; used by the Windows setup tool's Save editor. The checksum was recomputed in Python on a real save and matched. The Kit has no save editor. Garage is limited to 4 cars by the save layout. |
| 7 | When an invisible wall stops a jump, I want to lower it, so I can fly over. | Track tweaker | 3×4 | `track.sol` SOBL boxes (Anoka 713 records; 571 with 15 m half-height; tops up to 18.67 m above ground). | partly | `world_collision.py set-limit` (shrink-only) byte-verified; airborne pass-over not produced in game. |
| 8 | When an event's layout is boring, I want to move or remove its props (walls, hay bales, ramps), so I can build my own course. | Track tweaker | 3×4 | Event props are `dyno` entries in `c_<event>.va0` (Anoka figure-8: 212; demo derby: 42). World scenery is `track.grf`. | partly | `build_gameplay_mod.py --remove-dennison-dyno` removes props; `world_graf` translate is byte-exact; moved world models keep their old collision. No Kit surface. |
| 9 | When I've modelled a better body in Blender, I want to put it on the car, so I can see it in a race. | Vehicle | 2×5 | `Low0–4.mod` VIF geometry; `vehicle_export.py`, `vehicle_import.py patch`. | partly | 37 cars export; same-topology import native-verified once (12 % wider Ambulance). Different vertex counts are refused. Not in Kit. |
| 10 | When I want a car of my own, I want to copy an existing one under a new name, so I can tune and paint it without touching the original. | Vehicle | 2×4 | `vehicle_import.py new` → new `.CAR` + COMMON/CT/UICAR rows. | partly | Overlay directory, not a package; trailers refused; one class only. |
| 11 | When the menus say the wrong thing for my mod, I want to change the game's text, so I can rename events and cars. | Tinkerer | 2×3 | `LOCALE.RES#usenglish_lang.lng`, 877 `Group:Name` strings. | works (data); on-screen check pending | `tools/formats/lang.py`, labelled view, `%d` and Latin-1 enforced; keys fixed. |
| 12 | When I re-theme the game, I want to replace menu images, so I can match my mod. | Texture artist | 2×3 | 264 `IMAG` members (≤ 512 px, 1–20 frames). | works (data); on-screen check pending | `tools/formats/imag.py` same-size PNG frame replace. |
| 13 | When AI cars take a bad line, I want to move the racing line, so I can make races harder. | Track tweaker | 1×3 | `.ild` (Anoka `oval.ild` 268 nodes). | partly | Node x/z editable in the format view; tangents do not update; AI behaviour not checked natively. |
| 14 | When I want something silly, I want to drive a boat in Career, so I can tow-race a boat round the oval or drive one. | Tinkerer | 1×4 | **The only boat on the disc is `TRAILER5.CAR`: a jon boat on a trailer** (`BTrailer.tm0`, `dirt.val [vehicle] chassis = trailer5, is_trailer = yes`, numbers 404, …). Trailers are towed in Trailer Race contests (every track's `trailer_race`, plus "Trailer Derby" challenges). | missing | No drivable boat, no water surface or buoyancy. A trailer is not a player chassis: `vehicle_import.py new` refuses trailers; a CarLooks swap to a trailer vehicle is untested. Needs a new drivable chassis from a verified car plus a boat mesh with a new topology, which the mesh pipeline does not support. |
| 15 | When I want my own soundtrack or crash sounds, I want to swap audio, so I can hear my mod. | Tinkerer | 2×2 | 112 `SFX0`, `SPEECH.BNK`, movies. | missing (Kit) | `tools/audio.py` converts `.sfx` ↔ WAV; no Kit surface, no native check. |
| 16 | When my mod works, I want to package it and install it on another Mac, so friends can play it. | Distributor | 2×4 | `.eodmod` packages, dev profile overlay. | works | Native-verified 32-tile 4K sky package; Kit Mods tab. |
| 17 | When I've changed anything, I want one click to race it, so I can see the change. | everyone | 5×5 | `launch_plan`/`launch_profile`: direct-start race, isolated memory card. | works (fixed track list) | Kit "Run Mod in Game"; car must be in the verified race mapping (`CAR_NAMES`). Event choice and livery choice missing. |

---

## 3. Information architecture

### 3.1 Decision: object browser with a jobs Home in front, not a wizard

Options considered:

1. **Job-first wizard** ("What do you want to make?" → questions → editor).
   Rejected. NN/G: wizards suit novices and infrequent tasks and frustrate
   frequent and expert users; modding is iterative (edit, race, adjust). The
   owner dislikes being asked questions and reads missing buttons as missing
   features; a wizard hides the editor behind questions.
2. **By-kind browser only** (owner's brief: Tracks, Cars, Races, Interface,
   Text, Audio, Characters, Raw Archives). Close to Unreal's Content Browser,
   but it still makes the modder translate a goal ("drive the bus in Career")
   into a kind (is that a car? a race? a table?).
3. **Chosen: object workspaces, entered from a jobs Home or from the sidebar.**
   The sidebar lists the things modders think in (Cars, Tracks, Career, Saves,
   Text & Menus, Sounds), each opening a workspace whose tabs are the jobs for
   that thing (Car: Paint · Handling · Model · Test Drive). Home is the first
   sidebar row and the launch screen: large job tiles ("Repaint a car",
   "Change a sky", "Sell the school bus in Career", "Edit my save" …) that
   deep-link into a workspace and tab. Home is a hub, not a step: no
   questions, every tile is one click, and the sidebar is visible the whole
   time so the destination is learned (recognition, not recall). Raw archives
   stay one click away under **Game Files** for experts.

Mapping the owner's brief: "Races" becomes the **Events** tab of a track plus
the **Career** workspace (Eves, dealer); "Interface" and "Text" merge into
**Text & Menus** (strings, menu images, layouts); "Characters/Animation" is
not a job any persona named and has no decoder (`Anim`, `Anm0` generic
blocks), so it lives under Game Files until a job needs it.

### 3.2 Window anatomy

```
┌ toolbar ─────────────────────────────────────────────────────────────────────────┐
│ ◧  ‹ ›  Bus                         [● Ready · 3 changes]     Changes  ▶ Test Drive ▾  Install  ⓘ │
│         Cars › Bus › Handling                                                     │
├ sidebar ────────┬ workspace ───────────────────────────────────────┬ inspector ───┤
│ 🔍 Search        │ Bus                       Special · BUS.CAR      │ Peak power   │
│ ⌂ Home          │ [ Paint | Handling | Model | Test Drive ]         │ hp           │
│ MAKE            │                                                   │ [Chassis]…   │
│ 🚗 Cars    ›    │  Weight & grip                                    │ Original 420 │
│    Bus      ●   │   Weight     ───●────────  [ 9000 ] lb  ⟲         │ Read by …    │
│ 🏁 Tracks       │   Grip       ──────●─────  [ 1.20 ] ×             │              │
│ 🏆 Career       │  Engine …                                         │              │
│ 💾 Saves        │                                                   │              │
│ Aa Text & Menus │                                                   │              │
│ 🔊 Sounds       ├ bottom panel: Changes · Issues · Jobs · Log ──────┴──────────────┤
│ MY MOD          │ Jobs  ▸ Exporting textures 1,204 / 4,923  [Cancel]                 │
│ GAME FILES      │                                                                    │
└─────────────────┴────────────────────────────────────────────────────────────────────┘
```

- **NavigationSplitView**, two columns plus `.inspector`. Sidebar is
  `List(selection:)` with `.listStyle(.sidebar)`, sections Make / My Mod /
  Game Files, at most two levels (HIG). The object currently open appears as
  an indented child under its kind (Cars › Bus) with a modified dot; this is
  the "you are here" highlight.
- **Pickers** (Cars, Tracks) are the workspace's empty state: a grid of
  thumbnails grouped by class or league with a search field. Choosing one
  opens it; the child row appears in the sidebar.
- **Workspace header**: large title (object name), one secondary line with
  identity facts (class · price · archive), and a **segmented job bar** whose
  segments are at least 28 pt tall and 96 pt wide. The selected segment and
  the breadcrumb (`Cars › Bus › Handling`, navigation subtitle) answer *where
  am I*; Back/Forward in the toolbar answer *how did I get here*; when a
  workspace was opened from a Home tile or search, the breadcrumb root says so
  (`Home › Repaint a car › Bus`), answering *why is it open*.
- **Inspector** (trailing column, `inspectorColumnWidth(min: 240, ideal:
  280, max: 380)`, toggled with ⌥⌘0) re-targets on every sub-selection:
  a handling value, a livery, a sky tile, a wall box, a prop, a dealer row, a
  string. It never duplicates the editor's controls; it explains the
  selection: plain label, raw location (`[Chassis] damage_scale` in
  secondary monospaced text), original value with Revert, allowed range, what
  it does in game, confidence and evidence, where it is used (which cars,
  races or liveries share it).
- **Bottom panel** (one, collapsible, ⇧⌘Y): **Changes** (every edit as a row:
  object, field, before → after, Revert), **Issues** (validation, e.g. "Bus
  paint differs from its standard set"), **Jobs** (section 3.6), **Log** (game
  and service log, filterable, never an external file first).

### 3.3 Toolbar

Leading: sidebar toggle, Back/Forward, title + breadcrumb subtitle.
Centre: an activity view (like Xcode's): `Ready`, or the most recent running
job with its progress ("Building mod · 2 of 4"); clicking opens the Jobs tab.
Trailing (never overflow): **Changes** (count badge; popover = Changes list),
**▶ Test** (the one prominent primary action), **Install** (builds the
package and enables it in the dev profile), inspector toggle.
Setup items (Game Folder, Game Executable) move to Settings (⌘,); "Export All
Textures…" moves to Game Files and to the Texture artist's Home tile.

**Test** is contextual and remembers the last choice per project: in a Car
workspace "Test Drive" (this car, last track); in a Track workspace "Test
Race" (this track, last car); in Career "Test Career" (isolated memory card).
Its menu (the ▾ half of a split button) holds Track, Event (the track's Eves
and challenges from `<track>_eves.val`), Car, and Livery pickers. Unsupported
choices are listed disabled with the reason (e.g. Hurricane: "No verified race
mapping"). If there are unsaved changes, Test saves and builds first; no
separate "Save & Build" step.

### 3.4 State, undo and changes

- `@MainActor @Observable` models: `AppModel` (route, history, selection),
  `ProjectModel` (edits, change list), one model per workspace, `JobsModel`.
- Every edit is a `Change {object, assetId, field, before, after}` recorded in
  the project immediately (autosave to `project.eodproject`), registered with
  `UndoManager` (Edit › Undo, ⌘Z, per window), shown in the Changes list and
  marked with a dot on the field, the tab, the sidebar row and the picker
  tile. Revert per field (⟲, like Unreal's reset-to-default arrow), per object
  and per project. "Build" is no longer a modder concept: Test and Install
  build what they need.

### 3.5 Concurrency (AGENTS.md hard rule)

The main thread only draws and takes input. Lanes, each independent:

| Lane | Work | Runs in |
|---|---|---|
| Reads | catalog, inspect, index search, format decode | service read pool (concurrent) |
| Images | texture decode, thumbnails, previews | detached tasks; service thumbnail cache |
| 3D | model decode, mesh building, RealityKit entity creation | detached task → `@MainActor` only to attach the entity |
| Exports | texture/sky/world/vehicle export | service job (child process) |
| Builds | save member, package build, install/enable | service job; locks the project/profile only |
| Index | asset index build | service job |
| Game | test runs | service job; locks the dev profile and memory card only |

- **No app-wide busy state.** `busy` and `gameRunning` disappear as global
  disabling flags. A running job takes resource locks (`kit_service.Locks`):
  `asset:<assetId>`, `project:<path>`, `profile:dev`, `memcard:test`.
- **What greys out, and where:** only controls that would *write* a locked
  resource. Example: while "Building mod" holds `project`, Handling fields
  stay readable and selectable, show a small spinner badge "Being written",
  and editing is refused with a tooltip and inline reason "Read-only while
  the mod builds. Edits return when it finishes." Browsing, inspecting,
  previews, other workspaces and starting an export stay live. While a test
  race holds `profile:dev`, Install is disabled ("The test race is using the
  mod. Stop it or wait."); editing the project continues and the change list
  marks edits made after the race started ("Not in this race").
- **Jobs panel** (bottom panel tab, also the activity popover): one row per
  job: kind icon, label ("Export textures to …/exports"), state (queued,
  running, cancelling, done, failed), determinate progress with counts and
  time remaining when known, the resources it holds ("Writing Bus · paint"),
  Cancel (sends the service cancel; exports keep a resumable manifest), Show
  Result / Show Log, and Retry for failed jobs. Completed jobs collapse into
  a "Done today" group. A dock badge shows running jobs.
- **Per-asset indicators:** a small progress ring on the sidebar row, picker
  tile, livery cell or tile that a job is writing; tooltip names the job.

### 3.6 Disabled-with-reason pattern

A feature that exists in the product but cannot run for this object is shown,
disabled, with a one-line reason under or beside it (not only a tooltip), and
where possible a link to the nearest thing that works. Examples are in
section 5. Nothing is hidden for being unsupported: the owner treats a
missing button as a missing feature.

### 3.7 Where-am-I contract (every screen)

| Element | Answers |
|---|---|
| Window title + sidebar highlight + selected job segment | Where am I |
| Breadcrumb subtitle, Back/Forward history | How did I get here |
| Breadcrumb root when opened from Home or Search ("Home › Change a sky") | Why is it open |
| One prominent primary action per screen; empty state with one button | What next |
| Modified dots + Changes count | What have I done |

---

## 4. Screen specs

Control sizes: regular control size (22 pt fields, 28 pt segmented bar,
28 pt sidebar rows at medium sidebar size), hit areas ≥ 28 × 28 pt; 20 pt
spacing between groups. Sliders always pair with a numeric field and a
stepper and show the unit (HIG sliders). Labels are plain words; raw keys are
secondary text.

### 4.1 Home

Title: **Home**. Content: a "Make" grid of job tiles (min 220 × 96 pt, icon,
title, one line of consequence or status) and, beneath, "Continue" (the last
five objects edited, with their change counts) and "My Mod" (package state:
Not built / Built 3 changes / Installed and on).

| Tile | Opens |
|---|---|
| Repaint a car | Cars picker → Paint tab |
| Tune a car | Cars picker → Handling tab |
| Edit a car's model | Cars picker → Model tab |
| Change a sky | Tracks picker → Sky tab |
| Retexture a track | Tracks picker → Textures tab |
| Move or remove props | Tracks picker → Props tab |
| Open up walls | Tracks picker → Walls tab |
| Sell the school bus in Career | Career → Dealer, Bus row selected |
| Drive a boat in Career | Career → Special vehicles, boat trailer row (disabled, reason) |
| Edit my save | Saves |
| Change game text | Text & Menus → Strings |
| Make a 4K texture pack | Game Files → Export All Textures (job) |

### 4.2 Car workspace

Picker: grid grouped Compact (10) · Midsize (8) · Muscle (9) · Special (6) ·
Trailers (5); tile = render, name, dealer price or "Not sold", three rating
bars (speed / handling / durability from `car_stats.val`). Search by name.

Header: `Bus` · "Special · not sold at the dealer · BUS.CAR". Job bar:
**Paint · Handling · Model · Test Drive**.

**Paint.** Left: livery list (cells 64 pt tall: swatch, number, who uses it):
`790 · Wayne Nesbit (AI)`, `791 · Randy Crow (AI)` …; for dealer cars the five
dealer paints (`800–804 Digit`). Centre: before/after of the selected livery
texture (512 × 256), segmented **Texture · On the car**. Primary: **Replace
with Image…** (shows the consequence before choosing: "Same size 512 × 256 →
native; 2×, 4× … 32× → high resolution, needs this Kit's game build").
Secondary: Export PNG…, Revert. Inspector: livery number, members it writes
(`CT.RES#790.tm0`, `#790.tex`), every driver and car using it.

**Handling.** Grouped form, all groups open, pinned group index at top:

| Group | Label | Raw key | Unit | Control | Bus value |
|---|---|---|---|---|---|
| Weight & grip | Weight | `[Chassis] mass` | lb | slider 500–12,000 + field + stepper | 9000 |
| | Grip | `[Chassis] grip` | × | slider 0.5–2.0 | 1.2 |
| | Slip | `[Chassis] slip` | × | slider | 2.5 |
| | Load factor | `[Chassis] load_factor` | × | field | 3.0 |
| | Brakes | `[Chassis] brake_factor` | × | slider | 2.0 |
| | Springs | `[Chassis] spring_factor` | × | slider | 2.0 |
| Engine | Peak power | `[EngineNormal] power_max` | hp | slider 50–1,000 | 420 |
| | Peak torque | `[EngineNormal] torque_max` | lb-ft | slider | 350 |
| | Redline | `[EngineNormal] redline` | rpm | slider 3,000–12,000 | 8600 |
| Gearbox | Gears | `[Transmission] num_gears` | — | stepper 1–6 | 4 |
| | 1st–4th gear | `[Transmission] g1…g4` | ratio | field + stepper | 3.50 / 2.50 / 1.50 / 1.00 |
| | Final drive | `[Transmission] rear_end_ratio` | ratio | field + stepper | 10.5 |
| Tyres | Front / rear tyre diameter | `[Front]/[Rear] tire_diameter` | in | field | 43.3 / 43.3 |
| Damage | Damage taken | `[Chassis] damage_scale` | × | slider 0–2 | 0.3 |

Below: **Values the game does not read** (rate, bump, rebound, camber, toe,
sway, brake_force, wheel_lock, rspoiler_angle, fuel_capacity): shown,
read-only, reason "Not read by the game; changing it has no effect." Then
**All values** (expert), a full-width 32 pt header button opening a
searchable table of every key per section, edits allowed, labelled
"Effect not traced" where applicable. A "Compare with" pop-up shows another
car's value beside each field (class peers by default).

**Model.** RealityView of the assembled car (LOD 1, orbit, wireframe toggle),
LOD list (`Low0–Low4.mod`, `shadow1–2.mod` with triangle counts), materials.
Actions: **Export for Blender…** (OBJ/MTL/PNG), **Import Edited Model…**
(same topology; refusal names the model and the mismatch), **New Car From
This One…** (sheet: name ≤ 8 letters/digits, number, class; trailers refused
with reason).

**Test Drive.** Track picker (14 raceable tracks), Event picker (that
track's Eves and challenges), Livery picker, last capture image and log
excerpt. Primary: **Test Drive**.

### 4.3 Track workspace

Picker: 15 tracks by display name (`tracks.val`: Anoka City Speedway, Woodly
Racetrack (`AFTON.TRK`), Ashland Raceway (`SPRING_1.TRK`), Hampton Raceway
(`BEMIDJI.TRK`) …), sky status, event count.

Header `Anoka` · "City Speedway · ANOKA.TRK". Job bar: **Sky · Textures ·
Props · Walls · AI Line · Events · Test Race**. A RealityView viewport
(top-down by default, orbit with ⌥-drag) is shared by Props, Walls and AI
Line; the left side of the viewport has a 220 pt outliner (Scenery, Event
props, Collision, AI lines, Sky) that follows the tab and drives selection.

- **Sky**: panorama (equirectangular, x = azimuth), tile strip (16 horizon +
  16 cap), segmented **Panorama · Tiles**. Actions: **Import Panorama…**
  (primary; consequence before choosing: "2,048 px wide = native; 8,192 px →
  tiles at 4× (512 px)"), **Replace Tile…**, **Tint…** (colour well + hue,
  saturation, brightness sliders, live preview), Export. Inspector: selected
  tile, azimuth/elevation range, neighbours, duplicate keys (Anoka
  `sky12B`/`sky13B` share one).
- **Textures**: thumbnail grid of the track's textures (Anoka 209 names),
  same replace flow as Paint.
- **Props**: event selector (layouts `c_<event>.va0`; Anoka figure-8 race
  212 props: Hay_Bale4 35, Hay_Bale3 26, Hay_Bale 25, mike_barrier 16, wall 14
  …), list + viewport selection, Move/Remove with numeric X/Y/Z in metres.
  Status line: "Moving a world model does not move its collision."
- **Walls**: collision boxes drawn top-down coloured by height above ground;
  filter "Taller than 10 m" (Anoka 574 of 709 boxes); Lower to… (metres,
  shrink-only).
- **AI Line**: line chooser (`oval.ild` 268 nodes, `fig8.ild` 225, …), drag
  nodes in X/Z; warning "Stored tangents don't update."
- **Events**: the track's Eves and challenges with mode, car class and
  contests; opens the event in Test Race.

### 4.4 Career workspace

Tabs: **Dealer · Special Vehicles · Eves · Upgrades**.

- **Dealer**: table of the 26 `buycar.val [car]` rows (chassis, class,
  price, speed/handling/durability, description), editable price and
  description; a "Price multiplier" field (`[settings] car_price_multiplier`
  1.0).
- **Special Vehicles**: Ambulance, Bus, Cop Car, Hearse, Postal, Taxi (class
  Special, AI only) and the five trailers. Per row: **Put on Sale…** opens a
  sheet pre-filled from `build_career_specials` (Bus → "Schoolbus", Midsize,
  $280, vehicle 785; peers Ocelot $230, Desio $325). The boat trailer row is
  disabled: "The boat is a towed trailer. A drivable boat needs a new
  chassis and mesh the Kit can't build yet." with a link to the Trailer
  Derby events.
- **Eves**: the 25 Career Eves in order with rank windows (Ashland
  Smackfest 100→90, Woodly Crash Test 100→85, …).
- **Upgrades**: `catalog.val` parts by class (Turbo Engine Mod $50 / $250 /
  $2,000 …).

### 4.5 Saves

Picker: memory card saves found in the game's memory card folder and the
Kit's isolated test card. Workspace header: driver name · "Career rank 1st ·
3 of 25 Eves played". Groups: **Money** (field, $), **Reputation** (total and
its three parts), **Garage** (4 slots: car + paint picker of career vehicles
800–929; a 5th slot shown disabled: "The save holds four cars."), **Car
looks** (draw an owned car as another vehicle, e.g. the Bus; stored in
eod.ini), **Settings** (OPTs groups). Primary: **Save to Card** (backs up the
file first, rewrites the checksum). If the game is running on that card:
read-only, "The game is using this memory card."

### 4.6 Text & Menus

Tabs: **Strings · Menu Images · Layouts**. Strings: searchable two-column
table (key group as section header, text editable inline, 877 entries),
inspector shows key, original, placeholder rules (`%d`), line breaks.
Menu Images: thumbnail grid (264), same-size frame replace. Layouts: disabled,
"Menu layouts can be viewed in Game Files; editing isn't supported yet."

### 4.7 Game Files (expert)

Today's three-column archive browser, kept: archives → members → labelled
format view, raw bytes toggle, Export File…, Where Used. Reached from the
sidebar section and from every inspector ("Show in Game Files").

---

## 5. Copy

Titles are nouns; actions are verbs; reasons say what happens and what to do.

| Screen | Title / subtitle | Empty state (one action) | Primary action |
|---|---|---|---|
| Home | Home | — | (tiles) |
| Cars picker | Cars · "37 vehicles" | — | (choose a car) |
| Car | `Bus` · Special · Not sold · BUS.CAR | — | Test Drive |
| Paint | Paint | "No liveries use this car." → Show All Liveries | Replace with Image… |
| Handling | Handling | — | Test Drive |
| Model | Model | "No model to preview." → Show in Game Files | Export for Blender… |
| Tracks picker | Tracks · "15 tracks" | — | (choose a track) |
| Sky | Sky · "32 tiles · verified in game" / "not yet seen in game" | "EmptyTrack has no sky." → Choose Another Track | Import Panorama… |
| Props | Props · event name | "This event places no props." → Choose Another Event | Test Race |
| Walls | Walls | "No collision on this track." | Lower Selected… |
| Career | Career | — | Test Career |
| Saves | Saves | "No memory card saves yet. Play the game once to create one." → Choose Folder… | Save to Card |
| Text & Menus | Text & Menus · "877 strings" | "No string matches." → Clear Search | Test |
| Changes panel | Changes | "No changes." | Install |
| Jobs panel | Jobs | "Nothing running." | — |

Disabled reasons (shown inline):

| Control | Reason |
|---|---|
| Value not read by the game | Not read by the game; changing it has no effect. |
| Test Drive with an unmapped car | This car has no verified race setup. Test with Algar or choose another car. |
| Replace with Image on direct-colour texture | Only 4- and 8-bit textures can be replaced. |
| Drive a boat | The boat is a towed trailer. A drivable boat needs a new chassis and mesh the Kit can't build yet. |
| Import model with new topology | This model has a different number of points than Low1.mod (2,014 vs 1,980). Edit the exported OBJ without adding or removing points. |
| Sky on Welch Valley | Welch Valley's sky has a second mirrored half; tile placement isn't verified. |
| Garage slot 5 | The save holds four cars. |
| Editing while a build writes this car | Read-only while the mod builds. Edits return when it finishes. |
| Install while racing | The test race is using the mod. Stop it or wait. |

Errors (what happened · what it means · what to do):

- "Couldn't read BUS.CAR. The game folder changed since the Kit indexed it.
  Choose Game Folder… or Rebuild Index."
- "The test race closed before the first frame. Nothing was captured. Open
  the Log to see why, then try again."
- "The save's checksum didn't match before editing. The Kit didn't change it.
  Make a copy and open the copy, or play the game once to rewrite it."
- "Bus paint changes can only be built when the bus uses its standard paint
  set. This copy differs, so nothing was saved." (existing backend sentence,
  kept)

---

## 6. Implementation plan

### 6.1 Platform

Raise `Package.swift` to `.macOS(.v15)`: `RealityView` on macOS needs 15;
`@Observable` and `.inspector` need 14. Remove `.preferredColorScheme(.dark)`.
Replace `ObservableObject StudioState` with `@MainActor @Observable` models.
Keep `KitServiceClient` (JSON-RPC over Unix socket) as the only backend path;
retire `RuntimePaths.spawn` per-request Python processes once each action is
served by the service.

### 6.2 SwiftUI components

`AppShell` (NavigationSplitView + toolbar + bottom panel), `SidebarView`,
`HomeView`, `JobTile`, `ObjectPicker<Item>` (grid + search + groups),
`WorkspaceHeader` (title, identity line, `JobBar` segmented), `BreadcrumbTitle`
(navigationTitle/Subtitle), `NavigationHistory` (back/forward),
`ValueRow` (label, `Slider` + `TextField` + `Stepper`, unit, original + revert,
raw key, disabled reason), `GroupedForm` with pinned group index,
`ExpertTable` (searchable key/value table), `TextureCompare` (before/after,
on-the-car), `CarViewport` / `TrackViewport` (RealityView, entity built off
main), `Outliner`, `SkyPanoramaView`, `TileStrip`, `InspectorHost`
(re-targets on `Selection` enum), `ChangesList`, `JobsList`, `LogView`,
`IssuesList`, `ActivityView`, `DisabledReason` view modifier, `BeingWrittenBadge`.

### 6.3 Worker split (disjoint files)

| Worker | Owns (new or rewritten) | Depends on |
|---|---|---|
| W1 Shell | `App.swift`, `Shell/AppShell.swift`, `Shell/SidebarView.swift`, `Shell/Toolbar.swift`, `Shell/BottomPanel.swift`, `Shell/HomeView.swift`, `Models/AppModel.swift` (route, history, selection), `Models/ProjectModel.swift` (changes, undo), `Models/JobsModel.swift`, `Components/*` shared (ValueRow, ObjectPicker, WorkspaceHeader, DisabledReason, BeingWrittenBadge), `Package.swift` bump | `job.list/status/cancel`, `lock.list` (exist) |
| W2 Car | `Workspaces/Car/CarWorkspace.swift`, `PaintView.swift`, `HandlingView.swift`, `ModelView.swift`, `TestDriveView.swift`, `Models/CarModel.swift`, `Viewports/CarViewport.swift` | W1 components; backend `car.list`, `car.handling` (curated schema), `car.liveries`, `vehicle.export_obj` job, `vehicle.import_patch` job, `vehicle.new` job |
| W3 Track | `Workspaces/Track/*` (Sky, Textures, Props, Walls, AILine, Events, TestRace), `Viewports/TrackViewport.swift`, `Models/TrackModel.swift` | backend `track.list`, `sky.export`/`sky.reimport` jobs, `world.props`, `world.collision`, `ilin` via format view |
| W4 Career + Saves | `Workspaces/Career/*`, `Workspaces/Saves/*`, `Models/CareerModel.swift`, `Models/SaveModel.swift` | backend `career.tables`, `career.put_on_sale` (wraps `build_career_specials` logic per chassis, as project edits not a profile), `save.list/read/write` (Python port of `eodsave`: HSCM v3, checksum, money, reputation, garage, OPTs; backup before write) |
| W5 Text & Files | `Workspaces/Text/*` (Strings, MenuImages), `Workspaces/GameFiles/*` (refactor of today's `MainView` columns and `FormatInspectorView`), `Views/LogView.swift` | existing format view contract (`describe`, `apply_edits`), `index.search/get/thumbnail` |
| W6 Backend | `tools/kit_service/backend.py` new actions above, `kit_service/jobs.py` job kinds (vehicle/sky/world/collision/save-write), resource lock names (`asset:`, `project:`, `profile:dev`, `memcard:`), `tools/kit_service/careers.py`, `tools/kit_service/saves.py`, tests in `tools/tests/test_kit_service.py` | none (start first) |

Order: W6 and W1 in parallel first (W1 against stubbed models), then W2–W5
in parallel. `AppState.swift`, `MainView.swift` are retired by W1 and W5
together (W5 moves the archive browser, W1 deletes the rest).

### 6.4 New backend actions per job

| Job | Action | Notes |
|---|---|---|
| Repaint | `car.liveries(chassis)` → numbers, drivers, members; texture replace targets `CT.RES#<n>` | generalise the Bus-only rebuild via `vehicle_paint` |
| Handling | `car.handling(archive)` → schema (label, unit, range, read-by-game, evidence) | static schema file; values from VALT |
| Sky | `sky.export` job, `sky.reimport` job (panorama or tiles, k) | wraps `sky_export.py` |
| Props | `world.props(track, event)`; `world.props_edit` (dry run + apply) | `c_*.va0 dyno`, `world_graf` |
| Walls | `world.collision(track)`; `world.set_limit` | wraps `world_collision.py` |
| Career | `career.tables`; `career.put_on_sale(chassis, name, class, price, number)` | writes buycar/carclass/car_stats/CT/UICAR into the project; `career_content_check` as validation |
| Save | `save.list`, `save.read`, `save.write` (backup, checksum) | port of `runtime/core/eodsave.cpp`; `memcard:` lock against game runs |
| Test | `launch` with track, event, car, livery | extend `launch_plan` beyond the direct-start mapping |

### 6.5 Verification

1. Per screen: screenshot of the visible Kit window (light and dark) for
   every state in section 4, attached to the acceptance record.
2. Concurrency: start Export All Textures (4,923), then browse 10 cars,
   open the Bus model viewport and edit a handling value; frames must stay
   under 16 ms (Instruments, Hangs) and the export must keep progressing.
   Repeat with a test race running.
3. Jobs from section 2 driven through the UI (not the backend) on retail
   data, each ending with a native capture: Bus livery, Bus 15 % heavier,
   Anoka panorama sky, Schoolbus bought at the dealer in a test Career,
   save money edited and loaded by the game.
4. Hallway test with two people who have not used the Kit: "make the bus
   red and race it" and "put the bus on sale", record time and wrong turns.

---

## 7. Data and sources

### 7.1 Real data used by the prototype

Extracted read-only with repository tools (`rst0.unpack_archive`,
`valt.parse_binary_valt`, `formats.describe`, `sky_export.py export/registry`,
`world_collision.py census/list-limits`, `vehicle_export.py --preview`), from
the census of 72 archives / 7,329 members. Images for the prototype (renders,
paint textures, Anoka panorama and tiles, Anoka collision top-down) are
written to `work/kit-prototype-assets/` (ignored by Git), e.g.:

```sh
python3 -I tools/vehicle_export.py --disc "$DISC" --car DATA/CAR/BUS.CAR --dest work/veh --lod 1 --preview work/kit-prototype-assets/cars/BUS.png
python3 -I tools/sky_export.py export --disc "$DISC" --track DATA/TRACKS/ANOKA.TRK --dest work/sky/anoka
python3 -I tools/world_collision.py --disc "$DISC" render ANOKA work/kit-prototype-assets/tracks/ANOKA-walls.png --size 900
```

### 7.2 Research sources

Usability (NN/G)
- 10 usability heuristics: https://www.nngroup.com/articles/ten-usability-heuristics/
- Visibility of system status: https://www.nngroup.com/articles/visibility-system-status/
- Recognition rather than recall: https://www.nngroup.com/articles/recognition-and-recall/
- Progressive disclosure (two levels practical maximum; staged vs progressive): https://www.nngroup.com/articles/progressive-disclosure/
- Accordions on desktop (heading and icon both clickable; avoid when most content is needed): https://www.nngroup.com/articles/accordions-on-desktop/
- Fitts's law (make the whole labelled area clickable): https://www.nngroup.com/articles/fitts-law/
- Navigation: you are here: https://www.nngroup.com/articles/navigation-you-are-here/
- Breadcrumbs (hierarchy, not history): https://www.nngroup.com/articles/breadcrumbs/
- Empty states (status, learning cue, direct pathway): https://www.nngroup.com/articles/empty-state-interface-design/
- Wizards (novices, infrequent tasks; not experts): https://www.nngroup.com/articles/wizards/

Apple
- HIG Sidebars (≤ 2 levels, let people hide, avoid critical actions at the bottom): https://developer.apple.com/design/human-interface-guidelines/sidebars
- HIG Split views: https://developer.apple.com/design/human-interface-guidelines/split-views
- HIG Disclosure controls: https://developer.apple.com/design/human-interface-guidelines/disclosure-controls
- HIG Panels (inspector follows selection; sliders and steppers): https://developer.apple.com/design/human-interface-guidelines/panels
- HIG Toolbars (one prominent primary action, trailing; menu parity): https://developer.apple.com/design/human-interface-guidelines/toolbars
- HIG Sliders (pair with field/stepper, label ends): https://developer.apple.com/design/human-interface-guidelines/sliders
- HIG Accessibility (macOS control size 28 × 28 pt default, 20 × 20 pt minimum): https://developer.apple.com/design/human-interface-guidelines/accessibility
- HIG Onboarding (teach in context, keep it optional): https://developer.apple.com/design/human-interface-guidelines/onboarding
- WWDC23 "Inspectors in SwiftUI: Discover the details" (trailing column, `inspectorColumnWidth`, placement with NavigationSplitView): https://developer.apple.com/videos/play/wwdc2023/10161/

Jobs to be done
- Christensen, Hall, Dillon, Duncan, "Know Your Customers' Jobs to Be Done", HBR 2016: https://hbr.org/2016/09/know-your-customers-jobs-to-be-done
- Job story format (When / I want to / so I can), attributed to Alan Klement; original post at https://jtbd.info/replacing-the-user-story-with-the-job-story-af7cdee10c27 (not retrievable at writing; format confirmed via https://www.mountaingoatsoftware.com/blog/job-stories-offer-a-viable-alternative-to-user-stories)
- Ulwick, Outcome-Driven Innovation: https://strategyn.com/outcome-driven-innovation-process/ (not fetched)

Comparable tools
- Unreal Content Browser: https://dev.epicgames.com/documentation/en-us/unreal-engine/content-browser-in-unreal-engine
- Unreal Details panel (properties of the selected actor, categories, search, modified indicator with reset): https://dev.epicgames.com/documentation/en-us/unreal-engine/level-editor-details-panel-in-unreal-engine
- Forza Horizon livery editor (Garage › Designs › layers and vinyl groups): https://support.forza.net/hc/en-us/articles/360005306774-FH3-Painting-Designs-and-Vinyls
- Mod Organizer 2, Halo Sapien/Guerilla, FlatOut and GTA toolchains: sources in docs/MODDER_JOURNEYS.md section 5.
- Not fetched (background knowledge only; verify before citing externally): Blender Properties editor and panels (https://docs.blender.org/manual/en/latest/editors/properties_editor.html), Skyrim Creation Kit Object Window (https://ck.uesp.net/wiki/Object_Window, refused automated fetch), Gran Turismo livery editor.

Repository evidence: docs/MODDER_JOURNEYS.md, FORMAT_COVERAGE.md,
FORMAT_VIEW_CONTRACT.md, CREATION_KIT.md, CREATION_KIT_ACCEPTANCE.md,
MCP_SERVER.md, CAREER_UNLOCKS.md, CAREER_UPGRADES_DECOMPILATION.md,
SETU_CAR_SETUP.md, SKY_EXPORT.md, COLLISION_AND_BOUNDARIES.md,
VEHICLE_OBJ_PIPELINE.md, LANG_GAME_TEXT.md, IMAG_INTERFACE_IMAGES.md,
UNIFIED_ARCHITECTURE_MAP.md; runtime/core/eodsave.{h,cpp};
src/game_patches.cpp (car looks); tools/build_career_specials.py;
tools/modding_backend.py; tools/kit_service/.
