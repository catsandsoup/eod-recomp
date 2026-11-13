# Career warp (developer)

Start a fresh Career straight from boot and put the car at a free-roam place, or run a Career Eve, without
driving through the menus. For tests and for reproducing Career problems quickly.

```
EOD_DIRECT_START="mode=career;place=dealer"
EOD_DIRECT_START="mode=career;place=malt"
EOD_DIRECT_START="mode=career;eve=3"
```

Use a copy of a real save so runs skip the new-Career tutorial: `EOD_MEMCARD=<folder>` holding a copy of
`~/Library/Application Support/TDEODRecomp/memcard/port0` (never the original). With a saved Career the game goes
straight from CAREER to the Career hub (no tutorial notes), and resumes where it was saved.

Combine with `EOD_TIME_SCALE=8` (docs/MODERN_HARDWARE.md) for headless runs at several times real time.

## Places

The game's own Jump To list (`teleport.lyt`), in its order; the index is the Home area the teleport takes. Checked
from a copy of a rank-1 save at 8x (13 Nov 2026): each place opens the screen listed.

| index | `place=` (aliases) | arrives at |
|---|---|---|
| - | `here` | where the save left the car (no teleport) |
| 0 | `home` (`hub`, `trailer`) | the Career hub (Drive Around / Save / New Career / Exit) |
| 1 | `malt` (`tj's`, `maltshop`) | TJ's Diner: the car stops in the area, no screen opens (not yet understood) |
| 2 | `next_eve` (`eve`, `choose eve`) | the Choose Eve list |
| 3 | `junkyard` (`dealer`, `scrapyard`) | Sweeny's Buy / Sell / Drive screen |
| 4 | `paintshop` (`garage`, `upgrades`, `paint`) | the Garage: Upgrades / Paint / Drive |
| 5 | `gate1` (`time challenge 1`) | Time Challenge: Race the Course / Drive Around |
| 6 | `gate2` (`time challenge 2`) | Time Challenge (the second course) |
| 7 | `petes` (`troy's place`, `troy`) | Troy's Place: Choose a challenger |

`eve=<n>` runs that Career Eve as soon as free roam starts (CareerDoEve `0x2DDC08`, src/hle_dev_career.cpp; the
same as `EOD_CAREER_EVE`). With `EOD_CAREER_LAST=1` the Eve starts its last listed event.

## How it works (all native code; only screens are answered)

1. Title screen: skipped as for any direct start (`0x2BEC08`).
2. Main menu (`MainMenuUpdate 0x2A1298`): answered with its CAREER result, 15. The dispatcher `0x2A2720`
   (result table `0x414460`: 15 Career, 16 Arcade) runs the Career driver `0x112298` as if the player chose it.
3. A fresh Career's screens, by MenuRun caller: loading screens `0x2C1A0C` run natively; the tutorial note
   `0x3861B4` and the "select Drive Around" note `0x383C94` close with -2; the Career hub `0x36AB8C` answers
   Drive Around (4). Later visits to the hub are the player's (only the first is answered).
4. Free roam: the Home Driver (vtable `0x3F8398`, update `0x16FE48`, object `0x3CDA04`) runs each frame. 30
   updates after it starts, `src/career_warp.cpp` calls the Jump To teleport `0x16F488(area)`, which is what the
   pause menu's Jump To (action 9 in the free-roam pause table `0x3C3E98`, `0x386C90` -> `0x386DE8`) calls.
   `0x16F488` -> `0x170988(home, area)` moves the player's car (`0x179920(0)`) to area record
   `home + 0x584 + area * 0xE4` (`0x171110`). The area word `0x3CDA00` (9 = none) is logged afterwards.

`EOD_MENU_RUN_TRACE=1` logs every native menu screen's caller and answer (how the callers above were found).

## Timing

The teleport runs 30 Home Driver updates after free roam starts (`EOD_CAREER_WARP_DELAY=<updates>` changes it),
paused around the call as the pause menu does (`0x176710`/`0x2393B8`, then `0x1766F8`/`0x2393A8`). Entering the
area stops the car (`home + 3064 = 0`) and the Career drive loop opens the place's screen. With a new Career the
intro fly-by is still running at that point and the screen does not open: use a copy of a real save.

## Not covered

- Choosing between several saves on the card (the game loads the profile it finds).
- The Highway (`hiway`), Bug's Place and the TJ's challenger prompt.
