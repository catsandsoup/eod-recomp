# GS CPU-write preservation

Verified experimental source branch: `macos-cpu-preservation`. It is not merged
into `macos` because of the measured performance regression below.

A CPU upload can replace only part of a page last rendered on the GPU. Dropping
that page's owner without reading its remaining contents back loses untouched
pixels. The original partial-upload fixture differed from software in 8,188 of
4,194,304 VRAM bytes. This is a proven ownership defect; it has not been tied to
the owner's thin black lines.

## Actual supplied bytes, ordered writes

Host upload notifications now occur in `xfer_data`, for pixels actually supplied
by that call. TRXREG declares a future rectangle and cannot justify discarding a
page whose bytes may never arrive. IMAGE qwords contiguous in one GIF call are
coalesced; split/interrupted packets continue normally. Incomplete 24-bit pixels
wait for the remaining source bytes. Each supplied region synchronizes software
raster dependencies and invalidates affected texture memory.

Vulkan flushes earlier queued primitives before CPU reads/writes. It reads back
partial or masked GPU-owned pages before modifying CPU VRAM. Discarding an old
page without preservation requires every storage block to be fully covered by
this actual buffered write, not merely by the declared transfer. CT24/Z24 and
high-byte/nibble formats cannot use that discard proof.

This improves the shared transfer notifications and the Vulkan preservation
path. D3D11 retains its existing partial-page discard/pending-draw behavior and
has not received the Vulkan guarantee.

## Verification — October 16, 2026

- Full Mac build and all 25 CTests pass. `GSPageCoverage` checks 520 randomized
  address cases across 13 formats; `GSTransferChunks` checks 5,000 sequential
  region cases, pending CT24 bytes and interrupted transfers.
- `VulkanGSCPUWrites` and `VulkanGSCPUWrites6x` compare all 4 MiB of VRAM against
  software at 1x and 6x. All 12 asset-free fixtures pass: partial, neighbor,
  pending draw, full page, split chunks, interrupted declared-full upload, CT24,
  high8, high4 low/high, local copy and a partial row spanning two pages.
  Software fallback is a skip, never accepted as Vulkan verification.
- The local corrected font capture is pixel-identical to its reference. The
  five-frame retail race replay is byte-identical to its preceding image.
  This supports those scenes only; screenshots and retail data are not committed.
- Tests/build logs are ignored `work/codex-build-cpu-final.log` and
  `work/codex-ctest-cpu-final.log`; fixture outputs live in `build-mac/`.

The original flush-only fixture used a point on a separate render target. At
6x the current backend renders one physical pixel, which center-sampled 1x
readback can miss. The fixture now uses a small sprite on that separate target,
keeping the entire-VRAM comparison and transfer semantics intact. Point scaling
is a separate open renderer limitation.

## Measured cost and remaining work

An isolated alternating baseline/candidate test rebuilt the baseline from
`2b60019` and used the same 6x five-frame race dump. Warm baseline replay times
were 211.2–229.5 ms; preservation took 325.0–330.3 ms. Both made 135,695 draw
calls. Preservation added 58 CPU readbacks and increased submits from 7 to 65.
See ignored `work/cpu-write-retail/{baseline,candidate,baseline-again,candidate-again}.log`
and `timings.json`. This is a correctness fix with a substantial performance
cost, not a speed optimization. It does not meet full-speed 4K multiplayer.

An experimental exact-page coalescing through the existing rectangle helper
still needed 58 readbacks and took 328.5 ms; it was removed. Future optimization
must retain actual-byte ordering and untouched storage, potentially by buffering
writes until a proven complete transfer or preserving CPU/GPU subpage ownership.
Do not drop synchronization based on conservative drawn masks or presumed future
upload bytes. Public fixtures, font output and retail replay must stay correct.

The prior integrated 6x MP run reached vblank 6600 and exited with status 0;
late samples ranged 12.2–29.1 drawn FPS. The preservation build also reached 6600 and exited0, but late samples were
8.1–14.3 drawn FPS, final10.2 drawn /33.4 gameframes /60.2 vblanks persecond.
The main Mac build is restored to the preceding renderer; this candidate remains
available for correctness/performance development without making play slower.

## Published experimental GPU transfer continuation

Branch `macos-gpu-transfers` keeps complete physical 256-byte blocks on the GPU,
using existing GS swizzle mappings and upload rectangles. A paired write-complete
notification ensures uploads happen after CPU data is actually written. Partial,
masked, unsupported or ambiguous ownership retains conservative preservation.
CPU-known-block validity avoids unnecessary readbacks until GPU writes invalidate
that proof. The latest texture lookup checks whether all requested mip storage is
already CPU-current before acquiring a GPU view.

Latest checkpoint: Release build and all 26 CTests pass, including 22 procedural
full-4-MiB software/Vulkan comparisons at 1x and 6x plus the independent 1,904-case
block mapping test. This is experimental source, not merged to `macos`. Earlier
GPU patches moved stalls between CPU writes and texture reads without a material
race speedup. The latest early texture-validity selection has not been measured
on the retail race replay. Do not claim native full-game correctness or faster
racing from these fixtures alone. EOD_GPU_UPLOADS=0 selects the same-binary host
preservation baseline. See the main branch's docs/DEVELOPER_HANDOFF.md for current
owner priorities, build instructions and branch map. No disc/capture data shipped.


## Consolidation into main

All source above is now included in `main`. The default uses the conservative
host preservation path. Complete-block GPU uploads require `EOD_GPU_UPLOADS=1`;
`0` or unset keeps them off. The unified CTest suite explicitly tests both modes
at 1x and 6x. Historical branch names above identify experiments only; developers
do not need to merge or check out those branches. Alpha-range proofs are now integrated; see `docs/GS_ALPHA_ORDER.md` for measured
all-pass/all-fail draw elimination and unchanged preservation read counts.
