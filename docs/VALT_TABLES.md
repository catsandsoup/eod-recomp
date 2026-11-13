# VALT tables: what every value means

VALT members (`.val`, `.va0`, `.mvl`, `.bvl`, `.pln`) are nested key/value tables. The retail
disc has 755 of them in 25 families. `tools/formats/valt_schema.py` gives every table, section and
key a plain label, group, unit, explanation, confidence and reader status, and the Creation Kit
shows those instead of raw keys.

## How the game reads a table

Traced in `SLUS_209.10`:

| Address | Role |
| --- | --- |
| `0x00134A70` | Opens a loaded table in place (vtable `0x003F2190`). |
| `0x00134EA0` | Finds a section by name (table vtable +16). |
| `0x00135C80` | Finds a key in a section. Walks the pairs in order with `0x003A72D8`, a **case-insensitive** compare, returns the **first** match, and if the key is absent repeats the search in the section's **parent** (section +20). |
| `0x00135B48`, `0x00135BB0`, `0x00135C18` | Read a key as a number or text, with a default when absent. |

Consequences for modders:

- `League`, `league` and `LEAGUE` are the same key.
- If a section repeats a key, only the first one counts.
- A key missing from a section is taken from the section it inherits from (for example
  `contest_defaults` in the Eve files), so deleting a key can change the value rather than remove it.
- `$NNNN$name` sections are nested tables. `NNNN` is the nested table's number in the file; the parent
  refers to it with an `@name` entry. Edit the nested table, not the `@` link.

## Reader status

`valt_schema_build.py` rebuilds `valt_schema_data.json` from the disc: for every key it records the
numeric retail range and the functions that reference the key's text (string references, data
pointers, `%d` variants such as `g1` from `"g%d"`, and `%s%d` lookups such as `speedometer0`).

A key whose name never appears in the executable is reported as **not used** only when the game looks
up other keys of the same section by name. Some tables are read as lists (car class members, paint
panels, chassis allowed in an event, blur textures), so their key names never appear and are still
data. Those are reported as "may be read as a list".

## Confidence

| Value | Meaning | Shown in the Kit as |
| --- | --- | --- |
| high | Reader traced and the effect is documented or visible in the game | Confirmed in game / From the game's code |
| medium | Reader traced, or the effect is clear from the reader and the value pattern | From the game's code |
| low | From the key name, or the reader exists but its use is not decoded | Best guess |

## Coverage (retail disc)

`traced` counts key patterns with a written rule at medium or high confidence.

| Family | Files | Key patterns | Traced | Read by the game | Not used |
| --- | ---: | ---: | ---: | ---: | ---: |
| Career data (dealer, classes, challenges, sponsors...) | 30 | 654 | 382 | 623 | 23 |
| Career events (Eves) | 16 | 344 | 288 | 318 | 25 |
| Physics part library (PART.RES) | 12 | 308 | 0 | unknown | 5 |
| Track effects | 12 | 308 | 17 | 308 | 0 |
| Car handling and engine (physics.val) | 37 | 192 | 168 | 94 | 80 |
| Props and obstacles | 14 | 183 | 131 | 177 | 5 |
| Event layouts (c_*.va0) | 342 | 148 | 68 | 123 | 1 |
| Event intro sequences | 15 | 103 | 38 | 101 | 2 |
| Track rules and AI (track.val) | 16 | 91 | 24 | 36 | 53 |
| Part shop (catalog.val) | 2 | 90 | 78 | 90 | 0 |
| Track cameras | 29 | 69 | 57 | 68 | 1 |
| Victory fireworks | 7 | 51 | 40 | 51 | 0 |
| AI driving lines | 12 | 46 | 8 | 46 | 0 |
| Explosions and flares | 4 | 41 | 17 | 38 | 3 |
| AI drivers | 5 | 40 | 11 | 33 | 7 |
| Car effects | 33 | 39 | 20 | 39 | 0 |
| Firework effects | 1 | 34 | 17 | 34 | 0 |
| Track shadows (shadow.val) | 12 | 23 | 21 | 23 | 0 |
| Car damage skeleton | 73 | 20 | 12 | 19 | 1 |
| Engine sound | 5 | 16 | 8 | 15 | 1 |
| Car lights | 2 | 13 | 13 | 13 | 0 |
| Wheel models | 5 | 13 | 10 | 13 | 0 |
| Car detail levels (lod.val) | 37 | 11 | 6 | 11 | 0 |
| Collision shapes | 27 | 7 | 4 | 7 | 0 |
| Track-wide props | 7 | 7 | 5 | 7 | 0 |
| **Total** | **755** | **2,851** | **1,443** | **2,287** | **207** |

Every key has a label and explanation; none is shown by its raw name only. `physics.val` and
`shadow.val` have a written rule for every key.

## Findings

- `physics.val`: 80 of 192 key patterns are values from the physics engine the game is built on and are
  never read (fuel capacity, steering lock, spoilers, suspension geometry). Handling comes from mass,
  inertia, grip, the tyre curve (`magic_b`..`magic_e`, `frict_*`, `stiff_*`), engine curve, gears and
  `torque_balance`.
- `shadow.val` is the track's painted shadows (ellipses, arcs, parallelograms) loaded by `trkshad`
  (`0x0032AC00`). An Eve's `trkshad_force_brightness` overrides every shadow's light level.
- `track.val`: of the per-difficulty AI values only `easy_skill_range` is looked up; the arcade grip,
  pit speed and yellow-flag values are never read.
- Eve files: `easy_sfd`, `medium_srd` and the other per-difficulty adjustments are never read.
- Part shop values are fractions added to the car's tuning multipliers (1.0 = stock).
- Packed 64-character frames (`unfloat_frame64`, `frame64`, home-town `*_entrance`/`*_inside`,
  `house_car_frame64`) are decoded to metres in the field explanation.

## Backend JSON

`modding_backend.inspect()` for a VALT member returns `title` and `summary`; each section has
`label` and `explanation`; each field keeps `key`, `value`, `original`, `editable` and adds `label`,
`group`, `unit`, `explanation`, `confidence`, `used` (true, false or null), `min` and `max` (set only
where the game clamps a value).

Tests: `tools/tests/test_valt_schema.py` (retail checks skip without the disc or `EOD_DISC`).
