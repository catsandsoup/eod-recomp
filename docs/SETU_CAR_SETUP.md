# SETU: car setup sheets (`*.gar`)

A setup sheet stores one car setup: anti-roll bars, springs, shocks, camber and
tyre pressure for each wheel, steering lock, gear ratios, final drive and fuel.
Decoder: `tools/formats/setu.py` (`FOURCCS = ("SETU",)`); view contract:
`docs/FORMAT_VIEW_CONTRACT.md`. Tests: `tools/tests/test_formats_setu.py`.

**The retail game does not read these files.** They come from the NASCAR engine
the game is built on and ship inside the track archives, but Eve of Destruction
takes each car's handling from its `physics.val`. Editing a sheet changes the
file only; a setup change that should be felt in game belongs in the car's
`physics.val`. The editor shows this as a warning on every sheet.

## Retail members

64 sheets, all exactly 196 bytes, 38 distinct payloads.

| Archive | Sheets |
|---|---|
| `DATA/RES/TRKEDIT.RES` | `arcade.gar`, `default.gar` |
| `DATA/TRACKS/AFTON.TRK` | `Advanced.gar`, `default.gar` |
| `DATA/TRACKS/ANOKA.TRK` | `Advanced.gar`, `ai.gar`, `arcade.gar`, `default.gar` |
| `DATA/TRACKS/BAXTER.TRK` | `default.gar` |
| `DATA/TRACKS/BEMIDJI.TRK` | `Advanced.gar`, `ai.gar`, `arcade.gar`, `default.gar`, `dirtAC.gar`, `dirtC.gar` |
| `DATA/TRACKS/DENNISON.TRK` | `Advanced.gar`, `ai.gar`, `arcade.gar`, `default.gar`, `modifiedAC.gar`, `modifiedC.gar` |
| `DATA/TRACKS/DUNDAS.TRK` | `Advanced.gar`, `ai.gar`, `arcade.gar`, `default.gar`, `dirtAC.gar`, `dirtC.gar`, `fig8.gar` |
| `DATA/TRACKS/EAGAN.TRK` | `Advanced.gar`, `ai.gar`, `arcade.gar`, `default.gar` |
| `DATA/TRACKS/EMPTYT_1.TRK` | `Advanced.gar`, `default.gar` |
| `DATA/TRACKS/GILROY.TRK` | `default.gar` |
| `DATA/TRACKS/HASTINGS.TRK` | `Advanced.gar`, `ai.gar`, `arcade.gar`, `default.gar`, `dirtAC.gar`, `dirtC.gar` |
| `DATA/TRACKS/HOMEBASE.TRK` | `Advanced.gar`, `ai.gar`, `arcade.gar`, `default.gar`, `modifiedAC.gar`, `modifiedC.gar` |
| `DATA/TRACKS/KENYON.TRK` | `Advanced.gar`, `ai.gar`, `arcade.gar`, `default.gar`, `dirtC.gar` |
| `DATA/TRACKS/MANKATO.TRK` | `Advanced.gar`, `ai.gar`, `arcade.gar`, `default.gar`, `modifiedAC.gar`, `modifiedC.gar` |
| `DATA/TRACKS/SPRING_1.TRK` | `default.gar` |
| `DATA/TRACKS/WELCH.TRK` | `Advanced.gar`, `ai.gar`, `default.gar`, `modifiedAC.gar`, `modifiedC.gar` |

## Which sheet is used when

None, in the retail game (see Evidence). The names and data describe how the
source engine intended them:

| Sheet | Contents |
|---|---|
| `default.gar` | Track default |
| `Advanced.gar` | Advanced handling mode |
| `arcade.gar` | Arcade handling mode: more steering lock (25-30 degrees against 9-16), softer, higher right-side pressures |
| `ai.gar` | Computer opponents: round hand-typed values, equal pressures, no fuel. Only 3 distinct payloads across 11 tracks |
| `dirtAC.gar`, `modifiedAC.gar` | Byte-for-byte `Advanced.gar` of the same track except the final drive (all 6 cases) |
| `dirtC.gar`, `modifiedC.gar` | Variant of the default/Advanced sheet with its own final drive and front bar |
| `fig8.gar` | Dundas figure-8 layout |

The player cannot change car setups in game. `setup.lyt` in `UIACTION.RES` is
the controller/vibration/units options screen. Garage strings such as
"Load Setup" and "Save Setup" in `LOCALE.RES`, and `UI3D.RES#setups.val` (which
lists placeholder sheets `foo.gar` and `foo2.gar` at Bristol), are leftovers
from the source engine. Career tuning is the upgrade system
(`CDBupgr_GetCarTuningBlock`, `0x002E1380`), not setup sheets.

## File layout

No header: the type code SETU exists only in the archive directory entry
(entry `+0x20`), never in the payload. 49 little-endian float32 values.

Slider values are 0-1 positions on ranges every retail car declares in its
`physics.val` (identical in all 37 retail cars): `[Front]`/`[Rear]` `rate`
200-600, `bump` and `rebound` 60-840, `camber` -10 to 10 (front) and -1.5 to
1.5 (rear), `toe` -4 to 4 and -0.5 to 0.5, `sway` 0-800 and 0-50;
`[Chassis]` `wheel_lock` 0-35, `rspoiler_angle` 40-70, `fuel_capacity` 22.
None of these keys is read by the retail executable either.

| Offset | Field id | Setting | Stored as | Confidence | Evidence |
|---|---|---|---|---|---|
| 0x00 | `chassis.front_sway_bar` | Front anti-roll bar | slider = diameter^4 | medium | Every value is (k/16)^4 or (k/32)^4 (torsion stiffness grows with diameter to the fourth power) |
| 0x04 | `chassis.front_toe` | Front toe (unconfirmed) | slider | low | 0.5-0.55, 0.1 steps of the front toe range |
| 0x08 | `chassis.unknown_08` | Unknown | 0-1 | low | 0 except 0.75 / 0.125 in two default sheets |
| 0x0C | `chassis.unknown_0c` | Unknown | 0-1 | low | 0.05 steps, differs between modes |
| 0x10 | `chassis.rear_sway_bar` | Rear anti-roll bar | slider = diameter^4 | medium | Values 0, 0.5^4, 0.6^4, 1 |
| 0x14 | `chassis.rear_toe` | Rear toe (unconfirmed) | slider | low | 0.5-0.7, 0.1 steps of the rear toe range |
| 0x18 | `chassis.unknown_18` | Unknown | 0-1 | low | 0, 0.125, 0.3, 1 |
| 0x1C | `chassis.unknown_1c` | Unknown | 0-1 | low | 1 on player sheets, 0.5 on opponent sheets |
| 0x20 + 0x14·w | `<wheel>.spring` | Spring rate | slider | medium | 1/80 steps (5 lb/in of 200-600) |
| +0x04 | `<wheel>.bump` | Shock bump | slider | medium | Multiples of 1/78 (10-unit steps of 60-840), usually below rebound |
| +0x08 | `<wheel>.rebound` | Shock rebound | slider | medium | Multiples of 1/78 |
| +0x0C | `<wheel>.camber` | Camber | slider | medium | 0.1 degree steps (front) / 0.05 (rear); left positive, right negative on ovals |
| +0x10 | `<wheel>.pressure` | Tyre pressure | psi | high | 24-32.5 psi, left lower than right on oval sheets |
| 0x70 | `steering.lock` | Steering lock | slider | medium | Whole degrees of 0-35 in every sheet |
| 0x74 | `aero.rear_spoiler` | Rear spoiler (unconfirmed) | slider | low | Half degrees of 40-70 |
| 0x78-0x84 | `gear.1`-`gear.4` | Gear ratios 1-4 | ratio | high | Decreasing 3.5 → 0.9; stock cars have four gears |
| 0x88, 0x8C | `gear.5`, `gear.6` | Unused gear slots | ratio | low | Constant 0.6/0.2 or 1.0/0.75 |
| 0x90 | `gearbox.unknown_90` | Unknown | 0-1 | low | 0.5 everywhere |
| 0x94 | `final_drive` | Final drive | ratio | medium | 3.07-6.5; the only difference between `*AC` sheets and `Advanced.gar` |
| 0x98 | `brakes.balance` | Brake balance (unconfirmed) | 0-1 | low | 0.475-0.51 in 0.005 steps |
| 0x9C | `weight.unknown_9c` | Unknown (left weight?) | 0-1 | low | 0.5 or 0.542 |
| 0xA0 | `weight.unknown_a0` | Unknown (cross weight?) | 0-1 | low | 0.5-0.52 |
| 0xA4 | `fuel.load` | Fuel load | gallons | medium | 22 = `fuel_capacity`; 0 on opponent sheets |
| 0xA8 | `chassis.unknown_a8` | Unknown | 0-1 | low | 0, 0.125, 0.2 |
| 0xAC, 0xB0 | `chassis.unknown_ac`, `chassis.unknown_b0` | Unknown pair | number | low | 0, 9 or 11, always equal |
| 0xB4-0xC3 | block | Unknown block 1 | 4 values | — | Zero in all 64 sheets |

Wheels `w` = 0 front left, 1 front right, 2 rear left, 3 rear right (order from
the left/right pressure and camber pattern on oval sheets).

Editor limits: sliders 0-1; pressure 10-60 psi; gears 1-4 0.2-10; gear slots
5-6 0-10; final drive 1-10; fuel 0-22 gallons; the 0xAC pair 0-30. Saving
with no edits is byte-identical for all 64 retail sheets.

## Evidence

Checked in `SLUS_209.10` and the recompiled code in `gen/`:

- **No loader.** Assets are requested by type through `ResourceGet`
  (`0x00132638` → `0x00130D90`, which rejects a type mismatch) and
  `ResourceGetStream` (`0x001323E8`). Every call passes a constant type built
  with `lui`/`ori` or read from a table (`ILIN`, `SCOL`, `BPPT`, `SFX0`, …);
  no instruction builds `SETU` (`0x53455455`) or its byte-reversed form, and no
  data word holds either. The executable contains no `.gar`/`gar` string, no
  sheet name (`default.gar`, `arcade.gar`, …, in plain or XOR-0xA5 form), and
  no CRC of any sheet name. The only `SETU` bytes are inside a debug string.
- **No setup consumer.** Car physics initialisation (`0x00185470`) reads
  `grip`, `slip`, `load_factor`, `brake_factor`, `tire_diameter`,
  `spring_factor`, `rear_end_ratio` (`0x001860E8`, default 5.0), `redline`, and
  the gear table via `num_gears`/`g%d` (`0x00186E54`, `0x00186EBC`). The
  setup range keys (`rate`, `bump`, `rebound`, `camber`, `toe`, `sway`,
  `brake_force`, `wheel_lock`, `rspoiler_angle`, `fuel_capacity`) do not
  occur in the executable.
- **Remnants.** `0x00138E70` (orphan, no callers) fills a 196-byte block with
  `0xEE`, the source engine's "setup not loaded" initialiser. The orphan debug
  writer `0x00117F80` prints a ghost-file layout with
  `CARSETUP_OFFSET = 0x98` (setup copy at 0x98-0x163).
- The NASCAR vehicle import (`tools/nascar_roster.py`) reads source-game
  sheets with gears at 0x78 and final drive at 0x94, which matches this
  layout.

## Making a setup change felt in game

Edit the car's `physics.val` instead: `[Transmission]` `g1`-`g4` and
`rear_end_ratio`, `[Chassis]` `spring_factor`, `grip`, `brake_factor`.

Not yet observed in a native run. The decisive check is a scripted full-throttle
launch on Anoka with every Anoka sheet set to an extreme 1st gear and tyre
pressure: the speed and engine-speed readouts at fixed frames must match an
unmodified run, while the same 1st-gear change in the car's `physics.val` must
not.
