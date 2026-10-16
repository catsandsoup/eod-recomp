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
   between primitive batches). The game itself runs at 60. Fix: a Metal GS
   renderer, ported from `runtime/gs/d3d11/` (same design: GPU surfaces
   owning GS pages, one pipeline shader), which also gives Retina internal
   resolution.
2. **Xbox 360 wired pads (Rock Candy).** macOS has no 360 driver; SDL reaches
   them through HIDAPI over libusb, but Homebrew's SDL3 is built without
   libusb. Needs SDL3 built from source with `SDL_HIDAPI_LIBUSB=ON` (bundled
   in the app), then a test with the physical pad.
3. Player-facing packaging: `.app` bundle / DMG, first-run "choose your disc"
   flow, settings UI (the Windows `eodsetup.exe` equivalent).
4. Movie decode is slow during the intro (EE 100% busy, few movie frames/s)
   — check the IPU path on arm64.
