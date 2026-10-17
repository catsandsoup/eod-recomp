# macOS (Apple Silicon)

The macOS build uses the SDL3 platform backend (window, keyboard, controllers,
sound) and, optionally, the Vulkan GS renderer on MoltenVK. Both are CMake
options, off by default; the Windows build doesn't use them.

## Building

Requirements: Xcode's command line tools, CMake, Ninja, Python 3 with
`rabbitizer`, and from Homebrew:

```sh
brew install cmake ninja sdl3 vulkan-headers vulkan-loader molten-vk
brew install libusb        # only for -DEOD_BUNDLED_SDL=ON (Xbox 360 pads)
```

Put your `SLUS_209.10` in the repository root (see the README), then:

```sh
cmake -S . -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DEOD_SDL3=ON -DEOD_VULKAN=ON -DEOD_MAC_APP=ON \
      -DEOD_APP_DIR="$HOME/Library/Application Support/TDEODRecomp/local-build"
ninja -C build-mac
```

| option | |
|---|---|
| `EOD_SDL3` | SDL3 window, input and audio (without it: the headless test build) |
| `EOD_VULKAN` | the Vulkan GS renderer (`Renderer=vulkan`, the default in this build); needs `EOD_SDL3` |
| `EOD_MAC_APP` | also creates `Eve of Destruction.app` in `EOD_APP_DIR`: the libraries it needs (SDL3, libusb, the Vulkan loader, MoltenVK) copied in, ad-hoc signed |
| `EOD_APP_DIR` | where `EOD_MAC_APP` puts the app (default: the build folder). Finder metadata in iCloud-synced Desktop/Documents can prevent signing; use a folder outside them |
| `EOD_BUNDLED_SDL` | SDL3 3.4.18 built from source with HIDAPI over libusb, linked statically: wired Xbox 360 pads (and third-party ones), which macOS has no driver for and Homebrew's SDL3 doesn't see |

`tools/macos/make_dmg.sh "$HOME/Library/Application Support/TDEODRecomp/local-build/Eve of Destruction.app"` makes a disk
image of the app. The game's files are never put in the app or the image.

## Running

Open the app, or run `build-mac/eod`. Settings (`eod.ini`) and memory cards
live in `~/Library/Application Support/TDEODRecomp`. On the first run the game
asks for your disc: an `.iso`/`.bin`/`.cue` image or a folder with
`SLUS_209.10`, `SYSTEM.CNF` and `DATA`. It checks that it is the NTSC-U v1.00
release and copies those files to `TDEODRecomp/disc`. `--data DIR` (or
`EOD_DATA`) uses a folder in place instead.

- F1 or Cmd+,: settings. F11 or Option+Return: fullscreen.
- Keyboard: the `[Input]` keys in `eod.ini`, on player 1 (as on Windows).
- Controllers take the player ports in the order they are connected: the first
  one shares player 1 with the keyboard, the second is player 2.
- `--renderer software` (or `Renderer=software`) uses the software renderer.
- `--headless` / `EOD_HEADLESS=1` (no window, no dialogs) and `EOD_MUTE=1` work
  as in the headless build, e.g. for scripted tests with `EOD_PAD_SCRIPT`.

The in-game menu of the Windows build (Dear ImGui, Direct3D 11) isn't available
on macOS yet; the settings dialog takes its place.
