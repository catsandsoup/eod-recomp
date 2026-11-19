# Tutorial 3: Deleting Fences, Removing Invisible Barriers (`SOBL`) & Multi-Selection

> **Part of the [EOD Recomp Modding & World Editor Tutorials](Modding-Tutorials)** · **Previous:** **[Tutorial 2: Your First Track Mod & PIE](Tutorials-2-First-Track-Mod-and-PIE)** · **Next:** **[Tutorial 4: Cars, Wrecks & Scatter Brush](Tutorials-4-Cars-Wrecks-and-Scatter-Brush)**

---

## Overview

In this tutorial, you will learn why retail tracks block cars with 16-metre invisible walls and distant pop-in billboards, how to use **`Shift+Click`**, **`Select All Similar`**, and **`Delete All Similar`** to wipe out entire fence lines (including their overlapping **`lod-*` billboard twins**), and how to flatten **`SOBL`** (`track.sol`) collision barriers for true open-world driving.

---

## Why Retail Fences Have Three Hidden Layers

When Monster Games authored the 15 retail tracks in `SLUS_209.10`, every perimeter fence was built from three independent records:
1. **High-Detail 3D Fence Meshes (`track.grf` `child 0` leaves):** Individual fence segments (`fnce.tex`, `wire.tex`, `wd-fnc1.tex`).
2. **Overlapping `lod-*` & Billboard Twins (`GRAF` LOD groups `0x0` & `MSCAL 0x6A` sprites):** Lower-detail duplicate meshes (`child 1`, `child 2`) or camera-facing `lod-*` cards at the exact same coordinates that only draw when the camera is 50–250 m away. If you delete only the close-up mesh, the fence reappears as soon as you drive away!
3. **16-Metre Invisible Collision Walls (`track.sol`, `SOBL` v3):** 264-byte (`0x108`) Oriented Bounding Boxes with a half-height of **`ry = 15.0 m`** centered ~1 m above the road (`top = 16.0 m` above ground!). Because the `SOBL` 2D quadtree broadphase (`0x00177CA0`) never tests `Y`, that 15 m half-height is the sole barrier keeping cars inside the arena.

---

## Step 1: Multi-Selection with `Shift + Click`

1. Fly to the fence section you want to open up (for example, Woodly's outer wooden fence or Dennison's highway access gap).
2. **`LMB` Click** the first fence panel to select it.
3. Hold **`Shift`** and **`LMB` Click** neighboring fence panels to add them to your multi-selection set.
4. Drag the 3D gizmo to move all selected segments together, or press **`Del`** to delete them in one undoable command (`Ctrl+Z`).

---

## Step 2: Wipe Out Entire Fence Lines + `lod-*` Twins (`Select / Delete All Similar`)

To remove an entire fence line and every hidden `lod-*` duplicate in one click:
1. Select **one** fence panel in the 3D viewport.
2. **Right-click (`RMB`)** (or use the **Object Inspector** in the Right Panel) and select:
   - **`Select All Similar (Same Texture / Family + LOD Twins)`** — selects every fence segment sharing the primary texture (`fnce.tex`, `chlink.tex`, etc.) **plus** every sibling child in their `GRAF` LOD group (`0x0`) and overlapping `lod-*` billboard twins.
   - **`Delete All Similar (Fences + LOD Twins)`** — deletes the entire fence line and all of its distant LOD twins in a single undoable step!
   - **`Delete Selected + Overlapping LOD Twins in Radius (15m)`** — surgically opens a gap where you selected 2–3 panels while also sweeping away any overlapping `lod-*` twins at those coordinates.
3. On compile (`Ctrl+Shift+S` or `F5`), deleted retail `track.grf` leaves and their `lod-*` twins have their `q199.y` world offset patched to **`Y = -5000.0 m`** (with updated `mlist` and `0x102` cull spheres), keeping `track.grf` byte-size invariant.

---

## Step 3: Inspect & Flatten `SOBL` Invisible Barriers (`track.sol`)

1. Click the **`🛡 Collision Shield`** pill in the top-left corner of the viewport (or enable **`SOBL Barriers`** in `Explorer → Layers`).
2. All `SOBL` boxes in `track.sol` render as 3D wireframes color-coded by height above the ground:
   - **Red (`>= 10.0 m`):** 16 m invisible wall ceilings.
   - **Orange (`3.0 m – 10.0 m`):** Medium building/fence walls.
   - **Yellow (`< 3.0 m`):** Low kerbs or lowered walls.
3. Choose your barrier-removal mode:
   - **Single / Selected Barrier:** Click any red wireframe box to open the **SOBL Barrier Inspector** (Page 5 of the Right Panel) and click **`Flatten Barrier (-0.5 m Below Ground)`** (or dial **`Target Top Above Ground = 1.2 m`** for a jumpable low wall).
   - **Keep Outer World Perimeter Only:** Choose **`Collision → Flatten Interior Barriers (Keep Outer Perimeter)`** to drop all infield, barn, and shortcut walls below ground while keeping the outermost map-edge boundary so AI cars don't fall into the void.
   - **Full Open-World Free Roam:** Choose **`Collision → Remove / Flatten All Track Barriers`** to lower every vertical invisible wall on the track below ground in one undoable command (`world_collision.apply_box_edits`).
4. Press **`F5` (`Play Free Roam`)**, enable **`Manual Recovery Only`** in the `F1 → Gameplay` menu, and drive straight out into the surrounding countryside!

---

**Next Tutorial:** **[Tutorial 4: Placing Cars, Wrecked AI Cars & Scattering Props (Lumion/Twinmotion Workflow)](Tutorials-4-Cars-Wrecks-and-Scatter-Brush)**
