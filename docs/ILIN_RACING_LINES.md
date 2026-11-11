# ILIN racing and ghost lines (`*.ild`, `*.ili`)

Decoder: `tools/formats/ilin.py` (labelled view, editable node X/Z). Tests:
`tools/tests/test_formats_ilin.py` (retail-gated; disc read only). Node layout
first recorded in `tools/trk.py` (`decompile_ild`), `tools/world_export.py`
(`racing_line`, the red line in track previews) and `docs/BATTLE_NAVIGATION.md`.

Retail census: 211 members (125 in the 15 track archives, 86 in the ghost car
archive). 21 are empty (`roadrace.ili`, `optN.ili` in track archives: 0 bytes
on disc, directory checksum of an original that is not present; read them with
`verify_crc=False`). The other 190 all parse with the layout below.

## What it is

| Evidence | Finding | Confidence |
|---|---|---|
| `f_00137A68` (AI driver setup) loads a line and panics with "ai: Can't load centerline" | `.ild` is the path computer-driven cars follow | high |
| `f_0013D9A8` maps line type to file name: 0 `fig8`, 1 `oval`, 2 `track`, 0x200 `bw_fig8`, 0x201 `bw_oval`, 0x202 `bw`, prefixed by the event's line basename (`rr`, `opt6`, ...) | one line per layout and direction (`bw` = backwards) | high |
| After loading, the game stamps `0x104F128B` into header +0 | the file stores 0 there | high |
| `f_002EC2BC`, `f_002ECFEC` fail with "smoke: couldn't create ideal line / wear line" | the same line feeds the road smoke and tyre-wear effects | medium |
| `f_001174D0` builds `ghostcar.res/<name>_e.ili` and `*<archive>/<prefix><purpose>.ili`, "ghost: Loading trackline" | `.ili` are ghost lines with an embedded recorded-run block | medium |
| Lap counting, checkpoint progress, wrong-way detection | not traced to this file | unknown |

## Layout (little endian)

Header (48 bytes): `+0` load-time tag slot (0), `+4` header size 48, `+8` node size 60,
`+12/+16/+20` main-line length in metres (three copies; equals the sum of
segment lengths), `+24` f32 unknown, `+28` u16 0 then u16 main-line node count,
`+32` node array offset, `+36` offset of the first side-line node (or 0),
`+40` ghost record offset (0 in `.ild`), `+44` pointer table entry count.

Pointer fix-up table: `count` u32 file offsets of every stored pointer. Node
array: 60-byte nodes.

| Node offset | Field |
|---|---|
| +0, +4 | X, Z in metres (left-handed world; map right = +X, up = +Z). No height is stored. |
| +8, +12 | unit direction X, Z |
| +16 | main nodes: index << 16; side nodes: fixed-point (16.16) position where they meet the main line |
| +20 | next node (file offset; last main node points back to node 0) |
| +24 | segment length to the next node (m) |
| +28 | distance along the line (m) |
| +32, +36, +40, +44, +48 | unknown floats (+32 is 35.556 on main lines, half on side lines) |
| +52 | unknown flags (256 on every main node) |
| +56 | side-line list pointer (one node per line) |

After the nodes: `.ild` has either nothing or a 16-byte list record
`{0, 0, 2, first side node offset}`; `.ili` has a ghost record starting at
the header +40 offset (car and driver strings plus a closing 20-byte lap
summary: lap time in seconds, frame count at about 59.94 per second).

## Editing

Only node X/Z are editable (`node.<n>.x`, `node.<n>.z`; 8 bytes per node).
Direction, segment length, distance and the header lap length are stored, not
derived: after moving a node they are stale until recalculated by a tool.
Adding or removing nodes needs the pointer table, header counts and side-line
links rebuilt and is not supported.

## Decisive native test

Shift several consecutive main-line nodes of an oval line (for example
`oval.ild` of a short track) sideways by a few metres with the Kit, run an AI
race headlessly (`EOD_HEADLESS=1 EOD_MUTE=1`, `--headless`) and compare
captures with the retail line: AI cars should take the displaced path. Use a
separate mod profile and never edit the disc.
