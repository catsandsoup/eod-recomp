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

## Fixes in this version

* Race music no longer plays over the menu music after leaving a race
* Movie audio no longer repeats stale sound when decoding falls behind
* The widescreen picture no longer returns to stretched 4:3 after the first movie

Known issues and bug reports: https://github.com/pareidolico/eod-recomp/issues
