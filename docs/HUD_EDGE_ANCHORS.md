# Native race HUD edge anchors

Source and CPU-contract verification checkpoint, 2026-10-16. Rendered acceptance remains
pending. The existing matched 4:3/16:10 image shows real additional scenery but
also shows left position/lap/damage rows and right speed rows inset into the
old 4:3 area. Camera correctness does not establish HUD edge placement.

## Exact target evidence

Disassembled the supported local SLUS_209.10 (CRC32 75BB93EE) with tools/dis.py,
and checked the synchronous global-g_ee generated call convention.

- `00127050` updates the racing overlay, with actual POS/LAP/DAMAGE and KPH/MPH
  strings. It queues the row content via `0011DEE0` (right queue) and related
  native functions. This is game HUD evidence, not a classification of rendered
  texture coordinates.
- `0011CA48(a0=view)` calls `00119888`, `00119F40`, then `0011ACC0`.
  `00119888` draws the left row queue at `003C4370`; `00119F40` draws the right
  queue at `003C4600`. Each queue contains native row content, dimensions,
  padding, colour, and slide-animation state. The centred queue is separate.
- For nonnegative viewport IDs, `00119888` obtains its X anchor using
  `0011DC50`, the left endpoint at `003C4310 + view*0x18 + 4`.
  `00119F40` obtains its X anchor using `0011DC30`, the right endpoint at
  `003C4310 + view*0x18 + 8`. Their Y origins use separate accessors.
- `0011D2C8(a0=viewCount)` stores that count at `003C41AC` and builds the
  rectangle table: top+0, left+4, right+8, bottom+12, centreX+16, centreY+20.
  At `0011D550`–`0011D670` it derives the native bounds from 640x480 and the
  actual row/column counts, then applies the original overscan values. Solo
  and the game's two-player horizontal divider both have a full 640-unit X
  span; more views use different columns and are intentionally unsupported.
- `00297498` supplies overscan. Original 4:3 returns top/bottom36 and
  left/right48. Native wide/letterbox flags select different left/right values
  (-58/-53). Thus deriving an inset from an assumed fixed safe-area constant
  or permanently rewriting the table would be incorrect. The wrapper retains
  the actual existing value, including initialization timing and asymmetry.
- The real per-player loop `00101C00` derives the viewport with
  `00211398` and installs it using `0034FAF0` before calling
  `0011CA48` at `00101D34` with the current view ID from HUD state+0x20.
  Native HUD matrix `00297A88` is identity with Z=-365; it retains the active
  native projection. No host GS-vertex transform is needed for the relocation.
  The separate full-screen call at `001063DC` passes view=-1 and is retained.

In the existing comparison, the 16:10 inset is approximately 8.3% of the full
viewport width, consistent with a centred HUD X scale of 5/6. This measurement
is independent corroboration of the shift size, not a new precise calibration.
For the supported full-width view, extending an anchor by
`320 * (1/k - 1)` before that native projection gives 64 native units at 16:10
and 106.6667 at 16:9. After projection these account for the added half-width
without changing row sizes. This is an edge translation; it preserves whatever
border overlap or clipping the original 4:3 panel has.

## Scoped implementation

`src/hle_hud.cpp` wraps only the two semantic row-draw functions. It temporarily
subtracts delta from the selected left endpoint during the left draw, or adds
it to the selected right endpoint during the right draw, calls that original
exactly once, then restores the endpoint's exact original bits. All other
layout words and all native EE return-register and draw-list side effects are
retained. The native row animation continues to consume its own shifted edge;
when a row is sliding, its native animation equation controls its displacement.

`eod_presentation_aspect()` provides the same launch-cached race scope as the
camera hook: 16:9/16:10 qualify only inside `00114020`; title/menu/movie/preview
contexts return 4:3. Stretch/4:3 pass through. Only native count1/2 and a valid
nonnegative view ID qualify. Negative/global, unknown counts, invalid view IDs,
and nonfinite anchors pass through. Centred `0011ACC0`, global rectangles,
world UI, generic sprite/text functions and GS packets are untouched.

`EOD_HUD_EDGE_ANCHORS=0` disables the relocation for same-binary comparisons.
No other value disables it. `EOD_HUD_TRACE=1` enables diagnostics: first eight
calls per side, then every120th, at most32 lines per side. Each line includes
side, view/count, aspect, actual anchor, applied shift, current x/base scales
and caller. Diagnostics are bounded and do not infer HUD element identity from
indexes or screen coordinates.

## Reuse decision

Rechecked RoadTripAdventure-recomp main at
`0f6d3613e8e32ccda5305227a578b1fb659983a8` and its PS2Recomp submodule at
`818f279be3f9cdecaa9c7b4a3a7cbe8fe44fd6e4`:

- [Road Trip's standalone placement tests](https://github.com/silentsudin/RoadTripAdventure-recomp/blob/0f6d3613e8e32ccda5305227a578b1fb659983a8/tests/wide_layout_test.cpp)
- [PS2Recomp's WideLayout implementation](https://github.com/silentsudin/PS2Recomp/blob/818f279be3f9cdecaa9c7b4a3a7cbe8fe44fd6e4/ps2xRuntime/src/lib/gs/gs_wide_layout.cpp)

Its `place` preserves shape with left/centre/right affine placement.
`flush` groups touching rendered primitives and assigns anchors by thirds;
`transformUnit` infers HUD phase from packet path/state and adjusts scissors.
This is a useful geometry reference, but those Road Trip packet/phase
heuristics are not an EOD semantic proof, and EOD already has verified native
left/right row functions. Adapting the semantic EOD anchors is narrower than
importing that renderer-level classifier, and does not risk world/UI overlap.

## Validation and outstanding acceptance

CPU contract target links `tools/tests/hud_edge_test.cpp` with the
production `src/hle_hud.cpp`, include runtime, runtime/include, src, and runs
separately with no argument and with `disabled`. No graphics or game assets
are involved. Parent integration owns CMake, regeneration, full build and GPU
acceptance; config/hle.txt adds only the two wrappers.

Tests inspect the exact temporary guest-memory delta inside stub originals,
all register bytes after return, restoration of original layout bits, native
memory side effects, distinct viewport anchors/asymmetric insets, unchanged
shape widths, and passthrough for 4:3/Stretch/nonrace, unsupported counts and
negative/out-of-range viewport IDs. A passthrough-only implementation was first
shown to fail the native-edge-shift assertion. Stub originals do not establish
real-game layout or final raster pixels.

Focused local validation: Clang C++17 with `-Wall -Wextra -Werror` compiled the
production wrapper and CPU contract. Default, `disabled`, and `EOD_HUD_TRACE=1`
runs each passed all160 original-call cases. `git diff --check` passed. No game
launch, full build, source regeneration, or GPU workload was run by this worker.

Required game acceptance: headless/muted matched solo and 2P captures at
4:3/16:9/16:10, baseline switch off versus on in the same binary, checking each
viewport's position/lap/damage rows and speedometer, native padding/shape,
centred flags and messages, paused overlays, menus/movies and the first race
frames. Inspect EOD_HUD_TRACE to confirm nonnegative viewport IDs and actual
native scales/layout. Unknown/global rows are deliberately unchanged and must
be investigated from real traces if they carry visible edge panels.
