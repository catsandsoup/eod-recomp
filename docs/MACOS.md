# macOS (Apple Silicon) port — status

Branch `macos` of catsandsoup/eod-recomp. Native arm64 build of the same
recompiled game code; no emulator, no Rosetta.

## Building

```sh
brew install cmake ninja libusb molten-vk vulkan-loader vulkan-headers
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
| Input ports | Keyboard on `EOD_KEYBOARD_PORT` (1, 2 or `none`; default 1); the first controller takes the other port, and the second shares the keyboard port. Set `EOD_KEYBOARD_PORT=none` for two pads in P1/P2 plug order. Existing players keep their port on hot-plug. |
| Test input | `EOD_PAD_SCRIPT` (port 1) and `EOD_PAD_SCRIPT1` (port 2), optional left stick per entry: `frame:buttons:lx:ly`. |
| Threads | EE, GS, raster and offload threads ask for user-interactive QoS (P-cores). `EOD_GS_RASTER=n` is no longer capped at 4 when given explicitly. |

## Remaining validation

1. Visible resize/fullscreen and native first-run/settings dialogs need an
   interactive check; automation stays headless. High-refresh work is deferred.
2. Physical Rock Candy controller verification: bundled SDL/libusb and virtual
   tests pass, but no physical pad was connected during takeover. Original
   pressure mappings also need a usable modern throttle/brake layout.
3. Career save/resume and every event type need actual journey verification.
   Packet-level memory-card persistence tests pass; these alone do not establish
   that the game saves and resumes a career correctly.
4. Intro playback still needs fidelity/performance work. A bounded 8-second
   Apple Silicon CPU sample during `logos.pss` found the translated bitstream
   consume/refill helper `0039AFB0` at about 35% of main-thread self samples,
   with peek/consume wrapper `0039B048` at about 13%. Native IPU IDCT was below
   1% of self samples. This points to translated bitstream parsing as the first
   optimization target for that sampled segment; it does not establish the
   bottleneck for every movie or justify changing playback timing.

## Controllers and Rock Candy Xbox 360 wired pads

On macOS `EOD_BUNDLED_SDL=ON` is the default. CMake downloads pinned SDL
3.4.18 with a SHA-256 check, builds it statically with HIDAPI and libusb,
and fails configuration if libusb support is missing. Install `libusb`
with Homebrew first. `-DEOD_BUNDLED_SDL=OFF` uses installed SDL3; its libusb
capability depends on that build (Homebrew SDL3 does not enable it).
The linked Homebrew `libusb-1.0.0.dylib` must be copied into the application
bundle and its install name rewritten by the app packaging step. SDL itself
needs no separate dylib in the bundled configuration.

Plug the pad in and run this diagnostic from the repository (no disc data,
window, or audio needed):

```sh
EOD_HEADLESS=1 EOD_MUTE=1 ./build-mac/eod --list-pads --watch 30
```

Expect `HIDAPI over libusb: built in`, a controller with PDP vendor `0e6f`
(or Microsoft `045e`), `driver HIDAPI`, `opened as gamepad: yes`, and its SDL
mapping. The watch prints button changes, sticks centered at 128, and L2/R2
trigger pressure from 0 to 255; it briefly rumbles each assigned pad.
Unplug/replug during the watch to check hot-plug. With the default keyboard
on P1, one pad is P2. For two pads in plug order, use:

```sh
EOD_HEADLESS=1 EOD_MUTE=1 EOD_KEYBOARD_PORT=none ./build-mac/eod --list-pads --watch 30
```

If no pad appears, check `system_profiler SPUSBDataType` for PDP `0x0e6f`,
try another USB port/cable, and quit applications that may hold the device.
A virtual joystick cannot verify USB access, a physical mapping, or motors.
`EOD_INPUT_LOG=1` also logs changes per player during a game run.

Xbox A/B/X/Y map to Cross/Circle/Square/Triangle; LB/RB to L1/R1;
LT/RT to L2/R2 analog pressure plus the digital bit above 30/255 travel.
Back, Start, stick clicks and d-pad retain their corresponding PS2 controls.
Both sticks use a dead zone and the full 0..255 range. These mappings expose
PS2 L2/R2 pressure; the game's selected control scheme determines the actual
throttle and brake bindings.

Native settings also expose an opt-in Modern controller layout, persisted as
`[Input] ControllerLayout=modern` (default `original`). After relaunch, RT feeds
Cross pressure and LT feeds Square pressure, which the game's classic schemes
read as analog gas/brake. It suppresses the triggers' L2/R2 actions to avoid
double actions. Face buttons/keyboard keep full pressure even when a partial
trigger is held. Virtual tests cover partial/released triggers and two-port
isolation; physical driving feel remains unverified.

Hardware-free checks (headless and muted):

```sh
cmake --build build-mac --target eod eod_input_test eod_pad_rpc_test
EOD_HEADLESS=1 EOD_MUTE=1 ctest --test-dir build-mac --output-on-failure
EOD_HEADLESS=1 EOD_MUTE=1 tools/test_2p_race.sh "$HOME/Library/Application Support/TDEODRecomp/disc"
```
## Personal Mac app

The app requires your own North American SLUS-20910 v1.00 disc. No game
files are bundled. Generate and build as above, then enable packaging:

```sh
cmake -S . -B build-mac -G Ninja -DEOD_MAC_APP=ON
cmake --build build-mac -j3
```

The plain `build-mac/eod` remains available. The `eod_app` target creates
`~/Library/Application Support/TDEODRecomp/local-build/Eve of Destruction.app`
with its non-system dylib dependency closure, local rpaths, generic macOS
placeholder icon, and ad-hoc signature. `EOD_APP_OUTPUT` chooses another output
directory. Use a directory outside iCloud/FileProvider: Finder metadata can
invalidate code signing inside synced Documents folders. The minimum macOS
version in Info.plist is derived from the executable and bundled dependencies
(at least 13.0); local Homebrew builds may require the current OS.

First launch offers a native ISO or disc-folder picker. It verifies the exact
SLUS_209.10 size and CRC before copying SLUS_209.10, SYSTEM.CNF (if present),
and DATA into Application Support/TDEODRecomp/disc. Progress and friendly
errors appear in native windows. An existing valid disc skips the picker.
Imports stage separately, and failed/cancelled copies preserve the old disc.
The built-in ISO9660 reader supports plain ISO and raw Mode 1/2 BIN/CUE.

`eod.ini` and `memcard/` also live in Application Support/TDEODRecomp.
Press **F1** or **Cmd+,** during play for native settings: video size,
fullscreen, aspect, smoothing, frame skip, VSync, volume/audio, every keyboard
binding, keyboard port, vibration, renderer and internal resolution, plus
save/disc locations. Renderer, internal resolution, VSync, audio enable, frame skip, keyboard port
and file locations take effect on next launch. The software renderer has no internal
resolution scaling; Vulkan selection requires a build with that backend.

Automation must set `EOD_HEADLESS=1 EOD_MUTE=1` and pass `--headless`.
`--data DIR`/`EOD_DATA`, `--config FILE` and `EOD_APP_SUPPORT` override paths.
Missing data in headless mode returns an error without opening dialogs.

```sh
ctest --test-dir build-mac --output-on-failure
# Read-only source; import only to a scratch destination:
build-mac/disc_import_cli /path/game.iso /tmp/eod-test/disc 3368996 75BB93EE
EOD_HEADLESS=1 EOD_MUTE=1 "$HOME/Library/Application Support/TDEODRecomp/local-build/Eve of Destruction.app/Contents/MacOS/eod" --headless --data /tmp/eod-test/disc --frames 600
# Build a DMG locally; never upload or distribute:
tools/make_dmg.sh
```

Native dialogs are compile-verified only; automated tests never open visible
windows or play audio. The app and DMG are personal builds only.

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

Vulkan app bundles also include `libMoltenVK.dylib` and
`Contents/Resources/vulkan/icd.d/MoltenVK_icd.json`. MoltenVK is loaded
indirectly, so packaging includes it explicitly rather than relying only on
linked dependencies. Set `EOD_MOLTENVK_LIBRARY` at packaging time for a custom
driver path. Bundled launches set Vulkan driver discovery and SDL's Vulkan
loader path relative to the executable before both replay and normal startup;
copying the app requires neither Homebrew nor an external ICD configuration.
Plain command-line builds preserve the caller's Vulkan environment.

Test a relocated signed Vulkan app with intentionally invalid external ICD
overrides and an otherwise isolated environment:

```sh
tools/test_app_vulkan.sh "/path/Eve of Destruction.app" /path/race2p.gsd /tmp/app-vulkan.log
```

The script stays headless/muted and fails if replay falls back to software.

### Refresh-grid presentation (optional driver support)

Experimental and **off by default** while 1:1 fidelity is the priority. With
`EOD_PRESENT_TIMING=1` and `VK_GOOGLE_display_timing` advertised, the presenter learns
its refresh phase from completed presentation feedback and requests future
images near a fixed refresh grid. A 120 Hz display normally uses two refreshes
per guest image. The original 59.94 Hz game clock remains unchanged: on an
exactly 120 Hz fixed-rate display an occasional extra refresh is mathematically
necessary; missed deadlines also re-anchor instead of accumulating latency.
The default `EOD_PRESENT_TIMING=0` keeps this path deferred. Missing/invalid timing data
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

## Audio fidelity check

User volume is applied once by the shared SPU2 mixer. The SDL output stream
uses unity gain (or zero when disabled), so the Mac path no longer attenuates
50% volume twice. `audio_volume_offline` feeds constant planar stereo PCM
through the actual SPU2 AutoDMA/mixer and an unbound SDL stream, retrieves the
samples into memory, and checks 100%/50%/0% amplitudes of 11998/5999/0. It never
opens an audio device, window, or plays sound.

This fixes an output-level regression, not the whole PS2 sound model. The
current voice resampler is linear; noise/pitch-modulation and reverb fields
are stored but are not implemented in the mixing loop. Audio is driven from
wall-clock time, with a 100 ms catch-up limit and a 200 ms output queue cap.
Those approximations still require original-hardware or trusted-reference
comparison before describing audio or FMV synchronization as 1:1.

## Original-scale GS fidelity checks

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
