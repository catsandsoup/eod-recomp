# Tutorial 5: Editing AI Pathfinding (`ILIN` Road Tool) & Inventing Custom Game Modes

> **Part of the [EOD Recomp Modding & World Editor Tutorials](Modding-Tutorials)** · **Previous:** **[Tutorial 4: Cars, Wrecks & Scatter Brush](Tutorials-4-Cars-Wrecks-and-Scatter-Brush)** · **Next:** **[Tutorial 6: 4K Textures, Skyboxes & NASCAR Cars](Tutorials-6-4K-Textures-Skyboxes-and-NASCAR)**

---

## Overview

In this tutorial, you will use the **`Road (ILIN)`** tool to drag, split, and branch the 60-byte AI racing-line nodes (`*.ild`) with automatic tangent (`dirX/dirZ`) and lap-length recalculation, force **Free Roam** on any race layout, turn on **AI Auto-Drive** and **Free Cam**, and configure three custom game modes: **Tag**, **Cat & Mouse**, and **Fat Kid / Juggernaut**.

---

## Step 1: Drag, Split & Branch AI Pathfinding Nodes (`ILIN` Road Tool)

In `SLUS_209.10`, AI cars follow 60-byte nodes in `.ild` (`ILIN` v27) files loaded by `f_00137A68` into `0x003CD190` (`oval.ild`, `fig8.ild`, `track.ild`, `bw_*`).
1. Click **`Road`** in the top toolbar to display the 3D directional racing-line ribbon and yellow/magenta diamond waypoint handles.
2. Click any diamond node to inspect its coordinates, tangent vector (`dirX, dirZ` at `+8, +12`), and segment length (`segLen` at `+24`) in the **Road Node (`ILIN`) Inspector** (Page 2 of the Right Panel).
3. **Reshape the AI Line:**
   - **Drag (`White` hover):** Drag any diamond node's **Red (X)** or **Blue (Z)** gizmo arrow to reroute AI drivers over your new jump ramp or around an infield bus barricade.
   - **Split (`Cyan` hover):** Click the ribbon segment between two nodes (or click **`Split Segment`**) to insert a new intermediate waypoint.
   - **Branch / Join (`Green` hover):** Pull out or link a side-line node (`+36` header offset / `+56` `sidePtr`) to create a split passing lane or joker lap.
4. **Automatic Tangent & Lap-Length Recalculation:**
   - Raw `ILIN` files store `dirX/dirZ`, `segLen`, cumulative `dist`, and 3 header lap-length floats (`+12/+16/+20`). If left un-updated, AI cars brake or swerve erratically.
   - On save/compile (`Ctrl+Shift+S` or `F5`), `tools/formats/ilin.py` and `trk.compile_ild` automatically recompute all unit tangents, segment lengths, cumulative distances, header lap lengths, and the pointer fix-up table (`+44`)!

---

## Step 2: Force Free Roam, AI Auto-Drive & Free Cam

Open **`Races → Custom Race & AI Editor…`** (or click **`⚙ Custom Race`** in the toolbar):
- **Force Free Roam on Any Layout:** Set Launch Mode to **`Free Roam (Unbounded Sandbox)`** (or press **`F5`**) to load any event layout (`Jump Race`, `Figure 8`, `Gauntlet`) without lap limits or wrong-way resets.
- **AI Auto-Drive (`☑ AI Auto-Drive Player Car`):** Attaches `AICarUpdate` (`0x001A2D20`) to the player's car so you can watch how the AI navigates your custom `ILIN` spline hands-free!
- **Free Cam & 8K Photo Mode (`P11`):** Cycle all 10 gameplay & trackside `TV` cameras (`Triangle` / `EOD_CAMERA_KEYS`) or pause to fly the free camera (orbit, roll, FOV, DOF, hide HUD) and export `7680×4320` 8K PNG screenshots in `< 2 s`.

---

## Step 3: Setting Up Three Custom Game Modes

### 1. Tag / Keep-Away
1. Open **`DUNDAS.TRK`** or **`ANOKA.TRK`** and run **`Collision → Flatten Interior Barriers (Keep Outer Perimeter)`** to open the entire infield.
2. Scatter dirt ramps (`W_Jump4`) and hay-bale chicanes across the infield with **`B`**.
3. In **`⚙ Custom Race`** + **`F1 → Gameplay`**: Select **`Survival` (`save_ring`)** or **`Demolition Derby`**, enable **`AI Vengeance`** (`0x003C4190` bit `0x20`—forces every AI car to hunt and T-bone the player!), set **`AI Speed Scale = 1.15x`**, and drive a nimble **`MOTH`** or **`DIGIT`** against heavy AI muscle cars.

### 2. Cat & Mouse
1. Open **`GILROY.TRK`** or **`AFTON.TRK`** in the **`Road (ILIN)`** tool and pinch the racing line through a narrow gauntlet lined with **Ghost-Placed (`P`)** School Buses and concrete walls (`wall2`).
2. In **`⚙ Custom Race`**: Choose **`No-Rules Race`**, `5 Laps`, `12 AI` (mixing heavy `BUS` / `HEARSE` blockers with fast Compacts), and enable **`Manual Recovery Only`** so getting knocked off a bridge requires driving back out of the ditch!

### 3. Fat Kid / Juggernaut
1. Open **`BAXTER.TRK`** or **`EAGAN.TRK`**, place tall dirt launch ramps around the arena, and lower barn roof `SOBL` boxes (`Target Top Above Ground = 2.5 m`) so agile cars can jump onto rooftops.
2. **Play as the Juggernaut:** Pick **`BUS (School Bus)`** (`790`), enable **`Invincible Humans (GodMode)`** (`L1+R1+Square`), **`Chicken Fight`**, **`Chicken Blast Force = 3.0x`** (`75,000 N`), and **`Turbo Scale = 1.5x`**.
3. **Survive the AI Juggernauts:** Pick **`MOTH`**, turn Invincibility **off**, enable **`Battle Bots`** (armed with AI chicken launchers via `0x00227EE8`), and survive against 3 explosive-chicken-firing School Buses!

---

**Next Tutorial:** **[Tutorial 6: Live 4K Textures (`HOSTTEX`), Skybox Swapping & Importing *NASCAR: Dirt to Daytona* Cars](Tutorials-6-4K-Textures-Skyboxes-and-NASCAR)**
