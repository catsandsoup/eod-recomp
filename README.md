# eod-recomp — Test Drive: Eve of Destruction, native PC port

> **Work in progress.** The game boots and is playable from start to finish of
> a race, but there are known bugs, performance is still being worked on and
> several PC features are missing. See the [TODO](#todo) list below.

This project turns the PlayStation 2 executable of **Test Drive: Eve of
Destruction** (NTSC-U, `SLUS_209.10`, v1.00) into C/C++ source code that builds
into a native Windows program (Windows 7 or later, 32- or 64-bit) by **static
recompilation**, the approach used by projects like N64Recomp/Zelda64Recomp.
It is not an emulator: the game's code is translated to C ahead of time and
runs natively, on top of a runtime that replaces the PS2 hardware.

**No game files are included.** You need your own copy of the game disc.

![Title screen rendered by the port](docs/img/title_screen.png)

## Status

| Area | State |
| --- | --- |
| Recompiled code | Complete; boots through the SDK libraries and the game's init |
| Graphics | Direct3D 11, native Mac Vulkan/MoltenVK (up to 6x internal resolution), and software GS |
| Controller | Keyboard, Windows XInput and Mac SDL pads (DualShock 2 pressure); physical Rock Candy validation pending |
| Memory card | Saves to `memcard/port0` next to the game data |
| Sound | Sound effects, music and speech work; FMV audio is somewhat distorted |
| FMV | Intro, attract and menu background movies play |
| Gameplay | Menus, Career mode, the garage and races play |
| Performance | Playable on a low-end laptop (Core i3-6006U, Intel HD 520); heavy race scenes are still CPU-bound |
| Widescreen | Native Mac camera selection for 16:9/16:10 is under validation; legacy stretch remains available |

![Boot sequence](docs/img/boot_sequence.png)

## TODO

In order of priority:

1. **VU1 performance.** The game's VU1 micro programs (geometry/lighting) are
   compiled ahead of time to C++ but still take most of the CPU time in
   races. Make them cheaper (only compute pipeline flags where they are read,
   keep registers in locals, fewer scoreboard updates) so races run at full
   frame rate on low-end CPUs.
2. **FMV audio.** The audio of the movies plays somewhat distorted.
3. **More intuitive setup controls.** Make `eodsetup.exe` (keyboard and
   controller bindings, options) easier to understand and use.
4. **PC features, further down the line:**
   * real widescreen support (a wider field of view with correct proportions
     instead of stretching the 4:3 image), possibly based on existing PS2
     widescreen patches or SilentPatch-style fixes; FMVs keep playing in 4:3
   * mouse and keyboard support
   * mouselook

![Main menu](docs/img/main_menu_seq.png)

## macOS (Apple Silicon)

Developer review/build instructions and experimental branch map: [contribution handoff](docs/DEVELOPER_HANDOFF.md).

This fork adds a native arm64 Mac app with Vulkan/MoltenVK graphics, SDL3
audio and two independent player ports for split screen. It is still a work
in progress; [the player journey](docs/PLAYER_JOURNEY.md) distinguishes tested
outcomes from remaining gaps.

Open the locally built **Eve of Destruction.app** and select your own North
American v1.00 PS2 ISO or disc folder when prompted. The app installs a local
copy with progress and remembers it for later launches. No game files are
included. The app bundles its runtime libraries; players do not need Homebrew
or Xcode.

The default **Racing** keyboard preset uses **WASD or arrows** to drive,
**Space** to brake, and **Esc** to pause. In menus, WASD/arrows navigate,
**Enter** accepts, and **Esc** goes back. Enter starts the title screen.
**Original** keyboard layout preserves the per-PS2-button bindings. Mouse
interaction is available in native dialogs; mouse steering/camera is unfinished.

During play, **F1** or **Cmd+,** opens native settings for display size,
fullscreen, aspect, smoothing, resolution scale, VSync, sound/volume,
keyboard bindings/player port, vibration and save/disc locations. Settings
are stored in `~/Library/Application Support/TDEODRecomp/eod.ini`; saves and
the installed disc are beside it. The settings dialog identifies changes
that require a relaunch. The current settings use standard Mac controls;
game-themed presentation remains unfinished.

With the default keyboard on player 1, the first controller takes player 2.
For two controllers in plug order, set **Keyboard port** to **0** and restart.

### Joining player two on Mac

The original game's **“Press X for player 2”** means **PS2 Cross (✕)**, its
Accept button. It does **not** mean the letter X on a Mac keyboard. The game
still shows PS2 button artwork. Press **F2** during play for **Controls and
player 2**: it lists the currently assigned devices and each player's menu
Accept button, including custom Original keyboard bindings. Reopen it after
connecting or unplugging a pad to refresh the assignments. Menu window titles
and Settings also point to F2; automatic replacement of PS2 artwork remains
unfinished.

| Your setup | Settings before launch | What player two presses |
| --- | --- | --- |
| Keyboard for P1 + one controller for P2 | Keyboard port **1** (default); connect the controller | The controller's **bottom face button**: Xbox **A** or PlayStation **✕** |
| Controller for P1 + keyboard for P2 | Keyboard port **2**, then restart | **Enter** with the Racing keyboard layout |
| Two controllers | Keyboard port **0** (off), then restart; connect pads in player order | Bottom face button on the **second** controller |
| One keyboard, no controller | Keyboard port **1** | Use **SINGLE**; a single keyboard currently controls one player and cannot join both ports |

For a local race, navigate **ACTION → RACE → MULTI** with P1, then join and
confirm P2 using P2's assigned device. Each player uses their own Accept button
for their car selection. Continue through the original event/track choices.
For a solo custom race, choose **SINGLE** instead. In menus, keyboard
WASD/arrows navigate, **Enter** accepts and **Esc** goes back. During driving,
Enter is not an extra accelerator binding; **W/Up** accelerates,
**S/Down/Space** brakes and **A/D or Left/Right** steers.

If the game asks you to insert a controller, check the keyboard player setting
and controller assignment first. Choosing MULTI without a device assigned to P2
does not create a second keyboard player. Scripted/virtual two-player checks
pass; physical controller joining still needs hands-on confirmation.

**Controller layout** offers Original (the default PS2 mapping) and Modern
(analog RT gas / LT brake through Cross/Square pressure); restart after changing
it. Modern is intended for the game's classic control schemes. Changing the
game's own bindings can change which actions those inputs perform.
Scripted two-player racing and virtual pads are verified; physical Rock Candy
USB detection, trigger feel and rumble still need a connected device.

Current rendering work: menu font interiors are corrected, and matched race
captures support added horizontal scenery with preserved proportions. Full-speed
4K multiplayer, the reported thin black lines and visible keyboard latency are
still under investigation. An experimental CPU-upload preservation candidate on
`macos-cpu-preservation` passes full-VRAM comparisons at 1x/6x, but its
readback cost worsens performance; it is not enabled on `macos`. See [current evidence and limits](docs/STATUS.md).

Developer build steps, measured performance and known fidelity limits:
[docs/MACOS.md](docs/MACOS.md). The immediate priority is an accurate, stable, full-speed native 1x baseline,
then sequential 2x/4x/6x verification. After that, requested visual work includes
much longer high-detail draw distances and correct widescreen/ultrawide culling, with
quality settings for stronger Macs and high-end PCs at 8K. Maximum/Extreme tiers
must not be limited to the owner's Mac; keep a playable Mac preset and a faithful
baseline. CPU submission and asset streaming need separate work from GPU pixel
cost. DLSS is deferred until the very last stage. This is planned work, not a claim of zero
pop-in today. See [LOD/culling scope](docs/MODDING_SCOPE.md). High-refresh and
other mods remain deferred.

The [LOD/culling investigation](docs/LOD_CULLING_INVESTIGATION.md) identifies
per-car load-time thresholds and the native CPU visibility test. These are
implementation targets. An opt-in car LOD threshold experiment now validates
20 real loaded tables in both original and 2x runs; matched visual acceptance is
still pending. Track draw distance and quality tiers are not yet enabled. Native
widescreen HUD edge anchors are implemented and CPU-tested; rendered checking
is pending. See [LOD experiment](docs/LOD_RUNTIME.md) and
[HUD anchor work](docs/HUD_EDGE_ANCHORS.md).

## Game files

To run the port you need your own copy of the disc. Copy the whole disc (at
least `SLUS_209.10`, `SYSTEM.CNF` and the `DATA` folder) into one folder and
put `eod.exe` there (or pass `--data <folder>`). The executable checks that
`SLUS_209.10` is the exact version this code was generated from (NTSC-U v1.00).

To **build** the port, `tools/analyze.py` and `tools/recomp.py` also need a
copy of `SLUS_209.10` in the repository root: the C translation in `gen/` is
generated from it and is not part of the repository.

## Building

Requirements: your own `SLUS_209.10` in the repository root (see above),
CMake 3.16+, a C/C++17 compiler and Python 3 with `rabbitizer`
(`pip install rabbitizer`) to generate `gen/`.

```sh
# 1. generate the C translation (also done automatically on first configure)
python3 tools/analyze.py
python3 tools/recomp.py
python3 tools/vurecomp.py     # VU micro programs (optional)

# 2a. Windows (cross compile from Linux/WSL with MinGW-w64), 32-bit, runs on Windows 7+
cmake -S . -B build-win32 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake -DCMAKE_BUILD_TYPE=Release
ninja -C build-win32          # -> build-win32/eod.exe (static, no DLLs needed)

# 2a'. 64-bit Windows (faster on x64 systems)
cmake -S . -B build-win64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
ninja -C build-win64

# 2b. Windows with MSYS2/MinGW on Windows itself
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release && cmake --build build

# 2c. Linux (headless test build)
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && ninja -C build-linux
```

The 32-bit Windows build targets SSE2 (Pentium 4 / Core 2 and newer) and
imports only `kernel32`, `user32`, `gdi32`, `winmm` and `msvcrt`.

## Running

Work-in-progress builds for Windows (64- and 32-bit) and Linux are on the
[Releases](https://github.com/pareidolico/eod-recomp/releases) page. They are
built by the `Release builds` workflow (`.github/workflows/release.yml`) from
the Actions tab or when a `v*` tag is pushed. The game is not included in
them either.

`eodsetup.exe` sets the options (renderer, adapter, window size or fullscreen,
aspect ratio, internal resolution, vsync, sound, keyboard bindings, folders)
and writes them to `eod.ini` next to `eod.exe`.

The window title shows the speed ("100% speed" = real time) and the drawn
frames per second.

| Option | Meaning |
| --- | --- |
| `--data DIR` | folder containing `SLUS_209.10` and `DATA/` |
| `--renderer d3d11\|software\|warp` | GS renderer (`warp`: Direct3D 11 on Microsoft's software device) |
| `--scale N` | initial window size (N × 320×240) |
| `--log cats` / `--log-file F` | debug logging (`core,kernel,sif,file,gs,...` or `all`; `gsxfer` logs every GS transfer and has to be named) |
| `--frames N` | exit after N frames (testing) |
| `--screenshot-every N --screenshot-dir D` | dump frames as PPM |

Windows / Original keyboard layout: arrows = D-pad/left stick, X/Space = ✕, C = ○, Z = □, V = △,
Q/E = L1/R1, A or 1 = L2, D or 3 = R2, Enter = Start, Backspace = Select,
Alt+Enter or F11 = fullscreen. XInput controllers map to the DualShock layout.

### Reporting performance

Run a race with

```bat
set EOD_FPS=1
eod.exe --log gs,core --log-file eod_log.txt 2> eod_fps.txt
```

and attach `eod_fps.txt` and `eod_log.txt` to the report.

## How it works

Hand-decompiling ~717,000 MIPS instructions is not feasible in one go, so the
port uses **static recompilation**, the approach used by projects like
N64Recomp/Zelda64Recomp:

1. **`tools/analyze.py`** finds every function in the executable (call
   targets, vtables and other code pointers, tail-jump targets, orphaned code)
   and every `switch` jump table: about 13,500 functions.
2. **`tools/recomp.py`** translates each MIPS R5900 function into an equivalent
   C function (`gen/rec_*.c`). Every instruction is covered: 64/128-bit
   integer ops, MMI SIMD, FPU, VU0 macro mode, branch delay slots, branch-likely,
   unaligned loads/stores, jump tables and indirect calls.
3. The **runtime** (`runtime/`) replaces the PS2 hardware and operating system
   with high-level emulation:
   * `kernel/` – EE kernel: threads (on host fibers), semaphores, interrupt
     and DMA handlers, alarms, vblank timing, libkernel's kernel patches
   * `core/` – CPU state, memory map, MMI instructions, logging
   * `hw/` – DMA controller, timers, interrupt controller and the IPU
     (MPEG-2 macroblock decoding and colour conversion used by the movies)
   * `vu/` – VU0/VU1 interpreter (with the pipeline/stall timing the game's
     microcode depends on, including the 4-cycle delay of the MAC/status/clip flags) and VIF0/VIF1 (unpack, MPG, MSCAL, DIRECT, PATH3 masking).
     The game's micro programs are also compiled ahead of time to C++ by
     **`tools/vurecomp.py`** (from the images in `config/vu/`); the
     interpreter runs anything that doesn't match them.
   * `gs/` – Graphics Synthesizer: swizzled 4 MB VRAM in all pixel formats,
     GIF packets, transfers, CLUTs and a software rasteriser implementing the
     full GS pixel pipeline, with a decoded-texture cache and, on CPUs with
     four or more threads, rasterisation spread over worker threads.
     `gs/d3d11/` is the Direct3D 11 renderer: GPU surfaces own the pages of GS
     memory they draw, with GPU conversions between colour/depth formats and
     block layouts (the game's shadows reinterpret depth buffers as colour),
     a content-hashed texture cache and one shader for the GS pixel pipeline
     (`tools/build_shaders.py` compiles it ahead of time through Wine)
   * `iop/` – the I/O processor: the SIF RPC protocol, plus emulations of the
     IRX modules the game loads: `fileio` (game data from disk), `padman`
     (DualShock 2), `mcserv` (memory card in a host folder), `loadfile`,
     `iopheap`, multitap, the Logitech wheel driver, the game's `stream.irx`
     disc streamer and `sdrdrv` (sound) on top of an SPU2 emulation
     (ADPCM voices, ADSR, AutoDMA PCM streaming)
   * `platform/` – Win32 (GDI window, XInput/keyboard, waveOut, fibers) and
     POSIX (headless, used for automated testing)
4. **`src/`** holds hand-written C++ that replaces or wraps specific game
   functions (see `config/hle.txt`). Hand-decompiled game code goes here over
   time.

`config/symbols.txt` collects identified function names; `tools/dis.py` is an
annotated disassembler used for reverse engineering.

## Debugging aids

* `EOD_GS_TRACE=<frame>` logs every primitive drawn in that frame
* `EOD_FPS=1` prints vblanks/s (60 = full speed), game frames/s (drawn or
  skipped), drawn frames/s, how busy the EE and GS threads are and how long
  the EE waits for the GS, every 5 s
* `EOD_SKIP_MOVIES=1` skips the movies; `EOD_GS_THREAD=0` renders on the main thread;
  `EOD_GS_RASTER=n` sets the number of raster worker threads (0 = none);
  `EOD_FRAMESKIP=0` disables frame skipping (when rendering falls behind,
  whole game frames are dropped so the game keeps running at full speed)
* `--log gs` prints per-second renderer statistics (pixels, texture decoding,
  raster jobs and barriers, pixels per triangle type; with Direct3D 11 also draw
  calls, uploads, readbacks and texture cache use); `--log gsxfer` logs every
  GS memory transfer (very large: movies alone write thousands per second)
* `EOD_GS_CENSUS=file` tallies the GS features used (state combinations, rendered
  buffers read back as textures); `EOD_GS_REPLAY_TRACE=1` logs the primitives of a
  replay; `EOD_GS_REPLAY_STOP=n EOD_GS_REPLAY_DUMP=file` dumps GS memory after the
  n-th primitive (compare renderers with `--renderer d3d11|software`);
  `EOD_D3D_SHOW=bp` displays another buffer; `--renderer warp` runs the Direct3D 11 renderer on Microsoft's
  software device (WARP) instead of the graphics driver, to tell driver problems from renderer bugs
* `EOD_VU1_THREAD=1` (experimental) runs VIF1/VU1, GIF DMA and the GS command stream on a worker thread;
  `EOD_GS_SKIPALL=1` makes the GS skip all drawing (benchmarks of the EE side)
* `EOD_VU_CHECK=1` runs every compiled VU program a second time in the
  interpreter and reports differences; `EOD_VIF_CHECK=1` does the same for the
  fast VIF unpack paths
* `EOD_VU_DUMP=dir` writes the VU micro programs that run (input for
  `tools/vurecomp.py`, see `config/vu/`); `EOD_VU_INTERP=1` disables the compiled ones
* `EOD_GS_DUMP=file EOD_GS_DUMP_AT=vblank [EOD_GS_DUMP_N=count]` captures the GS state and
  command stream; `eod --gs-replay file [--repeat N]` replays it alone, prints the time and
  writes the final image to `file.ppm` (deterministic renderer benchmarks)
* `EOD_GS_NOTEXCACHE=1`, `EOD_DEBUG_NOMIP=1`, `EOD_DEBUG_PIXEL=x,y` (with
  `EOD_GS_TRACE`: logs every write to that pixel) help bisect rendering bugs
* `EOD_WATCH=addr,...` logs float values at EE addresses when they change;
  `EOD_WATCHW=addr` (Linux) reports the host code that first writes a huge value there
* `EOD_MEMCARD=dir` uses another memory card folder (e.g. an empty one for repeatable tests)
* `EOD_WAV=file` (POSIX build) writes the audio output to a WAV file
* `cmake -DEOD_TRACE_CALLS=ON` records the last EE function calls, printed on crashes
* `EOD_PAD_SCRIPT=frame:buttons,...` scripts controller input (port 0); `tools/padscript.py`
  builds it from button names (see its help for a Career-mode example)
* `python3 tools/dis.py -f <addr>` disassembles a function with string/symbol annotations
