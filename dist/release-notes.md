Version @VERSION@ of the native PC port of **Test Drive: Eve of Destruction** (PS2 static recompilation), built from commit @COMMIT@.

The game is **not** included: you need your own copy of the NTSC-U disc (`SLUS_209.10`, v1.00). Copy the whole disc into a folder, put `eod.exe` and `eodsetup.exe` there and run `eodsetup.exe` once. See `README.txt` inside the archive.

## Downloads

* `eod-win64.zip`: Windows 7 or later, 64-bit (recommended)
* `eod-win32.zip`: Windows 7 or later, 32-bit, any SSE2 CPU
* `eod-linux-x86_64.tar.gz`: headless Linux test build (no window or sound)
* `SHA256SUMS`: checksums of the archives

## Highlights

* The whole game: Career and Action modes, the garage, races and movies, with sound effects, music, speech and movie audio
* Direct3D 11 renderer up to 6x the PS2 resolution, frame skipping on slow PCs, and a software renderer
* The game's widescreen mode, kept from the start if you choose 16:9; movies stay 4:3
* In-game settings menu: F1, or L3 + R3 on a controller
* Keyboard and XInput controllers
* `eodsetup.exe`: PCSX2 save import, save editor (money, reputation, settings) and garage (any vehicle in the four career slots, including Action mode paint jobs and special vehicles)
* Optional "Car durability as rated in the menu": the per-car durability values from the game's car files, which the original game never applies (table in `README.txt`)

## Changes in this version

All from catsandsoup:

* Direct3D 11 above RenderScale 1: sprites at the edges of the picture now fill their whole last pixel, as the PS2 does. This removes the dark right-hand column and the thin dark line in two-player races (#5)
* Movies: the movie player's bit reader works directly on memory, with identical output and less CPU time per movie (#6)
* Builds: the headless test build runs on arm64 (Apple Silicon) (#8); the VU code builds without the compiled VU programs (#7); groundwork for a Vulkan renderer on other platforms (#9, #10). None of these change the Windows builds

Fixed in 1.0.1: the controller setup completes (vibration setup); clean exit on the game's Exit call.
Fixed in 1.0: race music playing over the menu music after leaving a race; stale movie audio when decoding falls behind; widescreen returning to stretched 4:3 after the first movie.

Known issues and bug reports: https://github.com/pareidolico/eod-recomp/issues
