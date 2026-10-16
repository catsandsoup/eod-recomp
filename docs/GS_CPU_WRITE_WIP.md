# UNVERIFIED actual CPU-write preservation candidate — handoff

Stopped at the user's request for a fresh chat. This candidate is **not GPU
verified and must not be described as a fixed same-page ownership bug**.
No full candidate executable was linked, no candidate GPU replay/game was run.

## Established evidence / already verified changes

- `dc7e239`: CPU reads synchronize exact requested physical pages. GSPageCoverage
  tests520 rectangles across13 formats against brute per-pixel GS addresses.
  Actual MP trace had97 unrequested extra-page reads at palette BP2FC4 because
  the old page_range conservatively includes a second page for unaligned BP.
- `698c281`: CPU writes invalidate exact requested pages too. Neighbor fixture
  baseline GPU lost a rendered neighbor(32,64,96) vs software(37,83,126).
  Integrated main29c997a GPU replay restored(37,83,126). No added sync.
- `471df6b`: stable launch-configured VSync prevents swapchain recreation on
  internal late/on-time oscillation. Interactive sample attributed44/224 GS
  samples to those resize drains and75/224 to CLUT readback fence waits.
- Same-page partial-write bug independently reproduced: GPU-colored rectangle,
  then a one-pixel CPU upload elsewhere on the same GS page, loses untouched
  rectangle pixels (GPU stale32,64,96; software37,83,126). Neighbor fix does NOT
  resolve this same-page case. Black HUD lines have not been causally attributed.

## What this WIP changes

- Host transfer notifications/cache/raster barriers move from TRXREG declaration
  to pixels actually supplied by each xfer_data chunk. Incomplete24-bit pixels
  do not notify until a complete pixel exists.
- GIF IMAGE processing coalesces contiguous qwords actually present in the call,
  not declared future bytes. It preserves partial packet continuation.
- Vulkan flushes queued earlier draws before actual CPU writes. Partial or
  masked pages are read back before CPU modification. Full-page fast path is
  allowed only if every storage block is completely overwritten by an actual
  buffered rectangle. CT24/Z24/high8/high4 formats never qualify.
- CPU reads also flush earlier queued draws (needed for local-copy source).
- New public-register fixture generator/test covers partial, neighbor, pending
  draw, full page, split IMAGE chunks, interrupted declared-full IMAGE, CT24,
  high8/high4 low/high preservation, and localcopy. No game assets committed.

## Validation actually completed

- Compiled candidate gs.cpp and gs_vulkan.cpp object files and generator.
- GSPageCoverage CPU CTest passed:520 randomized address comparisons plus
  assertions that any claimed complete block contains all unique storage units.
- GSTransferChunks CPU CTest passed:5000 actual sequential pixel-region cases,
  incomplete24-bit pixels and interrupted transfer coverage.
- **VulkanGSCPUWrites has NOT run.** Full executable link, GPU output/VRAM
  comparisons, existing VulkanGSPixels, original/new retail replays and performance
  impact remain to be tested. Do not ship this WIP without that verification.

## Continue safely

Merge source carefully into current main: main has newer presentation tagging,
shutdown and font batching changes; this branch is older outside owned hunks.
Do not replace whole files with this branch's versions. Regenerate only if HLE
registry differences require it. Then build and run CPU tests, followed by:

```sh
EOD_HEADLESS=1 EOD_MUTE=1 ctest --test-dir build-mac -R VulkanGSCPUWrites --output-on-failure
```

The test compares all4MiB GS VRAM for11 authored fixtures at1x, including preserved
alpha/nibbles; it skips77 if Vulkan is unavailable instead of accepting fallback.
A test failure may expose a fixture issue as well as renderer ordering: diagnose
rather than weakening expected storage semantics. Then run existing pixel tests
and actual scene checks. Added preservation may increase synchronous readbacks;
measure the cost rather than claiming a speedup.

Scratch diagnostics are in `/Users/monty/Documents/Codex/2026-10-16/h/work`:
`analyze_gs_trace.py`, `gs-diagnostic-plan.md`, `mp-diagnostic-summary.json`,
`mp-diagnostic-events.csv`, `make_partial_write_fixture.py`, partial/neighbor
synthetic fixtures and baseline/green logs. Parent MP trace/dump:
`/private/tmp/eod-mp-diagnostics-20261016` (trace capped50000 events). Capture
initialization itself forces VRAM readback; exclude that burst from CLUT timing.
GS trace frame counts presentations, not kernel vblank. No synchronization may
be skipped based solely on the conservative drawn masks.
