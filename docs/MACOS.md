# macOS (Apple Silicon) port — status

Branch `macos` of catsandsoup/eod-recomp. Native arm64 build of the same
recompiled game code; no emulator, no Rosetta.

## Building

```sh
brew install cmake ninja sdl3 libusb
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

1. **Rendering speed.** The software GS draws ~27–34 fps in a 2P race
   (`--gs-replay` of a captured race frame: ~29 ms/frame with 4 raster
   workers; more workers do not help, the workers mostly wait on each other
   between primitive batches). The game itself runs at 60. Fix: a Vulkan GS
   renderer running on macOS through MoltenVK (the route other PS2 recomps
   take first, for compatibility), ported from `runtime/gs/d3d11/` with the
   same design (GPU surfaces owning GS pages, one pipeline shader). The
   existing HLSL shader compiles to SPIR-V, so D3D11 and Vulkan share one
   shader source. It also gives Retina internal resolution. A native Metal
   backend can come later if MoltenVK costs too much.
2. **Xbox 360 wired pads (Rock Candy).** macOS has no 360 driver; SDL reaches
   them through HIDAPI over libusb, but Homebrew's SDL3 is built without
   libusb. Needs SDL3 built from source with `SDL_HIDAPI_LIBUSB=ON` (bundled
   in the app), then a test with the physical pad.
3. Player-facing packaging: `.app` bundle / DMG, first-run "choose your disc"
   flow, settings UI (the Windows `eodsetup.exe` equivalent).
4. Movie decode is slow during the intro (EE 100% busy, few movie frames/s)
   — check the IPU path on arm64.

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
