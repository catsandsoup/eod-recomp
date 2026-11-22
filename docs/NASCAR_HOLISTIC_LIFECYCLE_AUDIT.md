# Holistic NASCAR & Mod Vehicle / Track Lifecycle Audit (`SLUS_209.10` × `SLUS_204.41`)

**Date:** 2026-11-23  
**Scope:** End-to-end engineering, binary-format, audio, physics, Career/UI, World Studio, AI, and Multiplayer audit for bringing *NASCAR: Dirt to Daytona* (`SLUS_204.41`, 125 vehicles + 32 tracks) and arbitrary community mods into *Test Drive: Eve of Destruction* (`SLUS_209.10`) as **100% additive, first-party native content**.  
**Governance:** Enforces Mandates #6 (*Player-Outcome Acceptance Standard*) and #7 (*Anti-Archivist Directive*) from [`docs/EOD_2026_MASTER_ARCHITECTURE.md`](EOD_2026_MASTER_ARCHITECTURE.md). Simply having a car model spawn or a track load drivable is explicitly a **FAIL**.

---

## 1. Root-Cause Diagnosis: Why NASCAR Cars Are Stuck in 1st Gear & Repeating One Engine Sound

Empirical inspection of `SLUS_209.10`, the retail EOD disc (`DATA/CAR/*.CAR`, `DATA/RES/RACE.RES`, `DATA/RES/RACESND.RES`), the real *NASCAR: Dirt to Daytona* ISO (`DATA/*.CAR`, `DATA/RES/PART.RES`, `DATA/RES/RACESND.RES`, `DATA/TRACK*/*.TRK`), and our existing importer/runtime code identified **three compounding root causes**:

### 1.1 Unwired `ESFX` Engine Audio & Key-Name Mismatch
| Layer | *NASCAR: Dirt to Daytona* (`SLUS_204.41`) | *Eve of Destruction* (`SLUS_209.10`) | Current Defect |
|---|---|---|---|
| `physics.val` key | `[EngineNormal] sound = eng1` (`eng3` Hobby, `eng4` Modified) | `[EngineNormal] engine_sound = 4Stock.val` / `Mus68.val` | `f_0023C400` (`gen/rec_052.c:4897`) ignores `sound`, fails to find `engine_sound`, and silently falls back to VA `0x3D2298 -> "Mus68.val"` (or donor `Algar`'s `4Stock.val`). |
| Audio profile format | Binary `ESFX v2` (`PART.RES/eng1e.ens`..`eng4e.ens`, 3×44 B records): continuous pitch `rpm / sample_rpm` across `100..8100 RPM` + throttle-gated high-RPM scream layer `eng12.sfx` (`6400..8100 RPM`, `throttle > 0.2`) | Text/binary `VALT` (`RACE.RES/mus68.val`, `4stock.val`, `4tuned.val`, `6tuned.val`, `truck.val`, `v8junker.val`): 5 samples (`idle`, `onlow`, `onhigh`, `offlow`, `offhigh`), `sample_rpm=3999`, `rpm_factor=0.80` | `src/nascar_engine_audio.h` and `tools/nascar_engine_audio.py` are **pure offline scaffolding** (`runtime_enabled: False`; *"No EE/native hooks are installed"*). Neither `f_0023C400` nor `f_0023C8D0` executes `nascar_audio::mix()`. |
| Sample bank (`RACESND.RES`) | `eng10.sfx` (137 KB), `eng12.sfx` (137 KB), `eng1i.sfx` (33 KB), `eng20.sfx`, `eng30.sfx`, `blewengn.sfx` (22,050 Hz mono PS2 ADPCM `SFX0 v3`) | `4S_*.sfx`, `4T_*.sfx`, `6T_*.sfx`, `mus68_*.sfx`, `truck_*.sfx`, `junk8_*.sfx` | Importer does not mount namespaced `nseng*.sfx` into `RACESND.RES` or `nseng*.ens` into `RACE.RES` at runtime. |

### 1.2 Stuck-in-1st-Gear Transmission / Auto-Shift Mismatch (`f_00186DC8`)
In `SLUS_209.10`, **`f_00186DC8`** (`gen/rec_022.c:3008`) builds every vehicle's automatic-shift speed table at spawn time:
1. Reads `[Transmission] num_gears`, `g1..gN` (defaults at `0x3F9E00`: `3.80, 2.20, 1.65, 1.15, 0.90, 0.85`), and `rear_end_ratio`. **It never reads NASCAR's `[Transmission] ratio = 3.73` or track `.gar` (`SETU v2`) sheets.**
2. Sweeps engine RPM from **`3500.0 RPM`** (`0x455AC000`) to `redline` (`car+1032`) in **`50.0 RPM`** (`0x42480000`) steps, finding the torque-crossover point where `T(rpm) * g[i] < T(rpm * g[i+1]/g[i]) * g[i+1]` and storing upshift/downshift drive-shaft speed thresholds at `car+0x414 + i*16` (`+4` upshift, `+8`/`+12` hysteresis downshift `0.85x`/`0.90x`).
3. **Why NASCAR cars currently stay trapped in 1st gear on EOD tracks:**
   - **Superspeedway `.gar` applied globally (`nascar_roster.py`):** `RACE.RES/default.gar` (`final = 2.80–3.10`, `g1 = 2.15–2.65`) gives a **~75–98 mph 1st gear** (built for 200 mph Daytona). On EOD's tight dirt/short ovals (`Afton`, `Welch`, `Kenyon`, `Mankato`, avg speed `45–80 mph`), the car **never crosses `shift_up[0]`**, trapping it permanently in 1st gear!
   - **Donor merge path (`import_nascar_cars.py`):** Inherits Algar's `rear_end_ratio = 7.0`, `g = [3.50, 2.50, 1.50, 1.00]` with 720 HP / 600 lb-ft, causing instant redline wheelspin in 1st gear and miscalibrated shift thresholds (`DIRTCAR.CAR` also had broken `VALT v6` section slices `['Chassis', 'Speedway', 'rmal', 'ctor', '']` when parsed naively).
   - **Real D2D per-track `.gar` (`SETU v2`, 196 B, gears at `0x78`, final drive at `0x94`) proves D2D adapts gearing per circuit class:**
     - **Short Dirt Oval (`KENYON.TRK/default.gar`):** `g = [3.50, 2.70, 1.90, 1.25]`, `final = 5.67` (`dirtC.gar`: `final = 4.50`).
     - **Short Asphalt Oval (`BRISTOL.TRK/default.gar`):** `g = [3.00, 2.40, 1.70, 1.00]`, `final = 4.56` (`arcade.gar`: `final = 3.55`).
     - **Road Course (`WATKIN_1.TRK/default.gar`):** `g = [2.80, 1.80, 1.30, 1.00]`, `final = 3.70`.
     - **Superspeedway (`DAYTONA.TRK/default.gar`):** `g = [2.65, 1.90, 1.30, 0.85]`, `final = 2.80` (`truck.gar`: `final = 3.10`).

### 1.3 Exact C++ Runtime & Importer Fix Architecture (Sprint 3F-1)
1. **Hook `f_0023C400` (Engine Sound Init) & `f_0023C8D0` (Per-Frame Engine Sound Update) in `src/hle_nascar_audio.cpp`:**
   - Read live car RPM (`MEM_LW(car + 7008)` = `0x1B60`) and live throttle (`MEM_LW(car + 7044)` = `0x1B84`), plus viewed-camera state (`f_001D3F98` + `f_001D3F78` vs `car + 476`).
   - When a car's `physics.val` specifies a NASCAR `ESFX` profile (`nseng1.ens`..`nseng4.ens` or `[EngineNormal] sound = eng1..eng4`), allocate and bind native `SFX0` voices via `f_00238440` (`nseng10.sfx`, `nseng12.sfx`, `nseng1i.sfx`, `nseng20.sfx`, `nseng30.sfx`, `nsblewengn.sfx`) and evaluate `nascar_audio::mix()` every frame with an isolated audio-only PRNG stream (never touching gameplay RNG).
   - Add smooth **gear-shift RPM-drop transient & exhaust pop (`backfire.sfx`)** on gear transitions (`car_gear` delta) + authentic idle/decel modulation (`nseng1i.sfx` below 1,600 RPM) so every shift from 1st→2nd→3rd→4th produces a distinct pitch drop, rising V8 roar, and high-RPM `eng12.sfx` harmonic scream!
   - **Zero regression on retail EOD cars:** Any car referencing native `*.val` (`4Stock.val`, `Mus68.val`, etc.) falls through untouched to original `f_0023C400`/`f_0023C8D0`.
2. **Dynamic Track-Class `.gar` Setup Selector Hook (`f_00186DC8` wrapper):**
   - Preserve all 32 D2D tracks' `.gar` files and expose track-type classification (`DirtShort`, `AsphaltShort`, `RoadCourse`, `Speedway`, `Superspeedway`) at car spawn.
   - Automatically select matching `[Transmission]` ratios + `rear_end_ratio` + aero/suspension `spring_factor` / banking bias for the active circuit (e.g., `Kenyon/Afton/Welch` → Short Dirt `5.43–5.67` so NASCAR cars rip crisply through gears 1→2→3→4 between 0 and 125 mph; `Bristol` → `4.56`; `Watkins Glen` → `3.70`; `Daytona/Talladega` → `2.80` for 195+ mph drafting!), while still honoring player-tuned Career transmission upgrades and World Studio custom overrides.
3. **Fix `DIRTCAR.CAR` `VALT v6` parser edge case** and ensure all 5 NASCAR families (`Stock`, `Truck`, `Modified`, `Hobby`, `Pacecar`) plus Dirt Late Model carry distinct verified engine curves, redlines, tire models (including Career asphalt/dirt tire swap), and distinct `ESFX` sound signatures (`eng1` Cup/Truck/Pace, `eng4` Modified, `eng3` Hobby, `eng2` Dirt/V6-alternate).

---

## 2. Holistic Additive First-Party Vehicle Lifecycle (UI, Sweeney's, Paint Shop, Career, Online)

Every NASCAR or custom modded vehicle MUST appear as a **first-party additive citizen** across every screen and gameplay loop without replacing a single retail EOD car:

| Lifecycle Touchpoint | Retail EOD Mechanism | Current Gap | Required Native Additive Implementation (Sprint 3F-2) |
|---|---|---|---|
| **Class & Roster Registration** | `COMMON.RES/carclass.val`, `car_stats.val`, `dirt.val` | `nascar_roster.py` still defaults to temporary Midsize hack (`$0001$chassis`). | Register natively into **NASCAR Class 4** (`src/hle_nascar_class.cpp`, Sprint 3C) with unbounded chassis/livery tables; never overwrite retail IDs (`1..794`, `800..929`). Allocate deterministic mod ID blocks (`1000+` via manifest registry). |
| **Large & Small UI Icons** | `UICAR.RES/uicar_<num>.tex/.tm0` (256×256 showroom/arcade) + **`uicars_<num>.tex/.tm0`** (256×256 HUD/grid/standings mini-icon) | `import_nascar.py` omits `uicars_` and draws crude 16×16 colored rectangles; `buycar` vehicles need clean icons too. | Auto-render crisp alpha-masked 256×256 3/4 isometric renders of the actual 3D NASCAR `.mod` + livery into **BOTH `uicar_<id>.tm0/.tex` AND `uicars_<id>.tm0/.tex`** (plus crisp mip chain & alpha bbox matching retail `uicar_1.tm0`). |
| **Sweeney's / Career Dealership (Unpainted Primer!)** | `COMMON.RES/buycar.val` (`car` + 5 `vehicle` rows per chassis, numbers `800..929`) → `CT.RES/<num>.tm0` (512×256 **unpainted clean primer/base-coat** body skins, zero racing numbers/sponsors) | `nascar_roster.py` injects sponsored race liveries into `buycar.val` instead of clean unpainted skins! | Generate **5 authentic unpainted Sweeney's dealer skins per NASCAR chassis** (Primer Gray, Factory White, Barn Red, Steel Blue, Matte Black — preserving body panel seams, grille, headlights, roll-bar/trim AO, and **stripping 100% of sponsors & door numbers**!) registered cleanly in `buycar.val` + `CT.RES` (`base_dealer` IDs), while keeping authentic sponsored driver liveries available in Action/Arcade/AI rosters. |
| **In-Game Paint Shop (Every Panel Paintable & Saveable!)** | `PAINTKIT.RES/regions.val` (26 retail chassis UV boxes: `hood`, `roof`, `ldoor`, `rdoor`, `lfender`, `rfender`, `decklid` + mirror/rot flags) + `UIBIG.RES/<Chassis>-o.cvs` (512×256 `CANV` base UV map) | **Completely missing for NASCAR cars!** (`manifest['paint_shop'] = 'not yet mapped'`). Trying to paint a NASCAR in Career fails to spray panels or crashes on missing `<Chassis>-o.cvs`. | Auto-compute exact 512×256 UV bounding boxes for `hood`, `roof`, `ldoor`, `rdoor`, `lfender`, `rfender`, `decklid` directly from the transcoded NASCAR `Low1.mod` panel groups (`2, 9, 6, 5, 4, 3, 11`), append `[<Chassis>]` into `PAINTKIT.RES/regions.val`, and synthesize `UIBIG.RES/<Chassis>-o.cvs` (`CANV` 512×256 RGBA) so **every single panel sprays cleanly, saves to Career garage (`CDBGarageSlot`), persists across reload, and renders on track**. |
| **Career Upgrades & Damage** | `src/career_upgrades.cpp` (Engine/Trans/Tires/Suspension/Armor/Nitro/Dirt Tires) + `Bone.mvl` (157 bones) + `TJRM` detachable panel helpers (`hood`, `doors`, `decklid`, `bumpers`, `hitch`) | Verified `chassis_class % 4` fixed in Sprint 3C; need Dirt Tire upgrade hook to swap `FrontDirt`/`RearDirt` grip on dirt ovals + verify all 5 NASCAR families deform/detach cleanly. | Wire full Career purchase → Garage inspect → Paint every panel → Upgrade Engine/Tires/Armor → Save → Drive on dirt & asphalt → Repair cycle with zero crashes or state loss. |
| **Online Sharing & Netplay Sync** | `.eodmod` Schema v2 (`tools/mod_package_v2.py`) + 4-Layer Hot-Mount VFS (`runtime/vfs/layered_vfs.cpp`) + `src/netplay_content.h` | Needs automatic `.eodmod` v2 packager for NASCAR car/track packs + SHA-256 content negotiation in lobby. | Every imported NASCAR vehicle/track exports as a self-contained `.eodmod` v2 bundle (zero executable code, SHA-256 verified, hot-mountable via VFS layer 2/3) so peers auto-sync exact liveries, physics, audio, and UI icons online. |

---

## 3. NASCAR D2D Track (`.TRK`) Binary Breakdown & World Studio Importer Specification

Empirical inspection of all **32 real `.TRK` archives** inside `/Users/monty/Downloads/NASCAR - Dirt to Daytona (USA).iso` (`DATA/TRACKS/*.TRK` + `DATA/TRACKS2/*.TRK`: Daytona, Talladega, Bristol, Watkins Glen, Sears Point, Richmond, Darlington, Martinsville, Atlanta, Charlotte, California, Dover, Kansas, Las Vegas, Miami, Michigan, New Hampshire, North Carolina, Phoenix, Texas, plus the 12 dirt/short ovals Anoka, Baxter, Bemidji, Cedar, Dennison, Dundas, Eagan, Hastings, Kenyon, Mankato, Shakopee, Welch) versus retail EOD `AFTON.TRK`:

### 3.1 Binary Entry-by-Entry Comparison & Exact Transcode Path
| Subfile / FourCC | *NASCAR: Dirt to Daytona* (`0TSR v2`) | *Eve of Destruction* (`0TSR v6`) | Import / Conversion Strategy (Sprint 3F-3) |
|---|---|---|---|
| **Container** | `0TSR v2` (`140–259` entries, `4.6–8.0 MB`) | `0TSR v6` (`262+` entries, `SFX0` MUST stay at resident tail!) | Repack as `0TSR v6` via `tools/rst0.py` preserving the `SFX0`-tail resident invariant (`ddfa033`). |
| **`track.grf` (`GRAF`)** | `GRAF v13`: `2,919–7,028` `4SPS` parts, `flag1c = 2`, VU1 `MSCAL 0x1F` / `0x2D` | `GRAF v18`: `4SPS` parts, VU1 `MSCAL 0x4E` | Transcode `GRAF v13 → v18` + `4SPS` VIF streams (`0x1F`/`0x2D` → `0x4E`, preserving vertex colors, UVs, banking geometry, pit walls, grandstands, trackside props) and expose every static chunk & prop as selectable/editable World Studio objects! |
| **`track.bsc` (`SCOL`)** | `SCOL v1` (`2,180–12,297` verts, `3,363–18,308` tris, `119–671 KB`) | `SCOL v4` (capped at `32,767` tris on PS2) + native `.eodcol` (`EodCollisionMesh32`) | Convert `SCOL v1 → v4` (preserving surface material IDs: asphalt, apron, dirt, grass, wall, rumble strip, banking normals) AND emit `.eodcol` 32-bit mesh so World Studio edits, ramps, bridges, and high-poly tracks never hit the 32,767-tri ceiling. |
| **`track.sol` (`SOBL`)** | `SOBL v3` (`381–1,432` static objects, `109–409 KB`) | `SOBL v3` — **IDENTICAL VERSION `v3`!** | Direct binary-compatible parse + World Studio editable object table + spatial rebuild on save. |
| **`track.ild` (`ILIN`)** | `ILIN v19` (`stride = 76 B`, `ptrs_off = 1856`, `144–745` racing-groove nodes, banking + speed profiles) | `ILIN v27` (`stride = 60 B`, `hdr_sz = 48`) — `track.ild`, `oval.ild`, `bw.ild`, `bw_oval.ild` | Transcode `ILIN v19 → v27` directly (extracting centerline, inner/outer groove widths, corner curvature, speed targets, banking angle), generate forward (`track.ild`, `oval.ild`) AND reversed (`bw.ild`, `bw_oval.ild`) splines, and auto-build `lineinfo.val` (`@lookahead`, `@exit_path`, `@exceptions` from `aiexcept.val`). |
| **`*.tex` (`TEX `)** | `TEX v9` (`125–243` textures including `sky01a..16b.tex` day dome AND `nsky01a..16b.tex` night dome on Bristol/Daytona/Richmond!) | `TEX v10` (`203` textures) | Lossless `TEX v9 → v10` transcode (`tools/tex.py`), preserving BOTH day & night skyboxes so modders can toggle Day / Dusk / Night lighting in World Studio! |
| **`*.val` (`VALT`)** | `VALT v6` (`track.val`, `camera2.val`, `aiexcept.val`) | `VALT v11` (`track.val`, `camera2.val`, `camera3.val`, `lineinfo.val`, `def_objects.val`, `obstacle.val`, `shadow.val`, `trkefx.val`, `intrlude.val`, `c_*.va0`) | Upgrade `VALT v6 → v11`, map D2D `[Info]`/`[AI]`/`[Physics]`/`[Map]` keys into EOD schema, and synthesize missing EOD files (`track.bp` `BPPT`, `track.bs` `BSPT`, `def_objects.val`, `obstacle.val`, `shadow.val`, `trkefx.val`, `intrlude.val`, `camera3.val`, plus complete event layouts `c_normal_race.va0`, `c_last_man_race.va0`, `c_whip_around_race.va0`, `c_suicide_race.va0`, `c_battle.va0`, `c_kill_derby.va0`, `c_stuntman.va0`, `c_road_race.va0`). |
| **`*.gar` (`SETU v2`)** | `3–10` per-track setup sheets (`default.gar`, `arcade.gar`, `Advanced.gar`, `ai.gar`, `truck.gar`, `dirtC.gar`, `196 B`) | `SETU v2` (`default.gar`, `ai.gar`, `196 B`) | Preserve byte-exact & register with dynamic Sprint 3F-1 setup selector so Daytona runs `2.80` final drive (`200 mph`), Bristol runs `4.56`, Watkins Glen runs `3.70`, and Kenyon runs `5.67`! |
| **Ambient `SFX0`** | Absent in D2D `.TRK` | 3 ambient `SFX0` banks at resident tail | Bind clean trackside ambient `SFX0` bank at resident tail (preserving `0TSR` `SFX0`-tail rule). |

### 3.2 Modder & Gamer Expectations for Imported NASCAR Tracks in World Studio
When a modder opens `DAYTONA.TRK`, `BRISTOL.TRK`, or `WATKIN_1.TRK` in **EOD World Studio**, it MUST behave **identically to `AFTON.TRK`**:
1. **Zero-Friction Open & Visual Fidelity:** 1-click *Import Track from NASCAR ISO...* (with full 32-track picker showing track name, length, surface type, day/night sky), opening into Plume HQ 3D viewport with full banked oval geometry, crisp mipmapped textures, pit lane, grandstands, sky dome, and minimap preview (`< 2.0 s` cold import, `< 0.5 s` warm reload).
2. **Full Authoring Parity with EOD Tracks:**
   - Place, move, rotate, scale, Alt-duplicate, and delete EOD props, jumps/ramps, school buses, destructible barrels, figure-8 crossover barriers, and custom Blender meshes (`OBJ` round-trip).
   - Draw new **3D spline roads / infield shortcuts / figure-8 cross-overs** using our Milestone 2 Road Builder (`road_spline.py` + `road_builder.py` + `road_collision.py`) directly inside Daytona's or Bristol's infield!
   - Edit **AI racing splines** (visualize `ILIN` nodes, spawn grid slots, respawn nodes, Battle arena bounds, derby ringout bounds), **Day/Night skybox & sun/fog lighting**, and **per-track `.gar` gearing presets**.
   - **1-click Play Here (`Play From Camera`) & Play Full Race (with AI!)** in native `eod` directly from World Studio, and **1-click Export `.eodmod` v2** to share online with friends!
3. **Full In-Game Mode Parity:** Not just free-roam — playable in **Normal Race (up to full grid with working banking/drafting AI)**, **Suicide Race (reverse + oncoming)**, **Figure-8 / Infield Crossovers**, **Last Man / Whip-Around**, **Battle / Weapon Arena**, **Demolition Derby (infield)**, **Career Custom Eves**, and **Online Multiplayer** (auto-synced via `.eodmod` v2).

---

## 4. Unlimited Native Additive Extra Maps & Working AI (Anti-Archivist Extinction Plan)

Under Mandate #7 (*Anti-Archivist Directive*), EOD's 2004 PS2 track/event/eve limits are **mutable scaffolding**, NOT hard engine caps. We audited the exact EE functions enforcing those bounds in `SLUS_209.10`:

| Legacy PS2 Scaffolding Cap | Exact EE Address & Source | Current Failure Mode | Anti-Archivist Mutation Path (Sprint 3F-3) |
|---|---|---|---|
| **Max 40 Tracks (`tracks.val`)** | `f_00107E60` (`gen/rec_001.c:2195`: `R[16].sd[0] < 40ll`, panic `"race: Too many tracks!"`) | Adding 32 NASCAR tracks + 17 retail tracks = **49 tracks → instant boot panic!** | **Hook & Redirect** `f_00107E60` + track-lookup accessors (`0x107E60`, `0x12CC40`) to a dynamic host `std::vector<TrackDescriptor>` (unbounded 64-bit registry) while mirroring the active window into guest RAM for any un-decompiled callers. |
| **Max 80 Events (`events.val`)** | `f_00108350` (`gen/rec_001.c:2775`: `R[3].sd[0] < 80ll`) | Adding additive race/derby/battle events for extra maps silently drops events `>= 80` or overflows static array. | **Hook & Redirect** `f_00108350` into dynamic `std::vector<EventDescriptor>`, auto-synthesizing additive `Action` & `Free Race` events for every mounted `.eodmod` track! |
| **Max 40 Eves / 320 Contests / 512 Records / 7 Chassis per Eve** | `f_00123A58` (`gen/rec_005.c:6452`), `f_001244A8` (`src/battle_events.cpp:474`), `f_0012C158` | Prevents adding NASCAR Championship Eves or mixed-class grids on extra maps. | **Expand & Relocate** Eve/Contest tables in `src/battle_events.cpp` & `src/hle_additive_tracks.cpp` backed by dynamic host vectors; lift 7-chassis-per-Eve cap so all 5 NASCAR classes + all 4 EOD classes coexist cleanly. |
| **Track Unlock Roster & UI Picker (`Action` / `Free Race`)** | `f_0012C940` (`PointsRosterLoad`), `f_0012CC40` (`TrackPickerIsUnlocked`) | Extra tracks not present in retail `points.val` appear locked or invisible in menu UI. | Hook `0x12CC40` so every mounted additive track from `layered_vfs` is immediately unlocked & selectable in Action / Single Event / Split-Screen / Online Lobby, with auto-generated `UIBIG.RES/l-<track>.img` loading banner & track thumbnail! |
| **AI Navigation on Imported Tracks** | `f_0015DDE0`, `f_001633C0`, `f_00163DA0` (`lineinfo.val` + `ILIN v27` + `ai.gar`) | D2D tracks lack EOD `lineinfo.val`, `c_normal_race.va0`, and `BPPT`/`BSPT`; AI would stall on grid or drive straight into walls. | Transcode `ILIN v19 → v27`, synthesize valid `lineinfo.val` lookahead/exit bounds + `c_normal_race.va0` spawn grid + `c_battle.va0` nav nodes + `ai.gar` skill curves, and validate via **automated headless `eod` AI-lap soak test** (AI completes clean laps at competitive speed without wall-sticking). |
| **Static `SCOL` 32,767-tri & `100×100` `±1 km` Grid (`0x48D2B4`)** | `SCOL v4` 16-bit tri index + `phoblist` `±1 km` grid | Long superspeedways (Daytona 2.5 mi, Talladega 2.66 mi, Watkins Glen 2.45 mi) approach/exceed `±1 km` extents! | Wire `.eodcol` (`EodCollisionMesh32`) + native dynamic BVH collision & unbounded spatial hash (begun in Sprint 3F-3 for large ovals, completed globally in Milestone 4B) so 2.66-mile Daytona/Talladega never clip at `±1,000 m`! |

---

## 5. Player-Outcome Acceptance Criteria & Tracy/Perfetto Performance Budgets

Every sprint below is **FAIL** until all Player-Outcome Acceptance checks AND Tracy/Perfetto budgets pass automatically:

### 5.1 Player-Outcome Acceptance Matrix (No "Helper Exists" Sign-Offs)
| ID | Scenario | Exact Measurable Player-Visible Pass Condition |
|---|---|---|
| **POA-N1** | **Multi-Gear NASCAR Audio & Shift Feel** | Drive a NASCAR Cup (`eng1`), Truck (`eng1`), Modified (`eng4`), and Hobby (`eng3`) car from `0 → 120+ mph` on Afton/Kenyon and `0 → 195+ mph` on Daytona: telemetry verifies gears transition cleanly **`1 → 2 → 3 → 4`** (NEVER stuck in 1st gear!), live `ESFX` pitch scales monotonically with RPM inside each gear and **drops sharply on every upshift**, high-RPM `eng12.sfx` harmonic engages above `6,400 RPM` on throttle, and retail EOD `Algar`/`Bulldog` audio remains 100% identical. |
| **POA-N2** | **Additive First-Party UI & Zero Retail Loss** | Mount NASCAR pack alongside retail EOD disc: **all 26 retail EOD cars and all 17 retail tracks remain 100% present & playable**; NASCAR cars appear in **NASCAR Class 4** with crisp 256×256 alpha-masked 3D icons in BOTH `uicar_<id>` (showroom/selection) AND `uicars_<id>` (HUD/standings). |
| **POA-N3** | **Sweeney's Unpainted Buy → Paint Every Panel → Upgrade → Save → Drive** | In Career mode, visit Sweeney's Dealership: NASCAR appears in **5 clean unpainted primer colors (zero sponsors/numbers)**; player buys it, opens Paint Shop, sprays paint onto **every single panel (`hood`, `roof`, `ldoor`, `rdoor`, `lfender`, `rfender`, `decklid`)**, saves complex multi-layer paint job, installs Engine + Dirt Tire + Armor upgrades, saves Career, reloads, and drives onto track with **all painted panels intact, zero crash, zero missing panel, and working damage deformation (`Bone.mvl`) + detachable panels (`TJRM`)**. |
| **POA-N4** | **NASCAR Track Import → Edit → Race with Working AI** | Import real `BRISTOL.TRK`, `DAYTONA.TRK`, and `WATKIN_1.TRK` from the ISO into World Studio: open in Plume HQ viewport, place a jump/prop + spline road, switch day/night sky, compile & launch via PIE, AND run headless AI soak test proving **all AI cars launch from grid, navigate banked turns without wall-pinning, and advance laps cleanly**, while all 17 retail EOD tracks remain untouched (`> 40` total tracks active simultaneously!). |
| **POA-N5** | **Online `.eodmod` v2 Round-Trip & Zero Unsafe Payloads** | Export imported NASCAR car + track as `.eodmod` v2 (`tools/mod_package_v2.py`), mount via `runtime/vfs/layered_vfs`, verify SHA-256 content manifest, zero path-traversal/executable entries, and clean boot in headless `eod`. |

### 5.2 Tracy / Perfetto Import, Load & Runtime Telemetry Budgets
All import and runtime paths MUST be instrumented with `EOD_ZONE` (`runtime/core/eod_tracy.h`) and `perf_profiler.span()` (`tools/world_editor/perf_profiler.py`):
- **ISO Catalog Scan (all 125 cars + 32 tracks):** `< 250 ms` (currently `100 ms` for 125 cars).
- **Single NASCAR Car Full Import** (mesh transcode + panel split + `Bone.mvl` + 5 unpainted Sweeney's skins + `UICAR`/`uicars` 3D renders + `PAINTKIT` UV regions + `ESFX` audio): **`< 1.5 s` warm / `< 3.0 s` cold**, RSS peak `< 512 MB`.
- **Single NASCAR Track Full Import** (`GRAF v13→v18` + `SCOL v1→v4` + `.eodcol` + `ILIN v19→v27` + `TEX v9→v10` + EOD event synthesis): **`< 2.5 s` cold / `< 0.5 s` warm cache hit**, RSS peak `< 768 MB`.
- **World Editor UI Responsiveness on Imported D2D Tracks (`7,000+` `4SPS` parts):** Picking `p95 < 2.0 ms`, gizmo drag `60+ fps` (`frame < 16.6 ms`), Explorer search/filter `p95 < 10 ms`.
- **In-Game Runtime (`build-mac-plume/eod`):** `hle_nascar_audio` per-frame mixer cost **`< 0.05 ms`** across 20 active cars, zero audio underruns/glitches, zero frame-hitch spikes on VFS hot-mount.

---

## 6. Execution Roadmap (Zero-Conflict Parallel Sprints)

1. **Sprint 3F-1 — Native NASCAR `ESFX` Engine Audio & Dynamic `.gar` Transmission Fix (`src/hle_nascar_audio.*`, `tools/nascar_engine_audio.py`, `tools/nascar_roster.py`):**
   - Hook `f_0023C400` + `f_0023C8D0` + `f_00186DC8` to drive real `ESFX` (`eng1..eng4`) multi-layer pitch/volume audio + gear-shift backfire/drop + dynamic track-class `.gar` ratio selection (`ShortDirt`, `ShortAsphalt`, `RoadCourse`, `Speedway`, `Superspeedway`) + fix `DIRTCAR.CAR` `VALT v6` parsing + CTest & live telemetry gear-sweep verification (**satisfies POA-N1**).
2. **Sprint 3F-2 — Additive First-Party Vehicle Lifecycle (`UICAR`/`uicars`, Unpainted Sweeney's Skins, `PAINTKIT` Full-Panel Paintability, Fast-Import Tracy Budgets):**
   - Auto-render crisp 256×256 `uicar_` & `uicars_` icons, generate 5 clean unpainted Sweeney's dealer skins per chassis, auto-derive `PAINTKIT.RES/regions.val` UV rectangles + `UIBIG.RES/<Chassis>-o.cvs` canvases, wire additive NASCAR Class 4 registration (removing temporary Midsize hack), and optimize/profile end-to-end import with Tracy/Perfetto (**satisfies POA-N2, POA-N3**).
3. **Sprint 3F-3 + Red-Team Gate 2 — NASCAR D2D Track Importer, Unlimited Additive Extra Maps (`>40` Tracks / `>80` Events) & Working AI:**
   - Implement `tools/world_editor/nascar_track_importer.py` (`GRAF v13→v18`, `SCOL v1→v4` + `.eodcol`, `ILIN v19→v27`, `TEX v9→v10` day/night sky, EOD event synthesis + World Studio UI integration), hook `f_00107E60`/`f_00108350`/`f_00123A58`/`f_0012CC40` in `src/hle_additive_tracks.cpp` to extinguish the 40/80/40/320 caps, and run **Red-Team Gate 2** (full automated UI + headless `eod` + AI-lap + audio gear-sweep + Paint Shop + Tracy telemetry suite across cars AND imported D2D tracks — **satisfies POA-N4, POA-N5**).
