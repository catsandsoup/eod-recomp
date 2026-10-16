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
| Input ports | Keyboard on `EOD_KEYBOARD_PORT` (1, 2 or `none`; default 1); the first controller takes the other port, and the second shares the keyboard port. Set `EOD_KEYBOARD_PORT=none` for two pads in P1/P2 plug order. Existing players keep their port on hot-plug. |
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
2. **Physical Xbox 360 wired pad verification (Rock Candy).** The build now
   includes SDL HIDAPI over libusb; the virtual tests verify input conversion
   and hot-plug, but a physical Rock Candy pad still needs the check below.
3. Player-facing packaging: `.app` bundle / DMG, first-run "choose your disc"
   flow, settings UI (the Windows `eodsetup.exe` equivalent).
4. Movie decode is slow during the intro (EE 100% busy, few movie frames/s)
   — check the IPU path on arm64.

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

Hardware-free checks (headless and muted):

```sh
cmake --build build-mac --target eod eod_input_test eod_pad_rpc_test
EOD_HEADLESS=1 EOD_MUTE=1 ctest --test-dir build-mac --output-on-failure
EOD_HEADLESS=1 EOD_MUTE=1 tools/test_2p_race.sh "$HOME/Library/Application Support/TDEODRecomp/disc"
```

### Keyboard racing preset

The default **Racing (WASD + arrows)** keyboard layout uses WASD or arrow keys
for menus; Enter accepts and Escape goes back. In races, W/Up accelerates,
S/Down or **Space brakes**, A/D or Left/Right steers, and Escape pauses.
Enter starts the title screen and has no driving action. Release held keys
when changing screens. Opposite steering keys cancel to center.
Select **Original (PS2 keys)** to use the per-button key bindings instead;
`[Input] KeyboardLayout=racing|original` persists this choice. Gamepad layout
is a separate setting. Target-state evidence is in [KEYBOARD_CONTEXT.md](KEYBOARD_CONTEXT.md).
