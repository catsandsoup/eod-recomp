# Step 0: leftover worktree cleanup (2026-11-18)

**Situation.** `main` was at `79aa275` (R1) with three leftover worktrees from interrupted
subagents (`wf_45e54165-449-1`, `-2` on `eb25ad7`; `agent-aeb4fd0ce20ad56b6` on `79aa275`)
and uncommitted R2 work in the main checkout.

**Task.** Find what is salvageable, then remove the stale worktrees without losing work.

**Action.** Diffed each worktree against `main`, committed each worktree's uncommitted state
onto its own local branch, then `git worktree remove` on all three.

**Result.**
- `worktree-wf_45e54165-449-2` (`6b0737f`): `src/decomp/render_flush.cpp`, readable C++ for
  11 flush helpers `main` still runs as recompiled code — `0x354C00`, `0x354BB8`, `0x35A4D0`,
  `0x34F988`, `0x34F888`, `0x35E018`, `0x35DDC8`, `0x365288`, `0x35BA10`, `0x3613C0` (plus
  `0x35CB88`, which `main` already wraps). **Most valuable salvage.** Its base is pre-R1, so it
  has to be rebased and re-run under `EOD_DIFF` before use.
- `worktree-agent-aeb4fd0ce20ad56b6` (`ca34925`): `runtime/core/limit_tracker.{h,cpp}` (the
  runtime watcher for every PS2 cap, `[limit]` log lines) and `diff_enabled()` in the diff
  harness. Not wired to any pool yet; `docs/PS2_LIMITS.md` does not exist on `main`.
- `worktree-wf_45e54165-449-1` (`8201bad`): an earlier `src/decomp/render_queue.cpp`, replaced
  by `main`'s `src/render_queue.cpp`. `diff_harness.cpp` and `bone_pose.cpp` are identical to
  `main`. Nothing to take.
- Both `wf_*` branches: `EOD_BONE_SRC` RAM-search probes in `scene_emit.cpp`/`vif.cpp` and
  `g_ee_irq_hold` in the kernel. The probes are post-hoc GS-traffic heuristics (guardrail 1)
  and the bone path is already decompiled; `g_ee_irq_hold` is already on `main`. Discard.
- Main checkout, uncommitted: R2 feed half in `runtime/render/native_feed.cpp` /
  `native_renderer.h` (`ee_world_begin/end/commit`, `ee_water_*`, `KIND_WORLD/KIND_WATER`,
  `0x6A` facing flag). The caller it names, `src/native_world.cpp`, does not exist, so nothing
  calls it yet. Kept in place as the R2 starting point.

**Next decisive test.** R2: write `src/native_world.cpp` wrapping `0x358BD0` / `0x35CBB8` /
`0x325728`, then run Afton 1P headless with `EOD_NATIVE=1` and check the
`frames visible==tagged+facing` counter reaches 100%.

## R2 handoff (STAR, 2026-11-18)

**Situation.** R1 drew queued objects natively; the track world, sky and water were still the
GS's, and `native_feed.cpp` held an unwired R2 feed.

**Task.** Replace the world renderer, sky and water with readable C++ (EOD_DIFF), feed Plume from
it, fix split-screen water/passes (P8), add `EOD_NATIVE_ONLY`.

**Action.** `src/native_world.cpp` (10 functions + WaterCreate parity wrap), world leaves tagged
at their DMA REF from the C++, camera-facing parts expanded with the sun quad, water mesh from
`0x361708`'s inputs, P1 parity counted in-process, `SplitScreenParity`, limit tracker.

**Result.** docs/NATIVE_RENDERER_SUCCESS.md section 7: 0 mismatches (>= 1,024 checks each), P1
equal on 100% of frames in 5 scenes plus native-only, 0 heap corruptions. Dundas has no water
body in retail data.

**Next decisive test.** R3: skinned palettes; then R4 removes the remaining GS draws that
`EOD_NATIVE_ONLY=1` hides (shadows, particles, sprites, immediate geometry, HUD).

## R3 handoff (STAR, 2026-11-19)

**Situation.** R2 moved track world, sky and water to Plume; skinned characters, car damage dents,
detached wheel/panel debris pools, and per-view cockpit/chassis verification remained.

**Task.** Replace car damage/setup and debris pool functions with readable C++ (`EOD_DIFF`), add
`p99` P2 vertex reporting on `_exit`, pass the P2 bone/vertex gate, and run the 6-scene R3 suite
with rotated special vehicles, cycled camera views, `EOD_DIFF`, and `EOD_HEAP_CHECK=1`.

**Action.** Phase A (`41742a1`, `5e7319f`, `6c875f7`) moved car damage/panels (`src/car_damage.cpp`),
debris pool expansion (`src/debris_pool.cpp`), and camera view tracing/keys (`src/hle_camera.cpp`).
Phase B wired `p99` + `diff_add_exit_report` in `runtime/core/diff_harness.{h,cpp}` and
`runtime/render/native_renderer.cpp`, ran the P2 gate (`p2_gate2`, Dundas Battle, Hearse), ran all
6 scenes (Bus, Taxi, Career free roam, Car dealer, Copcar, Postal 2P split) plus `EOD_NATIVE_ONLY=1`
(Ambulance), and documented Section 8 of `docs/NATIVE_RENDERER_SUCCESS.md`.

**Result.** P2 bit-identical on 241,920 EE pose words and 581,652 VU1 bone/dent words (0 mismatched);
4,667,998 / 4,672,296 posed vertices within `1/16 px` (99.91%, p99 `0.0625 px`, max `0.0672 px`).
All 6 scenes + native-only: 100% P1 frame equality (queue and world), 0 `EOD_DIFF` mismatches, 0
heap corruptions, clean exits. R2 split-screen leftovers (`0x214B68` parity-off gate, Woodly 2P Battle
water) remain deferred.

**Next decisive test.** R4: move the remaining GS draws that `EOD_NATIVE_ONLY=1` drops (133,823
primitives in 1,200 vb Dundas Battle: projected shadows, particles, effect sprites, immediate
geometry, HUD, plus the non-queue cockpit quad in `0x219C48` via `0x35CAA0`) to native Plume.

## R4 handoff (STAR, 2026-11-19)

**Situation.** After R3, queued 3D objects, track world, sky, water, car damage/panels, and debris
pools ran natively on Plume, but projected shadows, flares, particles, effect sprites, immediate
geometry, and the non-queue cockpit quad (`0x219C48` -> `0x35CAA0`) were still drawn by the GS
(`133,823` dropped GS primitives in 1,200 vb Dundas Battle under `EOD_NATIVE_ONLY=1`), and VU1
`0x25F0` -> `0x2630` was untraced.

**Task.** Replace all R4 shadow, flare, particle, sprite, immediate, sphere-immediate, and indexed
immediate / cockpit quad functions with readable C++ (`EOD_DIFF`), feed them natively to Plume
(removing the PS2 `320x512` shadow RT cap and VU1 chunk caps), trace `0x25F0` -> `0x2630`, verify
D2 (`>= 1,000` checks, 0 mismatches) and P1 across the multi-scene suite, and document Section 9 of
`docs/NATIVE_RENDERER_SUCCESS.md`.

**Action.** Phase A1 (`d72b320`) moved projected shadows (`0x34C7D0`, `0x34C798`, `0x34C208`,
`0x34C4B0`, `0x35D170`, `0x35D298`) onto an adaptive full-RenderScale host shadow buffer
(`R8G8B8A8_UNORM` + main `D32` depth) and sun/point-light flares (`0x304D50`, plus `0x20F2D8`
split-screen parity lift) in `src/native_shadows_flares.cpp`. Phase A2 (`6734733`) moved 22
functions in `src/native_effects_imm.cpp` (particles `0x08E0`, simple/atlas sprites `0x01B8`/
`0x05E0`, immediate & sphere-imm geometry `0x0270`, indexed immediate / cockpit quad `0x35CAA0`)
and traced `0x25F0` -> `0x2630` (state-setup program sent by `0x3620A8`, 0 GS primitives). Phase B
ran the 7-scene headless suite with rotated special vehicles (Hearse, Bus, Ambulance, Career car,
Taxi 2P split, Copcar `EOD_NATIVE_ONLY=1`, plus Postal for `EOD_DIFF_REPLAY`), cycled all 10 camera
views including Cockpit/Bumper/Chassis, and proved `--scale 2` (`1280x896`) adaptive shadows.

**Result.** All 28 live-reachable R4 functions (of 29; `0x34EEB8` has 0 callers in retail
`SLUS_209.10`) passed D2 with `>= 1,001` checked calls and `0` mismatches. Every scene achieved
100% P1 frame equality across all 9 native kinds (`queue`, `world`, `water`, `shadow`, `flare`,
`particle`, `sprite`, `immediate`, `cockpit`), `0` heap corruptions, and clean exit (`0`).
`EOD_NATIVE_ONLY=1` dropped GS primitives fell 90.8% from `133,823` (R3) to `12,337` (R4, ~35/frame
in race — 2D/HUD/menus).

**Next decisive test.** R5 (fog, mipmaps, GS blend/alpha-test parity), R6 (skip redundant VU1
execution for native batches), and R7 (move the remaining ~35/frame 2D/HUD/menu GS draws to Plume
to reach 0 dropped primitives under `EOD_NATIVE_ONLY=1`).

## R6 handoff (STAR, 2026-11-20)

**Situation.** After R5, Plume drew every native kind in Classic only. R6 Phase A (`f0ef513`,
`7d43b3d`, `a393207`, `2927407`, `85d8aff`) added the Modern look (material records -> PBR shader
inputs, F9 / `EOD_LOOK` switch, Modern VFX on by default) with guardrails rows 1, 4..8.

**Task.** Phase B: verify Look=Modern with toggling and VFX on across a multi-scene headless suite
(Dundas day max cars, two night tracks, Career free roam, 2P, `EOD_NATIVE_ONLY=1`), numbers only,
with P5, P1, heap and one race-starting `EOD_DIFF` run, and document Section 10 of
`docs/NATIVE_RENDERER_SUCCESS.md`.

**Action.** Clean build check (`ninja -C build eod`: no work). Six 2,300-vb runs in
`D:/eod-runs/r6b` with 24 look toggles, five pad camera changes, rotated special vehicles (Bus,
Taxi, Limo, Career car, Hearse 2P, Postal native-only), 1-vblank pairs scored by
`tools/modern_guardrails.py`, and `EOD_DIFF=00214b68` on Dundas.

**Result.** Rows 4..8 PASS in all five toggled scenes (worst whole 1.042, region 1.117, row 6
0.0209%, row 7 0%, row 8 <= +0.482 ms). P5: 0 pipeline growth, 0 late sets. P1 100% for all 9 kinds
in all six runs, 0 heap corruptions, exit 0 everywhere. `EOD_DIFF` 506/506 checked, 0 mismatched.
Row 1 (native-only, Hampton night) FAIL at 44.3% non-zero vs the 50% bar.

**Next decisive test.** R7 (2D / HUD / menus on Plume, 0 dropped GS primitives, S5 <= 3.33 ms at
RenderScale 6), then the image step: decide the night row-1 bar, prove 2P split-screen camera
cycling, and close the open list in Section 10 (`0x214B68` Gilroy first call, `EOD_DIFF=all`
stall, `metal_painted`, R5 items).

## R7 VU1 no-op handoff (2026-11-21)

**Situation.** The native renderer's camera-only experiment retired the
`0x24D8` VU1 no-op on an opt-in basis. Batch, world, shadow and state
producers used the same verified instruction pairs.

**Task.** Retire the ordinary MSCAL no-op in Plume modes and verify the
interpreter and compiled state transitions.

**Action.** Reused the exact instruction-pair check at the ordinary MSCAL
boundary. Removed camera packet bookkeeping. Checked VU state and memory on
Dundas, Gilroy night and two-view Kenyon, and compared dispatch counts on
Dundas at 6×.

**Result.** The sampled Dundas 6× frames retired 590–662 no-ops after
vblank 120. The long Dundas differential run checked 1,337 calls with zero
mismatches. See `docs/NATIVE_RENDERER_SUCCESS.md` for the remaining
dispatch counts and GPU timing.

**Next decisive test.** Identify which state and GIF output following world
REFs at `0x35DEFC` still consume from `0x0270`, `0x0618` and
`0x0920`. Prove equivalent output and state on full passes across day,
night and split-screen before widening the single-leaf retirement. S4, the
native menu and render-target cases, and the owner-PC S5 benchmark remain open.

## R7 non-clipped world REF handoff (2026-11-21)

**Situation.** The original world retirement supported one 20-vertex
payload at a fixed address. Live non-clipped world REFs vary in size,
position base, color and texture LOD.

**Task.** Retire the verified non-clipped `0x0270` branch and measure
how many VU1 dispatches that removes.

**Action.** Matched the committed world tag and live VIF layout. Derived
the output and post-state from the input bank and compared both GIF
kicks, memory, registers and the active scoreboard against the
reference in recorded and live runs.

**Result.** 1,128 recorded entries matched. The 6× Dundas A/B removed
1,052 invocations at sampled vblank 240. Day, night and 2P live
checks passed, as did 97 CTest tests. The path remains opt-in and still
feeds compatibility state through GS. See
`docs/NATIVE_RENDERER_SUCCESS.md` for the measurements.

**Next decisive test.** Submit the matched world batch and its render
state directly in native order. Compare the previous GS-visible
state and output across the same scene rotation before removing the
compatibility kick and its dependence on VU-local state.

## R7 native world visibility handoff (2026-11-21)

**Situation.** The world visibility producer still uploaded graphics
microcode to VIF0, launched VU0 for every tested sphere and model, and used
VU0 macros for camera setup and LOD groups.

**Task.** Remove the visibility producer's dependence on VU0 while
preserving clipping, LOD, depth and the guest table layout.

**Action.** Derived the CPU calculation from the retail EE and VU0
instructions. Preserved visibility, matrix and depth indexing and the
quadword copies adjacent to the list pointer. Scoped the differential
exception to retired VF scratch. Added a counter for dispatches of the
retail visibility image, exact input-replay checks and CPU boundary tests.
CMake now excludes the six tracked source copies with numbered names that
were inadvertently pulled into fresh build graphs.

**Result.** The day, night, two-player and Career differential rotation
checked 2,949 live calls with zero mismatches. The native-only runs on
those scenes reported zero `vu0_mlist` dispatches. Compiled and interpreted
replays independently matched all 2,875 sampled clip/depth results. The
macOS build passed all 99 CTest tests.

**Next decisive test.** Trace the remaining graphics submission work,
remove it from the release link graph and measure the packaged game
against the full R7 lifecycle, visuals and timing gates.

## R7 bootstrap/loading icon handoff (2026-11-21)

**Situation.** The boot icon's TEX0 size caused the GS texture lookup
to read Z16S pages outside the icon upload. The optional Plume draw
also rejected normal sessions because the legacy scene emitter's
support flag was set even when idle.

**Task.** Keep the rotating icon visible in both looks while removing
its synchronous VRAM reads from the normal boot and load sequence.

**Action.** Decompiled 0x35BE08 and submitted its typed quad at the
original position in the display chain. Plume samples only the 32×32
crop after checking current memory blocks and destination ownership.
The compatibility packet stays available to the reference path.

**Result.** The live differential checked 1,117 calls with zero
mismatches. The 1× and 6× Dundas rotation across both looks recorded
138→0 synchronous readbacks with zero GS suppression. The optional
VU1 worker also passed. Headless loading-icon images agree to within
one RGB code value in the sampled matched poses. Ordinary movie
playback still has 210 synchronous readbacks in the A/B window.

**Next decisive test.** Isolate the movie-path readbacks and validate
the native movie transfer, crop and presentation at the same boundary
as the GS reference.

### 2026-11-21 R7 split-screen shadow ownership experiment (STAR)

- **Situation.** The compatibility shadow-depth scratch at BP 0x2BE0
  aliases texture uploads in a live 2P Kenyon race. The default path
  waits for the GPU to restore the shadow owner.
- **Task.** Draw both viewports from native scene depth and private
  shadow storage, remove the proven waits, and preserve moving shadows.
- **Action.** Tag the retail shadow begin/end reservations from the
  decompiled scene using the saved upper or lower viewport. Match the
  compatible packets and clipped rectangles, drain pending work before
  changing the target, and consume only the composite paired with the
  accepted same-frame prepare. Test the split route independently of
  full-screen and cascade settings.
- **Result.** The opt-in slice reached zero synchronous requests,
  deferred waits, and suppressed GS draws in tested 2P cases. Replay
  masks confirm visible shadows in both views with the native composite.
  Some moving lower-view masks still disagree; the two live runs did
  not repeat those exact poses. The normal split path remains the
  default, so players do not inherit an unverified visual change.
- **Next decisive test.** Replay the same moving lower-view inputs and
  camera/geometry states on both paths, compare the shadow-affected
  pixels in each view, then rerun the zero-wait audit before enabling
  the candidate by default.
