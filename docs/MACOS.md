# macOS (Apple Silicon) port — status

Branch `macos` of catsandsoup/eod-recomp. Native arm64 build of the same
recompiled game code; no emulator, no Rosetta.

## Building

```sh
brew install cmake ninja sdl3 libusb molten-vk vulkan-loader vulkan-headers
python3 -m venv .venv && .venv/bin/pip install rabbitizer
cp "<your disc folder>/SLUS_209.10" .          # NTSC-U v1.00, never committed
.venv/bin/python tools/analyze.py && .venv/bin/python tools/recomp.py && .venv/bin/python tools/vurecomp.py
cmake -S . -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE=$PWD/.venv/bin/python
ninja -C build-mac
./build-mac/eod --data "<your disc folder>"     # folder with SLUS_209.10, SYSTEM.CNF, DATA/
```

Headless, muted test run: `EOD_HEADLESS=1 EOD_MUTE=1 ./build-mac/eod --headless --data ... --frames N --screenshot-every 300 --screenshot-dir out`.

## What works (verified 2026-10-16, M2 Pro, headless)

- Boot, intro movies, title screen, main menu, Action > Race > Multi.
- **2-player split screen race** with two independent pads:
  `tools/test_2p_race.sh "<disc folder>"` drives both ports with scripted
  pads and writes a contact sheet where each viewport reacts only to its own
  player's throttle (GOALS §0 verification without physical pads).
- Game logic runs at full speed (59.9 game frames/s in a 2P race).

## What changed for macOS

| Area | Change |
|---|---|
| SIMD | `runtime/include/sse_compat.h`: the SSE2 intrinsics the VU/GS code uses, on NEON, with x86 results kept (cvttps out of range → 0x80000000; MXCSR DAZ/FTZ → FPCR.FZ). |
| Floats | `-ffp-contract=off` on arm64 so no FMA fusing: results match the x86 build. |
| Fibers | Direct arm64 register switch instead of ucontext (deprecated on macOS; swapcontext makes a syscall per switch). |
| Platform | `runtime/platform/sdl3/platform_sdl3.cpp`: window (SDL renderer, Metal), audio stream, keyboard + hot-plugged controllers on two player ports, analog triggers → L2/R2 pressure, rumble. |
| Input ports | Keyboard on `EOD_KEYBOARD_PORT` (1, 2 or `none`; default 1); controllers fill the remaining ports in plug-in order. Keyboard + pad and pad + pad both give two players. |
| Test input | `EOD_PAD_SCRIPT` (port 1) and `EOD_PAD_SCRIPT1` (port 2), optional left stick per entry: `frame:buttons:lx:ly`. |
| Threads | EE, GS, raster and offload threads ask for user-interactive QoS (P-cores). `EOD_GS_RASTER=n` is no longer capped at 4 when given explicitly. |

## Known gaps (in order)

1. **Display validation.** The Vulkan backend now runs through MoltenVK with
   native-resolution internal rendering (see below). Visible window resize,
   fullscreen presentation and even two-refresh cadence at 120 Hz still need
   an interactive check: all automated tests deliberately remain headless.
2. **Xbox 360 wired pads (Rock Candy).** macOS has no 360 driver; SDL reaches
   them through HIDAPI over libusb, but Homebrew's SDL3 is built without
   libusb. Needs SDL3 built from source with `SDL_HIDAPI_LIBUSB=ON` (bundled
   in the app), then a test with the physical pad.
3. Player-facing packaging: `.app` bundle / DMG, first-run "choose your disc"
   flow, settings UI (the Windows `eodsetup.exe` equivalent).
4. Movie decode is slow during the intro (EE 100% busy, few movie frames/s)
   — check the IPU path on arm64.

## Vulkan GS backend

`--renderer vulkan` is the macOS default when Vulkan headers/loader and SDL3
are available at configure time. `--renderer software` remains available;
`-DEOD_VULKAN=OFF` builds the software-only port. If the loader cannot discover
MoltenVK, set `VK_DRIVER_FILES` (or `VK_ICD_FILENAMES`) to its ICD JSON, normally
`/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json` for a Homebrew CLI build.

The backend retains the D3D11 renderer's GS page ownership, texture cache and
shader logic. Vulkan 1.3 dynamic rendering, push descriptors, dual-source
blending and depth clamp are required. Two fence-owned command/upload slots
allow CPU/GPU overlap. Sparse page conversions are coalesced without covering
unowned holes. Readbacks synchronize transfer writes to host reads. Default
presentation is FIFO; disabling vsync uses immediate/mailbox when supported.
`RenderScale=0` selects an initial scale covering the window's pixel height
(1x offscreen); explicit values 1–6 override it. A 1964-pixel panel needs 5x.
`EOD_SCREENSHOT_SCALE=5` captures the 5x composition rather than a 1x screenshot.

Verified 2026-10-16 on M2 Pro, entirely headless and muted:

- Existing `work/race2p.gsd`, 509,709 primitives / **nine** game frames per replay:
  software with four workers 369.8 ms; warm Vulkan 1x 113.0 ms (12.56 ms/frame),
  Vulkan 5x 120.4 ms (13.38 ms/frame). These are renderer replay timings;
  they do not prove visible presentation cadence.
- At 1x, RGB mean absolute difference versus software is 1.641/255;
  95% of channel differences are at most 4. Images are close, not bit exact.
  The 5x raster downsampled to 1x differs more (MAE 6.956/255).
- 379 forced rendered-page readbacks pass. Invalid ICD discovery falls back
  to software headlessly. SPIR-V regeneration validates all ten modules.
- 5x replay plus a 3200×2240 screenshot peaks near 1.00 GB total memory
  footprint including GPU allocations (315 MB resident), with no swaps.
- Scripted two-player racing reaches approximately 59.9 game/drawn frames/s;
  transient asset-loading dips remain. Logs and screenshots are local work
  artifacts, not committed game data.

Repeat full-game comparisons with `EOD_TEST_RENDERER=vulkan` and optionally
`EOD_TEST_CONFIG=work/vulkan5.ini` when invoking `tools/test_2p_race.sh`.
For a replay, save each generated `.gsd.ppm` before running another renderer,
because the next run overwrites that filename. Shader sources can be rebuilt
with `python3 tools/build_vk_shaders.py` (glslang and spirv-tools required);
the normal build uses the checked-in SPIR-V arrays.

### Refresh-grid presentation (optional driver support)

When `VK_GOOGLE_display_timing` is advertised, the Vulkan presenter now learns
its refresh phase from completed presentation feedback and requests future
images near a fixed refresh grid. A 120 Hz display normally uses two refreshes
per guest image. The original 59.94 Hz game clock remains unchanged: on an
exactly 120 Hz fixed-rate display an occasional extra refresh is mathematically
necessary; missed deadlines also re-anchor instead of accumulating latency.
`EOD_PRESENT_TIMING=0` disables this optional path. Missing/invalid timing data
keeps ordinary FIFO presentation. Swapchain/refresh changes reset the schedule,
and no-vsync/catch-up sends no future presentation deadline.

This adds no sleep, device lock, or GPU fence wait. It cannot prevent the Vulkan
swapchain/driver's existing acquire/present calls from blocking. Visible cadence,
ProMotion's actual chosen refresh rate and GPU deadline misses still require an
interactive measurement; headless runs cannot verify this window-only path.
`tools/test_present_timing.sh` checks the pure scheduling logic without a Vulkan
or SDL device, including 120 Hz jitter, 59.94 Hz, rate mismatch, stalls and resets.

The approach was identified in Road Trip's `schedulePresent()`/`waitForDrawable()`;
our implementation is original, using the public extension rather than copied
renderer code. Source references:

- [Vulkan presentation timestamps](https://docs.vulkan.org/refpages/latest/refpages/source/VkPresentTimeGOOGLE.html)
- [MoltenVK presentation implementation](https://github.com/KhronosGroup/MoltenVK/blob/main/MoltenVK/MoltenVK/GPUObjects/MVKImage.mm)
- [Apple's matching host-time epoch](https://developer.apple.com/documentation/quartzcore/cacurrentmediatime())
- [Road Trip's refresh-grid prior art](https://github.com/silentsudin/PS2Recomp/blob/818f279be3f9cdecaa9c7b4a3a7cbe8fe44fd6e4/ps2xRuntime/src/lib/gs/gs_pgs_presenter.cpp)

### Original-scale GS fidelity checks

Procedural 64×64 replays now exercise generated GS register packets without disc
assets or game code. `GSPixelArithmetic` and `VulkanGSPixels` are CTest checks
when Vulkan and testing are enabled. They demonstrate and guard these fixes:

- Gouraud RGBA truncation: an interior pixel formerly `(11,21,32)` is now
  `(10,21,31)`, matching the GS interpolation rule used by paraLLEl-GS.
- Linear UV precision: two interpolated coordinates in one 1/16-texel bucket
  formerly sampled 104 versus 96; both now sample 96.
- Fog interpolation: source and fog colour both 128 with F=255 formerly became
  127 in both software and Vulkan. Rounded interpolation about FogCol now
  preserves 128. The arithmetic test covers every constant colour/coefficient.

The original software-captured two-player fixture improves from RGB MAE
1.641 to 1.499/255 (95th-percentile channel difference 4→3). This is comparison
against our software renderer, not proof of exact PS2 output. In particular,
a generated blended rectangle exposes an interior `(15,29,45)` Vulkan result
where GS integer arithmetic gives `(14,28,44)`: fixed-function UNORM blending
still rounds. Large residual differences also remain around detailed geometry.
No complete 1:1 fidelity claim is justified yet; accurate GS/original-hardware
comparison is still needed. Filter-output rounding is deliberately not changed
merely to reduce software image error.

Hardware captures now flush pending GPU batches and read owned GS pages back
before serializing initial VRAM. Older captures taken from live Vulkan may
contain stale initial VRAM; software-captured fixtures avoid that limitation.
Replay state layout is unchanged and shared with the procedural generator.

Primary implementation evidence: [paraLLEl-GS interpolants, UV snapping and
fog counterexample](https://github.com/silentsudin/parallel-gs/blob/60e6cacead2dece90c4d85ebb5ae750d4aace143/gs/shaders/ubershader.comp).
