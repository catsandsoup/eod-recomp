# Mod & Online Engine Decompilation Log

Serial, per-function bit-exact C++ decompilation of `SLUS_209.10` engine functions that directly touch mod hooks and online netplay, verified via `diff_run` / `EOD_DIFF` (`runtime/core/diff_harness.h`).

## Task 1 — Vehicle Control, Damage/Invincibility, Turbo & Car-to-Car Impulse

Implementation: `src/decomp_vehicle_control.cpp` (preserving 100% of existing hooks in `src/hle_career.cpp`, `src/game_patches.cpp`, and `src/netplay_trace.cpp`).

| Address | Function | Subsystem | Key Struct Offsets / Globals | `EOD_DIFF` Calls | Mismatches |
| :--- | :--- | :--- | :--- | ---: | ---: |
| `0x0018AA68` | `CarSetBrake` | Vehicle Control (`hle_career`) | `car+0x1B74` (brake float) | 352 (Battle) / 869 (Race) | 0 |
| `0x0018AA70` | `CarSetThrottle` | Vehicle Control (`hle_career`) | `car+0x1B70` (throttle float) | 352 (Battle) / 869 (Race) | 0 |
| `0x0018AAD0` | `CarCanTakeDamage` | Damage Gate (`hle_career`) | `car+0x0C` (FourCC, `'TRLR'` bypass), `car+0x1E0` (driver id `0x50`), `car+0x260` (controller -> `vtable+0x10` contestant: `+0x50` state, `+0x58` flag) | 8 (Battle) | 0 |
| `0x0018AD20` | `CarSetDamage` | Scalar Damage (`game_patches`) | `car+0x1BEC` (scalar damage clamped `[0.0f, 1.0f]` via `add.s` / `min.s` / `max.s`) | 0 (vtable/indirect; static 1:1 verified) | 0 |
| `0x0018F2A8` | `HumanDriverUpdate` | Human Driver / Turbo / Weapons (`game_patches`) | `drv+0x10` (start frame), `+0x14..0x1C` (impact accum), `+0x20` (gain), `+0x24..0x30` (4-frame FFB ring), `+0x34` (mode), `+0x38` (car), `+0x3C` (input), `+0x40` (owner), `+0x44` (wrong-way time); `0x3C4190` (cheat mask: turbo `0x1\|0x2`, weapon `0x1\|0x8`), `0x3CDA8C` (frame), `0x3CDACC` (time), `0x3CE550` (speed), `0x3FA3D0` (boost pitch) | 322 (Battle) | 0 |
| `0x00189648` | `CarApplySolvedContact` | Collision Impulse & Online Handoff (`netplay_trace`) | `car+0x0C` (FourCC), `+0x28..0x30` (COM), `+0x34..0x60` (transform), `+0x64..0x90` (matrix/pos), `+0x104..0x10C` (vel), `+0x1DC` (car index), `+0x2F0` (env mult), `+0x4B0` (damage mult); threshold `741.6667f` -> `CarApplyCollisionDamage` (`0x0018CA98`) | 457 (Battle) | 0 |

### Verification Runs
- **Build**: `ninja eod` — 0 errors, 0 new warnings.
- **Outer pass (Battle 8 AI, Dennison, 3500 frames, software headless, `TurboBoost=1`, `EOD_HEAP_CHECK=1`, scripted throttle/brake/steer/L2)**:
  - `EOD_DIFF=0018AA68,0018AA70,0018AAD0,0018AD20,0018F2A8,00189648` -> `00189648`: 457/457, `0018F2A8`: 322/322, `0018AA68`: 4/4, `0018AA70`: 4/4, **0 mismatches**, **0 heap errors**.
- **Inner-callee pass (same Battle 8 AI scenario, `EOD_HEAP_CHECK=1`, excluding outer callers from `EOD_DIFF` so nested `diff_run` compares inner callees)**:
  - `EOD_DIFF=0018AA68,0018AA70,0018AAD0,0018AD20` -> `0018AA68`: 352/352, `0018AA70`: 352/352, `0018AAD0`: 8/8, **0 mismatches**, **0 heap errors**.
- **Race pass (Dennison `#0`, 1500 frames)**:
  - `0018AA68`: 869/869, `0018AA70`: 869/869, **0 mismatches**.

### STAR Handoff (Task 1 -> Task 2)
- **Situation**: Task 1 targeted the 6 vehicle control, damage gate, human driver/turbo/weapon, and solved car-to-car contact impulse functions wrapped by `hle_career`, `game_patches`, and `netplay_trace`.
- **Task**: Replace the recompiled originals with clean, bit-exact C++ backed by `diff_run` without altering any existing mod or online hook behavior.
- **Action**:
  - Created `src/decomp_vehicle_control.cpp` with exact EE FPU operations (`fadd_ps2`/`fsub_ps2`/`fmul_ps2`/`fdiv_ps2`/`fsqrt_ps2`/`frsqrt_ps2`/`fmin_ps2`/`fmax_ps2`/`madd3`), `Frame` RAII (`$sp` + `$ra` preservation), and exact exit `$v0` values required by `diff_run`.
  - Wired existing wrappers in `src/hle_career.cpp`, `src/game_patches.cpp`, and `src/netplay_trace.cpp` to call the `decomp_*` helpers instead of `f_XXXXXXXX_orig()`.
  - Updated `config/hle.txt` comments for all 6 functions.
- **Result & Subtleties for Task 2**:
  - All 5 exercised functions verified at **0 mismatches** across 1,143+ live Battle `EOD_DIFF` checks (plus 1,738 Race checks) and **0 heap errors** under `EOD_HEAP_CHECK=1`.
  - **Subtlety 1 (Implicit `$f14` argument in `HumanDriverUpdate`)**: `input->vtable+0x4C` (called at `0x001902CC`) reads `$f14` (which holds `0.9f` / `0x3F666666` left over from `0x00190288`) and writes it to `input` state. Always preserve/pass `$f14 = 0.9f` before that virtual call.
  - **Subtlety 2 (Nested `diff_run` masking)**: When both a caller (`00189648`, `0018F2A8`) and its callees (`0018AAD0`, `0018AA68`, `0018AA70`) are in `EOD_DIFF`, the outer comparison masks inner `diff_run` checks. Always run a second pass with only the inner callees in `EOD_DIFF` to verify them directly.
  - **Subtlety 3 (Headless renderer on Windows)**: Use a temporary `--config` with `Renderer=software` (`RenderScale=1`, `Fullscreen=0`) for headless `EOD_DIFF` runs on Windows to avoid Vulkan shadow-target instability and maximize throughput.
  - **Open edge**: `0x0018AD20` (`CarSetDamage`) had 0 runtime calls in standard Battle/Race (indirect/vtable scalar damage/repair path); its 9-instruction leaf body is 1:1 verified against disassembly and ready for any fire/repair/weapon scenario in Task 2.
