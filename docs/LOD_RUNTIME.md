# Car LOD runtime experiment

2026-10-16. Full Mac build and runtime traces now verify the first bounded
experiment. Two headless/muted 5000-vblank two-player runs completed with exit0;
all20 loaded car tables validated in baseline and multiplier2. The tables logged
expected native/effective thresholds and model handles. Camera/motion diverged
between the captures, so they are not accepted as a matched visual LOD comparison.
Broader visual/performance acceptance remains open. Track traversal,
fog, far-plane extension and ultrawide edge correctness remain separate work.

## Controls and bounded output

`EOD_LOD_TRACE=1` enables diagnostics on the EE thread. Like `EOD_CAMERA_TRACE`,
presence enables it (unset it to disable). Combine both for actual projection
inputs and later CPU visibility consumers. Defaults preserve native behavior.

* `0021ABF8`: after native loading, up to 64 tables and their individual levels:
  owner object, record/count/handle addresses, min, count, original/effective
  thresholds, all three record flags, original and active handles.
* `0021C6E0`: first 16 calls, then every 120th, maximum 128 lines: selected
  index/handle, scaled distance, minimum, current near/far and view-count global.
* `00218E68`: before/after the entire native selection/submission call; first two
  calls, then every 120th, at most 64 groups. After-call output
  observes the result; it does not alter submission already performed inside.
* `0034E308`: actual CPU sphere visibility result, first 16 calls then every
  4096th, maximum 128 lines. Captures caller RA, centre/radius, camera position,
  near/far and native tangent/sphere coefficients. Optional output pointers are
  shown as raw post-call words because native early rejects can leave them
  unwritten. This diagnostic does not infer a canonical six-plane frustum.

Invalid-load/rejected-multiplier diagnostics share a 64-line error budget. A
fresh process resets budgets and reads the multiplier again. Sparse samples
avoid turning a long race into unbounded logging; they do not capture every
transition or establish a per-frame performance measurement.

`EOD_CAR_LOD_MULTIPLIER=2` requests a load-time experiment. The parser accepts
only finite float values >=1; malformed/NaN/infinite/overflow values log once
and use 1. A table is changed only after its full validation: own embedded
records/count, count 1..16, valid minimum, original handles other than the native
`-1` sentinel, finite nondecreasing thresholds and finite scaled results. The
entire table is rejected before any write if validation fails. Multiply only
strictly positive thresholds; preserve signed zero and negative values. At 1,
no guest writes occur. Flags, min/count, model handles, shadow tables, physics,
simulation and the native selector are untouched. Do not infer that numeric
handle zero is invalid: these are opaque native handles, not guest pointers.

No preset or personal configuration enables the experiment. High factors may
exceed playable rendering/submission cost and have not been accepted visually.

## Concrete layout corrections and ABI evidence

Generated `gen/rec_046.c` function `00217DF0` stores these pointers before calling
the loader; confirmed with the retail ELF disassembler, not only the earlier
investigation's prose:

| Field | Role |
| --- | --- |
| object+0x74 | Original loaded model handles, four-byte stride |
| object+0xB4 | Active handle copies, initially duplicated by loader |
| object+0xF8 | Embedded 16-byte LOD records, capacity 16 |
| object+0x1F8 | Embedded native count |
| object+0x21C | Pointer to active handles, initially object+0xB4 |
| object+0x220 | Pointer to records, initially object+0xF8 |
| object+0x224 | **Pointer to count**, initially object+0x1F8 |
| object+0x548 / +0x54C / +0x550 | Scaled distance / selected index / minimum |

The earlier investigation omitted +0x224 and did not distinguish active from
original handles. `0021C130` explicitly aliases a different object's +0x74,
+0xF8 and +0x1F8 into a clone's three pointers. Thus editing every object each
frame could compound shared thresholds. This implementation only writes the
owning loader's freshly parsed embedded records; both selectors and clones
consume that storage naturally. A loader call reparses every threshold before
scaling, so reloading an owner does not multiply the prior edited thresholds.

Loader `0021ABF8` preserves entry a0 in s4 (`R[20]`); its loop at `0021B640`
calls `0034CEF0`, stores the return handle into +0x74 and +0xB4, then writes
capability flags at record+8/+12. Both selectors load the count **through**
+0x224 and compare scaled distance strictly `< threshold`, then clamp to min
and count. Disassembly checked `0021C130` and `0021C6E0`; generated bodies also
checked `00217DF0`, `0021ABF8`, `00218E68` and `0034E308`.

`tools/recomp.py` generates every guest function as `void f_ADDRESS(void)` using
global `g_ee`; `wrap` renames only the native definition to `_orig` and retains
calls/table entries pointing at the runtime wrapper. Each wrapper snapshots
needed input in host locals, calls `_orig` exactly once, then performs plain
memory/log operations. It does not restore entry registers over native results
or call any guest helper. CPU tests check all bytes of EEState, including upper
GPR halves, FPR/FACC/FCC/SP/RA, and native memory side effects after each wrapper.
This is the local synchronous generator ABI; it does not use Road Trip's
scheduler-resume wrapping API.

## Visual-car roster and lifetime

The scene-slot pointer list starts at `0x003D1C90`. Retail reserves 20 words.
The adjacent `0x003D1CE0` word is a lifecycle flag set to 1 at `0x21DF68` and
cleared at `0x21E080`. It holds no record count. The producer in `0x217DF0`
stores the object at its scene slot from `object+0x40` after initializing the
LOD table and wheels. Its callers are the scene factory `0x20FD48` and the
two constructors at `0x21C2F8` and `0x21C338`. The destructor at `0x218038`
clears the same index. `0x21DF40` and `0x21E070` bracket the scene lifetime.

Live native rendering keeps guest slots 0–19 and stores additional registered
slots in host memory. The three list traversals use a call-local snapshot
ordered by slot, skip vacated slots, and release their temporary storage on
return. Unregistering an object releases its overflow entry, and scene
initialization and shutdown clear the entire roster. Original rendering and
configured differential runs use the retail operations.

| Consumer | Access |
| --- | --- |
| `0x20F4A0` | Opens one queue, submits each visual model through `0x21A9B0`, then flushes layers 0–8 and 9. Its two direct retail callers, `0x20F508` and `0x20F870`, have no inbound direct calls or aligned function-table references in SLUS_209.10. |
| `0x218E68` | The race scene dispatcher calls the visual callback through its vtables, directly or through `0x21C108`. It selects LOD for all cars, then processes non-cockpit matrix, body and wheel passes in slot order. The viewport-specific cockpit still resolves by scene slot. The first registered slot owns the native batch so deletion of slot 0 leaves one batch callback. Its second array originally occupies 80 stack bytes, and only array elements beyond that use host scratch space. |
| `0x21C370` | Per-car transform update through `0x21BDF0`, called at `0x20E6D4`. |
| `0x21C130`, `0x20F508`, `0x21CA20` | Indexed shared chassis, auxiliary camera and slot accessor. The latter two have no observed inbound call or aligned function-table pointer. |
| `0x227B18`, `0x227CA0`, `0x227FA0`, Battle bots | Weapon and launcher geometry resolves by owner slot. The first and third EE helpers have no observed inbound call or pointer. |
| `0x2E7450`, `0x2E7748`, `0x306978`, `0x30CE28`, `0x31BFA0`, `0x32DB68` | Indexed audio, effects and water references retain the original scene slot. |
| Projected shadow flush | The shadow caster list at `0x004AE988`, with count at `0x003DC058`, supplies visual transforms and model handles through virtual callbacks. Its storage is independent of the visual roster. |

`tools/recomp.py` checks the original retail instruction at every redirected
load, store, bound and call. The scene factory gets its input count from the
session description at `+0x98C`. The world driver table at `0x00485230`
contains 20 pointers, uses the count at `0x0048522C`, and is written by the
driver factory `0x1B47E8`. The factories share description slot IDs. Running
more than 20 simulated participants requires expanding the gameplay owner
and its other indexed resources.

## Reuse research

Rechecked online RoadTripAdventure-recomp main at commit
`0f6d3613e8e32ccda5305227a578b1fb659983a8` on 2026-10-16:
[src/game/overrides.cpp](https://github.com/silentsudin/RoadTripAdventure-recomp/blob/0f6d3613e8e32ccda5305227a578b1fb659983a8/src/game/overrides.cpp).
Its `buildCameraWide`/`setFrustumScale` narrow projection matrices for the two
player cameras and separately widen the game's seven EE ground-tile frustum
points. `finishIfDone`/resume hooks apply postprocessing only when the camera
builder really returns after scheduler checkpoints. Reusable approach: scope
visual changes to verified native consumers, retain unmodified originals, and
keep CPU traversal bounds consistent with projection. Its hardcoded camera/
frustum addresses and `R5900Context`/`PS2Runtime` registration cannot be transplanted
into EOD's different ELF, visibility equations and global-state generator.
The inspected source contains no generic car LOD threshold hook to reuse.

## Integration and CPU verification

Add `src/hle_lod.cpp` to the runtime HLE sources in CMake. Add a CPU-only target
with `tools/tests/lod_runtime_test.cpp` and `src/hle_lod.cpp`; include directories
`runtime`, `runtime/include`, `src`. Invoke it separately with `off`, `one`,
`two`, `trace`, and `invalid` (environment is cached per process). No game asset
or graphics library is needed for this target. HLE config adds four `wrap`
entries. Regenerate the normal ignored game sources once integration is ready:

```
.venv/bin/python tools/recomp.py
```

Focused verification performed with Clang C++17 `-Wall -Wextra -Werror`, linking
the production wrapper. All five modes passed. Tests cover multiplier parsing,
identity including negative zero, overflow/unordered/count rejection, full guest
memory equality when off/1/trace/invalid, exact positive-word-only mutation at 2,
clone aliases, invalid handles, transactional rejection, and register/native
side-effect preservation. Stub native calls are explicitly not real-game proof.
No full CMake configure/regeneration/build, game launch or GPU run was performed
for this source checkpoint.

## Initial game evidence and remaining acceptance

Ignored logs and full4K captures: `work/lod-game/baseline/` and
`work/lod-game/lod2/`. Main build30CTests pass (including five LOD and three HUD
contracts); logs `work/hud-build.log` and `work/hud-ctest.log`. The gameplay
experiment remains opt-in, without changing the personal configuration.

## Remaining game acceptance

Coordinate with any renderer measurement and use headless/muted automation:
`EOD_HEADLESS=1 EOD_MUTE=1`, executable `--headless`. First run with
`EOD_LOD_TRACE=1 EOD_CAMERA_TRACE=1` and multiplier unset. Capture tables for real
cars/trailers; confirm finite ordered thresholds, valid min/count, handles and
record ownership/aliasing. Confirm single/batch outputs choose native levels and
capture current far/tangent values in race visibility samples. Compare unset
against multiplier1 for baseline images/state, then trial2 in a separate run.

Acceptance needs opponents moving both ways across thresholds, wheels/damage/
detached panels, mirrors, trailers, one/two views, and 4:3/16:10/16:9/ultrawide
edge sweeps. Compare selected indexes **and handles** with images, EE/submission
cost and memory/texture activity. CPU tests cannot certify assets remain resident,
no pop-in, edge coverage or full-speed rendering. Extend projection/fog/track
range only in a separate traced experiment; do not disable native culling or
force index zero.
