# Vulkan synchronization investigation

## Stable launch VSync

The visible swapchain now chooses FIFO from launch `VSync=1`, or immediate /
mailbox when available from launch `VSync=0`. Internal GS catch-up requests do
not replace the swapchain. Changing this setting takes effect next launch.
Window-size changes and invalid/out-of-date swapchains still recreate it.
The game's clock, frame-skipping decisions, GS pixel resolution and shading
are unchanged.

Reason: a three-second, 10 ms interval CPU sample of the interactive M2 Pro race
found 44 of 224 GS-thread samples in `acquire -> resize -> submit -> fence wait`.
The old code recreated the entire swapchain whenever the internal late/on-time
`present(false/true)` flag changed. These queue drains are avoidable. This is a
source-grounded fix; no visible-window benchmark has been run on the fix yet.
It cannot explain the earlier **headless** multiplayer slowdown, which has no
swapchain. That remains a separate performance investigation.

The same sample found 75/224 GS-thread samples waiting for CLUT readback fences,
and 26/224 in ordinary frame-recycle fence waits. Adjacent 60-vblank log intervals
with 52–64 CPU readbacks accumulated 548–582 ms of host fence waits; a no-readback
interval accumulated 58 ms. Pipeline creation was zero (17 cached pipelines).
These are samples and accumulated host waits, not GPU timestamp measurements.
The sampled live scene and earlier multiplayer benchmark are different workloads.

## Optional readback evidence

Set `EOD_GS_READBACK_TRACE=1` before launch to emit at most 4096 diagnostic lines
on stderr. Larger numeric values select a line limit, capped at one million.
It is disabled by default. Example standalone replay (no visible window/audio):

```sh
EOD_HEADLESS=1 EOD_MUTE=1 EOD_GS_READBACK_TRACE=4096 \
  build-mac/eod --headless --renderer vulkan --gs-replay /path/local-fixture.gsd --repeat 1 \
  2> /tmp/eod-readbacks.log
```

Each `cpu-read` event reports native frame number, readback reason, requested
base pointer/PSM/rectangle, owner base pointer/PSM, GS page, requested 256-byte
block mask, conservative drawn-block footprint, and their intersection. CLUT
loads typically request a 16×16 or 8×2 rectangle. Reason 0 also includes other
CPU reads; the logger does not pretend every such request is a CLUT.

The drawn footprint records bounding rectangles passed to surface acquisition,
not exact triangle coverage, write-channel masks, or GS pixel ownership. It is
cleared when that page is read back or its owner is dropped. It is observational,
not an authoritative cache-validity map. Nonzero overlap does not establish that
the palette pixels changed. Zero overlap suggests a possible page-granularity
false dependency, but still requires controlled fixture verification.

`cpu-write-drops-owner` reports an existing risky behavior: a partial CPU write
can drop ownership of an entire GPU page. This behavior is retained for now;
the trace is intended to expose its relevance to missing geometry/lines rather
than assume those writes are harmless.

These diagnostics never skip, delay, or otherwise change required readbacks,
CLUT contents, ownership decisions or rendering quality. They add CPU work only
when explicitly enabled. A timestamp-query/submit-duration investigation may be
needed if real GPU execution remains the bottleneck after dependency fixes.

## Exact requested-page correction

The measured 6x multiplayer trace contained 97 CPU readback events with zero
requested-block masks. They all came from CT32 palette requests at BP `2FC4`,
while the extra page was owned by the Z16S surface at BP `2BE0`. The existing
`page_range` API unconditionally adds a page for non-page-aligned BP; the 16x16
(or8x2) palette occupies only one actual physical page here. This was a proven
request-address false dependency, independent of conservative draw footprints.

`readback_pages` now enumerates physical GS blocks touched by the requested
pixel rectangle and synchronizes only those pages. It still reads every required
owned page, including palette data that genuinely overlaps GPU-written depth.
`GSPageCoverage` compares this enumeration with brute per-pixel addresses for
520 rectangles across13 GS formats, including unaligned coordinates/BP and VRAM
wrap. The old general `page_range` API stays unchanged for its other callers.
No output-quality reduction or dirty-mask-based readback skipping is involved.
This removes unnecessary submissions by construction; its whole-race speedup
still requires a matched run after integration.

Separately, a generated one-pixel CPU upload on a GPU-owned page reproduced
lost untouched pixels: software retained(37,83,126), baseline Vulkan returned the
stale background(32,64,96). That ownership-preservation bug is **not fixed by
exact requested-page enumeration**. Preserving all GPU pages before CPU writes
could be expensive; a declared full-page overwrite is not enough proof when a
transfer can be interrupted. It needs a separate correctness/coverage design.
