Test Drive: Eve of Destruction - native PC port (PS2 static recompilation)

Work in progress: expect bugs. Report them at
https://github.com/pareidolico/eod-recomp/issues

eod-win64.zip  (recommended on 64-bit Windows)
  eod.exe      - the game, Windows 7 x64 or later. Faster than the 32-bit build.
  eodsetup.exe - settings (graphics, sound, keyboard, folders); writes eod.ini
                 next to eod.exe and can start the game.

eod-win32.zip
  The same for 32-bit Windows 7 or later (also runs on 64-bit Windows),
  any SSE2 CPU (Core 2 Duo or newer).

Graphics: the default renderer is Direct3D 11 (needs a DirectX 10 or newer
graphics card, e.g. Intel HD Graphics, GeForce 8 / Radeon HD 2000 and
later). It renders at 2x the PS2 resolution by default (RenderScale in
eodsetup). If Direct3D 11 isn't available, the game falls back to the
software renderer, which can also be chosen in eodsetup.

The Windows builds only use DLLs that ship with Windows 7 (kernel32, user32,
gdi32, winmm, msvcrt, d3d11, dxgi; the setup program also comctl32, ole32
and shell32). No DirectX redistributable or Visual C++ runtime is needed.

eod-linux-x86_64.tar.gz
  eod - headless Linux test build (no window or sound output; used for
  automated testing with --frames / --screenshot-dir / EOD_PAD_SCRIPT).

The game itself is NOT included. You need your own copy of the NTSC-U disc
(SLUS_209.10, v1.00). Copy the whole disc (at least SLUS_209.10 and the DATA
folder) into one folder, put eod.exe there and run it, or pass
--data <folder>. The executable checks that SLUS_209.10 is the exact version
the code was generated from.

Run eodsetup.exe once to choose the settings (or just start eod.exe: it uses
the defaults when there is no eod.ini).

The window title shows the speed: "100% speed" means the game runs in real
time; "fps" is how many frames per second are drawn. On slow CPUs frames are
skipped (down to about 8 drawn per second) so the game keeps its speed.

Saves go to memcard/port0 in the game folder.

Keyboard: arrows = D-pad/left stick, X/Space = Cross, C = Circle,
Z = Square, V = Triangle, Q/E = L1/R1, A or 1 = L2, D or 3 = R2,
Enter = Start, Backspace = Select, Alt+Enter or F11 = fullscreen.
XInput controllers map to the DualShock layout.

Options: --renderer d3d11|software|warp (warp = Direct3D 11 in software,
slow, for testing), --fullscreen, --windowed, --scale N
(window size N x 320x240), --config FILE, --log cats --log-file F.
See README.md at https://github.com/pareidolico/eod-recomp for all options.

Log for a bug report:  eod.exe --log gs,core --log-file eod_log.txt
(well under 100 KB per minute; the gsxfer category logs every GS memory transfer
and grows by megabytes per second, so leave it out unless asked for it).
