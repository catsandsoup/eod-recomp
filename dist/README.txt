Test Drive: Eve of Destruction - native PC port (PS2 static recompilation)
Version 1.0.2

Report bugs at https://github.com/pareidolico/eod-recomp/issues

eod-win64.zip  (recommended on 64-bit Windows)
  eod.exe      - the game, Windows 7 x64 or later. Faster than the 32-bit build.
  eodsetup.exe - settings (graphics, sound, keyboard, folders); writes eod.ini
                 next to eod.exe and can start the game.

eod-win32.zip
  The same for 32-bit Windows 7 or later (also runs on 64-bit Windows),
  any SSE2 CPU (Core 2 Duo or newer).

Widescreen: choose "Widescreen (16:9)" in the game's Options menu (or 16:9 in
eodsetup): the 3D view widens, the HUD keeps its shape and the picture is
shown at 16:9. Movies stay 4:3.

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

In-game menu: press F1 (or L3 + R3 on a controller) while playing. Most
settings change right away (fullscreen, aspect ratio, VSync, sound, volume,
keyboard keys...); "Save settings" keeps them in eod.ini. The game pauses while
the menu is open, and "Show frame rate" adds a frame counter. (Direct3D 11
renderer only.)

The window title shows the speed: "100% speed" means the game runs in real
time; "fps" is how many frames per second are drawn. On slow CPUs frames are
skipped (down to about 8 drawn per second) so the game keeps its speed.

Saves go to memcard/port0 in the game folder. To continue a game saved in
PCSX2, use "Import PCSX2 save..." in eodsetup.exe and pick the PCSX2 memory
card (PCSX2's memcards folder, e.g. Mcd001.ps2); the save already in
memcard/port0, if any, is kept as a .bak folder. "Edit save..." changes the
career money, reputation (unlocks) and the game's settings in the save, and
its Garage puts any car in the four career slots: the dealer's career cars
go into the save (their upgrades move with the slot); Action mode paint jobs
and special vehicles (Bus, Hearse, Taxi...) are shown by this port over the
slot's career car (CarLooks in eod.ini). It drives as that vehicle, with the
career car's upgrades.

Car durability: in the game a car's toughness depends only on its weight
(and the Chassis Reinforcement upgrade); the durability ratings in the car
menus aren't used. "Car durability as rated in the menu" in eodsetup.exe
applies the per-car values from the game's car files that those ratings
follow (the Bus and the Durand get much tougher, the Postal van and the
Taxi more fragile).

Damage each car takes (x1.00 = a 4000 lb car; lower is tougher; the event's
own damage setting and the Chassis Reinforcement upgrade apply on top):

  Car         Weight    Menu    Normal   damage_  With "Car
                        rating  game     scale    durability"
  Bus*        9000 lb   10      x0.44    0.30     x0.13
  Limo        5700 lb   9       x0.70    0.60     x0.42
  Outlaw      5700 lb   8       x0.70    0.70     x0.49
  Cannon      5300 lb   7       x0.75    0.75     x0.57
  Ambulance*  5250 lb   6       x0.76    0.62     x0.47
  Hearse*     5000 lb   9       x0.80    (1)      x0.80
  Durand      4500 lb   10      x0.89    0.50     x0.44
  Empire      4400 lb   4       x0.91    1.00     x0.91
  Taxi*       4400 lb   7       x0.91    2.00     x1.82
  Blitz       4000 lb   6       x1.00    0.75     x0.75
  Coyote      4000 lb   6       x1.00    0.75     x0.75
  Guerilla    4000 lb   6       x1.00    0.75     x0.75
  Arrow       3700 lb   5       x1.08    1.00     x1.08
  Ocelot      3700 lb   4       x1.08    1.00     x1.08
  Gladiator   3650 lb   6       x1.10    0.90     x0.99
  Fillmore    3600 lb   4       x1.11    1.00     x1.11
  Hunter      3600 lb   4       x1.11    1.00     x1.11
  Bulldog     3450 lb   6       x1.16    1.00     x1.16
  Palo        3300 lb   3       x1.21    1.00     x1.21
  Copcar*     3250 lb   5       x1.23    (1)      x1.23
  Desio       3200 lb   2       x1.25    1.00     x1.25
  Mongoose    3000 lb   3       x1.33    0.90     x1.20
  Prince      2900 lb   1       x1.38    1.00     x1.38
  Algar       2800 lb   3       x1.43    1.00     x1.43
  Orbital     2700 lb   5       x1.48    0.80     x1.19
  Top         2675 lb   2       x1.50    1.00     x1.50
  Hurricane   2600 lb   2       x1.54    1.00     x1.54
  Postal*     2500 lb   2       x1.60    2.50     x4.00
  Rocket      2400 lb   4       x1.67    0.80     x1.33
  Venom       2400 lb   3       x1.67    0.90     x1.50
  Moth        2000 lb   1       x2.00    1.00     x2.00
  Digit       1800 lb   4       x2.22    1.00     x2.22

  * Action mode only. (1): no value in the car's file, so it stays as it is.

Keyboard: arrows = D-pad/left stick, X/Space = Cross, C = Circle,
Z = Square, V = Triangle, Q/E = L1/R1, A or 1 = L2, D or 3 = R2,
Enter = Start, Backspace = Select, Alt+Enter or F11 = fullscreen,
F1 = in-game menu.
XInput controllers map to the DualShock layout.

Options: --renderer d3d11|software|warp (warp = Direct3D 11 in software,
slow, for testing), --fullscreen, --windowed, --scale N
(window size N x 320x240), --config FILE, --log cats --log-file F.
See README.md at https://github.com/pareidolico/eod-recomp for all options.

Log for a bug report:  eod.exe --log gs,core --log-file eod_log.txt
(well under 100 KB per minute; the gsxfer category logs every GS memory transfer
and grows by megabytes per second, so leave it out unless asked for it).
