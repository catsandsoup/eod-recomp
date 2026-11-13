# Career unlocks

Everything here is on by default, is an ini key under `[Cheats]` in `eod.ini` and a live row in the front end
(MENU, MODS tab, "CAREER UNLOCKS"; saved like the other toggles). Nothing changes a damage or physics value,
and nothing removes or alters a track, race, event or mode: the Career is only opened up.

| Row | ini key | env override | What it does |
|---|---|---|---|
| CAREER MONEY FLOOR | `CareerMoney` | `EOD_CAREER_MONEY` | Money never drops below $10,000 while the Career is on the free-roam drive |
| CAREER RANK ON TOP | `CareerRank` | `EOD_CAREER_RANK` | Rank held at the top (stored 1, shown 1st) without ending the Career |
| ALL SHOPS AND EVES OPEN | `OpenCareer` | `EOD_OPEN_CAREER` | Upgrades, Time Challenges, the dealer's full stock, Highway, Troy's Place and every Eve open |
| DEALER SELLS EVERY CAR | `AllCars` | `EOD_ALL_CARS` | The dealer pages through every car (L2 / R2 on the dealer screen) |
| FREE CAR SWAPPING | `FreeSwap` | `EOD_FREE_SWAP` | Selling or trading a car refunds its full price |
| KILLABLE CHALLENGERS | `KillableChallengers` | `EOD_KILLABLE_CHALLENGERS` | Highway and Malt Shop challengers in Career free roam can reach 100% damage and be wrecked |

Code: `src/hle_career.cpp` (wrappers and the per-frame hook `career_vblank`), the `[Cheats]` keys in
`runtime/core/config.cpp`, the rows in `runtime/ui/frontend.cpp`, the wrap lines at the end of `config/hle.txt`
(rerun `tools/recomp.py` after changing it). Tests: `eod_career_unlocks_test` (ctest `CareerUnlocks`) and
`tools/tests/test_career_content.py` (ctest `CareerContent`, `EOD_CAREER_CONTENT=<COMMON.RES or content folder>`
checks real content).

Addresses are EE addresses in SLUS_209.10 (the `f_xxxxxxxx` of `gen/` and `tools/ee_dis.py`).

## Career state

| Address | Meaning |
|---|---|
| `*(0x3D9384)` -> `+0` | Career active flag (`f_002E0660`) |
| `*(0x3D9394)` (CDBResSaveInfo, 0x194 B) | `+0` rank, zero-based (start 99 = "100th", **0 = champion**), getter `f_002E0F10`, setter `f_002E0F20`; `+4` count and `+8..` four slots of the open-Eve list (only `f_002E6D08` reads it); `+0x118` per-Eve results |
| `*(0x3D9398)` (CDBupgr, 0xC68 B) | `+4` money (`f_002E1D60` adds, `f_002E1D98` reads), `+0xC` car count (max 4), `+0x68 + 0x300 * i` garage slot i |
| `0x41EFF8` | rank gate table `{79, 49, 0}`: `rank <= table[i]` (`f_002E27F8`); 0 = Upgrades + Time Challenges (80th), 1 = dealer stock, Highway (50th), 2 = champion lock-out |
| `0x3D93B8 / 0x3D93BC` | ASChassisList: 8-byte records (name pointer, price), count (28 with Phantom and Cup) |
| `0x3D93C0 / 0x3D93C4` | ASCarList: 0x14-byte records `+0` index, `+4` vehicle, `+8` price, `+0xC` chassis name (132) |
| `0x4AB130`, `0x3D93D0` | the five dealer slots (ASCarList indices) and their count |
| `0x4AB150 / 0x4AB158` | Eve window records, 12 bytes: `+0` Eve, `+4` min_rank - 1, `+8` max_rank - 1 |

## Money

`career_vblank` (called from `game_patches_vblank`) raises `*(*0x3D9398) + 4` to $10,000 when it is below that,
only while a Career is active and its free-roam world is the running event (`*(0x3CF19C) == 0x8000226`; a race or
Battle has another value, so the payout of an Eve is never skipped). Money above the floor is never lowered and winnings still add. The floor is
$10,000 rather than $9,999 because the dearest car (Rocket) costs $10,000.

## Rank

* Stored 0 is the champion: `f_002E6C78` (new rank after an Eve: `rank - 6 + place`, and 0 when an Eve is won
  at rank 1) ends the Career and empties the Eve list ("Now that you are the best, there are no more eves").
  So the stored rank stays at **1**: `career_vblank` lowers a larger rank to 1, `f_002E6C78` never returns less
  than 1 while CareerRank is on (an already champion save is left alone).
* The screens show stored + 1 ("2nd"). `f_002E0F10` (the getter) is wrapped: when it returns 1 to a call site
  that only displays the rank (return addresses in `kDisplay`: the free-roam HUD `0x170DD4`, the Career screens
  that print it with the `st/nd/th` suffix function `f_002E2A70`, the rank update screen, the Eve list) it
  returns 0, so the screen reads 1st. Game logic, gates and the Eve code see the real 1.
* The Eves are not driven by this rank (below), so every Eve stays playable.

## Gates, shops, Eves, Troy

* Gate table `0x41EFF8` is rewritten to `{99, 99, 0}` (and back to `{79, 49, 0}` with OpenCareer off). The
  champion entry stays 0. This opens Upgrades (`f_0037ECB8`), Time Challenges (`f_00384A18`), the dealer's full
  stock (`f_002E4230`), Highway (`f_0016EF58`) and the other callers of `f_002E27F8` / `f_002E2838`.
* Eve rank window: `f_002E5200(rank, record)` is `max_rank < rank <= min_rank` for an Eve's record
  (`0x4AB158 + 12 * record`: Eve, min, max). The Eve list (`f_003712D0`, "Choose Eve", reached by Jump To "Out of
  Town") lists the open Eves in record order (easiest first) but shows and cycles through **four**, so "every
  Eve open" is done as "the first four Eves not played yet": with OpenCareer the wrapper answers 1 when the native
  window does OR the Eve's result byte (`*(0x3D9394) + 0x118 + Eve`, 0xFF = not played; `f_002E0B18`) is still
  0xFF. Whatever the rank, the player is offered the next four Eves, and finishing one brings the next up, so
  all 25 are reached in order; with every Eve played all are open again. The refill of the four open Eves and the
  "new Eves" count see the same answers. `career_eves.val` is not touched. (Smoke: Jump To "Out of Town" at
  rank 100 lists Ashland Smackfest, Woodly Crash Test, Eagan County Derby and Crunch Time at Hampton; natively
  only the first three are in the window.)
* Troy's Place and T.J.'s (home challenges): `rep_required` in `home_challenges.val` is a reputation
  requirement (5..400), not a rank. `f_002E5758` (the `challenges.val` / `home_challenges.val` loader) is
  wrapped; after it returns, record `+0x2C` (0x44-byte records) is cleared.

## Dealer and garage

* The dealer lists five cars. `f_002E35F0` (AutoShopSetAvailable) is wrapped: with AllCars it replaces the
  five slots (ASCarList indices at `0x4AB130`, vehicles in `out[5]`) by one page of a list of **every chassis**
  in ASChassisList, cheapest first (so Phantom and the Cup car, price 1200, are on page 5 and the Rocket on the
  last page); the paint job shown rotates with every page turn, so every ASCarList entry shows up. Native prices.
* **Live dealer paging (L2 / R2, L1 / R1, Q / E, 1 / 3, Left / Right)**:
  `f_0036E9C0` (the Buy car screen) is wrapped to capture its local stack frame pointer while active.
  When the player presses page-cycling controls (`PAD_L2`, `PAD_L1`, `PAD_LEFT` for previous page;
  `PAD_R2`, `PAD_R1`, `PAD_RIGHT` for next page), `career_vblank` queues a page delta.
  The dealer's per-frame callback `f_003702F0` applies the turn: it calls `fill_dealer(0)` to update
  `kShopSlots` (0x4AB130), writes the new 5 car names, formatted prices, `ASCarList` indices, and widget
  result values directly into the active stack frame, and invalidates the cached selection (`0x004DD388 + 0x280`).
  `f_00370DD8` (`get_slot_for_car`) is wrapped so that off-page cars focus the highlighted lot bay
  smoothly without asserting in the native code or panic handlers.
* The garage keeps its four slots (`0xC68` bytes of CDBupgr is exactly the 0x68 header plus four 0x300 slots, so
  a bigger garage means a new save layout; not done). FreeSwap: `f_002E3BC8` (depreciation by condition, used
  when selling and trading in through `f_002E3F20` / `f_002E4178`) returns 0: the full chassis and parts price
  comes back, so every car can be tried at no loss.
* Content check: `tools/career_content_check.py` (and ctest `CareerContent`) require a `[car]` for every
  `[vehicle]` chassis of `buycar.val` (the game panics "No matching chassis to %s from buy list" otherwise),
  a `[vehicle]` for every `[car]`, and a class with parts for every chassis. The NASCAR import must keep this.

## Upgrades for every vehicle

`catalog.val` parts are per **car class** (`compact`, `midsize`, `muscle`: engine "Turbo Engine Mod",
suspension "Sport Suspension", exhaust "Chassis Reinforcement"), and `carclass.val` assigns every chassis a
class. All 28 dealer chassis (26 retail plus Phantom and Cupcar, both Midsize) are in a class that has all
three parts, so no entry is missing and nothing was added. Never add physics values here.

## Special vehicles

Retail has six special vehicles, in the Special class and only as AI drivers (dirt.val) and in two Special
Eves (Kenyon "School Bus Race"): Postal, Taxi, Ambulance, Copcar, Hearse and Bus. `carclass.val` also lists
Clown, Tow and Carrere, but retail has no model, physics or vehicle for them, so there is nothing to sell.
`tools/build_career_specials.py` adds, to an existing integrated profile, one Career car for each:

* a new chassis, a byte-for-byte copy of the retail `DATA/CAR` file under a new name. The original chassis
  stays in the Special class (the class lookup `f_00164218` is first-match, Compact, Midsize, Muscle, Special,
  so the same name could not be in two classes), so Action and the School Bus Race are untouched. Physics,
  damage and model are the retail ones: nothing is retuned.
* `[car]` (price) and `[vehicle]` (new vehicle numbers 780-785, unused in retail) in `buycar.val`, the original's
  ratings in `car_stats.val`, and the chassis in an **existing** class of `carclass.val` (the class of its
  closest native peers, so it can enter Eves of that class; no class was created).
* the original vehicle's paint and dealer art in `CT.RES` (`<n>.tex/.tm0`) and `UICAR.RES`
  (`uicar_<n>`, `uicars_<n>`).

Performance used (retail `physics.val` mass and engine power, ratings in `car_stats.val`); peers are the native
Career cars closest in speed rating, power to weight and mass; the price is inside the peers' price band, which
is what the native wealth tiering (`f_002E35F0`: the dearest chassis the player can afford, and the ones just
below it) goes by:

| Career car (retail chassis) | mass lb / hp | speed / handling / durability | Peers (price) | Price | Class | Offered from |
|---|---|---|---|---|---|---|
| Schoolbus (Bus) | 9000 / 420 | 3 / 1 / 10 | Ocelot ($230), Desio ($325) | $280 | Midsize | wealth $280, with the first cars |
| Funeral (Hearse) | 5000 / 400 | 5 / 5 / 9 | Hunter ($630), Arrow ($1,000) | $800 | Midsize | wealth $800 |
| Medic (Ambulance) | 5250 / 400 | 6 / 2 / 6 | Digit ($885), Fillmore ($1,250) | $1,100 | Midsize | wealth $1,100 |
| Cabbie (Taxi) | 4400 / 280 | 6 / 6 / 7 | Fillmore ($1,250), Empire ($1,700; the same engine and mass) | $1,600 | Midsize | wealth $1,600 |
| Mailvan (Postal) | 2500 / 165 | 6 / 8 / 2 | Digit ($885), Bulldog ($2,399), Hurricane ($3,000) | $1,800 | Compact (not Muscle) | wealth $1,800 |
| Patrol (Copcar) | 3250 / 300 | 7 / 7 / 5 | Hurricane ($3,000), Mongoose ($3,900) | $3,500 | Muscle | wealth $3,500 |

"Wealth" is money plus the value of the cars owned. With AllCars (default) the paged list overrides the
tiering and every car is on a page (the specials sit among the others by price); with AllCars off the
native tiering offers each special once the player's wealth reaches its price. Content: `integrated-specials`
(see the report); `tools/career_content_check.py` and ctest `CareerContent` check it.

## Killable challengers

In Career free roam, `f_001741C0` (`*(int32_t*)0x3CDA24 != 6`) returns 1 while the free-roam / Highway challenge
state machine is active:

* `f_0018AAD0` (`can_damage`, `RA = 0x18AB2C`) requires `f_001741C0() != 0` when `event_type == 1`, allowing cars
  in Career free roam to take deformation and damage.
* However, `f_0018CA98` (collision deformation and damage) calls `f_001741C0()` again at two wreck gates:
  * `0x0018D3F0` (`RA = 0x0018D3F8`): when `car->damage` (`car + 0x1BEC`, offset `7148`) exceeds `1.0f`
    (`0x3F800000`), if `f_001741C0() != 0`, it clamps `car->damage` back to `0.9f` (`0x3F666666`) so the car can
    never reach `1.0f` and enter wrecked state (`car + 0x1BE8 = 0xA / 0xB / 0xC`).
  * `0x0018D7E4` (`RA = 0x0018D7EC`): on a severe hit, if `f_001741C0() != 0`, it skips the instant-wreck path.

With `KillableChallengers` on, `f_001741C0` is wrapped: when called from `0x0018D3F8` or `0x0018D7EC` for a
non-player car (`$s2 = R[18]`, `car + 0x1E0 != 0x50`), it returns `0`, letting Highway and Malt Shop challengers
reach `1.0f` damage, shed a wheel, and wreck while keeping the player's own car from softlocking free roam. If the
active Highway challenger (`*(0x3CDA78)`) is wrecked before a challenge is accepted (`*(int32_t*)0x3CDA24` in `0` or
`1`), `career_vblank` sets `0x3CDA24` to `6` so a wrecked challenger never offers a race prompt.

A wrecked challenger stops where it died: its free-roam driver keeps setting controls through `CarSetThrottle`
(`0x18AA70`, `car + 0x1B70`) and `CarSetBrake` (`0x18AA68`, `car + 0x1B74`); with `KillableChallengers` on, both
are wrapped and, for a non-player car in Career free roam at 100 % damage (or wreck state `0xA`-`0xC`), hold the
throttle at 0 and the brake at full. Checked in the game with `EOD_CAREER_WRECK_TEST=<frame>` (sets every AI car
to 100 % damage and logs speed, throttle and brake each second): the wrapped values hold. Not yet checked: a
challenger that is moving when it is wrecked (free-roam AI cars stay parked until a challenge starts).
