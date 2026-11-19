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
