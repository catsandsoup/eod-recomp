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
