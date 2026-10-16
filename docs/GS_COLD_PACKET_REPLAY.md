# Bounded clean-start paraLLEl-GS experiment — 2026-10-16

## Player outcome and scoped question

The player still needs accurate native 1x/4:3 rendering at original game speed,
without frame skipping or changes to simulation. This experiment asks whether
Road Trip's existing paraLLEl-GS can consume actual EOD packet traffic before a
live-backend migration is considered. It does not establish race performance,
whole-game parity, latency or player acceptance.

## Capture added to the existing runtime

`EOD_GS_DUMP_COLD_START=N EOD_GS_DUMP=work/boot.gsd` records the initialized
GS state **before the first queued command**, followed by the existing command
stream through vblank N. N must be explicitly supplied in 1..6600. Recording
stops at a complete command boundary if it reaches 256 MiB. It is disabled by
default and does not change the `EODGSD01` ABI or existing renderer behavior.
The ordinary `EOD_GS_DUMP_AT` capture remains available. Automated captures must
be headless and muted; use a bounded `--frames` count as well.

The existing vblank-only capture could not prove empty CLUT/vertex/partial
transfer state. The clean-start probe rejects any State other than `gs_init`'s
zero state with IMR=0xff00 and PRMODECONT=1, plus nonempty paths/active transfer.
It uploads initial VRAM and processes all four existing command kinds using
`GSInterface::gif_transfer`, mutable `PrivRegisterState`, `get_gif_path`, and
`vsync`. No guessed arbitrary retail snapshot import is used. Hacks and
supersampling are disabled; the default GSOptions uses X1.

The scratch probe remains outside the product: its build currently refers to
local oracle/reference checkout paths. No third-party renderer code, binary,
retail packet data or image is committed. Publishing a portable standalone tool
and reviewing its dependency licensing remain separate work.

## Observed CRT bootstrap

A real bounded `--log gs` run observes `SetGsCrt(interlace=1, mode=2, ffmd=0)`.
`runtime/kernel/kernel.cpp` forwards the BIOS syscall parameters unchanged.
`gs_set_crt` records SMODE2 but currently ignores mode, so no SMODE1 reaches the
packet dump. Without a bootstrap, paraLLEl-GS reports unknown video format.

Official [PS2SDK graph_mode.c](https://github.com/ps2dev/ps2sdk/blob/master/ee/graph/src/graph_mode.c)
maps BIOS mode 0x02 to NTSC and its SMODE1 table uses CMOD=2, LC=32.
[GS_SET_SMODE1](https://github.com/ps2dev/ps2sdk/blob/master/common/include/gs_privileged.h)
defines their bit positions. The probe's explicit
`--observed-ntsc-mode2` option reuses exactly the two fields supplied by
[Road Trip's presenter boundary](https://github.com/silentsudin/PS2Recomp/blob/818f279be3f9cdecaa9c7b4a3a7cbe8fe44fd6e4/ps2xRuntime/src/lib/gs/gs_pgs_backend.cpp#L492).
This is a documented mode bootstrap for this observed NTSC run, not a claim to
restore every BIOS CRTC register or support other display modes. The production
runtime's CRT semantics were not changed.

## Actual evidence

Source baseline: `b15d0d9` plus the default-off capture hook. Dependency pins:
Road Trip `0f6d3613e8e32ccda5305227a578b1fb659983a8`, PS2Recomp
`818f279be3f9cdecaa9c7b4a3a7cbe8fe44fd6e4`, paraLLEl-GS
`60e6cacead2dece90c4d85ebb5ae750d4aace143`, Granite
`16e7395f6a4858c1783dbf6f521f90b9d5f82ac5`. All GPU work was exclusive,
headless and muted on the Apple M2 Pro through the existing MoltenVK oracle.

- A generated pristine-state fixture containing two GIF packets on different
  paths, a path reset, privileged writes and a vblank succeeds. Its complete
  4-MiB VRAM is byte-identical to EOD Vulkan.
- Actual boot through 30 vblanks/79 GIF calls succeeds, with byte-identical
  4-MiB VRAM against EOD Vulkan. The image is still black at that point.
- A movie prefix through 464 vblanks/464,504 GIF calls succeeds and produces
  an inspected Monster Games logo framebuffer. Its complete VRAM matches EOD
  Vulkan. This upload-heavy workload says little about race rendering.
- Numeric scripted Start taps at 40, 100, 200 and 400 skip the opening movie.
  The bounded trace reaches the title/main-menu transition at vblank 618.
  After trimming the 1,616-byte tail after its last complete vblank, all three
  renderers consume the identical 268,203,140-byte file. It contains 49,416 GIF
  calls and six path resets. paraLLEl-GS produces an inspected 640x448 CRTC
  title image with the explicit observed-NTSC bootstrap.

| Identical completed prefix | Time | Comparison limit |
| --- | ---: | --- |
| paraLLEl-GS, second replay | 951.6 ms | Includes vsync/CRTC and final GPU drain; excludes file parsing/final CPU output |
| EOD Vulkan, second replay | 781.4 ms | Existing replay/display path, including requested final VRAM output |
| EOD software, one replay | 3338.9 ms | Existing software path with eight raster workers |

These timings are diagnostic, not a matched race benchmark or a speedup claim.
PGS's completed CRTC image versus EOD's final displayed image has RGB mean
absolute differences of (6.66,6.45,6.08)/255 against software and
(6.89,6.70,6.35)/255 against Vulkan. Complete VRAM differs in 989,808 and
1,275,417 bytes respectively. The raw circuit framebuffer also differs. The
visible title/menu transition and double-buffer/display ownership need tracing;
known blend precision differences alone do not explain or validate these
results. Neither existing EOD backend is a hardware oracle. **Parity is not
established.** Earlier smaller raw-framebuffer differences from the untrimmed
trace must not be substituted for these completed-prefix diagnostics.

Local evidence is under ignored `work/pgs-cold-*`: capture/replay logs, complete
prefix, VRAM, raw framebuffer and CRTC PPMs. Retail artifacts stay local.
The normal Release runtime rebuild and all 41 CTests pass with the hook present.
The installed app remains the validated b15d0d9 production renderer: this later
change adds an opt-in diagnostic capture only, not a player-facing renderer fix.

## Concrete next success

The existing wheel is technically viable for real EOD command streams, but is
not yet a proven full-speed solution. The relevant mechanism remains eliminating
EOD's CPU local-to-local transfer round trips by letting paraLLEl-GS own GIF
rendering and VRAM. Road Trip's `Submit`, `LoadClut` and `UploadImage` callbacks
are empty; `BeginTransfer` pulls GPU VRAM only for local-to-host. EOD currently
calls `hw::vram_needed` then copies local-to-local pixels on the CPU.

Before a live adapter: provide bounded streaming/compressed clean-start capture
and replay to reach a representative short race window without collecting many
GiB of raw boot traffic; explicitly preserve observed CRT metadata; trace the
completed-frame image/VRAM discrepancies; measure the warmed race window with
GPU completion included and identical native scanout requirements. The 256-MiB
capture ceiling was intentionally retained here. A live adapter additionally
needs authoritative VRAM/CPU-read ownership, SIGNAL/FINISH synchronization,
reset handling, presentation, and frame-skip/timing integration. Arbitrary saved
retail state still has CLUT/queued-vertex/partial-image continuation gaps; Road
Trip itself omits CLUT/open-strip restore based on its own game's rewrite
behavior. No such assumption has been made for EOD.
